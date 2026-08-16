/*
 * Cobra direct native emitter.
 *
 * This backend deliberately has no intermediate representation: the validated
 * AST is lowered directly to Intel-syntax x86-64.  The symbol table below is
 * the small amount of lowering metadata needed to keep pointer/length pairs,
 * scalar values, and tensor descriptors distinct without adding runtime
 * objects to ordinary Cobra programs.
 */
#include "../include/cobra.h"
#include <stdint.h>
#include <math.h>

#define COBRA_FRAME_BYTES 4096
#define COBRA_LOCAL_BASE 256
#define COBRA_FRAME_LIMIT 4000
#define COBRA_SCR_M 160
#define COBRA_SCR_N 168
#define COBRA_SCR_K 176
#define COBRA_SCR_I 184
#define COBRA_SCR_J 192
#define COBRA_SCR_KI 200
#define COBRA_SCR_TMP 208
#define COBRA_SCR_IMK 224
#define COBRA_SCR_IN 232
#define COBRA_TENSOR_FIELDS (3 + 2 * COBRA_VIEW_MAX_RANK)
#define COBRA_ARG_SAVE_BASE 32
#define PAR_MAX_CAPTURES 8

/* Auto-vectorization of index-pure []f32 loops is on by default; -O0 or
   --cpu=portable disables it (see main.c). */
static bool g_opt_vectorize = true;
static bool g_portable_cpu = false;
static bool g_gpu_enabled = true;

void codegen_set_vectorize(bool enabled) { g_opt_vectorize = enabled; }
void codegen_set_portable(bool enabled) { g_portable_cpu = enabled; }
void codegen_set_gpu_enabled(bool enabled) { g_gpu_enabled = enabled; }

typedef enum { SYM_SCALAR = 0, SYM_ARRAY, SYM_SLICE, SYM_TENSOR, SYM_F32, SYM_LIST, SYM_DICT, SYM_STRUCT, SYM_BOOL, SYM_OPTION, SYM_RESULT } SymbolKind;

/* An active `with region NAME:` scope. Each entry owns a hidden three-slot
   i64 state array ([base, cur, cap]) that arena_create/arena_alloc/
   arena_destroy read and write; the backing store is released once after the
   body. Region-qualified alloc_i64, alloc_f32, and alloc_u8 calls bump from the innermost
   region whose name matches the qualifier. */
typedef struct {
    bool active;
    char name[COBRA_MAX_IDENT_LEN];
    int state_base;
    int capacity_slot;
} RegionInfo;

/* A proven @parallel loop queued during the caller's emission and flushed as
   a standalone worker function after the caller's body completes. */
typedef struct {
    bool active;
    int id;
    ASTNode *loop;
    char source[COBRA_MAX_IDENT_LEN];
    char bufs[PAR_MAX_CAPTURES][COBRA_MAX_IDENT_LEN];
    int nb;
    char scals[PAR_MAX_CAPTURES][COBRA_MAX_IDENT_LEN];
    CobraTypeKind scal_types[PAR_MAX_CAPTURES];
    int ns;
} PendingParallel;

typedef struct {
    char name[COBRA_MAX_IDENT_LEN];
    SymbolKind kind;
    CobraTypeKind type;
    CobraTypeKind element_type;
    int offset;
    int capacity_offset;
    int length_offset;
    int array_len;
    int array_base;
    int rank_offset;
    int tag_offset;
    int payload_offset;
    int error_offset;
    CobraTypeKind payload_type;
    CobraTypeKind error_type;
    int payload_size;
    int error_size;
    char payload_type_name[COBRA_MAX_IDENT_LEN];
    char error_type_name[COBRA_MAX_IDENT_LEN];
    int dim_offsets[COBRA_VIEW_MAX_RANK];
    int stride_offsets[COBRA_VIEW_MAX_RANK];
    int rank;
    bool contiguous;
    bool owned;
    bool borrowed;
    bool indirect;
    int qualifier;
    char type_name[COBRA_MAX_IDENT_LEN];
    /* Non-empty only for a `dyn TraitName`-typed local/param: names the
       trait so a qualified method call on this symbol can be resolved by
       vtable-block dispatch instead of the ordinary static-dispatch or
       direct-call paths. See emit_dyn_trait_call/emit_dyn_dispatch_call. */
    char dyn_trait_name[COBRA_MAX_IDENT_LEN];
} VarSymbol;

typedef struct {
    FILE *out;
    ASTNode *root;
    TargetPlatform target;
    VarSymbol symbols[256];
    int symbol_count;
    int stack_offset;
    int label_count;
    int string_count;
    int const_count;
    bool test_mode;
    bool opt_vectorize;
    char imported_functions[256][COBRA_MAX_IDENT_LEN];
    int imported_function_count;
    struct {
        bool active;
        char name[COBRA_MAX_IDENT_LEN];
        char secondary_name[COBRA_MAX_IDENT_LEN];
        char source[COBRA_MAX_IDENT_LEN];
        int index_offset;
        CobraTypeKind element_type;
        bool enumerate;
    } loops[16];
    int loop_depth;
    RegionInfo regions[16];
    int region_depth;
    CobraTypeKind current_return_type;
    CobraTypeKind current_return_payload_type;
    CobraTypeKind current_return_error_type;
    char current_return_type_name[COBRA_MAX_IDENT_LEN];
    char current_return_error_type_name[COBRA_MAX_IDENT_LEN];
    char current_return_dyn_trait_name[COBRA_MAX_IDENT_LEN];
    int propagation_label;
    PendingParallel pending_parallel[16];
    int pending_parallel_count;
    /* Plain top-level functions used as fn(...)->... values (closures aside)
       get a lazily-emitted {adapter_stub, 0} thunk the first time their
       address is taken, so every fn value - closure or plain - is a pointer
       to a uniform 16-byte {code_ptr, env_ptr} pair; see ensure_fn_thunk. */
    char fn_thunks_emitted[64][COBRA_MAX_IDENT_LEN];
    int fn_thunk_count;
    /* Names recorded by ensure_fn_thunk but not yet flushed to output. Actual
       adapter/thunk bytes must never be printed mid-function (execution would
       fall straight through the adapter's instructions as if they were part
       of the caller's own body) - flush_pending_fn_thunks prints them only
       after the current function's `ret`, mirroring flush_pending_parallel. */
    char fn_thunk_pending[16][COBRA_MAX_IDENT_LEN];
    int fn_thunk_pending_count;
    /* Struct locals proven, by a whole-body scan at function entry, to never
       be reassigned, copied elsewhere, passed as a call argument, or
       returned. Only these may have their owned string/slice fields freed
       automatically at scope exit; see compute_safe_autofree_structs. */
    char safe_autofree_structs[64][COBRA_MAX_IDENT_LEN];
    int safe_autofree_count;
    /* One static .rodata method-pointer array per (Trait,ConcreteType) pair
       actually coerced to dyn Trait, keyed "<Trait>|<Type>". Emitted once on
       first use (see emit_dyn_vtable_label) so every dispatch block for that
       pairing shares the same vtable instead of writing method pointers with
       per-call mov instructions into a freshly malloc'd block. */
    char dyn_vtables_emitted[128][COBRA_MAX_IDENT_LEN * 2];
    int dyn_vtable_count;
    /* Whole-program struct-parameter borrow summary cache (memory design
       phase 1). Keyed "<fn_name>|<param_name>". state: 0 = not started,
       1 = in progress (cycle guard - a parameter reached through recursion
       before its own scan finishes conservatively resolves to escaping),
       2 = done. See compute_param_borrowed. */
    struct {
        char fn_name[COBRA_MAX_IDENT_LEN];
        char param_name[COBRA_MAX_IDENT_LEN];
        int state;
        bool borrowed;
    } param_summary_cache[512];
    int param_summary_count;
} CodeGen;

static const char *SYSV_REGS[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
static const char *WIN_REGS[] = {"rcx", "rdx", "r8", "r9"};
static const char *SYSV_XMM_REGS[] = {"xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7"};

static bool expression_is_comparison(ASTNode *n) {
    if (!n || n->type != AST_BINARY_OP) return false;
    return !strcmp(n->name, "==") || !strcmp(n->name, "!=") ||
           !strcmp(n->name, "<") || !strcmp(n->name, ">") ||
           !strcmp(n->name, "<=") || !strcmp(n->name, ">=");
}

static bool expression_is_float(ASTNode *n) {
    if (!n) return false;
    if (n->type == AST_FLOAT_LITERAL || n->value_type == COBRA_TYPE_F32) return true;
    if (n->type == AST_FUNC_CALL &&
        (!strcmp(n->name, "sum_f32") || !strcmp(n->name, "mean_f32") ||
         !strcmp(n->name, "max_f32") || !strcmp(n->name, "exp_f32") ||
         !strcmp(n->name, "sqrt_f32") || !strcmp(n->name, "tanh_f32") ||
         !strcmp(n->name, "log_f32") || !strcmp(n->name, "pow_f32"))) return true;
    if (n->type == AST_BINARY_OP && !expression_is_comparison(n) && n->child_count >= 2)
        return expression_is_float(n->children[0]) || expression_is_float(n->children[1]);
    return false;
}
static const char *param_reg(CodeGen *cg, int index) {
    if (cg->target == TARGET_WIN64_X86_64) return index < 4 ? WIN_REGS[index] : "rcx";
    return index < 6 ? SYSV_REGS[index] : "rdi";
}

static ASTNode *find_function(CodeGen *cg, const char *name) {
    if (!cg->root) return NULL;
    for (size_t i = 0; i < cg->root->child_count; i++) {
        ASTNode *n = cg->root->children[i];
        if (n->type == AST_FUNCTION && strcmp(n->name, name) == 0) return n;
    }
    return NULL;
}

/* Emits, once per function name, a static {adapter_stub, 0} thunk so a plain
   top-level function used as an fn(...)->... value has the same 16-byte
   {code_ptr, env_ptr} shape a closure's thunk has (see emit_call's
   is_indirect_call path, which always dereferences a thunk pointer and
   passes env_ptr as an implicit leading argument). The adapter drops that
   unused env_ptr and shifts the real integer arguments left by one GPR
   before jumping to the real function; float arguments arrive in xmm0.. and
   are untouched by the shift, so this is safe for any declared signature
   this backend supports (int/float scalars only). */
static void ensure_fn_thunk(CodeGen *cg, const char *name) {
    for (int i = 0; i < cg->fn_thunk_count; i++)
        if (!strcmp(cg->fn_thunks_emitted[i], name)) return;
    if (cg->fn_thunk_count < 64) snprintf(cg->fn_thunks_emitted[cg->fn_thunk_count++], COBRA_MAX_IDENT_LEN, "%.63s", name);
    if (cg->fn_thunk_pending_count < 16) snprintf(cg->fn_thunk_pending[cg->fn_thunk_pending_count++], COBRA_MAX_IDENT_LEN, "%.63s", name);
}

/* Prints the adapter/thunk bytes ensure_fn_thunk queued during the function
   just compiled. Must only be called right after that function's epilogue
   (see flush_pending_parallel's call site) - the emitted adapter contains a
   real `jmp`, so it must never sit reachable via fallthrough inside another
   function's body. */
static void flush_pending_fn_thunks(CodeGen *cg) {
    for (int i = 0; i < cg->fn_thunk_pending_count; i++) {
        const char *name = cg->fn_thunk_pending[i];
        fprintf(cg->out,
                "    .text\n"
                "__fnval_adapter_%s:\n"
                "    mov rdi, rsi\n    mov rsi, rdx\n    mov rdx, rcx\n    mov rcx, r8\n    mov r8, r9\n"
                "    jmp %s\n"
                "    .data\n"
                "__fnthunk_%s:\n    .quad __fnval_adapter_%s\n    .quad 0\n"
                "    .text\n",
                name, name, name, name);
    }
    cg->fn_thunk_pending_count = 0;
}

static bool is_imported_function(CodeGen *cg, const char *name) {
    for (int i = 0; i < cg->imported_function_count; i++)
        if (!strcmp(cg->imported_functions[i], name)) return true;
    return false;
}

static bool function_param_used_as_buffer(ASTNode *node, const char *name) {
    if (!node) return false;
    if (node->type == AST_FOR_LOOP && node->child_count > 0 &&
        node->children[0]->type == AST_VAR_REF &&
        strcmp(node->children[0]->name, name) == 0) return true;
    if (node->type == AST_ARRAY_INDEX && strcmp(node->name, name) == 0) return true;
    if (node->type == AST_LEN_EXPR && node->child_count > 0 &&
        node->children[0]->type == AST_VAR_REF &&
        strcmp(node->children[0]->name, name) == 0) return true;
    for (size_t i = 0; i < node->child_count; i++)
        if (function_param_used_as_buffer(node->children[i], name)) return true;
    return false;
}

static CobraTypeKind function_param_type(CodeGen *cg, const char *name, size_t wanted) {
    ASTNode *fn = find_function(cg, name);
    if (!fn) return COBRA_TYPE_UNKNOWN;
    size_t index = 0;
    for (size_t i = 0; i < fn->child_count; i++) {
        ASTNode *p = fn->children[i];
        if (p->type != AST_PARAM) continue;
        if (index++ == wanted) {
            if ((p->declared_type == COBRA_TYPE_UNTYPED || p->declared_type == COBRA_TYPE_V256) &&
                function_param_used_as_buffer(fn, p->name)) return COBRA_TYPE_SLICE;
            return p->declared_type;
        }
    }
    return COBRA_TYPE_UNKNOWN;
}

static int abi_slots(CobraTypeKind type) {
    /* Scalar f32 values use the floating-point register class and therefore
       consume no general-purpose ABI slots. Tensor descriptors are one pointer
       slot; slices remain the historical pointer+length pair. */
    if (type == COBRA_TYPE_F32 || type == COBRA_TYPE_F64) return 0;
    if (type == COBRA_TYPE_SLICE || type == COBRA_TYPE_SLICE_F32 || type == COBRA_TYPE_SLICE_U8) return 2;
    if (type == COBRA_TYPE_LIST) return 3; /* pointer, length, capacity */
    if (type == COBRA_TYPE_DICT) return 2; /* table pointer, logical length */
    if (type == COBRA_TYPE_OPTION || type == COBRA_TYPE_RESULT) return 1; /* pointer to caller-owned sum storage */
    return 1;
}

/* ABI classification is canonical-first. Untyped parameters keep the
   historical integer/buffer ABI because they have no declared type to record;
   a typed parameter without canonical metadata is an invariant violation. The
   legacy kind table remains only as a differential oracle until removal. */
static int abi_slots_for(CobraTypeKind type, const ASTNode *param) {
    const CobraType *canonical = param ? param->canonical_type : NULL;
    if (!canonical) {
        if (param && param->declared_type != COBRA_TYPE_UNTYPED) {
            fprintf(stderr,
                    "CodeGen Error: internal missing canonical metadata for '%s'\n",
                    param->name[0] ? param->name : "<param>");
            exit(EXIT_FAILURE);
        }
        return abi_slots(type);
    }
    int slots = cobra_type_abi_slots(canonical);
    int legacy = abi_slots(type);
    if (slots != legacy) {
        fprintf(stderr,
                "CodeGen Error: internal ABI slot drift for '%s' (legacy %d, canonical %d)\n",
                cobra_type_kind_name(type), legacy, slots);
        exit(EXIT_FAILURE);
    }
    return slots;
}

/* A parameter's alias contract comes directly from canonical mutability.
   Untyped parameters carry no descriptor and keep the default contract. */
static int param_alias_contract(const ASTNode *param) {
    const CobraType *canonical = param ? param->canonical_type : NULL;
    if (!canonical) return 0;
    if (canonical->mutability == COBRA_MUTABILITY_READONLY) return 1;
    if (canonical->mutability == COBRA_MUTABILITY_OUT) return 2;
    return 0;
}

static CobraTypeKind ast_element_kind(const ASTNode *node) {
    const CobraType *type = cobra_type_node_element(node);
    return type ? type->kind : COBRA_TYPE_UNTYPED;
}

static CobraTypeKind ast_error_kind(const ASTNode *node) {
    const CobraType *type = cobra_type_node_error(node);
    return type ? type->kind : COBRA_TYPE_UNTYPED;
}

static const char *ast_payload_name(const ASTNode *node) {
    return cobra_type_node_name(node);
}

static const char *ast_error_name(const ASTNode *node) {
    return cobra_type_node_error_name(node);
}

static ASTNode *function_param_node(CodeGen *cg, const char *name, size_t wanted) {
    ASTNode *fn = find_function(cg, name);
    if (!fn) return NULL;
    size_t index = 0;
    for (size_t i = 0; i < fn->child_count; i++) {
        ASTNode *p = fn->children[i];
        if (p->type != AST_PARAM) continue;
        if (index++ == wanted) return p;
    }
    return NULL;
}



static int function_stack_slot_count(CodeGen *cg, const char *name, bool hidden_sret) {
    ASTNode *fn = find_function(cg, name);
    if (!fn) return 0;
    int gpr = hidden_sret ? 1 : 0, xmm = 0, stack = 0;
    size_t index = 0;
    for (size_t i = 0; i < fn->child_count; i++) {
        ASTNode *p = fn->children[i];
        if (p->type != AST_PARAM) continue;
        CobraTypeKind type = function_param_type(cg, name, index++);
        int slots = abi_slots_for(type, p);
        if (type == COBRA_TYPE_F32 || type == COBRA_TYPE_F64) {
            if (xmm++ >= 8) stack++;
        } else if (gpr + slots > 6) {
            stack += slots;
        } else {
            gpr += slots;
        }
    }
    return stack;
}

static VarSymbol *find_symbol(CodeGen *cg, const char *name);
static int reserve(CodeGen *cg, int bytes);
static void emit_expr(CodeGen *cg, ASTNode *node);
static void emit_call(CodeGen *cg, ASTNode *node);
static void emit_failure(CodeGen *cg, const char *message);
static int current_iter(CodeGen *cg, const char *name);
static void emit_load_buffer_ptr(CodeGen *cg, const char *name, const char *reg);

static void emit_import_call(CodeGen *cg, ASTNode *n) {
    if (n->child_count > 6) {
        fprintf(stderr,
                "CodeGen Error: imported C function '%s' has %zu arguments; "
                "the current direct ABI bridge supports at most six integer/pointer arguments\n",
                n->name, n->child_count);
        exit(EXIT_FAILURE);
    }

    int temp = reserve(cg, (int)n->child_count * 8 + 8);
    for (size_t i = 0; i < n->child_count; i++) {
        ASTNode *arg = n->children[i];
        if (expression_is_float(arg)) {
            fprintf(stderr,
                    "CodeGen Error: imported C function '%s' received a floating-point "
                    "argument; declare a typed Cobra wrapper or use integer/pointer ABI values\n",
                    n->name);
            exit(EXIT_FAILURE);
        }
        emit_expr(cg, arg);
        fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", temp + (int)i * 8);
    }
    for (size_t i = 0; i < n->child_count; i++) {
        fprintf(cg->out, "    mov %s, QWORD PTR [rbp-%d]\n",
                SYSV_REGS[i], temp + (int)i * 8);
    }
    /* AL=0 is required for SysV variadic calls such as printf. */
    fprintf(cg->out, "    xor eax, eax\n    call %s@PLT\n", n->name);
    if (n->propagate_error) {
        if (cg->region_depth == 0) {
            fprintf(cg->out, "    test rax, rax\n    jne .Lpropagate_%d\n", cg->propagation_label);
        } else {
            /* A failing `?` call must release every region that is live at
               this call site before jumping to the shared propagate label.
               The status value is saved and restored around the destroys so
               it reaches the label intact. */
            int status_slot = reserve(cg, 8);
            int skip = cg->label_count++;
            fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n    test rax, rax\n    je .Lprop_ok_%d\n", status_slot, skip);
            for (int i = cg->region_depth - 1; i >= 0; i--) {
                if (!cg->regions[i].active) continue;
                fprintf(cg->out,
                        "    lea rdi, [rbp-%d]\n    call arena_destroy@PLT\n",
                        cg->regions[i].state_base);
            }
            fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    jmp .Lpropagate_%d\n.Lprop_ok_%d:\n", status_slot, cg->propagation_label, skip);
        }
    }
}

static void emit_return_epilogue(CodeGen *cg) {
    fprintf(cg->out, "    mov rbx, QWORD PTR [rbp-248]\n    mov rsp, rbp\n    pop rbp\n    ret\n");
}

/* Recursively free owned string/slice fields inside `canonical`, including
   ones nested at any depth through embedded (by-value) struct fields.
   `array_base` is the containing local's stack-slot base (as passed to
   `[rbp-N]`); `base_offset` accumulates the byte offset of the enclosing
   struct field(s) walked so far, since a nested field's own `offset` is
   relative to the start of its own struct, not the outer one. */
static void emit_struct_owned_field_frees(CodeGen *cg, const CobraType *canonical,
                                          int array_base, int base_offset, int depth) {
    if (!canonical || depth > 8) return;
    for (size_t f = 0; f < canonical->field_count; f++) {
        const CobraTypeField *field = &canonical->fields[f];
        if (!field->type) continue;
        int field_offset = base_offset + (int)field->offset;
        if (field->ownership == COBRA_OWNERSHIP_OWNED &&
            (field->type->kind == COBRA_TYPE_STRING || cobra_type_is_slice_kind(field->type->kind))) {
            int field_addr = array_base - field_offset;
            fprintf(cg->out,
                    "    mov rdi, QWORD PTR [rbp-%d]\n    cmp rdi, 0\n    je .Lautofree_skip_%d\n    call free@PLT\n.Lautofree_skip_%d:\n",
                    field_addr, cg->label_count, cg->label_count);
            cg->label_count++;
        } else if (field->type->kind == COBRA_TYPE_STRUCT) {
            emit_struct_owned_field_frees(cg, field->type, array_base, field_offset, depth + 1);
        }
    }
}

/* Everyday owned values are reclaimed at function exit. Raw slices and tensor
   views deliberately stay outside this path: their lifetimes are explicit and
   their pointer/length ABI remains zero-overhead. */
static void emit_scope_cleanup(CodeGen *cg, const char *skip_name) {
    for (int i = 0; i < cg->symbol_count; i++) {
        VarSymbol *s = &cg->symbols[i];
        if (!s->owned || (skip_name && strcmp(skip_name, s->name) == 0)) continue;
        if (s->kind == SYM_LIST) {
            fprintf(cg->out,
                    "    lea rdi, [rbp-%d]\n    lea rsi, [rbp-%d]\n    lea rdx, [rbp-%d]\n    call cobra_list_free@PLT\n",
                    s->offset, s->length_offset, s->capacity_offset);
        } else if (s->kind == SYM_DICT) {
            fprintf(cg->out,
                    "    lea rdi, [rbp-%d]\n    lea rsi, [rbp-%d]\n    call cobra_dict_free@PLT\n",
                    s->offset, s->length_offset);
        } else if (s->type == COBRA_TYPE_STRING) {
            fprintf(cg->out,
                    "    mov rdi, QWORD PTR [rbp-%d]\n    call free@PLT\n    mov QWORD PTR [rbp-%d], 0\n",
                    s->offset, s->offset);
        } else if (s->kind == SYM_STRUCT) {
            const CobraType *canonical = (cg->root && cg->root->canonical_arena)
                ? cobra_type_struct_layout(cg->root->canonical_arena, cg->root, s->type_name) : NULL;
            if (!canonical) continue;
            emit_struct_owned_field_frees(cg, canonical, s->array_base, 0, 0);
        }
    }
}

static void emit_cleanup_preserving_result(CodeGen *cg, const char *skip_name,
                                           bool has_result, bool float_result) {
    if (has_result) {
        if (float_result) fprintf(cg->out, "    movss DWORD PTR [rbp-%d], xmm0\n", COBRA_SCR_TMP + 8);
        else fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", COBRA_SCR_TMP);
    }
    emit_scope_cleanup(cg, skip_name);
    /* Early returns inside `with region` bodies must release every live region
       before leaving the frame, inside the save/restore window so the call
       cannot clobber the preserved result. */
    for (int i = cg->region_depth - 1; i >= 0; i--) {
        if (!cg->regions[i].active) continue;
        fprintf(cg->out,
                "    lea rdi, [rbp-%d]\n    call arena_destroy@PLT\n",
                cg->regions[i].state_base);
    }
    if (has_result) {
        if (float_result) fprintf(cg->out, "    movss xmm0, DWORD PTR [rbp-%d]\n", COBRA_SCR_TMP + 8);
        else fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n", COBRA_SCR_TMP);
    }
}

static int sum_component_size(CodeGen *cg, CobraTypeKind type, const char *type_name);
static void emit_copy_memory(CodeGen *cg, const char *source_reg, const char *dest_reg, int bytes);
static void emit_zero_memory(CodeGen *cg, const char *base_reg, int bytes);
static void emit_sum_copy_ptr(CodeGen *cg, const char *source_reg, const char *dest_reg,
                              CobraTypeKind type, int payload_size, int error_size);
static void emit_sum_constructor(CodeGen *cg, ASTNode *node, const char *dest_reg,
                                 CobraTypeKind type, CobraTypeKind payload_type,
                                 CobraTypeKind error_type, const char *payload_name,
                                 const char *error_name);

static int struct_storage_size(CodeGen *cg, const char *type_name);

static void emit_tensor_return(CodeGen *cg, ASTNode *value) {
    if (!value) {
        fprintf(stderr, "CodeGen Error: tensor return requires a value\n");
        exit(EXIT_FAILURE);
    }
    /* The hidden sret pointer is saved in the fixed scratch slot. Copy only
       descriptor metadata; tensor storage is shared. A nested tensor-returning
       call is copied from its returned descriptor pointer instead of requiring
       an intermediate named local. */
    if (value->type == AST_VAR_REF) {
        VarSymbol *source = find_symbol(cg, value->name);
        if (!source || source->kind != SYM_TENSOR) {
            fprintf(stderr, "CodeGen Error: tensor return value must be a tensor variable\n");
            exit(EXIT_FAILURE);
        }
        for (int field = 0; field < COBRA_TENSOR_FIELDS; field++) {
            fprintf(cg->out, "    mov rdx, QWORD PTR [rbp-%d]\n    mov rax, QWORD PTR [rbp-240]\n    mov QWORD PTR [rax-%d], rdx\n",
                    source->offset + field * 8, field * 8);
        }
    } else if (value->type == AST_FUNC_CALL) {
        ASTNode *callee = find_function(cg, value->name);
        if (!callee || callee->declared_type != COBRA_TYPE_TENSOR_F32) {
            fprintf(stderr, "CodeGen Error: nested tensor return requires a tensor-returning function\n");
            exit(EXIT_FAILURE);
        }
        emit_call(cg, value);
        fprintf(cg->out, "    mov rsi, rax\n");
        for (int field = 0; field < COBRA_TENSOR_FIELDS; field++) {
            fprintf(cg->out, "    mov rdx, QWORD PTR [rsi-%d]\n    mov rax, QWORD PTR [rbp-240]\n    mov QWORD PTR [rax-%d], rdx\n",
                    field * 8, field * 8);
        }
    } else {
        fprintf(stderr, "CodeGen Error: tensor return value must be a tensor variable or tensor call\n");
        exit(EXIT_FAILURE);
    }
    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-240]\n");
}

static void emit_struct_return(CodeGen *cg, ASTNode *value) {
    if (!value || !cg->current_return_type_name[0]) {
        fprintf(stderr, "CodeGen Error: struct return requires a typed value\n");
        exit(EXIT_FAILURE);
    }
    emit_expr(cg, value);
    fprintf(cg->out, "    mov rsi, rax\n    mov rdi, QWORD PTR [rbp-240]\n");
    emit_copy_memory(cg, "rsi", "rdi",
                     struct_storage_size(cg, cg->current_return_type_name));
    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-240]\n");
}

static void emit_sum_return(CodeGen *cg, ASTNode *value, CobraTypeKind type) {
    if (!value) {
        fprintf(stderr, "CodeGen Error: Option or Result return requires a value\n");
        exit(EXIT_FAILURE);
    }
    int own_payload_size = sum_component_size(cg, cg->current_return_payload_type,
                                               cg->current_return_type_name);
    int own_error_size = type == COBRA_TYPE_RESULT ?
        sum_component_size(cg, cg->current_return_error_type, cg->current_return_error_type_name) : 0;
    if (value->type == AST_FUNC_CALL && value->propagate_error) {
        ASTNode *callee = find_function(cg, value->name);
        if (callee && (callee->declared_type == COBRA_TYPE_OPTION ||
                       callee->declared_type == COBRA_TYPE_RESULT) &&
            callee->declared_type == type) {
            /* emit_call exposes a successful scalar payload in rax/xmm0 and a
               successful struct payload as a pointer to its copied bytes. */
            emit_call(cg, value);
            fprintf(cg->out, "    mov rdi, QWORD PTR [rbp-240]\n    mov QWORD PTR [rdi], 1\n");
            if (ast_element_kind(callee) == COBRA_TYPE_STRUCT) {
                fprintf(cg->out, "    mov rsi, rax\n    lea rdi, [rdi-%d]\n", own_payload_size);
                emit_copy_memory(cg, "rsi", "rdi", own_payload_size);
            } else if (ast_element_kind(callee) == COBRA_TYPE_F32) {
                fprintf(cg->out, "    movss DWORD PTR [rdi-%d], xmm0\n", own_payload_size);
            } else {
                fprintf(cg->out, "    mov QWORD PTR [rdi-%d], rax\n", own_payload_size);
            }
            if (type == COBRA_TYPE_RESULT) {
                fprintf(cg->out, "    mov rdi, QWORD PTR [rbp-240]\n    lea rdi, [rdi-%d]\n", own_payload_size + own_error_size);
                emit_zero_memory(cg, "rdi", own_error_size);
            }
            fprintf(cg->out, "    mov rax, QWORD PTR [rbp-240]\n");
            return;
        }
    }
    bool typed_constructor = value->type == AST_FUNC_CALL &&
        (!strcmp(value->name, "some") || !strcmp(value->name, "none") ||
         !strcmp(value->name, "ok") || !strcmp(value->name, "err"));
    if (typed_constructor) {
        /* Constructor arguments describe their own scalar expression, not the
           enclosing return type. For Result[Struct, E], err(value) would
           otherwise allocate a scalar-sized temporary and place the error in
           the payload slot. Build the temporary with the function's declared
           sum layout so both variants share one ABI. */
        int total_size = COBRA_NATIVE_SUM_TAG_SIZE + own_payload_size + own_error_size;
        int temp = reserve(cg, total_size);
        int sum_ptr = temp - total_size + COBRA_NATIVE_SUM_TAG_SIZE;
        fprintf(cg->out, "    lea rdx, [rbp-%d]\n", sum_ptr);
        emit_sum_constructor(cg, value, "rdx", type,
                             cg->current_return_payload_type,
                             cg->current_return_error_type,
                             cg->current_return_type_name,
                             cg->current_return_error_type_name);
        fprintf(cg->out, "    lea rax, [rbp-%d]\n", sum_ptr);
    } else if (value->type == AST_NONE_LITERAL) {
        fprintf(cg->out, "    mov rax, QWORD PTR [rbp-240]\n    mov QWORD PTR [rax], 0\n");
        fprintf(cg->out, "    lea rdi, [rax-%d]\n", own_payload_size);
        emit_zero_memory(cg, "rdi", own_payload_size);
        if (type == COBRA_TYPE_RESULT) {
            fprintf(cg->out, "    lea rdi, [rax-%d]\n", own_payload_size + own_error_size);
            emit_zero_memory(cg, "rdi", own_error_size);
        }
        return;
    } else {
        emit_expr(cg, value);
    }
    fprintf(cg->out, "    mov rsi, rax\n    mov rdi, QWORD PTR [rbp-240]\n");
    /* A return-site constructor was lowered with the enclosing function's
       declared layout above. Other sum values already carry their own layout,
       but the destination ABI is the current function's layout. */
    emit_sum_copy_ptr(cg, "rsi", "rdi", type, own_payload_size, own_error_size);
    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-240]\n");
}

static VarSymbol *find_symbol(CodeGen *cg, const char *name) {
    for (int i = 0; i < cg->symbol_count; i++)
        if (strcmp(cg->symbols[i].name, name) == 0) return &cg->symbols[i];
    return NULL;
}

static bool expression_is_float_codegen(CodeGen *cg, ASTNode *node) {
    if (expression_is_float(node)) return true;
    if (!node) return false;
    if (node->type == AST_ARRAY_INDEX) {
        VarSymbol *s = find_symbol(cg, node->name);
        return s && ((s->kind == SYM_SLICE && s->type == COBRA_TYPE_SLICE_F32) ||
                     ((s->kind == SYM_LIST || s->kind == SYM_ARRAY) && s->element_type == COBRA_TYPE_F32) ||
                     s->kind == SYM_TENSOR);
    }
    if (node->type == AST_VAR_REF) {
        VarSymbol *s = find_symbol(cg, node->name);
        return s && s->kind == SYM_F32;
    }
    if (node->type == AST_FUNC_CALL && node->child_count == 1 &&
        node->children[0]->type == AST_VAR_REF) {
        VarSymbol *s = find_symbol(cg, node->children[0]->name);
        if (s && s->kind == SYM_OPTION && !strcmp(node->name, "unwrap"))
            return s->payload_type == COBRA_TYPE_F32;
        if (s && s->kind == SYM_RESULT && !strcmp(node->name, "unwrap_ok"))
            return s->payload_type == COBRA_TYPE_F32;
        if (s && s->kind == SYM_RESULT && !strcmp(node->name, "unwrap_err"))
            return s->error_type == COBRA_TYPE_F32;
    }
    return false;
}

static int reserve(CodeGen *cg, int bytes) {
    int aligned = (bytes + 7) & ~7;
    if (cg->stack_offset < COBRA_LOCAL_BASE) cg->stack_offset = COBRA_LOCAL_BASE;
    if (cg->stack_offset + aligned > COBRA_FRAME_LIMIT) {
        fprintf(stderr, "CodeGen Error: native frame exhausted\n"); exit(EXIT_FAILURE);
    }
    cg->stack_offset += aligned;
    return cg->stack_offset;
}

static VarSymbol *new_symbol(CodeGen *cg, const char *name) {
    if (cg->symbol_count >= 256) { fprintf(stderr, "CodeGen Error: symbol table exhausted\n"); exit(EXIT_FAILURE); }
    VarSymbol *s = &cg->symbols[cg->symbol_count++];
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", name);
    return s;
}

static VarSymbol *ensure_scalar(CodeGen *cg, const char *name, CobraTypeKind type) {
    VarSymbol *s = find_symbol(cg, name);
    if (s) { if (s->type == COBRA_TYPE_UNKNOWN) s->type = type; return s; }
    s = new_symbol(cg, name);
    s->kind = type == COBRA_TYPE_F32 ? SYM_F32 : SYM_SCALAR;
    s->type = type; s->offset = reserve(cg, 8); return s;
}

static bool is_sum_symbol_type(CobraTypeKind type) {
    return type == COBRA_TYPE_OPTION || type == COBRA_TYPE_RESULT;
}

static VarSymbol *ensure_sum(CodeGen *cg, const char *name, CobraTypeKind type,
                             CobraTypeKind payload_type, CobraTypeKind error_type,
                             const char *payload_name, const char *error_name) {
    VarSymbol *s = find_symbol(cg, name);
    if (s) return s;
    s = new_symbol(cg, name);
    s->kind = type == COBRA_TYPE_OPTION ? SYM_OPTION : SYM_RESULT;
    s->type = type;
    s->payload_type = payload_type;
    s->error_type = error_type;
    s->payload_size = sum_component_size(cg, payload_type, payload_name);
    s->error_size = type == COBRA_TYPE_RESULT ? sum_component_size(cg, error_type, error_name) : 0;
    snprintf(s->payload_type_name, sizeof(s->payload_type_name), "%.63s", payload_name ? payload_name : "");
    snprintf(s->error_type_name, sizeof(s->error_type_name), "%.63s", error_name ? error_name : "");
    /* The pointer to a sum names its tag. Payload and error bytes extend toward
       lower addresses, with fixed native alignment and no heap object. */
    int size = 8 + s->payload_size + s->error_size;
    s->offset = reserve(cg, size);
    s->tag_offset = s->offset - size + 8;
    s->payload_offset = s->tag_offset + s->payload_size;
    s->error_offset = type == COBRA_TYPE_RESULT ? s->tag_offset + s->payload_size + s->error_size : 0;
    return s;
}

static bool sum_payload_is_float(const VarSymbol *s, bool error_payload) {
    CobraTypeKind type = error_payload ? s->error_type : s->payload_type;
    return type == COBRA_TYPE_F32;
}

