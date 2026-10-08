#include "weaklink/reedsolomon.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

#include "galois.hpp"
#include "weaklink/bits.hpp"

namespace weaklink::rs {
namespace {

constexpr std::size_t kMaxCodewordBytes = 255;

std::vector<uint8_t> generator_poly(int parity_bytes) {
  std::vector<uint8_t> g{1};
  for (int i = 0; i < parity_bytes; ++i) {
    g = gf::poly_mul(g, {1, gf::pow(2, i)});
  }
  return g;
}

/// Syndromes with the leading zero ``reedsolo`` prepends. Downstream index
/// arithmetic (Forney in particular) assumes that offset is present.
std::vector<uint8_t> syndromes(const Bytes& codeword, int parity_bytes) {
  std::vector<uint8_t> out;
  out.reserve(static_cast<std::size_t>(parity_bytes) + 1);
  out.push_back(0);
  for (int i = 0; i < parity_bytes; ++i) {
    out.push_back(gf::poly_eval(codeword, gf::pow(2, i)));
  }
  return out;
}

/// Berlekamp-Massey. Returns an empty vector when the syndromes imply more
/// errors than the code can correct.
std::vector<uint8_t> find_error_locator(const std::vector<uint8_t>& synd, int parity_bytes) {
  std::vector<uint8_t> err_loc{1};
  std::vector<uint8_t> old_loc{1};

  std::size_t synd_shift = 0;
  if (synd.size() > static_cast<std::size_t>(parity_bytes)) {
    synd_shift = synd.size() - static_cast<std::size_t>(parity_bytes);
  }

  for (int i = 0; i < parity_bytes; ++i) {
    const std::size_t k = static_cast<std::size_t>(i) + synd_shift;
    uint8_t delta = synd[k];
    for (std::size_t j = 1; j < err_loc.size(); ++j) {
      delta = static_cast<uint8_t>(delta ^ gf::mul(err_loc[err_loc.size() - 1 - j], synd[k - j]));
    }
    old_loc.push_back(0);
    if (delta != 0) {
      if (old_loc.size() > err_loc.size()) {
        std::vector<uint8_t> new_loc = gf::poly_scale(old_loc, delta);
        old_loc = gf::poly_scale(err_loc, gf::inverse(delta));
        err_loc = std::move(new_loc);
      }
      err_loc = gf::poly_add(err_loc, gf::poly_scale(old_loc, delta));
    }
  }

  while (!err_loc.empty() && err_loc.front() == 0) {
    err_loc.erase(err_loc.begin());
  }
  const std::size_t errors = err_loc.empty() ? 0 : err_loc.size() - 1;
  if (errors * 2 > static_cast<std::size_t>(parity_bytes)) {
    return {};
  }
  return err_loc;
}

/// Chien search over the reversed locator. Returns an empty vector when the
/// root count disagrees with the locator degree -- that mismatch means the
/// codeword is corrupted past the code's reach, not that the search failed.
std::vector<std::size_t> find_errors(const std::vector<uint8_t>& err_loc_reversed,
                                     std::size_t codeword_length) {
  const std::size_t expected = err_loc_reversed.empty() ? 0 : err_loc_reversed.size() - 1;
  std::vector<std::size_t> positions;
  for (std::size_t i = 0; i < codeword_length; ++i) {
    if (gf::poly_eval(err_loc_reversed, gf::pow(2, static_cast<int>(i))) == 0) {
      positions.push_back(codeword_length - 1 - i);
    }
  }
  if (positions.size() != expected) {
    return {};
  }
  return positions;
}

std::vector<uint8_t> find_errata_locator(const std::vector<std::size_t>& coef_positions) {
  std::vector<uint8_t> loc{1};
  for (const std::size_t position : coef_positions) {
    loc = gf::poly_mul(loc, gf::poly_add({1}, {gf::pow(2, static_cast<int>(position)), 0}));
  }
  return loc;
}

std::vector<uint8_t> find_error_evaluator(const std::vector<uint8_t>& synd_reversed,
                                          const std::vector<uint8_t>& err_loc,
                                          std::size_t degree) {
  std::vector<uint8_t> remainder = gf::poly_mul(synd_reversed, err_loc);
  const std::size_t keep = degree + 1;
  if (remainder.size() > keep) {
    remainder.erase(remainder.begin(),
                    remainder.begin() + static_cast<std::ptrdiff_t>(remainder.size() - keep));
  }
  return remainder;
}

/// Forney's algorithm: compute each error magnitude and XOR it in place.
Bytes correct_errata(const Bytes& codeword, const std::vector<uint8_t>& synd,
                     const std::vector<std::size_t>& err_pos) {
  std::vector<std::size_t> coef_pos;
  coef_pos.reserve(err_pos.size());
  for (const std::size_t p : err_pos) {
    coef_pos.push_back(codeword.size() - 1 - p);
  }

  const std::vector<uint8_t> err_loc = find_errata_locator(coef_pos);

  std::vector<uint8_t> synd_reversed(synd.rbegin(), synd.rend());
  std::vector<uint8_t> err_eval =
      find_error_evaluator(synd_reversed, err_loc, err_loc.size() - 1);
  std::reverse(err_eval.begin(), err_eval.end());

  std::vector<uint8_t> x;
  x.reserve(coef_pos.size());
  for (const std::size_t position : coef_pos) {
    x.push_back(gf::pow(2, -static_cast<int>(255 - position)));
  }

  Bytes magnitudes(codeword.size(), 0);
  std::vector<uint8_t> err_eval_reversed(err_eval.rbegin(), err_eval.rend());
  for (std::size_t i = 0; i < x.size(); ++i) {
    const uint8_t xi_inverse = gf::inverse(x[i]);
    uint8_t locator_derivative = 1;
    for (std::size_t j = 0; j < x.size(); ++j) {
      if (j != i) {
        locator_derivative =
            gf::mul(locator_derivative, static_cast<uint8_t>(1 ^ gf::mul(xi_inverse, x[j])));
      }
    }
    uint8_t y = gf::poly_eval(err_eval_reversed, xi_inverse);
    y = gf::mul(gf::pow(x[i], 1), y);
    magnitudes[err_pos[i]] = gf::div(y, locator_derivative);
  }

  Bytes out = codeword;
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<uint8_t>(out[i] ^ magnitudes[i]);
  }
  return out;
}

}  // namespace

