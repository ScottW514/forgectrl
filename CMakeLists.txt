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
  src/liveness.c
  src/settings.c
  src/status.c
  src/super.c
  src/ui.c
  src/update.c
  src/vpu_jpeg.c
)
target_compile_options(forgectrl PRIVATE -Wall -Wextra -O2)
target_link_libraries(forgectrl ulfius jpeg pthread m)

install(TARGETS forgectrl RUNTIME DESTINATION bin)

# Host unit test for the machine_is_idle() fail-closed contract (X-2):
# a read failure - including fd exhaustion under a connection flood -
# must report not-idle. Built everywhere, installed nowhere; CI runs it
# after the build.
add_executable(status_idle_test tests/status_idle_test.c src/status.c)
target_compile_options(status_idle_test PRIVATE -Wall -Wextra)
target_link_libraries(status_idle_test m)
