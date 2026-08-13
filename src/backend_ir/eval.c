/*
 * Cobra Backend IR: SSA evaluator.
 *
 * Executes the flat SSA form directly. Every call frame owns its own value
 * array indexed by SsaValueRef, so recursion and re-entrant calls cannot
 * clobber an outer invocation's locals, parameters, or block arguments. Block
 * parameters are bound from edge arguments at block entry; a flat memory
 * backs load/store. Step and depth limits keep malformed IR from hanging the
 * host.
 */
#include "ssa.h"

typedef struct {
    int64_t *slots;            /* indexed by SsaValueRef (frame-owned) */
    SsaBlockRef return_block;
    size_t return_inst_index;
    SsaValueRef call_result_slot;
} EvalFrame;

typedef struct {
    const BackendIrModule *module;
    size_t slot_count;
    int64_t memory[BIR_MEMORY_SLOTS];
    EvalFrame stack[BIR_MAX_CALL_DEPTH];
    size_t depth;
    int64_t *current_slots;
    uint64_t steps;
    bool failed;
} SsaEval;

static void eval_fail(SsaEval *ev, const char *fmt, ...) {
    if (ev->failed) return;
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "backend-IR eval: ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
    ev->failed = true;
}

static int64_t eval_value(SsaEval *ev, SsaValueRef ref) {
    if (ref == SSA_VALUE_NONE || ref >= ev->slot_count) return 0;
    const SsaValue *value = &ev->module->arena.values[ref];
    if (value->kind == SSA_VALUE_CONST) return value->const_i64;
    return ev->current_slots[ref];
}

static bool eval_bind_function_params(SsaEval *ev, const BirFunctionInfo *info,
                                      const int64_t *args, size_t arg_count,
                                      int64_t entry_default) {
    for (size_t k = 0; k < info->param_count; k++) {
        SsaValueRef param = info->params[k];
        if (param == SSA_VALUE_NONE || param >= ev->slot_count) {
            eval_fail(ev, "function '%s' has an invalid parameter slot", info->name);
            return false;
        }
        int64_t arg = k < arg_count ? args[k] : entry_default;
        ev->current_slots[param] = arg;
    }
    return true;
}

static bool eval_bind_block_params(SsaEval *ev, SsaBlockRef block,
                                   const SsaValueRef *edge_args, size_t edge_count) {
    const SsaBlock *target = &ev->module->arena.blocks[block];
    if (edge_count != target->param_count) {
        eval_fail(ev, "block b%u param/edge arity mismatch (%zu vs %u)",
                  block, edge_count, (unsigned)target->param_count);
        return false;
    }
    for (size_t k = 0; k < edge_count; k++) {
        if (target->params[k] >= ev->slot_count) {
            eval_fail(ev, "invalid block parameter slot");
            return false;
        }
        ev->current_slots[target->params[k]] = eval_value(ev, edge_args[k]);
    }
    return true;
}

static bool eval_call(SsaEval *ev, const SsaInst *inst, SsaBlockRef *next_block,
                      size_t *next_inst) {
    const BirFunctionInfo *callee = bir_find_function(ev->module, inst->callee);
    if (!callee) {
        eval_fail(ev, "call to unknown function '%s'", inst->callee);
        return false;
    }
    int64_t args[BIR_MAX_PARAMS];
    size_t arg_count = inst->operand_count;
    if (arg_count > BIR_MAX_PARAMS) {
        eval_fail(ev, "call '%s' has too many arguments", inst->callee);
        return false;
    }
    for (size_t o = 0; o < arg_count; o++) {
        args[o] = eval_value(ev, ev->module->arena.operands[inst->operand_start + o]);
    }
    if (ev->depth >= BIR_MAX_CALL_DEPTH) {
        eval_fail(ev, "call depth exceeded in '%s'", inst->callee);
        return false;
    }
    /* The callee gets a fresh value array so recursion cannot clobber the
       caller's locals, parameters, or block arguments. */
    int64_t *callee_slots = calloc(ev->slot_count ? ev->slot_count : 1,
                                   sizeof(int64_t));
    if (!callee_slots) {
        eval_fail(ev, "out of memory in evaluator");
        return false;
    }
    EvalFrame *frame = &ev->stack[ev->depth++];
    frame->slots = ev->current_slots;
    frame->return_block = *next_block;
    frame->return_inst_index = *next_inst;
    frame->call_result_slot = inst->result;
    ev->current_slots = callee_slots;
    if (!eval_bind_function_params(ev, callee, args, arg_count, 0)) {
        return false;
    }
    *next_block = callee->entry;
    *next_inst = 0;
    return true;
}