static void emit_sum_copy_ptr(CodeGen *cg, const char *source_reg, const char *dest_reg,
                              CobraTypeKind type, int payload_size, int error_size) {
    (void)type;
    int bytes = 8 + payload_size + error_size;
    for (int off = 0; off < bytes; off += 8)
        fprintf(cg->out, "    mov rax, QWORD PTR [%s-%d]\n    mov QWORD PTR [%s-%d], rax\n",
                source_reg, off, dest_reg, off);
}

static void emit_sum_constructor(CodeGen *cg, ASTNode *node, const char *dest_reg,
                                CobraTypeKind type, CobraTypeKind payload_type,
                                CobraTypeKind error_type, const char *payload_name,
                                const char *error_name) {
    bool is_none = !strcmp(node->name, "none");
    bool is_err = !strcmp(node->name, "err");
    int payload_size = sum_component_size(cg, payload_type, payload_name);
    int error_size = type == COBRA_TYPE_RESULT ? sum_component_size(cg, error_type, error_name) : 0;
    int saved_dest = reserve(cg, 8);
    fprintf(cg->out, "    mov QWORD PTR [rbp-%d], %s\n", saved_dest, dest_reg);
    fprintf(cg->out, "    mov QWORD PTR [%s], %d\n", dest_reg, (is_none || is_err) ? 0 : 1);
    fprintf(cg->out, "    lea rdi, [%s-%d]\n", dest_reg, payload_size);
    emit_zero_memory(cg, "rdi", payload_size);
    if (type == COBRA_TYPE_RESULT) {
        fprintf(cg->out, "    lea rdi, [%s-%d]\n", dest_reg, payload_size + error_size);
        emit_zero_memory(cg, "rdi", error_size);
    }
    if (is_none) return;
    if (node->child_count != 1) {
        fprintf(stderr, "CodeGen Error: %s requires one payload\n", node->name);
        exit(EXIT_FAILURE);
    }
    emit_expr(cg, node->children[0]);
    fprintf(cg->out, "    mov rdx, QWORD PTR [rbp-%d]\n", saved_dest);
    bool error_payload = type == COBRA_TYPE_RESULT && is_err;
    CobraTypeKind component_type = error_payload ? error_type : payload_type;
    int component_size = error_payload ? error_size : payload_size;
    int component_offset = error_payload ? payload_size + error_size : payload_size;
    if (component_type == COBRA_TYPE_STRUCT) {
        fprintf(cg->out, "    mov rsi, rax\n    lea rdi, [rdx-%d]\n", component_offset);
        emit_copy_memory(cg, "rsi", "rdi", component_size);
    } else if (component_type == COBRA_TYPE_F32 || expression_is_float_codegen(cg, node->children[0])) {
        fprintf(cg->out, "    movss DWORD PTR [rdx-%d], xmm0\n", component_offset);
    } else {
        fprintf(cg->out, "    mov QWORD PTR [rdx-%d], rax\n", component_offset);
    }
}

static void emit_sum_accessor(CodeGen *cg, ASTNode *node) {
    if (node->child_count != 1 || node->children[0]->type != AST_VAR_REF) {
        fprintf(stderr, "CodeGen Error: Option and Result operations require a named value\n");
        exit(EXIT_FAILURE);
    }
    VarSymbol *s = find_symbol(cg, node->children[0]->name);
    if (!s || !is_sum_symbol_type(s->type)) {
        fprintf(stderr, "CodeGen Error: '%s' is not an Option or Result\n", node->children[0]->name);
        exit(EXIT_FAILURE);
    }
    bool option = s->type == COBRA_TYPE_OPTION;
    bool predicate = !strcmp(node->name, "is_some") || !strcmp(node->name, "is_ok");
    bool error_value = !strcmp(node->name, "unwrap_err");
    int ok_label = cg->label_count++;
    int fail_label = cg->label_count++;
    fprintf(cg->out, "    cmp QWORD PTR [rbp-%d], %d\n", s->tag_offset, option ? 1 : (error_value ? 0 : 1));
    if (predicate) {
        fprintf(cg->out, "    sete al\n    movzx eax, al\n");
        return;
    }
    /* Both unwrap directions branch to the valid variant. Keep the failure
       path out of the fall-through path so unwrap(some) cannot report none. */
    fprintf(cg->out, "    je .Lsum_ok_%d\n    jmp .Lsum_fail_%d\n.Lsum_fail_%d:\n",
            ok_label, fail_label, fail_label);
    if (error_value) emit_failure(cg, "unwrap_err called on ok");
    else emit_failure(cg, option ? "unwrap called on none" : "unwrap called on err");
    fprintf(cg->out, ".Lsum_ok_%d:\n", ok_label);
    int value_offset = error_value ? s->error_offset : s->payload_offset;
    CobraTypeKind value_type = error_value ? s->error_type : s->payload_type;
    if (value_type == COBRA_TYPE_STRUCT)
        fprintf(cg->out, "    lea rax, [rbp-%d]\n", value_offset);
    else if ((error_value ? sum_payload_is_float(s, true) : sum_payload_is_float(s, false)))
        fprintf(cg->out, "    movss xmm0, DWORD PTR [rbp-%d]\n", value_offset);
    else
        fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n", value_offset);
}

static VarSymbol *ensure_slice(CodeGen *cg, const char *name, CobraTypeKind type) {
    VarSymbol *s = find_symbol(cg, name);
    if (s) return s;
    s = new_symbol(cg, name); s->kind = SYM_SLICE; s->type = type;
    s->offset = reserve(cg, 8); s->length_offset = reserve(cg, 8); return s;
}

static VarSymbol *ensure_list(CodeGen *cg, const char *name, CobraTypeKind element_type) {
    VarSymbol *s = find_symbol(cg, name);
    if (s) return s;
    s = new_symbol(cg, name);
    s->kind = SYM_LIST;
    s->type = COBRA_TYPE_LIST;
    s->element_type = element_type;
    s->offset = reserve(cg, 8);
    s->length_offset = reserve(cg, 8);
    s->capacity_offset = reserve(cg, 8);
    return s;
}

static VarSymbol *ensure_dict(CodeGen *cg, const char *name) {
    VarSymbol *s = find_symbol(cg, name);
    if (s) return s;
    s = new_symbol(cg, name);
    s->kind = SYM_DICT;
    s->type = COBRA_TYPE_DICT;
    s->offset = reserve(cg, 8);
    s->length_offset = reserve(cg, 8);
    return s;
}

static int field_offset_for(CodeGen *cg, const char *struct_name,
                            const char *field_name);

static void emit_struct_address(CodeGen *cg, ASTNode *node) {
    if (!node) {
        fprintf(stderr, "CodeGen Error: missing struct expression\n");
        exit(EXIT_FAILURE);
    }
    if (node->type == AST_VAR_REF) {
        int loop = current_iter(cg, node->name);
        if (loop >= 0 && cg->loops[loop].source[0] != '\0') {
            /* Struct-element `for p in list_of_struct:` loop variable: the
               list stores each element as a pointer (see emit_list_append),
               so loading the slot value already yields the struct's address. */
            fprintf(cg->out, "    mov rdx, QWORD PTR [rbp-%d]\n", cg->loops[loop].index_offset);
            emit_load_buffer_ptr(cg, cg->loops[loop].source, "rbx");
            fprintf(cg->out, "    mov rax, QWORD PTR [rbx + rdx*8]\n");
            return;
        }
        VarSymbol *symbol = find_symbol(cg, node->name);
        if (!symbol || symbol->kind != SYM_STRUCT) {
            fprintf(stderr, "CodeGen Error: '%s' is not a struct value\n", node->name);
            exit(EXIT_FAILURE);
        }
        if (symbol->indirect)
            fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n", symbol->array_base);
        else
            fprintf(cg->out, "    lea rax, [rbp-%d]\n", symbol->array_base);
        return;
    }
    if (node->type == AST_MEMBER_ACCESS && node->value_type == COBRA_TYPE_STRUCT &&
        node->child_count > 0) {
        emit_struct_address(cg, node->children[0]);
        fprintf(cg->out, "    add rax, %d\n",
                field_offset_for(cg, ast_payload_name(node->children[0]),
                                 node->secondary_name));
        return;
    }
    fprintf(stderr, "CodeGen Error: struct address requires a struct value\n");
    exit(EXIT_FAILURE);
}

/* Struct storage is canonical-only. The canonical descriptor's packed size is
   authoritative; the legacy layout module remains solely as a differential
   oracle so any drift fails loudly instead of silently changing struct bytes. */
static int struct_storage_size(CodeGen *cg, const char *type_name) {
    const CobraType *canonical = (cg->root && cg->root->canonical_arena)
        ? cobra_type_struct_layout(cg->root->canonical_arena, cg->root, type_name)
        : NULL;
    if (!canonical) {
        fprintf(stderr, "CodeGen Error: internal missing canonical layout for struct '%s'\n",
                type_name ? type_name : "<null>");
        exit(EXIT_FAILURE);
    }
    return (int)canonical->size;
}

/* Field offsets are canonical-only. The struct name comes from the base
   expression (or the owner symbol for byte-view indexing) and the field name
   from secondary_name. Missing metadata is an invariant violation. */
static int field_offset_for(CodeGen *cg, const char *struct_name,
                            const char *field_name) {
    if (!cg->root || !cg->root->canonical_arena) {
        fprintf(stderr, "CodeGen Error: internal missing canonical arena for field offset\n");
        exit(EXIT_FAILURE);
    }
    int offset = cobra_type_field_offset(cg->root->canonical_arena, cg->root,
                                         struct_name, field_name);
    if (offset < 0) {
        fprintf(stderr,
                "CodeGen Error: internal missing canonical offset for field '%s.%s'\n",
                struct_name ? struct_name : "<null>", field_name ? field_name : "<null>");
        exit(EXIT_FAILURE);
    }
    return offset;
}

static int sum_component_size(CodeGen *cg, CobraTypeKind type, const char *type_name) {
    if (type == COBRA_TYPE_STRUCT) return struct_storage_size(cg, type_name);
    return COBRA_NATIVE_SUM_SCALAR_SIZE;
}

static void emit_copy_memory(CodeGen *cg, const char *source_reg, const char *dest_reg, int bytes) {
    for (int off = 0; off < bytes; off += 8) {
        fprintf(cg->out, "    mov rax, QWORD PTR [%s+%d]\n    mov QWORD PTR [%s+%d], rax\n",
                source_reg, off, dest_reg, off);
    }
}

static void emit_zero_memory(CodeGen *cg, const char *base_reg, int bytes) {
    for (int off = 0; off < bytes; off += 8)
        fprintf(cg->out, "    mov QWORD PTR [%s+%d], 0\n", base_reg, off);
}

static VarSymbol *ensure_struct(CodeGen *cg, const char *name, const char *type_name) {
    VarSymbol *s = find_symbol(cg, name);
    if (s) return s;
    int size = struct_storage_size(cg, type_name);
    s = new_symbol(cg, name);
    s->kind = SYM_STRUCT;
    s->type = COBRA_TYPE_STRUCT;
    snprintf(s->type_name, sizeof(s->type_name), "%.63s", type_name);
    s->array_base = reserve(cg, size);
    return s;
}

static VarSymbol *ensure_array(CodeGen *cg, const char *name, int count, CobraTypeKind element_type) {
    VarSymbol *s = find_symbol(cg, name);
    if (s) return s;
    s = new_symbol(cg, name); s->kind = SYM_ARRAY; s->type = COBRA_TYPE_ARRAY;
    s->element_type = element_type;
    s->offset = reserve(cg, 8); s->array_len = count; s->array_base = reserve(cg, count * 8); return s;
}

static VarSymbol *ensure_tensor(CodeGen *cg, const char *name) {
    VarSymbol *s = find_symbol(cg, name);
    if (s) return s;
    s = new_symbol(cg, name); s->kind = SYM_TENSOR; s->type = COBRA_TYPE_TENSOR_F32;
    /* Keep descriptor fields contiguous so a tensor parameter can be passed as
       one pointer without copying data or allocating a runtime object. `reserve`
       returns the end offset, so advance the allocator past every field rather
       than letting later expression scratch reuse descriptor metadata. */
    if (cg->stack_offset < COBRA_LOCAL_BASE) cg->stack_offset = COBRA_LOCAL_BASE;
    int base = cg->stack_offset;
    cg->stack_offset += COBRA_TENSOR_FIELDS * 8;
    s->offset = base;
    s->length_offset = base + 8;
    s->rank_offset = base + 16;
    for (int i = 0; i < COBRA_VIEW_MAX_RANK; i++) s->dim_offsets[i] = base + (3 + i) * 8;
    for (int i = 0; i < COBRA_VIEW_MAX_RANK; i++) s->stride_offsets[i] = base + (3 + COBRA_VIEW_MAX_RANK + i) * 8;
    return s;
}

static bool symbol_is_buffer(CodeGen *cg, const char *name) {
    VarSymbol *s = find_symbol(cg, name);
    return s && (s->kind == SYM_SLICE || s->kind == SYM_ARRAY || s->kind == SYM_TENSOR || s->kind == SYM_LIST);
}
static int current_iter(CodeGen *cg, const char *name) {
    for (int i = cg->loop_depth - 1; i >= 0; i--)
        if (cg->loops[i].active &&
            (strcmp(cg->loops[i].name, name) == 0 ||
             strcmp(cg->loops[i].secondary_name, name) == 0)) return i;
    return -1;
}

static RegionInfo *region_by_name(CodeGen *cg, const char *name) {
    if (!name || !*name) return NULL;
    for (int i = cg->region_depth - 1; i >= 0; i--) {
        if (cg->regions[i].active && strcmp(cg->regions[i].name, name) == 0) return &cg->regions[i];
    }
    return NULL;
}

static bool is_region_alloc(CodeGen *cg, ASTNode *n) {
    return n && n->qualifier[0] != '\0' && region_by_name(cg, n->qualifier) &&
           (!strcmp(n->name, "alloc_i64") || !strcmp(n->name, "alloc_f32") ||
            !strcmp(n->name, "alloc_u8"));
}

static void emit_failure(CodeGen *cg, const char *message);

/* scratch.alloc_i64(n), scratch.alloc_f32(n), and scratch.alloc_u8(n) bump storage out of the
   region and return the pointer+length pair in rax/rdx. A zero
   arena_alloc result means the bump crossed the backing store's capacity. */
static void emit_region_alloc(CodeGen *cg, ASTNode *n) {
    RegionInfo *r = region_by_name(cg, n->qualifier);
    if (!r) {
        fprintf(stderr, "CodeGen Error: allocation from unknown region '%s'\n", n->qualifier);
        exit(EXIT_FAILURE);
    }
    if (n->child_count != 1) {
        fprintf(stderr, "CodeGen Error: region allocation requires exactly one count argument\n");
        exit(EXIT_FAILURE);
    }
    int fail_neg = cg->label_count++;
    int fail_oom = cg->label_count++;
    int done = cg->label_count++;
    int count = reserve(cg, 8);
    emit_expr(cg, n->children[0]);
    fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n    cmp rax, 0\n    jl .Lreg_neg_%d\n", count, fail_neg);
    const char *scale = !strcmp(n->name, "alloc_u8") ? "" :
                        (!strcmp(n->name, "alloc_f32") ? "shl rsi, 2" : "shl rsi, 3");
    /* ArenaState is an out struct. Pass its address in rdi and the byte count
       in rsi, matching the ordinary Cobra struct-parameter ABI. */
    fprintf(cg->out, "    lea rdi, [rbp-%d]\n    mov rsi, QWORD PTR [rbp-%d]\n    %s\n    call arena_alloc@PLT\n    test rax, rax\n    je .Lreg_oom_%d\n", r->state_base, count, scale, fail_oom);
    fprintf(cg->out, "    mov rdx, QWORD PTR [rbp-%d]\n    jmp .Lreg_done_%d\n", count, done);
    fprintf(cg->out, ".Lreg_neg_%d:\n", fail_neg);
    emit_failure(cg, "region allocation count must be non-negative");
    fprintf(cg->out, ".Lreg_oom_%d:\n", fail_oom);
    emit_failure(cg, "region exhausted");
    fprintf(cg->out, ".Lreg_done_%d:\n", done);
}

static void emit_expr(CodeGen *cg, ASTNode *node);
static void emit_statement(CodeGen *cg, ASTNode *node);
static void emit_call(CodeGen *cg, ASTNode *node);
static void emit_failure(CodeGen *cg, const char *message);

static void emit_string_literal(CodeGen *cg, const char *value) {
    int id = cg->string_count++;
    fputs("    .section .rodata", cg->out);
    fputc(10, cg->out);
    fprintf(cg->out, ".LC%d:", id);
    fputc(10, cg->out);
    fputs("    .string ", cg->out);
    fputc(34, cg->out);
    for (const unsigned char *p = (const unsigned char *)value; p && *p; p++) {
        if (*p == '\\') { fputc(92, cg->out); fputc(92, cg->out); }
        else if (*p == '"') { fputc(92, cg->out); fputc(34, cg->out); }
        else if (*p == 10) { fputc(92, cg->out); fputc('n', cg->out); }
        else if (*p == 13) { fputc(92, cg->out); fputc('r', cg->out); }
        else if (*p == 9) { fputc(92, cg->out); fputc('t', cg->out); }
        else if (*p < 32 || *p > 126) { fputc(92, cg->out); fprintf(cg->out, "%03o", *p); }
        else fputc(*p, cg->out);
    }
    fputc(34, cg->out);
    fputc(10, cg->out);
    fputs("    .text", cg->out);
    fputc(10, cg->out);
    fprintf(cg->out, "    lea rax, [rip + .LC%d]", id);
    fputc(10, cg->out);
}

static void emit_string_concat(CodeGen *cg, ASTNode *n) {
    int left = reserve(cg, 8), right = reserve(cg, 8);
    int left_len = reserve(cg, 8), right_len = reserve(cg, 8);
    int total = reserve(cg, 8), result = reserve(cg, 8);
    emit_expr(cg, n->children[0]);
    fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n    mov rdi, rax\n    call strlen@PLT\n    mov QWORD PTR [rbp-%d], rax\n", left, left_len);
    emit_expr(cg, n->children[1]);
    fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n    mov rdi, rax\n    call strlen@PLT\n    mov QWORD PTR [rbp-%d], rax\n", right, right_len);
    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    add rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n    inc rax\n    mov rdi, rax\n    call malloc@PLT\n    mov QWORD PTR [rbp-%d], rax\n", left_len, right_len, total, result);
    fprintf(cg->out, "    mov rdi, QWORD PTR [rbp-%d]\n    mov rsi, QWORD PTR [rbp-%d]\n    mov rdx, QWORD PTR [rbp-%d]\n    call memcpy@PLT\n", result, left, left_len);
    fprintf(cg->out, "    mov rdi, QWORD PTR [rbp-%d]\n    add rdi, QWORD PTR [rbp-%d]\n    mov rsi, QWORD PTR [rbp-%d]\n    mov rdx, QWORD PTR [rbp-%d]\n    call memcpy@PLT\n", result, left_len, right, right_len);
    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov rdx, QWORD PTR [rbp-%d]\n    mov BYTE PTR [rax + rdx], 0\n", result, total);
}

static void emit_string_compare(CodeGen *cg, ASTNode *n) {
    int left = reserve(cg, 8), right = reserve(cg, 8);
    emit_expr(cg, n->children[0]); fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", left);
    emit_expr(cg, n->children[1]); fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", right);
    fprintf(cg->out, "    mov rdi, QWORD PTR [rbp-%d]\n    mov rsi, QWORD PTR [rbp-%d]\n    call strcmp@PLT\n    cmp eax, 0\n", left, right);
    const char *set = !strcmp(n->name, "==") ? "sete" : !strcmp(n->name, "!=") ? "setne" :
                      !strcmp(n->name, "<") ? "setl" : !strcmp(n->name, ">") ? "setg" :
                      !strcmp(n->name, "<=") ? "setle" : "setge";
    fprintf(cg->out, "    %s al\n    movzx eax, al\n", set);
}

static void emit_string_predicate(CodeGen *cg, ASTNode *n) {
    int text = reserve(cg, 8), pattern = reserve(cg, 8);
    emit_expr(cg, n->children[0]); fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", text);
    emit_expr(cg, n->children[1]); fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", pattern);
    if (!strcmp(n->name, "contains")) {
        fprintf(cg->out, "    mov rdi, QWORD PTR [rbp-%d]\n    mov rsi, QWORD PTR [rbp-%d]\n    call strstr@PLT\n    test rax, rax\n    setne al\n    movzx eax, al\n", text, pattern);
    } else {
        int text_len = reserve(cg, 8), pattern_len = reserve(cg, 8), fail = cg->label_count++;
        fprintf(cg->out, "    mov rdi, QWORD PTR [rbp-%d]\n    call strlen@PLT\n    mov QWORD PTR [rbp-%d], rax\n    mov rdi, QWORD PTR [rbp-%d]\n    call strlen@PLT\n    mov QWORD PTR [rbp-%d], rax\n", text, text_len, pattern, pattern_len);
        if (!strcmp(n->name, "starts_with")) {
            fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    cmp rax, QWORD PTR [rbp-%d]\n    jb .Lstr_pred_false_%d\n    mov rdi, QWORD PTR [rbp-%d]\n    mov rsi, QWORD PTR [rbp-%d]\n    mov rdx, QWORD PTR [rbp-%d]\n    call strncmp@PLT\n", text_len, pattern_len, fail, text, pattern, pattern_len);
        } else {
            fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    cmp rax, QWORD PTR [rbp-%d]\n    jb .Lstr_pred_false_%d\n    sub rax, QWORD PTR [rbp-%d]\n    add rax, QWORD PTR [rbp-%d]\n    mov rdi, rax\n    mov rsi, QWORD PTR [rbp-%d]\n    mov rdx, QWORD PTR [rbp-%d]\n    call strncmp@PLT\n", text_len, pattern_len, fail, pattern_len, text, pattern, pattern_len);
        }
        fprintf(cg->out, "    test eax, eax\n    sete al\n    movzx eax, al\n    jmp .Lstr_pred_done_%d\n.Lstr_pred_false_%d:\n    xor eax, eax\n.Lstr_pred_done_%d:\n", fail, fail, fail);
    }
}

static void emit_string_char_at(CodeGen *cg, ASTNode *n) {
    int text = reserve(cg, 8), index = reserve(cg, 8), length = reserve(cg, 8), fail = cg->label_count++;
    emit_expr(cg, n->children[0]); fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", text);
    emit_expr(cg, n->children[1]); fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n    cmp rax, 0\n    jl .Lstr_char_fail_%d\n", index, fail);
    fprintf(cg->out, "    mov rdi, QWORD PTR [rbp-%d]\n    call strlen@PLT\n    mov QWORD PTR [rbp-%d], rax\n    cmp QWORD PTR [rbp-%d], rax\n    jae .Lstr_char_fail_%d\n    mov rax, QWORD PTR [rbp-%d]\n    mov rdx, QWORD PTR [rbp-%d]\n    movzx eax, BYTE PTR [rax + rdx]\n    jmp .Lstr_char_done_%d\n.Lstr_char_fail_%d:\n", text, length, index, fail, text, index, fail, fail);
    emit_failure(cg, "string index out of bounds");
    fprintf(cg->out, ".Lstr_char_done_%d:\n", fail);
}

static void emit_failure(CodeGen *cg, const char *message) {
    int id = cg->string_count++;
    fprintf(cg->out, "    lea rdi, [rip + .LC%d]\n    call puts@PLT\n    mov edi, 1\n    call exit@PLT\n", id);
    fputs("    .section .rodata", cg->out);
    fputc(10, cg->out);
    fprintf(cg->out, ".LC%d:", id);
    fputc(10, cg->out);
    fputs("    .string ", cg->out);
    fputc(34, cg->out);
    fputs("[cobra] ", cg->out);
    fputs(message, cg->out);
    fputc(92, cg->out);
    fputc('n', cg->out);
    fputc(34, cg->out);
    fputc(10, cg->out);
    fputs("    .text", cg->out);
    fputc(10, cg->out);
}

static void emit_float_literal(CodeGen *cg, float value) {
    union { float f; uint32_t u; } bits; bits.f = value;
    fprintf(cg->out, "    mov eax, 0x%08x\n    movd xmm0, eax\n", bits.u);
}

static void emit_load_buffer_ptr(CodeGen *cg, const char *name, const char *reg) {
    VarSymbol *s = find_symbol(cg, name);
    if (!s) { fprintf(stderr, "CodeGen Error: undefined buffer '%s'\n", name ? name : "<null>"); exit(EXIT_FAILURE); }
    if (s->kind == SYM_ARRAY) fprintf(cg->out, "    lea %s, [rbp-%d]\n", reg, s->array_base);
    else fprintf(cg->out, "    mov %s, QWORD PTR [rbp-%d]\n", reg, s->offset);
}
static void emit_load_tensor_descriptor_ptr(CodeGen *cg, const char *name, const char *reg) {
    VarSymbol *s = find_symbol(cg, name);
    if (!s || s->kind != SYM_TENSOR) {
        fprintf(stderr, "CodeGen Error: '%s' is not a tensor descriptor\n", name ? name : "<null>");
        exit(EXIT_FAILURE);
    }
    /* Local fields are allocated at rbp-offset, so the descriptor's lowest
       address is its last field. The ABI pointer names the high-address end
       and fields are addressed with negative offsets. */
    fprintf(cg->out, "    lea %s, [rbp-%d]\n", reg, s->offset);
}

static void emit_copy_tensor_descriptor(CodeGen *cg, VarSymbol *dst, const char *ptr_reg) {
    if (!dst || dst->kind != SYM_TENSOR) return;
    for (int field = 0; field < COBRA_TENSOR_FIELDS; field++) {
        int offset = dst->offset + field * 8;
        fprintf(cg->out, "    mov rdx, QWORD PTR [%s-%d]\n    mov QWORD PTR [rbp-%d], rdx\n", ptr_reg, field * 8, offset);
    }
}

static void emit_load_buffer_len(CodeGen *cg, const char *name, const char *reg) {
    VarSymbol *s = find_symbol(cg, name);
    if (!s) { fprintf(stderr, "CodeGen Error: undefined buffer '%s'\n", name ? name : "<null>"); exit(EXIT_FAILURE); }
    if (s->kind == SYM_ARRAY) fprintf(cg->out, "    mov %s, %d\n", reg, s->array_len);
    else fprintf(cg->out, "    mov %s, QWORD PTR [rbp-%d]\n", reg, s->length_offset);
}

static void emit_list_append(CodeGen *cg, VarSymbol *s, ASTNode *value) {
    int temp = reserve(cg, 8);
    emit_expr(cg, value);
    if (s->element_type == COBRA_TYPE_F32) {
        if (!expression_is_float_codegen(cg, value)) fprintf(cg->out, "    cvtsi2ss xmm0, rax\n");
        fprintf(cg->out, "    movss DWORD PTR [rbp-%d], xmm0\n    lea rdi, [rbp-%d]\n    lea rsi, [rbp-%d]\n    lea rdx, [rbp-%d]\n    movss xmm0, DWORD PTR [rbp-%d]\n    call cobra_list_append_f32@PLT\n", temp, s->offset, s->length_offset, s->capacity_offset, temp);
    } else {
        fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n    lea rdi, [rbp-%d]\n    lea rsi, [rbp-%d]\n    lea rdx, [rbp-%d]\n    mov rcx, QWORD PTR [rbp-%d]\n    call cobra_list_append_i64@PLT\n", temp, s->offset, s->length_offset, s->capacity_offset, temp);
    }
}

static void emit_dict_set_key(CodeGen *cg, VarSymbol *s, const char *key, ASTNode *value) {
    int temp = reserve(cg, 8);
    emit_expr(cg, value);
    fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n    lea rdi, [rbp-%d]\n", temp, s->offset);
    emit_string_literal(cg, key);
    fprintf(cg->out, "    mov rsi, rax\n    mov rdx, QWORD PTR [rbp-%d]\n    call cobra_dict_set_i64@PLT\n    mov rdi, QWORD PTR [rbp-%d]\n    call cobra_dict_len@PLT\n    mov QWORD PTR [rbp-%d], rax\n", temp, s->offset, s->length_offset);
}

static void emit_dict_set(CodeGen *cg, VarSymbol *s, ASTNode *key, ASTNode *value) {
    int key_temp = reserve(cg, 8), value_temp = reserve(cg, 8);
    emit_expr(cg, key);
    fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", key_temp);
    emit_expr(cg, value);
    fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n    lea rdi, [rbp-%d]\n    mov rsi, QWORD PTR [rbp-%d]\n    mov rdx, QWORD PTR [rbp-%d]\n    call cobra_dict_set_i64@PLT\n    mov rdi, QWORD PTR [rbp-%d]\n    call cobra_dict_len@PLT\n    mov QWORD PTR [rbp-%d], rax\n", value_temp, s->offset, key_temp, value_temp, s->offset, s->length_offset);
}

static void emit_struct_field_index_read(CodeGen *cg, ASTNode *node) {
    if (!node || node->child_count != 1) {
        fprintf(stderr, "CodeGen Error: struct byte-view indexing requires one index\n");
        exit(EXIT_FAILURE);
    }
    VarSymbol *s = find_symbol(cg, node->name);
    if (!s || s->kind != SYM_STRUCT) {
        fprintf(stderr, "CodeGen Error: '%s' is not a struct value\n", node ? node->name : "<null>");
        exit(EXIT_FAILURE);
    }
    int index = reserve(cg, 8), fail = cg->label_count++;
    int offset = field_offset_for(cg, s->type_name, node->secondary_name);
    emit_expr(cg, node->children[0]);
    fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n    mov rax, QWORD PTR [rbp-%d]\n    cmp rax, 0\n    jl .Lstruct_view_index_fail_%d\n",
            index, index, fail);
    fprintf(cg->out, "    lea rbx, [rbp-%d]\n    mov rcx, QWORD PTR [rbx + %d]\n    cmp rax, rcx\n    jae .Lstruct_view_index_fail_%d\n    mov rbx, QWORD PTR [rbx + %d]\n    movzx eax, BYTE PTR [rbx + rax]\n    jmp .Lstruct_view_index_done_%d\n.Lstruct_view_index_fail_%d:\n",
            s->array_base, offset + 8, fail, offset, fail, fail);
    emit_failure(cg, "byte-view index out of bounds");
    fprintf(cg->out, ".Lstruct_view_index_done_%d:\n", fail);
}

static void emit_index_read(CodeGen *cg, const char *name, ASTNode **indices, size_t count) {
    VarSymbol *s = find_symbol(cg, name);
    if (!s) { fprintf(stderr, "CodeGen Error: undefined indexed value '%s'\n", name); exit(EXIT_FAILURE); }
    if (s->kind == SYM_DICT) {
        if (count != 1) { fprintf(stderr, "CodeGen Error: dict indexing requires one key\n"); exit(EXIT_FAILURE); }
        /* Save the dict pointer before evaluating the key: a call-bearing key
           expression would otherwise clobber the caller-saved rdi. */
        int dict_ptr = reserve(cg, 8);
        fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", s->offset, dict_ptr);
        emit_expr(cg, indices[0]);
        fprintf(cg->out, "    mov rsi, rax\n    mov rdi, QWORD PTR [rbp-%d]\n    xor edx, edx\n    call cobra_dict_get_i64@PLT\n", dict_ptr);
        return;
    }
    if (s->kind == SYM_TENSOR && count > 2) {
        fprintf(stderr, "CodeGen Error: tensor indexing currently supports at most two axes\n");
        exit(EXIT_FAILURE);
    }
    int fail = cg->label_count++;
    emit_expr(cg, indices[0]); fprintf(cg->out, "    mov rdx, rax\n");
    if (s->kind == SYM_TENSOR && count > 1) {
        int i0 = reserve(cg, 8), i1 = reserve(cg, 8);
        fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rdx\n    cmp QWORD PTR [rbp-%d], 2\n    jne .Lidx_fail_%d\n    cmp rdx, QWORD PTR [rbp-%d]\n    jae .Lidx_fail_%d\n", i0, s->rank_offset, fail, s->dim_offsets[0], fail);
        emit_expr(cg, indices[1]); fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n    cmp rax, QWORD PTR [rbp-%d]\n    jae .Lidx_fail_%d\n", i1, s->dim_offsets[1], fail);
        fprintf(cg->out, "    mov rdx, QWORD PTR [rbp-%d]\n    imul rdx, QWORD PTR [rbp-%d]\n    mov rcx, QWORD PTR [rbp-%d]\n    imul rcx, QWORD PTR [rbp-%d]\n    add rdx, rcx\n", i0, s->stride_offsets[0], i1, s->stride_offsets[1]);
    } else {
        fprintf(cg->out, "    cmp rdx, QWORD PTR [rbp-%d]\n    jae .Lidx_fail_%d\n", s->length_offset, fail);
    }
    emit_load_buffer_ptr(cg, name, "rbx");
    if (s->kind == SYM_SLICE && s->type == COBRA_TYPE_SLICE_U8)
        fprintf(cg->out, "    movzx eax, BYTE PTR [rbx + rdx]\n");
    else if ((s->kind == SYM_SLICE && s->type == COBRA_TYPE_SLICE_F32) ||
             ((s->kind == SYM_LIST || s->kind == SYM_ARRAY) && s->element_type == COBRA_TYPE_F32) ||
             s->kind == SYM_TENSOR)
        fprintf(cg->out, "    movss xmm0, DWORD PTR [rbx + rdx*4]\n");
    else fprintf(cg->out, "    mov rax, QWORD PTR [rbx + rdx*8]\n");
    fprintf(cg->out, "    jmp .Lidx_done_%d\n.Lidx_fail_%d:\n", fail, fail);
    emit_failure(cg, "buffer index out of bounds");
    fprintf(cg->out, ".Lidx_done_%d:\n", fail);
}

static void emit_struct_field_index_store(CodeGen *cg, ASTNode *node, ASTNode *value) {
    if (!node || node->child_count != 2) {
        fprintf(stderr, "CodeGen Error: struct byte-view assignment requires one index\n");
        exit(EXIT_FAILURE);
    }
    VarSymbol *s = find_symbol(cg, node->name);
    if (!s || s->kind != SYM_STRUCT) {
        fprintf(stderr, "CodeGen Error: '%s' is not a struct value\n", node ? node->name : "<null>");
        exit(EXIT_FAILURE);
    }
    int index = reserve(cg, 8), stored = reserve(cg, 8), fail = cg->label_count++;
    int offset = field_offset_for(cg, s->type_name, node->secondary_name);
    emit_expr(cg, value);
    fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", stored);
    emit_expr(cg, node->children[0]);
    fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n    lea rbx, [rbp-%d]\n    mov rcx, QWORD PTR [rbx + %d]\n    cmp QWORD PTR [rbp-%d], 0\n    jl .Lstruct_view_store_fail_%d\n    cmp QWORD PTR [rbp-%d], rcx\n    jae .Lstruct_view_store_fail_%d\n    mov rbx, QWORD PTR [rbx + %d]\n    mov rax, QWORD PTR [rbp-%d]\n    mov rcx, QWORD PTR [rbp-%d]\n    mov BYTE PTR [rbx + rax], cl\n    jmp .Lstruct_view_store_done_%d\n.Lstruct_view_store_fail_%d:\n",
            index, s->array_base, offset + 8, index, fail, index, fail,
            offset, index, stored, fail, fail);
    emit_failure(cg, "byte-view index out of bounds");
    fprintf(cg->out, ".Lstruct_view_store_done_%d:\n", fail);
}

