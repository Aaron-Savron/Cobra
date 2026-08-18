/*
 * Cobra Backend IR: dedicated SSA construction pass.
 *
 * Consumes the typed HIR (source-level mutable locals, explicit CFG) and
 * produces flat block-argument SSA. The pass is a separate phase: the HIR
 * builder and the parser never construct SSA. See docs/BACKEND_IR.md.
 *
 * The algorithm is a liveness-based two-pass construction without phi nodes:
 *
 *   1. per-block read-before-assign and assign sets (straight-line blocks);
 *   2. a backward liveness fixpoint computing live-in locals per block;
 *   3. direct SSA clients may represent live-ins as block parameters, but
 *      source locals in this lowering are materialized as typed stack slots;
 *   4. CFG edges in the memory-backed lowering carry no local values because
 *      loads and stores preserve the mutable-local semantics across blocks.
 */
#include "ssa.h"
#include <stdarg.h>

typedef struct {
    BackendIrModule *module;
    HirFunction *fn;
    size_t local_count;
    bool *reads_before_assign; /* [block * locals + local] */
    bool *assigns;
    bool *live_in;
    SsaValueRef *block_defs;   /* current SSA value per (block, local) */
    bool *block_def_known;
    SsaValueRef *param_values; /* block param value per (block, local) */
    bool *param_created;
    SsaValueRef *local_ptrs;   /* canonical stack pointer per local       */
    SsaBlockRef base;          /* first arena block id for this function */
    SsaValueRef *param_refs;   /* lowered parameter refs (out)              */
    SsaValueRef return_storage;/* hidden pointer for aggregate returns       */
    size_t next_allocation_id; /* owned slice allocation identities          */
    size_t frame_used;         /* bytes consumed by slots for temp sums     */
} SsaPass;

/* Sum types (Option/Result with scalar components) are value-owned
   aggregates in this lane: they occupy canonical stack slots and travel by
   aggregate copy, exactly like value-owned scalar structs. */
static bool ssa_is_aggregate_value_type(const CobraType *type) {
    return type && (type->kind == COBRA_TYPE_STRUCT ||
                    type->kind == COBRA_TYPE_ARRAY || bir_is_sum_type(type));
}

static bool ssa_is_view_type(const CobraType *type) {
    return bir_is_borrowed_view_type(type);
}

static bool ssa_is_string_value_type(const CobraType *type) {
    return (bir_is_borrowed_view_type(type) &&
            cobra_type_element(type) &&
            cobra_type_element(type)->kind == COBRA_TYPE_U8) ||
           (bir_is_owned_slice_type(type) && type->kind == COBRA_TYPE_SLICE_U8);
}

/* Borrowed views and owned slices are both pointer-plus-length SSA values;
   neither is spilled into raw frame bytes. */
static bool ssa_is_slice_value_type(const CobraType *type) {
    /* Owned lists and dicts use the same SSA payload as owned slices:
       pointer, length, capacity, and an owned allocation identity. Keep
       them out of raw stack slots so generic collection parameters and
       returns preserve ownership. */
    return bir_is_borrowed_view_type(type) ||
           bir_is_owned_slice_type(type) ||
           bir_is_owned_buffer_type(type) ||
           bir_is_owned_dict_type(type);
}

static BirPointerContract ssa_view_contract(const CobraType *type) {
    if (bir_is_owned_slice_type(type) || bir_is_owned_dict_type(type))
        return BIR_POINTER_CONTRACT_OWNED_SLICE;
    return bir_view_is_writable(type)
        ? BIR_POINTER_CONTRACT_BORROW_WRITE
        : BIR_POINTER_CONTRACT_BORROW_READONLY;
}

static void ssa_fail(SsaPass *p, int line, int col, const char *fmt, ...) {
    if (!p->module || p->module->error[0]) return;
    char message[COBRA_MAX_TOKEN_TEXT];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    snprintf(p->module->error, sizeof(p->module->error),
             "%.32s:%d:%d: %.50s", p->module->source_file, line > 0 ? line : 1,
             col > 0 ? col : 1, message);
}

static void mark_expr_reads(SsaPass *p, size_t block, HirExpr *expr) {
    if (!expr) return;
    switch (expr->kind) {
        case HIR_EXPR_CONST:
            break;
        case HIR_EXPR_LOCAL:
            if (!p->assigns[block * p->local_count + expr->local]) {
                p->reads_before_assign[block * p->local_count + expr->local] = true;
            }
            break;
        case HIR_EXPR_BINOP:
        case HIR_EXPR_CALL:
        case HIR_EXPR_MEMBER:
        case HIR_EXPR_INDEX:
        case HIR_EXPR_LEN:
        case HIR_EXPR_SLICE:
        case HIR_EXPR_ALLOC:
        case HIR_EXPR_BORROW:
        case HIR_EXPR_SUM_MAKE:
        case HIR_EXPR_SUM_ACCESS:
        case HIR_EXPR_ARRAY_LITERAL:
        case HIR_EXPR_DICT_LITERAL:
        case HIR_EXPR_DICT_GET:
        case HIR_EXPR_DICT_HAS:
        case HIR_EXPR_DICT_POP:
        case HIR_EXPR_DICT_LEN:
        case HIR_EXPR_CAST:
            for (size_t i = 0; i < expr->arg_count; i++) {
                mark_expr_reads(p, block, expr->args[i]);
            }
            break;
        default:
            break;
    }
}

static void analyze_block(SsaPass *p, size_t block, HirBlock *hb) {
    const size_t locals = p->local_count;
    for (size_t i = 0; i < hb->stmt_count; i++) {
        HirStmt *stmt = &hb->stmts[i];
        switch (stmt->kind) {
            case HIR_STMT_ASSIGN:
                mark_expr_reads(p, block, stmt->expr);
                p->assigns[block * locals + stmt->local] = true;
                break;
            case HIR_STMT_MEMBER_ASSIGN:
            case HIR_STMT_INDEX_ASSIGN:
                mark_expr_reads(p, block, stmt->target);
                mark_expr_reads(p, block, stmt->expr);
                break;
            case HIR_STMT_DICT_SET:
            case HIR_STMT_DICT_DELETE:
                mark_expr_reads(p, block, stmt->expr);
                break;
            case HIR_STMT_EXPR:
            case HIR_STMT_PRINT:
            case HIR_STMT_ASSERT:
                mark_expr_reads(p, block, stmt->expr);
                break;
            default:
                break;
        }
    }
    switch (hb->term.kind) {
        case HIR_TERM_BRANCH:
            mark_expr_reads(p, block, hb->term.cond);
            break;
        case HIR_TERM_RETURN:
            mark_expr_reads(p, block, hb->term.ret_expr);
            break;
        default:
            break;
    }
}

static void compute_liveness(SsaPass *p) {
    const size_t locals = p->local_count;
    const size_t blocks = p->fn->block_count;
    for (size_t b = 0; b < blocks; b++) {
        for (size_t l = 0; l < locals; l++) {
            p->live_in[b * locals + l] = p->reads_before_assign[b * locals + l];
        }
    }
    bool changed = true;
    size_t iterations = 0;
    while (changed && iterations++ < blocks * locals + 1) {
        changed = false;
        for (size_t b = 0; b < blocks; b++) {
            HirBlock *hb = &p->fn->blocks[b];
            for (size_t s = 0; s < hb->succ_count; s++) {
                size_t succ = hb->succs[s];
                for (size_t l = 0; l < locals; l++) {
                    if (!p->assigns[b * locals + l] &&
                        p->live_in[succ * locals + l] &&
                        !p->live_in[b * locals + l]) {
                        p->live_in[b * locals + l] = true;
                        changed = true;
                    }
                }
            }
        }
    }
}

/* Create the arena blocks and every block parameter up front so edge
   arguments can reference successor parameters when predecessors are
   emitted in creation order. */
static bool create_blocks_and_params(SsaPass *p) {
    HirFunction *fn = p->fn;
    const size_t locals = p->local_count;
    SsaArena *arena = &p->module->arena;
    p->base = (SsaBlockRef)arena->block_count;

    for (size_t b = 0; b < fn->block_count; b++) {
        HirBlock *hb = &fn->blocks[b];
        SsaBlockRef ref = b == 0
            ? bir_add_entry_block(arena, hb->name, hb->source_line, hb->source_col)
            : bir_add_block(arena, hb->name, hb->source_line, hb->source_col);
        if (ref == SSA_BLOCK_NONE) {
            ssa_fail(p, hb->source_line, hb->source_col, "out of memory creating blocks");
            return false;
        }
    }

    /* Locals are now materialized in frame memory, so control-flow edges do
       not carry local SSA values. Keep the block-argument machinery available
       for direct SSA clients, but do not create redundant parameters here:
       every block reads the current local value through its typed stack slot.
       This also makes mutable-local semantics explicit instead of pretending
       that a memory-backed local is an SSA value. */

    /* Function parameters are SSA values defined at the entry block. An
       aggregate return receives an explicit hidden pointer first; user
       parameters follow it in source order. */
    size_t ssa_param_index = 0;
    if (fn->return_type && ssa_is_aggregate_value_type(fn->return_type)) {
        const CobraType *return_pointer = bir_pointer_type(p->module, fn->return_type);
        SsaValueRef param = bir_add_value(arena, SSA_VALUE_PARAM,
                                          return_pointer,
                                          fn->blocks[0].source_line,
                                          fn->blocks[0].source_col);
        if (param == SSA_VALUE_NONE || !return_pointer) {
            ssa_fail(p, fn->blocks[0].source_line, fn->blocks[0].source_col,
                     "out of memory creating aggregate return storage parameter");
            return false;
        }
        arena->values[param].param_index = (uint32_t)ssa_param_index;
        arena->values[param].pointer_contract = BIR_POINTER_CONTRACT_CALLER_STORAGE;
        arena->values[param].pointer_origin = BIR_POINTER_ORIGIN_CALLER;
        if (p->param_refs) p->param_refs[ssa_param_index] = param;
        p->return_storage = param;
        ssa_param_index++;
    }
    size_t owned_parameter_ordinal = 0;
    size_t owning_sum_parameter_ordinal = 0;
    size_t owning_struct_parameter_ordinal = 0;
    size_t owned_slice_parameter_count = 0;
    for (size_t i = 0; i < fn->param_count; i++) {
        if (bir_is_owned_slice_type(fn->param_types[i]) ||
            bir_is_owned_dict_type(fn->param_types[i])) owned_slice_parameter_count++;
    }
    for (size_t i = 0; i < fn->param_count; i++, ssa_param_index++) {
        const CobraType *param_type = p->fn->param_types[i];
        if (param_type && ssa_is_aggregate_value_type(param_type))
            param_type = bir_pointer_type(p->module, param_type);
        SsaValueRef param = bir_add_value(arena, SSA_VALUE_PARAM,
                                          param_type,
                                          fn->locals[i].source_line,
                                          fn->locals[i].source_col);
        if (param == SSA_VALUE_NONE) {
            ssa_fail(p, fn->locals[i].source_line, fn->locals[i].source_col,
                     "out of memory creating function parameters");
            return false;
        }
        arena->values[param].param_index = (uint32_t)ssa_param_index;
        if (param_type && (param_type->kind == COBRA_TYPE_POINTER ||
                           ssa_is_slice_value_type(param_type))) {
            arena->values[param].pointer_contract = ssa_view_contract(param_type);
            arena->values[param].pointer_origin = BIR_POINTER_ORIGIN_CALLER;
            if (bir_is_owned_slice_type(param_type) ||
                bir_is_owned_dict_type(param_type)) {
                arena->values[param].allocation_id =
                    (uint32_t)(p->local_count + owned_parameter_ordinal + 1);
                owned_parameter_ordinal++;
            } else if (bir_is_sum_type(fn->param_types[i]) &&
                       bir_type_has_owned_payload(fn->param_types[i])) {
                arena->values[param].allocation_id =
                    (uint32_t)(p->local_count + owned_slice_parameter_count +
                               owning_sum_parameter_ordinal + 1);
                owning_sum_parameter_ordinal++;
            } else if (fn->param_types[i]->kind == COBRA_TYPE_STRUCT &&
                       bir_type_has_owned_payload(fn->param_types[i])) {
                /* An owning struct parameter carries nested payload handles
                   in its storage. Give it an allocation identity so the flow
                   analysis and evaluator can track transfer, extraction, and
                   destruction across the call boundary. */
                arena->values[param].allocation_id =
                    (uint32_t)(p->local_count + owned_slice_parameter_count +
                               owning_sum_parameter_ordinal +
                               owning_struct_parameter_ordinal + 1);
                owning_struct_parameter_ordinal++;
            }
        }
        if (p->param_refs) p->param_refs[ssa_param_index] = param;
        p->block_defs[0 * locals + i] = param;
        p->block_def_known[0 * locals + i] = true;
        /* Readonly view parameters are immutable SSA values rather than
           addressable local storage. They remain available in every CFG block;
           mutable scalar locals continue to use typed stack slots. */
        if (ssa_is_slice_value_type(param_type)) {
            for (size_t block = 1; block < fn->block_count; block++) {
                p->block_defs[block * locals + i] = param;
                p->block_def_known[block * locals + i] = true;
            }
        }
    }

    /* Every HIR local is addressable in the lowered representation. Allocate
       canonical, aligned frame slots before any body instructions are emitted,
       then seed parameter slots with their incoming SSA values. */
    p->local_ptrs = calloc(locals ? locals : 1, sizeof(SsaValueRef));
    if (!p->local_ptrs) {
        ssa_fail(p, fn->blocks[0].source_line, fn->blocks[0].source_col,
                 "out of memory creating local stack slots");
        return false;
    }
    size_t frame_offset = 0;
    p->frame_used = 0;
    for (size_t local = 0; local < locals; local++) {
        const CobraType *type = fn->locals[local].type;
        if (ssa_is_slice_value_type(type)) {
            /* A borrowed view or owned slice is a typed pointer-plus-length
               value. It is deliberately not spilled into raw frame bytes in
               this slice; its pointer provenance must remain attached to the
               SSA payload. */
            continue;
        }
        size_t alignment = type ? type->alignment : 0;
        size_t size = type ? type->size : 0;
        if (!type || !type->finalized || !alignment || !size ||
            (alignment & (alignment - 1)) != 0 || size > BIR_STACK_BYTES ||
            frame_offset > BIR_STACK_BYTES - size) {
            ssa_fail(p, fn->locals[local].source_line, fn->locals[local].source_col,
                     "local '%s' has an invalid stack layout", fn->locals[local].name);
            return false;
        }
        size_t remainder = frame_offset % alignment;
        if (remainder) frame_offset += alignment - remainder;
        if (frame_offset > BIR_STACK_BYTES - size ||
            local >= BIR_MAX_STACK_SLOTS) {
            ssa_fail(p, fn->locals[local].source_line, fn->locals[local].source_col,
                     "function stack frame is too large");
            return false;
        }
        const CobraType *pointer_type = bir_pointer_type(p->module, type);
        SsaInstRef slot = bir_add_stack_slot(arena, pointer_type, type,
                                             (int64_t)frame_offset,
                                             (uint32_t)alignment,
                                             (uint32_t)local,
                                             fn->locals[local].source_line,
                                             fn->locals[local].source_col);
        if (slot == SSA_INST_NONE ||
            !bir_block_add_inst(arena, p->base, slot)) {
            ssa_fail(p, fn->locals[local].source_line, fn->locals[local].source_col,
                     "could not allocate stack slot for local '%s'",
                     fn->locals[local].name);
            return false;
        }
        p->local_ptrs[local] = bir_inst_result(arena, slot,
                                                fn->locals[local].source_line,
                                                fn->locals[local].source_col);
        if (p->local_ptrs[local] == SSA_VALUE_NONE) return false;
        for (size_t parameter = 0; parameter < fn->param_count; parameter++) {
            if (parameter != local) continue;
            if (!p->param_refs) {
                ssa_fail(p, fn->locals[local].source_line, fn->locals[local].source_col,
                         "missing function parameter references");
                return false;
            }
            SsaValueRef parameter_value = p->param_refs[parameter +
                ((fn->return_type && ssa_is_aggregate_value_type(fn->return_type)) ? 1 : 0)];
            SsaInstRef store = ssa_is_aggregate_value_type(type)
                ? (bir_type_has_owned_payload(type)
                   ? (bir_is_sum_type(type)
                      ? bir_add_sum_move(arena, type, p->local_ptrs[local],
                                         parameter_value,
                                         fn->locals[local].source_line,
                                         fn->locals[local].source_col)
                      : bir_add_aggregate_move(arena, type, p->local_ptrs[local],
                                               parameter_value,
                                               fn->locals[local].source_line,
                                               fn->locals[local].source_col))
                   : bir_add_aggregate_copy(arena, type, p->local_ptrs[local],
                                            parameter_value,
                                            fn->locals[local].source_line,
                                            fn->locals[local].source_col))
                : bir_add_typed_store(arena, type, pointer_type,
                                      p->local_ptrs[local], parameter_value,
                                      (uint32_t)size, (uint32_t)alignment,
                                      fn->locals[local].source_line,
                                      fn->locals[local].source_col);
            if (store == SSA_INST_NONE || !bir_block_add_inst(arena, p->base, store)) {
                ssa_fail(p, fn->locals[local].source_line, fn->locals[local].source_col,
                         "could not initialize parameter local '%s'",
                         fn->locals[local].name);
                return false;
            }
        }
        frame_offset += size;
    }
    p->frame_used = frame_offset;
    return true;
}

