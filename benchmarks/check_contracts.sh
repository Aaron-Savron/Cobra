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
emit examples/100_generic_functions.cb "$work/generic-functions.s"
emit examples/101_generic_module.cb "$work/generic-module.s"
emit examples/103_generic_readonly_slices.cb "$work/generic-slices.s"
emit examples/104_generic_readonly_slice_module.cb "$work/generic-slice-module.s"
emit examples/105_generic_structs.cb "$work/generic-structs.s"
emit examples/106_generic_struct_module.cb "$work/generic-struct-module.s"
emit examples/107_generic_borrowed_views.cb "$work/generic-borrowed-views.s"
emit examples/108_generic_borrowed_view_module.cb "$work/generic-borrowed-view-module.s"

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
generic_i64=$(body "$work/generic-functions.s" unwrap_or__i64)
generic_f32=$(body "$work/generic-functions.s" unwrap_or__f32)
generic_calls=$(body "$work/generic-functions.s" test_generic_functions)
generic_module_calls=$(body "$work/generic-module.s" test_generic_module_boundary)
generic_slice_i64=$(body "$work/generic-slices.s" readonly_len__i64)
generic_slice_f32=$(body "$work/generic-slices.s" readonly_len__f32)
generic_slice_calls=$(body "$work/generic-slices.s" test_generic_readonly_slices)
generic_slice_module_calls=$(body "$work/generic-slice-module.s" test_generic_readonly_slice_module)
generic_struct_score=$(body "$work/generic-structs.s" box_score)
generic_struct_scale=$(body "$work/generic-structs.s" box_scale)
generic_struct_identity=$(body "$work/generic-structs.s" box_identity)
generic_struct_identity_f32=$(body "$work/generic-structs.s" box_identity_f32)
generic_struct_calls=$(body "$work/generic-structs.s" test_generic_structs)
generic_struct_module_score=$(body "$work/generic-struct-module.s" module_box_score)
generic_struct_module_identity=$(body "$work/generic-struct-module.s" module_box_identity)
generic_struct_module_calls=$(body "$work/generic-struct-module.s" test_generic_struct_module)
generic_borrowed_i64=$(body "$work/generic-borrowed-views.s" view_len_i64)
generic_borrowed_f32=$(body "$work/generic-borrowed-views.s" view_len_f32)
generic_borrowed_u8=$(body "$work/generic-borrowed-views.s" view_len_u8)
generic_borrowed_calls=$(body "$work/generic-borrowed-views.s" test_generic_borrowed_views)
generic_borrowed_module_calls=$(body "$work/generic-borrowed-view-module.s" test_generic_borrowed_view_module)

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

# Scalar source-level generics specialize before lowering. Each specialization
# uses the substituted ABI class, repeated calls reuse one canonical clone, and
# the generic declaration itself never reaches native output.
require_text "generic i64 parameter ABI" "$generic_i64" 'mov[[:space:]]+QWORD PTR \[rbp-[0-9]+\], rsi'
require_text "generic i64 scalar return" "$generic_i64" 'mov[[:space:]]+rax, QWORD PTR \[rbp-[0-9]+\]'
require_text "generic f32 parameter ABI" "$generic_f32" 'movss[[:space:]]+DWORD PTR \[rbp-[0-9]+\], xmm0'
require_text "generic f32 scalar return" "$generic_f32" 'movss[[:space:]]+xmm0, DWORD PTR \[rbp-[0-9]+\]'
require_text "generic i64 call specialization" "$generic_calls" 'call[[:space:]]+unwrap_or__i64@PLT'
require_text "generic f32 call specialization" "$generic_calls" 'call[[:space:]]+unwrap_or__f32@PLT'
if grep -Eq '^unwrap_or:' "$work/generic-functions.s"; then
    printf '%s\n' 'contract failed: unspecialized generic function was emitted' >&2
    exit 1
