# Cobra Roadmap

This roadmap covers the work required to turn Cobra's current compiler into a complete systems programming language with a native backend.

## Current position

Cobra currently has:

- A working lexer, parser, semantic validator, and direct Linux x86-64 backend
- Canonical type descriptors with struct layout and ABI metadata
- Ownership, borrowing, and region validation
- Scalar generics, readonly generic views, and scalar generic writable views
- A tested typed HIR and block-argument SSA prototype
- SSA verification and evaluation tests
- A scalar SSA verifier that checks function-owned block ranges, unreachable
  block contents, signatures, effects, dominance, and use ordering
- 72 positive suites and 86 negative diagnostics

The backend IR is currently isolated from production codegen and supports
`i64`, `i32`, `u8`, `u32`, `u64`, `bool`, `f32`, and `f64` scalars, with
byte-width `u8` arithmetic and memory semantics. Its evaluator stores values and call
frames as typed scalar payloads and provides typed frame-local pointer memory.
The HIR builder lowers supported scalar locals into canonical stack slots,
with typed loads for reads and stores for assignments. It also supports
value-owned scalar-field structs with canonical aggregate slots, field addresses,
aggregate copies, indirect struct parameters, and explicit caller-provided
struct return storage. It also lowers ownership-bearing struct fields for
owned strings, owned slices, and nested owning sums with explicit field move, drop, call, and return handling, including owning structs nested
inside owning structs (owning struct fields that themselves carry owned
strings, owned slices, or owning sums) with recursive payload tracking,
allocation-identity-carrying struct parameters through call boundaries, and
non-consuming `is_some`/`is_ok` predicate reads on member sums. Native ownership-bearing sums and struct
  fields now emit through indirect storage with payload moves and recursive
  cleanup. It also lowers bounded fixed value arrays `array[T, N]`
for scalar elements, including canonical inline layout, literal construction,
indexed loads and stores, `len`, aggregate calls and returns, and bounds
checks. Fixed arrays are also supported as struct fields with whole-field
and whole-struct value copies through both native emitters, including
bounds-checked index addressing. Nested fixed arrays `array[array[T, N], M]`
lower with whole-row value semantics: nested literals, index reads and writes
of whole rows, struct fields, calls, returns, and inline aggregate copies in
both native emitters. Fixed arrays of value-only structs (`array[Point, N]`)
lower with whole-struct element writes, struct-returning calls into element
slots, array parameters, struct fields, and inline aggregate copies in both
native emitters. Scalar `list[T]` dynamic buffers are now lowered separately with
explicit length/capacity, growth, append, pop, ownership-moving calls and
returns, destruction, bounds checks, and malformed-IR coverage. Non-scalar
buffers remain deferred. String-keyed scalar dicts
(`dict[string]i64`) now lower end to end: literals, index reads and writes,
`get`/`has`/`set`/`delete`/`pop`/`len`/`free` builtins, ownership-moving
parameters and calls, rehash growth, and use-after-free and double-free
rejection, with native emission calling the production `cobra_dict_*` runtime
in both emitters. Dicts with owning or non-scalar values remain deferred. Scalar generic functions are now monomorphized at the
isolated backend boundary through canonical substitution, with concrete calls,
returns, multiple specializations, and unresolved-generic rejection preserved.
Immutable scalar-field generic structs are now monomorphized through the
same canonical substitution boundary. Generic borrowed-view struct fields
also preserve readonly view layout, field loads and stores, aggregate calls,
and caller-storage returns. Scalar generic `list[T]` functions now reuse the
owned-buffer representation for scalar elements, including append, pop,
ownership-moving calls and returns, explicit destruction, and ownership-flow
rejection. Scalar generic `out []T` functions now reuse the writable borrowed
view representation for scalar elements, including indexed stores, calls,
borrowed returns, provenance propagation, and readonly-to-writable rejection.
Lifetime-aware generic view returns now preserve the single source-parameter
contract after specialization. A returned view may be forwarded from a
specialized readonly or writable view parameter, and an owned actual may satisfy
that borrowed parameter at the call boundary without losing allocation or
region provenance. Frame-local and region-local return sources remain rejected.
Non-scalar generic collections and ownership-bearing generic values remain
deferred.
The existing native backend still emits assembly directly. The isolated lane
now materializes a target-neutral `BirCallAbi` for every function, including
lowered parameter locations, hidden aggregate-return storage, abstract GPR and
XMM ordinals, aligned stack arguments, return locations, and call effects.
Target-independent MIR now lowers verified SSA into virtual registers,
machine-typed operations, explicit CFG edges, ABI moves, memory operations,
call clobber sets, and source locations. An isolated Linux x86-64 emitter now
produces scalar assembly using deterministic spill-all frames. A verified
linear-scan allocator now assigns MIR virtual registers to abstract
GPR/XMM positions or spill slots and honors call clobbers. The allocated
scalar x86-64 emitter now consumes those decisions, while the spill-all path
remains as a reference implementation. The narrow native ownership lane also
emits heap-backed owned slices, region cleanup, string concatenation, explicit string destruction, and cleanup-safe scalar and owned-view returns. Narrow
  owned-slice transfers and destruction are implemented. Native owning
  Option/Result payloads and ownership-bearing struct fields now use indirect
  aggregate storage, moves, field extraction, and recursive drops. General
  non-scalar aggregates, optimized instruction selection, and production backend
  selection remain deferred.



