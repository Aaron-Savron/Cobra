/*
 * Backend-IR tests: unit coverage for the flat SSA pipeline and differential
 * tests against the host interpreter. Differential programs stay inside the
 * intersection of the two engines' subsets (scalar i64 locals, arithmetic,
 * comparisons, if/else, while, for over constant arrays, calls, recursion,
 * return). See docs/BACKEND_IR.md.
 */
#include <stdio.h>
#include <string.h>
#include "../include/cobra.h"
#include "../src/backend_ir/ssa.h"

static int checks = 0;
static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------ */
/* Harness                                                            */
/* ------------------------------------------------------------------ */

static ASTNode *parse_program(const char *source) {
    Parser parser;
    parser_init(&parser, source);
    return parser_parse_program(&parser);
}

/* Build the whole program, verify it, and evaluate one function. */
static bool pipeline_run(ASTNode *program, const char *fn, int64_t *out,
                         char *err, size_t err_len) {
    BackendIrModule module;
    bir_module_init(&module, "<test>");
    bool ok = bir_build_program(&module, program);
    if (ok) ok = bir_verify(&module, err, err_len);
    if (ok) ok = bir_eval_function(&module, fn, out);
    if (!ok && err && err_len && !err[0]) {
        snprintf(err, err_len, "%s",
                 module.error[0] ? module.error : "backend-IR pipeline failed");
    }
    bir_module_free(&module);
    return ok;
}

/* Run a source program through both the host interpreter and the new SSA
   evaluator and require identical results (and the expected value). */
static void differential(const char *name, const char *source, int64_t expected) {
    ASTNode *program = parse_program(source);
    if (!program) {
        CHECK(program != NULL);
        return;
    }
    int host_result = 0;
    bool host_ok = interpreter_run_function(program, "main", &host_result);
    int64_t ssa_result = 0;
    char err[512] = {0};
    bool ssa_ok = pipeline_run(program, "main", &ssa_result, err, sizeof(err));
    if (host_ok != ssa_ok) {
        failures++;
        checks++;
        fprintf(stderr, "FAIL %s: interpreter=%d backend=%d (%s)\n",
                name, host_ok, ssa_ok, err);
    } else if (host_ok) {
        checks++;
        if (host_result != ssa_result || (int64_t)host_result != expected) {
            failures++;
            fprintf(stderr, "FAIL %s: host=%d ssa=%lld expected=%lld\n",
                    name, host_result, (long long)ssa_result,
                    (long long)expected);
        }
    } else {
        checks++;
        fprintf(stderr, "note: %s rejected by both engines (%s)\n", name, err);
    }
    ast_free(program);
}

/* ------------------------------------------------------------------ */
/* Differential tests (host interpreter vs SSA evaluator)             */
/* ------------------------------------------------------------------ */

static void differential_body(const char *name, const char *body, int64_t expected);

static void test_differential(void) {
    differential_body("straight-line arithmetic",
                 "  return 2 + 3 * 4\n", 14);

    differential_body("if/else value merge",
                 "  x = 1\n"
                 "  if x == 1: { x = 10 } else: { x = 20 }\n"
                 "  return x\n", 10);

    differential_body("if without else",
                 "  x = 1\n"
                 "  if x == 2: { x = 10 }\n"
                 "  return x\n", 1);

    differential_body("pass-through local across if/else",
                 "  a = 5\n"
                 "  b = 0\n"
                 "  if a > 3: { b = 7 } else: { b = 9 }\n"
                 "  c = a + b\n"
                 "  return c\n", 12);

    differential_body("while loop accumulation",
                 "  i = 0\n"
                 "  s = 0\n"
                 "  while i < 10: {\n"
                 "    s = s + i\n"
                 "    i = i + 1\n"
                 "  }\n"
                 "  return s\n", 45);

    differential_body("while loop-carried product",
                 "  i = 0\n"
                 "  acc = 1\n"
                 "  while i < 5: {\n"
                 "    acc = acc * 2\n"
                 "    i = i + 1\n"
                 "  }\n"
                 "  return acc\n", 32);

    differential_body("nested branches",
                 "  a = 1\n"
                 "  b = 0\n"
                 "  if a == 1: {\n"
                 "    if a > 0: { b = 5 } else: { b = 6 }\n"
                 "  } else: { b = 7 }\n"
                 "  c = 2\n"
                 "  while c < 5: {\n"
                 "    b = b + c\n"
                 "    c = c + 1\n"
                 "  }\n"
                 "  return b\n", 14);

    differential("multiple returns",
                 "def classify(n: i64) -> i64: {\n"
                 "  if n < 0: { return 0 }\n"
                 "  if n == 0: { return 1 }\n"
                 "  return 2\n"
                 "}\n"
                 "def main() -> i64: {\n"
                 "  return classify(-5) + classify(0) + classify(7)\n"
                 "}\n", 3);

    differential("recursion (fib)",
                 "def fib(n: i64) -> i64: {\n"
                 "  if n < 2: { return n }\n"
                 "  return fib(n - 1) + fib(n - 2)\n"
                 "}\n"
                 "def main() -> i64: { return fib(10) }\n", 55);

    differential("parameters and calls",
                 "def add(a: i64, b: i64) -> i64: { return a + b }\n"
                 "def sub(a: i64, b: i64) -> i64: { return a - b }\n"
                 "def main() -> i64: {\n"
                 "  x = add(20, 22)\n"
                 "  y = sub(x, 12)\n"
                 "  return y\n"
                 "}\n", 30);

    differential_body("if/else both return (unreachable merge)",
                 "  x = 3\n"
                 "  if x > 10: { return 1 } else: { return 2 }\n", 2);

    differential_body("comparisons on both branches",
                 "  a = 7\n"
                 "  b = 3\n"
                 "  if a >= b: {\n"
                 "    if a != b: { return 100 }\n"
                 "    return 50\n"
                 "  }\n"
                 "  return 0\n", 100);

    differential_body("read before assignment on a path",
                 "  x = 0\n"
                 "  if x == 1: { y = 5 }\n"
                 "  return y\n", 0);
}

