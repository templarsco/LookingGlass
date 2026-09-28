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
else()
  set_target_properties(lg-windows-client-loopback PROPERTIES
    LINK_FLAGS "-Wl,--gc-sections -z noexecstack")
endif()

include(CTest)
if(BUILD_TESTING)
  add_test(NAME loopback COMMAND lg-windows-client-loopback)
  set_tests_properties(loopback PROPERTIES TIMEOUT 60)
endif()

feature_summary(WHAT ENABLED_FEATURES DISABLED_FEATURES)
