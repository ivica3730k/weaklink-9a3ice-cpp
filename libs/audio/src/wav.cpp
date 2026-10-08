#include "weaklink/wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "weaklink/exceptions.hpp"

namespace weaklink::audio {
namespace {

constexpr std::uint16_t kFormatPcm = 1;
constexpr std::uint16_t kFormatFloat = 3;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;

void write_u32(std::ostream& out, std::uint32_t value) {
  char bytes[4] = {static_cast<char>(value & 0xFF), static_cast<char>((value >> 8) & 0xFF),
                   static_cast<char>((value >> 16) & 0xFF),
                   static_cast<char>((value >> 24) & 0xFF)};
  out.write(bytes, 4);
}

void write_u16(std::ostream& out, std::uint16_t value) {
  char bytes[2] = {static_cast<char>(value & 0xFF), static_cast<char>((value >> 8) & 0xFF)};
  out.write(bytes, 2);
}

std::uint32_t read_u32(std::istream& in) {
  unsigned char bytes[4] = {0, 0, 0, 0};
  in.read(reinterpret_cast<char*>(bytes), 4);
  return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) |
         (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint16_t read_u16(std::istream& in) {
  unsigned char bytes[2] = {0, 0};
  in.read(reinterpret_cast<char*>(bytes), 2);
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[0]) |
                                    static_cast<std::uint16_t>(bytes[1] << 8));
}

/// Decode one sample of the file's storage format into the [-1, 1] range the
/// modem works in.
float decode_sample(const unsigned char* raw, int bits_per_sample, bool is_float) {
  if (is_float) {
    if (bits_per_sample == 32) {
      float value = 0.0f;
      std::memcpy(&value, raw, sizeof(value));
      return value;
    }
    double value = 0.0;
    std::memcpy(&value, raw, sizeof(value));
    return static_cast<float>(value);
  }
  switch (bits_per_sample) {
    case 8:
      // 8-bit PCM is unsigned with a 128 bias, unlike every wider width.
      return (static_cast<float>(raw[0]) - 128.0f) / 128.0f;
    case 16: {
      const auto value = static_cast<std::int16_t>(static_cast<std::uint16_t>(raw[0]) |
                                                   (static_cast<std::uint16_t>(raw[1]) << 8));
      return static_cast<float>(value) / 32768.0f;
    }
    case 24: {
      std::int32_t value = static_cast<std::int32_t>(
          (static_cast<std::uint32_t>(raw[0]) << 8) |
          (static_cast<std::uint32_t>(raw[1]) << 16) |
          (static_cast<std::uint32_t>(raw[2]) << 24));
      value >>= 8;  // sign-extend from 24 bits
      return static_cast<float>(value) / 8388608.0f;
    }
    case 32: {
      const auto value = static_cast<std::int32_t>(
          static_cast<std::uint32_t>(raw[0]) | (static_cast<std::uint32_t>(raw[1]) << 8) |
          (static_cast<std::uint32_t>(raw[2]) << 16) |
          (static_cast<std::uint32_t>(raw[3]) << 24));
      return static_cast<float>(value) / 2147483648.0f;
    }
    default:
      throw ConfigError("unsupported WAV bit depth: " + std::to_string(bits_per_sample));
  }
}

}  // namespace

// ---- WavWriter ------------------------------------------------------------

WavWriter::WavWriter(const std::string& path, int sample_rate)
    : stream_(path, std::ios::binary | std::ios::trunc), path_(path) {
  if (!stream_) {
    throw ConfigError("cannot open WAV for writing: " + path);
  }
  const std::uint16_t channels = 1;
  const std::uint16_t bits = 32;
  const std::uint32_t byte_rate =
      static_cast<std::uint32_t>(sample_rate) * channels * (bits / 8u);

  stream_.write("RIFF", 4);
  write_u32(stream_, 0);  // patched on close
  stream_.write("WAVE", 4);
  stream_.write("fmt ", 4);
  write_u32(stream_, 16);
  write_u16(stream_, kFormatFloat);
  write_u16(stream_, channels);
  write_u32(stream_, static_cast<std::uint32_t>(sample_rate));
  write_u32(stream_, byte_rate);
  write_u16(stream_, static_cast<std::uint16_t>(channels * (bits / 8u)));
  write_u16(stream_, bits);
  stream_.write("data", 4);
  write_u32(stream_, 0);  // patched on close
}

WavWriter::~WavWriter() {
  try {
    close();
  } catch (...) {
    // A destructor must not throw; the file is already as complete as it can be.
  }
}

void WavWriter::write(const float* samples, std::size_t count) {
  if (closed_ || count == 0) {
    return;
  }
  stream_.write(reinterpret_cast<const char*>(samples),
                static_cast<std::streamsize>(count * sizeof(float)));
  frames_written_ += static_cast<std::uint32_t>(count);
}

void WavWriter::close() {
  if (closed_) {
    return;
  }
  closed_ = true;
  const std::uint32_t data_bytes = frames_written_ * static_cast<std::uint32_t>(sizeof(float));
  stream_.seekp(4, std::ios::beg);
  write_u32(stream_, 36 + data_bytes);
  stream_.seekp(40, std::ios::beg);
  write_u32(stream_, data_bytes);
  stream_.close();
}

