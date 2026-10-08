#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace weaklink::rs::gf {

/// GF(2^8) log/antilog tables for primitive polynomial 0x11d with generator 2.
///
/// ``exp`` is doubled to 512 entries so a sum of two logs can index it without
/// a modulo -- the standard trick, and the one ``reedsolo`` relies on.
struct Tables {
  std::array<uint8_t, 512> exp{};
  std::array<uint8_t, 256> log{};
  Tables();
};

const Tables& tables();

inline uint8_t add(uint8_t a, uint8_t b) { return static_cast<uint8_t>(a ^ b); }

inline uint8_t mul(uint8_t a, uint8_t b) {
  if (a == 0 || b == 0) {
    return 0;
  }
  const Tables& t = tables();
  return t.exp[static_cast<std::size_t>(t.log[a]) + static_cast<std::size_t>(t.log[b])];
}

inline uint8_t pow(uint8_t base, int exponent) {
  const Tables& t = tables();
  if (base == 0) {
    return 0;
  }
  int index = (static_cast<int>(t.log[base]) * exponent) % 255;
  if (index < 0) {
    index += 255;
  }
  return t.exp[static_cast<std::size_t>(index)];
}

inline uint8_t inverse(uint8_t value) {
  const Tables& t = tables();
  return t.exp[255 - static_cast<std::size_t>(t.log[value])];
}

inline uint8_t div(uint8_t a, uint8_t b) {
  if (a == 0) {
    return 0;
  }
  const Tables& t = tables();
  const int index =
      (static_cast<int>(t.log[a]) + 255 - static_cast<int>(t.log[b])) % 255;
  return t.exp[static_cast<std::size_t>(index)];
}

/// Polynomials are coefficient vectors, highest power first.
std::vector<uint8_t> poly_scale(const std::vector<uint8_t>& poly, uint8_t factor);
std::vector<uint8_t> poly_add(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b);
std::vector<uint8_t> poly_mul(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b);
uint8_t poly_eval(const std::vector<uint8_t>& poly, uint8_t x);

}  // namespace weaklink::rs::gf
