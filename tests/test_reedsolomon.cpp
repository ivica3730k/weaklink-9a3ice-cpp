// Reed-Solomon parity vectors taken from the Python implementation's
// ``reedsolo.RSCodec``. The parity bytes are part of the wire format, so these
// pin the field conventions (primitive polynomial 0x11d, generator 2, first
// consecutive root 0) rather than just "some working RS code".

#include <vector>

#include "catch.hpp"
#include "weaklink/bits.hpp"
#include "weaklink/reedsolomon.hpp"

using weaklink::rs::BlockCodec;
using weaklink::rs::BlockConfig;
using weaklink::rs::Bytes;
using weaklink::rs::ReedSolomonCodec;

namespace {

Bytes counting_bytes(int count) {
  Bytes out(static_cast<std::size_t>(count));
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<uint8_t>(i);
  }
  return out;
}

}  // namespace

TEST_CASE("parity bytes match reedsolo", "[rs]") {
  const Bytes message = counting_bytes(20);

  SECTION("8 parity bytes") {
    const Bytes expected = {134, 200, 218, 90, 112, 113, 248, 55};
    const Bytes encoded = ReedSolomonCodec(8).encode(message);
    REQUIRE(Bytes(encoded.begin() + 20, encoded.end()) == expected);
  }

  SECTION("16 parity bytes") {
    const Bytes expected = {124, 253, 92,  90,  64,  190, 102, 22,
                            116, 41,  171, 80,  22,  194, 133, 254};
    const Bytes encoded = ReedSolomonCodec(16).encode(message);
    REQUIRE(Bytes(encoded.begin() + 20, encoded.end()) == expected);
  }

  SECTION("32 parity bytes") {
    const Bytes expected = {96,  247, 38,  232, 126, 96,  92,  112, 46,  188, 128,
                            229, 189, 156, 57,  146, 198, 95,  221, 216, 149, 219,
                            160, 246, 178, 62,  86,  56,  99,  224, 253, 14};
    const Bytes encoded = ReedSolomonCodec(32).encode(message);
    REQUIRE(Bytes(encoded.begin() + 20, encoded.end()) == expected);
  }
}

TEST_CASE("CRC-32 matches zlib", "[rs]") {
  const Bytes hello = {'h', 'e', 'l', 'l', 'o'};
  REQUIRE(weaklink::crc32(hello) == 907060870u);
  REQUIRE(weaklink::crc32(counting_bytes(16)) == 3469664904u);
}

TEST_CASE("framed block matches the Python block codec", "[rs]") {
  BlockConfig config;
  config.data_bytes = 16;
  config.parity_bytes = 8;
  config.crc_enabled = true;
  const BlockCodec codec(config);

  const Bytes payload = counting_bytes(16);
  const Bytes expected = {0,   1,   2,   3,  4,   5,   6,   7,  8,  9,   10, 11,
                          12,  13,  14,  15, 206, 206, 226, 136, 54, 226, 250, 172,
                          32,  103, 192, 111};
  REQUIRE(codec.encode(payload) == expected);
}

TEST_CASE("RS corrects up to parity/2 byte errors", "[rs]") {
  BlockConfig config;
  config.data_bytes = 16;
  config.parity_bytes = 8;
  const BlockCodec codec(config);
  const Bytes payload = counting_bytes(16);
  const Bytes block = codec.encode(payload);

  SECTION("four corrupted bytes are repaired") {
    Bytes damaged = block;
    damaged[0] ^= 0xFF;
    damaged[5] ^= 0x3C;
    damaged[11] ^= 0x01;
    damaged[17] ^= 0x80;
    const auto decoded = codec.decode(damaged);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->payload == payload);
    REQUIRE(decoded->errors_corrected == 4);
  }

  SECTION("a clean block reports no corrections") {
    const auto decoded = codec.decode(block);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->payload == payload);
    REQUIRE(decoded->errors_corrected == 0);
  }

  SECTION("past the correcting power the block is rejected, not guessed") {
    Bytes damaged = block;
    for (std::size_t i = 0; i < 6; ++i) {
      damaged[i] ^= 0xFF;
    }
    REQUIRE_FALSE(codec.decode(damaged).has_value());
  }
}

TEST_CASE("a flipped CRC is rejected even when RS succeeds", "[rs]") {
  // RS can "succeed" on a codeword corrupted past its reach; the CRC is what
  // keeps that from reaching the output.
  BlockConfig config;
  config.data_bytes = 8;
  config.parity_bytes = 0;
  config.crc_enabled = true;
  const BlockCodec codec(config);

  Bytes block = codec.encode(counting_bytes(8));
  block[2] ^= 0x01;
  REQUIRE_FALSE(codec.decode(block).has_value());
}

TEST_CASE("block sizes beyond one RS codeword are rejected at construction", "[rs]") {
  // The Python implementation silently chunks here while still computing its
  // block length as data + crc + parity, which desynchronises the decoder.
  BlockConfig config;
  config.data_bytes = 256;
  config.parity_bytes = 32;
  REQUIRE_THROWS(BlockCodec(config));
}
