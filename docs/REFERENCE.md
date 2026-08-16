# Cobra Language CLI & Grammar Reference

## CLI Subcommands

```bash
cobra init [directory]
cobra check <file.cb>
cobra build <file.cb> [-o binary] [-O0|-O1|-O2|-O3|-Ofast] [--cpu=native|avx2|portable] [--target=win64|wasm32|arm64]
cobra run <file.cb>
cobra test <file.cb>
cobra bench <file.cb> [--warmup N] [--runs N]
cobra fmt <file.cb>
cobra emit-asm <file.cb> [-o output.s] [-O0|-O1|-O2|-O3|-Ofast] [--cpu=native|avx2|portable] [--target=win64|wasm32|arm64]
```

### Project workflow

`cobra init [directory]` creates a new directory when needed and writes a starter `cobra.toml` plus `main.cb`. It never overwrites an existing `cobra.toml` or `main.cb`.

`cobra check <file.cb>` discovers the nearest manifest, composes source imports, and runs the same IR validation used by native builds. It stops before assembly generation, linking, runtime selection, and execution, so it is the fast frontend gate for editors, CI, and package development. Semantic diagnostics use `file:line:column: error: message`; errors in imported modules point to the original module file rather than the flattened entry source. Unresolved or cyclic imports also print the active module chain.

For example:

```bash
cobra init vision_lab
cobra check vision_lab/main.cb
cobra run vision_lab/main.cb
```

`cobra test <file.cb>` discovers functions named `test_*`. Native test entries must take no parameters and return an integer status, with zero meaning pass. Void test entries are rejected for now because the runner records and reports that status value; use an integer-returning wrapper until void test support is added.

### CLI Options

* `-o <name>`: Specify target executable or assembly output path.
* `--warmup N`: Run N correctness-checked warmup processes before timing a benchmark (default: 2).
* `--runs N`: Collect N correctness-checked native process samples (default: 10).
* `-O0`: Disable auto-vectorization of user f32 loops; they lower as checked scalar code. This is the portable-speed build.
* `-O1`, `-O2`, `-O3`, `-Ofast`: Enable user-loop auto-vectorization. This is the default; the levels are accepted for tooling compatibility and currently compile identically.
* `--cpu=portable`: Linux x86_64 scalar mode. It disables user-loop vectorization and is the scalar baseline for the current native backend; it is not a cross-machine ARM64 or Wasm target.
* `--cpu=native`, `--cpu=avx2`: Enable the AVX2 lowering for user loops (default). Vectorization also requires AVX2 on the host CPU, since the kernel is emitted natively.
* `--target=win64`: Emit the current Win64 interface stub; production native execution is Linux x86_64.
* `--target=wasm32`: Emit the current WebAssembly interface stub (`.wat`).
* `--target=arm64`: Emit the current Apple Silicon interface stub.

---

## Structured status propagation

Cobra supports explicit integer statuses and native typed sums instead of hidden exceptions. Integer `0` means success and any nonzero value is an application-defined failure code. Postfix `?` checks a call result and propagates the same status or sum value without allocation. For `Option[T]` and `Result[T, E]`, a successful `?` expression yields `T`; a failure returns the original sum from the current function. In a sum-returning function, `return fallible()?` wraps the successful payload in `some` or `ok` automatically:

```cobra
def load_weights() -> i64: {
    return 7
}

def run_inference() -> i64: {
    load_weights()?
    return 0
}

def load_scale(value: i64) -> Result[i64, i64]: {
    if value < 0: {
        return err(7)
    }
    return ok(value * 2)
}

def run_checked(value: i64) -> Result[i64, i64]: {
    scaled = load_scale(value)?
    return ok(scaled + 1)
}
```

`?` is a direct branch to the current function epilogue. It allocates no error object, calls no runtime helper, and does not affect functions that do not use it. The existing integer form remains compatible with C-style APIs, while typed propagation requires the caller and callee to use the same `Option` or `Result` payload contract. Sum values stay in the native frame, and accessors currently require a named sum value.

## Collections

Cobra keeps fixed arrays (`[1, 2, 3]`) and ML slices (`[]i64`, `[]f32`) unchanged. For application code, `list[T]` is an owned growable sequence with unboxed scalar elements and `dict[string]i64` is an owned open-addressed map with copied string keys. `list[T]`/`dict[K]V` function parameters have reference semantics: append, index-write, and `set` calls performed inside a callee (including through further pass-through calls) are visible to the caller once the call returns, with a callee-side append that grows the backing buffer correctly writing the new pointer and capacity back through the reference. See `examples/150_list_dict_param_reference.cb`.

```cobra
values: list[i64] = [1, 2]
append(values, 3)
values[0] = 9

metrics: dict[string]i64 = {"loss": 7, "step": 2}
metrics["step"] = 3
current = get(metrics, "step", 0)
known = has(metrics, "loss")
```

Use `len(values)` or `len(metrics)`, ordinary indexing, `append`, `set`, `get`, and `has`. Membership uses the Python-shaped operators `in` and `not in`: a dictionary membership compiles to a native hash probe, and list/array/slice membership compiles to a direct element scan with an early exit. `delete(metrics, "step")` removes a key and returns success; `pop(metrics, "key", fallback)` removes a key and returns its value, or the fallback when the key is absent. List comprehensions build a fresh native list in one pass: `[expr for value in values]` and `[expr for value in values if guard]` lower to a single loop with one `cobra_list_append_*` call per kept element, writing directly into the declared `list` symbol. Local lists and dictionaries are reclaimed automatically when their containing function exits, including early returns. `free` remains available as an explicit compatibility operation for owned values; borrowed collection parameters cannot be freed, use-after-free is rejected, and unsafe owned aliases are rejected. Dictionary keys are copied into the table. There is no tracing garbage collector, boxing, or hidden list object on fixed-array/tensor paths. `list[i64]` and `list[f32]` are the first native element types; dictionary values are currently `i64`, and missing `get` keys return the supplied fallback.

## Function Values

`fn(T1, T2, ...) -> R` is a non-capturing function-value type: a plain top-level function assigned to it, passed as it, or returned as it carries a real checked signature (parameter and return types), not just a bare address. Phase 1 only: every parameter and the return type must be scalar (or `void` for the return) - no captures, no slices/structs/collections in the signature yet.

```cobra
def add(a: i64, b: i64) -> i64: { return a + b }

def apply(f: fn(i64, i64) -> i64, x: i64, y: i64) -> i64: {
    return f(x, y)
}

let op: fn(i64, i64) -> i64 = add
op(3, 4)          # 7
apply(add, 3, 4)  # 7
```

`call_i64_i64(func_ptr, arg)` / `call_f32_f32(func_ptr, arg)` remain as a deprecated one-argument alias; prefer calling the `fn(...)->...` value directly.

`def(params) -> ret: { body }` in expression position is an anonymous closure literal: it evaluates to a `fn(...)->...` value the same way a named function reference does. It may capture scalar (i64/f32/bool) variables from the immediately-enclosing function - its parameters, and its explicitly-typed `let` locals - by value: a snapshot taken when the literal is evaluated, not a live reference. Captures are read-only; assigning to a captured name inside the closure body is rejected. Capturing a non-scalar variable is rejected with a diagnostic naming it. See ROADMAP.md for the ABI and capture-analysis design, and examples/119_closures_capture.cb / examples/120_closures_map.cb for usage.

```cobra
def apply(f: fn(i64, i64) -> i64, x: i64, y: i64) -> i64: {
    return f(x, y)
}

apply(def(a: i64, b: i64) -> i64: { return a * b }, 6, 7)  # 42
```