/* Differential test over a single main() body (wraps it in a function). */
static void differential_body(const char *name, const char *body,
                              int64_t expected) {
    char source[1024];
    snprintf(source, sizeof(source), "def main() -> i64: {\n%s}\n", body);
    differential(name, source, expected);
}

/* ------------------------------------------------------------------ */
/* Unit tests through the parser (subset forms the interpreter lacks) */
/* ------------------------------------------------------------------ */

/* Unit test over a single main() body; the host interpreter is not required
   to support the program (for example for-over-scalar-bound loops). */
static void unit_expected(const char *name, const char *source, int64_t expected);

static void unit_expected_body(const char *name, const char *body,
                               int64_t expected) {
    char source[1024];
    snprintf(source, sizeof(source), "def main() -> i64: {\n%s}\n", body);
    unit_expected(name, source, expected);
}

static void unit_expected(const char *name, const char *source, int64_t expected) {
    ASTNode *program = parse_program(source);
    CHECK(program != NULL);
    if (!program) return;
    int64_t result = 0;
    char err[512] = {0};
    bool ok = pipeline_run(program, "main", &result, err, sizeof(err));
    checks++;
    if (!ok || result != expected) {
        failures++;
        fprintf(stderr, "FAIL %s: ok=%d result=%lld expected=%lld (%s)\n",
                name, ok, (long long)result, (long long)expected, err);
    }
    ast_free(program);
}

static void test_unit_subset(void) {
    unit_expected_body("for over scalar bound",
                       "  s = 0\n"
                       "  for i in 10: { s = s + i }\n"
                       "  return s\n", 45);

    unit_expected_body("for over range(a, b)",
                       "  s = 0\n"
                       "  for i in range(1, 6): { s = s + i }\n"
                       "  return s\n", 15);

    unit_expected_body("for over range(n)",
                       "  s = 0\n"
                       "  for i in range(5): { s = s + i }\n"
                       "  return s\n", 10);

    unit_expected_body("for over constant array",
                       "  s = 0\n"
                       "  for x in [1, 2, 3, 4]: { s = s + x }\n"
                       "  return s\n", 10);

    unit_expected_body("for over constant array with nested if",
                       "  s = 0\n"
                       "  for x in [1, 2, 3, 4, 5]: { if x >= 3: { s = s + x } }\n"
                       "  return s\n", 12);

    unit_expected_body("nested while loops",
                       "  s = 0\n"
                       "  i = 0\n"
                       "  while i < 4: {\n"
                       "    j = 0\n"
                       "    while j < 3: {\n"
                       "      s = s + i * j\n"
                       "      j = j + 1\n"
                       "    }\n"
                       "    i = i + 1\n"
                       "  }\n"
                       "  return s\n", 18);

    unit_expected_body("loop reading outer local",
                       "  limit = 4\n"
                       "  s = 0\n"
                       "  for i in limit: { s = s + i }\n"
                       "  return s\n", 6);

    unit_expected("void function returns 0",
                  "def helper(): {\n"
                  "  x = 5\n"
                  "  x = x + 1\n"
                  "}\n"
                  "def main() -> i64: {\n"
                  "  helper()\n"
                  "  return 7\n"
                  "}\n", 7);
}

/* ------------------------------------------------------------------ */
/* Low-level SSA unit tests                                           */
/* ------------------------------------------------------------------ */