static SsaValueRef ssa_eval_lvalue_ptr(SsaPass *p, size_t block, HirExpr *expr) {
    SsaArena *arena = &p->module->arena;
    if (!expr) return SSA_VALUE_NONE;
    if (expr->kind == HIR_EXPR_LOCAL) {
        if (expr->local >= p->local_count || !p->local_ptrs ||
            p->local_ptrs[expr->local] == SSA_VALUE_NONE ||
            !expr->type || !ssa_is_aggregate_value_type(expr->type)) {
            ssa_fail(p, expr->source_line, expr->source_col,
                     "expression is not an addressable aggregate local");
            return SSA_VALUE_NONE;
        }
        return p->local_ptrs[expr->local];
    }
    if (expr->kind != HIR_EXPR_MEMBER || expr->arg_count != 1) {
        ssa_fail(p, expr->source_line, expr->source_col,
                 "expression is not an addressable aggregate field");
        return SSA_VALUE_NONE;
    }
    SsaValueRef base = ssa_eval_lvalue_ptr(p, block, expr->args[0]);
    if (base == SSA_VALUE_NONE) return SSA_VALUE_NONE;
    const CobraType *pointer_type = bir_pointer_type(p->module, expr->type);
    SsaInstRef field = bir_add_field_addr(arena, pointer_type,
                                          expr->aggregate_type, expr->type,
                                          base, expr->field_offset,
                                          expr->source_line, expr->source_col);
    if (field == SSA_INST_NONE ||
        !bir_block_add_inst(arena, p->base + block, field)) return SSA_VALUE_NONE;
    return bir_inst_result(arena, field, expr->source_line, expr->source_col);
}

static SsaValueRef ssa_eval_expr(SsaPass *p, size_t block, HirExpr *expr);

static SsaValueRef ssa_materialize_buffer(SsaPass *p, size_t block, HirExpr *expr) {
    SsaArena *arena = &p->module->arena;
    if (!expr || !expr->type || !bir_is_owned_buffer_type(expr->type) ||
        expr->type->generic_arg_count != 1 ||
        p->next_allocation_id == 0 || p->next_allocation_id > BIR_MAX_STACK_SLOTS)
        return SSA_VALUE_NONE;
    const CobraType *element = cobra_type_element(expr->type);
    SsaValueRef length = bir_add_const(arena,
        bir_scalar_i64(p->module->type_i64, (int64_t)expr->arg_count),
        expr->source_line, expr->source_col);
    SsaInstRef alloc = bir_add_buffer_alloc(arena, expr->type, element, length,
                                            (uint32_t)p->next_allocation_id++,
                                            expr->source_line, expr->source_col);
    if (alloc == SSA_INST_NONE ||
        !bir_block_add_inst(arena, p->base + block, alloc)) return SSA_VALUE_NONE;
    SsaValueRef buffer = bir_inst_result(arena, alloc,
                                         expr->source_line, expr->source_col);
    const CobraType *pointer_type = bir_pointer_type(p->module, element);
    for (size_t i = 0; i < expr->arg_count; i++) {
        SsaInstRef base_ptr = bir_add_view_ptr(arena, pointer_type, element, buffer,
                                               expr->source_line, expr->source_col);
        if (base_ptr == SSA_INST_NONE ||
            !bir_block_add_inst(arena, p->base + block, base_ptr)) return SSA_VALUE_NONE;
        SsaValueRef base = bir_inst_result(arena, base_ptr,
                                           expr->source_line, expr->source_col);
        SsaValueRef offset = bir_add_const(arena,
            bir_scalar_i64(p->module->type_i64, (int64_t)i * (int64_t)element->size),
            expr->source_line, expr->source_col);
        SsaInstRef address = bir_add_ptr_add(arena, pointer_type, base, offset,
                                             expr->source_line, expr->source_col);
        if (address == SSA_INST_NONE ||
            !bir_block_add_inst(arena, p->base + block, address)) return SSA_VALUE_NONE;
        SsaValueRef value = ssa_eval_expr(p, block, expr->args[i]);
        if (value == SSA_VALUE_NONE) return SSA_VALUE_NONE;
        SsaValueRef element_ptr = bir_inst_result(arena, address,
                                                  expr->source_line,
                                                  expr->source_col);
        SsaInstRef store;
        if (ssa_is_aggregate_value_type(element)) {
            store = bir_type_has_owned_payload(element)
                ? (bir_is_sum_type(element)
                   ? bir_add_sum_move(arena, element, element_ptr, value,
                                      expr->source_line, expr->source_col)
                   : bir_add_aggregate_move(arena, element, element_ptr, value,
                                            expr->source_line, expr->source_col))
                : bir_add_aggregate_copy(arena, element, element_ptr, value,
                                         expr->source_line, expr->source_col);
        } else {
            store = bir_add_typed_store(arena, element, pointer_type,
                                        element_ptr, value, (uint32_t)element->size,
                                        (uint32_t)element->alignment,
                                        expr->source_line, expr->source_col);
        }
        if (store == SSA_INST_NONE ||
            !bir_block_add_inst(arena, p->base + block, store)) return SSA_VALUE_NONE;
    }
    return buffer;
}

/* Materialize a dict literal as one allocation plus one set per entry.
   Each set carries its literal key and threads the growing table, exactly
   like production codegen emits `let d = {...}` entry by entry. */
static SsaValueRef ssa_materialize_dict(SsaPass *p, size_t block, HirExpr *expr) {
    SsaArena *arena = &p->module->arena;
    if (!expr || !expr->type || !bir_is_owned_dict_type(expr->type) ||
        expr->type->generic_arg_count != 2 ||
        p->next_allocation_id == 0 || p->next_allocation_id > BIR_MAX_STACK_SLOTS)
        return SSA_VALUE_NONE;
    const CobraType *value_type = expr->type->generic_args[1];
    SsaValueRef capacity = bir_add_const(arena,
        bir_scalar_i64(p->module->type_i64, (int64_t)expr->arg_count),
        expr->source_line, expr->source_col);
    SsaInstRef alloc = bir_add_dict_alloc(arena, expr->type, value_type, capacity,
                                          (uint32_t)p->next_allocation_id++,
                                          expr->source_line, expr->source_col);
    if (alloc == SSA_INST_NONE ||
        !bir_block_add_inst(arena, p->base + block, alloc)) return SSA_VALUE_NONE;
    SsaValueRef dict = bir_inst_result(arena, alloc,
                                       expr->source_line, expr->source_col);
    for (size_t i = 0; i < expr->arg_count; i++) {
        SsaValueRef value = ssa_eval_expr(p, block, expr->args[i]);
        if (value == SSA_VALUE_NONE || p->next_allocation_id == 0 ||
            p->next_allocation_id > BIR_MAX_STACK_SLOTS) return SSA_VALUE_NONE;
        SsaInstRef set = bir_add_dict_set(arena, expr->type, value_type, dict,
                                          value, expr->dict_keys[i],
                                          (uint32_t)p->next_allocation_id++,
                                          expr->source_line, expr->source_col);
        if (set == SSA_INST_NONE ||
            !bir_block_add_inst(arena, p->base + block, set)) return SSA_VALUE_NONE;
        dict = bir_inst_result(arena, set, expr->source_line, expr->source_col);
    }
    return dict;
}

/* Allocate a private temp aggregate for a sum constructor used as a
   call argument. Slots are emitted in the current block; the frame cursor
   advances past locals so temps never overlap the addressable locals. */
static SsaValueRef ssa_alloc_temp_sum(SsaPass *p, size_t block, const CobraType *type,
                                      int line, int col) {
    SsaArena *arena = &p->module->arena;
    size_t alignment = type->alignment;
    size_t size = type->size;
    size_t remainder = p->frame_used % alignment;
    if (remainder) p->frame_used += alignment - remainder;
    if (p->frame_used > BIR_STACK_BYTES - size ||
        p->next_allocation_id > BIR_MAX_STACK_SLOTS) {
        ssa_fail(p, line, col, "function stack frame is too large");
        return SSA_VALUE_NONE;
    }
    uint32_t slot_id = (uint32_t)(p->next_allocation_id - 1);
    SsaInstRef slot = bir_add_stack_slot(arena, bir_pointer_type(p->module, type),
                                         type, (int64_t)p->frame_used,
                                         (uint32_t)alignment, slot_id, line, col);
    p->frame_used += size;
    p->next_allocation_id++;
    if (slot == SSA_INST_NONE || !bir_block_add_inst(arena, p->base + block, slot))
        return SSA_VALUE_NONE;
    return bir_inst_result(arena, slot, line, col);
}

/* Materialize an Option/Result constructor into an aggregate destination
   slot: store the tag, then the chosen payload or error component. */
