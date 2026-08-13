/*
 * Cobra Backend IR: flat arena-backed SSA construction.
 *
 * Stable integer handles into growable pools. Operands and edge arguments
 * are variable-length windows into flat pools. See docs/BACKEND_IR.md.
 */
#include "ssa.h"

static void *grow_pool(void *ptr, size_t elem, size_t *cap, size_t needed) {
    if (needed <= *cap) return ptr;
    size_t next = *cap ? *cap * 2 : 16;
    while (next < needed) next *= 2;
    void *grown = realloc(ptr, elem * next);
    if (!grown) return NULL;
    *cap = next;
    return grown;
}

void bir_arena_init(SsaArena *arena) {
    memset(arena, 0, sizeof(*arena));
    /* Reserve pool slot 0 as the invalid-handle sentinel so SSA_VALUE_NONE /
       SSA_INST_NONE / SSA_BLOCK_NONE (all 0) never collide with a real,
       first-created entry. Real handles are therefore 1-based. */
    SsaValue *values = grow_pool(arena->values, sizeof(SsaValue), &arena->value_cap, 1);
    SsaInst *insts = grow_pool(arena->insts, sizeof(SsaInst), &arena->inst_cap, 1);
    SsaBlock *blocks = grow_pool(arena->blocks, sizeof(SsaBlock), &arena->block_cap, 1);
    if (values) {
        arena->values = values;
        memset(&arena->values[0], 0, sizeof(SsaValue));
        arena->values[0].kind = SSA_VALUE_INVALID;
        arena->value_count = 1;
    }
    if (insts) {
        arena->insts = insts;
        memset(&arena->insts[0], 0, sizeof(SsaInst));
        arena->insts[0].op = SSA_OP_NONE;
        arena->inst_count = 1;
    }
    if (blocks) {
        arena->blocks = blocks;
        memset(&arena->blocks[0], 0, sizeof(SsaBlock));
        arena->blocks[0].terminator = SSA_INST_NONE;
        arena->block_count = 1;
    }
}

void bir_arena_free(SsaArena *arena) {
    if (!arena) return;
    for (size_t i = 0; i < arena->block_count; i++) {
        free(arena->blocks[i].insts);
        free(arena->blocks[i].params);
        free(arena->blocks[i].preds);
        free(arena->blocks[i].succs);
    }
    free(arena->blocks);
    free(arena->insts);
    free(arena->values);
    free(arena->operands);
    free(arena->edges);
    memset(arena, 0, sizeof(*arena));
}

SsaBlockRef bir_add_block(SsaArena *arena, const char *name, int line, int col) {
    if (!arena) return SSA_BLOCK_NONE;
    SsaBlock *grown = grow_pool(arena->blocks, sizeof(SsaBlock), &arena->block_cap,
                                arena->block_count + 1);
    if (!grown) return SSA_BLOCK_NONE;
    arena->blocks = grown;
    SsaBlock *block = &arena->blocks[arena->block_count];
    memset(block, 0, sizeof(*block));
    if (name) snprintf(block->name, sizeof(block->name), "%s", name);
    block->source_line = line;
    block->source_col = col;
    block->terminator = SSA_INST_NONE;
    return (SsaBlockRef)arena->block_count++;
}

SsaBlockRef bir_add_entry_block(SsaArena *arena, const char *name, int line, int col) {
    SsaBlockRef block = bir_add_block(arena, name, line, col);
    if (block != SSA_BLOCK_NONE) arena->blocks[block].is_entry = true;
    return block;
}

SsaValueRef bir_add_value(SsaArena *arena, SsaValueKind kind, const CobraType *type,
                          int line, int col) {
    if (!arena || kind == SSA_VALUE_INVALID) return SSA_VALUE_NONE;
    SsaValue *grown = grow_pool(arena->values, sizeof(SsaValue), &arena->value_cap,
                                arena->value_count + 1);
    if (!grown) return SSA_VALUE_NONE;
    arena->values = grown;
    SsaValue *value = &arena->values[arena->value_count];
    memset(value, 0, sizeof(*value));
    value->kind = kind;
    value->type = type;
    value->source_line = line;
    value->source_col = col;
    return (SsaValueRef)arena->value_count++;
}

