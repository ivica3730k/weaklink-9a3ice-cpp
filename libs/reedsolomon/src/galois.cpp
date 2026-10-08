#include "galois.hpp"

#include <algorithm>

namespace weaklink::rs::gf {

Tables::Tables() {
  constexpr int kPrimitivePolynomial = 0x11d;
  int x = 1;
  for (int i = 0; i < 255; ++i) {
    exp[static_cast<std::size_t>(i)] = static_cast<uint8_t>(x);
    log[static_cast<std::size_t>(x)] = static_cast<uint8_t>(i);
    x <<= 1;
    if (x & 0x100) {
      x ^= kPrimitivePolynomial;
    }
  }
  for (int i = 255; i < 512; ++i) {
    exp[static_cast<std::size_t>(i)] = exp[static_cast<std::size_t>(i - 255)];
  }
}

const Tables& tables() {
  static const Tables instance;
  return instance;
}

std::vector<uint8_t> poly_scale(const std::vector<uint8_t>& poly, uint8_t factor) {
  std::vector<uint8_t> out(poly.size());
  for (std::size_t i = 0; i < poly.size(); ++i) {
    out[i] = mul(poly[i], factor);
  }
  return out;
}

std::vector<uint8_t> poly_add(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  std::vector<uint8_t> out(std::max(a.size(), b.size()), 0);
  for (std::size_t i = 0; i < a.size(); ++i) {
    out[i + out.size() - a.size()] = a[i];
  }
  for (std::size_t i = 0; i < b.size(); ++i) {
    out[i + out.size() - b.size()] ^= b[i];
  }
  return out;
}

std::vector<uint8_t> poly_mul(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  if (a.empty() || b.empty()) {
    return {};
  }
  std::vector<uint8_t> out(a.size() + b.size() - 1, 0);
  for (std::size_t j = 0; j < b.size(); ++j) {
    if (b[j] == 0) {
      continue;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
      out[i + j] ^= mul(a[i], b[j]);
    }
  }
  return out;
}

uint8_t poly_eval(const std::vector<uint8_t>& poly, uint8_t x) {
  if (poly.empty()) {
    return 0;
  }
  uint8_t y = poly[0];
  for (std::size_t i = 1; i < poly.size(); ++i) {
    y = static_cast<uint8_t>(mul(y, x) ^ poly[i]);
  }
  return y;
}

}  // namespace weaklink::rs::gf