static bool ssa_materialize_sum(SsaPass *p, size_t block, HirExpr *expr,
                                SsaValueRef destination) {
    SsaArena *arena = &p->module->arena;
    const CobraType *sum = expr->type;
    if (!sum || !bir_is_sum_type(sum) || destination == SSA_VALUE_NONE)
        return false;
    const CobraType *tag_type = p->module->type_i64;
    const CobraType *tag_pointer_type = bir_pointer_type(p->module, tag_type);
    SsaInstRef tag_field = bir_add_field_addr(arena, tag_pointer_type, sum,
                                              tag_type, destination, 0,
                                              expr->source_line, expr->source_col);
    if (tag_field == SSA_INST_NONE ||
        !bir_block_add_inst(arena, p->base + block, tag_field)) return false;
    SsaValueRef tag_ptr = bir_inst_result(arena, tag_field,
                                          expr->source_line, expr->source_col);
    int64_t tag = 0;
    int selector = 1;
    if (sum->kind == COBRA_TYPE_ENUM) {
        /* Payload enum: the tag is the variant discriminant and the selector
           is the 1-based variant index carried in sum_selector. */
        const BirEnumInfo *info = bir_find_enum(p->module, sum->name);
        if (!info || expr->sum_selector < 1 ||
            (size_t)expr->sum_selector > info->variant_count) {
            ssa_fail(p, expr->source_line, expr->source_col,
                     "enum constructor has an invalid variant index");
            return false;
        }
        tag = info->variant_values[expr->sum_selector - 1];
        selector = expr->sum_selector;
    } else {
        /* Tag: some/ok carry 1; none/err carry 0. */
        tag = (expr->sum_variant == 1 || expr->sum_variant == 2) ? 1 : 0;
        selector = expr->sum_variant == 3 ? 2 : 1;
    }
    SsaValueRef tag_value = bir_add_const(arena,
        bir_scalar_i64(tag_type, tag),
        expr->source_line, expr->source_col);
    SsaInstRef tag_store = bir_add_typed_store(arena, tag_type, tag_pointer_type,
                                               tag_ptr, tag_value,
                                               (uint32_t)tag_type->size,
                                               (uint32_t)tag_type->alignment,
                                               expr->source_line, expr->source_col);
    if (tag_store == SSA_INST_NONE ||
        !bir_block_add_inst(arena, p->base + block, tag_store)) return false;
    if (expr->arg_count == 1) {
        SsaValueRef component = ssa_eval_expr(p, block, expr->args[0]);
        if (component == SSA_VALUE_NONE) return false;
        const CobraType *component_type = expr->args[0]->type;
        if (!component_type) return false;
        size_t offset = bir_sum_component_offset(sum, selector);
        const CobraType *component_pointer_type =
            bir_pointer_type(p->module, component_type);
        SsaInstRef field = bir_add_field_addr(arena, component_pointer_type, sum,
                                              component_type, destination,
                                              (int64_t)offset,
                                              expr->source_line, expr->source_col);
        if (field == SSA_INST_NONE ||
            !bir_block_add_inst(arena, p->base + block, field)) return false;
        SsaValueRef field_ptr = bir_inst_result(arena, field,
                                                expr->source_line, expr->source_col);
        SsaInstRef store;
        if (bir_is_owned_slice_type(component_type)) {
            store = bir_add_sum_payload_store(arena, sum, component_type,
                                              destination, component, offset,
                                              expr->source_line, expr->source_col);
        } else if (ssa_is_aggregate_value_type(component_type)) {
            /* Nested owning sums and owning structs must move their
               recursive payloads. A raw aggregate copy would duplicate the
               ownership handle. */
            store = bir_type_has_owned_payload(component_type)
                ? (bir_is_sum_type(component_type)
                   ? bir_add_sum_move(arena, component_type, field_ptr, component,
                                      expr->source_line, expr->source_col)
                   : bir_add_aggregate_move(arena, component_type, field_ptr,
                                            component,
                                            expr->source_line, expr->source_col))
                : bir_add_aggregate_copy(arena, component_type, field_ptr,
                                         component,
                                         expr->source_line, expr->source_col);
        } else {
            store = bir_add_typed_store(arena, component_type,
                                        component_pointer_type, field_ptr,
                                        component,
                                        (uint32_t)component_type->size,
                                        (uint32_t)component_type->alignment,
                                        expr->source_line, expr->source_col);
        }
        if (store == SSA_INST_NONE ||
            !bir_block_add_inst(arena, p->base + block, store)) return false;
    }
    return true;
}

static SsaValueRef ssa_eval_view_index_pointer(SsaPass *p, size_t block,
                                                HirExpr *expr) {
    SsaArena *arena = &p->module->arena;
    if (!expr || expr->local >= p->local_count || !expr->aggregate_type) {
        ssa_fail(p, expr ? expr->source_line : 0, expr ? expr->source_col : 0,
                 "invalid indexed aggregate");
        return SSA_VALUE_NONE;
    }
    if (expr->aggregate_type->kind == COBRA_TYPE_ARRAY) {
        if (!p->local_ptrs || p->local_ptrs[expr->local] == SSA_VALUE_NONE ||
            expr->aggregate_type->generic_arg_count != 1) {
            ssa_fail(p, expr->source_line, expr->source_col,
                     "fixed array local has no stack address");
            return SSA_VALUE_NONE;
        }
        SsaValueRef index = ssa_eval_expr(p, block, expr->args[0]);
        if (index == SSA_VALUE_NONE) return SSA_VALUE_NONE;
        const CobraType *element = expr->aggregate_type->generic_args[0];
        const CobraType *pointer_type = bir_pointer_type(p->module, element);
        SsaInstRef address = bir_add_array_index_addr(
            arena, pointer_type, expr->aggregate_type, element,
            p->local_ptrs[expr->local], index,
            expr->source_line, expr->source_col);
        if (address == SSA_INST_NONE ||
            !bir_block_add_inst(arena, p->base + block, address)) return SSA_VALUE_NONE;
        return bir_inst_result(arena, address, expr->source_line, expr->source_col);
    }
    if (!ssa_is_slice_value_type(expr->aggregate_type)) {
        ssa_fail(p, expr ? expr->source_line : 0, expr ? expr->source_col : 0,
                 "invalid borrowed view or owned slice index");
        return SSA_VALUE_NONE;
    }
    SsaValueRef view = p->block_defs[block * p->local_count + expr->local];
    if (view == SSA_VALUE_NONE) {
        ssa_fail(p, expr->source_line, expr->source_col,
                 "borrowed view is not initialized on this path");
        return SSA_VALUE_NONE;
    }
    const CobraType *element = cobra_type_element(expr->aggregate_type);
    const CobraType *pointer_type = bir_pointer_type(p->module, element);
    SsaInstRef view_ptr = bir_add_view_ptr(arena, pointer_type, element,
                                           view, expr->source_line,
                                           expr->source_col);
    if (view_ptr == SSA_INST_NONE ||
        !bir_block_add_inst(arena, p->base + block, view_ptr)) return SSA_VALUE_NONE;
    SsaValueRef base = bir_inst_result(arena, view_ptr,
                                       expr->source_line, expr->source_col);
    SsaValueRef index_value = ssa_eval_expr(p, block, expr->args[0]);
    if (index_value == SSA_VALUE_NONE || !element) return SSA_VALUE_NONE;
    SsaValueRef width = bir_add_const(arena,
        bir_scalar_i64(p->module->type_i64, (int64_t)element->size),
        expr->source_line, expr->source_col);
    const SsaValueRef mul_ops[2] = {index_value, width};
    SsaInstRef scale = bir_add_inst(arena, SSA_OP_MUL, p->module->type_i64,
                                    mul_ops, 2, expr->source_line,
                                    expr->source_col);
    if (scale == SSA_INST_NONE ||
        !bir_block_add_inst(arena, p->base + block, scale)) return SSA_VALUE_NONE;
    SsaValueRef byte_offset = bir_inst_result(arena, scale,
                                               expr->source_line,
                                               expr->source_col);    SsaInstRef add = bir_add_ptr_add(arena, pointer_type, base, byte_offset,
                                     expr->source_line, expr->source_col);
    if (add == SSA_INST_NONE ||
        !bir_block_add_inst(arena, p->base + block, add)) return SSA_VALUE_NONE;
    arena->insts[add].view_source = view;
    return bir_inst_result(arena, add,
 expr->source_line, expr->source_col);
}

static bool ssa_materialize_array(SsaPass *p, size_t block, HirExpr *expr,
                                   SsaValueRef destination) {
    SsaArena *arena = &p->module->arena;
    if (!expr || expr->kind != HIR_EXPR_ARRAY_LITERAL || !expr->type ||
        expr->type->kind != COBRA_TYPE_ARRAY || expr->type->generic_arg_count != 1 ||
        destination == SSA_VALUE_NONE) return false;
    const CobraType *element = expr->type->generic_args[0];
    for (size_t i = 0; i < expr->arg_count; i++) {
        SsaValueRef index = bir_add_const(arena,
            bir_scalar_i64(p->module->type_i64, (int64_t)i),
            expr->source_line, expr->source_col);
        SsaInstRef address = bir_add_array_index_addr(
            arena, bir_pointer_type(p->module, element), expr->type, element,
            destination, index, expr->source_line, expr->source_col);
        if (address == SSA_INST_NONE ||
            !bir_block_add_inst(arena, p->base + block, address)) return false;
        SsaValueRef element_pointer = bir_inst_result(arena, address,
                                                      expr->source_line,
                                                      expr->source_col);
        SsaValueRef value = ssa_eval_expr(p, block, expr->args[i]);
        if (value == SSA_VALUE_NONE) return false;
        SsaInstRef store;
        if (ssa_is_aggregate_value_type(element)) {
            /* Nested fixed arrays are value-owned aggregates: each element
               travels by aggregate copy from its materialized temp slot. */
            store = bir_type_has_owned_payload(element)
                ? (bir_is_sum_type(element)
                   ? bir_add_sum_move(arena, element, element_pointer, value,
                                      expr->source_line, expr->source_col)
                   : bir_add_aggregate_move(arena, element, element_pointer, value,
                                            expr->source_line, expr->source_col))
                : bir_add_aggregate_copy(arena, element, element_pointer, value,
                                         expr->source_line, expr->source_col);
        } else {
            store = bir_add_typed_store(
                arena, element, bir_pointer_type(p->module, element), element_pointer,
                value, (uint32_t)element->size, (uint32_t)element->alignment,
                expr->source_line, expr->source_col);
        }
        if (store == SSA_INST_NONE ||
            !bir_block_add_inst(arena, p->base + block, store)) return false;
    }
    return true;
}

