#include "../include/cobra.h"

#define COMPTIME_MAX_STEPS 10000
#define EVAL_MAX_LOCALS 128
#define EVAL_MAX_LOOP_STEPS 10000

typedef struct {
    char name[COBRA_MAX_IDENT_LEN];
    long long value;
    size_t array_start;
    size_t array_len;
    bool is_array;
    bool owned;
    bool freed;
} EvalLocal;

typedef struct {
    ASTNode *root;
    EvalLocal locals[EVAL_MAX_LOCALS];
    size_t local_count;
    long long array_storage[1024];
    size_t array_used;
    int call_depth;
    int failed;
} HostEvaluator;

static int comptime_step_count = 0;

void interpreter_reset_steps(void) {
    comptime_step_count = 0;
}

/* Sandboxed Compile-Time AST Interpreter for @comptime execution. */
int interpreter_eval_expr(ASTNode *node) {
    if (!node) return 0;

    comptime_step_count++;
    if (comptime_step_count > COMPTIME_MAX_STEPS) {
        fprintf(stderr, "Compile Error: @comptime execution exceeded max step limit (%d steps). Infinite loop detected!\n", COMPTIME_MAX_STEPS);
        exit(1);
    }

    if (node->type == AST_INT_LITERAL) return (int)node->literal_i64;
    if (node->type == AST_COMPTIME_EXPR) {
        return node->child_count > 0 ? interpreter_eval_expr(node->children[0]) : 0;
    }
    if (node->type == AST_BINARY_OP) {
        int left = node->child_count > 0 ? interpreter_eval_expr(node->children[0]) : 0;
        int right = node->child_count > 1 ? interpreter_eval_expr(node->children[1]) : 0;
        if (strcmp(node->name, "+") == 0) return left + right;
        if (strcmp(node->name, "-") == 0) return left - right;
        if (strcmp(node->name, "*") == 0) return left * right;
        if (strcmp(node->name, "/") == 0 || strcmp(node->name, "//") == 0) {
            if (right == 0) {
                fprintf(stderr, "Compile Error: division by zero in @comptime expression\n");
                exit(1);
            }
            return left / right;
        }
        if (strcmp(node->name, "**") == 0) {
            long long result = 1;
            for (long long i = 0; i < right; i++) result *= left;
            return result;
        }
        if (strcmp(node->name, "==") == 0) return left == right;
        if (strcmp(node->name, "!=") == 0) return left != right;
        if (strcmp(node->name, "<") == 0) return left < right;
        if (strcmp(node->name, ">") == 0) return left > right;
        if (strcmp(node->name, "<=") == 0) return left <= right;
        if (strcmp(node->name, ">=") == 0) return left >= right;
    }
    return 0;
}

static EvalLocal *eval_find_local_entry(HostEvaluator *ev, const char *name) {
    for (size_t i = 0; i < ev->local_count; i++) {
        if (strcmp(ev->locals[i].name, name) == 0) return &ev->locals[i];
    }
    return NULL;
}

static bool eval_find_local(HostEvaluator *ev, const char *name, long long *value) {
    EvalLocal *local = eval_find_local_entry(ev, name);
    if (!local || local->is_array) return false;
    if (value) *value = local->value;
    return true;
}

static bool eval_set_local(HostEvaluator *ev, const char *name, long long value) {
    for (size_t i = 0; i < ev->local_count; i++) {
        if (strcmp(ev->locals[i].name, name) == 0) {
            ev->locals[i].value = value;
            ev->locals[i].is_array = false;
            ev->locals[i].array_len = 0;
            return true;
        }
    }
    if (ev->local_count >= EVAL_MAX_LOCALS) return false;
    snprintf(ev->locals[ev->local_count].name, sizeof(ev->locals[ev->local_count].name), "%s", name);
    ev->locals[ev->local_count].value = value;
    ev->locals[ev->local_count].array_start = 0;
    ev->locals[ev->local_count].array_len = 0;
    ev->locals[ev->local_count].is_array = false;
    ev->local_count++;
    return true;
}

static long long eval_expression(HostEvaluator *ev, ASTNode *node);

