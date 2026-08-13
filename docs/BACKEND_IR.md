# Backend IR Design

This document describes the first backend-IR foundation: a typed, flat,
arena-backed SSA representation with an explicit CFG, built as a separate,
isolated component. It is deliberately **not** wired into the existing
direct-to-assembly pipeline yet. The current `CobraIR` (src/ir.c) remains a
validation pass over the AST and is untouched; codegen continues to consume
the AST directly.

```
Cobra source
  -> tokens (lexer)
  -> syntax AST (parser)
  -> typed HIR / explicit CFG with mutable source locals   <-- new
  -> block-argument SSA                                    <-- new
  -> [verifier, textual dump, evaluator]                   <-- new
  -> existing IR validation / native codegen (unchanged)
```

## 1. Responsibilities: HIR versus SSA

Two layers exist for two different jobs.

**Typed HIR (CFG).** The HIR is the semantic CFG that a source program is
lowered into. It keeps *source-level mutable locals*: a local is a named
slot, assignments write slots, expressions read slots, and control flow is
explicit blocks with jumps, conditional branches, and returns. No value
versioning happens here. The HIR builder consumes the existing AST produced
by the parser and accepts a deliberately small subset:

- scalar (`i64`) locals via `let`/`var`/`const` and implicit `x = expr`;
- assignments to scalar locals;
- integer arithmetic and comparisons;
- `if`/`else`;
- `while` loops;
- `for` loops over a scalar bound (`for i in n`), over `range(a, b)`, and over
  constant array literals (`for x in [1, 2, 3]`);
- `return`, including multiple returns from one function;
- function parameters and calls between user functions.

The HIR builder never constructs SSA, and the parser is never asked to
produce SSA. Unsupported AST forms (slices, tensors, buffers, dictionaries,
`match`, regions, `@parallel`, `@compute`, generic declarations) are
rejected with an explicit builder diagnostic. This keeps the new lane
provably separate from the ownership/region/generic machinery that the
existing compiler already validates.

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

- **values** — one entry per SSA value: kind (function parameter, block
  parameter, constant, instruction result), canonical type, definition
  back-reference (defining instruction or parameter index), constant
  payload, source span;
- **instructions** — one entry per instruction: opcode, result type and
  result value, variable-length operand range, terminator targets and edge
  argument ranges, effect tag, callee name for calls, source span;
- **blocks** — one entry per basic block: ordered instruction list,
  terminator reference, block parameter list, predecessor and successor
  lists, source span;
- **operands** — one flat pool of `SsaValueRef`; each instruction stores a
  `(start, count)` window into it, so operand lists are variable length and
  never fixed at `args[3]`;
- **edge arguments** — one flat pool of `SsaValueRef`; terminators store up
  to two `(start, count)` windows (then-edge and else-edge) aligned with
  their successors' block parameters.

Handles stay valid for the lifetime of the arena; appends never move earlier
entries because pools are pointer-stable growable arrays.

## 3. Instruction set

The instruction set is intentionally minimal and scalar:

| Opcode      | Operands          | Result | Notes                            |
|-------------|-------------------|--------|----------------------------------|
| `const`     | —                 | i64    | constant materialization         |
| `param`     | —                 | i64    | function parameter (entry block) |
| `block_arg` | —                 | i64    | block parameter (join value)     |
| `add/sub/mul/div/rem` | 2        | i64    | integer arithmetic               |
| `neg`       | 1                 | i64    | unary minus                      |
| `eq/ne/lt/le/gt/ge` | 2        | i64    | comparisons yield 0 or 1         |
| `load`      | 1 (address)      | i64    | memory read (effect: read)       |
| `store`     | 2 (addr, value)  | none   | memory write (effect: write)     |
| `call`      | n (arguments)    | i64    | user function call (effect: call)|
| `jump`      | target + edge args | none | unconditional terminator         |
| `branch`    | cond + 2 targets + edge args | none | conditional terminator   |
| `return`    | 0 or 1           | none   | function terminator              |

`load`/`store` are present so the instruction set is complete for the
stated contract, but the HIR builder's scalar subset never produces them;
they are exercised by direct unit tests against a small flat memory model.
No alias analysis is performed (out of scope).

### Memory/effect semantics

Every instruction that touches memory carries an explicit effect tag:

- `SSA_EFFECT_NONE` — pure value computation;
- `SSA_EFFECT_READ` — `load`; reads memory but never mutates it;
- `SSA_EFFECT_WRITE` — `store`; mutates memory;
- `SSA_EFFECT_CALL` — `call`; the callee's effects are opaque to this
  backend lane.

The tags are metadata for future passes (reordering, common-subexpression
elimination, liveness). The verifier only requires that a `store` is not
followed in the same block by another `store` with identical operands —
trivial by construction today — and that effects are recorded. Full
alias/ordering analysis is explicitly out of scope for this milestone.

### Call ABI metadata

Calls carry the callee name and argument list; the module keeps a function
table mapping name to entry block, parameter count, and return type. The
subset ABI is the existing native scalar contract: every parameter and the
result are `i64`, one value per slot, matching the System V register
convention for the current Linux x86-64 target. The design deliberately does
not model stack spilling, vector ABI classes, or sret storage; those belong
to the ABI-lowering layer that will consume this IR later.

## 4. Source spans

Every value, instruction, and block records `(line, column)` copied from the
AST node that produced it; the module owns a single source-file name for the
program. Spans are informational today (used by the printer and diagnostics)
and will feed the structured diagnostic system later.

## 5. Typed HIR/CFG builder

