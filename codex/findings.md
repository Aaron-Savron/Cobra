# Cobra Lab Findings

This report is based on the executable scenarios in this directory and the existing
cross-language harness. It describes the current tree, including the uncommitted GEMM
blocking work in the working tree.

## What works

The lab passed all positive checks, native tests, the HTTP service build, and the expected
readonly rejection. The scenarios cover:

- ordinary application values with structs, strings, lists, dictionaries, and assertions
- source modules with qualified calls and no runtime module lookup
- region-backed buffers and dynamic `matmul_f32`
- `@parallel` worker dispatch over a buffer
- postfix status propagation through region cleanup
- native socket setup, bind, listen, accept, send, and close code generation

The current cross-language run measured this dense workload:

| Language | Median | Relative to Cobra |
| --- | ---: | ---: |
| C | 1.627 ms | 1.25x |
| Rust | 2.527 ms | 1.94x |
| Cobra | 1.303 ms | 1.00x |
| Python | 1252.544 ms | 961.33x |

This is a useful result for the tested native kernel, not a claim that Cobra is faster for
all programs. The harness includes process startup and compares a narrow dense workload.
The existing benchmark history also shows reductions and memory-bound loops need separate
measurements because launch cost and memory bandwidth can dominate the result.

## What feels good

The direct lowering model is a real strength. Source modules compile away, slices stay close
to their C representation, and the compiler can enforce readonly writes before assembly is
emitted. Regions make temporary tensor storage explicit without forcing every small buffer
through the general allocator. The Python-shaped collection literals and comprehensions are
already a good bridge for application developers.

The language also has a strong performance story for code that matches its current optimizer:
typed f32 loops, dense kernels, explicit buffers, and disjoint parallel work. The optimizer
does not need an opaque runtime object model to produce useful native code.

## What feels weird or incomplete

1. A developer has to know several dialects of function declaration. The canonical form is
   `def name(args) -> type:`, while many examples use an omitted return type. That is valid,
   but the language needs one clear style in generated examples and formatter output.

2. Parallelism is expressed as a nested `@parallel: { ... }` block. It is powerful, but a
   Python developer will initially expect a loop modifier or a function annotation. The
   compiler should also diagnose exactly why a loop is not eligible for parallel lowering.

3. Native locals are often inferred, while user values use `let name: type = value`. That split
   is efficient for experiments but makes ownership and lifetime less obvious in larger code.
   A formatter and a language server should make inferred ownership and buffer provenance visible.

4. The current dictionary is `dict[string]i64`. That is a good first native container but not
   enough for configuration, JSON, HTTP, or package metadata. Users will hit this boundary
   before they hit the floating point backend boundary.

5. Errors are integer status codes plus postfix `?`. This is compact and fast, but it does not
   carry an error type, message, source location, or context. A real web server or package
   manager will quickly become a chain of undocumented numeric conventions.

6. The HTTP example is direct and honest, but the API is too low level for normal server work.
   A small typed `net` and `http` layer should sit above the syscall-shaped functions without
   changing the zero-overhead escape hatch.

7. Source imports are compile-time composition, not modules with visibility, package metadata,
   exports, or versioned dependencies. This is a fine foundation, but it is not yet a complete
   package ecosystem.

8. The current C bridge is deliberately narrow. It handles integer, pointer, and string-pointer
   calls, but typed floating point foreign calls need wrappers. That is a sharp edge for systems
   integration and scientific libraries.

## Recommended next steps

### P0: Make full programs comfortable

- Add `args`, environment variables, exit codes, stdin, stdout, stderr, and filesystem paths as
  typed standard library APIs.
- Define `Result[T, E]` and `Option[T]` semantics, with `?` preserving the short syntax. Include
  structured errors with a message, code, and source context.
- Add a typed JSON or configuration value model. It should support strings, booleans, numbers,
  arrays, and objects without making the numeric tensor path boxed.
- Add package manifests with exports, versions, lock files, and a reproducible local build.
- Add `cobra test` support for fixtures, subprocess tests, and expected compiler diagnostics.

### P1: Reduce syntax friction

- Make one return type spelling canonical and have `cobra fmt` normalize it.
- Add `defer` for cleanup that is not a region allocation. Keep explicit regions for hot paths.
- Add `match` over integer and tagged error values.
- Add named arguments and default values for configuration-heavy APIs.
- Add an explicit `borrow` or `view` spelling for non-owning slices so ownership does not have
  to be inferred from the allocator call site.
- Improve diagnostics with a one-line fix suggestion for missing types, invalid aliases, and
  unsupported optimizer patterns.

### P1: Finish the runtime surface

- Build typed `http`, `tls`, `url`, `process`, `path`, and `time` libraries on top of the existing
  native calls.
- Add a safe concurrent queue, channels, cancellation, and scoped task groups. Keep the current
  persistent pool as the low-level execution engine.
- Add f64, SIMD feature dispatch, ARM64 native lowering, and a portable scalar runtime with the
  same tests.
- Add sanitizer and fuzz modes for the parser, IR verifier, allocator, and network libraries.

### P2: Keep the performance lead honest

- Separate startup, dispatch, compute, and memory bandwidth in every benchmark report.
- Add workloads for parsing, file copying, HTTP requests, JSON, sorting, hashing, and database
  style scans. Dense GEMM alone will distort design decisions.
- Add a Cobra versus C and Rust correctness oracle for every benchmark, including randomized
  sizes, tails, aliases, NaNs, empty buffers, and portable CPU mode.
- Add profile-guided optimization only after representative full-program benchmarks exist.

## Bottom line

Cobra already has a credible native numerical core and a promising safety model for buffers.
The largest gap is not another GEMM trick. It is the application layer: typed errors, files,
processes, structured data, package management, diagnostics, and ergonomic networking. Closing
that gap while preserving direct lowering will make Cobra feel like a better Python for ordinary
programs and still provide the systems control that distinguishes it from Python.
