add_library(wl_dsp STATIC src/waveform.cpp)

target_include_directories(wl_dsp PUBLIC include)
target_link_libraries(wl_dsp PUBLIC wl_core PRIVATE wl_warnings wl_pocketfft)
