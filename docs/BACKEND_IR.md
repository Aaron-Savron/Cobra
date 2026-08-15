# Backend IR Design

This document describes the first backend-IR foundation: a flat,
arena-backed SSA representation with an explicit CFG and an i64/i32/u32/u64/
bool/f32/f64/u8 HIR subset plus bounded fixed value arrays, value-owned
scalar-field structs and borrowed readonly slice views, scalar dynamic buffers,
direct owning string and owning Option/Result payloads, and ownership-bearing
struct fields, built as a separate, isolated component. It is **not**
wired into the existing direct-to-assembly pipeline yet. The legacy `CobraIR` (src/ir.c) remains a separate validation pass over the
AST; production codegen continues to consume the AST directly. The isolated
backend does not replace that production path.

Generic borrowed-view structs are concrete, readonly aggregate values in this
lane. Their scalar type parameter is substituted before import, and a view field
uses the canonical pointer-plus-length layout while retaining borrow provenance
in evaluator state. Field copies copy the view descriptor without transferring
ownership; a field load still rejects an inactive source allocation. Generic
writable view fields, non-scalar generic collections, multiple type parameters,
and ownership-bearing generic aggregates remain outside this contract.


```
Cobra source
  -> tokens (lexer)
  -> syntax AST (parser)
  -> typed HIR / explicit CFG with mutable locals and scalar aggregates <-- new
  -> block-argument SSA                                    <-- new
  -> [verifier, textual dump, evaluator]
  -> target-independent MIR + MIR verifier + MIR dump
  -> isolated Linux x86-64 assembly emitter with scalar and supported owning aggregates <-- new
  -> existing IR validation / native codegen (unchanged)
```

## 1. Responsibilities: HIR versus SSA

The two layers serve different jobs.

**Scalar-typed HIR (CFG).** The HIR is the semantic CFG that a source program
is lowered into. Its locals and expressions carry canonical descriptors from
the backend module's own finalized type arena. The current subset supports
i64, i32, u32, u64, bool, f32, f64, and u8, bounded fixed value arrays,
and value-owned structs containing scalar and ownership-bearing fields, non-owning scalar and
struct-payload sums (value-only and owning struct components), borrowed
strings, writable views, owned slices, scalar dynamic buffers,
string-keyed scalar-value dicts, regions, direct owning
string payloads in Option/Result, nested owning sums, owned struct fields,
and owning structs nested inside owning structs (including owning struct
fields that carry strings, owning slices, or owning sums), plus
payload-carrying enums whose variants hold scalar or owning payloads
(constructed as `Shape.Circle(2.5)` or `Shape.Tag(concat(...))` and matched
with `case Shape.Circle(r)` or `case Shape.Tag(t)` bindings, with owning
payload moves, transfer, and destruction); it is
not yet a general typed HIR for ownership-bearing generic aggregates,
non-scalar generic collections, or other Cobra types. Scalar generic functions, scalar generic writable slices, and immutable
scalar-field generic structs are monomorphized into concrete HIR descriptors
before this boundary. It keeps *source-level mutable locals*: a
local is a named slot, assignments write slots, expressions read slots, and
control flow is
explicit blocks with jumps, conditional branches, and returns. No value
versioning happens here. The HIR builder consumes the existing AST produced
by the parser and accepts a small subset:

- scalar locals via `let`/`var`/`const` and implicit `x = expr` for `i64`,
  `i32`, `u32`, `u64`, `bool`, `f32`, and `f64`;
- assignments to scalar locals and copies of value-owned scalar-field structs;
- scalar-field reads and writes through canonical field addresses;
- struct parameters passed as pointers to caller-owned values, copied into
  callee-private aggregate slots;
- struct returns copied into explicit caller-provided return storage;
- i64/i32/u32/u64/f32/f64 arithmetic (width-wrapping for the fixed-width
  integers, IEEE-754 for floats) and bool-producing comparisons;
- `if`/`else`;
- `while` loops;
- bounded fixed value arrays `array[T, N]` with literal construction,
  indexed reads and writes, `len`, aggregate calls, aggregate returns, and
  fixed arrays as struct fields with whole-field and whole-struct value
  copies through bounds-checked index addressing;
- `for` loops over a scalar bound (`for i in n`), over `range(a, b)`, and over
  constant array literals (`for x in [1, 2, 3]`);
- `return`, including multiple returns from one function;
- function parameters and calls between user functions, including value-owned
  scalar-field struct arguments and explicit aggregate return storage;
- readonly slice parameters and non-escaping readonly slice locals for `i64`,
  `f32`, and `u8`; `slice_u8(view, start, length)` constructs a checked
  subview, while indexing lowers through `view_ptr`, byte-offset scaling,
  and a typed load;
- writable `out []T` view parameters with bounds-checked indexed stores;
- owned slice locals from `alloc_i64`, `alloc_f32`, and `alloc_u8`, with
  `free`, indexed access, `len`, subviews, and region-qualified allocation;
- owned scalar `list[T]` locals with literal construction, `append`, `pop`,
  indexed access, `len`, ownership-moving calls and returns, growth, and
  explicit `free` destruction;
- `with region` scopes with explicit region entry, cleanup, and lifetime checks;
- scalar generic functions with one type parameter, scalar call-site inference,
  canonical substitution, concrete clone names, calls, returns, and duplicate
  specialization reuse;
- immutable scalar-field generic structs with one scalar type parameter,
  concrete canonical layouts, field access, aggregate calls and returns, and
  specialization reuse;
- scalar generic collections with one type parameter, canonical `list[T]`
  substitution for scalar elements, ownership-moving calls and returns,
  append, pop, indexed access, and explicit destruction;
- scalar generic writable slices with one type parameter, canonical `out []T`
  substitution for scalar elements, indexed stores, writable-view calls,
  borrowed returns, provenance propagation, and readonly-to-writable rejection;
- lifetime-aware scalar generic view returns: a specialized readonly or
  writable return must derive from exactly one corresponding view parameter;
  owned actuals may satisfy that parameter only at the call boundary, where
  the result is retagged as a borrow without changing allocation or region
  identity;
- generic structs with one scalar type parameter and readonly borrowed-view
  fields, including canonical view layout, field assignment and extraction,
  aggregate parameter copies, aggregate returns, and lifetime provenance;

Fixed arrays are value-owned aggregates with a canonical element type and
compile-time length. Their storage is inline in a stack slot or aggregate
field, and indexing is lowered to a verifier-checked byte address. Scalar
`list[T]` values are owned growable buffers with explicit length/capacity,
append growth, pop, indexed access, calls, returns, and destruction. The
buffer lane accepts scalar and value-only struct elements; its ABI passes the
pointer, length, and capacity parts, with the capacity part discarded by the
native view model on receive and passed as length on call. The lane remains
isolated from production codegen.

The HIR builder never constructs SSA, and the parser is never asked to
produce SSA. Unsupported AST forms (tensors, ownership-bearing generic
structs, non-scalar generic collections, unresolved generic declarations,
dynamic buffers with owning elements, writable generic-view fields, and
ownership-bearing aggregate forms not listed above) are rejected
with an explicit builder diagnostic. The isolated lane accepts owned strings, owned slices, and
owning sums in struct fields, but it still rejects ownership-bearing forms
outside that contract. This keeps the new lane separate from the remaining
ownership/generic machinery that the existing compiler validates.

**Block-argument SSA.** The SSA pass is a dedicated consumer of the HIR. It
linearizes each block's expression trees into SSA instructions, replaces
mutable locals with value versioning, and inserts *block arguments* at join
points. The SSA form has no phi nodes: a join block declares its incoming
values as block parameters, and every predecessor terminator passes those
values as edge arguments on the edge it takes. This makes the CFG dataflow
explicit and easy to verify, and it matches the "block arguments" model used
by newer compiler pipelines (MLIR-style `br` with operands) better than
phi-as-instruction models for a minimal backend.

## 2. Flat arena-backed representation

All SSA objects live in one per-module arena implemented as growable pools.
Identity is a stable integer handle, never a pointer:

| Handle       | Type       | Meaning                                  |
|--------------|------------|------------------------------------------|
| `SsaValueRef`| `uint32_t` | index into the value pool                |
| `SsaInstRef` | `uint32_t` | index into the instruction pool          |
| `SsaBlockRef`| `uint32_t` | index into the block pool                |
| `0`          |            | the invalid/absent handle for every kind |

The pools are:

- **values**: one entry per SSA value: kind (function parameter, block
  parameter, constant, instruction result), canonical type, definition
  back-reference (defining instruction or parameter index), constant
  payload, source span;
- **instructions**: one entry per instruction: opcode, result type and
  result value, variable-length operand range, terminator targets and edge
  argument ranges, effect tag, callee name for calls, source span;
- **blocks**: one entry per basic block: ordered instruction list,
  terminator reference, block parameter list, predecessor and successor
  lists, source span;
- **operands**: one flat pool of `SsaValueRef`; each instruction stores a
  `(start, count)` window into it, so operand lists are variable length and
  never fixed at `args[3]`;