fi
if [ "$(grep -Ec '^unwrap_or__i64:' "$work/generic-functions.s")" -ne 1 ] ||
   [ "$(grep -Ec '^unwrap_or__f32:' "$work/generic-functions.s")" -ne 1 ]; then
    printf '%s\n' 'contract failed: local generic specializations were duplicated or missing' >&2
    exit 1
fi
if [ "$(grep -Ec '^module_unwrap_or__i64:' "$work/generic-module.s")" -ne 1 ] ||
   [ "$(grep -Ec '^module_unwrap_or__f32:' "$work/generic-module.s")" -ne 1 ]; then
    printf '%s\n' 'contract failed: module generic specializations were duplicated or missing' >&2
    exit 1
fi
if [ "$(grep -Ec 'call[[:space:]]+module_unwrap_or__i64@PLT' <<<"$generic_module_calls")" -ne 2 ]; then
    printf '%s\n' 'contract failed: repeated module generic specialization was not reused' >&2
    exit 1
fi
require_text "module f32 specialization" "$generic_module_calls" 'call[[:space:]]+module_unwrap_or__f32@PLT'

# Readonly generic slices preserve the pointer-plus-length ABI after scalar
# substitution. The specialization remains borrowed, and repeated module calls
# reuse one i64 clone rather than producing duplicate functions.
for slice_body in "$generic_slice_i64" "$generic_slice_f32"; do
    require_text "generic slice pointer" "$slice_body" 'mov[[:space:]]+QWORD PTR \[rbp-[0-9]+\], rdi'
    require_text "generic slice length" "$slice_body" 'mov[[:space:]]+QWORD PTR \[rbp-[0-9]+\], rsi'
    forbid_text "generic slice runtime dispatch" "$slice_body" 'call[[:space:]]+cobra_'
done
require_text "generic i64 slice call" "$generic_slice_calls" 'call[[:space:]]+readonly_len__i64@PLT'
require_text "generic f32 slice call" "$generic_slice_calls" 'call[[:space:]]+readonly_len__f32@PLT'
if [ "$(grep -Ec '^readonly_len__i64:' "$work/generic-slices.s")" -ne 1 ] ||
   [ "$(grep -Ec '^readonly_len__f32:' "$work/generic-slices.s")" -ne 1 ]; then
    printf '%s\n' 'contract failed: readonly slice specializations were duplicated or missing' >&2
    exit 1
fi
if [ "$(grep -Ec 'call[[:space:]]+module_readonly_len__i64@PLT' <<<"$generic_slice_module_calls")" -ne 2 ]; then
    printf '%s\n' 'contract failed: repeated readonly slice module specialization was not reused' >&2
    exit 1
fi
require_text "module readonly f32 slice" "$generic_slice_module_calls" 'call[[:space:]]+module_readonly_len__f32@PLT'

# Scalar generic structs are materialized before lowering. Their parameters are
# ordinary by-value struct pointers, fields use the specialized packed layout,
# and struct returns copy into caller-owned storage instead of returning a dead
# callee-frame address.
require_text "generic struct scalar field copy" "$generic_struct_score" 'mov rax, QWORD PTR \[rsi\+0\]'
require_text "generic struct f32 field load" "$generic_struct_scale" 'movss xmm0, DWORD PTR \[rax \+ 0\]'
require_text "generic struct return sret" "$generic_struct_identity" 'mov rdi, QWORD PTR \[rbp-240\]'
require_text "generic struct return copy" "$generic_struct_identity" 'mov QWORD PTR \[rdi\+0\], rax'
require_text "generic struct i64 call" "$generic_struct_calls" 'call[[:space:]]+box_score@PLT'
require_text "generic struct f32 call" "$generic_struct_calls" 'call[[:space:]]+box_scale@PLT'
require_text "generic struct return call" "$generic_struct_calls" 'call[[:space:]]+box_identity@PLT'
require_text "generic struct f32 return call" "$generic_struct_calls" 'call[[:space:]]+box_identity_f32@PLT'
require_text "generic struct f32 return copy" "$generic_struct_identity_f32" 'mov QWORD PTR \[rdi\+0\], rax'
forbid_text "generic struct heap" "$generic_struct_score $generic_struct_scale $generic_struct_identity $generic_struct_identity_f32" '((malloc|calloc|realloc|free)@PLT|cobra_(alloc|free|type))'
if [ "$(grep -Ec '^box_score:' "$work/generic-structs.s")" -ne 1 ] ||
   [ "$(grep -Ec '^box_scale:' "$work/generic-structs.s")" -ne 1 ] ||
   [ "$(grep -Ec '^box_identity:' "$work/generic-structs.s")" -ne 1 ] ||
   [ "$(grep -Ec '^box_identity_f32:' "$work/generic-structs.s")" -ne 1 ]; then
    printf '%s\n' 'contract failed: generic struct specializations were duplicated or missing' >&2
    exit 1
