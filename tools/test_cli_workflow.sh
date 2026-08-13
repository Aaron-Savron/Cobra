#!/usr/bin/env bash
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

make >/tmp/cobra-cli-workflow-build.log 2>&1
work=$(mktemp -d "${TMPDIR:-/tmp}/cobra-cli-workflow.XXXXXX")
trap 'rm -rf "$work"' EXIT

./cobra init "$work/nested/project" >/tmp/cobra-cli-workflow-init.log 2>&1
test -f "$work/nested/project/cobra.toml"
test -f "$work/nested/project/main.cb"
grep -q 'name = "project"' "$work/nested/project/cobra.toml"

./cobra check "$work/nested/project/main.cb" >/tmp/cobra-cli-workflow-check.log 2>&1
test ! -e "$work/nested/project/main.s"
test ! -e "$work/nested/project/output"
test ! -e "$work/nested/project/output.s"

cp "$work/nested/project/cobra.toml" "$work/manifest.before"
cp "$work/nested/project/main.cb" "$work/main.before"
if ./cobra init "$work/nested/project" >/tmp/cobra-cli-workflow-existing.log 2>&1; then
    echo "cobra init unexpectedly overwrote an existing project" >&2
    exit 1
fi
cmp "$work/manifest.before" "$work/nested/project/cobra.toml"
cmp "$work/main.before" "$work/nested/project/main.cb"

cat >"$work/diagnostic.cb" <<'EOF'
def broken() -> i64: {
    return missing_name
}
EOF
if ./cobra check "$work/diagnostic.cb" >"$work/diagnostic.log" 2>&1; then
    echo "cobra check unexpectedly accepted an undefined variable" >&2
    exit 1
fi
grep -Eq "diagnostic\\.cb:[0-9]+:[0-9]+: error: undefined variable 'missing_name'" "$work/diagnostic.log"

cat >"$work/syntax.cb" <<'EOF'
def broken() -> i64: {
    return -name
}
EOF
if ./cobra check "$work/syntax.cb" >"$work/syntax.log" 2>&1; then
    echo "cobra check unexpectedly accepted malformed syntax" >&2
    exit 1
fi
grep -Eq "syntax\\.cb:[0-9]+:[0-9]+: error: unary '-' requires a numeric literal" "$work/syntax.log"

mkdir -p "$work/modules/root" "$work/modules/dependency"
cat >"$work/modules/root/main.cb" <<'EOF'
import "../dependency/missing.cb"
def main() -> i64: {
    return 0
}
EOF
if ./cobra check "$work/modules/root/main.cb" >"$work/module.log" 2>&1; then
    echo "cobra check unexpectedly accepted an unresolved module" >&2
    exit 1
fi
grep -Eq "main\\.cb:[0-9]+:[0-9]+: error: cannot resolve module '.*missing\\.cb'" "$work/module.log"
grep -q 'Module import chain:' "$work/module.log"

cat >"$work/modules/dependency/bad.cb" <<'EOF'
def broken() -> i64: {
    return -name
}
EOF
cat >"$work/modules/root/main.cb" <<'EOF'
import "../dependency/bad.cb"
def main() -> i64: {
    return 0
}
EOF
if ./cobra check "$work/modules/root/main.cb" >"$work/imported-syntax.log" 2>&1; then
    echo "cobra check unexpectedly accepted malformed imported syntax" >&2
    exit 1
fi
grep -Eq "bad\\.cb:[0-9]+:[0-9]+: error: unary '-' requires a numeric literal" "$work/imported-syntax.log"

./cobra build examples/47_python_comfort.cb -o "$work/python-comfort" >/tmp/cobra-cli-workflow-python.log 2>&1
"$work/python-comfort" >"$work/python-comfort.log" 2>&1
grep -q 'Python comfort, native execution' "$work/python-comfort.log"

./cobra build examples/48_python_comfort_edges.cb -o "$work/python-comfort-edges" >/tmp/cobra-cli-workflow-python-edges.log 2>&1
"$work/python-comfort-edges" >"$work/python-comfort-edges.log" 2>&1
grep -q 'Python comfort edge coverage passed' "$work/python-comfort-edges.log"