static SsaValueRef ssa_eval_expr(SsaPass *p, size_t block, HirExpr *expr) {
    SsaArena *arena = &p->module->arena;
    switch (expr->kind) {
        case HIR_EXPR_CONST:
            return bir_add_const(arena, expr->const_value,
                                 expr->source_line, expr->source_col);
        case HIR_EXPR_ARRAY_LITERAL: {
            if (expr->type && expr->type->kind == COBRA_TYPE_LIST)
                return ssa_materialize_buffer(p, block, expr);
            SsaValueRef temp = ssa_alloc_temp_sum(p, block, expr->type,
                                                  expr->source_line, expr->source_col);
            if (temp == SSA_VALUE_NONE || !ssa_materialize_array(p, block, expr, temp))
                return SSA_VALUE_NONE;
            return temp;
        }
        case HIR_EXPR_LOCAL: {
            if (ssa_is_slice_value_type(expr->type)) {
                size_t index = block * p->local_count + expr->local;
                if (expr->local >= p->local_count || !p->block_def_known[index] ||
                    p->block_defs[index] == SSA_VALUE_NONE) {
                    ssa_fail(p, expr->source_line, expr->source_col,
                             "slice local '%s' is not initialized on this path",
                             expr->local < p->fn->local_count
                                 ? p->fn->locals[expr->local].name : "<invalid>");
                    return SSA_VALUE_NONE;
                }
                return p->block_defs[index];
            }
            if (expr->type && ssa_is_aggregate_value_type(expr->type))
                return ssa_eval_lvalue_ptr(p, block, expr);
            if (expr->local >= p->local_count || !p->local_ptrs ||
                p->local_ptrs[expr->local] == SSA_VALUE_NONE) {
                ssa_fail(p, expr->source_line, expr->source_col,
                         "internal error: local '%s' has no stack address",
                         expr->local < p->fn->local_count
                             ? p->fn->locals[expr->local].name : "<invalid>");
                return SSA_VALUE_NONE;
            }
            SsaValueRef pointer = p->local_ptrs[expr->local];
            const CobraType *pointer_type = arena->values[pointer].type;
            SsaInstRef load = bir_add_typed_load(arena, expr->type, pointer_type,
                                                 pointer,
                                                 (uint32_t)expr->type->size,
                                                 (uint32_t)expr->type->alignment,
                                                 expr->source_line, expr->source_col);
            if (load == SSA_INST_NONE || !bir_block_add_inst(arena, p->base + block, load))
                return SSA_VALUE_NONE;
            return bir_inst_result(arena, load, expr->source_line, expr->source_col);
        }
        case HIR_EXPR_SLICE: {
            SsaValueRef source_view = ssa_eval_expr(p, block, expr->args[0]);
            SsaValueRef start = ssa_eval_expr(p, block, expr->args[1]);
            SsaValueRef length = ssa_eval_expr(p, block, expr->args[2]);
            if (source_view == SSA_VALUE_NONE || start == SSA_VALUE_NONE ||
                length == SSA_VALUE_NONE) return SSA_VALUE_NONE;
            const CobraType *element = cobra_type_element(expr->type);
            const CobraType *pointer_type = bir_pointer_type(p->module, element);
            SsaInstRef base_ptr = bir_add_view_ptr(arena, pointer_type, element,
                                                   source_view, expr->source_line,
                                                   expr->source_col);
            if (base_ptr == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, base_ptr)) return SSA_VALUE_NONE;
            SsaValueRef base = bir_inst_result(arena, base_ptr,
                                               expr->source_line, expr->source_col);
            SsaValueRef width = bir_add_const(arena,
                bir_scalar_i64(p->module->type_i64, (int64_t)element->size),
                expr->source_line, expr->source_col);
            const SsaValueRef scale_ops[2] = {start, width};
            SsaInstRef scale = bir_add_inst(arena, SSA_OP_MUL, p->module->type_i64,
                                            scale_ops, 2, expr->source_line,
                                            expr->source_col);
            if (scale == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, scale)) return SSA_VALUE_NONE;
            SsaValueRef byte_offset = bir_inst_result(arena, scale,
                                                       expr->source_line,
                                                       expr->source_col);
            SsaInstRef add = bir_add_ptr_add(arena, pointer_type, base, byte_offset,
                                             expr->source_line, expr->source_col);
            if (add == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, add)) return SSA_VALUE_NONE;
            arena->insts[add].view_source = source_view;
            SsaValueRef sub_pointer = bir_inst_result(arena, add,
                                                       expr->source_line,
                                                       expr->source_col);
            SsaInstRef make = bir_add_view_make(arena, expr->type, element,
                                                sub_pointer, length,
                                                expr->source_line, expr->source_col);
            if (make == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, make)) return SSA_VALUE_NONE;
            arena->insts[make].view_source = source_view;
            return bir_inst_result(arena, make, expr->source_line, expr->source_col);
        }
        case HIR_EXPR_INDEX: {
            if (expr->aggregate_type && expr->aggregate_type->kind == COBRA_TYPE_ARRAY) {
                SsaValueRef element_pointer = ssa_eval_view_index_pointer(p, block, expr);
                if (element_pointer == SSA_VALUE_NONE ||
                    expr->aggregate_type->generic_arg_count != 1) return SSA_VALUE_NONE;
                const CobraType *element = expr->aggregate_type->generic_args[0];
                if (ssa_is_aggregate_value_type(element)) {
                    /* Reading a nested-array element yields a whole value:
                       copy the element bytes into a fresh aggregate temp. */
                    SsaValueRef temp = ssa_alloc_temp_sum(p, block, element,
                                                          expr->source_line,
                                                          expr->source_col);
                    if (temp == SSA_VALUE_NONE) return SSA_VALUE_NONE;
                    SsaInstRef copy = bir_add_aggregate_copy(
                        arena, element, temp, element_pointer,
                        expr->source_line, expr->source_col);
                    if (copy == SSA_INST_NONE ||
                        !bir_block_add_inst(arena, p->base + block, copy))
                        return SSA_VALUE_NONE;
                    return temp;
                }
                SsaInstRef load = bir_add_typed_load(
                    arena, element, bir_pointer_type(p->module, element),
                    element_pointer, (uint32_t)element->size,
                    (uint32_t)element->alignment,
                    expr->source_line, expr->source_col);
                if (load == SSA_INST_NONE ||
                    !bir_block_add_inst(arena, p->base + block, load)) return SSA_VALUE_NONE;
                return bir_inst_result(arena, load, expr->source_line, expr->source_col);
            }
            if (expr->local >= p->local_count || !expr->aggregate_type ||
                !ssa_is_slice_value_type(expr->aggregate_type)) {
                ssa_fail(p, expr->source_line, expr->source_col,
                         "invalid readonly view or owned slice index");
                return SSA_VALUE_NONE;
            }
            SsaValueRef view = p->block_defs[block * p->local_count + expr->local];
            if (view == SSA_VALUE_NONE) {
                ssa_fail(p, expr->source_line, expr->source_col,
                         "readonly view is not initialized on this path");
                return SSA_VALUE_NONE;
            }
            const CobraType *element = cobra_type_element(expr->aggregate_type);
            const CobraType *pointer_type = bir_pointer_type(p->module, element);
            SsaInstRef view_ptr = bir_add_view_ptr(arena, pointer_type, element,
                                                   view, expr->source_line,
                                                   expr->source_col);
            if (view_ptr == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, view_ptr)) return SSA_VALUE_NONE;
            SsaValueRef base = bir_inst_result(arena, view_ptr,
                                               expr->source_line, expr->source_col);
            SsaValueRef index_value = ssa_eval_expr(p, block, expr->args[0]);
            if (index_value == SSA_VALUE_NONE) return SSA_VALUE_NONE;
            SsaValueRef width = bir_add_const(arena,
                bir_scalar_i64(p->module->type_i64, (int64_t)element->size),
                expr->source_line, expr->source_col);
            const SsaValueRef mul_ops[2] = {index_value, width};
            SsaInstRef scale = bir_add_inst(arena, SSA_OP_MUL, p->module->type_i64,
                                            mul_ops, 2, expr->source_line,
                                            expr->source_col);
            if (scale == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, scale)) return SSA_VALUE_NONE;
            SsaValueRef byte_offset = bir_inst_result(arena, scale,
                                                       expr->source_line,
                                                       expr->source_col);
            SsaInstRef add = bir_add_ptr_add(arena, pointer_type, base, byte_offset,
                                             expr->source_line, expr->source_col);
            if (add == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, add)) return SSA_VALUE_NONE;
            arena->insts[add].view_source = view;
            SsaValueRef element_pointer = bir_inst_result(arena, add,
                                                           expr->source_line,
                                                           expr->source_col);
            if (ssa_is_aggregate_value_type(element)) {
                /* Reading a whole aggregate element from a view or owned
                   buffer copies it into a fresh aggregate temp. */
                SsaValueRef temp = ssa_alloc_temp_sum(p, block, element,
                                                      expr->source_line,
                                                      expr->source_col);
                if (temp == SSA_VALUE_NONE) return SSA_VALUE_NONE;
                SsaInstRef copy = bir_add_aggregate_copy(
                    arena, element, temp, element_pointer,
                    expr->source_line, expr->source_col);
                if (copy == SSA_INST_NONE ||
                    !bir_block_add_inst(arena, p->base + block, copy))
                    return SSA_VALUE_NONE;
                arena->insts[copy].view_source = view;
                return temp;
            }
            SsaInstRef load = bir_add_typed_load(arena, element, pointer_type,
                                                 element_pointer,
                                                 (uint32_t)element->size,
                                                 (uint32_t)element->alignment,
                                                 expr->source_line, expr->source_col);
            if (load == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, load)) return SSA_VALUE_NONE;
            arena->insts[load].view_source = view;
            return bir_inst_result(arena, load, expr->source_line, expr->source_col);
        }
        case HIR_EXPR_LEN: {
            if (expr->arg_count == 1 && expr->args[0] && expr->args[0]->type &&
                expr->args[0]->type->kind == COBRA_TYPE_ARRAY) {
                return bir_add_const(arena,
                    bir_scalar_i64(p->module->type_i64,
                                   (int64_t)expr->args[0]->type->array_length),
                    expr->source_line, expr->source_col);
            }
            SsaValueRef view = ssa_eval_expr(p, block, expr->args[0]);
            if (view == SSA_VALUE_NONE) return SSA_VALUE_NONE;
            SsaInstRef length = bir_add_view_len(arena, p->module->type_i64,
                                                 arena->values[view].type, view,
                                                 expr->source_line, expr->source_col);
            if (length == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, length)) return SSA_VALUE_NONE;
            return bir_inst_result(arena, length, expr->source_line, expr->source_col);
        }
        case HIR_EXPR_ALLOC: {
            SsaValueRef length = ssa_eval_expr(p, block, expr->args[0]);
            if (length == SSA_VALUE_NONE) return SSA_VALUE_NONE;
            if (p->next_allocation_id == 0 ||
                p->next_allocation_id > BIR_MAX_STACK_SLOTS) {
                ssa_fail(p, expr->source_line, expr->source_col,
                         "too many owned slice allocations in one function");
                return SSA_VALUE_NONE;
            }
            uint32_t allocation = (uint32_t)p->next_allocation_id++;
            SsaInstRef alloc = expr->region_id != BIR_REGION_NONE
                ? bir_add_region_slice_alloc(arena, expr->type, expr->aggregate_type,
                                             length, allocation, expr->region_id,
                                             expr->source_line, expr->source_col)
                : bir_add_slice_alloc(arena, expr->type, expr->aggregate_type,
                                      length, allocation,
                                      expr->source_line, expr->source_col);
            if (alloc == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, alloc)) return SSA_VALUE_NONE;
            return bir_inst_result(arena, alloc, expr->source_line, expr->source_col);
        }
        case HIR_EXPR_BUFFER_APPEND: {
            SsaValueRef buffer = ssa_eval_expr(p, block, expr->args[0]);
            SsaValueRef value = ssa_eval_expr(p, block, expr->args[1]);
            if (buffer == SSA_VALUE_NONE || value == SSA_VALUE_NONE ||
                p->next_allocation_id == 0 ||
                p->next_allocation_id > BIR_MAX_STACK_SLOTS) return SSA_VALUE_NONE;
            SsaInstRef append = bir_add_buffer_append(
                arena, expr->type, cobra_type_element(expr->type), buffer, value,
                (uint32_t)p->next_allocation_id++,
                expr->source_line, expr->source_col);
            if (append == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, append)) return SSA_VALUE_NONE;
            SsaValueRef result = bir_inst_result(arena, append,
                                                 expr->source_line, expr->source_col);
            if (expr->local < p->local_count) {
                size_t index = block * p->local_count + expr->local;
                p->block_defs[index] = result;
                p->block_def_known[index] = true;
            }
            return result;
        }
        case HIR_EXPR_BUFFER_POP: {
            SsaValueRef buffer = ssa_eval_expr(p, block, expr->args[0]);
            if (buffer == SSA_VALUE_NONE) return SSA_VALUE_NONE;
            SsaInstRef pop = bir_add_buffer_pop(arena, expr->type, buffer,
                                                expr->source_line, expr->source_col);
            if (pop == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, pop)) return SSA_VALUE_NONE;
            return bir_inst_result(arena, pop, expr->source_line, expr->source_col);
        }
        case HIR_EXPR_DICT_LITERAL:
            return ssa_materialize_dict(p, block, expr);
        case HIR_EXPR_DICT_GET: {
            SsaValueRef dict = ssa_eval_expr(p, block, expr->args[0]);
            SsaValueRef fallback = ssa_eval_expr(p, block, expr->args[1]);
            if (dict == SSA_VALUE_NONE || fallback == SSA_VALUE_NONE)
                return SSA_VALUE_NONE;
            SsaInstRef get = bir_add_dict_get(arena, expr->type, dict, fallback,
                                              expr->dict_key, expr->source_line,
                                              expr->source_col);
            if (get == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, get)) return SSA_VALUE_NONE;
            return bir_inst_result(arena, get, expr->source_line, expr->source_col);
        }
        case HIR_EXPR_DICT_HAS: {
            SsaValueRef dict = ssa_eval_expr(p, block, expr->args[0]);
            if (dict == SSA_VALUE_NONE) return SSA_VALUE_NONE;
            SsaInstRef has = bir_add_dict_has(arena, p->module->type_i64, dict,
                                              expr->dict_key, expr->source_line,
                                              expr->source_col);
            if (has == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, has)) return SSA_VALUE_NONE;
            return bir_inst_result(arena, has, expr->source_line, expr->source_col);
        }
        case HIR_EXPR_DICT_POP: {
            SsaValueRef dict = ssa_eval_expr(p, block, expr->args[0]);
            SsaValueRef fallback = ssa_eval_expr(p, block, expr->args[1]);
            if (dict == SSA_VALUE_NONE || fallback == SSA_VALUE_NONE)
                return SSA_VALUE_NONE;
            SsaInstRef pop = bir_add_dict_pop(arena,
                                              arena->values[dict].type,
                                              expr->type,
                                              dict, fallback, expr->dict_key,
                                              expr->source_line, expr->source_col);
            if (pop == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, pop)) return SSA_VALUE_NONE;
            return bir_inst_result(arena, pop, expr->source_line, expr->source_col);
        }
        case HIR_EXPR_DICT_LEN: {
            SsaValueRef dict = ssa_eval_expr(p, block, expr->args[0]);
            if (dict == SSA_VALUE_NONE) return SSA_VALUE_NONE;
            SsaInstRef len = bir_add_dict_len(arena, p->module->type_i64, dict,
                                              expr->source_line, expr->source_col);
            if (len == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, len)) return SSA_VALUE_NONE;
            return bir_inst_result(arena, len, expr->source_line, expr->source_col);
        }
        case HIR_EXPR_BORROW: {
            const CobraType *element = expr->aggregate_type;
            const CobraType *pointer_type = bir_pointer_type(p->module, element);
            SsaValueRef base, length;
            /* A fixed array has no runtime slice descriptor to read a
               pointer/length pair out of - it's an inline stack aggregate,
               so the view is built directly from its address and its
               compile-time-known length instead of going through
               VIEW_PTR/VIEW_LEN (which expect an existing slice/view value). */
            if (expr->args[0]->type && expr->args[0]->type->kind == COBRA_TYPE_ARRAY) {
                SsaValueRef array_ptr = ssa_eval_lvalue_ptr(p, block, expr->args[0]);
                if (array_ptr == SSA_VALUE_NONE) return SSA_VALUE_NONE;
                /* array_ptr's pointee type is the array type itself; VIEW_MAKE
                   needs a pointer to the element type, so retype it through
                   the array-index-address instruction at index 0 (a no-op
                   address computation that just reinterprets the pointer,
                   carrying the frame provenance forward). */
                SsaValueRef zero = bir_add_const(arena,
                    bir_scalar_i64(p->module->type_i64, 0),
                    expr->source_line, expr->source_col);
                SsaInstRef element_addr = bir_add_array_index_addr(
                    arena, pointer_type, expr->args[0]->type, element,
                    array_ptr, zero, expr->source_line, expr->source_col);
                if (element_addr == SSA_INST_NONE ||
                    !bir_block_add_inst(arena, p->base + block, element_addr))
                    return SSA_VALUE_NONE;
                base = bir_inst_result(arena, element_addr,
                                       expr->source_line, expr->source_col);
                length = bir_add_const(arena,
                    bir_scalar_i64(p->module->type_i64,
                                   (int64_t)expr->args[0]->type->array_length),
                    expr->source_line, expr->source_col);
            } else {
                SsaValueRef source = ssa_eval_expr(p, block, expr->args[0]);
                if (source == SSA_VALUE_NONE) return SSA_VALUE_NONE;
                SsaInstRef view_ptr = bir_add_view_ptr(arena, pointer_type, element,
                                                       source, expr->source_line,
                                                       expr->source_col);
                if (view_ptr == SSA_INST_NONE ||
                    !bir_block_add_inst(arena, p->base + block, view_ptr))
                    return SSA_VALUE_NONE;
                base = bir_inst_result(arena, view_ptr,
                                       expr->source_line, expr->source_col);
                SsaInstRef len = bir_add_view_len(arena, p->module->type_i64,
                                                  arena->values[source].type, source,
                                                  expr->source_line, expr->source_col);
                if (len == SSA_INST_NONE ||
                    !bir_block_add_inst(arena, p->base + block, len)) return SSA_VALUE_NONE;
                length = bir_inst_result(arena, len,
                                         expr->source_line, expr->source_col);
            }
            SsaInstRef make = bir_add_view_make(arena, expr->type, element, base,
                                                length, expr->source_line,
                                                expr->source_col);
            if (make == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, make)) return SSA_VALUE_NONE;
            if (expr->transient_borrow) arena->insts[make].transient_borrow = true;
            return bir_inst_result(arena, make, expr->source_line, expr->source_col);
        }
        case HIR_EXPR_MEMBER: {
            if (bir_is_owned_slice_type(expr->type)) {
                if (!expr->args || expr->arg_count != 1) return SSA_VALUE_NONE;
                SsaValueRef aggregate = ssa_eval_lvalue_ptr(p, block, expr->args[0]);
                if (aggregate == SSA_VALUE_NONE ||
                    p->next_allocation_id == 0 ||
                    p->next_allocation_id > BIR_MAX_STACK_SLOTS) return SSA_VALUE_NONE;
                SsaInstRef load = bir_add_field_payload_load(
                    arena, expr->aggregate_type, expr->type, aggregate,
                    expr->field_offset, expr->source_line, expr->source_col);
                if (load == SSA_INST_NONE ||
                    !bir_block_add_inst(arena, p->base + block, load)) return SSA_VALUE_NONE;
                arena->insts[load].allocation_id = (uint32_t)p->next_allocation_id++;
                arena->insts[load].pointer_origin = BIR_POINTER_ORIGIN_FRAME;
                arena->insts[load].region_id = BIR_REGION_NONE;
                return bir_inst_result(arena, load, expr->source_line, expr->source_col);
            }
            SsaValueRef pointer = ssa_eval_lvalue_ptr(p, block, expr);
            if (pointer == SSA_VALUE_NONE) return SSA_VALUE_NONE;
            if (expr->type && ssa_is_aggregate_value_type(expr->type)) {
                SsaValueRef temp = ssa_alloc_temp_sum(p, block, expr->type,
                                                      expr->source_line, expr->source_col);
                if (temp == SSA_VALUE_NONE) return SSA_VALUE_NONE;
                SsaInstRef move;
                if (bir_type_has_owned_payload(expr->type)) {
                    move = bir_is_sum_type(expr->type)
                        ? bir_add_sum_move(arena, expr->type, temp, pointer,
                                           expr->source_line, expr->source_col)
                        : bir_add_aggregate_move(arena, expr->type, temp, pointer,
                                                 expr->source_line, expr->source_col);
                } else {
                    move = bir_add_aggregate_copy(arena, expr->type, temp, pointer,
                                                  expr->source_line, expr->source_col);
                }
                if (move == SSA_INST_NONE ||
                    !bir_block_add_inst(arena, p->base + block, move)) return SSA_VALUE_NONE;
                return temp;
            }
            const CobraType *pointer_type = arena->values[pointer].type;
            SsaInstRef load = bir_add_typed_load(arena, expr->type, pointer_type,
                                                 pointer,
                                                 (uint32_t)expr->type->size,
                                                 (uint32_t)expr->type->alignment,
                                                 expr->source_line, expr->source_col);
            if (load == SSA_INST_NONE || !bir_block_add_inst(arena, p->base + block, load))
                return SSA_VALUE_NONE;
            return bir_inst_result(arena, load, expr->source_line, expr->source_col);
        }
        case HIR_EXPR_SUM_MAKE: {
            if (!expr->type || !bir_is_sum_type(expr->type)) {
                ssa_fail(p, expr->source_line, expr->source_col,
                         "sum constructor has no resolved type");
                return SSA_VALUE_NONE;
            }
            SsaValueRef temp = ssa_alloc_temp_sum(p, block, expr->type,
                                                  expr->source_line, expr->source_col);
            if (temp == SSA_VALUE_NONE) return SSA_VALUE_NONE;
            if (!ssa_materialize_sum(p, block, expr, temp)) return SSA_VALUE_NONE;
            return temp;
        }
        case HIR_EXPR_FLOAT_LITERAL: {
            /* A literal that reached SSA without a boundary defaults to f32.
               Normal programs complete literals in the HIR builder. */
            BirScalarValue value = bir_scalar_f32(p->module->type_f32,
                                                  (float)expr->float_value);
            return bir_add_const(arena, value,
                                 expr->source_line, expr->source_col);
        }
        case HIR_EXPR_STR_CONCAT: {
            if (expr->arg_count != 2 ||
                !ssa_is_string_value_type(expr->args[0]->type) ||
                !ssa_is_string_value_type(expr->args[1]->type) ||
                p->next_allocation_id == 0 ||
                p->next_allocation_id > BIR_MAX_STACK_SLOTS) {
                ssa_fail(p, expr->source_line, expr->source_col,
                         "invalid owned string concatenation");
                return SSA_VALUE_NONE;
            }
            SsaValueRef left = ssa_eval_expr(p, block, expr->args[0]);
            SsaValueRef right = ssa_eval_expr(p, block, expr->args[1]);
            if (left == SSA_VALUE_NONE || right == SSA_VALUE_NONE) return SSA_VALUE_NONE;
            SsaInstRef concat = bir_add_string_concat(arena, expr->type, left, right,
                                                      expr->source_line,
                                                      expr->source_col);
            if (concat == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, concat))
                return SSA_VALUE_NONE;
            arena->insts[concat].memory_type = p->module->type_u8;
            arena->insts[concat].allocation_id =
                (uint32_t)p->next_allocation_id++;
            return bir_inst_result(arena, concat, expr->source_line,
                                   expr->source_col);
        }
        case HIR_EXPR_STR_EQ: {
            if (expr->arg_count != 2 ||
                !ssa_is_string_value_type(expr->args[0]->type) ||
                !ssa_is_string_value_type(expr->args[1]->type)) {
                ssa_fail(p, expr->source_line, expr->source_col,
                         "invalid string equality operands");
                return SSA_VALUE_NONE;
            }
            SsaValueRef left = ssa_eval_expr(p, block, expr->args[0]);
            SsaValueRef right = ssa_eval_expr(p, block, expr->args[1]);
            if (left == SSA_VALUE_NONE || right == SSA_VALUE_NONE) return SSA_VALUE_NONE;
            SsaInstRef eq = bir_add_string_eq(arena, p->module->type_bool,
                                              p->module->type_u8, left, right,
                                              expr->source_line, expr->source_col);
            if (eq == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, eq))
                return SSA_VALUE_NONE;
            SsaValueRef result = bir_inst_result(arena, eq, expr->source_line,
                                                 expr->source_col);
            if (!expr->sum_variant) return result;
            /* != negates the byte-equality result via `result != true`,
               since SSA_OP_NOT does not exist in this instruction set. */
            SsaValueRef true_const = bir_add_const(arena,
                bir_scalar_bool(p->module->type_bool, true),
                expr->source_line, expr->source_col);
            const SsaValueRef ne_operands[2] = {result, true_const};
            SsaInstRef ne = bir_add_inst(arena, SSA_OP_NE, p->module->type_bool,
                                         ne_operands, 2, expr->source_line,
                                         expr->source_col);
            if (ne == SSA_INST_NONE || !bir_block_add_inst(arena, p->base + block, ne))
                return SSA_VALUE_NONE;
            return bir_inst_result(arena, ne, expr->source_line, expr->source_col);
        }
        case HIR_EXPR_STR_LITERAL: {
            const CobraType *view = expr->type;
            const CobraType *u8 = p->module->type_u8;
            if (!view || !bir_is_borrowed_view_type(view) ||
                cobra_type_element(view)->kind != COBRA_TYPE_U8) {
                ssa_fail(p, expr->source_line, expr->source_col,
                         "string literal is not a readonly u8 view");
                return SSA_VALUE_NONE;
            }
            const size_t length = strlen(expr->literal);
            if (length > BIR_STACK_BYTES ||
                p->next_allocation_id > BIR_MAX_STACK_SLOTS) {
                ssa_fail(p, expr->source_line, expr->source_col,
                         "string literal exceeds the frame memory model");
                return SSA_VALUE_NONE;
            }
            /* Private frame allocation holding the literal bytes. */
            const CobraType *owned = bir_owned_slice_type(p->module, u8);
            SsaValueRef length_value = bir_add_const(arena,
                bir_scalar_i64(p->module->type_i64, (int64_t)length),
                expr->source_line, expr->source_col);
            SsaInstRef alloc = bir_add_slice_alloc(arena, owned, u8,
                                                   length_value,
                                                   (uint32_t)p->next_allocation_id++,
                                                   expr->source_line, expr->source_col);
            if (alloc == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, alloc)) return SSA_VALUE_NONE;
            SsaValueRef owned_view = bir_inst_result(arena, alloc,
                                                     expr->source_line, expr->source_col);
            const CobraType *pointer_type = bir_pointer_type(p->module, u8);
            SsaInstRef base_ptr = bir_add_view_ptr(arena, pointer_type, u8,
                                                   owned_view, expr->source_line,
                                                   expr->source_col);
            if (base_ptr == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, base_ptr)) return SSA_VALUE_NONE;
            SsaValueRef base = bir_inst_result(arena, base_ptr,
                                               expr->source_line, expr->source_col);
            for (size_t i = 0; i < length; i++) {
                /* u8 elements occupy canonical 8-byte slots in the memory
                   model, so the byte offset scales by the element size. */
                SsaValueRef offset_value = bir_add_const(arena,
                    bir_scalar_i64(p->module->type_i64,
                                   (int64_t)i * (int64_t)u8->size),
                    expr->source_line, expr->source_col);
                SsaInstRef byte_ptr = bir_add_ptr_add(arena, pointer_type,
                                                      base, offset_value,
                                                      expr->source_line,
                                                      expr->source_col);
                if (byte_ptr == SSA_INST_NONE ||
                    !bir_block_add_inst(arena, p->base + block, byte_ptr))
                    return SSA_VALUE_NONE;
                SsaValueRef byte_value = bir_add_const(arena,
                    bir_scalar_u8(u8, (uint8_t)expr->literal[i]),
                    expr->source_line, expr->source_col);
                SsaInstRef store = bir_add_typed_store(arena, u8, pointer_type,
                                                       bir_inst_result(arena, byte_ptr,
                                                           expr->source_line, expr->source_col),
                                                       byte_value,
                                                       (uint32_t)u8->size,
                                                       (uint32_t)u8->alignment,
                                                       expr->source_line, expr->source_col);
                if (store == SSA_INST_NONE ||
                    !bir_block_add_inst(arena, p->base + block, store)) return SSA_VALUE_NONE;
            }
            /* The readonly view over the literal bytes. */
            SsaInstRef make = bir_add_view_make(arena, view, u8, base, length_value,
                                                expr->source_line, expr->source_col);
            if (make == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, make)) return SSA_VALUE_NONE;
            return bir_inst_result(arena, make, expr->source_line, expr->source_col);
        }
        case HIR_EXPR_SUM_ACCESS: {
            if (expr->arg_count != 1 || !expr->aggregate_type ||
                !bir_is_sum_type(expr->aggregate_type)) {
                ssa_fail(p, expr->source_line, expr->source_col,
                         "invalid Option or Result access");
                return SSA_VALUE_NONE;
            }
            /* A predicate (`is_some`/`is_ok`) reads only the tag. When the
               argument is an addressable aggregate field or local, address it
               in place so the read does not move the payload out of the
               source aggregate. `unwrap` below still consumes. */
            SsaValueRef base;
            if (expr->args[0]->kind == HIR_EXPR_LOCAL ||
                expr->args[0]->kind == HIR_EXPR_MEMBER)
                base = ssa_eval_lvalue_ptr(p, block, expr->args[0]);
            else
                base = ssa_eval_expr(p, block, expr->args[0]);
            if (base == SSA_VALUE_NONE) return SSA_VALUE_NONE;
            const CobraType *sum = expr->aggregate_type;
            const CobraType *tag_type = p->module->type_i64;
            const CobraType *tag_pointer_type = bir_pointer_type(p->module, tag_type);
            SsaInstRef tag_field = bir_add_field_addr(arena, tag_pointer_type, sum,
                                                      tag_type, base, 0,
                                                      expr->source_line, expr->source_col);
            if (tag_field == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, tag_field)) return SSA_VALUE_NONE;
            SsaValueRef tag_ptr = bir_inst_result(arena, tag_field,
                                                  expr->source_line, expr->source_col);
            SsaInstRef tag_load = bir_add_typed_load(arena, tag_type, tag_pointer_type,
                                                     tag_ptr, (uint32_t)tag_type->size,
                                                     (uint32_t)tag_type->alignment,
                                                     expr->source_line, expr->source_col);
            if (tag_load == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, tag_load)) return SSA_VALUE_NONE;
            SsaValueRef tag_value = bir_inst_result(arena, tag_load,
                                                    expr->source_line, expr->source_col);
            if (expr->sum_selector == 0) {
                /* is_some/is_ok: tag == expected yields the predicate boolean.
                   sum_expected_tag defaults to 1; match arms set it to 0 for
                   none/err patterns. */
                SsaValueRef expected = bir_add_const(arena,
                    bir_scalar_i64(tag_type, expr->sum_expected_tag),
                    expr->source_line, expr->source_col);
                const SsaValueRef eq_ops[2] = {tag_value, expected};
                SsaInstRef eq = bir_add_inst(arena, SSA_OP_EQ, p->module->type_bool,
                                             eq_ops, 2, expr->source_line,
                                             expr->source_col);
                if (eq == SSA_INST_NONE ||
                    !bir_block_add_inst(arena, p->base + block, eq)) return SSA_VALUE_NONE;
                return bir_inst_result(arena, eq, expr->source_line, expr->source_col);
            }
            if (expr->sum_checked) {
                int check_kind = expr->sum_selector == 2 ? 2
                    : (sum->kind == COBRA_TYPE_OPTION ? 0 : 1);
                if (sum->kind == COBRA_TYPE_ENUM) check_kind = 3;
                SsaInstRef check = bir_add_sum_check(arena, tag_value, check_kind,
                                                     expr->source_line, expr->source_col);
                if (check == SSA_INST_NONE ||
                    !bir_block_add_inst(arena, p->base + block, check)) return SSA_VALUE_NONE;
                if (check_kind == 3)
                    arena->insts[check].sum_check_expected = expr->sum_expected_tag;
            }
            const CobraType *component_type = expr->type;
            if (!component_type) return SSA_VALUE_NONE;
            size_t offset = bir_sum_component_offset(sum, expr->sum_selector);
            if (bir_is_owned_slice_type(component_type)) {
                if (p->next_allocation_id == 0 ||
                    p->next_allocation_id > BIR_MAX_STACK_SLOTS) {
                    ssa_fail(p, expr->source_line, expr->source_col,
                             "too many owned payload values in one function");
                    return SSA_VALUE_NONE;
                }
                SsaInstRef load = bir_add_sum_payload_load(
                    arena, component_type, sum, base, offset,
                    expr->source_line, expr->source_col);
                if (load == SSA_INST_NONE ||
                    !bir_block_add_inst(arena, p->base + block, load))
                    return SSA_VALUE_NONE;
                arena->insts[load].allocation_id =
                    (uint32_t)p->next_allocation_id++;
                arena->insts[load].pointer_origin = BIR_POINTER_ORIGIN_FRAME;
                arena->insts[load].region_id = BIR_REGION_NONE;
                return bir_inst_result(arena, load, expr->source_line,
                                       expr->source_col);
            }
            const CobraType *component_pointer_type =
                bir_pointer_type(p->module, component_type);
            SsaInstRef field = bir_add_field_addr(arena, component_pointer_type, sum,
                                                  component_type, base,
                                                  (int64_t)offset,
                                                  expr->source_line, expr->source_col);
            if (field == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, field)) return SSA_VALUE_NONE;
            SsaValueRef field_ptr = bir_inst_result(arena, field,
                                                    expr->source_line, expr->source_col);
            if (ssa_is_aggregate_value_type(component_type)) {
                /* Extract nested owning sums by move. Scalar nested sums may
                   still use an ordinary value copy. */
                SsaValueRef temp = ssa_alloc_temp_sum(p, block, component_type,
                                                      expr->source_line,
                                                      expr->source_col);
                if (temp == SSA_VALUE_NONE) return SSA_VALUE_NONE;
                SsaInstRef copy = bir_type_has_owned_payload(component_type)
                    ? (bir_is_sum_type(component_type)
                       ? bir_add_sum_move(arena, component_type, temp, field_ptr,
                                          expr->source_line, expr->source_col)
                       : bir_add_aggregate_move(arena, component_type, temp, field_ptr,
                                                expr->source_line, expr->source_col))
                    : bir_add_aggregate_copy(arena, component_type,
                                             temp, field_ptr,
                                             expr->source_line,
                                             expr->source_col);
                if (copy == SSA_INST_NONE ||
                    !bir_block_add_inst(arena, p->base + block, copy))
                    return SSA_VALUE_NONE;
                if (bir_type_has_owned_payload(component_type)) {
                    /* The payload has moved out of the source sum. Clear its
                       tag so a second extraction sees `none` and fails; the
                       evaluator's sum_check enforces it. */
                    const CobraType *tag_type = p->module->type_i64;
                    const CobraType *tag_pointer_type =
                        bir_pointer_type(p->module, tag_type);
                    SsaInstRef tag_field = bir_add_field_addr(
                        arena, tag_pointer_type, sum, tag_type, base, 0,
                        expr->source_line, expr->source_col);
                    if (tag_field == SSA_INST_NONE ||
                        !bir_block_add_inst(arena, p->base + block, tag_field))
                        return SSA_VALUE_NONE;
                    SsaValueRef tag_ptr = bir_inst_result(
                        arena, tag_field, expr->source_line, expr->source_col);
                    SsaValueRef zero = bir_add_const(
                        arena, bir_scalar_i64(tag_type, 0),
                        expr->source_line, expr->source_col);
                    SsaInstRef tag_store = bir_add_typed_store(
                        arena, tag_type, tag_pointer_type, tag_ptr, zero,
                        (uint32_t)tag_type->size, (uint32_t)tag_type->alignment,
                        expr->source_line, expr->source_col);
                    if (tag_store == SSA_INST_NONE ||
                        !bir_block_add_inst(arena, p->base + block, tag_store))
                        return SSA_VALUE_NONE;
                }
                return temp;
            }
            SsaInstRef load = bir_add_typed_load(arena, component_type,
                                                 component_pointer_type, field_ptr,
                                                 (uint32_t)component_type->size,
                                                 (uint32_t)component_type->alignment,
                                                 expr->source_line, expr->source_col);
            if (load == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, load)) return SSA_VALUE_NONE;
            return bir_inst_result(arena, load, expr->source_line, expr->source_col);
        }
        case HIR_EXPR_BINOP: {
            SsaValueRef lhs = ssa_eval_expr(p, block, expr->args[0]);
            SsaValueRef rhs = ssa_eval_expr(p, block, expr->args[1]);
            if (lhs == SSA_VALUE_NONE || rhs == SSA_VALUE_NONE) return SSA_VALUE_NONE;
            const SsaValueRef operands[2] = {lhs, rhs};
            SsaInstRef inst = bir_add_inst(arena, expr->binop,
                                           expr->type, operands, 2,
                                           expr->source_line, expr->source_col);
            if (inst == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, inst)) {
                return SSA_VALUE_NONE;
            }
            return bir_inst_result(arena, inst, expr->source_line, expr->source_col);
        }
        case HIR_EXPR_CAST: {
            SsaValueRef source = ssa_eval_expr(p, block, expr->args[0]);
            if (source == SSA_VALUE_NONE) return SSA_VALUE_NONE;
            SsaInstRef inst = bir_add_inst(arena, SSA_OP_CONVERT,
                                           expr->type, &source, 1,
                                           expr->source_line, expr->source_col);
            if (inst == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, inst)) {
                return SSA_VALUE_NONE;
            }
            return bir_inst_result(arena, inst, expr->source_line, expr->source_col);
        }
        case HIR_EXPR_CALL: {
            const BirFunctionInfo *callee = bir_find_function(p->module, expr->callee);
            if (!callee) {
                ssa_fail(p, expr->source_line, expr->source_col,
                         "call has invalid ABI metadata or lowered argument count");
                return SSA_VALUE_NONE;
            }
            /* Extern calls skip call_abi entirely: they use the raw SysV
               integer-register convention at x86 emission time, not the
               internal function-call ABI classification. */
            if (!callee->is_extern &&
                (!bir_validate_function_abi(p->module, callee) ||
                 callee->call_abi.param_count != expr->arg_count)) {
                ssa_fail(p, expr->source_line, expr->source_col,
                         "call has invalid ABI metadata or lowered argument count");
                return SSA_VALUE_NONE;
            }
            if (expr->type && ssa_is_aggregate_value_type(expr->type)) {
                ssa_fail(p, expr->source_line, expr->source_col,
                         "aggregate call requires an explicit destination local");
                return SSA_VALUE_NONE;
            }
            SsaValueRef *operands = NULL;
            if (expr->arg_count) {
                operands = calloc(expr->arg_count, sizeof(SsaValueRef));
                if (!operands) return SSA_VALUE_NONE;
            }
            for (size_t i = 0; i < expr->arg_count; i++) {
                operands[i] = ssa_eval_expr(p, block, expr->args[i]);
                if (operands[i] == SSA_VALUE_NONE) {
                    free(operands);
                    return SSA_VALUE_NONE;
                }
            }
            SsaInstRef inst = bir_add_inst(arena, SSA_OP_CALL,
                                           expr->type == p->module->type_void ? NULL : expr->type,
                                           operands, expr->arg_count,
                                           expr->source_line, expr->source_col);
            if (inst != SSA_INST_NONE && expr->type &&
                bir_is_owned_slice_type(expr->type)) {
                if (p->next_allocation_id == 0 ||
                    p->next_allocation_id > BIR_MAX_STACK_SLOTS) {
                    free(operands);
                    ssa_fail(p, expr->source_line, expr->source_col,
                             "too many owned slice values in one function");
                    return SSA_VALUE_NONE;
                }
                arena->insts[inst].pointer_contract =
                    BIR_POINTER_CONTRACT_OWNED_SLICE;
                arena->insts[inst].pointer_origin = BIR_POINTER_ORIGIN_FRAME;
                arena->insts[inst].allocation_id =
                    (uint32_t)p->next_allocation_id++;
            }
            if (inst != SSA_INST_NONE && expr->type && ssa_is_view_type(expr->type)) {
                const BirFunctionInfo *callee = bir_find_function(p->module, expr->callee);
                if (!callee || callee->return_view_param >= expr->arg_count) {
                    free(operands);
                    ssa_fail(p, expr->source_line, expr->source_col,
                             "borrowed-view call has no valid source parameter");
                    return SSA_VALUE_NONE;
                }
                SsaValueRef source = operands[callee->return_view_param];
                if (source >= arena->value_count) {
                    free(operands);
                    ssa_fail(p, expr->source_line, expr->source_col,
                             "borrowed-view call has an invalid source value");
                    return SSA_VALUE_NONE;
                }
                arena->insts[inst].pointer_contract = callee->return_pointer_contract;
                arena->insts[inst].pointer_origin = arena->values[source].pointer_origin;
                arena->insts[inst].region_id = arena->values[source].region_id;
                arena->insts[inst].allocation_id = arena->values[source].allocation_id;
            }
            free(operands);
            if (inst == SSA_INST_NONE ||
                !bir_block_add_inst(arena, p->base + block, inst)) {
                return SSA_VALUE_NONE;
            }
            snprintf(arena->insts[inst].callee, sizeof(arena->insts[inst].callee),
                     "%s", expr->callee);
            arena->insts[inst].effect = SSA_EFFECT_CALL;
            if (expr->type == p->module->type_void) return SSA_VALUE_NONE;
            return bir_inst_result(arena, inst, expr->source_line, expr->source_col);
        }
        default:
            return SSA_VALUE_NONE;
    }
}

