#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "weaklink/audio.hpp"
#include "weaklink/codec.hpp"

namespace weaklink::streaming {

using codec::ModemConfig;
using dsp::Samples;

/// Receives decoded bytes as they land.
using ByteSink = std::function<void(const ByteVector&)>;

/// Random MFSK symbols for ``duration_seconds``. Every tone is exercised
/// uniformly so the coarse-offset FFT has energy at each slot to lock onto.
Samples pilot_signal(const ModemConfig& config, double duration_seconds);

/// Chunk-in, decoded-bytes-out streaming RX.
///
/// The same class backs live audio and WAV playback, so a bug that only shows
/// up in the live path is reachable from a file-driven test.
class StreamingRxDecoder {
 public:
  StreamingRxDecoder(const ModemConfig& config, ByteSink sink);

  /// Buffer a chunk and attempt a decode pass.
  void push(const float* samples, std::size_t count);
  void push(const Samples& chunk) { push(chunk.data(), chunk.size()); }

  /// Buffer without decoding. The live callback uses this: ``try_emit`` runs
  /// heavy DSP and must not block the audio thread.
  void buffer(const float* samples, std::size_t count);

  /// One decode pass over the buffered audio. Returns true when the buffer
  /// actually advanced, so callers can loop until it stalls.
  bool try_emit();

  /// The codec flags a lost lock after having had one. Reset the block-index
  /// dedup so the next transmission starts fresh.
  void on_session_end();

  /// Flush at end-of-stream: streaming-decode until progress stalls, then do a
  /// batch decode over the tail so end-of-buffer slots still emit.
  void drain();

  /// One-second peak and RMS snapshot, logged to ``weaklink.streaming``.
  void log_audio_level() const;

  int sample_rate() const { return sample_rate_; }
  std::size_t max_window_samples() const { return max_window_samples_; }

 private:
  std::size_t total_buffered() const;
  void write_out(const ByteVector& bytes);

  ModemConfig config_;
  ByteSink sink_;
  int sample_rate_;
  std::deque<Samples> chunks_;
  std::size_t samples_before_buffer_ = 0;
  std::size_t cursor_ = 0;
  std::size_t max_window_samples_ = 0;
  codec::StreamingState state_;
};

/// Live streaming decode loop. Blocks until ``should_stop`` returns true
/// (the CLI wires that to SIGINT), then drains the tail.
void live_stream_decode(const ModemConfig& config, const std::string& audio_input,
                        const ByteSink& sink, const std::function<bool()>& should_stop);

}  // namespace weaklink::streaming