static void emit_index_store(CodeGen *cg, const char *name, ASTNode **indices, size_t count, ASTNode *value) {
    VarSymbol *s = find_symbol(cg, name);
    if (!s) { fprintf(stderr, "CodeGen Error: undefined indexed value '%s'\n", name); exit(EXIT_FAILURE); }
    if (s->kind == SYM_DICT) {
        if (count != 1) { fprintf(stderr, "CodeGen Error: dict assignment requires one key\n"); exit(EXIT_FAILURE); }
        emit_dict_set(cg, s, indices[0], value);
        return;
    }
    if (s->kind == SYM_TENSOR && count > 2) {
        fprintf(stderr, "CodeGen Error: tensor indexing currently supports at most two axes\n");
        exit(EXIT_FAILURE);
    }
    int fail = cg->label_count++;
    int address = reserve(cg, 8);
    emit_expr(cg, indices[0]); fprintf(cg->out, "    mov rdx, rax\n");
    if (s->kind == SYM_TENSOR && count > 1) {
        int i0 = reserve(cg, 8), i1 = reserve(cg, 8);
        fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rdx\n    cmp QWORD PTR [rbp-%d], 2\n    jne .Lstore_fail_%d\n    cmp rdx, QWORD PTR [rbp-%d]\n    jae .Lstore_fail_%d\n", i0, s->rank_offset, fail, s->dim_offsets[0], fail);
        emit_expr(cg, indices[1]); fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n    cmp rax, QWORD PTR [rbp-%d]\n    jae .Lstore_fail_%d\n", i1, s->dim_offsets[1], fail);
        fprintf(cg->out, "    mov rdx, QWORD PTR [rbp-%d]\n    imul rdx, QWORD PTR [rbp-%d]\n    mov rcx, QWORD PTR [rbp-%d]\n    imul rcx, QWORD PTR [rbp-%d]\n    add rdx, rcx\n", i0, s->stride_offsets[0], i1, s->stride_offsets[1]);
    } else {
        fprintf(cg->out, "    cmp rdx, QWORD PTR [rbp-%d]\n    jae .Lstore_fail_%d\n", s->length_offset, fail);
    }
    fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rdx\n", address);
    emit_expr(cg, value);
    fprintf(cg->out, "    mov rdx, QWORD PTR [rbp-%d]\n", address);
    emit_load_buffer_ptr(cg, name, "rbx");
    if (s->kind == SYM_SLICE && s->type == COBRA_TYPE_SLICE_U8) {
        fprintf(cg->out, "    mov BYTE PTR [rbx + rdx], al\n");
    } else if ((s->kind == SYM_SLICE && s->type == COBRA_TYPE_SLICE_F32) ||
        ((s->kind == SYM_LIST || s->kind == SYM_ARRAY) && s->element_type == COBRA_TYPE_F32) ||
        s->kind == SYM_TENSOR) {
        if (!expression_is_float_codegen(cg, value)) fprintf(cg->out, "    cvtsi2ss xmm0, rax\n");
        fprintf(cg->out, "    movss DWORD PTR [rbx + rdx*4], xmm0\n");
    } else fprintf(cg->out, "    mov QWORD PTR [rbx + rdx*8], rax\n");
    fprintf(cg->out, "    jmp .Lstore_done_%d\n.Lstore_fail_%d:\n", fail, fail);
    emit_failure(cg, "buffer index out of bounds");
    fprintf(cg->out, ".Lstore_done_%d:\n", fail);
}

static void emit_expr(CodeGen *cg, ASTNode *node) {
    if (!node) return;
    switch (node->type) {
        case AST_INT_LITERAL: fprintf(cg->out, "    mov rax, %lld\n", (long long)node->literal_i64); return;
        case AST_FLOAT_LITERAL: emit_float_literal(cg, node->float_val); return;
        case AST_STRING_LITERAL: emit_string_literal(cg, node->string_val); return;
        case AST_BOOL_LITERAL: fprintf(cg->out, "    mov rax, %d\n", node->int_val ? 1 : 0); return;
        case AST_NONE_LITERAL: fprintf(cg->out, "    xor eax, eax\n"); return;
        case AST_MEMBER_ACCESS: {
            if (node->value_type == COBRA_TYPE_ENUM) {
                /* Qualified enum variants (Phase.Active) are compile-time
                   integer constants, but an enum-typed struct field
                   (s.phase) is a memory load at its packed offset. The IR
                   resolves the constant only when the base names an enum
                   type; a base that resolves to a local symbol is a field. */
                bool base_is_symbol = node->child_count > 0 &&
                                      node->children[0]->type == AST_VAR_REF &&
                                      find_symbol(cg, node->children[0]->name) != NULL;
                if (!base_is_symbol) {
                    fprintf(cg->out, "    mov rax, %d\n", node->int_val);
                    return;
                }
            }
            if (node->child_count == 0) {
                fprintf(stderr, "CodeGen Error: member access requires a struct value\n");
                exit(EXIT_FAILURE);
            }
            emit_struct_address(cg, node->children[0]);
            int offset = field_offset_for(cg, ast_payload_name(node->children[0]),
                                          node->secondary_name);
            if (node->value_type == COBRA_TYPE_STRUCT) {
                fprintf(cg->out, "    add rax, %d\n", offset);
            } else if (node->value_type == COBRA_TYPE_SLICE ||
                       node->value_type == COBRA_TYPE_SLICE_F32 ||
                       node->value_type == COBRA_TYPE_SLICE_U8) {
                fprintf(cg->out, "    mov rdx, QWORD PTR [rax + %d]\n    mov rax, QWORD PTR [rax + %d]\n",
                        offset + 8, offset);
            } else if (node->value_type == COBRA_TYPE_F32) {
                fprintf(cg->out, "    movss xmm0, DWORD PTR [rax + %d]\n", offset);
            } else {
                fprintf(cg->out, "    mov rax, QWORD PTR [rax + %d]\n", offset);
            }
            return;
        }
        case AST_ENV_FIELD_LOAD: {
            if (node->child_count != 1) {
                fprintf(stderr, "CodeGen Error: env field load requires one pointer expression\n");
                exit(EXIT_FAILURE);
            }
            emit_expr(cg, node->children[0]);
            if (node->value_type == COBRA_TYPE_F32) {
                fprintf(cg->out, "    movss xmm0, DWORD PTR [rax + %d]\n", (int)node->int_val);
            } else {
                fprintf(cg->out, "    mov rax, QWORD PTR [rax + %d]\n", (int)node->int_val);
            }
            return;
        }
        case AST_COMPTIME_EXPR: fprintf(cg->out, "    mov rax, %d\n", interpreter_eval_expr(node)); return;
        case AST_VAR_REF: {
            int loop = current_iter(cg, node->name);
            if (loop >= 0) {
                fprintf(cg->out, "    mov rdx, QWORD PTR [rbp-%d]\n", cg->loops[loop].index_offset);
                if (cg->loops[loop].enumerate &&
                    cg->loops[loop].secondary_name[0] != '\0' &&
                    !strcmp(node->name, cg->loops[loop].secondary_name)) {
                    emit_load_buffer_ptr(cg, cg->loops[loop].source, "rbx");
                    if (cg->loops[loop].element_type == COBRA_TYPE_F32)
                        fprintf(cg->out, "    movss xmm0, DWORD PTR [rbx + rdx*4]\n");
                    else
                        fprintf(cg->out, "    mov rax, QWORD PTR [rbx + rdx*8]\n");
                } else if (cg->loops[loop].source[0] == '\0' ||
                           (cg->loops[loop].enumerate &&
                            !strcmp(node->name, cg->loops[loop].name))) {
                    fprintf(cg->out, "    mov rax, rdx\n");
                } else {
                    emit_load_buffer_ptr(cg, cg->loops[loop].source, "rbx");
                    if (cg->loops[loop].element_type == COBRA_TYPE_F32) fprintf(cg->out, "    movss xmm0, DWORD PTR [rbx + rdx*4]\n");
                    else fprintf(cg->out, "    mov rax, QWORD PTR [rbx + rdx*8]\n");
                }
                return;
            }
            if (node->is_closure_instance) {
                /* Closure literal's use site: build a fresh {code_ptr,env_ptr}
                   thunk. Captured values are this (the enclosing) function's
                   own live locals, since the literal is compiled as part of
                   the enclosing function's own body. */
                ASTNode *closure_fn = find_function(cg, node->name);
                if (!closure_fn) { fprintf(stderr, "CodeGen Error: undefined closure '%s'\n", node->name); exit(EXIT_FAILURE); }
                int env_slot = reserve(cg, 8);
                if (closure_fn->captured_count > 0) {
                    fprintf(cg->out, "    mov rdi, %d\n    call malloc@PLT\n    mov QWORD PTR [rbp-%d], rax\n",
                            closure_fn->captured_count * 8, env_slot);
                    for (int i = 0; i < closure_fn->captured_count; i++) {
                        VarSymbol *cap = find_symbol(cg, closure_fn->captured_names[i]);
                        if (!cap) { fprintf(stderr, "CodeGen Error: closure capture '%s' not found\n", closure_fn->captured_names[i]); exit(EXIT_FAILURE); }
                        if (closure_fn->captured_types[i] == COBRA_TYPE_F32)
                            fprintf(cg->out, "    movss xmm0, DWORD PTR [rbp-%d]\n    mov rdx, QWORD PTR [rbp-%d]\n    movss DWORD PTR [rdx+%d], xmm0\n",
                                    cap->offset, env_slot, i * 8);
                        else
                            fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov rdx, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rdx+%d], rax\n",
                                    cap->offset, env_slot, i * 8);
                    }
                } else {
                    fprintf(cg->out, "    mov QWORD PTR [rbp-%d], 0\n", env_slot);
                }
                fprintf(cg->out,
                        "    mov rdi, 16\n    call malloc@PLT\n"
                        "    lea rdx, [rip+%s]\n    mov QWORD PTR [rax], rdx\n"
                        "    mov rdx, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rax+8], rdx\n",
                        node->name, env_slot);
                return;
            }
            VarSymbol *s = find_symbol(cg, node->name);
            if (!s) {
                /* A bare identifier that isn't a local is a plain top-level
                   function used as an fn(...)->... value: its value is a
                   pointer to a lazily-emitted {adapter_stub, 0} thunk (see
                   ensure_fn_thunk), keeping every fn value - closure or
                   plain - a uniform pointer to a {code_ptr, env_ptr} pair. */
                ASTNode *fn_ref = find_function(cg, node->name);
                if (fn_ref) { ensure_fn_thunk(cg, node->name); fprintf(cg->out, "    lea rax, [rip+__fnthunk_%s]\n", node->name); return; }
                fprintf(stderr, "CodeGen Error: Undefined variable '%s'\n", node->name); exit(EXIT_FAILURE);
            }
            if (is_sum_symbol_type(s->type)) fprintf(cg->out, "    lea rax, [rbp-%d]\n", s->tag_offset);
            else if (s->kind == SYM_STRUCT) {
                if (s->indirect) fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n", s->array_base);
                else fprintf(cg->out, "    lea rax, [rbp-%d]\n", s->array_base);
            }
            else if (s->kind == SYM_SLICE) {
                fprintf(cg->out, "    mov rdx, QWORD PTR [rbp-%d]\n    mov rax, QWORD PTR [rbp-%d]\n",
                        s->length_offset, s->offset);
            } else if (s->kind == SYM_F32) fprintf(cg->out, "    movss xmm0, DWORD PTR [rbp-%d]\n", s->offset);
            else fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n", s->offset);
            return;
        }
        case AST_ARRAY_INDEX:
            if (node->secondary_name[0] != '\0') emit_struct_field_index_read(cg, node);
            else emit_index_read(cg, node->name, node->children, node->child_count);
            return;
        case AST_DICT_LITERAL: return;
        case AST_COMPREHENSION: {
            /* [expr for target in source (if guard)?] lowers to a fresh native
               list built with one direct loop and zero iterator objects. */
            if (node->child_count < 2) {
                fprintf(stderr, "CodeGen Error: comprehension requires output and source\n"); exit(EXIT_FAILURE);
            }
            if (node->comprehension_target[0] == '\0') {
                fprintf(stderr, "CodeGen Error: a comprehension must be the initializer of a list declaration\n"); exit(EXIT_FAILURE);
            }
            ASTNode *output = node->children[0];
            ASTNode *source = node->children[1];
            if (source->type != AST_VAR_REF) {
                fprintf(stderr, "CodeGen Error: comprehension source must be a named collection\n"); exit(EXIT_FAILURE);
            }
            VarSymbol *src = find_symbol(cg, source->name);
            if (!src) {
                fprintf(stderr, "CodeGen Error: unknown comprehension source '%s'\n", source->name); exit(EXIT_FAILURE);
            }
            bool f32_elements = (src->kind == SYM_LIST && src->element_type == COBRA_TYPE_F32) ||
                                (src->kind == SYM_ARRAY && src->element_type == COBRA_TYPE_F32) ||
                                (src->kind == SYM_SLICE && src->type == COBRA_TYPE_SLICE_F32);
            CobraTypeKind out_element = (output->value_type == COBRA_TYPE_F32) ? COBRA_TYPE_F32 : COBRA_TYPE_I64;
            /* Reuse the declared list symbol's pointer/length/capacity fields so
               the built list lands directly in the destination variable. The
               declaration reserved the symbol before emitting the initializer. */
            VarSymbol *dst = find_symbol(cg, node->comprehension_target);
            if (!dst) dst = ensure_list(cg, node->comprehension_target, out_element);
            dst->owned = true;
            int label = cg->label_count++;
            int counter = reserve(cg, 8), bound = reserve(cg, 8);
            fprintf(cg->out, "    mov QWORD PTR [rbp-%d], 0\n    mov QWORD PTR [rbp-%d], 0\n    mov QWORD PTR [rbp-%d], 0\n", dst->offset, dst->length_offset, dst->capacity_offset);
            /* The comprehension target is a real scalar local so the output
               expression can reference it on every iteration. If an outer symbol
               already uses that name, preserve its value across the loop so the
               comprehension does not leak its iterator into the enclosing scope
               (mirroring the IR's scoped-local restore). */
            VarSymbol *outer = find_symbol(cg, node->name);
            int outer_save = 0;
            if (outer) {
                outer_save = reserve(cg, 8);
                if (outer->kind == SYM_F32)
                    fprintf(cg->out, "    movss xmm0, DWORD PTR [rbp-%d]\n    movss DWORD PTR [rbp-%d], xmm0\n", outer->offset, outer_save);
                else
                    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", outer->offset, outer_save);
            }
            VarSymbol *iter = ensure_scalar(cg, node->name, f32_elements ? COBRA_TYPE_F32 : COBRA_TYPE_I64);
            int guard_label = node->child_count > 2 ? cg->label_count++ : 0;
            fprintf(cg->out, "    mov QWORD PTR [rbp-%d], 0\n.Lcomp_%d:\n", counter, label);
            emit_load_buffer_len(cg, source->name, "rcx");
            fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rcx\n    mov rax, QWORD PTR [rbp-%d]\n    cmp rax, rcx\n    jae .Lcomp_done_%d\n", bound, counter, label);
            emit_load_buffer_ptr(cg, source->name, "rbx");
            fprintf(cg->out, "    mov rdx, QWORD PTR [rbp-%d]\n", counter);
            if (f32_elements) fprintf(cg->out, "    movss xmm0, DWORD PTR [rbx + rdx*4]\n    movss DWORD PTR [rbp-%d], xmm0\n", iter->offset);
            else fprintf(cg->out, "    mov rax, QWORD PTR [rbx + rdx*8]\n    mov QWORD PTR [rbp-%d], rax\n", iter->offset);
            if (node->child_count > 2) {
                emit_expr(cg, node->children[2]);
                fprintf(cg->out, "    test rax, rax\n    je .Lcomp_skip_%d\n", guard_label);
            }
            emit_expr(cg, output);
            if (out_element == COBRA_TYPE_F32) {
                if (!expression_is_float_codegen(cg, output)) fprintf(cg->out, "    cvtsi2ss xmm0, rax\n");
            }
            fprintf(cg->out, "    lea rdi, [rbp-%d]\n    lea rsi, [rbp-%d]\n    lea rdx, [rbp-%d]\n", dst->offset, dst->length_offset, dst->capacity_offset);
            if (out_element == COBRA_TYPE_F32) {
                fprintf(cg->out, "    call cobra_list_append_f32@PLT\n");
            } else {
                fprintf(cg->out, "    mov rcx, rax\n    call cobra_list_append_i64@PLT\n");
            }
            if (node->child_count > 2) fprintf(cg->out, ".Lcomp_skip_%d:\n", guard_label);
            fprintf(cg->out, "    inc QWORD PTR [rbp-%d]\n    jmp .Lcomp_%d\n.Lcomp_done_%d:\n", counter, label, label);
            if (outer_save) {
                if (outer->kind == SYM_F32)
                    fprintf(cg->out, "    movss xmm0, DWORD PTR [rbp-%d]\n    movss DWORD PTR [rbp-%d], xmm0\n", outer_save, outer->offset);
                else
                    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", outer_save, outer->offset);
            }
            return;
        }
        case AST_MEMBERSHIP: {
            if (node->child_count != 2) {
                fprintf(stderr, "CodeGen Error: membership requires two children\n"); exit(EXIT_FAILURE);
            }
            bool negated = strcmp(node->name, "not in") == 0;
            ASTNode *element = node->children[0];
            ASTNode *container = node->children[1];
            VarSymbol *s = container->type == AST_VAR_REF ? find_symbol(cg, container->name) : NULL;
            /* Reserve every scratch slot before emitting the element expression:
               array literals are embedded in the frame, so slots reserved after
               the declaration would land inside the array's element region. */
            int result = reserve(cg, 8), label = cg->label_count++;
            int idx = reserve(cg, 8), bound = reserve(cg, 8), needle = reserve(cg, 8);
            if (s && s->kind == SYM_DICT) {
                /* Save the dict pointer before evaluating the key: a call-bearing
                   key expression would otherwise clobber the caller-saved rdi. */
                int dict_ptr = reserve(cg, 8);
                fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", s->offset, dict_ptr);
                emit_expr(cg, element);
                fprintf(cg->out, "    mov rsi, rax\n    mov rdi, QWORD PTR [rbp-%d]\n    call cobra_dict_has@PLT\n    test rax, rax\n", dict_ptr);
                if (negated) fprintf(cg->out, "    sete al\n"); else fprintf(cg->out, "    setne al\n");
                fprintf(cg->out, "    movzx eax, al\n");
            } else {
                /* Native scan over the collection with an early exit. */
                bool f32_elements = (s && s->kind == SYM_LIST && s->element_type == COBRA_TYPE_F32) ||
                                    (s && s->kind == SYM_ARRAY && s->element_type == COBRA_TYPE_F32) ||
                                    (s && s->kind == SYM_SLICE && s->type == COBRA_TYPE_SLICE_F32);
                emit_expr(cg, element);
                if (f32_elements) {
                    if (!expression_is_float_codegen(cg, element)) fprintf(cg->out, "    cvtsi2ss xmm0, rax\n");
                    fprintf(cg->out, "    movss DWORD PTR [rbp-%d], xmm0\n", needle);
                } else {
                    fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", needle);
                }
                if (s) {
                    emit_load_buffer_len(cg, container->name, "rcx");
                    fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rcx\n    mov QWORD PTR [rbp-%d], 0\n", bound, idx);
                } else {
                    fprintf(stderr, "CodeGen Error: membership requires a named collection\n");
                    exit(EXIT_FAILURE);
                }
                /* The result slot is the truth value (0 or 1); the search value
                   lives in its own needle slot so an absent element stays 0. */
                fprintf(cg->out, "    mov QWORD PTR [rbp-%d], 0\n.Lmember_%d:\n    mov rax, QWORD PTR [rbp-%d]\n    cmp rax, QWORD PTR [rbp-%d]\n    jae .Lmember_done_%d\n", result, label, idx, bound, label);
                emit_load_buffer_ptr(cg, container->name, "rbx");
                fprintf(cg->out, "    mov rdx, QWORD PTR [rbp-%d]\n", idx);
                if (f32_elements) {
                    fprintf(cg->out, "    movss xmm1, DWORD PTR [rbx + rdx*4]\n    movss xmm0, DWORD PTR [rbp-%d]\n    comiss xmm0, xmm1\n", needle);
                    fprintf(cg->out, "    jne .Lmember_next_%d\n", label);
                } else {
                    fprintf(cg->out, "    mov rcx, QWORD PTR [rbx + rdx*8]\n    cmp QWORD PTR [rbp-%d], rcx\n    jne .Lmember_next_%d\n", needle, label);
                }
                fprintf(cg->out, "    mov QWORD PTR [rbp-%d], 1\n    jmp .Lmember_done_%d\n.Lmember_next_%d:\n    inc QWORD PTR [rbp-%d]\n    jmp .Lmember_%d\n.Lmember_done_%d:\n", result, label, label, idx, label, label);
                fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n", result);
                if (negated) fprintf(cg->out, "    test rax, rax\n    sete al\n    movzx eax, al\n");
            }
            return;
        }
        case AST_LEN_EXPR: {
            ASTNode *a = node->child_count ? node->children[0] : NULL;
            if (a && a->type == AST_STRING_LITERAL) fprintf(cg->out, "    mov rax, %zu\n", strlen(a->string_val));
            else if (a && a->type == AST_ARRAY_LITERAL) fprintf(cg->out, "    mov rax, %zu\n", a->child_count);
            else if (a && a->type == AST_MEMBER_ACCESS &&
                     (a->value_type == COBRA_TYPE_SLICE ||
                      a->value_type == COBRA_TYPE_SLICE_F32 ||
                      a->value_type == COBRA_TYPE_SLICE_U8)) {
                emit_expr(cg, a);
                fprintf(cg->out, "    mov rax, rdx\n");
            } else if (a && a->type == AST_VAR_REF) {
                VarSymbol *s = find_symbol(cg, a->name);
                if (s && s->type == COBRA_TYPE_STRING) {
                    emit_expr(cg, a);
                    fprintf(cg->out, "    mov rdi, rax\n    call strlen@PLT\n");
                } else if (s && s->kind == SYM_DICT) {
                    fprintf(cg->out, "    mov rdi, QWORD PTR [rbp-%d]\n    call cobra_dict_len@PLT\n", s->offset);
                } else {
                    emit_load_buffer_len(cg, a->name, "rax");
                }
            } else { emit_expr(cg, a); fprintf(cg->out, "    mov rdi, rax\n    call strlen@PLT\n"); }
            return;
        }
        case AST_BINARY_OP: {
            if (node->value_type == COBRA_TYPE_STRING && !strcmp(node->name, "+")) { emit_string_concat(cg, node); return; }
            if ((node->children[0]->value_type == COBRA_TYPE_STRING || node->children[1]->value_type == COBRA_TYPE_STRING) &&
                expression_is_comparison(node)) { emit_string_compare(cg, node); return; }
            bool f = expression_is_float_codegen(cg, node) ||
                     expression_is_float_codegen(cg, node->children[0]) ||
                     expression_is_float_codegen(cg, node->children[1]);
            if (f) {
                fprintf(cg->out, "    sub rsp, 16\n");
                emit_expr(cg, node->children[1]);
                if (!expression_is_float_codegen(cg, node->children[1])) fprintf(cg->out, "    cvtsi2ss xmm0, rax\n");
                fprintf(cg->out, "    movss DWORD PTR [rsp], xmm0\n");
                emit_expr(cg, node->children[0]);
                if (!expression_is_float_codegen(cg, node->children[0])) fprintf(cg->out, "    cvtsi2ss xmm0, rax\n");
                fprintf(cg->out, "    movss xmm1, DWORD PTR [rsp]\n    add rsp, 16\n");
                if (!strcmp(node->name, "+")) fprintf(cg->out, "    addss xmm0, xmm1\n"); else if (!strcmp(node->name, "-")) fprintf(cg->out, "    subss xmm0, xmm1\n"); else if (!strcmp(node->name, "*")) fprintf(cg->out, "    mulss xmm0, xmm1\n"); else if (!strcmp(node->name, "/")) fprintf(cg->out, "    divss xmm0, xmm1\n"); else { fprintf(cg->out, "    ucomiss xmm0, xmm1\n"); const char *set = !strcmp(node->name, "==") ? "sete" : !strcmp(node->name, "!=") ? "setne" : !strcmp(node->name, "<") ? "setb" : !strcmp(node->name, ">") ? "seta" : !strcmp(node->name, "<=") ? "setbe" : "setae"; fprintf(cg->out, "    %s al\n    movzx eax, al\n", set); }            } else {
                fprintf(cg->out, "    push rbx\n"); emit_expr(cg, node->children[1]); fprintf(cg->out, "    push rax\n"); emit_expr(cg, node->children[0]); fprintf(cg->out, "    pop rbx\n");
                if (!strcmp(node->name, "+")) fprintf(cg->out, "    add rax, rbx\n"); else if (!strcmp(node->name, "-")) fprintf(cg->out, "    sub rax, rbx\n"); else if (!strcmp(node->name, "*")) fprintf(cg->out, "    imul rax, rbx\n"); else if (!strcmp(node->name, "/")) {
                    int fail = cg->label_count++;
                    fprintf(cg->out, "    cmp rbx, 0\n    je .Ldiv_zero_%d\n    cqo\n    idiv rbx\n    jmp .Ldiv_done_%d\n.Ldiv_zero_%d:\n", fail, fail, fail);
                    emit_failure(cg, "division by zero");
                    fprintf(cg->out, ".Ldiv_done_%d:\n", fail);
                } else { fprintf(cg->out, "    cmp rax, rbx\n"); const char *set = !strcmp(node->name, "==") ? "sete" : !strcmp(node->name, "!=") ? "setne" : !strcmp(node->name, "<") ? "setl" : !strcmp(node->name, ">") ? "setg" : !strcmp(node->name, "<=") ? "setle" : "setge"; fprintf(cg->out, "    %s al\n    movzx eax, al\n", set); }
                fprintf(cg->out, "    pop rbx\n");
            }
            return;
        }
        case AST_FUNC_CALL: emit_call(cg, node); return;
        default: return;
    }
}

static void emit_alloc(CodeGen *cg, ASTNode *call, int size) {
    int tmp = reserve(cg, 8); emit_expr(cg, call->children[0]);
    int fail = cg->label_count++;
    fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n    cmp rax, 0\n    jl .Lalloc_fail_%d\n    mov rdi, rax\n    mov esi, %d\n    call calloc@PLT\n    mov rdx, QWORD PTR [rbp-%d]\n    jmp .Lalloc_done_%d\n.Lalloc_fail_%d:\n", tmp, fail, size, tmp, fail + 1, fail);
    emit_failure(cg, "negative allocation count"); fprintf(cg->out, ".Lalloc_done_%d:\n", fail + 1);
}

static void emit_fill(CodeGen *cg, ASTNode *n) {
    const char *name = n->children[0]->name; int label = cg->label_count++;
    emit_load_buffer_ptr(cg, name, "rbx"); emit_load_buffer_len(cg, name, "rcx"); emit_expr(cg, n->children[1]);
    if (!expression_is_float(n->children[1])) fprintf(cg->out, "    cvtsi2ss xmm0, rax\n");
    fprintf(cg->out, "    xor rdx, rdx\n.Lfill_%d:\n    cmp rdx, rcx\n    jae .Lfill_done_%d\n    movss DWORD PTR [rbx + rdx*4], xmm0\n    inc rdx\n    jmp .Lfill_%d\n.Lfill_done_%d:\n", label, label, label, label);
}

static void emit_relu(CodeGen *cg, ASTNode *n) {
    const char *name = n->children[0]->name; int label = cg->label_count++;
    /* rbx is callee-saved, so it survives the gpu_* calls below untouched;
       rcx (the length) is caller-saved and must be reloaded after any call. */
    emit_load_buffer_ptr(cg, name, "rbx");
    if (g_gpu_enabled) {
        int gpu_skip = cg->label_count++;
        emit_load_buffer_len(cg, name, "rdi");
        fprintf(cg->out, "    call cobra_gpu_should_dispatch@PLT\n    cmp rax, 0\n    je .Lrelu_gpu_skip_%d\n", gpu_skip);
        fprintf(cg->out, "    mov rdi, rbx\n");
        emit_load_buffer_len(cg, name, "rsi");
        fprintf(cg->out, "    call cobra_gpu_relu_f32@PLT\n    cmp rax, 0\n    jne .Lrelu_done_%d\n", label);
        fprintf(cg->out, ".Lrelu_gpu_skip_%d:\n", gpu_skip);
    }
    emit_load_buffer_len(cg, name, "rcx");
    fprintf(cg->out, "    xor rdx, rdx\n    pxor xmm1, xmm1\n.Lrelu_%d:\n    cmp rdx, rcx\n    jae .Lrelu_done_%d\n    movss xmm0, DWORD PTR [rbx + rdx*4]\n    maxss xmm0, xmm1\n    movss DWORD PTR [rbx + rdx*4], xmm0\n    inc rdx\n    jmp .Lrelu_%d\n.Lrelu_done_%d:\n", label, label, label, label);
}

static void emit_reduce(CodeGen *cg, ASTNode *n, const char *op) {
    const char *name = n->children[0]->name;
    int label = cg->label_count++;
    int combine = cg->label_count++;
    int vec8 = cg->label_count++;
    int tail = cg->label_count++;
    int done = cg->label_count++;
    bool is_max = !strcmp(op, "max");
    bool is_mean = !strcmp(op, "mean");
    const char *opv = is_max ? "max" : "add";
    int end_label = cg->label_count++;

    emit_load_buffer_ptr(cg, name, "rbx");

    /* rbx (data pointer) is callee-saved and survives the calls below; rcx
       (length) is caller-saved and gets reloaded fresh both after the
       dispatch check and again below for the CPU fallback path. */
    if (g_gpu_enabled) {
        int gpu_skip = cg->label_count++;
        int result_slot = reserve(cg, 8);
        emit_load_buffer_len(cg, name, "rdi");
        fprintf(cg->out, "    call cobra_gpu_should_dispatch@PLT\n    cmp rax, 0\n    je .Lreduce_gpu_skip_%d\n", gpu_skip);
        fprintf(cg->out, "    mov rdi, rbx\n");
        emit_load_buffer_len(cg, name, "rsi");
        fprintf(cg->out, "    mov rdx, %d\n    lea rcx, [rbp-%d]\n    call cobra_gpu_reduce_f32@PLT\n    cmp rax, 0\n    je .Lreduce_gpu_skip_%d\n",
                is_max ? 1 : 0, result_slot, gpu_skip);
        fprintf(cg->out, "    movss xmm0, DWORD PTR [rbp-%d]\n", result_slot);
        if (is_mean) {
            int mean_zero = cg->label_count++;
            emit_load_buffer_len(cg, name, "rcx");
            fprintf(cg->out, "    cmp rcx, 0\n    je .Lreduce_gpu_mean_zero_%d\n    cvtsi2ss xmm1, rcx\n    divss xmm0, xmm1\n    jmp .Lreduce_end_%d\n.Lreduce_gpu_mean_zero_%d:\n    xorps xmm0, xmm0\n    jmp .Lreduce_end_%d\n",
                    mean_zero, end_label, mean_zero, end_label);
        } else {
            fprintf(cg->out, "    jmp .Lreduce_end_%d\n", end_label);
        }
        fprintf(cg->out, ".Lreduce_gpu_skip_%d:\n", gpu_skip);
    }

    emit_load_buffer_len(cg, name, "rcx");

    /* Four independent accumulators break the FP dependency chain the same
       way a -ffast-math C compiler unrolls a sum. max is order-independent
       and gets the same structure. The 0-31 element remainder runs through
       a single-accumulator loop, and the last 0-7 through the scalar tail,
       so every length keeps the checked semantics. */
    if (is_max) {
        fprintf(cg->out, "    mov eax, 0xff800000\n    movd xmm0, eax\n    vpbroadcastd ymm0, xmm0\n    vmovaps ymm1, ymm0\n    vmovaps ymm2, ymm0\n    vmovaps ymm3, ymm0\n");
    } else {
        fprintf(cg->out, "    vxorps ymm0, ymm0, ymm0\n    vxorps ymm1, ymm1, ymm1\n    vxorps ymm2, ymm2, ymm2\n    vxorps ymm3, ymm3, ymm3\n");
    }
    fprintf(cg->out,
            "    xor rdx, rdx\n"
            ".Lreduce_vec4_%d:\n"
            "    mov rax, rcx\n"
            "    sub rax, rdx\n"
            "    cmp rax, 32\n"
            "    jb .Lreduce_combine_%d\n",
            label, combine);
    fprintf(cg->out,
            "    v%sps ymm0, ymm0, YMMWORD PTR [rbx + rdx*4]\n"
            "    v%sps ymm1, ymm1, YMMWORD PTR [rbx + rdx*4 + 32]\n"
            "    v%sps ymm2, ymm2, YMMWORD PTR [rbx + rdx*4 + 64]\n"
            "    v%sps ymm3, ymm3, YMMWORD PTR [rbx + rdx*4 + 96]\n"
            "    add rdx, 32\n"
            "    jmp .Lreduce_vec4_%d\n"
            ".Lreduce_combine_%d:\n"
            "    v%sps ymm0, ymm0, ymm1\n"
            "    v%sps ymm2, ymm2, ymm3\n"
            "    v%sps ymm0, ymm0, ymm2\n",
            opv, opv, opv, opv, label, combine, opv, opv, opv);
    fprintf(cg->out,
            ".Lreduce_vec8_%d:\n"
            "    mov rax, rcx\n"
            "    sub rax, rdx\n"
            "    cmp rax, 8\n"
            "    jb .Lreduce_tail_%d\n",
            vec8, tail);
    fprintf(cg->out, "    v%sps ymm0, ymm0, YMMWORD PTR [rbx + rdx*4]\n", opv);
    fprintf(cg->out,
            "    add rdx, 8\n"
            "    jmp .Lreduce_vec8_%d\n"
            ".Lreduce_tail_%d:\n"
            "    vextractf128 xmm1, ymm0, 1\n",
            vec8, tail);
    if (is_max) {
        fprintf(cg->out,
                "    vmaxps xmm0, xmm0, xmm1\n"
                "    vshufps xmm1, xmm0, xmm0, 0x4e\n"
                "    vmaxps xmm0, xmm0, xmm1\n"
                "    vshufps xmm1, xmm0, xmm0, 0xb1\n"
                "    vmaxps xmm0, xmm0, xmm1\n"
                "    vzeroupper\n");
    } else {
        fprintf(cg->out,
                "    vaddps xmm0, xmm0, xmm1\n"
                "    vhaddps xmm0, xmm0, xmm0\n"
                "    vhaddps xmm0, xmm0, xmm0\n"
                "    vzeroupper\n");
    }
    fprintf(cg->out,
            ".Lreduce_scalar_%d:\n"
            "    cmp rdx, rcx\n"
            "    jae .Lreduce_done_%d\n"
            "    movss xmm1, DWORD PTR [rbx + rdx*4]\n",
            tail, done);
    if (is_max) {
        fprintf(cg->out, "    maxss xmm0, xmm1\n");
    } else {
        fprintf(cg->out, "    addss xmm0, xmm1\n");
    }
    fprintf(cg->out,
            "    inc rdx\n"
            "    jmp .Lreduce_scalar_%d\n"
            ".Lreduce_done_%d:\n",
            tail, done);

    if (!strcmp(op, "mean")) {
        int mean_done = cg->label_count++;
        fprintf(cg->out,
                "    cmp rcx, 0\n"
                "    je .Lmean_zero_%d\n"
                "    cvtsi2ss xmm1, rcx\n"
                "    divss xmm0, xmm1\n"
                "    jmp .Lmean_done_%d\n"
                ".Lmean_zero_%d:\n"
                "    xorps xmm0, xmm0\n"
                ".Lmean_done_%d:\n",
                label, mean_done, label, mean_done);
    }
    fprintf(cg->out, ".Lreduce_end_%d:\n", end_label);
}

static void emit_math(CodeGen *cg, ASTNode *n) {
    const char *fn = !strcmp(n->name, "exp_f32") ? "expf" : !strcmp(n->name, "sqrt_f32") ? "sqrtf" : !strcmp(n->name, "tanh_f32") ? "tanhf" : !strcmp(n->name, "log_f32") ? "logf" : "powf";
    if (!strcmp(n->name, "pow_f32")) { emit_expr(cg, n->children[0]); fprintf(cg->out, "    sub rsp, 16\n    movss DWORD PTR [rsp], xmm0\n"); emit_expr(cg, n->children[1]); fprintf(cg->out, "    movss xmm1, xmm0\n    movss xmm0, DWORD PTR [rsp]\n    add rsp, 16\n"); } else emit_expr(cg, n->children[0]);
    fprintf(cg->out, "    call %s@PLT\n", fn);
}

static bool const_int_value(ASTNode *n, int64_t *out) {
    if (!n) return false;
    if (n->type == AST_INT_LITERAL) { *out = n->literal_i64; return true; }
    if (n->type == AST_COMPTIME_EXPR) { *out = interpreter_eval_expr(n); return true; }
    return false;
}

/* Constant-shape GEMM inner loop: zeroes the four accumulators and emits the
   K loop that feeds them. K < 4 unrolls the dot product; K % 4 == 0 skips the
   remainder loop entirely. The combine label always runs, so the caller's
   bias and store code is emitted after the shared combine. */