static bool eval_set_array(HostEvaluator *ev, const char *name, ASTNode *literal) {
    if (!literal || literal->type != AST_ARRAY_LITERAL ||
        ev->array_used + literal->child_count > 1024) return false;
    EvalLocal *local = eval_find_local_entry(ev, name);
    if (!local) {
        if (ev->local_count >= EVAL_MAX_LOCALS) return false;
        local = &ev->locals[ev->local_count++];
        snprintf(local->name, sizeof(local->name), "%s", name);
    }
    local->array_start = ev->array_used;
    local->array_len = literal->child_count;
    local->is_array = true;
    local->owned = false;
    local->freed = false;
    for (size_t i = 0; i < literal->child_count; i++) {
        ev->array_storage[ev->array_used++] = eval_expression(ev, literal->children[i]);
    }
    return !ev->failed;
}

static ASTNode *eval_find_function(ASTNode *root, const char *name) {
    if (!root) return NULL;
    for (size_t i = 0; i < root->child_count; i++) {
        ASTNode *child = root->children[i];
        if (child->type == AST_FUNCTION && strcmp(child->name, name) == 0) return child;
    }
    return NULL;
}

static bool eval_function_body(HostEvaluator *ev, ASTNode *function, int *return_code);

static long long eval_binary(const char *op, long long left, long long right) {
    if (strcmp(op, "+") == 0) return left + right;
    if (strcmp(op, "-") == 0) return left - right;
    if (strcmp(op, "*") == 0) return left * right;
    if (strcmp(op, "/") == 0 || strcmp(op, "//") == 0) return right == 0 ? 0 : left / right;
    if (strcmp(op, "**") == 0) {
        long long result = 1;
        for (long long i = 0; i < right; i++) result *= left;
        return result;
    }
    if (strcmp(op, "==") == 0) return left == right;
    if (strcmp(op, "!=") == 0) return left != right;
    if (strcmp(op, "<") == 0) return left < right;
    if (strcmp(op, ">") == 0) return left > right;
    if (strcmp(op, "<=") == 0) return left <= right;
    if (strcmp(op, ">=") == 0) return left >= right;
    return 0;
}

