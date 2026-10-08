#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <vector>

#include "preamble.hpp"
#include "weaklink/codec.hpp"
#include "weaklink/fec.hpp"
#include "weaklink/log.hpp"

namespace weaklink::codec {
namespace {

Logger& log() {
  static Logger logger("weaklink.decode");
  return logger;
}

std::size_t coded_bits_for(const ModemConfig& config) {
  return 2 * (static_cast<std::size_t>(config.rs_codec().config().block_size()) * 8 +
              fec::kConstraintLength - 1);
}

std::vector<std::size_t> radiating_order(int expected_block_index) {
  const std::size_t cycle = interleaver::cycle_size();
  const auto expected = static_cast<std::size_t>(
      ((expected_block_index % static_cast<int>(cycle)) + static_cast<int>(cycle)) %
      static_cast<int>(cycle));
  std::vector<std::size_t> order{expected};
  for (std::size_t delta = 1; delta < cycle; ++delta) {
    order.push_back((expected + delta) % cycle);
    if (order.size() >= cycle) {
      break;
    }
    order.push_back((expected + cycle - delta) % cycle);
  }
  return order;
}

bool valid_seed_for_block(std::size_t found_slot, int block_index, int block_repeats) {
  const std::size_t cycle = interleaver::cycle_size();
  const std::size_t step =
      std::max<std::size_t>(1, cycle / static_cast<std::size_t>(std::max(1, block_repeats)));
  for (int copy = 0; copy < block_repeats; ++copy) {
    if ((static_cast<std::size_t>(block_index) + static_cast<std::size_t>(copy) * step) % cycle ==
        found_slot) {
      return true;
    }
  }
  return false;
}

struct SlotDecode {
  ByteVector payload;
  int errors_corrected = 0;
  std::size_t seed_slot = 0;
  bool ok = false;
};

/// Decode one block from soft LLRs using the permutation identified by
/// ``seed_slot``.
std::optional<rs::BlockCodec::Decoded> decode_one_block(const std::vector<double>& soft_bits,
                                                        const ModemConfig& config,
                                                        std::size_t seed_slot) {
  const std::size_t coded_bits = coded_bits_for(config);
  if (soft_bits.size() < coded_bits) {
    return std::nullopt;
  }
  const std::vector<double> deinterleaved = interleaver::deinterleave_soft(
      soft_bits, config.interleaver_config(), coded_bits, seed_slot);
  const BitVector payload_bits = fec::decode(
      deinterleaved, static_cast<std::size_t>(config.rs_codec().config().block_size()) * 8);
  return config.rs_codec().decode(bits_to_bytes_msb(payload_bits));
}

/// Try each permutation slot (expected first, then radiating outward) until
/// RS+CRC clears. Only ``cycle_size`` distinct permutations exist, so the
/// worst case is a fixed bound covering every possible bit ordering.
SlotDecode decode_slot_with_seed_search(const std::vector<double>& soft_bits,
                                        const ModemConfig& config, int expected_block_index) {
  for (const std::size_t seed_slot : radiating_order(expected_block_index)) {
    const auto decoded = decode_one_block(soft_bits, config, seed_slot);
    if (decoded) {
      return SlotDecode{decoded->payload, decoded->errors_corrected, seed_slot, true};
    }
  }
  return SlotDecode{};
}

/// Soft-LLR combine back-to-back copies of the same block, then Viterbi + RS +
/// CRC. Each copy is deinterleaved with its own per-copy seed before summing;
/// the block's base seed is brute-forced because the copies may not decode
/// individually.
SlotDecode decode_combined_copies(const std::vector<std::vector<double>>& copies,
                                  const ModemConfig& config, int expected_block_index) {
  const std::size_t coded_bits = coded_bits_for(config);
  for (const auto& copy : copies) {
    if (copy.size() < coded_bits) {
      return SlotDecode{};
    }
  }
  const std::size_t cycle = interleaver::cycle_size();
  const std::size_t step = std::max<std::size_t>(
      1, cycle / static_cast<std::size_t>(std::max(1, config.block_repeats())));

  for (const std::size_t candidate : radiating_order(expected_block_index)) {
    std::vector<double> combined(coded_bits, 0.0);
    for (std::size_t copy_index = 0; copy_index < copies.size(); ++copy_index) {
      const std::size_t seed = (candidate + copy_index * step) % cycle;
      const std::vector<double> part = interleaver::deinterleave_soft(
          copies[copy_index], config.interleaver_config(), coded_bits, seed);
      for (std::size_t i = 0; i < coded_bits; ++i) {
        combined[i] += part[i];
      }
    }
    const BitVector payload_bits = fec::decode(
        combined, static_cast<std::size_t>(config.rs_codec().config().block_size()) * 8);
    const auto decoded = config.rs_codec().decode(bits_to_bytes_msb(payload_bits));
    if (!decoded || decoded->payload.size() < kHeaderBytes) {
      continue;
    }
    const int header_block_index =
        (static_cast<int>(decoded->payload[1]) << 8) | static_cast<int>(decoded->payload[2]);
    // ``candidate`` is copy 0's seed, i.e. the block-index base; the header
    // must agree with it modulo the cycle.
    if (static_cast<std::size_t>(header_block_index) % cycle == candidate) {
      return SlotDecode{decoded->payload, decoded->errors_corrected, candidate, true};
    }
  }
  return SlotDecode{};
}

/// Strip the slot header and the NUL padding the encoder added.
ByteVector block_content(const ByteVector& decoded) {
  std::size_t length = decoded[0];
  const std::size_t payload_area = decoded.size() - kHeaderBytes;
  length = std::min(length, payload_area);
  return ByteVector(decoded.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes),
                    decoded.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes + length));
}

