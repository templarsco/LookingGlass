cmake_minimum_required(VERSION 3.10)
project(looking-glass-client C CXX)

get_filename_component(PROJECT_TOP "${PROJECT_SOURCE_DIR}/.." ABSOLUTE)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR
    "\n"
    "In-source builds are not supported\n"
    "See build instructions provided in: "
    "${PROJECT_TOP}/doc/build.rst\n"
    "Refusing to continue"
  )
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_TOP}/cmake/" "${PROJECT_SOURCE_DIR}/cmake/")

include(CheckSubmodule)
include(GNUInstallDirs)
include(CheckCCompilerFlag)
include(CMakeDependentOption)
include(FeatureSummary)

set(OPTIMIZE_FOR_NATIVE_DEFAULT ON)
include(OptimizeForNative) # option(OPTIMIZE_FOR_NATIVE)
include(UninstallTarget)

find_package(PkgConfig)
if(NOT WIN32)
  pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
  pkg_check_modules(FUSE3 REQUIRED IMPORTED_TARGET fuse3>=3.10)
endif()

# Windows has no EGL, PipeWire or PulseAudio, it uses the OpenGL renderer
if(WIN32)
  set(PLATFORM_DEFAULT OFF)
else()
  set(PLATFORM_DEFAULT ON)
endif()

option(ENABLE_OPENGL "Enable the OpenGL renderer"       ON)
add_feature_info(ENABLE_OPENGL ENABLE_OPENGL "Legacy OpenGL renderer.")

option(ENABLE_EGL    "Enable the EGL renderer"          ${PLATFORM_DEFAULT})
add_feature_info(ENABLE_EGL ENABLE_EGL "EGL renderer.")

if(WIN32 AND ENABLE_EGL)
  message(FATAL_ERROR "The EGL renderer is not supported on Windows")
endif()

option(ENABLE_TESTS "Build all client tests and test support" OFF)
add_feature_info(ENABLE_TESTS ENABLE_TESTS
  "Client unit and integration tests with synthetic transport support.")
if(ENABLE_TESTS)
  if(NOT ENABLE_EGL AND NOT WIN32)
    message(FATAL_ERROR "ENABLE_TESTS requires ENABLE_EGL")
  endif()
  add_definitions(-D ENABLE_TESTS)
endif()

# ENABLE_TESTS builds the unit tests on Linux. On Windows it adds the
# framebuffer capture to the test transport, which the packaged client needs
# too, so the unit tests are a separate choice there.
option(ENABLE_UNIT_TESTS "Build the unit tests on Windows too" OFF)
add_feature_info(ENABLE_UNIT_TESTS ENABLE_UNIT_TESTS
  "Client unit tests on Windows, with ENABLE_TESTS.")
if(ENABLE_UNIT_TESTS AND NOT ENABLE_TESTS)
  message(FATAL_ERROR "ENABLE_UNIT_TESTS requires ENABLE_TESTS")
endif()

option(ENABLE_BACKTRACE "Enable backtrace support on crash" ON)
add_feature_info(ENABLE_BACKTRACE ENABLE_BACKTRACE "Backtrace support.")

option(ENABLE_ASAN "Build with AddressSanitizer" OFF)
add_feature_info(ENABLE_ASAN ENABLE_ASAN "AddressSanitizer support.")

option(ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)
add_feature_info(ENABLE_UBSAN ENABLE_UBSAN "UndefinedBehaviorSanitizer support.")

option(ENABLE_AUDIO "Build with audio support" ${PLATFORM_DEFAULT})

cmake_dependent_option(
  ENABLE_PIPEWIRE
  "Build with PipeWire audio output support"
  ON
  "ENABLE_AUDIO"
  OFF
)
add_feature_info(ENABLE_PIPEWIRE ENABLE_PIPEWIRE "PipeWire audio support.")

cmake_dependent_option(
  ENABLE_PULSEAUDIO
  "Build with PulseAudio audio output support"
  ON
  "ENABLE_AUDIO"
  OFF
)
add_feature_info(ENABLE_PULSEAUDIO ENABLE_PULSEAUDIO "PulseAudio audio support.")

if(ENABLE_AUDIO AND NOT (ENABLE_PIPEWIRE OR ENABLE_PULSEAUDIO))
  message(STATUS "No audio backends enabled, disabling audio support")
  set(ENABLE_AUDIO OFF)
endif()
add_feature_info(ENABLE_AUDIO ENABLE_AUDIO "Audio support.")

cmake_dependent_option(
  ENABLE_USB_AUDIO
  "Build with USB audio support"
  ON
  "ENABLE_AUDIO"
  OFF
)

