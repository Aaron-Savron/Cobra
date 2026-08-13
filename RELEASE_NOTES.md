# Cobra v1.0.0

Cobra v1.0.0 is a Linux x86_64 development release for direct native code generation.

## Included

- Python comfort syntax for everyday programs
- Direct Intel-syntax assembly emission
- Scope cleanup and explicit region arenas
- Lists, dictionaries, strings, slices, and typed results
- AVX2 and FMA paths for numeric kernels
- Multi-core `@parallel` worker dispatch
- C ABI calls through `import c`
- `cobra run`, `check`, `test`, `bench`, `repl`, and `emit-asm`

## Platform status

Linux x86_64 is the supported execution target. Win64, ARM64, and Wasm32 are emitter stubs and contribution opportunities.

## Build the archive

```bash
make dist
```

This creates `cobra-v1.0.0-linux-x86_64.tar.gz` in the repository root. The archive contains the stripped compiler, Cobra libraries, runtime sources, installer, and documentation.

## Verify after unpacking

```bash
./bin/cobra --help
```

Run example programs from a Cobra checkout, or copy a `.cb` file beside the unpacked `bin/` and `lib/` directories.
