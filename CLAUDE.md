# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

C++17 port of the Python `weaklink-modem`. Streaming MFSK modem: bytes on stdin
→ audio → bytes on stdout. Reed-Solomon + convolutional K=7 r=1/2 + soft
Viterbi + per-block interleaver + soft-LLR combining across block repeats.
Modes: OOK and 2/4/8/16-FSK at 45 / 300 / 1200 baud.

The port is **wire compatible** with the Python implementation. That is the
primary constraint: anything that changes the bits on the wire breaks it.

## Common commands

```bash
make build-mac-arm                        # -> binaries/macos-arm64/ (build-mac is x86_64)
make test                                 # configure, build, run the suite
./binaries/test/weaklink-tests "~[slow]"  # quick tests only
./binaries/test/weaklink-tests "[rng]"    # one tag
make interop                              # cross-check against the Python repo

cmake -S . -B build/dev -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON  # for clangd
```

`compile_commands.json` is what makes clangd stop reporting missing headers;
generate it before expecting editor diagnostics to be meaningful.

## Architecture

Signal chain, TX top-down (RX is the mirror):

```
stdin → codec::encode_stream → rs::BlockCodec::encode → fec::encode
      → interleaver::interleave → dsp::modulate → audio::WavWriter / PortAudio
```

One CMake target per concern under `libs/`, each with its public headers in
`libs/<name>/include/weaklink/`:

- **`wl_core`** — exceptions, baud presets, bit/byte helpers, CRC-32, logger.
- **`wl_rng`** — NumPy SeedSequence + PCG64, and Python MT19937, bit-for-bit.
- **`wl_reedsolomon`** — GF(2^8) RS matching `reedsolo`, plus the inner CRC-32.
- **`wl_fec`** — K=7 r=1/2 convolutional code, soft Viterbi.
- **`wl_interleaver`** — per-block permutation, cycling every 32 blocks.
- **`wl_dsp`** — CPFSK modulator, non-coherent demod, frequency offset search.
- **`wl_codec`** — framing, preamble correlator, seed search, LLR combining.
- **`wl_audio`** — WAV (direct RIFF), PortAudio, `paplay`/`parec` subprocesses.
- **`wl_ptt`** — rigctld PTT over TCP.
- **`wl_streaming`** — pilot generation, streaming RX decoder, live-rx loop.
- **`wl_api`** — public API, mirroring the CLI one-to-one.

Apps: `apps/modem` (the `weaklink-modem` CLI) and `apps/benchmark`.

## Constraints and conventions

- **Wire compatibility is the hard constraint.** The interleaver permutation
  and the pilot symbol sequence come out of a bit-exact reimplementation of
  NumPy's PCG64. Changing `wl_rng` changes the wire format. The golden vectors
  in `tests/test_rng_numpy_parity.cpp` were produced by NumPy itself and are
  the tripwire; do not "fix" them to match new behaviour.
- **NumPy's `shuffle` and `integers` use different samplers.** `permutation`
  uses masked rejection, `integers` uses Lemire. They consume the stream
  differently and only the right pairing reproduces NumPy's output.
- **The ziggurat tables are NumPy's exact constants**, recovered from its
  output rather than regenerated — a table rebuilt from the published
  construction drifts in the 12th significant digit.
- **Legal**: do not mention transmission over regulated bands or licensed
  frequencies. The modem is an audio-domain byte pipe; keep docs framed that way.
- **Scope discipline**: mirror the Python source, answer questions literally,
  don't add features / aliases / tests unless asked.
- **No `**kwargs`-style passthrough.** Every option is an explicit named
  parameter or struct field, mirroring the CLI shape.
- **Comments are for non-obvious *why*** (a hidden constraint, a workaround, a
  surprising behaviour). Don't restate what the code does or reference tickets.
- Specific mode names like `4-FSK`, `16-FSK` are conventional shorthand for a
  fixed M and are kept as-is. `MFSK` is the family name. `N-FSK` is not used.
- Warnings are errors in spirit: the build is clean under `-Wall -Wextra
  -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast`. Keep it
  that way; vendored third-party headers are included as SYSTEM so their
  warnings do not leak in.

## Deliberate divergences from Python

Only these three, all documented in the README:

1. `rs_data_bytes` past one RS codeword raises `ConfigError` instead of
   silently producing a stream the decoder cannot parse.
2. Correlator ties break toward the later position. NumPy's `argsort` is
   unstable, so the reference resolves these arbitrarily; determinism here is
   what lets the port decode OOK where the reference cannot.
3. The benchmark parallelises with threads, not processes.

## Verification

Two things must hold before any change ships:

```bash
make test                                                     # the suite
python3 tools/interop_check.py --cpp-bin ./binaries/test/weaklink-modem
```

The interop check encodes with both implementations, decodes both WAVs with
both, and compares the encoders' waveforms sample by sample. It accounts for
the reference's own limitations (2-FSK drops the last byte; OOK at 45 and 1200
baud emits blocks out of order) and fails only when the port is worse than, or
reads audio differently from, the reference.
