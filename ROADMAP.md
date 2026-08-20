# Cobra Roadmap

This roadmap covers the work required to turn Cobra's current compiler into a complete systems programming language with a native backend.

## Tooling

Diagnostics now echo the offending source line with a caret under the
column, and undefined-variable errors suggest the closest-spelled name in
scope (locals, closure-enclosing scopes, and top-level functions) when one is
within edit-distance 1-2. `undefined function` codegen errors now carry
file:line:col like every IR-level diagnostic instead of a bare message.
`cobra check` now also rejects calls to undefined functions itself (the IR
pass's call-resolution fallback used to assume any unqualified name with no
matching declaration was an external import and defer the check to codegen,
so `check` silently accepted typo'd call names that only `build`/`test`
caught); a typo'd call name is now rejected at `check` time with the same
diagnostic codegen used to produce. No formatter, LSP, package manager, or
debugger exist yet; a minimal
diagnostics-only LSP (wrapping `cobra check`'s output as LSP diagnostics
over stdio) is likely the next-highest-value tooling piece since the compiler
already does the hard part, followed by a mechanical `cobra fmt`. A package
manager is premature before there's a multi-project ecosystem to serve.

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
- Python-style indentation blocks, `elif`, `and`/`or`, `not`, `True`/`False`/
  `None` aliases, chained comparisons (`0 <= x < 10`), `//` and `**`,
  compound assignment (`+=`, `//=`, `**=`), and parallel tuple assignment
  (`a, b = b, a`) alongside the existing brace syntax
- String methods (`strip`/`upper`/`lower`/`replace`), slicing `s[a:b]`,
  negative indexing, substring membership, and `f"..."` format strings
  with `str()` conversions
- 147 example programs (123 with `test_` suites) and 118 negative diagnostics

The backend IR is linked into the production `cobra` binary and selectable
with `cobra build|run <file> --backend=native` (see Phase 15 item 28 below
for the current state of that integration); the direct emitter
(`--backend=direct` or no flag) remains the default. Historically (through
most of Phases 1-13 below) it was fully isolated from production codegen;
that section is preserved as-written for the implementation history it
records, not as a description of the current wiring. Independent of the
production-wiring question, the backend IR supports
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
`list[T]` for T an all-scalar-field struct (e.g. `list[Point]`) now works for
append, index-read, index-write, and `for x in list` iteration, via the same
aggregate-copy codegen path used elsewhere for value-owned structs. The loop
variable takes the element's own canonical struct type, so normal member
access works inside the loop body. Ownership-bearing generic values (e.g.
`list[string]`) remain deferred.
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
- Non-scalar generic collections (list[T] for all-scalar-field struct T works for append/index/iteration; list[T] for a struct T with owned fields - owned strings, owned slices, or nested owning structs - now also works for append, index-read, index-write, iteration, and destruction: each append/index-write gives the element its own heap-owned copy, and destroying the list (scope exit, explicit `free`, or loop-scoped reuse) walks every live element and frees its owned fields before freeing the element and the buffer. No new move-tracking was added - a source local passed to append is simply left un-autofreed by the existing conservative autofree analysis, matching the codebase's existing leak-not-double-free convention for owned struct fields. `pop(list, default)` is now implemented for list[T] too, mirroring dict's pop convention: an empty list yields the caller-supplied default rather than a runtime abort or Option. Scalar and f32 elements pop by value; struct elements (scalar-field or owned-field) are copied out of their heap-owned element block into the caller's destination before that block is freed, transferring ownership of any owned fields without double-freeing them. Only named list locals are supported as the pop target so far - struct-field lists (`s.items`) are not yet wired into pop's codegen path)
- Generic dictionaries (`dict[string]V` now also accepts a named struct as V,
  scalar-field or owned-field, mirroring list[T]'s struct support: literal
  declaration, `d["k"] = v`/`set`, plain `d["k"]`/`get` reads (a borrowed view
  of the stored entry, no ownership change), `pop`, `delete`, and destruction
  (scope exit, explicit `free`, or loop-scoped reuse) all work. Each entry's
  value is a private heap-owned copy of the struct (its already-8-byte int64
  value slot just holds the pointer - no runtime hash-table changes needed);
  overwriting an existing key frees the old value's owned fields first, `pop`
  transfers ownership of the popped value's owned fields to the caller, and
  destruction walks every live entry via two new runtime accessors
  (`cobra_dict_capacity`/`cobra_dict_raw_entries`) freeing each value before
  freeing the dict itself. Key type is still string-only - widening it is a
  separate, larger undertaking left out of scope here. Iteration over a dict's
  entries now works too: `for k in dict:` (key-only, `k` a borrowed string
  view of the entry's own heap key) and `for k, v in dict:` (key+value; `v`
  is the scalar value or, for a struct V, a borrowed view of the entry's
  heap-owned struct copy - not a fresh copy, mirroring how list[Struct]
  iteration binds its loop variable). Codegen walks the raw hash-table entry
  array directly via `cobra_dict_capacity`/`cobra_dict_raw_entries`, skipping
  empty/tombstone slots, rather than routing through the index-into-buffer
  loop machinery `for x in list:` uses - a dict's entries aren't contiguous
  or index-addressable the way a list's buffer is. The dict can still be
  freed normally after iterating)
- [x] (isolated backend / `--backend=native` only, see `tests/backend_ir.c`'s
  `test_source_generic_writable_slices`) Scalar mutable generic slices through
  `out []T` writable views, including indexed stores, calls, borrowed returns,
  provenance, and borrow-contract checks.
