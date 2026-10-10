cmake_minimum_required(VERSION 3.16)
# C is enabled for the bundled PortAudio sources; none of the weaklink code is C.
project(weaklink-modem VERSION 0.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(WEAKLINK_BUILD_TESTS "Build the Catch2 test suite" ON)
option(WEAKLINK_LIVE_AUDIO  "Build live audio I/O (bundles PortAudio)" ON)
option(WEAKLINK_STATIC_RUNTIME "Link libstdc++/libgcc statically (portable Linux builds)" OFF)

# What `--version` reports. Release builds pass the tag; everything else is 0.0.0,
# matching the reference implementation, which keeps 0.0.0 in git and sets the
# real version from the tag at publish time.
set(WEAKLINK_VERSION "${PROJECT_VERSION}" CACHE STRING "Version string reported by the binaries")

# Static libstdc++/libgcc let one Linux binary run across distributions without
# matching the build machine's GCC runtime. Not meaningful on macOS, where the
# C++ runtime ships with the OS.
add_library(wl_runtime INTERFACE)
if(WEAKLINK_STATIC_RUNTIME)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE AND NOT MSVC)
    target_link_options(wl_runtime INTERFACE -static-libstdc++ -static-libgcc)
  else()
    message(STATUS "WEAKLINK_STATIC_RUNTIME ignored: not supported for this compiler/platform")
  endif()
endif()

# Binaries land in a single gitignored directory so CI can collect them
# without knowing the generator's layout.
set(WEAKLINK_BIN_DIR "${PROJECT_SOURCE_DIR}/binaries" CACHE PATH "Where built binaries are placed")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${WEAKLINK_BIN_DIR}")
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} "${WEAKLINK_BIN_DIR}")
endforeach()

include(cmake/Warnings.cmake)
include(cmake/ThirdParty.cmake)

add_subdirectory(libs/core)
add_subdirectory(libs/rng)
add_subdirectory(libs/reedsolomon)
add_subdirectory(libs/fec)
add_subdirectory(libs/interleaver)
add_subdirectory(libs/dsp)
add_subdirectory(libs/codec)
add_subdirectory(libs/audio)
add_subdirectory(libs/ptt)
add_subdirectory(libs/streaming)
add_subdirectory(libs/api)
add_subdirectory(apps/modem)
add_subdirectory(apps/benchmark)

if(WEAKLINK_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "weaklink-modem ${PROJECT_VERSION}")
message(STATUS "  build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  live audio   : ${WEAKLINK_LIVE_AUDIO}")
message(STATUS "  tests        : ${WEAKLINK_BUILD_TESTS}")
message(STATUS "  binaries ->   ${WEAKLINK_BIN_DIR}")