- **edge arguments**: one flat pool of `SsaValueRef`; terminators store up
  to two `(start, count)` windows (then-edge and else-edge) aligned with
  their successors' block parameters.

Handles stay valid for the lifetime of the arena; appends never move earlier
entries because pools are pointer-stable growable arrays.

## 3. Typed scalar values

Every HIR expression and SSA value carries a canonical scalar descriptor and,
when it is a constant or runtime slot, a `BirScalarValue` payload. The payload contains the canonical type, an explicit scalar kind (`i64`, `i32`,
`u32`, `u64`, `bool`, `f32`, `f64`, or `u8`), and a union payload. `f32` values are stored as IEEE-754 bit patterns rather
than being reinterpreted as integers. The same representation is used for
function arguments, call results, returns, evaluator frames, and simulated
memory slots. This prevents the evaluator from silently treating a floating
value as an integer.

The memory model now uses canonical `pointer[T]` descriptors. Stack slots
produce frame-local pointers, pointer arithmetic adds signed byte offsets, and
loads/stores carry the pointee type, byte width, alignment, and address space.
Integer widths are preserved in payloads and memory accesses; `i32` and `u32`
truncate to 32 bits, `u64` remains unsigned at comparison and division
boundaries, and `f32` and `f64` retain IEEE bit patterns.
The evaluator uses byte-backed per-frame memory. Native machine pointers,
global addresses, and escape analysis remain later backend work.

## 4. Instruction set

The instruction set is intentionally minimal, with explicit scalar and ownership-bearing aggregate operations:

| Opcode      | Operands          | Result | Notes                            |
|-------------|-------------------|--------|----------------------------------|
| `const`     | none              | scalar | constant materialization         |
| `param`     | none              | scalar | function parameter (entry block) |
| `block_arg` | none              | scalar | block parameter (join value)     |
| `add/sub/mul/div/rem` | 2        | integer or float scalar | numeric arithmetic (`rem` is integer-only) |
| `neg`       | 1                 | integer or float scalar | unary minus                      |
| `eq/ne/lt/le/gt/ge` | 2        | bool   | comparisons yield 0 or 1         |
| `stack_slot`| none              | `pointer[T]` | frame-local aligned slot         |
| `ptr_add`   | pointer, i64     | `pointer[T]` | signed byte offset               |
| `field_addr`| `pointer[struct]`| `pointer[field]` | canonical field offset       |
| `load`      | `pointer[T]`     | T      | typed memory read (effect: read) |
| `store`     | pointer, T       | none   | typed memory write (effect: write)|
| `agg_copy`  | `pointer[S]`, `pointer[S]` | none | aligned byte copy for value-owned aggregates (readwrite) |
| `sum_payload_store` | sum pointer, owned view | none | move an owned slice into the active sum payload |
| `sum_payload_load` | sum pointer | owned view | extract and clear the active owned sum payload |
| `sum_move` | sum pointer, sum pointer | none | move an owning sum storage region |
| `sum_drop` | sum pointer | none | drop the active owning sum payload |
| `field_payload_store` | aggregate pointer, owned view | none | move an owned slice into a struct field |
| `field_payload_load` | aggregate pointer | owned view | extract and clear an owned struct field |
| `agg_move` | aggregate pointer, aggregate pointer | none | move an ownership-bearing aggregate |
| `agg_drop` | aggregate pointer | none | recursively drop owned aggregate fields |
| `region_enter` | none | none | begin a declared region lifetime |
| `region_exit` | none | none | destroy allocations owned by a region |
| `transfer` | `pointer[T]` | `pointer[T]` | move an owned region allocation to another active region |
| `destroy`  | `pointer[T]` | none | explicitly destroy one owned allocation      |
| `view_make` | pointer, i64 | borrowed slice view | pointer-plus-length borrowed view |
| `view_ptr` | borrowed slice view | `pointer[T]` | borrowed element pointer preserving mutability |
| `view_len` | readonly slice view | i64 | logical element count |
| `slice_alloc` | i64 | owned slice view | frame allocation with owned-slice contract |
| `slice_free` | owned slice view | none | destroy the owned slice allocation |
| `buffer_alloc` | i64 | owned `list[T]` | create a scalar growable buffer |
| `buffer_append` | buffer, T | owned `list[T]` | grow, append, and move the old owner |
| `buffer_pop` | buffer | T | read and remove the last element |
| `buffer_free` | owned `list[T]` | none | destroy an unborrowed buffer |
| `call`      | n (lowered args) | scalar or none | scalar call result, or aggregate call into explicit storage |

| `jump`      | target + edge args | none | unconditional terminator         |
| `branch`    | cond + 2 targets + edge args | none | conditional terminator   |
| `return`    | 0 or 1           | none   | function terminator              |

`stack_slot`, `ptr_add`, `field_addr`, `load`, `store`, and `agg_copy` are
exercised by direct unit tests against the byte-backed evaluator memory. The
HIR builder lowers every supported scalar local and value-owned scalar-field
struct into an addressable typed stack slot. Local reads become typed loads and
assignments become typed stores; CFG edges therefore carry no local SSA values.
No alias analysis or object-boundary analysis is performed yet.

### Memory/effect semantics

Every memory instruction carries explicit pointer and memory metadata:

- pointers are `pointer[T]` values with a frame id and signed byte offset;
- stack slots are aligned ranges in a 64 KiB frame-local byte arena;
- typed scalar accesses use the pointee's finalized size and alignment;
- aggregate slots use canonical struct size/alignment; field addresses must
  match a real canonical field offset and field width;
- value-owned aggregate copies require two pointers to the same scalar-field
  aggregate and copy exactly its finalized byte width;
- ownership-bearing sums and structs use explicit payload stores, payload loads,
  aggregate moves, and recursive drops; raw aggregate copies are rejected for
  those values;
- owned slice payloads use the native pointer-plus-length representation, and
  payload moves clear the source descriptor before the next cleanup boundary;
- address space 0 denotes frame-local evaluator memory;
- pointer arithmetic is checked for signed overflow and memory accesses are
  checked for null, inactive-frame, bounds, and alignment violations;
- native pointer address spaces, globals, and alias identities are not
  supported yet.

Every instruction that touches memory carries an explicit effect tag:

- `SSA_EFFECT_NONE`: pure value computation;
- `SSA_EFFECT_READ`: `load`; reads memory but never mutates it;
- `SSA_EFFECT_WRITE`: `store`; mutates memory;
- `SSA_EFFECT_READWRITE`: aggregate copies, payload moves, payload extraction,
  aggregate moves, and drops read and write aggregate storage;
- `SSA_EFFECT_CALL`: `call`; the callee's effects are opaque to this
  backend lane.

The tags and memory fields are metadata for future passes (reordering,
common-subexpression elimination, liveness, and eventual alias analysis).
The verifier requires the typed pointer, width, alignment, and effect
contracts. Full alias/ordering analysis and stack-pointer escape checking are
explicitly out of scope for this milestone.

### Pointer ownership contracts

`pointer[T]` is the canonical representation, while each SSA pointer value
also carries a separate backend contract. The contract is not part of type
identity: it is the permission and provenance fact for that value at the
current IR point.

- `owned-frame` is produced by `stack_slot`; it is readable and writable only
  while its defining evaluator frame is active.
- `owned-region` is produced by a region stack slot or an ownership transfer.
  It is readable and writable while its declared region is active and is
  invalidated by explicit destruction or region exit.
- `borrow-readonly` is used for a callee's view of a pointer parameter. It may
  be read and derived into more readonly views, but it cannot be a store or
  aggregate-copy destination.
- `borrow-write` is a writable borrowed view. It may be read or written, but
  it does not transfer ownership.
- `caller-storage` is the callee-side contract for hidden aggregate return
  storage supplied by the caller. It is writable for the duration of the
  call and is never returned as a pointer.

Pointer arithmetic and field addressing preserve the source contract. Loads
require a readable contract; stores and aggregate-copy destinations require
a writable contract; aggregate-copy sources require a readable contract. A
struct parameter is passed as a readonly borrow and copied into callee-owned
storage. A struct return uses caller storage rather than returning a frame
pointer. Calls check that the actual contract can satisfy the callee contract,
and the evaluator retags the pointer at the call boundary to the callee's
borrow view.

