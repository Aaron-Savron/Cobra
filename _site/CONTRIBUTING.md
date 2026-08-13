# Contributing to Cobra

Thank you for your interest in contributing. This document covers everything you need to build, test, and submit changes to the Cobra compiler.

---

## Prerequisites

| Tool | Minimum version | Notes |
| :--- | :--- | :--- |
| `gcc` | 9.x | Must support C99 and `-no-pie` |
| `make` | 3.81 | GNU Make |
| `git` | 2.x | |
| `curl` | any | Required only for `cobra update` |

The build produces a Linux x86_64 ELF binary. CI runs on `ubuntu-latest`. macOS with `gcc` installed via Homebrew works for development builds, but the native execution target and test runner are Linux-only.

---

## Building from Source

```bash
git clone https://github.com/Aaron-Savron/Cobra.git
cd Cobra
make
```

This compiles all eight source files and links them into a single `cobra` binary in the repository root. There are no third-party dependencies.

To clean build artifacts:

```bash
make clean
```

To build and package a release archive:

```bash
make dist
```

The dist target strips the binary and produces `cobra-v1.0.0-linux-x86_64.tar.gz` in the repository root, with unpacked files under `dist/`.

---

## Source File Ownership

The compiler is split across seven C files. Each owns a well-defined phase of the pipeline.

| File | Responsibility |
| :--- | :--- |
| `src/lexer.c` | Single-pass tokenizer. Converts raw `.cb` source text into a `Token` stream. |
| `src/parser.c` | Recursive-descent parser. Consumes the token stream and builds the `ASTNode` tree. |
| `src/ast.c` | AST node allocator, child list management, printer, and `ast_free`. |
| `src/interpreter.c` | Sandboxed `@comptime` evaluator. Runs pure Cobra expressions at compile time. |
| `src/ir.c` | Ownership and type validation pass (`cobra_ir_build`). Rejects use-after-free, double-free, borrow violations, and type mismatches before codegen. |
| `src/codegen.c` | Assembly emitter. Produces GNU Intel-syntax `.s` output for the Linux x86_64, Win64, ARM64, and Wasm32 targets. |
| `src/main.c` | CLI driver. Implements `build`, `run`, `test`, `bench`, `fmt`, `repl`, `emit-asm`, and `update`. |

All shared types and function prototypes live in `include/cobra.h`. Do not split declarations across other header files.

---

## Running Tests

The primary regression test is:

```bash
./cobra test examples/14_stress_test.cb
```

This compiles all `test_*` functions in the file to native x86_64 assembly, links them with a generated POSIX process supervisor, and reports PASS/FAIL per function. It requires AVX2 support on the host CPU because several tests exercise `@compute` blocks.

Other useful test files:

```bash
./cobra test examples/16_correctness_tests.cb
./cobra test examples/18_typed_slices.cb
./cobra test examples/29_tensor_views.cb
```

The CI pipeline (`.github/workflows/ci.yml`) runs the native example matrix, negative diagnostics, strict C validation, and release packaging on every push and pull request.

### Test Philosophy

Every language feature must have a corresponding `.cb` test file in `examples/`. A test function must:

- Be named with the `test_` prefix.
- Take no parameters.
- Return an integer: `0` for pass, non-zero for fail.

The test runner isolates each `test_*` function in its own forked process with a 5-second wall-clock timeout. A crash, non-zero exit, or timeout counts as a failure.

If you add a new operator, type, intrinsic, or language construct, add a `test_` function that exercises it before opening a pull request.

---

## Code Conventions

- **Indent with 4 spaces.** No tabs anywhere.
- **C99 only.** Do not use C11 atomics, VLAs, or GCC extensions unless guarded by an `#if defined(__GNUC__)` check.
- **No `//` comments in C files.** Use `/* ... */` block comments for consistency with the existing style.
- **Error output goes to `stderr`.** Use `fprintf(stderr, ...)` for all diagnostic and error messages.
- **Prefix internal functions with `static`.** Nothing should leak internal linkage into the object file unless declared in `cobra.h`.
- **Keep identifiers under 64 characters.** This matches `COBRA_MAX_IDENT_LEN`.
- **Do not add new `.h` files.** All shared declarations belong in `include/cobra.h`.

---

## Adding a New Example

1. Create `examples/NN_description.cb`, where `NN` is the next available two-digit number.
2. Include at least one `test_` function that exercises the feature.
3. Verify it passes the test runner:
   ```bash
   ./cobra test examples/NN_description.cb
   ```
4. Verify it runs end-to-end:
   ```bash
   ./cobra run examples/NN_description.cb
   ```
5. Reference the new file in your pull request description.

---

## Opening a Pull Request

1. Fork the repository and create a branch from `main`.
2. Branch names should follow the pattern `<type>/<short-description>`:
   - `fix/double-free-in-ir-validation`
   - `feat/modulo-operator`
   - `docs/update-contributing`
   - `test/tensor-view-edge-cases`
3. Keep commits focused. One logical change per commit.
4. Run the full test suite before pushing:
   ```bash
   make clean && make && ./cobra test examples/14_stress_test.cb
   ```
5. The CI pipeline runs automatically on pull requests. Do not merge until it is green.
6. Describe what the change does, why it is needed, and which example file demonstrates it.

---

## Assembly Output Conventions

Generated assembly follows GNU Intel syntax (`.intel_syntax noprefix`). When adding codegen logic to `src/codegen.c`:

- Emit a blank line between logical sections.
- Do not hand-optimize register allocation without a comment explaining the intent.
- Cross-target stubs for Win64, ARM64, and Wasm32 live in the same `codegen.c` file, guarded by a `TargetPlatform` switch. Keep stub functions returning `false` with a `fprintf(stderr, ...)` diagnostic until the backend is implemented.
