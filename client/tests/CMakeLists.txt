cmake_minimum_required(VERSION 3.10)

if(WIN32)
  # the tests are written for Linux: this adds sys/mman.h for the ones that
  # host an LGMP session in anonymous memory, and alarm() as a watchdog
  include_directories(BEFORE "${CMAKE_CURRENT_SOURCE_DIR}/windows")

  # The tests link client sources that call more than the test provides, and
  # rely on --gc-sections to drop what they do not reach. On x86-64 Windows the
  # linker keeps every function that has unwind tables, so build without them.
  add_compile_options(-fno-asynchronous-unwind-tables -fno-unwind-tables)

  add_library(lg_test_windows OBJECT windows/support.c)
  link_libraries($<TARGET_OBJECTS:lg_test_windows> bcrypt)
endif()

add_executable(font-tests
  font_test.c
  ../src/font.c
)

add_executable(sw-surface-tests
  sw_surface_test.c
  ../src/sw_surface.c
)
target_include_directories(sw-surface-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(sw-surface-tests
  ${EXE_FLAGS}
  lg_common
)
add_test(NAME sw-surface-tests COMMAND sw-surface-tests)
set_tests_properties(sw-surface-tests PROPERTIES TIMEOUT 10)

add_executable(time-tests time_test.c)
target_link_libraries(time-tests
  ${EXE_FLAGS}
  lg_common
)
add_test(NAME time-tests COMMAND time-tests)
set_tests_properties(time-tests PROPERTIES TIMEOUT 20)

add_executable(option-tests option_test.c)
target_link_libraries(option-tests
  ${EXE_FLAGS}
  lg_common
)
add_test(NAME option-tests COMMAND option-tests)
set_tests_properties(option-tests PROPERTIES TIMEOUT 20)

add_executable(stringutils-tests stringutils_test.c)
target_link_libraries(stringutils-tests
  ${EXE_FLAGS}
  lg_common
)
add_test(NAME stringutils-tests COMMAND stringutils-tests)
set_tests_properties(stringutils-tests PROPERTIES TIMEOUT 20)

add_executable(placement-tests placement_test.c)
target_link_libraries(placement-tests
  ${EXE_FLAGS}
)
add_test(NAME placement-tests COMMAND placement-tests)
set_tests_properties(placement-tests PROPERTIES TIMEOUT 20)

add_executable(clipboard-format-tests
  clipboard_format_test.c
  ../displayservers/Win32/clipboard_format.c
)
target_link_libraries(clipboard-format-tests
  ${EXE_FLAGS}
)
add_test(NAME clipboard-format-tests COMMAND clipboard-format-tests)
set_tests_properties(clipboard-format-tests PROPERTIES TIMEOUT 60)

add_executable(win32-clipboard-tests
  win32_clipboard_test.c
  ../displayservers/Win32/clipboard.c
  ../displayservers/Win32/clipboard_format.c
)
target_include_directories(win32-clipboard-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../include"
)
target_link_libraries(win32-clipboard-tests
  ${EXE_FLAGS}
  lg_common
)
add_test(NAME win32-clipboard-tests COMMAND win32-clipboard-tests)
set_tests_properties(win32-clipboard-tests PROPERTIES TIMEOUT 60)

if(WIN32)
  add_executable(watchdog-tests watchdog_test.c)
  target_link_libraries(watchdog-tests
    ${EXE_FLAGS}
    lg_common
  )
  add_test(NAME watchdog-tests COMMAND watchdog-tests)
  set_tests_properties(watchdog-tests PROPERTIES TIMEOUT 60)

  add_executable(power-tests power_test.c)
  target_link_libraries(power-tests
    ${EXE_FLAGS}
    lg_common
  )
  add_test(NAME power-tests COMMAND power-tests)
  set_tests_properties(power-tests PROPERTIES TIMEOUT 20)

  # Windows' own codec decodes a PNG that the guest copied
  add_executable(clipboard-wic-tests
    clipboard_wic_test.c
    ../displayservers/Win32/clipboard_wic.c
    ../displayservers/Win32/clipboard_format.c
  )
  target_link_libraries(clipboard-wic-tests
    ${EXE_FLAGS}
    ole32
  )
  add_test(NAME clipboard-wic-tests COMMAND clipboard-wic-tests)
  set_tests_properties(clipboard-wic-tests PROPERTIES TIMEOUT 60)

  # Windows' own clipboard, in a window station that the test makes
  add_executable(win32-clipboard-real-tests
    win32_clipboard_real_test.c
    ../displayservers/Win32/clipboard.c
    ../displayservers/Win32/clipboard_format.c
    ../displayservers/Win32/clipboard_wic.c
    ../displayservers/Win32/clipboard_win32.c
  )
  target_include_directories(win32-clipboard-real-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
  )
  target_link_libraries(win32-clipboard-real-tests
    ${EXE_FLAGS}
    lg_common
    user32
  )
  add_test(NAME win32-clipboard-real-tests COMMAND win32-clipboard-real-tests)
  set_tests_properties(win32-clipboard-real-tests PROPERTIES TIMEOUT 120)
endif()

target_compile_definitions(font-tests PRIVATE
  FONT_TEST_FILE="${PROJECT_TOP}/repos/gui/cimgui/imgui/misc/fonts/DroidSans.ttf"
)
target_include_directories(font-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../include"
)
add_test(NAME font-tests
  COMMAND font-tests
)
set_tests_properties(font-tests PROPERTIES
  TIMEOUT 10
)

add_executable(lgmp-transport-tests
  lgmp_transport_test.c
  ../src/kb_hid.c
)
target_link_libraries(lgmp-transport-tests
  ${EXE_FLAGS}
  transport_LGMP
  lg_common
  lgmp
)
add_test(NAME lgmp-transport-tests
  COMMAND lgmp-transport-tests
)
set_tests_properties(lgmp-transport-tests PROPERTIES
  TIMEOUT 10
)

add_executable(lgmp-frame-tests
  lgmp_frame_test.c
  ../src/kb_hid.c
)
target_link_libraries(lgmp-frame-tests
  ${EXE_FLAGS}
  transport_LGMP
  lg_common
  lgmp
)
set(LGMP_FRAME_CASES
  malformed
  serial-damage
  lanes
  leases
  dma
)
foreach(name IN LISTS LGMP_FRAME_CASES)
  add_test(NAME lgmp-frame-${name}
    COMMAND lgmp-frame-tests ${name}
  )
  set_tests_properties(lgmp-frame-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

add_executable(lgmp-input-tests
  lgmp_input_test.c
  ../transports/LGMP/input.c
  ../src/kb_hid.c
)
target_include_directories(lgmp-input-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(lgmp-input-tests
  ${EXE_FLAGS}
  lg_common
  lgmp
)
set(LGMP_INPUT_CASES
  claim
  blocked
  restart
  pressure
  idle
  disconnect
)
foreach(name IN LISTS LGMP_INPUT_CASES)
  add_test(NAME lgmp-input-${name}
    COMMAND lgmp-input-tests ${name}
  )
  set_tests_properties(lgmp-input-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

add_executable(lgmp-clipboard-tests
  lgmp_clipboard_test.c
  ../transports/LGMP/clipboard.c
)
target_include_directories(lgmp-clipboard-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../transports/LGMP"
)
target_link_libraries(lgmp-clipboard-tests
  ${EXE_FLAGS}
  lg_common
  lgmp
)
set(LGMP_CLIPBOARD_CASES
  claim
  offer-request
  inbound-stream
  outbound-stream
  blocked
  cancel
  restart
  process-restart
  file-stream
  file-limits
  file-owner-loss
  file-malformed
  disconnect
  malformed
  file-format
)
foreach(name IN LISTS LGMP_CLIPBOARD_CASES)
  add_test(NAME lgmp-clipboard-${name}
    COMMAND lgmp-clipboard-tests ${name}
  )
  set_tests_properties(lgmp-clipboard-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

add_executable(wayland-motion-tests
  motion_test.c
)
target_include_directories(wayland-motion-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../displayservers/Wayland"
)
add_test(NAME wayland-motion-tests
  COMMAND wayland-motion-tests
)
set_tests_properties(wayland-motion-tests PROPERTIES
  TIMEOUT 10
)

if(ENABLE_WAYLAND)
  add_executable(wayland-presentation-tests
    wayland_presentation_test.c
    ../displayservers/Wayland/presentation.c
  )
  add_dependencies(wayland-presentation-tests wayland_protocol)
  target_include_directories(wayland-presentation-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../displayservers/Wayland"
    "${CMAKE_BINARY_DIR}/wayland"
  )
  target_link_libraries(wayland-presentation-tests
    ${EXE_FLAGS}
    lg_common
  )
  set(WAYLAND_PRESENTATION_CASES
    feedback-first
    swap-first
    discard
    swap-fail
    token-zero
    invalid-time
    free-pending
  )
  foreach(name IN LISTS WAYLAND_PRESENTATION_CASES)
    add_test(NAME wayland-presentation-${name}
      COMMAND wayland-presentation-tests ${name}
    )
    set_tests_properties(wayland-presentation-${name} PROPERTIES
      TIMEOUT 10
    )
  endforeach()

  add_executable(wayland-clipboard-tests
    wayland_clipboard_test.c
    ../displayservers/Wayland/clipboard.c
  )
  add_dependencies(wayland-clipboard-tests wayland_protocol)
  target_include_directories(wayland-clipboard-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../displayservers/Wayland"
    "${CMAKE_BINARY_DIR}/wayland"
  )
  target_link_libraries(wayland-clipboard-tests
    ${EXE_FLAGS}
    lg_common
  )
  set(WAYLAND_CLIPBOARD_CASES
    mime
    file-import
    file-source
    file-replace
    replace
    read-eof
    read-block
    read-empty
    read-ready
    read-error
    source
    source-release
    source-external
    source-replace
    source-cancel
    source-early
    source-error
    teardown
    teardown-external
    self-copy
    self-copy-late
    ignored
    invalidate-lock
    dnd
  )
  foreach(name IN LISTS WAYLAND_CLIPBOARD_CASES)
    add_test(NAME wayland-clipboard-${name}
      COMMAND wayland-clipboard-tests ${name}
    )
    set_tests_properties(wayland-clipboard-${name} PROPERTIES
      TIMEOUT 10
    )
  endforeach()
endif()

if(ENABLE_X11)
  add_executable(x11-clipboard-tests
    x11_clipboard_test.c
    ../displayservers/X11/clipboard.c
  )
  target_include_directories(x11-clipboard-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../displayservers/X11"
  )
  target_link_libraries(x11-clipboard-tests
    ${EXE_FLAGS}
    lg_common
    pthread
  )
  set(X11_CLIPBOARD_CASES
    targets
    file-import
    file-incr
    file-source
    file-source-active-replace
    normal
    incr
    incr-block
    incr-ready
    incr-cancel
    incr-large
    malformed
    replace
    source
    source-early
    teardown
  )
  foreach(name IN LISTS X11_CLIPBOARD_CASES)
    add_test(NAME x11-clipboard-${name}
      COMMAND x11-clipboard-tests ${name}
    )
    set_tests_properties(x11-clipboard-${name} PROPERTIES
      TIMEOUT 10
    )
  endforeach()
endif()

add_executable(displayserver-input-tests
  displayserver_input_test.c
  ../displayservers/Wayland/input_event.c
  ../displayservers/Win32/input_event.c
  ../displayservers/Win32/keymap.c
  ../displayservers/X11/input_event.c
  ../src/kb.c
)
target_compile_definitions(displayserver-input-tests PRIVATE
  CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
)
target_include_directories(displayserver-input-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../displayservers"
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
  "${PROJECT_TOP}/repos/gui/cimgui"
  "${PROJECT_TOP}/repos/gui/cimgui/imgui"
)

set(DISPLAYSERVER_INPUT_CASES
  pointer-events
  pointer-buttons
  pointer-release
  wheel-steps
  relative-motion
  keyboard-focus
  keyboard-keys
  keyboard-state
)

foreach(backend IN ITEMS wayland x11)
  foreach(name IN LISTS DISPLAYSERVER_INPUT_CASES)
    add_test(NAME display-input-${backend}-${name}
      COMMAND displayserver-input-tests ${backend} ${name}
    )
    set_tests_properties(display-input-${backend}-${name} PROPERTIES
      TIMEOUT 10
    )
  endforeach()
endforeach()

foreach(name IN ITEMS wheel-residual wheel-multiple)
  add_test(NAME display-input-wayland-${name}
    COMMAND displayserver-input-tests wayland ${name}
  )
  set_tests_properties(display-input-wayland-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

foreach(name IN ITEMS pointer-routing keyboard-routing held-keys)
  add_test(NAME display-input-x11-${name}
    COMMAND displayserver-input-tests x11 ${name}
  )
  set_tests_properties(display-input-x11-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

# Windows has no buttons past 7 and derives the modifiers from the held keys,
# so it runs its own versions of pointer-buttons and keyboard-state
foreach(name IN ITEMS
    pointer-events
    pointer-release
    wheel-steps
    relative-motion
    keyboard-focus
    keyboard-keys
    side-buttons
    pointer-leave
    wheel-partial
    raw-motion
    modifier-state
    key-filter
    keymap
  )
  add_test(NAME display-input-win32-${name}
    COMMAND displayserver-input-tests win32 ${name}
  )
  set_tests_properties(display-input-win32-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

add_executable(mouse-tests
  mouse_test.c
  ../src/core.c
  ../src/input.c
  ../src/util.c
)
target_compile_definitions(mouse-tests PRIVATE
  CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
)
target_include_directories(mouse-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
  "${PROJECT_TOP}/repos/gui/cimgui"
  "${PROJECT_TOP}/repos/gui/cimgui/imgui"
)
target_link_libraries(mouse-tests
  ${EXE_FLAGS}
  lg_common
)
set(MOUSE_CASES
  inset-exit
  surface-exit
  exit-immediate
  exit-guest
  absolute-sync
  auto-capture
  exit-reentry
  view-immediate
  capture-pending
  capture-revoked
  capture-release
  capture-align
  x11-capture-exit
  capture-fallback
  rotate-scale
  geometry
  edges
  scales
  border-exit
)
foreach(name IN LISTS MOUSE_CASES)
  add_test(NAME mouse-${name}
    COMMAND mouse-tests ${name}
  )
  set_tests_properties(mouse-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

add_executable(input-tests
  input_test.c
  ../src/input.c
)
target_include_directories(input-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(input-tests
  ${EXE_FLAGS}
  lg_common
)
set(INPUT_CASES
  preference
  handoff
  stale
  dead-drop
  rollback
  duplicate
  absolute
)
foreach(name IN LISTS INPUT_CASES)
  add_test(NAME input-${name}
    COMMAND input-tests ${name}
  )
  set_tests_properties(input-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

add_executable(keybind-tests
  keybind_test.c
)
target_compile_definitions(keybind-tests PRIVATE
  CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
)
target_include_directories(keybind-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
  "${PROJECT_TOP}/repos/gui/cimgui"
  "${PROJECT_TOP}/repos/gui/cimgui/imgui"
)
target_link_libraries(keybind-tests
  ${EXE_FLAGS}
  lg_common
)
set(KEYBIND_CASES
  common
  os
  order
  repeat
  actions
)
foreach(name IN LISTS KEYBIND_CASES)
  add_test(NAME keybind-${name}
    COMMAND keybind-tests ${name}
  )
  set_tests_properties(keybind-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

add_executable(app-keybind-tests
  app_keybind_test.c
  ../src/app.c
)
target_compile_definitions(app-keybind-tests PRIVATE
  CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
)
target_include_directories(app-keybind-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
  "${PROJECT_TOP}/repos/gui/cimgui"
  "${PROJECT_TOP}/repos/gui/cimgui/imgui"
)
target_link_libraries(app-keybind-tests
  ${EXE_FLAGS}
  lg_common
)
set(APP_KEYBIND_CASES
  invalid
  null-callback
  duplicate
  normal
  dispatch
  release-all
  focus
  reentrant
)
foreach(name IN LISTS APP_KEYBIND_CASES)
  add_test(NAME app-keybind-${name}
    COMMAND app-keybind-tests ${name}
  )
  set_tests_properties(app-keybind-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

add_executable(clipboard-tests
  clipboard_test.c
  clipboard_files_stub.c
  ../src/clipboard.c
)
target_compile_definitions(clipboard-tests PRIVATE
  CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
)
target_include_directories(clipboard-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
  "${PROJECT_TOP}/repos/gui/cimgui"
  "${PROJECT_TOP}/repos/gui/cimgui/imgui"
)
target_link_libraries(clipboard-tests
  ${EXE_FLAGS}
  lg_common
)
set(CLIPBOARD_CASES
  preference
  file-publication
  file-replacement
  file-failure
  callback-serial
  request
  remote-local
  pending-remote
  invalid
  cancel
  generation
  local
  reentrant
  stream-local
  stream-blocked
  stream-remote
  request-publish
  stream-legacy
)
foreach(name IN LISTS CLIPBOARD_CASES)
  add_test(NAME clipboard-${name}
    COMMAND clipboard-tests ${name}
  )
  set_tests_properties(clipboard-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

# file clipboard needs FUSE, which Windows does not have
if(NOT WIN32)
add_executable(clipboard-files-tests
  clipboard_files_test.c
  ../src/clipboard_files.c
)
target_include_directories(clipboard-files-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(clipboard-files-tests
  ${EXE_FLAGS}
  lg_common
  PkgConfig::FUSE3
)
add_test(NAME clipboard-files-lifecycle
  COMMAND clipboard-files-tests
)
set_tests_properties(clipboard-files-lifecycle PROPERTIES
  TIMEOUT 10
)
add_test(NAME clipboard-files-failures
  COMMAND clipboard-files-tests failures
)
set_tests_properties(clipboard-files-failures PROPERTIES
  TIMEOUT 10
)
add_test(NAME clipboard-files-mount
  COMMAND clipboard-files-tests mount
)
set_tests_properties(clipboard-files-mount PROPERTIES
  TIMEOUT 10
  SKIP_RETURN_CODE 77
)
endif()

# SPICE needs a Winsock port of PureSpice, so the Windows client has none
if(NOT WIN32)
add_executable(spice-clipboard-tests
  spice_clipboard_test.c
  clipboard_files_stub.c
  ../src/clipboard.c
  ../transports/SPICE/clipboard.c
)
target_compile_definitions(spice-clipboard-tests PRIVATE
  CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
)
target_include_directories(spice-clipboard-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
  "${CMAKE_CURRENT_SOURCE_DIR}/../transports/SPICE"
  "${PROJECT_TOP}/repos/PureSpice/include"
  "${PROJECT_TOP}/repos/gui/cimgui"
  "${PROJECT_TOP}/repos/gui/cimgui/imgui"
)
target_link_libraries(spice-clipboard-tests
  ${EXE_FLAGS}
  lg_common
)
foreach(name IN ITEMS outbound inbound)
  add_test(NAME spice-clipboard-${name}
    COMMAND spice-clipboard-tests ${name}
  )
  set_tests_properties(spice-clipboard-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()
endif()

add_executable(transport-fallback-tests
  transport_fallback_test.c
  ../src/fallback.c
  ../src/sw_surface.c
)
target_include_directories(transport-fallback-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
)
target_link_libraries(transport-fallback-tests
  ${EXE_FLAGS}
  lg_common
)
if(ENABLE_AUDIO)
  target_compile_definitions(transport-fallback-tests PRIVATE
    ENABLE_AUDIO
  )
endif()
set(TRANSPORT_FALLBACK_CASES
  graceful
  dead
  mismatch
  missing-uuid
  late-mismatch
  retry
  cancel
  no-video
  attach-fail
  active-fail
  activation-cancel
  activation-request-cancel
  deactivation-completes
  activation-mismatch
  pre-requested-activation
)
foreach(name IN LISTS TRANSPORT_FALLBACK_CASES)
  add_test(NAME transport-fallback-${name}
    COMMAND transport-fallback-tests ${name}
  )
  set_tests_properties(transport-fallback-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

# SPICE needs a Winsock port of PureSpice, so the Windows client has none
if(NOT WIN32)
add_executable(spice-tests
  spice_test.c
  ../transports/SPICE/session.c
  ../transports/SPICE/spice.c
  ../transports/SPICE/surface.c
)
target_compile_definitions(spice-tests PRIVATE
  ENABLE_AUDIO=0
  ENABLE_USB_AUDIO=0
)
target_include_directories(spice-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../transports/SPICE"
  "${PROJECT_TOP}/repos/PureSpice/include"
)
target_link_libraries(spice-tests
  ${EXE_FLAGS}
  lg_common
)
set(SPICE_CASES
  connect-fail
  pre-ready-drop
  cancel
  ready-drop
  session-order
  surface-active
  surface-cancel
  surface-events
  surface-detach
)
foreach(name IN LISTS SPICE_CASES)
  add_test(NAME spice-${name}
    COMMAND spice-tests ${name}
  )
  set_tests_properties(spice-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

add_executable(purespice-connect-cancel-tests
  purespice_connect_cancel_test.c
)
target_compile_definitions(purespice PRIVATE PURESPICE_TESTING)
target_compile_definitions(purespice-connect-cancel-tests PRIVATE
  PURESPICE_TESTING
)
target_include_directories(purespice-connect-cancel-tests PRIVATE
  "${PROJECT_TOP}/repos/PureSpice/include"
  "${PROJECT_TOP}/repos/PureSpice/src"
  "$<TARGET_PROPERTY:purespice,INCLUDE_DIRECTORIES>"
)
target_link_libraries(purespice-connect-cancel-tests
  ${EXE_FLAGS}
  purespice
  pthread
)
add_test(NAME purespice-connect-cancel
  COMMAND purespice-connect-cancel-tests
)
set_tests_properties(purespice-connect-cancel PROPERTIES
  TIMEOUT 5
)
endif()

add_executable(frame-scheduler-tests
  frame_scheduler_test.c
  ../src/frame_scheduler.c
)
target_compile_definitions(frame-scheduler-tests PRIVATE
  CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
)
target_include_directories(frame-scheduler-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
  "${PROJECT_TOP}/repos/gui/cimgui"
  "${PROJECT_TOP}/repos/gui/cimgui/imgui"
)
target_link_libraries(frame-scheduler-tests
  ${EXE_FLAGS}
  lg_common
)
set(FRAME_SCHEDULER_CASES
  absent
  start
  enqueue
  ack
  period
  cadence
  feedback
)
foreach(name IN LISTS FRAME_SCHEDULER_CASES)
  add_test(NAME frame-scheduler-${name}
    COMMAND frame-scheduler-tests ${name}
  )
  set_tests_properties(frame-scheduler-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

add_executable(vblank-tests
  vblank_test.c
  ../displayservers/Win32/vblank.c
)
target_include_directories(vblank-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../displayservers/Win32"
)
target_link_libraries(vblank-tests
  ${EXE_FLAGS}
  lg_common
)
set(VBLANK_CASES
  cadence
  coalesce
  interrupt
  period
  reset
  fallback
  retry
  retry-stays
  retry-clock
  clock
  submit
  stall
  blocked
  destroy
)
foreach(name IN LISTS VBLANK_CASES)
  add_test(NAME vblank-${name}
    COMMAND vblank-tests ${name}
  )
  set_tests_properties(vblank-${name} PROPERTIES
    TIMEOUT 30
  )
endforeach()

add_executable(frame-timing-tests
  frame_timing_test.c
)
target_compile_definitions(frame-timing-tests PRIVATE
  CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
)
target_compile_options(frame-timing-tests PRIVATE -O1)
target_include_directories(frame-timing-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
  "${PROJECT_TOP}/repos/gui/cimgui"
  "${PROJECT_TOP}/repos/gui/cimgui/imgui"
)
target_link_libraries(frame-timing-tests
  ${EXE_FLAGS}
  lg_common
)
set(FRAME_TIMING_CASES
  lifecycle
  order
  untracked
  accounting
  provider-unavailable
  timeout
  fifo
  retire
  feedback
  wrap
  component-epoch
  payload-gate
  frame-payload-bookkeeping
  connect-cancel
  recovery-errors
)
foreach(name IN LISTS FRAME_TIMING_CASES)
  add_test(NAME frame-timing-${name}
    COMMAND frame-timing-tests ${name}
  )
  set_tests_properties(frame-timing-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

add_executable(config-message-tests
  config_message_test.c
  ../src/message.c
  ../src/core.c
  ../src/config.c
)
target_compile_definitions(config-message-tests PRIVATE
  CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
)
target_include_directories(config-message-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
  "${PROJECT_TOP}/repos/gui/cimgui"
  "${PROJECT_TOP}/repos/gui/cimgui/imgui"
)
target_link_libraries(config-message-tests
  ${EXE_FLAGS}
  lg_common
)
set(CONFIG_MESSAGE_CASES
  debounce
  latest
  equal
  reinit
  control
  cfg-values
  cfg-position
  cfg-size
  cfg-precedence
)
foreach(name IN LISTS CONFIG_MESSAGE_CASES)
  add_test(NAME config-message-${name}
    COMMAND config-message-tests ${name}
  )
  set_tests_properties(config-message-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

if(WIN32)
  # these two set the clock, see windows/mock_clock.h
  foreach(target IN ITEMS frame-scheduler-tests config-message-tests)
    target_compile_options(${target} PRIVATE
      "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/windows/mock_clock.h")
  endforeach()
endif()

if(ENABLE_AUDIO)
  pkg_check_modules(AUDIO_TEST_SAMPLERATE REQUIRED IMPORTED_TARGET
    samplerate)
  add_executable(audio-tests
    audio_test.c
  )
  target_compile_definitions(audio-tests PRIVATE
    CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
    ENABLE_AUDIO
  )
  target_include_directories(audio-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    "${PROJECT_TOP}/repos/gui/cimgui"
    "${PROJECT_TOP}/repos/gui/cimgui/imgui"
  )
  target_link_libraries(audio-tests
    ${EXE_FLAGS}
    lg_common
    PkgConfig::AUDIO_TEST_SAMPLERATE
  )
  set(AUDIO_CASES
    format
    convert
    provider
    playback-retry
    jitter
    recovery
    stable-underrun
    resume
    consent
    quiesce
  )
  foreach(name IN LISTS AUDIO_CASES)
    add_test(NAME audio-${name}
      COMMAND audio-tests ${name}
    )
    set_tests_properties(audio-${name} PROPERTIES
      TIMEOUT 10
    )
  endforeach()
endif()

add_executable(render-queue-tests
  render_queue_test.c
  ../src/render_queue.c
)
target_compile_definitions(render-queue-tests PRIVATE
  CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1
)
target_include_directories(render-queue-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../src"
  "${PROJECT_TOP}/repos/gui/cimgui"
  "${PROJECT_TOP}/repos/gui/cimgui/imgui"
)
target_link_libraries(render-queue-tests
  ${EXE_FLAGS}
  lg_common
)
set(RENDER_QUEUE_CASES
  generation
  latest
  prepare
  presented
  payload
  validation
  cache
  cursor-replacement
)
foreach(name IN LISTS RENDER_QUEUE_CASES)
  add_test(NAME render-queue-${name}
    COMMAND render-queue-tests ${name}
  )
  set_tests_properties(render-queue-${name} PROPERTIES
    TIMEOUT 10
  )
endforeach()

if(ENABLE_EGL)
  add_executable(desktop-rects-tests
    desktop_rects_test.c
  )
  target_include_directories(desktop-rects-tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../renderers/EGL"
  )
  target_link_libraries(desktop-rects-tests
    ${EXE_FLAGS}
    renderer_EGL
  )
  set(DESKTOP_RECTS_CASES
    rotations
    fractional
    round-trip
    clipping
  )
  foreach(name IN LISTS DESKTOP_RECTS_CASES)
    add_test(NAME desktop-rects-${name}
      COMMAND desktop-rects-tests ${name}
    )
    set_tests_properties(desktop-rects-${name} PROPERTIES
      TIMEOUT 10
    )
  endforeach()

  add_executable(texture-dmabuf-tests
    texture_dmabuf_test.c
  )
  add_dependencies(texture-dmabuf-tests renderer_EGL)
  target_include_directories(texture-dmabuf-tests PRIVATE
    "${CMAKE_BINARY_DIR}/renderers/EGL/shader"
  )
  target_link_libraries(texture-dmabuf-tests
    ${EXE_FLAGS}
    lg_common
  )
  set(TEXTURE_DMABUF_CASES
    select
    poll
    saturation
    release
  )
  foreach(name IN LISTS TEXTURE_DMABUF_CASES)
    add_test(NAME texture-dmabuf-${name}
      COMMAND texture-dmabuf-tests ${name}
    )
    set_tests_properties(texture-dmabuf-${name} PROPERTIES
      TIMEOUT 10
    )
  endforeach()
endif()

# the rendering tests need GoogleTest and a Wayland or X11 compositor; the
# Windows client checks its rendering with windows-client/client_*_test.py
if(NOT WIN32)
find_package(GTest REQUIRED)
find_program(WESTON_EXECUTABLE NAMES weston)
if(NOT WESTON_EXECUTABLE)
  message(FATAL_ERROR "weston is required to run framebuffer rendering tests")
endif()
if(ENABLE_X11)
  find_program(XVFB_RUN_EXECUTABLE NAMES xvfb-run)
  if(NOT XVFB_RUN_EXECUTABLE)
    message(FATAL_ERROR "xvfb-run is required to run X11 rendering tests")
  endif()
endif()

add_executable(render-tests
  render_test.cpp
)
add_dependencies(render-tests looking-glass-client)

set_target_properties(render-tests PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
)

target_compile_options(render-tests PRIVATE
  -fno-fast-math
)

target_compile_definitions(render-tests PRIVATE
  LG_CLIENT_PATH="$<TARGET_FILE:looking-glass-client>"
)

target_include_directories(render-tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../include"
  "${PROJECT_TOP}/common/include"
)

target_link_libraries(render-tests
  LGProtocol::LGProtocol
)

if(TARGET GTest::gtest_main)
  target_link_libraries(render-tests GTest::gtest_main)
else()
  target_link_libraries(render-tests GTest::Main)
endif()

add_test(NAME render-tests
  COMMAND /bin/bash
    "${CMAKE_CURRENT_SOURCE_DIR}/run-with-weston.sh"
    "${WESTON_EXECUTABLE}"
    "$<TARGET_FILE:render-tests>"
    "--gtest_filter=-X11*"
)

set_tests_properties(render-tests PROPERTIES
  TIMEOUT 120
)

if(ENABLE_X11)
  add_test(NAME render-x11-tests
    COMMAND "${CMAKE_COMMAND}" -E env
      --unset=WAYLAND_DISPLAY
      LIBGL_ALWAYS_SOFTWARE=1
      "${XVFB_RUN_EXECUTABLE}"
      -a
      -e /dev/stderr
      -s "-screen 0 1024x768x24 +extension GLX"
      "$<TARGET_FILE:render-tests>"
      "--gtest_filter=X11*"
  )

  set_tests_properties(render-x11-tests PROPERTIES
    TIMEOUT 120
  )
endif()
endif()