- [x] Scalar `out []T` generic slice parameters in the direct/production
  backend (`src/ir.c`, `examples/171_generic_writable_slice.cb`). The old
  blanket rejection ("generic collection parameters are reserved for the
  backend-v2 path") turned out to be broader than necessary: the ordinary
  scalar generic-function specialization machinery
  (`specialize_generic_function` / `cobra_type_bind_generic` /
  `cobra_type_substitute`) already binds and substitutes generic slice types
  correctly, including under `out` mutability - the rejection was only ever
  needed for dynamic `COBRA_TYPE_LIST` generics, which is what it's now
  scoped to. Indexed load/store through a bound scalar `out []T` view
  monomorphizes per concrete T exactly like a non-generic `out []i64`/`out
  []f32` parameter, with the same borrow-contract and provenance checks. Also
  confirmed (not assumed) that scalar `readonly []T` element access already
  worked end to end; the "operator '==' cannot combine T and i64" failure
  from an earlier probe reproduces on plain scalar `T` params with no slice
  involved (`v == 1` inside a generic function returning `bool`) and is an
  unrelated, pre-existing generic-comparison-return-type gap, left as-is.
- [ ] Non-scalar mutable generic slices (direct backend). Blocked on
  struct-typed T support, which stays out of scope here.
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
- [x] Tuples and flat destructuring, scoped to scalar elements. A tuple type
  `(T1, T2, ...)` (2-8 scalar elements: i64/i32/u8/u32/u64/f32/f64/bool) is
  sugar over the existing struct machinery - the parser synthesizes an
  ordinary `AST_STRUCT_DECL` with positional fields `_0.._N-1`, deduplicated
  by a deterministic name (`__tuple_i64_i64`, ...), so layout, sret-style
  struct return, and field access all reuse struct codegen/IR unchanged; no
  new `CobraType`/`ASTNodeType` case was added anywhere that owns a switch
  the isolated backend also switches on, except one new `AST_TUPLE` node
  (direct-backend-only, matching how `AST_TRAIT_DECL`/`AST_IMPL_DECL` were
  added). A tuple literal `(a, b, c)` is a first-class expression only in
  two positions: the value of `return (...)` from a tuple-typed function
  (`emit_tuple_return` writes each element straight into the sret buffer at
  its field offset, since the literal has no addressable storage of its
  own), and the RHS of `let (a, b, c) = ...` destructuring. Destructuring
  supports exactly two RHS shapes: a direct tuple literal (desugars at parse
  time into N independent `let`s, no tuple type ever created) and a call to
  a function already declared earlier in the source with a tuple return
  type (desugars into a hidden struct-typed temp holding the call result
  plus N ordinary field-access `let`s). Both desugar into a spliced sibling
  statement list rather than a nested block, since IR validation scopes a
  nested `AST_PROGRAM` like an if/while body and would drop the bound names
  once the block ends. Deliberately out of scope: non-scalar tuple elements
  (string/struct - same ownership questions as the parallel list[T] owned-
  field work), tuple-typed parameters, a tuple value escaping through a
  plain variable or nested pattern, and any destructuring beyond flat
  tuple-to-names (no wildcard `_`, no nested patterns, no struct-field
  destructuring). See `examples/164_tuples_destructuring.cb` and
  `tests/negative/100_tuple_destructure_arity_mismatch.cb` /
  `tests/negative/101_tuple_destructure_non_tuple.cb`.
- [x] Plain struct methods, no trait required. `impl Type: { def method(params)
  -> ret: { body } ... }` (same body as `parse_impl_declaration` already
  parsed for `impl Trait for Type`, just without the `Trait for` prefix)
  attaches methods directly to a struct. The parser tells the two forms
  apart by whether `for` follows the first identifier; a plain impl stores
  an empty trait name as the sentinel meaning "no trait" - real trait names
  come from a `TOKEN_IDENTIFIER` and can never be empty, so the mangled name
  (`__impl__<Type>_<method>`, empty trait segment) can't collide with any
  real trait-qualified `__impl_<Trait>_<Type>_<method>`. `find_impl_method`
  (src/ir.c) needed no change at all: it already matches on the impl's
  `secondary_name` (implementing type) and method name only, ignoring the
  trait name entirely, so `x.method(args)` resolution, call-site rewriting,
  and codegen are shared unmodified with trait-based dispatch. The one
  change on the IR side is the conformance pass skipping impl blocks whose
  trait name is the empty sentinel, since there is nothing to conform to. A
  struct can have both a plain `impl Type: {...}` and one or more
  `impl Trait for Type: {...}` blocks at once; a duplicate method across
  two plain impls (or a plain impl re-declaring the same name twice) is
  caught for free by the existing duplicate-top-level-function check, since
  every impl method - plain or trait - is registered as an ordinary
  top-level function under its mangled name
  (examples/162_plain_struct_methods.cb,
  tests/negative/146_plain_impl_unknown_method.cb,
  tests/negative/147_plain_impl_duplicate_method.cb).
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
- [x] Traits (static dispatch only). `trait Name: { def method(params) -> ret ... }`
  declares required method signatures (no bodies); `impl Name for Type: { def
  method(params) -> ret: { body } ... }` implements them
  (examples/130_traits_basic.cb). Each impl method is registered as an
  ordinary top-level function with a mangled name
  (`__impl_<Trait>_<Type>_<method>`, `parse_impl_declaration`, src/parser.c),
  exactly like closure literals - no vtable, no runtime dispatch, no new
  value representation. `x.method(args)` reuses the existing
  `alias.function(...)` qualified-call parse path (`qualifier` holds "x");
  when the qualifier isn't a module/region alias, `find_impl_method`
  (src/ir.c) checks whether it names a struct-typed local with a matching
  impl method and, if so, rewrites the call node in place - mangled name,
  receiver prepended as the first argument, qualifier cleared - entirely at
  IR-build time, so codegen needs zero new cases (the rewritten node is an
  ordinary `AST_FUNC_CALL` by the time codegen ever sees it). Trait
  conformance is checked once per impl (every trait method must have a
  matching impl method by name; deeper signature comparison isn't needed
  since each impl method is independently type-checked as a normal function
  anyway) - see `tests/negative/128_impl_missing_method.cb` and
  `tests/negative/129_impl_unknown_trait.cb`.
- [x] Dynamic dispatch (`dyn TraitName`), as a function parameter type only.
  `def describe(shape: dyn Shape) -> i64: { return shape.area() }` accepts
  any struct implementing `Shape`, dispatched at runtime
  (examples/133_dyn_trait_dispatch.cb). Reuses the `fn(...)->...` single-
  pointer ABI (`declared_type == COBRA_TYPE_FUNC`, `dyn_trait_name` set on
  the AST/IR/VarSymbol node instead of a real signature): the value is one
  pointer to a heap block `{data_ptr, method0, method1, ...}` in the trait's
  declared method order, built at the call site
  (`emit_dyn_trait_call`, src/codegen.c) from the argument struct's address
  and the concrete type's existing mangled static-dispatch impls
  (`__impl_<Trait>_<Type>_<method>` - no separate vtable symbol, no new impl
  registration). A method call on a `dyn`-typed receiver
  (`emit_dyn_dispatch_call`) loads the code pointer from the block at
  `8*(1+method_index)` and calls it with the block's `data_ptr` as the
  receiver, genuinely resolved at runtime rather than rewritten to a fixed
  symbol the way static dispatch is. ir.c validates every trait method is
  implemented at the coercion (call) site, reusing `find_impl_method`
  (`tests/negative/134_dyn_trait_missing_method.cb`). The dispatch block is
  heap-allocated and never freed, matching this backend's existing
  no-automatic-drop convention for closure environments.
- [x] `dyn Trait` as a `let` declaration and function return type
  (examples/137_dyn_trait_let_and_return.cb). `let shape: dyn Shape = c`
  builds the dispatch block at the assignment site the same way a
  parameter coercion does. `-> dyn Shape` builds it before returning, but
  with one added twist the parameter case doesn't need: the struct's bytes
  are heap-copied (not just pointed at in place) before the block is built,
  since a returned local's stack frame is gone the instant the caller
  resumes - pointing data_ptr at the frame directly would dangle
  (`emit_build_dyn_dispatch_block_ex`'s `heap_copy_data` flag,
  src/codegen.c). `let x: dyn Trait = some_func(...)` is a plain scalar
  move, since a dyn-returning call already hands back a fully-built block
  pointer. Not yet supported: generic trait bounds (`def f[T: Shape](x: T)`),
  and multiple impls of the same trait for the same type (last registration
  wins silently - not yet diagnosed).
- [x] `list[dyn Trait]` (examples/169_list_dyn_trait.cb). A trait-object
  value is already one pointer to its heap-allocated dispatch block, so it
  slots into the same 8-byte scalar-element machinery a `list[i64]` uses -
  `append` just stores the pointer (no per-element heap copy the way
  `list[Struct]` needs), and index-read/iteration just load it back.
  `list[dyn Shape]` parses via a `dyn TraitName` case added to the list
  element-type grammar (`parser_component_type`, src/parser.c), whose
  trait name is stashed on the *declaring* node's own `dyn_trait_name`
  field (otherwise unused for a list declaration) rather than adding a new
  field - `add_local` (src/ir.c) already copies that onto the list's own
  `IRLocal`, so `shapes`'s `dyn_trait_name` becomes "the element trait"
  for free. `let x: dyn Shape = shapes[0]` and `for s in shapes: { ... }`
  both thread that trait name onto the resulting local/loop-variable so
  `.method()` calls resolve through the existing `emit_dyn_dispatch_call`
  exactly like any other `dyn Shape` value; a loop variable has no
  VarSymbol/stack slot of its own (see the struct-element loop case), so
  its method-call codegen loads the stored pointer into a fresh temp first
  (`emit_call`'s loop-var branch, src/codegen.c) rather than reusing
  `emit_dyn_dispatch_call`'s VarSymbol-based receiver lookup directly.
  `append` additionally checks the value's own `dyn_trait_name` matches the
  list's (`tests/negative/148_list_dyn_trait_mismatch.cb`) so a `dyn Named`
  value can't silently join a `list[dyn Shape]`. Destruction is unchanged
  from a scalar list: freeing the list's backing buffer never touches the
  dispatch-block pointers it held, matching the standalone `dyn Trait`
  no-automatic-drop convention above rather than inventing per-element drop
  semantics just because the value is now inside a list.
- [x] Default trait methods and supertraits. A trait method signature may
  include a default body (`def name(...) -> ret: { body }` inside the
  `trait` block); any impl that doesn't override it gets a synthesized
  `__impl_<Trait>_<Type>_<method>` cloned from the default at parse time
  with the receiver prepended, registered exactly like a hand-written impl
  method so no other pass needs to know a default was involved (a default
  body cannot reference a receiver, since the trait signature never names
  one). A trait may declare a supertrait (`trait Drawable: Shape: { ... }`);
  a type implementing the subtrait must also satisfy every supertrait
  method, checked via `find_impl_method` against any impl block for that
  type, recursively through a bounded-depth chain
  (examples/158_trait_default_methods.cb, examples/159_trait_supertraits.cb).
- [x] Static vtables for dyn Trait dispatch. The dispatch block is now a
  fixed 2 words (`data_ptr`, `vtable_ptr`) instead of `method_count + 1`
  words filled in with per-call mov instructions - the method-pointer
  portion is emitted once per (Trait,ConcreteType) pairing as a `.rodata`
  array (`emit_dyn_vtable_label`, src/codegen.c) and shared by every
  dispatch block built for that pairing, since method addresses never vary
  per instance. The remaining malloc per coercion is a fixed 16 bytes
  regardless of trait size; still heap-allocated and never freed, matching
  the existing no-automatic-drop convention. Eliminating that last malloc
  entirely would require widening the dyn Trait value from one pointer to
  a 16-byte {data_ptr, vtable_ptr} pair passed by value, touching every
  storage/parameter/return site that assumes a single 8-byte slot -
  deferred as a separate, larger ABI change.
- Recursion
- [x] Casts and explicit conversions. `expr as Type` (new `TOKEN_AS`/`AST_CAST_EXPR`)
  converts between the scalar numeric/bool types: i32, i64, u8, u32, u64, f32,
  bool - full pairwise coverage across all seven, including bool <-> numeric.
  f64 is excluded on purpose: it's already reserved language-wide (`let`,
  params, returns all reject it - see the "f64 is reserved until native
  double-precision lowering is implemented" checks in ir.c) and a cast is not
  a backdoor around that. String/struct/collection casts are out of scope -
  `as` only ever accepts a scalar numeric-or-bool source and target
  (ir.c rejects anything else with "cannot cast ... 'as' only converts
  between numeric and bool scalar types"). Parses at the same precedence as
  the other postfix-ish primaries so `a + b as f32` reads as `a + (b as f32)`
  (examples/166_casts.cb). Codegen (`emit_cast_int_width`,
  src/codegen.c) is real bit-pattern conversion, not a type relabel:
  int narrowing truncates (two's-complement wraparound - `1000 as u8 == 232`),
  int widening sign- or zero-extends per the source type's signedness
  (`movsxd` for i32, `movzx`/`mov r32,r32` for the unsigned widths),
  float-to-int truncates toward zero via `cvttss2si` (not round-to-nearest),
  int-to-float goes through the same `cvtsi2ss` path this backend already
  uses for implicit mixed-numeric coercion, and bool conversions are a
  nonzero test in either direction (float bool casts compare against 0.0
  directly rather than routing through the truncating int path, so `0.5 as
  bool` is `true`, not `false`). f32 and f64 share one single-precision xmm
  representation in this backend already, so a hypothetical f32<->f64 cast
  would cost no instruction - moot while f64 stays reserved.
  tests/negative/100_cast_struct_rejected.cb,
  tests/negative/101_cast_string_rejected.cb, and
  tests/negative/102_cast_f64_reserved.cb cover the rejected cases.
- Constant evaluation
- Compile-time execution
- [x] Richer pattern matching (partial). `match` arms now also accept
  literal int/bool patterns alongside the pre-existing enum-variant `case`
  form, on an integer or bool scrutinee: `match n: { 0: {...} 1: {...} _:
  {...} }`. Adds, on top of the existing enum-variant match: a `_` wildcard
  arm (same semantics as `else`); or-patterns (`6, 7: {...}`, up to 8
  literals per arm, new `match_literals`/`match_literal_count` on
  AST_MATCH_CASE); and `if` guards (`5 if cond: {...}`, or `_ if cond:
  {...}` for an unconditional-pattern/guarded catch, via the new
  `match_guard` field) - guards re-evaluate per arm since they can have
  side effects, so a literal-pattern match compiles to a sequential
  compare-and-branch chain rather than the jump table the plain
  enum-variant form still uses. A literal match with no default/`_` arm is
  rejected as non-exhaustive except for `bool`, where `true`/`false` both
  present is recognized as exhaustive; comparing a literal pattern against
  a non-integer/bool scrutinee (e.g. `string`) is rejected too
  (examples/167_match_patterns.cb,
  tests/negative/100_match_non_exhaustive_literal.cb,
  tests/negative/101_match_literal_type_mismatch.cb). Out of scope still:
  struct field/nested destructuring patterns in match arms - deliberately
  the same flat-destructuring boundary chosen for `let (a, b) = ...`
  tuple binding, not attempted here.
- [x] Package visibility - `pub`/`private` on top-level `def` (parser.c
  is_public/has_visibility), enforced by function_visible_from in ir.c:
  a private function is only callable from statements in its own
  source file, whether the call is qualified (`alias.fn()`) or bare
  (`fn()`); verified in both directions
  (examples/92_module_visibility.cb,
  tests/negative/92_private_module_access.cb). Scope is deliberately
  narrow: only function declarations carry a visibility modifier -
  structs, traits, and top-level consts have none and are always
  visible wherever their name is in scope. Not attempted here:
  per-field struct visibility or a re-export mechanism (an importer
  cannot forward a symbol under its own name for a third file to pick
  up); every file that wants a symbol must import the module that
  actually declares it.
- [x] Stable module rules - source imports resolve to a canonical
  on-disk path (main.c load_cobra_module/resolve_module_path), so a
  diamond import of the same file from two paths is deduplicated
  (examples/40_module_diamond.cb) and a real cycle is rejected
  (tests/negative/39_module_cycle_root.cb). Aliases are scoped to the
  importing file, must be unique within it
  (tests/negative/42_duplicate_module_alias.cb), and referencing an
  undeclared alias is rejected
  (tests/negative/41_unknown_module_alias.cb). Two modules independently
  defining the same top-level function name is a hard "duplicate
  function in composed program" error rather than silently picking one
  (tests/negative/40_duplicate_module_root.cb) - there is no
  overload/shadowing resolution to get wrong. Fixed this pass: a
  qualified call `alias.fn()` used to accept *any* function that
  existed anywhere in the merged program as long as `alias` was some
  valid module alias in the caller's file, regardless of which module
  `fn` actually lived in - so given `import a.cb as a` and `import
  b.cb as b`, `a.some_fn_only_defined_in_b()` compiled. ir.c now
  resolves the alias's import path relative to the caller's file and
  requires the callee's source file to match before accepting the call
  (tests/negative/149_module_alias_cross_binding.cb).

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
    rejected. Literal-pattern match (`match n: { 0: {...} 1, 2: {...} n if
    cond: {...} _: {...} }`) is done for integer and bool scrutinees: the
    same sequential compare-and-branch desugaring as enum match, extended
    with or-patterns (multiple literals per arm) and guards (re-evaluated
    once per arm, after any of that arm's literals match, never
    precomputed), matching the direct backend's exhaustiveness rule (an
    else/`_` arm is required unless the scrutinee is bool with both `true`
    and `false` covered by unguarded arms) and duplicate-literal check
    (skipped for guarded arms). Nested scalar sums are done to arbitrary depth with aggregate
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
     non-scalar elements remain deferred. `pop` now takes `(list, default)`
     like the direct backend: `SSA_OP_BUFFER_POP`/`MIR_OP_BUFFER_POP` carry a
     second fallback operand, and the underlying pop is only executed once
     the interpreter/emitters have proven the list non-empty (length check
     first, matching how the runtime already guarded the raw pop with a
     `ud2` trap) - an empty list yields the fallback instead of aborting,
     mirroring dict's pop convention. Scalar list elements only.
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
    (`--backend=direct` or no flag), fully unaffected. `--backend=native*`
    now parses `lib/std.cb`, `lib/mem.cb`, `lib/fs.cb`, `lib/cpu.cb`,
    `lib/time.cb`, and `lib/net.cb` as its prelude, added one module at a
    time with regression verification between each (`lib/nn.cb` and
    `lib/http.cb` are not yet included: nn.cb needs AVX2 tensor-kernel
    builtins - `dense_f32`/`relu_f32` and friends - that have no backend_ir
    lowering at all yet, and http.cb hits a distinct borrowed-view
    type-inference gap; blanket-adding every module at once was tried twice
    early on and regressed everything both times, since a module can call
    its own unregistered internal builtins, so modules are added
    deliberately and individually, not as a batch); a function that fails to lower
    is skipped rather than aborting the whole build, as long as nothing
    reachable from `main` actually calls it and the failure is a genuine
    "construct not supported yet" gap rather than a real semantic error.
    `--backend=native*` also skips the legacy direct-backend validation pass
    (`cobra_ir_build`) that `build`/`run` otherwise apply before codegen: the
    isolated pipeline performs its own complete, independent verification,
    and the two validators disagree in places (for example, freeing an owned
    slice received as a function parameter is valid to the isolated backend
    but rejected by the legacy validator), so gating on the legacy pass
    would reject isolated-backend programs the isolated backend can
    correctly compile and verify itself.

    A plain `[]T` function parameter is never freed by the direct backend
    either (its static auto-free pass only ever runs over provably-non
    -escaping locals, never over received parameters), so it is
    borrowed-mutable in practice regardless of the missing `out` qualifier.
    `bir_import_ast_type` now maps a bare `[]T` *parameter* specifically
    (`node->type == AST_PARAM`) to `bir_writable_view_type` instead of owned
    -slice storage, matching that real direct-backend semantics; declarations
    and other non-parameter positions are unaffected.

    This needed real borrow-lifetime tracking to be sound: previously, once
    a value was borrowed into a view, the verifier considered that view
    "active" for the rest of the function, so any read/write to the
    original buffer after a call that borrowed it into a plain `[]T`
    parameter was rejected as a write-while-borrowed conflict. The fix adds
    a `transient_borrow` flag on `HirExpr`/`SsaInst` (`HIR_EXPR_BORROW` set
    only at the call-argument alias site in hir.c's `AST_FUNC_CALL` case,
    distinct from the pre-existing `let`-bound-view-local and return-value
    borrow sites, which stay live for the rest of the function as before).
    `flow_simulate_block` in verify.c releases a transient borrow's
    readonly/writable count immediately after the `SSA_OP_CALL` that
    consumed it, by walking each call operand's `def_inst` and checking for
    a flagged `SSA_OP_VIEW_MAKE`; `eval_call` in eval.c mirrors the same
    release for the interpreter's independent borrow-count model. Both
    still reject a genuine overlapping borrow (e.g. passing the same
    allocation as both a writable and a readonly argument to one call) and
    a genuine use-after-free (reading/writing/freeing an allocation that was
    actually freed) exactly as before - only the specific transient
    call-argument borrow is released, never a named view local's borrow.

    `expr as Type` casts now lower to a new `SSA_OP_CONVERT`/`MIR_OP_CONVERT`
    instruction (ssa.h/ssa.c, verify.c, mir.c, eval.c, x86_64.c,
    x86_64_alloc.c, x86_64_obj.c) threaded the same way as every other
    scalar opcode, closing what had been the only runtime numeric-conversion
    gap in this backend. Verification gives CONVERT its own rule instead of
    reusing the equal-operand-result-type check every other unary op relies
    on (its whole point is operand type != result type): both sides must be
    one of the scalar numeric/bool kinds, checked independently. Covers every
    (from, to) pair among i32/i64/u8/u32/u64/f32/bool (f64 stays reserved at
    the HIR boundary, matching ir.c's own cast rule) - int narrowing/widening
    with correct sign vs zero extension, int<->float via truncating
    (never rounding) conversions matching codegen.c's emit_cast_int_width
    reference, and bool as a nonzero test in either direction. The
    register-allocated x86 emitter's own address-precompute path
    (x86_64_alloc.c) has a pre-existing, unrelated bug where a local's
    address kept in a caller-saved GPR across a `print`/call site can be
    clobbered by that call - reproduces with plain i64 locals and no casts
    at all - found while adversarially testing CONVERT but not fixed here
    (out of scope for this change; each cast verified individually against
    the direct backend to sidestep it, and every individual case matched
    bit-for-bit).

    Net effect across the whole `--backend=native` effort this session:
    def-main examples building under `--backend=native` went from 0/41
    (every build silently failed the same way, masked by a since-fixed test
    -harness bug that made `cobra test --backend=native` quietly run the
    direct backend instead) to 26/41. Remaining known gaps: a full tensor
    type (`tensor[N,M]f32`) and its AVX2 kernels (blocks 6 files, and is
    genuinely new-feature-sized work, not a lowering gap - backend_ir has no
    tensor type representation anywhere yet); `lib/http.cb`'s
    borrowed-view-inference gap (2 files); a `v256` SIMD parameter gap that
    predates this effort and exists in both backends, not just this one (1
    file); a real runtime FFI/PLT-linking segfault, found and documented but
    not chased (1 file); this backend's flat function-scoped locals (no
    block/lexical scoping, unlike the direct backend) blocking a couple of
    remaining stdlib-heavy examples; and inline `asm` blocks, which are a
    deliberate, permanent non-goal (2 files). Expanding language coverage
    (the tensor system above all) is now the main blocker to this being a
    generally usable backend rather than a subset one.

- [x] backend_ir: `impl` blocks (plain struct methods, trait static dispatch,
    default trait methods, supertraits). Previously every top-level `impl`
    was rejected outright ("top-level declaration is outside the backend-IR
    subset"). The shared parser already does almost all of the work before
    either backend sees it: `parse_impl_declaration` registers every impl
    method directly into the parser root as an ordinary top-level
    `AST_FUNCTION` named `__impl_<Trait>_<Type>_<method>` (empty-trait
    sentinel for plain, traitless impls), and `synthesize_trait_defaults`
    clones default trait method bodies into synthesized impl methods the
    same way, so default methods and supertrait conformance were already
    fully desugared upstream by the time backend_ir's HIR builder runs -
    nothing impl-specific needed to be taught to it for those. What backend_ir
    needed: (1) stop rejecting the bookkeeping `AST_IMPL_DECL`/`AST_TRAIT_DECL`
    top-level nodes (their methods are picked up separately as ordinary
    functions by the existing per-function loop); (2) rewrite `x.method(args)`
    to a direct call on the mangled function at HIR-build time
    (`hir_rewrite_impl_call` in `src/backend_ir/hir.c`), mirroring
    `find_impl_method` in `src/ir.c` exactly (same mangled-name lookup over
    the `AST_IMPL_DECL` marker children, same receiver-prepend rewrite) so
    every downstream case (ABI validation, argument lowering, aggregate
    -return hoisting) sees an ordinary call to a mangled top-level function
    and needs no impl-specific awareness anywhere else. Verified plain
    struct methods, trait static dispatch, default trait methods (used and
    overridden), and supertrait conformance all match the direct backend's
    output bit-for-bit on throwaway programs. Dynamic dispatch (`dyn Trait`
    as a parameter/local/return type, its 2-word dispatch-block
    representation, static vtables, and indirect-call codegen) and
    `list[dyn Trait]` are not yet supported by backend_ir - both still hit
    the same top-level-declaration or type-resolution gaps as before this
    change and are unattempted future work, not a partially-working or
    unsound implementation of either.

The main rule is:

> Do not optimize or add targets until the IR can represent the values, memory, calls, and ownership rules that the language actually supports.

## Static automatic deallocation (phase 1)

The direct backend now auto-frees owned string/slice fields on a struct
local at function-scope exit, with zero runtime cost, whenever a whole-body
scan proves it sound: the local is declared bare (no initializer), never
reassigned as a whole value, never copied into another variable, never
passed by value to a function (already rejected separately by the IR
checker for owned-field structs), never returned, and every owned field is
populated only from a fresh value (a literal or expression), never copied
in from an existing variable. This reuses the exact free@PLT codegen path
an explicit `free()` already emits (see `emit_scope_cleanup` in
src/codegen.c) - it is RAII via static analysis, not a new runtime.

Any local that fails one of these checks keeps today's behavior (leaked,
never freed) rather than risk a double-free; precision was prioritized over
coverage. Deferred for later phases: nested-block scope (today's sweep is
function-scope only), closure environment frees, and collection-element
frees.

## Static automatic deallocation (phase 2: recursive struct-of-struct frees)

Owned string/slice fields nested inside an embedded (by-value) struct field,
at any depth, are now freed too - `emit_struct_owned_field_frees` in
src/codegen.c walks a struct local's canonical layout recursively, computing
each nested field's absolute stack offset as the sum of every enclosing
field's own offset (nested struct fields are stored inline, never by
pointer, so this is address arithmetic, not a second allocation to reason
about). The same phase-1 soundness conditions apply unchanged: this only
frees fields on a candidate local that survives the whole-body disqualify
scan (never reassigned, copied, returned, or field-populated from an
existing variable) - a nested struct field written from an existing
variable already disqualifies the whole outer local today, so no separate
nested-aliasing check was needed.

This also lifted a validator restriction: `direct_struct_field_supported_kind`
in src/ir.c previously rejected any owned string/slice field nested inside
another struct field (`nested=true` returned false unconditionally); it now
accepts owned (not borrowed) nested fields, since codegen can now free them
correctly. Borrowed view fields stay depth-1 only - their safety depends on
region/lifetime checks this pass doesn't thread through nested struct
fields.

Verified with a 3M-iteration build-and-run stress test of a two-level owned
struct (`Outer { tag: string, inner: Inner { label: string, value: i64 } }`,
2 owned strings allocated and freed per iteration): flat 1.8MB RSS, exit 0,
no corruption. A parallel 2M-iteration test of the disqualified "returned
struct" pattern leaked as expected (126MB RSS) but did not crash, confirming
the exclusion holds at scale, not just correctness at small scale.

## Static automatic deallocation (phase 3: loop-body list/dict frees)

A `list[T]`/dict local declared with a fresh literal inside a `while` or
`for` loop body is now freed once per runtime iteration, right before the
loop jumps back to its condition, instead of only at function-scope exit -
see `emit_loop_owned_cleanup` in src/codegen.c. The soundness scope is
narrower on purpose: candidates are collected from the loop body only (not
the whole function), and the same `autofree_scan_disqualify` scan used by
phases 1-2 runs over just that body - any use of the candidate as a call
argument, a return value, or an rvalue copied elsewhere anywhere in the
body disqualifies it and it is left to leak exactly as before. In practice
this means direct-index use (`xs[0]`) benefits, while the common
`get(dict, key, default)`/list-builtin-call access patterns still
disqualify (the value is passed as a call argument) - a known,
already-conservative limitation inherited unchanged from phases 1-2, not a
new gap. Verified with per-iteration stress tests (flat RSS across 2-3M
iterations for the freed case, correct expected growth with no crash for
the explicitly-disqualified/leak-safe case) plus an explicit escaping-call
test confirming no double free when combined with a user's own `free()`.
Still deferred: closure-environment frees, and list/dict locals declared
via a function-call initializer (`let zs = returns_a_list()`) rather than a
literal, which are not yet tracked as owned at all on the caller side.

## Whole-program struct-parameter borrow inference (memory design phase 1)

A struct parameter is passed as one pointer ABI slot; previously every
non-`out` parameter unconditionally copied `struct_storage_size` bytes into
private frame storage at function entry, since the callee is otherwise free
to mutate its own copy without the caller observing it (documented,
tested by-value semantics - see `examples/72_struct_parameters.cb`). That
copy is now skipped when a whole-body scan proves the callee neither writes
to the parameter (whole reassignment, a field write at any nesting depth
via `AST_MEMBER_ASSIGN`, or copying it elsewhere as an rvalue) nor lets it
escape (return it, or pass it to a further call that doesn't itself prove
the same safety) - see `compute_param_borrowed`/`param_use_escapes` in
src/codegen.c. When proven safe, the parameter aliases the caller's pointer
directly, reusing the exact same `VarSymbol.indirect` mechanism `out`
parameters already use - never a new addressing path.

This is a real, memoized, cycle-safe interprocedural analysis, not a
single-function heuristic: a struct argument forwarded unchanged to another
function recursively consults that callee's own summary (computed lazily,
cached per `(function, parameter)` pair) and stays classified safe if the
callee also proves it safe - verified with an 80-function synthetic
pass-through chain compiling and running correctly, and with `emit-asm`
showing the per-call-site 16-byte copy genuinely gone from every link in
the chain. Self- and mutual recursion are handled by marking an in-progress
summary conservative (escaping) rather than an unbounded fixpoint, so the
analysis provably terminates. A call through a `dyn Trait` value, an
indirect `fn(...)->...` value, or any callee this pass can't resolve (FFI,
`import c`, builtins) is always treated as escaping, since none of those
have a summary to consult - matches this session's established boundary
for exact analysis.

