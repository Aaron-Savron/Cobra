#!/usr/bin/env bash
set -euo pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/cobra-contracts.XXXXXX")
trap 'rm -rf "$work"' EXIT

make -C "$root" >/dev/null

emit() {
    local source=$1
    local output=$2
    "$root/cobra" emit-asm "$root/$source" -o "$output" >/dev/null
}

body() {
    local assembly=$1
    local function=$2
    awk -v fn="$function" '
        $0 == fn ":" { inside = 1; next }
        inside && $0 ~ "^[[:space:]]+\\.global " { exit }
        inside && $0 ~ "^[[:space:]]+\\.type " { exit }
        inside { print }
    ' "$assembly"
}

require_text() {
    local label=$1
    local text=$2
    local pattern=$3
    if ! grep -Eq "$pattern" <<<"$text"; then
        printf 'contract failed: %s (missing /%s/)\n' "$label" "$pattern" >&2
        exit 1
    fi
}

forbid_text() {
    local label=$1
    local text=$2
    local pattern=$3
    if grep -Eq "$pattern" <<<"$text"; then
        printf 'contract failed: %s (found forbidden /%s/)\n' "$label" "$pattern" >&2
        exit 1
    fi
}

emit examples/59_option_result.cb "$work/sums.s"
emit examples/98_sum_parameters.cb "$work/sum-params.s"
emit examples/99_sum_abi_matrix.cb "$work/sum-matrix.s"
emit examples/60_enum_match.cb "$work/enums.s"
emit examples/62_sum_interactions.cb "$work/propagation.s"
emit examples/72_struct_parameters.cb "$work/structs.s"
emit examples/54_user_loop_vectorization.cb "$work/vector.s"
emit examples/56_constant_shape_kernels.cb "$work/constant-gemm.s"
emit examples/70_http_typed_api.cb "$work/http.s"
emit examples/90_recursive_struct_layout.cb "$work/nested-structs.s"
emit examples/92_module_visibility.cb "$work/modules.s"
emit examples/100_array_slice_coercion.cb "$work/array-coercion.s"

maybe=$(body "$work/sums.s" maybe_value)
checked=$(body "$work/sums.s" checked_value)
sum_params=$(body "$work/sum-params.s" consume_after_six)
sum_call=$(body "$work/sum-params.s" test_scalar_sum_parameters)
many_sums=$(body "$work/sum-matrix.s" consume_many)
mixed_sums=$(body "$work/sum-matrix.s" consume_mixed)
nested_sums=$(body "$work/sum-matrix.s" nested)
phase=$(body "$work/enums.s" phase_score)
point=$(body "$work/structs.s" point_score)
scale=$(body "$work/vector.s" scale_buffers)
affine=$(body "$work/vector.s" test_affine)
relu6=$(body "$work/vector.s" test_relu6)
unrolled=$(body "$work/constant-gemm.s" test_single_row_eight_cols)
propagate=$(body "$work/propagation.s" module_chain)
region=$(body "$work/propagation.s" loop_region_result)
http=$(body "$work/http.s" parse_request_checked)
nested=$(body "$work/nested-structs.s" box_score)
modules=$(cat "$work/modules.s")
array_coercion=$(body "$work/array-coercion.s" test_array_to_readonly_slice)

