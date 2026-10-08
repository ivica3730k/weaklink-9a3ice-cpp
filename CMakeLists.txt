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
option(WEAKLINK_STATIC_RUNTIME "Link the C++ runtime statically where supported" OFF)

# Binaries land in a single gitignored directory so CI can collect them
# without knowing the generator's layout.
set(WEAKLINK_BIN_DIR "${CMAKE_SOURCE_DIR}/binaries" CACHE PATH "Where built binaries are placed")
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
