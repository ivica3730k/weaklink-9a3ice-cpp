// Streaming modem CLI. Bytes on stdin/stdout, samples via WAV or live audio.
// Baud presets live in constants.hpp (45/300/1200); --modem-* flags override.

#include <atomic>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "CLI11.hpp"
#include "weaklink/api.hpp"
#include "weaklink/constants.hpp"
#include "weaklink/exceptions.hpp"
#include "weaklink/log.hpp"

namespace {

constexpr const char* kVersion = "0.0.0";

std::atomic<bool> g_interrupted{false};

void handle_interrupt(int) { g_interrupted.store(true, std::memory_order_relaxed); }

/// Every ``--modem-*`` flag, shared by both subcommands.
///
/// Presetable parameters stay unset at the CLI layer so "user didn't say" can
/// be told apart from "user asked for the preset value"; an explicit flag
/// always wins over the baud preset.
struct ModemArgs {
  double baud = 300.0;
  int rs_data_bytes = 0;
  int rs_parity_bytes = 0;
  bool no_rs_crc = false;
  int sync_every_blocks = 0;
  int num_tones = 4;
  int block_repeats = 0;
  std::string wav;
  std::string audio_output;
  std::string audio_input;
  bool debug = false;
  std::string log_file = weaklink::kDefaultLogPath;

  CLI::Option* rs_data_option = nullptr;
  CLI::Option* rs_parity_option = nullptr;
  CLI::Option* sync_option = nullptr;
  CLI::Option* tones_option = nullptr;
  CLI::Option* repeats_option = nullptr;
  CLI::Option* wav_option = nullptr;
  CLI::Option* audio_output_option = nullptr;
  CLI::Option* audio_input_option = nullptr;