static void emit_gemm_const_kbody(CodeGen *cg, int64_t K,
                                  int k_label, int tail_label, int combine_label) {
    fprintf(cg->out, "    vxorps ymm0, ymm0, ymm0\n    vxorps ymm1, ymm1, ymm1\n    vxorps ymm2, ymm2, ymm2\n    vxorps ymm3, ymm3, ymm3\n");
    if (K < 4) {
        /* Tiny inner dims: one FMA per k step, no loop at all. rdi advances by
           the B row stride rbx between steps. */
        for (int64_t step = 0; step < K; step++) {
            fprintf(cg->out, "    vbroadcastss ymm4, DWORD PTR [rsi + %lld]\n    vmovups ymm5, YMMWORD PTR [rdi]\n    vfmadd231ps ymm0, ymm5, ymm4\n",
                    (long long)(step * 4));
            if (step + 1 < K) fprintf(cg->out, "    add rdi, rbx\n");
        }
        fprintf(cg->out, "    jmp .Lkc_combine_%d\n", combine_label);
    } else {
        fprintf(cg->out, ".Lkc_k_%d:\n    lea rax, [rsi + 12]\n    cmp rax, rdx\n    jae .Lkc_tail_%d\n", k_label, tail_label);
        fprintf(cg->out, "    vbroadcastss ymm4, DWORD PTR [rsi]\n    vmovups ymm5, YMMWORD PTR [rdi]\n    vfmadd231ps ymm0, ymm5, ymm4\n    add rdi, rbx\n");
        fprintf(cg->out, "    vbroadcastss ymm4, DWORD PTR [rsi + 4]\n    vmovups ymm5, YMMWORD PTR [rdi]\n    vfmadd231ps ymm1, ymm5, ymm4\n    add rdi, rbx\n");
        fprintf(cg->out, "    vbroadcastss ymm4, DWORD PTR [rsi + 8]\n    vmovups ymm5, YMMWORD PTR [rdi]\n    vfmadd231ps ymm2, ymm5, ymm4\n    add rdi, rbx\n");
        fprintf(cg->out, "    vbroadcastss ymm4, DWORD PTR [rsi + 12]\n    vmovups ymm5, YMMWORD PTR [rdi]\n    vfmadd231ps ymm3, ymm5, ymm4\n    add rdi, rbx\n");
        fprintf(cg->out, "    add rsi, 16\n    jmp .Lkc_k_%d\n", k_label);
        if (K % 4 == 0) {
            /* The remainder loop can never run (K*4 is a multiple of 16), so
               the tail label folds into a direct jump to the combine. */
            fprintf(cg->out, ".Lkc_tail_%d:\n    jmp .Lkc_combine_%d\n", tail_label, combine_label);
        } else {
            fprintf(cg->out, ".Lkc_tail_%d:\n    cmp rsi, rdx\n    jae .Lkc_combine_%d\n", tail_label, combine_label);
            fprintf(cg->out, "    vbroadcastss ymm4, DWORD PTR [rsi]\n    vmovups ymm5, YMMWORD PTR [rdi]\n    vfmadd231ps ymm0, ymm5, ymm4\n    add rsi, 4\n    add rdi, rbx\n    jmp .Lkc_tail_%d\n", tail_label);
        }
    }
    fprintf(cg->out, ".Lkc_combine_%d:\n    vaddps ymm0, ymm0, ymm1\n    vaddps ymm0, ymm0, ymm2\n    vaddps ymm0, ymm0, ymm3\n", combine_label);
}

/* Fully unrolled constant-shape K body: K % 4 == 0 and K <= 64. Every k step
   is one broadcast, one load, and one FMA, all addressed by immediate A/B
   displacements (B rows are N*4 bytes apart), so the dot product has no
   counter, no branch, and no pointer advance. rsi must point at the A row and
   rdi at the B tile base; neither is modified, so callers may hoist them out
   of enclosing tile loops. The body touches only ymm registers and reads via
   rsi/rdi: rax, rbx, rcx, rdx, and r8-r11 survive untouched, which callers
   rely on (the M=1 tile loop keeps the tile offset in rax across the body).
   B displacements reach (K-1)*N*4 bytes; emit_gemm only reaches this path
   for N <= 8192 and K <= 64, so the largest displacement stays near 2 MB,
   well inside the signed 32-bit immediate range. The four accumulators are
   reduced inline, which matches the looped combine up to the same FP
   reassociation order. */
static void emit_gemm_const_kbody_unrolled(CodeGen *cg, int64_t K, int64_t N) {
    fprintf(cg->out, "    vxorps ymm0, ymm0, ymm0\n    vxorps ymm1, ymm1, ymm1\n    vxorps ymm2, ymm2, ymm2\n    vxorps ymm3, ymm3, ymm3\n");
    for (int64_t step = 0; step < K; step++) {
        fprintf(cg->out, "    vbroadcastss ymm4, DWORD PTR [rsi + %lld]\n    vmovups ymm5, YMMWORD PTR [rdi + %lld]\n    vfmadd231ps ymm%lld, ymm5, ymm4\n",
                (long long)(step * 4), (long long)(step * N * 4), (long long)(step % 4));
    }
    fprintf(cg->out, "    vaddps ymm0, ymm0, ymm1\n    vaddps ymm0, ymm0, ymm2\n    vaddps ymm0, ymm0, ymm3\n");
}

/* Constant-shape GEMM: when M, N, and K are compile-time constants and N is a
   multiple of eight, the kernel lowers with immediate trip bounds, no per-row
   vector/scalar decision, and no tail branches. M == 1 (single query row)
   drops the row loop and the hoisted i*k / i*n multiplies entirely; N == 8
   drops the column loop; and when K is a small multiple of four the whole dot
   product unrolls to straight-line FMA with immediate A/B displacements, so
   the hot path has no loop counter, no branch, and no pointer advance. The
   result is the same four-accumulator FMA tiling with the surrounding control
   flow specialized away. */
static void emit_gemm_const(CodeGen *cg, const char *a, const char *b, const char *c,
                            int64_t M, int64_t N, int64_t K,
                            bool add_bias, const char *bias) {
    int i_label = cg->label_count++;
    int j_label = cg->label_count++;
    int k_label = cg->label_count++;
    int tail_label = cg->label_count++;
    int combine_label = cg->label_count++;
    int next_i_label = cg->label_count++;
    int done_label = cg->label_count++;

    emit_load_buffer_ptr(cg, a, "r8");
    emit_load_buffer_ptr(cg, b, "r9");
    emit_load_buffer_ptr(cg, c, "r10");
    if (add_bias) emit_load_buffer_ptr(cg, bias, "r11");
    /* A small K multiple of four unrolls to straight-line FMA with immediate
       displacements: no stride (rbx) and no end pointer (rdx) are needed, so
       rbx is free to hold the tile counter. Larger or odd K keep the looped
       body below, which needs rbx as the B-row stride. */
    bool unroll = (K % 4) == 0 && K >= 4 && K <= 64;
    if (!unroll) fprintf(cg->out, "    mov rbx, %lld\n    shl rbx, 2\n", (long long)N);

    if (M == 1) {
        if (N == 8) {
            /* Single row, single tile: the entire kernel is straight-line. */
            if (unroll) {
                fprintf(cg->out, "    lea rsi, [r8]\n    lea rdi, [r9]\n");
                emit_gemm_const_kbody_unrolled(cg, K, N);
            } else {
                fprintf(cg->out, "    lea rsi, [r8]\n    lea rdx, [rsi + %lld]\n    lea rdi, [r9]\n", (long long)(K * 4));
                emit_gemm_const_kbody(cg, K, k_label, tail_label, combine_label);
            }
            if (add_bias)
                fprintf(cg->out, "    vmovups ymm1, YMMWORD PTR [r11]\n    vaddps ymm0, ymm0, ymm1\n    vxorps ymm1, ymm1, ymm1\n    vmaxps ymm0, ymm0, ymm1\n");
            fprintf(cg->out, "    vmovups YMMWORD PTR [r10], ymm0\n    vzeroupper\n");
            return;
        }
        if (unroll) {
            /* Single row, several eight-float tiles. The A row is
               loop-invariant and the tile counter lives in rbx (callee-saved,
               restored by the frame), so the tile loop has zero stack traffic
               and rax keeps the tile offset for bias and store. */
            fprintf(cg->out, "    lea rsi, [r8]\n    xor ebx, ebx\n.Lkc_j_%d:\n    cmp rbx, %lld\n    jae .Lkc_done_%d\n", j_label, (long long)(N / 8), done_label);
            fprintf(cg->out, "    mov rax, rbx\n    shl rax, 3\n    lea rdi, [r9 + rax*4]\n");
            emit_gemm_const_kbody_unrolled(cg, K, N);
            if (add_bias) {
                fprintf(cg->out, "    vmovups ymm1, YMMWORD PTR [r11 + rax*4]\n    vaddps ymm0, ymm0, ymm1\n    vxorps ymm1, ymm1, ymm1\n    vmaxps ymm0, ymm0, ymm1\n");
            }
            fprintf(cg->out, "    vmovups YMMWORD PTR [r10 + rax*4], ymm0\n    inc rbx\n    jmp .Lkc_j_%d\n", j_label);
            fprintf(cg->out, ".Lkc_done_%d:\n    vzeroupper\n", done_label);
            return;
        }
        /* One row, several N-tiles, looped K body. */
        fprintf(cg->out, "    mov QWORD PTR [rbp-%d], 0\n.Lkc_j_%d:\n", COBRA_SCR_J, j_label);
        fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    cmp rax, %lld\n    jae .Lkc_done_%d\n", COBRA_SCR_J, (long long)(N / 8), done_label);
        /* rsi runs to a_end inside the k body, so re-point it per tile. */
        fprintf(cg->out, "    lea rsi, [r8]\n    lea rdx, [rsi + %lld]\n", (long long)(K * 4));
        /* j counts eight-float tiles: a tile spans 32 bytes, which the SIB
           scale cannot express, so shift j by 3 and use scale 4. */
        fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    shl rax, 3\n    lea rdi, [r9 + rax*4]\n", COBRA_SCR_J);
        emit_gemm_const_kbody(cg, K, k_label, tail_label, combine_label);
        if (add_bias) {
            fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    shl rax, 3\n    vmovups ymm1, YMMWORD PTR [r11 + rax*4]\n    vaddps ymm0, ymm0, ymm1\n    vxorps ymm1, ymm1, ymm1\n    vmaxps ymm0, ymm0, ymm1\n", COBRA_SCR_J);
        }
        fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    shl rax, 3\n    vmovups YMMWORD PTR [r10 + rax*4], ymm0\n    inc QWORD PTR [rbp-%d]\n    jmp .Lkc_j_%d\n", COBRA_SCR_J, COBRA_SCR_J, j_label);
        fprintf(cg->out, ".Lkc_done_%d:\n    vzeroupper\n", done_label);
        return;
    }

    /* General M: a row loop with an immediate bound. i*k and i*n are hoisted
       once per row using the known constants, so the multiplies never touch
       memory for the bounds. */
    fprintf(cg->out, "    mov QWORD PTR [rbp-%d], 0\n.Lkc_i_%d:\n", COBRA_SCR_I, i_label);
    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    cmp rax, %lld\n    jae .Lkc_done_%d\n", COBRA_SCR_I, (long long)M, done_label);
    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    imul rax, %lld\n    mov QWORD PTR [rbp-%d], rax\n", COBRA_SCR_I, (long long)K, COBRA_SCR_IMK);
    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    imul rax, %lld\n    mov QWORD PTR [rbp-%d], rax\n", COBRA_SCR_I, (long long)N, COBRA_SCR_IN);
    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    lea rcx, [r10 + rax*4]\n", COBRA_SCR_IN);
    if (unroll) {
        if (N == 8) {
            /* Per row: re-point rsi at the A row; the B tile is fixed. */
            fprintf(cg->out, "    lea rdi, [r9]\n    mov rax, QWORD PTR [rbp-%d]\n    lea rsi, [r8 + rax*4]\n", COBRA_SCR_IMK);
            emit_gemm_const_kbody_unrolled(cg, K, N);
            if (add_bias)
                fprintf(cg->out, "    vmovups ymm1, YMMWORD PTR [r11]\n    vaddps ymm0, ymm0, ymm1\n    vxorps ymm1, ymm1, ymm1\n    vmaxps ymm0, ymm0, ymm1\n");
            fprintf(cg->out, "    vmovups YMMWORD PTR [rcx], ymm0\n    inc QWORD PTR [rbp-%d]\n    jmp .Lkc_i_%d\n", COBRA_SCR_I, i_label);
        } else {
            /* Per row, per tile: the A row is loop-invariant within the row
               and the tile counter lives in rbx (free because the K body is
               unrolled). */
            fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    lea rsi, [r8 + rax*4]\n", COBRA_SCR_IMK);
            fprintf(cg->out, "    xor ebx, ebx\n.Lkc_j_%d:\n    cmp rbx, %lld\n    jae .Lkc_next_i_%d\n", j_label, (long long)(N / 8), next_i_label);
            fprintf(cg->out, "    mov rax, rbx\n    shl rax, 3\n    lea rdi, [r9 + rax*4]\n");
            emit_gemm_const_kbody_unrolled(cg, K, N);
            if (add_bias) {
                fprintf(cg->out, "    mov rax, rbx\n    shl rax, 3\n    vmovups ymm1, YMMWORD PTR [r11 + rax*4]\n    vaddps ymm0, ymm0, ymm1\n    vxorps ymm1, ymm1, ymm1\n    vmaxps ymm0, ymm0, ymm1\n");
            }
            fprintf(cg->out, "    mov rax, rbx\n    shl rax, 3\n    vmovups YMMWORD PTR [rcx + rax*4], ymm0\n    inc rbx\n    jmp .Lkc_j_%d\n", j_label);
            fprintf(cg->out, ".Lkc_next_i_%d:\n    inc QWORD PTR [rbp-%d]\n    jmp .Lkc_i_%d\n", next_i_label, COBRA_SCR_I, i_label);
        }
    } else if (N == 8) {
        fprintf(cg->out, "    lea rdi, [r9]\n");
        fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    lea rsi, [r8 + rax*4]\n    lea rdx, [rsi + %lld]\n", COBRA_SCR_IMK, (long long)(K * 4));
        emit_gemm_const_kbody(cg, K, k_label, tail_label, combine_label);
        if (add_bias)
            fprintf(cg->out, "    vmovups ymm1, YMMWORD PTR [r11]\n    vaddps ymm0, ymm0, ymm1\n    vxorps ymm1, ymm1, ymm1\n    vmaxps ymm0, ymm0, ymm1\n");
        fprintf(cg->out, "    vmovups YMMWORD PTR [rcx], ymm0\n    inc QWORD PTR [rbp-%d]\n    jmp .Lkc_i_%d\n", COBRA_SCR_I, i_label);
    } else {
        fprintf(cg->out, "    mov QWORD PTR [rbp-%d], 0\n.Lkc_j_%d:\n", COBRA_SCR_J, j_label);
        fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    cmp rax, %lld\n    jae .Lkc_next_i_%d\n", COBRA_SCR_J, (long long)(N / 8), next_i_label);
        fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    shl rax, 3\n    lea rdi, [r9 + rax*4]\n", COBRA_SCR_J);
        /* rsi runs to a_end inside the k body, so re-point it per tile. */
        fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    lea rsi, [r8 + rax*4]\n    lea rdx, [rsi + %lld]\n", COBRA_SCR_IMK, (long long)(K * 4));
        emit_gemm_const_kbody(cg, K, k_label, tail_label, combine_label);
        if (add_bias) {
            fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    shl rax, 3\n    vmovups ymm1, YMMWORD PTR [r11 + rax*4]\n    vaddps ymm0, ymm0, ymm1\n    vxorps ymm1, ymm1, ymm1\n    vmaxps ymm0, ymm0, ymm1\n", COBRA_SCR_J);
        }
        fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    shl rax, 3\n    vmovups YMMWORD PTR [rcx + rax*4], ymm0\n    inc QWORD PTR [rbp-%d]\n    jmp .Lkc_j_%d\n", COBRA_SCR_J, COBRA_SCR_J, j_label);
        fprintf(cg->out, ".Lkc_next_i_%d:\n    inc QWORD PTR [rbp-%d]\n    jmp .Lkc_i_%d\n", next_i_label, COBRA_SCR_I, i_label);
    }
    fprintf(cg->out, ".Lkc_done_%d:\n    vzeroupper\n", done_label);
}

static void emit_gemm(CodeGen *cg, const char *a, const char *b, const char *c, ASTNode *m, ASTNode *n, ASTNode *k, bool add_bias, const char *bias) {
    /* Constant shapes lower to the specialized kernel: immediate trip bounds,
       no vector/scalar decision, no tail branches. Everything else keeps the
       checked generic lowering below. */
    int64_t cm, cn, ck;
    if (const_int_value(m, &cm) && const_int_value(n, &cn) && const_int_value(k, &ck) &&
        cm > 0 && cn > 0 && ck > 0 && (cn % 8) == 0 &&
        cm <= 4096 && cn <= 8192 && ck <= 65536) {
        emit_gemm_const(cg, a, b, c, cm, cn, ck, add_bias, bias);
        return;
    }
    /*
     * Keep the lowering direct: this is a specialized eight-column kernel,
     * not an IR or a runtime dispatch. Each full N-tile broadcasts one A
     * element, loads eight contiguous B elements, and accumulates with FMA.
     * The scalar path handles every N tail, so odd dimensions and tiny
     * matrices retain the old checked semantics.
     *
     * The tensor entry points are gated by main.c to Linux x86-64 hosts with
     * AVX2+FMA. `vmovups` is intentional because calloc does not promise
     * 32-byte alignment. No callee is called while YMM values are live.
     */
    int label = cg->label_count++;
    int vector = cg->label_count++;
    int full8 = cg->label_count++;
    int block16 = cg->label_count++;
    int block16_tail = cg->label_count++;
    int block16_combine = cg->label_count++;
    int scalar = cg->label_count++;
    int scalar_store = cg->label_count++;
    int tail = cg->label_count++;

    emit_expr(cg, m); fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", COBRA_SCR_M);
    emit_expr(cg, n); fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", COBRA_SCR_N);
    emit_expr(cg, k); fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", COBRA_SCR_K);
    /* Automatic GPU dispatch: no gpu_* call is required in source. Runtime
       checks element count against COBRA_GPU_DISPATCH_THRESHOLD and picks
       whichever GPU cobra_vk_pick_device finds (any Vulkan ICD - AMD, Intel,
       NVIDIA, Apple via MoltenVK), so this is vendor-neutral by construction.
       cobra_gpu_matmul_f32 returns 0 on any failure (no device, OOM, driver
       reject), in which case the AVX2 kernel below runs unchanged - the GPU
       path is strictly additive and never regresses correctness. The dispatch
       check must run before any buffer pointer is loaded into r8-r11: those
       are caller-saved under the SysV ABI, so a call clobbers them. */
    if (g_gpu_enabled) {
        int gpu_skip = cg->label_count++;
        fprintf(cg->out,
            "    mov rax, QWORD PTR [rbp-%d]\n    imul rax, QWORD PTR [rbp-%d]\n    imul rax, QWORD PTR [rbp-%d]\n"
            "    mov rdi, rax\n    call cobra_gpu_should_dispatch@PLT\n    cmp rax, 0\n    je .Lmm_gpu_skip_%d\n",
            COBRA_SCR_M, COBRA_SCR_N, COBRA_SCR_K, gpu_skip);
        emit_load_buffer_ptr(cg, a, "rdi");
        emit_load_buffer_ptr(cg, b, "rsi");
        emit_load_buffer_ptr(cg, c, "rdx");
        fprintf(cg->out, "    mov rcx, QWORD PTR [rbp-%d]\n    mov r8, QWORD PTR [rbp-%d]\n    mov r9, QWORD PTR [rbp-%d]\n",
                COBRA_SCR_M, COBRA_SCR_N, COBRA_SCR_K);
        fprintf(cg->out, "    sub rsp, 16\n");
        if (add_bias) {
            emit_load_buffer_ptr(cg, bias, "rax");
            fprintf(cg->out, "    mov QWORD PTR [rsp], rax\n    mov QWORD PTR [rsp+8], 1\n");
        } else {
            fprintf(cg->out, "    mov QWORD PTR [rsp], 0\n    mov QWORD PTR [rsp+8], 0\n");
        }
        fprintf(cg->out, "    call cobra_gpu_matmul_f32@PLT\n    add rsp, 16\n    cmp rax, 0\n    jne .Lmm_done_%d\n",
                label);
        fprintf(cg->out, ".Lmm_gpu_skip_%d:\n", gpu_skip);
    }

    emit_load_buffer_ptr(cg, a, "r8");
    emit_load_buffer_ptr(cg, b, "r9");
    emit_load_buffer_ptr(cg, c, "r10");
    if (add_bias) emit_load_buffer_ptr(cg, bias, "r11");
    /* B rows are n floats apart, so the running B pointer advances by n*4
       bytes per k step. The frame restores rbx at function exit and no callee
       is called while the kernel runs, so rbx is free for the stride. */
    fprintf(cg->out, "    mov rbx, QWORD PTR [rbp-%d]\n    shl rbx, 2\n", COBRA_SCR_N);

    fprintf(cg->out, "    mov QWORD PTR [rbp-%d], 0\n.Lmm_i_%d:\n", COBRA_SCR_I, label);
    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    cmp rax, QWORD PTR [rbp-%d]\n    jae .Lmm_done_%d\n", COBRA_SCR_I, COBRA_SCR_M, label);
    /* i*k and i*n are invariant across the whole j loop: hoist them once. */
    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    imul rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", COBRA_SCR_I, COBRA_SCR_K, COBRA_SCR_IMK);
    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    imul rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", COBRA_SCR_I, COBRA_SCR_N, COBRA_SCR_IN);
    fprintf(cg->out, "    mov QWORD PTR [rbp-%d], 0\n.Lmm_j_%d:\n", COBRA_SCR_J, label);
    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    cmp rax, QWORD PTR [rbp-%d]\n    jae .Lmm_next_i_%d\n", COBRA_SCR_J, COBRA_SCR_N, label);

    /* Two adjacent eight-column tiles share each A broadcast. This is the
       generic output-blocked path: it keeps the existing row-major ABI, does
       not allocate a packed buffer, and falls through to the old eight-column
       path whenever the runtime width has fewer than sixteen columns left. */
    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    add rax, 16\n    cmp rax, QWORD PTR [rbp-%d]\n    ja .Lmm_full8_%d\n", COBRA_SCR_J, COBRA_SCR_N, full8);
    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    lea rsi, [r8 + rax*4]\n    mov rax, QWORD PTR [rbp-%d]\n    lea rdx, [rsi + rax*4]\n", COBRA_SCR_IMK, COBRA_SCR_K);
    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    lea rdi, [r9 + rax*4]\n    mov rax, QWORD PTR [rbp-%d]\n    add rax, QWORD PTR [rbp-%d]\n    lea rcx, [r10 + rax*4]\n", COBRA_SCR_J, COBRA_SCR_IN, COBRA_SCR_J);
    fprintf(cg->out, "    vxorps ymm0, ymm0, ymm0\n    vxorps ymm1, ymm1, ymm1\n    vxorps ymm2, ymm2, ymm2\n    vxorps ymm3, ymm3, ymm3\n    vxorps ymm4, ymm4, ymm4\n    vxorps ymm5, ymm5, ymm5\n    vxorps ymm6, ymm6, ymm6\n    vxorps ymm7, ymm7, ymm7\n");
    fprintf(cg->out, ".Lmm_bk16_%d:\n    lea rax, [rsi + 12]\n    cmp rax, rdx\n    jae .Lmm_b16tail_%d\n", block16, block16_tail);
    fprintf(cg->out, "    vbroadcastss ymm8, DWORD PTR [rsi]\n    vmovups ymm9, YMMWORD PTR [rdi]\n    vfmadd231ps ymm0, ymm9, ymm8\n    vmovups ymm9, YMMWORD PTR [rdi + 32]\n    vfmadd231ps ymm4, ymm9, ymm8\n    add rdi, rbx\n");
    fprintf(cg->out, "    vbroadcastss ymm8, DWORD PTR [rsi + 4]\n    vmovups ymm9, YMMWORD PTR [rdi]\n    vfmadd231ps ymm1, ymm9, ymm8\n    vmovups ymm9, YMMWORD PTR [rdi + 32]\n    vfmadd231ps ymm5, ymm9, ymm8\n    add rdi, rbx\n");
    fprintf(cg->out, "    vbroadcastss ymm8, DWORD PTR [rsi + 8]\n    vmovups ymm9, YMMWORD PTR [rdi]\n    vfmadd231ps ymm2, ymm9, ymm8\n    vmovups ymm9, YMMWORD PTR [rdi + 32]\n    vfmadd231ps ymm6, ymm9, ymm8\n    add rdi, rbx\n");
    fprintf(cg->out, "    vbroadcastss ymm8, DWORD PTR [rsi + 12]\n    vmovups ymm9, YMMWORD PTR [rdi]\n    vfmadd231ps ymm3, ymm9, ymm8\n    vmovups ymm9, YMMWORD PTR [rdi + 32]\n    vfmadd231ps ymm7, ymm9, ymm8\n    add rdi, rbx\n    add rsi, 16\n    jmp .Lmm_bk16_%d\n", block16);
    fprintf(cg->out, ".Lmm_b16tail_%d:\n    cmp rsi, rdx\n    jae .Lmm_b16combine_%d\n", block16_tail, block16_combine);
    fprintf(cg->out, "    vbroadcastss ymm8, DWORD PTR [rsi]\n    vmovups ymm9, YMMWORD PTR [rdi]\n    vfmadd231ps ymm0, ymm9, ymm8\n    vmovups ymm9, YMMWORD PTR [rdi + 32]\n    vfmadd231ps ymm4, ymm9, ymm8\n    add rsi, 4\n    add rdi, rbx\n    jmp .Lmm_b16tail_%d\n", block16_tail);
    fprintf(cg->out, ".Lmm_b16combine_%d:\n    vaddps ymm0, ymm0, ymm1\n    vaddps ymm0, ymm0, ymm2\n    vaddps ymm0, ymm0, ymm3\n    vaddps ymm4, ymm4, ymm5\n    vaddps ymm4, ymm4, ymm6\n    vaddps ymm4, ymm4, ymm7\n", block16_combine);
    if (add_bias) {
        fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    vmovups ymm1, YMMWORD PTR [r11 + rax*4]\n    vmovups ymm2, YMMWORD PTR [r11 + rax*4 + 32]\n    vaddps ymm0, ymm0, ymm1\n    vaddps ymm4, ymm4, ymm2\n    vxorps ymm1, ymm1, ymm1\n    vmaxps ymm0, ymm0, ymm1\n    vxorps ymm1, ymm1, ymm1\n    vmaxps ymm4, ymm4, ymm1\n", COBRA_SCR_J);
    }
    fprintf(cg->out, "    vmovups YMMWORD PTR [rcx], ymm0\n    vmovups YMMWORD PTR [rcx + 32], ymm4\n    add QWORD PTR [rbp-%d], 16\n    jmp .Lmm_j_%d\n", COBRA_SCR_J, label);

    /* A full eight-column tile is the hot path. */
    fprintf(cg->out, ".Lmm_full8_%d:\n    mov rax, QWORD PTR [rbp-%d]\n    add rax, 8\n    cmp rax, QWORD PTR [rbp-%d]\n    ja .Lmm_scalar_%d\n", full8, COBRA_SCR_J, COBRA_SCR_N, scalar);
    /* Tile setup: a_ptr = A + i*k*4 with end pointer a_end = a_ptr + k*4,
       b_ptr = B + j*4, c_ptr = C + (i*n+j)*4. All address math is done once
       per tile; the k loop only advances the running pointers. */
    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    lea rsi, [r8 + rax*4]\n    mov rax, QWORD PTR [rbp-%d]\n    lea rdx, [rsi + rax*4]\n", COBRA_SCR_IMK, COBRA_SCR_K);
    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    lea rdi, [r9 + rax*4]\n", COBRA_SCR_J);
    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    add rax, QWORD PTR [rbp-%d]\n    lea rcx, [r10 + rax*4]\n", COBRA_SCR_IN, COBRA_SCR_J);
    fprintf(cg->out, "    vxorps ymm0, ymm0, ymm0\n    vxorps ymm1, ymm1, ymm1\n    vxorps ymm2, ymm2, ymm2\n    vxorps ymm3, ymm3, ymm3\n");
    /* Four independent accumulators, four B rows per iteration. rsi advances
       4 bytes per k step, rdi advances rbx (n*4) bytes per k step. */
    fprintf(cg->out, ".Lmm_vk_%d:\n    lea rax, [rsi + 12]\n    cmp rax, rdx\n    jae .Lmm_vtail_%d\n", vector, tail);
    /* Each B row is rbx bytes apart; advance rdi between loads so every
       operand is a plain [rdi] (SIB scales are limited to 1, 2, 4, 8). */
    fprintf(cg->out, "    vbroadcastss ymm4, DWORD PTR [rsi]\n    vmovups ymm5, YMMWORD PTR [rdi]\n    vfmadd231ps ymm0, ymm5, ymm4\n    add rdi, rbx\n");
    fprintf(cg->out, "    vbroadcastss ymm4, DWORD PTR [rsi + 4]\n    vmovups ymm5, YMMWORD PTR [rdi]\n    vfmadd231ps ymm1, ymm5, ymm4\n    add rdi, rbx\n");
    fprintf(cg->out, "    vbroadcastss ymm4, DWORD PTR [rsi + 8]\n    vmovups ymm5, YMMWORD PTR [rdi]\n    vfmadd231ps ymm2, ymm5, ymm4\n    add rdi, rbx\n");
    fprintf(cg->out, "    vbroadcastss ymm4, DWORD PTR [rsi + 12]\n    vmovups ymm5, YMMWORD PTR [rdi]\n    vfmadd231ps ymm3, ymm5, ymm4\n    add rdi, rbx\n");
    fprintf(cg->out, "    add rsi, 16\n    jmp .Lmm_vk_%d\n", vector);
    /* 0-3 remaining k steps run one at a time with the same running pointers. */
    fprintf(cg->out, ".Lmm_vtail_%d:\n    cmp rsi, rdx\n    jae .Lmm_vcombine_%d\n", tail, tail);
    fprintf(cg->out, "    vbroadcastss ymm4, DWORD PTR [rsi]\n    vmovups ymm5, YMMWORD PTR [rdi]\n    vfmadd231ps ymm0, ymm5, ymm4\n    add rsi, 4\n    add rdi, rbx\n    jmp .Lmm_vtail_%d\n", tail);
    fprintf(cg->out, ".Lmm_vcombine_%d:\n    vaddps ymm0, ymm0, ymm1\n    vaddps ymm0, ymm0, ymm2\n    vaddps ymm0, ymm0, ymm3\n", tail);
    if (add_bias) {
        fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    vmovups ymm1, YMMWORD PTR [r11 + rax*4]\n    vaddps ymm0, ymm0, ymm1\n    vxorps ymm1, ymm1, ymm1\n    vmaxps ymm0, ymm0, ymm1\n", COBRA_SCR_J);
    }
    fprintf(cg->out, "    vmovups YMMWORD PTR [rcx], ymm0\n    add QWORD PTR [rbp-%d], 8\n    jmp .Lmm_j_%d\n", COBRA_SCR_J, label);

    /* Existing scalar lowering is retained for N tails and all small widths. */
    fprintf(cg->out, ".Lmm_scalar_%d:\n    vzeroupper\n    xorps xmm0, xmm0\n    mov QWORD PTR [rbp-%d], 0\n.Lmm_k_%d:\n", scalar, COBRA_SCR_KI, scalar);
    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    cmp rax, QWORD PTR [rbp-%d]\n    jae .Lmm_sstore_%d\n", COBRA_SCR_KI, COBRA_SCR_K, scalar_store);
    fprintf(cg->out, "    mov rdx, QWORD PTR [rbp-%d]\n    add rdx, QWORD PTR [rbp-%d]\n    movss xmm1, DWORD PTR [r8 + rdx*4]\n", COBRA_SCR_IMK, COBRA_SCR_KI);
    fprintf(cg->out, "    mov rdx, QWORD PTR [rbp-%d]\n    imul rdx, QWORD PTR [rbp-%d]\n    add rdx, QWORD PTR [rbp-%d]\n    movss xmm2, DWORD PTR [r9 + rdx*4]\n    mulss xmm1, xmm2\n    addss xmm0, xmm1\n    inc QWORD PTR [rbp-%d]\n    jmp .Lmm_k_%d\n", COBRA_SCR_KI, COBRA_SCR_N, COBRA_SCR_J, COBRA_SCR_KI, scalar);
    fprintf(cg->out, ".Lmm_sstore_%d:\n    mov rdx, QWORD PTR [rbp-%d]\n    add rdx, QWORD PTR [rbp-%d]\n", scalar_store, COBRA_SCR_IN, COBRA_SCR_J);
    if (add_bias) {
        fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    movss xmm1, DWORD PTR [r11 + rax*4]\n    addss xmm0, xmm1\n    xorps xmm2, xmm2\n    maxss xmm0, xmm2\n", COBRA_SCR_J);
    }
    fprintf(cg->out, "    movss DWORD PTR [r10 + rdx*4], xmm0\n    inc QWORD PTR [rbp-%d]\n    jmp .Lmm_j_%d\n.Lmm_next_i_%d:\n    inc QWORD PTR [rbp-%d]\n    jmp .Lmm_i_%d\n.Lmm_done_%d:\n    vzeroupper\n", COBRA_SCR_J, label, label, COBRA_SCR_I, label, label);
}

/* matmul_f32_backward(a, b, dc, da, db, M, N, K) -> i64. Calls
   cobra_gpu_matmul_backward_f32(a_ptr, b_ptr, dc_ptr, da_ptr, db_ptr, M, N, K)
   directly - buffer arguments must be plain variables (same restriction as
   the @gpu kernel call paths above), M/N/K are ordinary i64 expressions.
   SysV passes the first 6 args in rdi/rsi/rdx/rcx/r8/r9 and the rest on the
   stack, so N and K (args 7 and 8) go through a 16-byte-aligned stack push. */
static void emit_matmul_backward(CodeGen *cg, ASTNode *n) {
    if (n->child_count != 8) {
        fprintf(stderr, "CodeGen Error: matmul_f32_backward requires (a, b, dc, da, db, M, N, K)\n");
        exit(EXIT_FAILURE);
    }
    for (int i = 0; i < 5; i++) {
        if (n->children[i]->type != AST_VAR_REF) {
            fprintf(stderr, "CodeGen Error: matmul_f32_backward's buffer arguments must be plain variables\n");
            exit(EXIT_FAILURE);
        }
    }
    int scratch = reserve(cg, 8 * 8);
    static const char *ptr_regs[5] = {"rdi", "rsi", "rdx", "rcx", "r8"};
    for (int i = 0; i < 5; i++) {
        emit_load_buffer_ptr(cg, n->children[i]->name, "rax");
        fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", scratch - 8 * i);
    }
    for (int i = 5; i < 8; i++) {
        emit_expr(cg, n->children[i]);
        fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", scratch - 8 * i);
    }
    for (int i = 0; i < 5; i++)
        fprintf(cg->out, "    mov %s, QWORD PTR [rbp-%d]\n", ptr_regs[i], scratch - 8 * i);
    fprintf(cg->out, "    mov r9, QWORD PTR [rbp-%d]\n", scratch - 8 * 5); /* M */
    fprintf(cg->out, "    sub rsp, 16\n");
    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rsp], rax\n", scratch - 8 * 6);   /* N */
    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rsp+8], rax\n", scratch - 8 * 7); /* K */
    fprintf(cg->out, "    call cobra_gpu_matmul_backward_f32@PLT\n    add rsp, 16\n");
}

static void emit_matmul(CodeGen *cg, ASTNode *n) {
    emit_gemm(cg, n->children[0]->name, n->children[1]->name, n->children[2]->name,
              n->children[3], n->children[4], n->children[5], false, NULL);
}
static void emit_dense(CodeGen *cg, ASTNode *n) {
    emit_gemm(cg, n->children[0]->name, n->children[1]->name, n->children[3]->name,
              n->children[4], n->children[5], n->children[6], true, n->children[2]->name);
}

/* Calls a user `@gpu` kernel. Its body was lowered to SPIR-V and wrapped in
   a real C function of the same name by the generated <program>_gpu_kernels.c
   (see src/gpu_lower.c / main.c), with signature
   (float **bufs, int64_t *lens, <scalar params in declaration order>) - not
   Cobra's normal calling convention, so this bypasses the generic argument
   marshalling below entirely. Buffer parameters may appear in any position
   and there may be more than one; each buffer argument at the call site
   must still be a plain variable (arbitrary buffer-valued expressions
   aren't supported). Scalars beyond 4 int/8 float still silently lose their
   value (no stack-spill path here yet), matching the pre-existing register
   budget for the ordinary call path this bypasses. */
enum { GPU_CALL_MAX_ARGS = 32 };

/* Shared marshalling core for both a direct @gpu kernel call and a
   `<kernel>_backward` call: both wrappers share the (float **bufs,
   int64_t *lens, <scalars>) ABI, differing only in which call-site
   arguments are buffers vs. scalars and what the callee symbol is named. */
