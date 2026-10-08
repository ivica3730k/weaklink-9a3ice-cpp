// MFSK waveform + soft demod unit tests.

#include <algorithm>
#include <vector>

#include "catch.hpp"
#include "weaklink/exceptions.hpp"
#include "weaklink/rng.hpp"
#include "weaklink/waveform.hpp"

using namespace weaklink;
using dsp::WaveformOptions;

namespace {

dsp::WaveformConfig config_48k() {
  WaveformOptions options;
  options.baud = 300.0;
  options.sample_rate = 48000.0;
  return options.build();
}

}  // namespace

TEST_CASE("bits and symbols round-trip", "[waveform]") {
  rng::NumpyGenerator generator(0);
  BitVector bits(400);
  for (uint8_t& bit : bits) {
    bit = static_cast<uint8_t>(generator.bounded(2));
  }
  const std::vector<int> symbols = dsp::bits_to_symbols(bits, 4);
  REQUIRE(*std::max_element(symbols.begin(), symbols.end()) < 4);
  REQUIRE(*std::min_element(symbols.begin(), symbols.end()) >= 0);
  REQUIRE(dsp::symbols_to_bits(symbols, 4) == bits);
}

TEST_CASE("bits_to_symbols rejects a partial symbol", "[waveform]") {
  REQUIRE_THROWS_AS(dsp::bits_to_symbols(BitVector{1, 0, 1}, 4), std::invalid_argument);
}

TEST_CASE("modulate produces one symbol period per symbol", "[waveform]") {
  const dsp::WaveformConfig config = config_48k();
  const std::vector<int> symbols = {0, 1, 2, 3, 0};
  const dsp::Samples samples = dsp::modulate(symbols, config);
  REQUIRE(samples.size() ==
          symbols.size() * static_cast<std::size_t>(config.samples_per_symbol()));
  for (const float sample : samples) {
    REQUIRE(std::abs(sample) <= config.amplitude() + 1e-6);
  }
}

TEST_CASE("demodulate recovers symbols on a clean channel", "[waveform]") {
  const dsp::WaveformConfig config = config_48k();
  const std::vector<int> original = {0, 1, 2, 3, 3, 2, 1, 0, 2, 1, 3, 0};
  const dsp::Samples samples = dsp::modulate(original, config);
  const dsp::Magnitudes magnitudes = dsp::demodulate_soft(samples, config);
  REQUIRE(magnitudes.rows() == original.size());
  REQUIRE(magnitudes.cols() == 4);
  for (std::size_t row = 0; row < magnitudes.rows(); ++row) {
    std::size_t best = 0;
    for (std::size_t tone = 1; tone < magnitudes.cols(); ++tone) {
      if (magnitudes.at(row, tone) > magnitudes.at(row, best)) {
        best = tone;
      }
    }
    REQUIRE(static_cast<int>(best) == original[row]);
  }
}

TEST_CASE("soft bit signs follow the transmitted bits", "[waveform]") {
  // Positive LLR means bit 0, negative means bit 1.
  const dsp::WaveformConfig config = config_48k();
  // Four symbols cover all four bit patterns: 00, 01, 11, 10.
  const std::vector<int> symbols = {0, 1, 2, 3};
  const dsp::Samples samples = dsp::modulate(symbols, config);
  const dsp::Magnitudes magnitudes = dsp::demodulate_soft(samples, config);
  const std::vector<double> soft = dsp::soft_bits_from_magnitudes(magnitudes, 4);

  const std::vector<int> expected = {0, 0, 0, 1, 1, 1, 1, 0};  // Gray order
  REQUIRE(soft.size() == expected.size());
  for (std::size_t i = 0; i < soft.size(); ++i) {
    REQUIRE((soft[i] < 0.0 ? 1 : 0) == expected[i]);
  }
}

TEST_CASE("OOK keeps the carrier phase running through off symbols", "[waveform]") {
  WaveformOptions options;
  options.baud = 300.0;
  options.num_tones = 1;
  const dsp::WaveformConfig config = options.build();

  const dsp::Samples samples = dsp::modulate({1, 0, 1, 1, 0, 1}, config);
  const auto samples_per_symbol = static_cast<std::size_t>(config.samples_per_symbol());
  REQUIRE(samples.size() == 6 * samples_per_symbol);
  // Silence symbols really are silent.
  for (std::size_t n = 0; n < samples_per_symbol; ++n) {
    REQUIRE(samples[samples_per_symbol + n] == 0.0f);
  }

  const dsp::Magnitudes magnitudes = dsp::demodulate_soft(samples, config);
  REQUIRE(magnitudes.cols() == 1);
  // Tone symbols carry far more energy than silence ones.
  REQUIRE(magnitudes.at(0, 0) > 100.0 * magnitudes.at(1, 0) + 1.0);
}
