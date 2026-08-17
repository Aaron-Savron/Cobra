/*
 * Cobra Backend IR: verifier.
 *
 * Checks the invariants documented in docs/BACKEND_IR.md: one definition per
 * value, valid operands, edge-argument arity, terminators, dominance and use
 * ordering, no unresolved generic parameters, and canonical finalized types.
 * Unreachable blocks are legal; their contents are checked structurally but
 * dominance inside them is not enforced.
 */
#include "ssa.h"
#include <stdarg.h>

typedef struct {
    const BackendIrModule *module;
    char *errbuf;
    size_t errbuf_size;
    bool *reachable;   /* per block */
    int *function_index; /* owning function, or -1 */
    bool *dom;         /* [block * blocks + dominator]  */
    SsaBlockRef *rpo;  /* reverse postorder over reachable blocks */
    size_t rpo_count;
} VerifyCtx;

static void verr(VerifyCtx *ctx, const char *fmt, ...) {
    if (!ctx->errbuf || ctx->errbuf_size == 0 || ctx->errbuf[0]) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(ctx->errbuf, ctx->errbuf_size, fmt, args);
    va_end(args);
}

static bool valid_value(const VerifyCtx *ctx, SsaValueRef ref) {
    return ref != SSA_VALUE_NONE && ref < ctx->module->arena.value_count &&
           ctx->module->arena.values[ref].kind != SSA_VALUE_INVALID;
}