Benchmarked both compile-time and runtime cost explicitly, per the
"must not become latent" requirement this phase was built under: compiling
the full `examples/*.cb` corpus is unchanged (31.1s vs. 31.4s baseline,
within noise); a 100M-call loop passing an 8-field struct runs in ~1.02s
against a ~1.22s baseline for the same binary before this change (~16-18%
faster, reproduced across repeated runs) - this phase makes call-heavy
struct-passing code faster, not slower, exactly because the eliminated copy
is real instruction and memory-traffic overhead, not just a bookkeeping
cost.

While building and testing this, found and fixed a real, pre-existing
double-free bug in the phase 1-3 auto-free work above: `autofree_scan_disqualify`
only recognized a struct field write when it appeared as an `AST_ASSIGN`
node with `secondary_name` set, but `candidate.field = value` (including
nested `candidate.a.b = value`) actually parses to a distinct
`AST_MEMBER_ASSIGN` node with the base identifier name preserved through
the whole access chain - meaning "field populated from an existing
variable" (the exact case the auto-free soundness comment already claimed
to reject) was silently never being disqualified for this node shape. A
struct local with a field assigned from an existing owned variable (e.g.
`let h: Holder; h.tag = some_string;`) could be auto-freed while the
assigned-from variable still held a live pointer to the same allocation,
producing a real double-free. Fixed with the same check `AST_MEMBER_ASSIGN`
handling this phase's own scan needed anyway; verified with a targeted
repro (`free(): invalid pointer` before the fix, correct output after) and
the full regression suite.

