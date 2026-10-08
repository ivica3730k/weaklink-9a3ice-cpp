add_library(wl_core STATIC
  src/bits.cpp
  src/constants.cpp
  src/log.cpp)

target_include_directories(wl_core PUBLIC include)
target_link_libraries(wl_core PRIVATE wl_warnings)