The verifier rejects unknown pointer contracts, pointer returns, writes
through readonly borrows, incompatible call arguments, and aggregate copies
with an invalid source or destination permission. Pointer values also carry an
allocation identity and region origin, so derived aliases retain the same
lifetime owner. Declared regions form a parent tree. `region_enter` and
`region_exit` establish and close lifetimes, `transfer` moves an owned
allocation to an active destination region, and `destroy` consumes one owned
allocation. The verifier rejects child-lifetime violations, use after region
exit, and double destruction with path-sensitive allocation and region state.
It joins predecessor states at branches and loop backedges; conflicting live,
dead, owner, or region states become ambiguous and are rejected at later uses.
The evaluator enforces the same checks dynamically for all executed paths.
Frame teardown remains an implicit cleanup boundary, and frame pointers are
still rejected after the frame is gone. The borrowed-view lane is represented
by canonical readonly and narrow writable slice values. A view contains only a typed
pointer and element count; it does not
own or free its backing allocation. `view_make` requires a readable source
pointer, `view_ptr` preserves the source frame/region/allocation identity, and
`view_len` exposes the logical count. The evaluator checks view bounds before
loads. Readonly views may be passed as parameters and returned only when they
are derived from a caller-provided view; returning a view backed by the
callee's frame or region, storing a view in owned aggregate storage, or using a
view after its region exits is rejected. The narrow writable view lane uses
`out []T` and `borrow-write`: one active writer may read or write through its
own extracted pointer, while a second mutable borrow, a readonly borrow
overlapping a writer, or an owner write while any view is active is rejected.
Writable views do not transfer ownership and preserve frame, allocation, and
region provenance. Calls propagate the returned view's provenance from its
single source view parameter. Generic specialization preserves this same
contract after replacing `T` with a scalar type. An owned actual may satisfy a
borrowed parameter at a call boundary, but a function cannot declare an owned
parameter and return a view of that parameter. Frame-local and region-local
return sources are rejected by the verifier. General flow-sensitive move
analysis and non-scalar owned collections remain future work.

### Call ABI metadata

Calls carry the callee name and lowered argument list. The module function table
also carries a complete `BirCallAbi` record for every signature, so a call is
checked against metadata rather than reconstructed from AST compatibility
fields. HIR predeclares all signatures before lowering bodies, so forward and
recursive calls use the same canonical signature and ABI record.

The call record contains:

- the Cobra calling convention identifier;
- lowered parameter count, including a hidden aggregate-return storage parameter;
- one or more abstract locations for every lowered parameter and return;
- direct versus indirect passing mode;
- GPR or XMM register class and abstract register ordinal;
- stack argument offsets, widths, alignments, total stack size, and required
  stack alignment;
- the variadic flag, currently false for this source subset.

Abstract register ordinals are not physical register names. The current profile
has six GPR argument positions, eight XMM argument positions, and 16-byte stack
alignment. Values that exhaust a class spill to aligned stack offsets. Views
occupy two GPR parts, scalar floating values use XMM locations, and scalar
integer or enum values use GPR locations. Structs and sums are passed
indirectly through one GPR or stack pointer location. Aggregate returns are
represented as an indirect return plus a hidden caller-storage parameter; they
produce no SSA call result. Scalar and view returns use the corresponding GPR
or XMM return locations.

Pointer contracts remain explicit at every aggregate call boundary: struct and
sum inputs are readonly borrows and hidden return storage is caller-provided
writable storage. These structs are mutable in HIR; "value-owned" describes
storage and field ownership, not constness. Struct and sum parameters lower to
pointer arguments and are copied into callee-private storage. The evaluator
checks the ABI record before entering a callee, and the verifier rejects
inconsistent or tampered location metadata. This is a target-neutral call
contract. MIR will later map abstract locations to a concrete target ABI.


## 5. Source spans

Every value, instruction, and block records `(line, column)` copied from the
AST node that produced it; the module owns a single source-file name for the
program. Spans are informational today (used by the printer and diagnostics)
and will feed the structured diagnostic system later.

## 6. Typed HIR/CFG builder

The builder walks an `ASTNode` function body with an explicit environment:

- **locals**: a function-wide table mapping source name to a local index.
  Parameters occupy indices `0..param_count-1`. A `let`/`var`/`const` or an
  implicit assignment registers or updates a slot. Reading an unregistered
  name is a builder error (matching the host interpreter's failure on reads
  of unknown locals).
- **blocks**: created for straight-line sequences, `if`/`else` joins,
  `while` headers/latches/exits, and `for` loops. `for` over a scalar bound
  or `range(a, b)` is lowered to the same shape as `while`: an induction
  local, a header comparing against the bound, a body, and a latch that
  increments. `for` over a constant array literal is unrolled into
  straight-line copies of the body with the loop variable rebound per copy
  (bounded at 64 elements), which matches the host interpreter's
  per-iteration rebinding semantics exactly.
- **terminators**: `jump`, `branch(cond)`, and `return(expr)`, always the
  last element of a block.

The HIR keeps source-level mutable locals: assignments write local slots and
expressions read them; no versioning happens in the builder. During SSA
lowering, each local receives one canonical stack slot, parameters are stored
at entry, reads become typed loads, and assignments become typed stores. This
preserves addressability and makes local state flow through memory rather than
through block arguments.

## 7. SSA construction pass

The pass consumes the HIR and produces block-argument SSA in three steps.
This is a deterministic, liveness-based two-pass construction for the current
straight-line-block subset; it is not an on-demand Braun or sealed-block
algorithm:

1. **Linearization**: each block's statements are processed in order.
   Expression trees evaluate to SSA values: constants become `const`
   instructions, binary operations emit their opcode, calls emit `call`.
   Assignments record the local's current SSA value. This is straight-line
   code, so ordering inside a block is trivially correct.

2. **Live-in sets**: a backward dataflow fixpoint over the CFG computes,
   for every block, the set of locals whose value must be available at block
   entry:
   `live_in(B) = reads_before_assign(B) ∪ { L in live_in(S) : B never assigns L }`
   over all successors `S`. `reads_before_assign` is the set of locals read
   by any statement or terminator before the block's own last assignment to
   them. Because each block is straight-line, this is precise.

3. **Block parameters and edge arguments**: direct SSA clients can use the
   liveness sets to represent live-ins as block parameters, ordered by local
   index for determinism. The source-lowering path deliberately does not do
   that for mutable locals: every local is already represented by a typed stack
   slot, so every CFG edge has zero local edge arguments and successor blocks
   reload their current values from memory. Reading a local that is never
   defined on some path is still rejected by HIR construction.

This remains a liveness-based block-argument construction without phi nodes for
SSA values that need to cross joins. The source-local memory lowering avoids
inventing SSA aliases for mutable storage. A sealed-block algorithm may replace
it later when the HIR supports richer control-flow and value definitions.

## 8. Verifier invariants

`backend_ir_verify` checks, for every module:

- **one definition per value**: every value has exactly one defining
  entity (one instruction, one parameter index, or one constant payload);
  duplicate or dangling definitions are rejected;