static void emit_gpu_wrapper_call(CodeGen *cg, ASTNode *call, const char *callee_name,
                                   const bool *arg_is_buffer, const CobraTypeKind *arg_scalar_type,
                                   int nparams, int nbuf, int nscalar) {
    for (int i = 0; i < nparams; i++) {
        if (arg_is_buffer[i] && call->children[i]->type != AST_VAR_REF) {
            fprintf(stderr, "CodeGen Error: '%s' call must pass buffer arguments as plain variables\n", callee_name);
            exit(EXIT_FAILURE);
        }
    }

    /* Scratch layout: nbuf buffer pointers, nbuf buffer lengths (two
       parallel arrays the wrapper receives as float** / int64_t*), then one
       8-byte slot per scalar argument. */
    int scratch = reserve(cg, 8 * (2 * nbuf + nscalar > 0 ? 2 * nbuf + nscalar : 1));
    int bufs_base = scratch, lens_base = scratch - 8 * nbuf;
    int scalar_base = lens_base - 8 * nbuf;

    int buf_idx = 0, scalar_idx = 0;
    for (int i = 0; i < nparams; i++) {
        if (arg_is_buffer[i]) {
            emit_load_buffer_ptr(cg, call->children[i]->name, "rax");
            fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", bufs_base - 8 * buf_idx);
            emit_load_buffer_len(cg, call->children[i]->name, "rax");
            fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", lens_base - 8 * buf_idx);
            buf_idx++;
        } else {
            int slot = scalar_base - 8 * scalar_idx;
            emit_expr(cg, call->children[i]);
            if (arg_scalar_type[i] == COBRA_TYPE_F32) {
                if (!expression_is_float(call->children[i])) fprintf(cg->out, "    cvtsi2ss xmm0, rax\n");
                fprintf(cg->out, "    movss DWORD PTR [rbp-%d], xmm0\n", slot);
            } else {
                fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", slot);
            }
            scalar_idx++;
        }
    }

    static const char *gp_regs[4] = {"rdx", "rcx", "r8", "r9"};
    int gp_idx = 0, sse_idx = 0;
    fprintf(cg->out, "    lea rdi, [rbp-%d]\n    lea rsi, [rbp-%d]\n", bufs_base, lens_base);
    scalar_idx = 0;
    for (int i = 0; i < nparams; i++) {
        if (arg_is_buffer[i]) continue;
        int slot = scalar_base - 8 * scalar_idx;
        if (arg_scalar_type[i] == COBRA_TYPE_F32) {
            if (sse_idx < 8) fprintf(cg->out, "    movss %s, DWORD PTR [rbp-%d]\n", SYSV_XMM_REGS[sse_idx], slot);
            sse_idx++;
        } else {
            if (gp_idx < 4) fprintf(cg->out, "    mov %s, QWORD PTR [rbp-%d]\n", gp_regs[gp_idx], slot);
            gp_idx++;
        }
        scalar_idx++;
    }
    fprintf(cg->out, "    call %s@PLT\n", callee_name);
}

/* Calls a user `@gpu` kernel. Its body was lowered to SPIR-V and wrapped in
   a real C function of the same name by the generated <program>_gpu_kernels.c
   (see src/gpu_lower.c / main.c), with signature
   (float **bufs, int64_t *lens, <scalar params in declaration order>) - not
   Cobra's normal calling convention, so this bypasses the generic argument
   marshalling below entirely. Buffer parameters may appear in any position
   and there may be more than one; each buffer argument at the call site
   must still be a plain variable (arbitrary buffer-valued expressions
   aren't supported). Scalars beyond 4 int/8 float still silently lose their
   value (no stack-spill path here yet), matching the pre-existing register
   budget for the ordinary call path this bypasses. */
static void emit_gpu_kernel_call(CodeGen *cg, ASTNode *call, ASTNode *fn) {
    bool arg_is_buffer[GPU_CALL_MAX_ARGS];
    CobraTypeKind arg_scalar_type[GPU_CALL_MAX_ARGS];
    int nparams = 0, nbuf = 0, nscalar = 0;
    for (size_t i = 0; i < fn->child_count && nparams < GPU_CALL_MAX_ARGS; i++) {
        ASTNode *p = fn->children[i];
        if (p->type != AST_PARAM) continue;
        bool is_buf = p->declared_type == COBRA_TYPE_SLICE_F32;
        arg_is_buffer[nparams] = is_buf;
        arg_scalar_type[nparams] = is_buf ? COBRA_TYPE_UNTYPED : p->declared_type;
        if (is_buf) nbuf++; else nscalar++;
        nparams++;
    }
    if ((int)call->child_count != nparams) {
        fprintf(stderr, "CodeGen Error: @gpu kernel '%s' called with %zu arguments, expected %d\n",
                call->name, call->child_count, nparams);
        exit(EXIT_FAILURE);
    }
    emit_gpu_wrapper_call(cg, call, call->name, arg_is_buffer, arg_scalar_type, nparams, nbuf, nscalar);
}

/* Calls `<kernel>_backward`, the compiler-generated reverse-mode gradient
   kernel for an elementwise @gpu kernel (see cobra_gpu_lower_backward in
   gpu_lower.c). Its ABI is derived mechanically from the forward kernel
   `fn`'s own params: grad_out, then fn's original buffers, then fn's
   original scalars, then one grad_<buffer> output per original buffer, then
   one grad_<scalar>_partial output per original scalar - all buffer-shaped
   arguments (including the gradient outputs) go through the same
   (float**, int64_t*) wrapper ABI as a direct kernel call. Whether the
   wrapper actually exists (the forward kernel might not have qualified for
   autodiff) is not known here - an ineligible kernel fails at link time
   with "undefined reference", not here. */
static void emit_gpu_backward_call(CodeGen *cg, ASTNode *call, ASTNode *fn, const char *backward_name) {
    bool arg_is_buffer[GPU_CALL_MAX_ARGS];
    CobraTypeKind arg_scalar_type[GPU_CALL_MAX_ARGS];
    int nparams = 0, nbuf = 0, nscalar = 0, orig_nbuf = 0, orig_nscalar = 0;
    for (size_t i = 0; i < fn->child_count; i++) {
        ASTNode *p = fn->children[i];
        if (p->type != AST_PARAM) continue;
        if (p->declared_type == COBRA_TYPE_SLICE_F32) orig_nbuf++; else orig_nscalar++;
    }
    /* grad_out */
    if (nparams < GPU_CALL_MAX_ARGS) { arg_is_buffer[nparams] = true; arg_scalar_type[nparams] = COBRA_TYPE_UNTYPED; nparams++; nbuf++; }
    /* original buffers (read) */
    for (int i = 0; i < orig_nbuf && nparams < GPU_CALL_MAX_ARGS; i++) { arg_is_buffer[nparams] = true; arg_scalar_type[nparams] = COBRA_TYPE_UNTYPED; nparams++; nbuf++; }
    /* original scalars, in declared order/type */
    for (size_t i = 0; i < fn->child_count && nparams < GPU_CALL_MAX_ARGS; i++) {
        ASTNode *p = fn->children[i];
        if (p->type != AST_PARAM || p->declared_type == COBRA_TYPE_SLICE_F32) continue;
        arg_is_buffer[nparams] = false; arg_scalar_type[nparams] = p->declared_type; nparams++; nscalar++;
    }
    /* grad_<buffer> outputs */
    for (int i = 0; i < orig_nbuf && nparams < GPU_CALL_MAX_ARGS; i++) { arg_is_buffer[nparams] = true; arg_scalar_type[nparams] = COBRA_TYPE_UNTYPED; nparams++; nbuf++; }
    /* grad_<scalar>_partial outputs */
    for (int i = 0; i < orig_nscalar && nparams < GPU_CALL_MAX_ARGS; i++) { arg_is_buffer[nparams] = true; arg_scalar_type[nparams] = COBRA_TYPE_UNTYPED; nparams++; nbuf++; }

    if ((int)call->child_count != nparams) {
        fprintf(stderr, "CodeGen Error: '%s' called with %zu arguments, expected %d\n",
                backward_name, call->child_count, nparams);
        exit(EXIT_FAILURE);
    }
    emit_gpu_wrapper_call(cg, call, backward_name, arg_is_buffer, arg_scalar_type, nparams, nbuf, nscalar);
}

/* Calls a @gpu kernel's resident fast path: `<kernel>_gpu(...)`, wrapped by
   the generated <program>_gpu_kernels.c as
   (int64_t handle_or_scalar, ...) - every argument is a plain 8-byte scalar
   (buffer parameters take an i64 handle from gpu_alloc_f32 instead of a
   []f32 array), so this is ordinary SysV register marshalling with no
   pointer/length array construction, unlike emit_gpu_kernel_call above. */
static void emit_gpu_resident_call(CodeGen *cg, ASTNode *call, ASTNode *fn) {
    enum { GPU_RESIDENT_MAX_ARGS = 16 };
    CobraTypeKind arg_type[GPU_RESIDENT_MAX_ARGS];
    int nparams = 0;
    for (size_t i = 0; i < fn->child_count && nparams < GPU_RESIDENT_MAX_ARGS; i++) {
        ASTNode *p = fn->children[i];
        if (p->type != AST_PARAM) continue;
        arg_type[nparams++] = (p->declared_type == COBRA_TYPE_SLICE_F32) ? COBRA_TYPE_I64 : p->declared_type;
    }
    if ((int)call->child_count != nparams) {
        fprintf(stderr, "CodeGen Error: '%s' called with %zu arguments, expected %d\n", call->name, call->child_count, nparams);
        exit(EXIT_FAILURE);
    }

    int base = reserve(cg, 8 * (nparams > 0 ? nparams : 1));
    for (int i = 0; i < nparams; i++) {
        int slot = base - 8 * i;
        emit_expr(cg, call->children[i]);
        if (arg_type[i] == COBRA_TYPE_F32) {
            if (!expression_is_float(call->children[i])) fprintf(cg->out, "    cvtsi2ss xmm0, rax\n");
            fprintf(cg->out, "    movss DWORD PTR [rbp-%d], xmm0\n", slot);
        } else {
            fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", slot);
        }
    }
    static const char *gp_regs[6] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    int gp_idx = 0, sse_idx = 0;
    for (int i = 0; i < nparams; i++) {
        int slot = base - 8 * i;
        if (arg_type[i] == COBRA_TYPE_F32) {
            if (sse_idx < 8) fprintf(cg->out, "    movss %s, DWORD PTR [rbp-%d]\n", SYSV_XMM_REGS[sse_idx], slot);
            sse_idx++;
        } else {
            if (gp_idx < 6) fprintf(cg->out, "    mov %s, QWORD PTR [rbp-%d]\n", gp_regs[gp_idx], slot);
            gp_idx++;
        }
    }
    fprintf(cg->out, "    call %s@PLT\n", call->name);
}

static ASTNode *find_trait_decl(CodeGen *cg, const char *trait_name) {
    if (!cg->root || !trait_name) return NULL;
    for (size_t k = 0; k < cg->root->child_count; k++) {
        ASTNode *d = cg->root->children[k];
        if (d->type == AST_TRAIT_DECL && !strcmp(d->name, trait_name)) return d;
    }
    return NULL;
}

/* A `dyn TraitName`-typed value is one pointer to a heap block: word 0 is
   the concrete instance's address, words 1..N are one code pointer per
   trait method (trait declaration order), each pointing at that concrete
   type's mangled static-dispatch impl (__impl_<Trait>_<Type>_<method>,
   already emitted by the ordinary impl-registration path). Building this
   block is the coercion step (struct value -> dyn Trait); reading it back
   is emit_dyn_dispatch_call below. ir.c's cobra_ir_build already verified
   the concrete type implements every trait method before codegen runs. */
/* Emit (once per program) a static .rodata method-pointer array for one
   (Trait,ConcreteType) pairing and write its label into `label_out`. Every
   dispatch block for that pairing points its second word at this same
   array instead of a per-call malloc'd copy of the method pointers - the
   method pointers never vary across instances of the same concrete type,
   so there is nothing per-instance to allocate for them. */
static void emit_dyn_vtable_label(CodeGen *cg, const char *trait_name, ASTNode *trait_decl,
                                   const char *type_name, char *label_out, size_t label_out_size) {
    snprintf(label_out, label_out_size, ".Ldyn_vtable_%.31s_%.31s", trait_name, type_name);
    char key[COBRA_MAX_IDENT_LEN * 2];
    snprintf(key, sizeof(key), "%.63s|%.63s", trait_name, type_name);
    for (int i = 0; i < cg->dyn_vtable_count; i++) {
        if (!strcmp(cg->dyn_vtables_emitted[i], key)) return;
    }
    if (cg->dyn_vtable_count < 128) {
        snprintf(cg->dyn_vtables_emitted[cg->dyn_vtable_count++], sizeof(cg->dyn_vtables_emitted[0]), "%s", key);
    }
    fprintf(cg->out, "    .section .rodata\n    .align 8\n%s:\n", label_out);
    for (size_t m = 0; m < trait_decl->child_count; m++) {
        fprintf(cg->out, "    .quad __impl_%.31s_%.31s_%.31s\n",
                trait_name, type_name, trait_decl->children[m]->name);
    }
    fprintf(cg->out, "    .text\n");
}

/* Build a `dyn TraitName` dispatch block for the struct value held by `s`
   and store the resulting pointer at dest_slot. Mirrors the block
   construction inside emit_dyn_trait_call's per-argument loop, factored out
   so `let x: dyn Trait = value` and `return` can build the same block.

   The block is always exactly 2 words: word 0 is the data pointer, word 1
   is the address of the static per-(Trait,Type) vtable from
   emit_dyn_vtable_label. The vtable itself is zero-allocation and shared
   across every dispatch-block construction for the same pairing, so the
   remaining malloc is a fixed 16 bytes regardless of trait size.

   heap_copy_data must be true whenever the dispatch block's data_ptr can
   outlive the struct's current stack frame - concretely, a `-> dyn Trait`
   return, where `s` is a local of the returning function: its stack slot is
   gone the instant the caller resumes, so data_ptr would dangle if it
   pointed at the frame directly. Call-argument and same-frame `let`
   coercions never outlive the frame `s` lives in, so they pass false and
   point straight at the stack value, matching the original (pre-return)
   behavior exactly. */
static void emit_build_dyn_dispatch_block_ex(CodeGen *cg, const char *trait_name,
                                              VarSymbol *s, int dest_slot, bool heap_copy_data) {
    ASTNode *trait_decl = find_trait_decl(cg, trait_name);
    if (!trait_decl) {
        fprintf(stderr, "CodeGen Error: unknown trait '%s'\n", trait_name);
        exit(EXIT_FAILURE);
    }
    char vtable_label[80];
    emit_dyn_vtable_label(cg, trait_name, trait_decl, s->type_name, vtable_label, sizeof(vtable_label));
    fprintf(cg->out, "    mov rdi, 16\n    call malloc@PLT\n    mov QWORD PTR [rbp-%d], rax\n", dest_slot);
    if (heap_copy_data) {
        int struct_size = struct_storage_size(cg, s->type_name);
        fprintf(cg->out, "    mov rdi, %d\n    call malloc@PLT\n    mov rbx, rax\n", struct_size);
        fprintf(cg->out, "    lea rsi, [rbp-%d]\n    mov rdi, rbx\n", s->array_base);
        emit_copy_memory(cg, "rsi", "rdi", struct_size);
    } else {
        fprintf(cg->out, "    lea rbx, [rbp-%d]\n", s->array_base);
    }
    fprintf(cg->out, "    mov rcx, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rcx], rbx\n", dest_slot);
    fprintf(cg->out, "    mov rcx, QWORD PTR [rbp-%d]\n    lea rax, [rip+%s]\n    mov QWORD PTR [rcx+8], rax\n",
            dest_slot, vtable_label);
}

static void emit_build_dyn_dispatch_block(CodeGen *cg, const char *trait_name,
                                           VarSymbol *s, int dest_slot) {
    emit_build_dyn_dispatch_block_ex(cg, trait_name, s, dest_slot, false);
}

static void emit_dyn_trait_call(CodeGen *cg, ASTNode *n, ASTNode *fn) {
    (void)fn;
    int arg_slot = reserve(cg, (int)n->child_count * 8 + 8);
    size_t param_index = 0;
    for (size_t i = 0; i < n->child_count; i++) {
        ASTNode *param = function_param_node(cg, n->name, param_index++);
        ASTNode *arg = n->children[i];
        int slot = arg_slot + (int)i * 8;
        if (param && param->dyn_trait_name[0]) {
            if (arg->type != AST_VAR_REF) {
                fprintf(stderr, "CodeGen Error: dyn %s argument must be a named struct value\n", param->dyn_trait_name);
                exit(EXIT_FAILURE);
            }
            VarSymbol *s = find_symbol(cg, arg->name);
            if (!s || s->kind != SYM_STRUCT) {
                fprintf(stderr, "CodeGen Error: '%s' is not a struct value coercible to dyn %s\n", arg->name, param->dyn_trait_name);
                exit(EXIT_FAILURE);
            }
            ASTNode *trait_decl = find_trait_decl(cg, param->dyn_trait_name);
            if (!trait_decl) {
                fprintf(stderr, "CodeGen Error: unknown trait '%s'\n", param->dyn_trait_name);
                exit(EXIT_FAILURE);
            }
            char vtable_label[80];
            emit_dyn_vtable_label(cg, param->dyn_trait_name, trait_decl, s->type_name, vtable_label, sizeof(vtable_label));
            fprintf(cg->out, "    mov rdi, 16\n    call malloc@PLT\n    mov QWORD PTR [rbp-%d], rax\n", slot);
            fprintf(cg->out, "    lea rbx, [rbp-%d]\n    mov rcx, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rcx], rbx\n",
                    s->array_base, slot);
            fprintf(cg->out, "    mov rcx, QWORD PTR [rbp-%d]\n    lea rax, [rip+%s]\n    mov QWORD PTR [rcx+8], rax\n",
                    slot, vtable_label);
        } else if (expression_is_float_codegen(cg, arg)) {
            emit_expr(cg, arg);
            fprintf(cg->out, "    movss DWORD PTR [rbp-%d], xmm0\n", slot);
        } else {
            emit_expr(cg, arg);
            fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", slot);
        }
    }
    int gpr = 0, xmm = 0;
    param_index = 0;
    for (size_t i = 0; i < n->child_count; i++) {
        ASTNode *param = function_param_node(cg, n->name, param_index++);
        int slot = arg_slot + (int)i * 8;
        bool is_float = param && (param->declared_type == COBRA_TYPE_F32 || param->declared_type == COBRA_TYPE_F64) &&
                         !(param->dyn_trait_name[0]);
        if (is_float) fprintf(cg->out, "    movss %s, DWORD PTR [rbp-%d]\n", SYSV_XMM_REGS[xmm++], slot);
        else fprintf(cg->out, "    mov %s, QWORD PTR [rbp-%d]\n", param_reg(cg, gpr++), slot);
    }
    fprintf(cg->out, "    xor eax, eax\n    call %s@PLT\n", n->name);
}

/* obj.method(args) where obj's static type is `dyn TraitName` (a local or
   parameter whose VarSymbol carries dyn_trait_name - see the COBRA_TYPE_FUNC
   parameter-binding path in emit_function, which is unchanged from an
   ordinary function-value parameter since a dyn-trait value is ABI-identical
   to one: a single pointer). Genuinely dynamic: the method slot is loaded
   from the receiver's own dispatch block at runtime, not resolved to a fixed
   mangled symbol at compile time the way static-dispatch x.method() is. */
static void emit_dyn_dispatch_call(CodeGen *cg, ASTNode *n, VarSymbol *recv) {
    ASTNode *trait_decl = find_trait_decl(cg, recv->dyn_trait_name);
    if (!trait_decl) {
        fprintf(stderr, "CodeGen Error: unknown trait '%s'\n", recv->dyn_trait_name);
        exit(EXIT_FAILURE);
    }
    int method_index = -1;
    for (size_t m = 0; m < trait_decl->child_count; m++) {
        if (!strcmp(trait_decl->children[m]->name, n->name)) { method_index = (int)m; break; }
    }
    if (method_index < 0) {
        fprintf(stderr, "CodeGen Error: trait '%s' has no method '%s'\n", recv->dyn_trait_name, n->name);
        exit(EXIT_FAILURE);
    }
    int block_slot = reserve(cg, 8);
    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", recv->offset, block_slot);
    int arg_slot = reserve(cg, (int)n->child_count * 8 + 8);
    for (size_t i = 0; i < n->child_count; i++) {
        ASTNode *arg = n->children[i];
        int slot = arg_slot + (int)i * 8;
        emit_expr(cg, arg);
        fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", slot);
    }
    fprintf(cg->out, "    mov r11, QWORD PTR [rbp-%d]\n    mov rdi, QWORD PTR [r11]\n", block_slot);
    for (size_t i = 0; i < n->child_count; i++) {
        int slot = arg_slot + (int)i * 8;
        fprintf(cg->out, "    mov %s, QWORD PTR [rbp-%d]\n", param_reg(cg, (int)(i + 1)), slot);
    }
    fprintf(cg->out, "    mov r11, QWORD PTR [rbp-%d]\n    mov r10, QWORD PTR [r11+8]\n    mov r10, QWORD PTR [r10+%d]\n    xor eax, eax\n    call r10\n",
            block_slot, (int)(8 * method_index));
}

