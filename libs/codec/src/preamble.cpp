#include "preamble.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <vector>

#include "weaklink/codec.hpp"

namespace weaklink::codec {
namespace {

/// 16-bit LFSR, taps at 15/13/12/10. The seed is fixed so TX and RX derive the
/// same sequence.
std::vector<int> generate_preamble(std::size_t length, int num_tones, uint16_t seed) {
  const int bits_per_symbol = dsp::bits_per_symbol_for(num_tones);
  const int mask = dsp::num_symbols_for(num_tones) - 1;
  uint16_t state = seed;
  if (state == 0) {
    state = 1;
  }
  std::vector<int> symbols;
  symbols.reserve(length);
  for (std::size_t i = 0; i < length; ++i) {
    int accumulator = 0;
    for (int b = 0; b < bits_per_symbol; ++b) {
      const int bit = state & 1;
      const int feedback =
          ((state >> 15) ^ (state >> 13) ^ (state >> 12) ^ (state >> 10)) & 1;
      state = static_cast<uint16_t>((state >> 1) | (feedback << 15));
      accumulator = (accumulator << 1) | bit;
    }
    symbols.push_back(accumulator & mask);
  }
  return symbols;
}

}  // namespace

const std::vector<int>& preamble_for(int num_tones) {
  static std::mutex mutex;
  static std::map<int, std::vector<int>> cache;
  std::lock_guard<std::mutex> guard(mutex);
  const auto found = cache.find(num_tones);
  if (found != cache.end()) {
    return found->second;
  }
  return cache
      .emplace(num_tones, generate_preamble(kPreambleLengthSymbols, num_tones, 0xC05A))
      .first->second;
}

namespace detail {

double preamble_deterministic_sidelobe(int num_tones) {
  static std::mutex mutex;
  static std::map<int, double> cache;
  {
    std::lock_guard<std::mutex> guard(mutex);
    const auto found = cache.find(num_tones);
    if (found != cache.end()) {
      return found->second;
    }
  }

  const std::vector<int>& preamble = preamble_for(num_tones);
  const auto length = static_cast<std::ptrdiff_t>(preamble.size());
  double max_score = 0.0;

  if (num_tones == 1) {
    // OOK correlator: score = (tone matches - silence matches) / total.
    for (std::ptrdiff_t shift = 1; shift < length; ++shift) {
      const std::ptrdiff_t overlap = length - shift;
      double numerator = 0.0;
      double denominator = 0.0;
      for (std::ptrdiff_t i = 0; i < overlap; ++i) {
        const double shifted = static_cast<double>(preamble[static_cast<std::size_t>(shift + i)]);
        const double tone = preamble[static_cast<std::size_t>(i)] == 1 ? 1.0 : 0.0;
        const double silence = preamble[static_cast<std::size_t>(i)] == 0 ? 1.0 : 0.0;
        numerator += shifted * tone - shifted * silence;
        denominator += shifted;
      }
      double outside_tone = 0.0;
      double outside_silence = 0.0;
      for (std::ptrdiff_t i = overlap; i < length; ++i) {
        outside_tone += preamble[static_cast<std::size_t>(i)] == 1 ? 1.0 : 0.0;
        outside_silence += preamble[static_cast<std::size_t>(i)] == 0 ? 1.0 : 0.0;
      }
      // Outside the overlap, random data sits at mean amplitude 0.5.
      numerator += 0.5 * (outside_tone - outside_silence);
      denominator += 0.5 * (outside_tone + outside_silence);
      if (denominator > 0.0) {
        max_score = std::max(max_score, std::abs(numerator / denominator));
      }
    }
  } else {
    const auto tones = static_cast<std::size_t>(num_tones);
    for (std::ptrdiff_t shift = 1; shift < length; ++shift) {
      const std::ptrdiff_t overlap = length - shift;
      std::vector<double> magnitudes(static_cast<std::size_t>(length) * tones, 0.0);
      for (std::ptrdiff_t i = 0; i < overlap; ++i) {
        magnitudes[static_cast<std::size_t>(i) * tones +
                   static_cast<std::size_t>(preamble[static_cast<std::size_t>(shift + i)])] = 1.0;
      }
      for (std::ptrdiff_t i = overlap; i < length; ++i) {
        for (std::size_t tone = 0; tone < tones; ++tone) {
          magnitudes[static_cast<std::size_t>(i) * tones + tone] = 1.0 / num_tones;
        }
      }
      double wanted = 0.0;
      double total = 0.0;
      for (std::ptrdiff_t i = 0; i < length; ++i) {
        wanted += magnitudes[static_cast<std::size_t>(i) * tones +
                             static_cast<std::size_t>(preamble[static_cast<std::size_t>(i)])];
        for (std::size_t tone = 0; tone < tones; ++tone) {
          total += magnitudes[static_cast<std::size_t>(i) * tones + tone];
        }
      }
      const double raw = (num_tones * wanted - total) / (num_tones - 1);
      if (total > 0.0) {
        max_score = std::max(max_score, std::abs(raw / total));
      }
    }
  }

  std::lock_guard<std::mutex> guard(mutex);
  cache[num_tones] = max_score;
  return max_score;
}

}  // namespace detail
}  // namespace weaklink::codec
