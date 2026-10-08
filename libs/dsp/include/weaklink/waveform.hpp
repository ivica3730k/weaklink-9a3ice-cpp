#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "weaklink/bits.hpp"

namespace weaklink::dsp {

using Samples = std::vector<float>;

/// Per-symbol tone magnitudes, row-major ``[symbol][tone]``.
class Magnitudes {
 public:
  Magnitudes() = default;
  Magnitudes(std::size_t rows, std::size_t cols) : rows_(rows), cols_(cols), data_(rows * cols, 0.0) {}

  std::size_t rows() const { return rows_; }
  std::size_t cols() const { return cols_; }
  bool empty() const { return rows_ == 0; }

  double& at(std::size_t row, std::size_t col) { return data_[row * cols_ + col]; }
  double at(std::size_t row, std::size_t col) const { return data_[row * cols_ + col]; }

  const double* row_data(std::size_t row) const { return data_.data() + row * cols_; }

  /// Rows ``[first, first + count)`` as a new block. Short tails are returned
  /// as-is rather than padded, so callers can detect truncation.
  Magnitudes slice(std::size_t first, std::size_t count) const;

  /// Zero rows prepended or appended -- used when the decoder projects a
  /// virtual preamble past the edge of the captured audio.
  static Magnitudes pad(const Magnitudes& source, std::size_t pad_rows, bool at_head);

 private:
  std::size_t rows_ = 0;
  std::size_t cols_ = 0;
  std::vector<double> data_;
};

/// MFSK waveform parameters.
///
/// A single tone is on air at any instant regardless of ``num_tones``:
/// ``num_tones`` is the alphabet size (log2 M bits per symbol), not a count of
/// simultaneous carriers. The envelope is constant for every M.
class WaveformConfig {
 public:
  /// Guardrail: no tone is allowed below this frequency.
  static constexpr double kMinToneHz = 500.0;

  /// 18 kHz is 5 * LCM(45, 300, 1200), so ``samples_per_symbol`` is an exact
  /// integer at every preset and no rounding drift accumulates.
  static constexpr double kDefaultSampleRate = 18000.0;

  WaveformConfig() : WaveformConfig(300.0, kDefaultSampleRate, 1500.0, 300.0, 0.25, 4) {}

  WaveformConfig(double baud, double sample_rate, double center_hz, double tone_spacing_hz,
                 double amplitude, int num_tones);

  double baud() const { return baud_; }
  double sample_rate() const { return sample_rate_; }
  double center_hz() const { return center_hz_; }
  double tone_spacing_hz() const { return tone_spacing_hz_; }
  double amplitude() const { return amplitude_; }
  int num_tones() const { return num_tones_; }
  const std::vector<double>& tones_hz() const { return tones_hz_; }

  int samples_per_symbol() const;

  /// Symbol-alphabet size: equals ``num_tones`` for MFSK, and 2 for OOK
  /// (``num_tones == 1``: symbol 0 is silence, symbol 1 is the carrier).
  int num_symbols() const;
  int bits_per_symbol() const;

 private:
  double baud_;
  double sample_rate_;
  double center_hz_;
  double tone_spacing_hz_;
  double amplitude_;
  int num_tones_;
  std::vector<double> tones_hz_;
};

/// Builder mirroring the Python dataclass's keyword defaults, so callers only
/// name the fields they actually change.
struct WaveformOptions {
  double baud = 300.0;
  double sample_rate = WaveformConfig::kDefaultSampleRate;
  double center_hz = 1500.0;
  double tone_spacing_hz = 300.0;
  double amplitude = 0.25;
  int num_tones = 4;

  WaveformConfig build() const {
    return WaveformConfig(baud, sample_rate, center_hz, tone_spacing_hz, amplitude, num_tones);
  }
};

int num_symbols_for(int num_tones);
int bits_per_symbol_for(int num_tones);

/// Pack a 0/1 bit stream into Gray-coded symbol indices. Throws unless the bit
/// count is a multiple of ``bits_per_symbol``.
std::vector<int> bits_to_symbols(const BitVector& bits, int num_tones);

/// Inverse of :func:`bits_to_symbols`.
BitVector symbols_to_bits(const std::vector<int>& symbols, int num_tones);

/// Continuous-phase MFSK, or on-off keying when ``num_tones == 1``. Phase runs
/// continuously across symbol boundaries (and across OOK's off symbols) so the
/// spectrum stays narrow and no boundary clicks appear.
Samples modulate(const std::vector<int>& symbols, const WaveformConfig& config);

/// Non-coherent I/Q demodulation. Returns squared magnitudes per symbol and
/// tone. ``frequency_offset_hz`` shifts the reference tones.
Magnitudes demodulate_soft(const float* samples, std::size_t count,
                           const WaveformConfig& config, double frequency_offset_hz = 0.0);
Magnitudes demodulate_soft(const Samples& samples, const WaveformConfig& config,
                           double frequency_offset_hz = 0.0);

/// Coarse LO offset via FFT with geometric-mean scoring across the tone slots.
/// A geometric mean forces energy at *every* slot; a plain sum lets one
/// dominant peak drag the estimate.
double estimate_coarse_frequency_offset(const float* samples, std::size_t count,
                                        const WaveformConfig& config,
                                        double search_range_hz = 1500.0);

/// Fine offset from a known symbol sequence (in practice, the preamble).
/// Sweeps ``prior_offset_hz +- search_range_hz`` and maximises the energy at
/// the expected tone of each symbol.
double estimate_frequency_offset(const float* samples, std::size_t count,
                                 const WaveformConfig& config,
                                 const std::vector<int>& expected_symbols,
                                 double search_range_hz = 50.0,
                                 double resolution_hz = 1.0,
                                 double prior_offset_hz = 0.0);

/// Per-bit soft LLR-shaped values from tone magnitudes, max-log-MAP.
/// Positive means the bit is more likely 0.
std::vector<double> soft_bits_from_magnitudes(const Magnitudes& magnitudes, int num_tones);

}  // namespace weaklink::dsp
