#pragma once

#include <cmath>
#include <string>
#include <vector>

#include "weaklink/api.hpp"
#include "weaklink/codec.hpp"
#include "weaklink/constants.hpp"
#include "weaklink/rng.hpp"
#include "weaklink/streaming.hpp"

namespace weaklink::test {

using codec::ModemConfig;
using dsp::Samples;

/// Build a config the way the ported Python tests do: waveform baud and tone
/// spacing together, everything else explicit.
inline ModemConfig make_config(double baud, double tone_spacing_hz, int num_tones,
                               int rs_data_bytes, int rs_parity_bytes, int block_repeats,
                               int sync_every_blocks = 4) {
  dsp::WaveformOptions waveform;
  waveform.baud = baud;
  waveform.tone_spacing_hz = tone_spacing_hz;
  waveform.num_tones = num_tones;

  ModemConfig::Options options;
  options.waveform = waveform.build();
  options.rs_data_bytes = rs_data_bytes;
  options.rs_parity_bytes = rs_parity_bytes;
  options.block_repeats = block_repeats;
  options.sync_every_blocks = sync_every_blocks;
  return ModemConfig(options);
}

/// The default config: 300 baud, 4 tones, RS(16,8), one copy per block.
inline ModemConfig default_config() { return ModemConfig(); }

/// Config for a baud straight from the preset table, like the tests that
/// assert preset behaviour.
inline ModemConfig preset_config(double baud) {
  const BaudPreset& preset = baud_presets().at(baud);
  return make_config(baud, preset.tone_spacing_hz, 4, preset.rs_data_bytes,
                     preset.rs_parity_bytes, preset.block_repeats, preset.sync_every_blocks);
}

inline ByteVector to_bytes(const std::string& text) {
  return ByteVector(text.begin(), text.end());
}

inline std::string to_string(const ByteVector& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

inline ByteVector strip_trailing_nul(ByteVector data) {
  while (!data.empty() && data.back() == 0) {
    data.pop_back();
  }
  return data;
}

inline bool contains(const ByteVector& haystack, const ByteVector& needle) {
  if (needle.empty()) {
    return true;
  }
  return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) !=
         haystack.end();
}

/// Additive white Gaussian noise, scaled so noise power in the 3 kHz reference
/// band sits ``snr_db`` below signal power. Sample-rate invariant, matching the
/// benchmark's convention.
///
/// The rounding order mirrors NumPy's: the standard normal is narrowed to
/// float32 before being scaled, so the C++ and Python suites see bit-identical
/// noise for a given seed.
inline Samples add_awgn(const Samples& samples, double snr_db, uint64_t seed,
                        double sample_rate = dsp::WaveformConfig::kDefaultSampleRate) {
  double sum_squares = 0.0;
  for (const float sample : samples) {
    const double value = static_cast<double>(sample);
    sum_squares += value * value;
  }
  const double signal_power = sum_squares / static_cast<double>(samples.size());
  const double noise_variance = signal_power * sample_rate / (2.0 * kReferenceBandwidthHz) /
                                std::pow(10.0, snr_db / 10.0);
  const auto sigma = static_cast<float>(std::sqrt(noise_variance));

  rng::NumpyGenerator generator(seed);
  Samples out(samples.size());
  for (std::size_t i = 0; i < samples.size(); ++i) {
    const auto noise = static_cast<float>(generator.standard_normal());
    out[i] = samples[i] + noise * sigma;
  }
  return out;
}

/// Variant used by the end-to-end sweep: the normal is drawn and scaled in
/// double precision, then narrowed once.
inline Samples add_awgn_scaled_in_double(const Samples& samples, double snr_db, uint64_t seed,
                                         double bandwidth_hz = kReferenceBandwidthHz,
                                         double sample_rate =
                                             dsp::WaveformConfig::kDefaultSampleRate) {
  double sum_squares = 0.0;
  for (const float sample : samples) {
    const double value = static_cast<double>(sample);
    sum_squares += value * value;
  }
  const double signal_power = sum_squares / static_cast<double>(samples.size());
  const double sigma = std::sqrt(signal_power * sample_rate / (2.0 * bandwidth_hz) /
                                 std::pow(10.0, snr_db / 10.0));

  rng::NumpyGenerator generator(seed);
  Samples out(samples.size());
  for (std::size_t i = 0; i < samples.size(); ++i) {
    out[i] = samples[i] + static_cast<float>(sigma * generator.standard_normal());
  }
  return out;
}

/// Push ``audio`` through the streaming decoder in fixed-size chunks and drain
/// at the end. Every batch-mode decode test has a companion that goes through
/// here instead, so bugs that only appear in the live path are still caught.
inline ByteVector stream_decode(const Samples& audio, const ModemConfig& config,
                                double chunk_seconds = 0.1) {
  ByteVector out;
  streaming::StreamingRxDecoder decoder(
      config, [&](const ByteVector& bytes) { out.insert(out.end(), bytes.begin(), bytes.end()); });
  const auto chunk_samples = static_cast<std::size_t>(
      std::max(1.0, chunk_seconds * config.waveform().sample_rate()));
  for (std::size_t start = 0; start < audio.size(); start += chunk_samples) {
    const std::size_t count = std::min(chunk_samples, audio.size() - start);
    decoder.push(audio.data() + start, count);
  }
  decoder.drain();
  return out;
}

/// Reproduce exactly what the CLI writes to the audio device: pilot, encoded
/// signal, pilot -- including the minimum-air-time floor that only applies when
/// the whole payload is known up front.
struct LiveTxBuffer {
  Samples audio;
  ModemConfig config;
};

inline LiveTxBuffer live_tx_buffer(double baud, const ByteVector& payload) {
  const ModemConfig config = preset_config(baud);
  const Samples samples = codec::encode(payload, config);
  const double signal_seconds =
      static_cast<double>(samples.size()) / config.waveform().sample_rate();
  const double pilot_each_side =
      std::max({kLiveTxPilotMinSeconds, (kLiveTxMinSeconds - signal_seconds) / 2.0,
                static_cast<double>(kLiveTxPilotMinSymbols) / config.waveform().baud()});
  const Samples pilot = streaming::pilot_signal(config, pilot_each_side);

  Samples audio;
  audio.reserve(2 * pilot.size() + samples.size());
  audio.insert(audio.end(), pilot.begin(), pilot.end());
  audio.insert(audio.end(), samples.begin(), samples.end());
  audio.insert(audio.end(), pilot.begin(), pilot.end());
  return LiveTxBuffer{std::move(audio), config};
}

}  // namespace weaklink::test