# Scalar values, enums, and sums stay in native registers/frame slots. They do
# not allocate a heap object or enter a runtime type dispatcher.
for pair in "maybe_value:$maybe" "checked_value:$checked" "phase_score:$phase"; do
    name=${pair%%:*}
    text=${pair#*:}
    forbid_text "$name heap" "$text" '((malloc|calloc|realloc|free)@PLT|cobra_(alloc|free|type))'
    forbid_text "$name runtime dispatch" "$text" 'call[[:space:]]+cobra_'
done

# Scalar sums use one pointer slot, copy into callee-private storage, and still
# spill correctly after the six SysV integer argument registers are exhausted.
require_text "sum parameter stack load" "$sum_params" 'mov rax, QWORD PTR \[rbp\+16\]'
require_text "sum parameter copy source" "$sum_params" 'mov rsi, QWORD PTR \[rbp-[0-9]+\]'
require_text "sum parameter copy destination" "$sum_params" 'lea rdi, \[rbp-[0-9]+\]'
require_text "sum parameter stack spill" "$sum_call" 'mov QWORD PTR \[rsp\+0\], rax'
forbid_text "sum parameter runtime dispatch" "$sum_params" 'call[[:space:]]+cobra_'

# The ABI matrix covers two stacked sums, mixed XMM/GPR classes, and nested
# calls where both sides return sums.
require_text "multiple sum stack load one" "$many_sums" 'mov rax, QWORD PTR \[rbp\+16\]'
require_text "multiple sum stack load two" "$many_sums" 'mov rax, QWORD PTR \[rbp\+24\]'
require_text "mixed f32 parameter" "$mixed_sums" 'movss DWORD PTR \[rbp-264\], xmm0'
require_text "mixed slice pointer" "$mixed_sums" 'mov rax, QWORD PTR \[rbp-32\]'
require_text "mixed slice length" "$mixed_sums" 'mov rax, QWORD PTR \[rbp-40\]'
require_text "mixed struct field copy" "$mixed_sums" 'mov rax, QWORD PTR \[rsi\+0\]'
require_text "mixed struct second field" "$mixed_sums" 'mov rax, QWORD PTR \[rsi\+8\]'
require_text "mixed checked sum capture" "$mixed_sums" 'mov rax, QWORD PTR \[rbp-56\]'
require_text "mixed optional sum capture" "$mixed_sums" 'mov rax, QWORD PTR \[rbp-64\]'
require_text "nested sum producer" "$nested_sums" 'call produce@PLT'
require_text "nested sum forwarder" "$nested_sums" 'call forward@PLT'

# Scalar struct parameters still copy bytes into a private native frame.
require_text "struct parameter copy" "$point" 'mov[[:space:]]+rax, QWORD PTR \[rsi\+0\]'
require_text "struct parameter second field" "$point" 'mov[[:space:]]+rax, QWORD PTR \[rsi\+8\]'
forbid_text "struct parameter heap" "$point" '((malloc|calloc|realloc|free)@PLT|cobra_(alloc|free|type))'

# Slice code exposes pointer and length frame loads and stays free of runtime
# collection/type objects.
require_text "slice pointer/length loads" "$scale" 'QWORD PTR \[rbp-'
require_text "slice indexed vector address" "$scale" 'YMMWORD PTR \[[a-z0-9]+ \+ [a-z0-9]+\*4\]'
require_text "slice length guard" "$scale" 'cmp[[:space:]]+[a-z0-9]+, [a-z0-9]+'
forbid_text "slice runtime object" "$scale" 'call[[:space:]]+cobra_(list|dict|type)'

# Fixed arrays passed to readonly slices use their existing stack storage as a
# pointer-plus-length view. No allocation, copy, or hidden collection object is
# introduced at the call boundary.
require_text "array view pointer" "$array_coercion" 'mov[[:space:]]+rdi, QWORD PTR \[rbp-[0-9]+\]'
require_text "array view length" "$array_coercion" 'mov[[:space:]]+rsi, QWORD PTR \[rbp-[0-9]+\]'
require_text "array view call" "$array_coercion" 'call[[:space:]]+readonly_sum@PLT'
forbid_text "array view allocator" "$array_coercion" 'call[[:space:]]+(malloc|calloc|realloc|free|memcpy|memmove)@PLT'

# A simple match is compares and branches, not a boxed state machine.
require_text "enum compare" "$phase" 'cmp[[:space:]]'
require_text "enum branch" "$phase" 'j(e|ne|g|l|le|ge)[[:space:]]'
forbid_text "enum runtime dispatch" "$phase" 'call[[:space:]]+cobra_'

# Postfix ? is a direct native tag/status branch and region failures clean up
# before propagating. The exact registers remain an emitter detail; the
# comparison, branch, and cleanup contracts do not.
require_text "typed propagation tag branch" "$propagate" 'cmp[[:space:]].*1'
require_text "region cleanup" "$region" 'arena_destroy@PLT'
region_cleanups=$(grep -Ec 'arena_destroy@PLT' <<<"$region" || true)
if [ "$region_cleanups" -lt 2 ]; then
    printf 'contract failed: region has only %s cleanup exits\n' "$region_cleanups" >&2
    exit 1
fi
if ! awk '/arena_destroy@PLT/ { seen = 1 } seen && /jmp[[:space:]]+[.]Lpropagate/ { found = 1 } END { exit !found }' <<<"$region"; then
    printf '%s\n' 'contract failed: region propagation path does not clean up before branching' >&2
    exit 1
fi
forbid_text "non-region path cleanup" "$propagate" 'arena_destroy@PLT'

# User loops and constant-shape kernels retain their vector lowering.
require_text "user-loop FMA" "$affine" 'vfmadd'
require_text "user-loop ABI fence" "$affine" 'vzeroupper'
require_text "ReLU6 lower clamp" "$relu6" 'vmaxps'
require_text "ReLU6 upper clamp" "$relu6" 'vminps'

gemm_kernel=$(awk '
    /vbroadcastss/ && !seen { seen = 1 }
    seen { print }
    seen && /vfmadd/ { fmas++ }
    seen && fmas >= 4 && /vaddps/ { exit }
' <<<"$unrolled")
fmas=$(grep -Ec '[[:space:]]vfmadd' <<<"$gemm_kernel" || true)
if [ "$fmas" -lt 4 ]; then
    printf 'contract failed: constant GEMM has only %s unrolled FMA instructions\n' "$fmas" >&2
    exit 1
fi
forbid_text "constant GEMM call fallback" "$unrolled" 'call[[:space:]]+dense_f32'
forbid_text "constant GEMM kernel branch" "$gemm_kernel" '^[[:space:]]+j[a-z]+'

# Nested scalar structs reuse direct field offsets and remain native values.
require_text "nested struct offset" "$nested" 'add[[:space:]]+rax, [0-9]+'
forbid_text "nested struct runtime dispatch" "$nested" 'call[[:space:]]+cobra_'

# Struct layout contracts: mixed-size, nested, enum-promoted, and
# borrowed-view fields must emit the canonical packed offsets, never a
# per-node offset carried from legacy layout data.
emit examples/102_struct_layout_offsets.cb "$work/layout-offsets.s"
mixed_writes=$(body "$work/layout-offsets.s" test_mixed_layout)
nested_writes=$(body "$work/layout-offsets.s" test_nested_layout)
state_read=$(body "$work/layout-offsets.s" state_level)
view_read=$(body "$work/layout-offsets.s" view_len)
require_text "mixed f32 field offset" "$mixed_writes" 'movss DWORD PTR \[rdx \+ 8\], xmm0'
require_text "mixed tail field offset" "$mixed_writes" 'mov QWORD PTR \[rdx \+ 12\], rax'
require_text "nested tag field offset" "$nested_writes" 'mov QWORD PTR \[rdx \+ 16\], rax'
require_text "enum field load" "$state_read" 'mov rax, QWORD PTR \[rax \+ 0\]'
require_text "borrowed view length offset" "$view_read" 'mov rdx, QWORD PTR \[rax \+ 8\]'
require_text "borrowed view pointer offset" "$view_read" 'mov rax, QWORD PTR \[rax \+ 0\]'
forbid_text "struct layout runtime dispatch" "$state_read $view_read" 'call[[:space:]]+cobra_'

# Private source functions stay local to the native object while public entry
# points remain externally visible.
require_text "private local symbol" "$modules" '^    \.local hidden_value$'
forbid_text "private global symbol" "$modules" '^    \.global hidden_value$'

# The typed HTTP wrapper remains a direct call boundary with no hidden heap or
# runtime type dispatch in the wrapper itself.
require_text "HTTP direct library call" "$http" 'call[[:space:]]+http_request_parse'
forbid_text "HTTP wrapper heap" "$http" '((malloc|calloc|realloc)@PLT|cobra_(alloc|free|type))'

# Portable output must not contain AVX instructions from uncalled auto-loaded
# NN helpers, and a reachable AVX-backed wrapper must be rejected.
"$root/cobra" emit-asm "$root/examples/91_portable_scalar.cb" -o "$work/portable.s" --cpu=portable >/dev/null
if grep -Eiq '^[[:space:]]+v[a-z0-9]+' "$work/portable.s"; then
    printf '%s\n' 'contract failed: portable assembly contains a vector instruction' >&2
    exit 1
fi
if "$root/cobra" emit-asm "$root/examples/93_portable_nn_rejected.cb" -o "$work/portable-nn.s" --cpu=portable >/dev/null 2>&1; then
    printf '%s\n' 'contract failed: portable NN wrapper was accepted' >&2
    exit 1
fi

printf '%s\n' 'native assembly contracts passed'
