# Cross Language Benchmarks

These benchmarks measure fixed workloads across Cobra, C, Rust, and Python.
Every implementation checks the final result.

Run it from the repository root:

```bash
python3 benchmarks/cross_language/run.py --warmup 3 --runs 10
python3 benchmarks/cross_language/run_reduction.py --warmup 3 --runs 10
```

C and Rust use AVX2 and FMA for the dense workload. Cobra's `dense_f32` path
emits the native AVX2 and FMA kernel. Python is a scalar reference and does
not use NumPy.

The timing is process wall time, so it includes process startup and excludes
compilation. This is a narrow result for dense numerical work. It does not
represent general application performance, web servers, allocation behavior,
compile time, or package usability.

## Measured results

Medians of 10 runs on the developer machine (Linux x86_64, AVX2). Numbers
vary by CPU, compiler version, and load. Rerun the harness for fresh values.

Dense (fixed matmul workload):

| Language | median | vs Cobra |
|---|---|---|
| Cobra | 1.380 ms | 1.00x |
| C (-O3 -march=native -ffast-math) | 1.659 ms | 1.20x |
| Rust (opt-level=3, target-cpu=native) | 2.238 ms | 1.62x |
| Python (scalar) | 1234.263 ms | 894.60x |

Reduction (sum, mean, and max over 16384 elements, 1500 iterations):

| Language | median | vs Cobra |
|---|---|---|
| C (-O3 -march=native -ffast-math) | 3.526 ms | 0.90x |
| Cobra | 3.913 ms | 1.00x |
| Rust (opt-level=3, target-cpu=native) | 22.104 ms | 5.65x |
| Python (scalar) | 447.426 ms | 114.34x |

The dense result comes from the native AVX2 GEMM kernel: register-resident
counters, four independent FMA accumulators, and running pointers with no
stack traffic in the hot loop. The reduction uses the same independent
vector-accumulator strategy: four independent eight-lane accumulators with a
tree combine break the FP
dependency chain, matching how `-ffast-math` C unrolls a sum. The result
stays within a few ulps of a left-to-right sum. The 16384-element workload
keeps the compute well above process startup so the medians are stable.
