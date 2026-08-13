# Cobra Language Tour

Welcome to Cobraa systems programming language combining Pythonic syntax with direct x86_64 assembly compilation, sub-5ms build times, and zero garbage collection pauses.

---

## 1. Quickstart

Create a project without a generator dependency:

```bash
$ cobra init vision_lab
$ cobra check vision_lab/main.cb
$ cobra run vision_lab/main.cb
```

`cobra init` is non-destructive: it refuses to overwrite an existing `cobra.toml` or `main.cb`. `cobra check` composes imports and runs frontend/IR validation, but emits no assembly and invokes no linker. To build and run your first existing Cobra program:

```bash
$ cobra run examples/01_hello.cb
```

Or generate native GNU Intel-syntax assembly:

```bash
$ cobra emit-asm examples/01_hello.cb -o output.s
```

---

## 2. Memory Architecture: Scope-Arenas vs. Heap

Cobra introduces **1-Cycle Scope-Arena Memory Teardown**:

```python
def process_data(): {
    # 1. Stack Scope-Arena Allocation (Reclaimed in 1 instruction: mov rsp, rbp)
    local_buffer = [1, 2, 3, 4, 5]
    
    # 2. Long-lived Heap Allocation (malloc@PLT)
    heap global_config = 9999
    
    return 0
}
```

---

## 3. Collections without boxing

Cobra has one deliberately sharp distinction: fixed arrays and ML slices stay raw, while application-facing collections opt into owned growth only when requested.

```cobra
values: list[i64] = [1, 2]
append(values, 3)
values[0] = 9

metrics: dict[string]i64 = {"loss": 7, "step": 2}
metrics["step"] = 3
assert(get(metrics, "step", 0) == 3)
free(values)
free(metrics)
```

A list is pointer + length + capacity with unboxed `i64`/`f32` elements. A dictionary is an owned open-addressed table with copied string keys. `len`, indexing, `append`, `set`, `get`, and `has` are native operations, and membership reads like Python:

```cobra
if "loss" in metrics: {
    print("tracked")
}
if 5 not in values: {
    print("absent")
}
```

`in` on a dictionary becomes a native hash probe; on a list, array, or slice it is a direct element scan with an early exit. Both compile to straight-line native code with no iterator objects. `delete(metrics, "step")` removes a key and `pop(metrics, "key", fallback)` removes and returns it (or returns the fallback when absent). Everyday lists and dictionaries are cleaned up automatically when the containing function exits, so normal code does not need ceremony. Explicit `free` is still accepted and validated for compatibility; model buffers never pay for this runtime unless a model chooses to use a collection.

## 4. Type Foundation: bool, none, const, and structs

Cobra now computes one layout for a composite value and reuses it in validation and native emission. That makes nested scalar structs ordinary values rather than a special case:

```cobra
struct Point: { x: i64, y: i64 }
struct Box: { top: Point, bottom: Point, tag: i64 }

box.top.x = 3
print(box.bottom.y)
```

By-value struct parameters are copied into callee-private storage. Nested field access uses direct offsets, while by-value recursive cycles are rejected at compile time. Borrowed slices and owned fields remain explicit follow-up work because their lifetime rules need to travel with the layout.


Cobra's type system grows from the same direct-native principle as everything else: a `bool` is a 0/1 slot, `none` fills any slot with zero, `const` bindings are immutable, and structs are contiguous frame regions with compiler-assigned field offsets.

```cobra
struct Point: { x: i64, y: i64 }
struct Config: { name: string, scale: f32, enabled: bool }

const limit: i64 = 42

let p: Point
p.x = 3
p.y = 4

let c: Config
c.scale = 2.5
c.enabled = true
assert(c.enabled == 1)

let flag: bool = true
let empty: i64 = none
```

`p.x` reads and `p.x = v` writes are single native loads and stores into the struct's frame region; field offsets come from the layout the IR registers, so codegen never recomputes alignment. Scalar-only structs also support semantic by-value parameters. The caller passes one pointer slot and the callee copies the aligned bytes into private frame storage, so parameter mutation cannot change the caller. Borrowed byte fields use explicit `readonly []u8` or `out []u8` qualifiers, preserve pointer-plus-length metadata during copies, keep per-field owner and region identity, and cannot escape their owner. A readonly field may be initialized once on a local, but later rebinding is rejected. Run `examples/50_type_foundation.cb`, `examples/72_struct_parameters.cb`, and `examples/73_http_borrowed_structs.cb` for the full coverage.

