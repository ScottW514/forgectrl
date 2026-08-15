cmake_minimum_required(VERSION 3.10)
project(forgectrl C)

set(CMAKE_C_STANDARD 11)

add_executable(forgectrl
  src/main.c
  src/auth.c
  src/cam.c
  src/cool.c
  src/debayer.c
  src/diag.c
  src/fflog.c
  src/liveness.c
  src/logs.c
  src/sanitize.c
  src/settings.c
  src/status.c
  src/super.c
  src/ui.c
  src/update.c
  src/vpu_jpeg.c
)
target_compile_options(forgectrl PRIVATE -Wall -Wextra -O2)
target_link_libraries(forgectrl ulfius jpeg z pthread m)

install(TARGETS forgectrl RUNTIME DESTINATION bin)

# Host unit test for the machine_is_idle() fail-closed contract (X-2):
# a read failure - including fd exhaustion under a connection flood -
# must report not-idle. Built everywhere, installed nowhere; CI runs it
# after the build.
add_executable(status_idle_test tests/status_idle_test.c src/status.c)
target_compile_options(status_idle_test PRIVATE -Wall -Wextra)
target_link_libraries(status_idle_test m)

# Host unit test for the log-export sanitizer: every identifying class
# redacted, ordinary log text untouched, stable placeholders, idempotent.
# A sanitized bundle is what gets attached to public issue reports.
add_executable(sanitize_test tests/sanitize_test.c src/sanitize.c)
target_compile_options(sanitize_test PRIVATE -Wall -Wextra)

# Emitter half of the host end-to-end logging check (tests/fflog_e2e.sh
# drives it against a private rsyslogd; manual, not CI).
add_executable(fflog_e2e_test tests/fflog_e2e_test.c src/fflog.c)
target_compile_options(fflog_e2e_test PRIVATE -Wall -Wextra)
target_link_libraries(fflog_e2e_test pthread)