## Implicit Generic Parameters

`def name[](params)` (empty brackets, no named type parameter) marks every
parameter left without a `: type` annotation as implicitly generic: each
omitted parameter gets its own independent slot (no unification between
distinct parameter names, up to `COBRA_MAX_TYPE_ARGS` = 8), monomorphized
together per call site through the same specialization pipeline as an
explicit `def name[T](x: T)`, instead of being rejected the way a bare
parameter is under `def name(params)` (a bare parameter with no brackets at
all is always a compile-time error - see Type Foundation above). The return
type and any explicitly-typed parameter must still be a fixed, non-generic
type.

```cobra
def double_it[](x) -> i64: { return x * 2 }
def times_ten[](x) -> f32: { return x * 10.0 }

double_it(21)   # 42, specialized for i64
times_ten(7.0)  # 70.0, an independent specialization for f32

def add[](a, b) -> i64: { return a + b }
def scale[](x, factor: i64) -> i64: { return x * factor }

add(3, 4)      # 7, two independently-inferred parameters
scale(6, 7)    # 42, one inferred parameter plus one fixed-type parameter
```

`export def name[](params)` requires at least one call site to exist by end
of compilation, so a template with no caller anywhere in the program does
not silently ship with a body that was never type-checked (a generic
template's body is only checked on its specialized clones, matching how
explicit `[T]` generics already work). When a specialization fails to
type-check, the diagnostic names both the triggering call site and the
template declaration.

Not yet supported: more than one inferred parameter per function, an
inferred return type, and using an implicitly-generic function as a
`fn(...)->...` value, a closure, or a `dyn Trait` receiver (same restriction
explicit `[T]` generics already have). See ROADMAP.md for the phase 2/3 plan
(multi-parameter inference, further diagnostics).

## Traits

`trait Name: { def method(params) -> ret ... }` declares required method signatures; `impl Name for Type: { def method(params) -> ret: { body } ... }` implements them for a struct type. Calling `x.method(args)` on a struct-typed value resolves at compile time to the matching impl (static dispatch only - no vtable, no runtime cost). Every trait method must have a matching impl method by name or the impl is rejected. See ROADMAP.md for what's deferred (generic trait bounds, default methods).

```cobra
struct Circle: { radius: i64 }

trait Shape: {
    def area() -> i64
}

impl Shape for Circle: {
    def area(c: Circle) -> i64: { return c.radius * c.radius * 3 }
}

let c: Circle
c.radius = 4
c.area()  # 48
```

A `dyn TraitName` function parameter accepts any struct implementing the trait and dispatches to the right impl at runtime, not at compile time:

```cobra
def describe(shape: dyn Shape) -> i64: { return shape.area() }

describe(c)  # 48, dispatched through a runtime vtable lookup
```

Only function parameters coerce to `dyn TraitName` today (not `let`/return types or `list[dyn Trait]`). See ROADMAP.md for the vtable representation and further limits.

## Type Foundation

Cobra's core types stay direct native slots: `bool` is a 0/1 value, `none` is the empty value (lowers to zero and is assignable to any type), `const` bindings are immutable after initialization, and `struct` defines a contiguous user type with compiler-assigned field offsets.

```cobra
struct Point: { x: i64, y: i64 }
struct Config: { name: string, scale: f32, enabled: bool }

const limit: i64 = 42

let p: Point
p.x = 3
p.y = 4

let flag: bool = true
let empty: i64 = none
```

* `bool` accepts `true` and `false` (lowered as 0/1) and compares with integer values.
* `none` initializes any value to zero; it is the seed for optional-value patterns.
* `const` bindings are validated: assigning to a `const` after declaration is rejected.
* `struct Name: { field: type, ... }` declares a user type. Supported field types include scalar values, borrowed byte views, owned strings, owned slices, and supported owning `Option`/`Result` payloads. Struct variables are zero-initialized contiguous frame regions; `var.field` reads and `var.field = value` writes use canonical field offsets. Unknown fields, duplicate fields, and type-mismatched member assignments are rejected during validation. Struct parameters use one pointer ABI slot and are copied or moved according to field ownership. Borrowed views preserve their pointer-plus-length, owner, and region metadata. Owned fields use explicit move, replacement, return, and drop operations. Struct returns use the caller-owned sret path described below. The isolated backend v2 supports these ownership-bearing fields through its evaluator and Linux x86-64 emitter; generic ownership-bearing fields, arbitrary non-scalar nesting, and production backend integration remain deferred.

See `examples/50_type_foundation.cb`.

Function parameters require an explicit type annotation (`def f(x: i64): { ... }`); an
omitted parameter type is a compile-time error rather than a silent default. `let`
locals remain inferrable from their initializer (`let x = 5`). Function return types
may still be omitted, defaulting to `i64` if a value is returned or `void` otherwise;
this is an established, widely-used convention, not the same class of bug.

### Option and Result

`Option[T]` and `Result[T, E]` are native tagged values for application APIs. Scalar payloads and named structs made from scalar fields are supported. Struct-bearing sums use caller-provided native return storage and copy payload bytes by value; they do not allocate a hidden heap object:

```cobra
def maybe_value(value: i64) -> Option[i64]: {
    if value > 0: {
        return some(value)
    }
    return none
}

def checked_value(value: i64) -> Result[i64, i64]: {
    if value >= 0: {
        return ok(value * 2)
    }
    return err(7)
}
```

Use `is_some`, `unwrap`, `is_ok`, `unwrap_ok`, and `unwrap_err` with a named value. An invalid unwrap reports a native Cobra failure and exits. Sum values are supported as locals, function returns, and parameters. Scalar and supported ownership-bearing sums use explicit tagged storage, caller-provided aggregate returns, and field-aware move and drop operations. Borrowed fields cannot be freed by a callee, and freeing an owner while a view is live is rejected. In the isolated backend v2, direct owned string and owned slice payloads, nested owning sums, indirect calls, returns, extraction, and recursive destruction are supported. Payload-carrying enums, arbitrary non-scalar payloads, ownership-bearing generic sums, and production backend integration remain deferred. Scalar generic helpers support monomorphic `T`, borrowed `readonly []T` parameters and returns, and writable `out []T` parameters and returns for scalar elements. Calls preserve allocation and region provenance, while local, region, unresolved recursive, and owner-preserving generic returns remain rejected. `?` preserves the full tagged result on failure and copies successful payloads into caller storage. See `examples/59_option_result.cb`, `examples/61_typed_propagation.cb`, `examples/62_sum_interactions.cb`, `examples/71_struct_sum_payloads.cb`, `examples/98_sum_parameters.cb`, and `examples/99_sum_abi_matrix.cb` for register, stack, mixed-class, module, and nested-return coverage.

### Shared composite layout

Cobra uses one native layout contract for structs. The IR computes field offsets and total size, and the direct emitter consumes those same values. Scalar fields keep their existing native slots, qualified top-level byte views occupy a pointer plus length pair, and supported owned fields retain explicit ownership metadata alongside their canonical offsets. Nested by-value structs are laid out recursively when their fields have a supported representation. Fields stay in declaration order without internal padding, and the complete value is rounded to an 8-byte boundary. A by-value cycle such as `struct Node: { next: Node }` is rejected because it has no finite representation. This is a Cobra-native packed layout, not a natural C struct layout. Do not cast a Cobra struct pointer to an ordinary C struct pointer unless an adapter uses the documented field offsets; C-compatible padding and alignment require a separate ABI contract.