### Native Option and Result

Cobra's first structured error values are `Option[T]` and `Result[T, E]`. They use a small native tag and numeric or `bool` payload slots, so ordinary success and failure paths add no heap allocation:

```cobra
def load_scale(value: i64) -> Result[i64, i64]: {
    if value >= 0: { return ok(value * 2) }
    return err(7)
}

result: Result[i64, i64] = load_scale(21)
assert(is_ok(result))
assert(unwrap_ok(result) == 42)
```

`some(value)` and `none` construct an `Option`; `ok(value)` and `err(error)` construct a `Result`. Use `is_some`, `unwrap`, `is_ok`, `unwrap_ok`, and `unwrap_err` to inspect named values. Unwrapping the wrong variant produces a native Cobra failure. Scalar payloads and named structs made from scalar fields work as locals and function returns. Scalar-only Option and Result parameters now cross function boundaries as pointers to caller-owned frame storage and are copied into private callee storage. Struct payloads are copied into caller-provided native sret storage, with no hidden heap object. Postfix `?` now works with these typed returns: success exposes or copies the payload, while failure copies the original sum into the caller's return value. `return fallible()?` wraps the success payload in `some` or `ok`. Sum parameters require named values and scalar payloads for now; unqualified slice fields, nested structs, nested sums, string-bearing parameter structs, and ownership-bearing generic helpers remain explicit rejections until their ownership and ABI contracts are ready. A `readonly []u8` request view can be received by a handler, while `out []u8` is required for indexed mutation. Multiple fields retain independent owners, and region-backed fields are rejected after their region ends. Structs containing borrowed fields still cannot be returned. The borrowed HTTP-style proof in `examples/73_http_borrowed_structs.cb` returns scalar response metadata through `Result`, exercises regions, malformed request input, and `?`, and crosses a source module. Calls inside unsupported `@parallel` bodies retain scalar lowering. See `examples/59_option_result.cb`, `examples/61_typed_propagation.cb`, `examples/62_sum_interactions.cb`, `examples/71_struct_sum_payloads.cb`, `examples/98_sum_parameters.cb`, and `examples/99_sum_abi_matrix.cb` for register, stack, mixed-class, module, and nested-return coverage.

The compiler is also introducing one canonical recursive type descriptor behind this surface. It records type identity, generic arguments, nested payloads, field layout, ownership, mutability, region origin, and ABI class in one graph. The first source-level generic lane is now live: one scalar `T` specializes functions such as `unwrap_or[T]` at monomorphic call sites, with canonical interning and ABI validation. Composite and ownership-polymorphic generics remain deferred. Readonly borrowed slices are the next supported composite lane: `readonly []T` specializes to the existing pointer-plus-length ABI for scalar `i64`, `f32`, and `u8` elements without transferring ownership. See `examples/100_generic_functions.cb`, `examples/101_generic_module.cb`, `examples/103_generic_readonly_slices.cb`, and `examples/104_generic_readonly_slice_module.cb` for local, repeated, and module-boundary specializations.

### Enums and match

Enums give application code named integer states without adding a runtime object:

```cobra
enum Phase: {
    Idle,
    Running,
    Failed = 7,
}

def score(phase: Phase) -> i64: {
    match phase: {
        case Phase.Idle: { return 0 }
        case Phase.Running: { return 1 }
        case Phase.Failed: { return 2 }
    }
    return -1
}
```

Cobra checks every case before native lowering. A match without `else` must cover all variants, while `else` provides a fallback for the remaining states. Duplicate cases, unknown variants, and cases from another enum are compile errors. This first version keeps enums unit-sized and integer-backed; payload variants come after the core pattern-matching contract is stable. See `examples/60_enum_match.cb`.

## 5. Checked Networking, Native Descriptors

Cobra's first network layer stays close to the operating system. Use raw `net_*` helpers when you want sentinel-style control, or use the checked forms for typed failure paths:

```cobra
def open_listener(): Result[i64, i64] {
    socket = net_socket_checked()?
    net_reuseaddr_checked(socket)?
    net_bind_checked(socket, 0)?
    net_listen_checked(socket, 16)?
    return ok(socket)
}
```

