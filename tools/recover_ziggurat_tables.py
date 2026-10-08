#!/usr/bin/env python3
"""Regenerate ``libs/rng/src/ziggurat_tables.hpp`` from NumPy's own output.

``standard_normal`` draws from a 256-level ziggurat whose tables NumPy compiles
in as constants. Rebuilding those tables from the published Marsaglia-Tsang
construction gets close but lands a few ULPs away, and the resulting samples
then differ from NumPy's in the 12th significant digit -- enough that a test
comparing noise realisations against the Python implementation fails.

So the tables are recovered rather than recomputed:

* ``wi[idx]`` is read straight off the output. A draw that lands inside its
  strip returns ``x = magnitude * wi[idx]`` unchanged, so dividing NumPy's
  result by the magnitude gives the table entry exactly. Replaying the raw
  PCG64 stream alongside the generated normals supplies both halves.
* ``x[idx]`` follows from ``wi[idx] * 2^52``, and ``fi``/``ki`` follow from the
  ``x`` values by the same formulas NumPy's generator script used -- which are
  exact once the ``x`` values are.

The accept/reject boundaries observed while replaying are used to check the
recovered ``ki`` values: every entry has to sit above the largest magnitude
seen accepted and at or below the smallest seen rejected.

Usage::

    python3 tools/recover_ziggurat_tables.py > libs/rng/src/ziggurat_tables.hpp

Requires NumPy. The output is checked in, so this only needs rerunning if the
tables are ever suspected of drifting.
"""

from __future__ import annotations

import math
import sys

import numpy as np
from numpy.random import PCG64, Generator

#: NumPy's ziggurat parameters.
ZIGGURAT_R = 3.6541528853610088
ZIGGURAT_INV_R = 0.27366123732975828
SECTION_AREA = 0.00492867323399

MANTISSA = float(1 << 52)
MANTISSA_MASK = 0x000FFFFFFFFFFFFF

#: Enough draws that every one of the 256 strips is hit many times.
RAW_DRAWS = 4_000_000
SEED = 12345


def seed_tables() -> tuple[list[int], list[float], list[float]]:
    """Tables from the published construction -- close enough to replay with."""
    ki = [0] * 256
    wi = [0.0] * 256
    fi = [0.0] * 256

    x = ZIGGURAT_R
    wi[255] = x / MANTISSA
    fi[255] = math.exp(-0.5 * x * x)
    ki[0] = int(x * fi[255] / SECTION_AREA * MANTISSA)
    wi[0] = SECTION_AREA / fi[255] / MANTISSA
    fi[0] = 1.0
    for i in range(254, 0, -1):
        nxt = math.sqrt(-2.0 * math.log(SECTION_AREA / x + fi[i + 1]))
        ki[i + 1] = int(nxt / x * MANTISSA)
        wi[i] = nxt / MANTISSA
        fi[i] = math.exp(-0.5 * nxt * nxt)
        x = nxt
    ki[1] = 0
    return ki, wi, fi


