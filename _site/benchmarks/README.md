# Cobra Native Performance Benchmarks

The benchmark target is the generated program, not the compiler source. Keep results tied to a machine and compiler revision; do not claim a universal speedup from one run.

## Baseline and contract suite

Capture the broad native baseline:

```bash
make perf-baseline
python3 benchmarks/performance_baseline.py --runs 5 --warmup 1 --output /tmp/cobra-baseline.json
```

The baseline covers scalar calls, slice loops, scalar struct copies, `Option` and `Result`, region allocation, HTTP parsing, file transforms, GEMM, reductions, user-authored model composition, and `@parallel`. Core sections retain raw samples; the existing benchmark workloads contribute min, median, p95, max, mean, and sample count. Every report records host CPU and the Cobra revision. Compare a later run on the same host with:

```bash
python3 benchmarks/performance_baseline.py --baseline /tmp/cobra-baseline.json --strict
```

The strict mode uses 2 percent limits for scalar calls and slices, and 5 percent limits for structs, sums, regions, I/O, and kernels. Run strict comparisons on a pinned CPU or dedicated runner. Interactive laptops and shared CI runners can show scheduler and frequency noise, so CI captures the full report but does not fail on timing. It does run the deterministic assembly contracts:

```bash
make perf-contracts
```

The contracts check that scalar sums and enums do not allocate, slices keep pointer-plus-length lowering, simple matches stay compares and branches, `?` stays a direct tag branch, regions clean up at required exits, user loops still emit AVX2, and constant-shape GEMM remains unrolled. They intentionally avoid exact registers, offsets, instruction order, and cycle counts.

For hardware-counter profiles of the generated binary, use Linux `perf` locally:

```bash
make perf-profile PROFILE_SOURCE=benchmarks/gemm_repeat.cb
make perf-profile PROFILE_SOURCE=benchmarks/parallel_poly.cb
```

The profiler reports cycles, instructions, branches, branch misses, and cache misses for the generated workload. It does not profile the Cobra compiler, and it is intentionally not a CI requirement because hosted runners may restrict hardware counters.


## Correctness first

```bash
make
./cobra test examples/21_dense_f32.cb
./cobra emit-asm benchmarks/gemm_repeat.cb -o /tmp/cobra_dense.s
grep -E 'vfmadd231ps|vbroadcastss|vmovups|vzeroupper' /tmp/cobra_dense.s
./benchmarks/check_assembly.sh
```

## Timing a built binary

```bash
/usr/bin/time -f '%e seconds' ./cobra build benchmarks/dense_repeat.cb -o /tmp/cobra_dense
/usr/bin/time -f '%e seconds' /tmp/cobra_dense
```

For reproducible samples, use Cobra's native benchmark runner:

```bash
./cobra bench benchmarks/dense_repeat.cb --warmup 3 --runs 20
```

`cobra bench` validates the workload by requiring every native process to exit successfully, performs warmups before collecting samples, and reports min/median/p95/max/mean using `CLOCK_MONOTONIC`. Its measurement is explicitly **native process wall time**; compilation is not included. The workload repeats kernels inside the process, so startup is amortized rather than measured as the kernel itself. For cycle-level kernel profiling, use the emitted assembly with `perf` or a separate C harness.

For the reduction milestone, use the in-process reduction workload:

```bash
./cobra test examples/33_reduction_regressions.cb
./cobra emit-asm benchmarks/reduction_repeat.cb -o /tmp/cobra_reduction.s
grep -E 'vaddps|vmaxps|vhaddps|vextractf128|vzeroupper' /tmp/cobra_reduction.s
./cobra bench benchmarks/reduction_repeat.cb --warmup 3 --runs 20
./cobra bench benchmarks/user_model_repeat.cb --warmup 3 --runs 20
```

The user-model workload keeps all intermediate buffers caller-owned and measures a real Cobra-authored MLP forward pass rather than a special model API.

The constant-shape milestone added straight-line K unrolling (K a multiple of four up to 64): the const kernel's hot path is pure broadcast-load-FMA with immediate displacements, no counter, no branch, and a register tile counter. The existing large-memory workloads stay flat because they are strided-B bandwidth bound, which no branch removal changes. The measurable win is on compute-bound small-K shapes: a repeated `1x16x4` dense call with 50M iterations runs 0.34s unrolled versus 0.50s looped on this host. The milestone also fixed a latent emitter bug where a batched constant shape with N==8 fell through after row 0.

The generic output-blocking milestone keeps dynamic row-major buffers unchanged but computes two adjacent eight-column tiles together when sixteen columns remain. One A broadcast feeds both B tiles and eight accumulators, with the existing eight-column and scalar tails handling the remainder. On this host, `benchmarks/gemm_dynamic_blocked_repeat.cb` ran 0.10s with blocking versus 0.12s with the previous generic emitter across 2,000,000 iterations of a runtime-shaped 2x24x6 workload. Treat that as a narrow compiler comparison, not a universal GEMM result. The benchmark's correctness assertions remain part of the measurement.

For a transparent scalar-Python comparison using the same dimensions and repetition count:

```bash
python3 benchmarks/compare.py --warmup 3 --runs 10 --iterations 1000
```

The comparator intentionally uses only Python's standard library. It is a reproducible reference workload, not a claim that Cobra beats optimized NumPy, PyTorch, or vendor BLAS. For framework comparisons, preserve the same data layout, warmup policy, synchronization rules, and compilation/startup accounting.

`benchmarks/dense_repeat.cb` preallocates one `1×64` input, one `64×128` weight matrix, one 128-element bias, and one output, then repeats the fused layer 1,000 times. `benchmarks/gemm_repeat.cb` repeats both `4×32 · 32×16` matmul and fused dense kernels 2,000 times per process. `benchmarks/gemm_dynamic_blocked_repeat.cb` keeps `M`, `N`, and `K` in runtime locals so it measures the generic output-blocked path. `benchmarks/user_model_repeat.cb` is the foundation benchmark: its MLP forward function is user-authored Cobra composition, with only GEMM as a primitive and bias/ReLU written as ordinary loops. They are intentionally workloads rather than claims: compare the same row-major dimensions, repetition count, and preallocation policy against a hand-written C baseline and Python/NumPy. Record CPU model, AVX2/FMA availability, compiler flags, matrix dimensions, repetitions, and whether startup/compilation is included. The final assertion also guards against a benchmark that silently computes the wrong result.

For `@parallel` worker-pool scaling, use the dedicated runner:

```bash
./benchmarks/parallel_scale.sh
```

It measures, on the host you run it on: the compute-bound poly kernel and the memory-bound affine kernel across 1/2/4/8 participants, dispatch overhead by input size, and cold-pool versus warm-pool per-call cost. The two kernels are deliberately paired: the poly body is pure arithmetic that dispatches and should scale toward core count, while the affine body is a single AVX2 read plus write that is DRAM-bound and should stay flat. `COBRA_WORKERS` (1 to 32) pins the participant count for any run. On a 4-core/8-thread laptop the honest result is 1.7x at 2 workers and a flat curve past that for the compute kernel, no gain for the streaming kernel, and a one-time pool spawn of roughly 15 ms that amortizes to a few microseconds per warm call.
