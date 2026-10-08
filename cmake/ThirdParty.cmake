# Header-only dependencies are vendored under third_party/ so a clean
# checkout builds with no network access. PortAudio is the one exception:
# it needs a real build, so it is fetched (or taken from the system).
include(FetchContent)

add_library(wl_pocketfft INTERFACE)
target_include_directories(wl_pocketfft SYSTEM INTERFACE "${CMAKE_SOURCE_DIR}/third_party/pocketfft")

add_library(wl_cli11 INTERFACE)
target_include_directories(wl_cli11 SYSTEM INTERFACE "${CMAKE_SOURCE_DIR}/third_party/cli11")

add_library(wl_catch2 INTERFACE)
target_include_directories(wl_catch2 SYSTEM INTERFACE "${CMAKE_SOURCE_DIR}/third_party/catch2")

if(WEAKLINK_LIVE_AUDIO)
  find_package(Threads REQUIRED)
  set(WEAKLINK_PORTAUDIO_TAG "v19.7.0" CACHE STRING "PortAudio git tag to bundle")

  set(PA_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(PA_BUILD_STATIC ON  CACHE BOOL "" FORCE)
  set(PA_BUILD_TESTS  OFF CACHE BOOL "" FORCE)
  set(PA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF)

  # PortAudio 19.7 still declares cmake_minimum_required(VERSION 3.0), which
  # CMake 4 refuses outright. Granting it the 3.5 floor is the documented
  # escape hatch and affects only the fetched subproject.
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

  FetchContent_Declare(portaudio
    GIT_REPOSITORY https://github.com/PortAudio/portaudio.git
    GIT_TAG ${WEAKLINK_PORTAUDIO_TAG}
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(portaudio)

  unset(CMAKE_POLICY_VERSION_MINIMUM)

  add_library(wl_portaudio INTERFACE)
  if(TARGET portaudio_static)
    target_link_libraries(wl_portaudio INTERFACE portaudio_static Threads::Threads)
  else()
    target_link_libraries(wl_portaudio INTERFACE portaudio Threads::Threads)
  endif()
endif()
