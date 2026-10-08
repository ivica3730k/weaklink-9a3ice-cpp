#include "weaklink/rng.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

#include "ziggurat_tables.hpp"

namespace weaklink::rng {
namespace {

using u128 = unsigned __int128;

// ---- NumPy SeedSequence ---------------------------------------------------
//
// Entropy is folded into a four-word pool, then the pool is cycled to produce
// however many output words the bit generator asks for. Constants are
// NumPy's; they have no meaning beyond "the mixer NumPy picked".

constexpr uint32_t kInitA = 0x43b0d7e5u;
constexpr uint32_t kMultA = 0x931e8875u;
constexpr uint32_t kInitB = 0x8b51f9ddu;
constexpr uint32_t kMultB = 0x58f38dedu;
constexpr uint32_t kMixMultL = 0xca01f9ddu;
constexpr uint32_t kMixMultR = 0x4973f715u;
constexpr int kXShift = 16;
constexpr std::size_t kPoolSize = 4;

class Hasher {
 public:
  uint32_t operator()(uint32_t value) {
    value ^= constant_;
    constant_ *= kMultA;
    value *= constant_;
    value ^= value >> kXShift;
    return value;
  }

 private:
  uint32_t constant_ = kInitA;
};

uint32_t mix(uint32_t x, uint32_t y) {
  uint32_t result = kMixMultL * x - kMixMultR * y;
  result ^= result >> kXShift;
  return result;
}

/// A Python ``int`` seed becomes little-endian 32-bit words; zero stays one
/// word wide rather than collapsing to an empty array.
std::vector<uint32_t> coerce_entropy(uint64_t seed) {
  if (seed == 0) {
    return {0u};
  }
  std::vector<uint32_t> words;
  while (seed != 0) {
    words.push_back(static_cast<uint32_t>(seed & 0xFFFFFFFFu));
    seed >>= 32;
  }
  return words;
}

std::array<uint32_t, kPoolSize> entropy_pool(const std::vector<uint32_t>& entropy) {
  std::array<uint32_t, kPoolSize> mixer{};
  Hasher hash;
  for (std::size_t i = 0; i < kPoolSize; ++i) {
    mixer[i] = hash(i < entropy.size() ? entropy[i] : 0u);
  }
  // Mix all bits together so late words can affect earlier ones.
  for (std::size_t src = 0; src < kPoolSize; ++src) {
    for (std::size_t dst = 0; dst < kPoolSize; ++dst) {
      if (src != dst) {
        mixer[dst] = mix(mixer[dst], hash(mixer[src]));
      }
    }
  }
  // Fold in any entropy beyond the pool width.
  for (std::size_t src = kPoolSize; src < entropy.size(); ++src) {
    for (std::size_t dst = 0; dst < kPoolSize; ++dst) {
      mixer[dst] = mix(mixer[dst], hash(entropy[src]));
    }
  }
  return mixer;
}

std::array<uint64_t, 4> generate_state(uint64_t seed) {
  const auto pool = entropy_pool(coerce_entropy(seed));
  std::array<uint32_t, 8> words{};
  uint32_t constant = kInitB;
  for (std::size_t i = 0; i < words.size(); ++i) {
    uint32_t value = pool[i % kPoolSize];
    value ^= constant;
    constant *= kMultB;
    value *= constant;
    value ^= value >> kXShift;
    words[i] = value;
  }
  std::array<uint64_t, 4> state{};
  for (std::size_t i = 0; i < state.size(); ++i) {
    state[i] = static_cast<uint64_t>(words[2 * i]) |
               (static_cast<uint64_t>(words[2 * i + 1]) << 32);
  }
  return state;
}

constexpr u128 pcg_multiplier() {
  return (static_cast<u128>(2549297995355413924ULL) << 64) | 4865540595714422341ULL;
}

uint32_t bit_mask_for(uint32_t max_value) {
  uint32_t mask = max_value;
  mask |= mask >> 1;
  mask |= mask >> 2;
  mask |= mask >> 4;
  mask |= mask >> 8;
  mask |= mask >> 16;
  return mask;
}

}  // namespace

// ---- NumpyGenerator -------------------------------------------------------

NumpyGenerator::NumpyGenerator(uint64_t seed) {
  const auto words = generate_state(seed);
  const u128 initial_state = (static_cast<u128>(words[0]) << 64) | words[1];
  const u128 sequence = (static_cast<u128>(words[2]) << 64) | words[3];
  inc_ = (sequence << 1) | 1;
  state_ = 0;
  state_ = state_ * pcg_multiplier() + inc_;
  state_ += initial_state;
  state_ = state_ * pcg_multiplier() + inc_;
}

uint64_t NumpyGenerator::next_uint64() {
  state_ = state_ * pcg_multiplier() + inc_;
  // XSL-RR: fold the 128-bit state in half, then rotate by its top 6 bits.
  const uint64_t folded =
      static_cast<uint64_t>(state_ >> 64) ^ static_cast<uint64_t>(state_);
  const unsigned rotation = static_cast<unsigned>(state_ >> 122);
  return (folded >> rotation) | (folded << ((-rotation) & 63u));
}

uint32_t NumpyGenerator::next_uint32() {
  if (has_buffered_uint32_) {
    has_buffered_uint32_ = false;
    return buffered_uint32_;
  }
  const uint64_t value = next_uint64();
  buffered_uint32_ = static_cast<uint32_t>(value >> 32);
  has_buffered_uint32_ = true;
  return static_cast<uint32_t>(value & 0xFFFFFFFFu);
}

double NumpyGenerator::next_double() {
  return static_cast<double>(next_uint64() >> 11) * (1.0 / 9007199254740992.0);
}

uint64_t NumpyGenerator::bounded(uint64_t bound_exclusive) {
  if (bound_exclusive <= 1) {
    return 0;
  }
  const uint64_t range = bound_exclusive - 1;
  if (range > 0xFFFFFFFFull) {
    throw std::invalid_argument("bounded() above 2^32 is not needed by the modem");
  }
  // Lemire's multiply-shift with rejection, matching ``Generator.integers``.
  const uint32_t range32 = static_cast<uint32_t>(range);
  const uint32_t span = range32 + 1u;
  uint64_t product = static_cast<uint64_t>(next_uint32()) * span;
  uint32_t leftover = static_cast<uint32_t>(product & 0xFFFFFFFFu);
  if (leftover < span) {
    const uint32_t threshold = (0xFFFFFFFFu - range32) % span;
    while (leftover < threshold) {
      product = static_cast<uint64_t>(next_uint32()) * span;
      leftover = static_cast<uint32_t>(product & 0xFFFFFFFFu);
    }
  }
  return product >> 32;
}

uint32_t NumpyGenerator::masked_bounded(uint32_t bound_inclusive) {
  const uint32_t mask = bit_mask_for(bound_inclusive);
  uint32_t value = next_uint32() & mask;
  while (value > bound_inclusive) {
    value = next_uint32() & mask;
  }
  return value;
}

std::vector<uint32_t> NumpyGenerator::permutation(std::size_t size) {
  std::vector<uint32_t> values(size);
  for (std::size_t i = 0; i < size; ++i) {
    values[i] = static_cast<uint32_t>(i);
  }
  for (std::size_t i = size; i-- > 1;) {
    const uint32_t j = masked_bounded(static_cast<uint32_t>(i));
    std::swap(values[i], values[j]);
  }
  return values;
}

double NumpyGenerator::standard_normal() {
  using namespace detail;
  for (;;) {
    const uint64_t draw = next_uint64();
    const std::size_t index = static_cast<std::size_t>(draw & 0xFFu);
    const uint64_t rest = draw >> 8;
    const bool negative = (rest & 1u) != 0;
    const uint64_t magnitude = (rest >> 1) & 0x000FFFFFFFFFFFFFull;

    double x = static_cast<double>(magnitude) * kZigguratWi[index];
    if (negative) {
      x = -x;
    }
    // ~99.3% of draws land inside the strip and return here.
    if (magnitude < kZigguratKi[index]) {
      return x;
    }
    if (index == 0) {
      // Tail: sample the exponential beyond R by rejection. 1-U rather than U
      // so the log never sees zero.
      for (;;) {
        const double xx = -kZigguratInvR * std::log1p(-next_double());
        const double yy = -std::log1p(-next_double());
        if (yy + yy > xx * xx) {
          return ((magnitude >> 8) & 1u) ? -(kZigguratR + xx) : (kZigguratR + xx);
        }
      }
    }
    const double wedge =
        (kZigguratFi[index - 1] - kZigguratFi[index]) * next_double() + kZigguratFi[index];
    if (wedge < std::exp(-0.5 * x * x)) {
      return x;
    }
  }
}

std::vector<uint8_t> NumpyGenerator::bytes(std::size_t count) {
  std::vector<uint8_t> out;
  out.reserve(count);
  while (out.size() < count) {
    uint64_t word = next_uint64();
    for (int i = 0; i < 8 && out.size() < count; ++i) {
      out.push_back(static_cast<uint8_t>(word & 0xFFu));
      word >>= 8;
    }
  }
  return out;
}

// ---- PythonRandom ---------------------------------------------------------
//
// CPython's MT19937 with ``init_by_array`` seeding, which is what
// ``random.Random(n)`` does for an integer seed.

namespace {
constexpr uint32_t kMtMatrixA = 0x9908b0dfu;
constexpr uint32_t kMtUpperMask = 0x80000000u;
constexpr uint32_t kMtLowerMask = 0x7fffffffu;
}  // namespace

PythonRandom::PythonRandom(uint64_t seed) {
  state_[0] = 19650218u;
  for (int i = 1; i < 624; ++i) {
    state_[static_cast<std::size_t>(i)] =
        1812433253u * (state_[static_cast<std::size_t>(i - 1)] ^
                       (state_[static_cast<std::size_t>(i - 1)] >> 30)) +
        static_cast<uint32_t>(i);
  }

  std::vector<uint32_t> key;
  if (seed == 0) {
    key.push_back(0u);
  } else {
    uint64_t rest = seed;
    while (rest != 0) {
      key.push_back(static_cast<uint32_t>(rest & 0xFFFFFFFFu));
      rest >>= 32;
    }
  }

  std::size_t i = 1;
  std::size_t j = 0;
  std::size_t k = std::max<std::size_t>(624, key.size());
  for (; k != 0; --k) {
    state_[i] = (state_[i] ^ ((state_[i - 1] ^ (state_[i - 1] >> 30)) * 1664525u)) +
                key[j] + static_cast<uint32_t>(j);
    ++i;
    ++j;
    if (i >= 624) {
      state_[0] = state_[623];
      i = 1;
    }
    if (j >= key.size()) {
      j = 0;
    }
  }
  for (k = 623; k != 0; --k) {
    state_[i] = (state_[i] ^ ((state_[i - 1] ^ (state_[i - 1] >> 30)) * 1566083941u)) -
                static_cast<uint32_t>(i);
    ++i;
    if (i >= 624) {
      state_[0] = state_[623];
      i = 1;
    }
  }
  state_[0] = 0x80000000u;
  index_ = 624;
}

void PythonRandom::twist() {
  for (std::size_t i = 0; i < 624; ++i) {
    const uint32_t y =
        (state_[i] & kMtUpperMask) | (state_[(i + 1) % 624] & kMtLowerMask);
    uint32_t next = state_[(i + 397) % 624] ^ (y >> 1);
    if (y & 1u) {
      next ^= kMtMatrixA;
    }
    state_[i] = next;
  }
  index_ = 0;
}

uint32_t PythonRandom::genrand_uint32() {
  if (index_ >= 624) {
    twist();
  }
  uint32_t y = state_[static_cast<std::size_t>(index_++)];
  y ^= (y >> 11);
  y ^= (y << 7) & 0x9d2c5680u;
  y ^= (y << 15) & 0xefc60000u;
  y ^= (y >> 18);
  return y;
}

double PythonRandom::random() {
  const uint32_t a = genrand_uint32() >> 5;
  const uint32_t b = genrand_uint32() >> 6;
  return (static_cast<double>(a) * 67108864.0 + static_cast<double>(b)) *
         (1.0 / 9007199254740992.0);
}

uint64_t PythonRandom::getrandbits(int bits) {
  if (bits <= 0 || bits > 64) {
    throw std::invalid_argument("getrandbits supports 1..64 bits");
  }
  if (bits <= 32) {
    return genrand_uint32() >> (32 - bits);
  }
  // Words are filled low-to-high; only the last one is narrowed.
  uint64_t result = 0;
  int remaining = bits;
  for (int word = 0; remaining > 0; ++word, remaining -= 32) {
    uint32_t value = genrand_uint32();
    if (remaining < 32) {
      value >>= (32 - remaining);
    }
    result |= static_cast<uint64_t>(value) << (32 * word);
  }
  return result;
}

uint64_t PythonRandom::randbelow(uint64_t stop) {
  if (stop == 0) {
    return 0;
  }
  int bits = 0;
  for (uint64_t v = stop; v != 0; v >>= 1) {
    ++bits;
  }
  uint64_t value = getrandbits(bits);
  while (value >= stop) {
    value = getrandbits(bits);
  }
  return value;
}

int64_t PythonRandom::randint(int64_t low, int64_t high) {
  const uint64_t width = static_cast<uint64_t>(high - low) + 1u;
  return low + static_cast<int64_t>(randbelow(width));
}

std::vector<uint8_t> PythonRandom::choices(const std::vector<uint8_t>& population,
                                           std::size_t count) {
  std::vector<uint8_t> out;
  out.reserve(count);
  const double size = static_cast<double>(population.size());
  for (std::size_t i = 0; i < count; ++i) {
    const auto index = static_cast<std::size_t>(random() * size);
    out.push_back(population[index]);
  }
  return out;
}

uint8_t PythonRandom::choice(const std::vector<uint8_t>& population) {
  return population[static_cast<std::size_t>(randbelow(population.size()))];
}

}  // namespace weaklink::rng
