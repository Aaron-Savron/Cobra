#!/usr/bin/env python3
"""Measure Cobra's generated programs and optionally enforce same-host gates.

The measurements belong to the generated programs, not the compiler process.
Core categories are timed inside one native Cobra workload. GEMM, reductions,
user-model code, and parallel work use the existing in-process workloads and
Cobra's native benchmark runner. Store the JSON output for a machine, then
compare a later run with --baseline on that same machine.

Timing gates are opt-in because shared CI runners are noisy. Assembly
contracts are deterministic and should run on every CI build.
"""
from __future__ import annotations

import argparse
import json
import platform
import re
import statistics
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CORE_SOURCE = ROOT / "benchmarks" / "perf_baseline.cb"

# These are deliberately small, but not identical: sums and structs have more
# representation work than plain scalar and slice paths.
TOLERANCES = {
    "scalar_calls": 0.02,
    "slice_loops": 0.02,
    "struct_copies": 0.05,
    "option_result": 0.05,
    "region_alloc": 0.05,
    "http_parse": 0.05,
    "file_transform": 0.05,
    "gemm": 0.05,
    "reduction": 0.05,
    "user_model": 0.05,
    "parallel": 0.05,
}


def run(command: list[str], *, capture: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=ROOT,
        check=True,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.STDOUT if capture else None,
    )


def cpu_name() -> str:
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.lower().startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or platform.machine()


def revision() -> str:
    try:
        return run(["git", "rev-parse", "HEAD"]).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def parse_core_output(output: str) -> dict[str, float]:
    lines = [line.strip() for line in output.splitlines() if line.strip()]
    if len(lines) % 2:
        raise RuntimeError(f"core baseline output is not name/value pairs:\n{output}")
    values: dict[str, float] = {}
    for index in range(0, len(lines), 2):
        name, raw = lines[index], lines[index + 1]
        if not re.fullmatch(r"[a-z_]+", name):
            raise RuntimeError(f"unexpected core baseline label: {name}")
        try:
            values[name] = float(raw)
        except ValueError as exc:
            raise RuntimeError(f"invalid timing for {name}: {raw}") from exc
    expected = {
        "scalar_calls",
        "slice_loops",
        "struct_copies",
        "option_result",
        "region_alloc",
        "http_parse",
        "file_transform",
    }
    missing = expected - values.keys()
    if missing:
        raise RuntimeError(f"core baseline missing measurements: {sorted(missing)}")
    return values


def measure_core(binary: Path, runs: int, warmup: int) -> dict[str, object]:
    for _ in range(warmup):
        run([str(binary)])
    samples: dict[str, list[float]] = {}
    for _ in range(runs):
        output = run([str(binary)]).stdout
        values = parse_core_output(output)
        for name, value in values.items():
            samples.setdefault(name, []).append(value)
    return {
        name: {
            "unit": "us",
            "samples": values,
            "median": statistics.median(values),
            "p95": sorted(values)[max(0, (len(values) * 95 + 99) // 100 - 1)],
        }
        for name, values in sorted(samples.items())
    }


def parse_bench_stats(output: str) -> dict[str, float]:
    stats: dict[str, float] = {}
    for name in ("min", "median", "p95", "max", "mean"):
        match = re.search(rf"^\[bench\] {name}:\s+([0-9.]+) ms$", output, re.MULTILINE)
        if not match:
            raise RuntimeError(f"could not find benchmark {name} in:\n{output}")
        stats[name] = float(match.group(1)) * 1000.0
    return stats


def measure_workload(name: str, source: str, runs: int, warmup: int) -> dict[str, object]:
    command = ["./cobra", "bench", str(ROOT / source), "--warmup", str(warmup), "--runs", str(runs)]
    stats = parse_bench_stats(run(command).stdout)
    return {"unit": "us", "sample_count": runs, "source": source, **stats}


def compare(current: dict[str, object], baseline_path: Path) -> list[str]:
    baseline = json.loads(baseline_path.read_text())
    failures: list[str] = []
    if baseline.get("schema") != 1:
        failures.append("baseline schema is not supported")

    current_host = current["host"]
    baseline_host = baseline.get("host", {})
    for field in ("machine", "cpu"):
        if baseline_host.get(field) != current_host.get(field):
            failures.append(
                f"baseline host mismatch for {field}: "
                f"{baseline_host.get(field)!r} != {current_host.get(field)!r}"
            )

    old = baseline.get("measurements", {})
    current_measurements = current["measurements"]
    missing = sorted(set(current_measurements) - set(old))
    if missing:
        failures.append(f"baseline is missing measurements: {', '.join(missing)}")
    unexpected = sorted(set(old) - set(current_measurements))
    if unexpected:
        failures.append(f"current run is missing measurements: {', '.join(unexpected)}")

    for name, value in current_measurements.items():
        if name not in old:
            continue
        current_median = float(value["median"])
        old_median = float(old[name]["median"])
        if old_median <= 0:
            failures.append(f"baseline has non-positive median for {name}")
            continue
        increase = current_median / old_median - 1.0
        tolerance = TOLERANCES.get(name, 0.05)
        if increase > tolerance:
            failures.append(
                f"{name}: {old_median:.2f} us -> {current_median:.2f} us "
                f"({increase * 100:.1f}% slower, limit {tolerance * 100:.1f}%)"
            )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runs", type=int, default=5, help="samples per workload")
    parser.add_argument("--warmup", type=int, default=1, help="warmup samples per workload")
    parser.add_argument("--output", type=Path, help="write JSON measurements to this path")
    parser.add_argument("--baseline", type=Path, help="compare medians against a prior JSON run")
    parser.add_argument("--strict", action="store_true", help="fail when a configured tolerance is exceeded")
    parser.add_argument("--core-only", action="store_true", help="skip existing GEMM, reduction, model, and parallel workloads")
    args = parser.parse_args()
    if args.runs < 1 or args.warmup < 0:
        parser.error("runs must be positive and warmup must be non-negative")

    with tempfile.TemporaryDirectory(prefix="cobra-perf-") as directory:
        binary = Path(directory) / "core-baseline"
        run(["./cobra", "build", str(CORE_SOURCE), "-o", str(binary)], capture=True)
        measurements = measure_core(binary, args.runs, args.warmup)

    if not args.core_only:
        workloads = {
            "gemm": "benchmarks/gemm_repeat.cb",
            "reduction": "benchmarks/reduction_repeat.cb",
            "user_model": "benchmarks/user_model_repeat.cb",
            "parallel": "benchmarks/parallel_poly.cb",
        }
        for name, source in workloads.items():
            measurements[name] = measure_workload(name, source, args.runs, args.warmup)

    result: dict[str, object] = {
        "schema": 1,
        "captured_at": datetime.now(timezone.utc).isoformat(),
        "revision": revision(),
        "host": {
            "system": platform.platform(),
            "machine": platform.machine(),
            "cpu": cpu_name(),
        },
        "runs": args.runs,
        "warmup": args.warmup,
        "measurements": measurements,
        "tolerances": TOLERANCES,
    }
    rendered = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered)
    print(rendered, end="")

    if args.baseline:
        failures = compare(result, args.baseline)
        if failures:
            print("Performance gates failed:", file=sys.stderr)
            print("\n".join(f"  {failure}" for failure in failures), file=sys.stderr)
            if args.strict:
                return 1
        else:
            print("Performance gates passed.", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