static void emit_call(CodeGen *cg, ASTNode *n) {
    if (n->qualifier[0]) {
        VarSymbol *recv = find_symbol(cg, n->qualifier);
        if (recv && recv->dyn_trait_name[0]) { emit_dyn_dispatch_call(cg, n, recv); return; }
    }
    {
        ASTNode *dyn_fn = find_function(cg, n->name);
        if (dyn_fn) {
            for (size_t i = 0; i < dyn_fn->child_count; i++) {
                ASTNode *p = dyn_fn->children[i];
                if (p->type == AST_PARAM && p->dyn_trait_name[0]) { emit_dyn_trait_call(cg, n, dyn_fn); return; }
            }
        }
    }
    if (n->is_indirect_call) {
        /* f(a, b, ...) where f is a local fn(...)->... value (ir.c already
           checked argument count/types against the stored signature). Each
           argument is evaluated into a stack temp first - same reasoning as
           call_i64_i64 below - because evaluating a later argument, or the
           callee address itself, may clobber a register still needed for an
           earlier one; only once every value is safely on the stack are they
           loaded into the SysV argument registers in order. */
        /* fn(...)->... values are always a pointer to a {code_ptr, env_ptr}
           thunk (see ensure_fn_thunk for plain functions, and the
           is_closure_instance codegen for closures), so env_ptr is always
           passed as an implicit leading argument in rdi; the real arguments
           start at rsi. Float arguments are unaffected since env only
           consumes a GPR. */
        VarSymbol *fs = find_symbol(cg, n->name);
        if (!fs) { fprintf(stderr, "CodeGen Error: undefined function value '%s'\n", n->name); exit(EXIT_FAILURE); }
        int thunk_slot = reserve(cg, 8);
        fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", fs->offset, thunk_slot);
        int arg_slot = reserve(cg, (int)n->child_count * 8);
        bool arg_is_float[16];
        for (size_t i = 0; i < n->child_count && i < 16; i++) {
            ASTNode *arg = n->children[i];
            arg_is_float[i] = expression_is_float_codegen(cg, arg);
            emit_expr(cg, arg);
            int slot = arg_slot - (int)i * 8;
            if (arg_is_float[i]) fprintf(cg->out, "    movss DWORD PTR [rbp-%d], xmm0\n", slot);
            else fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", slot);
        }
        int gpr = 1, xmm = 0;
        for (size_t i = 0; i < n->child_count && i < 16; i++) {
            int slot = arg_slot - (int)i * 8;
            if (arg_is_float[i]) fprintf(cg->out, "    movss %s, DWORD PTR [rbp-%d]\n", SYSV_XMM_REGS[xmm++], slot);
            else fprintf(cg->out, "    mov %s, QWORD PTR [rbp-%d]\n", param_reg(cg, gpr++), slot);
        }
        fprintf(cg->out,
                "    mov r11, QWORD PTR [rbp-%d]\n"
                "    mov rdi, QWORD PTR [r11+8]\n"
                "    mov r10, QWORD PTR [r11]\n"
                "    xor eax, eax\n    call r10\n",
                thunk_slot);
        return;
    }
    if (is_region_alloc(cg, n)) { emit_region_alloc(cg, n); return; }
    {
        ASTNode *gpu_fn = find_function(cg, n->name);
        if (gpu_fn && gpu_fn->target_device == TARGET_DEV_GPU_KERNEL) { emit_gpu_kernel_call(cg, n, gpu_fn); return; }
    }
    {
        size_t name_len = strlen(n->name);
        if (name_len > 4 && !strcmp(n->name + name_len - 4, "_gpu")) {
            char base_name[COBRA_MAX_IDENT_LEN];
            snprintf(base_name, sizeof(base_name), "%.*s", (int)(name_len - 4), n->name);
            ASTNode *kernel = find_function(cg, base_name);
            if (kernel && kernel->target_device == TARGET_DEV_GPU_KERNEL) { emit_gpu_resident_call(cg, n, kernel); return; }
        }
    }
    {
        size_t name_len = strlen(n->name);
        if (name_len > 9 && !strcmp(n->name + name_len - 9, "_backward")) {
            char base_name[COBRA_MAX_IDENT_LEN];
            snprintf(base_name, sizeof(base_name), "%.*s", (int)(name_len - 9), n->name);
            ASTNode *kernel = find_function(cg, base_name);
            if (kernel && kernel->target_device == TARGET_DEV_GPU_KERNEL) { emit_gpu_backward_call(cg, n, kernel, n->name); return; }
        }
    }
    if (!strcmp(n->name, "gpu_alloc_f32")) {
        emit_expr(cg, n->children[0]);
        fprintf(cg->out, "    mov rdi, rax\n    call cobra_gpu_alloc_f32@PLT\n");
        return;
    }
    if (!strcmp(n->name, "gpu_upload_f32") || !strcmp(n->name, "gpu_download_f32")) {
        bool upload = !strcmp(n->name, "gpu_upload_f32");
        int handle_slot = reserve(cg, 8);
        emit_expr(cg, n->children[0]);
        fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", handle_slot);
        const char *buf_name = n->children[1]->name;
        if (upload) {
            fprintf(cg->out, "    mov rdi, QWORD PTR [rbp-%d]\n", handle_slot);
            emit_load_buffer_ptr(cg, buf_name, "rsi");
            emit_load_buffer_len(cg, buf_name, "rdx");
            fprintf(cg->out, "    call cobra_gpu_upload_f32@PLT\n");
        } else {
            fprintf(cg->out, "    mov rdi, QWORD PTR [rbp-%d]\n", handle_slot);
            emit_load_buffer_ptr(cg, buf_name, "rsi");
            emit_load_buffer_len(cg, buf_name, "rdx");
            fprintf(cg->out, "    call cobra_gpu_download_f32@PLT\n");
        }
        return;
    }
    if (!strcmp(n->name, "gpu_free_resident")) {
        emit_expr(cg, n->children[0]);
        fprintf(cg->out, "    mov rdi, rax\n    call cobra_gpu_free_resident@PLT\n");
        return;
    }
    if (!strcmp(n->name, "gpu_batch_begin") || !strcmp(n->name, "gpu_batch_end")) {
        fprintf(cg->out, "    call cobra_%s@PLT\n", n->name);
        return;
    }
    if (!strcmp(n->name, "call_i64_i64") || !strcmp(n->name, "call_f32_f32")) {
        /* Indirect call through a function-reference value (see ir.c's
           AST_VAR_REF fallback: a bare function name evaluates to its
           address). The target address is stashed on the stack (not kept
           in a register) across evaluating the arg expression, which may
           itself contain calls that clobber any register; it's reloaded
           into r10 (caller-saved, not a SysV argument register) right
           before the indirect `call r10`. */
        bool is_f32 = !strcmp(n->name, "call_f32_f32");
        int ptr_slot = reserve(cg, 8);
        emit_expr(cg, n->children[0]);
        fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", ptr_slot);
        emit_expr(cg, n->children[1]);
        if (!is_f32) fprintf(cg->out, "    mov rdi, rax\n");
        fprintf(cg->out, "    mov r10, QWORD PTR [rbp-%d]\n    call r10\n", ptr_slot);
        return;
    }
    if (!strcmp(n->name, "pack_f16") || !strcmp(n->name, "unpack_f16")) {
        /* pack_f16(f32_src, u8_dst, count) / unpack_f16(u8_src, f32_dst, count)
           -> cobra_pack_f16/cobra_unpack_f16(src_ptr, dst_ptr, count). Both
           buffer arguments must be plain variables (same restriction as the
           other buffer-consuming builtins in this file). */
        if (n->child_count != 3 || n->children[0]->type != AST_VAR_REF || n->children[1]->type != AST_VAR_REF) {
            fprintf(stderr, "CodeGen Error: %s requires (buffer, buffer, count) with plain-variable buffers\n", n->name);
            exit(EXIT_FAILURE);
        }
        int count_slot = reserve(cg, 8);
        emit_expr(cg, n->children[2]);
        fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", count_slot);
        emit_load_buffer_ptr(cg, n->children[0]->name, "rdi");
        emit_load_buffer_ptr(cg, n->children[1]->name, "rsi");
        fprintf(cg->out, "    mov rdx, QWORD PTR [rbp-%d]\n    call cobra_%s@PLT\n",
                count_slot, n->name);
        return;
    }
    if (!strcmp(n->name, "gpu_available") || !strcmp(n->name, "gpu_device_count") ||
        !strcmp(n->name, "gpu_selftest")) {
        if (!g_gpu_enabled) { fprintf(cg->out, "    xor eax, eax\n"); return; }
        fprintf(cg->out, "    call cobra_%s@PLT\n", n->name);
        return;
    }
    if (!strcmp(n->name, "gpu_should_dispatch")) {
        if (!g_gpu_enabled || n->child_count != 1) { fprintf(cg->out, "    xor eax, eax\n"); return; }
        emit_expr(cg, n->children[0]);
        fprintf(cg->out, "    mov rdi, rax\n    call cobra_gpu_should_dispatch@PLT\n");
        return;
    }
    if (!strcmp(n->name, "concat")) { emit_string_concat(cg, n); return; }
    if (!strcmp(n->name, "some") || !strcmp(n->name, "none") ||
        !strcmp(n->name, "ok") || !strcmp(n->name, "err")) {
        bool result = !strcmp(n->name, "ok") || !strcmp(n->name, "err");
        CobraTypeKind sum_type = result ? COBRA_TYPE_RESULT : COBRA_TYPE_OPTION;
        int payload_size = sum_component_size(cg, ast_element_kind(n), ast_payload_name(n));
        int error_size = result ? sum_component_size(cg, ast_error_kind(n), ast_error_name(n)) : 0;
        int total_size = COBRA_NATIVE_SUM_TAG_SIZE + payload_size + error_size;
        int temp = reserve(cg, total_size);
        int sum_ptr = temp - total_size + COBRA_NATIVE_SUM_TAG_SIZE;
        fprintf(cg->out, "    lea rdx, [rbp-%d]\n", sum_ptr);
        emit_sum_constructor(cg, n, "rdx", sum_type,
                             ast_element_kind(n), ast_error_kind(n),
                             ast_payload_name(n), ast_error_name(n));
        fprintf(cg->out, "    lea rax, [rbp-%d]\n", sum_ptr);
        return;
    }
    if (!strcmp(n->name, "is_some") || !strcmp(n->name, "unwrap") ||
        !strcmp(n->name, "is_ok") || !strcmp(n->name, "unwrap_ok") ||
        !strcmp(n->name, "unwrap_err")) {
        emit_sum_accessor(cg, n);
        return;
    }
    if (!strcmp(n->name, "range") || !strcmp(n->name, "enumerate")) {
        /* range and enumerate are loop-source markers. The loop emitter reads
           their AST directly, so no runtime iterator object is created. */
        return;
    }
    if (!strcmp(n->name, "sum") || !strcmp(n->name, "min") ||
        !strcmp(n->name, "max") || !strcmp(n->name, "any") ||
        !strcmp(n->name, "all")) {
        if (n->child_count != 1 || n->children[0]->type != AST_VAR_REF) {
            fprintf(stderr, "CodeGen Error: aggregate builtin requires one named collection\n");
            exit(EXIT_FAILURE);
        }
        const char *source = n->children[0]->name;
        int index = reserve(cg, 8), bound = reserve(cg, 8), result = reserve(cg, 8);
        int label = cg->label_count++, done = cg->label_count++;
        VarSymbol *s = find_symbol(cg, source);
        if (!s) { fprintf(stderr, "CodeGen Error: unknown aggregate collection '%s'\n", source); exit(EXIT_FAILURE); }
        emit_load_buffer_len(cg, source, "rcx");
        fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rcx\n    mov QWORD PTR [rbp-%d], 0\n", bound, index);
        if (!strcmp(n->name, "min")) {
            fprintf(cg->out, "    mov rax, 0x7fffffffffffffff\n    mov QWORD PTR [rbp-%d], rax\n", result);
        } else if (!strcmp(n->name, "max")) {
            fprintf(cg->out, "    mov rax, 0x8000000000000000\n    mov QWORD PTR [rbp-%d], rax\n", result);
        }
        else if (!strcmp(n->name, "all")) fprintf(cg->out, "    mov QWORD PTR [rbp-%d], 1\n", result);
        else fprintf(cg->out, "    mov QWORD PTR [rbp-%d], 0\n", result);
        fprintf(cg->out, ".Lagg_%d:\n    mov rax, QWORD PTR [rbp-%d]\n    cmp rax, QWORD PTR [rbp-%d]\n    jae .Lagg_exit_%d\n", label, index, bound, done);
        emit_load_buffer_ptr(cg, source, "rbx");
        fprintf(cg->out, "    mov rdx, QWORD PTR [rbp-%d]\n", index);
        if ((s->kind == SYM_LIST && s->element_type == COBRA_TYPE_F32) ||
            (s->kind == SYM_ARRAY && s->element_type == COBRA_TYPE_F32) ||
            (s->kind == SYM_SLICE && s->type == COBRA_TYPE_SLICE_F32)) {
            fprintf(stderr, "CodeGen Error: generic integer aggregate cannot consume f32 collection '%s'\n", source);
            exit(EXIT_FAILURE);
        }
        fprintf(cg->out, "    mov rax, QWORD PTR [rbx + rdx*8]\n");
        if (!strcmp(n->name, "sum")) fprintf(cg->out, "    add QWORD PTR [rbp-%d], rax\n", result);
        else if (!strcmp(n->name, "min")) fprintf(cg->out, "    mov rdx, QWORD PTR [rbp-%d]\n    cmp rdx, rax\n    cmovg rdx, rax\n    mov QWORD PTR [rbp-%d], rdx\n", result, result);
        else if (!strcmp(n->name, "max")) fprintf(cg->out, "    mov rdx, QWORD PTR [rbp-%d]\n    cmp rdx, rax\n    cmovl rdx, rax\n    mov QWORD PTR [rbp-%d], rdx\n", result, result);
        else if (!strcmp(n->name, "any")) fprintf(cg->out, "    cmp rax, 0\n    jne .Lagg_true_%d\n", done);
        else if (!strcmp(n->name, "all")) fprintf(cg->out, "    cmp rax, 0\n    je .Lagg_false_%d\n", done);
        fprintf(cg->out, "    inc QWORD PTR [rbp-%d]\n    jmp .Lagg_%d\n", index, label);
        if (!strcmp(n->name, "any")) fprintf(cg->out, ".Lagg_true_%d:\n    mov QWORD PTR [rbp-%d], 1\n    jmp .Lagg_done_%d\n", done, result, done);
        if (!strcmp(n->name, "all")) fprintf(cg->out, ".Lagg_false_%d:\n    mov QWORD PTR [rbp-%d], 0\n    jmp .Lagg_done_%d\n", done, result, done);
        fprintf(cg->out, ".Lagg_exit_%d:\n", done);
        if (!strcmp(n->name, "min") || !strcmp(n->name, "max")) {
            fprintf(cg->out, "    cmp QWORD PTR [rbp-%d], 0\n    jne .Lagg_done_%d\n", bound, done);
            emit_failure(cg, "min/max requires a non-empty collection");
        } else {
            fprintf(cg->out, "    jmp .Lagg_done_%d\n", done);
        }
        fprintf(cg->out, ".Lagg_done_%d:\n    mov rax, QWORD PTR [rbp-%d]\n", done, result);
        return;
    }
    if (!strcmp(n->name, "append")) {
        if (n->child_count != 2 || n->children[0]->type != AST_VAR_REF) {
            fprintf(stderr, "CodeGen Error: append requires (list, value)\n"); exit(EXIT_FAILURE);
        }
        VarSymbol *s = find_symbol(cg, n->children[0]->name);
        if (!s || s->kind != SYM_LIST) { fprintf(stderr, "CodeGen Error: append target is not a list\\n"); exit(EXIT_FAILURE); }
        emit_list_append(cg, s, n->children[1]);
        return;
    }
    if (!strcmp(n->name, "set")) {
        if (n->child_count != 3 || n->children[0]->type != AST_VAR_REF) {
            fprintf(stderr, "CodeGen Error: set requires (dict, string, i64)\n"); exit(EXIT_FAILURE);
        }
        VarSymbol *s = find_symbol(cg, n->children[0]->name);
        if (!s || s->kind != SYM_DICT) { fprintf(stderr, "CodeGen Error: set target is not a dict\\n"); exit(EXIT_FAILURE); }
        emit_dict_set(cg, s, n->children[1], n->children[2]);
        return;
    }
    if (!strcmp(n->name, "get") || !strcmp(n->name, "has") ||
        !strcmp(n->name, "delete") || !strcmp(n->name, "pop")) {
        bool needs_default = !strcmp(n->name, "get") || !strcmp(n->name, "pop");
        size_t required = needs_default ? 3 : 2;
        if (n->child_count != required || n->children[0]->type != AST_VAR_REF) {
            fprintf(stderr, "CodeGen Error: invalid dict operation\n"); exit(EXIT_FAILURE);
        }
        VarSymbol *s = find_symbol(cg, n->children[0]->name);
        if (!s || s->kind != SYM_DICT) { fprintf(stderr, "CodeGen Error: lookup target is not a dict\n"); exit(EXIT_FAILURE); }
        /* ABI split: set/delete/pop take the owner pointer (void **) because they
           may rehash; get/has take the dict pointer itself (void *). */
        if (!strcmp(n->name, "get") || !strcmp(n->name, "has"))
            fprintf(cg->out, "    mov rdi, QWORD PTR [rbp-%d]\n", s->offset);
        else
            fprintf(cg->out, "    lea rdi, [rbp-%d]\n", s->offset);
        emit_expr(cg, n->children[1]);
        fprintf(cg->out, "    mov rsi, rax\n");
        if (!strcmp(n->name, "get")) { emit_expr(cg, n->children[2]); fprintf(cg->out, "    mov rdx, rax\n    call cobra_dict_get_i64@PLT\n"); }
        else if (!strcmp(n->name, "has")) fprintf(cg->out, "    call cobra_dict_has@PLT\n");
        else if (!strcmp(n->name, "delete")) fprintf(cg->out, "    call cobra_dict_delete@PLT\n");
        else { emit_expr(cg, n->children[2]); fprintf(cg->out, "    mov rdx, rax\n    call cobra_dict_pop@PLT\n"); }
        return;
    }
    if (!strcmp(n->name, "starts_with") || !strcmp(n->name, "ends_with") || !strcmp(n->name, "contains")) { emit_string_predicate(cg, n); return; }
    if (!strcmp(n->name, "char_at")) { emit_string_char_at(cg, n); return; }
    if (!strcmp(n->name, "string_free")) {
        if (n->child_count && n->children[0]->type == AST_VAR_REF) {
            VarSymbol *s = find_symbol(cg, n->children[0]->name);
            emit_expr(cg, n->children[0]);
            fprintf(cg->out, "    mov rdi, rax\n    call free@PLT\n");
            if (s) {
                s->owned = false;
                fprintf(cg->out, "    mov QWORD PTR [rbp-%d], 0\n", s->offset);
            }
        }
        return;
    }
    if (!strcmp(n->name, "alloc_i64")) { emit_alloc(cg, n, 8); return; }
    if (!strcmp(n->name, "alloc_f32")) { emit_alloc(cg, n, 4); return; }
    if (!strcmp(n->name, "alloc_u8")) { emit_alloc(cg, n, 1); return; }
    if (!strcmp(n->name, "fill_f32")) { emit_fill(cg, n); return; }
    if (!strcmp(n->name, "relu_f32")) { emit_relu(cg, n); return; }
    if (!strcmp(n->name, "sum_f32")) { emit_reduce(cg, n, "sum"); return; }
    if (!strcmp(n->name, "mean_f32")) { emit_reduce(cg, n, "mean"); return; }
    if (!strcmp(n->name, "max_f32")) { emit_reduce(cg, n, "max"); return; }
    if (!strcmp(n->name, "dense_f32")) { emit_dense(cg, n); return; }
    if (!strcmp(n->name, "matmul_f32")) { emit_matmul(cg, n); return; }
    if (!strcmp(n->name, "matmul_f32_backward")) { emit_matmul_backward(cg, n); return; }
    if (!strcmp(n->name, "exp_f32") || !strcmp(n->name, "sqrt_f32") || !strcmp(n->name, "tanh_f32") || !strcmp(n->name, "log_f32") || !strcmp(n->name, "pow_f32")) { emit_math(cg, n); return; }
    if (!strcmp(n->name, "free")) {
        if (n->child_count && n->children[0]->type == AST_VAR_REF) {
            VarSymbol *s = find_symbol(cg, n->children[0]->name);            if (s && s->kind == SYM_LIST) {
                fprintf(cg->out, "    lea rdi, [rbp-%d]\n    lea rsi, [rbp-%d]\n    lea rdx, [rbp-%d]\n    call cobra_list_free@PLT\n", s->offset, s->length_offset, s->capacity_offset);
                s->owned = false;
                return;
            }

            if (s && s->kind == SYM_DICT) {
                fprintf(cg->out, "    lea rdi, [rbp-%d]\n    lea rsi, [rbp-%d]\n    call cobra_dict_free@PLT\n", s->offset, s->length_offset);
                s->owned = false;
                return;
            }                /* Views borrow storage; freeing a view only ends the source
                   level borrow and must never release the owner's allocation. */
                if (s && ((s->kind == SYM_TENSOR && !s->owned) || s->borrowed)) return;

            emit_load_buffer_ptr(cg, n->children[0]->name, "rdi");
            fprintf(cg->out, "    call free@PLT\n");
        }
        return;
    }
    if (!strcmp(n->name, "reshape_view") || !strcmp(n->name, "slice_view") || !strcmp(n->name, "transpose_view")) return;

    ASTNode *fn = find_function(cg, n->name);
    if (!fn) {
        if (is_imported_function(cg, n->name)) {
            emit_import_call(cg, n);
            return;
        }
        const char *file = n->source_file[0] ? n->source_file : "<source>";
        int line = n->source_line > 0 ? n->source_line : 1;
        int col = n->source_col > 0 ? n->source_col : 1;
        fprintf(stderr, "%s:%d:%d: error: undefined function '%s' (not defined or imported with 'import c')\n", file, line, col, n->name);
        exit(EXIT_FAILURE);
    }
    bool tensor_return = fn->declared_type == COBRA_TYPE_TENSOR_F32;
    bool sum_return = fn->declared_type == COBRA_TYPE_OPTION || fn->declared_type == COBRA_TYPE_RESULT;
    bool struct_return = fn->declared_type == COBRA_TYPE_STRUCT;
    bool compound_return = tensor_return || sum_return || struct_return;
    int stack_slots = function_stack_slot_count(cg, n->name, compound_return);
    /* The first outgoing stack argument is written at [rsp+0] before call;
       the call's pushed return address becomes [rbp+8], so the callee reads
       that argument at [rbp+16]. Keep the area 16-byte aligned. */
    int stack_bytes = stack_slots ? ((stack_slots * 8 + 15) & ~15) : 0;
    if (stack_bytes) fprintf(cg->out, "    sub rsp, %d\n", stack_bytes);

    int result_temp = 0;
    if (compound_return) {
        /* Allocate the caller-owned result region first. Struct and tensor
           returns use its low-address base; sums use the historical high
           address tag pointer. */
        if (cg->stack_offset < COBRA_LOCAL_BASE) cg->stack_offset = COBRA_LOCAL_BASE;
        int result_size = tensor_return ? COBRA_TENSOR_FIELDS * 8 :
                           struct_return ? struct_storage_size(cg, ast_payload_name(fn)) :
                           8 + sum_component_size(cg, ast_element_kind(fn), ast_payload_name(fn)) +
                           (fn->declared_type == COBRA_TYPE_RESULT ?
                            sum_component_size(cg, ast_error_kind(fn), ast_error_name(fn)) : 0);
        if (tensor_return || struct_return) {
            result_temp = cg->stack_offset;
            cg->stack_offset += result_size;
        } else {
            result_temp = cg->stack_offset + 8;
            cg->stack_offset += result_size;
        }
    }
    int temp = reserve(cg, (int)n->child_count * 24 + 16);
    for (int i = (int)n->child_count - 1; i >= 0; i--) {
        CobraTypeKind t = function_param_type(cg, n->name, (size_t)i);
        ASTNode *arg = n->children[i];
        int slot = temp + i * 24;
        if (t == COBRA_TYPE_SLICE || t == COBRA_TYPE_SLICE_F32 || t == COBRA_TYPE_SLICE_U8) {
            if (arg->type != AST_VAR_REF) { fprintf(stderr, "CodeGen Error: slice arguments must be named buffers\n"); exit(EXIT_FAILURE); }
            emit_load_buffer_ptr(cg, arg->name, "rax");
            fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", slot);
            emit_load_buffer_len(cg, arg->name, "rax");
            fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", slot + 8);
        } else if (t == COBRA_TYPE_LIST) {
            if (arg->type != AST_VAR_REF) { fprintf(stderr, "CodeGen Error: list arguments must be named lists\n"); exit(EXIT_FAILURE); }
            VarSymbol *list = find_symbol(cg, arg->name);
            if (!list || list->kind != SYM_LIST) { fprintf(stderr, "CodeGen Error: list argument is not a list\n"); exit(EXIT_FAILURE); }
            fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", list->offset, slot, list->length_offset, slot + 8, list->capacity_offset, slot + 16);
        } else if (t == COBRA_TYPE_DICT) {
            if (arg->type != AST_VAR_REF) { fprintf(stderr, "CodeGen Error: dict arguments must be named dicts\n"); exit(EXIT_FAILURE); }
            VarSymbol *dict = find_symbol(cg, arg->name);
            if (!dict || dict->kind != SYM_DICT) { fprintf(stderr, "CodeGen Error: dict argument is not a dict\n"); exit(EXIT_FAILURE); }
            fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", dict->offset, slot, dict->length_offset, slot + 8);
        } else if (t == COBRA_TYPE_OPTION || t == COBRA_TYPE_RESULT) {
            if (arg->type != AST_VAR_REF) {
                fprintf(stderr, "CodeGen Error: Option and Result arguments must be named values\n");
                exit(EXIT_FAILURE);
            }
            VarSymbol *sum = find_symbol(cg, arg->name);
            if (!sum || sum->type != t) {
                fprintf(stderr, "CodeGen Error: sum argument does not match the parameter type\n");
                exit(EXIT_FAILURE);
            }
            /* Sums are passed as pointers to caller-owned frame storage. The
               callee copies the value into private storage before reading it. */
            fprintf(cg->out, "    lea rax, [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n",
                    sum->tag_offset, slot);
        } else if (t == COBRA_TYPE_TENSOR_F32) {
            if (arg->type != AST_VAR_REF) { fprintf(stderr, "CodeGen Error: tensor arguments must be named descriptors\n"); exit(EXIT_FAILURE); }
            emit_load_tensor_descriptor_ptr(cg, arg->name, "rax");
            fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", slot);
        } else if (t == COBRA_TYPE_F32 || t == COBRA_TYPE_F64) {
            emit_expr(cg, arg);
            if (!expression_is_float(arg)) fprintf(cg->out, "    cvtsi2ss xmm0, rax\n");
            fprintf(cg->out, "    movss DWORD PTR [rbp-%d], xmm0\n", slot);
        } else {
            emit_expr(cg, arg);
            fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", slot);
        }
    }

    int gpr = compound_return ? 1 : 0, xmm = 0, stack_index = 0;
    if (compound_return) fprintf(cg->out, "    lea rdi, [rbp-%d]\n", result_temp);
    for (size_t i = 0; i < n->child_count; i++) {
        CobraTypeKind t = function_param_type(cg, n->name, i);
        int slot = temp + (int)i * 24;
        if (t == COBRA_TYPE_F32 || t == COBRA_TYPE_F64) {
            if (xmm < 8) fprintf(cg->out, "    movss %s, DWORD PTR [rbp-%d]\n", SYSV_XMM_REGS[xmm++], slot);
            else { fprintf(cg->out, "    movss xmm15, DWORD PTR [rbp-%d]\n    movss DWORD PTR [rsp+%d], xmm15\n", slot, stack_index * 8); stack_index++; }
        } else {
            ASTNode *param_node = function_param_node(cg, n->name, i);
            int slots = abi_slots_for(t, param_node);
            bool fits = gpr + slots <= 6;
            if (fits) {
                if (t == COBRA_TYPE_TENSOR_F32) {
                    fprintf(cg->out, "    mov %s, QWORD PTR [rbp-%d]\n", param_reg(cg, gpr), slot);
                } else {
                    for (int k = 0; k < slots; k++) fprintf(cg->out, "    mov %s, QWORD PTR [rbp-%d]\n", param_reg(cg, gpr + k), slot + k * 8);
                }
                gpr += slots;
            } else {
                for (int k = 0; k < slots; k++) fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rsp+%d], rax\n", slot + k * 8, stack_index++ * 8);
            }
        }
    }
    fprintf(cg->out, "    xor eax, eax\n    call %s@PLT\n", n->name);
    if (stack_bytes) fprintf(cg->out, "    add rsp, %d\n", stack_bytes);
    if (compound_return) fprintf(cg->out, "    lea rax, [rbp-%d]\n", result_temp);
    if (n->propagate_error) {
        bool typed_sum_propagation = sum_return;
        if (typed_sum_propagation) {
            /* A sum return is an sret pointer, not a scalar status. A successful
               `?` exposes its payload; the failing value is copied into this
               function's own sret buffer before control reaches the shared
               propagation epilogue. */
            int typed_ok = cg->label_count++;
            fprintf(cg->out, "    cmp QWORD PTR [rax], 1\n    je .Ltyped_prop_ok_%d\n", typed_ok);
            fprintf(cg->out, "    mov rsi, rax\n    mov rdi, QWORD PTR [rbp-240]\n");
            emit_sum_copy_ptr(cg, "rsi", "rdi", fn->declared_type,
                              sum_component_size(cg, ast_element_kind(fn), ast_payload_name(fn)),
                              fn->declared_type == COBRA_TYPE_RESULT ?
                              sum_component_size(cg, ast_error_kind(fn), ast_error_name(fn)) : 0);
            fprintf(cg->out, "    mov rax, QWORD PTR [rbp-240]\n");
            if (cg->region_depth == 0) {
                fprintf(cg->out, "    jmp .Lpropagate_%d\n", cg->propagation_label);
            } else {
                /* The copied sum lives in the caller's return buffer, so only
                   the pointer must survive region destruction. */
                int result_slot = reserve(cg, 8);
                fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", result_slot);
                for (int i = cg->region_depth - 1; i >= 0; i--) {
                    if (!cg->regions[i].active) continue;
                    fprintf(cg->out,
                            "    lea rdi, [rbp-%d]\n    call arena_destroy@PLT\n",
                            cg->regions[i].state_base);
                }
                fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    jmp .Lpropagate_%d\n", result_slot, cg->propagation_label);
            }
            fprintf(cg->out, ".Ltyped_prop_ok_%d:\n", typed_ok);
            int callee_payload_size = sum_component_size(cg, ast_element_kind(fn), ast_payload_name(fn));
            if (ast_element_kind(fn) == COBRA_TYPE_STRUCT) {
                fprintf(cg->out, "    lea rax, [rax-%d]\n", callee_payload_size);
            } else if (ast_element_kind(fn) == COBRA_TYPE_F32) {
                fprintf(cg->out, "    movss xmm0, DWORD PTR [rax-%d]\n", callee_payload_size);
            } else {
                fprintf(cg->out, "    mov rax, QWORD PTR [rax-%d]\n", callee_payload_size);
            }
        } else if (cg->region_depth == 0) {
            fprintf(cg->out, "    test rax, rax\n    jne .Lpropagate_%d\n", cg->propagation_label);
        } else {
            /* A failing integer `?` call must release every region that is live
               at this call site before jumping to the shared propagate label. */
            int status_slot = reserve(cg, 8);
            int skip = cg->label_count++;
            fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n    test rax, rax\n    je .Lprop_ok_%d\n", status_slot, skip);
            for (int i = cg->region_depth - 1; i >= 0; i--) {
                if (!cg->regions[i].active) continue;
                fprintf(cg->out,
                        "    lea rdi, [rbp-%d]\n    call arena_destroy@PLT\n",
                        cg->regions[i].state_base);
            }
            fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    jmp .Lpropagate_%d\n.Lprop_ok_%d:\n", status_slot, cg->propagation_label, skip);
        }
    }
}

static void emit_tensor_metadata(CodeGen *cg, VarSymbol *s, ASTNode *decl) {
    int rank = decl->shape_rank > 0 ? decl->shape_rank : 1;
    fprintf(cg->out, "    mov QWORD PTR [rbp-%d], %d\n", s->rank_offset, rank);
    for (int i = 0; i < COBRA_VIEW_MAX_RANK; i++) {
        long long d = 1;
        if (i < decl->shape_rank) {
            char *end = NULL; d = strtoll(decl->shape_dims[i], &end, 10);
            if (!end || *end) {
                VarSymbol *dim = find_symbol(cg, decl->shape_dims[i]);
                if (dim) fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", dim->offset, s->dim_offsets[i]);
                else d = 0;
                d = -1;
            }
        }
        if (d >= 0) fprintf(cg->out, "    mov QWORD PTR [rbp-%d], %lld\n", s->dim_offsets[i], d);
    }
    for (int i = 0; i < COBRA_VIEW_MAX_RANK; i++) {
        long long stride = 1;
        for (int j = i + 1; j < decl->shape_rank; j++) {
            char *end = NULL; long long d = strtoll(decl->shape_dims[j], &end, 10);
            if (!end || *end) { stride = 0; break; }
            stride *= d;
        }
        fprintf(cg->out, "    mov QWORD PTR [rbp-%d], %lld\n", s->stride_offsets[i], stride);
    }
}

static void emit_view_init(CodeGen *cg, VarSymbol *dst, ASTNode *call) {
    const char *src_name = call->children[0]->name; VarSymbol *src = find_symbol(cg, src_name);
    if (!src || src->kind != SYM_TENSOR) { fprintf(stderr, "CodeGen Error: view source must be a tensor\n"); return; }
    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", src->offset, dst->offset);
    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", src->length_offset, dst->length_offset);
    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", src->rank_offset, dst->rank_offset);
    for (int i = 0; i < COBRA_VIEW_MAX_RANK; i++) {
        fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", src->dim_offsets[i], dst->dim_offsets[i]);
        fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", src->stride_offsets[i], dst->stride_offsets[i]);
    }
    if (!strcmp(call->name, "transpose_view")) {
        dst->contiguous = false;
        fprintf(cg->out, "    mov QWORD PTR [rbp-%d], 2\n", dst->rank_offset);
        for (int i = 0; i < 2; i++) { fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", src->dim_offsets[1-i], dst->dim_offsets[i]); fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", src->stride_offsets[1-i], dst->stride_offsets[i]); }
    } else if (!strcmp(call->name, "reshape_view")) {
        dst->contiguous = src->contiguous;
        int rows = reserve(cg, 8), cols = reserve(cg, 8), fail = cg->label_count++;
        emit_expr(cg, call->children[1]); fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", rows); emit_expr(cg, call->children[2]); fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", cols);
        fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    imul rax, QWORD PTR [rbp-%d]\n    cmp rax, QWORD PTR [rbp-%d]\n    jne .Lreshape_fail_%d\n    jmp .Lreshape_done_%d\n.Lreshape_fail_%d:\n", rows, cols, src->length_offset, fail, fail + 1, fail); emit_failure(cg, "reshape dimensions do not preserve element count"); fprintf(cg->out, ".Lreshape_done_%d:\n", fail + 1);
        fprintf(cg->out, "    mov QWORD PTR [rbp-%d], 2\n    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", dst->rank_offset, rows, dst->dim_offsets[0], cols, dst->dim_offsets[1]);
        fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n    mov QWORD PTR [rbp-%d], 1\n", cols, dst->stride_offsets[0], dst->stride_offsets[1]);
    } else {
        if (!src->contiguous) {
            fprintf(stderr, "CodeGen Error: slice_view requires a contiguous source tensor\n");
            exit(EXIT_FAILURE);
        }
        dst->contiguous = true;
        int start = reserve(cg, 8), length = reserve(cg, 8), fail = cg->label_count++;
        emit_expr(cg, call->children[1]); fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", start); emit_expr(cg, call->children[2]); fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", length);
        fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    add rax, QWORD PTR [rbp-%d]\n    cmp rax, QWORD PTR [rbp-%d]\n    ja .Lslice_fail_%d\n    jmp .Lslice_done_%d\n.Lslice_fail_%d:\n", start, length, src->length_offset, fail, fail + 1, fail); emit_failure(cg, "slice range out of bounds"); fprintf(cg->out, ".Lslice_done_%d:\n", fail + 1);
        fprintf(cg->out, "    mov QWORD PTR [rbp-%d], 1\n    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n    mov rax, QWORD PTR [rbp-%d]\n    imul rax, 4\n    add rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n    mov QWORD PTR [rbp-%d], 1\n", dst->rank_offset, length, dst->length_offset, start, src->offset, dst->offset, dst->dim_offsets[0]);
        for (int i = 1; i < COBRA_VIEW_MAX_RANK; i++) fprintf(cg->out, "    mov QWORD PTR [rbp-%d], 1\n", dst->dim_offsets[i]);
        for (int i = 0; i < COBRA_VIEW_MAX_RANK; i++) fprintf(cg->out, "    mov QWORD PTR [rbp-%d], 1\n", dst->stride_offsets[i]);
    }
}

/* ------------------------------------------------------------------ */
/* User-loop auto-vectorizer.                                          */
/*                                                                     */
/* An index-pure `for i in len(values):` loop over f32 buffers lowers  */
/* straight to AVX2: eight lanes per iteration plus a bounds-checked   */
/* scalar tail for the remaining 0-7 elements. No IR, no annotations,  */
/* no runtime dispatch: the analysis is local to the loop body.        */
/* ------------------------------------------------------------------ */

static bool vec_buffer_is_f32(CodeGen *cg, const char *name) {
    VarSymbol *s = find_symbol(cg, name);
    if (!s) return false;
    if (s->kind == SYM_SLICE && s->type == COBRA_TYPE_SLICE_F32) return true;
    if ((s->kind == SYM_LIST || s->kind == SYM_ARRAY) && s->element_type == COBRA_TYPE_F32) return true;
    return false;
}

/* A leaf cannot recurse, so it can be emitted directly into any ymm. */
static bool vec_leaf(ASTNode *e) {
    return e->type == AST_FLOAT_LITERAL || e->type == AST_INT_LITERAL ||
           e->type == AST_VAR_REF || e->type == AST_ARRAY_INDEX;
}

#define VEC_MAX_DEPTH 8

/* Expression is index-pure: literals, loop-invariant scalars, and
   current-element reads `buf[i]` with the loop index. Anything else,
   including non-current indices and calls, keeps the scalar lowering.
   The depth cap matches the per-level scratch slots the emitter reserves,
   so every accepted tree lowers within the register contract. */
static bool vec_expr_pure(CodeGen *cg, ASTNode *e, const char *loop_var, int depth) {
    if (!e) return false;
    switch (e->type) {
        case AST_FLOAT_LITERAL:
        case AST_INT_LITERAL:
            return true;
        case AST_VAR_REF: {
            if (!strcmp(e->name, loop_var)) return false;
            VarSymbol *s = find_symbol(cg, e->name);
            return s && (s->kind == SYM_F32 || s->kind == SYM_SCALAR);
        }
        case AST_ARRAY_INDEX: {
            if (e->child_count != 1) return false;
            ASTNode *idx = e->children[0];
            if (idx->type != AST_VAR_REF || strcmp(idx->name, loop_var)) return false;
            return vec_buffer_is_f32(cg, e->name);
        }
        case AST_BINARY_OP: {
            if (depth >= VEC_MAX_DEPTH) return false;
            if (strcmp(e->name, "+") && strcmp(e->name, "-") &&
                strcmp(e->name, "*") && strcmp(e->name, "/")) return false;
            if (e->child_count < 2) return false;
            return vec_expr_pure(cg, e->children[0], loop_var, depth + 1) &&
                   vec_expr_pure(cg, e->children[1], loop_var, depth + 1);
        }
        default:
            return false;
    }
}

#define VEC_CLAMP_MAX 1
#define VEC_CLAMP_MIN 2

/* ReLU/ReLU6-style clamp: `if buf[i] OP const { buf[i] = const }` in
   either operand order, where the clamp constant equals the bound so the
   whole statement is exactly vmaxps or vminps. Returns 0 otherwise. */
static int vec_clamp_kind(CodeGen *cg, ASTNode *stmt, const char *loop_var) {
    if (stmt->type != AST_IF_STMT || stmt->child_count != 2) return 0;
    ASTNode *cond = stmt->children[0];
    ASTNode *then_block = stmt->children[1];
    if (cond->type != AST_BINARY_OP || then_block->type != AST_PROGRAM ||
        then_block->child_count != 1) return 0;
    ASTNode *assign = then_block->children[0];
    if (assign->type != AST_INDEX_ASSIGN || assign->child_count != 2) return 0;
    ASTNode *idx = assign->children[0];
    ASTNode *clamp_val = assign->children[1];
    if (idx->type != AST_VAR_REF || strcmp(idx->name, loop_var)) return 0;
    if (clamp_val->type != AST_FLOAT_LITERAL) return 0;
    if (!vec_buffer_is_f32(cg, assign->name)) return 0;

    const char *op = cond->name;
    if (strcmp(op, "<") && strcmp(op, ">") && strcmp(op, "<=") && strcmp(op, ">=")) return 0;
    ASTNode *lhs = cond->children[0], *rhs = cond->children[1];
    bool read_lhs = lhs->type == AST_ARRAY_INDEX && lhs->child_count == 1 &&
                    lhs->children[0]->type == AST_VAR_REF &&
                    !strcmp(lhs->children[0]->name, loop_var) &&
                    vec_buffer_is_f32(cg, lhs->name);
    bool read_rhs = rhs->type == AST_ARRAY_INDEX && rhs->child_count == 1 &&
                    rhs->children[0]->type == AST_VAR_REF &&
                    !strcmp(rhs->children[0]->name, loop_var) &&
                    vec_buffer_is_f32(cg, rhs->name);
    ASTNode *bound = read_lhs && rhs->type == AST_FLOAT_LITERAL ? rhs :
                     read_rhs && lhs->type == AST_FLOAT_LITERAL ? lhs : NULL;
    if (!bound || bound->float_val != clamp_val->float_val) return 0;

    bool lt = !strcmp(op, "<") || !strcmp(op, "<=");
    if (read_lhs && lt) return VEC_CLAMP_MAX;   /* v[i] < c  => max(v, c) */
    if (read_lhs) return VEC_CLAMP_MIN;         /* v[i] > c  => min(v, c) */
    if (lt) return VEC_CLAMP_MIN;               /* c < v[i]  => min(v, c) */
    return VEC_CLAMP_MAX;                       /* c > v[i]  => max(v, c) */
}

/* Every statement must be an element-wise store or a constant clamp. */
static bool vec_body_pure(CodeGen *cg, ASTNode *body, const char *loop_var) {
    if (!body || body->type != AST_PROGRAM || body->child_count == 0) return false;
    for (size_t i = 0; i < body->child_count; i++) {
        ASTNode *s = body->children[i];
        if (s->type == AST_INDEX_ASSIGN) {
            if (s->child_count != 2) return false;
            ASTNode *idx = s->children[0];
            if (idx->type != AST_VAR_REF || strcmp(idx->name, loop_var)) return false;
            if (!vec_buffer_is_f32(cg, s->name)) return false;
            if (!vec_expr_pure(cg, s->children[1], loop_var, 0)) return false;
        } else if (!vec_clamp_kind(cg, s, loop_var)) {
            return false;
        }
    }
    return true;
}

/* Broadcast a 32-bit float constant from .rodata into ymm{reg}. */
static void vec_emit_float_const(CodeGen *cg, float value, int reg) {
    int id = cg->const_count++;
    union { float f; uint32_t u; } bits; bits.f = value;
    fprintf(cg->out, "    .section .rodata\n.LCf%d:\n    .long 0x%08x\n    .text\n", id, bits.u);
    fprintf(cg->out, "    vbroadcastss ymm%d, DWORD PTR [rip + .LCf%d]\n", reg, id);
}

/* Emit one index-pure expression into ymm{reg} as eight f32 lanes.
   Each recursion level spills through its own scratch slot (base - depth*32)
   so a nested subexpression can never clobber a value an outer level still
   needs; ymm1 is only reloaded after the lhs recursion completes. */
static void vec_emit_expr(CodeGen *cg, ASTNode *e, const char *loop_var, int reg, int base, int depth) {
    switch (e->type) {
        case AST_FLOAT_LITERAL: vec_emit_float_const(cg, e->float_val, reg); return;
        case AST_INT_LITERAL: vec_emit_float_const(cg, (float)e->literal_i64, reg); return;
        case AST_VAR_REF: {
            VarSymbol *s = find_symbol(cg, e->name);
            if (s->kind == SYM_F32) {
                fprintf(cg->out, "    vbroadcastss ymm%d, DWORD PTR [rbp-%d]\n", reg, s->offset);
            } else {
                fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n", s->offset);
                fprintf(cg->out, "    vcvtsi2ss xmm%d, xmm%d, rax\n    vbroadcastss ymm%d, xmm%d\n",
                        reg, reg, reg, reg);
            }
            return;
        }
        case AST_ARRAY_INDEX:
            emit_load_buffer_ptr(cg, e->name, "rbx");
            fprintf(cg->out, "    vmovups ymm%d, YMMWORD PTR [rbx + rdx*4]\n", reg);
            return;
        case AST_BINARY_OP: {
            ASTNode *lhs = e->children[0], *rhs = e->children[1];
            const char *op = e->name;
            /* FMA fusion: (a * b) + c -> vfmadd213ps ymm0, ymm2, ymm1. */
            if (!strcmp(op, "+") && reg == 0 && lhs->type == AST_BINARY_OP &&
                !strcmp(lhs->name, "*") && vec_leaf(lhs->children[0]) &&
                vec_leaf(lhs->children[1]) && vec_leaf(rhs)) {
                vec_emit_expr(cg, lhs->children[0], loop_var, 2, base, depth);
                vec_emit_expr(cg, lhs->children[1], loop_var, 0, base, depth);
                vec_emit_expr(cg, rhs, loop_var, 1, base, depth);
                fprintf(cg->out, "    vfmadd213ps ymm0, ymm2, ymm1\n");
                return;
            }
            int slot = base - depth * 32;
            if (vec_leaf(lhs) && vec_leaf(rhs)) {
                /* Leaves only: no recursion, so ymm1 is safe. */
                vec_emit_expr(cg, rhs, loop_var, 1, base, depth);
                vec_emit_expr(cg, lhs, loop_var, reg, base, depth);
            } else {
                /* Any nesting spills rhs at this level's own slot and reloads
                   it only after the lhs recursion (which uses deeper slots)
                   has finished. */
                vec_emit_expr(cg, rhs, loop_var, 0, base, depth + 1);
                fprintf(cg->out, "    vmovups YMMWORD PTR [rbp-%d], ymm0\n", slot);
                vec_emit_expr(cg, lhs, loop_var, reg, base, depth + 1);
                fprintf(cg->out, "    vmovups ymm1, YMMWORD PTR [rbp-%d]\n", slot);
            }
            if (!strcmp(op, "+")) fprintf(cg->out, "    vaddps ymm%d, ymm%d, ymm1\n", reg, reg);
            else if (!strcmp(op, "-")) fprintf(cg->out, "    vsubps ymm%d, ymm%d, ymm1\n", reg, reg);
            else if (!strcmp(op, "*")) fprintf(cg->out, "    vmulps ymm%d, ymm%d, ymm1\n", reg, reg);
            else fprintf(cg->out, "    vdivps ymm%d, ymm%d, ymm1\n", reg, reg);
            return;
        }
        default:
            fprintf(stderr, "CodeGen Error: unexpected node in vectorized expression\n");
            exit(EXIT_FAILURE);
    }
}

/* Emit one constant clamp as vmaxps/vminps over the eight live lanes. */
static void vec_emit_clamp(CodeGen *cg, ASTNode *stmt, const char *loop_var) {
    int kind = vec_clamp_kind(cg, stmt, loop_var);
    ASTNode *cond = stmt->children[0];
    ASTNode *assign = stmt->children[1]->children[0];
    ASTNode *lhs = cond->children[0], *rhs = cond->children[1];
    ASTNode *read_node = lhs->type == AST_ARRAY_INDEX ? lhs : rhs;
    ASTNode *bound = read_node == lhs ? rhs : lhs;
    emit_load_buffer_ptr(cg, read_node->name, "rbx");
    fprintf(cg->out, "    vmovups ymm0, YMMWORD PTR [rbx + rdx*4]\n");
    vec_emit_float_const(cg, bound->float_val, 1);
    if (kind == VEC_CLAMP_MAX) fprintf(cg->out, "    vmaxps ymm0, ymm0, ymm1\n");
    else fprintf(cg->out, "    vminps ymm0, ymm0, ymm1\n");
    emit_load_buffer_ptr(cg, assign->name, "rbx");
    fprintf(cg->out, "    vmovups YMMWORD PTR [rbx + rdx*4], ymm0\n");
}

/* Try to lower the whole `for` to the AVX2 kernel with a scalar tail.
   Returns true when emitted; the caller then skips the scalar loop. */
/* The AVX2 eight-lane loop plus bounds-checked scalar tail, emitted over an
   already-initialized range: the sequential path starts at 0 with the buffer
   length as the bound; a @parallel worker starts at its chunk start with its
   chunk end. The caller sets rdx (running index) and rcx (exclusive end) and
   the index/bound frame slots before calling this. */
static void emit_vec_range(CodeGen *cg, ASTNode *n, ASTNode *body,
                           int index, int bound, int label, int scratch) {
    fprintf(cg->out, ".Lvec_%d:\n    lea r8, [rdx + 8]\n    cmp r8, rcx\n    ja .Lvec_done_%d\n", label, label);
    for (size_t i = 0; i < body->child_count; i++) {
        ASTNode *s = body->children[i];
        if (s->type == AST_INDEX_ASSIGN) {
            vec_emit_expr(cg, s->children[s->child_count - 1], n->name, 0, scratch, 0);
            emit_load_buffer_ptr(cg, s->name, "rbx");
            fprintf(cg->out, "    vmovups YMMWORD PTR [rbx + rdx*4], ymm0\n");
        } else {
            vec_emit_clamp(cg, s, n->name);
        }
    }
    fprintf(cg->out, "    add rdx, 8\n    jmp .Lvec_%d\n.Lvec_done_%d:\n", label, label);
    /* Drop YMM state before the SSE scalar tail avoids AVX-SSE transition
       penalties on the remaining 0-7 elements. */
    fprintf(cg->out, "    vzeroupper\n");
    fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rdx\n", index);

    /* Bounds-checked scalar tail for the remaining 0-7 elements. */
    fprintf(cg->out, ".Lfor_%d:\n    mov rax, QWORD PTR [rbp-%d]\n    cmp rax, QWORD PTR [rbp-%d]\n    jae .Lfor_done_%d\n", label, index, bound, label);
    snprintf(cg->loops[cg->loop_depth].name, sizeof(cg->loops[cg->loop_depth].name), "%s", n->name);
    cg->loops[cg->loop_depth].secondary_name[0] = 0;
    cg->loops[cg->loop_depth].source[0] = 0;
    cg->loops[cg->loop_depth].active = true;
    cg->loops[cg->loop_depth].enumerate = false;
    cg->loops[cg->loop_depth].index_offset = index;
    cg->loops[cg->loop_depth].element_type = COBRA_TYPE_F32;
    cg->loop_depth++;
    emit_statement(cg, body);
    cg->loop_depth--;
    fprintf(cg->out, "    inc QWORD PTR [rbp-%d]\n    jmp .Lfor_%d\n.Lfor_done_%d:\n", index, label, label);
}

