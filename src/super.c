/*
 * super.c - forgectrl: controller-mode supervisor
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * forgectrl owns the lifecycle of the motion controllers: exactly one
 * of grblHAL (GRBL mode) or gfcloud (Glowforge web-service mode) runs
 * at a time, spawned as a direct child of this daemon. The parent-child
 * relationship is load-bearing: it is how the pulse-device broker hands
 * the controller its fd at spawn, and how a controller death is
 * detected the moment it happens.
 *
 * One thread owns the whole lifecycle (spawn, reap, respawn, stop) and
 * everything else talks to it through a small request mailbox - there
 * is exactly one waitpid() caller in the process. Unexpected controller
 * death safes the machine (cnc/stop + laser latch; today the kernel
 * dead-man on the child's own fd already covers this, but the broker
 * makes these writes the real mechanism) and respawns with backoff.
 *
 * The mode-switch sequence (POST /mode) is idle-gated: stop the active
 * controller, persist controller_mode, start the other, wait for its
 * first job-state report to reach the cooling engine. Diagnostics use
 * the same machinery to take the hardware: suspend (controller down,
 * mode unchanged) and resume - the controller that comes back is the
 * selected mode's, whichever that is.
 *
 * The boot-time init scripts do not start controllers; they defer here.
 * If an unmanaged controller is found running at startup anyway (legacy
 * scripts, manual start), the supervisor stands by rather than fight
 * over the exclusive-open pulse device and the Grbl port.
 */
#define _GNU_SOURCE
#include "cool.h"
#include "diag.h"
#include "liveness.h"
#include "settings.h"
#include "status.h"
#include "super.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define GRBL_BIN   "/usr/bin/grblHAL_glowforge"
#define GRBL_LOG   "/data/glowforge.log"
#define CLOUD_BIN  "/usr/sbin/gfcloud.py"
#define CLOUD_LOG  "/data/gfcloud.log"
#define PULSE_DEV  "/dev/glowforge"

#define STOP_TERM_WAIT_S   5      /* SIGTERM grace before SIGKILL */
#define RESPAWN_MIN_S      1
#define RESPAWN_MAX_S      30
#define HEALTHY_UPTIME_S   60     /* uptime that resets the backoff */
#define REPORT_WAIT_S      15     /* mode switch: first /cool/state */

typedef enum { Ctl_None = 0, Ctl_Grbl, Ctl_Cloud } ctl_t;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static pthread_t th;
static int th_run = 0;

static ctl_t want = Ctl_None;      /* selected mode */
static int suspended = 0;          /* diag takeover / standby */
static pid_t child_pid = 0;
static ctl_t child_ctl = Ctl_None;
static double spawned_at = 0.0;
static unsigned backoff_s = RESPAWN_MIN_S;
static double respawn_at = 0.0;    /* not before this time */
static unsigned generation = 0;    /* bumped on every state change */

static int broker_fd = -1;         /* /dev/glowforge, held for our lifetime */
static int probed = 0;             /* liveness verified since broker open */
static int motion_fault = 0;       /* probe failed after recovery - no spawn */
static int standby_takeover = 0;   /* unmanaged controller found: retake at idle */

static void wr_attr(const char *attr, const char *val);

/* Stop an unmanaged (legacy- or orphan-started) controller the legacy
 * way so the supervisor can take over. Called with mu NOT held. */
static void takeover_unmanaged(void)
{
    fprintf(stderr, "forgectrl: super: taking over from unmanaged "
                    "controller\n");
    system("/etc/init.d/grblhal stop >/dev/null 2>&1");
    system("/etc/init.d/gfcloud stop >/dev/null 2>&1");
    system("pkill -x grblHAL_glowfor 2>/dev/null");
    system("pkill -f '[g]fcloud\\.py' 2>/dev/null");
    sleep(1);
}