if(ENABLE_USB_AUDIO)
  pkg_check_modules(USBREDIRPARSER QUIET IMPORTED_TARGET
    libusbredirparser-0.5>=0.7.1)
  if(NOT USBREDIRPARSER_FOUND)
    message(STATUS
      "libusbredirparser was not found, disabling USB audio support")
    set(ENABLE_USB_AUDIO OFF)
  endif()
endif()
add_feature_info(ENABLE_USB_AUDIO ENABLE_USB_AUDIO "USB audio support.")

add_compile_options(
  "-Wall"
  "-Wextra"
  "-Wno-sign-compare"
  "-Wno-unused-parameter"
  "$<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>"
  "$<$<C_COMPILER_ID:GNU>:-Wimplicit-fallthrough=2>"
  "-Werror"
  "-Wfatal-errors"
  "-g"
  "-ffast-math"
  "-fdata-sections"
  "-ffunction-sections"
  "$<$<CONFIG:DEBUG>:-O0;-g3;-ggdb>"
)

if(ENABLE_BACKTRACE)
  add_compile_options(
    "-fno-omit-frame-pointer"
    "-fno-optimize-sibling-calls"
  )
endif()

if(WIN32)
  # link the MinGW runtime statically so the executable runs on its own
  set(EXE_FLAGS "-Wl,--gc-sections -Wl,--nxcompat -static")
  add_compile_options("-fdebug-prefix-map=${PROJECT_TOP}/=")
  add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)
else()
  set(EXE_FLAGS "-Wl,--gc-sections -z noexecstack")
endif()
set(CMAKE_C_STANDARD 11)

if (ENABLE_OPENGL)
  add_definitions(-D ENABLE_OPENGL)
endif()

if (ENABLE_EGL)
  add_definitions(-D ENABLE_EGL)
endif()

if(ENABLE_ASAN)
  add_compile_options("-fno-omit-frame-pointer" "-fsanitize=address")
  set(EXE_FLAGS "${EXE_FLAGS} -fno-omit-frame-pointer -fsanitize=address")
endif()

if(ENABLE_UBSAN)
  add_compile_options("-fsanitize=undefined")
  set(EXE_FLAGS "${EXE_FLAGS} -fsanitize=undefined")
endif()

add_definitions(-D ATOMIC_LOCKING)
add_definitions(-D GL_GLEXT_PROTOTYPES)

add_custom_command(
  OUTPUT  ${CMAKE_BINARY_DIR}/version.c
    ${CMAKE_BINARY_DIR}/include/version.h
    ${CMAKE_BINARY_DIR}/_version.c
  COMMAND ${CMAKE_COMMAND} -D PROJECT_TOP=${PROJECT_TOP} -P
    ${PROJECT_TOP}/version.cmake
)

include_directories(
  ${PROJECT_TOP}
  ${PROJECT_SOURCE_DIR}/include
  ${CMAKE_BINARY_DIR}
  ${CMAKE_BINARY_DIR}/include
  ${PROJECT_TOP}/repos/nanosvg/src
)

if(WIN32)
  # provides linux/input.h with the evdev key codes the client uses
  include_directories(
    ${PROJECT_SOURCE_DIR}/platform/Windows/include
    ${PROJECT_TOP}/vendor/linux-input
  )
endif()

if(WIN32)
  link_libraries(m)
else()
  link_libraries(
    ${CMAKE_DL_LIBS}
    rt
    m
  )
endif()

