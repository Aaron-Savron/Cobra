/*
 * Cobra Backend IR: typed HIR / CFG builder.
 *
 * Consumes the existing parser AST for a small scalar subset and produces a
 * CFG with source-level mutable locals. No SSA is constructed here; the SSA
 * pass (ssa_pass.c) consumes this HIR. See docs/BACKEND_IR.md.
 */
#include "ssa.h"

#define BIR_MAX_ARRAY_UNROLL 64

typedef struct {
    BackendIrModule *module;
    HirFunction *fn;
    SsaBlockRef current;
    int synthetic_seq;
    bool failed;
} HirBuilder;

/* ------------------------------------------------------------------ */
/* Diagnostics                                                        */
/* ------------------------------------------------------------------ */

static void bir_fail(HirBuilder *b, int line, int col, const char *fmt, ...) {
    if (!b->module || b->module->error[0]) return;
    char message[COBRA_MAX_TOKEN_TEXT];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    snprintf(b->module->error, sizeof(b->module->error),
             "%.32s:%d:%d: %.50s", b->module->source_file, line > 0 ? line : 1,
             col > 0 ? col : 1, message);
    b->failed = true;
}

/* ------------------------------------------------------------------ */
/* HIR building primitives                                            */
/* ------------------------------------------------------------------ */

static HirBlock *hir_new_block(HirBuilder *b, const char *name, int line, int col) {
    HirFunction *fn = b->fn;
    if (fn->block_count == fn->block_cap) {
        size_t next = fn->block_cap ? fn->block_cap * 2 : 16;
        HirBlock *grown = realloc(fn->blocks, sizeof(HirBlock) * next);
        if (!grown) {
            bir_fail(b, line, col, "out of memory building CFG");
            return NULL;
        }
        fn->blocks = grown;
        fn->block_cap = next;
    }
    HirBlock *block = &fn->blocks[fn->block_count];
    memset(block, 0, sizeof(*block));
    block->id = (SsaBlockRef)fn->block_count;
    snprintf(block->name, sizeof(block->name), "%s", name);
    block->source_line = line;
    block->source_col = col;
    fn->block_count++;
    return block;
}

static bool hir_add_edge(HirBuilder *b, SsaBlockRef from, SsaBlockRef to) {
    HirFunction *fn = b->fn;
    if (from >= fn->block_count || to >= fn->block_count) return false;
    HirBlock *source = &fn->blocks[from];
    HirBlock *target = &fn->blocks[to];
    size_t next = source->succ_count + 1;
    if (next > source->succ_cap) {
        size_t cap = source->succ_cap ? source->succ_cap * 2 : 4;
        SsaBlockRef *grown = realloc(source->succs, sizeof(SsaBlockRef) * cap);
        if (!grown) return false;
        source->succs = grown;
        source->succ_cap = cap;
    }
    next = target->pred_count + 1;
    if (next > target->pred_cap) {
        size_t cap = target->pred_cap ? target->pred_cap * 2 : 4;
        SsaBlockRef *grown = realloc(target->preds, sizeof(SsaBlockRef) * cap);
        if (!grown) return false;
        target->preds = grown;
        target->pred_cap = cap;
    }
    source->succs[source->succ_count++] = to;
    target->preds[target->pred_count++] = from;
    return true;
}

static bool hir_block_add_stmt(HirBuilder *b, SsaBlockRef block, HirStmt stmt) {
    HirBlock *target = &b->fn->blocks[block];
    if (target->term.kind != HIR_TERM_NONE) return false;
    if (target->stmt_count == target->stmt_cap) {
        size_t next = target->stmt_cap ? target->stmt_cap * 2 : 8;
        HirStmt *grown = realloc(target->stmts, sizeof(HirStmt) * next);
        if (!grown) return false;
        target->stmts = grown;
        target->stmt_cap = next;
    }
    target->stmts[target->stmt_count++] = stmt;
    return true;
}

static bool hir_set_term(HirBuilder *b, SsaBlockRef block, HirTerm term) {
    if (b->fn->blocks[block].term.kind != HIR_TERM_NONE) {
        bir_fail(b, 0, 0, "internal error: block already terminated");
        return false;
    }
    b->fn->blocks[block].term = term;
    return true;
}

static HirExpr *hir_expr_alloc(HirBuilder *b, int line, int col) {
    HirExpr *expr = calloc(1, sizeof(HirExpr));
    if (!expr) {
        bir_fail(b, line, col, "out of memory building expression");
        return NULL;
    }
    expr->source_line = line;
    expr->source_col = col;
    return expr;
}

/* Recursively free an expression tree (expr owns its argument list). */
static void hir_expr_free(HirExpr *expr) {
    if (!expr) return;
    for (size_t i = 0; i < expr->arg_count; i++) hir_expr_free(expr->args[i]);
    free(expr->args);
    free(expr);
}

