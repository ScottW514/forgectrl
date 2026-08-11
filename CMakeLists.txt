cmake_minimum_required(VERSION 3.10)
project(forgectrl C)

set(CMAKE_C_STANDARD 11)

add_executable(forgectrl
  src/main.c
  src/cam.c
  src/cool.c
  src/debayer.c
  src/diag.c
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