Struct parameters use one pointer ABI slot. Value-owned fields are copied into callee-private storage, while ownership-bearing fields use explicit move metadata and cleanup. Nested field reads and scalar writes use canonical offsets directly. Borrowed fields require explicit qualifiers and retain owner and region identity. The isolated backend v2 connects this metadata to copy, move, and cleanup analysis for supported owned strings, owned slices, and owning sums; arbitrary non-scalar aggregate layouts remain deferred.

See `examples/90_recursive_struct_layout.cb` for nested field access, private parameter copies, and the corresponding cycle diagnostic. The layout records whether each field is scalar, borrowed, or owned, and the backend rejects unsupported combinations before ABI lowering.

### Canonical type metadata

The compiler is moving type information into one recursive descriptor implemented in `src/type.c`. A canonical type records its identity, generic arguments, nested payloads, struct fields, ownership, mutability, region origin, size, alignment, and native ABI class. The descriptor finalizer is the single place that computes scalar, slice, sum, collection, and packed struct representations, and it rejects recursive by-value cycles.

Parser declarations now construct and attach canonical descriptors directly, and IR inference propagates those descriptors through locals, calls, and inferred expressions. A small set of scalar compatibility fields remains in native symbol records for transitional code-generation details; they are not used to reconstruct type identity. Canonical generic instantiation currently has one deliberately narrow lane: `cobra_type_substitute` recursively applies exactly one scalar `GENERIC_PARAM` binding through `Option[T]`, `Result[T, E]`, readonly slices, and immutable borrowed-view structs, then finalizes and interns the native ABI. Multiple bindings are rejected until multi-parameter specialization has a complete identity and ABI contract. Source-level functions with one scalar parameter now specialize monomorphically at call sites, for example `unwrap_or[T](value: Option[T], fallback: T) -> T`. Repeated calls reuse the canonical specialization, while unresolved, composite, mismatched, and recursive substitutions are rejected before code generation. Borrowed `readonly []T` parameters and writable `out []T` parameters and returns now specialize to the existing pointer-plus-length ABI for scalar elements. Scalar owned `list[T]` collections also support ownership-moving calls, returns, append, pop, and destruction. Lifetime-aware generic view returns may derive from one corresponding scalar view parameter, with owned actuals adapted only at the call boundary while allocation and region provenance are preserved; local, region, recursive, non-scalar, and arbitrary lifetime-polymorphic generic values remain deferred. Immutable scalar-only generic structs now specialize fields into packed `Box__i64`/`Box__f32` descriptors, support by-value parameter copies and caller-owned struct returns, and reuse canonical specializations across repeated and module-boundary calls. Generic structs may also contain `readonly []T` borrowed fields in non-escaping parameters and locals; `View[i64]`, `View[f32]`, and `View[u8]` preserve pointer-plus-length ABI, readonly mutability, and region ownership. Owned, mutable, returning, heap-stored, recursive, and lifetime-dependent generic structs are rejected before ABI lowering. See `examples/107_generic_borrowed_views.cb` and `examples/108_generic_borrowed_view_module.cb`. Generic functions can now accept these borrowed-view structs, for example `count[T](view: View[T]) -> i64`, with scalar call-site specialization, module reuse, and the same non-escaping lifetime checks. See `examples/116_generic_view_functions.cb` and `examples/117_generic_view_function_module.cb`. New type features should extend the canonical descriptor instead of adding another parallel metadata field.

Code generation reads struct and sum layout, plus calling-convention slot counts, from the canonical descriptor only. A typed declaration without canonical metadata is an internal error, and the legacy `type_layout.c` module has been deleted: struct offsets, sizes, and ABI slot counts now come exclusively from `cobra_type_field_offset` and the canonical finalizer. Parameter alias contracts now use canonical mutability, while IR keeps only a separate flow mutability state for rebinding after aliases. The canonical arena reuses structurally identical component nodes and interning rejects name collisions with a different field shape, so the fully composed standard library no longer exhausts the descriptor table.

### Struct parameter ABI

A scalar-only struct parameter has source-level by-value semantics. Cobra passes a pointer to the caller's struct bytes in one native ABI slot, then copies the complete layout into callee-owned frame storage before the function body runs. Assignments to a parameter therefore cannot mutate the caller's value. Borrow metadata is copied per field, and region-backed fields expire when their owning region ends. Layout sizes are rounded to 8-byte native slots; the validator reports unknown struct types, mismatched struct names, ownership-bearing fields, and unsupported nested fields before assembly generation. Struct parameters are accepted across composed source modules because module composition shares the same validated layout table. See `examples/72_struct_parameters.cb` and `examples/73_http_borrowed_structs.cb`, which also covers borrowed request views, scalar response metadata, regions, modules, and postfix propagation.

### Enums and pattern matching

Unit enums are compile-time declarations backed by ordinary integer values. They use no runtime object and can be passed, returned, stored, and compared like native scalar values:

```cobra
enum Phase: {
    Idle,
    Running,
    Failed = 7,
}

match phase: {
    case Phase.Idle: { print("idle") }
    case Phase.Running: { print("running") }
    case Phase.Failed: { print("failed") }
}
```

A match without `else` must cover every variant. An `else` arm handles the remaining variants. Duplicate cases, unknown variants, mixed enum types, and non-exhaustive matches are rejected before assembly generation. Each arm gets its own local scope, the same as an `if`/`else` branch, so two arms may each declare a local with the same name without colliding. Payload-carrying enums are a later milestone; this first form is intentionally a small, direct foundation for state machines and structured application errors. See `examples/60_enum_match.cb` and `examples/149_match_arm_scoping.cb`.

## Language Keywords

| Keyword | Description |
| :--- | :--- |
| `def` | Function declaration |
| `heap` | Explicit single-value heap allocation (`malloc@PLT`) |
| `alloc_i64` | Zero-initialized owned runtime-sized i64 buffer |
| `alloc_f32` | Zero-initialized owned runtime-sized f32 buffer |
| `alloc_u8` | Zero-initialized owned runtime-sized byte buffer |
| `free` | Explicitly release an owned `alloc_i64` or `alloc_f32` buffer |
| `import c` | Headerless direct C interop for integer/pointer ABI calls |
| `@comptime` | Sandboxed compile-time expression evaluation |
| `@compute` | Universal Compute Fabric 256-bit AVX2 SIMD vector block |
| `@parallel` | Proven element-wise `[]f32` range loop dispatched across a persistent worker pool (AVX2 body per chunk) |
| `len` | Built-in byte/array length operator |
| `string` | Immutable native NUL-terminated byte string passed as one pointer |
| `concat` | Allocate an owned string containing two strings joined together |
| `string_from_bytes` | Allocate an owned string by copying `len` bytes out of a `[]u8` buffer |
| `starts_with` / `ends_with` / `contains` | Native string predicates returning integer booleans |
| `char_at` | Bounds-checked byte lookup |
| `string_free` | Release an owned concatenated string |
| `[]i64` | Non-owning integer array view passed as pointer + length |
| `[]f32` | Non-owning f32 tensor buffer view passed as pointer + element count |
| `[]u8` | Non-owning byte buffer view passed as pointer + byte count |
| `fill_f32` | Native AVX2 fill of a `[]f32` buffer with an integer or fractional f32 scalar |
| `relu_f32` | Native AVX2 ReLU over a `[]f32` buffer |
| `matmul_f32` | Native AVX2/FMA row-major f32 matrix multiply: `C[M,N] = A[M,K] * B[K,N]`, tiled across eight columns |
| `dense_f32` | Fused AVX2/FMA dense layer with bias and ReLU: `output = ReLU(input × weights + bias)` |
| `sum_f32` | AVX2 reduction of a `[]f32` buffer to a float sum (four independent eight-lane accumulators, tree combine, + scalar tail) |
| `mean_f32` | AVX2 float mean of a `[]f32` buffer; empty buffers yield 0.0 |
| `max_f32` | AVX2 float maximum of a `[]f32` buffer with a scalar tail (softmax stability) |
| `exp_f32` / `sqrt_f32` / `tanh_f32` / `log_f32` / `pow_f32` | Transcendental math intrinsics dispatching to libm (`expf`, `sqrtf`, `tanhf`, `logf`, `powf`) with f32 ABI |
| `struct` | Native contiguous struct memory layout |
| `for ... in` | Iterator loop over an array/slice, or index-range loop over `len(view)` |
| auto-vectorization | Index-pure `[]f32` range loops lower to AVX2 automatically; any loop the analysis cannot prove element-wise stays checked scalar |
| `if / else` | Conditional branching |
| `while` | Loop execution |
| `asm:` | Bare-metal inline assembly block |
| `print` | Standard console output |