The checked API returns scalar descriptors and native `Result` values. `net_recv_checked` returns `ok(0)` when the peer closes, and positive short reads or writes remain visible to the caller. Stable errors cover invalid arguments, closed connections, address conflicts, connection failures, timeouts, interruptions, and generic I/O. Platform `errno` values stay inside the library instead of becoming part of application code. `net_socket_pair_checked` provides a deterministic local pair for tests and IPC. The current layer is intentionally raw and IPv4 focused. HTTP parsing, TLS, DNS, and higher-level request types come after this contract is proven. See `examples/65_net_results.cb`.

## 6. HTTP/1.1 Without a Runtime Object

Cobra's first HTTP layer keeps the wire format visible and the buffers explicit. Request bytes live in caller-owned `[]u8` storage, while parse results use small scalar metadata arrays:

```cobra
request = alloc_u8(4096)
received = net_recv_bytes_checked(client, request, 4096)
method = alloc_i64(2)
path = alloc_i64(2)
version = alloc_i64(2)
line = http_parse_request_line(request, unwrap_ok(received), method, path, version)
```

The parser requires exactly the HTTP/1.1 request-line shape, bounds every scan, compares header names without case sensitivity, and rejects invalid control bytes, duplicate Content-Length fields, oversized lines, oversized header blocks, and bodies above the single-request limit. `http_request_parse` fills a caller-owned twelve-slot metadata record with method, target, version, header, and body ranges. The body slots distinguish bytes currently available from the declared Content-Length, so partial receives never create an out-of-bounds view. `http_request_header` performs allocation-free lookup, `http_request_body_meta` exposes the available range for a zero-copy body view, and route helpers compare method and target spans directly. `http_read_body` uses zero-copy `slice_u8` windows to retry partial receives. `http_response_send` composes response construction and short-send retry with typed `?` propagation. Socket failures keep their stable NetError codes, and every first-cut response closes the connection explicitly. Chunked encoding, keep-alive state, hidden request objects, and struct-valued `Result` payloads are deliberately outside this contract. Run `examples/66_http_foundation.cb` for malformed-input coverage, `examples/70_http_typed_api.cb` for typed request/response and partial-I/O tests, and the same file's main program for a three-request localhost server.

## 7. Python Comfort, Native Loops

Cobra's normal code is meant to read like familiar Python while compiling to direct native loops:

```cobra
values = [4, 5, 6]
for index, value in enumerate(values): {
    print(index)
    print(value)
}
print(sum(values))
```

Use `range` for integer counters and `enumerate` for index/value pairs. `sum`, `min`, `max`, `any`, and `all` work on integer collections without creating iterator objects. Empty `any` is false and empty `all` is true; empty `min` and `max` report a runtime error. The compiler lowers these constructs to counters, bounds checks, and direct pointer loads.

List comprehensions also build native lists in one pass:

```cobra
doubled = [value * 2 for value in values]
evens = [value for value in values if value > 2]
```

The output expression, source collection, and optional `if` guard lower to one direct loop with a `cobra_list_append_*` call per kept element. The result lands in the declared `list` directly, with no temporary object and no intermediate list.

## 8. Automatic Everyday Values, Explicit Model Buffers

Cobra uses a simple two-lane memory model:

- **Everyday values:** lists, dictionaries, and owned concatenated strings clean up automatically at function exit.
- **Model buffers:** `alloc_i64`, `alloc_f32`, and tensor views keep explicit ownership and pointer/length lifetimes for predictable zero-copy performance.

The compiler rejects borrowed frees, use-after-free, and unsafe owned aliases instead of guessing. The everyday lane is familiar; the model lane is explicit when performance demands it.

## 9. Owned Dynamic Buffers

Cobra keeps model-buffer allocation explicit and zero-residency:

```cobra
values = alloc_i64(runtime_count)
process(values)   # borrowed []i64 view
free(values)      # explicit owner release
```

The buffer is zero-initialized and passed as pointer + element count. There is no garbage collector, hidden copy, or slice object. Ownership violations are rejected during validation. `alloc_f32` provides the same explicit contract for native tensor primitives.

---

## 10. Native strings

Strings are compact immutable native pointers rather than runtime objects:

```cobra
def label(name: string) -> string: {
    return "model: " + name
}

def main(): {
    let text: string = label("cobra")
    assert(starts_with(text, "model:"))
    print(text)
    string_free(text)
    return 0
}
```