The builder walks an `ASTNode` function body with an explicit environment:

- **locals** — a function-wide table mapping source name to a local index.
  Parameters occupy indices `0..param_count-1`. A `let`/`var`/`const` or an
  implicit assignment registers or updates a slot. Reading an unregistered
  name is a builder error (matching the host interpreter's failure on reads
  of unknown locals).
- **blocks** — created for straight-line sequences, `if`/`else` joins,
  `while` headers/latches/exits, and `for` loops. `for` over a scalar bound
  or `range(a, b)` is lowered to the same shape as `while`: an induction
  local, a header comparing against the bound, a body, and a latch that
  increments. `for` over a constant array literal is unrolled into
  straight-line copies of the body with the loop variable rebound per copy
  (bounded at 64 elements), which matches the host interpreter's
  per-iteration rebinding semantics exactly.
- **terminators** — `jump`, `branch(cond)`, and `return(expr)`, always the
  last element of a block.

The HIR keeps source-level mutable locals: assignments write local slots and
expressions read them; no versioning happens in the builder.

## 6. SSA construction pass

The pass consumes the HIR and produces block-argument SSA in three steps:

1. **Linearization** — each block's statements are processed in order.
   Expression trees evaluate to SSA values: constants become `const`
   instructions, binary operations emit their opcode, calls emit `call`.
   Assignments record the local's current SSA value. This is straight-line
   code, so ordering inside a block is trivially correct.

2. **Live-in sets** — a backward dataflow fixpoint over the CFG computes,
   for every block, the set of locals whose value must be available at block
   entry:
   `live_in(B) = reads_before_assign(B) ∪ { L in live_in(S) : B never assigns L }`
   over all successors `S`. `reads_before_assign` is the set of locals read
   by any statement or terminator before the block's own last assignment to
   them. Because each block is straight-line, this is precise.

3. **Block parameters and edge arguments** — the parameters of block `B`
   are exactly `live_in(B)`, ordered by local index for determinism. For
   every edge `P -> B`, the edge arguments are `P`'s exit values of `B`'s
   parameters: `P`'s last assignment to the local if it assigns it, else
   `P`'s own block parameter for that local (the value `P` received). The
   recursion bottoms out at function parameters and constants, so every
   parameter used on a path has exactly one defining edge value. Reading a
   local that is never defined on some path is rejected at build time.

This is a sealed-block construction without phi nodes: no placeholder
values, no late operand filling, and a deterministic result that the
verifier can check edge-for-edge.

## 7. Verifier invariants

`backend_ir_verify` checks, for every module:

- **one definition per value** — every value has exactly one defining
  entity (one instruction, one parameter index, or one constant payload);
  duplicate or dangling definitions are rejected;
- **valid operands** — every operand handle is in range and not the invalid
  handle; arity matches the opcode (binary ops take exactly two operands,
  `load` one, `store` two, `call` exactly its function's parameter count);
- **edge-argument arity** — every edge's argument list has exactly the
  successor's block-parameter count, and each argument is a valid value;
- **terminators** — every block has exactly one terminator, the terminator
  is the last instruction in its block, and no instruction appears after a
  terminator;
- **dominance and use ordering** — a dominator tree is computed with the
  standard iterative algorithm; every use of a value is dominated by its
  definition; a use inside the defining block appears strictly after the
  definition (block parameters, function parameters, and constants are
  defined at block entry). Unreachable blocks are legal; their contents are
  checked structurally (operands valid, edge arity, terminators) but their
  internal dominance is not enforced, since no execution path reaches them;
- **no unresolved generics** — no value, instruction, or block carries a
  type that recursively contains a `GENERIC_PARAM` descriptor;
- **canonical finalized types** — every type attached to a value is a
  non-NULL, canonical, finalized `CobraType`.

## 8. Textual dump

A deterministic printer emits the module in a stable form: functions and
blocks in creation order, values in definition order, operands in order,
types printed from the canonical descriptor names. Example:

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

## 9. Evaluator

A small interpreter executes the SSA form directly:

- one value-slot array per call frame indexed by `SsaValueRef`, so recursive
  and re-entrant calls cannot clobber their callers' locals or parameters;
- a flat memory of 8 KiB `i64` slots backing `load`/`store`;
- a call stack of return targets with depth and step limits, so malformed
  IR cannot hang the host;
- function parameters are bound from call arguments (and from zero at the
  entry function, matching the host interpreter's convention), block
  parameters are bound from edge arguments at block entry.

The evaluator is the reference execution semantics for the new lane and the
basis for differential tests against the existing host interpreter.

## 10. Unit and differential tests

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
- `load`/`store` round-trips;
- generic-parameter and non-finalized-type rejection by the verifier.

Differential tests parse the same Cobra source with the real parser, run
each function through the host interpreter (`interpreter_run_function`) and
through the new SSA evaluator, and require identical results. Supported
differential programs stay inside the intersection of the two engines'
subsets (scalar locals, arithmetic, comparisons, `if`/`else`, `while`,
`for` over constant arrays, calls, recursion, `return`).

## 11. Explicit non-goals for this milestone

- No register allocation, instruction selection, object/binary writers, JIT,
  or new targets.
- No replacement of the existing direct emitter or the existing `CobraIR`
  validation pass.
- No weakening of ownership/region/lifetime rules; memory operations only
  carry effect metadata, and no alias analysis is performed.
- No new parser syntax and no new generic lanes.

The new module lives under `src/backend_ir/` and is compiled and tested on
its own; it is not linked into the production `cobra` binary yet.