## Project manifest and package paths

Cobra optionally discovers the nearest `cobra.toml` by walking upward from the entry source file. The supported minimal shape is:

```toml
[package]
name = "my_model"
version = "0.1.0"

[dependencies]
math = "vendor/math"
```

For `import "math/linear.cb"`, resolution is deterministic: first the path relative to the importing file, then the manifest dependency prefix, then `COBRA_LIB_PATH`. Manifest dependency paths must be relative and resolved files must remain inside the project root. Unknown sections or package keys are rejected rather than ignored. The nearest manifest governs the complete composed graph; imported packages do not introduce nested runtime or manifest state.

The manifest is read only by the compiler driver and affects compile-time source composition. It adds no runtime object, linker lookup, or model execution overhead. If no manifest exists, existing relative imports and `COBRA_LIB_PATH` behavior remain unchanged.

## Portable scalar baseline

`--cpu=portable` disables auto-vectorization and selects scalar lowering for ordinary loops on the current Linux x86_64 backend. It is useful on x86_64 machines without AVX2, but it does not emit ARM64 or Wasm code. `--cpu=native` and `--cpu=avx2` enable the Linux x86_64 vector path when the host supports it. Tensor intrinsics remain an explicit AVX2 and FMA lane; `--cpu=portable` rejects them instead of emitting instructions the target cannot execute.

This split keeps the scalar contract honest: scalar values, slices, control flow, structs, errors, files, and ordinary application code have a baseline lowering. The driver follows calls into auto-loaded libraries, omits unreachable AVX-backed `lib/nn.cb` helpers, and rejects reachable tensor kernels instead of emitting instructions the target cannot execute. Scalar-safe wrappers such as `sigmoid_buf` remain available because their loop and math path lower to checked scalar code. The separate `--target=arm64`, `--target=win64`, and `--target=wasm32` options still select interface stubs, not production backends.

## Cobra source modules

Functions are public by default for compatibility with existing source modules. Mark a boundary explicitly when a helper must stay internal:

```cobra
private def parse_detail() -> i64: {
    return 40
}

pub def parse() -> i64: {
    return parse_detail() + 2
}
```

A private function can be called by code in its own module, but an imported module cannot reach it through an alias. The compiler reports the visibility error before code generation, and codegen emits the function as a local native symbol rather than an exported `.global` symbol. Use explicit `pub` and `private` declarations for new modules; future package tooling can make private-by-default modules opt in without changing the call syntax.

A plain `import "path/to/module.cb"` is a compile-time source import. The compiler resolves the path relative to the importing file, loads each module once, then composes the resulting functions into the same direct native program. There is no module object, dynamic loader, runtime lookup, or call overhead:

```cobra
import "modules/math.cb" as math

result = math.module_add(40, 2)
```

The `as math` alias and `math.` qualifier are compile-time only. The emitted call is still a direct call to `module_add`; no namespace object or runtime lookup exists. Plain imports without an alias remain valid, so a library can be used either as `module_add(...)` or, when collision safety matters, as `math.module_add(...)`.

Imported modules can import other modules with the same relative rule. The loader canonicalizes paths, rejects cyclic imports, and prevents diamond-shaped graphs from compiling the same file twice. `import c "..." (...)` remains the separate headerless native C bridge described below. Source modules currently share one native symbol namespace, so function definitions must still be unique across the composed program. Aliases improve call-site clarity; they do not create runtime objects or change native symbol ownership.

See `examples/39_source_modules.cb` and `examples/modules/math.cb`.

## Headerless C interop

```cobra
import c "libc.so.6" (abs)

value = abs(0 - 42)
```

`import c "library" (name, ...)` registers imported symbols for direct native calls and adds the library to the linker without a shell. Bare library names use the linker's exact-name form (`-l:library`); paths are passed as path arguments. The current bridge intentionally supports up to six integer, pointer, or string-pointer arguments on Linux x86-64 SysV and treats the result as an integer/pointer value in `rax`; imported calls must therefore use an integer/pointer-returning C function. Floating-point foreign arguments or returns, and more than six arguments, are outside this bridge and should be wrapped with a typed Cobra function or C shim rather than guessed. For typed f32 model code, use a normal Cobra wrapper or the C host ABI example in `examples/34_c_abi_host.cb` and `examples/34_c_abi_host.c`.

The emitted Cobra functions are global SysV symbols. A `[]f32` parameter is exposed as `(float *values, long length)`, so a C host can call a user-authored kernel without a runtime tensor object:

```bash
./cobra emit-asm examples/34_c_abi_host.cb -o /tmp/cobra_host.s
gcc -O2 -no-pie /tmp/cobra_host.s examples/34_c_abi_host.c -lm -o /tmp/cobra_host
/tmp/cobra_host
```

## File system and time libraries (lib/fs.cb, lib/time.cb)

`lib/fs.cb` and `lib/time.cb` ship in the same auto-prepended set as `lib/std.cb` and `lib/nn.cb`, so the functions below are in scope in every program with zero imports. They are ordinary Cobra on the integer/pointer bridge and compile to direct libc calls:

| Function | Behavior |
|---|---|
| `fs_open(path, flags)` | `Result[i64, i64]` file descriptor; error 1 on open failure |
| `fs_open_read(path)` / `fs_open_write(path)` / `fs_open_append(path)` | checked descriptor helpers |
| `fs_read_fd(fd, buf, max_bytes)` | `Result[i64, i64]` byte count; `ok(0)` is EOF |
| `fs_write_fd(fd, buf, bytes)` | `Result[i64, i64]` byte count; short positive writes are success |
| `fs_write_string(fd, content)` | checked string write with byte count |
| `fs_close(fd)` | `Result[i64, i64]`; second close reports error 1 |
| `fs_write_file_checked(path, content)` | checked path-level text write |
| `fs_write_file_bytes_checked(path, buf, bytes)` | checked path-level binary write |
| `fs_read_file_checked(path, buf, max_bytes)` | checked path-level binary read |
| `fs_read_file_f32_checked(path, buf, max_bytes)` | checked read into f32-backed scratch storage |
| `fs_size(path)` | raw file size in bytes, or -1 |
| `fs_exists(path)` | raw 1 when readable, else 0 |
| `fs_write(path, content)` / `fs_read(path, buf, max_bytes)` | original raw path-based compatibility helpers |
| `fs_append(path, content)` / `fs_remove(path)` / `fs_rename(old, new)` | raw compatibility helpers |
| `fs_buffers_equal(left, right, n)` | binary comparison including zero bytes |
| `time_now()` | Unix seconds, raw integer helper |
| `time_now_checked()` | `Result[i64, i64]`; error 1 on libc clock failure |
| `time_precise_us()` | microseconds since the epoch, or -1 on `gettimeofday` failure |
| `time_precise_us_checked()` | `Result[i64, i64]`; error 1 on `gettimeofday` failure |
| `time_elapsed_us(start)` | raw integer delta from a `time_precise_us()` point |
| `time_elapsed_us_checked(start)` | `Result[i64, i64]`; error 2 for invalid or rolled-back wall timestamps |
| `time_monotonic_us()` | monotonic microseconds, or -1 on clock failure |
| `time_monotonic_us_checked()` | `Result[i64, i64]`; error 1 on monotonic clock failure |
| `time_elapsed_monotonic_us_checked(start)` | monotonic `Result[i64, i64]` duration; error 2 for invalid timestamps |
| `time_clock_ticks()` | process CPU time in CLOCKS_PER_SEC ticks, raw integer helper |
| `time_clock_ticks_checked()` | `Result[i64, i64]`; error 3 on CPU clock failure |

