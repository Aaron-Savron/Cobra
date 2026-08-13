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
 *   3. block parameters are exactly the live-in locals, ordered by local
 *      index for determinism;
 *   4. edge arguments on every terminator are the predecessor's exit values
 *      of the successor's parameters (last assignment, else the block's own
 *      parameter value, which recurses to a defining assignment or function
 *      parameter).
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
    SsaBlockRef base;          /* first arena block id for this function */
    SsaValueRef *param_refs;   /* function-param value refs (out)          */
} SsaPass;

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
            case HIR_STMT_EXPR:
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

static bool block_is_param_local(SsaPass *p, size_t block, size_t local) {
    if (block != 0) return false;
    return p->fn->locals[local].is_param;
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

    for (size_t b = 0; b < fn->block_count; b++) {
        for (size_t l = 0; l < locals; l++) {
            if (!p->live_in[b * locals + l]) continue;
            if (b == 0) {
                /* The entry block has no predecessors; a live-in local must
                   be a function parameter or the program reads before write. */
                if (!block_is_param_local(p, b, l)) {
                    ssa_fail(p, fn->blocks[b].source_line, fn->blocks[b].source_col,
                             "local '%s' is read before assignment on some path",
                             fn->locals[l].name);
                    return false;
                }
                continue;
            }
            SsaValueRef param = bir_add_block_param(
                arena, p->base + b, p->fn->locals[l].type,
                fn->blocks[b].source_line, fn->blocks[b].source_col);
            if (param == SSA_VALUE_NONE) {
                ssa_fail(p, fn->blocks[b].source_line, fn->blocks[b].source_col,
                         "out of memory creating block parameters");
                return false;
            }
            p->param_values[b * locals + l] = param;
            p->param_created[b * locals + l] = true;
            /* Parameters are entry values: the first read of the local in the
               block sees the incoming edge value. */
            p->block_defs[b * locals + l] = param;
            p->block_def_known[b * locals + l] = true;
        }
    }

    /* Function parameters are SSA values defined at the entry block. Their
       value refs are recorded so the evaluator can bind the correct function's
       parameters even though the module value pool is shared. */
    for (size_t i = 0; i < fn->param_count; i++) {
        SsaValueRef param = bir_add_value(arena, SSA_VALUE_PARAM,
                                          p->fn->param_types[i],
                                          fn->locals[i].source_line,
                                          fn->locals[i].source_col);
        if (param == SSA_VALUE_NONE) {
            ssa_fail(p, fn->locals[i].source_line, fn->locals[i].source_col,
                     "out of memory creating function parameters");
            return false;
        }
        arena->values[param].param_index = (uint32_t)i;
        if (p->param_refs && i < fn->param_count) p->param_refs[i] = param;
        p->block_defs[0 * locals + i] = param;
        p->block_def_known[0 * locals + i] = true;
    }
    return true;
}

static SsaValueRef ssa_eval_expr(SsaPass *p, size_t block, HirExpr *expr) {
    SsaArena *arena = &p->module->arena;
    switch (expr->kind) {
        case HIR_EXPR_CONST:
            return bir_add_const(arena, expr->type, expr->const_i64,
                                 expr->source_line, expr->source_col);
        case HIR_EXPR_LOCAL: {
            const size_t index = block * p->local_count + expr->local;
            if (!p->block_def_known[index]) {
                ssa_fail(p, expr->source_line, expr->source_col,
                         "internal error: local '%s' has no SSA value",
                         p->fn->locals[expr->local].name);
                return SSA_VALUE_NONE;
            }
            return p->block_defs[index];
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
        case HIR_EXPR_CALL: {
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

/* Exit value of local L from block B: the last assignment in B, else B's own
   parameter value (the value B received), which recursion guarantees exists
   because liveness forwards unassigned locals through their blocks. */
static SsaValueRef ssa_exit_value(SsaPass *p, size_t block, size_t local) {
    const size_t index = block * p->local_count + local;
    if (p->block_def_known[index]) return p->block_defs[index];
    if (p->param_created[index]) return p->param_values[index];
    ssa_fail(p, p->fn->blocks[block].source_line, p->fn->blocks[block].source_col,
             "internal error: no exit value for local '%s'",
             p->fn->locals[local].name);
    return SSA_VALUE_NONE;
}

/* Build the edge-argument list for an edge from B to target by taking the
   exit value of each target parameter (ascending local order, matching the
   parameter creation order). */
static bool ssa_edge_args(SsaPass *p, size_t block, size_t target,
                          SsaValueRef **args_out, size_t *count_out) {
    const size_t locals = p->local_count;
    size_t count = 0;
    for (size_t l = 0; l < locals; l++) {
        if (p->live_in[target * locals + l]) count++;
    }
    if (count == 0) {
        *args_out = NULL;
        *count_out = 0;
        return true;
    }
    SsaValueRef *args = calloc(count, sizeof(SsaValueRef));
    if (!args) return false;
    size_t index = 0;
    for (size_t l = 0; l < locals; l++) {
        if (!p->live_in[target * locals + l]) continue;
        SsaValueRef value = ssa_exit_value(p, block, l);
        if (value == SSA_VALUE_NONE) {
            free(args);
            return false;
        }
        args[index++] = value;
    }
    *args_out = args;
    *count_out = index;
    return true;
}

static bool emit_block(SsaPass *p, size_t block, HirBlock *hb) {
    SsaArena *arena = &p->module->arena;
    const SsaBlockRef ref = p->base + (SsaBlockRef)block;

    for (size_t i = 0; i < hb->stmt_count; i++) {
        HirStmt *stmt = &hb->stmts[i];
        switch (stmt->kind) {
            case HIR_STMT_ASSIGN: {
                SsaValueRef value = ssa_eval_expr(p, block, stmt->expr);
                if (value == SSA_VALUE_NONE) return false;
                const size_t index = block * p->local_count + stmt->local;
                p->block_defs[index] = value;
                p->block_def_known[index] = true;
                break;
            }
            case HIR_STMT_EXPR: {
                SsaValueRef value = ssa_eval_expr(p, block, stmt->expr);
                if (value == SSA_VALUE_NONE && stmt->expr->type != p->module->type_void) {
                    return false;
                }
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
                value = ssa_eval_expr(p, block, hb->term.ret_expr);
                if (value == SSA_VALUE_NONE) return false;
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

    for (size_t b = 0; b < fn->block_count; b++) {
        analyze_block(&pass, b, &fn->blocks[b]);
    }
    compute_liveness(&pass);
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
    return true;

fail:
    free(pass.reads_before_assign);
    free(pass.assigns);
    free(pass.live_in);
    free(pass.block_defs);
    free(pass.block_def_known);
    free(pass.param_values);
    free(pass.param_created);
    return false;
}
