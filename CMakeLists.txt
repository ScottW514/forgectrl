cmake_minimum_required(VERSION 3.10)
project(forgectrl C)

set(CMAKE_C_STANDARD 11)

# The control panel is a normal static page under src/ui/ (index.html,
# theme.css, help.js, forms.js, panel.js, and Bootstrap in vendor/).
# embed.cmake bundles it into one self-contained page, gzips it, and
# emits the compressed bytes as `index_html_gz[]` (see src/ui.h); the
# bundled page itself lands in build/ui/index.html for inspection.
set(UI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/ui)
set(UI_OUT ${CMAKE_CURRENT_BINARY_DIR}/ui)
add_custom_command(
  OUTPUT ${UI_OUT}/ui_blob.c ${UI_OUT}/index.html
  COMMAND ${CMAKE_COMMAND} -E make_directory ${UI_OUT}
  COMMAND ${CMAKE_COMMAND} -DUI_DIR=${UI_DIR} -DOUT_HTML=${UI_OUT}/index.html
          -DOUT_C=${UI_OUT}/ui_blob.c -P ${UI_DIR}/embed.cmake
  DEPENDS ${UI_DIR}/index.html ${UI_DIR}/theme.css ${UI_DIR}/help.js
          ${UI_DIR}/forms.js ${UI_DIR}/panel.js
          ${UI_DIR}/vendor/bootstrap.min.css
          ${UI_DIR}/vendor/bootstrap.bundle.min.js
          ${UI_DIR}/embed.cmake
  COMMENT "Bundling the control panel (src/ui -> index_html_gz[])"
  VERBATIM)

add_executable(forgectrl
  src/main.c
  src/airflow.c
  src/auth.c
  src/cam.c
  src/peer.c
  src/camhealth.c
  src/cool.c
  src/coolfmt.c
  src/debayer.c
  src/diag.c
  src/fflog.c
  src/gates.c
  src/gpu_debayer.c
  src/ipu_copy.c
  src/liveness.c
  src/logs.c
  src/mp4mux.c
  src/sanitize.c
  src/settings.c
  src/status.c
  src/super.c
  src/update.c
  src/vpu_h264.c
  src/vpu_jpeg.c
  ${UI_OUT}/ui_blob.c
)
target_include_directories(forgectrl PRIVATE src)
target_compile_options(forgectrl PRIVATE -Wall -Wextra -O2)
# dl: the GPU demosaic loads libEGL/libGLESv2 at runtime, so the build
# has no GL dependency and images without Mesa fall back to NEON.
target_link_libraries(forgectrl ulfius jpeg z pthread m ${CMAKE_DL_LIBS})

install(TARGETS forgectrl RUNTIME DESTINATION bin)

# Host unit test for the machine_is_idle() fail-closed contract (X-2):
# a read failure - including fd exhaustion under a connection flood -
# must report not-idle. Built everywhere, installed nowhere; CI runs it
# after the build.
add_executable(status_idle_test tests/status_idle_test.c src/status.c)
target_compile_options(status_idle_test PRIVATE -Wall -Wextra)
target_link_libraries(status_idle_test m pthread)

# Host unit test for the machine status JSON: the sys block reports
# memory utilization at once and CPU utilization once primed by a prior
# read, and the document stays inside the handler's buffer. CI runs it
# after the build.
add_executable(status_sys_test tests/status_sys_test.c src/status.c)
target_compile_options(status_sys_test PRIVATE -Wall -Wextra)
target_link_libraries(status_sys_test m pthread)

# Host unit test for the log-export sanitizer: every identifying class
# redacted, ordinary log text untouched, stable placeholders, idempotent.
# A sanitized bundle is what gets attached to public issue reports.
add_executable(sanitize_test tests/sanitize_test.c src/sanitize.c)
target_compile_options(sanitize_test PRIVATE -Wall -Wextra)

