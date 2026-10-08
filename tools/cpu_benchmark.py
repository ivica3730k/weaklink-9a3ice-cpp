#!/usr/bin/env python3
"""Measure the CPU cost of the C++ port against the Python reference.

Both implementations encode the same payload to a WAV and decode that WAV back,
at every supported baud. What is reported is CPU time (user + system) charged to
the child process, taken from ``wait4`` rather than the wall clock, so a busy
machine does not flatter either side.

Each measurement is the minimum of several runs. Minimum rather than mean: the
quantity being estimated is the work the implementation has to do, and anything
above the floor is interference from the rest of the machine.

Decoding is the honest comparison -- it is the receiver that has to keep up with
audio arriving in real time. Encoding is reported too, but it is small enough
that interpreter startup dominates the Python side, which the baseline row makes
explicit.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

DEFAULT_PYTHON_REPO = Path(__file__).resolve().parents[2] / "weaklink-9a3ice"

BAUDS = (45.0, 300.0, 1200.0)
NUM_TONES = 4
PAYLOAD = b"weaklink cpu benchmark payload 0123456789 " * 5


@dataclass
class Runner:
    name: str
    argv: list[str]


def cpu_seconds(argv: list[str], stdin: bytes | None = None) -> tuple[float, bytes]:
    """Run argv and return the CPU seconds it was charged, plus its stdout."""
    process = subprocess.Popen(
        argv,
        stdin=subprocess.PIPE if stdin is not None else subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    # communicate() reaps the child itself, which would leave wait4 nothing to
    # report, so the pipes are drained by hand and the wait is done here.
    if stdin is not None:
        assert process.stdin is not None
        process.stdin.write(stdin)
        process.stdin.close()
    assert process.stdout is not None
    out = process.stdout.read()
    process.stdout.close()
    _, status, usage = os.wait4(process.pid, 0)
    process.returncode = os.waitstatus_to_exitcode(status)
    if process.returncode != 0:
        raise SystemExit(f"{argv[0]} exited {process.returncode}: {' '.join(argv)}")
    return usage.ru_utime + usage.ru_stime, out


def best_of(repeats: int, argv: list[str], stdin: bytes | None = None) -> tuple[float, bytes]:
    best = float("inf")
    out = b""
    for _ in range(repeats):
        seconds, out = cpu_seconds(argv, stdin)
        best = min(best, seconds)
    return best, out


def ratio(slow: float, fast: float) -> str:
    return "n/a" if fast <= 0 else f"{slow / fast:.0f}x"


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
        default=None,
        help="how to invoke the Python CLI (default: the reference repo's .venv entry point)",
    )
    parser.add_argument("--repeats", type=int, default=3, help="runs per measurement (min is kept)")
    args = parser.parse_args()

    if not args.cpp_bin.exists():
        raise SystemExit(f"no such binary: {args.cpp_bin}")

    python_cmd = args.python_cmd or str(args.python_repo / ".venv" / "bin" / "weaklink-modem")
    cpp = Runner("c++", [str(args.cpp_bin.resolve())])
    python = Runner("python", python_cmd.split())

    print(f"payload: {len(PAYLOAD)} bytes | {NUM_TONES} tones | min of {args.repeats} runs\n")

    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        log = work / "log.txt"
        for baud in BAUDS:
            mode = ["--modem-baud", f"{baud:g}", "--modem-num-tones", str(NUM_TONES)]
            common = [*mode, "--modem-log-file", str(log)]
            wavs = {}
            tx = {}
            for runner in (cpp, python):
                wav = work / f"{runner.name}-{baud:g}.wav"
                tx[runner.name], _ = best_of(
                    args.repeats, [*runner.argv, "tx", "--modem-wav", str(wav), *common], PAYLOAD
                )
                wavs[runner.name] = wav

            # Both decode the C++ encoder's WAV: identical audio in, so the only
            # difference left is what each implementation spends reading it.
            rx = {}
            for runner in (cpp, python):
                rx[runner.name], out = best_of(
                    args.repeats,
                    [*runner.argv, "rx", "--modem-wav", str(wavs["c++"]), *common],
                )
                if out.rstrip(b"\x00") != PAYLOAD:
                    print(f"  warning: {runner.name} rx did not recover the payload at {baud:g} baud")

            seconds = wavs["c++"].stat().st_size / (4 * 48000)
            rows.append((baud, seconds, tx, rx))
            print(
                f"{baud:>6g} baud  ({seconds:6.1f} s audio)  "
                f"tx {tx['c++']:6.2f} / {tx['python']:6.2f}   "
                f"rx {rx['c++']:6.2f} / {rx['python']:6.2f}  (c++ / python CPU s)"
            )

        startup = {r.name: best_of(args.repeats, [*r.argv, "--version"])[0] for r in (cpp, python)}

    print(
        "\n| Baud | Audio | Encode CPU (C++ / Python) | Decode CPU (C++ / Python) "
        "| Decode load at real time (C++ / Python) | Speedup |"
    )
    print("| --- | --- | --- | --- | --- | --- |")
    for baud, seconds, tx, rx in rows:
        print(
            f"| {baud:g} | {seconds:.1f} s | {tx['c++']:.2f} s / {tx['python']:.2f} s "
            f"| {rx['c++']:.2f} s / {rx['python']:.2f} s "
            f"| {100 * rx['c++'] / seconds:.2f}% / {100 * rx['python'] / seconds:.1f}% of one core "
            f"| {ratio(rx['python'], rx['c++'])} |"
        )
    print(
        f"\nprocess startup (--version): c++ {startup['c++']:.2f} s, "
        f"python {startup['python']:.2f} s CPU"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