static double wall_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* The pulse-device broker: one open for the daemon's lifetime, handed
 * to every controller as an inherited fd (GF_PULSE_FD). The device is
 * exclusive-open, so holding it also *enforces* that no one else can
 * open it; the flock arms the kernel dead-man on the shared
 * description (final close mid-run = e-stop - i.e. forgectrl dying
 * with no writer alive). Opened lazily at the first managed spawn so
 * standby next to a legacy self-opening controller stays conflict-free.
 * Called with mu held. */
static void broker_open_locked(void)
{
    if (broker_fd >= 0)
        return;
    /* A just-stopped legacy holder's close can lag its exit. */
    for (int i = 0; i < 30 && broker_fd < 0; i++) {
        broker_fd = open(PULSE_DEV, O_WRONLY);  /* no O_CLOEXEC: inherited */
        if (broker_fd < 0)
            usleep(100 * 1000);
    }
    if (broker_fd < 0) {
        fprintf(stderr, "forgectrl: super: cannot open " PULSE_DEV
                        " - controllers will self-open (no broker)\n");
        return;
    }
    if (flock(broker_fd, LOCK_EX) != 0)
        fprintf(stderr, "forgectrl: super: flock on " PULSE_DEV " failed\n");
    probed = 0;     /* a fresh hold means unverified motion */
    fprintf(stderr, "forgectrl: super: holding " PULSE_DEV " (broker)\n");
}

/* Motion-liveness gate, run with mu NOT held (takes seconds; the
 * recovery ladder takes a minute). The DRV8825 drivers can come out of
 * a rail power-up unserviceable; each recovery attempt gives them a
 * longer true power-off before re-probing. Returns 1 ok, 0 fault. */
static int probe_sequence(int fd)
{
    static const int ladder_s[] = { 0, 5, 15, 30 };
    char detail[96];

    for (size_t i = 0; i < sizeof(ladder_s) / sizeof(ladder_s[0]); i++) {
        if (ladder_s[i] > 0) {
            fprintf(stderr, "forgectrl: super: motion dead - rail off %d s "
                            "and re-probing (%zu/3)\n", ladder_s[i], i);
            wr_attr("cnc/disable", "1");
            sleep((unsigned)ladder_s[i]);
        }
        wr_attr("cnc/enable", "1");
        sleep(1);
        int rc = liveness_probe(fd, detail, sizeof(detail));
        fprintf(stderr, "forgectrl: super: liveness probe: %s - %s\n",
                rc == 1 ? "MOTION OK" : rc == 0 ? "NO MOTION" : "ERROR",
                detail);
        if (rc == 1)
            return 1;
        if (rc < 0)
            return 1;   /* cannot probe (no accel?): do not block the machine */
    }
    fprintf(stderr, "forgectrl: super: MOTION FAULT - the stepper drivers "
                    "did not recover; a full power cycle may be required. "
                    "Controllers stay down (retry via POST /mode).\n");
    return 0;
}

static void wr_attr(const char *attr, const char *val)
{
    char path[96];
    snprintf(path, sizeof(path), "/sys/glowforge/%s", attr);
    int fd = open(path, O_WRONLY);
    if (fd >= 0) {
        (void)!write(fd, val, strlen(val));
        close(fd);
    }
}

static const char *ctl_name(ctl_t c)
{
    return c == Ctl_Grbl ? "grbl" : c == Ctl_Cloud ? "cloud" : "none";
}

/* ------------------------------------------------------------- spawn */

