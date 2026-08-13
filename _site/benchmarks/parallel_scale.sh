#!/usr/bin/env bash
# Parallel worker-pool scaling measurements for @parallel.
#
# Covers, in order:
#   1. Worker sweep (1/2/4/8) on the compute-bound poly kernel
#   2. Worker sweep on the memory-bound affine kernel
#   3. Dispatch overhead by input size at full concurrency
#   4. Pool reuse: one-pass process vs amortized per-call cost
#
# Every number is tied to this machine and compiler revision. Rerun on the
# target host before quoting any result; do not treat one run as universal.
set -e
cd "$(dirname "$0")/.."

HOST=$(hostname)
REV=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
CORES=$(nproc)
echo "host=$HOST cores=$CORES rev=$REV"

make >/dev/null 2>&1
./cobra build benchmarks/parallel_poly.cb -o /tmp/cobra_poly >/dev/null 2>&1
./cobra build benchmarks/parallel_affine.cb -o /tmp/cobra_affine >/dev/null 2>&1

./cobra emit-asm benchmarks/parallel_poly.cb -o /tmp/cobra_poly.s >/dev/null 2>&1
./cobra emit-asm benchmarks/parallel_affine.cb -o /tmp/cobra_affine.s >/dev/null 2>&1
echo "poly dispatches:  $(grep -c 'call cobra_parallel_for' /tmp/cobra_poly.s)"
echo "affine dispatches: $(grep -c 'call cobra_parallel_for' /tmp/cobra_affine.s)"

# Pin to the first W physical cores when taskset exists; SMT siblings stay
# idle so hyperthread contention does not flatten the scaling curve. On hosts
# without taskset the run still works, just noisier.
PIN=
if command -v taskset >/dev/null 2>&1; then
    PIN="taskset -c"
fi

# min_of_5 WORKERS BIN -> min wall seconds across 5 runs
min_of_5() {
    local workers=$1 bin=$2 best=999999 cpus
    if [ -n "$PIN" ]; then
        cpus=$(seq -s, 0 $((workers - 1)))
    fi
    for i in 1 2 3 4 5; do
        local t
        if [ -n "$PIN" ]; then
            t=$(COBRA_WORKERS="$workers" taskset -c "$cpus" /usr/bin/time -f '%e' "$bin" 2>&1 >/dev/null | tail -1)
        else
            t=$(COBRA_WORKERS="$workers" /usr/bin/time -f '%e' "$bin" 2>&1 >/dev/null | tail -1)
        fi
        best=$(echo "$t $best" | awk '{print ($1<$2)?$1:$2}')
    done
    echo "$best"
}

echo
echo "=== 1. compute-bound poly, 8.4M elems x 30 passes (total seconds) ==="
for w in 1 2 4 8; do
    printf "  workers=%-2s %ss\n" "$w" "$(min_of_5 "$w" /tmp/cobra_poly)"
done

echo
echo "=== 2. memory-bound affine, 33.5M elems x 30 passes (total seconds) ==="
for w in 1 2 4 8; do
    printf "  workers=%-2s %ss\n" "$w" "$(min_of_5 "$w" /tmp/cobra_affine)"
done

echo
echo "=== 3. dispatch overhead by size (affine, 8 workers, 50 passes) ==="
WORK=$(mktemp -d /tmp/cobra_parsize.XXXXXX)
trap 'rm -rf "$WORK"' EXIT
for size in 1024 4096 65536 1048576 16777216; do
    sed "s/n = 33554432/n = $size/; s/range(30)/range(50)/" benchmarks/parallel_affine.cb > "$WORK/a.cb"
    ./cobra build "$WORK/a.cb" -o "$WORK/a" >/dev/null 2>&1
    t=$(COBRA_WORKERS=8 /usr/bin/time -f '%e' "$WORK/a" 2>&1 >/dev/null | tail -1)
    per=$(echo "$t 50" | awk '{printf "%.4f", $1/$2}')
    printf "  %-10s elems: %ss total, %ss per call\n" "$size" "$t" "$per"
done

echo
echo "=== 4. pool reuse: one-pass process vs amortized per-call ==="
sed "s/range(30)/range(1)/" benchmarks/parallel_poly.cb > "$WORK/p1.cb"
./cobra build "$WORK/p1.cb" -o "$WORK/p1" >/dev/null 2>&1
t1=$(COBRA_WORKERS=8 /usr/bin/time -f '%e' "$WORK/p1" 2>&1 >/dev/null | tail -1)
t30=$(min_of_5 8 /tmp/cobra_poly)
per=$(echo "$t30 30" | awk '{printf "%.4f", $1/$2}')
echo "  1 pass, cold pool:  ${t1}s (includes pool spawn)"
echo "  30 passes, warm:    ${t30}s total, ${per}s per call"