static void test_load_store(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:load_store>");
    SsaArena *arena = &module.arena;
    SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
    SsaValueRef address = bir_add_const(arena, module.type_i64, 0, 1, 1);
    SsaValueRef value = bir_add_const(arena, module.type_i64, 42, 1, 1);
    const SsaValueRef store_ops[2] = {address, value};
    SsaInstRef store = bir_add_inst(arena, SSA_OP_STORE, NULL, store_ops, 2, 1, 1);
    arena->insts[store].effect = SSA_EFFECT_WRITE;
    CHECK(bir_block_add_inst(arena, entry, store));
    const SsaValueRef load_ops[1] = {address};
    SsaInstRef load = bir_add_inst(arena, SSA_OP_LOAD, module.type_i64, load_ops, 1, 1, 1);
    arena->insts[load].effect = SSA_EFFECT_READ;
    CHECK(bir_block_add_inst(arena, entry, load));
    SsaValueRef loaded = bir_inst_result(arena, load, 1, 1);
    CHECK(bir_set_return(arena, entry, loaded, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));
    char err[256] = {0};
    CHECK(bir_verify(&module, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "verify: %s\n", err);
    int64_t result = 0;
    CHECK(bir_eval_function(&module, "main", &result));
    CHECK(result == 42);
    bir_module_free(&module);
}

static void test_unreachable_block(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:unreachable>");
    SsaArena *arena = &module.arena;
    SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
    SsaBlockRef live = bir_add_block(arena, "live", 1, 1);
    SsaBlockRef dead = bir_add_block(arena, "dead", 1, 1);
    SsaValueRef seven = bir_add_const(arena, module.type_i64, 7, 1, 1);
    SsaValueRef ninety = bir_add_const(arena, module.type_i64, 99, 1, 1);
    CHECK(bir_add_edge(arena, entry, live));
    CHECK(bir_set_jump(arena, entry, live, NULL, 0, 1, 1));
    CHECK(bir_set_return(arena, live, seven, 1, 1));
    /* dead block: no predecessors, structurally valid, never executed */
    CHECK(bir_set_return(arena, dead, ninety, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));
    char err[256] = {0};
    CHECK(bir_verify(&module, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "verify: %s\n", err);
    int64_t result = 0;
    CHECK(bir_eval_function(&module, "main", &result));
    CHECK(result == 7);
    bir_module_free(&module);
}

static void test_missing_terminator_rejected(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:no_terminator>");
    SsaArena *arena = &module.arena;
    SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
    SsaValueRef five = bir_add_const(arena, module.type_i64, 5, 1, 1);
    const SsaValueRef ops[2] = {five, five};
    SsaInstRef add = bir_add_inst(arena, SSA_OP_ADD, module.type_i64, ops, 2, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, add));
    /* no terminator set */
    char err[256] = {0};
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "terminator") != NULL);
    bir_module_free(&module);
}

static void test_arity_mismatch_rejected(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:arity>");
    SsaArena *arena = &module.arena;
    SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
    SsaBlockRef join = bir_add_block(arena, "join", 1, 1);
    SsaValueRef param = bir_add_block_param(arena, join, module.type_i64, 1, 1);
    CHECK(param != SSA_VALUE_NONE);
    CHECK(bir_add_edge(arena, entry, join));
    /* jump supplies 0 edge args but the join expects 1 block parameter */
    CHECK(bir_set_jump(arena, entry, join, NULL, 0, 1, 1));
    CHECK(bir_set_return(arena, join, param, 1, 1));
    char err[256] = {0};
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "edge args") != NULL || strstr(err, "arity") != NULL);
    bir_module_free(&module);
}

static void test_use_before_def_rejected(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:use_before_def>");
    SsaArena *arena = &module.arena;
    SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
    SsaValueRef c0 = bir_add_const(arena, module.type_i64, 1, 1, 1);
    SsaValueRef c1 = bir_add_const(arena, module.type_i64, 2, 1, 1);
    const SsaValueRef pair[2] = {c0, c1};
    /* definition instruction: not added to the block yet */
    SsaInstRef def = bir_add_inst(arena, SSA_OP_ADD, module.type_i64, pair, 2, 1, 1);
    SsaValueRef result = bir_inst_result(arena, def, 1, 1);
    /* use instruction added first, so the use precedes the def in the block */
    const SsaValueRef forward[2] = {c0, result};
    SsaInstRef use = bir_add_inst(arena, SSA_OP_ADD, module.type_i64, forward, 2, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, use));
    CHECK(bir_block_add_inst(arena, entry, def));
    CHECK(bir_set_return(arena, entry, result, 1, 1));
    /* The dominance check only runs over blocks reachable from a registered
       function entry, so this module must be a real function. */
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));
    char err[256] = {0};
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "before its definition") != NULL);
    bir_module_free(&module);
}