Strings are NUL-terminated char pointers and pass directly to libc. The checked filesystem lane uses raw descriptor handles, not `FILE*` objects, so it has no hidden stream state. Linux flags are `0` for read, `577` for write and truncate, and `1089` for append. `[]i64` buffers provide count times 8 bytes of storage, while `[]f32` buffers provide count times 4 bytes for temporary reads; requests are clamped to capacity. A positive short write is successful progress and callers may retry the remaining bytes. A read of zero bytes is successful EOF. Errors use stable operation codes: 1 for system I/O failure and 2 for invalid arguments. The old path-based stream operations remain under explicit raw names where compatibility matters, and stat, exists, append, remove, and rename remain raw scalar helpers until richer payload types exist. Directory iteration is intentionally deferred because it needs persistent state. Temporary file descriptors are closed on checked success and failure paths. Time failures use native `Result[i64, i64]` values with integer error codes, while the raw helpers remain available for low-level code that accepts sentinel values. `time_precise_us()` also returns -1 when `gettimeofday` fails. Use `time_monotonic_us_checked()` and `time_elapsed_monotonic_us_checked()` for durations that must not depend on wall-clock adjustments. Temporary timeval and timespec storage is released on every checked and raw path. The emitted assembly calls libc directly (`open@PLT`, `read@PLT`, `write@PLT`, `close@PLT`) with no runtime layer. See `examples/51_systems_lane.cb`, `examples/63_time_results.cb`, and `examples/64_fs_results.cb`.

## Raw networking and checked socket results

`lib/net.cb` is auto-prepended with the other system libraries. It keeps the original raw `net_*` helpers and adds checked operations that return `Result[i64, i64]` without allocating a socket object:

| Function | Contract |
|---|---|
| `net_socket_checked()` | IPv4 TCP descriptor or a stable error |
| `net_reuseaddr_checked(fd)` | Enable immediate address reuse |
| `net_bind_checked(fd, port)` | Bind to all interfaces; port `0` asks the kernel for a free port |
| `net_port_checked(fd)` | Read the kernel-selected local port |
| `net_listen_checked(fd, backlog)` | Begin accepting connections |
| `net_accept_checked(fd)` | Accept one client descriptor |
| `net_connect_checked(fd, host, port)` | Connect to a dotted IPv4 address |
| `net_send_checked(fd, text)` | Return bytes accepted by the kernel; short writes remain visible |
| `net_send_buffer_checked(fd, buf, bytes)` | Send binary buffer bytes, clamped to buffer capacity |
| `net_recv_checked(fd, buf, max_bytes)` | Return received bytes; `ok(0)` means the peer closed |
| `net_set_nonblocking_checked(fd)` | Set nonblocking mode |
| `net_close_checked(fd)` | Close a descriptor; a stale descriptor maps to error `3` |
| `net_socket_pair_checked(handles)` | Create a local full-duplex pair for IPC and tests |

Stable network errors are `1` for generic I/O failure, `2` for invalid arguments, `3` for a closed or invalid connection, `4` for address already in use, `5` for connection refusal or failure, `6` for timeout, and `7` for an interrupted operation. These are Cobra-level codes. Linux `errno` values remain private to `lib/net.cb` and are classified at the failure site. A successful send or receive never hides progress: positive short counts are returned directly, and a receive count of zero is EOF. The current raw layer is IPv4 and Linux x86-64 oriented; TLS, DNS, HTTP parsing, and directory-style persistent state belong above this layer. See `examples/65_net_results.cb` for deterministic socket-pair, EOF, invalid-argument, listener, nonblocking, and cleanup coverage.

## HTTP/1.1 foundation

`lib/http.cb` is auto-prepended after the socket layer. It parses bounded HTTP/1.1 request bytes without converting them to NUL-terminated strings or allocating hidden request objects. Every buffer is caller-owned `[]u8`, and metadata is returned through caller-owned scalar arrays:

```cobra
def parse_request(request: []u8, size: i64) -> Result[i64, i64]: {
    method = alloc_i64(2)
    path = alloc_i64(2)
    version = alloc_i64(2)
    return http_parse_request_line(request, size, method, path, version)
}
```

The request-line parser accepts a method, target, and `HTTP/1.1` version separated by exactly two spaces. It rejects missing delimiters, control bytes, extra spacing, unsupported versions, and lines above 8192 bytes. `http_parse_header` returns name and trimmed value spans. `http_parse_headers` enforces a 16 KiB total header limit, 64-header limit, rejects control bytes and duplicate `Content-Length`, and returns the body offset plus the parsed content length. Header names compare case-insensitively. Chunked transfer encoding is intentionally not accepted by this first layer.

HTTP error codes are `10` invalid request line, `11` oversized headers, body limit, or too many headers, `12` invalid header or argument, `13` invalid or duplicate content length, `14` unsupported HTTP version, `15` truncated body, `16` no progress during a response write, and `17` response buffer too small. Actual socket failures preserve the stable NetError codes from `lib/net.cb` rather than being collapsed into an HTTP code. `http_request_parse` is the transitional typed single-request entry point: it fills a caller-owned twelve-slot `request_meta` record with method, target, version, header, and body offsets and lengths, then returns `Result[i64, i64]`. Slots 8 and 9 describe the body bytes currently available in the receive buffer; slot 10 is the declared Content-Length, and slot 11 is the explicit close flag. `http_request_body_expected` reports the declared length, while `http_request_body_meta` exposes only the currently available range for a caller-created `slice_u8` view. `http_request_header` scans headers without allocation, and `http_request_method_is` plus `http_route_matches` provide direct route checks. `http_read_body` retries partial receives into zero-copy `slice_u8` windows and preserves progress through its output slot. `http_response_send` composes `http_build_response` and `http_write_response` with typed `?` propagation, so short sends remain visible. Responses always use explicit `Connection: close`; keep-alive and chunked transfer encoding are intentionally rejected until a connection-state object exists. The current compiler does not yet allow slice fields inside structs or struct payloads in `Result`, so this metadata record is an explicit transitional ABI rather than a hidden request object. Malformed request lines, duplicate lengths, NUL bytes, oversized headers, and unsupported transfer encoding are covered by `examples/66_http_foundation.cb`, `examples/70_http_typed_api.cb`, and the typed borrowed-view handler in `examples/73_http_borrowed_structs.cb`. See `examples/66_http_foundation.cb`, `examples/67_http_server_checked.cb`, and `examples/70_http_typed_api.cb`.

## Python comfort builtins

