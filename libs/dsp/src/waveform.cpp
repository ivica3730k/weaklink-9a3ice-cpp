#include "weaklink/waveform.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>

#include "pocketfft_hdronly.h"
#include "weaklink/exceptions.hpp"

namespace weaklink::dsp {
namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

bool is_power_of_two(int value) { return value >= 2 && (value & (value - 1)) == 0; }

void require_valid_tone_count(int num_tones) {
  if (num_tones != 1 && !is_power_of_two(num_tones)) {
    throw ConfigError("num_tones must be 1 or a power of 2 >= 2, got " +
                      std::to_string(num_tones));
  }
}

struct GrayTables {
  std::vector<int> bits_to_symbol;             ///< binary index -> Gray symbol
  std::vector<std::vector<uint8_t>> symbol_to_bits;  ///< Gray symbol -> source bits
};

/// ``symbol_to_bits`` is indexed by the *received* symbol and stores the bits
/// of the binary index that produced it, which is what the transmitter packed.
const GrayTables& gray_tables(int num_tones) {
  static std::mutex mutex;
  static std::map<int, GrayTables> cache;

  require_valid_tone_count(num_tones);
  std::lock_guard<std::mutex> guard(mutex);
  const auto found = cache.find(num_tones);
  if (found != cache.end()) {
    return found->second;
  }

  const int symbols = num_symbols_for(num_tones);
  const int bits = bits_per_symbol_for(num_tones);
  GrayTables tables;
  tables.bits_to_symbol.resize(static_cast<std::size_t>(symbols));
  tables.symbol_to_bits.assign(static_cast<std::size_t>(symbols),
                               std::vector<uint8_t>(static_cast<std::size_t>(bits), 0));
  for (int i = 0; i < symbols; ++i) {
    const int gray = i ^ (i >> 1);
    tables.bits_to_symbol[static_cast<std::size_t>(i)] = gray;
    for (int b = 0; b < bits; ++b) {
      tables.symbol_to_bits[static_cast<std::size_t>(gray)][static_cast<std::size_t>(b)] =
          static_cast<uint8_t>((i >> (bits - 1 - b)) & 1);
    }
  }
  return cache.emplace(num_tones, std::move(tables)).first->second;
}

/// NumPy's default percentile: linear interpolation between order statistics.
double percentile_linear(std::vector<double> values, double percent) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const double position = (percent / 100.0) * static_cast<double>(values.size() - 1);
  const auto lower = static_cast<std::size_t>(std::floor(position));
  const auto upper = static_cast<std::size_t>(std::ceil(position));
  if (lower == upper) {
    return values[lower];
  }
  const double fraction = position - static_cast<double>(lower);
  return values[lower] + fraction * (values[upper] - values[lower]);
}

}  // namespace

// ---- Magnitudes -----------------------------------------------------------

Magnitudes Magnitudes::slice(std::size_t first, std::size_t count) const {
  const std::size_t start = std::min(first, rows_);
  const std::size_t available = std::min(count, rows_ - start);
  Magnitudes out(available, cols_);
  for (std::size_t row = 0; row < available; ++row) {
    for (std::size_t col = 0; col < cols_; ++col) {
      out.at(row, col) = at(start + row, col);
    }
  }
  return out;
}

Magnitudes Magnitudes::pad(const Magnitudes& source, std::size_t pad_rows, bool at_head) {
  Magnitudes out(source.rows() + pad_rows, source.cols());
  const std::size_t offset = at_head ? pad_rows : 0;
  for (std::size_t row = 0; row < source.rows(); ++row) {
    for (std::size_t col = 0; col < source.cols(); ++col) {
      out.at(offset + row, col) = source.at(row, col);
    }
  }
  return out;
}

// ---- WaveformConfig -------------------------------------------------------

int num_symbols_for(int num_tones) { return num_tones == 1 ? 2 : num_tones; }

int bits_per_symbol_for(int num_tones) {
  int symbols = num_symbols_for(num_tones);
  int bits = 0;
  while (symbols > 1) {
    symbols >>= 1;
    ++bits;
  }
  return bits;
}