ReedSolomonCodec::ReedSolomonCodec(int parity_bytes)
    : parity_bytes_(parity_bytes), generator_(generator_poly(parity_bytes)) {
  if (parity_bytes < 0 || parity_bytes > 254) {
    throw std::invalid_argument("parity_bytes must be 0..254");
  }
}

Bytes ReedSolomonCodec::encode(const Bytes& message) const {
  if (message.size() + static_cast<std::size_t>(parity_bytes_) > kMaxCodewordBytes) {
    throw std::invalid_argument(
        "RS codeword would exceed 255 bytes (message " + std::to_string(message.size()) +
        " + parity " + std::to_string(parity_bytes_) + ")");
  }
  Bytes out = message;
  out.resize(message.size() + static_cast<std::size_t>(parity_bytes_), 0);

  // Synthetic division by the generator, done with log-domain multiplies.
  for (std::size_t i = 0; i < message.size(); ++i) {
    const uint8_t coefficient = out[i];
    if (coefficient == 0) {
      continue;
    }
    for (std::size_t j = 1; j < generator_.size(); ++j) {
      out[i + j] = static_cast<uint8_t>(out[i + j] ^ gf::mul(coefficient, generator_[j]));
    }
  }
  std::copy(message.begin(), message.end(), out.begin());
  return out;
}

