---
name: Feature request
about: Propose a new language feature, compiler flag, or standard library addition
labels: enhancement
---

## Problem statement

<!-- Describe the concrete problem or gap. What can you not express or do today?
     Be specific: reference the relevant .cb syntax, CLI command, or compiler output. -->

---

## Proposed syntax or API

<!-- Show what the new feature would look like from a user's perspective.
     Include a short .cb example if the feature touches the language surface. -->

```cobra
```

---

## Compiler phases affected

Check all that apply:

- [ ] `src/lexer.c` - new token type needed
- [ ] `src/parser.c` - new grammar rule or AST node type needed
- [ ] `src/ast.c` - new `ASTNodeType` or `CobraTypeKind` needed
- [ ] `src/ir.c` - new ownership or type validation rule needed
- [ ] `src/codegen.c` - new assembly emission logic needed
- [ ] `src/interpreter.c` - new `@comptime` evaluation behavior needed
- [ ] `src/main.c` - new CLI command or flag needed
- [ ] `include/cobra.h` - new type, constant, or prototype needed
- [ ] `lib/std.cb` or `lib/nn.cb` - standard library change only

---

## Alternatives considered

<!-- What workarounds exist today? Why are they insufficient?
     Are there similar features in C, Python, or Rust that informed this proposal? -->

---

## Additional context

<!-- Links to relevant issues, related examples in `examples/`, or prior discussion. -->
