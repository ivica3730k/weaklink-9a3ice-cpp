// Regression tests for the preamble correlator and its signal-presence gate.
//
// Guards against: a pure-noise buffer latching onto "one peak at [0]", a
// buffer-edge transient masking real preambles, and false positives on data
// that is valid audio but not a preamble.

#include <cmath>
#include <vector>

#include "catch.hpp"
#include "support.hpp"
#include "weaklink/rng.hpp"

using namespace weaklink;
using namespace weaklink::test;

namespace {

constexpr double kTwoPi = 6.283185307179586;

ModemConfig correlator_config() { return make_config(300.0, 300.0, 4, 16, 8, 1); }

Samples gaussian_noise(uint64_t seed, std::size_t count, float scale) {
  rng::NumpyGenerator generator(seed);
  Samples out(count);
  for (float& sample : out) {
    sample = static_cast<float>(generator.standard_normal()) * scale;
  }
  return out;
}

}  // namespace

TEST_CASE("pure noise finds no preambles", "[correlator]") {
  const ModemConfig config = correlator_config();
  // 5 s of Gaussian noise at a typical mic-input level.
  const Samples noise = gaussian_noise(
      0, static_cast<std::size_t>(5 * config.waveform().sample_rate()), 0.05f);
  const dsp::Magnitudes magnitudes = dsp::demodulate_soft(noise, config.waveform());
  REQUIRE(codec::find_preamble_peaks(magnitudes,
                                     codec::preamble_for(config.waveform().num_tones()),
                                     config)
              .empty());
}

TEST_CASE("streaming on pure noise does not advance the cursor", "[correlator]") {
  // Live-rx invariant: a noise-only buffer must not move the cursor forward,
  // or the next call starts past audio that has not been looked at yet.
  const ModemConfig config = correlator_config();
  const Samples noise = gaussian_noise(
      1, static_cast<std::size_t>(5 * config.waveform().sample_rate()), 0.05f);
  codec::StreamingState state;
  const codec::DecodeResult result = codec::decode(noise, config, true, &state);
  REQUIRE(result.bytes.empty());
  REQUIRE(result.safe_cursor_samples == 0);
}

TEST_CASE("a loud edge transient does not mask the signal after it", "[correlator]") {
  // A mic click or keyboard bump at buffer start must not stop the real
  // preambles that follow from being found.
  const ModemConfig config = correlator_config();
  const ByteVector payload = to_bytes("hello weaklink");
  const Samples real = codec::encode(payload, config);

  const auto lead_length = static_cast<std::size_t>(0.3 * config.waveform().sample_rate());
  const Samples lead = gaussian_noise(2, lead_length, 0.9f);
  const Samples tail(static_cast<std::size_t>(0.5 * config.waveform().sample_rate()), 0.0f);

  Samples buffer;
  buffer.insert(buffer.end(), lead.begin(), lead.end());
  buffer.insert(buffer.end(), real.begin(), real.end());
  buffer.insert(buffer.end(), tail.begin(), tail.end());

  REQUIRE(contains(codec::decode(buffer, config).bytes, payload));
  REQUIRE(contains(stream_decode(buffer, config), payload));
}

TEST_CASE("modulated random data produces almost no false peaks", "[correlator]") {
  // Valid audio that is not a preamble has zero expected correlation with the
  // PN sequence, but non-zero variance -- allow at most one spurious hit.
  const ModemConfig config = correlator_config();
  rng::NumpyGenerator generator(3);
  std::vector<int> symbols(1000);
  for (int& symbol : symbols) {
    symbol = static_cast<int>(generator.bounded(4));
  }
  const Samples audio = dsp::modulate(symbols, config.waveform());
  const dsp::Magnitudes magnitudes = dsp::demodulate_soft(audio, config.waveform());
  const auto peaks = codec::find_preamble_peaks(
      magnitudes, codec::preamble_for(config.waveform().num_tones()), config);
  INFO("found " << peaks.size() << " spurious peaks");
  REQUIRE(peaks.size() <= 1);
}

TEST_CASE("the correlator is amplitude-invariant under slow fading", "[correlator][slow]") {
  // Scores are normalised by window energy, so a 10 dB fade should not hide a
  // preamble that a louder stretch would reveal.
  struct Preset {
    double baud;
    int rs_data;
    int block_repeats;
  };
  for (const Preset& preset : {Preset{45.0, 32, 2}, Preset{300.0, 16, 1}}) {
    const ModemConfig config =
        make_config(preset.baud, preset.baud, 4, preset.rs_data, 8, preset.block_repeats);
    const ByteVector payload = to_bytes("weaklink fading test payload 12345678 abcdef");
    const Samples signal = codec::encode(payload, config);

    const double sample_rate = config.waveform().sample_rate();
    const double duration = static_cast<double>(signal.size()) / sample_rate;
    // A few fade cycles across the burst so different preambles land on
    // different fade phases.
    const double period = std::max(duration / 2.5, 1.0);

    Samples faded(signal.size());
    double sum_squares = 0.0;
    for (std::size_t i = 0; i < signal.size(); ++i) {
      const double t = static_cast<double>(i) / sample_rate;
      const double envelope = 0.316 + 0.684 * (0.5 + 0.5 * std::cos(kTwoPi * t / period));
      faded[i] = static_cast<float>(static_cast<double>(signal[i]) * envelope);
      sum_squares += static_cast<double>(faded[i]) * static_cast<double>(faded[i]);
    }

    const double signal_power = sum_squares / static_cast<double>(faded.size());
    const auto sigma = static_cast<float>(std::sqrt(signal_power * std::pow(10.0, -5.0 / 10.0)));
    rng::NumpyGenerator generator(0);
    Samples noisy(faded.size());
    for (std::size_t i = 0; i < faded.size(); ++i) {
      noisy[i] = faded[i] + static_cast<float>(generator.standard_normal()) * sigma;
    }

    INFO(preset.baud << " baud + 10 dB fade + 5 dB SNR");
    REQUIRE(contains(codec::decode(noisy, config).bytes, payload));
  }
}