Deferred, matching the rigor-checked design's own staging: region
assignment/merging (phase 2/3 of the wider design) and extending
caller-allocated returns beyond the struct/sum `sret` that already exists.
This phase only changes the borrow-vs-move decision at direct call sites;
nothing here changes what gets freed or when.

Implicit region inheritance (direct backend): an unqualified `alloc_i64`/
`alloc_f32`/`alloc_u8` call now binds to the innermost active `with region`
block automatically, exactly as if written `region_name.alloc_*(...)`. This
is a pure AST qualifier rewrite in `cobra_ir_build`'s `AST_FUNC_CALL` case,
done before any other qualifier-dependent logic runs, so it flows through
the existing region-alloc IR and codegen paths unchanged (confirmed via
`emit-asm`: region-bound allocations compile to `arena_alloc@PLT`, not
`calloc@PLT`). Outside any `with region` block, behavior is unchanged. A
region-bound allocation can no longer be explicitly `free()`'d (the region's
own exit now owns cleanup) -- existing example code that allocated inside a
region and freed explicitly was updated to drop those now-redundant/invalid
free() calls.

## list[T]/dict return values (bug fix)

Returning a `list[T]` or `dict[...]` from a function previously fell through
to scalar return codegen, which only moved the data pointer into `rax` and
silently dropped length and capacity -- the caller got a corrupted value with
the right pointer but wrong (usually zero) length. `list[T]`/`dict` returns
now go through the same caller-allocated sret convention already used for
struct returns (`emit_list_return`/`emit_dict_return` in src/codegen.c write
each field into the caller's result buffer individually, since a list/dict
value's fields live in separate stack slots rather than one contiguous
block like a struct). The underlying heap buffer itself was never the
problem -- `cobra_list_append_*`'s backing storage is `realloc`-based, not
region/stack-backed, so no dangling-pointer risk was introduced. See
`examples/142_list_return_value.cb`.

