#include "weaklink/api.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "weaklink/constants.hpp"
#include "weaklink/exceptions.hpp"
#include "weaklink/ptt.hpp"
#include "weaklink/wav.hpp"

namespace weaklink::api {
namespace {

/// Bauds read as "300" or "45.5", never "300.000000" -- std::to_string always
/// emits six decimals.
std::string format_baud(double baud) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%g", baud);
  return buffer;
}

/// Leading pilot, encoded blocks, trailing pilot -- what the CLI puts on the
/// wire. The leading pilot is sized without knowing the signal length (the
/// source is a stream), so only the two fixed floors apply.
void stream_with_pilots(const codec::ByteSource& source, const ModemConfig& config,
                        const codec::SampleSink& sink) {
  const double pilot_seconds =
      std::max(kLiveTxPilotMinSeconds,
               static_cast<double>(kLiveTxPilotMinSymbols) / config.waveform().baud());
  const Samples pilot = streaming::pilot_signal(config, pilot_seconds);
  sink(pilot);
  codec::encode_stream(source, config, sink);
  sink(pilot);
}

}  // namespace

ModemConfig build_config(const ModemOptions& options, int tx_volume) {
  const auto& presets = baud_presets();
  const auto preset = presets.find(options.baud);
  if (preset == presets.end()) {
    std::string supported;
    for (const auto& entry : presets) {
      if (!supported.empty()) {
        supported += ", ";
      }
      supported += format_baud(entry.first);
    }
    throw ConfigError("baud " + format_baud(options.baud) +
                      " is not supported; use one of " + supported);
  }
  if (tx_volume < 0 || tx_volume > 100) {
    throw ConfigError("tx_volume must be 0-100 (got " + std::to_string(tx_volume) + ")");
  }

  dsp::WaveformOptions waveform;
  waveform.baud = options.baud;
  waveform.tone_spacing_hz = options.tone_spacing_hz.value_or(preset->second.tone_spacing_hz);
  waveform.num_tones = options.num_tones;
  waveform.amplitude = static_cast<double>(tx_volume) / 100.0;

  ModemConfig::Options modem;
  modem.waveform = waveform.build();
  modem.rs_data_bytes = options.rs_data_bytes.value_or(preset->second.rs_data_bytes);
  modem.rs_parity_bytes = options.rs_parity_bytes.value_or(preset->second.rs_parity_bytes);
  modem.rs_crc_enabled = options.rs_crc_enabled;
  modem.sync_every_blocks = options.sync_every_blocks.value_or(preset->second.sync_every_blocks);
  modem.block_repeats = options.block_repeats.value_or(preset->second.block_repeats);
  return ModemConfig(modem);
}

Samples tx(const codec::ByteSource& source, const ModemOptions& options, int tx_volume,
           const TxTarget& target, const std::function<bool()>& should_stop) {
  const bool to_wav = !target.wav_path.empty();
  const bool to_device = target.to_audio_device || !target.audio_output.empty();
  if (to_wav && to_device) {
    throw ConfigError("pass either a WAV path or an audio output device, not both");
  }
  if (to_wav && !target.ptt.empty()) {
    throw ConfigError("PTT is only valid with live audio TX; drop the WAV path or the PTT");
  }
  if (target.tune && to_wav) {
    throw ConfigError("tune is a live-audio-only operation; drop the WAV path");
  }

  const ModemConfig config = build_config(options, tx_volume);

  if (target.tune) {
    std::vector<int> cycle(static_cast<std::size_t>(config.waveform().num_tones()));
    for (std::size_t i = 0; i < cycle.size(); ++i) {
      cycle[i] = static_cast<int>(i);
    }
    const Samples burst = dsp::modulate(cycle, config.waveform());
    ptt::HamlibPtt keyed(target.ptt);
    audio::play_stream(
        [&](Samples& chunk) {
          if (should_stop && should_stop()) {
            return false;
          }
          chunk = burst;
          return true;
        },
        config.waveform().sample_rate(), target.audio_output);
    return {};
  }

  if (to_wav) {
    audio::WavWriter writer(target.wav_path,
                            static_cast<int>(std::lround(config.waveform().sample_rate())));
    stream_with_pilots(source, config, [&](const Samples& part) { writer.write(part); });
    writer.close();
    return {};
  }

  if (to_device) {
    // Audio is produced lazily and handed to the sink as it is generated, so a
    // long transmission never has to be buffered whole. The encoder runs on
    // this thread between device writes.
    std::vector<Samples> queue;
    stream_with_pilots(source, config, [&](const Samples& part) { queue.push_back(part); });
    std::size_t next = 0;
    ptt::HamlibPtt keyed(target.ptt);
    audio::play_stream(
        [&](Samples& chunk) {
          if (next >= queue.size()) {
            return false;
          }
          chunk = std::move(queue[next++]);
          return true;
        },
        config.waveform().sample_rate(), target.audio_output);
    return {};
  }

  // No sink chosen: hand back the encoded samples, without pilot padding.
  Samples out;
  codec::encode_stream(source, config,
                       [&](const Samples& part) { out.insert(out.end(), part.begin(), part.end()); });
  return out;
}

Samples tx(const ByteVector& data, const ModemOptions& options, int tx_volume,
           const TxTarget& target) {
  bool sent = false;
  return tx(
      [&](ByteVector& chunk) {
        if (sent) {
          return false;
        }
        chunk = data;
        sent = true;
        return true;
      },
      options, tx_volume, target);
}

ByteVector rx(const RxSource& source, const ModemOptions& options,
              const streaming::ByteSink& sink, const std::function<bool()>& should_stop) {
  const int chosen = (source.samples != nullptr ? 1 : 0) + (source.wav_path.empty() ? 0 : 1) +
                     ((source.from_audio_device || !source.audio_input.empty()) ? 1 : 0);
  if (chosen == 0) {
    throw ConfigError("rx requires samples, a WAV path, or an audio input device");
  }
  if (chosen > 1) {
    throw ConfigError("pass at most one of samples, a WAV path, or an audio input device");
  }

  const ModemConfig config = build_config(options);
  ByteVector collected;
  const auto emit = [&](const ByteVector& bytes) {
    collected.insert(collected.end(), bytes.begin(), bytes.end());
    if (sink) {
      sink(bytes);
    }
  };

  if (source.samples != nullptr) {
    const codec::DecodeResult result = codec::decode(*source.samples, config);
    emit(result.bytes);
    return collected;
  }

  if (!source.wav_path.empty()) {
    audio::WavReader reader(source.wav_path);
    if (static_cast<int>(std::lround(config.waveform().sample_rate())) != reader.sample_rate()) {
      throw ConfigError("WAV sample rate " + std::to_string(reader.sample_rate()) +
                        " Hz does not match expected " +
                        std::to_string(static_cast<int>(
                            std::lround(config.waveform().sample_rate()))) +
                        " Hz");
    }
    streaming::StreamingRxDecoder decoder(config, emit);
    // Chunked at the live-rx poll cadence so WAV and live audio drive exactly
    // the same code path.
    const auto chunk_frames = static_cast<std::size_t>(
        std::max(1L, std::lround(0.1 * reader.sample_rate())));
    while (true) {
      const Samples chunk = reader.read(chunk_frames);
      if (chunk.empty()) {
        break;
      }
      decoder.push(chunk);
    }
    decoder.drain();
    return collected;
  }

  streaming::live_stream_decode(config, source.audio_input, emit,
                                should_stop ? should_stop : [] { return false; });
  return collected;
}

}  // namespace weaklink::api
