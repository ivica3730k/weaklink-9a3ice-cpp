add_library(wl_streaming STATIC src/streaming.cpp)

target_include_directories(wl_streaming PUBLIC include)
target_link_libraries(wl_streaming PUBLIC wl_audio wl_codec wl_core PRIVATE wl_rng wl_warnings)
