#pragma once

#include <cstddef>
#include <vector>

#include "weaklink/bits.hpp"

namespace weaklink::fec {

/// Constraint length. 64 trellis states.
inline constexpr int kConstraintLength = 7;
inline constexpr std::size_t kNumStates = 1u << (kConstraintLength - 1);

/// Rate-1/2 K=7 convolutional code with soft-decision Viterbi.
///
/// NASA/CCSDS generators 171 and 133 octal. The code is terminated: K-1 zero
/// tail bits are flushed so the trellis ends in state 0 and the decoder gets a
/// hard boundary condition at both ends.
///
/// Output length is ``2 * (bits.size() + K - 1)``.
BitVector encode(const BitVector& bits);

/// ``soft_bits`` are LLR-shaped: positive means the bit is more likely 0.
/// The path metric is the sum of LLRs along a path, maximised.
///
/// Throws ``std::invalid_argument`` unless
/// ``soft_bits.size() == 2 * (num_output_bits + K - 1)``.
BitVector decode(const std::vector<double>& soft_bits, std::size_t num_output_bits);

}  // namespace weaklink::fec