static long long eval_expression(HostEvaluator *ev, ASTNode *node) {
    if (!node) return 0;
    switch (node->type) {
        case AST_INT_LITERAL:
            return node->literal_i64;
        case AST_VAR_REF: {
            long long value = 0;
            if (!eval_find_local(ev, node->name, &value)) ev->failed = 1;
            return value;
        }
        case AST_ARRAY_LITERAL:
            return 0;
        case AST_ARRAY_INDEX: {
            EvalLocal *array = eval_find_local_entry(ev, node->name);
            long long index = node->child_count > 0 ? eval_expression(ev, node->children[0]) : 0;
            if (!array || !array->is_array || index < 0 || (size_t)index >= array->array_len) {
                ev->failed = 1;
                return 0;
            }
            return ev->array_storage[array->array_start + (size_t)index];
        }
        case AST_COMPTIME_EXPR:
            return node->child_count > 0 ? eval_expression(ev, node->children[0]) : 0;
        case AST_BINARY_OP: {
            long long left = node->child_count > 0 ? eval_expression(ev, node->children[0]) : 0;
            long long right = node->child_count > 1 ? eval_expression(ev, node->children[1]) : 0;
            if (strcmp(node->name, "/") == 0 && right == 0) {
                ev->failed = 1;
                return 0;
            }
            return eval_binary(node->name, left, right);
        }
        case AST_LEN_EXPR:
            if (node->child_count == 0) return 0;
            if (node->children[0]->type == AST_ARRAY_LITERAL) return (long long)node->children[0]->child_count;
            if (node->children[0]->type == AST_STRING_LITERAL) return (long long)strlen(node->children[0]->string_val);
            if (node->children[0]->type == AST_VAR_REF) {
                EvalLocal *array = eval_find_local_entry(ev, node->children[0]->name);
                if (array && array->is_array) return (long long)array->array_len;
            }
            ev->failed = 1;
            return 0;
        case AST_FUNC_CALL: {
            if (strcmp(node->name, "alloc_i64") == 0) {
                ev->failed = 1;
                return 0;
            }
            if (strcmp(node->name, "free") == 0) {
                if (node->child_count != 1 || node->children[0]->type != AST_VAR_REF) {
                    ev->failed = 1;
                    return 0;
                }
                EvalLocal *local = eval_find_local_entry(ev, node->children[0]->name);
                if (!local || !local->is_array || !local->owned || local->freed) {
                    ev->failed = 1;
                    return 0;
                }
                local->freed = true;
                return 0;
            }
            ASTNode *function = eval_find_function(ev->root, node->name);
            if (!function || ev->call_depth >= 64) {
                ev->failed = 1;
                return 0;
            }
            HostEvaluator child = *ev;
            child.local_count = 0;
            child.call_depth++;
            size_t argument_index = 0;
            for (size_t i = 0; i < function->child_count; i++) {
                ASTNode *param = function->children[i];
                if (param->type == AST_PARAM) {
                    if (param->declared_type == COBRA_TYPE_SLICE) {
                        ASTNode *argument = argument_index < node->child_count ? node->children[argument_index] : NULL;
                        if (!argument || argument->type != AST_VAR_REF) {
                            ev->failed = 1;
                            return 0;
                        }
                        EvalLocal *source = eval_find_local_entry(ev, argument->name);
                        EvalLocal *target = eval_find_local_entry(&child, param->name);
                        if (!source || !source->is_array || !target) {
                            if (child.local_count >= EVAL_MAX_LOCALS) {
                                ev->failed = 1;
                                return 0;
                            }
                            target = &child.locals[child.local_count++];
                            snprintf(target->name, sizeof(target->name), "%s", param->name);
                        }
                        *target = *source;
                        target->owned = false;
                        target->freed = false;
                        snprintf(target->name, sizeof(target->name), "%s", param->name);
                    } else {
                        long long value = argument_index < node->child_count ?
                            eval_expression(ev, node->children[argument_index]) : 0;
                        if (!eval_set_local(&child, param->name, value)) {
                            ev->failed = 1;
                            return 0;
                        }
                    }
                    argument_index++;
                }
            }
            int result = 0;
            if (!eval_function_body(&child, function, &result)) {
                ev->failed = 1;
                return 0;
            }
            return result;
        }
        default:
            return 0;
    }
}

typedef enum {
    EVAL_CONTINUE = 0,
    EVAL_RETURN,
    EVAL_FAILURE
} EvalStatementResult;

static EvalStatementResult eval_statement(HostEvaluator *ev, ASTNode *node, long long *return_value);

static EvalStatementResult eval_block(HostEvaluator *ev, ASTNode *block, long long *return_value) {
    if (!block) return EVAL_CONTINUE;
    for (size_t i = 0; i < block->child_count; i++) {
        EvalStatementResult result = eval_statement(ev, block->children[i], return_value);
        if (result != EVAL_CONTINUE) return result;
    }
    return EVAL_CONTINUE;
}

