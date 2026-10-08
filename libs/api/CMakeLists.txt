add_library(wl_api STATIC src/api.cpp)

target_include_directories(wl_api PUBLIC include)
target_link_libraries(wl_api PUBLIC wl_audio wl_codec wl_ptt wl_streaming PRIVATE wl_warnings)
