#!/usr/bin/env python3
"""Run the same dense workload across Cobra, C, Rust, and Python."""

from __future__ import annotations

import argparse
import statistics
import subprocess
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def timed(command: list[str], warmups: int, runs: int) -> list[float]:
    for _ in range(warmups):
        subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
    samples = []
    for _ in range(runs):
        start = time.perf_counter()
        subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
        samples.append(time.perf_counter() - start)
    return samples


def report(name: str, samples: list[float]) -> None:
    median = statistics.median(samples)
    print(f"{name:8} median={median * 1000:8.3f} ms  min={min(samples) * 1000:8.3f} ms  max={max(samples) * 1000:8.3f} ms")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--runs", type=int, default=10)
    args = parser.parse_args()
    if args.warmup < 0 or args.runs < 1:
        parser.error("warmup must be non-negative and runs must be positive")

    c_binary = Path("/tmp/cobra-bench-dense-c")
    rust_binary = Path("/tmp/cobra-bench-dense-rust")
    subprocess.run(["gcc", "-O3", "-march=native", "-ffast-math", "-o", str(c_binary), str(HERE / "dense_c.c")], check=True)
    subprocess.run(["rustc", "-C", "opt-level=3", "-C", "target-cpu=native", "-o", str(rust_binary), str(HERE / "dense_rust.rs")], check=True)
    cobra_binary = Path("/tmp/cobra-bench-dense-cobra")
    subprocess.run([str(ROOT / "cobra"), "build", str(ROOT / "benchmarks/dense_repeat.cb"), "-o", str(cobra_binary)], check=True, stdout=subprocess.DEVNULL)

    commands = {
        "C": [str(c_binary)],
        "Rust": [str(rust_binary)],
        "Cobra": [str(cobra_binary)],
        "Python": ["python3", str(HERE / "dense_python.py")],
    }
    print("Dense workload: 1x64 input, 64x128 weights, 1x128 output, 1,000 iterations")
    print("Process wall time includes startup. C and Rust use AVX2/FMA, matching Cobra's native kernel path.")
    results = {}
    for name, command in commands.items():
        results[name] = timed(command, args.warmup, args.runs)
        report(name, results[name])

    baseline = statistics.median(results["Cobra"])
    for name, samples in results.items():
        print(f"{name:8} vs Cobra: {statistics.median(samples) / baseline:6.2f}x")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