/* Build the edge-argument list for an edge from B to target by taking the
   exit value of each target parameter (ascending local order, matching the
   parameter creation order). */
static bool ssa_edge_args(SsaPass *p, size_t block, size_t target,
                          SsaValueRef **args_out, size_t *count_out) {
    (void)p;
    (void)block;
    (void)target;
    /* HIR locals are addressable stack slots in this lowering. Their values
       cross CFG edges through memory, not block arguments. */
    *args_out = NULL;
    *count_out = 0;
    return true;
}

static bool ssa_emit_aggregate_call(SsaPass *p, size_t block, HirExpr *expr,
                                     SsaValueRef destination) {
    SsaArena *arena = &p->module->arena;
    const BirFunctionInfo *callee = bir_find_function(p->module, expr->callee);
    if (!callee || !callee->has_hidden_return_storage ||
        expr->arg_count != callee->param_count ||
        !bir_validate_function_abi(p->module, callee)) return false;
    size_t operand_count = expr->arg_count + 1;
    if (operand_count != callee->call_abi.param_count) return false;
    SsaValueRef *operands = calloc(operand_count, sizeof(SsaValueRef));
    if (!operands) return false;
    operands[0] = destination;
    for (size_t i = 0; i < expr->arg_count; i++) {
        operands[i + 1] = ssa_eval_expr(p, block, expr->args[i]);
        if (operands[i + 1] == SSA_VALUE_NONE) {
            free(operands);
            return false;
        }
    }
    SsaInstRef call = bir_add_inst(arena, SSA_OP_CALL, NULL, operands,
                                   operand_count, expr->source_line, expr->source_col);
    free(operands);
    if (call == SSA_INST_NONE || !bir_block_add_inst(arena, p->base + block, call))
        return false;
    snprintf(arena->insts[call].callee, sizeof(arena->insts[call].callee),
             "%s", expr->callee);
    arena->insts[call].effect = SSA_EFFECT_CALL;
    return true;
}