SsaValueRef bir_add_const(SsaArena *arena, const CobraType *type, int64_t value,
                          int line, int col) {
    SsaValueRef ref = bir_add_value(arena, SSA_VALUE_CONST, type, line, col);
    if (ref != SSA_VALUE_NONE) arena->values[ref].const_i64 = value;
    return ref;
}

SsaInstRef bir_add_inst(SsaArena *arena, SsaOpcode op, const CobraType *type,
                        const SsaValueRef *operands, size_t operand_count,
                        int line, int col) {
    if (!arena || op == SSA_OP_NONE) return SSA_INST_NONE;
    SsaInst *inst_grown = grow_pool(arena->insts, sizeof(SsaInst), &arena->inst_cap,
                                    arena->inst_count + 1);
    if (!inst_grown) return SSA_INST_NONE;
    arena->insts = inst_grown;
    if (operand_count) {
        SsaValueRef *op_grown = grow_pool(arena->operands, sizeof(SsaValueRef),
                                          &arena->operand_cap,
                                          arena->operand_used + operand_count);
        if (!op_grown) return SSA_INST_NONE;
        arena->operands = op_grown;
    }
    SsaInst *inst = &arena->insts[arena->inst_count];
    memset(inst, 0, sizeof(*inst));
    inst->op = op;
    inst->type = type;
    inst->result = SSA_VALUE_NONE;
    inst->operand_start = (uint32_t)arena->operand_used;
    inst->operand_count = (uint32_t)operand_count;
    for (size_t i = 0; i < operand_count; i++)
        arena->operands[arena->operand_used + i] = operands[i];
    arena->operand_used += operand_count;
    inst->target = SSA_BLOCK_NONE;
    inst->target2 = SSA_BLOCK_NONE;
    inst->source_line = line;
    inst->source_col = col;
    return (SsaInstRef)arena->inst_count++;
}

static bool reserve_edges(SsaArena *arena, size_t extra) {
    if (!extra) return true;
    SsaValueRef *grown = grow_pool(arena->edges, sizeof(SsaValueRef), &arena->edge_cap,
                                   arena->edge_used + extra);
    if (!grown) return false;
    arena->edges = grown;
    return true;
}

SsaValueRef bir_inst_result(SsaArena *arena, SsaInstRef inst, int line, int col) {
    if (!arena || inst == SSA_INST_NONE || inst >= arena->inst_count) return SSA_VALUE_NONE;
    SsaInst *source = &arena->insts[inst];
    if (!bir_op_has_result(source->op)) return SSA_VALUE_NONE;
    SsaValueRef ref = bir_add_value(arena, SSA_VALUE_INST, source->type, line, col);
    if (ref == SSA_VALUE_NONE) return SSA_VALUE_NONE;
    arena->values[ref].def_inst = inst;
    source->result = ref;
    return ref;
}

bool bir_block_add_inst(SsaArena *arena, SsaBlockRef block, SsaInstRef inst) {
    if (!arena || block == SSA_BLOCK_NONE || block >= arena->block_count ||
        inst == SSA_INST_NONE || inst >= arena->inst_count)
        return false;
    SsaBlock *target = &arena->blocks[block];
    if (target->terminator != SSA_INST_NONE) return false; /* nothing after a terminator */
    SsaInstRef *grown = grow_pool(target->insts, sizeof(SsaInstRef), &target->inst_cap,
                                  target->inst_count + 1);
    if (!grown) return false;
    target->insts = grown;
    target->insts[target->inst_count++] = inst;
    return true;
}

bool bir_set_terminator(SsaArena *arena, SsaBlockRef block, SsaInstRef term) {
    if (!arena || block == SSA_BLOCK_NONE || block >= arena->block_count ||
        term == SSA_INST_NONE || term >= arena->inst_count)
        return false;
    SsaBlock *target = &arena->blocks[block];
    if (!bir_is_terminator(arena->insts[term].op)) return false;
    if (!bir_block_add_inst(arena, block, term)) return false;
    target->terminator = term;
    return true;
}

