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
 * is exactly one waitpid() caller for the controller pid. Every
 * transition out of a running child - expected or not, including
 * SIGKILL escalation - safes the machine (cnc/stop + laser latch):
 * under the broker a child exit is not a final close of the pulse
 * device, so these writes are the safing mechanism. Unexpected deaths
 * additionally respawn with backoff.
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
#include "cam.h"
#include "cool.h"
#include "diag.h"
#include "fflog.h"
#include "liveness.h"
#include "settings.h"
#include "status.h"
#include "super.h"
#include "update.h"

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
#define CLOUD_BIN  "/usr/sbin/gfcloud.py"
#define PULSE_DEV  "/dev/glowforge"
#define HOMED_ANCHOR "/run/grblhal.homed"

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
static int probed = 0;             /* liveness gate passed since broker open */
static int probe_skipped = 0;      /* ...but the probe itself could not run */
static int motion_fault = 0;       /* probe failed after recovery - no spawn */
static int standby_takeover = 0;   /* unmanaged controller found: retake at idle */

static void wr_attr(const char *attr, const char *val);

/* Stop an unmanaged (legacy- or orphan-started) controller the legacy
 * way so the supervisor can take over. Called with mu NOT held. */
static void takeover_unmanaged(void)
{
    fflog(LOG_WARNING, "super: taking over from unmanaged "
                       "controller");
    (void)!system("/etc/init.d/grblhal stop >/dev/null 2>&1");
    (void)!system("/etc/init.d/gfcloud stop >/dev/null 2>&1");
    (void)!system("pkill -x grblHAL_glowfor 2>/dev/null");
    (void)!system("pkill -f '[g]fcloud\\.py' 2>/dev/null");
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
    /* A just-stopped legacy holder's close can lag its exit. O_CLOEXEC:
     * only the controller spawn clears the flag before exec, so helper
     * children (curl, fwup, media-ctl, pkill, ...) can never pin the
     * pulse device and defeat the kernel dead-man on the description. */
    for (int i = 0; i < 30 && broker_fd < 0; i++) {
        broker_fd = open(PULSE_DEV, O_WRONLY | O_CLOEXEC);
        if (broker_fd < 0)
            usleep(100 * 1000);
    }
    if (broker_fd < 0) {
        fflog(LOG_ERR, "super: cannot open " PULSE_DEV
                       " - controllers will self-open (no broker)");
        return;
    }
    if (flock(broker_fd, LOCK_EX) != 0)
        fflog(LOG_ERR, "super: flock on " PULSE_DEV " failed");
    probed = 0;     /* a fresh hold means unverified motion */
    probe_skipped = 0;
    fflog(LOG_INFO, "super: holding " PULSE_DEV " (broker)");
}

/* Motion-liveness gate, run with mu NOT held (takes seconds; the
 * recovery ladder takes a minute). The DRV8825 drivers can come out of
 * a rail power-up unserviceable; each recovery attempt gives them a
 * longer true power-off before re-probing. Returns 1 verified, 0 fault,
 * 2 when the probe could not run (no accelerometer, enclosure open): the
 * machine is not blocked, but motion stays UNVERIFIED and is reported so. */
static int probe_sequence(int fd)
{
    static const int ladder_s[] = { 0, 5, 15, 30 };
    char detail[96];

    for (size_t i = 0; i < sizeof(ladder_s) / sizeof(ladder_s[0]); i++) {
        if (ladder_s[i] > 0) {
            fflog(LOG_WARNING, "super: motion dead - rail off %d s "
                               "and re-probing (%zu/3)", ladder_s[i], i);
            wr_attr("cnc/disable", "1");
            sleep((unsigned)ladder_s[i]);
        }
        wr_attr("cnc/enable", "1");
        sleep(1);
        int rc = liveness_probe(fd, detail, sizeof(detail));
        fflog(LOG_INFO, "super: liveness probe: %s - %s",
              rc == 1 ? "MOTION OK" : rc == 0 ? "NO MOTION" : "ERROR",
              detail);
        if (rc == 1)
            return 1;
        if (rc < 0)
            return 2;   /* cannot probe (no accel?): do not block the machine */
    }
    fflog(LOG_CRIT, "super: MOTION FAULT - the stepper drivers "
                   "did not recover; a full power cycle may be required. "
                   "Controllers stay down (retry via POST /mode).");
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

extern char **environ;

/* The controller's environment, assembled in the parent: the daemon is
 * multithreaded, so between fork() and exec the child may only call
 * async-signal-safe functions - setenv() (malloc + the environ lock)
 * is not one, and a lock another thread held at fork time would hang
 * the child forever while it holds the broker fd. Returns a NULL-
 * terminated vector to free with free_child_env(), or NULL. */
static char **build_child_env(ctl_t ctl, int pulse_fd)
{
    size_t n = 0;
    while (environ && environ[n])
        n++;
    char **env = calloc(n + 3, sizeof(*env));
    if (!env)
        return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        /* Our own entries replace any inherited value of the same key. */
        if (!strncmp(environ[i], "GF_PULSE_FD=", 12) ||
            !strncmp(environ[i], "GFSINK=", 7))
            continue;
        env[k] = strdup(environ[i]);
        if (!env[k])
            goto fail;
        k++;
    }
    if (pulse_fd >= 0) {
        char fdv[32];
        snprintf(fdv, sizeof(fdv), "GF_PULSE_FD=%d", pulse_fd);
        if (!(env[k] = strdup(fdv)))
            goto fail;
        k++;
    }
    if (ctl == Ctl_Grbl) {
        if (!(env[k] = strdup("GFSINK=" PULSE_DEV)))
            goto fail;
        k++;
    }
    env[k] = NULL;
    return env;
fail:
    for (size_t i = 0; i < k; i++)
        free(env[i]);
    free(env);
    return NULL;
}