static bool ssa_store_local(SsaPass *p, size_t block, uint32_t local,
                             SsaValueRef value, int line, int col) {
    SsaArena *arena = &p->module->arena;
    if (local >= p->local_count) return false;
    const CobraType *local_type = p->fn->locals[local].type;
    if (ssa_is_slice_value_type(local_type)) {
        size_t index = block * p->local_count + local;
        if (!value || !arena->values[value].type ||
            !cobra_type_equal(arena->values[value].type, local_type)) return false;
        p->block_defs[index] = value;
        p->block_def_known[index] = true;
        return true;
    }
    if (!p->local_ptrs || p->local_ptrs[local] == SSA_VALUE_NONE) return false;
    SsaValueRef pointer = p->local_ptrs[local];
    const CobraType *pointer_type = arena->values[pointer].type;
    const CobraType *type = p->fn->locals[local].type;
    if (ssa_is_aggregate_value_type(type)) {
        if (!value || arena->values[value].type == NULL ||
            arena->values[value].type->kind != COBRA_TYPE_POINTER ||
            arena->values[value].type->generic_arg_count != 1 ||
            !cobra_type_equal(arena->values[value].type->generic_args[0], type)) return false;
        SsaInstRef copy = bir_type_has_owned_payload(type)
            ? (bir_is_sum_type(type)
               ? bir_add_sum_move(arena, type, pointer, value, line, col)
               : bir_add_aggregate_move(arena, type, pointer, value, line, col))
            : bir_add_aggregate_copy(arena, type, pointer, value, line, col);
        return copy != SSA_INST_NONE && bir_block_add_inst(arena, p->base + block, copy);
    }
    SsaInstRef store = bir_add_typed_store(arena, type, pointer_type,
                                           pointer, value,
                                           (uint32_t)type->size,
                                           (uint32_t)type->alignment,
                                           line, col);
    return store != SSA_INST_NONE && bir_block_add_inst(arena, p->base + block, store);
}