static bool eval_terminator(SsaEval *ev, const SsaInst *term, SsaBlockRef block,
                            SsaBlockRef *next_block, size_t *next_inst,
                            bool *returned, int64_t *result) {
    const SsaArena *arena = &ev->module->arena;
    switch (term->op) {
        case SSA_OP_JUMP: {
            if (!eval_bind_block_params(ev, term->target,
                                        &arena->edges[term->edge_start],
                                        term->edge_count)) {
                return false;
            }
            *next_block = term->target;
            *next_inst = 0;
            return true;
        }
        case SSA_OP_BRANCH: {
            int64_t cond = eval_value(ev, arena->operands[term->operand_start]);
            SsaBlockRef target = cond != 0 ? term->target : term->target2;
            const SsaValueRef *edge_args = cond != 0
                ? &arena->edges[term->edge_start] : &arena->edges[term->edge2_start];
            size_t edge_count = cond != 0 ? term->edge_count : term->edge2_count;
            if (!eval_bind_block_params(ev, target, edge_args, edge_count)) return false;
            *next_block = target;
            *next_inst = 0;
            return true;
        }
        case SSA_OP_RETURN: {
            int64_t value = term->operand_count == 1
                ? eval_value(ev, arena->operands[term->operand_start]) : 0;
            if (ev->depth == 0) {
                *result = value;
                *returned = true;
                return true;
            }
            EvalFrame frame = ev->stack[--ev->depth];
            int64_t *callee_slots = ev->current_slots;
            ev->current_slots = frame.slots;
            if (frame.call_result_slot != SSA_VALUE_NONE &&
                frame.call_result_slot < ev->slot_count) {
                ev->current_slots[frame.call_result_slot] = value;
            }
            free(callee_slots);
            *next_block = frame.return_block;
            *next_inst = frame.return_inst_index;
            return true;
        }
        default:
            eval_fail(ev, "block b%u terminator is not a terminator", block);
            return false;
    }
}

