// Streaming TX: the encoder consumes bytes as they arrive and yields audio
// incrementally, and the result decodes byte-perfect -- including payloads
// that the old 1-byte block index used to cap.

#include <string>
#include <vector>

#include "catch.hpp"
#include "support.hpp"
#include "weaklink/rng.hpp"

using namespace weaklink;
using namespace weaklink::test;

namespace {

ModemConfig streaming_config(double baud = 1200.0) {
  return make_config(baud, baud, 4, 16, 8, 1);
}

/// Encode in ``chunk_size`` pieces, exactly as a pipe would deliver them.
Samples stream_encode(const ByteVector& payload, const ModemConfig& config,
                      std::size_t chunk_size) {
  Samples out;
  std::size_t offset = 0;
  codec::encode_stream(
      [&](ByteVector& chunk) {
        if (offset >= payload.size()) {
          return false;
        }
        const std::size_t count = std::min(chunk_size, payload.size() - offset);
        chunk.assign(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                     payload.begin() + static_cast<std::ptrdiff_t>(offset + count));
        offset += count;
        return true;
      },
      config, [&](const Samples& part) { out.insert(out.end(), part.begin(), part.end()); });
  return out;
}

ByteVector random_bytes(uint64_t seed, std::size_t count) {
  return rng::NumpyGenerator(seed).bytes(count);
}

}  // namespace

TEST_CASE("chunked encoding matches the batch encoder", "[streaming-tx]") {
  ByteVector payload(1000);
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
  }
  const ModemConfig config = streaming_config();
  REQUIRE(stream_encode(payload, config, 17) == codec::encode(payload, config));
}

TEST_CASE("random payloads round-trip at several sizes", "[streaming-tx]") {
  for (const std::size_t size : {std::size_t{500}, std::size_t{5000}, std::size_t{20000}}) {
    const ByteVector payload = random_bytes(size, size);
    const ModemConfig config = streaming_config();
    const Samples audio = stream_encode(payload, config, 100);

    INFO("payload size " << size);
    REQUIRE(codec::decode(audio, config).bytes == payload);
    REQUIRE(stream_decode(audio, config) == payload);
  }
}

TEST_CASE("a highly repetitive payload survives spurious correlator peaks",
          "[streaming-tx][slow]") {
  // Repetition used to fool the correlator into finding an extra peak
  // mid-stream; the decoder must drop the spurious peak instead of flushing
  // the whole tail.
  std::string text;
  for (int i = 0; i < 20; ++i) {
    text += std::string(500, 'a') + "\n" + std::string(500, 'b') + "\n";
  }
  const ByteVector payload = to_bytes(text);
  REQUIRE(payload.size() == 20040);

  const ModemConfig config = streaming_config();
  const Samples audio = stream_encode(payload, config, 100);
  REQUIRE(codec::decode(audio, config).bytes == payload);
  REQUIRE(stream_decode(audio, config) == payload);
}

TEST_CASE("streams longer than 256 blocks are fine", "[streaming-tx][slow]") {
  // The block index used to be one byte. Two bytes lift the cap well past the
  // 500 blocks this payload needs (13 payload bytes per block).
  const ModemConfig config = streaming_config();
  const ByteVector payload(6500, 'x');
  const Samples audio = stream_encode(payload, config, 250);
  REQUIRE(codec::decode(audio, config).bytes == payload);
  REQUIRE(stream_decode(audio, config) == payload);
}
