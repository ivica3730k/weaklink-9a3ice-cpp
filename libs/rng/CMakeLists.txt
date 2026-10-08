add_library(wl_rng STATIC src/rng.cpp)

target_include_directories(wl_rng PUBLIC include)
target_link_libraries(wl_rng PRIVATE wl_warnings)
