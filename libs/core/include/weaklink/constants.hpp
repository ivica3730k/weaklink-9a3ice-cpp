#pragma once

#include <cstddef>
#include <map>
#include <string>

namespace weaklink {

/// Per-baud preset. ``tone_spacing_hz`` is widened at low bauds so the tones
/// spread across enough Hz to survive room modes and mic roll-off.
struct BaudPreset {
  double tone_spacing_hz;
  int rs_data_bytes;
  int rs_parity_bytes;
  int block_repeats;
  int sync_every_blocks;
};

/// The three supported symbol rates. Adding or renaming an entry is a
/// breaking change for both tx and rx.
const std::map<double, BaudPreset>& baud_presets();

/// Pilot padding on each side of live-tx: brings the audio sink out of its
/// IDLE state (~50 ms of wake-up latency) and gives the coarse-offset FFT
/// real MFSK tone energy to lock onto.
inline constexpr double kLiveTxPilotMinSeconds = 0.2;

/// Pilot must also exceed the preamble in symbol space so back-to-back tx
/// buffers keep > 2 * preamble_length between adjacent preambles. Matters at
/// low baud where 0.2 s is only ~9 symbols.
inline constexpr int kLiveTxPilotMinSymbols = 40;

/// Floor on total live-tx duration. 1200-baud single-char is ~250 ms of
/// signal -- too short to give RX two clean poll windows. Pad to 1 s.
inline constexpr double kLiveTxMinSeconds = 1.0;

/// rigctld TCP default; matches the ``--hamlib-ptt`` bare-flag default.
inline constexpr int kHamlibDefaultPort = 4532;

/// PTT-to-audio guard. Radios need a small delay between key-up and first
/// sample or the leading pilot gets clipped by relay / AGC settling.
inline constexpr double kHamlibPttLeadSeconds = 0.1;

/// Symmetric tail: hold PTT past the last sample so the trailing pilot makes
/// it out before the relay drops.
inline constexpr double kHamlibPttTailSeconds = 0.1;

/// Live-rx poll cadence.
inline constexpr int kLiveRxPollMs = 100;

/// Frequency of the audio peak / rms snapshot log line during live rx.
inline constexpr int kLiveRxSnapshotEveryPolls = 10;

/// SNR normalisation convention shared by the benchmark and the tests.
inline constexpr double kReferenceBandwidthHz = 3000.0;

/// Where CLI diagnostics land by default (kept out of stdout).
inline const std::string kDefaultLogPath = "log.txt";

}  // namespace weaklink