Use `len`, `+`, comparisons, `starts_with`, `ends_with`, `contains`, and bounds-checked `char_at`. Concatenation and functions proven to return fresh concatenation storage transfer ownership; literals and borrowed parameters do not. `string_free` rejects non-owned strings and double frees during validation. See `docs/STRING_FOUNDATION.md`.

## 11. Native Tensor Primitives

Cobra's first tensor layer uses explicit f32 buffers and direct native kernels. When a model boundary deserves more documentation and safety, add a shape annotation that reads like a familiar Python type hint:

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

This is not a framework object. Directly allocated tensors still use the raw pointer/count fast path with no header allocation. Local view values use a compact stack-resident descriptor containing pointer, count, rank, dimensions, and element strides; typed calls pass that descriptor by pointer without copying data. Scalar `f32` calls use the XMM ABI. The compiler checks rank, numeric dimensions, repeated symbolic dimensions, literal allocation sizes, and known matrix layouts before emitting native code. Existing `[]f32` views stay available for dynamic or low-level code. `slice_view` requires a contiguous source; transpose remains a checked strided view. Views must be released before their owner. Run `examples/28_shape_safe_tensors.cb`, `examples/29_tensor_views.cb`, and `examples/30_abi_contracts.cb` for the ABI and lifetime contracts.

Cobra's first tensor layer uses explicit f32 buffers and direct native kernels:

```cobra
def inference_step(): {
    input = alloc_f32(4)
    weights = alloc_f32(4)
    bias = alloc_f32(1)
    output = alloc_f32(1)
    fill_f32(input, 2)
    fill_f32(weights, 3)
    fill_f32(bias, 1)
    dense_f32(input, weights, bias, output, 1, 1, 4)
    assert(sum_f32(output) == 25)
    free(input)
    free(weights)
    free(bias)
    free(output)
}
```

Direct allocations remain pointer plus element count; transformed tensors use the adaptive descriptor only when shape or stride metadata is needed. The kernels include row-major f32 matmul, fused dense-plus-bias-plus-ReLU, ReLU, fill, and reduction. Contiguous-only kernels reject strided transpose views explicitly rather than producing incorrect results. Users can also write reusable `[]f32` functions and index-range loops directly in Cobra; `for i in len(values)` keeps `i` as an integer index and `values[i]` remains bounds-checked. `dense_f32` removes the intermediate matrix and makes one native pass over each output tile. The generic GEMM path blocks two adjacent eight-column output tiles when runtime dimensions leave sixteen columns, sharing each A broadcast across both tiles without packing or allocating. CNN, recurrent, attention, and quantized operators can be authored on this same ABI rather than requiring incompatible special cases.

Beyond builtins, index-pure `[]f32` loops auto-vectorize. A `for i in len(values)` body that only touches `values[i]`, literals, and loop-invariant scalars lowers straight to AVX2. ReLU clamps become `vmaxps`, affine math fuses to a single `vfmadd213ps`, and ReLU6-style pairs become `vmaxps` + `vminps`. Any loop the analysis cannot prove element-wise, such as nested loops, accumulators, or `values[i-1]` reads, falls back to the checked scalar loop unchanged. Vectorization changes speed, not semantics. The last 0 to 7 elements of each buffer run through a per-element bounds-checked tail. The iterator and tile bound stay in registers and element-wise operands broadcast from registers or `.rodata`, so the vector body does no stack traffic. It is on by default; `cobra build file.cb -O0` or `--cpu=portable` keeps user loops scalar on Linux x86_64. This is a scalar execution mode for the current backend, not a cross-machine ARM64 or Wasm backend. `examples/54_user_loop_vectorization.cb` locks in both the vectorized and the scalar-fallback behaviors.

Model math is first-class in the base language: `sum_f32` (four AVX2 accumulators with a tree combine), `mean_f32`, and `max_f32` return f32, and `exp_f32`, `sqrt_f32`, `tanh_f32`, `log_f32`, and `pow_f32` dispatch to libm. Mixed int/float arithmetic coerces the integer to f32. `examples/25_user_model_forward.cb` shows a user-authored softmax and a two-layer MLP forward pass with a seven-buffer signature. The extra views travel on the stack and use the same primitives.

