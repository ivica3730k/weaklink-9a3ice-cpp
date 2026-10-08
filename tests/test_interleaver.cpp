// Interleaver tests.

#include <vector>

#include "catch.hpp"
#include "weaklink/interleaver.hpp"
#include "weaklink/rng.hpp"

using namespace weaklink;
using interleaver::InterleaverConfig;

namespace {

std::vector<double> soft_from_bits(const BitVector& bits) {
  std::vector<double> soft(bits.size());
  for (std::size_t i = 0; i < bits.size(); ++i) {
    soft[i] = bits[i] == 0 ? 1.0 : -1.0;
  }
  return soft;
}

}  // namespace

TEST_CASE("hard bits round-trip through the permutation", "[interleaver]") {
  const InterleaverConfig config{4, 8};
  rng::NumpyGenerator generator(0);
  BitVector bits(config.block_size());
  for (uint8_t& bit : bits) {
    bit = static_cast<uint8_t>(generator.bounded(2));
  }

  const BitVector interleaved = interleaver::interleave(bits, config, 0);
  const std::vector<double> recovered =
      interleaver::deinterleave_soft(soft_from_bits(interleaved), config, bits.size(), 0);

  BitVector hard(recovered.size());
  for (std::size_t i = 0; i < recovered.size(); ++i) {
    hard[i] = recovered[i] < 0.0 ? 1u : 0u;
  }
  REQUIRE(hard == bits);
}

TEST_CASE("short input is padded up to a whole block", "[interleaver]") {
  const InterleaverConfig config{4, 8};
  const BitVector bits = {1, 0, 1, 1, 0};  // not a multiple of 32
  const BitVector interleaved = interleaver::interleave(bits, config, 0);
  REQUIRE(interleaved.size() == config.block_size());

  const std::vector<double> recovered =
      interleaver::deinterleave_soft(soft_from_bits(interleaved), config, bits.size(), 0);
  BitVector hard(recovered.size());
  for (std::size_t i = 0; i < recovered.size(); ++i) {
    hard[i] = recovered[i] < 0.0 ? 1u : 0u;
  }
  REQUIRE(hard == bits);
}

TEST_CASE("deinterleave rejects a stream shorter than one block", "[interleaver]") {
  const InterleaverConfig config{4, 8};
  REQUIRE_THROWS_AS(
      interleaver::deinterleave_soft(std::vector<double>(10, 0.0), config, 32, 0),
      std::invalid_argument);
}

TEST_CASE("adjacent blocks use different permutations", "[interleaver]") {
  // This is what breaks up periodic noise: with one fixed shuffle, a repeating
  // disturbance would hit the same bit positions in every block.
  const InterleaverConfig config{8, 32};
  BitVector bits(config.block_size(), 0);
  bits[0] = 1;
  bits[1] = 1;
  REQUIRE(interleaver::interleave(bits, config, 0) != interleaver::interleave(bits, config, 1));
}

TEST_CASE("permutations repeat once per cycle", "[interleaver]") {
  const InterleaverConfig config{8, 32};
  BitVector bits(config.block_size(), 0);
  bits[3] = 1;
  const std::size_t cycle = interleaver::cycle_size();
  REQUIRE(interleaver::interleave(bits, config, 2) ==
          interleaver::interleave(bits, config, 2 + cycle));
}