bool bir_eval_function(const BackendIrModule *module, const char *name,
                       int64_t *result) {
    if (!module || !name || !result) return false;
    const BirFunctionInfo *info = bir_find_function(module, name);
    if (!info) {
        fprintf(stderr, "backend-IR eval: unknown function '%s'\n", name);
        return false;
    }
    const SsaArena *arena = &module->arena;
    if (info->entry == SSA_BLOCK_NONE || info->entry >= arena->block_count) {
        fprintf(stderr, "backend-IR eval: invalid entry for '%s'\n", name);
        return false;
    }

    SsaEval ev;
    memset(&ev, 0, sizeof(ev));
    ev.module = module;
    ev.slot_count = arena->value_count;
    ev.current_slots = calloc(ev.slot_count ? ev.slot_count : 1, sizeof(int64_t));
    if (!ev.current_slots) return false;

    if (!eval_bind_function_params(&ev, info, NULL, 0, 0)) {
        fprintf(stderr, "backend-IR eval: cannot bind params for '%s'\n", name);
        free(ev.current_slots);
        return false;
    }

    SsaBlockRef current = info->entry;
    size_t inst_index = 0;
    bool returned = false;
    int64_t result_value = 0;

    while (!returned && !ev.failed) {
        if (++ev.steps > BIR_MAX_STEPS) {
            eval_fail(&ev, "step limit exceeded");
            break;
        }
        const SsaBlock *block = &arena->blocks[current];
        if (inst_index >= block->inst_count) {
            eval_fail(&ev, "block b%u ran past its terminator", current);
            break;
        }
        const SsaInst *inst = &arena->insts[block->insts[inst_index]];
        SsaBlockRef next_block = current;
        size_t next_inst = inst_index + 1;
        switch (inst->op) {
            case SSA_OP_CONST:
                if (inst->result != SSA_VALUE_NONE && inst->result < ev.slot_count) {
                    ev.current_slots[inst->result] =
                        arena->values[inst->result].const_i64;
                }
                break;
            case SSA_OP_PARAM:
            case SSA_OP_BLOCK_ARG:
                break; /* bound at function/block entry */
            case SSA_OP_ADD:
            case SSA_OP_SUB:
            case SSA_OP_MUL:
            case SSA_OP_DIV:
            case SSA_OP_REM: {
                int64_t lhs = eval_value(&ev, arena->operands[inst->operand_start]);
                int64_t rhs = eval_value(&ev, arena->operands[inst->operand_start + 1]);
                int64_t value = 0;
                if (inst->op == SSA_OP_ADD) value = lhs + rhs;
                else if (inst->op == SSA_OP_SUB) value = lhs - rhs;
                else if (inst->op == SSA_OP_MUL) value = lhs * rhs;
                else if (inst->op == SSA_OP_DIV || inst->op == SSA_OP_REM) {
                    if (rhs == 0) {
                        eval_fail(&ev, "division by zero");
                        break;
                    }
                    value = inst->op == SSA_OP_DIV ? lhs / rhs : lhs % rhs;
                }
                if (inst->result != SSA_VALUE_NONE && inst->result < ev.slot_count) {
                    ev.current_slots[inst->result] = value;
                }
                break;
            }
            case SSA_OP_NEG: {
                int64_t value = -eval_value(&ev, arena->operands[inst->operand_start]);
                if (inst->result != SSA_VALUE_NONE && inst->result < ev.slot_count) {
                    ev.current_slots[inst->result] = value;
                }
                break;
            }
            case SSA_OP_EQ:
            case SSA_OP_NE:
            case SSA_OP_LT:
            case SSA_OP_LE:
            case SSA_OP_GT:
            case SSA_OP_GE: {
                int64_t lhs = eval_value(&ev, arena->operands[inst->operand_start]);
                int64_t rhs = eval_value(&ev, arena->operands[inst->operand_start + 1]);
                bool cmp = false;
                switch (inst->op) {
                    case SSA_OP_EQ: cmp = lhs == rhs; break;
                    case SSA_OP_NE: cmp = lhs != rhs; break;
                    case SSA_OP_LT: cmp = lhs < rhs; break;
                    case SSA_OP_LE: cmp = lhs <= rhs; break;
                    case SSA_OP_GT: cmp = lhs > rhs; break;
                    case SSA_OP_GE: cmp = lhs >= rhs; break;
                    default: break;
                }
                if (inst->result != SSA_VALUE_NONE && inst->result < ev.slot_count) {
                    ev.current_slots[inst->result] = cmp ? 1 : 0;
                }
                break;
            }
            case SSA_OP_LOAD: {
                int64_t address = eval_value(&ev, arena->operands[inst->operand_start]);
                if (address < 0 || address >= BIR_MEMORY_SLOTS) {
                    eval_fail(&ev, "load out of bounds (%lld)", (long long)address);
                    break;
                }
                if (inst->result != SSA_VALUE_NONE && inst->result < ev.slot_count) {
                    ev.current_slots[inst->result] = ev.memory[address];
                }
                break;
            }
            case SSA_OP_STORE: {
                int64_t address = eval_value(&ev, arena->operands[inst->operand_start]);
                int64_t value = eval_value(&ev, arena->operands[inst->operand_start + 1]);
                if (address < 0 || address >= BIR_MEMORY_SLOTS) {
                    eval_fail(&ev, "store out of bounds (%lld)", (long long)address);
                    break;
                }
                ev.memory[address] = value;
                break;
            }
            case SSA_OP_CALL:
                if (!eval_call(&ev, inst, &next_block, &next_inst)) {
                    ev.failed = true;
                }
                break;
            case SSA_OP_JUMP:
            case SSA_OP_BRANCH:
            case SSA_OP_RETURN:
                if (!eval_terminator(&ev, inst, current, &next_block, &next_inst,
                                     &returned, &result_value)) {
                    ev.failed = true;
                }
                break;
            default:
                eval_fail(&ev, "unknown opcode in evaluator");
                break;
        }
        if (ev.failed) break;
        current = next_block;
        inst_index = next_inst;
    }

    /* Any frames left on the stack are leaked by design in the failure path
       only; free every remaining frame's value array. */
    for (size_t i = 0; i < ev.depth; i++) free(ev.stack[i].slots);

    bool ok = !ev.failed && returned;
    if (ok) *result = result_value;
    free(ev.current_slots);
    return ok;
}
