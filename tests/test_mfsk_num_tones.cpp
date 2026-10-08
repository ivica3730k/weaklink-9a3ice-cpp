// Every (baud, num_tones) combination that fits under Nyquist encodes and
// decodes byte for byte. Combinations that do not fit raise when the waveform
// config is built; those are skipped rather than asserted on.

#include <string>

#include "catch.hpp"
#include "support.hpp"
#include "weaklink/exceptions.hpp"

using namespace weaklink;
using namespace weaklink::test;

TEST_CASE("MFSK round-trips at every feasible mode", "[mfsk]") {
  for (const double baud : {45.0, 300.0, 1200.0}) {
    for (const int num_tones : {2, 4, 8, 16}) {
      ModemConfig config = default_config();
      try {
        config = make_config(baud, baud, num_tones, 16, 8, 1);
      } catch (const ConfigError&) {
        continue;  // tone stack does not fit under Nyquist
      }

      const ByteVector payload =
          to_bytes("weaklink at " + std::to_string(num_tones) + "-FSK");
      const Samples audio = codec::encode(payload, config);
      const ByteVector decoded = codec::decode(audio, config).bytes;

      INFO(num_tones << "-FSK @ " << baud << " baud");
      REQUIRE(decoded == payload);
    }
  }
}

TEST_CASE("OOK round-trips at every baud", "[mfsk]") {
  // num_tones == 1 is on-off keying: one carrier, symbol 0 silent. It is the
  // narrowest mode and still carries one bit per symbol.
  for (const double baud : {45.0, 300.0, 1200.0}) {
    const ModemConfig config = make_config(baud, baud, 1, 16, 8, 1);
    const ByteVector payload = to_bytes("weaklink OOK");
    const Samples audio = codec::encode(payload, config);
    INFO("OOK @ " << baud << " baud");
    REQUIRE(codec::decode(audio, config).bytes == payload);
  }
}

TEST_CASE("bits per symbol scales with the alphabet", "[mfsk]") {
  REQUIRE(dsp::bits_per_symbol_for(1) == 1);  // OOK: 2 symbols, 1 bit
  REQUIRE(dsp::bits_per_symbol_for(2) == 1);
  REQUIRE(dsp::bits_per_symbol_for(4) == 2);
  REQUIRE(dsp::bits_per_symbol_for(8) == 3);
  REQUIRE(dsp::bits_per_symbol_for(16) == 4);
}

TEST_CASE("a non-power-of-two alphabet is rejected", "[mfsk]") {
  dsp::WaveformOptions options;
  options.num_tones = 6;
  REQUIRE_THROWS_AS(options.build(), ConfigError);
}
