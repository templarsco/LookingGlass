cmake_minimum_required(VERSION 3.10)
project(looking-glass-windows-client C CXX)

get_filename_component(PROJECT_TOP "${PROJECT_SOURCE_DIR}/.." ABSOLUTE)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR
    "\n"
    "In-source builds are not supported\n"
    "See build instructions provided in: "
    "${PROJECT_SOURCE_DIR}/README.md\n"
    "Refusing to continue"
  )
endif()

# only the protocol submodules are needed at this stage
foreach(required
    "repos/LGMP/lgmp/CMakeLists.txt"
    "repos/LGProtocol/CMakeLists.txt")
  if(NOT EXISTS "${PROJECT_TOP}/${required}")
    message(FATAL_ERROR
      "${required} is missing, the protocol submodules are not initialized.\n"
      "Run\n\tgit submodule update --init repos/LGMP repos/LGProtocol")
  endif()
endforeach()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_TOP}/cmake/")

include(CheckCCompilerFlag)
include(FeatureSummary)

include(OptimizeForNative) # option(OPTIMIZE_FOR_NATIVE)

option(ENABLE_BACKTRACE "Enable backtrace support on crash" OFF)
add_feature_info(ENABLE_BACKTRACE ENABLE_BACKTRACE "Backtrace support.")

add_compile_options(
  "-Wall"
  "-Werror"
  "-Wfatal-errors"
  "$<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>"
  "-ffast-math"
  "-fdata-sections"
  "-ffunction-sections"
  "$<$<CONFIG:DEBUG>:-O0;-g3;-ggdb>"
)
set(CMAKE_C_STANDARD 11)

if(WIN32)
  add_compile_options("-fdebug-prefix-map=${PROJECT_TOP}/=")
  add_compile_definitions(__USE_MINGW_ANSI_STDIO=0)
endif()

add_subdirectory("${PROJECT_TOP}/common"          "${CMAKE_BINARY_DIR}/common")
add_subdirectory("${PROJECT_TOP}/repos/LGMP/lgmp" "${CMAKE_BINARY_DIR}/lgmp"  )

add_executable(lg-windows-client-loopback src/loopback.c src/session.c)
target_link_libraries(lg-windows-client-loopback
  lg_common
  lgmp
)

# what the HCS probe's report says about the virtual machines of the PC
add_executable(lg-hcs-systems-test src/hcs_systems_test.c src/hcs_systems.c)

if(WIN32)
  set_target_properties(lg-windows-client-loopback PROPERTIES
    LINK_FLAGS "-Wl,--gc-sections -Wl,--nxcompat")

  # serves frames on a named section, for the client's LGMP smoke test
  add_executable(lg-windows-client-producer src/producer.c src/session.c)
  target_link_libraries(lg-windows-client-producer
    lg_common
    lgmp
    advapi32
  )
  set_target_properties(lg-windows-client-producer PROPERTIES
    LINK_FLAGS "-Wl,--gc-sections -Wl,--nxcompat")

  # what a display offers a client that renders in step with it
  add_executable(lg-windows-client-vblank-probe src/vblank_probe.c)
  target_link_libraries(lg-windows-client-vblank-probe
    dwmapi
    gdi32
    user32
  )
  set_target_properties(lg-windows-client-vblank-probe PROPERTIES
    LINK_FLAGS "-Wl,--gc-sections -Wl,--nxcompat")

  # runs in a Windows guest, which has no MinGW runtime
  add_executable(lg-hyperv-ivshmem src/hyperv_ivshmem.c)
  target_include_directories(lg-hyperv-ivshmem PRIVATE
    "${PROJECT_TOP}/vendor/ivshmem")
  target_link_libraries(lg-hyperv-ivshmem
    advapi32
    cfgmgr32
    newdev
    setupapi
  )
  set_target_properties(lg-hyperv-ivshmem PROPERTIES
    LINK_FLAGS "-municode -static -Wl,--gc-sections -Wl,--nxcompat")

  # which named sections the client takes, by the security that they have
  add_executable(lg-windows-client-section-test src/section_test.c)
  target_link_libraries(lg-windows-client-section-test
    lg_common
    advapi32
  )
  set_target_properties(lg-windows-client-section-test PROPERTIES
    LINK_FLAGS "-Wl,--gc-sections -Wl,--nxcompat")

  # the HCS shared memory probe boots VMs with its own init, which needs a
  # compiler for x86_64 Linux: the host's GCC when cross-compiling from
  # Linux, else Clang with LLD
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND
      CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    find_program(GUEST_GCC gcc)
    if(GUEST_GCC)
      set(GUEST_CC "${GUEST_GCC}" -fno-tree-loop-distribute-patterns)
    endif()
  else()
    find_program(GUEST_CLANG clang)
    find_program(GUEST_LLD ld.lld)
    if(GUEST_CLANG AND GUEST_LLD)
      set(GUEST_CC "${GUEST_CLANG}" --target=x86_64-linux-gnu -fuse-ld=lld)
    endif()
  endif()

  if(GUEST_CC)
    set(GUEST_INIT "${CMAKE_CURRENT_BINARY_DIR}/hcs-probe-guest-init")
    add_custom_command(
      OUTPUT "${GUEST_INIT}"
      COMMAND ${GUEST_CC} -std=gnu11 -O2 -Wall -Werror -Wstrict-prototypes
        -static -nostdlib -ffreestanding -fno-builtin -fno-stack-protector
        -fno-pie -fno-asynchronous-unwind-tables -fno-unwind-tables
        -ffunction-sections -fdata-sections -Wl,--gc-sections
        -Wl,--build-id=none -Wl,-z,noexecstack -s
        -o "${GUEST_INIT}" "${PROJECT_SOURCE_DIR}/src/hcs_probe_guest.c"
      DEPENDS src/hcs_probe_guest.c
      COMMENT "Building the HCS probe's guest init for x86_64 Linux"
      VERBATIM
    )

    # the init is a source only for the command that builds it to run
    add_executable(lg-windows-client-hcs-probe src/hcs_probe.c
      src/hcs_systems.c "${GUEST_INIT}")
    target_compile_definitions(lg-windows-client-hcs-probe PRIVATE
      LG_HCS_PROBE_GUEST="${GUEST_INIT}")
    set_source_files_properties(src/hcs_probe.c PROPERTIES
      OBJECT_DEPENDS "${GUEST_INIT}")
    target_link_libraries(lg-windows-client-hcs-probe
      advapi32
      bcrypt
      ole32
      shell32
    )
    set_target_properties(lg-windows-client-hcs-probe PROPERTIES
      LINK_FLAGS "-Wl,--gc-sections -Wl,--nxcompat")
  else()
    message(STATUS "No compiler for x86_64 Linux, not building "
      "lg-windows-client-hcs-probe")
  endif()
else()
  set_target_properties(lg-windows-client-loopback PROPERTIES
    LINK_FLAGS "-Wl,--gc-sections -z noexecstack")
endif()

include(CTest)
if(BUILD_TESTING)
  add_test(NAME loopback COMMAND lg-windows-client-loopback)
  set_tests_properties(loopback PROPERTIES TIMEOUT 60)
  add_test(NAME hcs-systems COMMAND lg-hcs-systems-test)
  set_tests_properties(hcs-systems PROPERTIES TIMEOUT 60)
  if(WIN32)
    add_test(NAME section COMMAND lg-windows-client-section-test)
    set_tests_properties(section PROPERTIES TIMEOUT 60)
  endif()
endif()

feature_summary(WHAT ENABLED_FEATURES DISABLED_FEATURES)
