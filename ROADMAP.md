# Cobra Roadmap

This document tracks the current state of the compiler and planned work. Items are grouped by time horizon. Status reflects the state of the `main` branch.

---

## Near-term (v1.x)

These are concrete gaps visible in the current codebase. Each item corresponds to something a contributor can pick up without understanding the full pipeline.

| Item | Owner | Status |
| :--- | :--- | :--- |
| Win64 x86_64 backend | unassigned | [ ] Stub only. `codegen.c` emits a placeholder; no ABI-correct prologue/epilogue or `rcx/rdx/r8/r9` argument mapping. |
| ARM64 (macOS Apple Silicon) backend | unassigned | [ ] Stub only. `TARGET_MACOS_ARM64` is accepted by the CLI but codegen does not emit valid AArch64 instructions. |
| Wasm32 backend | unassigned | [ ] Stub only. `TARGET_WASM32` produces `.wat` extension output but the emitter is not implemented. |
| Modulo operator (`%`) | unassigned | [ ] `TOKEN_PERCENT` is absent from `cobra.h`. Neither the lexer nor the parser handle `%`. |
| String mutation | unassigned | [ ] Concatenation, comparison, and predicates (`starts_with`, `ends_with`, `contains`, `char_at`) ship as native operations; in-place string mutation and slicing are still open. |
| Struct methods | unassigned | [ ] Struct declarations with real field access ship (see `examples/50_type_foundation.cb`); methods declared on a struct type are still open. Free functions remain the option. |
| `[]f32` slice view (language surface) | shipped | [x] `[]f32` typed slice parameters, writable indexing, and auto-vectorized loops ship; `lib/nn.cb` is written entirely against them. |
| Tensor parameters in function ABI | shipped | [x] `tensor[...]f32` parameters and returns flow through zero-copy stack-resident descriptors on the Linux x86-64 ABI (see `examples/30_abi_contracts.cb`). |
| `cobra fmt` implementation | unassigned | [ ] The `fmt` command parses the file and prints a confirmation message but does not reformat the source. |
| `--explain-asm` flag | unassigned | [ ] Described in `ideas.md`. Would embed original Cobra source lines as comments in the generated `.s` file. Parser line tracking is already present in `Token.line`. |
| Negative allocation count rejection on ARM64/Win64 | unassigned | [ ] The validation pass (`src/ir.c`) enforces this on the Linux path. It is not tested on cross-compile targets because execution is not yet possible. |

---

## Mid-term

These items require design work before implementation begins.

### Module system

The current compiler concatenates `lib/std.cb` and `lib/nn.cb` ahead of the user source on every compilation. A proper module system would replace this with explicit `import` syntax that is already partially parsed (`AST_IMPORT_DECL`, `TOKEN_IMPORT`) but has no codegen or file resolution logic.

Design questions to resolve before implementation:
- Search path resolution order (working directory, `COBRA_LIB_PATH`, installed prefix).
- Whether modules compile to separate object files or are always inlined.
- Visibility rules for symbols across module boundaries.

### Language server (LSP)

A basic LSP server would need at minimum: hover type information, go-to-definition, and diagnostic publishing. The compiler's single-pass architecture makes incremental parsing non-trivial. A practical starting point is a batch-mode LSP that re-parses the full file on every `textDocument/didChange` notification.

### VS Code syntax extension

A TextMate grammar for `.cb` files. The keyword list is stable and small enough to enumerate directly from the `TokenType` enum in `cobra.h`. This does not require touching the compiler.

### `@parallel` runtime portability

`runtime/cobra_parallel.c` is a pthreads implementation. The `@parallel` block is Linux-only today because the runtime path is resolved at link time. A Win32 threads backend and a build-time selection mechanism are needed before Win64 execution is viable.

### Visual diagnostic errors

`ideas.md` describes ASCII diagrams of stack frames and ownership graphs on validation failures. The IR pass in `src/ir.c` already tracks ownership state. The missing piece is a renderer that maps IR state to a human-readable layout.

### `bench` command: multi-target support

`cobra bench` currently requires `TARGET_LINUX_X86_64` and exits with an error for other targets. Once Win64 or ARM64 execution is implemented, the benchmark harness should generalize to those platforms.

---

## Long-term

These items are speculative and would require significant research or infrastructure before they become actionable.

### Neural compiler engine (LM/CM mode)

`future-experiments.md` describes a compilation path that replaces the traditional lexer/parser/codegen pipeline with a specialized model trained on intent-to-assembly pairs. The model would accept a `.lang` specification file and emit x86_64 or ARM assembly directly, with a formal verifier (SMT solver or assembly validator) as a correctness backstop.

Cobra's architecture is suited to this experiment because the `@compute` tagging and scope-arena memory model provide clean mathematical invariants that a model can learn to target. The `ASTNode` structure could serve as an intermediate representation between the model output and existing codegen infrastructure.

This is a research direction, not a scheduled feature. Correctness guarantees are the blocking problem: a traditional compiler bug is deterministic and fixable; a model that hallucinates a register overwrite produces silent memory corruption.

### Universal cross-platform native execution

Today, only Linux x86_64 produces executables. Full Win64, ARM64/macOS, and Wasm32 execution requires:
- Correct ABI implementations for each platform in `src/codegen.c`.
- A cross-linker or platform-native assembler invocation in `src/main.c`.
- CI runners for each target (GitHub Actions has `windows-latest` and `macos-latest`).

### Colorless concurrency

`ideas.md` describes transparent non-blocking I/O using `io_uring` on Linux, `IOCP` on Windows, and `kqueue` on macOS, emitted directly in assembly without `async/await` syntax. This would require new AST node types, new IR validation rules, and significant codegen work for each platform.

### Lexical scope graph lifetime checker

The design in `ideas.md` tracks single-mutable-writer / multi-immutable-reader rules using lexical scope graphs without annotation syntax. The current IR pass (`src/ir.c`) enforces ownership at the allocation/free level. A full lifetime checker would operate at the pointer alias level.
