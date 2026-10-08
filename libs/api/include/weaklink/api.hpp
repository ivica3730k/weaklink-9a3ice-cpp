#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "weaklink/codec.hpp"
#include "weaklink/streaming.hpp"

namespace weaklink::api {

using codec::ModemConfig;
using dsp::Samples;

/// Modem-layer parameters shared by ``tx`` and ``rx``.
///
/// Fields left unset fall back to the ``BAUD_PRESETS`` entry for the selected
/// baud. These are the same knobs the CLI exposes as ``--modem-*``.
struct ModemOptions {
  double baud = 300.0;
  int num_tones = 4;
  std::optional<int> rs_data_bytes;
  std::optional<int> rs_parity_bytes;
  bool rs_crc_enabled = true;
  std::optional<int> block_repeats;
  std::optional<int> sync_every_blocks;
  std::optional<double> tone_spacing_hz;
};

/// Resolve ``options`` (with preset fallbacks) into a full configuration.
/// ``tx_volume`` (0-100) maps to waveform amplitude; RX ignores it.
ModemConfig build_config(const ModemOptions& options = ModemOptions{}, int tx_volume = 100);

/// Where a transmission goes. Exactly one of these should be set.
struct TxTarget {
  std::string wav_path;       ///< Write to this WAV file.
  std::string audio_output;   ///< Stream live to this device hint.
  bool to_audio_device = false;  ///< Use the OS default output device.
  std::string ptt;            ///< rigctld ``host:port``; live audio only.
  /// Emit every tone of the mode in round-robin until stopped. No framing, no
  /// preamble, no input -- just clean tones for audio-path verification.
  bool tune = false;
};

/// Encode and dispatch to the requested sink.
///
/// With no sink set, the encoded samples are returned instead (and no pilot
/// padding is added -- that is a live-transmission concern).
Samples tx(const codec::ByteSource& source, const ModemOptions& options, int tx_volume,
           const TxTarget& target, const std::function<bool()>& should_stop = nullptr);

/// Convenience wrapper for a payload already held in memory.
Samples tx(const ByteVector& data, const ModemOptions& options = ModemOptions{},
           int tx_volume = 100, const TxTarget& target = TxTarget{});

/// Where a reception comes from. Exactly one should be set.
struct RxSource {
  const Samples* samples = nullptr;  ///< Batch-decode this buffer.
  std::string wav_path;              ///< Read and decode this WAV.
  std::string audio_input;           ///< Stream live from this device hint.
  bool from_audio_device = false;    ///< Use the OS default input device.
};

/// Decode from the requested source. Decoded bytes are handed to ``sink`` as
/// they land and also returned (empty for the live path, which only ends when
/// ``should_stop`` fires).
ByteVector rx(const RxSource& source, const ModemOptions& options,
              const streaming::ByteSink& sink = nullptr,
              const std::function<bool()>& should_stop = nullptr);

}  // namespace weaklink::api
