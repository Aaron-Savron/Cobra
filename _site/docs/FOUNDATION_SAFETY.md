# Foundation safety

Cobra rejects malformed or unsupported source before native code generation.

## Source limits

Identifiers are limited to 63 characters and string/numeric token payloads to
127 bytes. Oversized values produce a lexical error; they are never silently
truncated. Unterminated strings and unknown tokens are syntax errors.

Inline `asm` blocks are bounded by the compiler's assembly buffer and report a
syntax error when they exceed that limit.

## Arithmetic

Integer division by a compile-time zero is rejected during validation. Dynamic
integer division checks its divisor in native code and reports `[cobra] division
by zero` instead of raising an uncontrolled CPU exception.

## Tensor backend boundary

The tensor metadata format reserves space for higher-rank contracts, but the
current direct native backend supports rank-1 and rank-2 tensors only. Higher
rank declarations are rejected until generalized stride indexing is available.

## CLI execution

Build, test, benchmark, REPL compilation, and `run` use direct process argument
vectors rather than shell-interpolated compiler commands. Filenames containing
spaces or shell metacharacters are passed as data, not shell syntax.