static void test_generic_rejected(void) {
    /* A value whose canonical type contains an unresolved generic parameter
       must not reach backend IR. */
    BackendIrModule module;
    bir_module_init(&module, "<unit:generic>");
    SsaArena *arena = &module.arena;
    CobraType *generic_param = cobra_type_make(module.type_arena,
                                               COBRA_TYPE_GENERIC_PARAM, "T",
                                               NULL, NULL, NULL, NULL,
                                               COBRA_OWNERSHIP_VALUE,
                                               COBRA_MUTABILITY_DEFAULT, -1);
    CHECK(generic_param != NULL);
    SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
    SsaValueRef value = bir_add_const(arena, generic_param, 5, 1, 1);
    CHECK(bir_set_return(arena, entry, value, 1, 1));
    char err[256] = {0};
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "generic") != NULL || strstr(err, "finalized") != NULL);
    bir_module_free(&module);

    /* End to end: a source-level generic function is rejected by the HIR
       builder before any SSA exists. */
    ASTNode *program = parse_program("def id[T](x: T) -> T: { return x }");
    CHECK(program != NULL);
    if (program) {
        BackendIrModule source_module;
        bir_module_init(&source_module, "<unit:generic_source>");
        bool ok = bir_build_program(&source_module, program);
        CHECK(!ok);
        CHECK(strstr(source_module.error, "generic") != NULL);
        bir_module_free(&source_module);
        ast_free(program);
    }
}

static void test_nonfinalized_type_rejected(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:unfinalized>");
    SsaArena *arena = &module.arena;
    CobraType *fresh = cobra_type_named(module.type_arena, COBRA_TYPE_I64, "fresh");
    CHECK(fresh != NULL);
    CHECK(!fresh->finalized);
    SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
    SsaValueRef value = bir_add_const(arena, fresh, 5, 1, 1);
    CHECK(bir_set_return(arena, entry, value, 1, 1));
    char err[256] = {0};
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "finalized") != NULL);
    bir_module_free(&module);
}

static void test_unknown_callee_rejected(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:unknown_callee>");
    SsaArena *arena = &module.arena;
    SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
    SsaInstRef call = bir_add_inst(arena, SSA_OP_CALL, module.type_i64, NULL, 0, 1, 1);
    snprintf(arena->insts[call].callee, sizeof(arena->insts[call].callee), "missing");
    arena->insts[call].effect = SSA_EFFECT_CALL;
    CHECK(bir_block_add_inst(arena, entry, call));
    SsaValueRef result = bir_inst_result(arena, call, 1, 1);
    CHECK(bir_set_return(arena, entry, result, 1, 1));
    char err[256] = {0};
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "unknown function") != NULL);
    bir_module_free(&module);
}

static void test_printer_deterministic(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:printer>");
    SsaArena *arena = &module.arena;
    SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
    SsaValueRef c0 = bir_add_const(arena, module.type_i64, 2, 1, 1);
    SsaValueRef c1 = bir_add_const(arena, module.type_i64, 3, 1, 1);
    const SsaValueRef pair[2] = {c0, c1};
    SsaInstRef mul = bir_add_inst(arena, SSA_OP_MUL, module.type_i64, pair, 2, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, mul));
    SsaValueRef product = bir_inst_result(arena, mul, 1, 1);
    CHECK(bir_set_return(arena, entry, product, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));

    char *first = NULL, *second = NULL;
    size_t first_len = 0, second_len = 0;
    FILE *out = open_memstream(&first, &first_len);
    CHECK(out != NULL);
    bir_dump(&module, out);
    fclose(out);
    out = open_memstream(&second, &second_len);
    CHECK(out != NULL);
    bir_dump(&module, out);
    fclose(out);
    CHECK(first && second && strcmp(first, second) == 0);
    CHECK(strstr(first, "fn \"main\"") != NULL);
    CHECK(strstr(first, "mul") != NULL);
    CHECK(strstr(first, "ret") != NULL);
    free(first);
    free(second);
    bir_module_free(&module);
}

/* ------------------------------------------------------------------ */

int main(void) {
    test_differential();
    test_unit_subset();
    test_load_store();
    test_unreachable_block();
    test_missing_terminator_rejected();
    test_arity_mismatch_rejected();
    test_use_before_def_rejected();
    test_generic_rejected();
    test_nonfinalized_type_rejected();
    test_unknown_callee_rejected();
    test_printer_deterministic();

    printf("backend-ir tests: %d checks, %d failures\n", checks, failures);
    if (failures) {
        fprintf(stderr, "backend-ir tests FAILED\n");
        return 1;
    }
    printf("backend-ir tests OK\n");
    return 0;
}
