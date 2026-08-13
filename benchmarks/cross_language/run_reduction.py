#!/usr/bin/env python3
"""Run the fixed reduction workload across Cobra, C, Rust, and Python."""

from __future__ import annotations

import argparse
import statistics
import subprocess
from pathlib import Path

from run import report, timed


ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--runs", type=int, default=10)
    args = parser.parse_args()
    c_binary = Path("/tmp/cobra-bench-reduction-c")
    rust_binary = Path("/tmp/cobra-bench-reduction-rust")
    cobra_binary = Path("/tmp/cobra-bench-reduction-cobra")
    subprocess.run(["gcc", "-O3", "-march=native", "-ffast-math", "-o", str(c_binary), str(HERE / "reduction_c.c")], check=True)
    subprocess.run(["rustc", "-C", "opt-level=3", "-C", "target-cpu=native", "-o", str(rust_binary), str(HERE / "reduction_rust.rs")], check=True)
    subprocess.run([str(ROOT / "cobra"), "build", str(ROOT / "benchmarks/reduction_repeat.cb"), "-o", str(cobra_binary)], check=True, stdout=subprocess.DEVNULL)

    commands = {
        "C": [str(c_binary)],
        "Rust": [str(rust_binary)],
        "Cobra": [str(cobra_binary)],
        "Python": ["python3", str(HERE / "reduction_python.py")],
    }
    print("Reduction workload: 16,384 f32 values, sum/mean/max, 1,500 iterations")
    print("Process wall time includes startup. Every implementation checks the result.")
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