static bool block_is_function_entry(const VerifyCtx *ctx, SsaBlockRef block) {
    for (size_t f = 0; f < ctx->module->function_count; f++) {
        if (ctx->module->functions[f].entry == block) return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Reachability + reverse postorder (iterative DFS)                   */
/* ------------------------------------------------------------------ */

static void compute_reachability(VerifyCtx *ctx) {
    const SsaArena *arena = &ctx->module->arena;
    const size_t blocks = arena->block_count;
    bool *visited = calloc(blocks ? blocks : 1, sizeof(bool));
    SsaBlockRef *stack = calloc((blocks ? blocks : 1) + 1, sizeof(SsaBlockRef));
    size_t *next_succ = calloc(blocks ? blocks : 1, sizeof(size_t));
    if (!visited || !stack || !next_succ) {
        free(visited);
        free(stack);
        free(next_succ);
        return;
    }
    for (size_t f = 0; f < ctx->module->function_count; f++) {
        SsaBlockRef entry = ctx->module->functions[f].entry;
        if (entry == SSA_BLOCK_NONE || entry >= blocks || visited[entry]) continue;
        size_t top = 0;
        stack[top++] = entry;
        visited[entry] = true;
        ctx->function_index[entry] = (int)f;
        while (top) {
            SsaBlockRef block = stack[top - 1];
            const SsaBlock *b = &arena->blocks[block];
            if (next_succ[block] < b->succ_count) {
                SsaBlockRef succ = b->succs[next_succ[block]++];
                if (succ < blocks && !visited[succ]) {
                    visited[succ] = true;
                    ctx->function_index[succ] = (int)f;
                    stack[top++] = succ;
                }
            } else {
                ctx->rpo[ctx->rpo_count++] = block;
                top--;
            }
        }
    }
    for (size_t i = 0; i < blocks; i++) ctx->reachable[i] = visited[i];
    /* rpo holds postorder; reverse it in place for reverse postorder. */
    for (size_t i = 0; i < ctx->rpo_count / 2; i++) {
        SsaBlockRef tmp = ctx->rpo[i];
        ctx->rpo[i] = ctx->rpo[ctx->rpo_count - 1 - i];
        ctx->rpo[ctx->rpo_count - 1 - i] = tmp;
    }
    free(visited);
    free(stack);
    free(next_succ);
}

/* ------------------------------------------------------------------ */
/* Dominators (iterative dataflow over reachable blocks)              */
/* ------------------------------------------------------------------ */

static void compute_dominators(VerifyCtx *ctx) {
    const SsaArena *arena = &ctx->module->arena;
    const size_t blocks = arena->block_count;
    for (size_t i = 0; i < blocks * blocks; i++) ctx->dom[i] = false;
    /* Iterative dominators start with the universal set for every reachable
       non-entry block. Starting those rows empty gets stuck on backedges:
       a loop header would intersect its yet-uninitialized latch and lose its
       entry dominator forever. Restrict the universal set to the owning
       function so separate functions never dominate one another. */
    for (size_t b = 0; b < blocks; b++) {
        if (!ctx->reachable[b]) continue;
        if (block_is_function_entry(ctx, (SsaBlockRef)b)) {
            ctx->dom[b * blocks + b] = true;
            continue;
        }
        for (size_t d = 0; d < blocks; d++) {
            if (ctx->reachable[d] && ctx->function_index[d] == ctx->function_index[b]) {
                ctx->dom[b * blocks + d] = true;
            }
        }
    }
    bool *candidate = calloc(blocks ? blocks : 1, sizeof(bool));
    bool *scratch = calloc(blocks ? blocks : 1, sizeof(bool));
    if (!candidate || !scratch) {
        free(candidate);
        free(scratch);
        return;
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < ctx->rpo_count; i++) {
            SsaBlockRef b = ctx->rpo[i];
            if (block_is_function_entry(ctx, b)) continue;
            const SsaBlock *bb = &arena->blocks[b];
            bool have_pred = false;
            for (size_t d = 0; d < blocks; d++) candidate[d] = false;
            for (size_t p = 0; p < bb->pred_count; p++) {
                SsaBlockRef pred = bb->preds[p];
                if (pred >= blocks || !ctx->reachable[pred]) continue;
                if (!have_pred) {
                    for (size_t d = 0; d < blocks; d++) {
                        candidate[d] = ctx->dom[pred * blocks + d];
                    }
                    have_pred = true;
                } else {
                    for (size_t d = 0; d < blocks; d++) {
                        scratch[d] = candidate[d] && ctx->dom[pred * blocks + d];
                    }
                    for (size_t d = 0; d < blocks; d++) candidate[d] = scratch[d];
                }
            }
            if (!have_pred) continue;
            candidate[b] = true;
            for (size_t d = 0; d < blocks; d++) {
                if (candidate[d] && !ctx->dom[b * blocks + d]) {
                    ctx->dom[b * blocks + d] = true;
                    changed = true;
                }
            }
        }
    }
    free(candidate);
    free(scratch);
}

/* ------------------------------------------------------------------ */
/* Structural and semantic checks                                     */
/* ------------------------------------------------------------------ */

static bool type_matches(const CobraType *actual, const CobraType *expected) {
    return actual && expected &&
           (actual == expected || cobra_type_equal(actual, expected));
}

static bool check_value_type(VerifyCtx *ctx, SsaValueRef ref,
                             const CobraType *expected, const char *what) {
    if (!valid_value(ctx, ref)) {
        verr(ctx, "%s is not a valid SSA value", what);
        return false;
    }
    const CobraType *actual = ctx->module->arena.values[ref].type;
    if (!type_matches(actual, expected)) {
        verr(ctx, "%s has the wrong canonical type", what);
        return false;
    }
    return true;
}

static bool is_pointer_type(const CobraType *type);
static bool is_view_type(const CobraType *type);
static bool is_owned_slice_type(const CobraType *type);
static BirPointerContract view_contract(const CobraType *type);

static const BirRegionInfo *find_region(const BackendIrModule *module,
                                        uint32_t region_id) {
    if (!module || region_id == BIR_REGION_NONE) return NULL;
    for (size_t i = 0; i < module->region_count; i++) {
        if (module->regions[i].declared && module->regions[i].id == region_id)
            return &module->regions[i];
    }
    return NULL;
}

static bool pointer_origin_matches(const SsaValue *value) {
    if (!value || (!is_pointer_type(value->type) && !is_view_type(value->type) &&
                   !is_owned_slice_type(value->type))) return false;
    if (value->pointer_origin == BIR_POINTER_ORIGIN_REGION)
        return value->region_id != BIR_REGION_NONE;
    if (value->pointer_origin == BIR_POINTER_ORIGIN_FRAME ||
        value->pointer_origin == BIR_POINTER_ORIGIN_CALLER)
        return value->region_id == BIR_REGION_NONE;
    return false;
}

static bool pointer_contract_matches(const SsaValue *value,
                                     BirPointerContract expected) {
    return value && is_pointer_type(value->type) &&
           bir_pointer_contract_compatible(value->pointer_contract, expected);
}

static bool assign_function_ownership(VerifyCtx *ctx) {
    const SsaArena *arena = &ctx->module->arena;
    for (size_t f = 0; f < ctx->module->function_count; f++) {
        const BirFunctionInfo *info = &ctx->module->functions[f];
        if (info->is_extern) continue;
        if (info->first_block == SSA_BLOCK_NONE || info->block_count == 0 ||
            info->first_block >= arena->block_count ||
            info->block_count > arena->block_count - info->first_block) {
            verr(ctx, "function '%s' has an invalid owned block range", info->name);
            return false;
        }
        for (size_t offset = 0; offset < info->block_count; offset++) {
            size_t block = (size_t)info->first_block + offset;
            if (ctx->function_index[block] != -1) {
                verr(ctx, "SSA block b%zu belongs to multiple functions", block);
                return false;
            }
            ctx->function_index[block] = (int)f;
        }
    }
    return true;
}

static bool check_function_table(VerifyCtx *ctx) {
    const SsaArena *arena = &ctx->module->arena;
    for (size_t f = 0; f < ctx->module->function_count; f++) {
        const BirFunctionInfo *info = &ctx->module->functions[f];
        if (info->is_extern) continue;
        if (info->entry == SSA_BLOCK_NONE || info->entry >= arena->block_count ||
            info->first_block != info->entry || info->block_count == 0 ||
            info->block_count > arena->block_count - info->first_block) {
            verr(ctx, "function '%s' has an invalid entry or block range", info->name);
            return false;
        }
        if (!info->return_type || !info->return_type->finalized ||
            bir_type_has_generic(info->return_type)) {
            verr(ctx, "function '%s' has an invalid return type", info->name);
            return false;
        }
        if (!info->return_value_type || !info->return_value_type->finalized ||
            info->return_abi != info->return_type->abi ||
            info->return_value_abi != info->return_value_type->abi ||
            (info->has_hidden_return_storage &&
             (!is_pointer_type(info->return_value_type) ||
              !type_matches(info->return_value_type->generic_args[0], info->return_type))) ||
            (is_view_type(info->return_type) &&
             (info->return_view_param >= info->param_count ||
              !is_view_type(info->param_value_types[info->return_view_param]) ||
              info->return_pointer_contract != view_contract(info->return_type) ||
              !type_matches(cobra_type_element(info->return_type),
                            cobra_type_element(info->param_value_types[info->return_view_param]))))) {
            verr(ctx, "function '%s' has inconsistent return ABI metadata", info->name);
            return false;
        }
        if (info->ssa_param_count != info->param_count +
            (info->has_hidden_return_storage ? 1U : 0U) ||
            info->ssa_param_count > BIR_MAX_SSA_PARAMS) {
            verr(ctx, "function '%s' has an invalid lowered parameter count", info->name);
            return false;
        }
        if (!bir_validate_function_abi(ctx->module, info)) {
            verr(ctx, "function '%s' has inconsistent call ABI locations", info->name);
            return false;
        }
        for (size_t p = 0; p < info->param_count; p++) {
            if (!info->param_types[p] || !info->param_types[p]->finalized ||
                !info->param_value_types[p] || !info->param_value_types[p]->finalized ||
                bir_type_has_generic(info->param_types[p]) ||
                bir_type_has_generic(info->param_value_types[p]) ||
                info->param_abi[p] != info->param_types[p]->abi ||
                info->param_value_abi[p] != info->param_value_types[p]->abi ||
                ((is_pointer_type(info->param_value_types[p]) ||
                  is_view_type(info->param_value_types[p])) &&
                 info->param_pointer_contract[p] == BIR_POINTER_CONTRACT_UNKNOWN) ||
                (info->param_types[p]->kind == COBRA_TYPE_STRUCT &&
                 (!is_pointer_type(info->param_value_types[p]) ||
                  !type_matches(info->param_value_types[p]->generic_args[0],
                                info->param_types[p])))) {
                verr(ctx, "function '%s' has invalid parameter ABI metadata", info->name);
                return false;
            }
        }
    }
    return true;
}

static bool check_function_parameters(VerifyCtx *ctx) {
    const SsaArena *arena = &ctx->module->arena;
    for (size_t f = 0; f < ctx->module->function_count; f++) {
        const BirFunctionInfo *info = &ctx->module->functions[f];
        for (size_t p = 0; p < info->ssa_param_count; p++) {
            SsaValueRef ref = info->params[p];
            const CobraType *expected = NULL;
            size_t source_index = p;
            if (info->has_hidden_return_storage && p == 0) {
                expected = info->return_value_type;
            } else {
                if (info->has_hidden_return_storage) source_index--;
                expected = info->param_value_types[source_index];
            }
            if (!valid_value(ctx, ref) || arena->values[ref].kind != SSA_VALUE_PARAM ||
                arena->values[ref].param_index != p ||
                !type_matches(arena->values[ref].type, expected) ||
                ((is_pointer_type(expected) || is_view_type(expected)) &&
                 arena->values[ref].pointer_contract !=
                     (info->has_hidden_return_storage && p == 0
                          ? BIR_POINTER_CONTRACT_CALLER_STORAGE
                          : info->param_pointer_contract[source_index]))) {
                verr(ctx, "function '%s' lowered parameter %zu is not tied to its declared signature",
                     info->name, p + 1);
                return false;
            }
        }
    }
    return true;
}

static BirScalarKind scalar_kind_for_type(const BackendIrModule *module,
                                          const CobraType *type) {
    if (type == module->type_i64) return BIR_SCALAR_I64;
    if (type == module->type_i32) return BIR_SCALAR_I32;
    if (type == module->type_u32) return BIR_SCALAR_U32;
    if (type == module->type_u64) return BIR_SCALAR_U64;
    if (type == module->type_bool) return BIR_SCALAR_BOOL;
    if (type == module->type_f32) return BIR_SCALAR_F32;
    if (type == module->type_f64) return BIR_SCALAR_F64;
    if (type == module->type_u8) return BIR_SCALAR_U8;
    /* Integer-backed unit enums carry i64 payloads. */
    if (type && type->kind == COBRA_TYPE_ENUM) return BIR_SCALAR_I64;
    return BIR_SCALAR_INVALID;
}

static bool check_types(VerifyCtx *ctx) {
    const SsaArena *arena = &ctx->module->arena;
    for (size_t i = 1; i < arena->value_count; i++) { /* slot 0 is the sentinel */
        const SsaValue *value = &arena->values[i];
        if (value->kind == SSA_VALUE_INVALID) {
            verr(ctx, "value v%zu is invalid", i);
            return false;
        }
        if (!value->type) {
            verr(ctx, "value v%zu has no canonical type", i);
            return false;
        }
        if (!value->type->finalized) {
            verr(ctx, "value v%zu type is not finalized", i);
            return false;
        }
        if (bir_type_has_generic(value->type)) {
            verr(ctx, "value v%zu carries an unresolved generic parameter", i);
            return false;
        }
        if ((is_pointer_type(value->type) || is_view_type(value->type) ||
             is_owned_slice_type(value->type)) &&
            (value->pointer_contract == BIR_POINTER_CONTRACT_UNKNOWN ||
             !pointer_origin_matches(value) ||
             (value->pointer_origin == BIR_POINTER_ORIGIN_REGION &&
              !find_region(ctx->module, value->region_id)))) {
            verr(ctx, "pointer value v%zu has invalid ownership or region provenance", i);
            return false;
        }
        if (value->kind == SSA_VALUE_CONST) {
            BirScalarKind expected_kind = scalar_kind_for_type(ctx->module, value->type);
            if (expected_kind == BIR_SCALAR_INVALID ||
                value->const_value.type != value->type ||
                value->const_value.kind != expected_kind) {
                verr(ctx, "constant v%zu has a payload incompatible with its canonical type", i);
                return false;
            }
        }
        if (value->kind == SSA_VALUE_INST) {
            if (value->def_inst == SSA_INST_NONE || value->def_inst >= arena->inst_count) {
                verr(ctx, "value v%zu has no valid defining instruction", i);
                return false;
            }
            if (arena->insts[value->def_inst].result != i) {
                verr(ctx, "value v%zu is not the result of its defining instruction", i);
                return false;
            }
        }
        if (value->kind == SSA_VALUE_BLOCK_PARAM) {
            if (value->block == SSA_BLOCK_NONE || value->block >= arena->block_count) {
                verr(ctx, "block parameter v%zu has no owning block", i);
                return false;
            }
            bool found = false;
            const SsaBlock *b = &arena->blocks[value->block];
            for (size_t k = 0; k < b->param_count; k++) {
                if (b->params[k] == i) found = true;
            }
            if (!found) {
                verr(ctx, "block parameter v%zu is not listed in block b%u", i, value->block);
                return false;
            }
        }
    }
    return true;
}

static bool is_pointer_type(const CobraType *type) {
    return type && type->kind == COBRA_TYPE_POINTER &&
           type->generic_arg_count == 1 && type->generic_args[0];
}

static bool is_view_type(const CobraType *type) {
    return bir_is_borrowed_view_type(type);
}

static bool is_owned_slice_type(const CobraType *type) {
    return bir_is_owned_slice_type(type) || bir_is_owned_dict_type(type);
}

static BirPointerContract view_contract(const CobraType *type) {
    return bir_view_is_writable(type)
        ? BIR_POINTER_CONTRACT_BORROW_WRITE
        : BIR_POINTER_CONTRACT_BORROW_READONLY;
}

static bool is_memory_scalar(const CobraType *type) {
    return type && (type->kind == COBRA_TYPE_I64 ||
                    type->kind == COBRA_TYPE_I32 ||
                    type->kind == COBRA_TYPE_U32 ||
                    type->kind == COBRA_TYPE_U64 ||
                    type->kind == COBRA_TYPE_BOOL ||
                    type->kind == COBRA_TYPE_F32 ||
                    type->kind == COBRA_TYPE_F64 ||
                    type->kind == COBRA_TYPE_U8 ||
                    type->kind == COBRA_TYPE_ENUM);
}

static bool is_memory_aggregate(const CobraType *type) {
    if (!type || !type->finalized || type->ownership != COBRA_OWNERSHIP_VALUE ||
        type->mutability != COBRA_MUTABILITY_DEFAULT) return false;
    if (type->kind == COBRA_TYPE_ARRAY) {
        return type->generic_arg_count == 1 && type->array_length > 0 &&
               type->array_length <= COBRA_MAX_ARRAY_ELEMENTS &&
               (is_memory_scalar(type->generic_args[0]) ||
                is_memory_aggregate(type->generic_args[0])) &&
               type->size == type->generic_args[0]->size * type->array_length;
    }
    if (type->kind == COBRA_TYPE_OPTION || type->kind == COBRA_TYPE_RESULT ||
        (type->kind == COBRA_TYPE_ENUM && type->generic_arg_count > 0)) {
        /* Non-owning sums: components are memory scalars (fixed-width slots)
           or recursively memory aggregates (nested sums). Payload-carrying
           enums are the N-variant generalization; unit variants carry NULL. */
        size_t required = type->kind == COBRA_TYPE_RESULT ? 2 : 1;
        if (type->generic_arg_count < required) return false;
        for (size_t i = 0; i < type->generic_arg_count; i++) {
            const CobraType *component = type->generic_args[i];
            if (!component) continue; /* unit enum variant */
            if (!is_memory_scalar(component) &&
                !is_memory_aggregate(component) &&
                !is_owned_slice_type(component)) return false;
            if (is_memory_scalar(component) &&
                component->size > COBRA_NATIVE_SUM_SCALAR_SIZE) return false;
        }
        return true;
    }
    if (type->kind != COBRA_TYPE_STRUCT || type->field_count == 0) return false;
    for (size_t i = 0; i < type->field_count; i++) {
        const CobraTypeField *field = &type->fields[i];
        bool borrowed_view_field =
            is_view_type(field->type) &&
            field->ownership == COBRA_OWNERSHIP_BORROWED &&
            field->mutability == COBRA_MUTABILITY_READONLY &&
            field->region_id == -1;
        if ((!borrowed_view_field &&
             (field->ownership != COBRA_OWNERSHIP_VALUE ||
              field->mutability != COBRA_MUTABILITY_DEFAULT)) ||
            (!is_memory_scalar(field->type) &&
             !is_memory_aggregate(field->type) &&
             !is_owned_slice_type(field->type) &&
             !is_view_type(field->type)) ||
            field->offset > type->size ||
            field->type->size > type->size - field->offset) return false;
    }
    return true;
}

static bool is_memory_type(const CobraType *type) {
    return is_memory_scalar(type) || is_memory_aggregate(type);
}

static bool aggregate_field_matches(const CobraType *aggregate, int64_t offset,
                                    const CobraType *field_type, uint32_t width,
                                    uint32_t alignment) {
    if (!aggregate || offset < 0 || !field_type ||
        !is_memory_aggregate(aggregate)) return false;
    for (size_t i = 0; i < aggregate->field_count; i++) {
        const CobraTypeField *field = &aggregate->fields[i];
        if (field->offset == (size_t)offset && type_matches(field->type, field_type) &&
            field->type->size == width && field->type->alignment == alignment &&
            (is_memory_scalar(field->type) || is_memory_aggregate(field->type) ||
             is_owned_slice_type(field->type) || is_view_type(field->type))) return true;
    }
    return false;
}

static bool field_metadata_matches(const SsaInst *inst) {
    if (!is_memory_aggregate(inst->memory_type) ||
        !is_pointer_type(inst->type) || inst->type->generic_arg_count != 1) return false;
    const CobraType *field_type = inst->type->generic_args[0];
    if (inst->memory_type->kind == COBRA_TYPE_OPTION ||
        inst->memory_type->kind == COBRA_TYPE_RESULT ||
        (inst->memory_type->kind == COBRA_TYPE_ENUM &&
         inst->memory_type->generic_arg_count > 0)) {
        /* Canonical sum components: tag at 0, then one resident slot per
           component (payload, then error for Result). The tag pointer is
           i64-typed; enum variants are addressed by their generalized
           component offset. */
        if (inst->memory_offset == 0)
            return field_type->kind == COBRA_TYPE_I64 &&
                   inst->memory_width == COBRA_NATIVE_SUM_TAG_SIZE &&
                   inst->memory_alignment == field_type->alignment;
        for (size_t i = 1; i <= inst->memory_type->generic_arg_count; i++) {
            const CobraType *component = inst->memory_type->generic_args[i - 1];
            if (!component) continue;
            if ((size_t)inst->memory_offset ==
                    bir_sum_component_offset(inst->memory_type, (int)i) &&
                field_type == component &&
                inst->memory_width == field_type->size &&
                inst->memory_alignment == field_type->alignment)
                return true;
        }
        return false;
    }
    return aggregate_field_matches(inst->memory_type, inst->memory_offset,
                                    field_type, inst->memory_width,
                                    inst->memory_alignment);
}

static bool check_instruction_signature(VerifyCtx *ctx, SsaBlockRef ref,
                                         SsaInstRef iref, const SsaInst *inst) {
    const SsaArena *arena = &ctx->module->arena;
    const CobraType *i64 = ctx->module->type_i64;
    const CobraType *boolean = ctx->module->type_bool;
    const SsaValueRef *ops = inst->operand_count
        ? &arena->operands[inst->operand_start] : NULL;
    const char *opname = bir_opcode_name(inst->op);

    if (inst->op == SSA_OP_CALL) return true;
    if (inst->op == SSA_OP_PARAM || inst->op == SSA_OP_BLOCK_ARG) {
        verr(ctx, "block b%u instruction i%u uses an implicit opcode directly",
             ref, iref);
        return false;
    }

    size_t expected_operands = 0;
    const CobraType *result_type = NULL;
    bool has_result = false;
    bool operands_are_i64 = false;
    bool operands_are_numeric = false;
    bool operands_are_bool = false;
    switch (inst->op) {
        case SSA_OP_CONST:
            if (inst->type != i64 && inst->type != boolean &&
                inst->type != ctx->module->type_i32 &&
                inst->type != ctx->module->type_u32 &&
                inst->type != ctx->module->type_u64 &&
                inst->type != ctx->module->type_f32 &&
                inst->type != ctx->module->type_f64 &&
                inst->type != ctx->module->type_u8 &&
                !(inst->type && inst->type->kind == COBRA_TYPE_ENUM)) {
                verr(ctx, "block b%u const has an unsupported result type", ref);
                return false;
            }
            result_type = inst->type;
            has_result = true;
            break;
        case SSA_OP_ADD:
        case SSA_OP_SUB:
        case SSA_OP_MUL:
        case SSA_OP_DIV:
        case SSA_OP_REM:
            expected_operands = 2;
            has_result = true;
            operands_are_numeric = true;
            break;
        case SSA_OP_NEG:
            expected_operands = 1;
            has_result = true;
            operands_are_numeric = true;
            break;
        case SSA_OP_EQ:
        case SSA_OP_NE:
        case SSA_OP_LT:
        case SSA_OP_LE:
        case SSA_OP_GT:
        case SSA_OP_GE:
            expected_operands = 2;
            result_type = boolean;
            has_result = true;
            operands_are_numeric = true;
            break;
        case SSA_OP_STACK_SLOT:
            if (!is_pointer_type(inst->type) || !is_memory_type(inst->memory_type)) {
                verr(ctx, "block b%u stack slot has an invalid pointer or memory type", ref);
                return false;
            }
            if (!type_matches(inst->type->generic_args[0], inst->memory_type)) {
                verr(ctx, "block b%u stack slot pointer does not match its memory type", ref);
                return false;
            }
            result_type = inst->type;
            has_result = true;
            break;
        case SSA_OP_PTR_ADD:
            expected_operands = 2;
            result_type = inst->type;
            has_result = true;
            break;
        case SSA_OP_FIELD_ADDR:
            expected_operands = 1;
            result_type = inst->type;
            has_result = true;
            break;
        case SSA_OP_ARRAY_INDEX_ADDR:
            expected_operands = 2;
            result_type = inst->type;
            has_result = true;
            break;
        case SSA_OP_LOAD:
            expected_operands = 1;
            result_type = inst->memory_type;
            has_result = true;
            break;
        case SSA_OP_STORE:
            expected_operands = 2;
            break;
        case SSA_OP_AGG_COPY:
            expected_operands = 2;
            break;
        case SSA_OP_REGION_ENTER:
        case SSA_OP_REGION_EXIT:
            break;
        case SSA_OP_TRANSFER:
            expected_operands = 1;
            result_type = inst->type;
            has_result = true;
            break;
        case SSA_OP_DESTROY:
            expected_operands = 1;
            break;
        case SSA_OP_VIEW_MAKE:
            expected_operands = 2;
            result_type = inst->type;
            has_result = true;
            break;
        case SSA_OP_VIEW_PTR:
            expected_operands = 1;
            result_type = inst->type;
            has_result = true;
            break;
        case SSA_OP_VIEW_LEN:
            expected_operands = 1;
            result_type = i64;
            has_result = true;
            break;
        case SSA_OP_SLICE_ALLOC:
            expected_operands = 1;
            result_type = inst->type;
            has_result = true;
            break;
        case SSA_OP_SLICE_FREE:
            expected_operands = 1;
            break;
        case SSA_OP_BUFFER_ALLOC:
            expected_operands = 1;
            result_type = inst->type;
            has_result = true;
            break;
        case SSA_OP_BUFFER_APPEND:
            expected_operands = 2;
            result_type = inst->type;
            has_result = true;
            break;
        case SSA_OP_BUFFER_POP:
            expected_operands = 1;
            result_type = inst->type;
            has_result = true;
            break;
        case SSA_OP_BUFFER_FREE:
            expected_operands = 1;
            break;
        case SSA_OP_DICT_ALLOC:
            expected_operands = 1;
            result_type = inst->type;
            has_result = true;
            break;
        case SSA_OP_DICT_SET:
            expected_operands = 2;
            result_type = inst->type;
            has_result = true;
            break;
        case SSA_OP_DICT_GET:
            expected_operands = 2;
            result_type = inst->type;
            has_result = true;
            break;
        case SSA_OP_DICT_HAS:
            expected_operands = 1;
            result_type = i64;
            has_result = true;
            break;
        case SSA_OP_DICT_DELETE:
            expected_operands = 1;
            result_type = inst->type;
            has_result = true;
            break;
        case SSA_OP_DICT_POP:
            expected_operands = 2;
            result_type = inst->type;
            has_result = true;
            break;
        case SSA_OP_DICT_LEN:
            expected_operands = 1;
            result_type = i64;
            has_result = true;
            break;
        case SSA_OP_DICT_FREE:
            expected_operands = 1;
            break;
        case SSA_OP_STRING_CONCAT:
            expected_operands = 2;
            result_type = inst->type;
            has_result = true;
            break;
        case SSA_OP_SUM_PAYLOAD_STORE:
            expected_operands = 2;
            break;
        case SSA_OP_SUM_PAYLOAD_LOAD:
            expected_operands = 1;
            result_type = inst->type;
            has_result = true;
            break;
        case SSA_OP_SUM_MOVE:
            expected_operands = 2;
            break;
        case SSA_OP_SUM_DROP:
            expected_operands = 1;
            break;
        case SSA_OP_FIELD_PAYLOAD_STORE:
            expected_operands = 2;
            break;
        case SSA_OP_FIELD_PAYLOAD_LOAD:
            expected_operands = 1;
            result_type = inst->type;
            has_result = true;
            break;
        case SSA_OP_AGG_MOVE:
            expected_operands = 2;
            break;
        case SSA_OP_AGG_DROP:
            expected_operands = 1;
            break;
        case SSA_OP_SUM_CHECK:
            expected_operands = 1;
            break;
        case SSA_OP_PRINT_I64:
            expected_operands = 1;
            operands_are_i64 = true;
            break;
        case SSA_OP_PRINT_STRING:
            expected_operands = 1;
            break;
        case SSA_OP_ASSERT:
            expected_operands = 1;
            break;
        case SSA_OP_JUMP:
            break;
        case SSA_OP_BRANCH:
            expected_operands = 1;
            operands_are_bool = true;
            break;
        case SSA_OP_RETURN:
            if (inst->operand_count > 1) {
                verr(ctx, "block b%u return has too many operands", ref);
                return false;
            }
            expected_operands = inst->operand_count;
            break;
        default:
            verr(ctx, "block b%u instruction i%u has an unknown opcode", ref, iref);
            return false;
    }
    if (inst->operand_count != expected_operands) {
        verr(ctx, "block b%u %s has %u operands, expected %zu",
             ref, opname, inst->operand_count, expected_operands);
        return false;
    }
    if (operands_are_i64) {
        for (size_t i = 0; i < expected_operands; i++) {
            if (!check_value_type(ctx, ops[i], i64, "integer opcode operand")) return false;
        }
    }
    if (operands_are_numeric) {
        const CobraType *numeric_type = arena->values[ops[0]].type;
        bool is_comparison = inst->op >= SSA_OP_EQ && inst->op <= SSA_OP_GE;
        /* Integer-backed unit enums compare on their discriminants. */
        bool enum_comparison = is_comparison &&
                               numeric_type &&
                               numeric_type->kind == COBRA_TYPE_ENUM;
        bool numeric_ok = numeric_type == i64 ||
                          numeric_type == ctx->module->type_i32 ||
                          numeric_type == ctx->module->type_u32 ||
                          numeric_type == ctx->module->type_u64 ||
                          numeric_type == ctx->module->type_u8 ||
                          numeric_type == ctx->module->type_f32 ||
                          numeric_type == ctx->module->type_f64;
        if (!numeric_ok && !enum_comparison) {
            verr(ctx, "numeric opcode requires an integer or float scalar operand");
            return false;
        }
        if (inst->op == SSA_OP_REM &&
            (numeric_type == ctx->module->type_f32 ||
             numeric_type == ctx->module->type_f64)) {
            verr(ctx, "remainder is only defined for integer operands");
            return false;
        }
        for (size_t i = 1; i < expected_operands; i++) {
            if (!check_value_type(ctx, ops[i], numeric_type, "numeric opcode operand")) return false;
        }
        if (!result_type) result_type = numeric_type;
    }
    if (inst->op == SSA_OP_STACK_SLOT &&
        (inst->pointer_contract != BIR_POINTER_CONTRACT_OWNED_FRAME &&
         inst->pointer_contract != BIR_POINTER_CONTRACT_OWNED_REGION)) {
        verr(ctx, "block b%u stack slot has an invalid ownership contract", ref);
        return false;
    }
    if ((inst->op == SSA_OP_REGION_ENTER || inst->op == SSA_OP_REGION_EXIT) &&
        (!find_region(ctx->module, inst->region_id) ||
         (inst->op == SSA_OP_REGION_ENTER && inst->parent_region_id != 0 &&
          !find_region(ctx->module, inst->parent_region_id)))) {
        verr(ctx, "block b%u region operation references an undeclared region", ref);
        return false;
    }
    if (inst->op == SSA_OP_ARRAY_INDEX_ADDR) {
        const SsaValue *base = &arena->values[ops[0]];
        if (!is_pointer_type(base->type) || base->type->generic_arg_count != 1 ||
            !is_memory_aggregate(inst->memory_type) ||
            inst->memory_type->kind != COBRA_TYPE_ARRAY ||
            !type_matches(base->type->generic_args[0], inst->memory_type) ||
            !check_value_type(ctx, ops[1], i64, "fixed array index") ||
            !is_pointer_type(inst->type) || inst->type->generic_arg_count != 1 ||
            !type_matches(inst->type->generic_args[0],
                          inst->memory_type->generic_args[0]) ||
            inst->memory_width != inst->memory_type->generic_args[0]->size ||
            inst->memory_alignment != inst->memory_type->generic_args[0]->alignment ||
            !pointer_contract_matches(base, inst->pointer_contract)) {
            verr(ctx, "block b%u array index address has invalid bounds or pointer metadata", ref);
            return false;
        }
    }
    if (inst->op == SSA_OP_PTR_ADD) {
        if (!is_pointer_type(inst->type) || !is_pointer_type(arena->values[ops[0]].type) ||
            !check_value_type(ctx, ops[0], inst->type, "pointer arithmetic operand") ||
            !check_value_type(ctx, ops[1], i64, "pointer arithmetic offset") ||
            !pointer_contract_matches(&arena->values[ops[0]], inst->pointer_contract)) {
            verr(ctx, "block b%u pointer arithmetic changes pointer ownership contract", ref);
            return false;
        }
    }
    if (inst->op == SSA_OP_LOAD || inst->op == SSA_OP_STORE) {
        if (            !(is_memory_scalar(inst->memory_type) || is_view_type(inst->memory_type)) ||
            !is_pointer_type(arena->values[ops[0]].type) ||
            !type_matches(arena->values[ops[0]].type->generic_args[0], inst->memory_type)) {
            verr(ctx, "block b%u %s has an invalid typed pointer", ref, opname);
            return false;
        }
        BirPointerContract required = inst->op == SSA_OP_LOAD
            ? BIR_POINTER_CONTRACT_BORROW_READONLY
            : BIR_POINTER_CONTRACT_BORROW_WRITE;
        if (!pointer_contract_matches(&arena->values[ops[0]], required)) {
            verr(ctx, "block b%u %s violates pointer borrow contract", ref, opname);
            return false;
        }
        if (inst->op == SSA_OP_STORE &&
            !check_value_type(ctx, ops[1], inst->memory_type, "store value")) return false;
        if (inst->op == SSA_OP_STORE && is_view_type(inst->memory_type)) {
            const SsaValue *view = &arena->values[ops[1]];
            if (view->pointer_contract != view_contract(inst->memory_type) ||
                !pointer_origin_matches(view)) {
                verr(ctx, "block b%u view field store loses borrow provenance", ref);
                return false;
            }
        }
    }
    if (inst->op == SSA_OP_FIELD_ADDR) {
        if (!field_metadata_matches(inst) || !is_pointer_type(arena->values[ops[0]].type) ||
            !type_matches(arena->values[ops[0]].type->generic_args[0], inst->memory_type) ||
            !pointer_contract_matches(&arena->values[ops[0]],
                                      BIR_POINTER_CONTRACT_BORROW_READONLY) ||
            inst->pointer_contract != arena->values[ops[0]].pointer_contract) {
            verr(ctx, "block b%u field address has invalid aggregate borrow metadata", ref);
            return false;
        }
    }
    if (inst->op == SSA_OP_TRANSFER) {
        if (!is_pointer_type(inst->type) || !is_pointer_type(arena->values[ops[0]].type) ||
            !type_matches(arena->values[ops[0]].type, inst->type) ||
            !pointer_contract_matches(&arena->values[ops[0]],
                                      BIR_POINTER_CONTRACT_OWNED_REGION) ||
            arena->values[ops[0]].pointer_origin != BIR_POINTER_ORIGIN_REGION ||
            arena->values[ops[0]].region_id == BIR_REGION_NONE ||
            inst->region_id == BIR_REGION_NONE ||
            !find_region(ctx->module, inst->region_id) ||
            inst->pointer_contract != BIR_POINTER_CONTRACT_OWNED_REGION ||
            inst->pointer_origin != BIR_POINTER_ORIGIN_REGION ||
            inst->allocation_id != arena->values[ops[0]].allocation_id) {
            verr(ctx, "block b%u transfer violates region ownership", ref);
            return false;
        }
    }
    if (inst->op == SSA_OP_VIEW_MAKE) {
        const SsaValue *pointer = &arena->values[ops[0]];
        if (!is_view_type(inst->type) || !is_pointer_type(pointer->type) ||
            !type_matches(pointer->type->generic_args[0], inst->memory_type) ||
            !check_value_type(ctx, ops[1], i64, "view length") ||
            !pointer_contract_matches(pointer,
                view_contract(inst->type) == BIR_POINTER_CONTRACT_BORROW_WRITE
                    ? BIR_POINTER_CONTRACT_BORROW_WRITE
                    : BIR_POINTER_CONTRACT_BORROW_READONLY) ||
            inst->pointer_contract != view_contract(inst->type) ||
            inst->pointer_origin != pointer->pointer_origin ||
            inst->region_id != pointer->region_id ||
            inst->allocation_id != pointer->allocation_id) {
            verr(ctx, "block b%u view construction violates readonly borrow metadata", ref);
            return false;
        }
    }
    if (inst->op == SSA_OP_VIEW_PTR) {
        const SsaValue *view = &arena->values[ops[0]];
        BirPointerContract expected_contract = is_owned_slice_type(view->type)
            ? BIR_POINTER_CONTRACT_OWNED_SLICE
            : view_contract(view->type);
        if ((!is_view_type(view->type) && !is_owned_slice_type(view->type)) ||
            !is_pointer_type(inst->type) ||
            !type_matches(view->type->generic_args[0], inst->memory_type) ||
            !type_matches(inst->type->generic_args[0], inst->memory_type) ||
            inst->pointer_contract != expected_contract ||
            inst->pointer_origin != view->pointer_origin ||
            inst->region_id != view->region_id ||
            inst->allocation_id != view->allocation_id) {
            verr(ctx, "block b%u view pointer extraction violates slice borrow metadata", ref);
            return false;
        }
    }
    if (inst->op == SSA_OP_VIEW_LEN) {
        if (!is_view_type(arena->values[ops[0]].type) &&
            !is_owned_slice_type(arena->values[ops[0]].type)) {
            verr(ctx, "block b%u view length requires a slice view", ref);
            return false;
        }
    }
    if (inst->op == SSA_OP_SLICE_ALLOC) {
        bool region_owned = inst->pointer_origin == BIR_POINTER_ORIGIN_REGION &&
                            inst->region_id != BIR_REGION_NONE &&
                            find_region(ctx->module, inst->region_id);
        bool frame_owned = inst->pointer_origin == BIR_POINTER_ORIGIN_FRAME &&
                           inst->region_id == BIR_REGION_NONE;
        if (!is_owned_slice_type(inst->type) ||
            !is_memory_scalar(inst->memory_type) ||
            !type_matches(cobra_type_element(inst->type), inst->memory_type) ||
            !check_value_type(ctx, ops[0], i64, "slice allocation length") ||
            inst->pointer_contract != BIR_POINTER_CONTRACT_OWNED_SLICE ||
            (!frame_owned && !region_owned) ||
            inst->allocation_id == 0 ||
            inst->allocation_id > BIR_MAX_STACK_SLOTS) {
            verr(ctx, "block b%u slice allocation has invalid owned-slice metadata", ref);
            return false;
        }
    }
    if (inst->op == SSA_OP_SLICE_FREE) {
        const SsaValue *slice = &arena->values[ops[0]];
        if (!is_owned_slice_type(slice->type) ||
            slice->type->kind == COBRA_TYPE_LIST ||
            (slice->pointer_contract != BIR_POINTER_CONTRACT_OWNED_SLICE &&
             slice->pointer_contract != BIR_POINTER_CONTRACT_UNKNOWN) ||
            slice->allocation_id == 0 ||
            slice->allocation_id > BIR_MAX_STACK_SLOTS) {
            verr(ctx, "block b%u slice free requires a live owned slice", ref);
            return false;
        }
    }
    if (inst->op == SSA_OP_BUFFER_ALLOC) {
        if (!bir_is_owned_buffer_type(inst->type) ||
            !is_memory_type(inst->memory_type) ||
            !type_matches(cobra_type_element(inst->type), inst->memory_type) ||
            !check_value_type(ctx, ops[0], i64, "buffer allocation length") ||
            inst->pointer_contract != BIR_POINTER_CONTRACT_OWNED_SLICE ||
            inst->pointer_origin != BIR_POINTER_ORIGIN_FRAME ||
            inst->region_id != BIR_REGION_NONE || inst->allocation_id == 0 ||
            inst->allocation_id > BIR_MAX_STACK_SLOTS) {
            verr(ctx, "block b%u buffer allocation has invalid metadata", ref);
            return false;
        }
    }
    if (inst->op == SSA_OP_BUFFER_APPEND) {
        const SsaValue *buffer = &arena->values[ops[0]];
        const SsaValue *value = &arena->values[ops[1]];
        bool scalar_element = is_memory_scalar(inst->memory_type);
        bool aggregate_element = !scalar_element &&
            is_memory_aggregate(inst->memory_type);
        bool value_ok = scalar_element
            ? type_matches(value->type, inst->memory_type)
            : (is_pointer_type(value->type) && value->type->generic_arg_count == 1 &&
               type_matches(value->type->generic_args[0], inst->memory_type));
        if (!bir_is_owned_buffer_type(buffer->type) ||
            !bir_is_owned_buffer_type(inst->type) ||
            !type_matches(buffer->type, inst->type) ||
            !(scalar_element || aggregate_element) || !value_ok ||
            buffer->pointer_contract != BIR_POINTER_CONTRACT_OWNED_SLICE ||
            inst->pointer_contract != BIR_POINTER_CONTRACT_OWNED_SLICE ||
            inst->allocation_id == 0 ||
            inst->allocation_id > BIR_MAX_STACK_SLOTS ||
            inst->effect != SSA_EFFECT_READWRITE) {
            verr(ctx, "block b%u buffer append has invalid ownership metadata", ref);
            return false;
        }
    }
    if (inst->op == SSA_OP_BUFFER_POP) {
        const SsaValue *buffer = &arena->values[ops[0]];
        if (!bir_is_owned_buffer_type(buffer->type) ||
            !is_memory_scalar(inst->type) ||
            !type_matches(cobra_type_element(buffer->type), inst->type) ||
            inst->memory_type != inst->type || inst->effect != SSA_EFFECT_READWRITE) {
            verr(ctx, "block b%u buffer pop has invalid metadata", ref);
            return false;
        }
    }
    if (inst->op == SSA_OP_BUFFER_FREE) {
        const SsaValue *buffer = &arena->values[ops[0]];
        if (!bir_is_owned_buffer_type(buffer->type) ||
            buffer->pointer_contract != BIR_POINTER_CONTRACT_OWNED_SLICE) {
            verr(ctx, "block b%u buffer free requires a live owned buffer", ref);
            return false;
        }
    }
    if (inst->op == SSA_OP_DICT_ALLOC) {
        if (!bir_is_owned_dict_type(inst->type) ||
            !is_memory_scalar(inst->memory_type) ||
            !type_matches(inst->type->generic_args[1], inst->memory_type) ||
            !check_value_type(ctx, ops[0], i64, "dict allocation capacity") ||
            inst->pointer_contract != BIR_POINTER_CONTRACT_OWNED_SLICE ||
            inst->pointer_origin != BIR_POINTER_ORIGIN_FRAME ||
            inst->region_id != BIR_REGION_NONE || inst->allocation_id == 0 ||
            inst->allocation_id > BIR_MAX_STACK_SLOTS) {
            verr(ctx, "block b%u dict allocation has invalid metadata", ref);
            return false;
        }
    }
    if (inst->op == SSA_OP_DICT_SET) {
        const SsaValue *dict = &arena->values[ops[0]];
        const SsaValue *value = &arena->values[ops[1]];
        if (!bir_is_owned_dict_type(dict->type) ||
            !bir_is_owned_dict_type(inst->type) ||
            !type_matches(dict->type, inst->type) ||
            inst->dict_key[0] == '\0' ||
            !type_matches(value->type, inst->memory_type) ||
            !type_matches(inst->type->generic_args[1], inst->memory_type) ||
            dict->pointer_contract != BIR_POINTER_CONTRACT_OWNED_SLICE ||
            inst->pointer_contract != BIR_POINTER_CONTRACT_OWNED_SLICE ||
            inst->allocation_id == 0 ||
            inst->allocation_id > BIR_MAX_STACK_SLOTS ||
            inst->effect != SSA_EFFECT_READWRITE) {
            verr(ctx, "block b%u dict set has invalid ownership metadata", ref);
            return false;
        }
    }
    if (inst->op == SSA_OP_DICT_GET) {
        const SsaValue *dict = &arena->values[ops[0]];
        const SsaValue *fallback = &arena->values[ops[1]];
        if (!bir_is_owned_dict_type(dict->type) ||
            inst->dict_key[0] == '\0' ||
            !type_matches(inst->type, dict->type->generic_args[1]) ||
            !type_matches(fallback->type, inst->type) ||
            inst->effect != SSA_EFFECT_READ) {
            verr(ctx, "block b%u dict get has invalid metadata", ref);
            return false;
        }
    }
    if (inst->op == SSA_OP_DICT_HAS) {
        const SsaValue *dict = &arena->values[ops[0]];
        if (!bir_is_owned_dict_type(dict->type) ||
            inst->dict_key[0] == '\0' ||
            inst->type != i64 || inst->effect != SSA_EFFECT_READ) {
            verr(ctx, "block b%u dict has has invalid metadata", ref);
            return false;
        }
    }
    if (inst->op == SSA_OP_DICT_DELETE) {
        const SsaValue *dict = &arena->values[ops[0]];
        if (!bir_is_owned_dict_type(dict->type) ||
            !bir_is_owned_dict_type(inst->type) ||
            !type_matches(dict->type, inst->type) ||
            inst->dict_key[0] == '\0' ||
            dict->pointer_contract != BIR_POINTER_CONTRACT_OWNED_SLICE ||
            inst->pointer_contract != BIR_POINTER_CONTRACT_OWNED_SLICE ||
            inst->allocation_id == 0 ||
            inst->allocation_id > BIR_MAX_STACK_SLOTS ||
            inst->effect != SSA_EFFECT_READWRITE) {
            verr(ctx, "block b%u dict delete has invalid ownership metadata", ref);
            return false;
        }
    }
    if (inst->op == SSA_OP_DICT_POP) {
        const SsaValue *dict = &arena->values[ops[0]];
        const SsaValue *fallback = &arena->values[ops[1]];
        if (!bir_is_owned_dict_type(dict->type) ||
            !bir_is_owned_dict_type(inst->memory_type) ||
            !type_matches(dict->type, inst->memory_type) ||
            inst->dict_key[0] == '\0' ||
            !type_matches(inst->type, dict->type->generic_args[1]) ||
            !type_matches(fallback->type, inst->type) ||
            dict->pointer_contract != BIR_POINTER_CONTRACT_OWNED_SLICE ||
            inst->effect != SSA_EFFECT_READWRITE) {
            verr(ctx, "block b%u dict pop has invalid ownership metadata", ref);
            return false;
        }
    }
    if (inst->op == SSA_OP_DICT_LEN) {
        const SsaValue *dict = &arena->values[ops[0]];
        if (!bir_is_owned_dict_type(dict->type) ||
            inst->type != i64 || inst->effect != SSA_EFFECT_READ) {
            verr(ctx, "block b%u dict len has invalid metadata", ref);
            return false;
        }
    }
    if (inst->op == SSA_OP_DICT_FREE) {
        const SsaValue *dict = &arena->values[ops[0]];
        if (!bir_is_owned_dict_type(dict->type) ||
            dict->pointer_contract != BIR_POINTER_CONTRACT_OWNED_SLICE) {
            verr(ctx, "block b%u dict free requires a live owned dict", ref);
            return false;
        }
    }
    if (inst->op == SSA_OP_DESTROY) {
        const SsaValue *pointer = &arena->values[ops[0]];
        if (!is_pointer_type(pointer->type) || pointer->allocation_id == 0 ||
            (pointer->pointer_contract != BIR_POINTER_CONTRACT_OWNED_FRAME &&
             pointer->pointer_contract != BIR_POINTER_CONTRACT_OWNED_REGION) ||
            (pointer->pointer_origin != BIR_POINTER_ORIGIN_FRAME &&
             pointer->pointer_origin != BIR_POINTER_ORIGIN_REGION)) {
            verr(ctx, "block b%u destroy requires a live owned allocation", ref);
            return false;
        }
    }
    if (inst->op == SSA_OP_SUM_CHECK) {
        if (!check_value_type(ctx, ops[0], i64, "sum tag") ||
            inst->sum_check_kind < 0 || inst->sum_check_kind > 3) {
            verr(ctx, "block b%u sum check has an invalid tag operand or kind", ref);
            return false;
        }
    }
    if (inst->op == SSA_OP_AGG_COPY) {
        if (!is_memory_aggregate(inst->memory_type) ||
            (bir_is_sum_type(inst->memory_type) &&
             bir_sum_has_owned_payload(inst->memory_type)) ||
            !is_pointer_type(arena->values[ops[0]].type) ||
            !is_pointer_type(arena->values[ops[1]].type) ||
            !type_matches(arena->values[ops[0]].type->generic_args[0], inst->memory_type) ||
            !type_matches(arena->values[ops[1]].type->generic_args[0], inst->memory_type) ||
            !pointer_contract_matches(&arena->values[ops[0]],
                                      BIR_POINTER_CONTRACT_BORROW_WRITE) ||
            !pointer_contract_matches(&arena->values[ops[1]],
                                      BIR_POINTER_CONTRACT_BORROW_READONLY)) {
            verr(ctx, "block b%u aggregate copy violates source/destination borrow contracts", ref);
            return false;
        }
    }
    if (operands_are_bool &&
        !check_value_type(ctx, ops[0], boolean, "branch condition")) return false;
    if (has_result) {
        if (inst->result == SSA_VALUE_NONE || !inst->type || !result_type ||
            !type_matches(inst->type, result_type) ||
            !check_value_type(ctx, inst->result, result_type, "instruction result") ||
            ((is_pointer_type(inst->type) || is_view_type(inst->type) ||
              is_owned_slice_type(inst->type)) &&
             arena->values[inst->result].pointer_contract != inst->pointer_contract)) {
            verr(ctx, "block b%u %s has an invalid result type", ref, opname);
            return false;
        }
    } else if (inst->result != SSA_VALUE_NONE || inst->type) {
        verr(ctx, "block b%u %s unexpectedly produces a result", ref, opname);
        return false;
    }
    if ((inst->op == SSA_OP_REGION_ENTER || inst->op == SSA_OP_REGION_EXIT ||
         inst->op == SSA_OP_DESTROY || inst->op == SSA_OP_SLICE_ALLOC ||
         inst->op == SSA_OP_SLICE_FREE) &&
        (inst->effect != SSA_EFFECT_NONE ||
         (inst->op != SSA_OP_SLICE_ALLOC &&
         inst->op != SSA_OP_BUFFER_ALLOC &&
          inst->pointer_contract != BIR_POINTER_CONTRACT_UNKNOWN))) {
        verr(ctx, "block b%u lifetime instruction has unexpected metadata", ref);
        return false;
    }
    if (inst->op == SSA_OP_STRING_CONCAT &&
        (!is_owned_slice_type(inst->type) ||
         cobra_type_element(inst->type) == NULL ||
         cobra_type_element(inst->type)->kind != COBRA_TYPE_U8 ||
         inst->pointer_contract != BIR_POINTER_CONTRACT_OWNED_SLICE ||
         inst->allocation_id == 0)) {
        verr(ctx, "block b%u string concatenation has invalid owned-string metadata", ref);
        return false;
    }
    if (inst->op == SSA_OP_SUM_PAYLOAD_STORE) {
        const SsaValue *destination = &arena->values[ops[0]];
        const SsaValue *payload = &arena->values[ops[1]];
        if (!is_pointer_type(destination->type) ||
            !is_owned_slice_type(payload->type) ||
            destination->type->generic_arg_count != 1 ||
            !bir_sum_has_owned_payload(destination->type->generic_args[0]) ||
            !pointer_contract_matches(destination, BIR_POINTER_CONTRACT_BORROW_WRITE) ||
            payload->pointer_contract != BIR_POINTER_CONTRACT_OWNED_SLICE ||
            payload->allocation_id == 0 || inst->memory_type != payload->type ||
            inst->memory_offset < 0 || inst->effect != SSA_EFFECT_READWRITE) {
            verr(ctx, "block b%u sum payload store has invalid ownership metadata", ref);
            return false;
        }
    }
    if (inst->op == SSA_OP_SUM_PAYLOAD_LOAD) {
        const SsaValue *source = &arena->values[ops[0]];
        if (!is_pointer_type(source->type) || source->type->generic_arg_count != 1 ||
            !bir_sum_has_owned_payload(source->type->generic_args[0]) ||
            !pointer_contract_matches(source, BIR_POINTER_CONTRACT_BORROW_READONLY) ||
            !is_owned_slice_type(inst->type) ||
            inst->pointer_contract != BIR_POINTER_CONTRACT_OWNED_SLICE ||
            inst->allocation_id == 0 || inst->memory_type != inst->type ||
            inst->result == SSA_VALUE_NONE || inst->effect != SSA_EFFECT_READWRITE) {
            verr(ctx, "block b%u sum payload load has invalid ownership metadata", ref);
            return false;
        }
    }
    if (inst->op == SSA_OP_FIELD_PAYLOAD_STORE) {
        const SsaValue *destination = &arena->values[ops[0]];
        const SsaValue *payload = &arena->values[ops[1]];
        if (!is_pointer_type(destination->type) || !inst->aggregate_type ||
            !is_memory_aggregate(inst->aggregate_type) ||
            !is_owned_slice_type(inst->memory_type) ||
            !bir_type_has_owned_payload(inst->aggregate_type) ||
            !pointer_contract_matches(destination, BIR_POINTER_CONTRACT_BORROW_WRITE) ||
            payload->type != inst->memory_type ||
            payload->pointer_contract != BIR_POINTER_CONTRACT_OWNED_SLICE ||
            payload->allocation_id == 0 || inst->memory_offset < 0 ||
            inst->effect != SSA_EFFECT_READWRITE ||
            !aggregate_field_matches(inst->aggregate_type, inst->memory_offset,
                                      inst->memory_type, inst->memory_width,
                                      inst->memory_alignment)) {
            verr(ctx, "block b%u owned field store has invalid ownership metadata", ref);
            return false;
        }
    }
    if (inst->op == SSA_OP_FIELD_PAYLOAD_LOAD) {
        const SsaValue *source = &arena->values[ops[0]];
        if (!is_pointer_type(source->type) || !inst->aggregate_type ||
            !is_memory_aggregate(inst->aggregate_type) ||
            !is_owned_slice_type(inst->type) ||
            !bir_type_has_owned_payload(inst->aggregate_type) ||
            !pointer_contract_matches(source, BIR_POINTER_CONTRACT_BORROW_READONLY) ||
            inst->memory_type != inst->type || inst->pointer_contract != BIR_POINTER_CONTRACT_OWNED_SLICE ||
            inst->allocation_id == 0 || inst->result == SSA_VALUE_NONE ||
            inst->effect != SSA_EFFECT_READWRITE) {
            verr(ctx, "block b%u owned field load has invalid ownership metadata", ref);
            return false;
        }
    }
    if (inst->op == SSA_OP_SUM_MOVE || inst->op == SSA_OP_SUM_DROP) {
        const SsaValue *source = &arena->values[ops[inst->op == SSA_OP_SUM_MOVE ? 1 : 0]];
        if (!is_pointer_type(source->type) || source->type->generic_arg_count != 1 ||
            !bir_sum_has_owned_payload(source->type->generic_args[0]) ||
            !pointer_contract_matches(source,
                                      inst->op == SSA_OP_SUM_DROP
                                          ? BIR_POINTER_CONTRACT_BORROW_WRITE
                                          : BIR_POINTER_CONTRACT_BORROW_READONLY) ||
            inst->effect != SSA_EFFECT_READWRITE) {
            verr(ctx, "block b%u sum ownership operation has invalid storage", ref);
            return false;
        }
        if (inst->op == SSA_OP_SUM_MOVE) {
            const SsaValue *destination = &arena->values[ops[0]];
            if (!is_pointer_type(destination->type) ||
                destination->type->generic_arg_count != 1 ||
                !pointer_contract_matches(destination,
                                          BIR_POINTER_CONTRACT_BORROW_WRITE) ||
                !type_matches(destination->type->generic_args[0],
                              source->type->generic_args[0])) {
                verr(ctx, "block b%u sum move has mismatched storage", ref);
                return false;
            }
        }
    }
    if (inst->op == SSA_OP_AGG_MOVE || inst->op == SSA_OP_AGG_DROP) {
        const SsaValue *source = &arena->values[
            ops[inst->op == SSA_OP_AGG_MOVE ? 1 : 0]];
        if (!inst->aggregate_type || !is_memory_aggregate(inst->aggregate_type) ||
            !bir_type_has_owned_payload(inst->aggregate_type) ||
            !is_pointer_type(source->type) || source->type->generic_arg_count != 1 ||
            !type_matches(source->type->generic_args[0], inst->aggregate_type) ||
            !pointer_contract_matches(source,
                inst->op == SSA_OP_AGG_DROP
                    ? BIR_POINTER_CONTRACT_BORROW_WRITE
                    : BIR_POINTER_CONTRACT_BORROW_READONLY) ||
            inst->effect != SSA_EFFECT_READWRITE) {
            verr(ctx, "block b%u aggregate ownership operation has invalid storage", ref);
            return false;
        }
        if (inst->op == SSA_OP_AGG_MOVE) {
            const SsaValue *destination = &arena->values[ops[0]];
            if (!is_pointer_type(destination->type) ||
                !type_matches(destination->type->generic_args[0], inst->aggregate_type) ||
                !pointer_contract_matches(destination, BIR_POINTER_CONTRACT_BORROW_WRITE)) {
                verr(ctx, "block b%u aggregate move has invalid destination", ref);
                return false;
            }
        }
    }
    if (inst->op == SSA_OP_TRANSFER &&
        (inst->effect != SSA_EFFECT_NONE || inst->result == SSA_VALUE_NONE ||
         !type_matches(inst->type, arena->values[inst->result].type) ||
         arena->values[inst->result].pointer_origin != BIR_POINTER_ORIGIN_REGION ||
         arena->values[inst->result].region_id != inst->region_id)) {
        verr(ctx, "block b%u transfer result has invalid provenance", ref);
        return false;
    }
    return true;
}

static bool check_edge_args(VerifyCtx *ctx, SsaBlockRef pred,
                            SsaBlockRef target, uint32_t start, uint32_t count) {
    const SsaArena *arena = &ctx->module->arena;
    const SsaBlock *successor = &arena->blocks[target];
    if ((size_t)start > arena->edge_used ||
        (size_t)count > arena->edge_used - (size_t)start) {
        verr(ctx, "block b%u has an out-of-range edge argument window", pred);
        return false;
    }
    if (count != successor->param_count) {
        verr(ctx, "block b%u edge args (%u) do not match block b%u params (%zu)",
             pred, count, target, successor->param_count);
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        SsaValueRef argument = arena->edges[start + i];
        SsaValueRef parameter = successor->params[i];
        if (!valid_value(ctx, parameter) || !valid_value(ctx, argument) ||
            !type_matches(arena->values[argument].type, arena->values[parameter].type)) {
            verr(ctx, "block b%u edge argument %zu has the wrong target block-parameter type",
                 pred, i + 1);
            return false;
        }
    }
    return true;
}

static bool check_block(VerifyCtx *ctx, SsaBlockRef ref) {
    const SsaArena *arena = &ctx->module->arena;
    const SsaBlock *block = &arena->blocks[ref];

    if (block->terminator == SSA_INST_NONE) {
        verr(ctx, "block b%u has no terminator", ref);
        return false;
    }
    if (block->inst_count == 0 || block->insts[block->inst_count - 1] != block->terminator) {
        verr(ctx, "block b%u terminator is not its last instruction", ref);
        return false;
    }
    for (size_t i = 0; i < block->inst_count; i++) {
        SsaInstRef iref = block->insts[i];
        if (iref == SSA_INST_NONE || iref >= arena->inst_count) {
            verr(ctx, "block b%u references an invalid instruction", ref);
            return false;
        }
        const SsaInst *inst = &arena->insts[iref];
        if (inst->op == SSA_OP_NONE) {
            verr(ctx, "block b%u instruction i%u has no opcode", ref, iref);
            return false;
        }
        if (i + 1 < block->inst_count && bir_is_terminator(inst->op)) {
            verr(ctx, "block b%u has a terminator before its last instruction", ref);
            return false;
        }
        if ((size_t)inst->operand_start > arena->operand_used ||
            (size_t)inst->operand_count > arena->operand_used - (size_t)inst->operand_start) {
            verr(ctx, "block b%u instruction i%u has an out-of-range operand window",
                 ref, iref);
            return false;
        }
        for (size_t o = 0; o < inst->operand_count; o++) {
            SsaValueRef operand = arena->operands[inst->operand_start + o];
            if (!valid_value(ctx, operand)) {
                verr(ctx, "block b%u instruction i%u references invalid operand v%u",
                     ref, iref, operand);
                return false;
            }
        }
        if (!check_instruction_signature(ctx, ref, iref, inst)) return false;
        if (inst->op == SSA_OP_CALL && inst->effect != SSA_EFFECT_CALL) {
            verr(ctx, "block b%u call has no call effect", ref);
            return false;
        }        if (inst->effect != SSA_EFFECT_NONE &&
            inst->op != SSA_OP_LOAD && inst->op != SSA_OP_STORE &&
            inst->op != SSA_OP_AGG_COPY && inst->op != SSA_OP_CALL &&
            inst->op != SSA_OP_SUM_PAYLOAD_STORE &&
            inst->op != SSA_OP_SUM_PAYLOAD_LOAD &&
            inst->op != SSA_OP_SUM_MOVE &&
            inst->op != SSA_OP_SUM_DROP &&
            inst->op != SSA_OP_FIELD_PAYLOAD_STORE &&
            inst->op != SSA_OP_FIELD_PAYLOAD_LOAD &&
            inst->op != SSA_OP_AGG_MOVE &&
            inst->op != SSA_OP_AGG_DROP &&
            inst->op != SSA_OP_BUFFER_APPEND &&
            inst->op != SSA_OP_BUFFER_POP &&
            inst->op != SSA_OP_DICT_ALLOC &&
            inst->op != SSA_OP_DICT_SET &&
            inst->op != SSA_OP_DICT_GET &&
            inst->op != SSA_OP_DICT_HAS &&
            inst->op != SSA_OP_DICT_DELETE &&
            inst->op != SSA_OP_DICT_POP &&
            inst->op != SSA_OP_DICT_LEN) {
            verr(ctx, "block b%u instruction i%u carries an effect on a pure opcode", ref, iref);
            return false;
        }
        if (inst->op == SSA_OP_STORE && inst->operand_count != 2) {
            verr(ctx, "block b%u store must have exactly two operands", ref);
            return false;
        }
        if (inst->op == SSA_OP_LOAD && inst->operand_count != 1) {
            verr(ctx, "block b%u load must have exactly one operand", ref);
            return false;
        }
        if (inst->op == SSA_OP_STACK_SLOT ||
            inst->op == SSA_OP_FIELD_ADDR || inst->op == SSA_OP_LOAD ||
            inst->op == SSA_OP_STORE || inst->op == SSA_OP_AGG_COPY) {
            SsaEffect expected = inst->op == SSA_OP_LOAD ? SSA_EFFECT_READ :
                                 inst->op == SSA_OP_STORE ? SSA_EFFECT_WRITE :
                                 inst->op == SSA_OP_AGG_COPY ? SSA_EFFECT_READWRITE :
                                 SSA_EFFECT_NONE;
            bool aggregate = inst->op == SSA_OP_AGG_COPY ||
                             inst->op == SSA_OP_FIELD_ADDR ||
                             (inst->op == SSA_OP_STACK_SLOT &&
                              !is_memory_scalar(inst->memory_type) &&
                              !is_view_type(inst->memory_type));
            bool shape_valid = false;
            if (inst->memory_type && inst->memory_type->finalized) {
                shape_valid = inst->op == SSA_OP_FIELD_ADDR
                    ? (inst->memory_offset >= 0 && field_metadata_matches(inst))
                    : (inst->memory_width == inst->memory_type->size &&
                       inst->memory_alignment == inst->memory_type->alignment);
            }
            if (inst->effect != expected ||
                inst->address_kind != SSA_ADDRESS_TYPED_POINTER ||
                inst->memory_type == NULL || !inst->memory_type->finalized ||
                (aggregate ? !is_memory_aggregate(inst->memory_type)
                           : !(is_memory_scalar(inst->memory_type) ||
                                is_view_type(inst->memory_type))) ||
                !shape_valid ||
                inst->memory_alignment == 0 ||
                (inst->memory_alignment & (inst->memory_alignment - 1)) != 0 ||
                inst->address_space != 0) {
                verr(ctx, "block b%u typed memory instruction has invalid metadata", ref);
                return false;
            }
            if (inst->op == SSA_OP_STACK_SLOT &&
                (inst->memory_offset < 0 ||
                 inst->memory_offset > BIR_STACK_BYTES - (int64_t)inst->memory_width ||
                 inst->memory_offset % (int64_t)inst->memory_alignment != 0 ||
                 inst->stack_slot >= BIR_MAX_STACK_SLOTS)) {
                verr(ctx, "block b%u stack slot has an invalid offset or alignment", ref);
                return false;
            }
            if (inst->op == SSA_OP_FIELD_ADDR &&
                inst->memory_offset > BIR_STACK_BYTES - (int64_t)inst->memory_width) {
                verr(ctx, "block b%u field address exceeds the memory model", ref);
                return false;
            }
        }
        if (inst->op == SSA_OP_CALL) {

            if (!inst->callee[0]) {
                verr(ctx, "block b%u call has no callee name", ref);
                return false;
            }
            const BirFunctionInfo *callee = bir_find_function(ctx->module, inst->callee);
            if (!callee) {
                verr(ctx, "block b%u calls unknown function '%s'", ref, inst->callee);
                return false;
            }
            if (callee->is_extern) {
                if (inst->operand_count > BIR_ABI_MAX_GPR_ARGUMENT_REGISTERS) {
                    verr(ctx, "block b%u extern call '%s' has %u arguments, max %d",
                         ref, inst->callee, inst->operand_count,
                         BIR_ABI_MAX_GPR_ARGUMENT_REGISTERS);
                    return false;
                }
            } else {
            if (inst->operand_count != callee->ssa_param_count ||
                inst->operand_count != callee->call_abi.param_count) {
                verr(ctx, "block b%u call '%s' has %u lowered arguments, expected %zu",
                     ref, inst->callee, inst->operand_count, callee->call_abi.param_count);
                return false;
            }
            for (size_t arg = 0; arg < callee->ssa_param_count; arg++) {
                const SsaValue *value = &arena->values[
                    arena->operands[inst->operand_start + arg]];
                const CobraType *expected = arg == 0 && callee->has_hidden_return_storage
                    ? callee->return_value_type
                    : callee->param_value_types[arg -
                        (callee->has_hidden_return_storage ? 1U : 0U)];
                if (!value->type || !bir_call_arg_type_compatible(value->type, expected)) {
                    verr(ctx, "block b%u call '%s' lowered argument %zu has the wrong type",
                         ref, inst->callee, arg + 1);
                    return false;
                }
                if (is_pointer_type(expected) || is_view_type(expected)) {
                    BirPointerContract required = arg == 0 && callee->has_hidden_return_storage
                        ? BIR_POINTER_CONTRACT_CALLER_STORAGE
                        : callee->param_pointer_contract[arg -
                            (callee->has_hidden_return_storage ? 1U : 0U)];
                    if (!bir_pointer_contract_compatible(value->pointer_contract, required)) {
                        verr(ctx, "block b%u call '%s' argument %zu violates %s pointer contract",
                             ref, inst->callee, arg + 1,
                             bir_pointer_contract_name(required));
                        return false;
                    }
                }
            }
            }
            if (callee->has_hidden_return_storage) {
                if (inst->type || inst->result != SSA_VALUE_NONE) {
                    verr(ctx, "aggregate call '%s' unexpectedly produces an SSA value",
                         inst->callee);
                    return false;
                }
            } else if (callee->has_return) {
                if (!inst->type || !type_matches(inst->type, callee->return_type) ||
                    inst->result == SSA_VALUE_NONE ||
                    !check_value_type(ctx, inst->result, callee->return_type,
                                      "call result")) {
                    verr(ctx, "block b%u call '%s' has the wrong result type",
                         ref, inst->callee);
                    return false;
                }
                if (is_view_type(callee->return_type)) {
                    const SsaValue *source = &arena->values[
                        arena->operands[inst->operand_start + callee->return_view_param]];
                    const SsaValue *result = &arena->values[inst->result];
                    if ((!is_view_type(source->type) &&
                         !is_owned_slice_type(source->type)) ||
                        result->pointer_contract != callee->return_pointer_contract ||
                        result->pointer_origin != source->pointer_origin ||
                        result->region_id != source->region_id ||
                        result->allocation_id != source->allocation_id) {
                        verr(ctx, "block b%u call '%s' loses borrowed-view provenance",
                             ref, inst->callee);
                        return false;
                    }
                }
            } else if (inst->type || inst->result != SSA_VALUE_NONE) {
                verr(ctx, "void call '%s' unexpectedly produces a value", inst->callee);
                return false;
            }
        }
    }

    /* Terminator specifics. */
    const SsaInst *term = &arena->insts[block->terminator];
    switch (term->op) {
        case SSA_OP_JUMP:
            if (term->target == SSA_BLOCK_NONE || term->target >= arena->block_count) {
                verr(ctx, "block b%u jump targets an invalid block", ref);
                return false;
            }
            if (!check_edge_args(ctx, ref, term->target,
                                  term->edge_start, term->edge_count)) return false;
            break;
        case SSA_OP_BRANCH:
            if (term->operand_count != 1 ||
                !valid_value(ctx, arena->operands[term->operand_start]) ||
                !type_matches(arena->values[arena->operands[term->operand_start]].type,
                              ctx->module->type_bool)) {
                verr(ctx, "block b%u branch has an invalid condition", ref);
                return false;
            }
            if (term->target == SSA_BLOCK_NONE || term->target >= arena->block_count ||
                term->target2 == SSA_BLOCK_NONE || term->target2 >= arena->block_count) {
                verr(ctx, "block b%u branch targets an invalid block", ref);
                return false;
            }
            if (!check_edge_args(ctx, ref, term->target,
                                  term->edge_start, term->edge_count) ||
                !check_edge_args(ctx, ref, term->target2,
                                  term->edge2_start, term->edge2_count)) return false;
            break;
        case SSA_OP_RETURN: {
            int owner = ref < ctx->module->arena.block_count
                ? ctx->function_index[ref] : -1;
            if (term->operand_count == 1 &&
                is_pointer_type(arena->values[arena->operands[term->operand_start]].type)) {
                verr(ctx, "block b%u pointer return would escape its owner frame", ref);
                return false;
            }
            if (owner >= 0) {
                const SsaValue *returned = term->operand_count == 1
                    ? &arena->values[arena->operands[term->operand_start]] : NULL;
                const BirFunctionInfo *info = &ctx->module->functions[owner];
                if (is_view_type(info->return_type)) {
                    if (!returned || !is_view_type(returned->type) ||
                        returned->pointer_contract != info->return_pointer_contract ||
                        returned->pointer_origin != BIR_POINTER_ORIGIN_CALLER) {
                        verr(ctx, "block b%u borrowed-view return would escape its frame or region",
                             ref);
                        return false;
                    }
                }

                if (info->has_return && !info->has_hidden_return_storage) {
                    if (term->operand_count != 1 ||
                        !check_value_type(ctx,
                            term->operand_count == 1
                                ? arena->operands[term->operand_start] : SSA_VALUE_NONE,
                            info->return_type, "return operand")) {
                        verr(ctx, "block b%u return does not match function '%s' return type",
                             ref, info->name);
                        return false;
                    }
                } else if (term->operand_count != 0) {
                    verr(ctx, "%s function '%s' has a return operand",
                         info->has_hidden_return_storage ? "aggregate" : "void", info->name);
                    return false;
                }
            }
            break;
        }
        default:
            verr(ctx, "block b%u terminator is not a terminator", ref);
            return false;
    }

    /* Block parameters must be valid block-param values of this block. */
    for (size_t k = 0; k < block->param_count; k++) {
        SsaValueRef param = block->params[k];
        if (!valid_value(ctx, param)) {
            verr(ctx, "block b%u has an invalid parameter value", ref);
            return false;
        }
        if (arena->values[param].kind != SSA_VALUE_BLOCK_PARAM ||
            arena->values[param].block != ref) {
            verr(ctx, "block b%u parameter v%u is not a block parameter of this block",
                 ref, param);
            return false;
        }
    }

    /* Successor lists must be consistent (edge recorded from both sides). */
    for (size_t s = 0; s < block->succ_count; s++) {
        SsaBlockRef succ = block->succs[s];
        if (succ >= arena->block_count) {
            verr(ctx, "block b%u has an invalid successor", ref);
            return false;
        }
        bool recorded = false;
        for (size_t p = 0; p < arena->blocks[succ].pred_count; p++) {
            if (arena->blocks[succ].preds[p] == ref) recorded = true;
        }
        if (!recorded) {
            verr(ctx, "block b%u successor b%u is not recorded as a predecessor", ref, succ);
            return false;
        }
    }
    return true;
}

typedef enum {
    FLOW_ALLOC_UNSET = 0,
    FLOW_ALLOC_LIVE,
    FLOW_ALLOC_DEAD,
    FLOW_ALLOC_UNKNOWN
} FlowAllocationState;

typedef enum {
    FLOW_REGION_INACTIVE = 0,
    FLOW_REGION_ACTIVE,
    FLOW_REGION_UNKNOWN
} FlowRegionState;

#define FLOW_OWNER_UNKNOWN UINT32_MAX
#define FLOW_BORROW_UNKNOWN UINT16_MAX

typedef struct {
    uint8_t regions[BIR_MAX_REGIONS];
    uint8_t allocations[BIR_MAX_STACK_SLOTS + 1];
    uint32_t owners[BIR_MAX_STACK_SLOTS + 1];
    uint16_t readonly_borrows[BIR_MAX_STACK_SLOTS + 1];
    uint16_t writable_borrows[BIR_MAX_STACK_SLOTS + 1];
    uint32_t sum_payloads[BIR_MAX_STACK_SLOTS + 1];
} OwnershipState;

static bool ownership_state_equal(const OwnershipState *left,
                                  const OwnershipState *right) {
    return memcmp(left, right, sizeof(*left)) == 0;
}

static void ownership_state_init(OwnershipState *state) {
    memset(state, 0, sizeof(*state));
    for (size_t i = 0; i < BIR_MAX_REGIONS; i++)
        state->regions[i] = FLOW_REGION_INACTIVE;
    for (size_t i = 0; i <= BIR_MAX_STACK_SLOTS; i++) {
        state->allocations[i] = FLOW_ALLOC_UNSET;
        state->owners[i] = 0;
        state->readonly_borrows[i] = 0;
        state->writable_borrows[i] = 0;
        state->sum_payloads[i] = 0;
    }
}

static void ownership_seed_function_params(const BackendIrModule *module,
                                           const BirFunctionInfo *info,
                                           OwnershipState *state) {
    if (!module || !info || !state) return;
    for (size_t param = 0; param < info->param_count; param++) {
        bool owning_slice = is_owned_slice_type(info->param_types[param]);
        bool owning_sum = bir_is_sum_type(info->param_types[param]) &&
                          bir_sum_has_owned_payload(info->param_types[param]);
        bool owning_struct = info->param_types[param]->kind == COBRA_TYPE_STRUCT &&
                             bir_type_has_owned_payload(info->param_types[param]);
        if (!owning_slice && !owning_sum && !owning_struct) continue;
        size_t lowered = param + (info->has_hidden_return_storage ? 1U : 0U);
        if (lowered >= info->ssa_param_count) continue;
        SsaValueRef ref = info->params[lowered];
        if (ref == SSA_VALUE_NONE || ref >= module->arena.value_count) continue;
        uint32_t allocation = module->arena.values[ref].allocation_id;
        if (allocation == 0 || allocation > BIR_MAX_STACK_SLOTS) continue;
        state->allocations[allocation] = FLOW_ALLOC_LIVE;
        state->owners[allocation] = 0;
        if (owning_sum || owning_struct)
            state->sum_payloads[allocation] = FLOW_OWNER_UNKNOWN;
    }
}

static void ownership_state_join(OwnershipState *result,
                                 const OwnershipState *incoming) {
    for (size_t i = 0; i < BIR_MAX_REGIONS; i++) {
        if (result->regions[i] != incoming->regions[i])
            result->regions[i] = FLOW_REGION_UNKNOWN;
    }
    for (size_t i = 0; i <= BIR_MAX_STACK_SLOTS; i++) {
        if (result->allocations[i] != incoming->allocations[i])
            result->allocations[i] = FLOW_ALLOC_UNKNOWN;
        if (result->owners[i] != incoming->owners[i])
            result->owners[i] = FLOW_OWNER_UNKNOWN;
        if (result->readonly_borrows[i] != incoming->readonly_borrows[i])
            result->readonly_borrows[i] = FLOW_BORROW_UNKNOWN;
        if (result->writable_borrows[i] != incoming->writable_borrows[i])
            result->writable_borrows[i] = FLOW_BORROW_UNKNOWN;
        if (result->sum_payloads[i] != incoming->sum_payloads[i])
            result->sum_payloads[i] = FLOW_OWNER_UNKNOWN;
    }
}

static bool flow_check_value(VerifyCtx *ctx, const OwnershipState *state,
                             SsaValueRef ref, SsaBlockRef block,
                             const char *use) {
    const SsaArena *arena = &ctx->module->arena;
    if (!valid_value(ctx, ref)) return false;
    const SsaValue *value = &arena->values[ref];
    if (!is_pointer_type(value->type) && !is_view_type(value->type) &&
        !is_owned_slice_type(value->type)) return true;
    if (value->allocation_id == 0) return true; /* external caller storage */
    if (value->allocation_id > BIR_MAX_STACK_SLOTS) {
        verr(ctx, "block b%u %s references an invalid allocation identity", block, use);
        return false;
    }
    uint8_t allocation = state->allocations[value->allocation_id];
    if (allocation == FLOW_ALLOC_DEAD) {
        verr(ctx, "block b%u %s uses allocation %u after destruction", block,
             use, value->allocation_id);
        return false;
    }
    if (allocation == FLOW_ALLOC_UNKNOWN || allocation == FLOW_ALLOC_UNSET) {
        verr(ctx, "block b%u uses allocation %u with an ambiguous lifetime", block,
             value->allocation_id);
        return false;
    }
    uint32_t owner = state->owners[value->allocation_id];
    if (owner == FLOW_OWNER_UNKNOWN ||
        (value->pointer_origin == BIR_POINTER_ORIGIN_REGION &&
         owner != value->region_id) ||
        (value->pointer_origin == BIR_POINTER_ORIGIN_FRAME && owner != 0)) {
        verr(ctx, "block b%u %s uses an allocation after ownership transfer", block, use);
        return false;
    }
    return true;
}

static bool flow_borrow_count_valid(uint16_t count) {
    return count != FLOW_BORROW_UNKNOWN;
}

static bool flow_check_borrow_conflict(VerifyCtx *ctx, const OwnershipState *state,
                                       uint32_t allocation, BirPointerContract contract,
                                       SsaBlockRef block, const char *use) {
    if (allocation == 0) return true;
    if (allocation > BIR_MAX_STACK_SLOTS ||
        !flow_borrow_count_valid(state->readonly_borrows[allocation]) ||
        !flow_borrow_count_valid(state->writable_borrows[allocation])) {
        verr(ctx, "block b%u %s has an ambiguous active borrow", block, use);
        return false;
    }
    uint16_t readers = state->readonly_borrows[allocation];
    uint16_t writers = state->writable_borrows[allocation];
    if (contract == BIR_POINTER_CONTRACT_BORROW_WRITE &&
        (readers || writers > 1)) {
        verr(ctx, "block b%u %s conflicts with an active borrow on allocation %u",
             block, use, allocation);
        return false;
    }
    if ((contract == BIR_POINTER_CONTRACT_OWNED_FRAME ||
         contract == BIR_POINTER_CONTRACT_OWNED_REGION ||
         contract == BIR_POINTER_CONTRACT_OWNED_SLICE ||
         contract == BIR_POINTER_CONTRACT_CALLER_STORAGE) &&
        (readers || writers)) {
        verr(ctx, "block b%u %s writes while a borrowed view is active on allocation %u",
             block, use, allocation);
        return false;
    }
    if (contract == BIR_POINTER_CONTRACT_BORROW_READONLY && writers) {
        verr(ctx, "block b%u %s conflicts with an active writer on allocation %u",
             block, use, allocation);
        return false;
    }
    return true;
}

static bool flow_simulate_block(VerifyCtx *ctx, SsaBlockRef block,
                                OwnershipState *state) {
    const SsaArena *arena = &ctx->module->arena;
    const SsaBlock *bb = &arena->blocks[block];
    for (size_t i = 0; i < bb->inst_count; i++) {
        const SsaInst *inst = &arena->insts[bb->insts[i]];
        for (size_t o = 0; o < inst->operand_count; o++) {
            if (!flow_check_value(ctx, state,
                                  arena->operands[inst->operand_start + o],
                                  block, bir_opcode_name(inst->op))) return false;
        }
        if (inst->op == SSA_OP_LOAD || inst->op == SSA_OP_STORE) {
            const SsaValue *pointer = &arena->values[
                arena->operands[inst->operand_start]];
            if (!flow_check_borrow_conflict(ctx, state,
                                            pointer->allocation_id,
                                            pointer->pointer_contract,
                                            block,
                                            inst->op == SSA_OP_LOAD ? "load" : "store"))
                return false;
        } else if (inst->op == SSA_OP_VIEW_MAKE) {
            const SsaValue *pointer = &arena->values[
                arena->operands[inst->operand_start]];
            uint32_t allocation = pointer->allocation_id;
            if (allocation != 0) {
                BirPointerContract contract = inst->pointer_contract;
                if ((contract == BIR_POINTER_CONTRACT_BORROW_WRITE &&
                     (state->readonly_borrows[allocation] ||
                      state->writable_borrows[allocation])) ||
                    (contract == BIR_POINTER_CONTRACT_BORROW_READONLY &&
                     state->writable_borrows[allocation])) {
                    verr(ctx, "block b%u view construction conflicts with an active borrow on allocation %u",
                         block, allocation);
                    return false;
                }
                if (!flow_check_borrow_conflict(ctx, state, allocation,
                                                contract, block, "view construction"))
                    return false;
                if (contract == BIR_POINTER_CONTRACT_BORROW_WRITE) {
                    if (state->writable_borrows[allocation] == UINT16_MAX - 1U) {
                        verr(ctx, "block b%u has too many active writable borrows", block);
                        return false;
                    }
                    state->writable_borrows[allocation]++;
                } else {
                    if (state->readonly_borrows[allocation] == UINT16_MAX - 1U) {
                        verr(ctx, "block b%u has too many active readonly borrows", block);
                        return false;
                    }
                    state->readonly_borrows[allocation]++;
                }
            }
        } else if (inst->op == SSA_OP_CALL) {
            const BirFunctionInfo *callee = bir_find_function(ctx->module, inst->callee);
            if (!callee) {
                verr(ctx, "block b%u calls an unknown function during ownership analysis", block);
                return false;
            }
            for (size_t arg = 0; arg < callee->param_count; arg++) {
                size_t lowered = arg + (callee->has_hidden_return_storage ? 1U : 0U);
                const CobraType *expected = callee->param_types[arg];
                const SsaValue *actual = &arena->values[
                    arena->operands[inst->operand_start + lowered]];
                uint32_t allocation = actual->allocation_id;
                if (is_owned_slice_type(expected)) {
                    if (!is_owned_slice_type(actual->type) || allocation == 0 ||
                        allocation > BIR_MAX_STACK_SLOTS ||
                        state->allocations[allocation] != FLOW_ALLOC_LIVE ||
                        state->readonly_borrows[allocation] ||
                        state->writable_borrows[allocation]) {
                        verr(ctx, "block b%u transfers an invalid or borrowed owned slice", block);
                        return false;
                    }
                    state->allocations[allocation] = FLOW_ALLOC_DEAD;
                } else if (bir_is_sum_type(expected) &&
                           bir_sum_has_owned_payload(expected)) {
                    if (!is_pointer_type(actual->type) || allocation == 0 ||
                        allocation > BIR_MAX_STACK_SLOTS ||
                        state->allocations[allocation] != FLOW_ALLOC_LIVE) {
                        verr(ctx, "block b%u transfers an invalid owning sum", block);
                        return false;
                    }
                    state->allocations[allocation] = FLOW_ALLOC_DEAD;
                    state->sum_payloads[allocation] = 0;
                } else if (expected->kind == COBRA_TYPE_STRUCT &&
                           bir_type_has_owned_payload(expected)) {
                    /* Owning structs travel by value: the caller storage is
                       moved into the callee and its nested payloads transfer
                       with it. */
                    if (!is_pointer_type(actual->type) || allocation == 0 ||
                        allocation > BIR_MAX_STACK_SLOTS ||
                        state->allocations[allocation] != FLOW_ALLOC_LIVE ||
                        state->readonly_borrows[allocation] ||
                        state->writable_borrows[allocation]) {
                        verr(ctx, "block b%u transfers an invalid or borrowed owning struct", block);
                        return false;
                    }
                    state->allocations[allocation] = FLOW_ALLOC_DEAD;
                    state->sum_payloads[allocation] = 0;
                }
            }
            if (callee->has_hidden_return_storage &&
                (callee->return_type->kind == COBRA_TYPE_STRUCT ||
                 bir_is_sum_type(callee->return_type)) &&
                bir_type_has_owned_payload(callee->return_type)) {
                const SsaValue *destination = &arena->values[
                    arena->operands[inst->operand_start]];
                uint32_t allocation = destination->allocation_id;
                if (!is_pointer_type(destination->type) || allocation == 0 ||
                    allocation > BIR_MAX_STACK_SLOTS ||
                    state->allocations[allocation] != FLOW_ALLOC_LIVE ||
                    state->sum_payloads[allocation] != 0) {
                    verr(ctx, "block b%u call has invalid owning sum return storage", block);
                    return false;
                }
                /* The callee chooses the concrete payload allocation. Keep
                   the caller storage live and carry an explicit unknown
                   payload identity until extraction or drop resolves it. */
                state->allocations[allocation] = FLOW_ALLOC_LIVE;
                state->sum_payloads[allocation] = FLOW_OWNER_UNKNOWN;
            }
            if (inst->result != SSA_VALUE_NONE &&
                inst->result < arena->value_count &&
                is_owned_slice_type(arena->values[inst->result].type)) {
                uint32_t result_allocation = arena->values[inst->result].allocation_id;
                if (result_allocation == 0 || result_allocation > BIR_MAX_STACK_SLOTS ||
                    state->allocations[result_allocation] == FLOW_ALLOC_LIVE) {
                    verr(ctx, "block b%u call returns an invalid owned slice identity", block);
                    return false;
                }
                state->allocations[result_allocation] = FLOW_ALLOC_LIVE;
                state->owners[result_allocation] = 0;
            }
            for (size_t left = 0; left < inst->operand_count; left++) {
                const SsaValue *a = &arena->values[
                    arena->operands[inst->operand_start + left]];
                if (!is_pointer_type(a->type) && !is_view_type(a->type)) continue;
                for (size_t right = left + 1; right < inst->operand_count; right++) {
                    const SsaValue *b = &arena->values[
                        arena->operands[inst->operand_start + right]];
                    if ((!is_pointer_type(b->type) && !is_view_type(b->type)) ||
                        a->allocation_id == 0 ||
                        a->allocation_id != b->allocation_id) continue;
                    if (a->pointer_contract == BIR_POINTER_CONTRACT_BORROW_WRITE ||
                        b->pointer_contract == BIR_POINTER_CONTRACT_BORROW_WRITE) {
                        verr(ctx, "block b%u call passes overlapping writable borrows", block);
                        return false;
                    }
                }
            }
        }
        if (inst->op == SSA_OP_STACK_SLOT) {
            if (inst->allocation_id == 0 || inst->allocation_id > BIR_MAX_STACK_SLOTS) {
                verr(ctx, "block b%u defines an invalid stack allocation", block);
                return false;
            }
            if (inst->region_id != BIR_REGION_NONE &&
                state->regions[inst->region_id] != FLOW_REGION_ACTIVE) {
                verr(ctx, "block b%u creates a slot in an inactive region", block);
                return false;
            }
            state->allocations[inst->allocation_id] = FLOW_ALLOC_LIVE;
            state->owners[inst->allocation_id] = inst->region_id;
        } else if (inst->op == SSA_OP_REGION_ENTER) {
            if (state->regions[inst->region_id] != FLOW_REGION_INACTIVE ||
                (inst->parent_region_id != BIR_REGION_NONE &&
                 state->regions[inst->parent_region_id] != FLOW_REGION_ACTIVE)) {
                verr(ctx, "block b%u enters a region with an invalid path lifetime", block);
                return false;
            }
            state->regions[inst->region_id] = FLOW_REGION_ACTIVE;
        } else if (inst->op == SSA_OP_REGION_EXIT) {
            if (state->regions[inst->region_id] != FLOW_REGION_ACTIVE) {
                verr(ctx, "block b%u exits a region with an ambiguous path lifetime", block);
                return false;
            }
            for (size_t child = 1; child < BIR_MAX_REGIONS; child++) {
                const BirRegionInfo *info = find_region(ctx->module, (uint32_t)child);
                if (info && info->parent_id == inst->region_id &&
                    state->regions[child] == FLOW_REGION_ACTIVE) {
                    verr(ctx, "block b%u exits a region while a child is active", block);
                    return false;
                }
            }
            state->regions[inst->region_id] = FLOW_REGION_INACTIVE;
            for (size_t allocation = 1; allocation <= BIR_MAX_STACK_SLOTS; allocation++) {
                if (state->owners[allocation] == inst->region_id)
                    state->allocations[allocation] = FLOW_ALLOC_DEAD;
            }
        } else if (inst->op == SSA_OP_TRANSFER) {
            SsaValueRef source = arena->operands[inst->operand_start];
            const SsaValue *value = &arena->values[source];
            if (value->pointer_contract != BIR_POINTER_CONTRACT_OWNED_REGION ||
                value->pointer_origin != BIR_POINTER_ORIGIN_REGION ||
                state->regions[inst->region_id] != FLOW_REGION_ACTIVE) {
                verr(ctx, "block b%u transfers a value outside its active owner", block);
                return false;
            }
            state->owners[value->allocation_id] = inst->region_id;
        } else if (inst->op == SSA_OP_STRING_CONCAT) {
            if (inst->allocation_id == 0 || inst->allocation_id > BIR_MAX_STACK_SLOTS ||
                state->allocations[inst->allocation_id] == FLOW_ALLOC_LIVE) {
                verr(ctx, "block b%u string concatenation reuses a live allocation", block);
                return false;
            }
            state->allocations[inst->allocation_id] = FLOW_ALLOC_LIVE;
            state->owners[inst->allocation_id] = 0;
        } else if (inst->op == SSA_OP_FIELD_PAYLOAD_STORE) {
            const SsaValue *destination = &arena->values[
                arena->operands[inst->operand_start]];
            const SsaValue *payload = &arena->values[
                arena->operands[inst->operand_start + 1]];
            uint32_t aggregate_allocation = destination->allocation_id;
            uint32_t payload_allocation = payload->allocation_id;
            if (aggregate_allocation == 0 || payload_allocation == 0 ||
                aggregate_allocation > BIR_MAX_STACK_SLOTS ||
                payload_allocation > BIR_MAX_STACK_SLOTS ||
                state->allocations[aggregate_allocation] != FLOW_ALLOC_LIVE ||
                state->allocations[payload_allocation] != FLOW_ALLOC_LIVE ||
                state->sum_payloads[aggregate_allocation] != 0) {
                verr(ctx, "block b%u stores an invalid or already-owned struct field", block);
                return false;
            }
            state->sum_payloads[aggregate_allocation] = payload_allocation;
        } else if (inst->op == SSA_OP_FIELD_PAYLOAD_LOAD) {
            const SsaValue *source = &arena->values[
                arena->operands[inst->operand_start]];
            uint32_t aggregate_allocation = source->allocation_id;
            uint32_t payload_allocation = aggregate_allocation <= BIR_MAX_STACK_SLOTS
                ? state->sum_payloads[aggregate_allocation] : 0;
            uint32_t result_allocation = inst->result < arena->value_count
                ? arena->values[inst->result].allocation_id : 0;
            if (aggregate_allocation == 0 || payload_allocation == 0 ||
                result_allocation == 0 ||
                (payload_allocation != FLOW_OWNER_UNKNOWN &&
                 state->allocations[payload_allocation] != FLOW_ALLOC_LIVE) ||
                state->allocations[result_allocation] == FLOW_ALLOC_LIVE) {
                verr(ctx, "block b%u extracts an invalid struct field payload", block);
                return false;
            }
            state->sum_payloads[aggregate_allocation] = 0;
            if (payload_allocation != FLOW_OWNER_UNKNOWN)
                state->allocations[payload_allocation] = FLOW_ALLOC_DEAD;
            state->allocations[result_allocation] = FLOW_ALLOC_LIVE;
            state->owners[result_allocation] = 0;
        } else if (inst->op == SSA_OP_AGG_MOVE) {
            const SsaValue *destination = &arena->values[
                arena->operands[inst->operand_start]];
            const SsaValue *source = &arena->values[
                arena->operands[inst->operand_start + 1]];
            uint32_t destination_allocation = destination->allocation_id;
            uint32_t source_allocation = source->allocation_id;
            if (destination_allocation > BIR_MAX_STACK_SLOTS ||
                source_allocation > BIR_MAX_STACK_SLOTS ||
                state->sum_payloads[destination_allocation] != 0) {
                verr(ctx, "block b%u moves an invalid owning aggregate", block);
                return false;
            }
            state->sum_payloads[destination_allocation] =
                state->sum_payloads[source_allocation];
            state->sum_payloads[source_allocation] = 0;
        } else if (inst->op == SSA_OP_AGG_DROP) {
            const SsaValue *source = &arena->values[
                arena->operands[inst->operand_start]];
            uint32_t aggregate_allocation = source->allocation_id;
            uint32_t payload_allocation = aggregate_allocation <= BIR_MAX_STACK_SLOTS
                ? state->sum_payloads[aggregate_allocation] : 0;
            if (aggregate_allocation > BIR_MAX_STACK_SLOTS) {
                verr(ctx, "block b%u drops an invalid owning aggregate", block);
                return false;
            }
            if (payload_allocation != 0 &&
                payload_allocation != FLOW_OWNER_UNKNOWN) {
                if (state->allocations[payload_allocation] != FLOW_ALLOC_LIVE) {
                    verr(ctx, "block b%u drops a dead aggregate field payload", block);
                    return false;
                }
                state->allocations[payload_allocation] = FLOW_ALLOC_DEAD;
            }
            state->sum_payloads[aggregate_allocation] = 0;
            if (aggregate_allocation != 0)
                state->owners[aggregate_allocation] = FLOW_OWNER_UNKNOWN;
        } else if (inst->op == SSA_OP_SUM_PAYLOAD_STORE) {
            const SsaValue *destination = &arena->values[
                arena->operands[inst->operand_start]];
            const SsaValue *payload = &arena->values[
                arena->operands[inst->operand_start + 1]];
            uint32_t sum_allocation = destination->allocation_id;
            uint32_t payload_allocation = payload->allocation_id;
            if (sum_allocation > BIR_MAX_STACK_SLOTS || payload_allocation == 0 ||
                (sum_allocation != 0 &&
                 state->allocations[sum_allocation] != FLOW_ALLOC_LIVE) ||
                state->allocations[payload_allocation] != FLOW_ALLOC_LIVE ||
                state->sum_payloads[sum_allocation] != 0) {
                verr(ctx, "block b%u stores an invalid or already-owned sum payload", block);
                return false;
            }
            state->sum_payloads[sum_allocation] = payload_allocation;
        } else if (inst->op == SSA_OP_SUM_PAYLOAD_LOAD) {
            const SsaValue *source = &arena->values[
                arena->operands[inst->operand_start]];
            uint32_t sum_allocation = source->allocation_id;
            uint32_t payload_allocation = sum_allocation <= BIR_MAX_STACK_SLOTS
                ? state->sum_payloads[sum_allocation] : 0;
            uint32_t result_allocation = inst->result < arena->value_count
                ? arena->values[inst->result].allocation_id : 0;
            if (sum_allocation == 0 || payload_allocation == 0 ||
                result_allocation == 0 ||
                (payload_allocation != FLOW_OWNER_UNKNOWN &&
                 state->allocations[payload_allocation] != FLOW_ALLOC_LIVE) ||
                state->allocations[result_allocation] == FLOW_ALLOC_LIVE) {
                verr(ctx, "block b%u extracts an invalid or absent sum payload", block);
                return false;
            }
            state->sum_payloads[sum_allocation] = 0;
            if (payload_allocation != FLOW_OWNER_UNKNOWN &&
                payload_allocation != sum_allocation)
                state->allocations[payload_allocation] = FLOW_ALLOC_DEAD;
            state->allocations[result_allocation] = FLOW_ALLOC_LIVE;
            state->owners[result_allocation] = 0;
        } else if (inst->op == SSA_OP_SUM_MOVE) {
            const SsaValue *destination = &arena->values[
                arena->operands[inst->operand_start]];
            const SsaValue *source = &arena->values[
                arena->operands[inst->operand_start + 1]];
            uint32_t destination_allocation = destination->allocation_id;
            uint32_t source_allocation = source->allocation_id;
            if (destination_allocation > BIR_MAX_STACK_SLOTS ||
                source_allocation > BIR_MAX_STACK_SLOTS ||
                state->sum_payloads[destination_allocation] != 0) {
                verr(ctx, "block b%u moves an invalid owning sum", block);
                return false;
            }
            state->sum_payloads[destination_allocation] =
                state->sum_payloads[source_allocation];
            /* Moving a payload empties the source sum, but does not destroy
               its stack storage. The source remains valid for a later no-op
               drop; payload extraction still rejects it through sum_payloads. */
            state->sum_payloads[source_allocation] = 0;
        } else if (inst->op == SSA_OP_SUM_DROP) {
            const SsaValue *source = &arena->values[
                arena->operands[inst->operand_start]];
            uint32_t sum_allocation = source->allocation_id;
            uint32_t payload_allocation = sum_allocation <= BIR_MAX_STACK_SLOTS
                ? state->sum_payloads[sum_allocation] : 0;
            if (sum_allocation > BIR_MAX_STACK_SLOTS) {
                verr(ctx, "block b%u drops an invalid owning sum", block);
                return false;
            }
            if (payload_allocation != 0 &&
                payload_allocation != FLOW_OWNER_UNKNOWN) {
                if (state->allocations[payload_allocation] != FLOW_ALLOC_LIVE) {
                    verr(ctx, "block b%u drops a dead sum payload", block);
                    return false;
                }
                state->allocations[payload_allocation] = FLOW_ALLOC_DEAD;
            }
            state->sum_payloads[sum_allocation] = 0;
            if (sum_allocation != 0)
                state->owners[sum_allocation] = FLOW_OWNER_UNKNOWN;
        } else if (inst->op == SSA_OP_SLICE_ALLOC) {
            if (inst->allocation_id == 0 || inst->allocation_id > BIR_MAX_STACK_SLOTS ||
                state->allocations[inst->allocation_id] == FLOW_ALLOC_LIVE) {
                verr(ctx, "block b%u slice allocation reuses a live allocation", block);
                return false;
            }
            if (inst->region_id != BIR_REGION_NONE &&
                state->regions[inst->region_id] != FLOW_REGION_ACTIVE) {
                verr(ctx, "block b%u allocates a slice in an inactive region", block);
                return false;
            }
            state->allocations[inst->allocation_id] = FLOW_ALLOC_LIVE;
            state->owners[inst->allocation_id] = inst->region_id;
        } else if (inst->op == SSA_OP_BUFFER_ALLOC) {
            if (inst->allocation_id == 0 || inst->allocation_id > BIR_MAX_STACK_SLOTS ||
                state->allocations[inst->allocation_id] == FLOW_ALLOC_LIVE) {
                verr(ctx, "block b%u buffer allocation reuses a live allocation", block);
                return false;
            }
            if (inst->region_id != BIR_REGION_NONE &&
                state->regions[inst->region_id] != FLOW_REGION_ACTIVE) {
                verr(ctx, "block b%u allocates a buffer in an inactive region", block);
                return false;
            }
            state->allocations[inst->allocation_id] = FLOW_ALLOC_LIVE;
            state->owners[inst->allocation_id] = inst->region_id;
        } else if (inst->op == SSA_OP_BUFFER_APPEND) {
            SsaValueRef source = arena->operands[inst->operand_start];
            uint32_t source_allocation = arena->values[source].allocation_id;
            if (source_allocation == 0 || source_allocation > BIR_MAX_STACK_SLOTS ||
                state->allocations[source_allocation] != FLOW_ALLOC_LIVE ||
                state->readonly_borrows[source_allocation] ||
                state->writable_borrows[source_allocation] ||
                inst->allocation_id == 0 || inst->allocation_id > BIR_MAX_STACK_SLOTS ||
                state->allocations[inst->allocation_id] == FLOW_ALLOC_LIVE) {
                verr(ctx, "block b%u appends to an invalid or borrowed buffer", block);
                return false;
            }
            state->allocations[source_allocation] = FLOW_ALLOC_DEAD;
            state->allocations[inst->allocation_id] = FLOW_ALLOC_LIVE;
            state->owners[inst->allocation_id] = 0;
        } else if (inst->op == SSA_OP_BUFFER_POP) {
            SsaValueRef source = arena->operands[inst->operand_start];
            uint32_t allocation = arena->values[source].allocation_id;
            if (allocation == 0 || allocation > BIR_MAX_STACK_SLOTS ||
                state->allocations[allocation] != FLOW_ALLOC_LIVE) {
                verr(ctx, "block b%u pops an inactive buffer", block);
                return false;
            }
        } else if (inst->op == SSA_OP_BUFFER_FREE) {
            SsaValueRef source = arena->operands[inst->operand_start];
            uint32_t allocation = arena->values[source].allocation_id;
            if (allocation == 0 || allocation > BIR_MAX_STACK_SLOTS ||
                state->allocations[allocation] != FLOW_ALLOC_LIVE) {
                verr(ctx, "block b%u double-frees or ambiguously frees buffer %u",
                     block, allocation);
                return false;
            }
            if (state->owners[allocation] != 0 ||
                state->readonly_borrows[allocation] ||
                state->writable_borrows[allocation]) {
                verr(ctx, "block b%u frees a buffer with an active owner or borrow", block);
                return false;
            }
            state->allocations[allocation] = FLOW_ALLOC_DEAD;
        } else if (inst->op == SSA_OP_DICT_ALLOC) {
            if (inst->allocation_id == 0 || inst->allocation_id > BIR_MAX_STACK_SLOTS ||
                state->allocations[inst->allocation_id] == FLOW_ALLOC_LIVE) {
                verr(ctx, "block b%u dict allocation reuses a live allocation", block);
                return false;
            }
            if (inst->region_id != BIR_REGION_NONE &&
                state->regions[inst->region_id] != FLOW_REGION_ACTIVE) {
                verr(ctx, "block b%u allocates a dict in an inactive region", block);
                return false;
            }
            state->allocations[inst->allocation_id] = FLOW_ALLOC_LIVE;
            state->owners[inst->allocation_id] = inst->region_id;
        } else if (inst->op == SSA_OP_DICT_SET) {
            SsaValueRef source = arena->operands[inst->operand_start];
            uint32_t source_allocation = arena->values[source].allocation_id;
            if (source_allocation == 0 || source_allocation > BIR_MAX_STACK_SLOTS ||
                state->allocations[source_allocation] != FLOW_ALLOC_LIVE ||
                state->readonly_borrows[source_allocation] ||
                state->writable_borrows[source_allocation] ||
                inst->allocation_id == 0 || inst->allocation_id > BIR_MAX_STACK_SLOTS ||
                state->allocations[inst->allocation_id] == FLOW_ALLOC_LIVE) {
                verr(ctx, "block b%u inserts into an invalid or borrowed dict", block);
                return false;
            }
            state->allocations[source_allocation] = FLOW_ALLOC_DEAD;
            state->allocations[inst->allocation_id] = FLOW_ALLOC_LIVE;
            state->owners[inst->allocation_id] = 0;
        } else if (inst->op == SSA_OP_DICT_DELETE) {
            SsaValueRef source = arena->operands[inst->operand_start];
            uint32_t source_allocation = arena->values[source].allocation_id;
            if (source_allocation == 0 || source_allocation > BIR_MAX_STACK_SLOTS ||
                state->allocations[source_allocation] != FLOW_ALLOC_LIVE ||
                state->readonly_borrows[source_allocation] ||
                state->writable_borrows[source_allocation] ||
                inst->allocation_id == 0 || inst->allocation_id > BIR_MAX_STACK_SLOTS ||
                state->allocations[inst->allocation_id] == FLOW_ALLOC_LIVE) {
                verr(ctx, "block b%u deletes from an invalid or borrowed dict", block);
                return false;
            }
            state->allocations[source_allocation] = FLOW_ALLOC_DEAD;
            state->allocations[inst->allocation_id] = FLOW_ALLOC_LIVE;
            state->owners[inst->allocation_id] = 0;
        } else if (inst->op == SSA_OP_DICT_POP) {
            SsaValueRef source = arena->operands[inst->operand_start];
            uint32_t allocation = arena->values[source].allocation_id;
            if (allocation == 0 || allocation > BIR_MAX_STACK_SLOTS ||
                state->allocations[allocation] != FLOW_ALLOC_LIVE ||
                state->readonly_borrows[allocation] ||
                state->writable_borrows[allocation]) {
                verr(ctx, "block b%u pops from an invalid or borrowed dict", block);
                return false;
            }
        } else if (inst->op == SSA_OP_DICT_FREE) {
            SsaValueRef source = arena->operands[inst->operand_start];
            uint32_t allocation = arena->values[source].allocation_id;
            if (allocation == 0 || allocation > BIR_MAX_STACK_SLOTS ||
                state->allocations[allocation] != FLOW_ALLOC_LIVE) {
                verr(ctx, "block b%u double-frees or ambiguously frees dict %u",
                     block, allocation);
                return false;
            }
            if (state->owners[allocation] != 0 ||
                state->readonly_borrows[allocation] ||
                state->writable_borrows[allocation]) {
                verr(ctx, "block b%u frees a dict with an active owner or borrow", block);
                return false;
            }
            state->allocations[allocation] = FLOW_ALLOC_DEAD;
        } else if (inst->op == SSA_OP_SLICE_FREE) {
            SsaValueRef source = arena->operands[inst->operand_start];
            uint32_t allocation = arena->values[source].allocation_id;
            if (allocation == 0 || state->allocations[allocation] != FLOW_ALLOC_LIVE) {
                verr(ctx, "block b%u double-frees or ambiguously frees allocation %u",
                     block, allocation);
                return false;
            }
            if (state->owners[allocation] != 0) {
                verr(ctx, "block b%u cannot free a region-backed slice; the region releases it",
                     block);
                return false;
            }
            if (state->readonly_borrows[allocation] ||
                state->writable_borrows[allocation]) {
                verr(ctx, "block b%u frees allocation %u while a borrowed view is active",
                     block, allocation);
                return false;
            }
            state->allocations[allocation] = FLOW_ALLOC_DEAD;
        } else if (inst->op == SSA_OP_DESTROY) {
            SsaValueRef source = arena->operands[inst->operand_start];
            uint32_t allocation = arena->values[source].allocation_id;
            if (allocation == 0 || state->allocations[allocation] != FLOW_ALLOC_LIVE) {
                verr(ctx, "block b%u double-destroys or ambiguously destroys allocation %u",
                     block, allocation);
                return false;
            }
            state->allocations[allocation] = FLOW_ALLOC_DEAD;
        }
    }
    const SsaInst *terminator = bb->terminator < arena->inst_count
        ? &arena->insts[bb->terminator] : NULL;
    if (terminator && terminator->op == SSA_OP_RETURN) {
        int owner = block < arena->block_count ? ctx->function_index[block] : -1;
        if (owner >= 0) {
            const BirFunctionInfo *info = &ctx->module->functions[owner];
            for (size_t param = 0; param < info->param_count; param++) {
                if (!is_owned_slice_type(info->param_types[param])) continue;
                size_t lowered = param + (info->has_hidden_return_storage ? 1U : 0U);
                SsaValueRef ref = info->params[lowered];
                uint32_t allocation = ref < arena->value_count
                    ? arena->values[ref].allocation_id : 0;
                if (allocation != 0 && state->allocations[allocation] == FLOW_ALLOC_LIVE) {
                    bool returned_parameter = false;
                    const SsaInst *term = bb->terminator < arena->inst_count
                        ? &arena->insts[bb->terminator] : NULL;
                    if (term && term->operand_count == 1) {
                        SsaValueRef returned = arena->operands[term->operand_start];
                        returned_parameter = returned < arena->value_count &&
                            arena->values[returned].allocation_id == allocation &&
                            is_owned_slice_type(arena->values[returned].type);
                    }
                    if (!returned_parameter) {
                        verr(ctx, "block b%u returns while owned slice parameter %zu is still live",
                             block, param + 1);
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

static bool check_ownership_flow(VerifyCtx *ctx) {
    const size_t blocks = ctx->module->arena.block_count;
    OwnershipState *in = calloc(blocks ? blocks : 1, sizeof(OwnershipState));
    OwnershipState *out = calloc(blocks ? blocks : 1, sizeof(OwnershipState));
    bool *out_valid = calloc(blocks ? blocks : 1, sizeof(bool));
    if (!in || !out || !out_valid) {
        free(in);
        free(out);
        free(out_valid);
        verr(ctx, "out of memory in ownership dataflow");
        return false;
    }
    for (size_t f = 0; f < ctx->module->function_count; f++) {
        const BirFunctionInfo *info = &ctx->module->functions[f];
        if (info->entry == SSA_BLOCK_NONE || info->entry >= blocks) continue;
        ownership_state_init(&in[info->entry]);
        ownership_seed_function_params(ctx->module, info, &in[info->entry]);
        for (size_t iteration = 0; iteration < blocks * 2 + 2; iteration++) {
            bool changed = false;
            for (size_t r = 0; r < ctx->rpo_count; r++) {
                SsaBlockRef block = ctx->rpo[r];
                if (!ctx->reachable[block] || ctx->function_index[block] != (int)f) continue;
                OwnershipState incoming;
                bool have_predecessor = false;
                if (block == info->entry) {
                    ownership_state_init(&incoming);
                    ownership_seed_function_params(ctx->module, info, &incoming);
                    have_predecessor = true;
                } else {
                    const SsaBlock *bb = &ctx->module->arena.blocks[block];
                    for (size_t p = 0; p < bb->pred_count; p++) {
                        SsaBlockRef pred = bb->preds[p];
                        if (pred >= blocks || !ctx->reachable[pred] ||
                            ctx->function_index[pred] != (int)f || !out_valid[pred]) continue;
                        if (!have_predecessor) {
                            incoming = out[pred];
                            have_predecessor = true;
                        } else {
                            ownership_state_join(&incoming, &out[pred]);
                        }
                    }
                }
                if (!have_predecessor) continue; /* backedge not computed yet */
                if (!ownership_state_equal(&in[block], &incoming)) {
                    in[block] = incoming;
                    changed = true;
                }
                OwnershipState next = incoming;
                if (!flow_simulate_block(ctx, block, &next)) {
                    free(in);
                    free(out);
                    free(out_valid);
                    return false;
                }
                if (!out_valid[block] || !ownership_state_equal(&out[block], &next)) {
                    out[block] = next;
                    out_valid[block] = true;
                    changed = true;
                }
            }
            if (!changed) break;
        }
    }
    free(in);
    free(out);
    free(out_valid);
    return true;
}

static bool check_dominance(VerifyCtx *ctx) {
    const SsaArena *arena = &ctx->module->arena;
    const size_t blocks = arena->block_count;

    for (size_t v = 0; v < arena->value_count; v++) {
        const SsaValue *value = &arena->values[v];
        if (value->kind == SSA_VALUE_INVALID) continue;
        /* Constants and function parameters are defined at entry and dominate
           every reachable use. */
        if (value->kind == SSA_VALUE_PARAM || value->kind == SSA_VALUE_CONST) continue;

        SsaBlockRef def_block = SSA_BLOCK_NONE;
        size_t def_inst_index = 0;
        if (value->kind == SSA_VALUE_BLOCK_PARAM) {
            def_block = value->block;
        } else if (value->kind == SSA_VALUE_INST && value->def_inst < arena->inst_count) {
            for (size_t b = 0; b < blocks; b++) {
                if (!ctx->reachable[b]) continue;
                const SsaBlock *block = &arena->blocks[b];
                for (size_t i = 0; i < block->inst_count; i++) {
                    if (block->insts[i] == value->def_inst) {
                        def_block = (SsaBlockRef)b;
                        def_inst_index = i;
                    }
                }
            }
        }
        if (def_block == SSA_BLOCK_NONE || !ctx->reachable[def_block]) continue;

        for (size_t b = 0; b < blocks; b++) {
            if (!ctx->reachable[b]) continue;
            const SsaBlock *block = &arena->blocks[b];
            for (size_t i = 0; i < block->inst_count; i++) {
                const SsaInst *inst = &arena->insts[block->insts[i]];
                bool uses = false;
                for (size_t o = 0; o < inst->operand_count; o++) {
                    if (arena->operands[inst->operand_start + o] == v) uses = true;
                }
                if (!uses) continue;
                if (!ctx->dom[b * blocks + def_block]) {
                    verr(ctx, "value v%zu used in block b%u without a dominating definition",
                         v, b);
                    return false;
                }
                if (value->kind == SSA_VALUE_INST && b == def_block && i <= def_inst_index) {
                    verr(ctx, "value v%zu used before its definition in block b%u", v, b);
                    return false;
                }
            }
            /* Terminator edge arguments and return values are uses at the end
               of the block; only dominance applies (ordering is guaranteed by
               the terminator being last). */
            const SsaInst *term = &arena->insts[block->terminator];
            for (size_t e = 0; e < term->edge_count; e++) {
                if (arena->edges[term->edge_start + e] == v &&
                    !ctx->dom[b * blocks + def_block]) {
                    verr(ctx, "edge argument v%zu in block b%u lacks a dominating definition",
                         v, b);
                    return false;
                }
            }
            for (size_t e = 0; e < term->edge2_count; e++) {
                if (arena->edges[term->edge2_start + e] == v &&
                    !ctx->dom[b * blocks + def_block]) {
                    verr(ctx, "edge argument v%zu in block b%u lacks a dominating definition",
                         v, b);
                    return false;
                }
            }
            if (term->op == SSA_OP_RETURN && term->operand_count == 1 &&
                arena->operands[term->operand_start] == v &&
                !ctx->dom[b * blocks + def_block]) {
                verr(ctx, "return value v%zu in block b%u lacks a dominating definition",
                     v, b);
                return false;
            }
        }
    }
    return true;
}

bool bir_verify(const BackendIrModule *module, char *errbuf, size_t errbuf_size) {
    if (!module) {
        if (errbuf && errbuf_size) snprintf(errbuf, errbuf_size, "null module");
        return false;
    }
    if (errbuf && errbuf_size) errbuf[0] = '\0';
    const SsaArena *arena = &module->arena;
    const size_t blocks = arena->block_count;

    VerifyCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.module = module;
    ctx.errbuf = errbuf;
    ctx.errbuf_size = errbuf_size;
    ctx.reachable = calloc(blocks ? blocks : 1, sizeof(bool));
    ctx.function_index = malloc((blocks ? blocks : 1) * sizeof(int));
    ctx.dom = calloc((blocks ? blocks : 1) * (blocks ? blocks : 1), sizeof(bool));
    ctx.rpo = calloc(blocks ? blocks : 1, sizeof(SsaBlockRef));
    if (!ctx.reachable || !ctx.function_index || !ctx.dom || !ctx.rpo) {
        verr(&ctx, "out of memory in verifier");
        free(ctx.reachable);
        free(ctx.function_index);
        free(ctx.dom);
        free(ctx.rpo);
        return false;
    }

    for (size_t i = 0; i < (blocks ? blocks : 1); i++) ctx.function_index[i] = -1;
    bool ok = assign_function_ownership(&ctx);
    if (ok) compute_reachability(&ctx);
    if (ok) compute_dominators(&ctx);

    if (ok) ok = check_function_table(&ctx);
    if (ok) ok = check_types(&ctx);
    if (ok) ok = check_function_parameters(&ctx);
    /* Slot 0 is the reserved invalid-handle sentinel in every pool. */
    for (size_t b = 1; ok && b < blocks; b++) {
        ok = check_block(&ctx, (SsaBlockRef)b);
    }
    if (ok) ok = check_ownership_flow(&ctx);
    if (ok) ok = check_dominance(&ctx);

    free(ctx.reachable);
    free(ctx.function_index);
    free(ctx.dom);
    free(ctx.rpo);
    return ok;
}