The isolated backend also has pointer contracts, region provenance, explicit
transfers and destruction, path-sensitive ownership checks, and readonly slice
view operations. Source-level HIR now lowers readonly `i64`, `f32`, and `u8`
slice parameters and non-escaping local aliases into that view model; indexed
reads use `view_ptr` plus typed byte-offset arithmetic. Writable views and
caller-derived returned views now have explicit borrow/escape checks.
Source-level owned slices (`alloc_i64`/`alloc_f32`/`alloc_u8`, `free`,
indexed reads and writes, `len`, `slice_u8` subviews, call-boundary borrows,
view aliases) and source-level regions (`with region NAME(capacity)`,
region-qualified allocations, region-exit cleanup) now lower through the same
verifier and evaluator with use-after-free, double-free, and borrow-conflict
enforcement. Non-owning scalar sums (`Option[T]`/`Result[T, E]` with scalar
components) lower end to end: `some`/`none`/`ok`/`err` construction, `is_some`/
`is_ok`, checked `unwrap`/`unwrap_ok`/`unwrap_err`, and sum locals, parameters,
and returns, including nested scalar sums to arbitrary depth
(`Option[Option[i64]]`, `Result[Option[i64], i64]`) with aggregate-component
layout, construction, and access. Value-only struct components
(`Option[Point]`, `Result[Point, i64]`) now lower through the same
aggregate-copy machinery: construction byte-copies the struct into the
component field, `unwrap` copies it out, and struct components pass through
calls and returns like scalar-struct values. Owning struct components
(`Option[Owning]`, `Result[Owning, i64]`) now lower through the aggregate
move/drop machinery: construction byte-moves the struct and its payload
handles into the component field, `unwrap` moves it out and clears the source
tag, and call and return transfer walk the struct fields recursively. Borrowed strings (`string` as
readonly

`[]u8`) lower end to end:
string literals, `len`, byte indexing, `slice_u8` subviews, readonly string
parameters, and borrowed string returns, with bounds checks and escape
rejection for literals and local-derived returns. Unit enums and `match`
lower end to end: `enum` declarations, `Color.Variant` constants, enum
locals/parameters/returns, discriminant comparisons, and match with
exhaustiveness, duplicate-arm, and unknown-variant rejection. Payload-carrying enums now
lower end to end: `enum Shape: { Circle(f32), Rect(i64, i64), Line }`
variant payload type lists, tag-plus-payload aggregate layout with resident
payload slots, `Shape.Circle(2.5)` construction, payload params/returns
through calls, and match arms with payload bindings (`case Shape.Circle(r)`)
plus non-exhaustive, duplicate, unknown-variant, binding-count, arity, and
out-of-subset rejections. Owning enum payloads now lower end to end too:
string, owned-slice, owning-struct, and nested-sum payloads construct,
pass through calls and returns, and match with binding arms; the evaluator
resolves enum tags to variant selectors through the variant registry, and
double-free and use-after-free on extracted payloads are rejected by the
flow analysis. Owned slice
parameters and returns now lower with move semantics, including evaluator
transfer, returned allocation handoff, post-move rejection, and call-boundary
borrow checks. Fresh owned strings from concat and string `+`, indexed reads,
`string_free`, and fresh returns now reuse that transfer path. Direct owning
string and owned-slice Option/Result payloads now lower with explicit move,
extraction, call and return transfer, and `free(sum)` destruction. Nested owning sums now recursively move, extract, transfer, and drop active payloads. Ownership-bearing struct fields now lower recursively for owned strings, owned slices, and owning sums, with field replacement, move, call, return, and drop  checks. Returns inside region bodies remain restricted. Native emission now
  covers the supported owning sum and ownership-bearing struct layouts; arbitrary
  non-scalar aggregate layouts remain deferred.

## Phase 1: Scalar backend IR correctness

- [x] Commit the scalar verifier and evaluator correction in `b75e528`.
- [x] Validate unreachable-block returns against their owning function
  signatures.
- [x] Add verifier tests for malformed handles, opcode signatures, calls,
  returns, edges, effects, and memory metadata.
- [x] Document the scalar IR contract.
- [x] Add golden textual IR tests.
- [x] Add randomized CFG tests.
- [ ] Run AddressSanitizer and LeakSanitizer in CI.

Exit condition:

> Every valid scalar HIR program produces verified SSA, and every malformed
> scalar module is rejected before evaluation. The remaining proof work is
> golden output, randomized CFG coverage, and CI sanitizer jobs.

## Phase 2: Make scalar values typed

- [x] Replace the integer-only value representation with a backend-owned
  `BirScalarValue` payload.
- [x] Carry the typed payload through `HirExpr`, `SsaValue`, evaluator slots,
  call arguments, returns, and scalar prototype memory.
- [x] Store floating-point payloads by bit pattern and normalize booleans.
- [x] Add verifier checks tying constant payload kinds to canonical types.
- [x] Add deterministic scalar printing and comparison behavior.
- [x] Make `f32` dumps preserve exact bit patterns for regression fixtures,
  including signed zero, infinities, and NaN payloads.

Keep this limited to scalar values. Do not add pointers, aggregates, vectors,
or target-specific registers in this phase.

Exit condition:

> The HIR, SSA verifier, printer, evaluator, and call machinery carry typed
> scalar values without converting them through `int64_t`.

## Phase 3: Add real scalar types

Add backend support for:

- [x] `u8` byte arithmetic, comparisons, calls, returns, memory, and wraparound semantics
- [x] `i32`
- [x] `u32`
- [x] `u64`
- [x] `f32` values, arithmetic, comparisons, calls, returns, evaluation, and dumps
- [x] `f64`

For each type, define its constants, arithmetic, comparisons, conversions, canonical identity, evaluator behavior, ABI class, calls, and returns.

Do not add vector types during this phase.

Exit condition:

> The backend evaluator and verifier agree with Cobra's existing type rules for every supported scalar type.

## Phase 4: Add a real memory model

Replace the integer-slot prototype with explicit memory operations.

Add:

- [x] Typed pointer values
- [x] Frame-local pointer address space
- [x] Typed loads and stores
- [x] Byte widths
- [x] Alignment
- [x] Byte offsets and pointer arithmetic
- [x] Stack slots
- [ ] Global addresses
- [x] Null and inactive-frame rejection
- [x] Memory effects
- [ ] Volatile operations
- [x] Explicit call effects

The evaluator may continue to use simulated memory, but its semantics must match the native model.

Exit condition:

> A backend program can allocate stack slots, address them, load values, store values, and pass pointers through verified calls. Global addresses, volatile operations, native pointer ABI lowering, alias analysis, and stack-pointer escape analysis remain separate work.

## Phase 5: Lower Cobra values into backend values

Begin with a fixed-layout aggregate memory contract:

