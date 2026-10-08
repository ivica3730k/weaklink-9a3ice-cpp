// The Nyquist / spacing guardrail fires when the waveform config is built,
// which is the same code path TX and RX both go through. A config that is
// infeasible for one side is therefore infeasible for the other -- this guards
// a report where only one side enforced it.

#include "catch.hpp"
#include "weaklink/codec.hpp"
#include "weaklink/exceptions.hpp"
#include "weaklink/waveform.hpp"

using namespace weaklink;

namespace {

dsp::WaveformOptions infeasible() {
  // 32 tones at 300 baud with 300 Hz spacing puts the top tone above the
  // 9 kHz Nyquist of the 18 kHz internal rate.
  dsp::WaveformOptions options;
  options.baud = 300.0;
  options.tone_spacing_hz = 300.0;
  options.num_tones = 32;
  return options;
}

}  // namespace

TEST_CASE("an infeasible tone stack raises at construction", "[nyquist]") {
  REQUIRE_THROWS_AS(infeasible().build(), NyquistError);
}

TEST_CASE("NyquistError is a ConfigError is a WeaklinkError", "[nyquist]") {
  // Callers catch the base types, so the hierarchy is part of the contract.
  REQUIRE_THROWS_AS(infeasible().build(), ConfigError);
  REQUIRE_THROWS_AS(infeasible().build(), WeaklinkError);
}

TEST_CASE("sub-500 Hz tone stacks are shifted up, not rejected", "[nyquist]") {
  dsp::WaveformOptions options;
  options.baud = 300.0;
  options.tone_spacing_hz = 1200.0;
  options.num_tones = 4;
  const dsp::WaveformConfig config = options.build();
  REQUIRE(config.tones_hz().front() >= dsp::WaveformConfig::kMinToneHz);
}

TEST_CASE("too few samples per symbol is rejected", "[nyquist]") {
  dsp::WaveformOptions options;
  options.baud = 4000.0;  // 18000 / 4000 = 4.5 samples per symbol
  REQUIRE_THROWS_AS(options.build(), ConfigError);
}
