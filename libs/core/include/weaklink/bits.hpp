#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace weaklink {

/// One bit per element, values 0 or 1. Mirrors the Python side's habit of
/// carrying bit streams as byte arrays so the interleaver and the
/// convolutional coder can index them directly.
using BitVector = std::vector<uint8_t>;
using ByteVector = std::vector<uint8_t>;

/// MSB-first expansion: byte 0xA0 -> 1,0,1,0,0,0,0,0.
BitVector bytes_to_bits_msb(const ByteVector& data);

/// Inverse of :func:`bytes_to_bits_msb`. Throws if the bit count is not a
/// multiple of 8.
ByteVector bits_to_bytes_msb(const BitVector& bits);

/// Zero-pad ``bits`` up to the next multiple of ``multiple``.
BitVector pad_to_multiple(const BitVector& bits, std::size_t multiple);

/// Round ``value`` up to the next multiple of ``multiple``.
std::size_t round_up_multiple(std::size_t value, std::size_t multiple);

/// CRC-32 (IEEE 802.3, the one ``zlib.crc32`` computes).
uint32_t crc32(const uint8_t* data, std::size_t length);
uint32_t crc32(const ByteVector& data);

}  // namespace weaklink