- [x] Canonical aggregate size and alignment
- [x] Field offsets from canonical descriptors
- [x] Address calculation for fields
- [x] Aggregate copy width and alignment
- [x] Clear value ownership for aggregate storage
- [x] Verifier rules for aggregate pointers and accesses
- [x] Evaluator support for aggregate byte copies
- [x] Lower value-owned scalar-field structs from Cobra
- [x] Pass struct parameters through private aggregate copies
- [x] Return structs through explicit caller-provided storage

Start with value-owned structs. They remain mutable in HIR. Ownership-bearing
fields for owned strings, owned slices, and nested owning sums now have explicit
field move, replacement, call, return, and drop contracts. Raw aggregate copies
remain forbidden for ownership-bearing values. Ownership-bearing generic
aggregate values and unsupported non-scalar ownership layouts remain deferred.

### Ownership boundary: pointer contracts

The backend now has the first explicit ownership representation for pointers:

- [x] Distinguish owned-frame, readonly-borrow, writable-borrow, and
  caller-storage contracts from canonical `pointer[T]` identity
- [x] Preserve pointer contracts through stack slots, pointer arithmetic, and
  field addresses
- [x] Require readable pointers for loads and writable pointers for stores and
  aggregate-copy destinations
- [x] Model struct inputs as readonly borrows and hidden aggregate returns as
  caller-provided storage
- [x] Reject unknown contracts, incompatible call arguments, pointer returns,
  and invalid aggregate copy permissions
- [x] Retain runtime frame provenance and inactive-frame rejection
- [x] Add region origins and declared parent-region metadata
- [x] Add explicit ownership transfer between active regions
- [x] Add destruction and region-exit cleanup tracking
- [x] Reject use-after-free and double-free with path-sensitive CFG state

The backend now has a narrow borrowed readonly-view contract. Canonical slice
values carry a typed pointer and element count without owning the backing
allocation. The verifier propagates region and allocation state through
branches, joins, and loop backedges; conflicting live/dead states become
ambiguous and are rejected at later uses. The evaluator enforces bounds and
lifetime state across calls and unwinding. Source-level readonly slice parameters, aliases, checked `slice_u8` subviews,
and indexed reads are lowered. The narrow writable-view lane now lowers `out
[]T` parameters and indexed stores with active-reader/active-writer conflict
checks; caller-derived returned views are escape-checked, while owned collections
beyond source-level slices remain deferred.

Add lowering rules for:

- Scalar locals
- Bounded fixed value arrays
- Slices
- Strings
- Structs
- Readonly views
- Regions
- `Option`
- `Result`

Define the rules for field offsets, address calculation, copies, moves, destruction, borrowed fields, return storage, aggregate parameters, and aggregate returns. Scalar-struct, owning-sum, and ownership-bearing-struct call ABIs are explicit in the isolated evaluator and native lanes. Unsupported non-scalar aggregate ABIs remain deferred.

The backend must consume canonical descriptors. It must not reconstruct type information from AST compatibility fields.

Exit condition:

> Existing Cobra programs involving structs, slices, sums, and regions can be represented in backend IR without frontend-only shortcuts.

## Phase 6: Add an ABI-neutral call model

Status: complete for the isolated SSA lane. Calls now carry a canonical
`BirCallAbi` signature record with:

- [x] Calling convention identifier
- [x] Lowered parameter locations, including hidden return storage
- [x] Return locations
- [x] GPR and XMM register classes with abstract ordinals
- [x] Aligned stack arguments and total stack footprint
- [x] Direct and indirect arguments
- [x] Direct and indirect returns
- [x] Explicit stack alignment
- [x] Call effects
- [x] Explicit non-variadic status

The initial profile uses six GPR argument positions, eight XMM argument
positions, and 16-byte stack alignment. It is target-neutral at the SSA level:
MIR will map these abstract locations to physical registers and concrete stack
frames for Linux x86-64 System V first.

Exit condition:

> The backend IR describes every argument and return location without codegen-side type inference.

The isolated SSA verifier and evaluator enforce this contract. Physical
caller-saved and callee-saved register sets remain a MIR responsibility.

## Phase 7: Build target-independent machine IR

Status: complete for the isolated scalar and memory lanes. MIR now contains:

- [x] Virtual registers
- [x] Abstract physical register constraints through ABI locations
- [x] Stack slots
- [x] Machine-sized signed and unsigned integer types
- [x] Floating-point register classes
- [x] Loads and stores
- [x] Branches and CFG edge arguments
- [x] Calls and returns
- [x] ABI moves
- [x] Conservative abstract call clobber sets
- [x] Source locations

SSA represents program values. MIR represents machine constraints. The
lowering is deterministic and requires verified SSA as input. MIR retains
ownership and memory metadata without depending on AST compatibility fields.

Exit condition:

> A verified SSA function lowers deterministically into verified target-independent MIR.

The isolated MIR verifier checks virtual-register definitions and dominance,
function ownership, signatures, CFG edges, calls, ABI identity, effects, and
memory metadata. Physical register assignment, instruction selection, stack
frame construction, and assembly remain later work.

## Phase 8: Implement the first native backend

Status: partial. The isolated Linux x86-64 emitter now consumes verified MIR
and emits scalar assembly through both deterministic spill-all frames and a
verified allocated path. It also lowers value-owned scalar-field structs using
stack-resident aggregate storage, field addresses, aggregate copies, indirect
parameters, and caller-provided returns. It also lowers borrowed readonly
views as pointer-plus-length values, including view calls, derived returns,
indexed reads, subviews, native bounds traps, writable view stores, heap-backed
owned slices, explicit slice destruction, region cleanup, fresh string
concatenation, and scalar sum checks. Return cleanup preserves scalar payloads
and returned owned-view descriptors across allocator and runtime calls. The  native lane now supports narrow owned-slice argument moves, descriptor
  transfers, explicit destruction, owning Option/Result payload moves and drops,
  ownership-bearing struct field moves and drops, and indirect aggregate calls
  and returns for those layouts. It is not wired into production codegen and
  does not yet support arbitrary non-scalar aggregate operations.