/* Free a built HIR function: block storage, per-block statement and edge
   arrays, and every owned expression tree (statements and terminators). */
static void hir_function_free(HirFunction *fn) {
    if (!fn) return;
    for (size_t i = 0; i < fn->block_count; i++) {
        HirBlock *block = &fn->blocks[i];
        for (size_t j = 0; j < block->stmt_count; j++) {
            hir_expr_free(block->stmts[j].expr);
        }
        free(block->stmts);
        hir_expr_free(block->term.cond);
        hir_expr_free(block->term.ret_expr);
        free(block->preds);
        free(block->succs);
    }
    free(fn->blocks);
    fn->blocks = NULL;
    fn->block_count = 0;
    fn->block_cap = 0;
}

/* ------------------------------------------------------------------ */
/* Locals                                                             */
/* ------------------------------------------------------------------ */

static int hir_find_local(HirBuilder *b, const char *name) {
    for (size_t i = 0; i < b->fn->local_count; i++) {
        if (strcmp(b->fn->locals[i].name, name) == 0) return (int)i;
    }
    return -1;
}

static int hir_add_local(HirBuilder *b, const char *name, bool is_param,
                         int line, int col) {
    int existing = hir_find_local(b, name);
    if (existing >= 0) return existing;
    if (b->fn->local_count >= BIR_MAX_LOCALS) {
        bir_fail(b, line, col, "too many locals in backend-IR function");
        return -1;
    }
    HirLocal *local = &b->fn->locals[b->fn->local_count];
    memset(local, 0, sizeof(*local));
    snprintf(local->name, sizeof(local->name), "%s", name);
    local->is_param = is_param;
    local->source_line = line;
    local->source_col = col;
    return (int)b->fn->local_count++;
}

static int hir_require_local(HirBuilder *b, const char *name, int line, int col) {
    int existing = hir_find_local(b, name);
    if (existing < 0) {
        bir_fail(b, line, col, "use of unknown local '%s'", name);
        return -1;
    }
    return existing;
}

static int hir_synthetic_local(HirBuilder *b, const char *prefix, int line, int col) {
    char name[COBRA_MAX_IDENT_LEN];
    snprintf(name, sizeof(name), "@%s_%d", prefix, b->synthetic_seq++);
    return hir_add_local(b, name, false, line, col);
}

/* ------------------------------------------------------------------ */
/* Expressions                                                        */
/* ------------------------------------------------------------------ */

static bool hir_build_expr(HirBuilder *b, ASTNode *node, HirExpr **out);

static SsaOpcode hir_map_binop(const char *op) {
    if (strcmp(op, "+") == 0) return SSA_OP_ADD;
    if (strcmp(op, "-") == 0) return SSA_OP_SUB;
    if (strcmp(op, "*") == 0) return SSA_OP_MUL;
    if (strcmp(op, "/") == 0) return SSA_OP_DIV;
    if (strcmp(op, "==") == 0) return SSA_OP_EQ;
    if (strcmp(op, "!=") == 0) return SSA_OP_NE;
    if (strcmp(op, "<") == 0) return SSA_OP_LT;
    if (strcmp(op, ">") == 0) return SSA_OP_GT;
    if (strcmp(op, "<=") == 0) return SSA_OP_LE;
    if (strcmp(op, ">=") == 0) return SSA_OP_GE;
    return SSA_OP_NONE;
}

static bool hir_builtin_outside_subset(const char *name) {
    static const char *const builtins[] = {
        "alloc_i64", "alloc_f32", "alloc_u8", "free", "len", "range",
        "enumerate", "concat", "set", "get", "delete", "has", "append",
        "pop", "fill_f32", "sum_f32", "zero_f32", "matmul_f32", "dense_f32",
        "print", NULL
    };
    for (size_t i = 0; builtins[i]; i++) {
        if (strcmp(name, builtins[i]) == 0) return true;
    }
    return false;
}