SsaValueRef bir_add_block_param(SsaArena *arena, SsaBlockRef block,
                                const CobraType *type, int line, int col) {
    if (!arena || block == SSA_BLOCK_NONE || block >= arena->block_count)
        return SSA_VALUE_NONE;
    SsaBlock *target = &arena->blocks[block];
    SsaValueRef *grown = grow_pool(target->params, sizeof(SsaValueRef), &target->param_cap,
                                   target->param_count + 1);
    if (!grown) return SSA_VALUE_NONE;
    target->params = grown;
    SsaValueRef ref = bir_add_value(arena, SSA_VALUE_BLOCK_PARAM, type, line, col);
    if (ref == SSA_VALUE_NONE) return SSA_VALUE_NONE;
    arena->values[ref].block = block;
    target->params[target->param_count++] = ref;
    return ref;
}

bool bir_add_edge(SsaArena *arena, SsaBlockRef pred, SsaBlockRef succ) {
    if (!arena || pred == SSA_BLOCK_NONE || pred >= arena->block_count ||
        succ == SSA_BLOCK_NONE || succ >= arena->block_count)
        return false;
    SsaBlock *from = &arena->blocks[pred];
    SsaBlock *to = &arena->blocks[succ];
    SsaBlockRef *succ_grown = grow_pool(from->succs, sizeof(SsaBlockRef),
                                        &from->succ_cap, from->succ_count + 1);
    if (!succ_grown) return false;
    from->succs = succ_grown;
    SsaBlockRef *pred_grown = grow_pool(to->preds, sizeof(SsaBlockRef),
                                        &to->pred_cap, to->pred_count + 1);
    if (!pred_grown) return false;
    to->preds = pred_grown;
    from->succs[from->succ_count++] = succ;
    to->preds[to->pred_count++] = pred;
    return true;
}

static bool append_edge_args(SsaArena *arena, SsaInst *inst, bool second,
                             const SsaValueRef *args, size_t count) {
    if (!reserve_edges(arena, count)) return false;
    uint32_t start = (uint32_t)arena->edge_used;
    for (size_t i = 0; i < count; i++) arena->edges[arena->edge_used++] = args[i];
    if (second) {
        inst->edge2_start = start;
        inst->edge2_count = (uint32_t)count;
    } else {
        inst->edge_start = start;
        inst->edge_count = (uint32_t)count;
    }
    return true;
}

bool bir_set_jump(SsaArena *arena, SsaBlockRef block, SsaBlockRef target,
                  const SsaValueRef *edge_args, size_t edge_count, int line, int col) {
    if (!arena || block == SSA_BLOCK_NONE || block >= arena->block_count ||
        target == SSA_BLOCK_NONE || target >= arena->block_count)
        return false;
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_JUMP, NULL, NULL, 0, line, col);
    if (inst == SSA_INST_NONE) return false;
    SsaInst *jump = &arena->insts[inst];
    jump->target = target;
    if (!append_edge_args(arena, jump, false, edge_args, edge_count)) return false;
    return bir_set_terminator(arena, block, inst);
}

bool bir_set_branch(SsaArena *arena, SsaBlockRef block, SsaValueRef cond,
                    SsaBlockRef then_block, SsaBlockRef else_block,
                    const SsaValueRef *then_args, size_t then_count,
                    const SsaValueRef *else_args, size_t else_count,
                    int line, int col) {
    if (!arena || block == SSA_BLOCK_NONE || block >= arena->block_count ||
        cond == SSA_VALUE_NONE || cond >= arena->value_count ||
        then_block == SSA_BLOCK_NONE || then_block >= arena->block_count ||
        else_block == SSA_BLOCK_NONE || else_block >= arena->block_count)
        return false;
    const SsaValueRef operands[1] = {cond};
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_BRANCH, NULL, operands, 1, line, col);
    if (inst == SSA_INST_NONE) return false;
    SsaInst *branch = &arena->insts[inst];
    branch->target = then_block;
    branch->target2 = else_block;
    if (!append_edge_args(arena, branch, false, then_args, then_count) ||
        !append_edge_args(arena, branch, true, else_args, else_count))
        return false;
    return bir_set_terminator(arena, block, inst);
}

bool bir_set_return(SsaArena *arena, SsaBlockRef block, SsaValueRef value,
                    int line, int col) {
    if (!arena || block == SSA_BLOCK_NONE || block >= arena->block_count)
        return false;
    SsaInstRef inst;
    if (value != SSA_VALUE_NONE) {
        const SsaValueRef operands[1] = {value};
        inst = bir_add_inst(arena, SSA_OP_RETURN, NULL, operands, 1, line, col);
    } else {
        inst = bir_add_inst(arena, SSA_OP_RETURN, NULL, NULL, 0, line, col);
    }
    if (inst == SSA_INST_NONE) return false;
    return bir_set_terminator(arena, block, inst);
}

