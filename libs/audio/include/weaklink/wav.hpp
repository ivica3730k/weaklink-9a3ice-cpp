#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace weaklink::audio {

using Samples = std::vector<float>;

/// Streaming WAV writer. Mono 32-bit float, which is what the modem produces
/// and what the Python implementation writes (``subtype="FLOAT"``).
///
/// The RIFF sizes are patched on close, so a writer that is destroyed without
/// closing still leaves a readable file as long as the process exits cleanly.
class WavWriter {
 public:
  WavWriter(const std::string& path, int sample_rate);
  ~WavWriter();

  WavWriter(const WavWriter&) = delete;
  WavWriter& operator=(const WavWriter&) = delete;

  void write(const float* samples, std::size_t count);
  void write(const Samples& samples) { write(samples.data(), samples.size()); }
  void close();

 private:
  std::ofstream stream_;
  std::string path_;
  std::uint32_t frames_written_ = 0;
  bool closed_ = false;
};

/// Chunked WAV reader. Multi-channel files are downmixed to mono by averaging,
/// matching the Python reader.
class WavReader {
 public:
  explicit WavReader(const std::string& path);
  ~WavReader();

  WavReader(const WavReader&) = delete;
  WavReader& operator=(const WavReader&) = delete;

  int sample_rate() const { return sample_rate_; }
  int channels() const { return channels_; }
  std::uint64_t frames() const { return frames_; }

  /// Reads up to ``frame_count`` mono frames. Returns an empty vector at EOF.
  Samples read(std::size_t frame_count);

 private:
  std::ifstream stream_;
  int sample_rate_ = 0;
  int channels_ = 0;
  int bits_per_sample_ = 0;
  bool is_float_ = false;
  std::uint64_t frames_ = 0;
  std::uint64_t frames_read_ = 0;
  std::streampos data_offset_ = 0;
};

/// Write float32 mono samples in one shot.
void write_wav(const std::string& path, const Samples& samples, double sample_rate);

/// Read a whole WAV, downmixing to mono.
///
/// Throws ``ConfigError`` when ``expected_sample_rate`` is given and the file
/// disagrees -- a rate mismatch silently destroys the symbol timing, so it is
/// worth failing loudly.
struct WavData {
  Samples samples;
  int sample_rate = 0;
};
WavData read_wav(const std::string& path, double expected_sample_rate = 0.0);

}  // namespace weaklink::audio
