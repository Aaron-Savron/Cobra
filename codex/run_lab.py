#!/usr/bin/env python3
"""Run the Cobra language lab without hiding individual failures."""

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
COBRA = ROOT / "cobra"
SCENARIOS = [
    ROOT / "codex/01_workbench.cb",
    ROOT / "codex/02_data_pipeline.cb",
    ROOT / "codex/03_module_app.cb",
    ROOT / "codex/04_http_service.cb",
    ROOT / "codex/05_status_flow.cb",
]


def run(label: str, command: list[str], *, allow_failure: bool = False) -> bool:
    print(f"\n== {label} ==")
    print("$ " + " ".join(command))
    result = subprocess.run(command, cwd=ROOT, text=True)
    ok = result.returncode == 0
    if not ok and not allow_failure:
        print(f"FAILED: {label}")
    return ok


def main() -> int:
    if not COBRA.exists():
        print("cobra executable is missing. Run make first.", file=sys.stderr)
        return 2

    failures = 0
    for source in SCENARIOS:
        if not run(f"check {source.name}", [str(COBRA), "check", str(source)]):
            failures += 1

    for source in SCENARIOS[:3] + [SCENARIOS[4]]:
        if not run(f"test {source.name}", [str(COBRA), "test", str(source)]):
            failures += 1

    if not run("build http service", [str(COBRA), "build", str(SCENARIOS[3]), "-o", "/tmp/cobra-codex-http"]):
        failures += 1

    negative = ROOT / "codex/negative/readonly_rebind.cb"
    print(f"\n== expected rejection {negative.name} ==")
    result = subprocess.run([str(COBRA), "check", str(negative)], cwd=ROOT, text=True)
    if result.returncode == 0:
        print("FAILED: invalid readonly write was accepted")
        failures += 1
    else:
        print("PASS: invalid readonly write was rejected")

    if shutil.which("python3") and (ROOT / "benchmarks/cross_language/run.py").exists():
        benchmark = ROOT / "benchmarks/cross_language/run.py"
        if not run(
            "cross-language benchmark",
            ["python3", str(benchmark), "--warmup", "2", "--runs", "5"],
            allow_failure=True,
        ):
            print("Benchmark comparison was unavailable or failed. The language lab still completed.")

    print(f"\nLab complete: {len(SCENARIOS) + 6 - failures} required steps passed, {failures} failed.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
