// Pipe arbitrary-length files through the modem end to end. The payload is
// padded to a whole number of blocks on the way out; the caller strips the
// trailing NULs on the way back in.

#include <vector>

#include "catch.hpp"
#include "support.hpp"
#include "weaklink/rng.hpp"

using namespace weaklink;
using namespace weaklink::test;

namespace {

ByteVector random_text(uint64_t seed, std::size_t size) {
  std::vector<uint8_t> alphabet;
  for (char c = 'a'; c <= 'z'; ++c) {
    alphabet.push_back(static_cast<uint8_t>(c));
  }
  for (char c = 'A'; c <= 'Z'; ++c) {
    alphabet.push_back(static_cast<uint8_t>(c));
  }
  for (char c = '0'; c <= '9'; ++c) {
    alphabet.push_back(static_cast<uint8_t>(c));
  }
  for (const char c : {'!', '"', '#', '$', '%', '&', ' ', '\n'}) {
    alphabet.push_back(static_cast<uint8_t>(c));
  }
  return rng::PythonRandom(seed).choices(alphabet, size);
}

}  // namespace

TEST_CASE("files of various sizes round-trip", "[pipe][slow]") {
  struct Case {
    std::size_t size;
    uint64_t seed;
  };
  for (const Case& test_case : {Case{500, 1}, Case{1000, 2}}) {
    const ByteVector payload = random_text(test_case.seed, test_case.size);
    const ModemConfig config = make_config(300.0, 300.0, 4, 16, 8, 1);
    const Samples samples = codec::encode(payload, config);

    INFO("payload size " << test_case.size);
    REQUIRE(strip_trailing_nul(codec::decode(samples, config).bytes) == payload);
    REQUIRE(strip_trailing_nul(stream_decode(samples, config)) == payload);
  }
}

TEST_CASE("an odd-sized payload pads out and strips back", "[pipe]") {
  // 97 bytes is not a multiple of the 13 payload bytes a block carries.
  const ByteVector payload = random_text(3, 97);
  const ModemConfig config = make_config(300.0, 300.0, 4, 16, 8, 1);
  const Samples samples = codec::encode(payload, config);

  const ByteVector decoded = strip_trailing_nul(codec::decode(samples, config).bytes);
  REQUIRE(decoded == payload);
  REQUIRE(decoded.size() == 97);
  REQUIRE(strip_trailing_nul(stream_decode(samples, config)) == payload);
}