Target Linux x86-64 and the System V ABI first. Scalar output now assembles
and executes in the isolated test harness, with results compared against the
backend evaluator and an equivalent legacy compiler fixture. The native
matrix covers i32 wrapping, u32 and u64 arithmetic and division, f32 and f64
arithmetic, loops, stack-backed locals, calls, and allocated register paths.
Division fixed-register clobbers and loop-backedge liveness are modeled by the
allocator.

Support:

- Scalar integer operations
- Scalar floating-point operations
- Value-owned scalar-field structs
- Stack frames
- Calls
- Indirect aggregate parameters
- Caller-provided aggregate returns
- Returns
- Branches
- Loads and stores
- Aggregate copies
- Borrowed readonly views
- View calls and caller-derived view returns
- Native view bounds checks
- Writable view stores and ABI parameters
- Heap-backed owned slice allocation and destruction
- Region allocation cleanup
- Fresh owned string concatenation and destruction
- Cleanup-safe scalar and owned-view returns
- Owned-slice argument moves and descriptor transfers
- Explicit destruction for supported native ownership forms
- Scalar Option and Result construction checks
- Owning Option/Result payload moves, extraction, indirect calls, and drops
- Ownership-bearing struct field moves, aggregate returns, and recursive drops

Emit assembly before attempting object files. The current bootstrap and
allocated emitters support scalar arithmetic, comparisons, stack-backed memory,
scalar-field struct layout and copies, branches, calls, indirect aggregate
parameters, caller-provided aggregate returns, borrowed readonly views,
view calls and returns, writable view stores and ABI parameters, heap-backed
owned slices, region cleanup, fresh string concatenation, explicit string
freeing, cleanup-safe scalar and owned-view returns, owned-slice argument moves, descriptor transfers, explicit destruction, owning Option/Result payload
  moves and drops, ownership-bearing struct field moves and drops, f32, and f64.
  They continue to reject arbitrary non-scalar aggregate layouts and unsupported
  ownership forms until their native layouts and cleanup sequences are defined.

Use the existing direct emitter as the behavioral and ABI oracle. Compare exit status, output, and assembly contracts for the same programs.

Exit condition:

> The new backend compiles the selected Cobra subset and matches the existing backend's behavior and ABI contracts.

## Phase 9: Register allocation

Status: partial. A target-independent linear-scan allocator now runs after MIR
verification and produces deterministic allocation metadata.

Support:

- [x] Conservative live intervals over MIR CFG order
- [ ] Target-specific fixed register constraints
- [x] Spill slots
- [x] Machine-typed integer classes
- [x] Floating-point register classes
- [x] Call clobber handling
- [x] Internal malloc/memcpy/free clobber coverage for slice, buffer, string,
  drop, destroy, and region-cleanup opcodes
- [x] Register overlap verification
- [x] Spill-backed address values until callee-saved support lands
- [ ] Interval holes and rematerialization
- [ ] Two-address instruction constraints
- [ ] Callee-saved registers
- [x] Native stack-frame rewriting for the scalar emitter
- [x] Feeding allocated locations into scalar instruction emission

The current allocator overapproximates loop and branch liveness, which is safe
but not optimal. General aggregate ownership transfer, general destruction,
callee-saved, and target-specific constraints remain deferred.

Exit condition:

> Allocated MIR passes machine verification and produces correct native programs under register pressure.

## Phase 10: Instruction selection and encoding

Implement table-driven x86-64 selection for:

- Integer arithmetic
- Comparisons
- Branches
- Loads and stores
- Calls
- Returns
- Stack operations
- Floating-point operations
- Address calculations

Keep assembly output available for debugging and comparison. Add direct binary encoding only after assembly output is stable.

Exit condition:

> The backend emits correct assembly for supported MIR and produces identical behavior through the text and binary paths.

## Phase 11: Object files and linking

Add ELF relocatable object output with:

- Symbol tables
- Local and global symbols
- Relocations
- Sections
- Alignment
- External symbol declarations
- Debug location hooks

Use the system linker initially. A complete linker is a separate project.

Exit condition:

> Cobra can produce a valid ELF object without invoking an assembler.

## Phase 12: Performance work

Measure the new backend against:

- The existing Cobra emitter
- Clang
- QBE
- Cranelift where appropriate

Measure frontend time, IR construction time, verification time, register allocation time, emission time, peak memory, code size, and runtime performance.

Publish the workload and measurement method with every comparison.

Exit condition:

> The backend has measured strengths and known weaknesses on representative programs.

## Phase 13: Additional targets

Implement additional targets only after Linux x86-64 is complete:

1. Win64 x86-64
2. ARM64
3. Wasm32

Each target needs its own ABI, register model, object format, relocation rules, stack rules, runtime boundary, and test suite.

Unsupported target output must fail clearly instead of producing a placeholder artifact that looks complete.

## Phase 14: Complete the language surface

Finish the language contracts for:

- [x] Scalar generic functions through canonical monomorphization
- [x] Immutable scalar-field generic structs
- [x] Generic borrowed-view struct fields
- [x] Scalar generic collections over `list[T]` with canonical substitution,
  owned-buffer calls, returns, append, pop, destruction, and ownership checks
- [ ] Owned generic values
- Non-scalar generic collections
- Generic dictionaries
- [x] Scalar mutable generic slices through `out []T` writable views, including
  indexed stores, calls, borrowed returns, provenance, and borrow-contract checks
- [ ] Non-scalar mutable generic slices
- [x] Lifetime-aware generic returns for scalar `readonly []T` and `out []T`
  specializations, with one source view parameter, call-boundary provenance,
  and frame or region escape rejection
- [x] Nested scalar sums to arbitrary depth
- [x] Payload-carrying enums through the backend IR: variant payload type
  lists, tag-plus-payload aggregate layout, construction, payload
  parameters/returns, match with binding arms and rejection coverage, and
  owning payloads (strings, owned slices, owning structs, nested sums)
  with extraction, transfer, destruction, and flow-analysis protection.
  Production codegen still rejects the syntax.
