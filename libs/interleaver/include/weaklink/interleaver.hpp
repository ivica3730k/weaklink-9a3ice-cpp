#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "weaklink/bits.hpp"

namespace weaklink::interleaver {

/// Interleaver geometry. Only ``rows * cols`` matters -- it is the quantum the
/// bit stream is padded up to before permuting.
struct InterleaverConfig {
  int rows = 8;
  int cols = 32;

  std::size_t block_size() const {
    return static_cast<std::size_t>(rows) * static_cast<std::size_t>(cols);
  }
};

/// Number of distinct permutations cycled across a stream. Block N reuses the
/// permutation of block N + cycle_size. Exposed so RX can cap its brute-force
/// seed search at exactly the pool size -- further tries would only repeat
/// permutations already attempted.
std::size_t cycle_size();

/// Permute ``bits`` with the permutation for ``block_index``, zero-padding up
/// to a whole number of interleaver blocks first.
///
/// Per-block variation is what defends against periodic man-made noise: with a
/// fixed interleaver a repeating disturbance hits the same bit positions in
/// every block and RS sees a persistent error pattern instead of random one.
BitVector interleave(const BitVector& bits, const InterleaverConfig& config,
                     std::size_t block_index);

/// Undo the permutation on a soft-LLR stream and trim back to
/// ``output_length``. Throws ``std::invalid_argument`` if ``soft`` is shorter
/// than the padded block length.
std::vector<double> deinterleave_soft(const std::vector<double>& soft,
                                      const InterleaverConfig& config,
                                      std::size_t output_length,
                                      std::size_t block_index);

}  // namespace weaklink::interleaver
