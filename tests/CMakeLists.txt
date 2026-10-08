# One translation unit provides Catch2's main; the rest are plain test files.
add_executable(weaklink-tests
  main.cpp
  test_cliff_monotonic.cpp
  test_fec.cpp
  test_interleaver.cpp
  test_llr_combining.cpp
  test_mfsk_num_tones.cpp
  test_modem_end_to_end.cpp
  test_modem_file_pipe.cpp
  test_nyquist_symmetry.cpp
  test_periodic_noise.cpp
  test_preamble_correlator.cpp
  test_public_api.cpp
  test_reedsolomon.cpp
  test_rng_numpy_parity.cpp
  test_sequential_tx.cpp
  test_streaming_rx.cpp
  test_streaming_tx.cpp
  test_wav_damage.cpp
  test_wav_io.cpp
  test_waveform.cpp)

target_include_directories(weaklink-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(weaklink-tests PRIVATE wl_api wl_catch2 wl_warnings)

# The slow tag covers the SNR sweeps and multi-kilobyte round-trips. They are
# part of the suite but split out so `ctest -L fast` stays usable while
# iterating.
add_test(NAME weaklink-fast COMMAND weaklink-tests "~[slow]")
set_tests_properties(weaklink-fast PROPERTIES LABELS fast TIMEOUT 600)

add_test(NAME weaklink-slow COMMAND weaklink-tests "[slow]")
set_tests_properties(weaklink-slow PROPERTIES LABELS slow TIMEOUT 5400)
