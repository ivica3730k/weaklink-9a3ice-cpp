add_library(wl_audio STATIC
  src/devices.cpp
  src/live_input.cpp
  src/playback.cpp
  src/process.cpp
  src/wav.cpp)

target_include_directories(wl_audio PUBLIC include)
target_link_libraries(wl_audio PUBLIC wl_core PRIVATE wl_warnings)

if(WEAKLINK_LIVE_AUDIO)
  target_compile_definitions(wl_audio PUBLIC WEAKLINK_HAVE_PORTAUDIO=1)
  target_link_libraries(wl_audio PRIVATE wl_portaudio)
else()
  target_compile_definitions(wl_audio PUBLIC WEAKLINK_HAVE_PORTAUDIO=0)
endif()
