#!/usr/bin/env bash
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source=${1:-benchmarks/gemm_repeat.cb}
work=$(mktemp -d "${TMPDIR:-/tmp}/cobra-profile.XXXXXX")
trap 'rm -rf "$work"' EXIT

if ! command -v perf >/dev/null 2>&1; then
    printf '%s\n' 'perf is not installed; install linux-tools to profile generated programs.' >&2
    exit 2
fi

make -C "$root" >/dev/null
binary="$work/cobra-workload"
"$root/cobra" build "$root/$source" -o "$binary" >/dev/null

printf 'Profiling generated program: %s\n' "$source"
printf '%s\n' 'Counters: cycles, instructions, branches, branch-misses, cache-misses'
set +e
perf stat -e cycles,instructions,branches,branch-misses,cache-misses -- "$binary" 2>"$work/perf.log"
status=$?
set -e
cat "$work/perf.log" >&2
if [ "$status" -ne 0 ]; then
    if grep -Eq 'perf_event_paranoid|Access to performance monitoring|CAP_PERFMON' "$work/perf.log"; then
        printf '%s\n' 'hardware counters unavailable in this environment; generated program was built but not profiled.' >&2
        exit 2
    fi
    exit "$status"
fi
