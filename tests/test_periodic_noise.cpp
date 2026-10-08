// Periodic man-made noise -- switch-mode supplies, mains harmonics, alternator
// whine -- lands on the same bit positions in every block when the interleaver
// is fixed. The per-block permutation scrambles that periodicity so RS sees
// random errors across blocks instead of one persistent pattern.
//
// The injected notch repeats about once per block, which is the synthetic
// worst case for a fixed interleaver.

#include <cmath>

#include "catch.hpp"
#include "support.hpp"
#include "weaklink/rng.hpp"

using namespace weaklink;
using namespace weaklink::test;

namespace {

/// Multiply by an envelope that drops ``depth_db`` for ``notch_width_seconds``
/// once every ``period_seconds``.
Samples inject_periodic_notch(const Samples& audio, double sample_rate, double period_seconds,
                              double notch_width_seconds, double depth_db) {
  const double notch_fraction = notch_width_seconds / period_seconds;
  const auto trough = static_cast<float>(std::pow(10.0, -depth_db / 20.0));
  Samples out(audio.size());
  for (std::size_t i = 0; i < audio.size(); ++i) {
    const double t = static_cast<double>(i) / sample_rate;
    const double phase = std::fmod(t, period_seconds) / period_seconds;
    out[i] = audio[i] * (phase < notch_fraction ? trough : 1.0f);
  }
  return out;
}

}  // namespace

TEST_CASE("decode survives notches that repeat once per block", "[periodic][slow]") {
  for (const std::size_t payload_size : {std::size_t{200}, std::size_t{500}}) {
    const ModemConfig config = make_config(300.0, 300.0, 4, 16, 8, 1);
    const ByteVector payload =
        rng::NumpyGenerator(static_cast<uint64_t>(payload_size)).bytes(payload_size);
    const Samples audio = codec::encode(payload, config);

    // A block is about 0.85 s at 300 baud with RS(16,8), so this is roughly
    // one notch per block at a 10% duty cycle.
    const Samples notched = inject_periodic_notch(audio, config.waveform().sample_rate(),
                                                  0.85, 0.08, 15.0);

    INFO("payload size " << payload_size);
    REQUIRE(codec::decode(notched, config).bytes == payload);
    REQUIRE(stream_decode(notched, config) == payload);
  }
}
