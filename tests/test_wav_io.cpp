// WAV reader/writer. The Python implementation delegates this to libsndfile;
// the port writes RIFF directly, so the format details are worth pinning.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "catch.hpp"
#include "weaklink/exceptions.hpp"
#include "weaklink/wav.hpp"

using namespace weaklink;

namespace {

/// Temporary file that removes itself, so a failing assertion cannot leave
/// scratch WAVs behind.
struct ScratchFile {
  explicit ScratchFile(std::string name) : path(std::move(name)) {}
  ~ScratchFile() { std::remove(path.c_str()); }
  ScratchFile(const ScratchFile&) = delete;
  ScratchFile& operator=(const ScratchFile&) = delete;
  std::string path;
};

}  // namespace

TEST_CASE("float32 mono survives a write/read round-trip", "[wav]") {
  ScratchFile file("wl_test_io.wav");
  audio::Samples samples(1000);
  for (std::size_t i = 0; i < samples.size(); ++i) {
    samples[i] = static_cast<float>(std::sin(static_cast<double>(i) * 0.01) * 0.25);
  }

  audio::write_wav(file.path, samples, 18000.0);
  const audio::WavData reloaded = audio::read_wav(file.path);

  REQUIRE(reloaded.sample_rate == 18000);
  REQUIRE(reloaded.samples.size() == samples.size());
  // float32 in, float32 out: the samples should come back bit-identical.
  REQUIRE(reloaded.samples == samples);
}

TEST_CASE("the chunked reader returns the same samples as a whole read", "[wav]") {
  ScratchFile file("wl_test_chunks.wav");
  audio::Samples samples(5000);
  for (std::size_t i = 0; i < samples.size(); ++i) {
    samples[i] = static_cast<float>(i % 97) / 100.0f - 0.5f;
  }
  audio::write_wav(file.path, samples, 18000.0);

  audio::WavReader reader(file.path);
  REQUIRE(reader.sample_rate() == 18000);
  REQUIRE(reader.channels() == 1);
  REQUIRE(reader.frames() == samples.size());

  audio::Samples collected;
  while (true) {
    const audio::Samples chunk = reader.read(333);
    if (chunk.empty()) {
      break;
    }
    collected.insert(collected.end(), chunk.begin(), chunk.end());
  }
  REQUIRE(collected == samples);
}

TEST_CASE("a streaming writer matches a one-shot write", "[wav]") {
  ScratchFile streamed("wl_test_streamed.wav");
  ScratchFile oneshot("wl_test_oneshot.wav");

  audio::Samples samples(777);
  for (std::size_t i = 0; i < samples.size(); ++i) {
    samples[i] = static_cast<float>(i) / 1000.0f;
  }

  {
    audio::WavWriter writer(streamed.path, 18000);
    for (std::size_t offset = 0; offset < samples.size(); offset += 100) {
      const std::size_t count = std::min<std::size_t>(100, samples.size() - offset);
      writer.write(samples.data() + offset, count);
    }
    writer.close();
  }
  audio::write_wav(oneshot.path, samples, 18000.0);

  REQUIRE(audio::read_wav(streamed.path).samples == audio::read_wav(oneshot.path).samples);
}

TEST_CASE("a sample-rate mismatch is rejected", "[wav]") {
  // A rate mismatch silently destroys symbol timing, so it is worth failing
  // loudly rather than decoding noise.
  ScratchFile file("wl_test_rate.wav");
  audio::write_wav(file.path, audio::Samples(100, 0.0f), 18000.0);
  REQUIRE_THROWS_AS(audio::read_wav(file.path, 48000.0), ConfigError);
  REQUIRE_NOTHROW(audio::read_wav(file.path, 18000.0));
}

TEST_CASE("opening a missing or non-RIFF file raises", "[wav]") {
  REQUIRE_THROWS_AS(audio::read_wav("wl_test_does_not_exist.wav"), ConfigError);

  ScratchFile junk("wl_test_junk.wav");
  {
    std::FILE* handle = std::fopen(junk.path.c_str(), "wb");
    REQUIRE(handle != nullptr);
    std::fputs("not a wav file at all", handle);
    std::fclose(handle);
  }
  REQUIRE_THROWS_AS(audio::read_wav(junk.path), ConfigError);
}

TEST_CASE("an empty file still produces a readable WAV", "[wav]") {
  ScratchFile file("wl_test_empty.wav");
  audio::write_wav(file.path, audio::Samples{}, 18000.0);
  const audio::WavData reloaded = audio::read_wav(file.path);
  REQUIRE(reloaded.sample_rate == 18000);
  REQUIRE(reloaded.samples.empty());
}