static bool try_emit_vectorized(CodeGen *cg, ASTNode *n, ASTNode *target, int index, int bound, int label) {
    if (!cg->opt_vectorize || cg->target != TARGET_LINUX_X86_64) return false;
    const char *source = NULL;
    if (target->type == AST_LEN_EXPR && target->child_count == 1 &&
        target->children[0]->type == AST_VAR_REF) {
        source = target->children[0]->name;
    } else if (target->type == AST_FUNC_CALL && !strcmp(target->name, "range") &&
               target->child_count == 1 && target->children[0]->type == AST_LEN_EXPR &&
               target->children[0]->child_count == 1 &&
               target->children[0]->children[0]->type == AST_VAR_REF) {
        source = target->children[0]->children[0]->name;
    }
    if (!source || !vec_buffer_is_f32(cg, source)) return false;
    if (n->child_count < 2) return false;
    ASTNode *body = n->children[1];
    if (body->type != AST_PROGRAM || !vec_body_pure(cg, body, n->name)) return false;

    int scratch = reserve(cg, 32 * VEC_MAX_DEPTH);
    emit_load_buffer_len(cg, source, "rax");
    fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", bound);
    fprintf(cg->out, "    xor edx, edx\n    mov rcx, rax\n");
    emit_vec_range(cg, n, body, index, bound, label, scratch);
    return true;
}

/* --- Native worker-pool dispatch for proven @parallel loops --------------- */
/* A `@parallel: { for i in len(v): ... }` block whose body passes the same
   index-purity proof as auto-vectorization is lowered to one call of the
   persistent worker pool. The loop body becomes a standalone worker function
   with signature void worker(void *context, size_t start, size_t end); each
   worker loads the touched buffers and loop-invariant scalars from a
   contiguous context region in the caller's frame, then runs the exact AVX2
   vectorized body over its own disjoint chunk [start, end). Small ranges and
   unsupported bodies fall back to the normal checked loop. */

typedef struct {
    char name[COBRA_MAX_IDENT_LEN];
    CobraTypeKind type;
} ParCapture;

static bool par_buffer_ok(CodeGen *cg, const char *name) {
    VarSymbol *s = find_symbol(cg, name);
    return s && s->kind == SYM_SLICE && s->type == COBRA_TYPE_SLICE_F32;
}

static void par_collect(CodeGen *cg, ASTNode *node, const char *loop_var,
                        ParCapture *bufs, int *nb, ParCapture *scals, int *ns,
                        int depth) {
    if (!node || depth > 8) return;
    switch (node->type) {
        case AST_VAR_REF:
            if (strcmp(node->name, loop_var) != 0) {
                VarSymbol *s = find_symbol(cg, node->name);
                if (s && (s->kind == SYM_F32 || s->kind == SYM_SCALAR)) {
                    for (int i = 0; i < *ns; i++)
                        if (strcmp(scals[i].name, node->name) == 0) return;
                    if (*ns < PAR_MAX_CAPTURES) {
                        snprintf(scals[*ns].name, sizeof(scals[*ns].name), "%.63s", node->name);
                        scals[*ns].type = s->type;
                        (*ns)++;
                    }
                }
            }
            return;
        case AST_ARRAY_INDEX:
            if (par_buffer_ok(cg, node->name)) {
                bool seen = false;
                for (int i = 0; i < *nb; i++)
                    if (strcmp(bufs[i].name, node->name) == 0) seen = true;
                if (!seen && *nb < PAR_MAX_CAPTURES) {
                    snprintf(bufs[*nb].name, sizeof(bufs[*nb].name), "%.63s", node->name);
                    (*nb)++;
                }
            }
            break;
        default:
            break;
    }
    for (size_t i = 0; i < node->child_count; i++)
        par_collect(cg, node->children[i], loop_var, bufs, nb, scals, ns, depth + 1);
}

static bool try_emit_parallel(CodeGen *cg, ASTNode *n) {
    if (!cg->opt_vectorize || cg->target != TARGET_LINUX_X86_64) return false;
    if (!n || n->child_count != 1) return false;
    ASTNode *block = n->children[0];
    if (!block || block->type != AST_PROGRAM || block->child_count != 1) return false;
    ASTNode *loop = block->children[0];
    if (loop->type != AST_FOR_LOOP || loop->child_count < 2) return false;
    ASTNode *target = loop->children[0];
    const char *source = NULL;
    if (target->type == AST_LEN_EXPR && target->child_count == 1 &&
        target->children[0]->type == AST_VAR_REF) {
        source = target->children[0]->name;
    } else if (target->type == AST_FUNC_CALL && !strcmp(target->name, "range") &&
               target->child_count == 1 && target->children[0]->type == AST_LEN_EXPR &&
               target->children[0]->child_count == 1 &&
               target->children[0]->children[0]->type == AST_VAR_REF) {
        source = target->children[0]->children[0]->name;
    }
    if (!source || !par_buffer_ok(cg, source)) return false;
    ASTNode *body = loop->children[1];
    if (body->type != AST_PROGRAM || !vec_body_pure(cg, body, loop->name)) return false;

    ParCapture bufs[PAR_MAX_CAPTURES];
    ParCapture scals[PAR_MAX_CAPTURES];
    int nb = 0, ns = 0;
    snprintf(bufs[nb++].name, sizeof(bufs[0].name), "%.63s", source);
    par_collect(cg, body, loop->name, bufs, &nb, scals, &ns, 0);
    if (nb > PAR_MAX_CAPTURES || ns > PAR_MAX_CAPTURES) return false;
    if (cg->pending_parallel_count >= 16) return false;

    int worker = cg->label_count++;
    int ctx = reserve(cg, (2 * nb + ns) * 8);

    /* Queue the worker; it is emitted after the caller's body completes. */
    int slot = cg->pending_parallel_count++;
    PendingParallel *pw = &cg->pending_parallel[slot];
    pw->active = true;
    pw->id = worker;
    pw->loop = loop;
    snprintf(pw->source, sizeof(pw->source), "%.63s", source);
    pw->nb = nb;
    pw->ns = ns;
    for (int i = 0; i < nb; i++) snprintf(pw->bufs[i], sizeof(pw->bufs[i]), "%.63s", bufs[i].name);
    for (int i = 0; i < ns; i++) {
        snprintf(pw->scals[i], sizeof(pw->scals[i]), "%.63s", scals[i].name);
        pw->scal_types[i] = scals[i].type;
    }

    /* Caller: fill the context and dispatch. */
    for (int i = 0; i < nb; i++) {
        VarSymbol *s = find_symbol(cg, bufs[i].name);
        if (!s) return false;
        fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", s->offset, ctx - i * 16);
        fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", s->length_offset, ctx - (i * 16 + 8));
    }
    for (int i = 0; i < ns; i++) {
        VarSymbol *s = find_symbol(cg, scals[i].name);
        if (!s) return false;
        fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", s->offset, ctx - (2 * nb + i) * 8);
    }
    emit_load_buffer_len(cg, source, "rax");
    fprintf(cg->out, "    lea rdi, [rip + cobra_par_worker_%d]\n    lea rsi, [rbp-%d]\n    mov rdx, rax\n    call cobra_parallel_for@PLT\n", worker, ctx);
    return true;
}

/* Flush queued @parallel workers as standalone functions after the caller's
   body (and propagate label) have been emitted. Each worker has its own
   frame and ABI: rdi = context pointer, rsi = chunk start, rdx = chunk end.
   It loads the touched buffers and loop-invariant scalars from the context
   region, then runs the same AVX2 vectorized body over [start, end). */
static void flush_pending_parallel(CodeGen *cg) {
    for (int s = 0; s < cg->pending_parallel_count; s++) {
        PendingParallel *pw = &cg->pending_parallel[s];
        if (!pw->active) continue;
        ASTNode *loop = pw->loop;
        ASTNode *body = loop->child_count > 1 ? loop->children[1] : NULL;
        if (!body || body->type != AST_PROGRAM) continue;

        cg->symbol_count = 0;
        cg->stack_offset = COBRA_LOCAL_BASE;
        cg->loop_depth = 0;

        fprintf(cg->out, "    .type cobra_par_worker_%d, @function\ncobra_par_worker_%d:\n    push rbp\n    mov rbp, rsp\n    sub rsp, %d\n    mov QWORD PTR [rbp-248], rbx\n", pw->id, pw->id, COBRA_FRAME_BYTES);
        int ctx_slot = reserve(cg, 8);
        fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rdi\n", ctx_slot);
        for (int i = 0; i < pw->nb; i++) {
            VarSymbol *sv = ensure_slice(cg, pw->bufs[i], COBRA_TYPE_SLICE_F32);
            fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov rbx, QWORD PTR [rax + %d]\n    mov QWORD PTR [rbp-%d], rbx\n", ctx_slot, i * 16, sv->offset);
            fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov rbx, QWORD PTR [rax + %d]\n    mov QWORD PTR [rbp-%d], rbx\n", ctx_slot, i * 16 + 8, sv->length_offset);
        }
        for (int i = 0; i < pw->ns; i++) {
            VarSymbol *sv = ensure_scalar(cg, pw->scals[i], pw->scal_types[i]);
            fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov rbx, QWORD PTR [rax + %d]\n    mov QWORD PTR [rbp-%d], rbx\n", ctx_slot, (2 * pw->nb + i) * 8, sv->offset);
        }
        int idx = reserve(cg, 8), end = reserve(cg, 8);
        fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rsi\n    mov QWORD PTR [rbp-%d], rdx\n    mov rdx, rsi\n    mov rcx, QWORD PTR [rbp-%d]\n", idx, end, end);
        int scratch = reserve(cg, 32 * VEC_MAX_DEPTH);
        emit_vec_range(cg, loop, body, idx, end, pw->id, scratch);
        fprintf(cg->out, "    mov rbx, QWORD PTR [rbp-248]\n    mov rsp, rbp\n    pop rbp\n    ret\n");
        pw->active = false;
    }
    cg->pending_parallel_count = 0;
}

static void emit_loop_owned_cleanup(CodeGen *cg, ASTNode *body);

static void emit_for(CodeGen *cg, ASTNode *n) {
    int index = reserve(cg, 8), bound = reserve(cg, 8), label = cg->label_count++;
    ASTNode *target = n->child_count ? n->children[0] : NULL;
    bool is_range = target && target->type == AST_FUNC_CALL && !strcmp(target->name, "range");
    bool is_enumerate = target && target->type == AST_FUNC_CALL && !strcmp(target->name, "enumerate");
    bool element = target && target->type == AST_VAR_REF && symbol_is_buffer(cg, target->name);
    const char *source = element || is_enumerate ?
        (is_enumerate && target->child_count && target->children[0]->type == AST_VAR_REF ?
         target->children[0]->name : (element ? target->name : NULL)) : NULL;
    int step = 0;

    /* User-loop auto-vectorization: an index-pure `for i in len(values):`
       over f32 buffers lowers straight to AVX2 with a scalar tail. */
    if (target && (target->type == AST_LEN_EXPR ||
                   (target->type == AST_FUNC_CALL && !strcmp(target->name, "range"))) &&
        try_emit_vectorized(cg, n, target, index, bound, label)) {
        return;
    }

    if (is_range) {
        if (target->child_count == 1) {
            fprintf(cg->out, "    mov QWORD PTR [rbp-%d], 0\n", index);
            emit_expr(cg, target->children[0]);
        } else {
            emit_expr(cg, target->children[0]);
            fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", index);
            emit_expr(cg, target->children[1]);
        }
        fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", bound);
        if (target->child_count == 3) {
            step = reserve(cg, 8);
            emit_expr(cg, target->children[2]);
            fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", step);
        } else {
            step = reserve(cg, 8);
            fprintf(cg->out, "    mov QWORD PTR [rbp-%d], 1\n", step);
        }
        /* The current compact loop uses an increasing or decreasing bound,
           selected once from the sign of step. */
        int direction = cg->label_count++;
        fprintf(cg->out, "    cmp QWORD PTR [rbp-%d], 0\n    jg .Lrange_positive_%d\n    jl .Lrange_negative_%d\n", step, direction, direction);
        emit_failure(cg, "range step cannot be zero");
        fprintf(cg->out, ".Lrange_positive_%d:\n", direction);
        fprintf(cg->out, "    jmp .Lrange_direction_done_%d\n.Lrange_negative_%d:\n", direction, direction);
        fprintf(cg->out, ".Lrange_direction_done_%d:\n", direction);
    } else if (source) {
        emit_load_buffer_len(cg, source, "rax");
        fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n    mov QWORD PTR [rbp-%d], 0\n", bound, index);
    } else {
        emit_expr(cg, target);
        fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n    mov QWORD PTR [rbp-%d], 0\n", bound, index);
    }

    fprintf(cg->out, ".Lfor_%d:\n    mov rax, QWORD PTR [rbp-%d]\n", label, index);
    if (is_range && step) {
        int positive = cg->label_count++;
        fprintf(cg->out, "    cmp QWORD PTR [rbp-%d], 0\n    jg .Lrange_check_pos_%d\n    cmp rax, QWORD PTR [rbp-%d]\n    jle .Lfor_done_%d\n    jmp .Lrange_body_%d\n.Lrange_check_pos_%d:\n    cmp rax, QWORD PTR [rbp-%d]\n    jge .Lfor_done_%d\n.Lrange_body_%d:\n", step, positive, bound, label, positive, positive, bound, label, positive);
    } else {
        fprintf(cg->out, "    cmp rax, QWORD PTR [rbp-%d]\n    jae .Lfor_done_%d\n", bound, label);
    }
    snprintf(cg->loops[cg->loop_depth].name, sizeof(cg->loops[cg->loop_depth].name), "%s", n->name);
    snprintf(cg->loops[cg->loop_depth].secondary_name, sizeof(cg->loops[cg->loop_depth].secondary_name), "%s", n->secondary_name);
    if (source) snprintf(cg->loops[cg->loop_depth].source, sizeof(cg->loops[cg->loop_depth].source), "%s", source); else cg->loops[cg->loop_depth].source[0] = 0;
    cg->loops[cg->loop_depth].active = true;
    cg->loops[cg->loop_depth].enumerate = is_enumerate;
    cg->loops[cg->loop_depth].index_offset = index;
    VarSymbol *ss = source ? find_symbol(cg, source) : NULL;
    cg->loops[cg->loop_depth].element_type = ss &&
        ((ss->type == COBRA_TYPE_SLICE_F32) ||
         ((ss->kind == SYM_LIST || ss->kind == SYM_ARRAY) && ss->element_type == COBRA_TYPE_F32)) ?
        COBRA_TYPE_F32 : COBRA_TYPE_I64;
    if (!source && !is_range) {
        VarSymbol *iter_symbol = ensure_scalar(cg, n->name, COBRA_TYPE_I64);
        fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", index, iter_symbol->offset);
    }
    if (is_enumerate && n->secondary_name[0] != '\0') {
        VarSymbol *value_symbol = ensure_scalar(cg, n->secondary_name, cg->loops[cg->loop_depth].element_type);
        (void)value_symbol;
    }
    cg->loop_depth++;
    if (n->child_count > 1) { emit_statement(cg, n->children[1]); emit_loop_owned_cleanup(cg, n->children[1]); }
    cg->loop_depth--;
    if (is_range) fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    add rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", index, step, index);
    else fprintf(cg->out, "    inc QWORD PTR [rbp-%d]\n", index);
    fprintf(cg->out, "    jmp .Lfor_%d\n.Lfor_done_%d:\n", label, label);
}

static void emit_inline_asm(CodeGen *cg, const char *source) {
    if (!source) return;
    for (const char *p = source; *p; p++) {
        if ((unsigned char)p[0] == 92 && p[1] == 'n') { fputc('\n', cg->out); p++; }
        else if ((unsigned char)p[0] == 92 && p[1] == '"') { fputc('"', cg->out); p++; }
        else fputc(*p, cg->out);
    }
    fputc('\n', cg->out);
}

/* True if `canonical` has, directly or through an embedded (by-value) nested
   struct field at any depth, at least one field the direct backend already
   recognizes as heap-owned (owned string or owned slice) - the exact shape
   the struct-field-ownership work this session made valid. Nested struct
   fields are stored inline (contiguous, by-value), never by pointer, so
   walking into them here is just byte-address arithmetic, not a second
   allocation to reason about. */
static bool struct_canonical_has_owned_payload(const CobraType *canonical, int depth) {
    if (!canonical || depth > 8) return false;
    for (size_t i = 0; i < canonical->field_count; i++) {
        const CobraTypeField *f = &canonical->fields[i];
        if (!f->type) continue;
        if (f->ownership == COBRA_OWNERSHIP_OWNED &&
            (f->type->kind == COBRA_TYPE_STRING || cobra_type_is_slice_kind(f->type->kind))) {
            return true;
        }
        if (f->type->kind == COBRA_TYPE_STRUCT &&
            struct_canonical_has_owned_payload(f->type, depth + 1)) {
            return true;
        }
    }
    return false;
}

static bool struct_type_has_owned_scalar_fields(CodeGen *cg, const char *type_name) {
    if (!cg->root || !cg->root->canonical_arena || !type_name || !type_name[0]) return false;
    const CobraType *canonical = cobra_type_struct_layout(cg->root->canonical_arena, cg->root, type_name);
    return struct_canonical_has_owned_payload(canonical, 0);
}

/* --- Whole-program struct-parameter borrow inference ---
   A struct parameter is passed as a single pointer ABI slot (see
   cobra_type_abi_slots); today every non-`out` parameter unconditionally
   copies struct_storage_size bytes into private frame storage at function
   entry (emit_function's COBRA_TYPE_STRUCT branch) so the callee can freely
   mutate its own copy without the caller observing it, matching
   examples/72_struct_parameters.cb's documented by-value semantics.

   That copy is only needed when the callee's body could either mutate the
   parameter or let a reference to it outlive the call (assign it elsewhere,
   return it, or pass it to a further call that itself doesn't prove the same
   safety). When neither is true, aliasing the caller's pointer directly -
   exactly the mechanism `out` parameters already use via VarSymbol.indirect -
   is behaviorally identical and skips the copy. compute_param_borrowed
   decides this per (function, parameter) with a memoized, cycle-safe
   recursive scan; a call through a `dyn Trait` or an unresolvable callee
   (FFI/import) is always treated as escaping, since neither has a summary
   this pass can consult. */
static bool compute_param_borrowed(CodeGen *cg, ASTNode *fn, const char *param_name);

static bool param_use_escapes(CodeGen *cg, const ASTNode *n, const char *param_name) {
    if (!n) return false;
    if (n->type == AST_MEMBER_ASSIGN || n->type == AST_INDEX_ASSIGN) {
        /* box.field = v / box.top.x = v: the base identifier name is
           preserved through the whole access chain (see parse_primary's
           member-chain construction), so a direct name match here is a
           write through the parameter at any nesting depth. */
        if (!strcmp(n->name, param_name)) return true;
    }
    if (n->type == AST_ASSIGN || n->type == AST_VAR_DECL) {
        bool is_field_write = n->secondary_name[0] != '\0';
        if (!strcmp(n->name, param_name)) {
            /* Any write through the parameter's own name - whole reassignment
               or a field store - mutates the memory an alias would share
               with the caller. */
            return true;
        }
        if (n->child_count > 0 && n->children[0]->type == AST_VAR_REF &&
            !strcmp(n->children[0]->name, param_name)) {
            return true; /* copied elsewhere as an rvalue */
        }
        (void)is_field_write;
    }
    if (n->type == AST_RETURN) {
        for (size_t i = 0; i < n->child_count; i++)
            if (n->children[i]->type == AST_VAR_REF && !strcmp(n->children[i]->name, param_name))
                return true;
    }
    if (n->type == AST_FUNC_CALL) {
        bool any_arg_is_param = false;
        for (size_t i = 0; i < n->child_count; i++)
            if (n->children[i]->type == AST_VAR_REF && !strcmp(n->children[i]->name, param_name))
                any_arg_is_param = true;
        if (any_arg_is_param) {
            /* Qualified calls (module/dyn Trait/static-dispatch method
               syntax) and indirect fn(...)->... calls have no statically
               resolvable single callee summary to consult here - treat
               conservatively as escaping. */
            if (n->qualifier[0] || n->is_indirect_call) return true;
            ASTNode *callee = find_function(cg, n->name);
            if (!callee) return true; /* FFI/imported/builtin: opaque */
            for (size_t i = 0; i < n->child_count; i++) {
                if (n->children[i]->type != AST_VAR_REF || strcmp(n->children[i]->name, param_name)) continue;
                ASTNode *callee_param = function_param_node(cg, n->name, i);
                if (!callee_param || callee_param->declared_type != COBRA_TYPE_STRUCT) return true;
                if (!compute_param_borrowed(cg, callee, callee_param->name)) return true;
            }
        }
    }
    for (size_t i = 0; i < n->child_count; i++)
        if (param_use_escapes(cg, n->children[i], param_name)) return true;
    return false;
}

static bool compute_param_borrowed(CodeGen *cg, ASTNode *fn, const char *param_name) {
    int slot = -1;
    for (int i = 0; i < cg->param_summary_count; i++) {
        if (!strcmp(cg->param_summary_cache[i].fn_name, fn->name) &&
            !strcmp(cg->param_summary_cache[i].param_name, param_name)) { slot = i; break; }
    }
    if (slot < 0) {
        if (cg->param_summary_count >= 512) return false; /* table exhausted: conservative */
        slot = cg->param_summary_count++;
        snprintf(cg->param_summary_cache[slot].fn_name, COBRA_MAX_IDENT_LEN, "%.63s", fn->name);
        snprintf(cg->param_summary_cache[slot].param_name, COBRA_MAX_IDENT_LEN, "%.63s", param_name);
        cg->param_summary_cache[slot].state = 0;
    }
    if (cg->param_summary_cache[slot].state == 1) return false; /* recursion: conservative */
    if (cg->param_summary_cache[slot].state == 2) return cg->param_summary_cache[slot].borrowed;
    cg->param_summary_cache[slot].state = 1;
    bool escapes = false;
    for (size_t i = 0; i < fn->child_count && !escapes; i++) {
        if (fn->children[i]->type == AST_PARAM) continue;
        escapes = param_use_escapes(cg, fn->children[i], param_name);
    }
    cg->param_summary_cache[slot].borrowed = !escapes;
    cg->param_summary_cache[slot].state = 2;
    return !escapes;
}

typedef struct { char name[COBRA_MAX_IDENT_LEN]; const ASTNode *decl_node; } AutofreeCandidate;

/* Walk the whole function body; disqualify a candidate the moment it's used
   anywhere that could create a second owner of its heap storage: reassigned
   as a whole value, copied into another variable, passed as a call argument,
   returned, or one of its fields is written from an existing variable
   (rather than a fresh literal/expression) - any of those could leave a
   second live pointer to the same allocation, and this pass has no way to
   prove that pointer dies first. Candidates that survive are the narrow,
   provably-safe case: declared bare, mutated only via field writes from
   fresh values, and never observed anywhere else. */
static void autofree_scan_disqualify(const ASTNode *n, AutofreeCandidate *cands, bool *disq, int count) {
    if (!n) return;
    if (n->type == AST_FUNC_CALL) {
        for (size_t i = 0; i < n->child_count; i++) {
            const ASTNode *arg = n->children[i];
            if (arg->type != AST_VAR_REF) continue;
            for (int k = 0; k < count; k++)
                if (!disq[k] && !strcmp(arg->name, cands[k].name)) disq[k] = true;
        }
    }
    if (n->type == AST_RETURN) {
        for (size_t i = 0; i < n->child_count; i++) {
            const ASTNode *rv = n->children[i];
            if (rv->type != AST_VAR_REF) continue;
            for (int k = 0; k < count; k++)
                if (!disq[k] && !strcmp(rv->name, cands[k].name)) disq[k] = true;
        }
    }
    if (n->type == AST_ASSIGN || n->type == AST_VAR_DECL) {
        for (int k = 0; k < count; k++) {
            if (disq[k] || n == cands[k].decl_node) continue;
            bool is_field_write = n->secondary_name[0] != '\0';
            if (!is_field_write && !strcmp(n->name, cands[k].name)) {
                disq[k] = true; /* whole-value reassignment of the candidate */
            } else if (is_field_write && !strcmp(n->name, cands[k].name) &&
                       n->child_count > 0 && n->children[0]->type == AST_VAR_REF) {
                disq[k] = true; /* field populated from an existing variable, not a fresh value */
            } else if (n->child_count > 0 && n->children[0]->type == AST_VAR_REF &&
                       !strcmp(n->children[0]->name, cands[k].name) &&
                       strcmp(n->name, cands[k].name) != 0) {
                disq[k] = true; /* candidate copied elsewhere as an rvalue */
            }
        }
    }
    /* `candidate.field = value` (and nested `candidate.a.b = value`) parses
       to AST_MEMBER_ASSIGN, not AST_ASSIGN with secondary_name set - the base
       identifier name is preserved through the whole access chain (see
       parse_primary), so a name match here is exactly the "field write"
       case above. Populating a field from an existing variable gives that
       variable's storage a second owner (the candidate's would-be freed
       field), so it must disqualify the same way - e.g. `let h: Holder;
       h.tag = some_string` needs h disqualified or some_string double-frees. */
    if (n->type == AST_MEMBER_ASSIGN) {
        for (int k = 0; k < count; k++) {
            if (disq[k] || n == cands[k].decl_node) continue;
            if (!strcmp(n->name, cands[k].name) && n->child_count > 0 &&
                n->children[n->child_count - 1]->type == AST_VAR_REF) {
                disq[k] = true;
            }
        }
    }
    for (size_t i = 0; i < n->child_count; i++) autofree_scan_disqualify(n->children[i], cands, disq, count);
}

static void autofree_collect_candidates(const ASTNode *n, CodeGen *cg, AutofreeCandidate *cands, int *count) {
    if (!n || *count >= 64) return;
    if ((n->type == AST_ASSIGN || n->type == AST_VAR_DECL) &&
        n->declared_type == COBRA_TYPE_STRUCT && n->child_count == 0 &&
        struct_type_has_owned_scalar_fields(cg, ast_payload_name(n))) {
        snprintf(cands[*count].name, COBRA_MAX_IDENT_LEN, "%.63s", n->name);
        cands[*count].decl_node = n;
        (*count)++;
    }
    for (size_t i = 0; i < n->child_count && *count < 64; i++)
        autofree_collect_candidates(n->children[i], cg, cands, count);
}

/* Populates cg->safe_autofree_structs with the names of bare-declared struct
   locals in `fn` provably safe to auto-free at scope exit; see the scan
   functions above for the exact soundness conditions. */
static void compute_safe_autofree_structs(CodeGen *cg, ASTNode *fn) {
    cg->safe_autofree_count = 0;
    static AutofreeCandidate cands[64];
    int count = 0;
    for (size_t i = 0; i < fn->child_count; i++) autofree_collect_candidates(fn->children[i], cg, cands, &count);
    bool disq[64] = {0};
    for (size_t i = 0; i < fn->child_count; i++) autofree_scan_disqualify(fn->children[i], cands, disq, count);
    for (int i = 0; i < count && cg->safe_autofree_count < 64; i++) {
        if (!disq[i]) snprintf(cg->safe_autofree_structs[cg->safe_autofree_count++], COBRA_MAX_IDENT_LEN, "%.63s", cands[i].name);
    }
}

static bool is_safe_autofree_struct(CodeGen *cg, const char *name) {
    for (int i = 0; i < cg->safe_autofree_count; i++)
        if (!strcmp(cg->safe_autofree_structs[i], name)) return true;
    return false;
}

/* Collect list/dict locals declared with a fresh literal inside `n` (a loop
   body), matching exactly the codegen sites that set VarSymbol.owned=true
   for SYM_LIST/SYM_DICT (the AST_ARRAY_LITERAL/AST_DICT_LITERAL branches in
   the AST_VAR_DECL/AST_ASSIGN case above). */
static void loop_owned_collect_candidates(const ASTNode *n, AutofreeCandidate *cands, int *count) {
    if (!n || *count >= 64) return;
    if ((n->type == AST_ASSIGN || n->type == AST_VAR_DECL) && n->child_count > 0) {
        const ASTNode *v = n->children[0];
        bool is_owned_list = v->type == AST_ARRAY_LITERAL && n->declared_type == COBRA_TYPE_LIST;
        bool is_owned_dict = v->type == AST_DICT_LITERAL;
        if (is_owned_list || is_owned_dict) {
            snprintf(cands[*count].name, COBRA_MAX_IDENT_LEN, "%.63s", n->name);
            cands[*count].decl_node = n;
            (*count)++;
        }
    }
    for (size_t i = 0; i < n->child_count && *count < 64; i++)
        loop_owned_collect_candidates(n->children[i], cands, count);
}

/* Free list/dict locals declared fresh inside one textual pass through a
   loop body, once per runtime iteration, right before the body's assembly
   jumps back to the loop condition. Reuses `autofree_scan_disqualify`
   (generic over any candidate-name list) scoped to just this loop body, so
   the exact same non-escaping soundness conditions phases 1-2 established
   for struct locals apply here: any use as a call argument, a return value,
   a whole-value reassignment target, or an rvalue copied elsewhere anywhere
   in the body disqualifies the candidate and it is left to leak as before.
   A survivor is set VarSymbol.owned=false after the emitted free so the
   function-exit cleanup in emit_scope_cleanup does not free it a second
   time; the next runtime iteration's declaration re-sets it, since the
   declaration and this cleanup are emitted once and both execute every
   iteration together. */
static void emit_loop_owned_cleanup(CodeGen *cg, ASTNode *body) {
    if (!body) return;
    AutofreeCandidate cands[64];
    int count = 0;
    loop_owned_collect_candidates(body, cands, &count);
    if (count == 0) return;
    bool disq[64] = {0};
    autofree_scan_disqualify(body, cands, disq, count);
    for (int i = 0; i < count; i++) {
        if (disq[i]) continue;
        VarSymbol *s = find_symbol(cg, cands[i].name);
        if (!s || !s->owned) continue;
        if (s->kind == SYM_LIST) {
            fprintf(cg->out,
                    "    lea rdi, [rbp-%d]\n    lea rsi, [rbp-%d]\n    lea rdx, [rbp-%d]\n    call cobra_list_free@PLT\n",
                    s->offset, s->length_offset, s->capacity_offset);
            s->owned = false;
        } else if (s->kind == SYM_DICT) {
            fprintf(cg->out,
                    "    lea rdi, [rbp-%d]\n    lea rsi, [rbp-%d]\n    call cobra_dict_free@PLT\n",
                    s->offset, s->length_offset);
            s->owned = false;
        }
    }
}

