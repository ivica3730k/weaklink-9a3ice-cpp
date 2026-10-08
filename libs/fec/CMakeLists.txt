add_library(wl_fec STATIC src/fec.cpp)

target_include_directories(wl_fec PUBLIC include)
target_link_libraries(wl_fec PUBLIC wl_core PRIVATE wl_warnings)