# Host unit test for the camera privacy gate's lid read: every way it
# can fail must land on "not closed", or the cameras could capture with
# the enclosure open. CI runs it after the build.
add_executable(lid_gate_test tests/lid_gate_test.c src/status.c)
target_compile_options(lid_gate_test PRIVATE -Wall -Wextra)
target_link_libraries(lid_gate_test m pthread)

# Host unit test for the raw-frame front end: the 10-bit -> 8-bit
# narrowing the 8 MP (OV8856) capture path depends on, and the Bayer
# phase surviving it. CI runs it after the build.
add_executable(debayer_test tests/debayer_test.c src/debayer.c)
target_compile_options(debayer_test PRIVATE -Wall -Wextra)

# Host unit test for the capture frame-health ladder: a frame the queue
# flagged errored must never be demosaiced, a run of them must escalate to
# a stream restart and then stop being retried, and one usable frame must
# put the ladder back at the bottom. CI runs it after the build.
add_executable(camhealth_test tests/camhealth_test.c src/camhealth.c)
target_compile_options(camhealth_test PRIVATE -Wall -Wextra)

# Host unit test for the gate-settings table the cooling gates, their
# validators, the settings reply and the panel all read: the legal
# range, the recommended band, the off end, and the two JSON shapes.
# CI runs it after the build.
add_executable(cool_gate_test tests/cool_gate_test.c src/gates.c)
target_compile_options(cool_gate_test PRIVATE -Wall -Wextra)
target_link_libraries(cool_gate_test m)

# Host unit test for the flow check's reading: the baseline as the mean
# of the settled window, the end as the mean of the last samples, the
# tube's share subtracted from the rise and bounded, and a stopped pump
# still read as stopped under a lit tube. Includes the engine source
# against a fake sysfs tree and a fake clock; CI runs it after the build.
add_executable(cool_flow_test tests/cool_flow_test.c src/gates.c src/airflow.c src/coolfmt.c)
target_compile_options(cool_flow_test PRIVATE -Wall -Wextra)
target_link_libraries(cool_flow_test m pthread)

# Host unit test for the per-fan airflow gate: grace, the three-tick
# debounce, the latch, off by value, and the tach conversions. CI runs
# it after the build.
add_executable(airflow_test tests/airflow_test.c src/airflow.c)
target_compile_options(airflow_test PRIVATE -Wall -Wextra)
target_link_libraries(airflow_test m)

# Host unit test for the /cool/status document: the widest legal
# rendering of every fragment fits the buffer the engine uses, the
# widest document fits COOL_STATUS_JSON_MAX and parses, and an oversize
# value set is refused rather than served cut off.
add_executable(coolfmt_test tests/coolfmt_test.c src/coolfmt.c)
target_compile_options(coolfmt_test PRIVATE -Wall -Wextra)
target_link_libraries(coolfmt_test m)

# Host unit test for the loopback peer check behind the cooling report
# channel: every spelling of this host passes (127/8, ::1, the v4-mapped
# ::ffff:127/8 a dual-stack listener reports for an IPv4 client); every
# other address, another family and a truncated sockaddr fail closed.
add_executable(auth_peer_test tests/auth_peer_test.c src/peer.c)
target_compile_options(auth_peer_test PRIVATE -Wall -Wextra)

# Host unit test for the fragmented-MP4 muxer behind /cam/h264: the init
# segment's box tree and avcC must match the fed SPS/PPS, fragments must
# carry the right sizes, offsets and sample flags, and the Annex B
# reframing must drop parameter sets and length-prefix the rest. CI runs
# it after the build.
add_executable(mp4mux_test tests/mp4mux_test.c src/mp4mux.c)
target_compile_options(mp4mux_test PRIVATE -Wall -Wextra)

# Emitter half of the host end-to-end logging check (tests/fflog_e2e.sh
# drives it against a private rsyslogd; manual, not CI).
add_executable(fflog_e2e_test tests/fflog_e2e_test.c src/fflog.c)
target_compile_options(fflog_e2e_test PRIVATE -Wall -Wextra)
target_link_libraries(fflog_e2e_test pthread)
