#include "weaklink/interleaver.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include "weaklink/rng.hpp"

namespace weaklink::interleaver {
namespace {

/// Base seed the per-block permutation is XOR'd with. Any fixed non-zero value
/// works; it is shared with the Python implementation so both derive the same
/// shuffle.
constexpr uint32_t kSeedBase = 0xC0DEC0DEu;

/// Knuth's multiplicative constant, used to spread adjacent slot numbers
/// across the seed space.
constexpr uint32_t kSlotSpread = 2654435761u;

/// 32 keeps the cached pool tiny while still giving 32 unique bit orderings
/// before any repeat, and bounds RX's worst-case seed search.
constexpr std::size_t kCycleSize = 32;

struct Permutation {
  std::vector<uint32_t> forward;
  std::vector<uint32_t> inverse;
};

/// Permutations are deterministic in (slot, size) and a stream touches at most
/// ``kCycleSize`` of them per size, so computing one twice is pure waste.
const Permutation& permutation_for(std::size_t slot, std::size_t size) {
  static std::mutex mutex;
  static std::map<std::pair<std::size_t, std::size_t>, Permutation> cache;

  const auto key = std::make_pair(slot, size);
  std::lock_guard<std::mutex> guard(mutex);
  const auto found = cache.find(key);
  if (found != cache.end()) {
    return found->second;
  }

  const uint32_t seed =
      kSeedBase ^ static_cast<uint32_t>(static_cast<uint32_t>(slot) * kSlotSpread);
  rng::NumpyGenerator generator(seed);

  Permutation permutation;
  permutation.forward = generator.permutation(size);
  permutation.inverse.resize(size);
  for (std::size_t i = 0; i < size; ++i) {
    permutation.inverse[permutation.forward[i]] = static_cast<uint32_t>(i);
  }
  return cache.emplace(key, std::move(permutation)).first->second;
}

}  // namespace

std::size_t cycle_size() { return kCycleSize; }

BitVector interleave(const BitVector& bits, const InterleaverConfig& config,
                     std::size_t block_index) {
  const std::size_t padded_length = round_up_multiple(bits.size(), config.block_size());
  BitVector padded(padded_length, 0);
  std::copy(bits.begin(), bits.end(), padded.begin());

  const Permutation& permutation = permutation_for(block_index % kCycleSize, padded_length);
  BitVector out(padded_length);
  for (std::size_t i = 0; i < padded_length; ++i) {
    out[i] = padded[permutation.forward[i]];
  }
  return out;
}

std::vector<double> deinterleave_soft(const std::vector<double>& soft,
                                      const InterleaverConfig& config,
                                      std::size_t output_length,
                                      std::size_t block_index) {
  const std::size_t padded_length = round_up_multiple(output_length, config.block_size());
  if (soft.size() < padded_length) {
    throw std::invalid_argument("soft stream length " + std::to_string(soft.size()) +
                                " shorter than padded target " +
                                std::to_string(padded_length));
  }
  const Permutation& permutation = permutation_for(block_index % kCycleSize, padded_length);
  std::vector<double> out(output_length);
  for (std::size_t i = 0; i < output_length; ++i) {
    out[i] = soft[permutation.inverse[i]];
  }
  return out;
}

}  // namespace weaklink::interleaver