static void free_child_env(char **env)
{
    if (!env)
        return;
    for (size_t i = 0; env[i]; i++)
        free(env[i]);
    free(env);
}

/* A controller's stray stdout/stderr - interpreter tracebacks, library
 * messages, anything not sent through its own syslog emitter - flows
 * through a `logger` relay into syslog under the controller's program
 * name, so it lands in that controller's log directory. The relay is
 * double-forked (init reaps it) and lives as long as the pipe has a
 * writer, so it outlives this daemon whenever the controller does.
 * Returns the pipe's write end, or -1 (the controller then runs with
 * its output discarded rather than not at all). */
static int spawn_output_relay(const char *tag)
{
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        return -1;
    }
    if (pid == 0) {
        pid_t g = fork();
        if (g != 0)
            _exit(g < 0 ? 127 : 0);
        dup2(p[0], 0);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) {
            dup2(nul, 1);
            dup2(nul, 2);
        }
        long maxfd = sysconf(_SC_OPEN_MAX);
        if (maxfd < 0 || maxfd > 4096)
            maxfd = 4096;
        for (int fd = 3; fd < (int)maxfd; fd++)
            close(fd);
        /* Reclaimed no sooner than the controller it serves: a dead
         * relay turns the controller's stray writes into EPIPE. */
        int ofd = open("/proc/self/oom_score_adj", O_WRONLY);
        if (ofd >= 0) {
            (void)!write(ofd, "-500", 4);
            close(ofd);
        }
        execl("/usr/bin/logger", "logger", "-t", tag, "-p",
              "daemon.warning", (char *)NULL);
        _exit(127);
    }
    close(p[0]);
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    return p[1];
}

