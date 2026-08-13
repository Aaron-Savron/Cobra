---
name: Bug report
about: Report incorrect compiler behavior, a crash, or wrong output
labels: bug
---

## Environment

**Cobra version** (output of `./cobra --version` or `./cobra` with no args):

```
```

**OS and architecture:**
<!-- e.g. Ubuntu 22.04 x86_64, Fedora 40 x86_64 -->

**CPU features** (output of `grep -m1 flags /proc/cpuinfo | tr ' ' '\n' | grep -E 'avx2|fma'`):

```
```

---

## Reproduction

**Minimal `.cb` file that triggers the bug:**

```cobra
```

**Command used:**
<!-- e.g. ./cobra run examples/foo.cb  or  ./cobra test examples/foo.cb -->

```bash
```

---

## Output

**Actual output** (include full stdout and stderr):

```
```

**Expected output:**

```
```

---

## Assembly (if relevant)

If the bug is in generated code, include the relevant snippet from `./cobra emit-asm <file.cb> -o out.s`:

```asm
```

---

## Additional context

<!-- Stack traces, related issue numbers, or notes on which compiler phase is likely involved (lexer, parser, ir, codegen). -->
