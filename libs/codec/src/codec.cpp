#include "weaklink/codec.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>

#include "preamble.hpp"
#include "weaklink/exceptions.hpp"
#include "weaklink/fec.hpp"

namespace weaklink::codec {
namespace {

void validate_data_bytes(int data_bytes) {
  if (data_bytes < static_cast<int>(kHeaderBytes) + 1) {
    throw ConfigError("rs_data_bytes must be >= " + std::to_string(kHeaderBytes + 1) +
                      " (header + 1 payload byte)");
  }
  if (data_bytes > 256) {
    throw ConfigError("rs_data_bytes must be <= 256 (length header is 1 byte)");
  }
}

/// NumPy's default quantile: linear interpolation between order statistics.
double quantile_linear(std::vector<double> values, double q) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const double position = q * static_cast<double>(values.size() - 1);
  const auto lower = static_cast<std::size_t>(std::floor(position));
  const auto upper = static_cast<std::size_t>(std::ceil(position));
  if (lower == upper) {
    return values[lower];
  }
  return values[lower] + (position - static_cast<double>(lower)) *
                             (values[upper] - values[lower]);
}

double median(std::vector<double> values) { return quantile_linear(std::move(values), 0.5); }

/// Copy K of block B uses a different permutation from copy K-1: seeds are
/// spread ``cycle_size / block_repeats`` apart so successive copies look
/// uncorrelated to the RX bit-error pattern. That is real diversity when
/// combining soft LLRs, not just retry-until-success.
std::size_t copy_seed_slot(std::size_t block_index, std::size_t copy_index,
                           int block_repeats) {
  const std::size_t cycle = interleaver::cycle_size();
  const std::size_t step = std::max<std::size_t>(1, cycle / static_cast<std::size_t>(
                                                       std::max(1, block_repeats)));
  return (block_index + copy_index * step) % cycle;
}

ByteVector frame_block(const ByteVector& chunk, int block_index, std::size_t payload_per_block) {
  ByteVector framed;
  framed.reserve(kHeaderBytes + payload_per_block);
  framed.push_back(static_cast<uint8_t>(chunk.size()));
  framed.push_back(static_cast<uint8_t>((block_index >> 8) & 0xFF));
  framed.push_back(static_cast<uint8_t>(block_index & 0xFF));
  framed.insert(framed.end(), chunk.begin(), chunk.end());
  framed.resize(kHeaderBytes + payload_per_block, 0);
  return framed;
}

std::vector<int> encode_one_block(const ByteVector& payload, const ModemConfig& config,
                                  std::size_t seed_slot) {
  const int bits_per_symbol = config.waveform().bits_per_symbol();
  const ByteVector rs_encoded = config.rs_codec().encode(payload);
  const BitVector payload_bits = bytes_to_bits_msb(rs_encoded);
  const BitVector coded = fec::encode(payload_bits);
  const BitVector interleaved =
      interleaver::interleave(coded, config.interleaver_config(), seed_slot);
  const BitVector padded =
      pad_to_multiple(interleaved, static_cast<std::size_t>(bits_per_symbol));
  return dsp::bits_to_symbols(padded, config.waveform().num_tones());
}

std::size_t coded_bits_for(const ModemConfig& config) {
  return 2 * (static_cast<std::size_t>(config.rs_codec().config().block_size()) * 8 +
              fec::kConstraintLength - 1);
}

}  // namespace

// ---- ModemConfig ----------------------------------------------------------

ModemConfig::ModemConfig(const Options& options) : options_(options) {
  if (options_.sync_every_blocks < 1) {
    throw ConfigError("sync_every_blocks must be >= 1");
  }
  if (options_.rs_data_bytes < 1) {
    throw ConfigError("rs_data_bytes must be >= 1");
  }
  if (options_.block_repeats < 1) {
    throw ConfigError("block_repeats must be >= 1");
  }
  rs::BlockConfig block_config;
  block_config.data_bytes = options_.rs_data_bytes;
  block_config.parity_bytes = options_.rs_parity_bytes;
  block_config.crc_enabled = options_.rs_crc_enabled;
  try {
    codec_ = std::make_shared<rs::BlockCodec>(block_config);
  } catch (const std::invalid_argument& error) {
    throw ConfigError(error.what());
  }
}

std::size_t ModemConfig::block_symbol_length() const {
  const auto bits_per_symbol = static_cast<std::size_t>(options_.waveform.bits_per_symbol());
  const std::size_t coded_bits = coded_bits_for(*this);
  const std::size_t interleaved =
      round_up_multiple(coded_bits, options_.interleaver.block_size());
  return round_up_multiple(interleaved, bits_per_symbol) / bits_per_symbol;
}

// ---- encode ---------------------------------------------------------------