static bool hir_build_expr(HirBuilder *b, ASTNode *node, HirExpr **out) {
    if (!node) {
        bir_fail(b, 0, 0, "missing expression in backend-IR subset");
        return false;
    }
    HirExpr *expr = NULL;
    switch (node->type) {
        case AST_INT_LITERAL:
            expr = hir_expr_alloc(b, node->source_line, node->source_col);
            if (!expr) return false;
            expr->kind = HIR_EXPR_CONST;
            expr->const_i64 = node->int_val;
            break;
        case AST_BOOL_LITERAL:
            expr = hir_expr_alloc(b, node->source_line, node->source_col);
            if (!expr) return false;
            expr->kind = HIR_EXPR_CONST;
            expr->const_i64 = node->int_val ? 1 : 0;
            break;
        case AST_VAR_REF: {
            int local = hir_require_local(b, node->name,
                                          node->source_line, node->source_col);
            if (local < 0) return false;
            expr = hir_expr_alloc(b, node->source_line, node->source_col);
            if (!expr) return false;
            expr->kind = HIR_EXPR_LOCAL;
            expr->local = (uint32_t)local;
            break;
        }
        case AST_BINARY_OP: {
            SsaOpcode op = hir_map_binop(node->name);
            if (op == SSA_OP_NONE || node->child_count != 2) {
                bir_fail(b, node->source_line, node->source_col,
                         "operator '%s' is outside the backend-IR subset", node->name);
                return false;
            }
            expr = hir_expr_alloc(b, node->source_line, node->source_col);
            if (!expr) return false;
            expr->kind = HIR_EXPR_BINOP;
            expr->binop = op;
            expr->args = calloc(2, sizeof(HirExpr *));
            if (!expr->args) {
                bir_fail(b, node->source_line, node->source_col, "out of memory");
                return false;
            }
            if (!hir_build_expr(b, node->children[0], &expr->args[0]) ||
                !hir_build_expr(b, node->children[1], &expr->args[1])) {
                return false;
            }
            expr->arg_count = 2;
            break;
        }
        case AST_FUNC_CALL: {
            if (hir_builtin_outside_subset(node->name)) {
                bir_fail(b, node->source_line, node->source_col,
                         "builtin '%s' is outside the backend-IR subset", node->name);
                return false;
            }
            expr = hir_expr_alloc(b, node->source_line, node->source_col);
            if (!expr) return false;
            expr->kind = HIR_EXPR_CALL;
            snprintf(expr->callee, sizeof(expr->callee), "%s", node->name);
            if (node->child_count) {
                expr->args = calloc(node->child_count, sizeof(HirExpr *));
                if (!expr->args) {
                    bir_fail(b, node->source_line, node->source_col, "out of memory");
                    return false;
                }
                for (size_t i = 0; i < node->child_count; i++) {
                    if (!hir_build_expr(b, node->children[i], &expr->args[i])) {
                        return false;
                    }
                }
                expr->arg_count = node->child_count;
            }
            break;
        }
        case AST_COMPTIME_EXPR:
            return hir_build_expr(b, node->child_count ? node->children[0] : NULL, out);
        default:
            bir_fail(b, node->source_line, node->source_col,
                     "expression form is outside the backend-IR subset");
            return false;
    }
    *out = expr;
    return true;
}

/* ------------------------------------------------------------------ */
/* Statements and structured control flow                             */
/* ------------------------------------------------------------------ */

static bool hir_emit_assign(HirBuilder *b, uint32_t local, HirExpr *expr) {
    HirStmt stmt;
    memset(&stmt, 0, sizeof(stmt));
    stmt.kind = HIR_STMT_ASSIGN;
    stmt.local = local;
    stmt.expr = expr;
    return hir_block_add_stmt(b, b->current, stmt);
}

static bool hir_build_stmt_list(HirBuilder *b, ASTNode **stmts, size_t stmt_count,
                                 bool *terminated);

static bool hir_emit_simple(HirBuilder *b, HirStmtKind kind, HirExpr *expr) {
    HirStmt stmt;
    memset(&stmt, 0, sizeof(stmt));
    stmt.kind = kind;
    stmt.expr = expr;
    return hir_block_add_stmt(b, b->current, stmt);
}