std::optional<ReedSolomonCodec::Decoded> ReedSolomonCodec::decode(const Bytes& codeword) const {
  if (codeword.size() > kMaxCodewordBytes ||
      codeword.size() <= static_cast<std::size_t>(parity_bytes_)) {
    return std::nullopt;
  }

  Bytes working = codeword;
  std::vector<uint8_t> synd = syndromes(working, parity_bytes_);
  const bool clean = std::all_of(synd.begin(), synd.end(), [](uint8_t v) { return v == 0; });
  if (clean) {
    working.resize(working.size() - static_cast<std::size_t>(parity_bytes_));
    return Decoded{std::move(working), 0};
  }

  // Forney syndromes degenerate to synd[1:] when there are no erasures, and
  // the modem never declares any.
  const std::vector<uint8_t> forney(synd.begin() + 1, synd.end());
  const std::vector<uint8_t> err_loc = find_error_locator(forney, parity_bytes_);
  if (err_loc.empty()) {
    return std::nullopt;
  }

  std::vector<uint8_t> err_loc_reversed(err_loc.rbegin(), err_loc.rend());
  const std::vector<std::size_t> err_pos = find_errors(err_loc_reversed, working.size());
  if (err_pos.empty()) {
    return std::nullopt;
  }

  working = correct_errata(working, synd, err_pos);

  synd = syndromes(working, parity_bytes_);
  const bool corrected =
      std::all_of(synd.begin(), synd.end(), [](uint8_t v) { return v == 0; });
  if (!corrected) {
    return std::nullopt;
  }
  working.resize(working.size() - static_cast<std::size_t>(parity_bytes_));
  return Decoded{std::move(working), static_cast<int>(err_pos.size())};
}

// ---- BlockCodec -----------------------------------------------------------

BlockCodec::BlockCodec(const BlockConfig& config)
    : config_(config), codec_(config.parity_bytes) {
  if (static_cast<std::size_t>(config.block_size()) > kMaxCodewordBytes) {
    throw std::invalid_argument(
        "block_size " + std::to_string(config.block_size()) +
        " exceeds the 255-byte RS codeword limit; lower rs_data_bytes or rs_parity_bytes");
  }
}

Bytes BlockCodec::encode(const Bytes& payload) const {
  if (payload.size() != static_cast<std::size_t>(config_.data_bytes)) {
    throw std::invalid_argument("payload must be exactly " +
                                std::to_string(config_.data_bytes) + " bytes, got " +
                                std::to_string(payload.size()));
  }
  Bytes message = payload;
  if (config_.crc_enabled) {
    const uint32_t crc = weaklink::crc32(payload);
    message.push_back(static_cast<uint8_t>((crc >> 24) & 0xFFu));
    message.push_back(static_cast<uint8_t>((crc >> 16) & 0xFFu));
    message.push_back(static_cast<uint8_t>((crc >> 8) & 0xFFu));
    message.push_back(static_cast<uint8_t>(crc & 0xFFu));
  }
  return codec_.encode(message);
}

std::optional<BlockCodec::Decoded> BlockCodec::decode(const Bytes& block) const {
  const auto decoded = codec_.decode(block);
  if (!decoded) {
    return std::nullopt;
  }
  if (!config_.crc_enabled) {
    return Decoded{decoded->message, decoded->errors_corrected};
  }
  if (decoded->message.size() < static_cast<std::size_t>(kCrcBytes)) {
    return std::nullopt;
  }
  Bytes payload(decoded->message.begin(),
                decoded->message.end() - kCrcBytes);
  uint32_t carried = 0;
  for (std::size_t i = decoded->message.size() - kCrcBytes; i < decoded->message.size(); ++i) {
    carried = (carried << 8) | decoded->message[i];
  }
  if (carried != weaklink::crc32(payload)) {
    return std::nullopt;
  }
  return Decoded{std::move(payload), decoded->errors_corrected};
}

}  // namespace weaklink::rs