Cobra accepts ordinary Python-shaped iteration while keeping the lowering native:

```cobra
for i in range(10): {
    print(i)
}
for index, value in enumerate(values): {
    print(index)
    print(value)
}
```

`range(stop)`, `range(start, stop)`, and `range(start, stop, step)` use integer counters. A zero step is rejected. `enumerate` accepts a named array, slice, or list. `sum`, `min`, `max`, `any`, and `all` accept one named integer array, slice, or list. Empty collections return `0` for `any` and `1` for `all`; `min` and `max` report a runtime error for empty collections. These constructs lower to direct native loops, not boxed iterator objects.

## Native strings

Strings are immutable NUL-terminated byte strings represented by one native pointer. Literals and typed `string` parameters/returns use the ordinary GPR ABI without a hidden object header. `+` and `concat(a, b)` allocate a new owned string; comparisons are lexicographic; `starts_with`, `ends_with`, and `contains` return integer booleans; `char_at` returns a byte and traps on invalid indexes. Release concatenated or functions proven to return fresh concatenation storage with `string_free`; literals and borrowed forwarded values are not owned. `string_from_bytes(buf, len)` allocates and copies `len` bytes out of a `[]u8` buffer plus a NUL terminator, producing an owned string usable anywhere a string is (including as a `dict[string]...` key) from runtime-derived data such as parsed input or file contents, not just source literals and concatenation. Source escapes decode `\\n`, `\\r`, `\\t`, `\\\\`, and `\\\"`.

See `docs/STRING_FOUNDATION.md` and `examples/31_string_foundation.cb`.

## Shape-Aware Tensor Views

Tensor annotations are the natural next step after `[]f32` views:

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

`tensor[...]f32` is a compile-time shape contract with an adaptive native representation. A direct `alloc_f32` tensor has no heap header: its data pointer and element count use the established fast path, while view values carry a compact descriptor in the native stack frame. The descriptor contains pointer, element count, rank, dimensions, and element strides; it is copied through tensor calls, never heap-allocated. Dimension names such as `batch` and `hidden` are symbolic; repeated names must agree across a call. Numeric dimensions are checked when known, and rank mismatches are rejected before native assembly is emitted. A shaped declaration also checks a literal allocation size, so `tensor[2, 3]f32 = alloc_f32(5)` fails early. Dynamic dimensions remain valid and retain native runtime bounds checks.

Shape checks are additive: existing `[]f32` code remains valid and intentionally stays dynamic. Use `tensor[...]f32` at library boundaries where a senior developer wants the compiler to document and verify the model contract, while keeping ordinary buffer code lightweight. `matmul_f32` and `dense_f32` also validate known matrix layouts against their `M, N, K` arguments without changing the fast kernel or ABI.

A typed array view uses `[]i64` in a function parameter:

```cobra
def scale_values(values: []i64): {
    @compute: {
        for value in values: {
            value = value * 2
        }
    }
}

values = [1, 2, 3, 4]
scale_values(values)
```

`[]i64` is not a heap object and does not add a runtime. Native lowering passes two scalar values directly through the platform ABI: the array pointer followed by its element count. `len(values)`, indexing, iteration, and the supported `@compute` kernels use that pair. The view is non-owning; the caller owns the backing storage, and returning or storing views beyond that storage's lifetime is not supported.

The tensor foundation supports `[]f32` views, shaped local `tensor[...]f32` values, checked indexed reads/writes, and index-range loops such as `for i in len(values)`. Typed model calls are supported on Linux x86-64: scalar `f32` parameters/returns use XMM registers, while tensor parameters/returns use zero-copy stack-resident descriptors passed by pointer. Range loops preserve the integer index, so `values[i]` uses checked 4-byte f32 addressing; ordinary `for value in values` remains the element-iteration form. `for i in <scalar>` is also an index-range loop: a non-slice integer target is evaluated once as the loop bound. Decimal literals such as `1.5` and `2.5` are lowered as native IEEE-754 f32 bit patterns. See `examples/30_abi_contracts.cb`; `f64` remains reserved until double-precision lowering is implemented.

Mixed integer/float arithmetic coerces the integer operand to f32, so `sum_f32(v) / len(v)` and `values[i] * 2` behave like Python numerics. `sum_f32`, `mean_f32`, and `max_f32` return f32, keeping softmax and normalization math float-precise. Functions may declare more than three slice parameters: slots beyond `r9` are passed on the stack per SysV and loaded from `[rbp + 16 + 8k]`, so model forward functions with many buffers work without an intermediate struct. Every function reserves a 1 KiB native frame: user local slots start at offset 192 and may use up to 1008 bytes, leaving the region the inlined AVX2 kernels use for their dimensions and loop counters exclusive. Model functions therefore hold ten or more slice buffers plus all of their loop bookkeeping (examples 25 and 26 use 7- and 10-buffer signatures). Writable indexing works on both `[]f32` (float values, integers coerced) and `[]i64` (integer values) buffers, so token-id and index buffers can be built and mutated directly.

View operations are explicit and zero-copy: `reshape_view(tensor, rows, cols)` proves element-count preservation and retains contiguous layout; `slice_view(tensor, start, length)` is supported for contiguous sources and advances the pointer; it rejects strided transpose sources; `transpose_view(tensor)` swaps rank-2 dimensions and strides for checked indexing. Contiguous kernels reject strided views with a clear failure instead of silently treating them as flat buffers. A one-index tensor access is flat storage access; two indices are logical stride-aware rank-2 access. Views are borrows: release derived views before their owner, and freeing a view never releases the source allocation.

`sum_f32`, `mean_f32`, and `max_f32` use four independent eight-lane AVX2 accumulators combined with a tree reduce, plus a scalar tail; empty sums and means remain zero. The four accumulators break the FP dependency chain the same way a `-ffast-math` C compiler unrolls a sum, so the result can differ from a left-to-right scalar sum by a few ulps for non-exactly-representable data. `matmul_f32` uses an AVX2/FMA eight-column tile when the output width permits it and a scalar tail for the remaining columns. The same direct lowering is used by `dense_f32`; its fused bias and ReLU stay in the vector register before one store. `dense_f32(input, weights, bias, output, M, N, K)` uses the same row-major layouts, starts each output with `bias[j]`, accumulates `input[i,k] * weights[k,j]`, applies ReLU, and writes `output[i,j]` without an intermediate matrix. It validates `input >= M*K`, `weights >= K*N`, `bias >= N`, and `output >= M*N`. Linux native builds and tests require AVX2 and FMA; this is an explicit capability check rather than a runtime illegal-instruction crash.

## Automatic Vectorization of User f32 Loops

Index-range loops over `[]f32` views auto-vectorize when every statement in the body is index-pure. No `@compute`, builtin, or annotation is required:

```cobra
def relu6_in_place(values: []f32): {
    for i in len(values): {
        values[i] = values[i] * 2.0 + 1.0
        if values[i] < 0.0: {
            values[i] = 0.0
        }
        if values[i] > 6.0: {
            values[i] = 6.0
        }
    }
    return 0
}
```

A single bounded analysis decides vectorizability rather than a catalog of pre-enumerated patterns:

* Accepted statements are `buffer[i] = expr` where `expr` uses `+ - * /`, numeric literals, loop-invariant scalars, and only current-element reads (`buffer[i]`), plus masked constant clamps such as the ReLU/ReLU6 `if buffer[i] < 0.0 { buffer[i] = 0.0 }` form.
* Anything else, including nested loops, accumulators, `buffer[i-1]` or other non-current indices, and function calls, keeps the checked scalar lowering. Vectorization changes speed, not semantics.