The reference library `lib/nn.cb` ships that composition as reusable building blocks, auto-prepended to every program exactly like `lib/std.cb` (so model math is always in scope, with no import syntax and no runtime library): softmax, row-wise softmax, sigmoid, GELU, layer norm, scaled dot-product attention, a full self-attention encoder block, token embedding over writable `[]i64` ids, im2col+GEMM convolution, max pooling, and MSE loss. Every function is ordinary Cobra code on the native primitives. GEMM-heavy paths dispatch to the AVX2 kernels, element-wise loops vectorize, and only data movement such as transpose, im2col, and embedding gather uses user-loop code. `examples/26_nn_library.cb` validates each operation against hand-computed values and runs a complete encoder-block forward pass with ten buffers, most of which travel on the stack.

## 12. Native Multi-core Kernels (`@parallel`)

When an element-wise stage is large enough to benefit from multiple cores, mark the range directly:

```cobra
def parallel_affine(values: []f32): {
    @parallel: {
        for i in len(values): {
            values[i] = values[i] * 2.0 + 1.0
        }
    }
}
```

Cobra proves the body is index-pure, then lowers the loop to a standalone worker function and dispatches it across a persistent worker pool (`runtime/cobra_parallel.c`): the pool is spawned once and reused, each call partitions the range into contiguous chunks, and every chunk runs the same AVX2 vector path plus a checked scalar tail. Small ranges stay single-threaded so the annotation costs one indirect call. The pool is a process-wide singleton with a busy guard: a nested dispatch from inside a worker callback or a concurrent call from another application thread runs inline on the calling thread rather than corrupting the shared job record, so parallel blocks stay safe under user-created threads. `COBRA_WORKERS` (1 to 32) pins the participant count for scaling experiments. Unsupported bodies fall back to the normal checked loop, and non-Linux or non-AVX2 hosts preserve scalar semantics. This is the primitive model authors can compose into their own preprocessing, vision, and LLM libraries.

See `examples/27_parallel_kernels.cb` for fallback, worker, and clamp coverage.

## 13. Explicit status propagation

Cobra makes failure flow visible without exceptions or hidden runtime objects. Functions can return `0` or a nonzero application-defined status, or use native `Option[T]` and `Result[T, E]` values. Put `?` after a call when the current function should immediately propagate the matching status or typed sum:

```cobra
def load_weights() -> i64: {
    return 7
}

def run_inference() -> i64: {
    load_weights()?
    return 0
}
```

The compiler lowers integer propagation to a native `test` and conditional branch. Typed propagation compares the native sum tag, copies a failing sum into the caller's return buffer, and loads the successful payload directly. There is no allocation or runtime dispatch. The existing `name?` inspector remains available; propagation is specifically `call()?`.

## 14. Postfix `?` Debug Inspector

Instant variable observability without typing verbose print calls:

```python
price = 4500
price?   # Output: [COBRA INSPECT] price = 4500
```

---

## 15. Universal Compute Fabric (`@compute` SIMD)

Execute 256-bit AVX2 hardware SIMD over a local array or a typed array view:

```cobra
def scale_vector(numbers: []i64): {
    @compute: {
        for i in numbers: {
            i = i * 2
        }
    }
}

values = [1, 2, 3, 4, 5, 6]
scale_vector(values)
```

`[]i64` lowers directly to a pointer and length pair. There is no slice object, garbage-collector work, or intermediate IR in the production path. The native emitter uses the dynamic length for the vector loop and scalar tail, then `vzeroupper` before returning.

Representative assembly:

```assembly
mov rbx, QWORD PTR [rbp-8]    # pointer
mov r8,  QWORD PTR [rbp-16]   # length
vmovdqu ymm0, [rbx + rcx*8]
vpaddq ymm0, ymm0, ymm0
vmovdqu [rbx + rcx*8], ymm0
```

---

## 16. Compile-Time Meta Engine (`@comptime`)

Evaluate constants at compile-time directly in the sandboxed AST interpreter:

```python
x = @comptime(25 + 500)  # Bakes 'mov rax, 525' into native assembly
```

---

## 17. Projects and package paths

A project can add a tiny `cobra.toml` at its root:

```toml
[package]
name = "vision_lab"
version = "0.1.0"

[dependencies]
vision = "vendor/vision"
```

