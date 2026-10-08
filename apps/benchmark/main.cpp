// Streaming-modem benchmark: sweep baud x tones x RS x repeats, measure the
// SNR cliff, compute the Shannon limit at the same info rate, and rewrite the
// results table.
//
// Cliff finding walks down in 1 dB steps and records the lowest SNR at which
// every trial still decodes the payload byte for byte. That is conservative --
// the 50% cliff usually sits 1-2 dB below.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "CLI11.hpp"
#include "weaklink/codec.hpp"
#include "weaklink/constants.hpp"
#include "weaklink/exceptions.hpp"
#include "weaklink/rng.hpp"

namespace {

using weaklink::ByteVector;
using weaklink::codec::ModemConfig;
using weaklink::dsp::Samples;

constexpr const char* kResultsStartMarker = "<!-- BENCHMARK RESULTS START -->";
constexpr const char* kResultsEndMarker = "<!-- BENCHMARK RESULTS END -->";

constexpr int kPayloadBytes = 100;
/// OOK carries 1 bit per symbol, so 100 bytes take ~10x the air time of 4-FSK
/// at the same baud. Cap it lower or the sweep never finishes.
constexpr int kOokPayloadBytes = 16;
constexpr uint64_t kPayloadSeed = 0;
constexpr int kSyncEveryFixed = 4;

const std::vector<int> kBauds = {45, 300};
const std::vector<std::pair<int, int>> kRsConfigs = {{16, 8}, {32, 8}, {128, 32}};
const std::vector<int> kBlockRepeats = {1, 2, 4, 8};
const std::vector<int> kNumTones = {1, 2, 4, 8, 16};

struct SweepConfig {
  int baud;
  int rs_data;
  int rs_parity;
  int block_repeats;
  int num_tones;
  int sync_every = kSyncEveryFixed;
  int payload_bytes = kPayloadBytes;

  ModemConfig build() const {
    weaklink::dsp::WaveformOptions waveform;
    waveform.baud = static_cast<double>(baud);
    waveform.tone_spacing_hz = static_cast<double>(baud);
    waveform.num_tones = num_tones;

    ModemConfig::Options options;
    options.waveform = waveform.build();
    options.rs_data_bytes = rs_data;
    options.rs_parity_bytes = rs_parity;
    options.rs_crc_enabled = true;
    options.sync_every_blocks = sync_every;
    options.block_repeats = block_repeats;
    return ModemConfig(options);
  }

