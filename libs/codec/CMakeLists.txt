add_library(wl_codec STATIC
  src/codec.cpp
  src/decode.cpp
  src/preamble.cpp)

target_include_directories(wl_codec PUBLIC include)
target_link_libraries(wl_codec
  PUBLIC wl_core wl_dsp wl_fec wl_interleaver wl_reedsolomon
  PRIVATE wl_warnings)