fi
require_text "module generic struct field" "$generic_struct_module_score" 'mov rax, QWORD PTR \[rsi\+0\]'
require_text "module generic struct return" "$generic_struct_module_identity" 'mov QWORD PTR \[rdi\+0\], rax'
require_text "module generic struct call" "$generic_struct_module_calls" 'call[[:space:]]+module_box_score@PLT'
if [ "$(grep -Ec 'call[[:space:]]+module_box_identity@PLT' <<<"$generic_struct_module_calls")" -ne 2 ]; then
    printf '%s\n' 'contract failed: repeated module generic struct specialization was not reused' >&2
    exit 1
fi
if [ "$(grep -Ec '^module_box_identity:' "$work/generic-struct-module.s")" -ne 1 ]; then
    printf '%s\n' 'contract failed: module generic struct identity was duplicated or missing' >&2
    exit 1
fi

# Generic borrowed-field structs retain a two-word view field after scalar
# substitution. The field is copied by value, read through canonical offsets,
# and never allocates or dispatches through a runtime type object.
for borrowed_body in "$generic_borrowed_i64" "$generic_borrowed_f32" "$generic_borrowed_u8"; do
    require_text "generic borrowed field pointer" "$borrowed_body" 'mov rax, QWORD PTR \[rsi\+0\]'
    require_text "generic borrowed field length" "$borrowed_body" 'mov rax, QWORD PTR \[rsi\+8\]'
    forbid_text "generic borrowed field runtime dispatch" "$borrowed_body" 'call[[:space:]]+cobra_'
    forbid_text "generic borrowed field allocator" "$borrowed_body" 'call[[:space:]]+(malloc|calloc|realloc|memcpy|memmove)@PLT'
done
require_text "generic borrowed i64 view call" "$generic_borrowed_calls" 'call[[:space:]]+view_len_i64@PLT'
require_text "generic borrowed f32 view call" "$generic_borrowed_calls" 'call[[:space:]]+view_len_f32@PLT'
require_text "generic borrowed u8 view call" "$generic_borrowed_calls" 'call[[:space:]]+view_len_u8@PLT'
if [ "$(grep -Ec '^module_view_len_i64:' "$work/generic-borrowed-view-module.s")" -ne 1 ] ||
   [ "$(grep -Ec '^module_view_len_f32:' "$work/generic-borrowed-view-module.s")" -ne 1 ] ||
   [ "$(grep -Ec '^module_view_len_u8:' "$work/generic-borrowed-view-module.s")" -ne 1 ]; then
    printf '%s\n' 'contract failed: generic borrowed-view module specializations were duplicated or missing' >&2
    exit 1
fi
if [ "$(grep -Ec 'call[[:space:]]+module_view_len_i64@PLT' <<<"$generic_borrowed_module_calls")" -ne 2 ]; then
    printf '%s\n' 'contract failed: repeated generic borrowed-view module specialization was not reused' >&2
    exit 1
fi
require_text "module generic borrowed f32 call" "$generic_borrowed_module_calls" 'call[[:space:]]+module_view_len_f32@PLT'
require_text "module generic borrowed u8 call" "$generic_borrowed_module_calls" 'call[[:space:]]+module_view_len_u8@PLT'

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