WaveformConfig::WaveformConfig(double baud, double sample_rate, double center_hz,
                               double tone_spacing_hz, double amplitude, int num_tones)
    : baud_(baud),
      sample_rate_(sample_rate),
      center_hz_(center_hz),
      tone_spacing_hz_(tone_spacing_hz),
      amplitude_(amplitude),
      num_tones_(num_tones) {
  require_valid_tone_count(num_tones);

  // OOK sits on a single carrier at the centre; MFSK spreads symmetrically
  // around it.
  std::vector<double> offsets;
  if (num_tones_ == 1) {
    offsets.push_back(0.0);
  } else {
    const double mean_offset = (num_tones_ - 1) / 2.0;
    offsets.reserve(static_cast<std::size_t>(num_tones_));
    for (int i = 0; i < num_tones_; ++i) {
      offsets.push_back((static_cast<double>(i) - mean_offset) * tone_spacing_hz_);
    }
  }

  const double lowest = center_hz_ + *std::min_element(offsets.begin(), offsets.end());
  if (lowest < kMinToneHz) {
    center_hz_ += kMinToneHz - lowest;
  }

  tones_hz_.reserve(offsets.size());
  for (const double offset : offsets) {
    tones_hz_.push_back(center_hz_ + offset);
  }

  if (samples_per_symbol() < 8) {
    throw ConfigError("sample_rate / baud must be >= 8 samples per symbol");
  }
  const double nyquist = sample_rate_ / 2.0;
  const double highest = *std::max_element(tones_hz_.begin(), tones_hz_.end());
  if (highest >= nyquist) {
    char message[256];
    std::snprintf(message, sizeof(message),
                  "top tone %.0f Hz exceeds Nyquist (%.0f Hz) -- num_tones=%d at %g baud "
                  "needs more bandwidth than the sample rate provides",
                  highest, nyquist, num_tones_, baud_);
    throw NyquistError(message);
  }
}

int WaveformConfig::samples_per_symbol() const {
  return static_cast<int>(std::lround(sample_rate_ / baud_));
}

int WaveformConfig::num_symbols() const { return num_symbols_for(num_tones_); }

int WaveformConfig::bits_per_symbol() const { return bits_per_symbol_for(num_tones_); }

// ---- symbol mapping -------------------------------------------------------

std::vector<int> bits_to_symbols(const BitVector& bits, int num_tones) {
  const int bits_per_symbol = bits_per_symbol_for(num_tones);
  if (bits.size() % static_cast<std::size_t>(bits_per_symbol) != 0) {
    throw std::invalid_argument("bit count " + std::to_string(bits.size()) +
                                " not a multiple of " + std::to_string(bits_per_symbol));
  }
  const GrayTables& tables = gray_tables(num_tones);
  std::vector<int> symbols(bits.size() / static_cast<std::size_t>(bits_per_symbol));
  for (std::size_t i = 0; i < symbols.size(); ++i) {
    int key = 0;
    for (int b = 0; b < bits_per_symbol; ++b) {
      // Big-endian packing: the first bit of the group is the most significant.
      key = (key << 1) | (bits[i * static_cast<std::size_t>(bits_per_symbol) +
                               static_cast<std::size_t>(b)] & 1);
    }
    symbols[i] = tables.bits_to_symbol[static_cast<std::size_t>(key)];
  }
  return symbols;
}

BitVector symbols_to_bits(const std::vector<int>& symbols, int num_tones) {
  const GrayTables& tables = gray_tables(num_tones);
  const int bits_per_symbol = bits_per_symbol_for(num_tones);
  BitVector bits;
  bits.reserve(symbols.size() * static_cast<std::size_t>(bits_per_symbol));
  for (const int symbol : symbols) {
    const auto& group = tables.symbol_to_bits[static_cast<std::size_t>(symbol)];
    bits.insert(bits.end(), group.begin(), group.end());
  }
  return bits;
}

// ---- modulation -----------------------------------------------------------

Samples modulate(const std::vector<int>& symbols, const WaveformConfig& config) {
  if (symbols.empty()) {
    return {};
  }
  const auto samples_per_symbol = static_cast<std::size_t>(config.samples_per_symbol());
  const double dt = 1.0 / config.sample_rate();
  Samples out(symbols.size() * samples_per_symbol);

  if (config.num_tones() == 1) {
    // OOK. The carrier phase keeps running through off symbols so on symbols
    // never start with a phase discontinuity.
    const double omega = kTwoPi * config.tones_hz()[0] * dt;
    for (std::size_t i = 0; i < symbols.size(); ++i) {
      const double gate = static_cast<double>(symbols[i]);
      const double start_index = static_cast<double>(i * samples_per_symbol);
      for (std::size_t n = 0; n < samples_per_symbol; ++n) {
        const double phase = omega * (start_index + static_cast<double>(n) + 1.0);
        out[i * samples_per_symbol + n] =
            static_cast<float>(config.amplitude() * gate * std::sin(phase));
      }
    }
    return out;
  }

  // Standard MFSK: per-symbol frequency with phase carried across boundaries.
  double start_phase = 0.0;
  for (std::size_t i = 0; i < symbols.size(); ++i) {
    const double omega = kTwoPi * config.tones_hz()[static_cast<std::size_t>(symbols[i])] * dt;
    for (std::size_t n = 0; n < samples_per_symbol; ++n) {
      const double phase = start_phase + omega * (static_cast<double>(n) + 1.0);
      out[i * samples_per_symbol + n] =
          static_cast<float>(config.amplitude() * std::sin(phase));
    }
    start_phase += omega * static_cast<double>(samples_per_symbol);
  }
  return out;
}

