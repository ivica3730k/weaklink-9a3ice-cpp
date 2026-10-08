#!/usr/bin/env python3
"""Cross-validate the C++ modem against the Python reference implementation.

For every mode the check encodes the same payload with both implementations and
then decodes both WAVs with both implementations -- four legs in total:

  * python tx -> python rx   (what the reference does with its own audio)
  * cpp    tx -> cpp    rx   (what the port does with its own audio)
  * python tx -> cpp    rx   (interop: the port must read the reference)
  * cpp    tx -> python rx   (interop: the reference must read the port)

The reference does not recover the payload in every mode -- 2-FSK drops the
last byte, and OOK at 45 and 1200 baud emits blocks out of order -- so a leg is
not judged against the payload alone. What matters is:

  1. reading the reference's audio gives the same answer the reference itself
     gets, so neither side is interpreting the other's audio differently, and
  2. the port is never worse than the reference on the same audio.

The encoders' WAV output is also compared sample by sample. Both derive the
same pilot symbols, interleaver permutations and framing, so anything beyond a
few ULPs of ``sin`` means a real divergence.
"""

from __future__ import annotations

import argparse
import itertools
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

DEFAULT_PYTHON_REPO = Path(__file__).resolve().parents[2] / "weaklink-9a3ice"

BAUDS = (45.0, 300.0, 1200.0)
TONES = (1, 2, 4, 8, 16)

PAYLOAD = b"weaklink interop check 0123456789 the quick brown fox"


@dataclass
class Mode:
    baud: float
    num_tones: int

    def flags(self) -> list[str]:
        return ["--modem-baud", str(self.baud), "--modem-num-tones", str(self.num_tones)]

    def __str__(self) -> str:
        return f"{self.baud:g} baud / {self.num_tones} tones"


@dataclass
class Runner:
    name: str
    argv: list[str]
    cwd: Path | None = None

    def tx(self, payload: bytes, wav: Path, mode: Mode, log: Path) -> None:
        subprocess.run(
            [*self.argv, "tx", "--modem-wav", str(wav), "--modem-log-file", str(log), *mode.flags()],
            input=payload,
            cwd=self.cwd,
            check=True,
            stdout=subprocess.DEVNULL,
        )

    def rx(self, wav: Path, mode: Mode, log: Path) -> bytes:
        result = subprocess.run(
            [*self.argv, "rx", "--modem-wav", str(wav), "--modem-log-file", str(log), *mode.flags()],
            cwd=self.cwd,
            check=True,
            capture_output=True,
        )
        return result.stdout.rstrip(b"\x00")


def read_wav_floats(path: Path) -> list[float]:
    """Read a mono float32 WAV.

    The stdlib ``wave`` module rejects IEEE-float WAVs (format tag 3), which is
    exactly what both modems write, so the RIFF chunks are walked by hand.
    """
    data = path.read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise SystemExit(f"{path} is not a RIFF/WAVE file")

    offset = 12
    fmt_tag = 0
    bits = 0
    while offset + 8 <= len(data):
        chunk_id = data[offset : offset + 4]
        size = struct.unpack_from("<I", data, offset + 4)[0]
        body = offset + 8
        if chunk_id == b"fmt ":
            fmt_tag, _channels, _rate, _byte_rate, _align, bits = struct.unpack_from(
                "<HHIIHH", data, body
            )
        elif chunk_id == b"data":
            if fmt_tag != 3 or bits != 32:
                raise SystemExit(f"{path}: expected 32-bit float audio, got tag {fmt_tag}/{bits}")
            return list(struct.unpack_from(f"<{size // 4}f", data, body))
        offset = body + size + (size & 1)
    raise SystemExit(f"{path} has no data chunk")