static void emit_statement(CodeGen *cg, ASTNode *n) {
    if (!n) return;
    switch (n->type) {
        case AST_PROGRAM: for (size_t i = 0; i < n->child_count; i++) emit_statement(cg, n->children[i]); return;
        case AST_VAR_DECL:
        case AST_ASSIGN: {
            if (n->declared_type == COBRA_TYPE_STRUCT) {
                /* Zero-initialized contiguous struct region; fields are written
                   through member assignment afterwards. */
                VarSymbol *s = ensure_struct(cg, n->name, ast_payload_name(n));
                int size = struct_storage_size(cg, ast_payload_name(n));
                /* Zero every native slot so unset fields read as 0. Struct
                   initializers copy bytes into this caller-owned local region;
                   no struct heap object is created. */
                for (int off = 0; off < size; off += 8)
                    fprintf(cg->out, "    mov QWORD PTR [rbp-%d], 0\n", s->array_base - off);
                if (n->child_count == 0 && is_safe_autofree_struct(cg, n->name)) s->owned = true;
                if (n->child_count > 0) {
                    ASTNode *initializer = n->children[0];
                    emit_expr(cg, initializer);
                    fprintf(cg->out, "    mov rsi, rax\n    lea rdi, [rbp-%d]\n", s->array_base);
                    emit_copy_memory(cg, "rsi", "rdi", size);
                }
                return;
            }
            if (n->child_count == 0) return;
            ASTNode *v = n->children[0];
            {
                /* Reassigning an existing struct local (`b = a`, no `: Type`
                   annotation - that's the declared_type==STRUCT case above)
                   is a whole-value member-wise copy, not a pointer store.
                   Falling through to the generic scalar path below would
                   overwrite the destination's first 8 bytes with the
                   source's address instead of copying its fields. */
                VarSymbol *existing_struct = find_symbol(cg, n->name);
                if (existing_struct && existing_struct->kind == SYM_STRUCT &&
                    (v->type == AST_VAR_REF ||
                     (v->type == AST_MEMBER_ACCESS && v->value_type == COBRA_TYPE_STRUCT))) {
                    int size = struct_storage_size(cg, existing_struct->type_name);
                    emit_struct_address(cg, v);
                    fprintf(cg->out, "    mov rsi, rax\n");
                    if (existing_struct->indirect)
                        fprintf(cg->out, "    mov rdi, QWORD PTR [rbp-%d]\n", existing_struct->array_base);
                    else
                        fprintf(cg->out, "    lea rdi, [rbp-%d]\n", existing_struct->array_base);
                    emit_copy_memory(cg, "rsi", "rdi", size);
                    return;
                }
            }
            CobraTypeKind sum_type = n->declared_type;
            if (sum_type != COBRA_TYPE_OPTION && sum_type != COBRA_TYPE_RESULT &&
                (v->value_type == COBRA_TYPE_OPTION || v->value_type == COBRA_TYPE_RESULT))
                sum_type = v->value_type;
            if (sum_type == COBRA_TYPE_OPTION || sum_type == COBRA_TYPE_RESULT) {
                CobraTypeKind payload = ast_element_kind(n) != COBRA_TYPE_UNTYPED ? ast_element_kind(n) : ast_element_kind(v);
                CobraTypeKind error = ast_error_kind(n) != COBRA_TYPE_UNTYPED ? ast_error_kind(n) : ast_error_kind(v);
                const char *payload_name = ast_payload_name(n)[0] ? ast_payload_name(n) : ast_payload_name(v);
                const char *error_name = ast_error_name(n)[0] ? ast_error_name(n) : ast_error_name(v);
                VarSymbol *sum = ensure_sum(cg, n->name, sum_type, payload, error,
                                             payload_name, error_name);
                if (v->type == AST_NONE_LITERAL) {
                    fprintf(cg->out, "    mov QWORD PTR [rbp-%d], 0\n    lea rdi, [rbp-%d]\n", sum->tag_offset, sum->payload_offset);
                    emit_zero_memory(cg, "rdi", sum->payload_size);
                    if (sum_type == COBRA_TYPE_RESULT) {
                        fprintf(cg->out, "    lea rdi, [rbp-%d]\n", sum->error_offset);
                        emit_zero_memory(cg, "rdi", sum->error_size);
                    }
                } else if (v->type == AST_FUNC_CALL &&
                    (!strcmp(v->name, "some") || !strcmp(v->name, "none") ||
                     !strcmp(v->name, "ok") || !strcmp(v->name, "err"))) {
                    fprintf(cg->out, "    lea rdx, [rbp-%d]\n", sum->tag_offset);
                    emit_sum_constructor(cg, v, "rdx", sum_type, payload, error,
                                         payload_name, error_name);
                } else if (v->type == AST_FUNC_CALL) {
                    emit_call(cg, v);
                    fprintf(cg->out, "    mov rsi, rax\n    lea rdi, [rbp-%d]\n", sum->tag_offset);
                    emit_sum_copy_ptr(cg, "rsi", "rdi", sum_type, sum->payload_size, sum->error_size);
                } else if (v->type == AST_VAR_REF) {
                    VarSymbol *source = find_symbol(cg, v->name);
                    if (!source || source->type != sum_type) {
                        fprintf(stderr, "CodeGen Error: sum assignment requires a matching Option or Result\n");
                        exit(EXIT_FAILURE);
                    }
                    fprintf(cg->out, "    lea rsi, [rbp-%d]\n    lea rdi, [rbp-%d]\n", source->tag_offset, sum->tag_offset);
                    emit_sum_copy_ptr(cg, "rsi", "rdi", sum_type, sum->payload_size, sum->error_size);
                } else {
                    fprintf(stderr, "CodeGen Error: Option and Result values require a constructor or matching value\n");
                    exit(EXIT_FAILURE);
                }
                return;
            }
            if (v->type == AST_DICT_LITERAL) {
                VarSymbol *s = ensure_dict(cg, n->name);
                s->owned = true;
                fprintf(cg->out, "    mov QWORD PTR [rbp-%d], 0\n    mov QWORD PTR [rbp-%d], 0\n", s->offset, s->length_offset);
                for (size_t i = 0; i < v->child_count; i++) {
                    ASTNode *entry = v->children[i];
                    if (entry->child_count == 1) emit_dict_set_key(cg, s, entry->name, entry->children[0]);
                }
                return;
            }
            if (v->type == AST_FUNC_CALL && !strcmp(v->name, "slice_u8")) {
                VarSymbol *s = ensure_slice(cg, n->name, COBRA_TYPE_SLICE_U8);
                s->borrowed = true;
                s->owned = false;
                int source_ptr = reserve(cg, 8);
                int source_len = reserve(cg, 8);
                int start = reserve(cg, 8);
                int length = reserve(cg, 8);
                int fail = cg->label_count++;
                emit_load_buffer_ptr(cg, v->children[0]->name, "rax");
                fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", source_ptr);
                emit_load_buffer_len(cg, v->children[0]->name, "rax");
                fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", source_len);
                emit_expr(cg, v->children[1]);
                fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", start);
                emit_expr(cg, v->children[2]);
                fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", length);
                fprintf(cg->out, "    cmp QWORD PTR [rbp-%d], 0\n    jl .Lslice_u8_fail_%d\n", start, fail);
                fprintf(cg->out, "    cmp QWORD PTR [rbp-%d], 0\n    jl .Lslice_u8_fail_%d\n", length, fail);
                fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    cmp rax, QWORD PTR [rbp-%d]\n    ja .Lslice_u8_fail_%d\n", start, source_len, fail);
                fprintf(cg->out, "    mov rdx, QWORD PTR [rbp-%d]\n    sub rdx, rax\n    cmp QWORD PTR [rbp-%d], rdx\n    ja .Lslice_u8_fail_%d\n", source_len, length, fail);
                fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    add rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n    jmp .Lslice_u8_done_%d\n.Lslice_u8_fail_%d:\n", source_ptr, start, s->offset, length, s->length_offset, fail, fail);
                emit_failure(cg, "byte slice bounds error");
                fprintf(cg->out, ".Lslice_u8_done_%d:\n", fail);
                return;
            }
            if (v->type == AST_FUNC_CALL && (!strcmp(v->name, "alloc_i64") ||
                                             !strcmp(v->name, "alloc_f32") ||
                                             !strcmp(v->name, "alloc_u8"))) {
                CobraTypeKind t = !strcmp(v->name, "alloc_f32") ? COBRA_TYPE_SLICE_F32 :
                                  (!strcmp(v->name, "alloc_u8") ? COBRA_TYPE_SLICE_U8 : COBRA_TYPE_SLICE);
                VarSymbol *s = n->declared_type == COBRA_TYPE_TENSOR_F32 ? ensure_tensor(cg, n->name) : ensure_slice(cg, n->name, t);
                if (is_region_alloc(cg, v)) emit_call(cg, v); /* bump: rax=ptr, rdx=len */
                else emit_alloc(cg, v, t == COBRA_TYPE_SLICE ? 8 :
                                (t == COBRA_TYPE_SLICE_F32 ? 4 : 1));
                fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n    mov QWORD PTR [rbp-%d], rdx\n", s->offset, s->length_offset);
                if (s->kind == SYM_TENSOR) { s->owned = true; s->contiguous = true; emit_tensor_metadata(cg, s, n); }
                return;
            }
            if (v->type == AST_FUNC_CALL && (!strcmp(v->name, "reshape_view") || !strcmp(v->name, "slice_view") || !strcmp(v->name, "transpose_view"))) { emit_view_init(cg, ensure_tensor(cg, n->name), v); return; }
            if (v->type == AST_FUNC_CALL) {
                ASTNode *callee = find_function(cg, v->name);
                if (callee && callee->declared_type == COBRA_TYPE_TENSOR_F32) {
                    VarSymbol *destination = ensure_tensor(cg, n->name);
                    emit_call(cg, v);
                    emit_copy_tensor_descriptor(cg, destination, "rax");
                    return;
                }
            }
            if (v->type == AST_COMPREHENSION) {
                /* The comprehension emit builds the list into the variable's
                   symbol directly (see AST_COMPREHENSION in emit_expr). */
                snprintf(v->comprehension_target, sizeof(v->comprehension_target), "%s", n->name);
                CobraTypeKind element = ast_element_kind(v) == COBRA_TYPE_UNTYPED ? COBRA_TYPE_I64 : ast_element_kind(v);
                ensure_list(cg, n->name, element);
                emit_expr(cg, v);
                return;
            }
            if (v->type == AST_ARRAY_LITERAL && n->declared_type == COBRA_TYPE_LIST) {
                CobraTypeKind element = ast_element_kind(n) == COBRA_TYPE_UNTYPED ? COBRA_TYPE_I64 : ast_element_kind(n);
                VarSymbol *s = ensure_list(cg, n->name, element);
                s->owned = true;
                fprintf(cg->out, "    mov QWORD PTR [rbp-%d], 0\n    mov QWORD PTR [rbp-%d], 0\n    mov QWORD PTR [rbp-%d], 0\n", s->offset, s->length_offset, s->capacity_offset);
                for (size_t i = 0; i < v->child_count; i++) emit_list_append(cg, s, v->children[i]);
                return;
            }
            if (v->type == AST_ARRAY_LITERAL) {
                CobraTypeKind element_type = ast_element_kind(n) != COBRA_TYPE_UNTYPED ?
                    ast_element_kind(n) : ast_element_kind(v);
                if (element_type == COBRA_TYPE_UNTYPED) element_type = COBRA_TYPE_I64;
                VarSymbol *s = ensure_array(cg, n->name, (int)v->child_count, element_type);
                for (size_t i = 0; i < v->child_count; i++) {
                    emit_expr(cg, v->children[i]);
                    int element_offset = s->array_base - (int)i *
                        (element_type == COBRA_TYPE_F32 ? 4 : 8);
                    if (element_type == COBRA_TYPE_F32)
                        fprintf(cg->out, "    movss DWORD PTR [rbp-%d], xmm0\n", element_offset);
                    else
                        fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", element_offset);
                }
                fprintf(cg->out, "    lea rax, [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", s->array_base, s->offset); return;
            }
            if (current_iter(cg, n->name) >= 0) {
                int it = current_iter(cg, n->name);
                if (cg->loops[it].source[0] != '\0') { emit_expr(cg, v); emit_load_buffer_ptr(cg, cg->loops[it].source, "rbx"); fprintf(cg->out, "    mov rdx, QWORD PTR [rbp-%d]\n", cg->loops[it].index_offset); if (cg->loops[it].element_type == COBRA_TYPE_F32) { if (!expression_is_float_codegen(cg, v)) fprintf(cg->out, "    cvtsi2ss xmm0, rax\n"); fprintf(cg->out, "    movss DWORD PTR [rbx + rdx*4], xmm0\n"); } else fprintf(cg->out, "    mov QWORD PTR [rbx + rdx*8], rax\n"); return; }
            }
            if (n->declared_type == COBRA_TYPE_FUNC && n->dyn_trait_name[0]) {
                /* `let x: dyn Trait = concrete_value` - ir.c's cobra_ir_build
                   already verified conformance; build the dispatch block
                   here the same way a dyn-typed call argument does. A
                   function-call initializer already returns a fully-built
                   dispatch block pointer (the callee's own `-> dyn Trait`
                   return codegen built it), so that case is just an ordinary
                   scalar move that tags the destination symbol as dyn. */
                VarSymbol *s = find_symbol(cg, n->name);
                if (!s) s = ensure_scalar(cg, n->name, COBRA_TYPE_FUNC);
                if (s->dyn_trait_name[0] == '\0')
                    snprintf(s->dyn_trait_name, sizeof(s->dyn_trait_name), "%.63s", n->dyn_trait_name);
                if (v->type == AST_FUNC_CALL) {
                    emit_expr(cg, v);
                    fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", s->offset);
                    return;
                }
                if (v->type != AST_VAR_REF) {
                    fprintf(stderr, "CodeGen Error: dyn %s initializer must be a named struct value or call\n", n->dyn_trait_name);
                    exit(EXIT_FAILURE);
                }
                VarSymbol *src = find_symbol(cg, v->name);
                if (!src || src->kind != SYM_STRUCT) {
                    fprintf(stderr, "CodeGen Error: '%s' is not a struct value coercible to dyn %s\n", v->name, n->dyn_trait_name);
                    exit(EXIT_FAILURE);
                }
                emit_build_dyn_dispatch_block(cg, n->dyn_trait_name, src, s->offset);
                return;
            }
            VarSymbol *s = find_symbol(cg, n->name);
            if (!s) {
                CobraTypeKind inferred = n->declared_type;
                if (inferred == COBRA_TYPE_UNTYPED && expression_is_float(v)) inferred = COBRA_TYPE_F32;
                if (inferred == COBRA_TYPE_UNTYPED && v->value_type == COBRA_TYPE_STRING) inferred = COBRA_TYPE_STRING;
                if (inferred == COBRA_TYPE_ARRAY && v->type == AST_ARRAY_LITERAL && v->child_count > 0)
                    inferred = COBRA_TYPE_ARRAY;
                s = ensure_scalar(cg, n->name, inferred);
            }
            if (s->type == COBRA_TYPE_STRING && v->fresh_string_result) s->owned = true;
            emit_expr(cg, v);
            if (expression_is_float_codegen(cg, v) || s->kind == SYM_F32) fprintf(cg->out, "    movss DWORD PTR [rbp-%d], xmm0\n", s->offset);
            else fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", s->offset);
            return;
        }        case AST_INDEX_ASSIGN:
            if (n->secondary_name[0] != '\0') emit_struct_field_index_store(cg, n, n->children[n->child_count - 1]);
            else emit_index_store(cg, n->name, n->children, n->child_count - 1, n->children[n->child_count - 1]);
            return;
        case AST_MEMBER_ASSIGN: {
            if (n->child_count != 2) {
                fprintf(stderr, "CodeGen Error: member assignment requires a struct value\n");
                exit(EXIT_FAILURE);
            }
            int field_address = reserve(cg, 8);
            emit_struct_address(cg, n->children[0]);
            fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", field_address);
            emit_expr(cg, n->children[1]);
            int offset = field_offset_for(cg, ast_payload_name(n->children[0]),
                                          n->secondary_name);
            CobraTypeKind field_type = n->declared_type;
            if (field_type == COBRA_TYPE_SLICE ||
                field_type == COBRA_TYPE_SLICE_F32 ||
                field_type == COBRA_TYPE_SLICE_U8) {
                int field_length = reserve(cg, 8);
                fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rdx\n    mov rdx, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rdx + %d], rax\n    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rdx + %d], rax\n",
                        field_length, field_address, offset, field_length, offset + 8);
            } else if (field_type == COBRA_TYPE_F32) {
                if (!expression_is_float_codegen(cg, n->children[1])) fprintf(cg->out, "    cvtsi2ss xmm0, rax\n");
                fprintf(cg->out, "    mov rdx, QWORD PTR [rbp-%d]\n    movss DWORD PTR [rdx + %d], xmm0\n",
                        field_address, offset);
            } else {
                fprintf(cg->out, "    mov rdx, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rdx + %d], rax\n",
                        field_address, offset);
            }
            return;
        }
        case AST_ENV_FIELD_STORE: {
            if (n->child_count != 2) {
                fprintf(stderr, "CodeGen Error: env field store requires a pointer and a value expression\n");
                exit(EXIT_FAILURE);
            }
            int ptr_temp = reserve(cg, 8);
            emit_expr(cg, n->children[0]);
            fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", ptr_temp);
            emit_expr(cg, n->children[1]);
            if (n->value_type == COBRA_TYPE_F32) {
                if (!expression_is_float_codegen(cg, n->children[1])) fprintf(cg->out, "    cvtsi2ss xmm0, rax\n");
                fprintf(cg->out, "    mov rdx, QWORD PTR [rbp-%d]\n    movss DWORD PTR [rdx + %d], xmm0\n",
                        ptr_temp, n->int_val);
            } else {
                fprintf(cg->out, "    mov rdx, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rdx + %d], rax\n",
                        ptr_temp, n->int_val);
            }
            return;
        }
        case AST_FUNC_CALL: emit_call(cg, n); return;
        case AST_RETURN: {
            const char *returned_name = NULL;
            bool has_result = n->child_count != 0;
            bool float_result = cg->current_return_type == COBRA_TYPE_F32;
            if (has_result && n->children[0]->type == AST_VAR_REF) returned_name = n->children[0]->name;
            if (cg->current_return_type == COBRA_TYPE_TENSOR_F32) {
                if (has_result) emit_tensor_return(cg, n->children[0]);
            } else if (cg->current_return_type == COBRA_TYPE_STRUCT) {
                if (has_result) emit_struct_return(cg, n->children[0]);
            } else if (cg->current_return_type == COBRA_TYPE_OPTION ||
                       cg->current_return_type == COBRA_TYPE_RESULT) {
                if (has_result) emit_sum_return(cg, n->children[0], cg->current_return_type);
            } else if (cg->current_return_dyn_trait_name[0] && has_result) {
                /* `-> dyn Trait`: ir.c already verified the returned value
                   implements the trait. Build the dispatch block into a
                   scratch slot and leave its pointer in rax, matching every
                   other scalar-return path below. */
                if (n->children[0]->type != AST_VAR_REF) {
                    fprintf(stderr, "CodeGen Error: dyn %s return value must be a named struct value\n", cg->current_return_dyn_trait_name);
                    exit(EXIT_FAILURE);
                }
                VarSymbol *src = find_symbol(cg, n->children[0]->name);
                if (!src || src->kind != SYM_STRUCT) {
                    fprintf(stderr, "CodeGen Error: '%s' is not a struct value coercible to dyn %s\n",
                            n->children[0]->name, cg->current_return_dyn_trait_name);
                    exit(EXIT_FAILURE);
                }
                int dest_slot = reserve(cg, 8);
                emit_build_dyn_dispatch_block_ex(cg, cg->current_return_dyn_trait_name, src, dest_slot, true);
                fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n", dest_slot);
            } else if (has_result) {
                emit_expr(cg, n->children[0]);
            }
            emit_cleanup_preserving_result(cg, returned_name, has_result, float_result);
            emit_return_epilogue(cg);
            return;
        }
        case AST_ASSERT_STMT: {
            int ok = cg->label_count++;
            ASTNode *condition = n->children[0];
            emit_expr(cg, condition);
            if (expression_is_float(condition)) {
                fprintf(cg->out, "    pxor xmm1, xmm1\n    ucomiss xmm0, xmm1\n    setne al\n    movzx eax, al\n");
            }
            fprintf(cg->out, "    cmp rax, 0\n    jne .Lassert_ok_%d\n", ok);
            emit_failure(cg, "assertion failed");
            fprintf(cg->out, ".Lassert_ok_%d:\n", ok);
            return;
        }
        case AST_PRINT_STMT:
            if (n->child_count) {
                emit_expr(cg, n->children[0]);
                if (n->children[0]->value_type == COBRA_TYPE_STRING) {
                    fprintf(cg->out, "    mov rsi, rax\n    lea rdi, [rip + .fmt_string]\n    xor eax, eax\n    call printf@PLT\n");
                } else {
                    fprintf(cg->out, "    mov rsi, rax\n    lea rdi, [rip + .fmt_int]\n    xor eax, eax\n    call printf@PLT\n");
                }
            }
            return;
        case AST_MATCH_STMT: {
            int target_slot = reserve(cg, 8);
            int end_label = cg->label_count++;
            int fallback_label = cg->label_count++;
            int case_labels[COBRA_MAX_ENUM_VARIANTS + 1] = {0};
            for (size_t i = 1; i < n->child_count && i <= COBRA_MAX_ENUM_VARIANTS; i++) {
                if (!n->children[i]->is_default_case) case_labels[i] = cg->label_count++;
            }
            emit_expr(cg, n->children[0]);
            fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", target_slot);
            for (size_t i = 1; i < n->child_count; i++) {
                ASTNode *arm = n->children[i];
                if (!arm->is_default_case)
                    fprintf(cg->out, "    cmp QWORD PTR [rbp-%d], %d\n    je .Lmatch_case_%d\n",
                            target_slot, arm->int_val, case_labels[i]);
            }
            fprintf(cg->out, "    jmp .Lmatch_fallback_%d\n", fallback_label);
            for (size_t i = 1; i < n->child_count; i++) {
                ASTNode *arm = n->children[i];
                if (arm->is_default_case) continue;
                fprintf(cg->out, ".Lmatch_case_%d:\n", case_labels[i]);
                if (arm->child_count > 0) emit_statement(cg, arm->children[0]);
                fprintf(cg->out, "    jmp .Lmatch_end_%d\n", end_label);
            }
            fprintf(cg->out, ".Lmatch_fallback_%d:\n", fallback_label);
            for (size_t i = 1; i < n->child_count; i++) {
                ASTNode *arm = n->children[i];
                if (arm->is_default_case && arm->child_count > 0) {
                    emit_statement(cg, arm->children[0]);
                    break;
                }
            }
            fprintf(cg->out, ".Lmatch_end_%d:\n", end_label);
            return;
        }
        case AST_IF_STMT: { int l = cg->label_count++; emit_expr(cg, n->children[0]); fprintf(cg->out, "    cmp rax, 0\n    je .Lelse_%d\n", l); if (n->child_count > 1) emit_statement(cg, n->children[1]); fprintf(cg->out, "    jmp .Lif_done_%d\n.Lelse_%d:\n", l, l); if (n->child_count > 2) emit_statement(cg, n->children[2]); fprintf(cg->out, ".Lif_done_%d:\n", l); return; }
        case AST_WHILE_STMT: { int l = cg->label_count++; fprintf(cg->out, ".Lwhile_%d:\n", l); emit_expr(cg, n->children[0]); fprintf(cg->out, "    cmp rax, 0\n    je .Lwhile_done_%d\n", l); if (n->child_count > 1) { emit_statement(cg, n->children[1]); emit_loop_owned_cleanup(cg, n->children[1]); } fprintf(cg->out, "    jmp .Lwhile_%d\n.Lwhile_done_%d:\n", l, l); return; }
        case AST_WITH_REGION: {
            if (n->child_count < 1) return;
            if (cg->region_depth >= 16) {
                fprintf(stderr, "CodeGen Error: too many nested regions\n"); exit(EXIT_FAILURE);
            }
            RegionInfo *r = &cg->regions[cg->region_depth++];
            memset(r, 0, sizeof(*r));
            r->active = true;
            snprintf(r->name, sizeof(r->name), "%.63s", n->name);
            /* Hidden state array: [base, cur, cap] as three i64 slots. */
            r->state_base = reserve(cg, 24);
            r->capacity_slot = reserve(cg, 8);
            int fail_neg = cg->label_count++;
            int fail_oom = cg->label_count++;
            int done = cg->label_count++;
            emit_expr(cg, n->children[0]);
            fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n    cmp rax, 0\n    jl .Lreg_neg_%d\n", r->capacity_slot, fail_neg);
            /* ArenaState is an out struct, so capacity is the second
               argument rather than a third slice-ABI slot. */
            fprintf(cg->out, "    lea rdi, [rbp-%d]\n    mov rsi, QWORD PTR [rbp-%d]\n    call arena_create@PLT\n    test rax, rax\n    je .Lreg_oom_%d\n", r->state_base, r->capacity_slot, fail_oom);
            if (n->child_count > 1) emit_statement(cg, n->children[1]);
            /* Exactly one release at scope exit. */
            fprintf(cg->out, "    lea rdi, [rbp-%d]\n    call arena_destroy@PLT\n    jmp .Lreg_done_%d\n", r->state_base, done);
            fprintf(cg->out, ".Lreg_neg_%d:\n", fail_neg);
            emit_failure(cg, "region capacity must be non-negative");
            fprintf(cg->out, ".Lreg_oom_%d:\n", fail_oom);
            emit_failure(cg, "region creation failed");
            fprintf(cg->out, ".Lreg_done_%d:\n", done);
            cg->region_depth--;
            return;
        }
        case AST_FOR_LOOP: emit_for(cg, n); return;
        case AST_COMPUTE_BLOCK: if (n->child_count) emit_statement(cg, n->children[0]); return;
        case AST_PARALLEL_BLOCK:
            if (try_emit_parallel(cg, n)) return;
            if (n->child_count) emit_statement(cg, n->children[0]);
            return;
        case AST_ASM_BLOCK: {
            static const char *const arg_regs[6] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
            if (n->asm_input_count > 0) fprintf(cg->out, "    # inline asm inputs\n");
            for (int i = 0; i < n->asm_input_count; i++) {
                VarSymbol *s = find_symbol(cg, n->asm_inputs[i]);
                if (!s) {
                    fprintf(stderr, "CodeGen Error: asm input '%s' is not a declared local\n", n->asm_inputs[i]);
                    exit(EXIT_FAILURE);
                }
                fprintf(cg->out, "    mov %s, QWORD PTR [rbp-%d]\n", arg_regs[i], s->offset);
            }
            fprintf(cg->out, "    # inline asm\n");
            emit_inline_asm(cg, n->asm_code);
            if (n->asm_has_output) {
                VarSymbol *s = ensure_scalar(cg, n->asm_output, COBRA_TYPE_I64);
                fprintf(cg->out, "    # inline asm output\n    mov QWORD PTR [rbp-%d], rax\n", s->offset);
            }
            return;
        }
        default: return;
    }
}

static void emit_function(CodeGen *cg, ASTNode *fn) {
    cg->symbol_count = 0; cg->stack_offset = COBRA_LOCAL_BASE; cg->loop_depth = 0;
    compute_safe_autofree_structs(cg, fn);
    cg->current_return_type = fn->declared_type;
    cg->current_return_payload_type = ast_element_kind(fn);
    cg->current_return_error_type = ast_error_kind(fn);
    snprintf(cg->current_return_type_name, sizeof(cg->current_return_type_name), "%.63s", ast_payload_name(fn));
    snprintf(cg->current_return_error_type_name, sizeof(cg->current_return_error_type_name), "%.63s", ast_error_name(fn));
    snprintf(cg->current_return_dyn_trait_name, sizeof(cg->current_return_dyn_trait_name), "%.63s", fn->dyn_trait_name);
    cg->propagation_label = cg->label_count++;
    bool tensor_return = fn->declared_type == COBRA_TYPE_TENSOR_F32;
    bool sum_return = fn->declared_type == COBRA_TYPE_OPTION || fn->declared_type == COBRA_TYPE_RESULT;
    bool struct_return = fn->declared_type == COBRA_TYPE_STRUCT;
    bool compound_return = tensor_return || sum_return || struct_return;
    fprintf(cg->out, "    .intel_syntax noprefix\n");
    /* The native test runner calls test_* functions from a separate C
       translation unit. They remain externally visible only in test mode;
       ordinary private functions keep local linkage. */
    bool test_entry = cg->test_mode && strncmp(fn->name, "test_", 5) == 0;
    if (fn->has_visibility && !fn->is_public && !test_entry) {
        fprintf(cg->out, "    .local %s\n", fn->name);
    } else {
        fprintf(cg->out, "    .global %s\n", fn->name);
    }
    fprintf(cg->out, "    .type %s, @function\n%s:\n    push rbp\n    mov rbp, rsp\n    sub rsp, %d\n    mov QWORD PTR [rbp-248], rbx\n", fn->name, fn->name, COBRA_FRAME_BYTES);
    /* Snapshot the six incoming GPR arguments before descriptor copying can
       use rsi/rdx as metadata scratch. */
    for (int arg = 0; arg < 6; arg++)
        fprintf(cg->out, "    mov QWORD PTR [rbp-%d], %s\n", COBRA_ARG_SAVE_BASE + arg * 8, SYSV_REGS[arg]);
    if (compound_return) fprintf(cg->out, "    mov rdi, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-240], rdi\n", COBRA_ARG_SAVE_BASE);

    int gpr = compound_return ? 1 : 0, xmm = 0, stack_index = 0;
    size_t parameter_index = 0;
    for (size_t i = 0; i < fn->child_count; i++) {
        ASTNode *p = fn->children[i]; if (p->type != AST_PARAM) continue;
        CobraTypeKind parameter_type = function_param_type(cg, fn->name, parameter_index++);
        if (parameter_type == COBRA_TYPE_SLICE || parameter_type == COBRA_TYPE_SLICE_F32 ||
            parameter_type == COBRA_TYPE_SLICE_U8) {
            VarSymbol *s = ensure_slice(cg, p->name, parameter_type);
            s->qualifier = param_alias_contract(p);
            int slots = abi_slots_for(parameter_type, p);
            if (gpr + slots <= 6) {
                fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", COBRA_ARG_SAVE_BASE + gpr * 8, s->offset);
                fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", COBRA_ARG_SAVE_BASE + (gpr + 1) * 8, s->length_offset);
                gpr += slots;
            } else {
                fprintf(cg->out, "    mov rax, QWORD PTR [rbp+%d]\n    mov QWORD PTR [rbp-%d], rax\n", 16 + stack_index++ * 8, s->offset);
                fprintf(cg->out, "    mov rax, QWORD PTR [rbp+%d]\n    mov QWORD PTR [rbp-%d], rax\n", 16 + stack_index++ * 8, s->length_offset);
            }
        } else if (parameter_type == COBRA_TYPE_LIST) {
            VarSymbol *s = ensure_list(cg, p->name, ast_element_kind(p));
            int slots = abi_slots_for(parameter_type, p);
            if (gpr + slots <= 6) {
                fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", COBRA_ARG_SAVE_BASE + gpr * 8, s->offset);
                fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", COBRA_ARG_SAVE_BASE + (gpr + 1) * 8, s->length_offset);
                fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", COBRA_ARG_SAVE_BASE + (gpr + 2) * 8, s->capacity_offset);
                gpr += slots;
            } else {
                for (int k = 0; k < 3; k++) fprintf(cg->out, "    mov rax, QWORD PTR [rbp+%d]\n    mov QWORD PTR [rbp-%d], rax\n", 16 + (stack_index++) * 8, k == 0 ? s->offset : (k == 1 ? s->length_offset : s->capacity_offset));
            }
        } else if (parameter_type == COBRA_TYPE_DICT) {
            VarSymbol *s = ensure_dict(cg, p->name);
            int slots = abi_slots_for(parameter_type, p);
            if (gpr + slots <= 6) {
                fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", COBRA_ARG_SAVE_BASE + gpr * 8, s->offset);
                fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", COBRA_ARG_SAVE_BASE + (gpr + 1) * 8, s->length_offset);
                gpr += slots;
            } else {
                for (int k = 0; k < 2; k++) fprintf(cg->out, "    mov rax, QWORD PTR [rbp+%d]\n    mov QWORD PTR [rbp-%d], rax\n", 16 + (stack_index++) * 8, k == 0 ? s->offset : s->length_offset);
            }
        } else if (parameter_type == COBRA_TYPE_TENSOR_F32) {
            VarSymbol *s = ensure_tensor(cg, p->name);
            int incoming = reserve(cg, 8);
            if (gpr < 6) {
                fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", COBRA_ARG_SAVE_BASE + gpr * 8, incoming);
                gpr++;
            } else {
                fprintf(cg->out, "    mov rax, QWORD PTR [rbp+%d]\n    mov QWORD PTR [rbp-%d], rax\n", 16 + stack_index++ * 8, incoming);
            }
            /* Copy through a separate incoming-pointer slot. Keeping the
               pointer outside the descriptor prevents field 0's data-pointer
               store from aliasing the source pointer during the copy. */
            fprintf(cg->out, "    mov rsi, QWORD PTR [rbp-%d]\n", incoming);
            emit_copy_tensor_descriptor(cg, s, "rsi");
        } else if (parameter_type == COBRA_TYPE_OPTION || parameter_type == COBRA_TYPE_RESULT) {
            VarSymbol *s = ensure_sum(cg, p->name, parameter_type,
                                      ast_element_kind(p), ast_error_kind(p),
                                      ast_payload_name(p), ast_error_name(p));
            int incoming = reserve(cg, 8);
            if (gpr < 6) {
                fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n",
                        COBRA_ARG_SAVE_BASE + gpr * 8, incoming);
                gpr++;
            } else {
                fprintf(cg->out, "    mov rax, QWORD PTR [rbp+%d]\n    mov QWORD PTR [rbp-%d], rax\n",
                        16 + stack_index++ * 8, incoming);
            }
            fprintf(cg->out, "    mov rsi, QWORD PTR [rbp-%d]\n    lea rdi, [rbp-%d]\n",
                    incoming, s->tag_offset);
            emit_sum_copy_ptr(cg, "rsi", "rdi", parameter_type,
                              s->payload_size, s->error_size);
        } else if (parameter_type == COBRA_TYPE_STRUCT) {
            /* Struct parameters use one pointer ABI slot. Ordinary parameters
               copy scalar bytes into private frame storage. An `out` parameter
               keeps the caller's pointer in one frame slot, so field writes
               update the caller without a heap object or boxed reference. */
            VarSymbol *s = ensure_struct(cg, p->name, ast_payload_name(p));
            s->qualifier = param_alias_contract(p);
            /* `out` always aliases. Otherwise, alias only when the whole-body
               scan proves this parameter is never mutated or let to escape -
               see compute_param_borrowed; this must stay a pure superset of
               `out`'s existing correctness, never a substitute for it. */
            s->indirect = param_alias_contract(p) == 2 ||
                          compute_param_borrowed(cg, fn, p->name);
            int incoming = reserve(cg, 8);
            if (gpr < 6) {
                fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n",
                        COBRA_ARG_SAVE_BASE + gpr * 8, incoming);
                gpr++;
            } else {
                fprintf(cg->out, "    mov rax, QWORD PTR [rbp+%d]\n    mov QWORD PTR [rbp-%d], rax\n",
                        16 + stack_index++ * 8, incoming);
            }
            if (s->indirect) {
                fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n",
                        incoming, s->array_base);
            } else {
                fprintf(cg->out, "    mov rsi, QWORD PTR [rbp-%d]\n    lea rdi, [rbp-%d]\n",
                        incoming, s->array_base);
                emit_copy_memory(cg, "rsi", "rdi", struct_storage_size(cg, ast_payload_name(p)));
            }
        } else if (parameter_type == COBRA_TYPE_F32 || parameter_type == COBRA_TYPE_F64) {
            VarSymbol *s = ensure_scalar(cg, p->name, parameter_type);
            if (xmm < 8) fprintf(cg->out, "    movss DWORD PTR [rbp-%d], %s\n", s->offset, SYSV_XMM_REGS[xmm++]);
            else fprintf(cg->out, "    movss xmm15, DWORD PTR [rbp+%d]\n    movss DWORD PTR [rbp-%d], xmm15\n", 16 + stack_index++ * 8, s->offset);
        } else {
            VarSymbol *s = ensure_scalar(cg, p->name, parameter_type);
            if (p->dyn_trait_name[0]) snprintf(s->dyn_trait_name, sizeof(s->dyn_trait_name), "%.63s", p->dyn_trait_name);
            if (gpr < 6) {
                fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n    mov QWORD PTR [rbp-%d], rax\n", COBRA_ARG_SAVE_BASE + gpr * 8, s->offset);
                gpr++;
            } else {
                fprintf(cg->out, "    mov rax, QWORD PTR [rbp+%d]\n    mov QWORD PTR [rbp-%d], rax\n", 16 + stack_index++ * 8, s->offset);
            }
        }
    }
    bool returned = false;
    for (size_t i = 0; i < fn->child_count; i++) if (fn->children[i]->type != AST_PARAM) { emit_statement(cg, fn->children[i]); if (fn->children[i]->type == AST_RETURN) returned = true; }
    if (!returned) {
        fprintf(cg->out, "    xor eax, eax\n");
    }
    fprintf(cg->out, ".Lpropagate_%d:\n", cg->propagation_label);
    fprintf(cg->out, "    mov QWORD PTR [rbp-%d], rax\n", COBRA_SCR_TMP);
    emit_scope_cleanup(cg, NULL);
    fprintf(cg->out, "    mov rax, QWORD PTR [rbp-%d]\n", COBRA_SCR_TMP);
    emit_return_epilogue(cg);
    /* @parallel workers are emitted after the caller's body, so the caller
       never falls through into a worker prologue. */
    flush_pending_parallel(cg);
    flush_pending_fn_thunks(cg);
}

static bool codegen_is_nn_function(const ASTNode *node) {
    return node && node->type == AST_FUNCTION &&
           strstr(node->source_file, "lib/nn.cb") != NULL;
}

static ASTNode *codegen_find_function(ASTNode *root, const char *name) {
    if (!root || !name) return NULL;
    for (size_t i = 0; i < root->child_count; i++) {
        ASTNode *node = root->children[i];
        if (node->type == AST_FUNCTION && strcmp(node->name, name) == 0) return node;
    }
    return NULL;
}

static bool codegen_function_reaches_candidate(ASTNode *root, ASTNode *function,
                                               ASTNode *candidate, ASTNode **stack,
                                               size_t depth);

static bool codegen_node_reaches_candidate(ASTNode *root, ASTNode *node,
                                           ASTNode *candidate, ASTNode **stack,
                                           size_t depth) {
    if (!node) return false;
    if (node->type == AST_FUNC_CALL) {
        ASTNode *callee = codegen_find_function(root, node->name);
        if (callee == candidate) return true;
        if (callee && codegen_function_reaches_candidate(root, callee, candidate,
                                                         stack, depth)) return true;
    }
    for (size_t i = 0; i < node->child_count; i++) {
        if (codegen_node_reaches_candidate(root, node->children[i], candidate,
                                           stack, depth)) return true;
    }
    return false;
}

static bool codegen_function_reaches_candidate(ASTNode *root, ASTNode *function,
                                               ASTNode *candidate, ASTNode **stack,
                                               size_t depth) {
    if (!function) return false;
    if (function == candidate) return true;
    /* A very deep call graph is kept in the output conservatively. */
    if (depth >= 128) return true;
    for (size_t i = 0; i < depth; i++) {
        if (stack[i] == function) return false;
    }
    stack[depth] = function;
    for (size_t i = 0; i < function->child_count; i++) {
        if (codegen_node_reaches_candidate(root, function->children[i], candidate,
                                           stack, depth + 1)) return true;
    }
    return false;
}

static bool codegen_nn_function_is_reachable(ASTNode *root, ASTNode *candidate) {
    if (!root || !candidate) return false;
    for (size_t i = 0; i < root->child_count; i++) {
        ASTNode *root_function = root->children[i];
        if (root_function->type != AST_FUNCTION || codegen_is_nn_function(root_function)) continue;
        ASTNode *stack[128] = {0};
        if (codegen_function_reaches_candidate(root, root_function, candidate, stack, 0)) return true;
    }
    return false;
}

static bool emit_target_stub(ASTNode *root, const char *path, TargetPlatform target) {
    FILE *f = fopen(path, "w"); if (!f) return false;
    if (target == TARGET_MACOS_ARM64) fprintf(f, "// Cobra ARM64 target placeholder\n.global _main\n_main:\n mov x0, #0\n ret\n"); else fprintf(f, ";; Cobra WASM target placeholder\n(module)\n");
    fclose(f); (void)root; return true;
}

static bool generate(ASTNode *root, const char *path, TargetPlatform target, bool test_mode) {
    if (target != TARGET_LINUX_X86_64) return emit_target_stub(root, path, target);
    FILE *f = fopen(path, "w"); if (!f) return false;
    CodeGen cg; memset(&cg, 0, sizeof(cg)); cg.out = f; cg.root = root; cg.target = target; cg.test_mode = test_mode; cg.opt_vectorize = g_opt_vectorize;
    for (size_t i = 0; i < root->child_count; i++) {
        ASTNode *decl = root->children[i];
        if (decl->type != AST_IMPORT_DECL) continue;
        for (size_t j = 0; j < decl->child_count; j++) {
            ASTNode *ref = decl->children[j];
            if (ref->type != AST_VAR_REF) continue;
            if (cg.imported_function_count >= 256) {
                fprintf(stderr, "CodeGen Error: imported function table exhausted\n");
                fclose(f);
                return false;
            }
            snprintf(cg.imported_functions[cg.imported_function_count++],
                     COBRA_MAX_IDENT_LEN, "%s", ref->name);
        }
    }
    fputs("# Cobra direct native assembly output", f);
    fputc(10, f);
    fputs(".section .rodata", f);
    fputc(10, f);
    fputs(".fmt_int:", f);
    fputc(10, f);
    fputs(".string \"%ld", f);
    fputc(92, f);
    fputc('n', f);
    fputc(34, f);
    fputc(10, f);
    fputs(".fmt_string:", f);
    fputc(10, f);
    fputs(".string \"%s", f);
    fputc(92, f);
    fputc('n', f);
    fputc(34, f);
    fputc(10, f);
    fputs(".text", f);
    fputc(10, f);
    for (size_t i = 0; i < root->child_count; i++) { ASTNode *fn = root->children[i];        if (fn->type == AST_FUNCTION && fn->generic_param_count == 0 &&
            fn->target_device != TARGET_DEV_GPU_KERNEL &&
            !(test_mode && !strncmp(fn->name, "main", 4)) &&
            (!g_portable_cpu || !codegen_is_nn_function(fn) ||
             codegen_nn_function_is_reachable(root, fn)))
            emit_function(&cg, fn); }
    fclose(f); return true;
}

bool codegen_generate_assembly(ASTNode *root, const char *output_asm_path, TargetPlatform target) { return generate(root, output_asm_path, target, false); }
bool codegen_generate_test_assembly(ASTNode *root, const char *output_asm_path, TargetPlatform target) { return generate(root, output_asm_path, target, true); }