static bool hir_build_if(HirBuilder *b, ASTNode *stmt, SsaBlockRef *continue_block) {
    if (stmt->child_count < 2) {
        bir_fail(b, stmt->source_line, stmt->source_col,
                 "if statement is missing its body");
        return false;
    }
    HirExpr *cond = NULL;
    if (!hir_build_expr(b, stmt->children[0], &cond)) return false;

    HirBlock *then_block = hir_new_block(b, "then", stmt->source_line, stmt->source_col);
    HirBlock *else_block = stmt->child_count > 2
        ? hir_new_block(b, "else", stmt->source_line, stmt->source_col) : NULL;
    HirBlock *merge = hir_new_block(b, "merge", stmt->source_line, stmt->source_col);
    if (!then_block || !merge || (stmt->child_count > 2 && !else_block)) return false;

    SsaBlockRef pre = b->current;
    HirTerm term;
    memset(&term, 0, sizeof(term));
    term.kind = HIR_TERM_BRANCH;
    term.cond = cond;
    term.target = then_block->id;
    term.target2 = else_block ? else_block->id : merge->id;
    if (!hir_set_term(b, pre, term)) return false;
    if (!hir_add_edge(b, pre, then_block->id) ||
        !hir_add_edge(b, pre, term.target2)) return false;

    b->current = then_block->id;
    bool then_terminated = false;
    if (!hir_build_stmt_list(b, stmt->children[1]->children,
                             stmt->children[1]->child_count, &then_terminated)) {
        return false;
    }
    if (!then_terminated) {
        HirTerm jump;
        memset(&jump, 0, sizeof(jump));
        jump.kind = HIR_TERM_JUMP;
        jump.target = merge->id;
        if (!hir_set_term(b, b->current, jump) ||
            !hir_add_edge(b, b->current, merge->id)) return false;
    }

    if (else_block) {
        b->current = else_block->id;
        bool else_terminated = false;
        if (!hir_build_stmt_list(b, stmt->children[2]->children,
                                 stmt->children[2]->child_count, &else_terminated)) {
            return false;
        }
        if (!else_terminated) {
            HirTerm jump;
            memset(&jump, 0, sizeof(jump));
            jump.kind = HIR_TERM_JUMP;
            jump.target = merge->id;
            if (!hir_set_term(b, b->current, jump) ||
                !hir_add_edge(b, b->current, merge->id)) return false;
        }
    }

    /* The merge block receives its terminator from the continuation: the
       statement list that follows the if, or the function's fall-through
       return. A merge that both branches returned from is unreachable and is
       given a bare return terminator by the function-level fixup. */
    b->current = merge->id;
    *continue_block = merge->id;
    return true;
}

static bool hir_build_while(HirBuilder *b, ASTNode *stmt, SsaBlockRef *continue_block) {
    if (stmt->child_count < 2) {
        bir_fail(b, stmt->source_line, stmt->source_col,
                 "while statement is missing its body");
        return false;
    }
    HirBlock *header = hir_new_block(b, "while_header", stmt->source_line, stmt->source_col);
    HirBlock *body = hir_new_block(b, "while_body", stmt->source_line, stmt->source_col);
    HirBlock *exit_block = hir_new_block(b, "while_exit", stmt->source_line, stmt->source_col);
    if (!header || !body || !exit_block) return false;

    HirTerm pre;
    memset(&pre, 0, sizeof(pre));
    pre.kind = HIR_TERM_JUMP;
    pre.target = header->id;
    if (!hir_set_term(b, b->current, pre) || !hir_add_edge(b, b->current, header->id)) {
        return false;
    }

    b->current = header->id;
    HirExpr *cond = NULL;
    if (!hir_build_expr(b, stmt->children[0], &cond)) return false;
    HirTerm head;
    memset(&head, 0, sizeof(head));
    head.kind = HIR_TERM_BRANCH;
    head.cond = cond;
    head.target = body->id;
    head.target2 = exit_block->id;
    if (!hir_set_term(b, header->id, head) ||
        !hir_add_edge(b, header->id, body->id) ||
        !hir_add_edge(b, header->id, exit_block->id)) {
        return false;
    }

    b->current = body->id;
    bool body_terminated = false;
    if (!hir_build_stmt_list(b, stmt->children[1]->children,
                             stmt->children[1]->child_count, &body_terminated)) {
        return false;
    }
    if (!body_terminated) {
        HirTerm back;
        memset(&back, 0, sizeof(back));
        back.kind = HIR_TERM_JUMP;
        back.target = header->id;
        if (!hir_set_term(b, b->current, back) ||
            !hir_add_edge(b, b->current, header->id)) return false;
    }

    b->current = exit_block->id;
    *continue_block = exit_block->id;
    return true;
}

