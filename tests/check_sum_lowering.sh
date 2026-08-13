#!/usr/bin/env bash
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

sum_asm=$(mktemp /tmp/cobra-sum-lowering.XXXXXX.s)
status_asm=$(mktemp /tmp/cobra-status-lowering.XXXXXX.s)
trap 'rm -f "$sum_asm" "$status_asm"' EXIT

./cobra emit-asm examples/62_sum_interactions.cb -o "$sum_asm" >/dev/null
./cobra emit-asm examples/37_structured_status.cb -o "$status_asm" >/dev/null

grep -q 'cmp QWORD PTR \[rax\], 1' "$sum_asm"
# The interaction suite contains an unsupported call and accumulator inside
# @parallel. It must keep scalar lowering rather than dispatching a worker.
parallel_body=$(awk '/^parallel_fallback:/{inside=1} inside{print} /^\.size parallel_fallback/{inside=0}' "$sum_asm")
if grep -q 'cobra_parallel_for' <<< "$parallel_body"; then
    echo "unexpected worker dispatch for unsupported @parallel body" >&2
    exit 1
fi
# Typed failure inside the loop-region function must retain the arena cleanup
# path before it reaches the shared propagation epilogue.
region_body=$(awk '/^loop_region_result:/{inside=1} inside{print} /^\.size loop_region_result/{inside=0}' "$sum_asm")
grep -q 'arena_destroy@PLT' <<< "$region_body"

grep -q 'test rax, rax' "$status_asm"
printf '%s\n' 'typed and integer propagation lowering checks passed'