cat >"$work/dynamic-zero-step.cb" <<'EOF'
def main(): {
    step = 0
    for value in range(0, 5, step): {
        print(value)
    }
    return 0
}
EOF
./cobra build "$work/dynamic-zero-step.cb" -o "$work/dynamic-zero-step" >/tmp/cobra-cli-workflow-zero-build.log 2>&1
if "$work/dynamic-zero-step" >"$work/dynamic-zero-step.log" 2>&1; then
    echo "dynamic zero-step range unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'range step cannot be zero' "$work/dynamic-zero-step.log"

cat >"$work/empty-min.cb" <<'EOF'
def main(): {
    values = []
    min(values)
    return 0
}
EOF
./cobra build "$work/empty-min.cb" -o "$work/empty-min" >/tmp/cobra-cli-workflow-empty-min-build.log 2>&1
if "$work/empty-min" >"$work/empty-min.log" 2>&1; then
    echo "empty min unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'min/max requires a non-empty collection' "$work/empty-min.log"

cat >"$work/empty-max.cb" <<'EOF'
def main(): {
    values = []
    max(values)
    return 0
}
EOF
./cobra build "$work/empty-max.cb" -o "$work/empty-max" >/tmp/cobra-cli-workflow-empty-max-build.log 2>&1
if "$work/empty-max" >"$work/empty-max.log" 2>&1; then
    echo "empty max unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'min/max requires a non-empty collection' "$work/empty-max.log"

if ./cobra check tests/negative/45_two_target_loop.cb >"$work/two-target-loop.log" 2>&1; then
    echo "two-target loop unexpectedly succeeded without enumerate" >&2
    exit 1
fi
grep -q 'two loop targets require enumerate(collection)' "$work/two-target-loop.log"

./cobra test examples/45_everyday_core.cb >/tmp/cobra-cli-workflow-everyday.log 2>&1
grep -q 'Result: 2 passed, 0 failed' /tmp/cobra-cli-workflow-everyday.log

cat >"$work/negative-borrowed-free.cb" <<'EOF'
def consume(values: list[i64]): {
    free(values)
    return 0
}
def main(): { return 0 }
EOF
if ./cobra check "$work/negative-borrowed-free.cb" >/tmp/cobra-cli-workflow-ownership.log 2>&1; then
    echo "cobra check unexpectedly accepted freeing a borrowed collection" >&2
    exit 1
fi
grep -q 'free requires an owned collection' /tmp/cobra-cli-workflow-ownership.log

cat >"$work/negative-alias.cb" <<'EOF'
def main(): {
    let first: list[i64] = [1]
    let second: list[i64] = [2]
    second = first
    return 0
}
EOF
if ./cobra check "$work/negative-alias.cb" >/tmp/cobra-cli-workflow-alias.log 2>&1; then
    echo "cobra check unexpectedly accepted an unsafe owned alias" >&2
    exit 1
fi
grep -q 'owned value' /tmp/cobra-cli-workflow-alias.log

cat >"$work/negative-discarded-string.cb" <<'EOF'
def main(): {
    concat("discarded", " value")
    return 0
}
EOF
if ./cobra check "$work/negative-discarded-string.cb" >"$work/discarded-string.log" 2>&1; then
    echo "cobra check unexpectedly accepted a discarded fresh string" >&2
    exit 1
fi
grep -q 'discarded fresh string result would leak' "$work/discarded-string.log"

cat >"$work/negative-string-borrow.cb" <<'EOF'
def identity_text(value: string) -> string: {
    return value
}
def main(): {
    let source: string = "source" + " value"
    let forwarded: string = identity_text(source)
    string_free(source)
    return 0
}
EOF
if ./cobra check "$work/negative-string-borrow.cb" >"$work/string-borrow.log" 2>&1; then
    echo "cobra check unexpectedly accepted freeing a live forwarded string source" >&2
    exit 1
fi
grep -q 'forwarded alias is live' "$work/string-borrow.log"

cat >"$work/negative-string-return.cb" <<'EOF'
def identity_text(value: string) -> string: {
    return value
}
def forward_text(value: string) -> string: {
    let forwarded: string = identity_text(value)
    return forwarded
}
def main(): {
    return 0
}
EOF
if ./cobra check "$work/negative-string-return.cb" >"$work/string-return.log" 2>&1; then
    echo "cobra check unexpectedly accepted a forwarded string escape" >&2
    exit 1
fi
grep -q 'forwarded string alias would escape its owner' "$work/string-return.log"

echo 'CLI workflow checks passed'
