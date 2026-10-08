// Streaming RX behaviours that the batch path cannot exercise: cross-call
// block dedup, and locking on after the decoder has already chewed through
// silence or noise.

#include <string>
#include <vector>

#include "catch.hpp"
#include "support.hpp"
#include "weaklink/rng.hpp"

using namespace weaklink;
using namespace weaklink::test;

namespace {

/// Drive the decoder with fixed-size chunks, exactly like the live-rx callback
/// or the WAV chunk reader do.
ByteVector stream_chunks(const Samples& audio, const ModemConfig& config,
                         std::size_t chunk_samples) {
  ByteVector out;
  streaming::StreamingRxDecoder decoder(
      config, [&](const ByteVector& bytes) { out.insert(out.end(), bytes.begin(), bytes.end()); });
  for (std::size_t start = 0; start < audio.size(); start += chunk_samples) {
    decoder.push(audio.data() + start, std::min(chunk_samples, audio.size() - start));
  }
  decoder.drain();
  return out;
}

/// A payload long enough to span several blocks, with enough structure that a
/// duplicated or dropped block is obvious in the diff.
ByteVector multi_block_payload() {
  std::string text;
  for (int line = 0; line < 10; ++line) {
    text += "line " + std::to_string(line) +
            ": weaklink streaming decoder regression payload\n";
  }
  return to_bytes(text);
}

}  // namespace

TEST_CASE("block copies straddling a poll boundary are not emitted twice",
          "[streaming-rx][slow]") {
  // block_repeats > 1 means several copies of each block are on the wire. If
  // the copies land either side of a decode call, the block must still be
  // written exactly once.
  for (const double baud : {300.0, 1200.0}) {
    const ByteVector payload = multi_block_payload();
    const ModemConfig config = preset_config(baud);
    const Samples audio = codec::encode(payload, config);
    const auto chunk = static_cast<std::size_t>(0.1 * config.waveform().sample_rate());

    INFO(baud << " baud");
    REQUIRE(stream_chunks(audio, config, chunk) == payload);
  }
}

TEST_CASE("the decoder locks on after leading silence", "[streaming-rx]") {
  // Live RX starts listening before TX fires. The coarse-offset cache used to
  // be filled on the first call regardless of whether preambles were found, so
  // a first call on silence would latch a noise-floor peak and never recover.
  for (const double baud : {300.0, 1200.0}) {
    const ModemConfig config = preset_config(baud);
    const ByteVector payload = to_bytes("weaklink starts on silence and still decodes");
    const Samples signal = codec::encode(payload, config);
    const auto rate = static_cast<std::size_t>(config.waveform().sample_rate());

    Samples buffer(3 * rate, 0.0f);  // 3 s of silence first
    buffer.insert(buffer.end(), signal.begin(), signal.end());

    INFO(baud << " baud after silence");
    REQUIRE(contains(stream_chunks(buffer, config, rate / 10), payload));
  }
}

TEST_CASE("the decoder locks on after a leading noise floor", "[streaming-rx]") {
  // Closer to what a real microphone or monitor source delivers when nothing
  // is being transmitted.
  for (const double baud : {300.0, 1200.0}) {
    const ModemConfig config = preset_config(baud);
    const ByteVector payload = to_bytes("weaklink starts on room noise");
    const Samples signal = codec::encode(payload, config);
    const auto rate = static_cast<std::size_t>(config.waveform().sample_rate());

    rng::NumpyGenerator generator(static_cast<uint64_t>(baud));
    Samples buffer(3 * rate);
    for (float& sample : buffer) {
      sample = static_cast<float>(generator.standard_normal() * 0.005);
    }
    buffer.insert(buffer.end(), signal.begin(), signal.end());

    INFO(baud << " baud after noise");
    REQUIRE(contains(stream_chunks(buffer, config, rate / 10), payload));
  }
}
