// WAV robustness: encode, damage, decode. The buffer damaged here is the same
// pilot-padded one the CLI writes to the wire, so the damage models (late
// start, early stop, fading, noise) map onto real failures.

#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "catch.hpp"
#include "support.hpp"

using namespace weaklink;
using namespace weaklink::test;

namespace {

constexpr double kTwoPi = 6.283185307179586;

const ByteVector kPayload1B = to_bytes("h");
const ByteVector kPayload10B = to_bytes("helloWorld");

/// RX started late.
Samples head_chop(const Samples& buffer, double chop_ms, double sample_rate) {
  const auto cut = static_cast<std::size_t>(chop_ms * sample_rate / 1000.0);
  if (cut >= buffer.size()) {
    return {};
  }
  return Samples(buffer.begin() + static_cast<std::ptrdiff_t>(cut), buffer.end());
}

/// Sink underrun, or Ctrl-C too early.
Samples tail_chop(const Samples& buffer, double chop_ms, double sample_rate) {
  const auto cut = static_cast<std::size_t>(chop_ms * sample_rate / 1000.0);
  if (cut == 0 || cut >= buffer.size()) {
    return cut == 0 ? buffer : Samples{};
  }
  return Samples(buffer.begin(), buffer.end() - static_cast<std::ptrdiff_t>(cut));
}

/// Sinusoidal amplitude envelope from 10^(-dB/20) up to 1.0.
Samples fade(const Samples& buffer, double depth_db, double cycles, double sample_rate) {
  const double duration = static_cast<double>(buffer.size()) / sample_rate;
  const double trough = std::pow(10.0, -depth_db / 20.0);
  Samples out(buffer.size());
  for (std::size_t i = 0; i < buffer.size(); ++i) {
    const double t = static_cast<double>(i) / sample_rate;
    const double envelope =
        trough + (1.0 - trough) * (0.5 + 0.5 * std::cos(kTwoPi * cycles * t / duration));
    out[i] = static_cast<float>(static_cast<double>(buffer[i]) * envelope);
  }
  return out;
}

struct DamageCase {
  double baud;
  ByteVector payload;
  std::string label;
  std::function<Samples(const Samples&, double)> apply;
};

std::vector<DamageCase> damage_cases() {
  std::vector<DamageCase> cases;
  const std::vector<double> bauds = {45.0, 300.0, 1200.0};

  // Clean baseline for every baud and payload -- catches plain encode/decode
  // regressions before any damage model muddies the picture.
  for (const double baud : bauds) {
    for (const ByteVector& payload : {kPayload1B, kPayload10B}) {
      cases.push_back({baud, payload, "clean", [](const Samples& a, double) { return a; }});
    }
  }

  // Head chop: the decoder projects a virtual leading preamble; when that
  // lands before the buffer start the magnitudes are zero-padded and RS fixes
  // the resulting errors.
  for (const double baud : bauds) {
    for (const double chop_ms : {100.0, 300.0, 500.0}) {
      cases.push_back({baud, kPayload1B, "head-chop-" + std::to_string(static_cast<int>(chop_ms)) + "ms",
                       [chop_ms](const Samples& a, double sr) { return head_chop(a, chop_ms, sr); }});
    }
  }

  // Tail chop: the trailing pilot is 200 ms, so anything up to that is a no-op.
  for (const double baud : bauds) {
    for (const double chop_ms : {200.0, 400.0}) {
      cases.push_back({baud, kPayload1B, "tail-chop-" + std::to_string(static_cast<int>(chop_ms)) + "ms",
                       [chop_ms](const Samples& a, double sr) { return tail_chop(a, chop_ms, sr); }});
    }
  }

  // Slow fading: 10 dB peak to trough, about one cycle across the burst.
  for (const double baud : bauds) {
    cases.push_back({baud, kPayload10B, "fade-10dB",
                     [](const Samples& a, double sr) { return fade(a, 10.0, 1.0, sr); }});
  }

  // Compound: the worst plausible real-world pairing.
  for (const double baud : bauds) {
    cases.push_back({baud, kPayload10B, "head-chop-100ms+fade-6dB",
                     [](const Samples& a, double sr) {
                       return fade(head_chop(a, 100.0, sr), 6.0, 1.5, sr);
                     }});
  }

  // AWGN roughly 3 dB above each preset's cliff: below-noise operation has to
  // keep working.
  const std::vector<std::pair<double, double>> noise_targets = {
      {45.0, -11.0}, {300.0, -2.0}, {1200.0, 5.0}};
  for (const auto& target : noise_targets) {
    cases.push_back({target.first, kPayload10B,
                     "awgn-" + std::to_string(static_cast<int>(target.second)) + "dB",
                     [snr = target.second](const Samples& a, double sr) {
                       return add_awgn(a, snr, 1, sr);
                     }});
  }

  return cases;
}

}  // namespace

TEST_CASE("decode survives WAV damage", "[damage][slow]") {
  for (const DamageCase& test_case : damage_cases()) {
    const LiveTxBuffer buffer = live_tx_buffer(test_case.baud, test_case.payload);
    const Samples damaged =
        test_case.apply(buffer.audio, buffer.config.waveform().sample_rate());

    INFO(test_case.baud << " baud, " << test_case.payload.size() << "-byte payload, "
                        << test_case.label);

    REQUIRE(contains(codec::decode(damaged, buffer.config).bytes, test_case.payload));
    REQUIRE(contains(stream_decode(damaged, buffer.config), test_case.payload));
  }
}