  /// Block layout label. Wire = data + 4 (CRC-32) + parity. Textbook notation
  /// would be RS(wire, data+4); spelling it out keeps the numbers matching the
  /// --modem-rs-* flags.
  std::string rs_label() const {
    return std::to_string(rs_data + rs_parity + 4) + "B block / " + std::to_string(rs_data) +
           "B data / " + std::to_string(rs_parity) + "B parity";
  }
};

struct Result {
  SweepConfig config;
  double duration_seconds = 0.0;
  double info_rate_bit_per_s = 0.0;
  std::optional<double> cliff_snr_db;
  double shannon_snr_db = 0.0;
};

ByteVector random_payload(int size) {
  ByteVector alphabet;
  for (char c = 'a'; c <= 'z'; ++c) {
    alphabet.push_back(static_cast<uint8_t>(c));
  }
  for (char c = 'A'; c <= 'Z'; ++c) {
    alphabet.push_back(static_cast<uint8_t>(c));
  }
  for (char c = '0'; c <= '9'; ++c) {
    alphabet.push_back(static_cast<uint8_t>(c));
  }
  alphabet.push_back(static_cast<uint8_t>(' '));
  // Python's random.Random, so the payload matches the reference benchmark.
  weaklink::rng::PythonRandom generator(kPayloadSeed);
  return generator.choices(alphabet, static_cast<std::size_t>(size));
}

double shannon_snr_db(double info_rate_bit_per_s,
                      double bandwidth_hz = weaklink::kReferenceBandwidthHz) {
  if (info_rate_bit_per_s <= 0.0) {
    return -std::numeric_limits<double>::infinity();
  }
  return 10.0 * std::log10(std::pow(2.0, info_rate_bit_per_s / bandwidth_hz) - 1.0);
}

Samples add_awgn(const Samples& samples, double snr_db, double sample_rate, uint64_t seed) {
  double sum_squares = 0.0;
  for (const float sample : samples) {
    const double value = static_cast<double>(sample);
    sum_squares += value * value;
  }
  const double signal_power = sum_squares / static_cast<double>(samples.size());
  const double noise_variance = signal_power * sample_rate /
                                (2.0 * weaklink::kReferenceBandwidthHz) /
                                std::pow(10.0, snr_db / 10.0);
  const double sigma = std::sqrt(noise_variance);

  weaklink::rng::NumpyGenerator generator(seed);
  Samples out(samples.size());
  for (std::size_t i = 0; i < samples.size(); ++i) {
    out[i] = samples[i] + static_cast<float>(sigma * generator.standard_normal());
  }
  return out;
}

ByteVector strip_trailing_nul(ByteVector data) {
  while (!data.empty() && data.back() == 0) {
    data.pop_back();
  }
  return data;
}

Result find_cliff(const SweepConfig& config, int trials, const ByteVector& payload) {
  const ModemConfig modem = config.build();
  const Samples samples = weaklink::codec::encode(payload, modem);

  Result result;
  result.config = config;
  result.duration_seconds =
      static_cast<double>(samples.size()) / modem.waveform().sample_rate();
  result.info_rate_bit_per_s =
      static_cast<double>(payload.size()) * 8.0 / result.duration_seconds;
  result.shannon_snr_db = shannon_snr_db(result.info_rate_bit_per_s);

  const ByteVector expected = strip_trailing_nul(payload);
  // OOK's cliff sits ~10 dB higher than MFSK, so its sweep has to start higher
  // to see it at all.
  double snr_db = config.num_tones == 1 ? 25.0 : 10.0;
  while (snr_db >= -28.0) {
    int successes = 0;
    for (int trial = 0; trial < trials; ++trial) {
      const long seed_input = static_cast<long>(config.baud) * 1000003L +
                              static_cast<long>(config.rs_data) * 71L +
                              static_cast<long>(config.sync_every) * 13L +
                              static_cast<long>(config.block_repeats) * 97L +
                              static_cast<long>(trial) * 31L +
                              static_cast<long>(snr_db * 10.0);
      const auto seed = static_cast<uint64_t>(std::abs(seed_input) & 0x7FFFFFFFL);
      const Samples noisy =
          add_awgn(samples, snr_db, modem.waveform().sample_rate(), seed);
      if (strip_trailing_nul(weaklink::codec::decode(noisy, modem).bytes) == expected) {
        ++successes;
      }
    }
    if (successes != trials) {
      break;
    }
    result.cliff_snr_db = snr_db;
    snr_db -= 1.0;
  }
  return result;
}

std::vector<SweepConfig> enumerate_configs() {
  std::vector<SweepConfig> configs;
  for (const int baud : kBauds) {
    for (const auto& rs : kRsConfigs) {
      for (const int repeats : kBlockRepeats) {
        for (const int tones : kNumTones) {
          SweepConfig config;
          config.baud = baud;
          config.rs_data = rs.first;
          config.rs_parity = rs.second;
          config.block_repeats = repeats;
          config.num_tones = tones;
          config.payload_bytes = tones == 1 ? kOokPayloadBytes : kPayloadBytes;
          // Skip Nyquist-infeasible combinations, e.g. 300 baud x 32 tones
          // needs 9.3 kHz of tone stack and the 18 kHz internal rate cannot
          // provide it.
          try {
            config.build();
          } catch (const weaklink::ConfigError&) {
            continue;
          }
          configs.push_back(config);
        }
      }
    }
  }
  return configs;
}

std::string cli_snippet(const SweepConfig& config) {
  return "`--modem-baud " + std::to_string(config.baud) + "`<br/>`--modem-num-tones " +
         std::to_string(config.num_tones) + "`<br/>`--modem-rs-data-bytes " +
         std::to_string(config.rs_data) + "`<br/>`--modem-rs-parity-bytes " +
         std::to_string(config.rs_parity) + "`<br/>`--modem-block-repeats " +
         std::to_string(config.block_repeats) + "`";
}

std::string format_number(double value, int precision, bool force_sign = false) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(precision);
  if (force_sign) {
    stream << std::showpos;
  }
  stream << value;
  return stream.str();
}

std::string format_table(const std::vector<Result>& results) {
  std::ostringstream out;
  out << "Streaming modem. Payload: " << kPayloadBytes
      << " random-ASCII bytes. Sync every " << kSyncEveryFixed
      << " data blocks. Reference bandwidth: 3 kHz.\n\n";
  out << "| Baud | Tones | CLI (both tx / rx) | Throughput | Info rate | Best SNR | Shannon | "
         "Gap |\n";
  out << "|---:|---:|---|---|---:|---:|---:|---:|\n";
  for (const Result& result : results) {
    const std::string cliff = result.cliff_snr_db
                                  ? "**" + format_number(*result.cliff_snr_db, 0, true) + " dB**"
                                  : "not reached";
    const std::string gap =
        result.cliff_snr_db
            ? format_number(*result.cliff_snr_db - result.shannon_snr_db, 1) + " dB"
            : "n/a";
    out << "| " << result.config.baud << " | " << result.config.num_tones << " | "
        << cli_snippet(result.config) << " | " << result.config.payload_bytes << " chars in "
        << format_number(result.duration_seconds, 1) << " s | "
        << format_number(result.info_rate_bit_per_s, 1) << " bit/s | " << cliff << " | "
        << format_number(result.shannon_snr_db, 1, true) << " dB | " << gap << " |\n";
  }
  return out.str();
}