static bool emit_block(SsaPass *p, size_t block, HirBlock *hb) {
    SsaArena *arena = &p->module->arena;
    const SsaBlockRef ref = p->base + (SsaBlockRef)block;

    /* Slice-value locals (views and owned slices) are SSA values, not frame
       memory. A definition in a predecessor block is visible in successor
       blocks: join the known predecessor definitions conservatively. If any
       *already-emitted* predecessor is unknown or the defs disagree, the
       block keeps the local unknown and a later use is rejected as not
       initialized on this path.

       Blocks are emitted in creation order, and loop bodies are always
       created after their header (see hir_build_for/hir_build_for_container/
       hir_build_while), so a pred with index >= block is necessarily a
       back-edge (the loop latch) that hasn't run through this join yet.
       Skipping it rather than forcing the whole join unknown lets a loop
       header/body see a container local defined before the loop, which is
       the common case (the local isn't itself reassigned inside the loop);
       ignoring it and finding no known preds at all still resolves to
       unknown below, so a header with only a back-edge pred correctly stays
       rejected. */
    for (size_t local = 0; local < p->local_count; local++) {
        if (!ssa_is_slice_value_type(p->fn->locals[local].type)) continue;
        size_t index = block * p->local_count + local;
        if (p->block_def_known[index]) continue;
        SsaValueRef joined = SSA_VALUE_NONE;
        bool known = true;
        bool any_forward_pred = false;
        for (size_t s = 0; s < hb->pred_count; s++) {
            size_t pred = hb->preds[s];
            if (pred >= block) continue; /* not yet emitted: back-edge */
            any_forward_pred = true;
            size_t pred_index = pred * p->local_count + local;
            if (!p->block_def_known[pred_index]) {
                known = false;
                break;
            }
            if (joined == SSA_VALUE_NONE) {
                joined = p->block_defs[pred_index];
            } else if (joined != p->block_defs[pred_index]) {
                known = false;
                break;
            }
        }
        if (known && any_forward_pred && joined != SSA_VALUE_NONE) {
            p->block_defs[index] = joined;
            p->block_def_known[index] = true;
        }
    }

    for (size_t i = 0; i < hb->stmt_count; i++) {
        HirStmt *stmt = &hb->stmts[i];
        switch (stmt->kind) {
            case HIR_STMT_ASSIGN: {
                if (stmt->local < p->local_count &&
                    p->fn->locals[stmt->local].type &&
                    ssa_is_aggregate_value_type(p->fn->locals[stmt->local].type) &&
                    stmt->expr && stmt->expr->kind == HIR_EXPR_CALL) {
                    if (!ssa_emit_aggregate_call(p, block, stmt->expr,
                                                  p->local_ptrs[stmt->local])) return false;
                    break;
                }
                if (stmt->local < p->local_count && stmt->expr &&
                    ssa_is_aggregate_value_type(stmt->expr->type) &&
                    p->local_ptrs && p->local_ptrs[stmt->local] != SSA_VALUE_NONE) {
                    if (stmt->expr->kind == HIR_EXPR_SUM_MAKE) {
                        if (!ssa_materialize_sum(p, block, stmt->expr,
                                                 p->local_ptrs[stmt->local])) return false;
                        break;
                    }
                    if (stmt->expr->kind == HIR_EXPR_ARRAY_LITERAL) {
                        if (!ssa_materialize_array(p, block, stmt->expr,
                                                   p->local_ptrs[stmt->local])) return false;
                        break;
                    }
                }
                SsaValueRef value = ssa_eval_expr(p, block, stmt->expr);
                if (value == SSA_VALUE_NONE) return false;
                const size_t index = block * p->local_count + stmt->local;
                if (!ssa_store_local(p, block, stmt->local, value,
                                     stmt->expr->source_line, stmt->expr->source_col)) {
                    return false;
                }
                p->block_defs[index] = value;
                p->block_def_known[index] = true;
                break;
            }
            case HIR_STMT_MEMBER_ASSIGN: {
                SsaValueRef pointer = ssa_eval_lvalue_ptr(p, block, stmt->target);
                if (pointer == SSA_VALUE_NONE) return false;
                const CobraType *field_type = stmt->target ? stmt->target->type : NULL;
                if (field_type && ssa_is_aggregate_value_type(field_type) &&
                    stmt->expr && stmt->expr->kind == HIR_EXPR_CALL) {
                    /* An aggregate-returning call writes straight into the
                       field slot through its hidden sret storage. */
                    if (!ssa_emit_aggregate_call(p, block, stmt->expr, pointer))
                        return false;
                    break;
                }
                SsaValueRef value = ssa_eval_expr(p, block, stmt->expr);
                if (value == SSA_VALUE_NONE) return false;
                const CobraType *pointer_type = arena->values[pointer].type;
                SsaInstRef store;
                if (field_type && bir_is_owned_slice_type(field_type)) {
                    SsaValueRef aggregate = ssa_eval_lvalue_ptr(p, block,
                                                                  stmt->target->args[0]);
                    store = aggregate == SSA_VALUE_NONE ? SSA_INST_NONE
                        : bir_add_field_payload_store(arena, stmt->target->aggregate_type,
                                                      field_type, aggregate, value,
                                                      stmt->target->field_offset,
                                                      stmt->expr->source_line,
                                                      stmt->expr->source_col);
                } else if (field_type && ssa_is_aggregate_value_type(field_type)) {
                    if (bir_type_has_owned_payload(field_type)) {
                        store = bir_is_sum_type(field_type)
                            ? bir_add_sum_move(arena, field_type, pointer, value,
                                               stmt->expr->source_line,
                                               stmt->expr->source_col)
                            : bir_add_aggregate_move(arena, field_type, pointer, value,
                                                     stmt->expr->source_line,
                                                     stmt->expr->source_col);
                    } else {
                        store = bir_add_aggregate_copy(arena, field_type, pointer, value,
                                                       stmt->expr->source_line,
                                                       stmt->expr->source_col);
                    }
                } else {
                    store = bir_add_typed_store(arena, field_type, pointer_type,
                                                pointer, value,
                                                (uint32_t)(field_type ? field_type->size : 0),
                                                (uint32_t)(field_type ? field_type->alignment : 0),
                                                stmt->expr->source_line,
                                                stmt->expr->source_col);
                }
                if (store == SSA_INST_NONE || !bir_block_add_inst(arena, p->base + block, store))
                    return false;
                break;
            }
            case HIR_STMT_INDEX_ASSIGN: {
                SsaValueRef pointer = ssa_eval_view_index_pointer(p, block, stmt->target);
                if (pointer == SSA_VALUE_NONE) return false;
                const CobraType *element = stmt->target ? stmt->target->type : NULL;
                if (element && ssa_is_aggregate_value_type(element) &&
                    stmt->expr && stmt->expr->kind == HIR_EXPR_CALL) {
                    /* An aggregate-returning call writes straight into the
                       indexed element slot through its hidden sret storage. */
                    if (!ssa_emit_aggregate_call(p, block, stmt->expr, pointer))
                        return false;
                    break;
                }
                SsaValueRef value = ssa_eval_expr(p, block, stmt->expr);
                if (value == SSA_VALUE_NONE) return false;
                const CobraType *pointer_type = arena->values[pointer].type;
                SsaInstRef store;
                if (element && ssa_is_aggregate_value_type(element)) {
                    store = bir_type_has_owned_payload(element)
                        ? (bir_is_sum_type(element)
                           ? bir_add_sum_move(arena, element, pointer, value,
                                              stmt->expr->source_line,
                                              stmt->expr->source_col)
                           : bir_add_aggregate_move(arena, element, pointer, value,
                                                    stmt->expr->source_line,
                                                    stmt->expr->source_col))
                        : bir_add_aggregate_copy(arena, element, pointer, value,
                                                 stmt->expr->source_line,
                                                 stmt->expr->source_col);
                } else {
                    store = bir_add_typed_store(arena, element, pointer_type,
                                                pointer, value,
                                                (uint32_t)(element ? element->size : 0),
                                                (uint32_t)(element ? element->alignment : 0),
                                                stmt->expr->source_line,
                                                stmt->expr->source_col);
                }
                if (store == SSA_INST_NONE || !bir_block_add_inst(arena, p->base + block, store))
                    return false;
                if (stmt->target && stmt->target->local < p->local_count)
                    arena->insts[store].view_source =
                        p->block_defs[block * p->local_count + stmt->target->local];
                break;
            }
            case HIR_STMT_EXPR: {
                SsaValueRef value = ssa_eval_expr(p, block, stmt->expr);
                if (value == SSA_VALUE_NONE && stmt->expr->type != p->module->type_void) {
                    return false;
                }
                break;
            }
            case HIR_STMT_PRINT: {
                SsaValueRef value = ssa_eval_expr(p, block, stmt->expr);
                if (value == SSA_VALUE_NONE) return false;
                SsaInstRef print = stmt->local
                    ? bir_add_print_string(arena, value, hb->source_line, hb->source_col)
                    : bir_add_print_i64(arena, value, hb->source_line, hb->source_col);
                if (print == SSA_INST_NONE || !bir_block_add_inst(arena, p->base + block, print))
                    return false;
                break;
            }
            case HIR_STMT_ASSERT: {
                SsaValueRef cond = ssa_eval_expr(p, block, stmt->expr);
                if (cond == SSA_VALUE_NONE) return false;
                SsaInstRef assert_inst = bir_add_assert(arena, cond, hb->source_line, hb->source_col);
                if (assert_inst == SSA_INST_NONE || !bir_block_add_inst(arena, p->base + block, assert_inst))
                    return false;
                break;
            }
            case HIR_STMT_FREE: {
                size_t index = block * p->local_count + stmt->local;
                const CobraType *owned_type = stmt->local < p->local_count
                    ? p->fn->locals[stmt->local].type : NULL;
                bool owning_aggregate = owned_type &&
                    bir_type_has_owned_payload(owned_type) &&
                    (owned_type->kind == COBRA_TYPE_STRUCT ||
                     bir_is_sum_type(owned_type));
                if (stmt->local >= p->local_count ||
                    (!owning_aggregate && (!p->block_def_known[index] ||
                                           p->block_defs[index] == SSA_VALUE_NONE)) ||
                    (owning_aggregate && (!p->local_ptrs ||
                                          p->local_ptrs[stmt->local] == SSA_VALUE_NONE))) {
                    ssa_fail(p, hb->source_line, hb->source_col,
                             "free requires a live owned slice or owning sum local");
                    return false;
                }

                SsaInstRef free;
                if (bir_is_sum_type(owned_type) && bir_type_has_owned_payload(owned_type)) {
                    free = bir_add_sum_drop(arena, owned_type, p->local_ptrs[stmt->local],
                                            hb->source_line, hb->source_col);
                } else if (owned_type && owned_type->kind == COBRA_TYPE_STRUCT &&
                           bir_type_has_owned_payload(owned_type)) {
                    free = bir_add_aggregate_drop(arena, owned_type,
                                                  p->local_ptrs[stmt->local],
                                                  hb->source_line, hb->source_col);
                } else if (bir_is_owned_buffer_type(owned_type)) {
                    free = bir_add_buffer_free(arena, p->block_defs[index],
                                               hb->source_line, hb->source_col);
                } else if (bir_is_owned_dict_type(owned_type)) {
                    free = bir_add_dict_free(arena, p->block_defs[index],
                                             hb->source_line, hb->source_col);
                } else {
                    free = bir_add_slice_free(arena, p->block_defs[index],
                                              hb->source_line, hb->source_col);
                }
                if (free == SSA_INST_NONE ||
                    !bir_block_add_inst(arena, p->base + block, free)) return false;
                break;
            }
            case HIR_STMT_DICT_SET: {
                size_t index = block * p->local_count + stmt->local;
                if (stmt->local >= p->local_count || !p->block_def_known[index] ||
                    p->block_defs[index] == SSA_VALUE_NONE ||
                    p->next_allocation_id == 0 ||
                    p->next_allocation_id > BIR_MAX_STACK_SLOTS) {
                    ssa_fail(p, hb->source_line, hb->source_col,
                             "dict set requires a live dict local");
                    return false;
                }
                const CobraType *dict_type = p->fn->locals[stmt->local].type;
                SsaValueRef dict = p->block_defs[index];
                SsaValueRef value = ssa_eval_expr(p, block, stmt->expr);
                if (dict == SSA_VALUE_NONE || value == SSA_VALUE_NONE)
                    return false;
                SsaInstRef set = bir_add_dict_set(arena, dict_type,
                                                  dict_type->generic_args[1],
                                                  dict, value, stmt->dict_key,
                                                  (uint32_t)p->next_allocation_id++,
                                                  hb->source_line, hb->source_col);
                if (set == SSA_INST_NONE ||
                    !bir_block_add_inst(arena, p->base + block, set)) return false;
                SsaValueRef result = bir_inst_result(arena, set,
                                                     hb->source_line, hb->source_col);
                p->block_defs[index] = result;
                p->block_def_known[index] = true;
                break;
            }
            case HIR_STMT_DICT_DELETE: {
                size_t index = block * p->local_count + stmt->local;
                if (stmt->local >= p->local_count || !p->block_def_known[index] ||
                    p->block_defs[index] == SSA_VALUE_NONE ||
                    p->next_allocation_id == 0 ||
                    p->next_allocation_id > BIR_MAX_STACK_SLOTS) {
                    ssa_fail(p, hb->source_line, hb->source_col,
                             "dict delete requires a live dict local");
                    return false;
                }
                const CobraType *dict_type = p->fn->locals[stmt->local].type;
                SsaValueRef dict = p->block_defs[index];
                SsaInstRef del = bir_add_dict_delete(arena, dict_type, dict,
                                                     stmt->dict_key,
                                                     (uint32_t)p->next_allocation_id++,
                                                     hb->source_line, hb->source_col);
                if (del == SSA_INST_NONE ||
                    !bir_block_add_inst(arena, p->base + block, del)) return false;
                SsaValueRef result = bir_inst_result(arena, del,
                                                     hb->source_line, hb->source_col);
                p->block_defs[index] = result;
                p->block_def_known[index] = true;
                break;
            }
            case HIR_STMT_REGION_ENTER: {
                uint32_t parent = BIR_REGION_NONE;
                for (size_t i = 0; i < p->module->region_count; i++) {
                    if (p->module->regions[i].id == stmt->local) {
                        parent = p->module->regions[i].parent_id;
                        break;
                    }
                }
                SsaInstRef enter = bir_add_region_enter(arena, stmt->local, parent,
                                                        hb->source_line, hb->source_col);
                if (enter == SSA_INST_NONE ||
                    !bir_block_add_inst(arena, p->base + block, enter)) return false;
                break;
            }
            case HIR_STMT_REGION_EXIT: {
                SsaInstRef exit = bir_add_region_exit(arena, stmt->local,
                                                      hb->source_line, hb->source_col);
                if (exit == SSA_INST_NONE ||
                    !bir_block_add_inst(arena, p->base + block, exit)) return false;
                break;
            }
            default:
                break;
        }
    }

    switch (hb->term.kind) {
        case HIR_TERM_JUMP: {
            SsaValueRef *args = NULL;
            size_t count = 0;
            if (!ssa_edge_args(p, block, hb->term.target, &args, &count)) return false;
            bool ok = bir_add_edge(arena, ref, p->base + hb->term.target) &&
                      bir_set_jump(arena, ref, p->base + hb->term.target,
                                   args, count, hb->source_line, hb->source_col);
            free(args);
            return ok;
        }
        case HIR_TERM_BRANCH: {
            SsaValueRef cond = ssa_eval_expr(p, block, hb->term.cond);
            if (cond == SSA_VALUE_NONE) return false;
            SsaValueRef *then_args = NULL;
            SsaValueRef *else_args = NULL;
            size_t then_count = 0, else_count = 0;
            if (!ssa_edge_args(p, block, hb->term.target, &then_args, &then_count) ||
                !ssa_edge_args(p, block, hb->term.target2, &else_args, &else_count)) {
                free(then_args);
                free(else_args);
                return false;
            }
            bool ok = bir_add_edge(arena, ref, p->base + hb->term.target) &&
                      bir_add_edge(arena, ref, p->base + hb->term.target2) &&
                      bir_set_branch(arena, ref, cond,
                                     p->base + hb->term.target,
                                     p->base + hb->term.target2,
                                     then_args, then_count,
                                     else_args, else_count,
                                     hb->source_line, hb->source_col);
            free(then_args);
            free(else_args);
            return ok;
        }
        case HIR_TERM_RETURN: {
            SsaValueRef value = SSA_VALUE_NONE;
            if (hb->term.ret_expr) {
                bool aggregate_return = hb->term.ret_expr->type &&
                    ssa_is_aggregate_value_type(hb->term.ret_expr->type);
                if (aggregate_return && hb->term.ret_expr->kind == HIR_EXPR_CALL) {
                    /* An aggregate-returning call in tail position (e.g.
                       `return fs_open(path, flags)?` desugared to a plain
                       call) writes straight into the caller's sret storage,
                       same as the assignment/member-store paths below. */
                    if (p->return_storage == SSA_VALUE_NONE) {
                        ssa_fail(p, hb->source_line, hb->source_col,
                                 "aggregate return has no caller-provided storage");
                        return false;
                    }
                    if (!ssa_emit_aggregate_call(p, block, hb->term.ret_expr,
                                                 p->return_storage)) return false;
                    return bir_set_return(arena, ref, SSA_VALUE_NONE,
                                          hb->source_line, hb->source_col);
                }
                if (aggregate_return &&
                    (hb->term.ret_expr->kind == HIR_EXPR_SUM_MAKE ||
                     hb->term.ret_expr->kind == HIR_EXPR_ARRAY_LITERAL)) {
                    value = SSA_VALUE_NONE;
                } else {
                    value = ssa_eval_expr(p, block, hb->term.ret_expr);
                    if (value == SSA_VALUE_NONE) return false;
                }
                if (aggregate_return) {
                    if (p->return_storage == SSA_VALUE_NONE) {
                        ssa_fail(p, hb->source_line, hb->source_col,
                                 "aggregate return has no caller-provided storage");
                        return false;
                    }
                    if (hb->term.ret_expr->kind == HIR_EXPR_SUM_MAKE) {
                        if (!ssa_materialize_sum(p, block, hb->term.ret_expr,
                                                 p->return_storage)) return false;
                        value = SSA_VALUE_NONE;
                    } else if (hb->term.ret_expr->kind == HIR_EXPR_ARRAY_LITERAL) {
                        if (!ssa_materialize_array(p, block, hb->term.ret_expr,
                                                   p->return_storage)) return false;
                        value = SSA_VALUE_NONE;
                    } else {                SsaInstRef copy = bir_type_has_owned_payload(hb->term.ret_expr->type)
                    ? (bir_is_sum_type(hb->term.ret_expr->type)
                       ? bir_add_sum_move(arena, hb->term.ret_expr->type,
                                          p->return_storage, value,
                                          hb->source_line, hb->source_col)
                       : bir_add_aggregate_move(arena, hb->term.ret_expr->type,
                                                p->return_storage, value,
                                                hb->source_line, hb->source_col))
                    : bir_add_aggregate_copy(
                                arena, hb->term.ret_expr->type, p->return_storage,
                                value, hb->source_line, hb->source_col);
                        if (copy == SSA_INST_NONE || !bir_block_add_inst(arena, ref, copy))
                            return false;
                        value = SSA_VALUE_NONE;
                    }
                }
            }
            return bir_set_return(arena, ref, value,
                                  hb->source_line, hb->source_col);
        }
        default:
            ssa_fail(p, hb->source_line, hb->source_col,
                     "internal error: block has no terminator");
            return false;
    }
}

