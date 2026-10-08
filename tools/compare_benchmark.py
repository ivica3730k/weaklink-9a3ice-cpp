#!/usr/bin/env python3
"""Compare the port's SNR cliffs against the reference implementation's.

Decoding the same bytes is necessary but not sufficient: the port could be
wire compatible and still be a dB or two worse in noise if the soft-decision
chain lost precision somewhere. The cliff numbers are what catch that.

Both sides sweep the same configurations and record the lowest SNR at which
every trial still decodes the payload byte for byte, so the figures are
directly comparable. The sweep is coarse -- 1 dB steps, a handful of trials --
so a one-step difference is sampling noise rather than a regression; anything
wider is worth looking at.

Usage::

    weaklink-modem-benchmark --bauds 300 --trials 5 --dry-run > cpp.md
    python3 tools/compare_benchmark.py cpp.md ../weaklink-9a3ice/results.md
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

#: A row's identity: everything that changes the measured cliff.
Config = tuple[int, int, int, int, int]  # baud, tones, rs_data, rs_parity, repeats

FLAG = {
    "baud": re.compile(r"--modem-baud (\d+)"),
    "tones": re.compile(r"--modem-num-tones (\d+)"),
    "rs_data": re.compile(r"--modem-rs-data-bytes (\d+)"),
    "rs_parity": re.compile(r"--modem-rs-parity-bytes (\d+)"),
    "repeats": re.compile(r"--modem-block-repeats (\d+)"),
}
CLIFF = re.compile(r"\*\*([+-]?\d+) dB\*\*")


def parse(path: Path) -> dict[Config, int | None]:
    """Pull (config -> cliff) out of a results table, ignoring everything else."""
    rows: dict[Config, int | None] = {}
    for line in path.read_text().splitlines():
        if not line.startswith("|") or "--modem-baud" not in line:
            continue
        try:
            config: Config = tuple(  # type: ignore[assignment]
                int(FLAG[key].search(line).group(1))  # type: ignore[union-attr]
                for key in ("baud", "tones", "rs_data", "rs_parity", "repeats")
            )
        except AttributeError:
            continue  # a row that does not carry a full CLI snippet
        cliff = CLIFF.search(line)
        rows[config] = int(cliff.group(1)) if cliff else None
    return rows


def describe(config: Config) -> str:
    baud, tones, rs_data, rs_parity, repeats = config
    return f"{baud:>4} baud  {tones:>2} tones  RS({rs_data},{rs_parity})  x{repeats}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", type=Path, help="the C++ benchmark's --dry-run output")
    parser.add_argument("reference", type=Path, help="the Python project's results.md")
    parser.add_argument(
        "--tolerance",
        type=int,
        default=1,
        help="dB difference treated as sampling noise rather than a regression",
    )
    args = parser.parse_args()

    port = parse(args.port)
    reference = parse(args.reference)

    shared = sorted(set(port) & set(reference))
    if not shared:
        print("no configurations in common -- check that both files are results tables")
        return 1

    worse: list[str] = []
    better: list[str] = []
    print(f"{'configuration':<40} {'reference':>10} {'port':>8} {'delta':>8}")
    print("-" * 70)
    for config in shared:
        want, got = reference[config], port[config]
        if want is None or got is None:
            verdict = "n/a"
            delta_text = "-"
        else:
            delta = got - want  # positive means the port needs more SNR
            delta_text = f"{delta:+d} dB"
            if delta > args.tolerance:
                verdict = "WORSE"
                worse.append(f"{describe(config)}: {want:+d} -> {got:+d} dB")
            elif delta < -args.tolerance:
                verdict = "better"
                better.append(f"{describe(config)}: {want:+d} -> {got:+d} dB")
            else:
                verdict = "ok"
        want_text = f"{want:+d} dB" if want is not None else "none"
        got_text = f"{got:+d} dB" if got is not None else "none"
        print(f"{describe(config):<40} {want_text:>10} {got_text:>8} {delta_text:>8}  {verdict}")

    print()
    print(f"{len(shared)} configurations compared, tolerance +/-{args.tolerance} dB")
    if better:
        print(f"\n{len(better)} better than the reference:")
        for line in better:
            print(f"  - {line}")
    if worse:
        print(f"\n{len(worse)} WORSE than the reference:")
        for line in worse:
            print(f"  - {line}")
        return 1
    print("\nno configuration is worse than the reference")
    return 0


if __name__ == "__main__":
    sys.exit(main())
