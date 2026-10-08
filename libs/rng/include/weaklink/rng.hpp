#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace weaklink::rng {

/// NumPy's SeedSequence -> PCG64 pipeline, reproduced bit-for-bit.
///
/// This is wire-format-defining, not a convenience: the interleaver's
/// per-block permutation and the pilot burst's symbol sequence are both
/// derived from ``numpy.random.default_rng(seed)``. A C++ receiver that
/// drew different numbers would not decode a Python transmitter's audio.
/// The generated streams are verified against NumPy in the test suite.
class NumpyGenerator {
 public:
  explicit NumpyGenerator(uint64_t seed);

  /// Raw 64-bit output, equivalent to ``BitGenerator.random_raw()``.
  uint64_t next_uint64();

  /// 32-bit output with NumPy's half-word buffering: the low half of a
  /// 64-bit draw is returned first, the high half on the next call.
  uint32_t next_uint32();

  /// ``Generator.random()`` -- 53 significant bits in [0, 1).
  double next_double();

  /// ``Generator.integers(0, bound)``. NumPy routes this through Lemire's
  /// multiply-shift rejection sampler.
  uint64_t bounded(uint64_t bound_exclusive);

  /// ``Generator.permutation(size)`` -- Fisher-Yates walking down from the
  /// end. NumPy's shuffle uses masked rejection here, *not* Lemire; the two
  /// consume the stream differently and only the masked variant reproduces
  /// NumPy's output.
  std::vector<uint32_t> permutation(std::size_t size);

  /// ``Generator.standard_normal()`` -- the 256-level ziggurat.
  double standard_normal();

  /// ``Generator.bytes(count)``.
  std::vector<uint8_t> bytes(std::size_t count);

 private:
  uint32_t masked_bounded(uint32_t bound_inclusive);

  unsigned __int128 state_ = 0;
  unsigned __int128 inc_ = 0;
  uint32_t buffered_uint32_ = 0;
  bool has_buffered_uint32_ = false;
};

/// Python's ``random.Random`` (MT19937). Used by the benchmark's payload
/// generator and by tests ported from the Python suite, both of which seed
/// ``random.Random(n)`` rather than NumPy.
class PythonRandom {
 public:
  explicit PythonRandom(uint64_t seed);

  /// ``random.random()``.
  double random();

  /// ``random.getrandbits(k)`` for k <= 64.
  uint64_t getrandbits(int bits);

  /// ``random.randrange(stop)`` -- uses ``_randbelow_with_getrandbits``.
  uint64_t randbelow(uint64_t stop);

  /// ``random.randint(low, high)`` (inclusive on both ends).
  int64_t randint(int64_t low, int64_t high);

  /// ``random.choices(population, k=count)``.
  std::vector<uint8_t> choices(const std::vector<uint8_t>& population, std::size_t count);

  /// ``random.choice(population)``.
  uint8_t choice(const std::vector<uint8_t>& population);

 private:
  uint32_t genrand_uint32();
  void twist();

  uint32_t state_[624]{};
  int index_ = 625;
};

}  // namespace weaklink::rng