- **valid operands**: every operand handle is in range and not the invalid
  handle; arity matches the opcode (binary ops take exactly two operands,
  `load` one, `store` two, `call` exactly its function's parameter count);
- **edge arguments**: every edge's argument list has exactly the
  successor's block-parameter count; every argument is valid and has the
  target block parameter's canonical type;
- **opcode signatures**: arithmetic, comparison, branch, memory, call, and
  return instructions have the documented operand arity, canonical operand
  types, result type, and effect metadata;
- **function signatures**: parameter SSA values, call arguments/results,
  and return operands match their owning function's canonical signature and
  ABI metadata;
- **terminators**: every block has exactly one terminator, the terminator
  is the last instruction in its block, and no instruction appears after a
  terminator;
- **dominance and use ordering**: a dominator tree is computed with the
  standard iterative algorithm; every use of a value is dominated by its
  definition; a use inside the defining block appears strictly after the
  definition (block parameters, function parameters, and constants are
  defined at block entry). Unreachable blocks are legal and are still
  associated with an explicit owning function block range, so their signature
  and structural checks run; dominance is not enforced because no execution
  path reaches them;
- **no unresolved generics**: no value, instruction, or block carries a
  type that recursively contains a `GENERIC_PARAM` descriptor;
- **canonical finalized types**: every type attached to a value is a
  non-NULL, canonical, finalized `CobraType`;
- **ownership dataflow**: region activity, allocation liveness, and ownership
  transfer are propagated through predecessor joins and loop backedges;
  conflicting states are marked ambiguous and cannot be used, destroyed, or
  transferred;

## 9. Textual dump

A deterministic printer emits the module in a stable form: functions and
blocks in creation order, values in definition order, operands in order,
types printed from the canonical descriptor names. Scalar `f32` constants are
printed as `f32bits(0xNNNNNNNN)`, preserving signed zero, infinities, and NaN
payloads exactly. Example:

```
fn "main"() -> i64
block b0 [entry] {
  v0: i64 = const 2
  v1: i64 = const 3
  v2: i64 = mul v0, v1
  v3: i64 = const 1
  v4: i64 = add v2, v3
  ret v4
}
```

The dump is used for debugging and for golden assertions in tests; it is
deterministic across runs for identical input.

## 10. Evaluator

A small interpreter executes the SSA form directly:

- one value-slot array per call frame indexed by `SsaValueRef`, so recursive
  and re-entrant calls cannot clobber their callers' locals or parameters;
- typed scalar value slots per frame, including f32 bit-pattern payloads;
- per-frame 64 KiB byte memory backing typed `load`/`store`; accesses enforce
  pointer provenance, bounds, pointee width, and alignment;
- a call stack of return targets with depth and step limits, so malformed
  IR cannot hang the host;
- function parameters are bound from call arguments (and from zero at the
  entry function, matching the host interpreter's convention), block
  parameters are bound from edge arguments at block entry.

The evaluator is the reference execution semantics for the new lane and the
basis for differential tests against the existing host interpreter.

## 11. Unit and differential tests

`tests/backend_ir.c` covers:

- straight-line arithmetic;
- `if`/`else` value merge;
- loop-carried values (`while` and `for`);
- nested branches;
- multiple returns;
- unreachable blocks (valid, never executed);
- malformed CFG rejection (terminator to a missing block, missing
  terminator);
- malformed block-argument arity rejection;
- typed i64, i32, u32, u64, bool, f32, f64, and u8 stack-slot load/store round-trips;
- value-owned scalar-struct stack slots, field addresses, aggregate copies, and
  Cobra source lowering;
- struct parameter copies, hidden return storage, cross-function calls, and
  recursive aggregate-argument calls;
- pointer byte-offset arithmetic;
- pointer arguments through the evaluator's typed call machinery;
- malformed pointer, width, alignment, and pointee-type rejection;
- fixed value-array layout, literal construction, indexed loads and stores,
  aggregate calls and returns, bounds checks, and malformed index rejection;
-  borrowed readonly view construction, pointer extraction, length access,
  readonly call parameters, source-level readonly slice locals, indexed
  source reads, generic readonly and writable view returns, call-boundary
  owned-to-borrow adaptation, provenance preservation, escape rejection, and
  malformed return metadata;
- writable view parameters, indexed stores, writable calls, and borrow-conflict
  rejection;
- source-level owned slice allocation, indexed access, subviews, destruction,
  region-qualified allocation, and lifetime rejection;
- source-level scalar dynamic buffers: list construction, capacity growth,
  append, pop, indexed access, bounds failures, ownership-moving calls and
  returns, explicit destruction, and malformed buffer-IR rejection;
- readonly view mutation, region escape, and invalidation rejection;
- region parent/child lifetime ordering;
- explicit ownership transfer;
- use-after-region-exit and double-destroy rejection;
- scalar f32 constants, arithmetic, comparisons, calls, returns, evaluator execution, and deterministic dumps;
- malformed scalar payload rejection;
- generic-parameter and non-finalized-type rejection by the verifier;
- verified SSA to MIR lowering, virtual-register definitions, machine types,
  CFG edges, ABI moves, calls, returns, clobber sets, deterministic dumps, and
  MIR rejection of malformed call metadata;
- isolated x86-64 assembly emission for scalar calls, branches, stack memory,
  f32/f64 arithmetic, returns, ABI moves, view bounds checks, and writable
  stores;
- native scalar-field struct parameter copies, aggregate calls, field access,
  caller-provided returns, and execution through both spill-all and allocated
  emitters;
- native readonly and writable view ABI calls, returns, indexed access, and
  execution through both spill-all and allocated emitters;
- isolated evaluator lowering for ownership-bearing struct fields, including
  field replacement, recursive cleanup, parameter moves, and caller-storage
  returns;
- native owning Option/Result payloads and ownership-bearing struct fields,
  including indirect calls, returns, payload extraction, moves, and recursive
  drops through both spill-all and allocated emitters.

Differential tests parse the same Cobra source with the real parser, run
each successful function through the host interpreter
(`interpreter_run_function`) and through the new SSA evaluator, and require
both successful execution and an identical result. Rejection cases are tested
separately: both engines must reject and the backend diagnostic must contain
the expected error class. Supported differential programs stay inside the
intersection of the two engines' subsets (i64/bool/f32 locals where the host result model permits it, arithmetic,
comparisons, `if`/`else`, `while`,
`for` over constant arrays, calls, recursion, `return`).

## 12. Type-arena ownership boundary

The parser owns its frontend canonical arena. Backend IR does not retain
pointers into that arena. During HIR construction, supported frontend scalar
kinds, struct layouts, scalar generic specializations, readonly borrowed-view
fields, and ownership-bearing field descriptors are imported into the backend
module's separately owned arena; HIR and SSA values reference
only those backend-owned, finalized descriptors. Struct size, alignment, field
offsets, and field ownership are checked against the frontend descriptor at the
import boundary. This is an explicit prototype boundary, not a general
type-cloning facility: non-scalar generic collections, ownership-bearing
generic aggregates, and native machine-pointer lowering remain unsupported
until their layout, ownership, and ABI import rules are specified.
Scalar-payload enums are an exception: their variants import as
tag-plus-payload aggregates with resident payload slots, and their match
bindings lower through the checked sum-access lane (see section 13.8.2). The backend
prototype's frame-local `pointer[T]` descriptors are owned by its canonical
arena and are not frontend source declarations.

The isolated backend modules live under `src/backend_ir/` and are compiled
and tested on their own; HIR, SSA, and MIR are not linked into the production
`cobra` binary yet.

## 13. Explicit non-goals for this milestone

- No target-specific register allocation completion, instruction selection,
  object/binary writers, JIT, or new targets. The isolated scalar lane already
  has a verified linear-scan allocation pass and an allocated x86-64 emitter.
- No replacement of the existing direct emitter or the existing `CobraIR`
  validation pass.
- No weakening of ownership/region/lifetime rules; aggregate storage is
  value-owned, struct calls copy or move parameters according to field
  ownership and use explicit caller storage, while pointer origins, region
  lifetimes, explicit transfer, destruction, and memory effects are verified
  without full alias analysis or general flow-sensitive move analysis.
- Owned slices in the isolated backend contract are implemented and tested:  explicit `slice_alloc`/`slice_free` with use-after-free and double-free
  rejection and borrow-conflict checks against active views. Source-level
  owned slice locals, `alloc_i64`/`alloc_f32`/`alloc_u8` creation, `free`
  destruction, indexed reads/writes, `len`, `slice_u8` subviews of owned
  `u8` buffers, owned-to-view borrows at call boundaries and view aliases, and
  `with region` blocks with region-qualified allocations now lower end to end.
  Scalar generic `list[T]` specializations use the same owned-buffer contract
  for scalar element types, including append, pop, calls, returns, and
  destruction.
  Owned slice parameters and returns now use explicit move semantics: a call
  consumes the caller owner, a returned owned value restores or transfers the
  allocation into caller storage, and post-move uses are rejected. Fresh
  owned strings now use the same owned u8-slice transfer path for concat,
  indexed reads, fresh returns, and `string_free`. Direct owning string and owned-slice Option/Result payloads
  now use explicit sum move, extraction, transfer, and `sum_drop`. Nested
  owning sums use recursive move and drop over active inner variants.
  Ownership-bearing struct fields use explicit field payload load/store,
  aggregate move, aggregate drop, parameter move, and caller-storage return
  paths. Scalar generic view returns are implemented for one source view
  parameter with provenance and escape checks. Arbitrary lifetime-polymorphic
  returns, owner-preserving generic return forms, and new ownership-aware
  generic lanes remain deferred.
- No general borrow checker for arbitrary source CFGs yet; the verifier's
  ownership state is conservative and rejects ambiguous path joins.
- No native global addresses, object-boundary alias analysis, or general
  native pointer ABI lowering. The isolated view lane is the explicit
  exception and receives pointer-plus-length values through its verified ABI.
  Native writable-view stores, heap-backed owned slices, region cleanup, scalar
  sum checks, owned-slice argument moves, descriptor transfers, explicit
  destruction, supported owning sum payloads, and ownership-bearing struct
  fields are supported. Arbitrary non-scalar aggregate layouts and production
  pointer lowering remain outside this emitter.

## 13.5 Source-feature support and gaps

The HIR builder lowers these Cobra source forms today:

- scalar locals and parameters (`i64`, `i32`, `u32`, `u64`, `bool`, `f32`,
  `f64`) with typed stack slots;
- fixed value arrays `array[T, N]` for scalar `T`, with literal construction,
  indexed reads and writes, `len`, aggregate calls, aggregate returns,
  verifier-enforced bounds, and struct-field arrays with whole-field and
  whole-struct value copies through a bounds-checked `array_index_addr` MIR
  opcode emitted by both native emitters;
- nested fixed arrays `array[array[T, N], M]` for scalar `T`: recursive
  canonical import, nested literals, whole-row index reads and writes via
  aggregate copies, struct fields, aggregate calls and returns, and inline
  byte-wise aggregate emission in both native emitters; non-scalar inner
  elements are rejected by the parser;
- fixed arrays of value-only structs `array[Point, N]`: parser support for
  named struct elements and zero-initialized array declarations, canonical
  import gated to structs without owned payloads or borrowed views,
  whole-struct element writes, aggregate-returning calls stored directly into
  element and field slots, array parameters, struct fields, calls and
  returns, and inline aggregate copies in both native emitters; owning-struct
  elements are rejected at the isolated boundary;
- `if`/`else`, `while`, `for` over a scalar bound, `range(a, b)`, and constant
  array literals (unrolled up to 64 elements);
- value-owned structs with scalar, owned string, owned slice, and owning
  sum fields; canonical field addresses, field payload load/store, recursive
  field move/drop, struct parameters through private copies or moves, and
  struct returns through caller storage;
- immutable scalar-field generic structs with one scalar parameter, concrete
  specialization, field access, aggregate calls, and caller-storage returns;
- generic structs with one scalar parameter and readonly borrowed-view fields,
  including field assignment and extraction, aggregate copies, safe parameter
  forwarding, and escape rejection;
- `readonly []i64`/`[]f32`/`[]u8` parameters and non-escaping local aliases,
  `slice_u8` subviews, indexed reads, `len`, and borrowed-view returns derived
  from one view parameter;
- `out []T` writable view parameters with one-writer borrow-conflict checks;
- scalar generic `readonly []T` and `out []T` returns derived from exactly one
  specialized view parameter, including call-boundary provenance, malformed
  return metadata rejection, unresolved recursive specialization rejection,
  and frame or region escape rejection;
- owned slice locals created by `alloc_i64`/`alloc_f32`/`alloc_u8`, destroyed
  by `free`, with indexed reads/writes, `len`, `slice_u8` subviews, and
  owned-to-view borrows at call boundaries and view-typed aliases;
- owned slice parameters and returns with move semantics, including returned
  allocation handoff and post-move lifetime checks;
- scalar `list[T]` buffers with literal construction, `append`, `pop`, indexed
  reads, `len`, growth, ownership-moving calls and returns, explicit `free`,
  bounds failures, and malformed-IR checks;
- `with region NAME(capacity):` blocks with `NAME.alloc_*` region-qualified
  allocations; the isolated evaluator models region exit as the cleanup
  boundary (the capacity expression is accepted but not enforced);
- non-owning scalar sums: `Option[T]`/`Result[T, E]` with scalar components
  (`i64`/`i32`/`u32`/`u64`/`bool`/`f32`/`f64`/`u8` payloads), `some`/`none`/
  `ok`/`err` construction, `is_some`/`is_ok`, and checked
  `unwrap`/`unwrap_ok`/`unwrap_err`, including nested scalar sums to
  arbitrary depth (see section 13.6);
- direct owning string and owned-slice payloads in `Option`/`Result`, with
  `sum_payload_store`, `sum_payload_load`, `sum_move`, `sum_drop`, ownership
  transfer across calls and returns, and `free(sum)` destruction;
- nested owning sums with recursive `sum_move` and `sum_drop`, including
  nested construction, extraction, inactive branches, calls, and returns;
- ownership-bearing struct fields with recursive field move/drop, field
  replacement, calls, returns, inactive variants, and use-after-move checks;
- borrowed strings as readonly u8 views: string literals, `string` parameters,
  string locals, `len`, `s[i]` byte reads, `slice_u8` subviews, and string
  arguments to readonly string parameters;
- fresh owned strings from `concat` and string `+`, owned string returns,
  indexed reads, and `string_free` (see section 13.7);
- unit enums and `match`: `enum` declarations, `Color.Variant` constants,
  enum locals/parameters/returns, discriminant comparisons, and match with
  exhaustiveness and duplicate-arm rejection (see section 13.8);
- `match` on `Option`/`Result` with payload binding arms: `case some(x)`/
  `case none`/`case ok(v)`/`case err(e)`, moving scalar, owning-struct, and
  nested-sum payloads into the binding through the sum-access machinery
  (see section 13.8.1).

Exact source forms that still cannot lower into backend IR:

- returns on any path inside a `with region` body are rejected (region
  release on early return is not yet emitted);
- string comparisons, `starts_with`/`ends_with`/`contains`/`char_at`, and
  owning string operations beyond fresh concat, transfer, indexing, and
  `string_free`;
- enums as struct fields and `match` with guard arms (payload-carrying
  enums and match on `Option`/`Result` with payload binding are supported);
- owning struct payloads in sums now lower through the aggregate move/drop
  machinery; owning structs with unsupported field contracts (tensors,
  non-scalar generic collections) remain deferred;
- ownership-bearing struct forms outside the supported owned string, owned
  slice, and owning sum field contract; nested owning structs lower through
  the aggregate move/drop machinery with recursive payload tracking, and
  `is_some`/`is_ok` on a member sum reads its tag in place without consuming
  the payload;
- dynamic `list[T]` values with non-scalar elements, tensors,
  `@parallel`, `@compute`; fixed value arrays `array[T, N]` and dynamic lists
  are supported only for scalar elements, with fixed-array bounds known by
  their canonical descriptor; `dict[string]T` is supported for scalar values
  with literal string keys;
- ownership-bearing generic structs, non-scalar generic collections, methods,
  closures, recursion on owned values, and generic functions whose type
  parameter is not a scalar value;
- explicit integer casts and implicit widening between integer widths, and
  mixed-type numeric expressions without literal coercion; `u8` arithmetic is
  supported with byte-width wraparound semantics;
- writable or lifetime-polymorphic borrowed views stored in aggregate fields,
  and ownership-bearing field forms outside the supported owned string, owned
  slice, owning sum, and readonly generic-view contracts; dynamic lists with
  non-scalar elements. Generic view returns are supported only for one scalar
  source view parameter. Arbitrary lifetime-polymorphic returns, view fields
  with writable or nested ownership, and owner-preserving generic return forms
  remain deferred.

## 13.5.1 Dynamic buffers

The isolated backend represents a `list[T]` as an owned growable
pointer-plus-length-capacity value. The canonical type remains `list[T]`, and
scalar and value-only struct elements are admitted in this lane; buffers of
owning structs stay rejected. The evaluator uses a frame local byte arena,
while the native production representation remains outside this isolated
contract.

- `buffer_alloc` creates a zero-initialized buffer with the requested logical
  length and an initial capacity of at least eight elements;
- `buffer_append` allocates a larger backing range, copies the live prefix,
  writes the new element, and consumes the old allocation, so aliases to the
  old owner become invalid;
- `buffer_pop` reads the last element and decreases logical length without
  changing capacity; empty buffers fail at evaluation;
- indexed reads and writes use the same typed pointer and bounds checks as
  slices, while `len` reads logical length rather than capacity;
- calls and returns move the owner through the verified owned-slice contract;
  explicit `free` destroys an unborrowed live buffer, and frame teardown
  releases any remaining frame-local allocation;
- the verifier rejects wrong element descriptors, invalid ownership metadata,
  append to dead or borrowed storage, double free, and ambiguous lifetime
  states.

Scalar generic list specializations reuse the same buffer representation and
ownership transfer rules as concrete scalar lists. Lists with non-scalar
elements, region-backed dynamic buffers, and native ownership transfer remain
deferred.

## 13.5.2 String-keyed scalar dicts

The isolated backend represents a `dict[string]T` as an owned
pointer-plus-length view value, matching the production two-part dict ABI.
Only scalar value types are admitted; dicts of owning or view-bearing values
stay rejected until their growth and destruction contracts are defined.
Keys are string literals carried in instruction metadata, so no key value
flows through SSA or the native ABI.

- `dict_alloc` reserves a fresh allocation identity and zeroes the view;
- `dict_set` inserts or updates a literal key, growing the table at a 0.7
  load factor; the old allocation is consumed and the result owns the new
  table, so aliases to the old owner become invalid;
- `dict_get` returns the stored value or the provided fallback; `dict_has`
  reports presence; `dict_pop` removes an entry and returns its value or the
  fallback; `dict_delete` removes an entry;
- `dict_len` reports the live entry count;
- `dict_free` destroys a live unborrowed dict; frame teardown releases any
  remaining owned dict storage;
- the verifier rejects non-literal keys, invalid ownership metadata, set or
  delete on dead or borrowed storage, double free, use after free, and
  ambiguous lifetime states.

Source-level dict literals, index reads (`d["key"]`) and writes
(`d["key"] = value`), `get`/`has`/`set`/`delete`/`pop`/`len`/`free`
builtins, dict parameters passed by ownership transfer, and rehash growth
lower end to end through HIR, SSA, verification, evaluation, MIR, and both
native emitters. The native emitters call the production `cobra_dict_*`
runtime, which is linked into native test binaries exactly like the direct
emitter's dict support. Region-backed dicts and dicts with owning values
remain deferred.

## 13.6 Sum values: Option and Result

`Option[T]` and `Result[T, E]` are canonical tagged values. The layout is the
one finalized in the shared type arena (`type.c`), so the backend never
reconstructs it:

- Scalar `Option[T]`: size 16, alignment 8. Tag at offset 0 (i64), scalar
  payload `T` at offset 8. Tag 1 means `some`, tag 0 means `none`.
- Scalar `Result[T, E]`: size 24, alignment 8. Tag at offset 0 (i64), scalar
  payload `T` at offset 8, error `E` at offset 16. Tag 1 means `ok`, tag 0
  means `err`.
- An owned string or owned slice payload occupies its canonical 16-byte
  pointer-plus-length descriptor, so the enclosing sum uses that component
  width instead of the scalar payload slot. The active descriptor is moved,
  never copied as an independent owner.
- ABI is sum-indirect (`COBRA_ABI_SUM_INDIRECT`): sums travel as pointers to
  caller-owned storage. Scalar sums use the aggregate-copy machinery;
  ownership-bearing sums use explicit move and drop operations instead.

The sum lane accepts memory scalars (`i64`, `bool`, `f32`, `u8`), nested
scalar sums, value-only structs (`Option[Point]`), and owning structs
(`Option[Owning]`) as components. Value-only struct components travel through
the canonical aggregate-copy machinery: construction byte-copies the struct
into the component field, `unwrap` copies it into a private temp slot, and
struct components are passed through calls and returns like scalar-struct
values. Owning struct components use `agg_move`/`agg_drop` instead of raw
copies so their payload handles move, never duplicate. Structs with
unsupported field contracts (tensors, non-scalar generic collections) remain
rejected at the HIR boundary. Direct owned string and owned-slice payloads use
the separate operations in section 13.6.1.
Construction writes the tag and component into a sum slot; `is_some`/`is_ok`
read the tag; `unwrap` reads a component after a runtime tag check.

Source forms lowered today:

- `some(x)`, `none()` constructing `Option[T]`;
- `ok(x)`, `err(e)` constructing `Result[T, E]`;
- `is_some(o)`, `is_ok(r)` yielding `bool`;
- `unwrap(o)`, `unwrap_ok(r)`, `unwrap_err(r)` yielding the component, with a
  runtime failure on the wrong tag (`unwrap` on `none`, `unwrap_ok` on `err`,
  `unwrap_err` on `ok`);
- sum locals, sum parameters, and sum returns through private copies and
  caller-provided return storage, mirroring the scalar-struct call ABI;
- direct owning string and owned-slice payloads in Option/Result with explicit
  payload move, extraction, transfer, and `free(sum)` destruction;
- sum construction as an assignment initializer, a call argument, and a
  return value.

Lowering notes:

- A sum slot is a `stack_slot` with the canonical width; construction lowers
  to `field_addr`/`store` pairs for the tag and the chosen component, and the
  unused component area is left zeroed by the slot allocator.
- Component reads lower to `field_addr` plus a typed `load`. `is_some`/`is_ok`
  compare the loaded tag against 1.
- `unwrap`/`unwrap_ok`/`unwrap_err` lower to a `sum_check` instruction on the
  loaded tag; the evaluator fails with the variant-specific message when the
  tag does not match.
- Scalar sum copies, parameters, and returns reuse `agg_copy` with the
  canonical sum width. Ownership-bearing sums use `sum_move`,
  `sum_payload_store`, `sum_payload_load`, and `sum_drop`, with no raw
  aggregate copy.

Nested scalar sums are supported. A component may itself be an `Option` or
`Result` with scalar components, to arbitrary depth. The canonical layout
(`type.c`) gives aggregate components their real size inside the enclosing
sum: `Option[Option[i64]]` is 24 bytes (tag 8 + inner 16), and
`Result[Option[i64], i64]` is 32 bytes. Construction and access treat a
nested component as an aggregate: `some(inner)` byte-copies the inner sum
into the component field, and `unwrap` on a nested payload copies the field
into a private temp slot before returning it. Type completion recurses, so
`some(none)` resolves `Option[Option[T]]` from the declared or parameter
type outward.

Owning struct payloads (`Option[Owning]` where `Owning` carries owned
strings, slices, or owning sums) now lower through the aggregate machinery:
construction byte-moves the struct into the component field with its owned
payload handles, `unwrap` moves it out into a private temp, and the source
sum's tag is cleared so a second extraction fails at the evaluator's
`sum_check`. Drop, call transfer, and return transfer walk the struct fields
recursively.

Owning structs may also nest directly inside owning structs. An owning
struct field carries its payload through `field_payload_store`/`field_payload_load`
regardless of depth, so `Mid.inner.text` resolves through a recursive
drop, move, and payload lookup. Bare owning struct parameters now carry an
allocation identity through call boundaries, and `is_some`/`is_ok` on a
member sum (`b.payload`) reads the tag in place so the predicate does not
consume the payload; the later `unwrap` still moves it out. Use-after-free,
double free, post-call use, and payload reads after a move on nested owning
structs are rejected by the flow analysis. Match on `Option`/`Result` with
payload binding arms (`case some(x)`/`case none`/`case ok(v)`/`case err(e)`)
now lowers through the same chain machinery as enum match, moving the
payload into the binding on the payload arms. Payload-carrying enums
(`enum Shape: { Circle(f32), Rect(i64, i64), Line }`) lower end to end:
variant payload type lists, tag-plus-payload aggregate layout, `Shape.Circle(2.5)`
construction, payload parameters and returns, and `case Shape.Circle(r)`
binding arms with exhaustiveness, duplicate, arity, and binding-count
rejections. Direct owning string and owned-slice payloads,
including nested owning sums, are covered by the owning-sum operations
below. `match` remains enum-only in the production validator and is not
lowered there. The legacy frontend validator still rejects nested owning sums
at IR validation; the new backend pipeline accepts them.

## 13.6.1 Owned sum payload contract

The owning-sum lane preserves the canonical indirect sum ABI. An `Option` or
`Result` with an owned string or owned slice payload stores the same tag
followed by the payload descriptor: pointer, length, allocation identity, and
ownership contract. The descriptor is not a scalar byte value.

Owning sums require explicit operations:

- `sum_payload_store` moves a fresh or owned payload into the active variant;
- `sum_payload_load` extracts the active owned payload without copying it;
- `sum_move` transfers the active payload and invalidates the source;
- `sum_drop` destroys the active owned payload at scope, branch, call, or
  return cleanup boundaries.

`agg_copy` is invalid for sums containing ownership-bearing payloads. Scalar
and nested scalar sums continue to use aggregate copies.The verifier and evaluator now prove and enforce one live owner per payload allocation, reject inactive payload access, transfer payloads through calls and returns, and make
`free(sum)` a variant-aware `sum_drop`. Direct owned string and owned-slice payloads are supported, including
nested owning sums. Ownership-bearing struct fields use the same explicit
recursive move/drop model. Struct payloads inside sums remain deferred.

Native lowering now uses the same pointer-plus-length ABI for supported owned
slice payloads. Sum and struct aggregates remain indirect through caller or
callee-owned storage, and native drop sequences inspect the active sum tag before
freeing an owned payload.

## 13.7 Borrowed and owned strings

A borrowed string is the readonly u8 view already in the memory model:
canonical `pointer[u8]` plus a byte count, with the borrowed-readonly
contract. Ordinary `string` parameters, literals, and forwarded returns use
this representation. A fresh concat result is an owned u8 slice with the same
pointer-plus-length payload and the owned-slice contract.


Source forms lowered today:

- string literals, materialized as a private frame allocation holding the
  literal bytes with a readonly view over it;
- `string` parameters, which import as readonly u8 views and flow as
  immutable SSA values like readonly slice parameters;
- string locals via `let s: string = "..."` (view-typed locals);
- `len(s)` yielding the byte count;
- `s[i]` yielding the byte at index `i` as `u8`;
- `slice_u8(s, start, length)` yielding a checked readonly subview;
- string arguments to `string` parameters at call boundaries;
- `concat(left, right)` and `left + right`, which allocate a fresh owned u8
  string and copy both source views;
- fresh string returns and `string_free` through the owned-slice transfer and
  destruction rules.

Lowering notes:

- A literal lowers to `slice_alloc` for the byte count, `view_ptr`, one typed
  store per byte, and `view_make` producing the readonly view. The backing
  allocation is private to the frame and freed with the frame arena.
- `string_concat` validates both readonly source views, allocates an owned u8
  slice, copies the canonical element slots, and returns the owned value.
- Owned string returns reuse allocation handoff across calls. A fresh result
  is copied into caller slice storage when it leaves the callee frame; a
  returned owned parameter keeps its original backing allocation.
- Bounds enforcement comes from the existing view pointer and load checks;
  lifetime, move, and borrow conflicts use the existing owned-slice dataflow.
- Comparisons, predicates, and mutation remain outside this lane.

Native string concatenation, explicit `string_free`, region cleanup, and
cleanup-safe scalar and owned-view returns are implemented in the isolated x86-64
emitters. Supported owning sum payloads and ownership-bearing struct fields now
use the same move and drop operations; arbitrary non-scalar aggregate ownership
remains deferred.

## 13.8 Unit enums and match

A unit enum is a compile-time integer discriminant set. The backend imports
`enum Name: { A, B = 3, ... }` from the canonical `COBRA_TYPE_ENUM`
descriptor, which is an 8-byte scalar with GPR ABI in the frontend layout.
The enum carries no payload, no ownership, and no destruction in this lane:
it is a pure value type, like an i64 with a fixed domain.

Representation:

- one canonical `COBRA_TYPE_ENUM` descriptor per enum in the backend type
  arena, plus a per-module variant registry mapping variant names to their
  integer discriminants;
- enum values flow as `BIR_SCALAR_I64` payloads carrying the enum type, so
  constants, locals, parameters, returns, and comparisons reuse the scalar
  machinery unchanged;
- enum-typed locals are ordinary scalar stack slots; the verifier and
  evaluator classify enums as memory scalars (kind i64, width 8, align 8).

Source forms lowered today:

- `enum` declarations with implicit or explicit (`= N`) discriminants;
- `let c: Color = Color.Green` and implicit `c = Color.Green` assignments;
- `Color.Variant` references, resolved to enum constants at build time;
- enum parameters and returns through the scalar call ABI;
- enum comparisons (`==`, `!=`, and the ordering operators on
  discriminants), which produce bool;
- `match value: { case Color.A: { ... } else: { ... } }`.

Match lowering:

- The target expression is evaluated exactly once into a synthetic local,
  then lowered to an if/else-if chain of `EQ` comparisons against each
  variant discriminant, with the `else` arm (or the merge block when the
  match is exhaustive) as the fallback. Each arm body is its own HIR block
  that falls through to the merge unless it returns.
- The builder rejects, mirroring the frontend IR validation: matches on
  non-enum values, arms from a different enum, unknown variants, duplicate
  arms, more than one `else` arm, and non-exhaustive matches without an
  `else` arm.
- Match inside loops and nested control flow works through the same block
  machinery as `if`; the target local is frame-lifetime, so reassigning the
  matched variable inside an arm does not disturb the ongoing comparison.

Explicitly not in this lane: enums as struct fields, `match` with guard
arms, and enum destruction or ownership. Payload-carrying enums are covered
separately in section 13.8.2.

## 13.8.1 Match on Option/Result sums

`match` also accepts an `Option` or `Result` target with payload patterns:

- `match o: { case some(x): { ... } case none: { ... } }` for `Option`;
- `match r: { case ok(v): { ... } case err(e): { ... } }` for `Result`;
- an `else` arm covers any pattern omitted from the two-arm form.

The pattern keywords are `some`/`none` for an `Option` and `ok`/`err` for a
`Result`; a pattern of the wrong kind is rejected. `some`, `ok`, and `err`
require a payload binding (`case some(x)`), while `none` must not bind.
Duplicate patterns, more than one `else`, and non-exhaustive matches without
an `else` arm are rejected, mirroring enum match.

Lowering:

- The target is evaluated once into a synthetic local, then lowered to the
  same if/else-if chain as enum match, with each arm branching on the tag
  (`some`/`ok` expect tag 1, `none`/`err` expect tag 0) through the sum tag
  predicate.
- A payload arm binds the component by extracting it from the synthetic
  local with the checked sum-access path: scalar payloads load the component
  field, and aggregate or owning payloads (structs, nested sums, owned
  strings) move out through `agg_move`/`sum_move` with the source tag
  cleared. The binding behaves like `unwrap`: the payload is owned by the
  binding and must be freed or consumed in the arm. After the match the
  synthetic local is empty on every path that extracted, so a later `free`
  of the original sum is a no-op drop.
- Scalar, owning-struct, and nested-sum payloads all lower through the
  existing sum-access and flow machinery; no new opcodes are introduced.

## 13.8.2 Payload-carrying enums

`enum Shape: { Circle(f32), Rect(i64, i64), Line }` declares an enum whose
variants may carry payloads. The backend imports it as a
`COBRA_TYPE_ENUM` aggregate whose variants are tag-plus-payload components:

Representation:

- the canonical enum descriptor carries per-variant payload type lists; a
  unit variant has no payload;
- the aggregate layout is a resident tag word followed by fixed slots for
  the union of all variant payloads, sized by the largest variant. Payload
  slots are 8-byte aligned scalar slots for scalars and synthesized
  structs for multi-field payloads, so any variant's payload fits without
  reallocation;
- enum values are memory aggregates (not scalars): construction writes the
  variant tag and payload slots through the SUM_MAKE aggregate path, and
  parameters and returns flow through the aggregate call ABI.

Source forms lowered today:

- `enum` declarations with payload type lists on variants;
- construction `Shape.Circle(2.5)` and `Shape.Rect(1, 2)` (payload arity
  checked against the variant's declared type list); unit variants still
  use `Shape.Line`;
- enum locals, parameters, and returns through aggregate copies and the
  aggregate call ABI;
- `match s: { case Shape.Circle(r): { ... } ... }` with payload bindings:
  each arm branches on the tag and binds the variant payload through the
  checked sum-access lane (the arm binding list length must equal the
  variant's declared payload arity);
- exhaustive, duplicate, unknown-variant, wrong-binding-count, arity, and
  non-exhaustive rejections.

Construction and match lowering reuse the sum machinery: SUM_MAKE
materializes the tag-plus-payload aggregate, the tag predicate branches on
`sum_expected_tag`, and extraction reads the resident payload slot at the
variant offset. Owning payloads lower too: a declared single payload
(`A(P)`) imports directly (strings become owned u8 slices, slices become
owned slices, structs import through the aggregate lane, sums through the
sum lane) and is passed as one argument; a multi-field payload
(`A(i64, i64)`) synthesizes an inline struct and binds per field. The
evaluator resolves enum tags to variant selectors through the module's
variant registry, so owning payload stores, extraction, call transfer, and
destruction work for any variant count. Double-free and use-after-free on
extracted owning payloads are rejected by the flow analysis.

## 13.9 Scalar generic functions

The first generic backend contract is deliberate monomorphization, not runtime
polymorphism. A source function with exactly one type parameter is retained as
a parser template. Each concrete call site supplies a scalar argument type and
causes one canonical specialization before HIR construction. The specialized
function receives a stable symbol such as `id__i64`, but its identity is the
canonical template plus substituted argument, not the generated name.

The specialization pass:

- infers the type from a scalar literal or a scalar local with canonical type
  metadata;
- substitutes the generic parameter through parameter, return, and expression
  descriptors using `cobra_type_substitute`;
- clones the AST body, rewrites the call to the concrete function, and reuses
  an existing specialization when the canonical argument is equal;
- registers and lowers only concrete functions. The template and any unresolved
  `COBRA_TYPE_GENERIC_PARAM` descriptor never enter HIR, SSA, verification, or
  evaluation.This lane supports scalar parameters and returns, arithmetic,
comparisons, recursive control flow, calls from ordinary functions, multiple scalar
specializations, module-local call boundaries, and scalar `list[T]`
collections with owned-buffer transfer. It rejects non-scalar collection or
aggregate arguments, multiple generic parameters, nested instantiation through
a generic template, and ownership-bearing generic values.

### Immutable scalar generic structs

A generic struct with exactly one scalar parameter may specialize when every
field becomes a value-owned scalar or an immutable nested scalar-only struct.
The prepass substitutes the canonical template once per concrete argument,
imports the finalized descriptor directly, and uses its concrete name only as a
symbol and aggregate lookup key. The verifier still relies on canonical field
sizes, offsets, and ABI metadata.

Supported forms include `Box[i64]` and `Box[f32]` parameters, locals, field
reads and writes, aggregate calls, aggregate returns through caller storage,
and repeated specialization reuse. Strings, lists, sums, arrays, mutable
fields, and ownership-bearing fields remain outside this generic struct lane.

### Scalar generic collections

A generic function with one type parameter may specialize a `list[T]` when
`T` resolves to a supported scalar. The concrete list uses the owned buffer
layout, including length and capacity, and retains the same move contract as a
non-generic list. The specialization supports literal construction, indexed
reads, `len`, `append`, `pop`, ownership-moving calls and returns, explicit
`free`, and verifier rejection of leaked or duplicated owners. Non-scalar
collection elements, region-backed generic buffers, and multiple type
parameters remain deferred.

### Lifetime-aware generic view returns

Generic view return types are monomorphic in their element type but polymorphic in
which caller-owned allocation supplies the lifetime. The specialized function
metadata records `return_view_param`, the one source parameter from which the
returned view must derive, together with the readonly or writable return
contract. The parameter itself must be a matching borrowed view. At a call
boundary, an owned scalar slice or list may satisfy that borrowed parameter; the
SSA result is retagged as a borrow while preserving its allocation id, frame
origin, and region id. No owner is transferred by the returned view.

A return built from a local allocation, a region allocation, or a different
parameter is rejected. The verifier checks the specialized element descriptor,
mutability, pointer contract, source parameter index, and provenance. The
runtime evaluator repeats the frame, region, bounds, and allocation checks.
This gives generic helpers Rust-level lifetime behavior without exposing
lifetime annotation syntax in Cobra source.

### Generic borrowed-view fields

A specialized generic struct may contain one readonly borrowed view field,
such as `View[T] { data: readonly []T }`, when `T` becomes a supported scalar.
The backend imports the concrete pointer-plus-length field layout, lowers field
stores and loads through typed memory operations, copies the descriptor across
aggregate parameter and return storage, and retains the runtime source pointer
and region provenance in evaluator state. A view aggregate may be forwarded
from a parameter, but a locally created view aggregate cannot escape its frame.
Writable fields, non-scalar generic collections, multiple type parameters,
and ownership-bearing generic aggregates remain deferred.

## 13.11 Fixed-width scalar types: i32, u32, u64, f64

Four scalar kinds complete the numeric surface this lane needs before MIR
work: `i32`, `u32`, `u64`, and `f64`. They share the existing scalar
machinery (typed payloads, stack slots, loads/stores, calls, returns) with
these contracts:

- `i32`/`u32` are 32-bit fixed-width integers. Arithmetic is computed in
  unsigned domain and truncated to 32 bits, so overflow wraps; `u32`
  division and remainder are unsigned, `i32` division and remainder are
  signed (truncating toward zero). Comparisons use the signed (i32) or
  unsigned (u32) domain. Locals occupy an 8-byte canonical slot like every
  other scalar, matching the frontend layout.
- `u64` is the full unsigned 64-bit integer. A literal whose magnitude
  exceeds `INT64_MAX` (for example `18446744073709551615`) parses as an
  unsigned literal and defaults to `u64`; coercing it to a signed or
  narrower context is a range error. Negative literals stay signed.
- `f64` is IEEE-754 double precision. Literals keep their exact double value
  through the HIR and are completed against the boundary context: a float
  literal in an `f64` declaration, return, call argument, member store, or
  sum payload is `f64`; untyped float arithmetic (`1.5 + 2.25`) defaults to
  `f32` unless the whole pure-literal expression sits in an `f64` context,
  in which case both literals widen without an f32 round-trip. The evaluator
  stores f64 by bit pattern and dumps preserve exact bits.

Literal and coercion rules:

- integer literals default to `i64` (or `u64` for unsigned magnitudes) and
  narrow to `i32`/`u32`/`u8` with range checks at every boundary:
  declarations, assignments, returns, call arguments, binop operands,
  member stores, writable-view stores, and sum payloads;
- float literals complete to `f32`/`f64` at the same boundaries;
- there are no implicit conversions between distinct scalar types: `i64` to
  `u64`, `f32` to `f64`, or mixed-width arithmetic without a literal operand
  are type errors. This keeps the verifier and evaluator type-exact.

Not in this lane: explicit casts or widening operators, and `f64` in the
production direct emitter. The legacy validator still rejects `f64`, while the
isolated native lane lowers and executes it.

## 13.10 ABI-neutral call model

The isolated backend now materializes a `BirCallAbi` record for every function
signature. The record is owned by the canonical backend function table and is
the only call contract consumed by verification and evaluation. It is not
reconstructed from AST fields and it does not contain physical register names.

Each lowered parameter, including hidden caller-provided aggregate return
storage, has one or more `BirAbiLocation` parts. A part records direct or
indirect passing, GPR or XMM register class, an abstract register ordinal or
stack offset, byte width, and alignment. The return record uses the same
representation. A view has two GPR parts, floating scalars use XMM, integer
and enum scalars use GPR, and structs and sums use one indirect pointer part.

The initial deterministic profile has six GPR argument positions, eight XMM
argument positions, and 16-byte stack alignment. Once a register class is
exhausted, later parts are placed in aligned stack slots. The stack footprint
is rounded to the required alignment. Aggregate returns have an indirect
return record and a hidden first caller-storage parameter, so they never
produce an SSA call result.

The verifier checks that the stored location record exactly matches the
canonical signature and rejects inconsistent metadata. It also checks every
call's lowered argument count and pointer contract against the callee record.
The evaluator validates the same record before entering a callee.The current SSA evaluator continues to pass typed values directly. MIR now maps SSA values
to virtual registers and preserves abstract ABI locations, memory metadata,
clobber sets, CFG edges, and source locations. A later target pass maps these
abstract locations to physical registers, stack frames, and a concrete calling
convention.

## 14. Target-independent MIR

MIR is constructed only after the complete SSA verifier succeeds. It has its
own arena and does not retain AST nodes or legacy codegen state. Every source
function becomes a MIR function with the same block range and call ABI.

MIR values are virtual registers. Each register retains its canonical Cobra
type, machine type, source SSA value for diagnostics, source location, and
pointer ownership metadata. Machine types distinguish signed and unsigned
integer widths, booleans, floating widths, addresses, views, and aggregates.

MIR instructions preserve the verified SSA operations needed by the current
lane:

- arithmetic, comparisons, constants, and virtual-register ABI moves;
- typed stack slots, pointer arithmetic, field addresses, loads, stores, and
  aggregate copies;
- region enter and exit, transfer, destruction, view operations, slice
  allocation and release, and sum checks;
- calls with callee identity, verified argument count, ABI locations, and a
  conservative abstract clobber set;
- branches, jumps, block parameters, ABI-aware returns, and source locations.

The lowering is deterministic. SSA constants become entry-block MIR constant
instructions, function parameters become ABI moves, SSA block parameters become
MIR block parameters, and CFG edge arguments are copied into MIR edge pools.
The MIR verifier checks function ownership, type and machine-type consistency,
operand windows, result definitions, terminators, CFG edge arity, call ABI
identity, memory effects, clobber metadata, and virtual-register dominance.

MIR is still target independent. The isolated backend now has a separate
Linux x86-64 assembly emitter that consumes MIR, but MIR itself does not select
instructions, assign physical registers, or emit assembly. Those remain later
target and allocation passes.

## 15. Isolated Linux x86-64 emitter

`bir_x86_64_emit` consumes only verified MIR and writes GNU AT&T assembly. It
is not linked into the production `cobra` binary and does not replace the
existing direct emitter.

The bootstrap emitter supports:

- scalar integer, boolean, f32, and f64 arithmetic and comparisons;
- value-owned scalar-field structs with canonical stack slots, field addresses,
  aggregate copies, indirect parameters, and caller-provided returns;
- deterministic stack frames with one spill slot per virtual register;
- scalar and scalar-field aggregate stack slots, pointer arithmetic, field
  addresses, loads, and stores;
- conditional branches, jumps, scalar and indirect aggregate calls, borrowed
  readonly view calls and returns, scalar returns, aggregate return copies, and
  ABI moves;
- readonly view construction, pointer extraction, length access, indexed loads,
  subview bounds checks, and caller-derived view return checks;
- the current abstract ABI mapped to Linux x86-64 System V argument registers,
  XMM registers, return registers, and aligned stack arguments;
- conservative call clobber handling because all virtual values remain spilled.

The original emitter uses a spill-all strategy as a correctness fallback.
The allocated emitter consumes linear-scan locations for the scalar and
scalar-field aggregate subset and uses a staged call area to avoid
argument-register cycles. The test harness assembles output with the system
toolchain and executes fixtures against expected evaluator and legacy-backend
results. The native matrix covers i32 wrapping, u32 and u64 arithmetic and
division, f32 and f64 arithmetic, byte-width `u8` arithmetic and comparisons,
loops, stack-backed locals, calls, and
allocated register paths. Separate native fixtures execute scalar-field
structs, borrowed readonly views, writable views, heap-backed owned slices,
regions, scalar dynamic `list[T]` buffers, fresh string concatenation, owning
Option/Result payloads, and ownership-bearing struct fields through both
emitters, including view indexing,
subviews, calls, returns, stores, growth, append, pop, cleanup, and bounds traps.
It is not a
performance implementation. Narrow owned-slice argument moves, descriptor
transfers, and explicit destruction are implemented. Native owning sum payload
moves and drops, ownership-bearing struct field moves and drops, and indirect
aggregate calls and returns are implemented for the supported layouts.
Arbitrary non-scalar aggregates remain outside this native
lane. Target-specific fixed constraints,
instruction selection improvements, frame optimization,
object output, and production backend selection remain separate milestones.

## 16. MIR linear-scan allocation

The isolated allocator consumes verified MIR and assigns virtual registers to
abstract GPR or XMM positions, or to function-local spill slots. Readonly view
values use two deterministic spill components so their pointer and length stay
paired across calls and returns. It computes
conservative intervals over the deterministic block order, includes branch
edge uses, and marks intervals that cross calls with the call's clobber mask.

The abstract GPR class has eleven positions: the six Linux x86-64 System V
caller-saved argument registers (indices 0-5) and five callee-saved registers,
rbx and r12-r15 (indices 6-10). Calls only clobber the caller-saved positions,
so an interval that crosses a call may still receive a register instead of
spilling; it lands in a callee-saved position once the caller-saved ones are
forbidden by the call's clobber mask. Address-valued registers follow the same
path and are no longer forced to spill purely to survive a call. Intervals
that cross opcodes emitting malloc/memcpy/free internally (slice and buffer
allocation and destruction, buffer append, string concatenation, owning-sum and
aggregate drops, destroy, and region cleanup) are still forced into spill
storage, since those opcodes clobber every register in their class.

Allocation verification rejects missing assignments, wrong register classes,
forbidden clobbered registers, overlapping live intervals sharing a register,
and invalid spill metadata. The allocation dump is deterministic and records
intervals, register positions, forbidden masks, and spill slots.

This is the first allocator, not the final machine allocator. The allocated
scalar x86-64 emitter now consumes its register and spill decisions, while the
original spill-all emitter remains available as a reference path. The
allocated emitter's prologue saves whichever callee-saved registers a
function's allocation actually used into dedicated frame slots, and every
return site restores them before `leave`/`ret`; the SysV ABI guarantees any
call inside the function already preserves them, so no extra clobber handling
is required at call sites. Future work includes target-specific fixed
constraints, interval holes, rematerialization, optimized stack-frame layout,
and allocator stress under native register pressure.
