add_library(wl_reedsolomon STATIC
  src/galois.cpp
  src/reedsolomon.cpp)

target_include_directories(wl_reedsolomon PUBLIC include)
target_link_libraries(wl_reedsolomon PUBLIC wl_core PRIVATE wl_warnings)