// ---- demodulation ---------------------------------------------------------

Magnitudes demodulate_soft(const float* samples, std::size_t count,
                           const WaveformConfig& config, double frequency_offset_hz) {
  const auto samples_per_symbol = static_cast<std::size_t>(config.samples_per_symbol());
  const std::size_t num_symbols = count / samples_per_symbol;
  const auto num_tones = static_cast<std::size_t>(config.num_tones());
  if (num_symbols == 0) {
    return Magnitudes(0, num_tones);
  }

  Magnitudes magnitudes(num_symbols, num_tones);
  std::vector<double> cosine(samples_per_symbol);
  std::vector<double> sine(samples_per_symbol);

  for (std::size_t tone = 0; tone < num_tones; ++tone) {
    const double frequency = config.tones_hz()[tone] + frequency_offset_hz;
    for (std::size_t n = 0; n < samples_per_symbol; ++n) {
      const double angle =
          kTwoPi * frequency * (static_cast<double>(n) / config.sample_rate());
      cosine[n] = std::cos(angle);
      sine[n] = std::sin(angle);
    }
    for (std::size_t symbol = 0; symbol < num_symbols; ++symbol) {
      const float* window = samples + symbol * samples_per_symbol;
      double in_phase = 0.0;
      double quadrature = 0.0;
      for (std::size_t n = 0; n < samples_per_symbol; ++n) {
        const double value = static_cast<double>(window[n]);
        in_phase += value * cosine[n];
        quadrature += value * sine[n];
      }
      magnitudes.at(symbol, tone) = in_phase * in_phase + quadrature * quadrature;
    }
  }
  return magnitudes;
}

Magnitudes demodulate_soft(const Samples& samples, const WaveformConfig& config,
                           double frequency_offset_hz) {
  return demodulate_soft(samples.data(), samples.size(), config, frequency_offset_hz);
}

// ---- frequency offset estimation ------------------------------------------

double estimate_coarse_frequency_offset(const float* samples, std::size_t count,
                                        const WaveformConfig& config,
                                        double search_range_hz) {
  if (count == 0) {
    return 0.0;
  }
  std::vector<double> input(count);
  for (std::size_t i = 0; i < count; ++i) {
    input[i] = static_cast<double>(samples[i]);
  }

  const std::size_t spectrum_size = count / 2 + 1;
  std::vector<std::complex<double>> spectrum(spectrum_size);
  const pocketfft::shape_t shape{count};
  const pocketfft::stride_t stride_in{static_cast<std::ptrdiff_t>(sizeof(double))};
  const pocketfft::stride_t stride_out{
      static_cast<std::ptrdiff_t>(sizeof(std::complex<double>))};
  pocketfft::r2c(shape, stride_in, stride_out, pocketfft::shape_t{0}, pocketfft::FORWARD,
                 input.data(), spectrum.data(), 1.0);

  std::vector<double> magnitude(spectrum_size);
  for (std::size_t i = 0; i < spectrum_size; ++i) {
    magnitude[i] = std::abs(spectrum[i]);
  }

  const double bin_hz = config.sample_rate() / static_cast<double>(count);
  const double step_hz = std::max(bin_hz, 2.0);
  // +-10 Hz of smoothing absorbs the CPFSK spectral smear.
  const auto window_bins = static_cast<std::ptrdiff_t>(
      std::max<long>(1, std::lround(10.0 / bin_hz)));

  const auto tone_energy = [&](std::ptrdiff_t bin) {
    const std::ptrdiff_t low = std::max<std::ptrdiff_t>(0, bin - window_bins);
    const std::ptrdiff_t high =
        std::min<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(spectrum_size), bin + window_bins + 1);
    double total = 0.0;
    for (std::ptrdiff_t i = low; i < high; ++i) {
      total += magnitude[static_cast<std::size_t>(i)];
    }
    return total;
  };

  double best_offset = 0.0;
  double best_score = -std::numeric_limits<double>::infinity();
  for (double offset = -search_range_hz; offset <= search_range_hz + step_hz * 0.5;
       offset += step_hz) {
    double log_score = 0.0;
    for (const double tone : config.tones_hz()) {
      const auto bin = static_cast<std::ptrdiff_t>(std::lround((tone + offset) / bin_hz));
      log_score += std::log(tone_energy(bin) + 1e-12);
    }
    if (log_score > best_score) {
      best_score = log_score;
      best_offset = offset;
    }
  }
  return best_offset;
}

