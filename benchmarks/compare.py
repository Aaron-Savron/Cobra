#!/usr/bin/env python3
"""Compare Cobra native process timing with an equivalent Python workload.

This is deliberately a transparent workload comparator, not a universal speed
claim. Both sides execute the same 1x64 by 64x128 dense layer 1,000 times.
Compilation is excluded; Cobra's process-start timing includes native startup,
matching `cobra bench`.
"""
from __future__ import annotations

import argparse
import statistics
import subprocess
import time
from pathlib import Path


def samples(command: list[str], warmups: int, runs: int) -> list[float]:
    for _ in range(warmups):
        subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
    values: list[float] = []
    for _ in range(runs):
        start = time.perf_counter()
        subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
        values.append(time.perf_counter() - start)
    return values


def python_workload(iterations: int) -> None:
    # Keep the reference intentionally dependency-free and explicit. This is a
    # scalar Python baseline, not NumPy or a framework implementation.
    x = [1.0] * 64
    w = [0.5] * (64 * 128)
    bias = [1.0] * 128
    for _ in range(iterations):
        out = [0.0] * 128
        for j in range(128):
            total = bias[j]
            for k in range(64):
                total += x[k] * w[k * 128 + j]
            out[j] = total
    assert abs(sum(out) - 4224.0) < 1e-5


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", type=int, default=10)
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--iterations", type=int, default=1000,
                        help="must remain 1000 because dense_repeat.cb is fixed")
    parser.add_argument("--cobra", default="./cobra")
    args = parser.parse_args()
    if args.runs < 1 or args.warmup < 0 or args.iterations != 1000:
        parser.error("runs must be positive; warmup non-negative; iterations must be 1000")

    root = Path(__file__).resolve().parents[1]
    workload = root / "benchmarks" / "dense_repeat.cb"
    cobra = [args.cobra, "bench", str(workload), "--warmup", str(args.warmup), "--runs", str(args.runs)]
    print("Cobra benchmark (native process wall time):")
    subprocess.run(cobra, check=True)

    for _ in range(args.warmup):
        python_workload(args.iterations)
    py_values: list[float] = []
    for _ in range(args.runs):
        start = time.perf_counter()
        python_workload(args.iterations)
        py_values.append(time.perf_counter() - start)
    print("Python reference (scalar, same dimensions/repetitions):")
    print(f"  median: {statistics.median(py_values) * 1000:.3f} ms")
    print(f"  min:    {min(py_values) * 1000:.3f} ms")
    print(f"  max:    {max(py_values) * 1000:.3f} ms")
    print("Record CPU, OS, Cobra revision, and whether startup is included before comparing runs.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