def recover_wi() -> tuple[list[float], list[int], list[int]]:
    """Replay the raw stream against the generated normals to read off ``wi``."""
    ki, wi, fi = seed_tables()

    raws = PCG64(SEED).random_raw(RAW_DRAWS).astype(np.uint64)
    values = Generator(PCG64(SEED)).standard_normal(RAW_DRAWS // 2)

    def next_double(index: int) -> float:
        return float((int(raws[index]) >> 11) * (1.0 / 9007199254740992.0))

    observed: list[dict[float, int]] = [dict() for _ in range(256)]
    accepted_max = [0] * 256
    rejected_min = [1 << 60] * 256

    position = 0
    emitted = 0
    while position < RAW_DRAWS - 8 and emitted < len(values):
        draw = int(raws[position])
        position += 1
        idx = draw & 0xFF
        rest = draw >> 8
        magnitude = (rest >> 1) & MANTISSA_MASK
        x = magnitude * wi[idx]
        if rest & 1:
            x = -x

        if magnitude < ki[idx]:
            value = values[emitted]
            emitted += 1
            if magnitude:
                candidate = abs(value) / magnitude
                observed[idx][candidate] = observed[idx].get(candidate, 0) + 1
            accepted_max[idx] = max(accepted_max[idx], magnitude)
            continue

        rejected_min[idx] = min(rejected_min[idx], magnitude)
        if idx == 0:
            while True:
                xx = -ZIGGURAT_INV_R * math.log1p(-next_double(position))
                position += 1
                yy = -math.log1p(-next_double(position))
                position += 1
                if yy + yy > xx * xx:
                    emitted += 1
                    break
        else:
            wedge = (fi[idx - 1] - fi[idx]) * next_double(position) + fi[idx]
            position += 1
            if wedge < math.exp(-0.5 * x * x):
                value = values[emitted]
                emitted += 1
                if magnitude:
                    candidate = abs(value) / magnitude
                    observed[idx][candidate] = observed[idx].get(candidate, 0) + 1

    # Fast accepts are ~99% of draws, so the modal candidate is the true entry.
    # The candidates arrive as NumPy scalars; coerce them so the emitted header
    # carries plain float literals.
    recovered = [
        float(max(counts.items(), key=lambda kv: kv[1])[0]) if counts else float(wi[idx])
        for idx, counts in enumerate(observed)
    ]
    return recovered, [int(v) for v in accepted_max], [int(v) for v in rejected_min]


def derive(wi: list[float]) -> tuple[list[int], list[float]]:
    """``ki`` and ``fi`` follow exactly from the recovered ``x`` values."""
    x = [value * MANTISSA for value in wi]
    fi = [1.0] + [math.exp(-0.5 * x[i] * x[i]) for i in range(1, 256)]
    ki = [0] * 256
    ki[0] = int(x[255] * fi[255] / SECTION_AREA * MANTISSA)
    ki[1] = 0
    for i in range(254, 0, -1):
        ki[i + 1] = int(x[i] / x[i + 1] * MANTISSA)
    return ki, fi


def emit(ki: list[int], wi: list[float], fi: list[float]) -> None:
    def rows(values: list[object], suffix: str = "") -> str:
        lines = []
        for start in range(0, 256, 4):
            chunk = ", ".join(f"{v!r}{suffix}" for v in values[start : start + 4])
            lines.append(f"    {chunk},")
        return "\n".join(lines)

    print("#pragma once")
    print()
    print("// NumPy's 256-level ziggurat tables for ``standard_normal``.")
    print("//")
    print("// These are the exact constants NumPy compiles in, recovered from its own")
    print("// output stream rather than regenerated: a table rebuilt from the published")
    print("// construction formula lands a few ULPs away and the resulting samples then")
    print("// differ from NumPy's in the 12th significant digit. Tests that compare noise")
    print("// realisations against the Python implementation need the real values.")
    print("//")
    print("// Generated by tools/recover_ziggurat_tables.py -- do not hand-edit.")
    print()
    print("#include <cstdint>")
    print()
    print("namespace weaklink::rng::detail {")
    print()
    print(f"inline constexpr double kZigguratR = {ZIGGURAT_R!r};")
    print(f"inline constexpr double kZigguratInvR = {ZIGGURAT_INV_R!r};")
    print()
    print("inline constexpr uint64_t kZigguratKi[256] = {")
    print(rows(ki, "ULL"))
    print("};")
    print()
    print("inline constexpr double kZigguratWi[256] = {")
    print(rows(wi))
    print("};")
    print()
    print("inline constexpr double kZigguratFi[256] = {")
    print(rows(fi))
    print("};")
    print()
    print("}  // namespace weaklink::rng::detail")


def main() -> int:
    wi, accepted_max, rejected_min = recover_wi()
    ki, fi = derive(wi)

    # Index 1 is always zero (it forces the rejection branch), so it has no
    # boundary to check.
    inconsistent = [
        i for i in range(256) if i != 1 and not (accepted_max[i] < ki[i] <= rejected_min[i])
    ]
    if inconsistent:
        print(f"ki inconsistent with observed boundaries at {inconsistent}", file=sys.stderr)
        return 1

    emit(ki, wi, fi)
    return 0


if __name__ == "__main__":
    sys.exit(main())