double estimate_frequency_offset(const float* samples, std::size_t count,
                                 const WaveformConfig& config,
                                 const std::vector<int>& expected_symbols,
                                 double search_range_hz, double resolution_hz,
                                 double prior_offset_hz) {
  const auto samples_per_symbol = static_cast<std::size_t>(config.samples_per_symbol());
  const std::size_t expected_length = expected_symbols.size() * samples_per_symbol;
  if (count < expected_length) {
    return prior_offset_hz;
  }

  const bool is_ook = config.num_tones() == 1;
  std::vector<double> cosine(samples_per_symbol);
  std::vector<double> sine(samples_per_symbol);

  double best_offset = prior_offset_hz;
  double best_score = -std::numeric_limits<double>::infinity();
  for (double offset = prior_offset_hz - search_range_hz;
       offset <= prior_offset_hz + search_range_hz + resolution_hz * 0.5;
       offset += resolution_hz) {
    double total_energy = 0.0;
    for (std::size_t position = 0; position < expected_symbols.size(); ++position) {
      const int symbol = expected_symbols[position];
      // OOK silence symbols carry no reference frequency to align to.
      if (is_ook && symbol == 0) {
        continue;
      }
      const std::size_t tone_index = is_ook ? 0 : static_cast<std::size_t>(symbol);
      const double frequency = config.tones_hz()[tone_index] + offset;
      double in_phase = 0.0;
      double quadrature = 0.0;
      const float* window = samples + position * samples_per_symbol;
      for (std::size_t n = 0; n < samples_per_symbol; ++n) {
        const double angle =
            kTwoPi * frequency * (static_cast<double>(n) / config.sample_rate());
        const double value = static_cast<double>(window[n]);
        in_phase += value * std::cos(angle);
        quadrature += value * std::sin(angle);
      }
      total_energy += in_phase * in_phase + quadrature * quadrature;
    }
    if (total_energy > best_score) {
      best_score = total_energy;
      best_offset = offset;
    }
  }
  return best_offset;
}

// ---- soft bits ------------------------------------------------------------

std::vector<double> soft_bits_from_magnitudes(const Magnitudes& magnitudes, int num_tones) {
  if (magnitudes.rows() == 0) {
    return {};
  }

  if (num_tones == 1) {
    // OOK has a single magnitude column, so there is no competing tone to
    // compare against. Estimate the noise floor from the lowest quintile and
    // the tone level from the highest, then threshold at the midpoint: that
    // survives a block whose bit distribution is skewed (lots of zero padding,
    // say), which a plain median threshold would collapse.
    std::vector<double> column(magnitudes.rows());
    for (std::size_t row = 0; row < magnitudes.rows(); ++row) {
      column[row] = magnitudes.at(row, 0);
    }
    const double noise = percentile_linear(column, 20.0);
    const double tone = percentile_linear(column, 80.0);
    const double threshold = 0.5 * (noise + tone);
    std::vector<double> llrs(column.size());
    for (std::size_t i = 0; i < column.size(); ++i) {
      // Positive for silence (bit 0), negative for tone (bit 1).
      llrs[i] = threshold - column[i];
    }
    return llrs;
  }

  const GrayTables& tables = gray_tables(num_tones);
  const int bits_per_symbol = bits_per_symbol_for(num_tones);
  std::vector<double> llrs(magnitudes.rows() * static_cast<std::size_t>(bits_per_symbol));

  for (int bit_position = 0; bit_position < bits_per_symbol; ++bit_position) {
    std::vector<std::size_t> zero_symbols;
    std::vector<std::size_t> one_symbols;
    for (std::size_t symbol = 0; symbol < tables.symbol_to_bits.size(); ++symbol) {
      if (tables.symbol_to_bits[symbol][static_cast<std::size_t>(bit_position)] == 0) {
        zero_symbols.push_back(symbol);
      } else {
        one_symbols.push_back(symbol);
      }
    }
    for (std::size_t row = 0; row < magnitudes.rows(); ++row) {
      double max_zero = -std::numeric_limits<double>::infinity();
      for (const std::size_t symbol : zero_symbols) {
        max_zero = std::max(max_zero, magnitudes.at(row, symbol));
      }
      double max_one = -std::numeric_limits<double>::infinity();
      for (const std::size_t symbol : one_symbols) {
        max_one = std::max(max_one, magnitudes.at(row, symbol));
      }
      llrs[row * static_cast<std::size_t>(bits_per_symbol) +
           static_cast<std::size_t>(bit_position)] = max_zero - max_one;
    }
  }
  return llrs;
}

}  // namespace weaklink::dsp
