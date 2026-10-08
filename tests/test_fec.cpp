// Convolutional encoder + soft Viterbi tests.

#include <vector>

#include "catch.hpp"
#include "weaklink/fec.hpp"
#include "weaklink/rng.hpp"

using namespace weaklink;

namespace {

std::vector<double> soft_from_bits(const BitVector& bits) {
  std::vector<double> soft(bits.size());
  for (std::size_t i = 0; i < bits.size(); ++i) {
    soft[i] = bits[i] == 0 ? 1.0 : -1.0;
  }
  return soft;
}

BitVector random_bits(uint64_t seed, std::size_t count) {
  rng::NumpyGenerator generator(seed);
  BitVector bits(count);
  for (uint8_t& bit : bits) {
    bit = static_cast<uint8_t>(generator.bounded(2));
  }
  return bits;
}

}  // namespace

TEST_CASE("encode adds the tail flush", "[fec]") {
  const BitVector bits = {1, 0, 1, 1, 0, 0, 1, 0};
  REQUIRE(fec::encode(bits).size() == 2 * (bits.size() + fec::kConstraintLength - 1));
}

TEST_CASE("all-zero input encodes to all zeros", "[fec]") {
  const BitVector bits(20, 0);
  const BitVector coded = fec::encode(bits);
  REQUIRE(coded.size() == 2 * (20 + fec::kConstraintLength - 1));
  REQUIRE(std::all_of(coded.begin(), coded.end(), [](uint8_t b) { return b == 0; }));
}

TEST_CASE("Viterbi round-trips a clean channel", "[fec]") {
  for (const std::size_t length : {std::size_t{1}, std::size_t{8}, std::size_t{32},
                                   std::size_t{128}}) {
    const BitVector bits = random_bits(length, length);
    REQUIRE(fec::decode(soft_from_bits(fec::encode(bits)), length) == bits);
  }
}

TEST_CASE("Viterbi shrugs off scattered bit flips", "[fec]") {
  const BitVector bits = random_bits(42, 64);
  BitVector coded = fec::encode(bits);
  // Three flips out of ~140 coded bits is well inside a rate-1/2 K=7 code's
  // reach.
  for (const std::size_t position : {std::size_t{7}, std::size_t{61}, std::size_t{103}}) {
    coded[position] ^= 1u;
  }
  REQUIRE(fec::decode(soft_from_bits(coded), 64) == bits);
}

TEST_CASE("decode rejects a mismatched soft-bit length", "[fec]") {
  REQUIRE_THROWS_AS(fec::decode(std::vector<double>(10, 0.0), 8), std::invalid_argument);
}