- Tuples
- Destructuring
- Methods
- [x] Non-capturing function values: `fn(...)->...` is a real checked
  signature type (scalar params + scalar/void return only), covering
  assignment, parameter passing, return, and indirect calls
  (`f(a, b, ...)`). The direct backend's `call_i64_i64`/`call_f32_f32`
  builtins remain as a deprecated one-argument alias.
- [x] Closures. `def(params) -> ret: { body }` in expression position is a
  real anonymous function literal that can capture scalar (i64/f32/bool)
  variables from the immediately-enclosing named function by value - a
  snapshot taken when the closure literal is evaluated, not a live
  reference (examples/119_closures_capture.cb,
  examples/120_closures_map.cb). Capturable sources: the enclosing
  function's own parameters, and its explicitly-typed top-level `let`
  bindings (`let n: i64 = ...`; type-inferred `let n = ...` locals are not
  yet supported, since a closure can be IR-compiled before the function
  that lexically contains it - see below). Capturing a non-scalar
  (struct/slice/string) variable is rejected with a clear diagnostic
  (tests/negative/125_closure_capture_not_supported.cb); assigning to a
  captured name inside the closure body is rejected too, since captures are
  read-only (tests/negative/126_closure_capture_assignment.cb).

  Every `fn(...)->...` value - closure or plain top-level function
  reference - is a pointer to a heap- or statically-allocated
  `{code_ptr, env_ptr}` thunk (16 bytes), not a bare code pointer: a plain
  function gets a lazily-emitted static thunk pointing at a small adapter
  stub that drops the unused `env_ptr` and shifts real integer arguments
  left by one register before jumping to the real function
  (`ensure_fn_thunk`/`flush_pending_fn_thunks`, src/codegen.c - the adapter
  bytes are queued and only flushed right after the *current* function's
  own epilogue, never inline mid-function, since the adapter contains a
  real `jmp` that must not be reachable by fallthrough); a closure's thunk
  points directly at the synthesized closure function, which already takes
  an implicit leading `__env` parameter. `emit_call`'s indirect-call path
  always dereferences the thunk and passes `env_ptr` as an implicit leading
  argument, so both cases share one calling convention. Captured values are
  copied into a `malloc@PLT`-allocated environment struct at the closure
  literal's evaluation point (same allocator `emit_string_concat` already
  uses); captured environments leak like any other owned allocation in this
  backend's no-GC/no-automatic-drop model - there is no scope-exit drop for
  anything else either. Capture analysis (`collect_closure_captures`,
  src/ir.c) seeds each capture as an ordinary read-only local in the
  closure's own `IRContext` before `validate_statement` runs, so the rest
  of type-checking (binary ops, calls, returns, ...) needs no new cases at
  all; the reads are rewritten to `AST_ENV_FIELD_LOAD` only *after*
  validate_statement finishes (`rewrite_closure_captures`), so `infer_expr`
  never has to learn the new node kind. `IRContext.parent_scope` (added for
  this feature) ended up unused - a closure's own locals table is
  sufficient since captures are seeded directly rather than resolved via
  live cross-context chaining, which was found to be structurally
  incompatible with the existing one-`IRContext`-per-top-level-function
  compile loop (a closure is IR-compiled as its own root-child iteration,
  not nested inside its enclosing function's compile pass - which is also
  why only *explicitly-typed* enclosing `let` locals are captureable: their
  type is known at parse time, independent of compile order, whereas a
  type-inferred local's type is only known once its own function's
  validate_statement pass runs, which may not have happened yet).
- Recursion
- Casts and explicit conversions
- Constant evaluation
- Compile-time execution
- Richer pattern matching
- Package visibility
- Stable module rules

Every feature needs a defined type rule, ownership rule, lifetime rule, ABI rule, interpreter rule, backend rule, diagnostic rule, and test matrix.

## Phase 15: Tooling and release quality

Build:

- Structured diagnostics
- Parser recovery
- Formatter
- Syntax highlighting
- Language server
- Completion
- Hover information
- Go-to-definition
- Find references
- Rename support
- JSON diagnostics
- Debugger integration
- Reproducible documentation
- Package metadata
- Version compatibility rules

Add CI for clean-checkout builds, strict warnings, sanitizers, fuzzing, positive programs, negative diagnostics, backend differential tests, cross-target assembly, and reproducible output.

## Immediate order

1. Finish sanitizer CI for the scalar and memory lanes.
2. [done] Replace the integer-only HIR, SSA, evaluator, and call value paths
   with a typed scalar representation.
3. [done] Add `f32` end to end, including constants, arithmetic, comparisons,
   calls, returns, printing, evaluation, and typed evaluator tests.
4. [done] Define exact `f32` dump semantics and add edge-case fixtures.
5. [done] Replace slot memory with typed pointer and stack-slot semantics.
6. [done] Lower scalar locals into addressable canonical stack slots,
   descriptor-driven loads, stores, and parameter copies.
7. [done] Define aggregate memory for value-owned scalar-field structs.
8. [done] Lower value-owned scalar-field structs using canonical layout and
   field offsets.
9. [done] Add the scalar-struct call ABI with private parameter copies and
   caller-provided return storage.
10. [done] Add pointer contracts for ownership, borrowing, and caller storage.
11. [done] Add region origins, explicit transfer, destruction, use-after-free,
    and double-free tracking for the isolated backend IR.
12. [done] Add isolated borrowed readonly views with region-aware lifetime
    checks, pointer extraction, length access, and readonly call parameters.
13. [done] Add path-sensitive ownership dataflow for branches, joins, and
    loops, rejecting ambiguous lifetime states.
14. [done] Lower source-level readonly slice parameters, non-escaping aliases,
    checked `slice_u8` subviews, and indexed reads through the canonical view
    operations.
15. [done] Add writable views with borrow conflict analysis.
16. [done] Add caller-derived returned borrowed views with escape analysis.
17. [done] Add owned slices with explicit allocation (`slice_alloc`/`slice_free`)
    and destruction semantics, use-after-free and double-free rejection, and
    borrow-conflict checks against active views.
