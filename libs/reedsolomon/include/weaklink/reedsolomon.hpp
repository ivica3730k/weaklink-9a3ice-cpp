#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace weaklink::rs {

using Bytes = std::vector<uint8_t>;

/// Systematic Reed-Solomon over GF(2^8) with the conventions the Python
/// implementation inherits from ``reedsolo.RSCodec``: primitive polynomial
/// 0x11d, generator 2, first consecutive root 0. Codewords are
/// ``message || parity`` and must fit in a single 255-byte block.
class ReedSolomonCodec {
 public:
  explicit ReedSolomonCodec(int parity_bytes);

  int parity_bytes() const { return parity_bytes_; }

  /// Append ``parity_bytes`` of parity to ``message``.
  Bytes encode(const Bytes& message) const;

  /// Berlekamp-Massey + Chien + Forney. Returns the corrected message (parity
  /// stripped) and the number of byte-symbols that had to be fixed, or
  /// nothing when the codeword is beyond the code's correcting power.
  struct Decoded {
    Bytes message;
    int errors_corrected;
  };
  std::optional<Decoded> decode(const Bytes& codeword) const;

 private:
  int parity_bytes_;
  Bytes generator_;  ///< Generator polynomial, highest power first.
};

/// Bytes added by the CRC-32 that sits inside the RS-protected region.
inline constexpr int kCrcBytes = 4;

struct BlockConfig {
  int data_bytes = 16;
  int parity_bytes = 8;
  bool crc_enabled = true;

  int block_size() const {
    return data_bytes + (crc_enabled ? kCrcBytes : 0) + parity_bytes;
  }
};

/// ``data + [CRC-32] + parity`` block framer. The CRC is what makes a decode
/// trustworthy: RS can "succeed" on a codeword that was corrupted past its
/// correcting power, and the CRC is what catches that.
class BlockCodec {
 public:
  explicit BlockCodec(const BlockConfig& config);

  const BlockConfig& config() const { return config_; }

  /// Throws ``std::invalid_argument`` unless ``payload`` is exactly
  /// ``data_bytes`` long.
  Bytes encode(const Bytes& payload) const;

  struct Decoded {
    Bytes payload;
    int errors_corrected;
  };
  /// Returns nothing when RS failed outright or the CRC did not match.
  std::optional<Decoded> decode(const Bytes& block) const;

 private:
  BlockConfig config_;
  ReedSolomonCodec codec_;
};

}  // namespace weaklink::rs
