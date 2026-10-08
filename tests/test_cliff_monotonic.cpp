// Decode success must not go up as SNR goes down.
//
// A correlator threshold that is mistuned for a particular alphabet size can
// make a mode decode *better* with more noise, which is how the 2-FSK sidelobe
// bug showed up. Monotonicity is the property that catches it.

#include <vector>

#include "catch.hpp"
#include "support.hpp"
#include "weaklink/exceptions.hpp"

using namespace weaklink;
using namespace weaklink::test;

TEST_CASE("decode success is monotonic in SNR", "[cliff][slow]") {
  const std::vector<double> snr_sweep = {10.0, 5.0, 0.0, -5.0, -10.0};

  for (const double baud : {45.0, 300.0, 1200.0}) {
    for (const int num_tones : {2, 4, 8, 16}) {
      ModemConfig config = default_config();
      try {
        config = make_config(baud, baud, num_tones, 16, 8, 2);
      } catch (const ConfigError&) {
        continue;  // tone stack does not fit under Nyquist
      }

      const ByteVector payload = to_bytes("HI!!");
      const Samples samples = codec::encode(payload, config);

      std::vector<int> successes;
      successes.reserve(snr_sweep.size());
      for (const double snr : snr_sweep) {
        int count = 0;
        for (int trial = 0; trial < 3; ++trial) {
          const auto seed = static_cast<uint64_t>(trial * 17 + num_tones);
          const Samples noisy =
              add_awgn(samples, snr, seed, config.waveform().sample_rate());
          if (contains(codec::decode(noisy, config).bytes, payload)) {
            ++count;
          }
        }
        successes.push_back(count);
      }

      INFO("baud=" << baud << " num_tones=" << num_tones);
      for (std::size_t i = 0; i + 1 < successes.size(); ++i) {
        INFO("SNR " << snr_sweep[i] << " dB -> " << successes[i] << ", " << snr_sweep[i + 1]
                    << " dB -> " << successes[i + 1]);
        REQUIRE(successes[i] >= successes[i + 1]);
      }
    }
  }
}