void update_results_file(const std::string& table, const std::string& path) {
  std::ifstream input(path);
  if (!input) {
    throw weaklink::WeaklinkError("cannot read " + path);
  }
  const std::string text((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  const std::size_t start = text.find(kResultsStartMarker);
  const std::size_t end = text.find(kResultsEndMarker);
  if (start == std::string::npos || end == std::string::npos || end < start) {
    throw weaklink::WeaklinkError(std::string("could not find markers ") + kResultsStartMarker +
                                  " and " + kResultsEndMarker + " in " + path);
  }
  const std::string before = text.substr(0, start + std::char_traits<char>::length(kResultsStartMarker));
  const std::string after = text.substr(end);
  std::ofstream output(path, std::ios::trunc);
  output << before << "\n\n" << table << "\n" << after;
}

void print_row(const Result& result) {
  const std::string cliff =
      result.cliff_snr_db ? format_number(*result.cliff_snr_db, 0, true) + " dB" : "no decode";
  std::printf("baud=%4d %13s repeats=%dx  duration=%6.1fs  info=%7.1f bit/s  cliff=%9s  "
              "shannon=%+.1f dB\n",
              result.config.baud, result.config.rs_label().c_str(),
              result.config.block_repeats, result.duration_seconds,
              result.info_rate_bit_per_s, cliff.c_str(), result.shannon_snr_db);
  std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"Streaming-modem SNR-cliff benchmark.", "weaklink-modem-benchmark"};

  int trials = 5;
  bool dry_run = false;
  std::string bauds_text;
  unsigned workers = std::max(1u, std::thread::hardware_concurrency() > 2
                                      ? std::thread::hardware_concurrency() - 2
                                      : 1u);
  std::string results_path = "results.md";

  for (const int baud : kBauds) {
    if (!bauds_text.empty()) {
      bauds_text += ",";
    }
    bauds_text += std::to_string(baud);
  }

  app.add_option("--trials", trials, "Trials per SNR step.")->default_val(5);
  app.add_flag("--dry-run", dry_run, "Print the table instead of patching the results file.");
  app.add_option("--bauds", bauds_text,
                 "Comma-separated baud rates to sweep. Default: all supported. Example: "
                 "--bauds 300 skips the slow 45-baud rows.");
  app.add_option("--workers", workers,
                 "Parallel worker threads. Configs are independent, so this scales roughly "
                 "linearly with core count. Default: cores - 2.");
  app.add_option("--results", results_path,
                 "Markdown file to update between the BENCHMARK RESULTS markers.")
      ->default_val("results.md");

  CLI11_PARSE(app, argc, argv);

  std::vector<int> selected;
  {
    std::istringstream stream(bauds_text);
    std::string item;
    while (std::getline(stream, item, ',')) {
      if (!item.empty()) {
        selected.push_back(std::stoi(item));
      }
    }
  }

  std::vector<SweepConfig> configs;
  for (const SweepConfig& config : enumerate_configs()) {
    if (std::find(selected.begin(), selected.end(), config.baud) != selected.end()) {
      configs.push_back(config);
    }
  }
  if (configs.empty()) {
    std::printf("no configs match --bauds %s\n", bauds_text.c_str());
    return 1;
  }

  std::printf("Sweeping %zu configs with %d trials/point across %u worker(s).\n\n",
              configs.size(), trials, workers);

  std::vector<Result> results(configs.size());
  std::atomic<std::size_t> next_index{0};
  std::mutex print_mutex;

  const auto worker = [&] {
    while (true) {
      const std::size_t index = next_index.fetch_add(1);
      if (index >= configs.size()) {
        return;
      }
      const ByteVector payload = random_payload(configs[index].payload_bytes);
      results[index] = find_cliff(configs[index], trials, payload);
      std::lock_guard<std::mutex> guard(print_mutex);
      print_row(results[index]);
    }
  };

  std::vector<std::thread> pool;
  pool.reserve(workers);
  for (unsigned i = 0; i < workers; ++i) {
    pool.emplace_back(worker);
  }
  for (std::thread& thread : pool) {
    thread.join();
  }

  const std::string table = format_table(results);
  if (dry_run) {
    std::printf("\n%s", table.c_str());
    return 0;
  }
  try {
    update_results_file(table, results_path);
  } catch (const weaklink::WeaklinkError& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 2;
  }
  std::printf("\nPatched %s\n", results_path.c_str());
  return 0;
}