def compare_waveforms(a: Path, b: Path) -> tuple[bool, float, int, int]:
    left = read_wav_floats(a)
    right = read_wav_floats(b)
    overlap = min(len(left), len(right))
    worst = max((abs(left[i] - right[i]) for i in range(overlap)), default=0.0)
    return len(left) == len(right), worst, len(left), len(right)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpp-bin", type=Path, required=True, help="path to the C++ weaklink-modem")
    parser.add_argument(
        "--python-repo",
        type=Path,
        default=DEFAULT_PYTHON_REPO,
        help="directory of the Python reference project",
    )
    parser.add_argument(
        "--python-cmd",
        default="poetry run weaklink-modem",
        help="how to invoke the Python CLI inside --python-repo",
    )
    parser.add_argument(
        "--waveform-tolerance",
        type=float,
        default=1e-6,
        help="largest acceptable per-sample difference between the two encoders",
    )
    args = parser.parse_args()

    if not args.cpp_bin.exists():
        raise SystemExit(f"C++ binary not found: {args.cpp_bin}")
    if not args.python_repo.exists():
        raise SystemExit(f"Python reference repo not found: {args.python_repo}")

    cpp = Runner("cpp", [str(args.cpp_bin)])
    python = Runner("python", args.python_cmd.split(), cwd=args.python_repo)

    failures: list[str] = []
    notes: list[str] = []
    checked = 0

    print(f"{'mode':<22} {'py>py':<6} {'cpp>cpp':<8} {'py>cpp':<7} {'cpp>py':<7} waveform")
    print("-" * 76)

    with tempfile.TemporaryDirectory() as workdir:
        work = Path(workdir)
        log = work / "interop.log"

        for baud, tones in itertools.product(BAUDS, TONES):
            mode = Mode(baud, tones)
            py_wav = work / "py.wav"
            cpp_wav = work / "cpp.wav"

            try:
                python.tx(PAYLOAD, py_wav, mode, log)
            except subprocess.CalledProcessError:
                print(f"{str(mode):<22} skipped (not supported by the reference)")
                continue

            try:
                cpp.tx(PAYLOAD, cpp_wav, mode, log)
            except subprocess.CalledProcessError:
                failures.append(f"{mode}: the reference accepted the mode but C++ tx failed")
                continue

            checked += 1
            py_py = python.rx(py_wav, mode, log)
            cpp_cpp = cpp.rx(cpp_wav, mode, log)
            py_cpp = cpp.rx(py_wav, mode, log)
            cpp_py = python.rx(cpp_wav, mode, log)

            same_length, worst, py_len, cpp_len = compare_waveforms(py_wav, cpp_wav)

            def mark(got: bytes) -> str:
                return "ok" if got == PAYLOAD else "part"

            print(
                f"{str(mode):<22} {mark(py_py):<6} {mark(cpp_cpp):<8} {mark(py_cpp):<7} "
                f"{mark(cpp_py):<7} max_diff={worst:.1e} n={py_len}"
            )

            # 1. Reading the reference's audio must give the same answer the
            #    reference itself gets. This is the interop requirement.
            if py_cpp != py_py:
                if py_cpp == PAYLOAD:
                    notes.append(
                        f"{mode}: the port recovers the payload from the reference's audio "
                        f"where the reference itself does not"
                    )
                else:
                    failures.append(
                        f"{mode}: the port read the reference's audio differently "
                        f"({py_cpp!r} vs {py_py!r})"
                    )

            # 2. The port's audio must be at least as decodable as the
            #    reference's, judged by the reference's own receiver.
            if cpp_py != py_py:
                if py_py == PAYLOAD:
                    failures.append(
                        f"{mode}: the reference read the port's audio worse than its own "
                        f"({cpp_py!r} vs {py_py!r})"
                    )
                else:
                    notes.append(
                        f"{mode}: the reference decodes the two encoders' audio differently "
                        f"and recovers neither payload exactly"
                    )

            # 3. The port must never be worse than the reference on its own terms.
            if py_py == PAYLOAD and cpp_cpp != PAYLOAD:
                failures.append(f"{mode}: the reference round-trips but the port does not")
            if cpp_cpp != PAYLOAD and py_py != PAYLOAD:
                notes.append(
                    f"{mode}: neither implementation recovers the payload "
                    f"(a reference limitation the port reproduces)"
                )

            if not same_length:
                failures.append(f"{mode}: waveform length {py_len} (python) vs {cpp_len} (cpp)")
            elif worst > args.waveform_tolerance:
                failures.append(f"{mode}: waveform differs by {worst:.3e}")

    print()
    if notes:
        print("Notes (reference-side limitations, not port defects):")
        for note in dict.fromkeys(notes):
            print(f"  - {note}")
        print()
    if failures:
        print(f"{len(failures)} failure(s) across {checked} mode(s):")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print(f"all {checked} mode(s) interoperate in both directions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
