# weaklink-modem (C++)

Streaming MFSK modem: bytes on stdin → audio → bytes on stdout. Reed-Solomon +
convolutional K=7 r=1/2 + soft Viterbi + per-block interleaver + soft-LLR
combining across block repeats. Modes: OOK and 2/4/8/16-FSK at 45 / 300 / 1200
baud.

This is a C++17 port of the Python implementation. It is **wire compatible**:
audio produced by either implementation decodes in the other, and the two
encoders produce the same waveform to within floating-point rounding.

## Quick start

```bash
make build-mac-arm      # or build-mac / build-linux / build-linux-arm
echo "hello" | ./binaries/macos-arm64/weaklink-modem tx --modem-wav /tmp/out.wav
./binaries/macos-arm64/weaklink-modem rx --modem-wav /tmp/out.wav
```

## Building

Binaries land in `binaries/<platform>/`, which is gitignored.

| Target | What it does |
| --- | --- |
| `make build-mac` | macOS x86_64 (Intel) |
| `make build-mac-arm` | macOS arm64 (Apple silicon) |
| `make build-mac-universal` | macOS arm64 + x86_64 in one file (what releases ship) |
| `make build-mac-host` | macOS for whatever the build machine is |
| `make build-linux` | Linux x86_64 in `docker/linux-amd64` |
| `make build-linux-arm` | Linux arm64 in `docker/linux-arm64` |
| `make build-windows` | not implemented yet (see below) |
| `make test` | configure, build and run the test suite |
| `make interop` | cross-validate against the Python implementation |

macOS has no Docker image, and cannot have one: Apple's SDK is not licensed or
technically able to run in a Linux container. Build it natively on a `macos-*`
CI runner using the same `make` target you would run locally. Linux and Windows
build in containers, so a CI job is just `docker compose run --rm <service>`.

### One binary per platform

There are no per-distribution builds. macOS ships a single universal binary
carrying both slices; Linux needs one file per CPU architecture because that is
machine code, but neither file is tied to a distribution:

```
$ ldd weaklink-modem
        linux-vdso.so.1
        libm.so.6 => /lib/aarch64-linux-gnu/libm.so.6
        libc.so.6 => /lib/aarch64-linux-gnu/libc.so.6
```

`libstdc++` is linked statically, and PortAudio is built with `PA_ALSA_DYNAMIC`
so ALSA is `dlopen`'d rather than linked -- the binary starts on a bare image
and only needs the library when live audio is actually opened. WAV and
stdin/stdout modes never touch it. Building against Ubuntu 22.04's glibc keeps
it forward compatible with newer distributions.

CI proves this on every push: the exact artifact is dropped into stock Ubuntu
22.04/24.04, Debian bookworm/trixie, Arch and Raspberry Pi OS images, with
nothing installed, and has to complete a tx → WAV → rx round-trip in three
different modes.

Windows is scaffolded but unimplemented. The modem core cross-compiles under
mingw-w64, but the live-audio layer needs a Windows backend and the
`paplay`/`parec` subprocess path has no Windows equivalent. The compose
service, Makefile target and Dockerfile are in place for when that lands.

### Dependencies

Everything needed to build is vendored under `third_party/` (pocketfft, Catch2,
CLI11), so a clean checkout builds offline. PortAudio is the exception: it needs
a real build, so it is fetched at configure time from a pinned tag. Build with
`-DWEAKLINK_LIVE_AUDIO=OFF` to drop it and keep only the WAV paths.

## CLI

The command line is unchanged from the Python implementation: same subcommands,
same `--modem-*` flags, same defaults, same semantics.

```
weaklink-modem tx [--modem-wav PATH | --modem-audio-output DEVICE]
                  [--modem-baud 45|300|1200] [--modem-num-tones 1|2|4|8|16]
                  [--modem-rs-data-bytes N] [--modem-rs-parity-bytes N]
                  [--modem-no-rs-crc] [--modem-sync-every-blocks N]
                  [--modem-block-repeats N] [--modem-tx-volume 0-100]
                  [--modem-tune] [--hamlib-ptt [HOST:PORT]]
                  [--modem-debug] [--modem-log-file PATH]

weaklink-modem rx [--modem-wav PATH | --modem-audio-input DEVICE] [same modem flags]
```

Diagnostics never touch stdout or stderr -- stdout is the byte pipe. They go to
`./log.txt` unless `--modem-log-file` says otherwise.

## Architecture

Signal chain, TX top-down (RX is the mirror):

```
stdin → codec::encode_stream → rs::BlockCodec::encode → fec::encode
      → interleaver::interleave → dsp::modulate → audio::WavWriter / PortAudio
```

Each library is a separate CMake target under `libs/`:

- **`wl_core`** — exceptions, baud presets, bit/byte helpers, CRC-32, the file logger.
- **`wl_rng`** — NumPy's SeedSequence + PCG64 and Python's MT19937, reproduced
  bit-for-bit. This is wire-format-defining, not a convenience: the interleaver
  permutation and the pilot symbol sequence both come out of it.