/* Called with mu held. */
static void spawn_locked(ctl_t ctl)
{
    broker_open_locked();

    char **env = build_child_env(ctl, broker_fd);
    if (!env) {
        fflog(LOG_ERR, "super: cannot build the controller "
                       "environment: %s", strerror(errno));
        respawn_at = wall_s() + backoff_s;
        return;
    }

    int lfd = spawn_output_relay(ctl == Ctl_Grbl ? "grblhal" : "gfcloud");
    pid_t pid = fork();
    if (pid < 0) {
        fflog(LOG_ERR, "super: fork failed: %s",
              strerror(errno));
        free_child_env(env);
        if (lfd >= 0)
            close(lfd);
        respawn_at = wall_s() + backoff_s;
        return;
    }
    if (pid == 0) {
        /* Child: async-signal-safe calls only from here to exec. Own
         * process group so stop() can signal helpers too. */
        setpgid(0, 0);
        if (lfd >= 0) {
            dup2(lfd, 1);
            dup2(lfd, 2);
        }
        if (broker_fd >= 0) {
            /* The broker fd is opened O_CLOEXEC; a controller is the
             * one child that must inherit it across exec. */
            fcntl(broker_fd, F_SETFD, 0);
        }
        /* The broker fd is the only descriptor a controller inherits.
         * Everything else the daemon holds - listening and client
         * sockets, capture and encoder nodes - would otherwise stay
         * pinned for the whole life of the child. */
        long maxfd = sysconf(_SC_OPEN_MAX);
        if (maxfd < 0 || maxfd > 4096)
            maxfd = 4096;
        for (int fd = 3; fd < (int)maxfd; fd++)
            if (fd != broker_fd)
                close(fd);
        /* A controller must be a less-preferred OOM victim than
         * ordinary processes, but a MORE preferred one than the daemon
         * (which is its dead-man and respawns it). */
        int ofd = open("/proc/self/oom_score_adj", O_WRONLY);
        if (ofd >= 0) {
            (void)!write(ofd, "-500", 4);
            close(ofd);
        }
        if (ctl == Ctl_Grbl) {
            if (chdir("/data") != 0)
                _exit(126);
            execle(GRBL_BIN, GRBL_BIN, "-p", "23",
                   "-e", "/data/EEPROM-glowforge.DAT", (char *)NULL, env);
        } else {
            execle(CLOUD_BIN, CLOUD_BIN, (char *)NULL, env);
        }
        _exit(127);
    }
    free_child_env(env);
    if (lfd >= 0)
        close(lfd);         /* the controller holds the only writer now */
    child_pid = pid;
    child_ctl = ctl;
    spawned_at = wall_s();
    generation++;
    fflog(LOG_NOTICE, "super: started %s controller (pid %d)",
          ctl_name(ctl), (int)pid);
    /* The idle lid lamp is asserted around every spawn: a cloud client
     * drives its own level while it runs and leaves it behind. */
    cam_lamp_apply_idle();
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
    fflog(expected ? LOG_NOTICE : LOG_WARNING,
          "super: %s controller exited (status 0x%x, up %.0f s)%s",
          ctl_name(died), (unsigned)status, up,
          expected ? "" : " - unexpected");

    /* Safe posture on EVERY transition out of a running child, expected
     * or not. Under the device broker a child's exit is not a final
     * close of the pulse device, so the kernel's close-relocks backstop
     * never fires for managed controllers - these two writes are the
     * real safing mechanism, and both are harmless when the machine is
     * already idle and latched. */
    wr_attr("cnc/stop", "1");
    wr_attr("cnc/laser_latch", "1");
    /* The homing anchor belongs to the controller that wrote it: it
     * must not survive into another mode (cloud re-zeros the counters
     * it anchors) or into a respawn. A fresh GRBL controller starts
     * unreferenced and re-homes. */
    unlink(HOMED_ANCHOR);

    if (expected)
        return;

    if (up >= HEALTHY_UPTIME_S)
        backoff_s = RESPAWN_MIN_S;
    respawn_at = wall_s() + backoff_s;
    fflog(LOG_WARNING, "super: respawn in %u s", backoff_s);
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
    fflog(LOG_INFO, "super: stopping %s controller (pid %d)",
          ctl_name(child_ctl), (int)pid);
    /* Safe the machine BEFORE the signal, not only after the reap: this
     * path is also the emergency lever (POST /controller/stop is not
     * idle-gated), and a controller that is mid-job may take its whole
     * SIGTERM grace to leave. cnc/stop is a controlled deceleration and
     * the latch relock severs FIRE - both instantaneous at the kernel,
     * both harmless no-ops when the machine is already idle and latched. */
    wr_attr("cnc/stop", "1");
    wr_attr("cnc/laser_latch", "1");
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
        /* Reaped meanwhile by the supervisor thread (shutdown runs this
         * from the caller's thread while that loop may still be
         * draining): the pid is free for reuse and must not be
         * signaled again. */
        if (child_pid != pid || (r < 0 && errno == ECHILD))
            break;
        if (wall_s() > deadline) {
            fflog(LOG_WARNING, "super: escalating to SIGKILL");
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            /* A SIGKILLed child can run no cleanup of its own - safe
             * the machine now rather than waiting for the reap. */
            wr_attr("cnc/stop", "1");
            wr_attr("cnc/laser_latch", "1");
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
                int rc = probe_sequence(fd);
                pthread_mutex_lock(&mu);
                probed = rc != 0;
                probe_skipped = rc == 2;
                motion_fault = rc == 0;
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
        fflog(LOG_WARNING, "super: unmanaged controller already "
                           "running - standing by until the machine is idle");
    }
    th_run = 1;
    pthread_mutex_unlock(&mu);
    if (pthread_create(&th, NULL, super_main, NULL) != 0) {
        th_run = 0;
        fflog(LOG_ERR, "super: thread failed to start");
        return;
    }
    fflog(LOG_INFO, "super: supervising mode %s%s",
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
            fflog(LOG_WARNING, "super: machine busy - leaving %s "
                               "controller running (pid %d, unmanaged)",
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
    if (update_job_running()) {
        snprintf(err, elen, "an update job is running");
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
    fflog(LOG_INFO, "super: %s controller running but no "
                    "job-state report yet", mode);
    return 0;
}

int super_controller_stop(void)
{
    pthread_mutex_lock(&mu);
    suspended = 1;
    int ok = wait_for(pred_child_gone, 15.0) == 0;
    if (!ok)
        suspended = 0;  /* takeover failed: resume normal supervision so
                         * the machine is never left controller-less */
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
             motion_fault ? "fault"
                 : probed && !probe_skipped ? "verified" : "unverified");
    pthread_mutex_unlock(&mu);
    return 0;
}