void encode_stream(const ByteSource& source, const ModemConfig& config,
                   const SampleSink& sink) {
  const int data_bytes = config.rs_codec().config().data_bytes;
  validate_data_bytes(data_bytes);
  const auto payload_per_block = static_cast<std::size_t>(data_bytes) - kHeaderBytes;
  const std::vector<int>& preamble = preamble_for(config.waveform().num_tones());
  const Samples preamble_audio = dsp::modulate(preamble, config.waveform());

  const auto emit_block = [&](const ByteVector& chunk, int block_index) {
    const ByteVector framed = frame_block(chunk, block_index, payload_per_block);
    for (int copy = 0; copy < config.block_repeats(); ++copy) {
      const std::size_t seed =
          copy_seed_slot(static_cast<std::size_t>(block_index), static_cast<std::size_t>(copy),
                         config.block_repeats());
      std::vector<int> merged = preamble;
      const std::vector<int> block_symbols = encode_one_block(framed, config, seed);
      merged.insert(merged.end(), block_symbols.begin(), block_symbols.end());
      sink(dsp::modulate(merged, config.waveform()));
    }
  };

  ByteVector buffer;
  int block_index = 0;
  bool emitted_any = false;
  ByteVector chunk;
  while (true) {
    chunk.clear();
    if (!source(chunk)) {
      break;
    }
    buffer.insert(buffer.end(), chunk.begin(), chunk.end());
    while (buffer.size() >= payload_per_block) {
      if (block_index > kMaxBlockIndex) {
        throw EncodeError("stream too long: block_index exceeded " +
                          std::to_string(kMaxBlockIndex));
      }
      emit_block(ByteVector(buffer.begin(),
                            buffer.begin() + static_cast<std::ptrdiff_t>(payload_per_block)),
                 block_index);
      buffer.erase(buffer.begin(),
                   buffer.begin() + static_cast<std::ptrdiff_t>(payload_per_block));
      ++block_index;
      emitted_any = true;
    }
  }

  if (!buffer.empty() || !emitted_any) {
    if (block_index > kMaxBlockIndex) {
      throw EncodeError("stream too long: block_index exceeded " +
                        std::to_string(kMaxBlockIndex));
    }
    emit_block(buffer, block_index);
  }

  sink(preamble_audio);  // trailing marker
}

Samples encode(const ByteVector& input, const ModemConfig& config) {
  Samples out;
  bool consumed = false;
  encode_stream(
      [&](ByteVector& chunk) {
        if (consumed) {
          return false;
        }
        chunk = input;
        consumed = true;
        return true;
      },
      config,
      [&](const Samples& part) { out.insert(out.end(), part.begin(), part.end()); });
  return out;
}

// ---- preamble correlator --------------------------------------------------

