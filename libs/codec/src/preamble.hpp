#pragma once

#include <vector>

namespace weaklink::codec::detail {

/// Peak-normalised worst sidelobe of the correlator run over the preamble at
/// every aperiodic offset != 0, assuming positions outside the overlap carry
/// balanced random data (mean 0.5 of tone amplitude).
///
/// This depends only on the fixed PN sequence, so it is computed once per mode
/// and used as a detection floor -- no hand-tuned fudge factor.
double preamble_deterministic_sidelobe(int num_tones);

}  // namespace weaklink::codec::detail