/* Called with mu held. */
static void spawn_locked(ctl_t ctl)
{
    broker_open_locked();

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "forgectrl: super: fork failed: %s\n",
                strerror(errno));
        respawn_at = wall_s() + backoff_s;
        return;
    }
    if (pid == 0) {
        /* Child: own process group so stop() can signal helpers too. */
        setpgid(0, 0);
        const char *log = ctl == Ctl_Grbl ? GRBL_LOG : CLOUD_LOG;
        int lfd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (lfd >= 0) {
            dup2(lfd, 1);
            dup2(lfd, 2);
            if (lfd > 2)
                close(lfd);
        }
        if (broker_fd >= 0) {
            char fdv[16];
            snprintf(fdv, sizeof(fdv), "%d", broker_fd);
            setenv("GF_PULSE_FD", fdv, 1);
        }
        if (ctl == Ctl_Grbl) {
            /* Mirrors the former init-script launch. */
            if (chdir("/data") != 0)
                _exit(126);
            setenv("GFSINK", PULSE_DEV, 1);
            execl(GRBL_BIN, GRBL_BIN, "-p", "23",
                  "-e", "/data/EEPROM-glowforge.DAT", (char *)NULL);
        } else {
            execl(CLOUD_BIN, CLOUD_BIN, (char *)NULL);
        }
        _exit(127);
    }
    child_pid = pid;
    child_ctl = ctl;
    spawned_at = wall_s();
    generation++;
    fprintf(stderr, "forgectrl: super: started %s controller (pid %d)\n",
            ctl_name(ctl), (int)pid);
}

/* Reap and, if the death was unexpected, safe the machine and arm the
 * respawn backoff. Called with mu held. */
static void reap_locked(int status)
{
    ctl_t died = child_ctl;
    double up = wall_s() - spawned_at;
    child_pid = 0;
    child_ctl = Ctl_None;
    generation++;
    pthread_cond_broadcast(&cv);

    int expected = suspended || want != died;
    fprintf(stderr,
            "forgectrl: super: %s controller exited (status 0x%x, up %.0f s)%s\n",
            ctl_name(died), (unsigned)status, up,
            expected ? "" : " - unexpected");
    if (expected)
        return;

    /* Safe posture first. Today the kernel dead-man on the child's own
     * fd already stopped motion and locked the latch; under the device
     * broker these writes become the real mechanism. */
    wr_attr("cnc/stop", "1");
    wr_attr("cnc/laser_latch", "1");

    if (up >= HEALTHY_UPTIME_S)
        backoff_s = RESPAWN_MIN_S;
    respawn_at = wall_s() + backoff_s;
    fprintf(stderr, "forgectrl: super: respawn in %u s\n", backoff_s);
    if (backoff_s < RESPAWN_MAX_S)
        backoff_s *= 2;
}

/* Stop the child: SIGTERM its group, escalate to SIGKILL. Called with
 * mu held; drops and retakes the lock while waiting (the thread's
 * waitpid runs reap_locked). */
static void stop_locked(void)
{
    if (child_pid <= 0)
        return;
    pid_t pid = child_pid;
    fprintf(stderr, "forgectrl: super: stopping %s controller (pid %d)\n",
            ctl_name(child_ctl), (int)pid);
    kill(-pid, SIGTERM);
    kill(pid, SIGTERM);

    double deadline = wall_s() + STOP_TERM_WAIT_S;
    while (child_pid == pid) {
        pthread_mutex_unlock(&mu);
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);
        pthread_mutex_lock(&mu);
        if (r == pid) {
            reap_locked(status);
            break;
        }
        if (wall_s() > deadline) {
            fprintf(stderr, "forgectrl: super: escalating to SIGKILL\n");
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            deadline = wall_s() + STOP_TERM_WAIT_S;
        }
        pthread_mutex_unlock(&mu);
        usleep(100 * 1000);
        pthread_mutex_lock(&mu);
    }
}

/* ------------------------------------------------------ thread body */