18. [done] Lower source-level owned slices: `alloc_i64`/`alloc_f32`/`alloc_u8`
    creation, `free` destruction, owned slice locals, indexed reads and
    writes, `len`, `slice_u8` subviews of owned `u8` buffers, owned-slice
    borrows at call boundaries and view aliases, and owned parameter/return
    transfer. Verifier and evaluator checks cover UAF, double-free,
    post-move use, returned allocation handoff, and borrow conflicts.
19. [done] Lower source-level regions: `with region NAME(capacity)` enter/exit,
    region-qualified allocations (`NAME.alloc_i64/f32/u8`), and region-exit
    cleanup. Returns inside region bodies remain restricted in this milestone.
20. [partial] Add strings, sums, and ownership-bearing struct fields. The narrow
    non-owning scalar-sum lane is done: `Option[T]`/`Result[T, E]` with scalar
    components, `some`/`none`/`ok`/`err` construction, `is_some`/`is_ok`,
    checked `unwrap`/`unwrap_ok`/`unwrap_err`, and sum locals, parameters, and
    returns through aggregate copies. Borrowed strings are done: literals,
    `len`, byte indexing, `slice_u8` subviews, readonly parameters, borrowed
    returns with escape rejection. Unit enums and `match` are done: enum
    declarations, variant constants, enum locals/parameters/returns,
    discriminant comparisons, and match with exhaustiveness and duplicate-arm
    checks. Match on `Option`/`Result` with payload binding arms (`case
    some(x)`/`case none`/`case ok(v)`/`case err(e)`) is done: the target
    lowers to the same if/else-if chain as enum match, payload arms bind
    through the checked sum-access path (scalar loads, owning-struct and
    nested-sum moves with source-tag clearing), and missing patterns,
    duplicates, wrong-kind patterns, and binding-rule violations are
    rejected. Nested scalar sums are done to arbitrary depth with aggregate
    component layout, construction, access, params, and returns. Fresh owned strings from concat and string `+`, indexed reads,
`string_free`, and fresh returns are done. Direct owned string and owned
slice payloads in Option/Result now have move, extraction, call, return,
and drop coverage. Nested owning sums now have recursive move and drop
coverage. Ownership-bearing struct fields now have recursive move and drop
coverage for owned strings, owned slices, and owning sums. Value-only struct
sum components (`Option[Point]`) lower end to end through aggregate copies.
Owning struct sum payloads (`Option[Owning]`) now lower end to end through
`agg_move`/`agg_drop` with recursive field payload moves, source-tag clearing
on extraction, and call/return transfer. Nested owning structs now lower end
to end: owning structs inside owning structs with recursive payload
lookup/drop, bare owning struct parameters carrying allocation identities
through call boundaries, and non-consuming `is_some`/`is_ok` predicate reads
on member sums, with use-after-free, double-free, post-call use, and
post-move payload reads rejected by flow analysis and validated natively in
both emitters. Payload-carrying enums are done: `enum Shape: { Circle(f32),
Rect(i64, i64), Line }` imports as a tag-plus-payload aggregate with resident
payload slots, `Shape.Circle(2.5)` construction, payload parameters and
returns, and `case Shape.Circle(r)` match bindings through the checked
sum-access lane, with non-exhaustive, duplicate, unknown-variant,
binding-count, arity, and out-of-subset rejections. Owning enum payloads are
done as well: declared single payloads (`A(string)`, `A([]i64)`, `A(P)`,
`A(Option[string])`) import through the sum-component lane and bind as one
value in match arms, multi-field payloads synthesize inline structs, the
evaluator resolves enum tags to variant selectors through the module's
variant registry (covering arbitrary variant counts), and extracted owning
payloads are protected by flow analysis (double-free, use-after-free);
arbitrary non-scalar ownership-bearing aggregate emission remains deferred.
20b. [done] Add the remaining scalar types: `i32`/`u32`/`u64` (32/64-bit
    width-wrapping integer kinds with signed/unsigned division and
    comparison semantics) and `f64` (IEEE-754 double precision), with
    full-precision integer and float literals (`literal_i64`/`literal_u64`/
    `literal_f64` in the AST, unsigned magnitudes above INT64_MAX, and exact
    double literals that never round-trip through f32). Integer literals
    narrow with range checks and float literals complete to f32/f64 at every
    boundary: declarations, returns, call arguments, binops, member and
    view stores, and sum payloads. `u8` arithmetic now uses byte-width wraparound semantics.
21. [done] Lower ownership-bearing struct fields through canonical field
    addresses. Owned string, owned slice, and owning sum fields use explicit
    field payload store/load, aggregate move, aggregate drop, parameter move,
    caller-storage return, replacement, inactive-variant, and use-after-move
    checks. The legacy production validator continues to reject these forms,
    and    native emission now covers supported owning sums and ownership-bearing
    struct fields; arbitrary non-scalar aggregate emission remains deferred.

21b. [done] Add bounded fixed value arrays `array[T, N]` for scalar elements,
     with canonical inline layout, literal construction, indexed loads and
     stores, `len`, aggregate calls and returns, compile-time bounds metadata,
     runtime bounds checks, and malformed-index verifier coverage.
21c. [done] Add scalar dynamic `list[T]` buffers with explicit length and
     capacity, literal construction, growth, append, pop, indexed access,
     ownership-moving calls and returns, explicit destruction, bounds and
     lifetime failures, and malformed buffer-IR verifier coverage. Lists with
     non-scalar elements remain deferred.
21f. [done] Add string-keyed scalar dicts `dict[string]T` with literal
     construction, index reads and writes, `get`/`has`/`set`/`delete`/`pop`/
     `len`/`free`, ownership-moving parameters and calls, rehash growth, and
     use-after-free and double-free rejection. Native emission calls the
     production `cobra_dict_*` runtime in both emitters. Dicts with owning or
     non-scalar values remain deferred.
21d. [done] Add scalar generic function monomorphization before HIR: infer one
     scalar argument at each call site, substitute canonical parameter and
     return descriptors, clone concrete functions, reuse equal specializations,      and reject non-scalar collection arguments or unresolved generic
      values.