set(SOURCES
  ${CMAKE_BINARY_DIR}/version.c
  src/main.c
  src/core.c
  src/app.c
  src/message.c
  src/config.c
  src/keybind.c
  src/font.c
  src/util.c
  src/clipboard.c
  src/clipboard_files.c
  src/kb.c
  src/kb_hid.c
  src/gl_dynprocs.c
  src/egl_dynprocs.c
  src/eglutil.c
  src/frame_scheduler.c
  src/overlay_utils.c
  src/render_queue.c
  src/evdev.c
  src/transport.c
  src/fallback.c
  src/sw_surface.c
  src/input.c

  src/overlay/splash.c
  src/overlay/alert.c
  src/overlay/fps.c
  src/overlay/graphs.c
  src/overlay/help.c
  src/overlay/config.c
  src/overlay/msg.c
  src/overlay/status.c
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(src/overlay/graphs.c PROPERTIES
    COMPILE_FLAGS "-fno-fast-math"
  )
endif()

if(ENABLE_AUDIO)
  list(APPEND SOURCES
    src/audio.c
  )
endif()

if(WIN32)
  # FUSE file transfers, evdev input and EGL only exist on Linux
  list(REMOVE_ITEM SOURCES
    src/clipboard_files.c
    src/evdev.c
    src/egl_dynprocs.c
    src/eglutil.c
  )
  list(APPEND SOURCES
    platform/Windows/stubs.c
    platform/Windows/uifont.c
  )
endif()

add_subdirectory("${PROJECT_TOP}/resources"       "${CMAKE_BINARY_DIR}/resources")
add_subdirectory("${PROJECT_TOP}/common"          "${CMAKE_BINARY_DIR}/common"   )
add_subdirectory("${PROJECT_TOP}/repos/LGMP/lgmp" "${CMAKE_BINARY_DIR}/LGMP"     )
add_subdirectory("${PROJECT_TOP}/repos/gui"       "${CMAKE_BINARY_DIR}/gui"      EXCLUDE_FROM_ALL)

add_subdirectory(displayservers)
add_subdirectory(renderers)
add_subdirectory(transports)
add_subdirectory(audiodevs)

if(NOT WIN32)
  configure_file("${PROJECT_TOP}/resources/looking-glass-client.desktop.in" "${CMAKE_BINARY_DIR}/resources/looking-glass-client.desktop" @ONLY)
endif()
add_executable(looking-glass-client ${SOURCES})

target_compile_definitions(looking-glass-client PRIVATE CIMGUI_DEFINE_ENUMS_AND_STRUCTS=1)

if(NOT CMAKE_OBJCOPY)
  message(FATAL_ERROR "objcopy is required to generate the client debug file")
endif()

if(NOT CMAKE_STRIP)
  message(FATAL_ERROR "strip is required to generate the client debug file")
endif()

set(CLIENT_DEBUG_FILE
  "${CMAKE_CURRENT_BINARY_DIR}/looking-glass-client.debug")

add_custom_command(TARGET looking-glass-client POST_BUILD
  COMMAND "${CMAKE_OBJCOPY}" --only-keep-debug
    "$<TARGET_FILE:looking-glass-client>" "${CLIENT_DEBUG_FILE}"
  COMMAND "${CMAKE_STRIP}" --strip-debug "$<TARGET_FILE:looking-glass-client>"
  COMMAND "${CMAKE_OBJCOPY}" "--add-gnu-debuglink=${CLIENT_DEBUG_FILE}"
    "$<TARGET_FILE:looking-glass-client>"
  COMMENT "Extracting Looking Glass client debug information"
  VERBATIM
)

target_link_libraries(looking-glass-client
  ${EXE_FLAGS}
  $<$<NOT:$<PLATFORM_ID:Windows>>:PkgConfig::FONTCONFIG>
  $<$<NOT:$<PLATFORM_ID:Windows>>:PkgConfig::FUSE3>
  lg_resources
  lg_common
  displayservers
  renderers
  transports
  audiodevs
  lg_gui
)

if(WIN32)
  target_link_libraries(looking-glass-client dwrite uuid)

  # the resource compiler can't take the quoted definitions that lg_gui
  # passes to its users, so the resources build in a target of their own
  add_library(looking-glass-client-resources OBJECT
    platform/Windows/resource.rc
  )
  set_source_files_properties(platform/Windows/resource.rc PROPERTIES
    OBJECT_DEPENDS "${PROJECT_SOURCE_DIR}/platform/Windows/app.manifest;${PROJECT_TOP}/resources/icon.ico;${CMAKE_BINARY_DIR}/include/version.h"
  )
  # version.h must exist before the resources build, and the executable
  # waits for it so that the version command never runs twice at once
  add_custom_target(looking-glass-client-version DEPENDS
    ${CMAKE_BINARY_DIR}/version.c
    ${CMAKE_BINARY_DIR}/include/version.h
  )
  add_dependencies(looking-glass-client-resources looking-glass-client-version)
  add_dependencies(looking-glass-client looking-glass-client-version)
  target_sources(looking-glass-client PRIVATE
    $<TARGET_OBJECTS:looking-glass-client-resources>
  )
endif()

if(ENABLE_TESTS AND (NOT WIN32 OR ENABLE_UNIT_TESTS))
  include(CTest)
  enable_testing()
  add_subdirectory(tests)
endif()

if(ENABLE_AUDIO)
  target_compile_definitions(looking-glass-client PRIVATE ENABLE_AUDIO)
  pkg_check_modules(SAMPLERATE REQUIRED IMPORTED_TARGET samplerate)
  target_link_libraries(looking-glass-client
    PkgConfig::SAMPLERATE
  )
endif()

install(TARGETS looking-glass-client
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  COMPONENT binary)

install(FILES "${CLIENT_DEBUG_FILE}"
  DESTINATION "${CMAKE_INSTALL_BINDIR}/.debug"
  COMPONENT binary)

if(NOT WIN32)
  install(FILES "${CMAKE_BINARY_DIR}/resources/looking-glass-client.desktop"
      DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/applications")
  install(FILES "${PROJECT_TOP}/resources/lg-logo.svg"
          DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps"
          RENAME "looking-glass.svg")
endif()

feature_summary(WHAT ENABLED_FEATURES DISABLED_FEATURES)