This does not fix the separate issue of mutating a `list[T]` through a
function *parameter*: a list argument is copied field-by-field into the
callee's own stack slots at entry, so an `append`/index-write inside the
callee never touches the caller's copy. That is by-value parameter passing,
not a return-path bug, and needs pass-by-reference semantics for list
parameters to fix -- left for a dedicated pass.

`list[T]` still cannot be a struct field. The canonical type layer models a
list as an 8-byte reference (`COBRA_ABI_REFERENCE`, `size = 8`), but a local
list variable is actually three separate 8-byte stack slots (data pointer,
length, capacity -- see `ensure_list_named` in `src/codegen.c`), not one
boxed pointer. Embedding a list in a struct field would need a real boxed
representation (heap-allocate the three-word header, store its pointer in
the field) plus struct-field read/write/copy codegen that dereferences
through that box -- a second list representation alongside the flat one
used everywhere else, not a small addition. Left undone rather than forced;
`direct_struct_field_supported_kind` in `src/ir.c` still has no
`COBRA_TYPE_LIST` case.

An untyped function parameter used to silently default to `i64`
(`src/ir.c`, `is_param && p->declared_type == COBRA_TYPE_UNTYPED`) instead
of being rejected or inferred -- `def add(a, b): { return a + b }` compiled
today with both parameters hardcoded to `i64`, so calling it with an `f32`
argument silently coerced/truncated rather than erroring. Fixed: an
untyped parameter is now a compile-time error naming the parameter and
suggesting an annotation. `lib/std.cb`'s prelude had four such parameters
(`abs_val`, `max_val`, `min_val`, `vector_scale_avx2`, `file_write_log`)
that were silently relying on this default; all now carry explicit types.
Function return types are unaffected -- an omitted `->` type is an
established, separate convention (defaults to `i64` if a value is
returned, `void` otherwise), not the same silent-wrong-default bug.
Real single-parameter type inference (as opposed to rejecting the omission)
remains future work, tracked alongside the larger implicit-generics design
question.