Then `import "vision/resize.cb" as resize` checks the importing file’s relative path first, resolves the `vision` prefix through the nearest manifest, and finally falls back to `COBRA_LIB_PATH`. Paths are canonicalized and kept inside the project root. The manifest is compile-time configuration only; Cobra still emits one direct native program.

## 18. Source modules with no runtime machinery

Source modules now have an explicit visibility boundary. Functions are public by default for existing modules, while `private def` keeps a helper inside its source file and `pub def` documents an exported API. Calls across that boundary are checked before native emission, and private helpers become local native symbols rather than exported linkage.

The scalar baseline follows the same principle of one source and multiple valid lowerings. `--cpu=portable` keeps ordinary loops scalar on Linux x86_64, omits unreachable AVX-backed NN helpers, and rejects reachable tensor kernels. It does not emit ARM64 or Wasm code. Scalar-safe wrappers such as `sigmoid_buf` still lower through their checked scalar loop. Native x86_64 builds can enable the AVX2 path with `--cpu=native` or `--cpu=avx2`.


Use a plain quoted import for user-authored Cobra libraries:

```cobra
import "modules/math.cb" as math

def test_source_module_imports() -> i64: {
    return math.module_add(40, 2)
}
```

Cobra resolves the module relative to the importing file, recursively composes its source before validation, and emits functions directly into the same native symbol space. The alias is compile-time syntax only: `math.module_add(...)` becomes a direct native `module_add(...)` call. Each canonical file is loaded once, cycles are rejected early, and there is no module object, dynamic dispatch, or runtime linker cost. Plain unaliased imports remain available for tiny scripts. `import c "..." (...)` remains the explicit native C bridge.

See `examples/39_source_modules.cb`, `examples/40_module_diamond.cb`, `examples/41_module_alias.cb`, and `examples/modules/math.cb`.

## 19. Zero-Header C Interop (`import c`)

Call integer/pointer C functions directly without glue code or `.h` headers. Cobra passes the declaration to the native emitter and linker as arguments, never through a shell:

```cobra
import c "libc.so.6" (abs)

result = abs(0 - 42)
```

The direct bridge supports up to six integer, pointer, or string-pointer arguments and an integer/pointer return on Linux x86-64. It rejects floating-point foreign arguments; floating-point-returning functions are outside this untyped bridge and should use a typed Cobra wrapper or C shim. Unannotated and `pub` Cobra functions are global native symbols; `private` functions use local linkage and cannot be called by an external C host. See `examples/34_c_abi_host.cb` and `examples/34_c_abi_host.c` for a C host calling a `[]f32` kernel with the ABI `(float *values, long length)`.

---

## 20. Integrated Tooling (`cobra init`, `cobra check`, `cobra test` & `cobra fmt`)

Initialize, validate, format, and test your code out of the box with the single Cobra CLI binary:

```bash
$ cobra init my_project
$ cobra check my_project/main.cb
$ cobra test examples/12_test_suite.cb
$ cobra fmt examples/12_test_suite.cb
```

`cobra check` is intentionally lighter than `cobra build`: it validates the composed source graph and native contracts without generating `.s` files or linking a binary. Errors use `file:line:column: error: message`, including the original file when a composed module fails validation.

---

## 21. Systems Lane: File I/O and Time (lib/fs.cb, lib/time.cb)

The systems lane is userland Cobra on the `import c` bridge, auto-prepended exactly like `lib/std.cb` and `lib/nn.cb`. The checked filesystem API uses direct POSIX descriptors, so open, read, write, and close have no hidden `FILE*` object or wrapper runtime:

```cobra
opened = fs_open_write(path)
assert(is_ok(opened))
let fd: i64 = unwrap_ok(opened)
written = fs_write_string(fd, content)
assert(is_ok(written))
assert(unwrap_ok(written) == len(content))
assert(is_ok(fs_close(fd)))

buf = alloc_i64(64)
received = fs_read_file_checked(path, buf, 64)
assert(is_ok(received))
assert(unwrap_ok(received) == len(content))
```

