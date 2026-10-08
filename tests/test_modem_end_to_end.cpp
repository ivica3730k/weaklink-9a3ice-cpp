// End-to-end round-trips, batch and streaming, plus an SNR sweep baseline.

#include <string>
#include <vector>

#include "catch.hpp"
#include "support.hpp"
#include "weaklink/rng.hpp"
#include "weaklink/wav.hpp"

using namespace weaklink;
using namespace weaklink::test;

namespace {

ByteVector random_ascii(uint64_t seed, std::size_t count) {
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
  return rng::PythonRandom(seed).choices(alphabet, count);
}

}  // namespace

TEST_CASE("short payload round-trips through a WAV file", "[e2e]") {
  const ModemConfig config = default_config();
  const ByteVector payload = to_bytes("round-trip via WAV");
  const Samples samples = codec::encode(payload, config);

  const std::string path = "wl_test_roundtrip.wav";
  audio::write_wav(path, samples, config.waveform().sample_rate());
  const audio::WavData reloaded = audio::read_wav(path, config.waveform().sample_rate());
  std::remove(path.c_str());

  REQUIRE(reloaded.sample_rate ==
          static_cast<int>(std::lround(config.waveform().sample_rate())));
  REQUIRE(strip_trailing_nul(codec::decode(reloaded.samples, config).bytes) == payload);
}

TEST_CASE("short payload round-trips in batch mode", "[e2e]") {
  const ModemConfig config = default_config();
  const ByteVector payload = to_bytes("weaklink modem streaming hello");
  const Samples samples = codec::encode(payload, config);
  REQUIRE(strip_trailing_nul(codec::decode(samples, config).bytes) == payload);
}

TEST_CASE("short payload round-trips through the streaming decoder", "[e2e]") {
  // Same audio and assertion as the batch test, but driven through the live-rx
  // code path: chunked pushes, cross-call state, drain at the end.
  const ModemConfig config = default_config();
  const ByteVector payload = to_bytes("weaklink modem streaming hello");
  const Samples samples = codec::encode(payload, config);
  REQUIRE(strip_trailing_nul(stream_decode(samples, config)) == payload);
}

TEST_CASE("100 random bytes round-trip", "[e2e]") {
  const ModemConfig config = default_config();
  const ByteVector payload = random_ascii(7, 100);
  const Samples samples = codec::encode(payload, config);

  SECTION("batch") {
    REQUIRE(strip_trailing_nul(codec::decode(samples, config).bytes) == payload);
  }
  SECTION("streaming") {
    REQUIRE(strip_trailing_nul(stream_decode(samples, config)) == payload);
  }
}

TEST_CASE("decode is reliable at +5 dB SNR", "[e2e][slow]") {
  // The sweep below is printed for context; the assertion is that the top of
  // the sweep is solid, which is what regressions actually break.
  const ModemConfig config = default_config();  // 300 baud
  const ByteVector payload = random_ascii(1337, 20);
  const Samples samples = codec::encode(payload, config);

  constexpr int kTrials = 10;
  const auto successes_at = [&](double snr_db) {
    int successes = 0;
    for (int trial = 0; trial < kTrials; ++trial) {
      const Samples noisy = add_awgn_scaled_in_double(
          samples, snr_db, static_cast<uint64_t>(trial), kReferenceBandwidthHz,
          config.waveform().sample_rate());
      if (strip_trailing_nul(codec::decode(noisy, config).bytes) == payload) {
        ++successes;
      }
    }
    return successes;
  };

  for (const double snr_db : {5.0, 0.0, -3.0, -5.0, -8.0, -10.0}) {
    const int successes = successes_at(snr_db);
    WARN("SNR " << snr_db << " dB in 3 kHz: " << (100 * successes / kTrials) << "% decoded");
  }

  REQUIRE(successes_at(5.0) == kTrials);
}