Implicit-generic parameter inference, phase 1: `def name[](params)` (empty
brackets) marks an omitted-type parameter as implicitly generic, reusing
`specialize_generic_function`/`specialize_ast_tree` unchanged -- the
omitted parameter gets the same `COBRA_TYPE_GENERIC_PARAM` placeholder an
explicit `x: T` would (`assign_implicit_generic_param` in `src/parser.c`),
so it is monomorphized per call site through the identical pipeline. This
does not reverse the bare-parameter rejection above: `def name(params)`
(no brackets) stays an error; only the new `[]` form infers. Return types
stay fixed and explicit -- they are not generic. `export def name[](params)`
(mirrors the existing `pub`/`private` modifier position, before `def`)
requires at least one real call site to exist by end of compilation,
checked once at the end of `cobra_ir_build`, so a template with no caller
does not silently ship unchecked. A failed specialization's diagnostic
names both the triggering call site and the template declaration
(`specialization_call_line/col/file` on the specialized `ASTNode`, set in
`specialize_generic_function` from the calling `AST_FUNC_CALL` node).

Phase 2: more than one inferred parameter per function. Each omitted
parameter gets its own independent generic slot (`assign_implicit_generic_param`
now increments `fn_node->generic_param_count` per omission instead of
erroring on a second one), up to `COBRA_MAX_TYPE_ARGS` (8) -- a 9th is a
compile error, not silent truncation. `specialize_generic_function`/
`find_specialization` take an argument array instead of one type, and the
call-site binding loop in `cobra_ir_build`'s `AST_FUNC_CALL` handling maps
each parameter to its matching slot by canonical-type pointer identity
before calling `bind_generic_type`, rather than assuming slot 0. One real
subtlety: substituting N slots one at a time over the whole specialized
tree would make an unsubstituted later slot fail ABI validation mid-walk
(a bare `COBRA_TYPE_GENERIC_PARAM` looks unresolved until every slot is
filled), so `specialize_ast_tree_impl` takes a `validate_when_complete`
flag -- every slot substitutes first with validation off, then a second
pass (now a no-op for already-substituted nodes) validates once everything
is resolved. Named `def name[T](x: T)` generics are unaffected and stay
capped at exactly one explicit type parameter by the parser; only the
implicit `[]` form uses the N-slot path.