static bool hir_build_for(HirBuilder *b, ASTNode *stmt, SsaBlockRef *continue_block) {
    if (stmt->secondary_name[0]) {
        bir_fail(b, stmt->source_line, stmt->source_col,
                 "for index,value form is outside the backend-IR subset");
        return false;
    }
    if (stmt->child_count < 2) {
        bir_fail(b, stmt->source_line, stmt->source_col,
                 "for statement is missing its body");
        return false;
    }
    ASTNode *target = stmt->children[0];
    ASTNode *body = stmt->children[1];
    int loop_local = hir_add_local(b, stmt->name, false,
                                   stmt->source_line, stmt->source_col);
    if (loop_local < 0) return false;

    /* Constant array literal: unroll each element into a straight-line copy
       of the body. The loop variable is reassigned at the top of every copy,
       which matches the host interpreter's per-iteration rebinding. */
    if (target->type == AST_ARRAY_LITERAL) {
        if (target->child_count > BIR_MAX_ARRAY_UNROLL) {
            bir_fail(b, stmt->source_line, stmt->source_col,
                     "array literal for-loop exceeds the %d-element unroll limit",
                     BIR_MAX_ARRAY_UNROLL);
            return false;
        }
        for (size_t i = 0; i < target->child_count && !b->failed; i++) {
            HirExpr *element = hir_expr_alloc(b, target->source_line, target->source_col);
            if (!element) return false;
            element->kind = HIR_EXPR_CONST;
            element->const_i64 = target->children[i]->type == AST_INT_LITERAL
                ? target->children[i]->int_val : 0;
            if (!hir_emit_assign(b, (uint32_t)loop_local, element)) return false;
            bool body_terminated = false;
            if (!hir_build_stmt_list(b, body->children, body->child_count,
                                     &body_terminated)) {
                return false;
            }
            if (body_terminated) {
                bir_fail(b, stmt->source_line, stmt->source_col,
                         "return inside a for-loop body is outside the backend-IR subset");
                return false;
            }
        }
        *continue_block = b->current;
        return true;
    }

    /* Scalar-bound and range forms lower to the same CFG as a while loop:
       i = start; while i < bound { body; i = i + 1 }. */
    bool is_range = target->type == AST_FUNC_CALL &&
                    strcmp(target->name, "range") == 0;
    if (is_range && (target->child_count < 1 || target->child_count > 2)) {
        bir_fail(b, stmt->source_line, stmt->source_col,
                 "range() requires one or two arguments");
        return false;
    }
    int start_local = hir_synthetic_local(b, "for_start", stmt->source_line, stmt->source_col);
    int bound_local = hir_synthetic_local(b, "for_bound", stmt->source_line, stmt->source_col);
    if (start_local < 0 || bound_local < 0) return false;

    /* Only pre-allocate the constant start; the range form builds its start
       expression directly so nothing is leaked by an overwrite. */
    HirExpr *start_expr = NULL;
    if (is_range && target->child_count == 2) {
        if (!hir_build_expr(b, target->children[0], &start_expr)) return false;
    } else {
        start_expr = hir_expr_alloc(b, stmt->source_line, stmt->source_col);
        if (!start_expr) return false;
        start_expr->kind = HIR_EXPR_CONST;
        start_expr->const_i64 = 0;
    }
    HirExpr *bound_expr = NULL;
    if (is_range) {
        if (!hir_build_expr(b, target->children[target->child_count - 1], &bound_expr)) {
            return false;
        }
    } else {
        if (!hir_build_expr(b, target, &bound_expr)) return false;
    }
    if (!hir_emit_assign(b, (uint32_t)start_local, start_expr) ||
        !hir_emit_assign(b, (uint32_t)bound_local, bound_expr)) {
        return false;
    }
    /* i = start; the induction variable is a separate local so the loop body
       can read and assign it without touching the fixed start value. */
    HirExpr *start_ref = hir_expr_alloc(b, stmt->source_line, stmt->source_col);
    if (!start_ref) return false;
    start_ref->kind = HIR_EXPR_LOCAL;
    start_ref->local = (uint32_t)start_local;
    if (!hir_emit_assign(b, (uint32_t)loop_local, start_ref)) return false;

    HirBlock *header = hir_new_block(b, "for_header", stmt->source_line, stmt->source_col);
    HirBlock *body_block = hir_new_block(b, "for_body", stmt->source_line, stmt->source_col);
    HirBlock *latch = hir_new_block(b, "for_latch", stmt->source_line, stmt->source_col);
    HirBlock *exit_block = hir_new_block(b, "for_exit", stmt->source_line, stmt->source_col);
    if (!header || !body_block || !latch || !exit_block) return false;

    HirTerm pre;
    memset(&pre, 0, sizeof(pre));
    pre.kind = HIR_TERM_JUMP;
    pre.target = header->id;
    if (!hir_set_term(b, b->current, pre) || !hir_add_edge(b, b->current, header->id)) {
        return false;
    }

    /* header: i < bound -> body : exit */
    HirExpr *index_ref = hir_expr_alloc(b, stmt->source_line, stmt->source_col);
    HirExpr *bound_ref = hir_expr_alloc(b, stmt->source_line, stmt->source_col);
    if (!index_ref || !bound_ref) return false;
    index_ref->kind = HIR_EXPR_LOCAL;
    index_ref->local = (uint32_t)loop_local;
    bound_ref->kind = HIR_EXPR_LOCAL;
    bound_ref->local = (uint32_t)bound_local;
    HirExpr *cond = hir_expr_alloc(b, stmt->source_line, stmt->source_col);
    if (!cond) return false;
    cond->kind = HIR_EXPR_BINOP;
    cond->binop = SSA_OP_LT;
    cond->args = calloc(2, sizeof(HirExpr *));
    if (!cond->args) return false;
    cond->args[0] = index_ref;
    cond->args[1] = bound_ref;
    cond->arg_count = 2;

    b->current = header->id;
    HirTerm head;
    memset(&head, 0, sizeof(head));
    head.kind = HIR_TERM_BRANCH;
    head.cond = cond;
    head.target = body_block->id;
    head.target2 = exit_block->id;
    if (!hir_set_term(b, header->id, head) ||
        !hir_add_edge(b, header->id, body_block->id) ||
        !hir_add_edge(b, header->id, exit_block->id)) {
        return false;
    }

    b->current = body_block->id;
    bool body_terminated = false;
    if (!hir_build_stmt_list(b, body->children, body->child_count,
                             &body_terminated)) {
        return false;
    }
    if (body_terminated) {
        bir_fail(b, stmt->source_line, stmt->source_col,
                 "return inside a for-loop body is outside the backend-IR subset");
        return false;
    }
    HirTerm to_latch;
    memset(&to_latch, 0, sizeof(to_latch));
    to_latch.kind = HIR_TERM_JUMP;
    to_latch.target = latch->id;
    if (!hir_set_term(b, b->current, to_latch) ||
        !hir_add_edge(b, b->current, latch->id)) return false;

    /* latch: i = i + 1; jump header */
    b->current = latch->id;
    HirExpr *one = hir_expr_alloc(b, stmt->source_line, stmt->source_col);
    HirExpr *index_again = hir_expr_alloc(b, stmt->source_line, stmt->source_col);
    HirExpr *plus = hir_expr_alloc(b, stmt->source_line, stmt->source_col);
    if (!one || !index_again || !plus) return false;
    one->kind = HIR_EXPR_CONST;
    one->const_i64 = 1;
    index_again->kind = HIR_EXPR_LOCAL;
    index_again->local = (uint32_t)loop_local;
    plus->kind = HIR_EXPR_BINOP;
    plus->binop = SSA_OP_ADD;
    plus->args = calloc(2, sizeof(HirExpr *));
    if (!plus->args) return false;
    plus->args[0] = index_again;
    plus->args[1] = one;
    plus->arg_count = 2;
    if (!hir_emit_assign(b, (uint32_t)loop_local, plus)) return false;
    HirTerm back;
    memset(&back, 0, sizeof(back));
    back.kind = HIR_TERM_JUMP;
    back.target = header->id;
    if (!hir_set_term(b, latch->id, back) || !hir_add_edge(b, latch->id, header->id)) {
        return false;
    }

    b->current = exit_block->id;
    *continue_block = exit_block->id;
    return true;
}

