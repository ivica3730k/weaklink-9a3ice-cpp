#include "weaklink/streaming.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>

#include "weaklink/constants.hpp"
#include "weaklink/log.hpp"
#include "weaklink/rng.hpp"

namespace weaklink::streaming {
namespace {

Logger& log() {
  static Logger logger("weaklink.streaming");
  return logger;
}

double to_db(double value) {
  return value > 0.0 ? 20.0 * std::log10(value) : -std::numeric_limits<double>::infinity();
}

}  // namespace

Samples pilot_signal(const ModemConfig& config, double duration_seconds) {
  const auto symbols_needed = static_cast<std::size_t>(
      std::max<long>(1, std::lround(duration_seconds * config.waveform().baud())));
  // Fixed seed: TX and RX both need the pilot to be the same every time, and
  // the generator is the NumPy-compatible one so a Python receiver sees the
  // identical symbol sequence.
  rng::NumpyGenerator generator(0xC0DE);
  std::vector<int> symbols(symbols_needed);
  for (std::size_t i = 0; i < symbols_needed; ++i) {
    symbols[i] = static_cast<int>(
        generator.bounded(static_cast<uint64_t>(config.waveform().num_tones())));
  }
  return dsp::modulate(symbols, config.waveform());
}

StreamingRxDecoder::StreamingRxDecoder(const ModemConfig& config, ByteSink sink)
    : config_(config),
      sink_(std::move(sink)),
      sample_rate_(static_cast<int>(std::lround(config.waveform().sample_rate()))) {
  const std::size_t max_group_symbols =
      static_cast<std::size_t>(config_.sync_every_blocks()) * config_.block_symbol_length() *
          static_cast<std::size_t>(config_.block_repeats()) +
      codec::kPreambleLengthSymbols;
  const double max_group_seconds =
      static_cast<double>(max_group_symbols) / config_.waveform().baud();
  max_window_samples_ = static_cast<std::size_t>(std::max(60.0, 3.0 * max_group_seconds) *
                                                 static_cast<double>(sample_rate_));
}

void StreamingRxDecoder::buffer(const float* samples, std::size_t count) {
  chunks_.emplace_back(samples, samples + count);
}

void StreamingRxDecoder::push(const float* samples, std::size_t count) {
  buffer(samples, count);
  try_emit();
}

std::size_t StreamingRxDecoder::total_buffered() const {
  std::size_t total = samples_before_buffer_;
  for (const Samples& chunk : chunks_) {
    total += chunk.size();
  }
  return total;
}

void StreamingRxDecoder::write_out(const ByteVector& bytes) {
  if (bytes.empty()) {
    return;
  }
  sink_(bytes);
}

void StreamingRxDecoder::on_session_end() {
  if (state_.session_ended) {
    state_.session_ended = false;
    state_.emitted.clear();
  }
}

bool StreamingRxDecoder::try_emit() {
  if (chunks_.empty()) {
    return false;
  }
  // Wait until there is enough audio to bracket at least one slot between two
  // preambles; decoding less than that can only waste work.
  const std::size_t min_group_symbols =
      2 * codec::kPreambleLengthSymbols + config_.block_symbol_length();
  const auto min_wait_samples = static_cast<std::size_t>(
      static_cast<double>(min_group_symbols) / config_.waveform().baud() *
      static_cast<double>(sample_rate_));
  const std::size_t seen = total_buffered();
  if (seen - cursor_ < min_wait_samples) {
    return false;
  }

  Samples buffer;
  buffer.reserve(seen - samples_before_buffer_);
  for (const Samples& chunk : chunks_) {
    buffer.insert(buffer.end(), chunk.begin(), chunk.end());
  }

  const std::size_t buffer_start = samples_before_buffer_;
  const std::size_t cursor_in_buffer = cursor_ > buffer_start ? cursor_ - buffer_start : 0;
  if (cursor_in_buffer >= buffer.size()) {
    return false;
  }
  const std::size_t window_size = buffer.size() - cursor_in_buffer;
  if (window_size < static_cast<std::size_t>(sample_rate_)) {
    return false;
  }

  const codec::DecodeResult result =
      codec::decode(buffer.data() + cursor_in_buffer, window_size, config_, true, &state_);
  const bool progress = result.safe_cursor_samples > 0 || !result.bytes.empty();
  write_out(result.bytes);
  cursor_ = buffer_start + cursor_in_buffer + result.safe_cursor_samples;

  // Retire chunks the cursor has moved past.
  while (!chunks_.empty()) {
    const std::size_t first_chunk_end = samples_before_buffer_ + chunks_.front().size();
    if (first_chunk_end <= cursor_) {
      samples_before_buffer_ += chunks_.front().size();
      chunks_.pop_front();
    } else {
      break;
    }
  }

  // Cap the retained window so a long listening session cannot grow without
  // bound when nothing ever decodes.
  while (!chunks_.empty()) {
    const std::size_t retained = total_buffered() - samples_before_buffer_;
    if (retained <= max_window_samples_) {
      break;
    }
    const std::size_t overflow = retained - max_window_samples_;
    Samples& front = chunks_.front();
    if (overflow >= front.size()) {
      samples_before_buffer_ += front.size();
      chunks_.pop_front();
    } else {
      front.erase(front.begin(), front.begin() + static_cast<std::ptrdiff_t>(overflow));
      samples_before_buffer_ += overflow;
    }
    if (cursor_ < samples_before_buffer_) {
      cursor_ = samples_before_buffer_;
    }
  }

  return progress;
}

void StreamingRxDecoder::drain() {
  while (try_emit()) {
    // Keep going while the buffer is still advancing.
  }
  if (chunks_.empty()) {
    return;
  }
  Samples buffer;
  for (const Samples& chunk : chunks_) {
    buffer.insert(buffer.end(), chunk.begin(), chunk.end());
  }
  const std::size_t cursor_in_buffer =
      cursor_ > samples_before_buffer_ ? cursor_ - samples_before_buffer_ : 0;
  if (cursor_in_buffer >= buffer.size()) {
    return;
  }
  // Batch decode over the tail: no more audio is coming, so end-of-buffer
  // slots that streaming mode would hold back must be flushed.
  const codec::DecodeResult result =
      codec::decode(buffer.data() + cursor_in_buffer, buffer.size() - cursor_in_buffer, config_,
                    false, &state_);
  write_out(result.bytes);
  samples_before_buffer_ += buffer.size();
  chunks_.clear();
}

void StreamingRxDecoder::log_audio_level() const {
  if (chunks_.empty() || !Logger::enabled(LogLevel::kInfo)) {
    return;
  }
  const auto wanted = static_cast<std::size_t>(sample_rate_);  // one second
  Samples recent;
  for (auto chunk = chunks_.rbegin(); chunk != chunks_.rend(); ++chunk) {
    recent.insert(recent.begin(), chunk->begin(), chunk->end());
    if (recent.size() >= wanted) {
      break;
    }
  }
  if (recent.size() > wanted) {
    recent.erase(recent.begin(), recent.end() - static_cast<std::ptrdiff_t>(wanted));
  }
  double peak = 0.0;
  double sum_squares = 0.0;
  for (const float sample : recent) {
    const double value = static_cast<double>(sample);
    peak = std::max(peak, std::abs(value));
    sum_squares += value * value;
  }
  const double rms =
      recent.empty() ? 0.0 : std::sqrt(sum_squares / static_cast<double>(recent.size()));
  log().info("audio: peak ", to_db(peak), " dBFS, rms ", to_db(rms), " dBFS");
}

void live_stream_decode(const ModemConfig& config, const std::string& audio_input,
                        const ByteSink& sink, const std::function<bool()>& should_stop) {
  const audio::AudioTarget target =
      audio::resolve_audio_target(audio_input, audio::DeviceKind::kInput);
  if (!audio_input.empty()) {
    log().debug("audio input hint ", audio_input, " -> ", target.describe());
  }

  StreamingRxDecoder decoder(config, sink);
  log().debug("live rx buffer cap: ",
              static_cast<double>(decoder.max_window_samples()) / decoder.sample_rate(), " s");

  // Buffer only: try_emit runs heavy DSP and must not run on the audio thread.
  audio::LiveInputStream stream(
      decoder.sample_rate(),
      [&decoder](const float* samples, std::size_t count) { decoder.buffer(samples, count); },
      target);

  log().debug("live rx: polling every ", kLiveRxPollMs, " ms, source ", target.describe());
  stream.start();

  int poll_counter = 0;
  while (!should_stop()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kLiveRxPollMs));
    ++poll_counter;
    decoder.try_emit();
    decoder.on_session_end();
    if (poll_counter % kLiveRxSnapshotEveryPolls == 0) {
      decoder.log_audio_level();
    }
  }
  stream.stop();
  log().debug("live rx: stopping, draining tail");
  decoder.drain();
}

}  // namespace weaklink::streaming