static void *super_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&mu);
    while (th_run) {
        /* Reap. */
        if (child_pid > 0) {
            pid_t pid = child_pid;
            pthread_mutex_unlock(&mu);
            int status;
            pid_t r = waitpid(pid, &status, WNOHANG);
            pthread_mutex_lock(&mu);
            if (r == pid)
                reap_locked(status);
        }
        /* Standing by next to an unmanaged controller (found at start,
         * or left orphaned by a previous forgectrl): retake supervision
         * as soon as the machine is idle - a busy one is left to finish
         * its job first. */
        if (standby_takeover && child_pid == 0 && want != Ctl_None) {
            pthread_mutex_unlock(&mu);
            int idle = machine_is_idle() && !diag_running();
            if (idle)
                takeover_unmanaged();
            pthread_mutex_lock(&mu);
            if (idle) {
                standby_takeover = 0;
                suspended = 0;
            }
        }

        /* Converge on the wanted state. The first spawn after taking
         * the broker passes the motion-liveness gate first (unlocked -
         * the probe and its recovery ladder take a while). */
        if (child_pid > 0 && (suspended || child_ctl != want)) {
            stop_locked();
        } else if (child_pid == 0 && !suspended && !motion_fault
                   && want != Ctl_None && wall_s() >= respawn_at) {
            broker_open_locked();
            if (broker_fd >= 0 && !probed) {
                int fd = broker_fd;
                pthread_mutex_unlock(&mu);
                int ok = probe_sequence(fd);
                pthread_mutex_lock(&mu);
                probed = ok;
                motion_fault = !ok;
            } else
                spawn_locked(want);
        }
        pthread_mutex_unlock(&mu);
        usleep(200 * 1000);
        pthread_mutex_lock(&mu);
    }
    pthread_mutex_unlock(&mu);
    return NULL;
}

/* Wait until the lifecycle state satisfies pred (generation-driven).
 * Called with mu held; returns 0, or -1 on timeout. */
static int wait_for(int (*pred)(void), double timeout_s)
{
    double deadline = wall_s() + timeout_s;
    while (!pred()) {
        if (wall_s() > deadline)
            return -1;
        pthread_mutex_unlock(&mu);
        usleep(100 * 1000);
        pthread_mutex_lock(&mu);
    }
    return 0;
}

static int pred_child_gone(void)  { return child_pid == 0; }

/* --------------------------------------------------------------- api */

static ctl_t configured_mode(void)
{
    char v[16];
    if (settings_get("controller_mode", v, sizeof(v)) == 0
        && strcmp(v, "cloud") == 0)
        return Ctl_Cloud;
    return Ctl_Grbl;
}

/* The bracketed patterns keep pgrep/pkill -f from matching their own
 * sh -c wrapper. */
static int unmanaged_controller_running(void)
{
    return system("pgrep -x grblHAL_glowfor >/dev/null 2>&1") == 0 ||
           system("pgrep -f '[g]fcloud\\.py' >/dev/null 2>&1") == 0;
}

void super_init(void)
{
    pthread_mutex_lock(&mu);
    want = configured_mode();
    if (unmanaged_controller_running()) {
        suspended = 1;
        standby_takeover = 1;
        fprintf(stderr, "forgectrl: super: unmanaged controller already "
                        "running - standing by until the machine is idle\n");
    }
    th_run = 1;
    pthread_mutex_unlock(&mu);
    if (pthread_create(&th, NULL, super_main, NULL) != 0) {
        th_run = 0;
        fprintf(stderr, "forgectrl: super: thread failed to start\n");
        return;
    }
    fprintf(stderr, "forgectrl: super: supervising mode %s%s\n",
            ctl_name(want), suspended ? " (standby)" : "");
}

void super_shutdown(void)
{
    pthread_mutex_lock(&mu);
    int was = th_run;
    th_run = 0;
    suspended = 1;
    if (child_pid > 0) {
        /* A busy controller survives a forgectrl stop: it reparents to
         * init and finishes its job (its own device fd carries the
         * dead-man). The restarted supervisor finds it unmanaged and
         * stands by. Idle controllers stop with us. */
        if (machine_is_idle())
            stop_locked();
        else
            fprintf(stderr, "forgectrl: super: machine busy - leaving %s "
                            "controller running (pid %d, unmanaged)\n",
                    ctl_name(child_ctl), (int)child_pid);
    }
    /* Our reference goes away either way. Idle: the device closes and
     * the kernel locks the latch. Busy-orphan: the child's dup keeps
     * the description open (job survives), and the child's own exit
     * then IS the final close - the kernel dead-man backstop. */
    if (broker_fd >= 0) {
        close(broker_fd);
        broker_fd = -1;
    }
    pthread_mutex_unlock(&mu);
    if (was)
        pthread_join(th, NULL);
}

