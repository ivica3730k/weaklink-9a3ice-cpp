// Soft-LLR combining across block_repeats copies.
//
// Each copy is interleaved with its own permutation, so a channel burst hits
// different code positions in each one. Summing the deinterleaved LLRs before
// Viterbi lets the good bits reinforce while the error patterns average out --
// classical soft-combining diversity. Two marginal copies should therefore
// decode a payload that neither clears alone.

#include "catch.hpp"
#include "support.hpp"
#include "weaklink/rng.hpp"

using namespace weaklink;
using namespace weaklink::test;

TEST_CASE("combined LLRs decode below the single-copy cliff", "[llr][slow]") {
  for (const int block_repeats : {2, 4}) {
    const ByteVector payload = rng::NumpyGenerator(static_cast<uint64_t>(block_repeats)).bytes(80);
    const ModemConfig config = make_config(300.0, 300.0, 4, 16, 8, block_repeats);
    const Samples audio = codec::encode(payload, config);
    // 3 dB below where a single copy decodes, so combining has to earn its dB.
    const Samples noisy =
        add_awgn(audio, -4.0, static_cast<uint64_t>(block_repeats) * 101, 18000.0);

    INFO("block_repeats = " << block_repeats);
    REQUIRE(codec::decode(noisy, config).bytes == payload);
    REQUIRE(stream_decode(noisy, config) == payload);
  }
}
