<div align="center">
  <img src="web/cobra_logo.jpg" width="180" alt="Cobra">

  # Cobra

  **The comfort of Python. The bare metal of x86_64.**

  [![CI](https://github.com/Aaron-Savron/Cobra/actions/workflows/ci.yml/badge.svg)](https://github.com/Aaron-Savron/Cobra/actions/workflows/ci.yml)
  [![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](https://opensource.org/licenses/MIT)
  [![Discord](https://img.shields.io/badge/discord-join-5865F2?logo=discord&logoColor=white)](https://discord.gg/b9h4ZD5JNM)

  [Language Tour](docs/TOUR.md) · [CLI Reference](docs/REFERENCE.md) · [Contributing](CONTRIBUTING.md) · [Roadmap](ROADMAP.md) · [Discord](https://discord.gg/b9h4ZD5JNM)
</div>

---

> **Active development: Day #5.** Linux x86_64 is the production target. Win64, ARM64, and Wasm32 are interface stubs open for contributors. Expect the language to keep moving.

## Why Cobra exists

I built Cobra after learning Rust and then reaching for Python to do AI work. Rust gave me control. Python gave me momentum. I wanted both without making everyday programs pay for a heavyweight runtime or forcing every developer to start with a systems textbook.

Cobra is that experiment made real: **Python-shaped code with direct native execution**. You can write a small script, a file tool, a web service, or a model kernel in the same language. When you need control, the machine is still visible.

## The short version

- **Familiar:** Python-like assignments, collections, iteration, modules, and a small CLI.
- **Native:** Cobra validates `.cb` source with a recursive-descent front end and emits Intel-syntax GNU assembly directly.
- **Portable on x86_64:** `--cpu=portable` keeps ordinary loops on a scalar baseline, omits unreachable AVX-backed NN helpers, and rejects reachable tensor kernels. It is not yet a cross-machine ARM64 or Wasm backend.
- **Predictable:** Scope cleanup, region arenas, explicit buffer lifetimes, and zero-copy views keep memory behavior visible.
- **Fast where it matters:** `@compute` lowers proven loops to 256-bit AVX2, `@parallel` dispatches disjoint work across a persistent worker pool, and model kernels use direct pointer-plus-length ABIs.
- **Small by design:** The compiler is split across eight small C source files and was designed around a 15KB binary target. Actual release size varies with compiler and linker flags, so measure the artifact you build.
- **Open:** The compiler, runtime, libraries, tests, and roadmap are all in the repository.

No virtual machine. No bytecode. No tracing garbage collector in the generated program. Just a small front end, a validator that rejects unsafe contracts, and native code you can inspect.

## Get running in one minute

```bash
git clone https://github.com/Aaron-Savron/Cobra.git
cd Cobra
make
./cobra run examples/01_hello.cb
```

Start a project:

```bash
./cobra init vision_lab
./cobra check vision_lab/main.cb
./cobra run vision_lab/main.cb
```

Inspect what the compiler emits:

```bash
./cobra emit-asm examples/09_compute.cb -o /tmp/cobra.s
less /tmp/cobra.s
```

Run a test and a benchmark:

```bash
./cobra test examples/14_stress_test.cb
./cobra bench benchmarks/dense_repeat.cb --warmup 3 --runs 20
```

The CLI also includes `build`, `fmt`, `repl`, and `check`. `cobra check` validates the composed source graph and native contracts without linking a binary.

## The language feels familiar

The everyday lane should feel like discovering a sharper Python, not learning a new ceremony-heavy dialect:

```cobra
values = [4, 5, 6]
for index, value in enumerate(values): {
    print(index)
    print(value)
}
print(sum(values))
```

Use `range`, `enumerate`, `sum`, `min`, `max`, `any`, `all`, list comprehensions, strings, dictionaries, source modules, and project manifests. Collections use unboxed scalar elements instead of boxed runtime objects. Fixed arrays and ML slices stay on their raw pointer paths, so application conveniences do not tax code that never uses them.

```cobra
metrics: dict[string]i64 = {"loss": 7, "step": 2}
metrics["step"] = 3

if "loss" in metrics: {
    print("tracked")
}
```

The syntax is intentionally light. Add types where an ABI, model boundary, or safety contract benefits from them. Keep quick experiments quick.

## Memory without guesswork

Python's convenience is great until a long-running process starts hiding allocations. C and Rust give you control, but the cost is often ceremony and a steep first climb. Cobra takes a narrower route.

**Scope-arena cleanup** gives ordinary local values predictable lifetimes without a tracing garbage collector breathing down your neck. Lists, dictionaries, and owned concatenated strings clean up at function exit, including early returns. Raw buffers remain explicit when you need model-level control.

```cobra
def process(): {
    values: list[i64] = [1, 2, 3]
    text: string = "model: " + "cobra"
    print(text)
} # ordinary owned values are cleaned up here
```

For scratch work, use a region arena:

```cobra
with region scratch(64): {
    temporary = scratch.alloc_u8(16)
    tensor_work = scratch.alloc_f32(8)
}
```

The region releases its storage once, handles early returns and status propagation, and rejects views that would outlive their owner. Model buffers such as `alloc_i64` and `alloc_f32` remain explicit pointer-plus-length views. That gives you a simple default and a precise escape hatch.

## The compiler makes performance visible

Cobra does not ask you to trust a black box. The source tells the compiler what can be proven, and the emitted assembly shows what happened.

### `@compute`: AVX2 without a framework

Mark a proven element-wise loop for 256-bit SIMD:

```cobra
def scale(values: []f32): {
    @compute: {
        for i in len(values): {
            values[i] = values[i] * 2.0 + 1.0
        }
    }
}
```

The native emitter produces an AVX2 body plus a checked scalar tail for the final 0 to 7 elements. Index-pure user loops can also auto-vectorize without an annotation. When the compiler cannot prove element-wise safety, it keeps the checked scalar loop instead of changing semantics.

Representative output:

```asm
vmovups ymm0, [r8 + rcx*4]
vfmadd213ps ymm0, ymm1, ymm2
vmovups [r8 + rcx*4], ymm0
vzeroupper
```

`@compute` is not a runtime object or a scheduling layer. It is a compile-time lowering decision.

### `@parallel`: use more than one core

For large disjoint ranges, ask for worker dispatch:

```cobra
def affine(values: []f32): {
    @parallel: {
        for i in len(values): {
            values[i] = values[i] * 2.0 + 1.0
        }
    }
}
```

Cobra generates a standalone worker, captures the pointer and length context, partitions the range into disjoint chunks, and reuses a persistent worker pool. Each chunk keeps the same AVX2 body and scalar tail. Unsupported bodies fall back to the checked scalar path.

## Built for model code, not just benchmarks

The ML lane stays close to the hardware:

- `[]f32` is a pointer plus element count.
- Tensor views are zero-copy and carry shape and stride contracts when needed.
- `matmul_f32`, `dense_f32`, reductions, activations, attention, convolution, and pooling use native kernels.
- Constant shapes can specialize into straight-line FMA paths with fewer branches and index calculations.
- Users can write their own MLP, CNN, recurrent, attention, or LLM building blocks in ordinary Cobra code.

```cobra
def project(
    x: tensor[batch, hidden]f32,
    w: tensor[hidden, classes]f32,
    out: tensor[batch, classes]f32,
    batch: i64,
    classes: i64,
    hidden: i64
): {
    matmul_f32(x, w, out, batch, classes, hidden)
    return 0
}
```

The reference `lib/nn.cb` library provides reusable model building blocks, but it does not lock users into a framework object model. The same pointer, slice, and function machinery is available for custom architectures.

## Errors that stay in the control flow

Cobra supports integer statuses for simple native APIs and typed `Option[T]` and `Result[T, E]` values for larger programs. The postfix `?` operator propagates failure without exceptions, heap error objects, or hidden runtime dispatch.

```cobra
def load_scale(value: i64) -> Result[i64, i64]: {
    if value < 0: {
        return err(7)
    }
    return ok(value * 2)
}

def run() -> Result[i64, i64]: {
    scaled = load_scale(21)?
    return ok(scaled + 1)
}
```

Simple enums and exhaustive `match` provide named states without runtime objects. Scalar struct payloads use caller-owned return storage. Borrowed byte views inside structs preserve owner and region metadata, and invalid escapes are rejected before assembly generation.

## Systems APIs in userland Cobra

Cobra's systems lane is ordinary Cobra layered over the explicit `import c` bridge:

```cobra
opened = fs_open_write(path)?
written = fs_write_string(opened, content)?
fs_close(opened)?
```

`lib/fs.cb` and `lib/time.cb` provide checked file and clock operations with direct libc calls. The network layer exposes socket-shaped primitives and stable error codes. The HTTP/1.1 foundation keeps request bytes in caller-owned buffers, parses headers without allocation, exposes zero-copy body views, rejects unsupported chunked framing, and makes connection close behavior explicit.

When the high-level API is not enough, drop to the metal:

```cobra
import c "libc.so.6" (abs)

result = abs(0 - 42)
```

The current bridge supports up to six Linux x86_64 SysV integer, pointer, or string-pointer arguments and integer or pointer returns. Floating-point foreign calls should use a typed Cobra wrapper or a C shim rather than guessed ABI behavior.

## Modules and tooling without runtime overhead

Quoted imports compose Cobra source at compile time. The compiler resolves paths, rejects cycles, and emits one native program. There is no module object, dynamic lookup, or hot-path dispatch.

```cobra
import "modules/math.cb" as math

result = math.module_add(40, 2)
```

Projects can declare a small `cobra.toml` with package and dependency paths. The manifest controls compile-time composition, not runtime behavior. New modules can use `private def` for internal helpers and `pub def` for an explicit exported API; private calls across module boundaries are rejected before code generation, and private functions are local native symbols rather than exported linkage.

## See the assembly. Measure the machine.

Cobra is designed for direct native execution, but no honest compiler promises one universal speed number. CPU features, flags, input shape, memory bandwidth, and startup cost all matter.

```bash
make
./cobra emit-asm examples/54_user_loop_vectorization.cb -o /tmp/vector.s
./cobra bench benchmarks/dense_repeat.cb --warmup 3 --runs 20
/usr/bin/time -f '%e sec, %M KB peak RSS' ./cobra run examples/01_hello.cb
```

Use `-O0` or `--cpu=portable` to keep user loops scalar on Linux x86_64. Use `--cpu=native` or `--cpu=avx2` for the AVX2 path on Linux x86_64. The output is ordinary assembly, so you can inspect it, profile it, and call Cobra functions from C.

## Measure it and protect the fast paths

The repository now keeps two performance layers separate. `make perf-baseline` measures generated programs on your machine. `make perf-contracts` checks deterministic properties in emitted assembly. The suite covers scalar calls, slices, struct copies, typed sums, region allocation, HTTP parsing, file transforms, GEMM, reductions, user-authored model code, and `@parallel`.

```bash
make perf-contracts
python3 benchmarks/performance_baseline.py --runs 5 --warmup 1 --output /tmp/cobra-baseline.json
make perf-profile PROFILE_SOURCE=benchmarks/gemm_repeat.cb
```

`make perf-profile` uses Linux `perf stat` on the generated workload binary, so the counters describe the program Cobra emitted rather than the compiler itself. It is an optional local tool because hosted CI often restricts hardware counters.

A baseline records the CPU, Cobra revision, samples, medians, and p95 values. Compare it on the same host with explicit gates:

```bash
python3 benchmarks/performance_baseline.py \
  --baseline /tmp/cobra-baseline.json \
  --runs 5 --warmup 1 --strict
```

The default gates allow 2 percent for scalar calls and slice loops, and 5 percent for structs, sums, regions, I/O, and kernels. Timing gates are opt-in and strict comparisons belong on a pinned CPU or dedicated runner. An interactive laptop or shared CI host can still report scheduler and frequency noise. Assembly contracts run in CI on every build and protect the properties that should not drift: no heap object for scalar sums or enums, pointer-plus-length slices, direct `?` branches, region cleanup, AVX2 loops, and unrolled constant GEMM.

## Build a release archive

```bash
make dist
```

This creates `cobra-v1.0.0-linux-x86_64.tar.gz` with the stripped compiler, Cobra libraries, runtime sources, installer, and release notes. The package is local-only. GitHub publishing remains an explicit separate action.

## Current platform status

| Target | Status |
| :--- | :--- |
| Linux x86_64 | Production target with native execution |
| Win64 | Interface emitter stub |
| ARM64 | Interface emitter stub |
| Wasm32 | Interface emitter stub |

The missing backends are not marketing fog. They are clear contribution lanes: ABI mapping, object emission, linker integration, runtime support, and cross-target tests. `--cpu=portable` does not change this table. It means scalar execution for the existing Linux x86_64 backend.

## Where to look next

- [Language Tour](docs/TOUR.md): guided examples from collections to tensors and systems APIs
- [CLI and Language Reference](docs/REFERENCE.md): grammar, contracts, ABI details, and command options
- [Contributing](CONTRIBUTING.md): build instructions, conventions, and PR process
- [Roadmap](ROADMAP.md): backend and language priorities
- [Examples](examples/): runnable programs and regression suites
- [Cobra Language Lab](codex/): executable evaluation scenarios and findings
- [Discord](https://discord.gg/b9h4ZD5JNM): design discussions and project updates

Every new feature should earn an example, a positive test, a negative test where applicable, and documentation. That is how the language stays small enough to understand while growing strong enough to use.

## License

MIT. See [LICENSE](LICENSE).