21e. [done] Add immutable scalar-field generic structs before backend import:
      specialize one scalar parameter through canonical layouts, import concrete
      descriptors, lower fields, aggregate calls, caller-storage returns, reuse
      specializations, and reject ownership-bearing fields.
21f. [done] Add readonly borrowed-view fields to scalar generic structs:
      preserve canonical view layout, field assignment and extraction, aggregate
      parameter copies, aggregate returns, lifetime provenance, and evaluator
      handle storage without making generic views owning.
21g. [done] Add scalar generic `list[T]` functions through canonical
      substitution: specialize scalar list parameters and returns, preserve
      owned-buffer pointer-plus-length-capacity values through HIR, SSA,
      verifier, and evaluator, and cover append, pop, calls, returns,
      destruction, leaked-owner rejection, and malformed buffer metadata.
21h. [done] Add scalar generic writable slices through canonical substitution:
      specialize `out []T` parameters and returns, preserve writable-view
      contracts through HIR, SSA, verifier, and evaluator, and cover indexed
      stores, calls, borrowed returns, provenance, and readonly-to-writable
      rejection.
21i. [done] Make scalar generic view returns lifetime-aware: preserve the one
      source-view-parameter contract through specialization, retag owned actuals
      only at borrowed call boundaries, preserve allocation and region provenance,
      reject local and region escapes, reject recursive unresolved specialization,
      and verify tampered return metadata.
21j. [done] Add fixed scalar arrays as struct fields: member reads, member
      assignment with array-literal completion, whole-field and whole-struct
      value copies, aggregate calls and returns, and bounds-checked index
      addressing through a dedicated `array_index_addr` MIR opcode with
      strict verifier metadata, emitted and executed by both native emitters.
21k. [done] Add nested fixed arrays `array[array[T, N], M]` for scalar `T`:
      recursive canonical import, parser support for nested element types,
      nested array literals, whole-row index reads and writes through
      aggregate copies, struct fields, aggregate calls and returns, inline
      byte-wise aggregate emission in both native emitters, and parser
      rejection of non-scalar inner elements.
21l. [done] Add fixed arrays of value-only structs `array[Point, N]`:
      parser support for named struct elements and zero-initialized array
      declarations, canonical import gated to structs without owned payloads
      or borrowed views, whole-struct element writes, aggregate-returning
      calls stored directly into element and field slots, array parameters,
      struct fields, calls and returns, inline aggregate copies in both
      native emitters, and rejection of owning-struct elements at the
      isolated boundary.
21m. [done] Add non-scalar buffers `list[Point]` with value-only struct
      elements: buffer types and imports accept struct elements, aggregate
      element stores and reads through the buffer machinery, index
      assignment, ownership-moving buffer parameters and calls, and the
      3-part buffer ABI (pointer, length, capacity) in both native emitters,
      with the capacity part discarded on receive and passed as length on
      call; pop of non-scalar elements and buffers of owning structs stay
      rejected at the isolated boundary.
22. [done] Add the remaining scalar types (i32, u32, u64, f64) with their ABI
    and memory rules. `u8` byte-width arithmetic, comparisons, and wraparound
    semantics now lower end to end, including native emission.
23. [done] Define the ABI-neutral call model with canonical lowered parameter
    locations, return locations, direct and indirect passing, abstract register
    classes, aligned stack arguments, hidden aggregate-return storage, and
    verifier/evaluator checks.
24. [done] Lower verified SSA into target-independent MIR with virtual
    registers, machine types, CFG edges, ABI moves, calls, returns, memory
    operations, clobber sets, source locations, and a dedicated verifier.
25. [partial] Implement x86-64 assembly emission from MIR for the scalar,
    value-owned scalar-field struct, borrowed view, writable view, heap-backed
    owned-slice, region-cleanup, fresh string-concat, scalar dynamic `list[T]`
    buffer, owning Option/Result payload, and ownership-bearing struct subsets,
    including spill-all and allocated stack frames, calls, indirect parameters,
    caller-provided returns, view calls and returns, native bounds checks,
    branches, memory, aggregate copies, f32, and f64. Assemble and execute
    native fixtures against evaluator and legacy results. Return cleanup now
    preserves scalar payloads and owned-view descriptors across free calls.
    Narrow owned-slice argument moves, descriptor transfers, and explicit     destruction are implemented. Native owning sum and ownership-bearing
     struct aggregate moves and drops are implemented for the supported layouts;
     arbitrary non-scalar aggregate lowering remains deferred.
26. [partial] Add linear-scan register allocation with abstract GPR/XMM
    classes, spill slots, interval overlap checks, call-clobber handling,
    fixed-instruction clobber handling, internal malloc/memcpy/free clobber
    coverage for buffer, string, drop, destroy, and region-cleanup opcodes,
    loop-backedge liveness, and scalar x86-64 emitter consumption. The
    abstract GPR class now includes five callee-saved positions (rbx, r12-r15
    on Linux x86-64) alongside the six caller-saved argument positions;
    intervals crossing a call may land in a callee-saved register instead of
    always spilling, and address values are no longer forced to spill purely
    to survive a call. The allocated emitter saves used callee-saved
    registers to dedicated frame slots in the prologue and restores them
    before every return site. View values still require paired spill
    storage. Target-specific fixed constraints and broader native value
    coverage remain deferred.