- **`wl_reedsolomon`** — GF(2^8) Reed-Solomon matching `reedsolo`'s conventions
  (primitive polynomial 0x11d, generator 2, first consecutive root 0), plus the
  CRC-32 that sits inside the RS-protected region.
- **`wl_fec`** — K=7 r=1/2 NASA/CCSDS convolutional code (generators 171, 133
  octal) with a soft-decision Viterbi decoder driven by per-bit LLRs.
- **`wl_interleaver`** — per-block bit permutation cycling every 32 blocks.
  Breaks up burst errors before Viterbi and stops periodic noise from hitting
  the same bit positions in every block.
- **`wl_dsp`** — MFSK CPFSK modulator and non-coherent I/Q demodulator.
  Continuous phase across symbol boundaries, Gray-coded symbol-to-tone mapping,
  max-log-MAP soft output. **One tone is on air at a time regardless of
  `num_tones`** — that is the alphabet size (log2 M bits per symbol), not a
  count of simultaneous carriers.
- **`wl_codec`** — slot and session framing, preamble correlator, interleaver
  seed search, soft-LLR combining across block repeats.
- **`wl_audio`** — WAV read/write (RIFF directly, no libsndfile), PortAudio
  live I/O, and a `paplay`/`parec` subprocess path for named Pulse endpoints
  PortAudio's compatibility layer cannot reach.
- **`wl_ptt`** — rigctld PTT over TCP.
- **`wl_streaming`** — pilot generation, the streaming RX decoder, the live-rx
  poll loop.
- **`wl_api`** — the public API, mirroring the CLI one-to-one.

### Wire format

```
[pilot] [preamble][slot] [preamble][slot] ... [preamble] [pilot]
```

Each slot carries one RS block wrapping `[length(1B)][block_index(2B)][payload][pad][CRC-32(4B)][RS parity]`.
Message boundaries are inferred at RX from non-block-length spans between
preambles, so one RX pipe can watch many independent TX sessions in a row.

## Testing

```bash
make test                 # everything
ctest --test-dir build/test -L fast    # just the quick ones
./binaries/test/weaklink-tests "[rng]" # one tag
```

The suite mirrors the Python test suite case for case, plus tests the Python
version does not have: NumPy/`random` generator parity against golden vectors,
Reed-Solomon parity vectors from `reedsolo`, and WAV reader/writer round-trips.

Tests tagged `[slow]` are the SNR sweeps, the damage matrix and the
multi-kilobyte round-trips.

### Interop

`tools/interop_check.py` encodes the same payload with both implementations and
decodes both WAVs with both, across the full baud × tone matrix, and compares
the encoders' waveforms sample by sample.

```bash
python3 tools/interop_check.py --cpp-bin ./binaries/test/weaklink-modem
```

All 14 feasible modes interoperate in both directions. Waveforms agree to
within 1.5e-11 per sample (the residual is last-ULP `sin` differences between
NumPy's vectorised math and libm); OOK modes are bit-identical.

### SNR cliffs

Decoding the same bytes is necessary but not sufficient -- the port could be
wire compatible and still be a dB or two worse in noise if the soft-decision
chain lost precision. `tools/compare_benchmark.py` diffs the two
implementations' measured cliffs:

```bash
weaklink-modem-benchmark --bauds 300 --trials 5 --dry-run > /tmp/cpp.md
python3 tools/compare_benchmark.py /tmp/cpp.md ../weaklink-9a3ice/results.md
```

Across all 60 configurations at 300 baud, the port's cliff matches the
reference's to the dB. Note that the reference's checked-in `results.md`
predates some of its own code changes: the two rows that appear to differ are
both OOK, and both match when the reference is re-measured rather than read
from the table.

Three modes behave differently from a naive "did the payload survive" check,
and in every case the difference is on the reference side:

| Mode | Behaviour |
| --- | --- |
| 2-FSK at 45 and 300 baud | Both implementations drop the final byte, identically. A reference limitation the port reproduces exactly. |
| OOK at 45 and 1200 baud | The port recovers the payload; the reference emits blocks out of order. The port is strictly better here. |

## Differences from the Python implementation

Deliberate, and limited to these:

1. **`rs_data_bytes` beyond one RS codeword is rejected.** The Python version
   accepts `data + 4 + parity > 255`, silently chunks the RS encode, and then
   computes its block length as `data + crc + parity` — which desynchronises
   the decoder. The port raises `ConfigError` instead. No supported preset or
   test reaches that range.
2. **Correlator ties break toward the later position.** When several
   alignments score identically — which happens inside a noiseless OOK pilot —
   NumPy's unstable `argsort` picks arbitrarily. The port picks deterministically,
   which is what lets it decode OOK where the reference cannot.
3. **The benchmark uses threads, not processes**, since there is no pickling
   constraint to work around.
