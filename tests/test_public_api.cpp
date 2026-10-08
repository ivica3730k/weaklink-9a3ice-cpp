// The public API surface. Anything asserted here is part of the compatibility
// contract: changing a signature or the exception type a call raises is a
// breaking change.

#include <algorithm>
#include <cmath>

#include "catch.hpp"
#include "support.hpp"
#include "weaklink/api.hpp"
#include "weaklink/constants.hpp"
#include "weaklink/exceptions.hpp"

using namespace weaklink;
using namespace weaklink::test;

TEST_CASE("bytes in, bytes out", "[api]") {
  const ByteVector payload = to_bytes("weaklink public API roundtrip");
  api::ModemOptions options;
  options.baud = 300.0;

  const Samples audio = api::tx(payload, options);
  REQUIRE_FALSE(audio.empty());

  api::RxSource source;
  source.samples = &audio;
  REQUIRE(api::rx(source, options) == payload);
}

TEST_CASE("preset defaults land", "[api]") {
  api::ModemOptions options;
  options.baud = 300.0;
  const ModemConfig config = api::build_config(options);

  const BaudPreset& preset = baud_presets().at(300.0);
  REQUIRE(config.rs_data_bytes() == preset.rs_data_bytes);
  REQUIRE(config.block_repeats() == preset.block_repeats);
  REQUIRE(config.waveform().tone_spacing_hz() == preset.tone_spacing_hz);
}

TEST_CASE("an explicit option beats the preset", "[api]") {
  api::ModemOptions options;
  options.baud = 300.0;
  options.block_repeats = 7;
  REQUIRE(api::build_config(options).block_repeats() == 7);
}

TEST_CASE("tx_volume scales the peak amplitude", "[api]") {
  const ByteVector payload = to_bytes("loud vs quiet");
  api::ModemOptions options;
  options.baud = 300.0;

  const auto peak = [](const Samples& samples) {
    float highest = 0.0f;
    for (const float sample : samples) {
      highest = std::max(highest, std::abs(sample));
    }
    return highest;
  };

  REQUIRE(peak(api::tx(payload, options, 100)) == Approx(1.0).margin(0.01));
  REQUIRE(peak(api::tx(payload, options, 25)) == Approx(0.25).margin(0.01));
}

TEST_CASE("an unsupported baud raises ConfigError", "[api]") {
  api::ModemOptions options;
  options.baud = 888.0;
  REQUIRE_THROWS_AS(api::tx(to_bytes("x"), options), ConfigError);
  // Every modem error shares one base, so callers can catch just that.
  REQUIRE_THROWS_AS(api::tx(to_bytes("x"), options), WeaklinkError);
}

TEST_CASE("an infeasible tone count raises NyquistError", "[api]") {
  // 16 tones at 1200 baud puts the top tone above Nyquist.
  api::ModemOptions options;
  options.baud = 1200.0;
  options.num_tones = 16;
  REQUIRE_THROWS_AS(api::tx(to_bytes("x"), options), NyquistError);
}

TEST_CASE("an out-of-range tx_volume raises ConfigError", "[api]") {
  api::ModemOptions options;
  REQUIRE_THROWS_AS(api::build_config(options, 101), ConfigError);
  REQUIRE_THROWS_AS(api::build_config(options, -1), ConfigError);
}

TEST_CASE("rx requires exactly one source", "[api]") {
  api::ModemOptions options;

  api::RxSource none;
  REQUIRE_THROWS_AS(api::rx(none, options), ConfigError);

  const Samples samples(100, 0.0f);
  api::RxSource both;
  both.samples = &samples;
  both.wav_path = "ignored.wav";
  REQUIRE_THROWS_AS(api::rx(both, options), ConfigError);
}

TEST_CASE("tx rejects contradictory sinks", "[api]") {
  api::ModemOptions options;

  api::TxTarget wav_and_device;
  wav_and_device.wav_path = "out.wav";
  wav_and_device.to_audio_device = true;
  REQUIRE_THROWS_AS(api::tx(to_bytes("x"), options, 100, wav_and_device), ConfigError);

  api::TxTarget wav_and_ptt;
  wav_and_ptt.wav_path = "out.wav";
  wav_and_ptt.ptt = "localhost:4532";
  REQUIRE_THROWS_AS(api::tx(to_bytes("x"), options, 100, wav_and_ptt), ConfigError);

  api::TxTarget tune_to_wav;
  tune_to_wav.wav_path = "out.wav";
  tune_to_wav.tune = true;
  REQUIRE_THROWS_AS(api::tx(to_bytes("x"), options, 100, tune_to_wav), ConfigError);
}

TEST_CASE("the baud preset table is frozen", "[api]") {
  // Adding or renaming a supported baud is a breaking change for both sides of
  // a link, so accidental edits should fail loudly here.
  REQUIRE(baud_presets().size() == 3);
  REQUIRE(baud_presets().count(45.0) == 1);
  REQUIRE(baud_presets().count(300.0) == 1);
  REQUIRE(baud_presets().count(1200.0) == 1);
}