Phase 3: richer diagnostics beyond the two-location note already shipped.
Deferred indefinitely: implicitly-generic functions as `fn(...)->...`
values/closures/`dyn Trait` receivers (same restriction explicit `[T]`
generics already have, `src/ir.c` "generic functions cannot be used as
function values yet").

Function frames were a flat `sub rsp, 4096` regardless of a function's
actual local/temp usage (`COBRA_FRAME_BYTES`, `emit_function` in
`src/codegen.c`), capping safe recursion depth at ~2000 calls universally
-- even a zero-local function paid the same reservation as one that
genuinely needed 4KB. Fixed: `emit_function` now buffers a function's
whole prologue+body through `open_memstream`, reads the real peak
`cg->stack_offset` reached during compilation once the body is done
(reserve() only ever grows it, so the value at the end is the true peak,
not an underestimate), and patches the placeholder `sub rsp, 4096` text to
the real 16-byte-aligned size before flushing to the real output. This is
purely a text patch on the already-emitted prologue line -- no new
runtime mechanism, no change to how locals get their offsets. Measured
result on this machine (8MB `ulimit -s`): a trivial small-frame recursive
function's safe depth went from ~2200 to ~25000-30000 (roughly 12x);
depth and computed values verified correct, not just "didn't crash" (see
`examples/154_deep_recursion.cb`). @parallel workers and fn-value thunks
(`flush_pending_parallel`/`flush_pending_fn_thunks`) get their own
separate function frames emitted after this function's frame size is
captured and flushed, so they're unaffected and still use the flat
`COBRA_FRAME_BYTES` reservation -- narrowing that is future work.

That first pass left a second half of the same bug unfixed: `reserve()`
still refused any function's real usage past `COBRA_FRAME_LIMIT` (4000
bytes), and `emit_function` still clamped its patched `sub rsp` to the old
flat `COBRA_FRAME_BYTES` (4096) even after computing a larger real peak --
so a function that legitimately needed more than one page of locals still
hit "native frame exhausted" at compile time or got silently truncated,
same as before. Fixed by giving `CodeGen` a `stack_limit` field that
`reserve()` checks instead of the hardcoded constant: ordinary functions
now compile against a much larger `COBRA_FRAME_LIMIT_LARGE` (1MB) ceiling,
while @parallel workers/fn-thunks (which still use the flat, unpatched
frame) keep the original `COBRA_FRAME_LIMIT` so their real usage can never
exceed what they actually reserve. `emit_function`'s frame-size clamp now
matches `COBRA_FRAME_LIMIT_LARGE` instead of the old 4096 ceiling.
Verified with a function declaring twenty 32-field structs as locals
(5712 bytes of real usage, `sub rsp, 5712` in the emitted assembly,
confirmed by inspection) that previously would have been rejected outright
by the 4000-byte `reserve()` ceiling; it now compiles and runs correctly.
Full regression re-run after this change: all 118 `examples/*.cb` test
files (`cobra test`) pass, all 114 `tests/negative/*.cb` cases are still
rejected by `cobra check`, `make type-tests` and `make backend-ir-tests`
(1629 checks) are unaffected.

