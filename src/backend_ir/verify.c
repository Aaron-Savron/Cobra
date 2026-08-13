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
        while (top) {
            SsaBlockRef block = stack[top - 1];
            const SsaBlock *b = &arena->blocks[block];
            if (next_succ[block] < b->succ_count) {
                SsaBlockRef succ = b->succs[next_succ[block]++];
                if (succ < blocks && !visited[succ]) {
                    visited[succ] = true;
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
    for (size_t f = 0; f < ctx->module->function_count; f++) {
        SsaBlockRef entry = ctx->module->functions[f].entry;
        if (entry < blocks && ctx->reachable[entry]) {
            ctx->dom[entry * blocks + entry] = true;
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

static bool check_function_table(VerifyCtx *ctx) {
    const SsaArena *arena = &ctx->module->arena;
    for (size_t f = 0; f < ctx->module->function_count; f++) {
        const BirFunctionInfo *info = &ctx->module->functions[f];
        if (info->entry == SSA_BLOCK_NONE || info->entry >= arena->block_count) {
            verr(ctx, "function '%s' has an invalid entry block", info->name);
            return false;
        }
        if (!info->return_type || !info->return_type->finalized ||
            bir_type_has_generic(info->return_type)) {
            verr(ctx, "function '%s' has an invalid return type", info->name);
            return false;
        }
        if (info->return_abi != info->return_type->abi) {
            verr(ctx, "function '%s' has inconsistent return ABI metadata", info->name);
            return false;
        }
        for (size_t p = 0; p < info->param_count; p++) {
            if (!info->param_types[p] || !info->param_types[p]->finalized ||
                bir_type_has_generic(info->param_types[p]) ||
                info->param_abi[p] != info->param_types[p]->abi) {
                verr(ctx, "function '%s' has invalid parameter ABI metadata", info->name);
                return false;
            }
        }
    }
    return true;
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
        for (size_t o = 0; o < inst->operand_count; o++) {
            SsaValueRef operand = arena->operands[inst->operand_start + o];
            if (!valid_value(ctx, operand)) {
                verr(ctx, "block b%u instruction i%u references invalid operand v%u",
                     ref, iref, operand);
                return false;
            }
        }
        if (inst->effect != SSA_EFFECT_NONE &&
            inst->op != SSA_OP_LOAD && inst->op != SSA_OP_STORE &&
            inst->op != SSA_OP_CALL) {
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
        if (inst->op == SSA_OP_LOAD || inst->op == SSA_OP_STORE) {
            SsaEffect expected = inst->op == SSA_OP_LOAD
                ? SSA_EFFECT_READ : SSA_EFFECT_WRITE;
            if (inst->effect != expected || inst->address_kind != SSA_ADDRESS_INTEGER_SLOT ||
                inst->memory_width == 0 || inst->memory_alignment == 0 ||
                inst->address_space != 0 ||
                arena->values[arena->operands[inst->operand_start]].type !=
                    ctx->module->type_i64) {
                verr(ctx, "block b%u memory instruction has incomplete slot-memory metadata", ref);
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
            if (inst->operand_count != callee->param_count) {
                verr(ctx, "block b%u call '%s' has %u arguments, expected %zu",
                     ref, inst->callee, inst->operand_count, callee->param_count);
                return false;
            }
            for (size_t arg = 0; arg < callee->param_count; arg++) {
                const SsaValue *value = &arena->values[
                    arena->operands[inst->operand_start + arg]];
                if (!value->type || value->type != callee->param_types[arg]) {
                    verr(ctx, "block b%u call '%s' argument %zu has the wrong canonical type",
                         ref, inst->callee, arg + 1);
                    return false;
                }
            }
            if (callee->has_return) {
                if (!inst->type || inst->type != callee->return_type ||
                    inst->result == SSA_VALUE_NONE) {
                    verr(ctx, "block b%u call '%s' has the wrong result type",
                         ref, inst->callee);
                    return false;
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
            if (term->edge_count != arena->blocks[term->target].param_count) {
                verr(ctx, "block b%u jump edge args (%u) do not match block b%u params (%zu)",
                     ref, term->edge_count, term->target,
                     arena->blocks[term->target].param_count);
                return false;
            }
            for (size_t e = 0; e < term->edge_count; e++) {
                if (!valid_value(ctx, arena->edges[term->edge_start + e])) {
                    verr(ctx, "block b%u jump passes an invalid edge argument", ref);
                    return false;
                }
            }
            break;
        case SSA_OP_BRANCH:
            if (term->operand_count != 1 ||
                !valid_value(ctx, arena->operands[term->operand_start]) ||
                arena->values[arena->operands[term->operand_start]].type != ctx->module->type_bool) {
                verr(ctx, "block b%u branch has an invalid condition", ref);
                return false;
            }
            if (term->target == SSA_BLOCK_NONE || term->target >= arena->block_count ||
                term->target2 == SSA_BLOCK_NONE || term->target2 >= arena->block_count) {
                verr(ctx, "block b%u branch targets an invalid block", ref);
                return false;
            }
            if (term->edge_count != arena->blocks[term->target].param_count ||
                term->edge2_count != arena->blocks[term->target2].param_count) {
                verr(ctx, "block b%u branch edge-arg arity mismatch", ref);
                return false;
            }
            for (size_t e = 0; e < term->edge_count; e++) {
                if (!valid_value(ctx, arena->edges[term->edge_start + e])) return false;
            }
            for (size_t e = 0; e < term->edge2_count; e++) {
                if (!valid_value(ctx, arena->edges[term->edge2_start + e])) return false;
            }
            break;
        case SSA_OP_RETURN:
            if (term->operand_count > 1) {
                verr(ctx, "block b%u return has too many operands", ref);
                return false;
            }
            break;
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
                if (!ctx->dom[def_block * blocks + b]) {
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
                    !ctx->dom[def_block * blocks + b]) {
                    verr(ctx, "edge argument v%zu in block b%u lacks a dominating definition",
                         v, b);
                    return false;
                }
            }
            for (size_t e = 0; e < term->edge2_count; e++) {
                if (arena->edges[term->edge2_start + e] == v &&
                    !ctx->dom[def_block * blocks + b]) {
                    verr(ctx, "edge argument v%zu in block b%u lacks a dominating definition",
                         v, b);
                    return false;
                }
            }
            if (term->op == SSA_OP_RETURN && term->operand_count == 1 &&
                arena->operands[term->operand_start] == v &&
                !ctx->dom[def_block * blocks + b]) {
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
    ctx.dom = calloc((blocks ? blocks : 1) * (blocks ? blocks : 1), sizeof(bool));
    ctx.rpo = calloc(blocks ? blocks : 1, sizeof(SsaBlockRef));
    if (!ctx.reachable || !ctx.dom || !ctx.rpo) {
        verr(&ctx, "out of memory in verifier");
        free(ctx.reachable);
        free(ctx.dom);
        free(ctx.rpo);
        return false;
    }

    compute_reachability(&ctx);
    compute_dominators(&ctx);

    bool ok = check_function_table(&ctx);
    if (ok) ok = check_types(&ctx);
    /* Slot 0 is the reserved invalid-handle sentinel in every pool. */
    for (size_t b = 1; ok && b < blocks; b++) {
        ok = check_block(&ctx, (SsaBlockRef)b);
    }
    if (ok) ok = check_dominance(&ctx);

    free(ctx.reachable);
    free(ctx.dom);
    free(ctx.rpo);
    return ok;
}