bool bir_ssa_lower(BackendIrModule *module, HirFunction *fn, SsaBlockRef *entry_out,
                   SsaValueRef *param_refs_out) {
    if (!module || !fn || fn->block_count == 0) {
        if (module) snprintf(module->error, sizeof(module->error),
                             "SSA pass requires a non-empty HIR function");
        return false;
    }
    module->error[0] = '\0';
    const size_t locals = fn->local_count;

    SsaPass pass;
    memset(&pass, 0, sizeof(pass));
    pass.module = module;
    pass.fn = fn;
    pass.local_count = locals;
    pass.param_refs = param_refs_out;
    /* Stack slots occupy allocation identities 1..locals. Owned parameters
       reserve the next identities, and local slice allocations continue past
       those parameter identities. */
    size_t owned_parameters = 0;
    size_t owning_sum_parameters = 0;
    size_t owning_struct_parameters = 0;
    for (size_t i = 0; i < fn->param_count; i++) {
        if (bir_is_owned_slice_type(fn->param_types[i]) ||
            bir_is_owned_dict_type(fn->param_types[i])) owned_parameters++;
        if (bir_is_sum_type(fn->param_types[i]) &&
            bir_type_has_owned_payload(fn->param_types[i])) owning_sum_parameters++;
        if (fn->param_types[i]->kind == COBRA_TYPE_STRUCT &&
            bir_type_has_owned_payload(fn->param_types[i])) owning_struct_parameters++;
    }
    pass.next_allocation_id = locals + owned_parameters + owning_sum_parameters +
                              owning_struct_parameters + 1;

    const size_t cells = fn->block_count * (locals ? locals : 1);
    pass.reads_before_assign = calloc(cells, sizeof(bool));
    pass.assigns = calloc(cells, sizeof(bool));
    pass.live_in = calloc(cells, sizeof(bool));
    pass.block_defs = calloc(cells, sizeof(SsaValueRef));
    pass.block_def_known = calloc(cells, sizeof(bool));
    pass.param_values = calloc(cells, sizeof(SsaValueRef));
    pass.param_created = calloc(cells, sizeof(bool));
    if (!pass.reads_before_assign || !pass.assigns || !pass.live_in ||
        !pass.block_defs || !pass.block_def_known ||
        !pass.param_values || !pass.param_created) {
        snprintf(module->error, sizeof(module->error), "out of memory in SSA pass");
        goto fail;
    }

    /* Aggregate locals are zero-initialized stack storage when declared
       without an initializer, matching Cobra's struct-local contract. Mark
       those entry slots initialized for read-before-assignment analysis. */
    for (size_t local = 0; local < locals; local++) {
        if (fn->locals[local].type &&
            (fn->locals[local].type->kind == COBRA_TYPE_STRUCT ||
             fn->locals[local].type->kind == COBRA_TYPE_ARRAY)) {
            pass.assigns[local] = true;
        }
    }
    for (size_t b = 0; b < fn->block_count; b++) {
        analyze_block(&pass, b, &fn->blocks[b]);
    }
    compute_liveness(&pass);
    for (size_t local = 0; local < locals; local++) {
        if (pass.live_in[local] && !fn->locals[local].is_param) {
            ssa_fail(&pass, fn->locals[local].source_line,
                     fn->locals[local].source_col,
                     "local '%s' is read before assignment on some path",
                     fn->locals[local].name);
            goto fail;
        }
    }
    if (!create_blocks_and_params(&pass)) goto fail;

    for (size_t b = 0; b < fn->block_count; b++) {
        if (!emit_block(&pass, b, &fn->blocks[b])) goto fail;
    }

    if (entry_out) *entry_out = pass.base;
    free(pass.reads_before_assign);
    free(pass.assigns);
    free(pass.live_in);
    free(pass.block_defs);
    free(pass.block_def_known);
    free(pass.param_values);
    free(pass.param_created);
    free(pass.local_ptrs);
    return true;

fail:
    free(pass.reads_before_assign);
    free(pass.assigns);
    free(pass.live_in);
    free(pass.block_defs);
    free(pass.block_def_known);
    free(pass.param_values);
    free(pass.param_created);
    free(pass.local_ptrs);
    return false;
}