// ---- WavReader ------------------------------------------------------------

WavReader::WavReader(const std::string& path) : stream_(path, std::ios::binary) {
  if (!stream_) {
    throw ConfigError("cannot open WAV for reading: " + path);
  }
  char tag[4];
  stream_.read(tag, 4);
  if (std::memcmp(tag, "RIFF", 4) != 0) {
    throw ConfigError("not a RIFF file: " + path);
  }
  read_u32(stream_);
  stream_.read(tag, 4);
  if (std::memcmp(tag, "WAVE", 4) != 0) {
    throw ConfigError("not a WAVE file: " + path);
  }

  bool have_format = false;
  std::uint32_t data_bytes = 0;
  // Walk the chunk list rather than assuming fmt is immediately followed by
  // data: writers interleave LIST/fact/cue chunks freely.
  while (stream_.read(tag, 4)) {
    const std::uint32_t chunk_size = read_u32(stream_);
    if (std::memcmp(tag, "fmt ", 4) == 0) {
      std::uint16_t format = read_u16(stream_);
      channels_ = read_u16(stream_);
      sample_rate_ = static_cast<int>(read_u32(stream_));
      read_u32(stream_);  // byte rate
      read_u16(stream_);  // block align
      bits_per_sample_ = read_u16(stream_);
      if (format == kFormatExtensible && chunk_size >= 40) {
        read_u16(stream_);  // extension size
        read_u16(stream_);  // valid bits
        read_u32(stream_);  // channel mask
        format = read_u16(stream_);  // first 2 bytes of the sub-format GUID
        stream_.seekg(14, std::ios::cur);
      } else if (chunk_size > 16) {
        stream_.seekg(static_cast<std::streamoff>(chunk_size - 16), std::ios::cur);
      }
      if (format != kFormatPcm && format != kFormatFloat) {
        throw ConfigError("unsupported WAV encoding (compressed audio) in " + path);
      }
      is_float_ = format == kFormatFloat;
      have_format = true;
    } else if (std::memcmp(tag, "data", 4) == 0) {
      data_offset_ = stream_.tellg();
      data_bytes = chunk_size;
      break;
    } else {
      // Chunks are word-aligned; odd sizes carry a pad byte.
      stream_.seekg(static_cast<std::streamoff>(chunk_size + (chunk_size & 1u)), std::ios::cur);
    }
  }

  if (!have_format || data_offset_ == std::streampos(0)) {
    throw ConfigError("WAV is missing a fmt or data chunk: " + path);
  }
  const auto frame_bytes =
      static_cast<std::uint32_t>(channels_ * (bits_per_sample_ / 8));
  frames_ = frame_bytes > 0 ? data_bytes / frame_bytes : 0;
  stream_.seekg(data_offset_);
}

WavReader::~WavReader() = default;

Samples WavReader::read(std::size_t frame_count) {
  const auto remaining = static_cast<std::size_t>(frames_ - frames_read_);
  const std::size_t wanted = std::min(frame_count, remaining);
  if (wanted == 0) {
    return {};
  }
  const auto sample_bytes = static_cast<std::size_t>(bits_per_sample_ / 8);
  const std::size_t frame_bytes = sample_bytes * static_cast<std::size_t>(channels_);
  std::vector<unsigned char> raw(wanted * frame_bytes);
  stream_.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
  const auto bytes_read = static_cast<std::size_t>(stream_.gcount());
  const std::size_t frames_available = frame_bytes > 0 ? bytes_read / frame_bytes : 0;

  Samples out(frames_available);
  for (std::size_t frame = 0; frame < frames_available; ++frame) {
    double total = 0.0;
    for (int channel = 0; channel < channels_; ++channel) {
      const unsigned char* pointer =
          raw.data() + frame * frame_bytes + static_cast<std::size_t>(channel) * sample_bytes;
      total += static_cast<double>(decode_sample(pointer, bits_per_sample_, is_float_));
    }
    out[frame] = static_cast<float>(total / static_cast<double>(channels_));
  }
  frames_read_ += frames_available;
  return out;
}

// ---- convenience ----------------------------------------------------------

void write_wav(const std::string& path, const Samples& samples, double sample_rate) {
  WavWriter writer(path, static_cast<int>(std::lround(sample_rate)));
  writer.write(samples);
  writer.close();
}

WavData read_wav(const std::string& path, double expected_sample_rate) {
  WavReader reader(path);
  if (expected_sample_rate > 0.0 &&
      static_cast<int>(std::lround(expected_sample_rate)) != reader.sample_rate()) {
    throw ConfigError("WAV sample rate " + std::to_string(reader.sample_rate()) +
                      " Hz does not match expected " +
                      std::to_string(static_cast<int>(std::lround(expected_sample_rate))) + " Hz");
  }
  WavData data;
  data.sample_rate = reader.sample_rate();
  data.samples.reserve(static_cast<std::size_t>(reader.frames()));
  while (true) {
    const Samples chunk = reader.read(65536);
    if (chunk.empty()) {
      break;
    }
    data.samples.insert(data.samples.end(), chunk.begin(), chunk.end());
  }
  return data;
}

}  // namespace weaklink::audio