double to_db(double value) {
  return value > 0.0 ? 20.0 * std::log10(value) : -std::numeric_limits<double>::infinity();
}

}  // namespace

DecodeResult decode(const float* samples, std::size_t count, const ModemConfig& config,
                    bool streaming, StreamingState* state) {
  if (count == 0) {
    log().debug("empty sample buffer; nothing to decode");
    return {};
  }
  const auto samples_per_symbol = static_cast<std::size_t>(config.waveform().samples_per_symbol());

  if (Logger::enabled(LogLevel::kDebug)) {
    double peak = 0.0;
    double sum_squares = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
      const double value = static_cast<double>(samples[i]);
      peak = std::max(peak, std::abs(value));
      sum_squares += value * value;
    }
    const double rms = std::sqrt(sum_squares / static_cast<double>(count));
    log().debug("input: ", count, " samples, ",
                static_cast<double>(count) / config.waveform().sample_rate(), " s, peak ", peak,
                " (", to_db(peak), " dBFS), rms ", rms, " (", to_db(rms), " dBFS)");
    if (to_db(peak) < -40.0) {
      log().debug("peak level below -40 dBFS");
    }
    if (to_db(rms) < -60.0) {
      log().debug("rms level below -60 dBFS");
    }
  }

  // Coarse LO offset via FFT. Cached across streaming calls, but only trusted
  // once a call has actually found preambles: a first call on silence would
  // otherwise latch a noise-floor peak and never recover.
  double coarse_offset = 0.0;
  bool fresh_estimate = false;
  const bool have_cached = streaming && state && state->coarse_offset_hz.has_value();
  if (have_cached) {
    coarse_offset = *state->coarse_offset_hz;
  } else if (config.coarse_frequency_search_hz() > 0.0) {
    coarse_offset = dsp::estimate_coarse_frequency_offset(samples, count, config.waveform(),
                                                          config.coarse_frequency_search_hz());
    fresh_estimate = true;
  }
  log().debug("coarse frequency offset: ", coarse_offset, " Hz");

  dsp::Magnitudes coarse_magnitudes =
      dsp::demodulate_soft(samples, count, config.waveform(), coarse_offset);
  if (coarse_magnitudes.rows() == 0) {
    log().debug("demodulator returned no symbols; sample count below one symbol");
    return {};
  }

  const std::vector<int>& preamble = preamble_for(config.waveform().num_tones());
  std::vector<std::size_t> peaks = find_preamble_peaks(coarse_magnitudes, preamble, config);
  if (Logger::enabled(LogLevel::kDebug)) {
    std::string offsets;
    for (std::size_t i = 0; i < peaks.size() && i < 8; ++i) {
      offsets += (i == 0 ? "" : ", ") + std::to_string(peaks[i]);
    }
    log().debug("preamble peaks found: ", peaks.size(), " at symbol offsets [", offsets, "]");
  }

  // Local containers stand in for the streaming state when the caller does not
  // supply one, so the rest of the function reads the same either way.
  std::set<int> local_emitted;
  std::map<int, ByteVector> local_pending;
  std::map<int, int> local_copies_seen;
  std::set<int>& emitted_indices = state ? state->emitted : local_emitted;
  std::map<int, ByteVector>& pending_blocks = state ? state->pending_blocks : local_pending;
  std::map<int, int>& copies_seen = state ? state->copies_seen : local_copies_seen;

  if (peaks.empty()) {
    log().debug("no preambles above threshold");
    // The cached offset stays unproven: drop it so the next call re-runs the
    // FFT on fresher audio. Only signal end-of-session if we HAD lock --
    // silence before the first TX must not fire the flush.
    if (streaming && state && state->coarse_offset_hz.has_value()) {
      state->coarse_offset_hz.reset();
      state->session_ended = true;
      // Session is over: flush pending blocks even though the last of them
      // may have fewer than block_repeats copies.
      ByteVector tail;
      for (const auto& entry : pending_blocks) {
        if (emitted_indices.find(entry.first) == emitted_indices.end()) {
          tail.insert(tail.end(), entry.second.begin(), entry.second.end());
          emitted_indices.insert(entry.first);
        }
      }
      pending_blocks.clear();
      copies_seen.clear();
      state->expected_block_index = 0;
      if (!tail.empty()) {
        return DecodeResult{std::move(tail), 0};
      }
    }
    return {};
  }

  // Peaks found, so the offset estimate is proven. Cache it if it was fresh.
  if (fresh_estimate && streaming && state) {
    state->coarse_offset_hz = coarse_offset;
  }

  if (streaming && peaks.size() < 2) {
    // Two preambles are needed to bracket a slot. Keep the one we have as an
    // anchor, unless it has been sitting past two block durations.
    const std::size_t block_length = config.block_symbol_length();
    const std::size_t symbols_past = coarse_magnitudes.rows() - peaks[0];
    if (symbols_past > 2 * (block_length + preamble.size())) {
      return DecodeResult{{}, (peaks[0] + preamble.size()) * samples_per_symbol};
    }
    return DecodeResult{{}, peaks[0] * samples_per_symbol};
  }

  // Per-preamble fine offset, which tracks drift marker by marker.
  std::vector<double> peak_offsets;
  peak_offsets.reserve(peaks.size());
  for (const std::size_t peak : peaks) {
    const std::size_t start = peak * samples_per_symbol;
    const std::size_t end = start + preamble.size() * samples_per_symbol;
    if (end > count) {
      peak_offsets.push_back(coarse_offset);
      continue;
    }
    if (config.frequency_search_hz() > 0.0) {
      peak_offsets.push_back(dsp::estimate_frequency_offset(
          samples + start, end - start, config.waveform(), preamble,
          config.frequency_search_hz(), config.frequency_resolution_hz(), coarse_offset));
    } else {
      peak_offsets.push_back(coarse_offset);
    }
  }

  const std::size_t block_length = config.block_symbol_length();

  // Batch mode projects a virtual leading / trailing preamble so head- or
  // tail-chopped signals can still be recovered via zero-padding plus RS.
  std::vector<float> padded_storage;
  const float* work_samples = samples;
  std::size_t work_count = count;
  const auto adopt_padding = [&](std::size_t pad_rows, bool at_head) {
    const std::size_t pad_samples = pad_rows * samples_per_symbol;
    // Build into a fresh buffer: a head pad followed by a tail pad would
    // otherwise have the second call overwrite the storage it is copying from.
    std::vector<float> grown(work_count + pad_samples, 0.0f);
    const std::size_t offset = at_head ? pad_samples : 0;
    std::copy(work_samples, work_samples + work_count,
              grown.begin() + static_cast<std::ptrdiff_t>(offset));
    padded_storage = std::move(grown);
    work_samples = padded_storage.data();
    work_count = padded_storage.size();
    coarse_magnitudes = dsp::Magnitudes::pad(coarse_magnitudes, pad_rows, at_head);
  };

  if (!streaming) {
    if (peaks[0] > preamble.size()) {
      std::ptrdiff_t virtual_peak = static_cast<std::ptrdiff_t>(peaks[0]) -
                                    static_cast<std::ptrdiff_t>(block_length) -
                                    static_cast<std::ptrdiff_t>(preamble.size());
      if (virtual_peak < 0) {
        const auto pad_rows = static_cast<std::size_t>(-virtual_peak);
        adopt_padding(pad_rows, true);
        for (std::size_t& peak : peaks) {
          peak += pad_rows;
        }
        virtual_peak = 0;
      }
      peak_offsets.insert(peak_offsets.begin(), coarse_offset);
      peaks.insert(peaks.begin(), static_cast<std::size_t>(virtual_peak));
    }
    // Only project a trailing preamble when there is meaningful signal past
    // the last real peak. If the buffer ends right at one, projecting would
    // decode zero-padded silence and log a bogus CRC failure.
    const std::ptrdiff_t tail_symbols = static_cast<std::ptrdiff_t>(coarse_magnitudes.rows()) -
                                        static_cast<std::ptrdiff_t>(peaks.back()) -
                                        static_cast<std::ptrdiff_t>(preamble.size());
    if (tail_symbols > static_cast<std::ptrdiff_t>(block_length / 2)) {
      const std::size_t virtual_end = peaks.back() + preamble.size() + block_length;
      if (virtual_end > coarse_magnitudes.rows()) {
        adopt_padding(virtual_end - coarse_magnitudes.rows(), false);
      }
      peak_offsets.push_back(coarse_offset);
      peaks.push_back(virtual_end);
    }
  }

  ByteVector output;
  int total_rs_errors_corrected = 0;
  int slots_attempted = 0;
  int slots_decoded = 0;

  std::map<int, ByteVector> current_message;

  const auto flush_message = [&]() {
    // Emit in block-index order. A missing index leaves a gap rather than
    // truncating the tail, so one unrecoverable slot does not cost the rest of
    // a long stream.
    for (const auto& entry : current_message) {
      if (emitted_indices.find(entry.first) != emitted_indices.end()) {
        continue;
      }
      output.insert(output.end(), entry.second.begin(), entry.second.end());
      emitted_indices.insert(entry.first);
    }
    current_message.clear();
  };

  const auto flush_pending_all = [&](const char* reason) {
    // Emit every buffered block, confirmed or not. Called at a message
    // boundary or session end, where the message is over and partial copy
    // counts are all we are going to get.
    if (pending_blocks.empty()) {
      return;
    }
    log().debug("flushing ", pending_blocks.size(), " unfinalised block(s) (", reason, ")");
    for (const auto& entry : pending_blocks) {
      if (emitted_indices.find(entry.first) == emitted_indices.end()) {
        output.insert(output.end(), entry.second.begin(), entry.second.end());
        emitted_indices.insert(entry.first);
      }
    }
    pending_blocks.clear();
    copies_seen.clear();
  };

  const auto observe_copy = [&](int header_block_index, const ByteVector& content) {
    if (emitted_indices.find(header_block_index) != emitted_indices.end()) {
      return;
    }
    pending_blocks.emplace(header_block_index, content);
    const int seen = ++copies_seen[header_block_index];
    if (seen >= config.block_repeats()) {
      current_message[header_block_index] = pending_blocks[header_block_index];
      pending_blocks.erase(header_block_index);
      copies_seen.erase(header_block_index);
    }
  };

  const std::size_t stride = block_length + preamble.size();
  int expected_block_index = state ? state->expected_block_index : 0;
  int copies_seen_this_block = 0;
  // Soft LLRs of consecutive slots that failed to decode independently; once
  // block_repeats of them accumulate we try soft-LLR combining.
  std::vector<std::vector<double>> combining_buffer;

  std::size_t slot = 0;
  while (slot + 1 < peaks.size()) {
    const std::size_t slot_start = peaks[slot] + preamble.size();
    const std::size_t slot_end = peaks[slot + 1];
    const std::ptrdiff_t span =
        static_cast<std::ptrdiff_t>(slot_end) - static_cast<std::ptrdiff_t>(slot_start);
    if (std::abs(span - static_cast<std::ptrdiff_t>(block_length)) > 4) {
      // Either a spurious peak -- dropping peaks[slot+1] leaves a
      // stride-consistent chain -- or a real message boundary. peaks[slot+1]
      // is spurious when it is off-stride AND ignoring it reaches a successor
      // at peaks[slot] + K*stride for some K >= 1.
      bool spurious = false;
      for (std::size_t j = slot + 2; j < peaks.size(); ++j) {
        const std::ptrdiff_t from_anchor =
            static_cast<std::ptrdiff_t>(peaks[j]) - static_cast<std::ptrdiff_t>(peaks[slot]);
        if (from_anchor <= 0) {
          continue;
        }
        const auto nearest = static_cast<std::ptrdiff_t>(
            std::lround(static_cast<double>(from_anchor) / static_cast<double>(stride)));
        if (nearest >= 1 &&
            std::abs(from_anchor - nearest * static_cast<std::ptrdiff_t>(stride)) <= 4) {
          spurious = true;
          break;
        }
      }
      if (spurious) {
        log().debug("slot ", slot, ": dropping spurious peak ", peaks[slot + 1]);
        peaks.erase(peaks.begin() + static_cast<std::ptrdiff_t>(slot) + 1);
        peak_offsets.erase(peak_offsets.begin() + static_cast<std::ptrdiff_t>(slot) + 1);
        continue;  // retry the same slot
      }
      log().debug("slot ", slot, " span ", span, ": message boundary");
      flush_pending_all("message boundary");
      flush_message();
      // A new message starts fresh: the previous message's block indices stop
      // acting as dedup keys against it.
      emitted_indices.clear();
      expected_block_index = 0;
      copies_seen_this_block = 0;
      combining_buffer.clear();
      ++slot;
      continue;
    }

    const double slot_offset = peak_offsets[slot];
    dsp::Magnitudes slot_magnitudes;
    if (std::abs(slot_offset - coarse_offset) > 0.5) {
      const std::size_t start = slot_start * samples_per_symbol;
      const std::size_t end = std::min(slot_end * samples_per_symbol, work_count);
      if (end > start) {
        slot_magnitudes = dsp::demodulate_soft(work_samples + start, end - start,
                                               config.waveform(), slot_offset)
                              .slice(0, block_length);
      }
    } else {
      slot_magnitudes = coarse_magnitudes.slice(slot_start, block_length);
    }

    if (slot_magnitudes.rows() >= block_length) {
      ++slots_attempted;
      std::vector<double> soft =
          dsp::soft_bits_from_magnitudes(slot_magnitudes, config.waveform().num_tones());
      const SlotDecode decoded =
          decode_slot_with_seed_search(soft, config, expected_block_index);
      bool accepted = false;
      if (decoded.ok && decoded.payload.size() >= kHeaderBytes) {
        const int header_block_index = (static_cast<int>(decoded.payload[1]) << 8) |
                                       static_cast<int>(decoded.payload[2]);
        if (valid_seed_for_block(decoded.seed_slot, header_block_index,
                                 config.block_repeats())) {
          accepted = true;
          observe_copy(header_block_index, block_content(decoded.payload));
          ++slots_decoded;
          total_rs_errors_corrected += decoded.errors_corrected;
          ++copies_seen_this_block;
          if (copies_seen_this_block >= config.block_repeats()) {
            expected_block_index = header_block_index + 1;
            copies_seen_this_block = 0;
          } else {
            expected_block_index = header_block_index;
          }
          combining_buffer.clear();
        }
      }
      if (!accepted && config.block_repeats() > 1) {
        // Independent decode failed. Buffer the soft LLRs; once block_repeats
        // copies have accumulated, sum their deinterleaved LLRs -- classical
        // soft-combining diversity.
        combining_buffer.push_back(std::move(soft));
        if (static_cast<int>(combining_buffer.size()) >= config.block_repeats()) {
          const SlotDecode combined =
              decode_combined_copies(combining_buffer, config, expected_block_index);
          if (combined.ok && combined.payload.size() >= kHeaderBytes) {
            const int header_block_index = (static_cast<int>(combined.payload[1]) << 8) |
                                           static_cast<int>(combined.payload[2]);
            // A combined decode consumed every copy at once, so the block is
            // final regardless of how many copies were counted so far.
            if (emitted_indices.find(header_block_index) == emitted_indices.end()) {
              current_message[header_block_index] = block_content(combined.payload);
              pending_blocks.erase(header_block_index);
              copies_seen.erase(header_block_index);
            }
            ++slots_decoded;
            total_rs_errors_corrected += combined.errors_corrected;
            expected_block_index = header_block_index + 1;
            copies_seen_this_block = 0;
          }
          // Worked or not, this block has had its shot.
          combining_buffer.clear();
        }
      }
    }
    ++slot;
  }

  flush_message();
  if (streaming && state) {
    // Persist the unfinalised state so a block whose copies straddle two
    // streaming calls picks up where it left off.
    state->expected_block_index = expected_block_index;
  } else {
    // Batch mode (drain / WAV rx): no more calls are coming.
    flush_pending_all("batch mode end");
  }

  if (total_rs_errors_corrected > 0) {
    log().warning("RS corrected ", total_rs_errors_corrected, " byte-symbol(s) total");
  }
  const int slots_failed = slots_attempted - slots_decoded;
  if (slots_failed > 0) {
    log().error(slots_failed, " slot(s) failed CRC/RS -- copies elsewhere may recover");
  }
  log().debug("slots: ", slots_attempted, " attempted, ", slots_decoded, " decoded; ",
              output.size(), " bytes emitted");

  DecodeResult result;
  result.bytes = std::move(output);
  if (streaming) {
    result.safe_cursor_samples = peaks.back() * samples_per_symbol;
  }
  return result;
}

DecodeResult decode(const Samples& samples, const ModemConfig& config, bool streaming,
                    StreamingState* state) {
  return decode(samples.data(), samples.size(), config, streaming, state);
}

}  // namespace weaklink::codec