Tightening the frame size exposed two real, previously-latent bugs that
the old 4096-byte margin had silently absorbed -- both fixed alongside
the frame-size change, since shipping the tighter frame without them
would have reintroduced stack corruption for real programs:
1. Multi-argument call marshalling (`emit_call`'s `slot = temp + i * 24`)
   used `reserve()`'s return value (the TOP of the newly reserved region)
   as if it were the region's base, then walked to increasingly higher
   offsets for each argument -- overrunning the reservation by
   `(child_count-1)*24` bytes for any call with 2+ arguments. Fixed by
   anchoring from `call_arg_base = temp - child_count*24` instead, so
   every argument's slot stays within what was actually reserved. See
   `examples/155_frame_size_regression_coverage.cb`'s
   `test_seven_argument_call`.
2. Struct/list/dict/tensor return values pass a caller-allocated sret
   pointer (`result_temp`) to the callee, but the callee's write loop
   addresses it as the NEAR end and walks to deeper offsets
   (`[rax-0]` .. `[rax-(result_size-8)]`), not toward `rbp` -- the caller
   was only reserving up to `result_temp` itself, not the additional
   `result_size-8` bytes the callee actually writes past it. Fixed by
   reserving that extra depth too. See the same example's
   `test_struct_return_round_trip` (a struct value round-tripped through
   two chained calls, the exact shape that segfaulted before this fix).

`list[dyn Trait]` was later implemented after all - see the dynamic
dispatch section above (examples/169_list_dyn_trait.cb). The trait name
threading this note worried about turned out not to need a new
`element_dyn_trait_name`-shaped field: it reuses `IRLocal`/`VarSymbol`'s
existing (otherwise-idle-for-a-list) `dyn_trait_name` field on the list's
own local/symbol instead, and `list[T]` parsing gained a `dyn` case

## `cobra test --backend=native`: a real native test runner for backend_ir

Every parity measurement of backend_ir up to this point went through
`cobra build <file> --backend=native`, checked only against the 41
`def main`-entry-point examples. The other 118 example files - the actual
regression suite, each with one or more `def test_...` functions and
internal `assert()` calls, normally run via `cobra test` against the direct
backend - had never once been run through backend_ir. `cobra test
--backend=native` was previously made to error out outright ("the isolated
backend has no native test runner") rather than silently run the direct
backend and lie about the result, which was correct as far as it went but
left a real measurement gap. That gap is now closed: `cobra test
--backend=native` runs a real backend_ir test runner (`src/main.c`,
`run_native_tests_isolated` and friends), output-compatible with the
direct backend's `PASS: name()` / `FAIL: name()` / `Result: N passed, M
failed` format, plus a third state direct testing has no precedent for.

Design: PASS/FAIL/SKIP, not just PASS/FAIL. backend_ir does not support
every construct a test might use, and conflating "backend_ir doesn't build
this yet" with "backend_ir built this and got the wrong answer" would make
the numbers useless for prioritizing convergence work. So:
- The runner first tries to compile the whole program at once (mirroring
  `cobra build --backend=native`), rooted at a *synthesized* `main` that
  calls every discovered `test_...` function - not the user's own `main`,
  which is dropped. This matters mechanically as well as semantically:
  `bir_build_program`'s reachability pass (`mark_reachable_functions` in
  hir.c) only tolerates an "unsupported construct" failure as skippable
  when the failing function is unreachable from a top-level function
  literally named `main`; with no `main` at all it conservatively treats
  every top-level declaration as reachable, so a single unsupported
  construct anywhere across the six always-linked prelude libraries
  (std/mem/fs/cpu/time/net) would fail the *entire* compile regardless of
  whether the test in question ever touches it. Rooting reachability at a
  synthetic `main(): { test_a(); test_b(); ... }` makes pruning precise:
  prelude code no test actually uses is dropped instead of failing the
  build, while a genuinely unsupported construct in a *reachable* test
  still fails that attempt. The synthesized `main`'s compiled code is
  never invoked - it exists purely to root reachability - so its symbol is
  renamed post-compile (`sed` over the emitted assembly) before linking
  against a tiny generated single-call C runner per test, avoiding a
  `main`/`main` link collision.
- If the whole program compiles, every test is genuinely PASS/FAIL, run
  the same way the direct runner isolates each test (its own process,
  polled with a timeout, SIGKILL on hang).
- If the whole program does not compile, the runner falls back to
  compiling each test function individually - dropping every *other*
  `test_` function from the AST and synthesizing a `main` that calls only
  that one - so one unsupported test can't hide the pass/fail signal for
  the rest. A test whose individual compile also fails is reported `SKIP:
  name() (unsupported by backend_ir: <reason>)`; a test that compiles is
  run and reported PASS or FAIL exactly as in the fast path.
- Every `bir_backend_compile_program` call runs inside a forked child, not
  the CLI process itself. This uncovered a real, separate backend_ir bug:
  it had never previously been invoked more than once per process (every
  prior caller was a one-shot `cobra build`/`cobra test` invocation), and
  calling it repeatedly in this runner's loop reproducibly corrupted the
  heap on a later call (`bir_add_const`'s realloc segfaulting on bad heap
  metadata) after an earlier one. Forking gives each attempt a pristine
  copy-on-write snapshot, so this latent cross-call state bug - worth
  fixing in backend_ir itself at some point - can no longer take down the
  whole measurement run.

Real numbers, measured for the first time across the full 118-file suite
(`for f in examples/*.cb; do grep -qE '^def test_' "$f" && ./cobra test
"$f" --backend=native; done`), by individual test function:

- **107 passed**, **20 failed**, **165 skipped (unsupported)** - 292 test
  functions total.
- By file: 46 files pass every test with nothing skipped, 52 files skip
  every test (backend_ir cannot yet build any test in the file), 13 files
  have at least one real failure, and the remaining 7 are a mix of passing
  and skipped tests with no failures.
- The 20 real failures (backend_ir compiles the test but produces the
  wrong answer) cluster tightly: `list[T]` value semantics
  (examples/121_generic_list_struct.cb, 142_list_return_value.cb,
  165_list_pop.cb), collections (35_collections.cb), and the fs/net/time
  stdlib wrapper result types (63_time_results.cb, 64_fs_results.cb,
  65_net_results.cb) plus memory-contract edge cases
  (97_memory_contracts.cb). These are genuine regressions, not compile
  gaps, and are the most concrete next-work list this backend has had.
- The skips are dominated by constructs already known to be unsupported
  from the `cobra build --backend=native` def-main survey above: closures
  (`apply: parameter 'f' is outside the backend-IR subset`), generic
  aggregate/struct layouts, and GPU-dispatched tensor builtins
  (`sum_f32`/`matmul_f32`/etc.) - plus some newly visible ones specific to
  the assertion-test corpus, like nested-struct ownership verification
  failures (`cannot lower unverified SSA: ... stores an invalid or
  already-owned struct field`, examples/132_nested_struct_autofree.cb).

Verified: a deliberately broken assertion in a normally-passing test
(examples/100_generic_functions.cb, `assert(value_i64 == 7)` changed to
`== 999`) correctly reports `FAIL: test_generic_functions()`, matching the
direct backend's own report for the same break, then reverted cleanly. A
test using an unsupported construct (closures) correctly reports SKIP
without crashing or silently vanishing from the count. Full regression
check after adding this: `cobra test` (direct backend, no flag) is
unaffected - all 118 example files still pass with 0 regressions; all 114
`tests/negative/*.cb` cases are still rejected by `cobra check`; `make
type-tests` and `make backend-ir-tests` (1629 checks, 0 failures) are
unaffected.
directly in `parser_component_type`'s list branch.