static bool hir_build_stmt_list(HirBuilder *b, ASTNode **stmts, size_t stmt_count,
                                 bool *terminated) {
    SsaBlockRef cur = b->current;
    bool block_terminated = false;
    for (size_t i = 0; i < stmt_count && !b->failed; i++) {
        ASTNode *stmt = stmts[i];
        if (block_terminated) continue; /* dead code after a return */
        switch (stmt->type) {
            case AST_VAR_DECL:
            case AST_ASSIGN: {
                if (stmt->declared_type != COBRA_TYPE_UNTYPED &&
                    stmt->declared_type != COBRA_TYPE_I64) {
                    bir_fail(b, stmt->source_line, stmt->source_col,
                             "declaration type is outside the scalar backend-IR subset");
                    return false;
                }
                if (stmt->child_count == 0) {
                    bir_fail(b, stmt->source_line, stmt->source_col,
                             "declaration without initializer is outside the backend-IR subset");
                    return false;
                }
                int local = hir_add_local(b, stmt->name, false,
                                          stmt->source_line, stmt->source_col);
                if (local < 0) return false;
                HirExpr *value = NULL;
                if (!hir_build_expr(b, stmt->children[0], &value)) return false;
                if (!hir_emit_assign(b, (uint32_t)local, value)) return false;
                break;
            }
            case AST_RETURN: {
                HirExpr *value = NULL;
                if (stmt->child_count > 0 &&
                    !hir_build_expr(b, stmt->children[0], &value)) {
                    return false;
                }
                HirTerm term;
                memset(&term, 0, sizeof(term));
                term.kind = HIR_TERM_RETURN;
                term.ret_expr = value;
                if (!hir_set_term(b, b->current, term)) return false;
                block_terminated = true;
                break;
            }
            case AST_FUNC_CALL: {
                /* Expression statement: evaluate the call for effect. */
                HirExpr *value = NULL;
                if (!hir_build_expr(b, stmt, &value)) return false;
                if (!hir_emit_simple(b, HIR_STMT_EXPR, value)) return false;
                break;
            }
            case AST_IF_STMT: {
                SsaBlockRef next = cur;
                if (!hir_build_if(b, stmt, &next)) return false;
                cur = next;
                b->current = next;
                break;
            }
            case AST_WHILE_STMT: {
                SsaBlockRef next = cur;
                if (!hir_build_while(b, stmt, &next)) return false;
                cur = next;
                b->current = next;
                break;
            }
            case AST_FOR_LOOP: {
                SsaBlockRef next = cur;
                if (!hir_build_for(b, stmt, &next)) return false;
                cur = next;
                b->current = next;
                break;
            }
            default:
                bir_fail(b, stmt->source_line, stmt->source_col,
                         "statement form is outside the backend-IR subset");
                return false;
        }
    }
    if (terminated) *terminated = block_terminated;
    b->current = cur;
    return !b->failed;
}

