add_library(wl_ptt STATIC src/ptt.cpp)

target_include_directories(wl_ptt PUBLIC include)
target_link_libraries(wl_ptt PUBLIC wl_core PRIVATE wl_warnings)

if(WIN32)
  target_link_libraries(wl_ptt PRIVATE ws2_32)
endif()
