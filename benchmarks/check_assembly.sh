#!/usr/bin/env bash
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
output=${1:-/tmp/cobra-gemm-check.s}

make -C "$root" >/dev/null
"$root/cobra" emit-asm "$root/benchmarks/gemm_repeat.cb" -o "$output" >/dev/null

for instruction in vfmadd231ps vbroadcastss vmovups vzeroupper; do
    if ! grep -q "[[:space:]]$instruction" "$output"; then
        printf 'missing expected instruction: %s\n' "$instruction" >&2
        exit 1
    fi
done

printf 'AVX2/FMA GEMM path verified in %s\n' "$output"
