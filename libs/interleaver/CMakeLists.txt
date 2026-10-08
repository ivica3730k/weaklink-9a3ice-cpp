add_library(wl_interleaver STATIC src/interleaver.cpp)

target_include_directories(wl_interleaver PUBLIC include)
# wl_rng is PUBLIC on purpose: the permutation it generates is part of the
# wire format, so anything reasoning about that format needs the generator.
target_link_libraries(wl_interleaver PUBLIC wl_core wl_rng PRIVATE wl_warnings)