27. [partial] Add binary object emission. `bir_x86_64_emit_object`
    (src/backend_ir/x86_64_obj.c) encodes verified, allocated MIR directly to
    x86-64 machine code and writes a real ELF64 relocatable object
    (src/backend_ir/elf64.c) with a global function symbol table, without
    invoking the system assembler. Coverage: 64-bit integer, boolean,
    address, f32, and f64 scalars (arithmetic, NaN-safe ordered/unordered
    comparisons, negation, division), register-passed calls (six-GPR/eight-
    XMM budget), stack-resident locals, branches, returns, flat scalar-field
    structs (field addressing and whole-struct copies, gated by
    `x86obj_supported_struct` to structs with no nested aggregates, arrays,
    sums, or owned payloads), and readonly borrowed views: construction with
    optional source-view provenance checks, pointer/length extraction,
    indexed pointer arithmetic and load/store with native bounds traps, and
    heap-backed slice allocation and release via real `R_X86_64_PLT32`
    relocations against external `malloc`/`free` symbols (a new
    `.rela.text` section and undefined-symbol table entries in the ELF
    writer - the first and so far only externally-resolved calls this
    emitter makes; all internal calls and branches still resolve to
    concrete offsets during emission, needing no relocation). The system
    linker (`ld`, invoked via `gcc`) still performs the final link - writing
    an object file is a distinct milestone from writing a linker, and
    reusing the system linker there matches how production compilers such
    as rustc operate. Writable (`out []T`) views need no separate support -
    mutability is a type-system concern enforced upstream of codegen, so the
    readonly-view opcode coverage already handles them; verified with an
    `out []T` fill-then-read round trip matching the direct backend. Moving
    ownership of an owned slice across a call boundary is implemented: a
    call argument whose callee parameter type is an owned slice zeroes the
    caller's view after loading it into the call's argument registers,
    mirroring the text emitter's `x86_alloc_call_arg_moves_ownership` gate;
    verified with a parameter that frees its own received slice.
    Struct-by-value call/return needed no new code: indirect passing is a
    single ADDRESS-typed hidden pointer under the ABI record
    (`bir_abi_fill_return`'s `indirect` path collapses to one GPR location),
    already flowing through the existing scalar/ADDRESS handling; removing
    an overcautious blanket rejection in `x86obj_emit_function` was enough.
    Sums/enums (`MIR_OP_SUM_CHECK`, tag reads through the existing
    FIELD_ADDR/LOAD path) and nested value structs (`x86obj_supported_struct`
    recursing into struct-typed fields) are implemented for flat,
    non-owning shapes. Owning-field structs and sum/struct fields holding an
    owned view (string, `list[T]`) are implemented too:
    `MIR_OP_FIELD_PAYLOAD_STORE/LOAD` (shared with `MIR_OP_SUM_PAYLOAD_*`)
    transfer ownership into and out of a field, `MIR_OP_AGG_MOVE` copies then
    zeroes the source, and `MIR_OP_AGG_DROP` recursively frees owned bytes
    reachable inside a value (struct fields only under the current gates -
    the sum-payload branch of that recursion is unreachable until
    `x86obj_supported_sum` is ever widened to admit owned payloads, since it
    does not admit them today). `MIR_OP_STRING_CONCAT` (two `memcpy` calls
    plus a `malloc`) and dynamic buffers (`MIR_OP_BUFFER_ALLOC/APPEND/POP/
    FREE`, scalar elements only, always-realloc append - not a performance
    implementation) are implemented on the same external-call machinery as
    heap slices. `dict[string]T` is implemented end to end
    (`MIR_OP_DICT_ALLOC/SET/GET/HAS/DELETE/POP/LEN/FREE`) via real
    `R_X86_64_PLT32` calls into the production `cobra_dict_*` runtime and a
    new `.rodata` section holding key string literals, referenced by
    `lea reg, [rip+disp32]` through a new `R_X86_64_PC32` relocation kind
    against a local section symbol - the first non-function, non-external
    relocation target this emitter needed. Fixed arrays as struct fields
    (`MIR_OP_ARRAY_INDEX_ADDR`, bounds-checked) are implemented, sharing the
    same widened field-type predicate structs and sums already used
    (`x86obj_supported_field_type`, also covering array-of-struct and
    array-of-array). Owned sum payloads (Option/Result holding a string or
    `list[T]`) are implemented: `x86obj_supported_sum` admits an owned-view
    component only for the two-component Option/Result shape specifically
    (matching the text emitter's own scope - an arbitrary N-variant user
    enum with an owned payload stays rejected, since the drop recursion only
    knows how to walk two components), and `x86obj_emit_drop_owned_value`'s
    sum branch is now a real implementation rather than an unreachable stub.
    Non-scalar (struct-element) `list[T]` buffers are implemented for
    `append` and indexed reads (through the existing view-bounds-checked
    `PTR_ADD` plus `AGG_COPY`, unchanged); struct-element `pop` stays
    rejected, matching the text emitter, which never implemented it either.
    All of the above is cross-checked against the text emitter and the
    differential test suite; see `test_source_object_emitter_coverage`
    through `test_source_object_emitter_coverage3` in tests/backend_ir.c,
    which run on every `make backend-native-tests`. With these closed, the
    v1 object-emitter lane covers everything the text emitter's own
    "supported layouts" cover except owning-struct call/return ABI (an
    owning struct or owning-payload sum passed or returned by value, as
    opposed to via a `let` local) and dynamic dict/buffer *moves* through a
    call boundary (only slices currently zero the caller's view on an
    ownership-transferring call).
28. [partial] Production wiring. The isolated backend (bir_backend_compile_program
    and bir_backend_compile_program_object, src/backend_ir/driver.c) is
    linked into the production `cobra` binary and selectable with
    `cobra build|run <file> --backend=native` (text emission through the
    system assembler) or `--backend=native-object` (direct ELF object
    emission). The default remains the production direct emitter
    (`--backend=direct` or no flag), fully unaffected. Because the isolated
    backend does not yet parse the standard library, `--backend=native*`
    reparses only the user's own module (imports included, library prelude
    excluded); programs that reach outside the supported subset - including
    any standard-library call - are rejected with a diagnostic at compile
    time rather than silently mishandled. `--backend=native*` also skips the
    legacy direct-backend validation pass (`cobra_ir_build`) that `build`/
    `run` otherwise apply before codegen: the isolated pipeline performs its
    own complete, independent verification, and the two validators disagree
    in places (for example, freeing an owned slice received as a function
    parameter is valid to the isolated backend but rejected by the legacy
    validator), so gating on the legacy pass would reject isolated-backend
    programs the isolated backend can correctly compile and verify itself.
    Expanding language coverage (the
    standard library above all) is the main blocker to this being a
    generally usable backend rather than a subset one.

The main rule is:

> Do not optimize or add targets until the IR can represent the values, memory, calls, and ownership rules that the language actually supports.
