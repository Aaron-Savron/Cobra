# Native strings

Cobra strings are immutable, NUL-terminated UTF-8 byte strings represented as one native pointer. A literal and a string parameter/return value have no hidden object header and pass through the normal GPR ABI.

```cobra
def label(name: string) -> string: {
    return "model: " + name
}

def main(): {
    let text: string = label("cobra")
    print(text)
    string_free(text)
    return 0
}
```

## Operations

- `len(text)` returns the byte length.
- `left + right` concatenates two strings and allocates a new owned string.
- `left == right`, `!=`, `<`, `>`, `<=`, `>=` compare lexicographically.
- `starts_with(text, prefix)`, `ends_with(text, suffix)`, and `contains(text, part)` return an integer boolean.
- `char_at(text, index)` returns the byte value and traps on an out-of-range index.
- `string_free(text)` releases a string created by concatenation or returned by a function proven to return fresh concatenation storage.

Literals, parameters, and values forwarded without concatenation are borrowed/read-only and must not be passed to `string_free`. Cobra rejects double frees and rejects freeing a non-owned string during validation.

Escapes are decoded in source strings: `\\n`, `\\r`, `\\t`, `\\\\`, and `\\\"`. Unknown escapes preserve their backslash rather than silently changing the source text.

Only direct concatenation and functions proven to return fresh concatenation storage transfer ownership; ambiguous string-returning functions remain borrowed and cannot be freed. This is intentionally a pointer representation rather than a runtime string object. It keeps the common path ABI-cheap and leaves room for future bounded slices, Unicode helpers, formatting, and streaming I/O without forcing those costs on every string.
