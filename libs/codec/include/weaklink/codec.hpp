#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include "weaklink/bits.hpp"
#include "weaklink/interleaver.hpp"
#include "weaklink/reedsolomon.hpp"
#include "weaklink/waveform.hpp"

namespace weaklink::codec {

using dsp::Samples;

/// Symbols in the per-slot synchronisation preamble.
inline constexpr std::size_t kPreambleLengthSymbols = 32;

/// Per-slot data-area header: ``[length 1B][block_index 2B]``. Length strips
/// trailing NUL padding; block_index dedupes copies and picks the RX output
/// slot.
inline constexpr std::size_t kHeaderBytes = 3;

/// 2-byte block index -- 65535 slots per session.
inline constexpr int kMaxBlockIndex = 0xFFFF;

/// Deterministic PN preamble over the mode's symbol alphabet. The same LFSR
/// runs at every mode, consuming ``bits_per_symbol`` bits per symbol.
const std::vector<int>& preamble_for(int num_tones);

/// Full modem configuration. Wire format: ``[pre][slot][pre][slot]...[pre]``,
/// where each slot carries one RS block routed through RS+CRC -> convolutional
/// K=7 r=1/2 -> per-block interleave -> MFSK.
class ModemConfig {
 public:
  struct Options {
    dsp::WaveformConfig waveform = dsp::WaveformConfig();
    interleaver::InterleaverConfig interleaver = interleaver::InterleaverConfig{8, 32};
    int rs_data_bytes = 16;
    int rs_parity_bytes = 8;
    bool rs_crc_enabled = true;
    /// Preamble inserted at the start and every N data blocks thereafter.
    int sync_every_blocks = 4;
    /// Each RS block sent this many times, round-robin. RX combines soft LLRs
    /// across copies before Viterbi+RS -- roughly 3 dB per doubling in AWGN.
    int block_repeats = 1;
    /// Half-range for the pre-sync FFT LO-offset search. Covers typical dial
    /// drift at a cost of ~50 ms per decode.
    double coarse_frequency_search_hz = 500.0;
    double frequency_search_hz = 20.0;
    double frequency_resolution_hz = 1.0;
  };

  ModemConfig() : ModemConfig(Options{}) {}
  explicit ModemConfig(const Options& options);

  const dsp::WaveformConfig& waveform() const { return options_.waveform; }
  const interleaver::InterleaverConfig& interleaver_config() const {
    return options_.interleaver;
  }
  int rs_data_bytes() const { return options_.rs_data_bytes; }
  int rs_parity_bytes() const { return options_.rs_parity_bytes; }
  bool rs_crc_enabled() const { return options_.rs_crc_enabled; }
  int sync_every_blocks() const { return options_.sync_every_blocks; }
  int block_repeats() const { return options_.block_repeats; }
  double coarse_frequency_search_hz() const { return options_.coarse_frequency_search_hz; }
  double frequency_search_hz() const { return options_.frequency_search_hz; }
  double frequency_resolution_hz() const { return options_.frequency_resolution_hz; }

  const rs::BlockCodec& rs_codec() const { return *codec_; }

  /// Symbols occupied by one slot's data area (preamble excluded).
  std::size_t block_symbol_length() const;

 private:
  Options options_;
  std::shared_ptr<rs::BlockCodec> codec_;
};

/// Mutable state a streaming caller threads across decode calls. Carries the
/// cross-call dedup sets and the cached coarse LO offset so a block whose
/// copies straddle two poll windows still finalises correctly.
struct StreamingState {
  std::optional<double> coarse_offset_hz;
  bool session_ended = false;
  /// Block indices already written to the output in this session.
  std::set<int> emitted;
  /// Decoded but not yet confirmed by ``block_repeats`` copies.
  std::map<int, ByteVector> pending_blocks;
  std::map<int, int> copies_seen;
  int expected_block_index = 0;
};

struct DecodeResult {
  ByteVector bytes;
  /// Streaming only: keep audio from this sample offset onward for the next
  /// call. Zero in batch mode.
  std::size_t safe_cursor_samples = 0;
};

/// Pull bytes from the caller. Return false once the stream is exhausted.
using ByteSource = std::function<bool(ByteVector&)>;

/// Receives float32 audio one slot at a time.
using SampleSink = std::function<void(const Samples&)>;

/// Consume bytes, emit audio slot by slot, then a trailing preamble.
///
/// Copies of the same block are adjacent so no lookahead is needed; RX dedupes
/// by block index. Throws ``EncodeError`` past ``kMaxBlockIndex`` slots.
void encode_stream(const ByteSource& source, const ModemConfig& config,
                   const SampleSink& sink);

/// Batch wrapper around :func:`encode_stream`.
Samples encode(const ByteVector& input, const ModemConfig& config);

/// Decode audio to bytes; undecodable blocks are dropped rather than taking
/// the rest of the stream with them.
///
/// ``state`` may be null in batch mode. When non-null it is honoured even if
/// ``streaming`` is false, so a final drain decode still dedupes against what
/// earlier streaming calls emitted.
DecodeResult decode(const float* samples, std::size_t count, const ModemConfig& config,
                    bool streaming = false, StreamingState* state = nullptr);
DecodeResult decode(const Samples& samples, const ModemConfig& config,
                    bool streaming = false, StreamingState* state = nullptr);

/// Exposed for the correlator regression tests.
std::vector<std::size_t> find_preamble_peaks(const dsp::Magnitudes& magnitudes,
                                             const std::vector<int>& preamble,
                                             const ModemConfig& config);

}  // namespace weaklink::codec