The vector body runs eight lanes at a time while `index + 8 <= len`; the remaining 0 to 7 elements run through the existing per-element bounds-checked scalar tail, so vector loads and stores are in bounds by construction. `values[i] * 2.0 + 1.0` lowers to a single `vfmadd213ps` (FMA fusion), ReLU clamps to `vmaxps`, and ReLU6-style pairs to `vmaxps` + `vminps`. The iterator and tile bound live in registers (`rdx`/`rcx`) and element-wise operands are either loads from the buffer, register broadcasts of loop-invariant scalars, or `.rodata` broadcasts of constants, so the vector body has zero stack traffic on the common path. Only a non-leaf subexpression (rare in element-wise math) touches a scratch slot, once per tile, so model functions with many buffers fit the frame budget. Vectorization is on by default and can be disabled with `-O0` or `--cpu=portable`, which keeps every such loop on the checked scalar path on Linux x86_64. As with the native `relu_f32` builtin, the vectorized clamp path uses `vmaxps`/`vminps`, which return the non-NaN operand when a lane is NaN. NaN inputs therefore take the clamp value on the fast path rather than the NaN-safe scalar comparison result. FMA fusion also combines `a*b + c` into one rounding step, so the vector result can differ from the scalar path by at most one ulp. Vectorization currently applies to `[]f32` range loops on the Linux x86_64 target (AVX2); other targets compile the same loops scalar.

## Native Parallel Element-wise Kernels

`@parallel` is the base-language multi-core primitive for model authors and library builders. It accepts one `for i in len(values)` loop whose body passes the same index-purity proof used by auto-vectorization:

```cobra
def scale_parallel(values: []f32): {
    @parallel: {
        for i in len(values): {
            values[i] = values[i] * 2.0 + 1.0
        }
    }
}
```

On Linux x86_64, a proven `@parallel` loop is lowered to a standalone worker function with signature `void worker(void *context, size_t start, size_t end)` plus one call to the persistent worker pool in `runtime/cobra_parallel.c`. The pool is spawned once per process and reused, so repeated parallel blocks never pay `pthread_create`/`pthread_join`; each call partitions the range into contiguous disjoint chunks and runs the AVX2 eight-lane body plus checked scalar tail per chunk. The worker captures the touched `[]f32` buffers and loop-invariant f32 scalars from a contiguous context region in the caller's frame. Small ranges (under 1024 elements) and single-core hosts stay single-threaded, so annotation overhead is one indirect call rather than a synchronization round trip. The pool is a process-wide singleton: at most one job is in flight, and a nested dispatch from inside a worker callback or a concurrent call from another application thread degrades to inline execution on the calling thread instead of clobbering the shared job record. Parallelism is therefore an optimization, never a correctness contract. `COBRA_WORKERS` (1 to 32) overrides the participant count for controlled scaling experiments and benchmarks. The inline fallback means a concurrent caller runs its whole range sequentially on its own thread: applications that fan out many application threads over `@parallel` blocks should coordinate so one dispatch is in flight at a time, since concurrent dispatches serialize rather than share the pool. The lowering deliberately rejects hidden scalar captures, calls, reductions, nested loops, and non-`[]f32` ranges; those cases use the ordinary checked loop instead of changing semantics. When an `@parallel` body falls back specifically because it contains a nested loop, the compiler prints a `file:line: note:` diagnostic to stderr at compile time so the fallback is visible rather than silent; the build still succeeds and the loop still runs correctly, just sequentially. Other fallback reasons (captures, calls, reductions, non-`[]f32` ranges) do not yet emit this note. Other targets also retain the scalar path.

`@parallel` is therefore complementary rather than mandatory: users can write ordinary Cobra model code and libraries, use automatic SIMD when one core is enough, and keep the multi-core annotation ready for large independent element-wise stages. `examples/27_parallel_kernels.cb` covers the small-range fallback and the large worker path.

A bare `for i in len(values):` loop that passes the same index-purity proof is now dispatched to the worker pool automatically, without writing `@parallel:` at all. The explicit annotation still works exactly as before and is the only form that prints the nested-loop fallback note; automatic detection stays silent on ineligible loops since most ordinary loops were never asking to be parallelized. The runtime's existing size threshold means this is purely additive: an eligible loop only ever runs faster or the same, never differently, since the underlying proof and dispatch mechanism are identical either way. `examples/145_automatic_parallel_dispatch.cb` covers in-place and cross-buffer automatic dispatch.

### Automatic GPU dispatch

There is no `@gpu` source annotation; GPU-capable builtins (`matmul_f32`, `relu_f32`, `sum_f32`, `mean_f32`, `max_f32`) decide for themselves whether to run on the GPU or fall back to the CPU AVX2 path, on every call, with no code change required. `cobra_gpu_should_dispatch` (`runtime/cobra_gpu.c`) gates this with a cost model, not just a capability check: it compares the operation's total arithmetic work (`M*N*K` for matmul, element count for relu/reduce) against a threshold set from real per-call round-trip cost (buffer upload, dispatch, fence wait, readback - no residency or reuse across calls) measured on real GPU hardware. Below the threshold the fixed per-call GPU cost outweighs what the kernel saves versus AVX2, so it stays on the CPU path; above it, GPU dispatch measured faster. If dispatch is attempted and fails for any reason (no device, OOM, driver reject), the AVX2 path runs unchanged - GPU dispatch is strictly additive and never affects correctness, only which path a given call takes. This model prices every call as a one-shot round trip; it does not yet account for a buffer that stays resident and is reused across many calls (see `cobra_gpu_resident_*` for that separate path), where GPU dispatch would be worth it at a much smaller size.

## Neural Network Reference Library (lib/nn.cb)

`lib/nn.cb` is auto-prepended to every program after `lib/std.cb` by the same mechanism that makes the standard library available: model math is always in scope, with no import syntax and zero runtime library. Every function in it is ordinary Cobra code composed from the native AVX2 builtins (`matmul_f32`, `dense_f32`, reductions), the libm intrinsics, and auto-vectorized user loops:

* **Activations:** `softmax_f32(row)`, `softmax_rows_f32(p, seq)`, `sigmoid_buf(x)`, `gelu_buf(x)`, `dense_relu_f32(input, w, b, out, m, n, k)`
* **Normalization:** `layernorm_f32(x)` (whole-buffer mean-centering and unit variance; the normalize pass auto-vectorizes)
* **Attention:** `transpose_f32(a, b, m, n)`, `scale_attention_f32(p, d)`, `attention_forward(q, k, v, p, out, kt, seq, d)` (scaled dot-product attention, both GEMMs native AVX2), and `self_attn_block_f32(...)` (a full encoder block: self-attention, layer norm, MLP)
* **Embedding:** `embed_f32(table, ids, out, dim, count)` with writable `[]i64` token-id buffers
* **Convolution:** `im2col_f32(input, col, in_h, in_w, c, k, stride)` plus `conv2d_f32(...)`: im2col turns convolution into one AVX2 GEMM
* **Pooling:** `maxpool_f32(input, out, in_h, in_w, pool)`
* **Loss:** `mse_loss_f32(a, b, out, n)`

Conventions: buffers are caller-owned `[]f32` (`alloc_f32`/`free`), workspaces (attention probabilities, im2col columns) are caller-provided so hot loops never allocate, and shapes are integer arguments. `examples/26_nn_library.cb` validates every operation against hand-computed values, including a full encoder-block forward pass.

## Owned Dynamic Buffers

Use `alloc_i64(count)` when the size is known only at runtime:

```cobra
def process(values: []i64): {
    @compute: {
        for value in values: {
            value = value * 2
        }
    }
}

def main(): {
    values = alloc_i64(4096)
    process(values)       # borrowed pointer + length, no copy
    free(values)          # creator releases the owned buffer
}
```

`alloc_i64` returns a zero-initialized `[]i64`-shaped buffer. `alloc_f32` returns the same pointer-plus-count contract with 4-byte elements. Negative counts fail; zero creates an empty view. The allocation is exact-sized and uses the platform allocator directly. These raw buffers remain explicitly owned: freeing a borrowed function parameter, freeing twice, or using a buffer after `free` is rejected by Cobra's validation pass. Implicit slice aliases are also rejected until shared ownership identity exists. This explicit model is intentionally separate from automatic everyday lists, dictionaries, and owned strings.

## Buffer alias contracts (`out` / `readonly`)

Slice parameters may declare how they are used so validation and the kernel lowerings can rely on it. `readonly []f32` is a read-only contract: the IR rejects any write to the buffer, including writes through an implicit alias. `out []f32` marks the write target and documents that it must not alias another buffer parameter; it is a contract for callers and kernels, not an enforced rule today. Plain `[]f32` keeps the default read/write contract. Both words are contextual keywords, so they remain valid identifier names outside the parameter slot. See `examples/55_alias_contracts.cb`.

## Constant-shape kernel specialization

`dense_f32(a, w, b, out, M, N, K)` with compile-time M, N, K (integer literals or `@comptime` expressions) and N a multiple of eight lowers to a specialized kernel instead of the checked generic GEMM. Immediate trip bounds replace stack-loaded counters, the per-row vector/scalar decision and the N-tail branches disappear, M=1 omits the row loop and its multiplies, N=8 omits the column loop, and K<4 unrolls the dot product. When K is a multiple of four and at most 64, the whole K loop unrolls to straight-line FMA: every k step is one broadcast, one load, and one FMA addressed by immediate A/B displacements, so the hot path has no counter, no branch, and no pointer advance, and the four-accumulator combine runs inline per tile. The M=1 tile counter then lives in a register (rbx, saved by the frame) instead of a stack slot, so the tile loop has zero stack traffic. Every row of a batched call is emitted with its own back-edge; a past emitter fell through after row 0 for the M>1, N==8 constant shape, which `test_batched_single_tile` in `examples/56_constant_shape_kernels.cb` now locks in. The four-accumulator FMA tiling is unchanged, so results match the generic kernel up to FP reassociation within a few ulps. Any non-constant or non-multiple-of-eight shape keeps the checked generic lowering. See `examples/56_constant_shape_kernels.cb`.

The checked generic GEMM also uses a two-tile output block whenever at least sixteen columns remain at runtime. One A broadcast feeds two adjacent eight-column B loads and eight accumulators, then both tiles are stored together. The existing eight-column and scalar tails remain in place for the final columns. This is a compiler-only layout choice: buffers stay row-major `[]f32`, no packed copy or runtime descriptor is added, and GEMM requires the output buffer to be disjoint from its input and bias buffers. Direct same-variable aliases are rejected by the IR; indirect pointer aliases remain a caller contract. `benchmarks/gemm_dynamic_blocked_repeat.cb` covers the runtime-shaped path.

## Region arenas (`with region`)

`with region NAME(capacity): { body }` creates a bump arena with an optional capacity, default 1 MiB, given as any integer expression. Inside the body, `NAME.alloc_f32(count)` returns a `[]f32` backed by the arena and `NAME.alloc_u8(count)` returns a byte view backed by the same arena. The userland memory library uses the exact native contract `arena_create(state: out ArenaState, capacity) -> pointer`, `arena_alloc(state: out ArenaState, byte_count) -> pointer`, and `arena_destroy(state: out ArenaState) -> status`. The generated region lowering passes the state pointer in `rdi` and the byte count in `rsi`; f32 allocations request `count * 4` bytes and report `count`, while u8 allocations request and report `count` bytes. The pointer is returned in `rax` and the element count in `rdx` before the destination stores the pair. The backing store is released exactly once, at the natural end of the block or on an early `return` from inside the body. Negative counts, overflowed sizes, and bumps past capacity return failure from the raw helper or fail with a native region diagnostic. `free` rejects region-backed owners because the region owns them. A zero-copy view records its owner, so releasing the view never releases the region and freeing the owner while the view is live is rejected. Returning a region-backed or borrowed buffer is rejected because it would dangle after the release. Regions nest, and a region inside a loop is recreated per iteration. Buffers from a region are valid only inside the block; using them afterward is a use-after-free, matching explicit `free` semantics. See `examples/57_region_arenas.cb`, `examples/69_u8_region_lifetimes.cb`, and the negative tests `tests/negative/45_region_unknown_qualifier.cb`, `tests/negative/46_region_out_of_scope.cb`, `tests/negative/47_region_free.cb`, `tests/negative/48_region_return_dangle.cb`, `tests/negative/71_region_u8_owner_free.cb`, and `tests/negative/72_region_u8_return_dangle.cb`.

An unqualified `alloc_i64(count)`/`alloc_f32(count)`/`alloc_u8(count)` call inside an active `with region` block implicitly binds to the innermost enclosing region, exactly as if written `NAME.alloc_*(count)` -- no qualifier needed. Outside any region, `alloc_*` still uses the global heap and requires an explicit `free`. A region-bound allocation, implicit or explicit, cannot be `free`'d; the region's own exit owns cleanup. See `examples/140_implicit_region_alloc.cb` and `examples/141_implicit_region_alloc_nested.cb`.

### Pool and aligned memory contracts

`pool_create(state, free_list, block_size, count)` returns the pool base or `0`. It rejects nonpositive dimensions, a free list shorter than `count`, and multiplication overflow before calling `mmap`. `pool_alloc` returns a block pointer or `0`; it validates the stored free index before using it. `pool_free` returns `0` for success and `-1` for a null, foreign, out-of-range, misaligned, or already-free pointer. Double-free detection scans the caller-owned free-index stack and adds no hidden runtime metadata. `pool_destroy` unmaps the pool, clears `base`, `block_size`, `count`, and `free_top`, and is a successful no-op when called again after destruction. If its stored state is invalid, it returns `-1` and leaves the state unchanged. `mem_aligned_alloc` accepts positive power-of-two alignments up to 4096, rejects other sizes or alignments, and returns `0` on failure; `mem_aligned_free` returns `-1` for invalid arguments. See `examples/97_memory_contracts.cb`.

## Runtime fatal errors

Runtime fatal errors (byte slice and buffer bounds checks, division by zero, failed `assert`, region capacity and creation failures, string index and predicate bounds) print the source `file:line` of the failing operation ahead of the message, e.g. `[cobra] path/to/file.cb:12: byte slice bounds error`, matching compile-time diagnostics. See `examples/142_runtime_error_location.cb`.

Unbounded recursion prints `[cobra] stack overflow (possible unbounded recursion)` and exits with code `3`, instead of a silent SIGSEGV. This is a SIGSEGV handler installed once per process (an ELF constructor, `runtime/cobra_stackguard.c`, linked into every native build/test/bench binary) rather than a per-call check, so it costs nothing until a real overflow happens. It distinguishes a stack overflow from an unrelated segfault by checking whether the fault address is within a few pages of the stack pointer at the time of the fault; an unrelated segfault (a null-pointer dereference, a wild pointer through FFI) falls through to the OS's normal segfault behavior (exit 139) rather than being misreported. See `examples/157_stack_overflow_diagnostic.cb`.