std::vector<std::size_t> find_preamble_peaks(const dsp::Magnitudes& magnitudes,
                                             const std::vector<int>& preamble,
                                             const ModemConfig& config) {
  (void)config;
  const std::size_t preamble_length = preamble.size();
  if (magnitudes.rows() < preamble_length) {
    return {};
  }
  const std::size_t positions = magnitudes.rows() - preamble_length + 1;
  const std::size_t num_tones = magnitudes.cols();

  // Rolling window energy, used to make the score amplitude-invariant: a real
  // preamble lands near 1.0 whether the signal is loud or deep in a fade.
  std::vector<double> per_symbol_total(magnitudes.rows(), 0.0);
  for (std::size_t row = 0; row < magnitudes.rows(); ++row) {
    double total = 0.0;
    for (std::size_t tone = 0; tone < num_tones; ++tone) {
      total += magnitudes.at(row, tone);
    }
    per_symbol_total[row] = total;
  }
  std::vector<double> window_total(positions, 0.0);
  for (std::size_t start = 0; start < positions; ++start) {
    double total = 0.0;
    for (std::size_t i = 0; i < preamble_length; ++i) {
      total += per_symbol_total[start + i];
    }
    window_total[start] = total;
  }

  std::vector<double> raw(positions, 0.0);
  if (num_tones == 1) {
    // OOK: tone-symbol energy minus silence-symbol energy.
    for (std::size_t start = 0; start < positions; ++start) {
      double score = 0.0;
      for (std::size_t i = 0; i < preamble_length; ++i) {
        const double value = magnitudes.at(start + i, 0);
        score += preamble[i] == 1 ? value : -value;
      }
      raw[start] = score;
    }
  } else {
    // MFSK: sum the energy at the tone the preamble expects at each position.
    // The old per-tone inner loop simplifies to (M * wanted - total) / (M - 1).
    for (std::size_t start = 0; start < positions; ++start) {
      double wanted = 0.0;
      for (std::size_t i = 0; i < preamble_length; ++i) {
        wanted += magnitudes.at(start + i, static_cast<std::size_t>(preamble[i]));
      }
      raw[start] = (static_cast<double>(num_tones) * wanted - window_total[start]) /
                   static_cast<double>(num_tones - 1);
    }
  }

  std::vector<double> scores(positions, 0.0);
  for (std::size_t i = 0; i < positions; ++i) {
    scores[i] = window_total[i] > 1e-12 ? raw[i] / window_total[i] : 0.0;
  }
  if (scores.empty()) {
    return {};
  }
  const double peak_score = *std::max_element(scores.begin(), scores.end());

  std::vector<double> noise_pool;
  if (num_tones == 1) {
    // OOK scores are symmetric in [-1, 1]: an anti-correlated window looks as
    // different from a real peak as unrelated noise does. Use |score| to size
    // the noise cloud around zero and treat the top quartile as the
    // correlated tail.
    std::vector<double> absolute(scores.size());
    for (std::size_t i = 0; i < scores.size(); ++i) {
      absolute[i] = std::abs(scores[i]);
    }
    const double cutoff = quantile_linear(absolute, 0.75);
    for (std::size_t i = 0; i < scores.size(); ++i) {
      if (absolute[i] <= cutoff) {
        noise_pool.push_back(scores[i]);
      }
    }
  } else {
    // At M=2 sidelobes fill the lower half, so the quantile floors at 0.25;
    // higher M has tighter sidelobes and the lower half is fine.
    const double q = std::max(0.25, 1.0 - 2.0 / static_cast<double>(num_tones));
    const double cutoff = quantile_linear(scores, q);
    for (const double score : scores) {
      if (score <= cutoff) {
        noise_pool.push_back(score);
      }
    }
  }
  if (noise_pool.size() < 4) {
    return {};
  }

  const double noise_centre = median(noise_pool);
  std::vector<double> deviations(noise_pool.size());
  for (std::size_t i = 0; i < noise_pool.size(); ++i) {
    deviations[i] = std::abs(noise_pool[i] - noise_centre);
  }
  const double noise_sigma = std::max(2.0 * 1.4826 * median(deviations), 1e-9);

  // OOK's 2-symbol alphabet makes near-matches unavoidable, so the sigma gate
  // would reject valid peaks there; the sidelobe bound below is the only
  // filter that mode needs.
  if (num_tones > 1 && peak_score < noise_centre + 6.0 * noise_sigma) {
    return {};
  }

  // In the noise-only limit, scores at wrong alignments are ~Gaussian with
  // std 1/sqrt((M-1) * L), and the expected max of N samples is
  // std * sqrt(2 ln N). Scale by the observed peak, which amplitude
  // normalisation has put at ~1.0.
  const double position_count = static_cast<double>(std::max<std::size_t>(scores.size(), 2));
  const double gaussian_bound =
      std::sqrt(2.0 * std::log(position_count)) /
      std::sqrt(static_cast<double>(std::max<std::size_t>(1, num_tones - 1) * preamble_length));
  // Short alphabets also produce deterministic autocorrelation sidelobes above
  // that noise floor. Take whichever bound is larger.
  const double sidelobe_bound =
      std::max(gaussian_bound, detail::preamble_deterministic_sidelobe(static_cast<int>(num_tones)));

  const double threshold =
      num_tones == 1 ? sidelobe_bound * peak_score
                     : std::max(noise_centre + 5.0 * noise_sigma, sidelobe_bound * peak_score);

  // Noiseless OOK produces exact ties: inside the leading pilot several
  // alignments score exactly 1.0, and whichever one wins becomes the frame
  // anchor for everything after it. Ties break toward the later position,
  // which sits closer to the real frame start than a pilot artefact does.
  // (NumPy's argsort is unstable, so the reference implementation resolves
  // these ties arbitrarily rather than by any stated rule.)
  std::vector<std::size_t> order(scores.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    if (scores[a] != scores[b]) {
      return scores[a] > scores[b];
    }
    return a > b;
  });

  std::vector<std::size_t> peaks;
  std::vector<bool> taken(scores.size(), false);
  const std::size_t guard = preamble_length;
  for (const std::size_t candidate : order) {
    if (scores[candidate] < threshold) {
      break;
    }
    const std::size_t low = candidate >= guard ? candidate - guard : 0;
    const std::size_t high = std::min(scores.size(), candidate + guard + 1);
    bool overlaps = false;
    for (std::size_t i = low; i < high; ++i) {
      if (taken[i]) {
        overlaps = true;
        break;
      }
    }
    if (overlaps) {
      continue;
    }
    peaks.push_back(candidate);
    for (std::size_t i = low; i < high; ++i) {
      taken[i] = true;
    }
  }
  std::sort(peaks.begin(), peaks.end());
  return peaks;
}

}  // namespace weaklink::codec