/* ------------------------------------------------------------------ */
/* Function entry                                                     */
/* ------------------------------------------------------------------ */

bool bir_build_function(BackendIrModule *module, ASTNode *function,
                        BirFunctionInfo **out_info) {
    if (!module || !function || function->type != AST_FUNCTION) {
        if (module) snprintf(module->error, sizeof(module->error),
                             "backend-IR builder requires an AST_FUNCTION node");
        return false;
    }
    module->error[0] = '\0';
    if (function->generic_param_count > 0) {
        snprintf(module->error, sizeof(module->error),
                 "%s: generic functions are outside the backend-IR subset",
                 function->name);
        return false;
    }
    if (bir_find_function(module, function->name)) {
        snprintf(module->error, sizeof(module->error),
                 "%s: duplicate function in backend-IR module", function->name);
        return false;
    }

    HirBuilder b;
    memset(&b, 0, sizeof(b));
    b.module = module;
    b.current = SSA_BLOCK_NONE;

    HirFunction fn;
    memset(&fn, 0, sizeof(fn));
    snprintf(fn.name, sizeof(fn.name), "%s", function->name);
    b.fn = &fn;

    size_t param_count = 0;
    for (size_t i = 0; i < function->child_count; i++) {
        if (function->children[i]->type == AST_PARAM) param_count++;
    }
    fn.param_count = param_count;

    bool has_return = function->declared_type != COBRA_TYPE_VOID;
    if (!has_return && function->declared_type != COBRA_TYPE_UNTYPED &&
        function->declared_type != COBRA_TYPE_I64 &&
        function->declared_type != COBRA_TYPE_VOID) {
        snprintf(module->error, sizeof(module->error),
                 "%s: return type is outside the scalar backend-IR subset",
                 function->name);
        goto fail;
    }

    HirBlock *entry = hir_new_block(&b, "entry", function->source_line, function->source_col);
    if (!entry) goto fail;
    entry->is_entry = true;
    b.current = entry->id;

    /* Parameters are locals 0..param_count-1. */
    param_count = 0;
    for (size_t i = 0; i < function->child_count; i++) {
        ASTNode *child = function->children[i];
        if (child->type != AST_PARAM) continue;
        if (child->declared_type != COBRA_TYPE_UNTYPED &&
            child->declared_type != COBRA_TYPE_I64) {
            bir_fail(&b, child->source_line, child->source_col,
                     "parameter '%s' is outside the scalar backend-IR subset",
                     child->name);
            goto fail;
        }
        int local = hir_add_local(&b, child->name, true,
                                  child->source_line, child->source_col);
        if (local < 0 || (size_t)local != param_count) {
            bir_fail(&b, child->source_line, child->source_col,
                     "internal error: parameter local ordering");
            goto fail;
        }
        param_count++;
    }

    /* Body statements; a bare fall-through returns 0 for i64 (matching the
       host interpreter) or returns void. */
    bool body_terminated = false;
    size_t body_count = 0;
    for (size_t i = 0; i < function->child_count; i++) {
        if (function->children[i]->type != AST_PARAM) body_count++;
    }
    ASTNode **body_stmts = NULL;
    if (body_count) {
        body_stmts = malloc(sizeof(ASTNode *) * body_count);
        if (!body_stmts) {
            snprintf(module->error, sizeof(module->error), "out of memory");
            goto fail;
        }
        size_t index = 0;
        for (size_t i = 0; i < function->child_count; i++) {
            ASTNode *child = function->children[i];
            if (child->type == AST_PARAM) continue;
            body_stmts[index++] = child;
        }
    }
    if (!hir_build_stmt_list(&b, body_stmts, body_count, &body_terminated)) {
        free(body_stmts);
        goto fail;
    }
    free(body_stmts);
    if (b.failed) goto fail;

    /* Fall-through: i64 -> ret 0; void -> bare ret. */
    if (!body_terminated && b.fn->blocks[b.current].term.kind == HIR_TERM_NONE) {
        HirTerm term;
        memset(&term, 0, sizeof(term));
        term.kind = HIR_TERM_RETURN;
        if (!has_return) {
            HirExpr *zero = hir_expr_alloc(&b, function->source_line, function->source_col);
            if (!zero) goto fail;
            zero->kind = HIR_EXPR_CONST;
            zero->const_i64 = 0;
            term.ret_expr = zero;
        }
        if (!hir_set_term(&b, b.current, term)) goto fail;
    }
    /* Every block must carry a terminator. */
    for (size_t i = 0; i < fn.block_count; i++) {
        if (fn.blocks[i].term.kind == HIR_TERM_NONE) {
            HirTerm term;
            memset(&term, 0, sizeof(term));
            term.kind = HIR_TERM_RETURN;
            if (!has_return) {
                HirExpr *zero = hir_expr_alloc(&b, function->source_line, function->source_col);
                if (!zero) goto fail;
                zero->kind = HIR_EXPR_CONST;
                zero->const_i64 = 0;
                term.ret_expr = zero;
            }
            fn.blocks[i].term = term;
        }
    }

    /* Hand the HIR function to the SSA pass (ssa_pass.c). The pass creates
       the flat SSA blocks and returns the arena entry block for this
       function (block ids are module-global and not per-function). */
    extern bool bir_ssa_lower(BackendIrModule *module, HirFunction *fn,
                              SsaBlockRef *entry_out, SsaValueRef *param_refs_out);
    SsaBlockRef entry_ref = SSA_BLOCK_NONE;
    SsaValueRef param_refs[BIR_MAX_PARAMS];
    memset(param_refs, 0, sizeof(param_refs));
    if (!bir_ssa_lower(module, &fn, &entry_ref, param_refs)) goto fail;
    if (entry_ref == SSA_BLOCK_NONE ||
        !bir_register_function_info(module, function->name, entry_ref, param_count,
                                    param_refs,
                                    has_return ? module->type_i64 : module->type_void,
                                    has_return)) {
        goto fail;
    }

    if (out_info) *out_info = &module->functions[module->function_count - 1];
    hir_function_free(&fn);
    return true;

fail:
    hir_function_free(&fn);
    return false;
}