int super_mode_switch(const char *mode, char *err, size_t elen)
{
    ctl_t target;
    if (!strcmp(mode, "grbl"))
        target = Ctl_Grbl;
    else if (!strcmp(mode, "cloud"))
        target = Ctl_Cloud;
    else {
        snprintf(err, elen, "mode must be grbl or cloud");
        return -1;
    }

    if (diag_running()) {
        snprintf(err, elen, "a diagnostic is running");
        return -1;
    }
    if (!machine_is_idle()) {
        snprintf(err, elen, "machine is not idle");
        return -1;
    }

    pthread_mutex_lock(&mu);
    int takeover = suspended && unmanaged_controller_running();
    pthread_mutex_unlock(&mu);
    if (takeover)
        takeover_unmanaged();

    if (settings_set("controller_mode", mode) != 0) {
        snprintf(err, elen, "cannot persist controller_mode");
        return -1;
    }

    pthread_mutex_lock(&mu);
    want = target;
    suspended = 0;
    standby_takeover = 0;
    motion_fault = 0;   /* a switch is also the retry lever after a fault */
    /* The thread stops the old controller and starts the new one; wait
     * until the running child IS the target. A pending liveness
     * (re-)probe runs first, so allow for it. */
    double sw_deadline = wall_s() + 90.0;
    while (!(child_pid > 0 && child_ctl == target)) {
        if (wall_s() > sw_deadline) {
            pthread_mutex_unlock(&mu);
            snprintf(err, elen, "controller did not start");
            return -1;
        }
        pthread_mutex_unlock(&mu);
        usleep(100 * 1000);
        pthread_mutex_lock(&mu);
    }
    double t_spawn = spawned_at;
    pthread_mutex_unlock(&mu);

    /* Wait for the controller to report in to the cooling engine - a
     * report NEWER than the spawn, so the previous controller's last
     * report cannot satisfy the wait. The cloud client signs into the
     * web service first, so give it time; a slow first report is a
     * warning, not a failure - but a child that keeps dying (bad
     * binary, immediate crash) is one. */
    double deadline = wall_s() + REPORT_WAIT_S;
    while (wall_s() < deadline) {
        double age = cool_report_age();
        if (age >= 0.0 && age < wall_s() - t_spawn)
            return 0;
        pthread_mutex_lock(&mu);
        int dead = child_pid == 0 || child_ctl != target;
        pthread_mutex_unlock(&mu);
        if (dead) {
            snprintf(err, elen,
                     "%s controller keeps exiting - check its log", mode);
            return -1;
        }
        usleep(500 * 1000);
    }
    fprintf(stderr, "forgectrl: super: %s controller running but no "
                    "job-state report yet\n", mode);
    return 0;
}

int super_controller_stop(void)
{
    pthread_mutex_lock(&mu);
    suspended = 1;
    int ok = wait_for(pred_child_gone, 15.0) == 0;
    pthread_mutex_unlock(&mu);
    return ok ? 0 : -1;
}

void super_controller_start(void)
{
    pthread_mutex_lock(&mu);
    suspended = 0;
    motion_fault = 0;
    respawn_at = 0.0;
    backoff_s = RESPAWN_MIN_S;
    pthread_mutex_unlock(&mu);
}

int super_status_json(char *buf, size_t len)
{
    pthread_mutex_lock(&mu);
    snprintf(buf, len,
             "{\"mode\":\"%s\",\"controller\":\"%s\",\"pid\":%d,"
             "\"motion\":\"%s\"}",
             ctl_name(want),
             child_pid > 0 ? "running"
                 : motion_fault ? "motion-fault"
                 : suspended ? "standby" : "stopped",
             (int)child_pid,
             motion_fault ? "fault" : probed ? "verified" : "unverified");
    pthread_mutex_unlock(&mu);
    return 0;
}