bool bir_register_function_info(BackendIrModule *module, const char *name,
                                SsaBlockRef entry, size_t param_count,
                                const SsaValueRef *params,
                                const CobraType *return_type, bool has_return) {
    if (!module || !name || !name[0] || entry == SSA_BLOCK_NONE ||
        entry >= module->arena.block_count || !return_type ||
        param_count > BIR_MAX_PARAMS) {
        if (module) snprintf(module->error, sizeof(module->error),
                             "invalid function registration");
        return false;
    }
    if (bir_find_function(module, name)) {
        snprintf(module->error, sizeof(module->error),
                 "duplicate function '%s'", name);
        return false;
    }
    if (module->function_count >= BIR_MAX_FUNCTIONS) {
        snprintf(module->error, sizeof(module->error),
                 "too many functions in backend-IR module");
        return false;
    }
    BirFunctionInfo *info = &module->functions[module->function_count++];
    memset(info, 0, sizeof(*info));
    snprintf(info->name, sizeof(info->name), "%s", name);
    info->entry = entry;
    info->param_count = param_count;
    for (size_t k = 0; k < param_count && k < BIR_MAX_PARAMS; k++) {
        info->params[k] = params ? params[k] : SSA_VALUE_NONE;
    }
    info->return_type = return_type;
    info->has_return = has_return;
    return true;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

const char *bir_opcode_name(SsaOpcode op) {
    switch (op) {
        case SSA_OP_NONE: return "none";
        case SSA_OP_CONST: return "const";
        case SSA_OP_PARAM: return "param";
        case SSA_OP_BLOCK_ARG: return "block_arg";
        case SSA_OP_ADD: return "add";
        case SSA_OP_SUB: return "sub";
        case SSA_OP_MUL: return "mul";
        case SSA_OP_DIV: return "div";
        case SSA_OP_REM: return "rem";
        case SSA_OP_NEG: return "neg";
        case SSA_OP_EQ: return "eq";
        case SSA_OP_NE: return "ne";
        case SSA_OP_LT: return "lt";
        case SSA_OP_LE: return "le";
        case SSA_OP_GT: return "gt";
        case SSA_OP_GE: return "ge";
        case SSA_OP_LOAD: return "load";
        case SSA_OP_STORE: return "store";
        case SSA_OP_CALL: return "call";
        case SSA_OP_JUMP: return "jump";
        case SSA_OP_BRANCH: return "branch";
        case SSA_OP_RETURN: return "return";
    }
    return "?";
}

bool bir_is_terminator(SsaOpcode op) {
    return op == SSA_OP_JUMP || op == SSA_OP_BRANCH || op == SSA_OP_RETURN;
}

bool bir_op_has_result(SsaOpcode op) {
    switch (op) {
        case SSA_OP_ADD:
        case SSA_OP_SUB:
        case SSA_OP_MUL:
        case SSA_OP_DIV:
        case SSA_OP_REM:
        case SSA_OP_NEG:
        case SSA_OP_EQ:
        case SSA_OP_NE:
        case SSA_OP_LT:
        case SSA_OP_LE:
        case SSA_OP_GT:
        case SSA_OP_GE:
        case SSA_OP_LOAD:
        case SSA_OP_CALL:
            return true;
        default:
            return false;
    }
}

const char *bir_value_kind_name(SsaValueKind kind) {
    switch (kind) {
        case SSA_VALUE_INVALID: return "invalid";
        case SSA_VALUE_PARAM: return "param";
        case SSA_VALUE_BLOCK_PARAM: return "block_param";
        case SSA_VALUE_CONST: return "const";
        case SSA_VALUE_INST: return "inst";
    }
    return "?";
}

bool bir_type_has_generic(const CobraType *type) {
    if (!type) return false;
    if (type->kind == COBRA_TYPE_GENERIC_PARAM) return true;
    for (size_t i = 0; i < type->generic_arg_count; i++) {
        if (bir_type_has_generic(type->generic_args[i])) return true;
    }
    for (size_t i = 0; i < type->field_count; i++) {
        if (bir_type_has_generic(type->fields[i].type)) return true;
    }
    return false;
}