bool bir_build_program(BackendIrModule *module, ASTNode *root) {
    if (!module || !root) return false;
    module->error[0] = '\0';
    for (size_t i = 0; i < root->child_count; i++) {
        ASTNode *decl = root->children[i];
        if (decl->type == AST_FUNCTION) {
            if (!bir_build_function(module, decl, NULL)) return false;
            continue;
        }
        snprintf(module->error, sizeof(module->error),
                 "%.60s:%d:%d: top-level declaration is outside the backend-IR subset",
                 module->source_file, decl->source_line, decl->source_col);
        return false;
    }
    return true;
}

const BirFunctionInfo *bir_find_function(const BackendIrModule *module,
                                         const char *name) {
    if (!module) return NULL;
    for (size_t i = 0; i < module->function_count; i++) {
        if (strcmp(module->functions[i].name, name) == 0) {
            return &module->functions[i];
        }
    }
    return NULL;
}

void bir_module_init(BackendIrModule *module, const char *source_file) {
    memset(module, 0, sizeof(*module));
    bir_arena_init(&module->arena);
    if (source_file) {
        snprintf(module->source_file, sizeof(module->source_file), "%s", source_file);
    } else {
        snprintf(module->source_file, sizeof(module->source_file), "<source>");
    }
    module->type_arena = calloc(1, sizeof(CobraTypeArena));
    if (!module->type_arena) {
        /* Allocation failure: leave types unset; callers must check for NULL. */
        return;
    }
    cobra_type_arena_init(module->type_arena);
    module->type_i64 = cobra_type_make(module->type_arena, COBRA_TYPE_I64, NULL,
                                       NULL, NULL, NULL, NULL,
                                       COBRA_OWNERSHIP_VALUE,
                                       COBRA_MUTABILITY_DEFAULT, -1);
    module->type_void = cobra_type_make(module->type_arena, COBRA_TYPE_VOID, NULL,
                                        NULL, NULL, NULL, NULL,
                                        COBRA_OWNERSHIP_VALUE,
                                        COBRA_MUTABILITY_DEFAULT, -1);
    if (module->type_i64 && !module->type_i64->finalized) {
        cobra_type_finalize(module->type_arena, (CobraType *)module->type_i64);
    }
    if (module->type_void && !module->type_void->finalized) {
        cobra_type_finalize(module->type_arena, (CobraType *)module->type_void);
    }
}

void bir_module_free(BackendIrModule *module) {
    if (!module) return;
    bir_arena_free(&module->arena);
    free(module->type_arena);
    module->type_arena = NULL;
    memset(module, 0, sizeof(*module));
}