  weaklink::api::ModemOptions to_options() const {
    weaklink::api::ModemOptions options;
    options.baud = baud;
    options.num_tones = num_tones;
    options.rs_crc_enabled = !no_rs_crc;
    if (rs_data_option != nullptr && rs_data_option->count() > 0) {
      options.rs_data_bytes = rs_data_bytes;
    }
    if (rs_parity_option != nullptr && rs_parity_option->count() > 0) {
      options.rs_parity_bytes = rs_parity_bytes;
    }
    if (sync_option != nullptr && sync_option->count() > 0) {
      options.sync_every_blocks = sync_every_blocks;
    }
    if (repeats_option != nullptr && repeats_option->count() > 0) {
      options.block_repeats = block_repeats;
    }
    return options;
  }
};

std::string supported_bauds() {
  std::string text = "[";
  bool first = true;
  for (const auto& entry : weaklink::baud_presets()) {
    if (!first) {
      text += ", ";
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%g", entry.first);
    text += buffer;
    first = false;
  }
  return text + "]";
}

void add_modem_args(CLI::App* command, ModemArgs& args) {
  auto* group = command->add_option_group("modem", "modem-layer configuration + sample-side I/O");

  group->add_option("--modem-baud", args.baud,
                    "Symbol rate. Supported values: " + supported_bauds() + ".")
      ->default_val(300.0);
  args.rs_data_option = group->add_option(
      "--modem-rs-data-bytes", args.rs_data_bytes,
      "RS data bytes per block. Preset default depends on --modem-baud.");
  args.rs_parity_option = group->add_option(
      "--modem-rs-parity-bytes", args.rs_parity_bytes,
      "RS parity bytes per block. Preset default depends on --modem-baud.");
  group->add_flag("--modem-no-rs-crc", args.no_rs_crc,
                  "Skip the CRC-32 inside the RS-protected region.");
  args.sync_option =
      group->add_option("--modem-sync-every-blocks", args.sync_every_blocks,
                        "Preamble inserted every N data blocks. Preset default: 4.");
  args.tones_option =
      group
          ->add_option("--modem-num-tones", args.num_tones,
                       "Number of MFSK tones. 4 (default) is standard; 8/16 pack more bits per "
                       "symbol at wider bandwidth and worse cliff. 2 halves throughput but fits "
                       "narrow audio paths. 1 selects OOK (single carrier, on/off keying) -- "
                       "narrowest bandwidth of any mode, still 1 bit per symbol. TX / RX must "
                       "match.")
          ->check(CLI::IsMember({1, 2, 4, 8, 16}));
  args.repeats_option = group->add_option(
      "--modem-block-repeats", args.block_repeats,
      "Each block transmitted N times, round-robin. Preset default depends on --modem-baud.");
  args.wav_option =
      group->add_option("--modem-wav", args.wav,
                        "Read from / write to a WAV file instead of the live audio device.");
  args.audio_output_option = group->add_option(
      "--modem-audio-output", args.audio_output,
      "Audio output device for tx. Accepts a Pulse sink id (e.g. '42' -- from `pactl list short "
      "sinks`), a device index (same syntax, used when pactl doesn't know the id), a substring of "
      "a device name (e.g. 'USB'), or a Pulse sink name (e.g. 'virt'). Prefix with 'pulse:' to "
      "force the Pulse path (e.g. 'pulse:42'). Default: OS default output.");
  args.audio_input_option = group->add_option(
      "--modem-audio-input", args.audio_input,
      "Audio input device for rx. Same syntax as --modem-audio-output but matches against input "
      "devices / Pulse source names (e.g. 'virt.monitor') or ids from `pactl list short sources`. "
      "Default: OS default input.");
  group->add_flag("--modem-debug", args.debug,
                  "Verbose diagnostics (DEBUG level) in the log file: per-group decode results, "
                  "offset estimates, etc.");
  group->add_option("--modem-log-file", args.log_file,
                    "Path to the log file (default: ./" + weaklink::kDefaultLogPath +
                        "). stdout/stderr are never used for diagnostics.")
      ->default_val(weaklink::kDefaultLogPath);
}

/// Modest reads so the encoder can start emitting audio before the whole input
/// arrives -- it matters for pipes like ``tail -f | weaklink-modem tx``.
bool read_stdin_chunk(weaklink::ByteVector& chunk) {
  constexpr std::size_t kChunkBytes = 4096;
  chunk.resize(kChunkBytes);
  const std::size_t read = std::fread(chunk.data(), 1, kChunkBytes, stdin);
  chunk.resize(read);
  return read > 0;
}

void write_stdout(const weaklink::ByteVector& bytes) {
  if (bytes.empty()) {
    return;
  }
  std::fwrite(bytes.data(), 1, bytes.size(), stdout);
  std::fflush(stdout);
}

int run_tx(const ModemArgs& args, bool tune, int tx_volume, const std::string& ptt) {
  weaklink::api::TxTarget target;
  target.tune = tune;
  target.ptt = ptt;
  if (tune) {
    target.audio_output = args.audio_output;
    target.to_audio_device = true;
  } else if (args.wav_option->count() > 0) {
    target.wav_path = args.wav;
  } else {
    target.audio_output = args.audio_output;
    target.to_audio_device = true;
  }
  weaklink::api::tx(read_stdin_chunk, args.to_options(), tx_volume, target,
                    [] { return g_interrupted.load(std::memory_order_relaxed); });
  return 0;
}

int run_rx(const ModemArgs& args) {
  weaklink::api::RxSource source;
  if (args.wav_option->count() > 0) {
    source.wav_path = args.wav;
  } else {
    source.audio_input = args.audio_input;
    // PULSE_SOURCE fallback matches the Python CLI's default.
    if (source.audio_input.empty()) {
      const char* environment = std::getenv("PULSE_SOURCE");
      if (environment != nullptr) {
        source.audio_input = environment;
      }
    }
    source.from_audio_device = true;
  }
  weaklink::api::rx(source, args.to_options(), write_stdout,
                    [] { return g_interrupted.load(std::memory_order_relaxed); });
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"Streaming MFSK modem.", "weaklink-modem"};
  app.set_version_flag("--version", std::string("weaklink-modem ") + kVersion);
  app.require_subcommand(1);

  ModemArgs tx_args;
  ModemArgs rx_args;
  bool tune = false;
  int tx_volume = 100;
  std::string ptt;

  CLI::App* tx_command =
      app.add_subcommand("tx", "Encode stdin bytes and transmit (or write to WAV).");
  add_modem_args(tx_command, tx_args);
  tx_command->add_flag("--modem-tune", tune,
                       "Emit every tone of the selected mode in round-robin (one symbol each, "
                       "cycling). No framing, no preamble, no stdin -- just clean tones for radio "
                       "tuneup / audio path verification. Runs until Ctrl-C. Honours "
                       "--modem-tx-volume and --hamlib-ptt.");
  tx_command
      ->add_option("--modem-tx-volume", tx_volume,
                   "TX peak amplitude, 0-100 (default: 100 = full scale). Bump if the downstream "
                   "audio path is faint; drop to leave headroom for a hot chain.")
      ->default_val(100);
  // A bare --hamlib-ptt means localhost:4532; an argument overrides it.
  CLI::Option* ptt_option =
      tx_command
          ->add_option("--hamlib-ptt", ptt,
                       "Keyed PTT via rigctld before audio starts, released after. Bare "
                       "--hamlib-ptt defaults to localhost:4532; pass HOST:PORT to override. Only "
                       "applied when playing to a live audio device.")
          ->expected(0, 1);

  CLI::App* rx_command =
      app.add_subcommand("rx", "Receive (or read WAV) and decode to stdout bytes.");
  add_modem_args(rx_command, rx_args);

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& error) {
    // argparse exits 2 on a usage error and 0 once --help or --version has
    // printed; CLI11 uses its own codes, so they are mapped here.
    return app.exit(error) == 0 ? 0 : 2;
  }

  const bool is_tx = tx_command->parsed();
  const ModemArgs& args = is_tx ? tx_args : rx_args;

  weaklink::Logger::configure_file(
      args.log_file, args.debug ? weaklink::LogLevel::kDebug : weaklink::LogLevel::kInfo);

  std::signal(SIGINT, handle_interrupt);
#if !defined(_WIN32)
  std::signal(SIGTERM, handle_interrupt);
#endif

  weaklink::Logger cli_log("weaklink.cli");
  cli_log.debug("weaklink-modem ", is_tx ? "tx" : "rx", " starting");

  try {
    if (is_tx) {
      if (ptt_option->count() > 0 && ptt.empty()) {
        ptt = "localhost:4532";
      }
      return run_tx(tx_args, tune, tx_volume, ptt_option->count() > 0 ? ptt : std::string{});
    }
    return run_rx(rx_args);
  } catch (const weaklink::WeaklinkError& error) {
    // Typed library errors get a clean shell line, not a stack trace.
    std::cerr << "error: " << error.what() << '\n';
    return 2;
  }
}
