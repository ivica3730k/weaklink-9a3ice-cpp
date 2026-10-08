#include "weaklink/bits.hpp"

#include <array>
#include <stdexcept>

namespace weaklink {
namespace {

std::array<uint32_t, 256> build_crc_table() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    table[i] = c;
  }
  return table;
}

const std::array<uint32_t, 256>& crc_table() {
  static const std::array<uint32_t, 256> table = build_crc_table();
  return table;
}

}  // namespace

BitVector bytes_to_bits_msb(const ByteVector& data) {
  BitVector out(data.size() * 8);
  for (std::size_t byte_index = 0; byte_index < data.size(); ++byte_index) {
    const uint8_t value = data[byte_index];
    for (std::size_t bit_index = 0; bit_index < 8; ++bit_index) {
      out[byte_index * 8 + bit_index] =
          static_cast<uint8_t>((value >> (7 - bit_index)) & 1u);
    }
  }
  return out;
}

ByteVector bits_to_bytes_msb(const BitVector& bits) {
  if (bits.size() % 8 != 0) {
    throw std::invalid_argument("bit length " + std::to_string(bits.size()) +
                                " not a multiple of 8");
  }
  ByteVector out(bits.size() / 8, 0);
  for (std::size_t index = 0; index < bits.size(); ++index) {
    out[index / 8] = static_cast<uint8_t>(
        out[index / 8] | static_cast<uint8_t>((bits[index] & 1u) << (7 - (index % 8))));
  }
  return out;
}

BitVector pad_to_multiple(const BitVector& bits, std::size_t multiple) {
  if (multiple == 0 || bits.size() % multiple == 0) {
    return bits;
  }
  BitVector out = bits;
  out.resize(bits.size() + (multiple - (bits.size() % multiple)), 0);
  return out;
}

std::size_t round_up_multiple(std::size_t value, std::size_t multiple) {
  if (multiple == 0) {
    return value;
  }
  const std::size_t remainder = value % multiple;
  return remainder == 0 ? value : value + (multiple - remainder);
}

uint32_t crc32(const uint8_t* data, std::size_t length) {
  const auto& table = crc_table();
  uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < length; ++i) {
    crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

uint32_t crc32(const ByteVector& data) { return crc32(data.data(), data.size()); }

}  // namespace weaklink
