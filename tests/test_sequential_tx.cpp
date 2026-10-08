// One RX session watching while several TX sessions fire in sequence.
//
// This is what a live listener sees when the same RX pipe is left open across
// several independent ``tx`` invocations. Each TX buffer carries its own pilot
// padding, so preambles from adjacent sessions are separated by real silence
// rather than butted together -- which is what lets the decoder find the
// message boundary between them.

#include <string>
#include <vector>

#include "catch.hpp"
#include "support.hpp"
#include "weaklink/rng.hpp"

using namespace weaklink;
using namespace weaklink::test;

namespace {

std::vector<ByteVector> random_payloads(uint64_t seed, int count) {
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
  alphabet.push_back(static_cast<uint8_t>(' '));

  rng::PythonRandom generator(seed);
  std::vector<ByteVector> payloads;
  payloads.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    const auto length = static_cast<std::size_t>(generator.randint(1, 20));
    ByteVector payload;
    payload.reserve(length);
    for (std::size_t j = 0; j < length; ++j) {
      payload.push_back(generator.choice(alphabet));
    }
    payloads.push_back(std::move(payload));
  }
  return payloads;
}

}  // namespace

TEST_CASE("ten sequential transmissions all decode in order", "[sequential][slow]") {
  for (const double baud : {45.0, 300.0, 1200.0}) {
    const std::vector<ByteVector> payloads =
        random_payloads(static_cast<uint64_t>(42 + static_cast<int>(baud)), 10);

    ByteVector expected;
    Samples combined;
    ModemConfig config = preset_config(baud);
    for (const ByteVector& payload : payloads) {
      expected.insert(expected.end(), payload.begin(), payload.end());
      const LiveTxBuffer buffer = live_tx_buffer(baud, payload);
      config = buffer.config;
      combined.insert(combined.end(), buffer.audio.begin(), buffer.audio.end());
    }

    INFO(baud << " baud, " << payloads.size() << " sessions");
    REQUIRE(codec::decode(combined, config).bytes == expected);

    // 45 baud is omitted from the streaming leg: the audio runs long enough
    // that the chunked pass dominates the suite's runtime without testing
    // anything the 300 and 1200 baud cases do not.
    if (baud > 45.0) {
      REQUIRE(stream_decode(combined, config) == expected);
    }
  }
}