`fs_open_read`, `fs_open_write`, and `fs_open_append` return `Result[i64, i64]` descriptor handles. `fs_read_fd` and `fs_write_fd` return byte counts in `Result`; `ok(0)` from read is EOF, and a short positive write is successful progress that callers can retry. The original path-based `fs_read` and `fs_write` names remain raw compatibility helpers. Negative handles and byte counts return error 2; system failures return error 1. The path-level checked helpers always close their descriptor on success and read or write failure. Raw stat, existence, append, remove, and rename helpers remain available for simple scripts. Binary buffers are caller-owned `[]i64` storage, and `fs_read_file_f32_checked` also supports region-backed f32 scratch buffers. Time stays integer throughout, with two levels of API:

- `time_now()`, `time_precise_us()`, `time_elapsed_us(start)`, and `time_clock_ticks()` are raw helpers for low-level code that accepts sentinel values. The raw precise helper returns -1 if `gettimeofday` fails.
- The `_checked` variants return `Result[i64, i64]`, so clock failures and invalid timestamps travel through the same native `?` path used by application code. Error 1 means a libc clock failure, error 2 means an invalid or rolled-back timestamp, and error 3 means a CPU clock failure. Use `time_monotonic_us_checked()` and `time_elapsed_monotonic_us_checked()` when duration measurements must ignore wall-clock adjustments.

The checked microsecond helper releases its temporary timeval buffer on both paths, and the monotonic helpers do the same for their timespec storage. `examples/51_systems_lane.cb` uses the checked filesystem and time APIs, while `examples/63_time_results.cb` covers time propagation and `examples/64_fs_results.cb` covers missing paths, inaccessible paths, empty files, partial reads, EOF, binary zero bytes, large files, double close, region cleanup, and a checked transform pipeline.

---

## 22. Kernel Contracts: Alias-Aware Buffers, Constant Shapes, and Regions

Three additions make model code say what it means, so the compiler can prove more and allocate less.

### Alias-aware buffers (`out` / `readonly`)

Slice parameters can carry an alias contract. `readonly` promises the buffer is never written; `out` promises the buffer is the write target and never aliases another buffer parameter. The validator rejects writes to `readonly` buffers at compile time, so the contract holds before any kernel runs.

```cobra
def elementwise_add(dst: out []f32, left: readonly []f32, right: readonly []f32): {
    for i in len(dst): { dst[i] = left[i] + right[i] }
}
```

`out` and `readonly` are contextual keywords: they only mean something in the parameter slot, so existing code that uses them as identifier names keeps working. See `examples/55_alias_contracts.cb` and `tests/negative/44_readonly_write.cb`.

### Constant-shape kernels

When `dense_f32` gets compile-time dimensions and N is a multiple of eight, the emitter lowers a specialized kernel: immediate trip bounds, no per-row vector/scalar decision, and no tail branches. M=1 (a single query row) drops the row loop and the i*k/i*n multiplies entirely; N=8 drops the column loop; K<4 unrolls the dot product. For K a multiple of four up to 64 the K loop disappears too: the dot product is emitted as straight-line broadcast-load-FMA with immediate displacements, so the hot path has no branch and no counter, and the single-row tile loop counts in a register instead of the stack.

```cobra
dense_f32(a, w, b, out, @comptime(1), @comptime(128), @comptime(64))
```

Plain integer literals specialize too. The existing dense benchmark calls now hit this path and beat C by a wider margin. See `examples/56_constant_shape_kernels.cb`.

### Region arenas (`with region`)

`with region NAME(capacity):` bumps allocations out of one backing store and releases it exactly once at scope exit. Inside the block, `NAME.alloc_f32(n)` returns a `[]f32` and `NAME.alloc_u8(n)` returns a byte view backed by the arena, so scratch buffers never touch the general allocator. The region passes its three-slot state as `[]i64` length `3` to `arena_alloc`; f32 requests use `n * 4` bytes but report `n` elements, while u8 requests use `n` bytes and report `n` elements.

```cobra
with region scratch: {
    a = scratch.alloc_f32(64)
    b = scratch.alloc_f32(128)
    dense_f32(a, w, b, out, @comptime(1), @comptime(8), @comptime(4))
}
```

Capacity defaults to 1 MiB and may be an integer expression: `with region tight(64):`. Regions nest, and a region inside a loop is created and released per iteration. Early returns and integer `?` propagation release active regions inside-out. Region owners cannot be passed out or released with `free`; zero-copy views record their owner, can be released without freeing storage, and cannot outlive it. See `examples/57_region_arenas.cb` and `examples/69_u8_region_lifetimes.cb`.