static EvalStatementResult eval_statement(HostEvaluator *ev, ASTNode *node, long long *return_value) {
    if (!node) return EVAL_CONTINUE;
    switch (node->type) {
        case AST_VAR_DECL:
        case AST_ASSIGN: {
            if (node->child_count > 0 && node->children[0]->type == AST_ARRAY_LITERAL) {
                if (!eval_set_array(ev, node->name, node->children[0])) return EVAL_FAILURE;
                return ev->failed ? EVAL_FAILURE : EVAL_CONTINUE;
            }
            if (node->child_count > 0 && node->children[0]->type == AST_FUNC_CALL &&
                strcmp(node->children[0]->name, "alloc_i64") == 0) {
                long long count = node->children[0]->child_count > 0 ?
                    eval_expression(ev, node->children[0]->children[0]) : -1;
                if (count < 0 || ev->array_used + (size_t)count > 1024) return EVAL_FAILURE;
                EvalLocal *local = eval_find_local_entry(ev, node->name);
                if (!local) {
                    if (ev->local_count >= EVAL_MAX_LOCALS) return EVAL_FAILURE;
                    local = &ev->locals[ev->local_count++];
                    snprintf(local->name, sizeof(local->name), "%s", node->name);
                }
                local->array_start = ev->array_used;
                local->array_len = (size_t)count;
                local->is_array = true;
                local->owned = true;
                local->freed = false;
                for (size_t i = 0; i < (size_t)count; i++) ev->array_storage[ev->array_used++] = 0;
                return EVAL_CONTINUE;
            }
            long long value = node->child_count > 0 ? eval_expression(ev, node->children[0]) : 0;
            if (!eval_set_local(ev, node->name, value)) return EVAL_FAILURE;
            return ev->failed ? EVAL_FAILURE : EVAL_CONTINUE;
        }
        case AST_RETURN:
            *return_value = node->child_count > 0 ? eval_expression(ev, node->children[0]) : 0;
            return ev->failed ? EVAL_FAILURE : EVAL_RETURN;
        case AST_ASSERT_STMT:
            if (node->child_count == 0 || eval_expression(ev, node->children[0]) == 0) return EVAL_FAILURE;
            return ev->failed ? EVAL_FAILURE : EVAL_CONTINUE;
        case AST_PRINT_STMT:
            if (node->child_count > 0) (void)eval_expression(ev, node->children[0]);
            return ev->failed ? EVAL_FAILURE : EVAL_CONTINUE;
        case AST_FUNC_CALL:
            (void)eval_expression(ev, node);
            return ev->failed ? EVAL_FAILURE : EVAL_CONTINUE;
        case AST_IF_STMT: {
            bool branch = node->child_count > 0 && eval_expression(ev, node->children[0]) != 0;
            size_t index = branch ? 1 : 2;
            if (index < node->child_count) return eval_block(ev, node->children[index], return_value);
            return EVAL_CONTINUE;
        }
        case AST_WHILE_STMT: {
            int steps = 0;
            while (node->child_count > 0 && eval_expression(ev, node->children[0]) != 0) {
                if (++steps > EVAL_MAX_LOOP_STEPS) return EVAL_FAILURE;
                if (node->child_count > 1) {
                    EvalStatementResult result = eval_block(ev, node->children[1], return_value);
                    if (result != EVAL_CONTINUE) return result;
                }
            }
            return ev->failed ? EVAL_FAILURE : EVAL_CONTINUE;
        }
        case AST_COMPUTE_BLOCK:
            /* Hardware blocks require native execution; never silently skip them in tests. */
            ev->failed = 1;
            return EVAL_FAILURE;
        case AST_FOR_LOOP: {
            if (node->child_count < 2 || node->children[0]->type != AST_VAR_REF) {
                ev->failed = 1;
                return EVAL_FAILURE;
            }
            EvalLocal *array = eval_find_local_entry(ev, node->children[0]->name);
            if (!array || !array->is_array) {
                ev->failed = 1;
                return EVAL_FAILURE;
            }
            for (size_t i = 0; i < array->array_len; i++) {
                if (!eval_set_local(ev, node->name, ev->array_storage[array->array_start + i])) return EVAL_FAILURE;
                EvalStatementResult result = eval_block(ev, node->children[1], return_value);
                if (result != EVAL_CONTINUE) return result;
            }
            return ev->failed ? EVAL_FAILURE : EVAL_CONTINUE;
        }
        default:
            return EVAL_CONTINUE;
    }
}

static bool eval_function_body(HostEvaluator *ev, ASTNode *function, int *return_code) {
    long long result = 0;
    EvalStatementResult status = EVAL_CONTINUE;
    for (size_t i = 0; i < function->child_count; i++) {
        if (function->children[i]->type == AST_PARAM) continue;
        status = eval_statement(ev, function->children[i], &result);
        if (status != EVAL_CONTINUE) break;
    }
    if (status == EVAL_FAILURE || ev->failed) return false;
    if (return_code) *return_code = (int)result;
    return true;
}

bool interpreter_run_function(ASTNode *root, const char *function_name, int *return_code) {
    ASTNode *function = eval_find_function(root, function_name);
    if (!function) return false;

    HostEvaluator ev = {0};
    ev.root = root;
    ev.call_depth = 1;
    for (size_t i = 0; i < function->child_count; i++) {
        ASTNode *child = function->children[i];
        if (child->type == AST_PARAM && !eval_set_local(&ev, child->name, 0)) return false;
    }
    return eval_function_body(&ev, function, return_code);
}
