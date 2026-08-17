/*
 * Backend-IR tests: unit coverage for the flat SSA pipeline and differential
 * tests against the host interpreter. Differential programs stay inside the
 * intersection of the two engines' subsets (scalar i64 locals, arithmetic,
 * comparisons, if/else, while, for over constant arrays, calls, recursion,
 * return). See docs/BACKEND_IR.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/cobra.h"
#include "../src/backend_ir/ssa.h"
#include "../src/backend_ir/mir.h"
#include "../src/backend_ir/alloc.h"
#include "../src/backend_ir/x86_64.h"
#include "../src/backend_ir/x86_64_obj.h"

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
static bool pipeline_run(ASTNode *program, const char *fn, BirScalarValue *out,
                         char *err, size_t err_len) {
    BackendIrModule module;
    bir_module_init(&module, "<test>");
    bool ok = bir_build_program(&module, program);
    if (ok) ok = bir_verify(&module, err, err_len);
    if (ok) ok = bir_eval_function_value(&module, fn, out);
    if (!ok && err && err_len && !err[0]) {
        snprintf(err, err_len, "%.255s",
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
    BirScalarValue ssa_result = {0};
    char err[512] = {0};
    bool ssa_ok = pipeline_run(program, "main", &ssa_result, err, sizeof(err));
    checks++;
    if (!host_ok || !ssa_ok) {
        failures++;
        fprintf(stderr, "FAIL %s: expected success, interpreter=%d backend=%d (%s)\n",
                name, host_ok, ssa_ok, err);
    } else if (ssa_result.kind != BIR_SCALAR_I64 ||
               ssa_result.payload.i64 != expected ||
               host_result != ssa_result.payload.i64) {
        failures++;
        fprintf(stderr, "FAIL %s: host=%d ssa=%lld expected=%lld\n",
                name, host_result, (long long)ssa_result.payload.i64,
                (long long)expected);
    }
    ast_free(program);
}

/* Rejection cases are separate from successful differential cases: both
   engines must reject, and the backend must report the expected diagnostic
   class rather than passing merely because execution did not happen. */
static void differential_rejected(const char *name, const char *source,
                                  const char *expected_reason) {
    ASTNode *program = parse_program(source);
    CHECK(program != NULL);
    if (!program) return;
    int host_result = 0;
    bool host_ok = interpreter_run_function(program, "main", &host_result);
    BirScalarValue ssa_result = {0};
    char err[512] = {0};
    bool ssa_ok = pipeline_run(program, "main", &ssa_result, err, sizeof(err));
    checks++;
    if (host_ok || ssa_ok || !strstr(err, expected_reason)) {
        failures++;
        fprintf(stderr, "FAIL %s: expected rejection '%s', interpreter=%d backend=%d (%s)\n",
                name, expected_reason, host_ok, ssa_ok, err);
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

    differential_body("bool local through typed memory",
                 "  flag = 1 < 2\n"
                 "  if flag: { return 9 }\n"
                 "  return 0\n", 9);

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

    differential_rejected("read before assignment on a path",
                 "def main() -> i64: {\n"
                 "  x = 0\n"
                 "  if x == 1: { y = 5 }\n"
                 "  return y\n"
                 "}\n", "read before assignment");
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
    BirScalarValue result = {0};
    char err[512] = {0};
    bool ok = pipeline_run(program, "main", &result, err, sizeof(err));
    checks++;
    if (!ok || result.kind != BIR_SCALAR_I64 || result.payload.i64 != expected) {
        failures++;
        fprintf(stderr, "FAIL %s: ok=%d result=%lld expected=%lld (%s)\n",
                name, ok, (long long)result.payload.i64, (long long)expected, err);
    }
    if ((!ok || result.kind != BIR_SCALAR_I64 || result.payload.i64 != expected) &&
        strstr(name, "owned list")) {
        BackendIrModule debug_module;
        bir_module_init(&debug_module, "<debug-buffer>");
        if (bir_build_program(&debug_module, program)) bir_dump(&debug_module, stderr);
        else fprintf(stderr, "debug build: %s\n", debug_module.error);
        bir_module_free(&debug_module);
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
    const CobraType *i64_ptr = bir_pointer_type(&module, module.type_i64);
    SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
    SsaInstRef slot = bir_add_stack_slot(arena, i64_ptr, module.type_i64,
                                         0, 8, 0, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, slot));
    SsaValueRef address = bir_inst_result(arena, slot, 1, 1);
    SsaValueRef value = bir_add_const(arena, bir_scalar_i64(module.type_i64, 42), 1, 1);
    SsaInstRef store = bir_add_typed_store(arena, module.type_i64, i64_ptr,
                                           address, value, 8, 8, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, store));
    SsaInstRef load = bir_add_typed_load(arena, module.type_i64, i64_ptr,
                                         address, 8, 8, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, load));
    SsaValueRef loaded = bir_inst_result(arena, load, 1, 1);
    CHECK(bir_set_return(arena, entry, loaded, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));

    SsaBlockRef offset_entry = bir_add_entry_block(arena, "offset_mem", 1, 1);
    SsaInstRef offset_slot = bir_add_stack_slot(arena, i64_ptr, module.type_i64,
                                                0, 8, 2, 1, 1);
    CHECK(bir_block_add_inst(arena, offset_entry, offset_slot));
    SsaValueRef base = bir_inst_result(arena, offset_slot, 1, 1);
    SsaValueRef byte_offset = bir_add_const(arena, bir_scalar_i64(module.type_i64, 8), 1, 1);
    SsaInstRef shifted = bir_add_ptr_add(arena, i64_ptr, base, byte_offset, 1, 1);
    CHECK(bir_block_add_inst(arena, offset_entry, shifted));
    SsaValueRef shifted_pointer = bir_inst_result(arena, shifted, 1, 1);
    SsaValueRef shifted_value = bir_add_const(arena, bir_scalar_i64(module.type_i64, 99), 1, 1);
    SsaInstRef shifted_store = bir_add_typed_store(arena, module.type_i64, i64_ptr,
                                                   shifted_pointer, shifted_value, 8, 8, 1, 1);
    CHECK(bir_block_add_inst(arena, offset_entry, shifted_store));
    SsaInstRef shifted_load = bir_add_typed_load(arena, module.type_i64, i64_ptr,
                                                 shifted_pointer, 8, 8, 1, 1);
    CHECK(bir_block_add_inst(arena, offset_entry, shifted_load));
    SsaValueRef shifted_loaded = bir_inst_result(arena, shifted_load, 1, 1);
    CHECK(bir_set_return(arena, offset_entry, shifted_loaded, 1, 1));
    CHECK(bir_register_function_info(&module, "offset_mem", offset_entry, 0, NULL,
                                     module.type_i64, true));

    const CobraType *f32_ptr = bir_pointer_type(&module, module.type_f32);
    SsaBlockRef fentry = bir_add_entry_block(arena, "f32_mem", 1, 1);
    SsaInstRef fslot = bir_add_stack_slot(arena, f32_ptr, module.type_f32,
                                          16, 4, 1, 1, 1);
    CHECK(bir_block_add_inst(arena, fentry, fslot));
    SsaValueRef faddress = bir_inst_result(arena, fslot, 1, 1);
    SsaValueRef fvalue = bir_add_const(arena, bir_scalar_f32(module.type_f32, 3.5f), 1, 1);
    SsaInstRef fstore = bir_add_typed_store(arena, module.type_f32, f32_ptr,
                                            faddress, fvalue, 4, 4, 1, 1);
    CHECK(bir_block_add_inst(arena, fentry, fstore));
    SsaInstRef fload = bir_add_typed_load(arena, module.type_f32, f32_ptr,
                                          faddress, 4, 4, 1, 1);
    CHECK(bir_block_add_inst(arena, fentry, fload));
    SsaValueRef floaded = bir_inst_result(arena, fload, 1, 1);
    CHECK(bir_set_return(arena, fentry, floaded, 1, 1));
    CHECK(bir_register_function_info(&module, "f32_mem", fentry, 0, NULL,
                                     module.type_f32, true));

    const CobraType *read_params[1] = {i64_ptr};
    CHECK(bir_declare_function(&module, "read_ptr", 1, read_params,
                               module.type_i64, true));
    SsaBlockRef read_entry = bir_add_entry_block(arena, "read_ptr", 1, 1);
    SsaValueRef read_param = bir_add_value(arena, SSA_VALUE_PARAM, i64_ptr, 1, 1);
    arena->values[read_param].param_index = 0;
    SsaInstRef read_load = bir_add_typed_load(arena, module.type_i64, i64_ptr,
                                              read_param, 8, 8, 1, 1);
    CHECK(bir_block_add_inst(arena, read_entry, read_load));
    SsaValueRef read_value = bir_inst_result(arena, read_load, 1, 1);
    CHECK(bir_set_return(arena, read_entry, read_value, 1, 1));
    SsaValueRef read_params_refs[1] = {read_param};
    CHECK(bir_register_function_info(&module, "read_ptr", read_entry, 1,
                                     read_params_refs, module.type_i64, true));

    SsaBlockRef call_entry = bir_add_entry_block(arena, "call_ptr", 1, 1);
    SsaInstRef call_slot = bir_add_stack_slot(arena, i64_ptr, module.type_i64,
                                              0, 8, 3, 1, 1);
    CHECK(bir_block_add_inst(arena, call_entry, call_slot));
    SsaValueRef call_pointer = bir_inst_result(arena, call_slot, 1, 1);
    SsaValueRef call_value = bir_add_const(arena, bir_scalar_i64(module.type_i64, 55), 1, 1);
    SsaInstRef call_store = bir_add_typed_store(arena, module.type_i64, i64_ptr,
                                                call_pointer, call_value, 8, 8, 1, 1);
    CHECK(bir_block_add_inst(arena, call_entry, call_store));
    SsaInstRef call = bir_add_inst(arena, SSA_OP_CALL, module.type_i64,
                                   &call_pointer, 1, 1, 1);
    snprintf(arena->insts[call].callee, sizeof(arena->insts[call].callee), "read_ptr");
    arena->insts[call].effect = SSA_EFFECT_CALL;
    CHECK(bir_block_add_inst(arena, call_entry, call));
    SsaValueRef call_result = bir_inst_result(arena, call, 1, 1);
    CHECK(bir_set_return(arena, call_entry, call_result, 1, 1));
    CHECK(bir_register_function_info(&module, "call_ptr", call_entry, 0, NULL,
                                     module.type_i64, true));

    char err[256] = {0};
    CHECK(bir_verify(&module, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "verify: %s\n", err);
    BirScalarValue result = {0};
    CHECK(bir_eval_function_value(&module, "main", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 42);
    CHECK(bir_eval_function_value(&module, "offset_mem", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 99);
    CHECK(bir_eval_function_value(&module, "f32_mem", &result));
    CHECK(result.kind == BIR_SCALAR_F32 && bir_scalar_as_f32(result) > 3.49f &&
          bir_scalar_as_f32(result) < 3.51f);
    CHECK(bir_eval_function_value(&module, "call_ptr", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 55);
    arena->insts[load].memory_width = 4;
    CHECK(!bir_eval_function_value(&module, "main", &result));
    bir_module_free(&module);
}

static void test_aggregate_memory(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:aggregate-memory>");
    SsaArena *arena = &module.arena;
    CobraType *point = cobra_type_make(module.type_arena, COBRA_TYPE_STRUCT, "Point",
                                        NULL, NULL, NULL, NULL,
                                        COBRA_OWNERSHIP_VALUE,
                                        COBRA_MUTABILITY_DEFAULT, -1);
    CHECK(point != NULL);
    CHECK(cobra_type_add_field(point, "x", module.type_i64,
                               COBRA_OWNERSHIP_VALUE, COBRA_MUTABILITY_DEFAULT, -1));
    CHECK(cobra_type_add_field(point, "scale", module.type_f32,
                               COBRA_OWNERSHIP_VALUE, COBRA_MUTABILITY_DEFAULT, -1));
    CHECK(cobra_type_finalize(module.type_arena, point));
    const CobraType *point_ptr = bir_pointer_type(&module, point);
    const CobraType *f32_ptr = bir_pointer_type(&module, module.type_f32);
    SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
    SsaInstRef source_slot = bir_add_stack_slot(arena, point_ptr, point,
                                                0, 8, 0, 1, 1);
    SsaInstRef destination_slot = bir_add_stack_slot(arena, point_ptr, point,
                                                     32, 8, 1, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, source_slot));
    CHECK(bir_block_add_inst(arena, entry, destination_slot));
    SsaValueRef source = bir_inst_result(arena, source_slot, 1, 1);
    SsaValueRef destination = bir_inst_result(arena, destination_slot, 1, 1);
    SsaInstRef source_field = bir_add_field_addr(arena, f32_ptr, point,
                                                 module.type_f32, source, 8, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, source_field));
    SsaValueRef source_field_ptr = bir_inst_result(arena, source_field, 1, 1);
    SsaValueRef value = bir_add_const(arena, bir_scalar_f32(module.type_f32, 3.5f), 1, 1);
    SsaInstRef store = bir_add_typed_store(arena, module.type_f32, f32_ptr,
                                           source_field_ptr, value, 4, 4, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, store));
    SsaInstRef copy = bir_add_aggregate_copy(arena, point, destination, source, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, copy));
    SsaInstRef destination_field = bir_add_field_addr(arena, f32_ptr, point,
                                                      module.type_f32, destination, 8, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, destination_field));
    SsaValueRef destination_field_ptr = bir_inst_result(arena, destination_field, 1, 1);
    SsaInstRef load = bir_add_typed_load(arena, module.type_f32, f32_ptr,
                                         destination_field_ptr, 4, 4, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, load));
    SsaValueRef loaded = bir_inst_result(arena, load, 1, 1);
    CHECK(bir_set_return(arena, entry, loaded, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_f32, true));
    char err[256] = {0};
    CHECK(bir_verify(&module, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "aggregate verify: %s\n", err);
    BirScalarValue result = {0};
    CHECK(bir_eval_function_value(&module, "main", &result));
    CHECK(result.kind == BIR_SCALAR_F32 && bir_scalar_as_f32(result) > 3.49f &&
          bir_scalar_as_f32(result) < 3.51f);
    arena->insts[destination_field].memory_offset = 4;
    CHECK(!bir_verify(&module, err, sizeof(err)));
    arena->insts[destination_field].memory_offset = 8;
    arena->insts[copy].memory_width = 8;
    CHECK(!bir_verify(&module, err, sizeof(err)));
    bir_module_free(&module);
}

static void test_typed_memory_rejected(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:typed-memory-rejected>");
    SsaArena *arena = &module.arena;
    const CobraType *i64_ptr = bir_pointer_type(&module, module.type_i64);
    SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
    SsaInstRef slot = bir_add_stack_slot(arena, i64_ptr, module.type_i64,
                                         2, 8, 0, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, slot));
    SsaValueRef pointer = bir_inst_result(arena, slot, 1, 1);
    CHECK(bir_set_return(arena, entry, pointer, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     i64_ptr, true));
    char err[256] = {0};
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "offset") != NULL || strstr(err, "alignment") != NULL);
    bir_module_free(&module);

    bir_module_init(&module, "<unit:typed-memory-kind>");
    arena = &module.arena;
    i64_ptr = bir_pointer_type(&module, module.type_i64);
    entry = bir_add_entry_block(arena, "entry", 1, 1);
    slot = bir_add_stack_slot(arena, i64_ptr, module.type_i64, 0, 8, 0, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, slot));
    pointer = bir_inst_result(arena, slot, 1, 1);
    SsaInstRef bad_load = bir_add_typed_load(arena, module.type_f32, i64_ptr,
                                             pointer, 4, 4, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, bad_load));
    SsaValueRef bad_value = bir_inst_result(arena, bad_load, 1, 1);
    CHECK(bir_set_return(arena, entry, bad_value, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_f32, true));
    memset(err, 0, sizeof(err));
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "typed pointer") != NULL || strstr(err, "memory") != NULL);
    bir_module_free(&module);
}

static void test_edge_type_rejected(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:edge_type>");
    SsaArena *arena = &module.arena;
    SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
    SsaBlockRef join = bir_add_block(arena, "join", 1, 1);
    SsaValueRef parameter = bir_add_block_param(arena, join, module.type_i64, 1, 1);
    SsaValueRef boolean = bir_add_const(arena, bir_scalar_bool(module.type_bool, true), 1, 1);
    CHECK(bir_add_edge(arena, entry, join));
    CHECK(bir_set_jump(arena, entry, join, &boolean, 1, 1, 1));
    CHECK(bir_set_return(arena, join, parameter, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));
    char err[256] = {0};
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "target block-parameter type") != NULL);
    bir_module_free(&module);
}

static void test_return_type_rejected(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:return_type>");
    SsaArena *arena = &module.arena;
    SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
    SsaValueRef boolean = bir_add_const(arena, bir_scalar_bool(module.type_bool, true), 1, 1);
    CHECK(bir_set_return(arena, entry, boolean, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));
    char err[256] = {0};
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "return") != NULL);
    bir_module_free(&module);
}

static void test_opcode_signature_rejected(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:opcode_signature>");
    SsaArena *arena = &module.arena;
    SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
    SsaValueRef one = bir_add_const(arena, bir_scalar_i64(module.type_i64, 1), 1, 1);
    SsaInstRef add = bir_add_inst(arena, SSA_OP_ADD, module.type_i64, &one, 1, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, add));
    CHECK(bir_set_return(arena, entry, one, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));
    char err[256] = {0};
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "add") != NULL && strstr(err, "operands") != NULL);
    bir_module_free(&module);
}

static void test_parameter_signature_rejected(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:param_signature>");
    SsaArena *arena = &module.arena;
    SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
    SsaValueRef parameter = bir_add_value(arena, SSA_VALUE_PARAM,
                                          module.type_bool, 1, 1);
    arena->values[parameter].param_index = 0;
    CHECK(bir_set_return(arena, entry, parameter, 1, 1));
    SsaValueRef params[1] = {parameter};
    CHECK(bir_register_function_info(&module, "main", entry, 1, params,
                                     module.type_bool, true));
    /* The compatibility registration API declares scalar parameters as i64;
       the bool SSA parameter must therefore be rejected by the verifier. */
    char err[256] = {0};
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "parameter") != NULL);
    bir_module_free(&module);
}

static void test_call_result_signature_rejected(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:call_result>");
    SsaArena *arena = &module.arena;
    SsaBlockRef callee_entry = bir_add_entry_block(arena, "callee", 1, 1);
    SsaValueRef one = bir_add_const(arena, bir_scalar_i64(module.type_i64, 1), 1, 1);
    CHECK(bir_set_return(arena, callee_entry, one, 1, 1));
    CHECK(bir_register_function_info(&module, "callee", callee_entry, 0, NULL,
                                     module.type_i64, true));

    SsaBlockRef caller_entry = bir_add_entry_block(arena, "caller", 1, 1);
    SsaInstRef call = bir_add_inst(arena, SSA_OP_CALL, module.type_bool,
                                   NULL, 0, 1, 1);
    snprintf(arena->insts[call].callee, sizeof(arena->insts[call].callee), "callee");
    arena->insts[call].effect = SSA_EFFECT_CALL;
    CHECK(bir_block_add_inst(arena, caller_entry, call));
    SsaValueRef result = bir_inst_result(arena, call, 1, 1);
    CHECK(bir_set_return(arena, caller_entry, result, 1, 1));
    CHECK(bir_register_function_info(&module, "caller", caller_entry, 0, NULL,
                                     module.type_bool, true));
    char err[256] = {0};
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "result") != NULL);
    bir_module_free(&module);
}

static void test_unreachable_block(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:unreachable>");
    SsaArena *arena = &module.arena;
    SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
    SsaBlockRef live = bir_add_block(arena, "live", 1, 1);
    SsaBlockRef dead = bir_add_block(arena, "dead", 1, 1);
    SsaValueRef seven = bir_add_const(arena, bir_scalar_i64(module.type_i64, 7), 1, 1);
    SsaValueRef ninety = bir_add_const(arena, bir_scalar_i64(module.type_i64, 99), 1, 1);
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
    BirScalarValue result = {0};
    CHECK(bir_eval_function_value(&module, "main", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 7);
    bir_module_free(&module);
}

static void test_unreachable_return_type_rejected(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:unreachable_return>");
    SsaArena *arena = &module.arena;
    SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
    SsaBlockRef live = bir_add_block(arena, "live", 1, 1);
    SsaBlockRef dead = bir_add_block(arena, "dead", 1, 1);
    SsaValueRef good = bir_add_const(arena, bir_scalar_i64(module.type_i64, 1), 1, 1);
    SsaValueRef wrong = bir_add_const(arena, bir_scalar_bool(module.type_bool, true), 1, 1);
    CHECK(bir_add_edge(arena, entry, live));
    CHECK(bir_set_jump(arena, entry, live, NULL, 0, 1, 1));
    CHECK(bir_set_return(arena, live, good, 1, 1));
    /* Dead blocks still belong to the registered function's block range and
       must obey its signature even though dominance is not checked there. */
    CHECK(bir_set_return(arena, dead, wrong, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));
    char err[256] = {0};
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "return") != NULL);
    bir_module_free(&module);
}

static void test_missing_terminator_rejected(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:no_terminator>");
    SsaArena *arena = &module.arena;
    SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
    SsaValueRef five = bir_add_const(arena, bir_scalar_i64(module.type_i64, 5), 1, 1);
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
    SsaValueRef c0 = bir_add_const(arena, bir_scalar_i64(module.type_i64, 1), 1, 1);
    SsaValueRef c1 = bir_add_const(arena, bir_scalar_i64(module.type_i64, 2), 1, 1);
    const SsaValueRef pair[2] = {c0, c1};
    /* definition instruction: not added to the block yet */
    SsaInstRef def = bir_add_inst(arena, SSA_OP_ADD, module.type_i64, pair, 2, 1, 1);
    SsaValueRef result = bir_inst_result(arena, def, 1, 1);
    /* use instruction added first, so the use precedes the def in the block */
    const SsaValueRef forward[2] = {c0, result};
    SsaInstRef use = bir_add_inst(arena, SSA_OP_ADD, module.type_i64, forward, 2, 1, 1);
    SsaValueRef use_result = bir_inst_result(arena, use, 1, 1);
    CHECK(use_result != SSA_VALUE_NONE);
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
    SsaValueRef value = bir_add_const(arena, bir_scalar_i64(generic_param, 5), 1, 1);
    CHECK(bir_set_return(arena, entry, value, 1, 1));
    char err[256] = {0};
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "generic") != NULL || strstr(err, "finalized") != NULL);
    bir_module_free(&module);

    /* End to end: scalar generic functions are monomorphized before HIR.
       The unresolved template itself is never registered as a backend
       function, while two concrete call sites share canonical substitution. */
    ASTNode *program = parse_program(
        "def id[T](x: T) -> T: { return x }\n"
        "def add[T](a: T, b: T) -> T: { return a + b }\n"
        "def main() -> i64: {\n"
        "  let x: i64 = 7\n"
        "  return add(x, id(4))\n"
        "}\n"
        "def float_main() -> f32: { return id(2.5) }\n");
    CHECK(program != NULL);
    if (program) {
        BackendIrModule source_module;
        bir_module_init(&source_module, "<unit:generic_source>");
        char source_err[256] = {0};
        bool ok = bir_build_program(&source_module, program);
        CHECK(ok);
        CHECK(bir_verify(&source_module, source_err, sizeof(source_err)));
        CHECK(bir_find_function(&source_module, "id__i64") != NULL);
        CHECK(bir_find_function(&source_module, "add__i64") != NULL);
        CHECK(bir_find_function(&source_module, "id__f32") != NULL);
        BirScalarValue result = {0};
        CHECK(bir_eval_function_value(&source_module, "main", &result));
        CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 11);
        CHECK(bir_eval_function_value(&source_module, "float_main", &result));
        CHECK(result.kind == BIR_SCALAR_F32 && bir_scalar_as_f32(result) > 2.49f &&
              bir_scalar_as_f32(result) < 2.51f);
        if (!ok || source_err[0]) fprintf(stderr, "generic source: %s\\n",
                                          source_module.error[0] ? source_module.error : source_err);
        bir_module_free(&source_module);
        ast_free(program);
    }
}

static void test_source_scalar_generic_boundaries(void) {
    ASTNode *program = parse_program(
        "def id[T](value: T) -> T: { return value }\n"
        "def main() -> i64: { return id(1) + id(2) + id(3) }\n"
        "def bool_main() -> bool: { return id(true) }\n");
    CHECK(program != NULL);
    if (program) {
        BackendIrModule module;
        bir_module_init(&module, "<unit:generic-boundaries>");
        char err[256] = {0};
        CHECK(bir_build_program(&module, program));
        CHECK(bir_verify(&module, err, sizeof(err)));
        CHECK(bir_find_function(&module, "id__i64") != NULL);
        CHECK(bir_find_function(&module, "id__bool") != NULL);
        BirScalarValue result = {0};
        CHECK(bir_eval_function_value(&module, "main", &result));
        CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 6);
        CHECK(bir_eval_function_value(&module, "bool_main", &result));
        CHECK(result.kind == BIR_SCALAR_BOOL && result.payload.i64 == 1);
        bir_module_free(&module);
        ast_free(program);
    }

    /* Generic parameters are scalar-only in this first backend lane. A
       collection argument must not be silently specialized by its ABI shape. */
    program = parse_program(
        "def id[T](value: T) -> T: { return value }\n"
        "def main() -> i64: {\n"
        "  let values: list[i64] = []\n"
        "  id(values)\n"
        "  return 0\n"
        "}\n");
    CHECK(program != NULL);
    if (program) {
        BackendIrModule module;
        bir_module_init(&module, "<unit:generic-nonscalar>");
        CHECK(!bir_build_program(&module, program));
        CHECK(strstr(module.error, "generic") != NULL || module.error[0] != '\0');
        bir_module_free(&module);
        ast_free(program);
    }
}

static void test_source_scalar_generic_structs(void) {
    ASTNode *program = parse_program(
        "struct Box[T]: { value: T }\n"
        "def box_score(box: Box[i64]) -> i64: { return box.value }\n"
        "def box_scale(box: Box[f32]) -> f32: { return box.value * 2.0 }\n"
        "def box_identity(box: Box[i64]) -> Box[i64]: { return box }\n"
        "def main() -> i64: {\n"
        "  let box: Box[i64]\n"
        "  box.value = 7\n"
        "  let copied: Box[i64] = box_identity(box)\n"
        "  return box_score(copied)\n"
        "}\n"
        "def float_main() -> f32: {\n"
        "  let box: Box[f32]\n"
        "  box.value = 2.5\n"
        "  return box_scale(box)\n"
        "}\n");
    CHECK(program != NULL);
    if (program) {
        BackendIrModule module;
        bir_module_init(&module, "<unit:generic-structs>");
        char err[256] = {0};
        bool ok = bir_build_program(&module, program);
        CHECK(ok);
        CHECK(bir_verify(&module, err, sizeof(err)));
        BirScalarValue result = {0};
        CHECK(bir_eval_function_value(&module, "main", &result));
        CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 7);
        CHECK(bir_eval_function_value(&module, "float_main", &result));
        CHECK(result.kind == BIR_SCALAR_F32 && bir_scalar_as_f32(result) > 4.99f &&
              bir_scalar_as_f32(result) < 5.01f);
        if (!ok || err[0]) fprintf(stderr, "generic struct: %s\\n",
                                  module.error[0] ? module.error : err);
        bir_module_free(&module);
        ast_free(program);
    }

    /* Generic readonly view structs bind T through their concrete aggregate
       descriptor and preserve the borrowed field through HIR and SSA. */
    program = parse_program(
        "struct View[T]: { data: readonly []T }\n"
        "def count[T](view: View[T]) -> i64: { return len(view.data) }\n"
        "def keep[T](view: View[T]) -> View[T]: { return view }\n"
        "def main() -> i64: {\n"
        "  let data: readonly []i64 = alloc_i64(3)\n"
        "  let view: View[i64]\n"
        "  view.data = data\n"
        "  let returned: View[i64] = keep(view)\n"
        "  return count(returned)\n"
        "}\n");
    CHECK(program != NULL);
    if (program) {
        BackendIrModule module;
        bir_module_init(&module, "<unit:generic-view-structs>");
        char err[256] = {0};
        bool ok = bir_build_program(&module, program);
        CHECK(ok);
        CHECK(bir_verify(&module, err, sizeof(err)));
        BirScalarValue result = {0};
        CHECK(bir_eval_function_value(&module, "main", &result));
        CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 3);
        if (!ok || err[0]) fprintf(stderr, "generic view struct: %s\\n",
                                  module.error[0] ? module.error : err);
        bir_module_free(&module);
        ast_free(program);
    }

    /* A generic view aggregate cannot return a view stored in a local built
       from a frame allocation. Only parameter forwarding has a proven owner. */
    program = parse_program(
        "struct View[T]: { data: readonly []T }\n"
        "def bad() -> View[i64]: {\n"
        "  let data: readonly []i64 = alloc_i64(2)\n"
        "  let view: View[i64]\n"
        "  view.data = data\n"
        "  return view\n"
        "}\n"
        "def main() -> i64: { return 0 }\n");
    CHECK(program != NULL);
    if (program) {
        BackendIrModule module;
        bir_module_init(&module, "<unit:generic-view-escape-reject>");
        CHECK(!bir_build_program(&module, program));
        CHECK(strstr(module.error, "escape") != NULL || module.error[0] != '\0');
        bir_module_free(&module);
        ast_free(program);
    }

    /* Generic struct substitution rejects ownership-bearing fields before a
       concrete descriptor reaches backend aggregate import. */
    program = parse_program(
        "struct OwnedBox[T]: { text: string }\n"
        "def main() -> i64: {\n"
        "  let box: OwnedBox[i64]\n"
        "  return 0\n"
        "}\n");
    CHECK(program != NULL);
    if (program) {
        BackendIrModule module;
        bir_module_init(&module, "<unit:generic-struct-owned-reject>");
        CHECK(!bir_build_program(&module, program));
        CHECK(module.error[0] != '\0');
        bir_module_free(&module);
        ast_free(program);
    }
}

static void test_source_scalar_generic_collections(void) {
    ASTNode *program = parse_program(
        "def first[T](values: list[T]) -> T: {\n"
        "  let value: T = values[0]\n"
        "  free(values)\n"
        "  return value\n"
        "}\n"
        "def forward[T](values: list[T]) -> list[T]: { return values }\n"
        "def append_one[T](values: list[T], item: T) -> list[T]: {\n"
        "  return append(values, item)\n"
        "}\n"
        "def last[T](values: list[T]) -> T: {\n"
        "  let value: T = pop(values)\n"
        "  free(values)\n"
        "  return value\n"
        "}\n"
        "def main() -> i64: {\n"
        "  let source: list[i64] = [4, 9]\n"
        "  let forwarded: list[i64] = forward(source)\n"
        "  let grown: list[i64] = append_one(forwarded, 12)\n"
        "  return last(grown)\n"
        "}\n");
    CHECK(program != NULL);
    if (program) {
        BackendIrModule module;
        bir_module_init(&module, "<unit:generic-scalar-collections>");
        char err[256] = {0};
        bool ok = bir_build_program(&module, program);
        CHECK(ok);
        CHECK(bir_verify(&module, err, sizeof(err)));
        BirScalarValue result = {0};
        CHECK(bir_eval_function_value(&module, "main", &result));
        CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 12);

        /* A concrete generic specialization must retain the buffer opcode
           contract after lowering. Tampering with its element descriptor must
           be rejected by the verifier before evaluation. */
        const BirFunctionInfo *append_info =
            bir_find_function(&module, "append_one__i64");
        bool corrupted = false;
        if (append_info) {
            for (size_t block = append_info->first_block;
                 block < append_info->first_block + append_info->block_count && !corrupted;
                 block++) {
                SsaBlock *ssa_block = &module.arena.blocks[block];
                for (size_t i = 0; i < ssa_block->inst_count; i++) {
                    SsaInst *inst = &module.arena.insts[ssa_block->insts[i]];
                    if (inst->op == SSA_OP_BUFFER_APPEND) {
                        inst->memory_type = module.type_f32;
                        corrupted = true;
                        break;
                    }
                }
            }
        }
        CHECK(corrupted);
        char malformed_err[256] = {0};
        CHECK(!bir_verify(&module, malformed_err, sizeof(malformed_err)));
        CHECK(malformed_err[0] != 0);

        if (!ok || err[0]) fprintf(stderr, "generic scalar collection: %s\\n",
                                  module.error[0] ? module.error : err);
        bir_module_free(&module);
        ast_free(program);
    }

    /* An owned generic list must be consumed or returned. The HIR accepts the
       specialized function, while ownership verification rejects the leak. */
    program = parse_program(
        "def leak[T](values: list[T]) -> T: { return values[0] }\n"
        "def main() -> i64: {\n"
        "  let values: list[i64] = [7]\n"
        "  return leak(values)\n"
        "}\n");
    CHECK(program != NULL);
    if (program) {
        BackendIrModule module;
        bir_module_init(&module, "<unit:generic-owned-leak>");
        char err[256] = {0};
        CHECK(bir_build_program(&module, program));
        CHECK(!bir_verify(&module, err, sizeof(err)));
        CHECK(strstr(err, "owned slice parameter") != NULL || err[0] != 0);
        bir_module_free(&module);
        ast_free(program);
    }

    /* Generic collection inference remains scalar-element-only. A concrete
       list of strings must not be accepted merely because list[T] exists. */
    program = parse_program(
        "def first[T](values: list[T]) -> T: { return values[0] }\n"
        "def main() -> i64: {\n"
        "  let values: list[string] = [\"x\"]\n"
        "  return first(values)\n"
        "}\n");
    CHECK(program != NULL);
    if (program) {
        BackendIrModule module;
        bir_module_init(&module, "<unit:generic-nonscalar-collection>");
        CHECK(!bir_build_program(&module, program));
        CHECK(module.error[0] != '\0');
        bir_module_free(&module);
        ast_free(program);
    }
}

static void test_source_generic_writable_slices(void) {
    ASTNode *program = parse_program(
        "def write_first[T](values: out []T, value: T) -> T: {\n"
        "  values[0] = value\n"
        "  return values[0]\n"
        "}\n"
        "def forward[T](values: out []T) -> out []T: { return values }\n"
        "def readonly_forward[T](values: readonly []T) -> readonly []T: { return values }\n"
        "def main() -> i64: {\n"
        "  let values: []i64 = alloc_i64(1)\n"
        "  values[0] = 3\n"
        "  let writable: out []i64 = forward(values)\n"
        "  let ro: readonly []i64 = readonly_forward(values)\n"
        "  let result = write_first(writable, 9)\n"
        "  return result + ro[0]\n"
        "}\n"
        "def float_main() -> f32: {\n"
        "  let values: []f32 = alloc_f32(1)\n"
        "  let writable: out []f32 = forward(values)\n"
        "  let result = write_first(writable, 2.5)\n"
        "  return result\n"
        "}\n");
    CHECK(program != NULL);
    if (program) {
        BackendIrModule module;
        bir_module_init(&module, "<unit:generic-writable-slices>");
        char err[256] = {0};
        bool ok = bir_build_program(&module, program);
        CHECK(ok);
        CHECK(bir_find_function(&module, "write_first__i64") != NULL);
        CHECK(bir_find_function(&module, "write_first__f32") != NULL);
        const BirFunctionInfo *readonly_forward =
            bir_find_function(&module, "readonly_forward__i64");
        CHECK(readonly_forward != NULL && readonly_forward->return_view_param == 0);
        CHECK(bir_verify(&module, err, sizeof(err)));
        if (readonly_forward) {
            BirFunctionInfo *mutable_info = (BirFunctionInfo *)readonly_forward;
            mutable_info->return_view_param = UINT32_MAX;
            char malformed_err[256] = {0};
            CHECK(!bir_verify(&module, malformed_err, sizeof(malformed_err)));
            CHECK(strstr(malformed_err, "return ABI metadata") != NULL);
            mutable_info->return_view_param = 0;
            memset(err, 0, sizeof(err));
            CHECK(bir_verify(&module, err, sizeof(err)));
        }
        BirScalarValue result = {0};
        CHECK(bir_eval_function_value(&module, "main", &result));
        /* Both readonly and writable generic returns preserve the caller's
           allocation identity. The readonly view is derived from an owned
           actual at the call boundary, while the generic function itself
           remains declared against a borrowed parameter. */
        CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 18);
        CHECK(bir_eval_function_value(&module, "float_main", &result));
        CHECK(result.kind == BIR_SCALAR_F32 && bir_scalar_as_f32(result) > 2.49f &&
              bir_scalar_as_f32(result) < 2.51f);
        if (!ok || err[0]) fprintf(stderr, "generic writable slice: %s\\n",
                                  module.error[0] ? module.error : err);
        bir_module_free(&module);
        ast_free(program);
    }

    /* A generic borrowed return may not be manufactured from a callee-local
       owner. The explicit return borrow is rejected by provenance checks. */
    program = parse_program(
        "def bad[T](values: out []T) -> out []T: {\n"
        "  let local: []i64 = alloc_i64(1)\n"
        "  return local\n"
        "}\n"
        "def main() -> i64: {\n"
        "  let values: []i64 = alloc_i64(1)\n"
        "  let view: out []i64 = bad(values)\n"
        "  return view[0]\n"
        "}\n");
    CHECK(program != NULL);
    if (program) {
        BackendIrModule module;
        bir_module_init(&module, "<unit:generic-writable-return-escape>");
        CHECK(bir_build_program(&module, program));
        char escape_err[256] = {0};
        CHECK(!bir_verify(&module, escape_err, sizeof(escape_err)));
        CHECK(strstr(escape_err, "escape") != NULL ||
              strstr(escape_err, "borrowed") != NULL || escape_err[0] != '\0');
        bir_module_free(&module);
        ast_free(program);
    }

    /* Generic return specialization must not recurse through an unresolved
       generic symbol. This keeps lifetime provenance monomorphic and prevents
       an unbounded specialization chain. */
    program = parse_program(
        "def recurse[T](values: out []T) -> out []T: { return recurse(values) }\n"
        "def main() -> i64: {\n"
        "  let values: []i64 = alloc_i64(1)\n"
        "  let view: out []i64 = recurse(values)\n"
        "  return len(view)\n"
        "}\n");
    CHECK(program != NULL);
    if (program) {
        BackendIrModule module;
        bir_module_init(&module, "<unit:generic-writable-return-recursion>");
        CHECK(!bir_build_program(&module, program));
        CHECK(module.error[0] != '\0');
        bir_module_free(&module);
        ast_free(program);
    }

    /* A readonly view cannot satisfy a generic writable parameter. */
    program = parse_program(
        "def write_first[T](values: out []T, value: T) -> T: {\n"
        "  values[0] = value\n"
        "  return values[0]\n"
        "}\n"
        "def bad(values: readonly []i64) -> i64: {\n"
        "  return write_first(values, 9)\n"
        "}\n"
        "def main() -> i64: { return 0 }\n");
    CHECK(program != NULL);
    if (program) {
        BackendIrModule module;
        bir_module_init(&module, "<unit:generic-writable-readonly-reject>");
        CHECK(!bir_build_program(&module, program));
        CHECK(strstr(module.error, "wrong type") != NULL ||
              strstr(module.error, "writable") != NULL || module.error[0] != '\0');
        bir_module_free(&module);
        ast_free(program);
    }

    /* Generic mutable slices remain scalar-element-only and cannot be used
       with a non-scalar parameter. */
    program = parse_program(
        "def write_first[T](values: out []T, value: T) -> T: {\n"
        "  values[0] = value\n"
        "  return values[0]\n"
        "}\n"
        "def main() -> i64: {\n"
        "  let values: list[i64] = [1]\n"
        "  return write_first(values, 2)\n"
        "}\n");
    CHECK(program != NULL);
    if (program) {
        BackendIrModule module;
        bir_module_init(&module, "<unit:generic-writable-nonscalar-reject>");
        CHECK(!bir_build_program(&module, program));
        CHECK(module.error[0] != '\0');
        bir_module_free(&module);
        ast_free(program);
    }
}

static void test_typed_call_rejected(void) {
    ASTNode *program = parse_program(
        "def takes(x: i64) -> i64: { return x }\n"
        "def main() -> i64: { return takes(true) }\n");
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule module;
    bir_module_init(&module, "<unit:typed_call>");
    CHECK(!bir_build_program(&module, program));
    CHECK(strstr(module.error, "wrong type") != NULL);
    bir_module_free(&module);
    ast_free(program);
}

static void test_nonfinalized_type_rejected(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:unfinalized>");
    SsaArena *arena = &module.arena;
    CobraType *fresh = cobra_type_named(module.type_arena, COBRA_TYPE_I64, "fresh");
    CHECK(fresh != NULL);
    CHECK(!fresh->finalized);
    SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
    SsaValueRef value = bir_add_const(arena, bir_scalar_i64(fresh, 5), 1, 1);
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
    SsaValueRef c0 = bir_add_const(arena, bir_scalar_i64(module.type_i64, 2), 1, 1);
    SsaValueRef c1 = bir_add_const(arena, bir_scalar_i64(module.type_i64, 3), 1, 1);
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
    const char *golden =
        "; backend IR: <unit:printer>\n"
        "fn \"main\"() -> i64\n"
        "  abi cobra stack_align=16 stack_size=0 params=[] return=[gpr0]\n"
        "value v1: i64 = const 2\n"
        "value v2: i64 = const 3\n"
        "block b1 \"entry\" [entry]() ; line 1\n"
        "  v3: i64 = mul(v1, v2) ; line 1\n"
        "  return(v3) ; line 1\n";
    CHECK(first && strcmp(first, golden) == 0);
    free(first);
    free(second);
    bir_module_free(&module);
}

static void test_call_abi_metadata(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:call-abi>");

    const CobraType *many_params[9] = {
        module.type_i64, module.type_i64, module.type_i64,
        module.type_i64, module.type_i64, module.type_i64,
        module.type_f64, module.type_i64, module.type_i64
    };
    CHECK(bir_declare_function(&module, "many", 9, many_params,
                               module.type_f64, true));
    BirFunctionInfo *many = &module.functions[0];
    CHECK(many->call_abi.convention == BIR_CALLING_CONVENTION_COBRA);
    CHECK(many->call_abi.param_count == 9);
    CHECK(many->call_abi.stack_alignment == BIR_ABI_STACK_ALIGNMENT);
    CHECK(many->call_abi.stack_size == 16);
    CHECK(many->call_abi.params[0].count == 1 &&
          many->call_abi.params[0].parts[0].storage == BIR_ABI_STORAGE_REGISTER &&
          many->call_abi.params[0].parts[0].register_class == BIR_ABI_REGISTER_GPR &&
          many->call_abi.params[0].parts[0].register_index == 0);
    CHECK(many->call_abi.params[5].parts[0].register_index == 5);
    CHECK(many->call_abi.params[6].count == 1 &&
          many->call_abi.params[6].parts[0].register_class == BIR_ABI_REGISTER_XMM &&
          many->call_abi.params[6].parts[0].register_index == 0);
    CHECK(many->call_abi.params[7].parts[0].storage == BIR_ABI_STORAGE_STACK &&
          many->call_abi.params[7].parts[0].stack_offset == 0);
    CHECK(many->call_abi.params[8].parts[0].storage == BIR_ABI_STORAGE_STACK &&
          many->call_abi.params[8].parts[0].stack_offset == 8);
    CHECK(many->call_abi.returns.count == 1 &&
          many->call_abi.returns.parts[0].storage == BIR_ABI_STORAGE_REGISTER &&
          many->call_abi.returns.parts[0].register_class == BIR_ABI_REGISTER_XMM &&
          many->call_abi.returns.parts[0].register_index == 0);
    CHECK(bir_validate_function_abi(&module, many));

    const CobraType *view = bir_view_type(&module, module.type_i64);
    CHECK(view != NULL);
    CHECK(bir_declare_function(&module, "view_identity", 1, &view, view, true));
    BirFunctionInfo *view_identity = &module.functions[1];
    CHECK(view_identity->call_abi.params[0].count == 2 &&
          view_identity->call_abi.params[0].parts[0].register_class == BIR_ABI_REGISTER_GPR &&
          view_identity->call_abi.params[0].parts[1].register_index == 1);
    CHECK(view_identity->call_abi.returns.count == 2 &&
          view_identity->call_abi.returns.parts[0].register_class == BIR_ABI_REGISTER_GPR &&
          view_identity->call_abi.returns.parts[1].register_index == 1);

    CobraType *pair = cobra_type_make(module.type_arena, COBRA_TYPE_STRUCT, "AbiPair",
                                      NULL, NULL, NULL, NULL,
                                      COBRA_OWNERSHIP_VALUE,
                                      COBRA_MUTABILITY_DEFAULT, -1);
    CHECK(pair != NULL);
    CHECK(cobra_type_add_field(pair, "value", module.type_i64,
                               COBRA_OWNERSHIP_VALUE, COBRA_MUTABILITY_DEFAULT, -1));
    CHECK(cobra_type_finalize(module.type_arena, pair));
    const CobraType *pair_params[1] = {pair};
    CHECK(bir_declare_function(&module, "pair_identity", 1, pair_params,
                               pair, true));
    BirFunctionInfo *pair_identity = &module.functions[2];
    CHECK(pair_identity->has_hidden_return_storage &&
          pair_identity->call_abi.param_count == 2);
    CHECK(pair_identity->call_abi.params[0].count == 1 &&
          pair_identity->call_abi.params[0].parts[0].pass_mode == BIR_ABI_PASS_INDIRECT &&
          pair_identity->call_abi.params[0].parts[0].register_class == BIR_ABI_REGISTER_GPR &&
          pair_identity->call_abi.params[0].parts[0].register_index == 0);
    CHECK(pair_identity->call_abi.params[1].parts[0].pass_mode == BIR_ABI_PASS_INDIRECT &&
          pair_identity->call_abi.params[1].parts[0].register_index == 1);
    CHECK(pair_identity->call_abi.returns.count == 1 &&
          pair_identity->call_abi.returns.parts[0].pass_mode == BIR_ABI_PASS_INDIRECT &&
          pair_identity->call_abi.returns.parts[0].storage == BIR_ABI_STORAGE_NONE);
    CHECK(bir_validate_function_abi(&module, pair_identity));

    uint16_t saved_index = many->call_abi.params[0].parts[0].register_index;
    many->call_abi.params[0].parts[0].register_index = 99;
    CHECK(!bir_validate_function_abi(&module, many));
    many->call_abi.params[0].parts[0].register_index = saved_index;
    bir_module_free(&module);
}

static uint32_t random_cfg_next(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static void test_randomized_cfg(void) {
    uint32_t state = 0xC0BFA123u;
    for (size_t iteration = 0; iteration < 64; iteration++) {
        BackendIrModule module;
        bir_module_init(&module, "<unit:random-cfg>");
        SsaArena *arena = &module.arena;
        SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
        SsaBlockRef current = entry;
        SsaValueRef final_value = SSA_VALUE_NONE;
        int64_t expected = 0;
        size_t stages = 1 + random_cfg_next(&state) % 6;
        bool valid = true;

        for (size_t stage = 0; stage < stages; stage++) {
            SsaBlockRef then_block = bir_add_block(arena, "then", 1, 1);
            SsaBlockRef else_block = bir_add_block(arena, "else", 1, 1);
            SsaBlockRef merge = bir_add_block(arena, "merge", 1, 1);
            bool take_then = (random_cfg_next(&state) & 1u) != 0;
            int64_t then_value = (int64_t)(random_cfg_next(&state) % 1000u);
            int64_t else_value = (int64_t)(random_cfg_next(&state) % 1000u);
            SsaValueRef condition = bir_add_const(
                arena, bir_scalar_bool(module.type_bool, take_then), 1, 1);
            SsaValueRef then_result = bir_add_const(
                arena, bir_scalar_i64(module.type_i64, then_value), 1, 1);
            SsaValueRef else_result = bir_add_const(
                arena, bir_scalar_i64(module.type_i64, else_value), 1, 1);
            SsaValueRef parameter = bir_add_block_param(arena, merge,
                                                         module.type_i64, 1, 1);
            valid = valid && then_block != SSA_BLOCK_NONE &&
                    else_block != SSA_BLOCK_NONE && merge != SSA_BLOCK_NONE &&
                    condition != SSA_VALUE_NONE && then_result != SSA_VALUE_NONE &&
                    else_result != SSA_VALUE_NONE && parameter != SSA_VALUE_NONE;
            valid = valid && bir_add_edge(arena, current, then_block) &&
                    bir_add_edge(arena, current, else_block) &&
                    bir_set_branch(arena, current, condition,
                                   then_block, else_block, NULL, 0, NULL, 0, 1, 1);
            valid = valid && bir_add_edge(arena, then_block, merge) &&
                    bir_set_jump(arena, then_block, merge, &then_result, 1, 1, 1);
            valid = valid && bir_add_edge(arena, else_block, merge) &&
                    bir_set_jump(arena, else_block, merge, &else_result, 1, 1, 1);
            current = merge;
            final_value = parameter;
            expected = take_then ? then_value : else_value;
        }
        valid = valid && bir_set_return(arena, current, final_value, 1, 1) &&
                bir_register_function_info(&module, "main", entry, 0, NULL,
                                           module.type_i64, true);
        char err[512] = {0};
        bool verified = valid && bir_verify(&module, err, sizeof(err));
        BirScalarValue result = {0};
        bool evaluated = verified && bir_eval_function_value(&module, "main", &result);
        checks++;
        if (!evaluated || result.kind != BIR_SCALAR_I64 ||
            result.payload.i64 != expected) {
            failures++;
            fprintf(stderr, "FAIL randomized CFG %zu: valid=%d verify=%d eval=%d result=%lld expected=%lld (%s)\n",
                    iteration, valid, verified, evaluated,
                    (long long)result.payload.i64, (long long)expected, err);
        }
        bir_module_free(&module);
    }
}

static void test_f32_dump_bit_patterns(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:f32-bits>");
    SsaArena *arena = &module.arena;
    SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
    BirScalarValue neg_zero = bir_scalar_f32(module.type_f32, 0.0f);
    BirScalarValue infinity = bir_scalar_f32(module.type_f32, 0.0f);
    BirScalarValue nan = bir_scalar_f32(module.type_f32, 0.0f);
    neg_zero.payload.f32_bits = 0x80000000u;
    infinity.payload.f32_bits = 0x7f800000u;
    nan.payload.f32_bits = 0x7fc12345u;
    CHECK(bir_add_const(arena, bir_scalar_f32(module.type_f32, 0.0f), 1, 1) != SSA_VALUE_NONE);
    CHECK(bir_add_const(arena, neg_zero, 1, 1) != SSA_VALUE_NONE);
    CHECK(bir_add_const(arena, infinity, 1, 1) != SSA_VALUE_NONE);
    SsaValueRef nan_value = bir_add_const(arena, nan, 1, 1);
    CHECK(nan_value != SSA_VALUE_NONE);
    CHECK(bir_set_return(arena, entry, nan_value, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_f32, true));
    char *dump = NULL;
    size_t dump_len = 0;
    FILE *out = open_memstream(&dump, &dump_len);
    CHECK(out != NULL);
    bir_dump(&module, out);
    fclose(out);
    CHECK(dump && strstr(dump, "f32bits(0x00000000)") != NULL);
    CHECK(dump && strstr(dump, "f32bits(0x80000000)") != NULL);
    CHECK(dump && strstr(dump, "f32bits(0x7f800000)") != NULL);
    CHECK(dump && strstr(dump, "f32bits(0x7fc12345)") != NULL);
    free(dump);
    bir_module_free(&module);
}

static void test_scalar_struct_pipeline(void) {
    ASTNode *program = parse_program(
        "struct Point: { x: i64, y: i64 }\n"
        "def main() -> i64: {\n"
        "  let point: Point\n"
        "  point.x = 3\n"
        "  point.y = 4\n"
        "  let copy: Point\n"
        "  copy = point\n"
        "  return copy.x * 10 + copy.y\n"
        "}\n");
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule module;
    bir_module_init(&module, "<unit:scalar-struct>");
    char err[512] = {0};
    bool built = bir_build_program(&module, program);
    CHECK(built);
    if (!built) fprintf(stderr, "struct build: %s\n", module.error);
    CHECK(bir_verify(&module, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "struct verify: %s\n", err);
    char *dump = NULL;
    size_t dump_len = 0;
    FILE *dump_file = open_memstream(&dump, &dump_len);
    CHECK(dump_file != NULL);
    bir_dump(&module, dump_file);
    fclose(dump_file);
    CHECK(dump && strstr(dump, "field_addr") != NULL);
    CHECK(dump && strstr(dump, "agg_copy") != NULL);
    free(dump);
    BirScalarValue result = {0};
    CHECK(bir_eval_function_value(&module, "main", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 34);
    bir_module_free(&module);
    ast_free(program);
}

static void test_struct_call_abi(void) {
    ASTNode *program = parse_program(
        "struct Point: { x: i64, y: i64 }\n"
        "def score(point: Point) -> i64: {\n"
        "  return point.x * 10 + point.y\n"
        "}\n"
        "def make_point(value: i64) -> Point: {\n"
        "  let point: Point\n"
        "  point.x = value\n"
        "  point.y = value + 1\n"
        "  return point\n"
        "}\n"
        "def recursive_score(n: i64, point: Point) -> i64: {\n"
        "  if n == 0: { return score(point) }\n"
        "  return recursive_score(n - 1, point) + 1\n"
        "}\n"
        "def main() -> i64: {\n"
        "  let point: Point\n"
        "  point.x = 3\n"
        "  point.y = 4\n"
        "  let made: Point\n"
        "  made = make_point(7)\n"
        "  return score(point) + score(made) + recursive_score(3, point)\n"
        "}\n");
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule module;
    bir_module_init(&module, "<unit:struct-call-abi>");
    char err[512] = {0};
    bool built = bir_build_program(&module, program);
    CHECK(built);
    if (!built) fprintf(stderr, "struct ABI build: %s\n", module.error);
    CHECK(bir_verify(&module, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "struct ABI verify: %s\n", err);
    const BirFunctionInfo *score = bir_find_function(&module, "score");
    const BirFunctionInfo *make = bir_find_function(&module, "make_point");
    CHECK(score && score->param_count == 1 && score->ssa_param_count == 1);
    CHECK(score && score->param_types[0]->kind == COBRA_TYPE_STRUCT &&
          score->param_value_types[0]->kind == COBRA_TYPE_POINTER);
    CHECK(make && make->has_hidden_return_storage && make->ssa_param_count == 2);
    BirScalarValue result = {0};
    CHECK(bir_eval_function_value(&module, "main", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 34 + 78 + 37);
    bir_module_free(&module);
    ast_free(program);
}

static void test_typed_scalar_payload_rejected(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:scalar-payload>");
    SsaArena *arena = &module.arena;
    SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
    SsaValueRef value = bir_add_const(arena, bir_scalar_f32(module.type_f32, 1.0f), 1, 1);
    arena->values[value].const_value.kind = BIR_SCALAR_I64;
    CHECK(bir_set_return(arena, entry, value, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_f32, true));
    char err[256] = {0};
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "payload") != NULL);
    bir_module_free(&module);
}

static void test_pointer_ownership_contracts(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:pointer-ownership>");
    SsaArena *arena = &module.arena;
    const CobraType *i64_ptr = bir_pointer_type(&module, module.type_i64);
    const CobraType *params[1] = {i64_ptr};
    CHECK(bir_declare_function(&module, "readonly_sink", 1, params,
                               module.type_i64, true));
    SsaBlockRef entry = bir_add_entry_block(arena, "readonly_sink", 1, 1);
    SsaValueRef pointer = bir_add_value(arena, SSA_VALUE_PARAM, i64_ptr, 1, 1);
    arena->values[pointer].param_index = 0;
    SsaValueRef value = bir_add_const(arena, bir_scalar_i64(module.type_i64, 7), 1, 1);
    SsaInstRef store = bir_add_typed_store(arena, module.type_i64, i64_ptr,
                                           pointer, value, 8, 8, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, store));
    CHECK(bir_set_return(arena, entry, value, 1, 1));
    SsaValueRef refs[1] = {pointer};
    CHECK(bir_register_function_info(&module, "readonly_sink", entry, 1, refs,
                                     module.type_i64, true));
    char err[256] = {0};
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "borrow contract") != NULL || strstr(err, "readonly") != NULL);
    bir_module_free(&module);

    bir_module_init(&module, "<unit:pointer-return-escape>");
    arena = &module.arena;
    i64_ptr = bir_pointer_type(&module, module.type_i64);
    CHECK(bir_declare_function(&module, "leak", 1, &i64_ptr,
                               i64_ptr, true));
    entry = bir_add_entry_block(arena, "leak", 1, 1);
    pointer = bir_add_value(arena, SSA_VALUE_PARAM, i64_ptr, 1, 1);
    arena->values[pointer].param_index = 0;
    CHECK(bir_set_return(arena, entry, pointer, 1, 1));
    refs[0] = pointer;
    CHECK(bir_register_function_info(&module, "leak", entry, 1, refs,
                                     i64_ptr, true));
    memset(err, 0, sizeof(err));
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "escape") != NULL || strstr(err, "pointer return") != NULL);
    bir_module_free(&module);

    bir_module_init(&module, "<unit:pointer-contract-missing>");
    arena = &module.arena;
    i64_ptr = bir_pointer_type(&module, module.type_i64);
    entry = bir_add_entry_block(arena, "entry", 1, 1);
    SsaInstRef slot = bir_add_stack_slot(arena, i64_ptr, module.type_i64,
                                         0, 8, 0, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, slot));
    pointer = bir_inst_result(arena, slot, 1, 1);
    arena->values[pointer].pointer_contract = BIR_POINTER_CONTRACT_UNKNOWN;
    CHECK(bir_set_return(arena, entry, pointer, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     i64_ptr, true));
    memset(err, 0, sizeof(err));
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "contract") != NULL || strstr(err, "ownership") != NULL);
    bir_module_free(&module);
}

static void test_path_sensitive_ownership(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:ownership-branch-safe>");
    CHECK(bir_declare_region(&module, 1, 0));
    const CobraType *pointer_type = bir_pointer_type(&module, module.type_i64);
    SsaArena *arena = &module.arena;
    SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
    SsaBlockRef then_block = bir_add_block(arena, "then", 1, 1);
    SsaBlockRef else_block = bir_add_block(arena, "else", 1, 1);
    SsaBlockRef merge = bir_add_block(arena, "merge", 1, 1);
    SsaInstRef inst = bir_add_region_enter(arena, 1, 0, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, inst));
    inst = bir_add_region_stack_slot(arena, pointer_type, module.type_i64,
                                     0, 8, 0, 1, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, inst));
    SsaValueRef pointer = bir_inst_result(arena, inst, 1, 1);
    SsaValueRef condition = bir_add_const(arena, bir_scalar_bool(module.type_bool, true), 1, 1);
    CHECK(bir_add_edge(arena, entry, then_block));
    CHECK(bir_add_edge(arena, entry, else_block));
    CHECK(bir_set_branch(arena, entry, condition, then_block, else_block,
                         NULL, 0, NULL, 0, 1, 1));
    inst = bir_add_destroy(arena, pointer, 1, 1);
    CHECK(bir_block_add_inst(arena, then_block, inst));
    CHECK(bir_add_edge(arena, then_block, merge));
    CHECK(bir_set_jump(arena, then_block, merge, NULL, 0, 1, 1));
    inst = bir_add_destroy(arena, pointer, 1, 1);
    CHECK(bir_block_add_inst(arena, else_block, inst));
    CHECK(bir_add_edge(arena, else_block, merge));
    CHECK(bir_set_jump(arena, else_block, merge, NULL, 0, 1, 1));
    inst = bir_add_region_exit(arena, 1, 1, 1);
    CHECK(bir_block_add_inst(arena, merge, inst));
    SsaValueRef result = bir_add_const(arena, bir_scalar_i64(module.type_i64, 7), 1, 1);
    CHECK(bir_set_return(arena, merge, result, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));
    char err[256] = {0};
    CHECK(bir_verify(&module, err, sizeof(err)));
    BirScalarValue evaluated = {0};
    CHECK(bir_eval_function_value(&module, "main", &evaluated));
    CHECK(evaluated.payload.i64 == 7);
    bir_module_free(&module);

    /* One path destroys and the other preserves the allocation: a merge use
       must be rejected instead of choosing one predecessor's state. */
    bir_module_init(&module, "<unit:ownership-branch-ambiguous>");
    CHECK(bir_declare_region(&module, 1, 0));
    pointer_type = bir_pointer_type(&module, module.type_i64);
    arena = &module.arena;
    entry = bir_add_entry_block(arena, "entry", 1, 1);
    then_block = bir_add_block(arena, "then", 1, 1);
    else_block = bir_add_block(arena, "else", 1, 1);
    merge = bir_add_block(arena, "merge", 1, 1);
    inst = bir_add_region_enter(arena, 1, 0, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, inst));
    inst = bir_add_region_stack_slot(arena, pointer_type, module.type_i64,
                                     0, 8, 0, 1, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, inst));
    pointer = bir_inst_result(arena, inst, 1, 1);
    condition = bir_add_const(arena, bir_scalar_bool(module.type_bool, true), 1, 1);
    CHECK(bir_add_edge(arena, entry, then_block));
    CHECK(bir_add_edge(arena, entry, else_block));
    CHECK(bir_set_branch(arena, entry, condition, then_block, else_block,
                         NULL, 0, NULL, 0, 1, 1));
    inst = bir_add_destroy(arena, pointer, 1, 1);
    CHECK(bir_block_add_inst(arena, then_block, inst));
    CHECK(bir_add_edge(arena, then_block, merge));
    CHECK(bir_set_jump(arena, then_block, merge, NULL, 0, 1, 1));
    CHECK(bir_add_edge(arena, else_block, merge));
    CHECK(bir_set_jump(arena, else_block, merge, NULL, 0, 1, 1));
    SsaInstRef load = bir_add_typed_load(arena, module.type_i64, pointer_type,
                                         pointer, 8, 8, 1, 1);
    CHECK(bir_block_add_inst(arena, merge, load));
    result = bir_inst_result(arena, load, 1, 1);
    CHECK(bir_set_return(arena, merge, result, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));
    memset(err, 0, sizeof(err));
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "ambiguous") != NULL || strstr(err, "destruction") != NULL);
    bir_module_free(&module);

    /* A loop-carried state also joins the live preheader with the destroyed
       backedge and must not make a post-loop use appear safe. */
    bir_module_init(&module, "<unit:ownership-loop-ambiguous>");
    CHECK(bir_declare_region(&module, 1, 0));
    pointer_type = bir_pointer_type(&module, module.type_i64);
    arena = &module.arena;
    entry = bir_add_entry_block(arena, "entry", 1, 1);
    SsaBlockRef header = bir_add_block(arena, "header", 1, 1);
    SsaBlockRef body = bir_add_block(arena, "body", 1, 1);
    SsaBlockRef exit_block = bir_add_block(arena, "exit", 1, 1);
    inst = bir_add_region_enter(arena, 1, 0, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, inst));
    inst = bir_add_region_stack_slot(arena, pointer_type, module.type_i64,
                                     0, 8, 0, 1, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, inst));
    pointer = bir_inst_result(arena, inst, 1, 1);
    CHECK(bir_add_edge(arena, entry, header));
    CHECK(bir_set_jump(arena, entry, header, NULL, 0, 1, 1));
    condition = bir_add_const(arena, bir_scalar_bool(module.type_bool, true), 1, 1);
    CHECK(bir_add_edge(arena, header, body));
    CHECK(bir_add_edge(arena, header, exit_block));
    CHECK(bir_set_branch(arena, header, condition, body, exit_block,
                         NULL, 0, NULL, 0, 1, 1));
    inst = bir_add_destroy(arena, pointer, 1, 1);
    CHECK(bir_block_add_inst(arena, body, inst));
    CHECK(bir_add_edge(arena, body, header));
    CHECK(bir_set_jump(arena, body, header, NULL, 0, 1, 1));
    load = bir_add_typed_load(arena, module.type_i64, pointer_type,
                              pointer, 8, 8, 1, 1);
    CHECK(bir_block_add_inst(arena, exit_block, load));
    result = bir_inst_result(arena, load, 1, 1);
    CHECK(bir_set_return(arena, exit_block, result, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));
    memset(err, 0, sizeof(err));
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "ambiguous") != NULL || strstr(err, "destruction") != NULL);
    bir_module_free(&module);
}

static void test_borrowed_readonly_views(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:readonly-view>");
    const CobraType *view_type = bir_view_type(&module, module.type_i64);
    const CobraType *pointer_type = bir_pointer_type(&module, module.type_i64);
    CHECK(view_type != NULL && view_type->ownership == COBRA_OWNERSHIP_BORROWED &&
          view_type->mutability == COBRA_MUTABILITY_READONLY);
    CHECK(pointer_type != NULL);
    const CobraType *view_params[1] = {view_type};
    CHECK(bir_declare_function(&module, "view_length", 1, view_params,
                               module.type_i64, true));
    SsaArena *arena = &module.arena;
    SsaBlockRef sink_entry = bir_add_entry_block(arena, "view_length", 1, 1);
    SsaValueRef view_param = bir_add_value(arena, SSA_VALUE_PARAM, view_type, 1, 1);
    arena->values[view_param].param_index = 0;
    SsaInstRef sink_len = bir_add_view_len(arena, module.type_i64, view_type,
                                           view_param, 1, 1);
    CHECK(bir_block_add_inst(arena, sink_entry, sink_len));
    SsaValueRef sink_result = bir_inst_result(arena, sink_len, 1, 1);
    CHECK(bir_set_return(arena, sink_entry, sink_result, 1, 1));
    SsaValueRef sink_params[1] = {view_param};
    CHECK(bir_register_function_info(&module, "view_length", sink_entry, 1,
                                     sink_params, module.type_i64, true));

    SsaBlockRef entry = bir_add_entry_block(arena, "main", 1, 1);
    SsaInstRef slot = bir_add_stack_slot(arena, pointer_type, module.type_i64,
                                         0, 8, 0, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, slot));
    SsaValueRef pointer = bir_inst_result(arena, slot, 1, 1);
    SsaValueRef source = bir_add_const(arena, bir_scalar_i64(module.type_i64, 73), 1, 1);
    SsaInstRef store = bir_add_typed_store(arena, module.type_i64, pointer_type,
                                           pointer, source, 8, 8, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, store));
    SsaValueRef length = bir_add_const(arena, bir_scalar_i64(module.type_i64, 1), 1, 1);
    SsaInstRef make = bir_add_view_make(arena, view_type, module.type_i64,
                                        pointer, length, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, make));
    SsaValueRef    view = bir_inst_result(arena, make, 1, 1);
    SsaInstRef view_ptr = bir_add_view_ptr(arena, pointer_type, module.type_i64,
                                           view, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, view_ptr));
    SsaValueRef borrowed_pointer = bir_inst_result(arena, view_ptr, 1, 1);
    SsaInstRef load = bir_add_typed_load(arena, module.type_i64, pointer_type,
                                         borrowed_pointer, 8, 8, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, load));
    SsaValueRef loaded = bir_inst_result(arena, load, 1, 1);
    SsaInstRef call = bir_add_inst(arena, SSA_OP_CALL, module.type_i64,
                                   &view, 1, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, call));
    snprintf(arena->insts[call].callee, sizeof(arena->insts[call].callee),
             "view_length");
    arena->insts[call].effect = SSA_EFFECT_CALL;
    SsaValueRef called_length = bir_inst_result(arena, call, 1, 1);
    SsaValueRef sum_ops[2] = {loaded, called_length};
    SsaInstRef sum = bir_add_inst(arena, SSA_OP_ADD, module.type_i64,
                                  sum_ops, 2, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, sum));
    SsaValueRef result = bir_inst_result(arena, sum, 1, 1);
    CHECK(bir_set_return(arena, entry, result, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));
    char err[256] = {0};
    CHECK(bir_verify(&module, err, sizeof(err)));
    BirScalarValue evaluated = {0};
    CHECK(bir_eval_function_value(&module, "main", &evaluated));
    CHECK(evaluated.kind == BIR_SCALAR_I64 && evaluated.payload.i64 == 74);
    bir_module_free(&module);

    /* View construction also enforces the backing allocation's byte bound. */
    bir_module_init(&module, "<unit:readonly-view-bounds>");
    view_type = bir_view_type(&module, module.type_i64);
    pointer_type = bir_pointer_type(&module, module.type_i64);
    arena = &module.arena;
    entry = bir_add_entry_block(arena, "bounds", 1, 1);
    slot = bir_add_stack_slot(arena, pointer_type, module.type_i64, 0, 8, 0, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, slot));
    pointer = bir_inst_result(arena, slot, 1, 1);
    length = bir_add_const(arena, bir_scalar_i64(module.type_i64, 2), 1, 1);
    make = bir_add_view_make(arena, view_type, module.type_i64, pointer, length, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, make));
    view = bir_inst_result(arena, make, 1, 1);
    CHECK(bir_set_return(arena, entry, length, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));
    memset(err, 0, sizeof(err));
    CHECK(bir_verify(&module, err, sizeof(err)));
    CHECK(!bir_eval_function_value(&module, "main", &evaluated));
    (void)view;
    bir_module_free(&module);

    /* A readonly view's extracted pointer cannot be used as a store target. */
    bir_module_init(&module, "<unit:readonly-view-write>");
    view_type = bir_view_type(&module, module.type_i64);
    pointer_type = bir_pointer_type(&module, module.type_i64);
    arena = &module.arena;
    entry = bir_add_entry_block(arena, "write", 1, 1);
    slot = bir_add_stack_slot(arena, pointer_type, module.type_i64, 0, 8, 0, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, slot));
    pointer = bir_inst_result(arena, slot, 1, 1);
    length = bir_add_const(arena, bir_scalar_i64(module.type_i64, 1), 1, 1);
    make = bir_add_view_make(arena, view_type, module.type_i64, pointer, length, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, make));
    view = bir_inst_result(arena, make, 1, 1);
    view_ptr = bir_add_view_ptr(arena, pointer_type, module.type_i64, view, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, view_ptr));
    borrowed_pointer = bir_inst_result(arena, view_ptr, 1, 1);
    source = bir_add_const(arena, bir_scalar_i64(module.type_i64, 1), 1, 1);
    store = bir_add_typed_store(arena, module.type_i64, pointer_type,
                                borrowed_pointer, source, 8, 8, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, store));
    CHECK(bir_set_return(arena, entry, source, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));
    memset(err, 0, sizeof(err));
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "borrow contract") != NULL || strstr(err, "readonly") != NULL);
    bir_module_free(&module);

    /* Region exit invalidates a view even when the view itself is still in SSA. */
    bir_module_init(&module, "<unit:readonly-view-region>");
    CHECK(bir_declare_region(&module, 1, 0));
    view_type = bir_view_type(&module, module.type_i64);
    pointer_type = bir_pointer_type(&module, module.type_i64);
    arena = &module.arena;
    entry = bir_add_entry_block(arena, "region_view", 1, 1);
    SsaInstRef enter = bir_add_region_enter(arena, 1, 0, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, enter));
    slot = bir_add_region_stack_slot(arena, pointer_type, module.type_i64,
                                     0, 8, 0, 1, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, slot));
    pointer = bir_inst_result(arena, slot, 1, 1);
    length = bir_add_const(arena, bir_scalar_i64(module.type_i64, 1), 1, 1);
    make = bir_add_view_make(arena, view_type, module.type_i64, pointer, length, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, make));
    view = bir_inst_result(arena, make, 1, 1);
    SsaInstRef exit = bir_add_region_exit(arena, 1, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, exit));
    view_ptr = bir_add_view_ptr(arena, pointer_type, module.type_i64, view, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, view_ptr));
    borrowed_pointer = bir_inst_result(arena, view_ptr, 1, 1);
    load = bir_add_typed_load(arena, module.type_i64, pointer_type,
                              borrowed_pointer, 8, 8, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, load));
    loaded = bir_inst_result(arena, load, 1, 1);
    CHECK(bir_set_return(arena, entry, loaded, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));
    memset(err, 0, sizeof(err));
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "destruction") != NULL || strstr(err, "destroyed") != NULL);
    bir_module_free(&module);
}

static void test_region_lifetimes(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:regions>");
    CHECK(bir_declare_region(&module, 1, 0));
    CHECK(bir_declare_region(&module, 2, 1));
    SsaArena *arena = &module.arena;
    const CobraType *pointer_type = bir_pointer_type(&module, module.type_i64);
    SsaBlockRef entry = bir_add_entry_block(arena, "region", 1, 1);
    SsaInstRef enter = bir_add_region_enter(arena, 1, 0, 1, 1);
    SsaInstRef slot = bir_add_region_stack_slot(arena, pointer_type, module.type_i64,
                                                0, 8, 0, 1, 1, 1);
    SsaValueRef pointer;
    SsaValueRef value = bir_add_const(arena, bir_scalar_i64(module.type_i64, 55), 1, 1);
    SsaValueRef loaded;
    SsaInstRef store;
    SsaInstRef load;
    CHECK(bir_block_add_inst(arena, entry, enter));
    CHECK(bir_block_add_inst(arena, entry, slot));
    pointer = bir_inst_result(arena, slot, 1, 1);
    store = bir_add_typed_store(arena, module.type_i64, pointer_type,
                                pointer, value, 8, 8, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, store));
    load = bir_add_typed_load(arena, module.type_i64, pointer_type,
                              pointer, 8, 8, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, load));
    loaded = bir_inst_result(arena, load, 1, 1);
    SsaInstRef exit = bir_add_region_exit(arena, 1, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, exit));
    CHECK(bir_set_return(arena, entry, loaded, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));
    char err[256] = {0};
    CHECK(bir_verify(&module, err, sizeof(err)));
    BirScalarValue result = {0};
    CHECK(bir_eval_function_value(&module, "main", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 55);
    bir_module_free(&module);

    /* A region exit invalidates every allocation still owned by that region. */
    bir_module_init(&module, "<unit:region-uaf>");
    CHECK(bir_declare_region(&module, 1, 0));
    arena = &module.arena;
    pointer_type = bir_pointer_type(&module, module.type_i64);
    entry = bir_add_entry_block(arena, "uaf", 1, 1);
    enter = bir_add_region_enter(arena, 1, 0, 1, 1);
    slot = bir_add_region_stack_slot(arena, pointer_type, module.type_i64,
                                     0, 8, 0, 1, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, enter));
    CHECK(bir_block_add_inst(arena, entry, slot));
    pointer = bir_inst_result(arena, slot, 1, 1);
    exit = bir_add_region_exit(arena, 1, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, exit));
    load = bir_add_typed_load(arena, module.type_i64, pointer_type,
                              pointer, 8, 8, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, load));
    loaded = bir_inst_result(arena, load, 1, 1);
    CHECK(bir_set_return(arena, entry, loaded, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));
    memset(err, 0, sizeof(err));
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "after destruction") != NULL || strstr(err, "destroyed") != NULL);
    bir_module_free(&module);

    /* Explicit transfer changes the owner region; exiting the child then
       destroys the moved allocation exactly once. */
    bir_module_init(&module, "<unit:region-transfer>");
    CHECK(bir_declare_region(&module, 1, 0));
    CHECK(bir_declare_region(&module, 2, 1));
    arena = &module.arena;
    pointer_type = bir_pointer_type(&module, module.type_i64);
    entry = bir_add_entry_block(arena, "transfer", 1, 1);
    enter = bir_add_region_enter(arena, 1, 0, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, enter));
    SsaInstRef child = bir_add_region_enter(arena, 2, 1, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, child));
    slot = bir_add_region_stack_slot(arena, pointer_type, module.type_i64,
                                     0, 8, 0, 1, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, slot));
    pointer = bir_inst_result(arena, slot, 1, 1);
    SsaInstRef transfer = bir_add_transfer(arena, pointer_type, pointer, 2, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, transfer));
    SsaValueRef moved = bir_inst_result(arena, transfer, 1, 1);
    exit = bir_add_region_exit(arena, 2, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, exit));
    exit = bir_add_region_exit(arena, 1, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, exit));
    value = bir_add_const(arena, bir_scalar_i64(module.type_i64, 9), 1, 1);
    (void)moved;
    CHECK(bir_set_return(arena, entry, value, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));
    memset(err, 0, sizeof(err));
    CHECK(bir_verify(&module, err, sizeof(err)));
    CHECK(bir_eval_function_value(&module, "main", &result));
    CHECK(result.payload.i64 == 9);
    {
        MirModule mir;
        char mir_err[256] = {0};
        memset(&mir, 0, sizeof(mir));
        CHECK(mir_lower_module(&module, &mir, mir_err, sizeof(mir_err)));
        CHECK(mir_verify(&mir, mir_err, sizeof(mir_err)));
        char *assembly = NULL;
        size_t assembly_len = 0;
        FILE *out = open_memstream(&assembly, &assembly_len);
        CHECK(out != NULL);
        CHECK(bir_x86_64_emit(&mir, out, mir_err, sizeof(mir_err)));
        fclose(out);
        CHECK(assembly && strstr(assembly, "movq") != NULL);
        free(assembly);
        MirAllocation allocation;
        mir_allocation_init(&allocation, &mir);
        CHECK(mir_allocate(&mir, &allocation, mir_err, sizeof(mir_err)));
        assembly = NULL;
        assembly_len = 0;
        out = open_memstream(&assembly, &assembly_len);
        CHECK(out != NULL);
        CHECK(bir_x86_64_emit_allocated(&mir, &allocation, out,
                                        mir_err, sizeof(mir_err)));
        fclose(out);
        CHECK(assembly && strstr(assembly, "movq") != NULL);
        free(assembly);
        mir_allocation_free(&allocation);
        mir_module_free(&mir);
    }
    bir_module_free(&module);

    /* A valid explicit destroy is accepted by both native emitters. Stack
       allocations need no libc release, but the ownership opcode must remain
       part of the verified MIR contract. */
    bir_module_init(&module, "<unit:native-destroy>");
    CHECK(bir_declare_region(&module, 1, 0));
    arena = &module.arena;
    pointer_type = bir_pointer_type(&module, module.type_i64);
    entry = bir_add_entry_block(arena, "destroy", 1, 1);
    enter = bir_add_region_enter(arena, 1, 0, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, enter));
    slot = bir_add_region_stack_slot(arena, pointer_type, module.type_i64,
                                     0, 8, 0, 1, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, slot));
    pointer = bir_inst_result(arena, slot, 1, 1);
    SsaInstRef native_destroy = bir_add_destroy(arena, pointer, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, native_destroy));
    exit = bir_add_region_exit(arena, 1, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, exit));
    value = bir_add_const(arena, bir_scalar_i64(module.type_i64, 0), 1, 1);
    CHECK(bir_set_return(arena, entry, value, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));
    memset(err, 0, sizeof(err));
    CHECK(bir_verify(&module, err, sizeof(err)));
    {
        MirModule mir;
        char mir_err[256] = {0};
        memset(&mir, 0, sizeof(mir));
        CHECK(mir_lower_module(&module, &mir, mir_err, sizeof(mir_err)));
        char *assembly = NULL;
        size_t assembly_len = 0;
        FILE *out = open_memstream(&assembly, &assembly_len);
        CHECK(out != NULL);
        CHECK(bir_x86_64_emit(&mir, out, mir_err, sizeof(mir_err)));
        fclose(out);
        CHECK(assembly && strstr(assembly, "leave") != NULL);
        free(assembly);
        mir_module_free(&mir);
    }
    bir_module_free(&module);

    /* Destruction is linear even when two SSA values alias the same slot. */
    bir_module_init(&module, "<unit:double-destroy>");
    arena = &module.arena;
    pointer_type = bir_pointer_type(&module, module.type_i64);
    entry = bir_add_entry_block(arena, "destroy", 1, 1);
    slot = bir_add_stack_slot(arena, pointer_type, module.type_i64, 0, 8, 0, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, slot));
    pointer = bir_inst_result(arena, slot, 1, 1);
    SsaInstRef destroy = bir_add_destroy(arena, pointer, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, destroy));
    destroy = bir_add_destroy(arena, pointer, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, destroy));
    value = bir_add_const(arena, bir_scalar_i64(module.type_i64, 0), 1, 1);
    CHECK(bir_set_return(arena, entry, value, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));
    memset(err, 0, sizeof(err));
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "double") != NULL || strstr(err, "destruction") != NULL);
    bir_module_free(&module);
}

static void test_writable_view_borrow_conflicts(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:writable-view>");
    const CobraType *pointer_type = bir_pointer_type(&module, module.type_i64);
    const CobraType *view_type = bir_writable_view_type(&module, module.type_i64);
    CHECK(pointer_type != NULL && view_type != NULL && bir_view_is_writable(view_type));
    SsaArena *arena = &module.arena;
    SsaBlockRef entry = bir_add_entry_block(arena, "write_view", 1, 1);
    SsaInstRef slot = bir_add_stack_slot(arena, pointer_type, module.type_i64,
                                         0, 8, 0, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, slot));
    SsaValueRef pointer = bir_inst_result(arena, slot, 1, 1);
    SsaValueRef initial = bir_add_const(arena, bir_scalar_i64(module.type_i64, 0), 1, 1);
    SsaInstRef store = bir_add_typed_store(arena, module.type_i64, pointer_type,
                                           pointer, initial, 8, 8, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, store));
    SsaValueRef length = bir_add_const(arena, bir_scalar_i64(module.type_i64, 1), 1, 1);
    SsaInstRef make = bir_add_view_make(arena, view_type, module.type_i64,
                                        pointer, length, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, make));
    SsaValueRef    view = bir_inst_result(arena, make, 1, 1);
    SsaInstRef view_ptr = bir_add_view_ptr(arena, pointer_type, module.type_i64,
                                           view, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, view_ptr));
    SsaValueRef writable_pointer = bir_inst_result(arena, view_ptr, 1, 1);
    SsaValueRef replacement = bir_add_const(arena, bir_scalar_i64(module.type_i64, 7), 1, 1);
    store = bir_add_typed_store(arena, module.type_i64, pointer_type,
                                writable_pointer, replacement, 8, 8, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, store));
    SsaInstRef load = bir_add_typed_load(arena, module.type_i64, pointer_type,
                                         writable_pointer, 8, 8, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, load));
    SsaValueRef loaded = bir_inst_result(arena, load, 1, 1);
    CHECK(bir_set_return(arena, entry, loaded, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));
    char err[512] = {0};
    CHECK(bir_verify(&module, err, sizeof(err)));
    BirScalarValue result = {0};
    CHECK(bir_eval_function_value(&module, "main", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 7);
    bir_module_free(&module);

    /* A second mutable view of the same allocation is conservatively rejected. */
    bir_module_init(&module, "<unit:writable-view-double-borrow>");
    pointer_type = bir_pointer_type(&module, module.type_i64);
    view_type = bir_writable_view_type(&module, module.type_i64);
    arena = &module.arena;
    entry = bir_add_entry_block(arena, "double_writer", 1, 1);
    slot = bir_add_stack_slot(arena, pointer_type, module.type_i64, 0, 8, 0, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, slot));
    pointer = bir_inst_result(arena, slot, 1, 1);
    length = bir_add_const(arena, bir_scalar_i64(module.type_i64, 1), 1, 1);
    make = bir_add_view_make(arena, view_type, module.type_i64, pointer, length, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, make));
    view = bir_inst_result(arena, make, 1, 1);
    make = bir_add_view_make(arena, view_type, module.type_i64, pointer, length, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, make));
    view = bir_inst_result(arena, make, 1, 1);
    initial = bir_add_const(arena, bir_scalar_i64(module.type_i64, 0), 1, 1);
    CHECK(bir_set_return(arena, entry, initial, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));
    memset(err, 0, sizeof(err));
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "active borrow") != NULL || strstr(err, "borrow") != NULL);
    bir_module_free(&module);

    /* An owner cannot write while a readonly alias remains live. */
    bir_module_init(&module, "<unit:writable-view-reader-conflict>");
    pointer_type = bir_pointer_type(&module, module.type_i64);
    view_type = bir_view_type(&module, module.type_i64);
    arena = &module.arena;
    entry = bir_add_entry_block(arena, "reader_conflict", 1, 1);
    slot = bir_add_stack_slot(arena, pointer_type, module.type_i64, 0, 8, 0, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, slot));
    pointer = bir_inst_result(arena, slot, 1, 1);
    length = bir_add_const(arena, bir_scalar_i64(module.type_i64, 1), 1, 1);
    make = bir_add_view_make(arena, view_type, module.type_i64, pointer, length, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, make));
    view = bir_inst_result(arena, make, 1, 1);
    initial = bir_add_const(arena, bir_scalar_i64(module.type_i64, 3), 1, 1);
    store = bir_add_typed_store(arena, module.type_i64, pointer_type,
                                pointer, initial, 8, 8, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, store));
    CHECK(bir_set_return(arena, entry, initial, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));
    memset(err, 0, sizeof(err));
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "active") != NULL || strstr(err, "borrow") != NULL);
    bir_module_free(&module);
}

static void test_returned_view_escape_analysis(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:return-view-escape>");
    const CobraType *view_type = bir_view_type(&module, module.type_i64);
    const CobraType *pointer_type = bir_pointer_type(&module, module.type_i64);
    const CobraType *params[1] = {view_type};
    char err[512] = {0};

    /* Returning the caller's view directly is the valid escape case. */
    CHECK(bir_declare_function(&module, "identity_view", 1, params,
                               view_type, true));
    SsaArena *arena = &module.arena;
    SsaBlockRef entry = bir_add_entry_block(arena, "identity_view", 1, 1);
    SsaValueRef parameter = bir_add_value(arena, SSA_VALUE_PARAM, view_type, 1, 1);
    arena->values[parameter].param_index = 0;
    CHECK(bir_set_return(arena, entry, parameter, 1, 1));
    SsaValueRef refs[1] = {parameter};
    CHECK(bir_register_function_info(&module, "identity_view", entry, 1,
                                     refs, view_type, true));
    CHECK(bir_verify(&module, err, sizeof(err)));
    bir_module_free(&module);

    /* A view backed by this function's frame cannot be returned. */
    bir_module_init(&module, "<unit:return-view-frame-escape>");
    view_type = bir_view_type(&module, module.type_i64);
    pointer_type = bir_pointer_type(&module, module.type_i64);
    params[0] = view_type;
    CHECK(bir_declare_function(&module, "frame_escape", 1, params,
                               view_type, true));
    arena = &module.arena;
    entry = bir_add_entry_block(arena, "frame_escape", 1, 1);
    parameter = bir_add_value(arena, SSA_VALUE_PARAM, view_type, 1, 1);
    arena->values[parameter].param_index = 0;
    SsaInstRef slot = bir_add_stack_slot(arena, pointer_type, module.type_i64,
                                         0, 8, 0, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, slot));
    SsaValueRef local_pointer = bir_inst_result(arena, slot, 1, 1);
    SsaValueRef length = bir_add_const(arena,
        bir_scalar_i64(module.type_i64, 1), 1, 1);
    SsaInstRef make = bir_add_view_make(arena, view_type, module.type_i64,
                                        local_pointer, length, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, make));
    SsaValueRef local_view = bir_inst_result(arena, make, 1, 1);
    CHECK(bir_set_return(arena, entry, local_view, 1, 1));
    refs[0] = parameter;
    CHECK(bir_register_function_info(&module, "frame_escape", entry, 1,
                                     refs, view_type, true));
    memset(err, 0, sizeof(err));
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "escape") != NULL || strstr(err, "frame") != NULL ||
          strstr(err, "region") != NULL);
    bir_module_free(&module);

    /* A view backed by a callee-owned region cannot be returned either. */
    bir_module_init(&module, "<unit:return-view-region-escape>");
    CHECK(bir_declare_region(&module, 1, 0));
    view_type = bir_view_type(&module, module.type_i64);
    pointer_type = bir_pointer_type(&module, module.type_i64);
    params[0] = view_type;
    CHECK(bir_declare_function(&module, "region_escape", 1, params,
                               view_type, true));
    arena = &module.arena;
    entry = bir_add_entry_block(arena, "region_escape", 1, 1);
    parameter = bir_add_value(arena, SSA_VALUE_PARAM, view_type, 1, 1);
    arena->values[parameter].param_index = 0;
    SsaInstRef enter = bir_add_region_enter(arena, 1, 0, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, enter));
    slot = bir_add_region_stack_slot(arena, pointer_type, module.type_i64,
                                     0, 8, 0, 1, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, slot));
    local_pointer = bir_inst_result(arena, slot, 1, 1);
    length = bir_add_const(arena, bir_scalar_i64(module.type_i64, 1), 1, 1);
    make = bir_add_view_make(arena, view_type, module.type_i64,
                             local_pointer, length, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, make));
    local_view = bir_inst_result(arena, make, 1, 1);
    CHECK(bir_set_return(arena, entry, local_view, 1, 1));
    refs[0] = parameter;
    CHECK(bir_register_function_info(&module, "region_escape", entry, 1,
                                     refs, view_type, true));
    memset(err, 0, sizeof(err));
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "escape") != NULL || strstr(err, "region") != NULL ||
          strstr(err, "frame") != NULL);
    bir_module_free(&module);
}

static void test_owned_slices(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:owned-slice>");
    const CobraType *owned = bir_owned_slice_type(&module, module.type_i64);
    const CobraType *pointer_type = bir_pointer_type(&module, module.type_i64);
    CHECK(owned != NULL && bir_is_owned_slice_type(owned) &&
          owned->ownership == COBRA_OWNERSHIP_OWNED &&
          !bir_view_is_writable(owned));
    SsaArena *arena = &module.arena;
    SsaBlockRef entry = bir_add_entry_block(arena, "owned", 1, 1);
    SsaValueRef length = bir_add_const(arena, bir_scalar_i64(module.type_i64, 2), 1, 1);
    SsaInstRef alloc = bir_add_slice_alloc(arena, owned, module.type_i64,
                                           length, 1, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, alloc));
    SsaValueRef slice = bir_inst_result(arena, alloc, 1, 1);
    SsaInstRef view_ptr = bir_add_view_ptr(arena, pointer_type, module.type_i64,
                                           slice, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, view_ptr));
    SsaValueRef base = bir_inst_result(arena, view_ptr, 1, 1);
    SsaValueRef one = bir_add_const(arena, bir_scalar_i64(module.type_i64, 1), 1, 1);
    SsaValueRef two = bir_add_const(arena, bir_scalar_i64(module.type_i64, 2), 1, 1);
    SsaValueRef byte_offset = bir_add_const(arena,
                                            bir_scalar_i64(module.type_i64, 8), 1, 1);
    SsaInstRef store = bir_add_typed_store(arena, module.type_i64, pointer_type,
                                           base, one, 8, 8, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, store));
    SsaInstRef offset_inst = bir_add_ptr_add(arena, pointer_type, base, byte_offset, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, offset_inst));
    SsaValueRef offset = bir_inst_result(arena, offset_inst, 1, 1);
    SsaInstRef store2 = bir_add_typed_store(arena, module.type_i64, pointer_type,
                                            offset, two, 8, 8, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, store2));
    SsaInstRef load = bir_add_typed_load(arena, module.type_i64, pointer_type,
                                         offset, 8, 8, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, load));
    SsaValueRef loaded = bir_inst_result(arena, load, 1, 1);
    SsaInstRef len = bir_add_view_len(arena, module.type_i64, owned, slice, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, len));
    SsaValueRef slice_len = bir_inst_result(arena, len, 1, 1);
    SsaValueRef sum_ops[2] = {loaded, slice_len};
    SsaInstRef sum = bir_add_inst(arena, SSA_OP_ADD, module.type_i64,
                                  sum_ops, 2, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, sum));
    SsaValueRef result = bir_inst_result(arena, sum, 1, 1);
    SsaInstRef free = bir_add_slice_free(arena, slice, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, free));
    CHECK(bir_set_return(arena, entry, result, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));
    char err[512] = {0};
    CHECK(bir_verify(&module, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "owned slice verify: %s\n", err);
    BirScalarValue evaluated = {0};
    CHECK(bir_eval_function_value(&module, "main", &evaluated));
    CHECK(evaluated.kind == BIR_SCALAR_I64 && evaluated.payload.i64 == 4);
    bir_module_free(&module);

    /* Use after free is rejected. */
    bir_module_init(&module, "<unit:owned-slice-uaf>");
    owned = bir_owned_slice_type(&module, module.type_i64);
    pointer_type = bir_pointer_type(&module, module.type_i64);
    arena = &module.arena;
    entry = bir_add_entry_block(arena, "uaf", 1, 1);
    length = bir_add_const(arena, bir_scalar_i64(module.type_i64, 1), 1, 1);
    alloc = bir_add_slice_alloc(arena, owned, module.type_i64, length, 1, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, alloc));
    slice = bir_inst_result(arena, alloc, 1, 1);
    free = bir_add_slice_free(arena, slice, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, free));
    view_ptr = bir_add_view_ptr(arena, pointer_type, module.type_i64, slice, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, view_ptr));
    SsaValueRef dead_ptr = bir_inst_result(arena, view_ptr, 1, 1);
    load = bir_add_typed_load(arena, module.type_i64, pointer_type, dead_ptr, 8, 8, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, load));
    result = bir_inst_result(arena, load, 1, 1);
    CHECK(bir_set_return(arena, entry, result, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));
    memset(err, 0, sizeof(err));
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "destroyed") != NULL || strstr(err, "destruction") != NULL);
    bir_module_free(&module);

    /* Double free is rejected. */
    bir_module_init(&module, "<unit:owned-slice-double-free>");
    owned = bir_owned_slice_type(&module, module.type_i64);
    arena = &module.arena;
    entry = bir_add_entry_block(arena, "double_free", 1, 1);
    length = bir_add_const(arena, bir_scalar_i64(module.type_i64, 1), 1, 1);
    alloc = bir_add_slice_alloc(arena, owned, module.type_i64, length, 1, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, alloc));
    slice = bir_inst_result(arena, alloc, 1, 1);
    free = bir_add_slice_free(arena, slice, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, free));
    free = bir_add_slice_free(arena, slice, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, free));
    result = bir_add_const(arena, bir_scalar_i64(module.type_i64, 0), 1, 1);
    CHECK(bir_set_return(arena, entry, result, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));
    memset(err, 0, sizeof(err));
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "double") != NULL || strstr(err, "free") != NULL ||
          strstr(err, "destruction") != NULL);
    bir_module_free(&module);

    /* An owner cannot free while a borrowed view is active. */
    bir_module_init(&module, "<unit:owned-slice-borrow>");
    owned = bir_owned_slice_type(&module, module.type_i64);
    const CobraType *borrowed = bir_view_type(&module, module.type_i64);
    pointer_type = bir_pointer_type(&module, module.type_i64);
    arena = &module.arena;
    entry = bir_add_entry_block(arena, "borrow", 1, 1);
    length = bir_add_const(arena, bir_scalar_i64(module.type_i64, 1), 1, 1);
    alloc = bir_add_slice_alloc(arena, owned, module.type_i64, length, 1, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, alloc));
    slice = bir_inst_result(arena, alloc, 1, 1);
    view_ptr = bir_add_view_ptr(arena, pointer_type, module.type_i64, slice, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, view_ptr));
    SsaValueRef owned_ptr = bir_inst_result(arena, view_ptr, 1, 1);
    SsaInstRef make = bir_add_view_make(arena, borrowed, module.type_i64,
                                        owned_ptr, length, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, make));
    SsaValueRef borrowed_view = bir_inst_result(arena, make, 1, 1);
    CHECK(borrowed_view != SSA_VALUE_NONE);
    free = bir_add_slice_free(arena, slice, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, free));
    result = bir_add_const(arena, bir_scalar_i64(module.type_i64, 0), 1, 1);
    CHECK(bir_set_return(arena, entry, result, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));
    memset(err, 0, sizeof(err));
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "borrow") != NULL || strstr(err, "active") != NULL);
    bir_module_free(&module);
}

static void test_source_readonly_slice_lowering(void) {
    ASTNode *program = parse_program(
        "def first(values: readonly []f32) -> f32: {\n"
        "  let alias: readonly []f32 = values\n"
        "  return alias[1]\n"
        "}\n"
        "def first_byte(values: readonly []u8) -> u8: {\n"
        "  let prefix: readonly []u8 = slice_u8(values, 0, 1)\n"
        "  return prefix[0]\n"
        "}\n"
        "def view_length(values: readonly []i64) -> i64: {\n"
        "  let alias: readonly []i64 = values\n"
        "  return len(alias)\n"
        "}\n"
        "def identity_view(values: readonly []f32) -> readonly []f32: {\n"
        "  return values\n"
        "}\n"
        "def prefix_view(values: readonly []u8) -> readonly []u8: {\n"
        "  return slice_u8(values, 0, 1)\n"
        "}\n"
        "def returned_length(values: readonly []f32) -> i64: {\n"
        "  return len(identity_view(values))\n"
        "}\n"
        "def write_first(values: out []f32) -> f32: {\n"
        "  values[0] = 2.5\n"
        "  return values[0]\n"
        "}\n");
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule module;
    bir_module_init(&module, "<unit:source-readonly-view>");
    char err[512] = {0};
    CHECK(bir_build_program(&module, program));
    CHECK(bir_verify(&module, err, sizeof(err)));
    CHECK(err[0] == '\0');
    char *dump = NULL;
    size_t dump_len = 0;
    FILE *dump_file = open_memstream(&dump, &dump_len);
    CHECK(dump_file != NULL);
    bir_dump(&module, dump_file);
    fclose(dump_file);
    CHECK(dump && strstr(dump, "view_make") != NULL);
    CHECK(dump && strstr(dump, "view_ptr") != NULL);
    CHECK(dump && strstr(dump, "ptr_add") != NULL);
    CHECK(dump && strstr(dump, "load(v") != NULL);
    CHECK(dump && strstr(dump, "view_len") != NULL);
    CHECK(dump && strstr(dump, "store(v") != NULL);
    free(dump);
    bir_module_free(&module);
    ast_free(program);
}

static void test_source_readonly_slice_rejections(void) {
    ASTNode *mutation = parse_program(
        "def bad(values: readonly []f32) -> i64: {\n"
        "  values[0] = 1.0\n"
        "  return 0\n"
        "}\n");
    CHECK(mutation != NULL);
    if (mutation) {
        BackendIrModule module;
        bir_module_init(&module, "<unit:readonly-view-mutation>");
        CHECK(!bir_build_program(&module, mutation));
        CHECK(strstr(module.error, "outside the backend-IR subset") != NULL ||
              strstr(module.error, "readonly") != NULL);
        bir_module_free(&module);
        ast_free(mutation);
    }

    ASTNode *escape = parse_program(
        "def escape(values: readonly []f32) -> []f32: {\n"
        "  return values\n"
        "}\n");
    CHECK(escape != NULL);
    if (escape) {
        BackendIrModule module;
        bir_module_init(&module, "<unit:readonly-view-return>");
        CHECK(!bir_build_program(&module, escape));
        CHECK(strstr(module.error, "return type") != NULL ||
              strstr(module.error, "outside") != NULL ||
              strstr(module.error, "slice") != NULL ||
              strstr(module.error, "wrong type") != NULL);
        bir_module_free(&module);
        ast_free(escape);
    }
}

/* ------------------------------------------------------------------ */
/* Source-level owned slices and regions (full pipeline)               */
/* ------------------------------------------------------------------ */

static void test_source_owned_slices(void) {
    /* alloc, indexed writes and reads, len, free through the full
       source -> types -> HIR -> SSA -> verifier -> evaluator pipeline. */
    unit_expected("owned slice write/read/len/free",
        "def main() -> i64: {\n"
        "  s = alloc_i64(3)\n"
        "  s[0] = 7\n"
        "  s[1] = 8\n"
        "  s[2] = s[0] + s[1]\n"
        "  r = len(s) + s[0] + s[1] + s[2]\n"
        "  free(s)\n"
        "  return r\n"
        "}\n", 33);

    /* An owned slice satisfies a readonly view parameter at the call
       boundary; the call borrows for its duration and the owner can free
       afterwards. */
    unit_expected("owned slice borrow to readonly view parameter",
        "def sum(values: readonly []i64) -> i64: {\n"
        "  return values[0] + values[1]\n"
        "}\n"
        "def main() -> i64: {\n"
        "  s = alloc_i64(2)\n"
        "  s[0] = 3\n"
        "  s[1] = 4\n"
        "  r = sum(s)\n"
        "  free(s)\n"
        "  return r\n"
        "}\n", 7);

    /* An owned slice can also satisfy a writable out parameter. */
    unit_expected("owned slice borrow to writable out parameter",
        "def bump(values: out []i64) -> i64: {\n"
        "  values[0] = values[0] + 100\n"
        "  return values[0]\n"
        "}\n"
        "def main() -> i64: {\n"
        "  s = alloc_i64(1)\n"
        "  s[0] = 5\n"
        "  r = bump(s)\n"
        "  free(s)\n"
        "  return r\n"
        "}\n", 105);

    /* An owned slice aliased into a readonly view local borrows the backing
       allocation, so no explicit free is issued afterwards. */
    unit_expected("owned slice aliased as readonly view",
        "def main() -> i64: {\n"
        "  s = alloc_i64(2)\n"
        "  s[0] = 3\n"
        "  s[1] = 4\n"
        "  let v: readonly []i64 = s\n"
        "  return v[0] + v[1]\n"
        "}\n", 7);

    /* slice_u8 subview of an owned u8 buffer yields a readonly view. */
    {
        ASTNode *program = parse_program(
            "def main() -> u8: {\n"
            "  buf = alloc_u8(4)\n"
            "  buf[0] = 10\n"
            "  buf[1] = 20\n"
            "  let v: readonly []u8 = slice_u8(buf, 0, 2)\n"
            "  return v[1]\n"
            "}\n");
        CHECK(program != NULL);
        if (program) {
            BirScalarValue result = {0};
            char err[512] = {0};
            CHECK(pipeline_run(program, "main", &result, err, sizeof(err)));
            CHECK(result.kind == BIR_SCALAR_U8 && result.payload.i64 == 20);
            ast_free(program);
        }
    }

    /* Dump inspection: slice_alloc and slice_free are emitted. */
    {
        ASTNode *program = parse_program(
            "def main() -> i64: {\n"
            "  s = alloc_i64(1)\n"
            "  s[0] = 1\n"
            "  free(s)\n"
            "  return 0\n"
            "}\n");
        CHECK(program != NULL);
        if (program) {
            BackendIrModule module;
            bir_module_init(&module, "<unit:owned-slice-dump>");
            char err[512] = {0};
            CHECK(bir_build_program(&module, program));
            CHECK(bir_verify(&module, err, sizeof(err)));
            CHECK(err[0] == '\0');
            char *dump = NULL;
            size_t dump_len = 0;
            FILE *dump_file = open_memstream(&dump, &dump_len);
            CHECK(dump_file != NULL);
            bir_dump(&module, dump_file);
            fclose(dump_file);
            CHECK(dump && strstr(dump, "slice_alloc") != NULL);
            CHECK(dump && strstr(dump, "slice_free") != NULL);
            free(dump);
            bir_module_free(&module);
            ast_free(program);
        }
    }
}

static void test_source_region_lowering(void) {
    /* with region blocks and region-qualified allocations: the allocation
       dies at region exit without an explicit free. */
    unit_expected("with region qualified allocation",
        "def main() -> i64: {\n"
        "  with region scratch: {\n"
        "    a = scratch.alloc_i64(2)\n"
        "    a[0] = 5\n"
        "    a[1] = 6\n"
        "    r = a[0] + a[1]\n"
        "  }\n"
        "  return r\n"
        "}\n", 11);

    unit_expected("nested with region blocks",
        "def main() -> i64: {\n"
        "  with region outer: {\n"
        "    o = outer.alloc_i64(1)\n"
        "    o[0] = 3\n"
        "    with region inner: {\n"
        "      i = inner.alloc_i64(1)\n"
        "      i[0] = 4\n"
        "      r = o[0] + i[0]\n"
        "    }\n"
        "  }\n"
        "  return r\n"
        "}\n", 7);

    /* Dump inspection: region_enter and region_exit frame the allocation. */
    {
        ASTNode *program = parse_program(
            "def main() -> i64: {\n"
            "  with region scratch: {\n"
            "    a = scratch.alloc_i64(1)\n"
            "    a[0] = 1\n"
            "  }\n"
            "  return 0\n"
            "}\n");
        CHECK(program != NULL);
        if (program) {
            BackendIrModule module;
            bir_module_init(&module, "<unit:region-dump>");
            char err[512] = {0};
            CHECK(bir_build_program(&module, program));
            CHECK(bir_verify(&module, err, sizeof(err)));
            CHECK(err[0] == '\0');
            char *dump = NULL;
            size_t dump_len = 0;
            FILE *dump_file = open_memstream(&dump, &dump_len);
            CHECK(dump_file != NULL);
            bir_dump(&module, dump_file);
            fclose(dump_file);
            CHECK(dump && strstr(dump, "region_enter") != NULL);
            CHECK(dump && strstr(dump, "region_exit") != NULL);
            CHECK(dump && strstr(dump, "slice_alloc") != NULL);
            free(dump);
            bir_module_free(&module);
            ast_free(program);
        }
    }
}

static void test_source_owned_slice_rejections(void) {
    BirScalarValue result = {0};
    char err[512] = {0};

    /* Double free. */
    {
        ASTNode *program = parse_program(
            "def main() -> i64: {\n"
            "  s = alloc_i64(1)\n"
            "  free(s)\n"
            "  free(s)\n"
            "  return 0\n"
            "}\n");
        CHECK(program != NULL);
        if (program) {
            memset(err, 0, sizeof(err));
            CHECK(!pipeline_run(program, "main", &result, err, sizeof(err)));
            CHECK(strstr(err, "destruction") != NULL ||
                  strstr(err, "double") != NULL || strstr(err, "free") != NULL);
            ast_free(program);
        }
    }

    /* Use after free. */
    {
        ASTNode *program = parse_program(
            "def main() -> i64: {\n"
            "  s = alloc_i64(2)\n"
            "  free(s)\n"
            "  return s[0]\n"
            "}\n");
        CHECK(program != NULL);
        if (program) {
            memset(err, 0, sizeof(err));
            CHECK(!pipeline_run(program, "main", &result, err, sizeof(err)));
            CHECK(strstr(err, "destruction") != NULL ||
                  strstr(err, "destroyed") != NULL);
            ast_free(program);
        }
    }

    /* free of a borrowed readonly view local. */
    {
        ASTNode *program = parse_program(
            "def main() -> i64: {\n"
            "  buf = alloc_i64(2)\n"
            "  let v: readonly []i64 = buf\n"
            "  free(v)\n"
            "  return 0\n"
            "}\n");
        CHECK(program != NULL);
        if (program) {
            memset(err, 0, sizeof(err));
            CHECK(!pipeline_run(program, "main", &result, err, sizeof(err)));
            CHECK(strstr(err, "free requires an owned slice") != NULL ||
                  strstr(err, "owned slice or string") != NULL);
            ast_free(program);
        }
    }

    /* free of a region-backed slice. */
    {
        ASTNode *program = parse_program(
            "def main() -> i64: {\n"
            "  with region scratch: {\n"
            "    a = scratch.alloc_i64(1)\n"
            "    free(a)\n"
            "  }\n"
            "  return 0\n"
            "}\n");
        CHECK(program != NULL);
        if (program) {
            memset(err, 0, sizeof(err));
            CHECK(!pipeline_run(program, "main", &result, err, sizeof(err)));
            CHECK(strstr(err, "region-backed") != NULL);
            ast_free(program);
        }
    }

    /* A bare `[]i64` parameter is borrowed-mutable, matching the direct
       backend (which never frees a received parameter either): the callee
       can read and write through it but not free it, and the caller's own
       allocation stays live and usable across the call. */
    unit_expected("plain slice parameter is borrowed, not owned",
        "def bump_and_read(s: []i64) -> i64: {\n"
        "  s[0] = s[0] + 1\n"
        "  return s[0]\n"
        "}\n"
        "def main() -> i64: {\n"
        "  s = alloc_i64(1)\n"
        "  s[0] = 40\n"
        "  r = bump_and_read(s)\n"
        "  r = r + s[0]\n"
        "  free(s)\n"
        "  return r\n"
        "}\n", 82);

    /* Freeing a plain `[]i64` parameter inside the callee is rejected: it
       is a borrowed view of the caller's allocation, not an owned value the
       callee received transfer of. */
    {
        ASTNode *program = parse_program(
            "def take(s: []i64) -> i64: {\n"
            "  value = s[0]\n"
            "  free(s)\n"
            "  return value\n"
            "}\n"
            "def main() -> i64: {\n"
            "  s = alloc_i64(1)\n"
            "  s[0] = 41\n"
            "  return take(s) + 1\n"
            "}\n");
        CHECK(program != NULL);
        if (program) {
            BackendIrModule module;
            CHECK(!bir_build_program(&module, program));
            bir_module_free(&module);
            ast_free(program);
        }
    }

    /* A callee-created owned slice transfers into caller storage on return. */
    unit_expected("owned slice return transfer",
        "def make() -> []i64: {\n"
        "  s = alloc_i64(1)\n"
        "  s[0] = 40\n"
        "  return s\n"
        "}\n"
        "def main() -> i64: {\n"
        "  s = make()\n"
        "  value = s[0]\n"
        "  free(s)\n"
        "  return value + 2\n"
        "}\n", 42);

    /* A bare `[]i64` parameter is a borrowed view internally, so returning
       it unchanged as an owned `[]i64` result is a type mismatch (view vs.
       owned) - matching the direct backend, which also has no notion of
       moving a received parameter back out as an owned return. */
    {
        ASTNode *program = parse_program(
            "def identity(s: []i64) -> []i64: {\n"
            "  return s\n"
            "}\n"
            "def main() -> i64: {\n"
            "  s = alloc_i64(1)\n"
            "  s[0] = 40\n"
            "  out = identity(s)\n"
            "  value = out[0]\n"
            "  free(out)\n"
            "  return value + 2\n"
            "}\n");
        CHECK(program != NULL);
        if (program) {
            BackendIrModule module;
            CHECK(!bir_build_program(&module, program));
            bir_module_free(&module);
            ast_free(program);
        }
    }

    /* A bare `[]i64` parameter is a borrowed view, so freeing it inside the
       callee is rejected at build time regardless of whether the caller
       reads the allocation afterward. */
    {
        ASTNode *program = parse_program(
            "def take(s: []i64) -> i64: {\n"
            "  free(s)\n"
            "  return 0\n"
            "}\n"
            "def main() -> i64: {\n"
            "  s = alloc_i64(1)\n"
            "  take(s)\n"
            "  return s[0]\n"
            "}\n");
        CHECK(program != NULL);
        if (program) {
            BackendIrModule module;
            CHECK(!bir_build_program(&module, program));
            bir_module_free(&module);
            ast_free(program);
        }
    }

    /* free while a readonly alias is live. */
    {
        ASTNode *program = parse_program(
            "def main() -> i64: {\n"
            "  s = alloc_i64(2)\n"
            "  let v: readonly []i64 = s\n"
            "  free(s)\n"
            "  return 0\n"
            "}\n");
        CHECK(program != NULL);
        if (program) {
            memset(err, 0, sizeof(err));
            CHECK(!pipeline_run(program, "main", &result, err, sizeof(err)));
            CHECK(strstr(err, "borrowed view is active") != NULL ||
                  strstr(err, "borrow") != NULL);
            ast_free(program);
        }
    }

    /* Writing through the owner while a readonly alias is live. */
    {
        ASTNode *program = parse_program(
            "def main() -> i64: {\n"
            "  s = alloc_i64(2)\n"
            "  let v: readonly []i64 = s\n"
            "  s[0] = 5\n"
            "  return 0\n"
            "}\n");
        CHECK(program != NULL);
        if (program) {
            memset(err, 0, sizeof(err));
            CHECK(!pipeline_run(program, "main", &result, err, sizeof(err)));
            CHECK(strstr(err, "borrowed view is active") != NULL ||
                  strstr(err, "borrow") != NULL);
            ast_free(program);
        }
    }

    /* return inside a with region body. */
    {
        ASTNode *program = parse_program(
            "def main() -> i64: {\n"
            "  with region scratch: {\n"
            "    a = scratch.alloc_i64(1)\n"
            "    return 5\n"
            "  }\n"
            "  return 0\n"
            "}\n");
        CHECK(program != NULL);
        if (program) {
            memset(err, 0, sizeof(err));
            CHECK(!pipeline_run(program, "main", &result, err, sizeof(err)));
            CHECK(strstr(err, "return inside a with region body") != NULL);
            ast_free(program);
        }
    }

    /* Region-qualified allocation outside any active region. */
    {
        ASTNode *program = parse_program(
            "def main() -> i64: {\n"
            "  a = scratch.alloc_i64(1)\n"
            "  return 0\n"
            "}\n");
        CHECK(program != NULL);
        if (program) {
            memset(err, 0, sizeof(err));
            CHECK(!pipeline_run(program, "main", &result, err, sizeof(err)));
            CHECK(strstr(err, "unknown or inactive region") != NULL);
            ast_free(program);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Source-level scalar sums (Option/Result)                           */
/* ------------------------------------------------------------------ */

/* Rejection helper for the full pipeline: the program must be rejected
   somewhere in build, verify, or eval. */
static void unit_rejected(const char *name, const char *source) {
    ASTNode *program = parse_program(source);
    CHECK(program != NULL);
    if (!program) return;
    BirScalarValue result = {0};
    char err[512] = {0};
    bool ok = pipeline_run(program, "main", &result, err, sizeof(err));
    checks++;
    if (ok) {
        failures++;
        fprintf(stderr, "FAIL %s: expected rejection but the pipeline accepted it\n", name);
    }
    ast_free(program);
}

static void test_source_sums(void) {
    /* Construction, predicate, and extraction through the full source ->
       types -> HIR -> SSA -> verifier -> evaluator pipeline. */
    unit_expected("some/unwrap round trip",
        "def main() -> i64: {\n"
        "  let x: Option[i64] = some(7)\n"
        "  if is_some(x): {} else: { return 1 }\n"
        "  let v: i64 = unwrap(x)\n"
        "  return v - 7\n"
        "}\n", 0);

    unit_expected("none is not some",
        "def main() -> i64: {\n"
        "  let x: Option[i64] = none\n"
        "  if is_some(x): { return 1 }\n"
        "  return 0\n"
        "}\n", 0);

    unit_expected("ok/unwrap_ok round trip",
        "def main() -> i64: {\n"
        "  let r: Result[i64, i64] = ok(9)\n"
        "  if is_ok(r): {} else: { return 1 }\n"
        "  let v: i64 = unwrap_ok(r)\n"
        "  return v - 9\n"
        "}\n", 0);

    unit_expected("err/unwrap_err round trip",
        "def main() -> i64: {\n"
        "  let r: Result[i64, i64] = err(2)\n"
        "  if is_ok(r): { return 1 }\n"
        "  let e: i64 = unwrap_err(r)\n"
        "  return e - 2\n"
        "}\n", 0);

    unit_expected("sum parameter through copy",
        "def take(o: Option[i64]) -> i64: {\n"
        "  if is_some(o): {} else: { return 99 }\n"
        "  return unwrap(o)\n"
        "}\n"
        "def main() -> i64: {\n"
        "  let x: Option[i64] = some(5)\n"
        "  return take(x) - 5\n"
        "}\n", 0);

    unit_expected("sum return through caller storage",
        "def make() -> Option[i64]: {\n"
        "  return some(4)\n"
        "}\n"
        "def main() -> i64: {\n"
        "  let x: Option[i64] = make()\n"
        "  if is_some(x): {} else: { return 1 }\n"
        "  return unwrap(x) - 4\n"
        "}\n", 0);

    unit_expected("none return through caller storage",
        "def make() -> Option[i64]: {\n"
        "  return none\n"
        "}\n"
        "def main() -> i64: {\n"
        "  let x: Option[i64] = make()\n"
        "  if is_some(x): { return 1 }\n"
        "  return 0\n"
        "}\n", 0);

    unit_expected("sum local copy",
        "def main() -> i64: {\n"
        "  let x: Option[i64] = some(3)\n"
        "  let y: Option[i64] = x\n"
        "  if is_some(y): {} else: { return 1 }\n"
        "  return unwrap(y) - 3\n"
        "}\n", 0);

    unit_expected("inline constructor as call argument",
        "def take(o: Option[i64]) -> i64: {\n"
        "  if is_some(o): {} else: { return 99 }\n"
        "  return unwrap(o)\n"
        "}\n"
        "def main() -> i64: {\n"
        "  return take(some(11)) - 11\n"
        "}\n", 0);

    unit_expected("f32 payload",
        "def main() -> i64: {\n"
        "  let x: Option[f32] = some(1.5)\n"
        "  if is_some(x): {} else: { return 1 }\n"
        "  return 0\n"
        "}\n", 0);

    unit_expected("bool payload",
        "def main() -> i64: {\n"
        "  let x: Option[bool] = some(true)\n"
        "  if is_some(x): {} else: { return 1 }\n"
        "  return 0\n"
        "}\n", 0);

    unit_expected("result with bool error component",
        "def main() -> i64: {\n"
        "  let r: Result[i64, bool] = err(false)\n"
        "  if is_ok(r): { return 1 }\n"
        "  let e: bool = unwrap_err(r)\n"
        "  if e: { return 1 }\n"
        "  return 0\n"
        "}\n", 0);

    /* Rejection cases. */
    unit_rejected("unwrap on none fails at eval",
        "def main() -> i64: {\n"
        "  let x: Option[i64] = none\n"
        "  let v: i64 = unwrap(x)\n"
        "  return v\n"
        "}\n");

    unit_rejected("unwrap_ok on err fails at eval",
        "def main() -> i64: {\n"
        "  let r: Result[i64, i64] = err(2)\n"
        "  let v: i64 = unwrap_ok(r)\n"
        "  return v\n"
        "}\n");

    unit_rejected("unwrap_err on ok fails at eval",
        "def main() -> i64: {\n"
        "  let r: Result[i64, i64] = ok(2)\n"
        "  let e: i64 = unwrap_err(r)\n"
        "  return e\n"
        "}\n");

    unit_rejected("is_some on a Result",
        "def main() -> i64: {\n"
        "  let r: Result[i64, i64] = ok(2)\n"
        "  if is_some(r): { return 1 }\n"
        "  return 0\n"
        "}\n");

    unit_rejected("is_ok on an Option",
        "def main() -> i64: {\n"
        "  let x: Option[i64] = some(2)\n"
        "  if is_ok(x): { return 1 }\n"
        "  return 0\n"
        "}\n");

    unit_rejected("unwrap on a Result",
        "def main() -> i64: {\n"
        "  let r: Result[i64, i64] = ok(2)\n"
        "  let v: i64 = unwrap(r)\n"
        "  return v\n"
        "}\n");

    unit_rejected("unwrap_ok on an Option",
        "def main() -> i64: {\n"
        "  let x: Option[i64] = some(2)\n"
        "  let v: i64 = unwrap_ok(x)\n"
        "  return v\n"
        "}\n");

    unit_rejected("sum assignment type mismatch",
        "def main() -> i64: {\n"
        "  let x: Option[i64] = some(2)\n"
        "  let r: Result[i64, i64] = x\n"
        "  return 0\n"
        "}\n");

    unit_rejected("sum payload type mismatch",
        "def main() -> i64: {\n"
        "  let x: Option[i64] = some(true)\n"
        "  return 0\n"
        "}\n");

    unit_rejected("none without a declared type",
        "def main() -> i64: {\n"
        "  let x = none\n"
        "  return 0\n"
        "}\n");

    unit_rejected("nested sum payload",
        "def main() -> i64: {\n"
        "  let x: Option[i64] = some(some(2))\n"
        "  return 0\n"
        "}\n");
}

static void test_source_owning_sums(void) {
    /* An owning string payload is moved into the active Option variant,
       extracted without copying, and explicitly dropped after extraction. */
    unit_expected("Option[string] payload move and drop",
        "def main() -> i64: {\n"
        "  let value: Option[string] = some(concat(\"a\", \"bc\"))\n"
        "  let text: string = unwrap(value)\n"
        "  n = len(text)\n"
        "  string_free(text)\n"
        "  free(value)\n"
        "  return n\n"
        "}\n", 3);

    unit_expected("inactive owning Option drop",
        "def main() -> i64: {\n"
        "  let value: Option[string] = none\n"
        "  free(value)\n"
        "  return 17\n"
        "}\n", 17);

    unit_expected("owning Result error payload move",
        "def main() -> i64: {\n"
        "  let value: Result[string, i64] = err(7)\n"
        "  if is_ok(value): { return 1 }\n"
        "  let error: i64 = unwrap_err(value)\n"
        "  free(value)\n"
        "  return error + 1\n"
        "}\n", 8);

    unit_expected("owning Result string payload move",
        "def main() -> i64: {\n"
        "  let value: Result[string, i64] = ok(concat(\"r\", \"s\"))\n"
        "  let text: string = unwrap_ok(value)\n"
        "  n = len(text)\n"
        "  string_free(text)\n"
        "  free(value)\n"
        "  return n\n"
        "}\n", 2);

    unit_expected("owning sum parameter transfer",
        "def take(value: Option[string]) -> i64: {\n"
        "  let text: string = unwrap(value)\n"
        "  n = len(text)\n"
        "  string_free(text)\n"
        "  free(value)\n"
        "  return n\n"
        "}\n"
        "def main() -> i64: {\n"
        "  let value: Option[string] = some(concat(\"z\", \"!\"))\n"
        "  return take(value)\n"
        "}\n", 2);

    unit_expected("owning sum return transfer",
        "def make() -> Option[string]: {\n"
        "  return some(concat(\"o\", \"k\"))\n"
        "}\n"
        "def main() -> i64: {\n"
        "  let value: Option[string] = make()\n"
        "  let text: string = unwrap(value)\n"
        "  n = len(text)\n"
        "  string_free(text)\n"
        "  free(value)\n"
        "  return n\n"
        "}\n", 2);

    unit_rejected("owning sum payload double drop",
        "def main() -> i64: {\n"
        "  let value: Option[string] = some(concat(\"a\", \"b\"))\n"
        "  free(value)\n"
        "  free(value)\n"
        "  return 0\n"
        "}\n");

    unit_rejected("owning sum use after drop",
        "def main() -> i64: {\n"
        "  let value: Option[string] = some(concat(\"a\", \"b\"))\n"
        "  free(value)\n"
        "  let text: string = unwrap(value)\n"
        "  return len(text)\n"
        "}\n");

    unit_rejected("owning sum post-move use",
        "def main() -> i64: {\n"
        "  let value: Option[string] = some(concat(\"a\", \"b\"))\n"
        "  let moved: Option[string] = value\n"
        "  let text: string = unwrap(value)\n"
        "  return len(text)\n"
        "}\n");
}

static void test_nested_owning_sums(void) {
    /* Nested owning sums move the inner sum storage rather than copying its
       payload handle. Drop must recursively release the active string. */
    unit_expected("nested owning Option drop",
        "def main() -> i64: {\n"
        "  let value: Option[Option[string]] = some(some(concat(\"a\", \"bc\")))\n"
        "  free(value)\n"
        "  return 13\n"
        "}\n", 13);

    unit_expected("nested owning extraction",
        "def main() -> i64: {\n"
        "  let value: Option[Option[string]] = some(some(concat(\"a\", \"bc\")))\n"
        "  let inner: Option[string] = unwrap(value)\n"
        "  let text: string = unwrap(inner)\n"
        "  n = len(text)\n"
        "  string_free(text)\n"
        "  free(value)\n"
        "  return n\n"
        "}\n", 3);

    unit_expected("nested owning inactive inner drop",
        "def main() -> i64: {\n"
        "  let value: Option[Option[string]] = some(none)\n"
        "  free(value)\n"
        "  return 17\n"
        "}\n", 17);

    unit_expected("nested owning parameter transfer",
        "def take(value: Option[Option[string]]) -> i64: {\n"
        "  let inner: Option[string] = unwrap(value)\n"
        "  let text: string = unwrap(inner)\n"
        "  n = len(text)\n"
        "  string_free(text)\n"
        "  free(value)\n"
        "  return n\n"
        "}\n"
        "def main() -> i64: {\n"
        "  let value: Option[Option[string]] = some(some(concat(\"x\", \"y\")))\n"
        "  return take(value)\n"
        "}\n", 2);

    unit_expected("nested owning return transfer",
        "def make() -> Option[Option[string]]: {\n"
        "  return some(some(concat(\"o\", \"k\")))\n"
        "}\n"
        "def main() -> i64: {\n"
        "  let value: Option[Option[string]] = make()\n"
        "  let inner: Option[string] = unwrap(value)\n"
        "  let text: string = unwrap(inner)\n"
        "  n = len(text)\n"
        "  string_free(text)\n"
        "  free(value)\n"
        "  return n\n"
        "}\n", 2);

    unit_rejected("nested owning post-move use",
        "def main() -> i64: {\n"
        "  let value: Option[Option[string]] = some(some(concat(\"a\", \"b\")))\n"
        "  let moved: Option[Option[string]] = value\n"
        "  let inner: Option[string] = unwrap(value)\n"
        "  return 0\n"
        "}\n");
}

static void test_nested_owning_sum_ir_rejected(void) {
    BackendIrModule module;
    bir_module_init(&module, "<unit:nested-owning-sum-ir>");
    const CobraType *owned = bir_owned_slice_type(&module, module.type_u8);
    const CobraType *inner = bir_sum_type(&module, COBRA_TYPE_OPTION, owned, NULL);
    const CobraType *outer = bir_sum_type(&module, COBRA_TYPE_OPTION, inner, NULL);
    CHECK(owned != NULL && inner != NULL && outer != NULL);
    if (!outer) {
        bir_module_free(&module);
        return;
    }
    SsaArena *arena = &module.arena;
    const CobraType *pointer = bir_pointer_type(&module, outer);
    SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
    SsaInstRef source_slot = bir_add_stack_slot(arena, pointer, outer,
                                                0, 8, 0, 1, 1);
    SsaInstRef destination_slot = bir_add_stack_slot(arena, pointer, outer,
                                                     40, 8, 1, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, source_slot));
    CHECK(bir_block_add_inst(arena, entry, destination_slot));
    SsaValueRef source = bir_inst_result(arena, source_slot, 1, 1);
    SsaValueRef destination = bir_inst_result(arena, destination_slot, 1, 1);
    SsaInstRef copy = bir_add_aggregate_copy(arena, outer, destination, source, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, copy));
    SsaValueRef zero = bir_add_const(arena,
        bir_scalar_i64(module.type_i64, 0), 1, 1);
    CHECK(bir_set_return(arena, entry, zero, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));
    char err[512] = {0};
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "aggregate copy") != NULL ||
          strstr(err, "ownership") != NULL);
    bir_module_free(&module);
}

static void test_source_strings(void) {
    /* Borrowed strings are readonly u8 views: length, indexing, slicing,
       and calls through the full source -> types -> HIR -> SSA -> verifier
       -> evaluator pipeline. */
    unit_expected("string literal length",
        "def main() -> i64: {\n"
        "  return len(\"hello\")\n"
        "}\n", 5);

    unit_expected("empty string literal",
        "def main() -> i64: {\n"
        "  return len(\"\")\n"
        "}\n", 0);

    unit_expected("string local length",
        "def main() -> i64: {\n"
        "  let s: string = \"abc\"\n"
        "  return len(s)\n"
        "}\n", 3);

    unit_expected("string length in expression",
        "def main() -> i64: {\n"
        "  let s: string = \"world\"\n"
        "  let n: i64 = len(s)\n"
        "  return n + len(s)\n"
        "}\n", 10);

    unit_expected("string parameter length",
        "def size(s: string) -> i64: {\n"
        "  return len(s)\n"
        "}\n"
        "def main() -> i64: {\n"
        "  return size(\"hello\")\n"
        "}\n", 5);

    unit_expected("string parameter passthrough",
        "def twice(s: string) -> string: {\n"
        "  return s\n"
        "}\n"
        "def main() -> i64: {\n"
        "  let t: string = twice(\"abcd\")\n"
        "  return len(t) + len(t)\n"
        "}\n", 8);

    unit_expected("owned string concat and free",
        "def main() -> i64: {\n"
        "  let s: string = concat(\"cobra\", \"lang\")\n"
        "  n = len(s)\n"
        "  string_free(s)\n"
        "  return n\n"
        "}\n", 9);

    {
        ASTNode *program = parse_program(
            "def main() -> u8: {\n"
            "  let s: string = concat(\"cobra\", \"lang\")\n"
            "  value = s[5]\n"
            "  string_free(s)\n"
            "  return value\n"
            "}\n");
        CHECK(program != NULL);
        if (program) {
            BirScalarValue result = {0};
            char err[512] = {0};
            CHECK(pipeline_run(program, "main", &result, err, sizeof(err)));
            CHECK(result.kind == BIR_SCALAR_U8 && result.payload.i64 == 108);
            ast_free(program);
        }
    }

    unit_expected("owned string return transfer",
        "def make() -> string: {\n"
        "  return \"left\" + \" right\"\n"
        "}\n"
        "def main() -> i64: {\n"
        "  let s: string = make()\n"
        "  n = len(s)\n"
        "  string_free(s)\n"
        "  return n\n"
        "}\n", 10);

    unit_expected("string borrow return length",
        "def tail(s: string) -> string: {\n"
        "  return s\n"
        "}\n"
        "def main() -> i64: {\n"
        "  let t: string = tail(\"hi\")\n"
        "  return len(t)\n"
        "}\n", 2);

    /* A string satisfies a readonly u8 view parameter at the call boundary. */
    {
        ASTNode *program = parse_program(
            "def byte_at(v: readonly []u8) -> u8: {\n"
            "  return v[0]\n"
            "}\n"
            "def main() -> u8: {\n"
            "  return byte_at(\"xy\")\n"
            "}\n");
        CHECK(program != NULL);
        if (program) {
            BirScalarValue result = {0};
            char err[512] = {0};
            CHECK(pipeline_run(program, "main", &result, err, sizeof(err)));
            CHECK(result.kind == BIR_SCALAR_U8 && result.payload.i64 == 120);
            ast_free(program);
        }
    }

    /* Indexed reads of strings produce u8 bytes. */
    {
        ASTNode *program = parse_program(
            "def main() -> u8: {\n"
            "  let s: string = \"hello\"\n"
            "  return s[1]\n"
            "}\n");
        CHECK(program != NULL);
        if (program) {
            BirScalarValue result = {0};
            char err[512] = {0};
            CHECK(pipeline_run(program, "main", &result, err, sizeof(err)));
            CHECK(result.kind == BIR_SCALAR_U8 && result.payload.i64 == 101);
            ast_free(program);
        }
    }

    /* Indexing a borrowed string parameter. */
    {
        ASTNode *program = parse_program(
            "def first(s: string) -> u8: {\n"
            "  return s[0]\n"
            "}\n"
            "def main() -> u8: {\n"
            "  return first(\"hi\")\n"
            "}\n");
        CHECK(program != NULL);
        if (program) {
            BirScalarValue result = {0};
            char err[512] = {0};
            CHECK(pipeline_run(program, "main", &result, err, sizeof(err)));
            CHECK(result.kind == BIR_SCALAR_U8 && result.payload.i64 == 104);
            ast_free(program);
        }
    }

    /* slice_u8 over a string yields a readonly view. */
    unit_expected("slice_u8 over string length",
        "def main() -> i64: {\n"
        "  let s: string = \"abcdef\"\n"
        "  let v: readonly []u8 = slice_u8(s, 1, 3)\n"
        "  return len(v)\n"
        "}\n", 3);

    /* Indexing through a subview of a string. */
    {
        ASTNode *program = parse_program(
            "def main() -> u8: {\n"
            "  let s: string = \"abcdef\"\n"
            "  let v: readonly []u8 = slice_u8(s, 2, 2)\n"
            "  return v[1]\n"
            "}\n");
        CHECK(program != NULL);
        if (program) {
            BirScalarValue result = {0};
            char err[512] = {0};
            CHECK(pipeline_run(program, "main", &result, err, sizeof(err)));
            CHECK(result.kind == BIR_SCALAR_U8 && result.payload.i64 == 100);
            ast_free(program);
        }
    }

    /* A character read through a returned-borrow chain. */
    {
        ASTNode *program = parse_program(
            "def mid(s: string) -> string: {\n"
            "  return s\n"
            "}\n"
            "def main() -> u8: {\n"
            "  let t: string = mid(\"abcde\")\n"
            "  return t[4]\n"
            "}\n");
        CHECK(program != NULL);
        if (program) {
            BirScalarValue result = {0};
            char err[512] = {0};
            CHECK(pipeline_run(program, "main", &result, err, sizeof(err)));
            CHECK(result.kind == BIR_SCALAR_U8 && result.payload.i64 == 101);
            ast_free(program);
        }
    }

    /* Rejection cases. */
    unit_rejected("string literal return escapes",
        "def make() -> string: {\n"
        "  return \"abc\"\n"
        "}\n"
        "def main() -> i64: {\n"
        "  let s: string = make()\n"
        "  return len(s)\n"
        "}\n");

    unit_rejected("string index out of bounds",
        "def main() -> u8: {\n"
        "  let s: string = \"hi\"\n"
        "  return s[5]\n"
        "}\n");

    unit_rejected("string slice out of bounds",
        "def main() -> i64: {\n"
        "  let s: string = \"abc\"\n"
        "  let v: readonly []u8 = slice_u8(s, 1, 10)\n"
        "  return len(v)\n"
        "}\n");

    unit_rejected("string_free requires ownership",
        "def main() -> i64: {\n"
        "  let s: string = \"borrowed\"\n"
        "  string_free(s)\n"
        "  return 0\n"
        "}\n");

    unit_rejected("owned string use after free",
        "def main() -> i64: {\n"
        "  let s: string = concat(\"a\", \"b\")\n"
        "  string_free(s)\n"
        "  return len(s)\n"
        "}\n");

    unit_rejected("string mutation is rejected",
        "def main() -> i64: {\n"
        "  let s: string = \"abc\"\n"
        "  s[0] = 120\n"
        "  return 0\n"
        "}\n");

    /* A string whose view derives from a local (not a parameter) cannot be
       returned: its source storage is this function's frame. */
    unit_rejected("string local return escapes",
        "def make() -> string: {\n"
        "  let s: string = \"abc\"\n"
        "  return s\n"
        "}\n"
        "def main() -> i64: {\n"
        "  let t: string = make()\n"
        "  return len(t)\n"
        "}\n");
}

static void test_source_enums(void) {
    /* Unit enums lower as integer-backed scalars through the full source ->
       types -> HIR -> SSA -> verifier -> evaluator pipeline, and match
       lowers to verified discriminant comparisons. */
    unit_expected("basic match over all variants",
        "enum Color: { Red, Green, Blue }\n"
        "def main() -> i64: {\n"
        "  let c: Color = Color.Green\n"
        "  match c: {\n"
        "    case Color.Red: { return 1 }\n"
        "    case Color.Green: { return 2 }\n"
        "    case Color.Blue: { return 3 }\n"
        "  }\n"
        "  return 0\n"
        "}\n", 2);

    unit_expected("match else arm",
        "enum Color: { Red, Green, Blue }\n"
        "def main() -> i64: {\n"
        "  let c: Color = Color.Blue\n"
        "  match c: {\n"
        "    case Color.Red: { return 1 }\n"
        "    else: { return 9 }\n"
        "  }\n"
        "  return 0\n"
        "}\n", 9);

    unit_expected("else arm catches uncovered variant",
        "enum Color: { Red, Green, Blue }\n"
        "def main() -> i64: {\n"
        "  let c: Color = Color.Green\n"
        "  match c: {\n"
        "    case Color.Red: { return 1 }\n"
        "    else: { return 9 }\n"
        "  }\n"
        "  return 0\n"
        "}\n", 9);

    unit_expected("explicit discriminants",
        "enum Level: { Low = 10, Mid = 20, High = 30 }\n"
        "def main() -> i64: {\n"
        "  let l: Level = Level.High\n"
        "  match l: {\n"
        "    case Level.Low: { return 1 }\n"
        "    case Level.Mid: { return 2 }\n"
        "    case Level.High: { return 3 }\n"
        "  }\n"
        "  return 0\n"
        "}\n", 3);

    unit_expected("enum parameter through call",
        "enum Color: { Red, Green, Blue }\n"
        "def score(c: Color) -> i64: {\n"
        "  match c: {\n"
        "    case Color.Red: { return 1 }\n"
        "    case Color.Green: { return 2 }\n"
        "    case Color.Blue: { return 3 }\n"
        "  }\n"
        "  return 0\n"
        "}\n"
        "def main() -> i64: { return score(Color.Blue) }\n", 3);

    unit_expected("enum return and comparison",
        "enum Color: { Red, Green, Blue }\n"
        "def next(c: Color) -> Color: {\n"
        "  match c: {\n"
        "    case Color.Red: { return Color.Green }\n"
        "    else: { return Color.Red }\n"
        "  }\n"
        "  return Color.Red\n"
        "}\n"
        "def main() -> i64: {\n"
        "  let c: Color = next(Color.Red)\n"
        "  if c == Color.Green: { return 7 }\n"
        "  return 0\n"
        "}\n", 7);

    unit_expected("enum equality comparison",
        "enum Color: { Red, Green, Blue }\n"
        "def main() -> i64: {\n"
        "  let a: Color = Color.Red\n"
        "  let b: Color = Color.Red\n"
        "  if a == b: { return 5 }\n"
        "  return 0\n"
        "}\n", 5);

    unit_expected("enum inequality comparison",
        "enum Color: { Red, Green, Blue }\n"
        "def main() -> i64: {\n"
        "  let a: Color = Color.Red\n"
        "  let b: Color = Color.Blue\n"
        "  if a != b: { return 6 }\n"
        "  return 0\n"
        "}\n", 6);

    unit_expected("match inside a loop",
        "enum Color: { Red, Green, Blue }\n"
        "def main() -> i64: {\n"
        "  let c: Color = Color.Blue\n"
        "  s = 0\n"
        "  for i in 3: {\n"
        "    match c: {\n"
        "      case Color.Red: { s = s + 1 }\n"
        "      case Color.Green: { s = s + 2 }\n"
        "      case Color.Blue: { s = s + 3 }\n"
        "    }\n"
        "  }\n"
        "  return s\n"
        "}\n", 9);

    unit_expected("implicit enum local assignment",
        "enum Color: { Red, Green, Blue }\n"
        "def main() -> i64: {\n"
        "  c = Color.Green\n"
        "  match c: {\n"
        "    case Color.Red: { return 1 }\n"
        "    case Color.Green: { return 2 }\n"
        "    else: { return 3 }\n"
        "  }\n"
        "  return 0\n"
        "}\n", 2);

    unit_expected("match arm side effects",
        "enum Color: { Red, Green, Blue }\n"
        "def main() -> i64: {\n"
        "  let c: Color = Color.Red\n"
        "  x = 0\n"
        "  match c: {\n"
        "    case Color.Red: { x = x + 100 }\n"
        "    case Color.Green: { x = x + 200 }\n"
        "    case Color.Blue: { x = x + 300 }\n"
        "  }\n"
        "  return x\n"
        "}\n", 100);

    unit_expected("match target evaluated once into a local",
        "enum Color: { Red, Green, Blue }\n"
        "def main() -> i64: {\n"
        "  c = Color.Red\n"
        "  match c: {\n"
        "    case Color.Red: { c = Color.Blue }\n"
        "    case Color.Green: { return 2 }\n"
        "    case Color.Blue: { return 3 }\n"
        "  }\n"
        "  if c == Color.Blue: { return 7 }\n"
        "  return 0\n"
        "}\n", 7);

    /* Rejection cases: exhaustiveness, duplicates, unknown variants,
       foreign-enum arms, multiple else arms, and non-enum targets. */
    unit_rejected("non-exhaustive match without else",
        "enum Color: { Red, Green, Blue }\n"
        "def main() -> i64: {\n"
        "  let c: Color = Color.Red\n"
        "  match c: {\n"
        "    case Color.Red: { return 1 }\n"
        "    case Color.Green: { return 2 }\n"
        "  }\n"
        "  return 0\n"
        "}\n");

    unit_rejected("duplicate match arm",
        "enum Color: { Red, Green, Blue }\n"
        "def main() -> i64: {\n"
        "  let c: Color = Color.Red\n"
        "  match c: {\n"
        "    case Color.Red: { return 1 }\n"
        "    case Color.Red: { return 2 }\n"
        "    case Color.Blue: { return 3 }\n"
        "  }\n"
        "  return 0\n"
        "}\n");

    unit_rejected("unknown enum variant in match",
        "enum Color: { Red, Green, Blue }\n"
        "def main() -> i64: {\n"
        "  let c: Color = Color.Red\n"
        "  match c: {\n"
        "    case Color.Red: { return 1 }\n"
        "    case Color.Yellow: { return 2 }\n"
        "    case Color.Blue: { return 3 }\n"
        "  }\n"
        "  return 0\n"
        "}\n");

    unit_rejected("match case from a different enum",
        "enum Color: { Red, Green, Blue }\n"
        "enum Shape: { Circle, Square }\n"
        "def main() -> i64: {\n"
        "  let c: Color = Color.Red\n"
        "  match c: {\n"
        "    case Color.Red: { return 1 }\n"
        "    case Shape.Circle: { return 2 }\n"
        "    case Color.Blue: { return 3 }\n"
        "  }\n"
        "  return 0\n"
        "}\n");

    unit_rejected("two else arms",
        "enum Color: { Red, Green, Blue }\n"
        "def main() -> i64: {\n"
        "  let c: Color = Color.Red\n"
        "  match c: {\n"
        "    case Color.Red: { return 1 }\n"
        "    else: { return 2 }\n"
        "    else: { return 3 }\n"
        "  }\n"
        "  return 0\n"
        "}\n");

    unit_rejected("match on a non-enum value",
        "def main() -> i64: {\n"
        "  match 5: {\n"
        "    else: { return 2 }\n"
        "  }\n"
        "  return 0\n"
        "}\n");

    unit_rejected("unknown enum variant reference",
        "enum Color: { Red, Green, Blue }\n"
        "def main() -> i64: {\n"
        "  let c: Color = Color.Yellow\n"
        "  return 0\n"
        "}\n");

    unit_rejected("enum assignment type mismatch",
        "enum Color: { Red, Green, Blue }\n"
        "enum Shape: { Circle, Square }\n"
        "def main() -> i64: {\n"
        "  let c: Color = Shape.Circle\n"
        "  return 0\n"
        "}\n");
}

static void test_nested_sums(void) {
    /* Nested scalar sums through the full source -> types -> HIR -> SSA ->
       verifier -> evaluator pipeline. Aggregate sum components occupy their
       real canonical size, and construction/access copy the inner sum as an
       aggregate. */
    unit_expected("nested some/unwrap round trip",
        "def main() -> i64: {\n"
        "  let x: Option[Option[i64]] = some(some(7))\n"
        "  if is_some(x): {} else: { return 1 }\n"
        "  let inner: Option[i64] = unwrap(x)\n"
        "  if is_some(inner): {} else: { return 2 }\n"
        "  let v: i64 = unwrap(inner)\n"
        "  return v - 7\n"
        "}\n", 0);

    unit_expected("outer none is not some",
        "def main() -> i64: {\n"
        "  let x: Option[Option[i64]] = none\n"
        "  if is_some(x): { return 1 }\n"
        "  return 0\n"
        "}\n", 0);

    unit_expected("some of inner none",
        "def main() -> i64: {\n"
        "  let x: Option[Option[i64]] = some(none)\n"
        "  if is_some(x): {} else: { return 1 }\n"
        "  let inner: Option[i64] = unwrap(x)\n"
        "  if is_some(inner): { return 2 }\n"
        "  return 0\n"
        "}\n", 0);

    unit_expected("result with option value",
        "def main() -> i64: {\n"
        "  let r: Result[Option[i64], i64] = ok(some(3))\n"
        "  if is_ok(r): {} else: { return 1 }\n"
        "  let o: Option[i64] = unwrap_ok(r)\n"
        "  if is_some(o): {} else: { return 2 }\n"
        "  return unwrap(o) - 3\n"
        "}\n", 0);

    unit_expected("result err with option error",
        "def main() -> i64: {\n"
        "  let r: Result[i64, Option[i64]] = err(some(4))\n"
        "  if is_ok(r): { return 1 }\n"
        "  let e: Option[i64] = unwrap_err(r)\n"
        "  if is_some(e): {} else: { return 2 }\n"
        "  return unwrap(e) - 4\n"
        "}\n", 0);

    unit_expected("nested sum local copy",
        "def main() -> i64: {\n"
        "  let x: Option[Option[i64]] = some(some(5))\n"
        "  let y: Option[Option[i64]] = x\n"
        "  let inner: Option[i64] = unwrap(y)\n"
        "  return unwrap(inner) - 5\n"
        "}\n", 0);

    unit_expected("nested sum parameter through call",
        "def take(x: Option[Option[i64]]) -> i64: {\n"
        "  if is_some(x): {} else: { return 99 }\n"
        "  let inner: Option[i64] = unwrap(x)\n"
        "  if is_some(inner): {} else: { return 98 }\n"
        "  return unwrap(inner)\n"
        "}\n"
        "def main() -> i64: {\n"
        "  return take(some(some(11))) - 11\n"
        "}\n", 0);

    unit_expected("nested sum return through caller storage",
        "def make() -> Option[Option[i64]]: {\n"
        "  return some(some(4))\n"
        "}\n"
        "def main() -> i64: {\n"
        "  let x: Option[Option[i64]] = make()\n"
        "  let inner: Option[i64] = unwrap(x)\n"
        "  return unwrap(inner) - 4\n"
        "}\n", 0);

    unit_expected("triple nesting",
        "def main() -> i64: {\n"
        "  let x: Option[Option[Option[i64]]] = some(some(some(9)))\n"
        "  if is_some(x): {} else: { return 1 }\n"
        "  let a: Option[Option[i64]] = unwrap(x)\n"
        "  let b: Option[i64] = unwrap(a)\n"
        "  return unwrap(b) - 9\n"
        "}\n", 0);

    unit_expected("nested sum inside if/else",
        "def main() -> i64: {\n"
        "  let x: Option[Option[i64]] = some(some(6))\n"
        "  if is_some(x): {\n"
        "    let inner: Option[i64] = unwrap(x)\n"
        "    if is_some(inner): { return unwrap(inner) - 6 }\n"
        "  }\n"
        "  return 1\n"
        "}\n", 0);

    /* Rejection cases: unwrap failures and type mismatches at both levels. */
    unit_rejected("unwrap of inner none fails at eval",
        "def main() -> i64: {\n"
        "  let x: Option[Option[i64]] = some(none)\n"
        "  let inner: Option[i64] = unwrap(x)\n"
        "  let v: i64 = unwrap(inner)\n"
        "  return v\n"
        "}\n");

    unit_rejected("unwrap of outer none fails at eval",
        "def main() -> i64: {\n"
        "  let x: Option[Option[i64]] = none\n"
        "  let inner: Option[i64] = unwrap(x)\n"
        "  return 0\n"
        "}\n");

    unit_rejected("nested sum assignment type mismatch",
        "def main() -> i64: {\n"
        "  let x: Option[Option[i64]] = some(5)\n"
        "  return 0\n"
        "}\n");

    unit_rejected("unwrap on a nested sum with wrong accessor",
        "def main() -> i64: {\n"
        "  let x: Result[Option[i64], i64] = ok(some(2))\n"
        "  let v: i64 = unwrap(x)\n"
        "  return v\n"
        "}\n");
}

/* ------------------------------------------------------------------ */
/* Remaining scalar types: i32, u32, u64, f64                         */
/* ------------------------------------------------------------------ */

/* Run a source program and require the result to be a specific scalar kind
   with a specific i64 payload (i32, u32, and u64 all carry payload.i64). */
static void unit_expected_kind(const char *name, const char *source,
                               BirScalarKind kind, int64_t expected) {
    ASTNode *program = parse_program(source);
    CHECK(program != NULL);
    if (!program) return;
    BirScalarValue result = {0};
    char err[512] = {0};
    bool ok = pipeline_run(program, "main", &result, err, sizeof(err));
    checks++;
    if (!ok || result.kind != kind || result.payload.i64 != expected) {
        failures++;
        fprintf(stderr, "FAIL %s: ok=%d kind=%d result=%lld expected=%lld (%s)\n",
                name, ok, (int)result.kind, (long long)result.payload.i64,
                (long long)expected, err);
    }
    ast_free(program);
}

/* Run a source program and require an exact f64 bit pattern result. */
static void unit_expected_f64_bits(const char *name, const char *source,
                                   uint64_t expected_bits) {
    ASTNode *program = parse_program(source);
    CHECK(program != NULL);
    if (!program) return;
    BirScalarValue result = {0};
    char err[512] = {0};
    bool ok = pipeline_run(program, "main", &result, err, sizeof(err));
    checks++;
    if (!ok || result.kind != BIR_SCALAR_F64 ||
        result.payload.f64_bits != expected_bits) {
        failures++;
        fprintf(stderr, "FAIL %s: ok=%d kind=%d bits=%016llx expected=%016llx (%s)\n",
                name, ok, (int)result.kind,
                (unsigned long long)result.payload.f64_bits,
                (unsigned long long)expected_bits, err);
    }
    ast_free(program);
}

static void test_source_scalar_types(void) {
    /* i32: 32-bit wrapping arithmetic, signed division, comparisons. */
    unit_expected_kind("i32 constant",
        "def main() -> i32: {\n  return 42\n}\n",
        BIR_SCALAR_I32, 42);
    unit_expected_kind("i32 wrap on add",
        "def main() -> i32: {\n  let x: i32 = 2147483647\n  return x + 1\n}\n",
        BIR_SCALAR_I32, INT32_MIN);
    unit_expected_kind("i32 minimum literal",
        "def main() -> i32: {\n  return -2147483648\n}\n",
        BIR_SCALAR_I32, INT32_MIN);
    unit_expected_kind("i32 signed division truncates",
        "def main() -> i32: {\n  let x: i32 = -7\n  return x / 2\n}\n",
        BIR_SCALAR_I32, -3);
    unit_expected_kind("i32 multiply wraps",
        "def main() -> i32: {\n  let x: i32 = 65536\n  return x * 65536\n}\n",
        BIR_SCALAR_I32, 0);
    unit_expected_kind("i32 comparison",
        "def main() -> bool: {\n  let x: i32 = 5\n  return x < 6\n}\n",
        BIR_SCALAR_BOOL, 1);
    unit_expected_kind("i32 literal narrows to i32 return",
        "def main() -> i32: {\n  return 300\n}\n",
        BIR_SCALAR_I32, 300);

    /* u32: unsigned 32-bit wrap and unsigned division. */
    unit_expected_kind("u32 constant above i32 range",
        "def main() -> u32: {\n  return 3000000000\n}\n",
        BIR_SCALAR_U32, 3000000000LL);
    unit_expected_kind("u32 wrap on add",
        "def main() -> u32: {\n  let x: u32 = 4294967295\n  return x + 1\n}\n",
        BIR_SCALAR_U32, 0);
    unit_expected_kind("u32 wraps below zero",
        "def main() -> u32: {\n  let x: u32 = 0\n  return x - 1\n}\n",
        BIR_SCALAR_U32, UINT32_MAX);
    unit_expected_kind("u32 comparison",
        "def main() -> bool: {\n  let x: u32 = 4000000000\n  return x > 3\n}\n",
        BIR_SCALAR_BOOL, 1);

    /* u8: byte arithmetic uses unsigned wraparound and byte comparisons. */
    unit_expected_kind("u8 add",
        "def main() -> u8: {\n  let x: u8 = 250\n  return x + 10\n}\n",
        BIR_SCALAR_U8, 4);
    unit_expected_kind("u8 subtract wraps",
        "def main() -> u8: {\n  let x: u8 = 3\n  return x - 5\n}\n",
        BIR_SCALAR_U8, 254);
    unit_expected_kind("u8 multiply wraps",
        "def main() -> u8: {\n  let x: u8 = 20\n  return x * 20\n}\n",
        BIR_SCALAR_U8, 144);
    unit_expected_kind("u8 division",
        "def main() -> u8: {\n  let x: u8 = 250\n  return x / 10\n}\n",
        BIR_SCALAR_U8, 25);
    unit_expected_kind("u8 comparison",
        "def main() -> bool: {\n  let x: u8 = 250\n  return x > 3\n}\n",
        BIR_SCALAR_BOOL, 1);
    unit_expected_kind("u8 call argument and return",
        "def bump(value: u8) -> u8: { return value + 1 }\n"
        "def main() -> u8: { return bump(254) }\n",
        BIR_SCALAR_U8, 255);
    unit_expected_kind("u8 sum payload arithmetic",
        "def main() -> u8: {\n  let value: Option[u8] = some(255)\n  return unwrap(value) + 1\n}\n",
        BIR_SCALAR_U8, 0);

    /* u64: full 64-bit unsigned range including the maximum literal. */
    unit_expected_kind("u64 maximum literal",
        "def main() -> u64: {\n  return 18446744073709551615\n}\n",
        BIR_SCALAR_U64, -1);
    unit_expected_kind("u64 wrap on add",
        "def main() -> u64: {\n  let x: u64 = 18446744073709551615\n  return x + 1\n}\n",
        BIR_SCALAR_U64, 0);
    unit_expected_kind("u64 comparison",
        "def main() -> bool: {\n  let x: u64 = 18446744073709551615\n  return x > 0\n}\n",
        BIR_SCALAR_BOOL, 1);

    /* f64: exact IEEE-754 bit patterns, so no f32 round-trip. */
    unit_expected_f64_bits("f64 literal keeps double precision",
        "def main() -> f64: {\n  return 0.1\n}\n",
        UINT64_C(0x3fb999999999999a));
    unit_expected_f64_bits("f64 addition",
        "def main() -> f64: {\n  return 1.5 + 2.25\n}\n",
        UINT64_C(0x400e000000000000));
    unit_expected_f64_bits("f64 multiplication",
        "def main() -> f64: {\n  return 3.0 * 4.5\n}\n",
        UINT64_C(0x402b000000000000));
    unit_expected_f64_bits("f64 division",
        "def main() -> f64: {\n  return 7.5 / 2.0\n}\n",
        UINT64_C(0x400e000000000000));
    unit_expected_f64_bits("f64 negative zero",
        "def main() -> f64: {\n  return -0.0\n}\n",
        UINT64_C(0x8000000000000000));
    unit_expected_kind("f64 comparison",
        "def main() -> bool: {\n  return 1.5 < 2.5\n}\n",
        BIR_SCALAR_BOOL, 1);
    unit_expected_kind("f64 literal widens against f64 local",
        "def main() -> bool: {\n  let a: f64 = 1.5\n  return a < 2.5\n}\n",
        BIR_SCALAR_BOOL, 1);

    /* Parameters and calls across the ABI. */
    unit_expected_kind("i32 parameter",
        "def main(a: i32) -> i32: {\n  return a + 5\n}\n",
        BIR_SCALAR_I32, 5);
    unit_expected_kind("u64 parameter",
        "def main(a: u64) -> u64: {\n  return a + 1\n}\n",
        BIR_SCALAR_U64, 1);
    unit_expected_f64_bits("f64 parameter",
        "def main(a: f64) -> f64: {\n  return a + 1.0\n}\n",
        UINT64_C(0x3ff0000000000000));
    unit_expected_kind("i32 call argument and return",
        "def add(a: i32, b: i32) -> i32: {\n  return a + b\n}\n"
        "def main() -> i32: {\n  return add(10, 32)\n}\n",
        BIR_SCALAR_I32, 42);
    unit_expected_f64_bits("f64 call argument and return",
        "def scale(a: f64) -> f64: {\n  return a * 2.0\n}\n"
        "def main() -> f64: {\n  return scale(2.25)\n}\n",
        UINT64_C(0x4012000000000000));

    /* Locals and reassignment. */
    unit_expected_kind("i32 local reassignment",
        "def main() -> i32: {\n  let x: i32 = 5\n  x = 6\n  return x\n}\n",
        BIR_SCALAR_I32, 6);

    /* Struct fields of the new scalar kinds. */
    unit_expected_kind("i32 struct fields",
        "struct Pair: { a: i32, b: i32 }\n"
        "def main() -> i32: {\n  let p: Pair\n  p.a = 7\n  p.b = 9\n  return p.a + p.b\n}\n",
        BIR_SCALAR_I32, 16);
    unit_expected_kind("u64 struct fields wrap",
        "struct Pair: { a: u64, b: u64 }\n"
        "def main() -> u64: {\n  let p: Pair\n  p.a = 18446744073709551615\n  p.b = 1\n  return p.a + p.b\n}\n",
        BIR_SCALAR_U64, 0);
    unit_expected_f64_bits("f64 struct fields",
        "struct Pair: { a: f64, b: f64 }\n"
        "def main() -> f64: {\n  let p: Pair\n  p.a = 1.5\n  p.b = 2.25\n  return p.a + p.b\n}\n",
        UINT64_C(0x400e000000000000));

    /* Sum payloads of the new scalar kinds. */
    unit_expected_kind("i32 sum payload narrows",
        "def main() -> i32: {\n  let x: Option[i32] = some(-5)\n  return unwrap(x)\n}\n",
        BIR_SCALAR_I32, -5);
    unit_expected_kind("u32 sum payload",
        "def main() -> u32: {\n  let x: Option[u32] = some(3000000000)\n  return unwrap(x)\n}\n",
        BIR_SCALAR_U32, 3000000000LL);
    unit_expected_kind("u64 sum payload",
        "def main() -> u64: {\n  let x: Option[u64] = some(18446744073709551615)\n  return unwrap(x)\n}\n",
        BIR_SCALAR_U64, -1);
    unit_expected_f64_bits("f64 sum payload",
        "def main() -> f64: {\n  let x: Option[f64] = some(0.1)\n  return unwrap(x)\n}\n",
        UINT64_C(0x3fb999999999999a));

    /* Rejections: range checks at every coercion boundary. */
    unit_rejected("i32 literal too large",
        "def main() -> i32: {\n  return 3000000000\n}\n");
    unit_rejected("u32 negative literal",
        "def main() -> u32: {\n  return -1\n}\n");
    unit_rejected("u64 negative literal",
        "def main() -> u64: {\n  return -1\n}\n");
    unit_rejected("i32 literal too large at declaration",
        "def main() -> i64: {\n  let x: i32 = 3000000000\n  return x\n}\n");
    unit_rejected("u64 literal in i64 context",
        "def main() -> i64: {\n  return 18446744073709551615\n}\n");
    unit_rejected("i64 value in u64 context",
        "def main() -> u64: {\n  let x: i64 = 5\n  return x\n}\n");
    unit_rejected("float literal in i64 context",
        "def main() -> i64: {\n  return 1.5\n}\n");
    unit_rejected("f32 and f64 comparison rejected",
        "def main() -> bool: {\n  let a: f64 = 1.5\n  let b: f32 = 1.5\n  return a == b\n}\n");
}

/* ------------------------------------------------------------------ */
/* Post-milestone audit regressions                                    */
/* ------------------------------------------------------------------ */

static void test_audit_regressions(void) {
    BirScalarValue result = {0};
    char err[512] = {0};

    /* Early returns on any path inside a region body must not skip the
       region exit. */
    {
        ASTNode *program = parse_program(
            "def main() -> i64: {\n"
            "  with region scratch: {\n"
            "    if 1 == 1: { return 5 }\n"
            "  }\n"
            "  return 0\n"
            "}\n");
        CHECK(program != NULL);
        if (program) {
            memset(err, 0, sizeof(err));
            CHECK(!pipeline_run(program, "main", &result, err, sizeof(err)));
            CHECK(strstr(err, "return inside a with region body") != NULL);
            ast_free(program);
        }
    }

    /* A borrowed view of a region allocation used after the region exit is
       rejected; the region is the cleanup boundary. */
    {
        ASTNode *program = parse_program(
            "def main() -> i64: {\n"
            "  with region scratch: {\n"
            "    a = scratch.alloc_i64(1)\n"
            "    let v: readonly []i64 = a\n"
            "  }\n"
            "  return v[0]\n"
            "}\n");
        CHECK(program != NULL);
        if (program) {
            memset(err, 0, sizeof(err));
            CHECK(!pipeline_run(program, "main", &result, err, sizeof(err)));
            CHECK(strstr(err, "destruction") != NULL ||
                  strstr(err, "destroyed") != NULL);
            ast_free(program);
        }
    }

    /* Double free through an owned-slice alias. */
    {
        ASTNode *program = parse_program(
            "def main() -> i64: {\n"
            "  s = alloc_i64(1)\n"
            "  t = s\n"
            "  free(s)\n"
            "  free(t)\n"
            "  return 0\n"
            "}\n");
        CHECK(program != NULL);
        if (program) {
            memset(err, 0, sizeof(err));
            CHECK(!pipeline_run(program, "main", &result, err, sizeof(err)));
            CHECK(strstr(err, "double") != NULL || strstr(err, "free") != NULL ||
                  strstr(err, "destruction") != NULL);
            ast_free(program);
        }
    }

    /* Use after free through an owned-slice alias. */
    {
        ASTNode *program = parse_program(
            "def main() -> i64: {\n"
            "  s = alloc_i64(1)\n"
            "  t = s\n"
            "  free(s)\n"
            "  return t[0]\n"
            "}\n");
        CHECK(program != NULL);
        if (program) {
            memset(err, 0, sizeof(err));
            CHECK(!pipeline_run(program, "main", &result, err, sizeof(err)));
            CHECK(strstr(err, "destruction") != NULL ||
                  strstr(err, "destroyed") != NULL);
            ast_free(program);
        }
    }

    /* Allocation identity must not collide across a call boundary: the
       callee's own slice and the caller's passed view have independent
       lifetimes. */
    unit_expected("caller and callee slice allocations are independent",
        "def helper(v: readonly []i64) -> i64: {\n"
        "  s = alloc_i64(1)\n"
        "  s[0] = 1\n"
        "  free(s)\n"
        "  return v[0]\n"
        "}\n"
        "def main() -> i64: {\n"
        "  x = 0\n"
        "  t = alloc_i64(1)\n"
        "  t[0] = 7\n"
        "  return helper(t)\n"
        "}\n", 7);

    /* Recursion keeps per-frame slice allocations independent. */
    unit_expected("recursive per-frame slice allocations",
        "def descend(n: i64) -> i64: {\n"
        "  s = alloc_i64(1)\n"
        "  s[0] = n\n"
        "  if n == 0: {\n"
        "    free(s)\n"
        "    return 0\n"
        "  }\n"
        "  r = descend(n - 1)\n"
        "  free(s)\n"
        "  return r + n\n"
        "}\n"
        "def main() -> i64: { return descend(3) }\n", 6);

    /* Source/backend behavior matching: the host interpreter and the SSA
       evaluator agree on allocation, length, and destruction. */
    differential("alloc/len/free source and backend match",
        "def main() -> i64: {\n"
        "  s = alloc_i64(4)\n"
        "  r = len(s)\n"
        "  free(s)\n"
        "  return r + 10\n"
        "}\n", 14);
}

static void test_mir_lowering(void) {
    ASTNode *program = parse_program(
        "def add(a: i64, b: i64) -> i64: { return a + b }\n"
        "def main() -> i64: {\n"
        "  x = add(2, 3)\n"
        "  if x == 5: { return x } else: { return 0 }\n"
        "}\n");
    CHECK(program != NULL);
    if (!program) return;

    BackendIrModule source;
    bir_module_init(&source, "<unit:mir>");
    char err[512] = {0};
    CHECK(bir_build_program(&source, program));
    CHECK(bir_verify(&source, err, sizeof(err)));

    MirModule mir;
    memset(&mir, 0, sizeof(mir));
    memset(err, 0, sizeof(err));
    CHECK(mir_lower_module(&source, &mir, err, sizeof(err)));
    CHECK(mir_verify(&mir, err, sizeof(err)));
    CHECK(mir.function_count == source.function_count);
    CHECK(mir.arena.reg_count > 1 && mir.arena.block_count > 1);

    bool saw_abi_move = false;
    bool saw_call = false;
    bool saw_branch = false;
    bool saw_memory = false;
    MirInstRef call_ref = MIR_INST_NONE;
    for (size_t i = 1; i < mir.arena.inst_count; i++) {
        MirOpcode op = mir.arena.insts[i].op;
        if (op == MIR_OP_ABI_MOVE) saw_abi_move = true;
        if (op == MIR_OP_CALL) {
            saw_call = true;
            call_ref = (MirInstRef)i;
        }
        if (op == MIR_OP_BRANCH) saw_branch = true;
        if (op == MIR_OP_STACK_SLOT || op == MIR_OP_LOAD || op == MIR_OP_STORE)
            saw_memory = true;
    }
    CHECK(saw_abi_move && saw_call && saw_branch && saw_memory);

    char *dump = NULL;
    size_t dump_len = 0;
    FILE *out = open_memstream(&dump, &dump_len);
    CHECK(out != NULL);
    mir_dump(&mir, out);
    fclose(out);
    CHECK(dump && strstr(dump, "abi_move") != NULL);
    CHECK(dump && strstr(dump, "call \"add\"") != NULL);
    CHECK(dump && strstr(dump, "branch") != NULL);
    free(dump);

    if (call_ref != MIR_INST_NONE) {
        CHECK(mir.arena.insts[call_ref].clobbers.gpr_mask != 0 &&
              mir.arena.insts[call_ref].clobbers.xmm_mask != 0);
        uint32_t saved = mir.arena.insts[call_ref].operand_count;
        mir.arena.insts[call_ref].operand_count = 0;
        memset(err, 0, sizeof(err));
        CHECK(!mir_verify(&mir, err, sizeof(err)));
        CHECK(strstr(err, "callee ABI") != NULL || strstr(err, "argument") != NULL);
        mir.arena.insts[call_ref].operand_count = saved;
    }
    mir_module_free(&mir);
    bir_module_free(&source);
    ast_free(program);
}

static void emit_x86_source_file(const char *source_text, const char *path) {
    ASTNode *program = parse_program(source_text);
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule source;
    bir_module_init(&source, "<unit:x86-file>");
    char err[512] = {0};
    CHECK(bir_build_program(&source, program));
    MirModule mir;
    memset(&mir, 0, sizeof(mir));
    CHECK(mir_lower_module(&source, &mir, err, sizeof(err)));
    FILE *out = fopen(path, "w");
    CHECK(out != NULL);
    if (out) {
        CHECK(bir_x86_64_emit(&mir, out, err, sizeof(err)));
        CHECK(fclose(out) == 0);
    }
    if (err[0]) fprintf(stderr, "x86 file emit: %s\n", err);
    mir_module_free(&mir);
    bir_module_free(&source);
    ast_free(program);
}

static void test_mir_allocation(void) {
    ASTNode *program = parse_program(
        "def identity(a: i64) -> i64: { return a }\n"
        "def float_identity(a: f64) -> f64: { return a }\n"
        "def main(a: i64) -> i64: {\n"
        "  x = a + 1\n"
        "  y = identity(a)\n"
        "  return x + y\n"
        "}\n");
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule source;
    bir_module_init(&source, "<unit:allocation>");
    char err[512] = {0};
    CHECK(bir_build_program(&source, program));
    MirModule mir;
    memset(&mir, 0, sizeof(mir));
    CHECK(mir_lower_module(&source, &mir, err, sizeof(err)));
    MirAllocation allocation;
    mir_allocation_init(&allocation, &mir);
    CHECK(mir_allocate(&mir, &allocation, err, sizeof(err)));
    CHECK(mir_allocation_verify(&allocation, err, sizeof(err)));

    bool saw_gpr_register = false;
    bool saw_xmm_register = false;
    bool saw_call_survivor = false;
    for (MirReg reg = 1; reg < allocation.reg_count; reg++) {
        const MirRegAllocation *result = &allocation.regs[reg];
        if (result->kind == MIR_ALLOC_REGISTER &&
            result->register_class == BIR_ABI_REGISTER_GPR) saw_gpr_register = true;
        if (result->kind == MIR_ALLOC_REGISTER &&
            result->register_class == BIR_ABI_REGISTER_XMM) saw_xmm_register = true;
        /* A value live across the identity(a) call now survives in a
           callee-saved register instead of always spilling. */
        if (result->kind == MIR_ALLOC_REGISTER && result->interval.forbidden_register_mask &&
            !(result->interval.forbidden_register_mask & (UINT64_C(1) << result->register_index)))
            saw_call_survivor = true;
    }
    CHECK(saw_gpr_register && saw_xmm_register && saw_call_survivor);

    char *dump = NULL;
    size_t dump_len = 0;
    FILE *out = open_memstream(&dump, &dump_len);
    CHECK(out != NULL);
    mir_allocation_dump(&allocation, out);
    fclose(out);
    CHECK(dump && strstr(dump, "spill") != NULL);
    CHECK(dump && strstr(dump, "xmm") != NULL);
    free(dump);

    for (MirReg reg = 1; reg < allocation.reg_count; reg++) {
        if (allocation.regs[reg].kind != MIR_ALLOC_NONE) {
            MirAllocationKind saved = allocation.regs[reg].kind;
            allocation.regs[reg].kind = MIR_ALLOC_NONE;
            memset(err, 0, sizeof(err));
            CHECK(!mir_allocation_verify(&allocation, err, sizeof(err)));
            allocation.regs[reg].kind = saved;
            break;
        }
    }
    mir_allocation_free(&allocation);
    mir_module_free(&mir);
    bir_module_free(&source);
    ast_free(program);
}

static void test_x86_64_emission(void) {
    ASTNode *program = parse_program(
        "def add(a: i64, b: i64) -> i64: { return a + b }\n"
        "def addf(a: f64, b: f64) -> f64: { return a + b }\n"
        "def main() -> i64: {\n"
        "  x = add(20, 22)\n"
        "  y = addf(1.5, 2.5)\n"
        "  if x == 42: {\n"
        "    if y == 4.0: { return x }\n"
        "  }\n"
        "  return 0\n"
        "}\n");
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule source;
    bir_module_init(&source, "<unit:x86-64>");
    char err[512] = {0};
    CHECK(bir_build_program(&source, program));
    CHECK(bir_verify(&source, err, sizeof(err)));
    MirModule mir;
    memset(&mir, 0, sizeof(mir));
    CHECK(mir_lower_module(&source, &mir, err, sizeof(err)));
    char *assembly = NULL;
    size_t assembly_len = 0;
    FILE *out = open_memstream(&assembly, &assembly_len);
    CHECK(out != NULL);
    CHECK(bir_x86_64_emit(&mir, out, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "x86 emit: %s\n", err);
    fclose(out);
    CHECK(assembly && strstr(assembly, ".text") != NULL);
    CHECK(assembly && strstr(assembly, "pushq %rbp") != NULL);
    CHECK(assembly && strstr(assembly, "call add") != NULL);
    CHECK(assembly && strstr(assembly, "call addf") != NULL);
    CHECK(assembly && strstr(assembly, "addq") != NULL);
    CHECK(assembly && strstr(assembly, "jne .Lcobra") != NULL);
    CHECK(assembly && strstr(assembly, "addsd") != NULL);
    CHECK(assembly && strstr(assembly, "ucomisd") != NULL);
    CHECK(assembly && strstr(assembly, "ret") != NULL);
    const char *assembly_path = getenv("BIR_X86_ASM_OUT");
    if (assembly_path && assembly) {
        FILE *assembly_file = fopen(assembly_path, "w");
        CHECK(assembly_file != NULL);
        if (assembly_file) {
            CHECK(fputs(assembly, assembly_file) >= 0);
            CHECK(fclose(assembly_file) == 0);
        }
    }

    MirAllocation allocation;
    mir_allocation_init(&allocation, &mir);
    CHECK(mir_allocate(&mir, &allocation, err, sizeof(err)));
    char *allocated_assembly = NULL;
    size_t allocated_assembly_len = 0;
    FILE *allocated_out = open_memstream(&allocated_assembly, &allocated_assembly_len);
    CHECK(allocated_out != NULL);
    memset(err, 0, sizeof(err));
    CHECK(bir_x86_64_emit_allocated(&mir, &allocation, allocated_out,
                                    err, sizeof(err)));
    if (err[0]) fprintf(stderr, "allocated x86 emit: %s\n", err);
    fclose(allocated_out);
    CHECK(allocated_assembly && strstr(allocated_assembly, "call add") != NULL);
    CHECK(allocated_assembly && strstr(allocated_assembly, "addsd") != NULL);
    CHECK(allocated_assembly && strstr(allocated_assembly, "ucomisd") != NULL);
    const char *allocated_path = getenv("BIR_X86_ALLOC_ASM_OUT");
    if (allocated_path && allocated_assembly) {
        FILE *allocated_file = fopen(allocated_path, "w");
        CHECK(allocated_file != NULL);
        if (allocated_file) {
            CHECK(fputs(allocated_assembly, allocated_file) >= 0);
            CHECK(fclose(allocated_file) == 0);
        }
    }
    free(allocated_assembly);
    mir_allocation_free(&allocation);
    free(assembly);
    mir_module_free(&mir);
    bir_module_free(&source);
    ast_free(program);

    const char *integer_assembly_path = getenv("BIR_X86_INT_ASM_OUT");
    if (integer_assembly_path) {
        emit_x86_source_file(
            "def checked_value() -> i64: { return 42 }\n"
            "def main() -> i64: { return checked_value() }\n",
            integer_assembly_path);
    }

    program = parse_program(
        "def main() -> i64: {\n"
        "  let s: string = \"x\"\n"
        "  result = len(s)\n"
        "  return result\n"
        "}\n");
    CHECK(program != NULL);
    if (program) {
        bir_module_init(&source, "<unit:x86-64-reject>");
        memset(err, 0, sizeof(err));
        CHECK(bir_build_program(&source, program));
        memset(&mir, 0, sizeof(mir));
        CHECK(mir_lower_module(&source, &mir, err, sizeof(err)));
        out = open_memstream(&assembly, &assembly_len);
        CHECK(out != NULL);
        memset(err, 0, sizeof(err));
        CHECK(bir_x86_64_emit(&mir, out, err, sizeof(err)));
        if (err[0]) fprintf(stderr, "native string literal emit: %s\\n", err);
        fclose(out);
        CHECK(assembly && strstr(assembly, "call malloc@PLT") != NULL);
        CHECK(assembly && strstr(assembly, "call free@PLT") != NULL);
        free(assembly);
        mir_module_free(&mir);
        bir_module_free(&source);
        ast_free(program);
    }
}

static void test_x86_native_matrix(void) {
    const char *source_text =
        "def check_i32() -> i64: {\n"
        "  let x: i32 = 2147483647\n"
        "  x = x + 1\n"
        "  if x < 0: { return 1 }\n"
        "  return 0\n"
        "}\n"
        "def check_u32() -> i64: {\n"
        "  let x: u32 = 4294967295\n"
        "  x = x + 1\n"
        "  if x == 0: { return 1 }\n"
        "  return 0\n"
        "}\n"
        "def check_u64() -> i64: {\n"
        "  let x: u64 = 100\n"
        "  if x / 4 == 25: { return 1 }\n"
        "  return 0\n"
        "}\n"
        "def check_f32() -> i64: {\n"
        "  let x: f32 = 1.5\n"
        "  x = x + 2.5\n"
        "  if x == 4.0: { return 1 }\n"
        "  return 0\n"
        "}\n"
        "def check_f64() -> i64: {\n"
        "  let x: f64 = 1.5\n"
        "  x = x + 2.5\n"
        "  if x == 4.0: { return 1 }\n"
        "  return 0\n"
        "}\n"
        "def check_loop() -> i64: {\n"
        "  let i: i64 = 0\n"
        "  let sum: i64 = 0\n"
        "  while i < 5: {\n"
        "    sum = sum + i\n"
        "    i = i + 1\n"
        "  }\n"
        "  if sum == 10: { return 1 }\n"
        "  return 0\n"
        "}\n"
        "def main() -> i64: {\n"
        "  return check_i32() + check_u32() * 2 + check_u64() * 4 + check_f32() * 8 + check_f64() * 16 + check_loop() * 32\n"
        "}\n";
    ASTNode *program = parse_program(source_text);
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule source;
    bir_module_init(&source, "<unit:x86-native-matrix>");
    char err[512] = {0};
    CHECK(bir_build_program(&source, program));
    MirModule mir;
    memset(&mir, 0, sizeof(mir));
    CHECK(mir_lower_module(&source, &mir, err, sizeof(err)));
    MirAllocation allocation;
    mir_allocation_init(&allocation, &mir);
    CHECK(mir_allocate(&mir, &allocation, err, sizeof(err)));
    char *assembly = NULL;
    size_t assembly_len = 0;
    FILE *out = open_memstream(&assembly, &assembly_len);
    CHECK(out != NULL);
    memset(err, 0, sizeof(err));
    CHECK(bir_x86_64_emit_allocated(&mir, &allocation, out, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "native matrix emit: %s\n", err);
    fclose(out);
    CHECK(assembly && strstr(assembly, "addl") != NULL);
    CHECK(assembly && strstr(assembly, "divq") != NULL);
    CHECK(assembly && strstr(assembly, "addss") != NULL);
    CHECK(assembly && strstr(assembly, "addsd") != NULL);
    CHECK(assembly && strstr(assembly, ".Lcobra_alloc") != NULL);
    const char *path = getenv("BIR_X86_MATRIX_ASM_OUT");
    if (path && assembly) {
        FILE *file = fopen(path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    free(assembly);
    mir_allocation_free(&allocation);
    mir_module_free(&mir);
    bir_module_free(&source);
    ast_free(program);
}

static void test_x86_native_scalar_structs(void) {
    const char *source_text =
        "struct Pair: { left: i64, right: i64 }\n"
        "def make_pair(a: i64, b: i64) -> Pair: {\n"
        "  let p: Pair\n"
        "  p.left = a\n"
        "  p.right = b\n"
        "  return p\n"
        "}\n"
        "def score_pair(p: Pair) -> i64: {\n"
        "  let copy: Pair\n"
        "  copy = p\n"
        "  copy.left = copy.left + 1\n"
        "  return copy.left * 10 + copy.right\n"
        "}\n"
        "def main() -> i64: {\n"
        "  let p: Pair\n"
        "  p = make_pair(4, 2)\n"
        "  return score_pair(p)\n"
        "}\n";
    ASTNode *program = parse_program(source_text);
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule source;
    bir_module_init(&source, "<unit:x86-native-structs>");
    char err[512] = {0};
    CHECK(bir_build_program(&source, program));
    CHECK(bir_verify(&source, err, sizeof(err)));
    BirScalarValue result = {0};
    CHECK(bir_eval_function_value(&source, "main", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 52);

    MirModule mir;
    memset(&mir, 0, sizeof(mir));
    CHECK(mir_lower_module(&source, &mir, err, sizeof(err)));
    MirAllocation allocation;
    mir_allocation_init(&allocation, &mir);
    CHECK(mir_allocate(&mir, &allocation, err, sizeof(err)));

    char *spill_assembly = NULL;
    size_t spill_length = 0;
    FILE *spill_out = open_memstream(&spill_assembly, &spill_length);
    CHECK(spill_out != NULL);
    CHECK(bir_x86_64_emit(&mir, spill_out, err, sizeof(err)));
    fclose(spill_out);
    CHECK(spill_assembly && strstr(spill_assembly, "call make_pair") != NULL);
    CHECK(spill_assembly && strstr(spill_assembly, "call score_pair") != NULL);
    CHECK(spill_assembly && strstr(spill_assembly, "movq 0(%r11), %rax") != NULL);

    char *allocated_assembly = NULL;
    size_t allocated_length = 0;
    FILE *allocated_out = open_memstream(&allocated_assembly, &allocated_length);
    CHECK(allocated_out != NULL);
    CHECK(bir_x86_64_emit_allocated(&mir, &allocation, allocated_out,
                                    err, sizeof(err)));
    if (err[0]) fprintf(stderr, "native struct allocated emit: %s\n", err);
    fclose(allocated_out);
    CHECK(allocated_assembly && strstr(allocated_assembly, "call make_pair") != NULL);
    CHECK(allocated_assembly && strstr(allocated_assembly, "call score_pair") != NULL);
    CHECK(allocated_assembly && strstr(allocated_assembly, "movq 0(%r11), %rax") != NULL);

    const char *spill_path = getenv("BIR_X86_STRUCT_SPILL_ASM_OUT");
    if (spill_path && spill_assembly) {
        FILE *file = fopen(spill_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(spill_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    const char *allocated_path = getenv("BIR_X86_STRUCT_ALLOC_ASM_OUT");
    if (allocated_path && allocated_assembly) {
        FILE *file = fopen(allocated_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(allocated_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    free(spill_assembly);
    free(allocated_assembly);
    mir_allocation_free(&allocation);
    mir_module_free(&mir);
    bir_module_free(&source);
    ast_free(program);
}

static void test_x86_native_readonly_views(void) {
    const char *source_text =
        "def read_view(v: readonly []i64) -> i64: {\n"
        "  return v[1] + len(v)\n"
        "}\n"
        "def identity_view(v: readonly []i64) -> readonly []i64: {\n"
        "  return v\n"
        "}\n"
        "def subview_value(v: readonly []u8) -> i64: {\n"
        "  let sub: readonly []u8 = slice_u8(v, 1, 2)\n"
        "  return len(sub)\n"
        "}\n";
    ASTNode *program = parse_program(source_text);
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule source;
    bir_module_init(&source, "<unit:x86-native-views>");
    char err[512] = {0};
    CHECK(bir_build_program(&source, program));
    if (source.error[0]) fprintf(stderr, "native view build: %s\n", source.error);
    CHECK(bir_verify(&source, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "native view verify: %s\n", err);
    MirModule mir;
    memset(&mir, 0, sizeof(mir));
    CHECK(mir_lower_module(&source, &mir, err, sizeof(err)));
    MirAllocation allocation;
    mir_allocation_init(&allocation, &mir);
    CHECK(mir_allocate(&mir, &allocation, err, sizeof(err)));

    char *spill_assembly = NULL;
    size_t spill_length = 0;
    FILE *spill_out = open_memstream(&spill_assembly, &spill_length);
    CHECK(spill_out != NULL);
    CHECK(bir_x86_64_emit(&mir, spill_out, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "native view spill emit: %s\n", err);
    fclose(spill_out);
    CHECK(spill_assembly && strstr(spill_assembly, "read_view") != NULL);
    CHECK(spill_assembly && strstr(spill_assembly, "ud2") != NULL);

    char *allocated_assembly = NULL;
    size_t allocated_length = 0;
    FILE *allocated_out = open_memstream(&allocated_assembly, &allocated_length);
    CHECK(allocated_out != NULL);
    CHECK(bir_x86_64_emit_allocated(&mir, &allocation, allocated_out,
                                    err, sizeof(err)));
    fclose(allocated_out);
    CHECK(allocated_assembly && strstr(allocated_assembly, "identity_view") != NULL);
    CHECK(allocated_assembly && strstr(allocated_assembly, "ud2") != NULL);

    const char *spill_path = getenv("BIR_X86_VIEW_SPILL_ASM_OUT");
    if (spill_path && spill_assembly) {
        FILE *file = fopen(spill_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(spill_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    const char *allocated_path = getenv("BIR_X86_VIEW_ALLOC_ASM_OUT");
    if (allocated_path && allocated_assembly) {
        FILE *file = fopen(allocated_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(allocated_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    free(spill_assembly);
    free(allocated_assembly);
    mir_allocation_free(&allocation);
    mir_module_free(&mir);
    bir_module_free(&source);
    ast_free(program);
}

static void test_x86_native_writable_views(void) {
    const char *source_text =
        "def write_view(v: out []i64) -> i64: {\n"
        "  v[1] = 42\n"
        "  return v[1]\n"
        "}\n"
    ;
    ASTNode *program = parse_program(source_text);
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule source;
    bir_module_init(&source, "<unit:x86-native-writable-views>");
    char err[512] = {0};
    CHECK(bir_build_program(&source, program));
    if (source.error[0]) fprintf(stderr, "native writable view build: %s\n", source.error);
    CHECK(bir_verify(&source, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "native writable view verify: %s\n", err);
    MirModule mir;
    memset(&mir, 0, sizeof(mir));
    CHECK(mir_lower_module(&source, &mir, err, sizeof(err)));
    MirAllocation allocation;
    mir_allocation_init(&allocation, &mir);
    CHECK(mir_allocate(&mir, &allocation, err, sizeof(err)));

    char *spill_assembly = NULL;
    size_t spill_length = 0;
    FILE *spill_out = open_memstream(&spill_assembly, &spill_length);
    CHECK(spill_out != NULL);
    CHECK(bir_x86_64_emit(&mir, spill_out, err, sizeof(err)));
    fclose(spill_out);
    CHECK(spill_assembly && strstr(spill_assembly, "write_view") != NULL);
    CHECK(spill_assembly && strstr(spill_assembly, "movq %r11, (%r10)") != NULL);

    char *allocated_assembly = NULL;
    size_t allocated_length = 0;
    FILE *allocated_out = open_memstream(&allocated_assembly, &allocated_length);
    CHECK(allocated_out != NULL);
    CHECK(bir_x86_64_emit_allocated(&mir, &allocation, allocated_out,
                                    err, sizeof(err)));
    fclose(allocated_out);
    CHECK(allocated_assembly && strstr(allocated_assembly, "write_view") != NULL);
    CHECK(allocated_assembly && strstr(allocated_assembly, "ud2") != NULL);

    const char *spill_path = getenv("BIR_X86_WRITABLE_SPILL_ASM_OUT");
    if (spill_path && spill_assembly) {
        FILE *file = fopen(spill_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(spill_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    const char *allocated_path = getenv("BIR_X86_WRITABLE_ALLOC_ASM_OUT");
    if (allocated_path && allocated_assembly) {
        FILE *file = fopen(allocated_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(allocated_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    free(spill_assembly);
    free(allocated_assembly);
    mir_allocation_free(&allocation);
    mir_module_free(&mir);
    bir_module_free(&source);
    ast_free(program);
}

/* ------------------------------------------------------------------ */
/* Fixed scalar arrays as struct fields: whole-field and whole-struct   */
/* value semantics through both native emitters.                       */
/* ------------------------------------------------------------------ */

static void test_x86_native_array_fields(void) {
    const char *source_text =
        "struct Grid: { data: array[i64, 4], scale: i64 }\n"
        "def make_grid() -> Grid: {\n"
        "  let g: Grid\n"
        "  g.data = [1, 2, 3, 4]\n"
        "  g.scale = 10\n"
        "  return g\n"
        "}\n"
        "def sum_grid(g: Grid) -> i64: {\n"
        "  let copy: array[i64, 4] = g.data\n"
        "  return copy[0] + copy[1] + copy[2] + copy[3] + g.scale\n"
        "}\n"
        "def grid_main() -> i64: {\n"
        "  let g: Grid = make_grid()\n"
        "  return sum_grid(g)\n"
        "}\n";
    ASTNode *program = parse_program(source_text);
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule source;
    bir_module_init(&source, "<unit:x86-native-array-fields>");
    char err[512] = {0};
    CHECK(bir_build_program(&source, program));
    if (source.error[0]) fprintf(stderr, "native array fields build: %s\n", source.error);
    CHECK(bir_verify(&source, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "native array fields verify: %s\n", err);
    BirScalarValue result = {0};
    CHECK(bir_eval_function_value(&source, "grid_main", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 20);

    MirModule mir;
    memset(&mir, 0, sizeof(mir));
    memset(err, 0, sizeof(err));
    CHECK(mir_lower_module(&source, &mir, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "native array fields mir lower: %s\n", err);
    MirAllocation allocation;
    mir_allocation_init(&allocation, &mir);
    memset(err, 0, sizeof(err));
    CHECK(mir_allocate(&mir, &allocation, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "native array fields mir alloc: %s\n", err);

    char *spill_assembly = NULL;
    size_t spill_length = 0;
    FILE *spill_out = open_memstream(&spill_assembly, &spill_length);
    CHECK(spill_out != NULL);
    memset(err, 0, sizeof(err));
    CHECK(bir_x86_64_emit(&mir, spill_out, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "native array fields spill emit: %s\n", err);
    fclose(spill_out);
    CHECK(spill_assembly && strstr(spill_assembly, "call make_grid") != NULL);
    CHECK(spill_assembly && strstr(spill_assembly, "call sum_grid") != NULL);

    char *allocated_assembly = NULL;
    size_t allocated_length = 0;
    FILE *allocated_out = open_memstream(&allocated_assembly, &allocated_length);
    CHECK(allocated_out != NULL);
    CHECK(bir_x86_64_emit_allocated(&mir, &allocation, allocated_out,
                                    err, sizeof(err)));
    if (err[0]) fprintf(stderr, "native array fields allocated emit: %s\n", err);
    fclose(allocated_out);
    CHECK(allocated_assembly && strstr(allocated_assembly, "call make_grid") != NULL);
    CHECK(allocated_assembly && strstr(allocated_assembly, "call sum_grid") != NULL);

    const char *spill_path = getenv("BIR_X86_GRID_SPILL_ASM_OUT");
    if (spill_path && spill_assembly) {
        FILE *file = fopen(spill_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(spill_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    const char *allocated_path = getenv("BIR_X86_GRID_ALLOC_ASM_OUT");
    if (allocated_path && allocated_assembly) {
        FILE *file = fopen(allocated_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(allocated_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    free(spill_assembly);
    free(allocated_assembly);

    /* Tampering with the element descriptor of a fixed-array index address
       must be rejected by the MIR verifier before emission. */
    bool corrupted = false;
    for (size_t i = 1; i < mir.arena.inst_count && !corrupted; i++) {
        MirInst *inst = &mir.arena.insts[i];
        if (inst->op == MIR_OP_ARRAY_INDEX_ADDR) {
            inst->memory_width = inst->memory_width + 1;
            corrupted = true;
        }
    }
    CHECK(corrupted);
    memset(err, 0, sizeof(err));
    CHECK(!mir_verify(&mir, err, sizeof(err)));
    CHECK(strstr(err, "fixed-array index address") != NULL);    mir_allocation_free(&allocation);
    mir_module_free(&mir);
    bir_module_free(&source);
    ast_free(program);
}

/* ------------------------------------------------------------------ */
/* Nested fixed arrays: array-of-array values with whole-row value     */
/* semantics through literals, index assignment, struct fields, calls,  */
/* returns, and both native emitters.                                  */
/* ------------------------------------------------------------------ */

static void test_x86_native_nested_arrays(void) {
    const char *source_text =
        "struct Matrix: { rows: array[array[i64, 2], 2], scale: i64 }\n"
        "def make_matrix() -> Matrix: {\n"
        "  let m: Matrix\n"
        "  m.rows = [[1, 2], [3, 4]]\n"
        "  m.scale = 10\n"
        "  return m\n"
        "}\n"
        "def nested_main() -> i64: {\n"
        "  let grid: array[array[i64, 2], 2] = [[1, 2], [3, 4]]\n"
        "  let row: array[i64, 2] = grid[0]\n"
        "  grid[1] = row\n"
        "  let second: array[i64, 2] = grid[1]\n"
        "  return row[0] + row[1] + second[0] + second[1]\n"
        "}\n"
        "def matrix_main() -> i64: {\n"
        "  let m: Matrix = make_matrix()\n"
        "  let rows: array[array[i64, 2], 2] = m.rows\n"
        "  let r0: array[i64, 2] = rows[0]\n"
        "  return r0[0] + r0[1] + m.scale\n"
        "}\n";
    ASTNode *program = parse_program(source_text);
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule source;
    bir_module_init(&source, "<unit:x86-native-nested-arrays>");
    char err[512] = {0};
    CHECK(bir_build_program(&source, program));
    if (source.error[0]) fprintf(stderr, "nested arrays build: %s\n", source.error);
    CHECK(bir_verify(&source, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "nested arrays verify: %s\n", err);
    BirScalarValue result = {0};
    CHECK(bir_eval_function_value(&source, "nested_main", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 6);
    CHECK(bir_eval_function_value(&source, "matrix_main", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 13);

    MirModule mir;
    memset(&mir, 0, sizeof(mir));
    memset(err, 0, sizeof(err));
    CHECK(mir_lower_module(&source, &mir, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "nested arrays mir lower: %s\n", err);
    MirAllocation allocation;
    mir_allocation_init(&allocation, &mir);
    memset(err, 0, sizeof(err));
    CHECK(mir_allocate(&mir, &allocation, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "nested arrays mir alloc: %s\n", err);

    char *spill_assembly = NULL;
    size_t spill_length = 0;
    FILE *spill_out = open_memstream(&spill_assembly, &spill_length);
    CHECK(spill_out != NULL);
    memset(err, 0, sizeof(err));
    CHECK(bir_x86_64_emit(&mir, spill_out, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "nested arrays spill emit: %s\n", err);
    fclose(spill_out);
    CHECK(spill_assembly && strstr(spill_assembly, "call make_matrix") != NULL);
    CHECK(spill_assembly && strstr(spill_assembly, "movq 0(%r11), %rax") != NULL);

    char *allocated_assembly = NULL;
    size_t allocated_length = 0;
    FILE *allocated_out = open_memstream(&allocated_assembly, &allocated_length);
    CHECK(allocated_out != NULL);
    CHECK(bir_x86_64_emit_allocated(&mir, &allocation, allocated_out,
                                    err, sizeof(err)));
    if (err[0]) fprintf(stderr, "nested arrays allocated emit: %s\n", err);
    fclose(allocated_out);
    CHECK(allocated_assembly && strstr(allocated_assembly, "call make_matrix") != NULL);

    const char *spill_path = getenv("BIR_X86_NESTED_SPILL_ASM_OUT");
    if (spill_path && spill_assembly) {
        FILE *file = fopen(spill_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(spill_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    const char *allocated_path = getenv("BIR_X86_NESTED_ALLOC_ASM_OUT");
    if (allocated_path && allocated_assembly) {
        FILE *file = fopen(allocated_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(allocated_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    free(spill_assembly);
    free(allocated_assembly);
    mir_allocation_free(&allocation);
    mir_module_free(&mir);
    bir_module_free(&source);
    ast_free(program);
}

/* ------------------------------------------------------------------ */
/* Fixed arrays of value-owned structs: whole-struct element writes,   */
/* reads, struct returns, array parameters, struct fields, and both    */
/* native emitters.                                                   */
/* ------------------------------------------------------------------ */

static void test_x86_native_struct_arrays(void) {
    const char *source_text =
        "struct Point: { x: i64, y: i64 }\n"
        "def make_point(v: i64) -> Point: {\n"
        "  let p: Point\n"
        "  p.x = v\n"
        "  p.y = v * 2\n"
        "  return p\n"
        "}\n"
        "def sum_points(pts: array[Point, 2]) -> i64: {\n"
        "  let a: Point = pts[0]\n"
        "  let b: Point = pts[1]\n"
        "  return a.x + b.y\n"
        "}\n"
        "def struct_array_main() -> i64: {\n"
        "  let pts: array[Point, 2]\n"
        "  pts[0] = make_point(1)\n"
        "  pts[1] = make_point(3)\n"
        "  let q: Point = pts[1]\n"
        "  let r: Point = pts[0]\n"
        "  return q.x + q.y + r.x + r.y\n"
        "}\n"
        "def call_points() -> i64: {\n"
        "  let pts: array[Point, 2]\n"
        "  let p: Point\n"
        "  p.x = 2\n"
        "  p.y = 3\n"
        "  pts[0] = p\n"
        "  p.x = 4\n"
        "  p.y = 5\n"
        "  pts[1] = p\n"
        "  return sum_points(pts)\n"
        "}\n"
        "def make_points() -> array[Point, 2]: {\n"
        "  let out: array[Point, 2]\n"
        "  let p: Point\n"
        "  p.x = 5\n"
        "  p.y = 6\n"
        "  out[0] = p\n"
        "  p.x = 7\n"
        "  p.y = 8\n"
        "  out[1] = p\n"
        "  return out\n"
        "}\n"
        "def return_points() -> i64: {\n"
        "  let pts: array[Point, 2] = make_points()\n"
        "  let a: Point = pts[0]\n"
        "  let b: Point = pts[1]\n"
        "  return a.y + b.y\n"
        "}\n"
        "struct Board: { cells: array[Point, 2], scale: i64 }\n"
        "def board_main() -> i64: {\n"
        "  let b: Board\n"
        "  let a: array[Point, 2]\n"
        "  let p1: Point\n"
        "  p1.x = 4\n"
        "  p1.y = 5\n"
        "  a[0] = p1\n"
        "  let p2: Point\n"
        "  p2.x = 6\n"
        "  p2.y = 7\n"
        "  a[1] = p2\n"
        "  b.cells = a\n"
        "  b.scale = 10\n"
        "  let c: array[Point, 2] = b.cells\n"
        "  let q: Point = c[1]\n"
        "  return q.x + q.y + b.scale\n"
        "}\n";
    ASTNode *program = parse_program(source_text);
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule source;
    bir_module_init(&source, "<unit:x86-native-struct-arrays>");
    char err[512] = {0};
    CHECK(bir_build_program(&source, program));
    if (source.error[0]) fprintf(stderr, "struct arrays build: %s\n", source.error);
    CHECK(bir_verify(&source, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "struct arrays verify: %s\n", err);
    BirScalarValue result = {0};
    CHECK(bir_eval_function_value(&source, "struct_array_main", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 12);
    CHECK(bir_eval_function_value(&source, "call_points", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 7);
    CHECK(bir_eval_function_value(&source, "return_points", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 14);
    CHECK(bir_eval_function_value(&source, "board_main", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 23);

    /* Arrays of owning structs must stay rejected at the isolated boundary:
       an owning struct element has no value-copy semantics. */
    ASTNode *bad = parse_program(
        "struct Owning: { text: string }\n"
        "def bad_main() -> i64: {\n"
        "  let bad: array[Owning, 2]\n"
        "  return 0\n"
        "}\n");
    CHECK(bad != NULL);
    if (bad) {
        BackendIrModule bad_module;
        bir_module_init(&bad_module, "<unit:struct-array-owning-reject>");
        CHECK(!bir_build_program(&bad_module, bad));
        CHECK(bad_module.error[0] != '\0');
        bir_module_free(&bad_module);
        ast_free(bad);
    }

    MirModule mir;
    memset(&mir, 0, sizeof(mir));
    memset(err, 0, sizeof(err));
    CHECK(mir_lower_module(&source, &mir, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "struct arrays mir lower: %s\n", err);
    MirAllocation allocation;
    mir_allocation_init(&allocation, &mir);
    memset(err, 0, sizeof(err));
    CHECK(mir_allocate(&mir, &allocation, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "struct arrays mir alloc: %s\n", err);

    char *spill_assembly = NULL;
    size_t spill_length = 0;
    FILE *spill_out = open_memstream(&spill_assembly, &spill_length);
    CHECK(spill_out != NULL);
    memset(err, 0, sizeof(err));
    CHECK(bir_x86_64_emit(&mir, spill_out, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "struct arrays spill emit: %s\n", err);
    fclose(spill_out);
    CHECK(spill_assembly && strstr(spill_assembly, "call make_point") != NULL);
    CHECK(spill_assembly && strstr(spill_assembly, "call sum_points") != NULL);

    char *allocated_assembly = NULL;
    size_t allocated_length = 0;
    FILE *allocated_out = open_memstream(&allocated_assembly, &allocated_length);
    CHECK(allocated_out != NULL);
    CHECK(bir_x86_64_emit_allocated(&mir, &allocation, allocated_out,
                                    err, sizeof(err)));
    if (err[0]) fprintf(stderr, "struct arrays allocated emit: %s\n", err);
    fclose(allocated_out);
    CHECK(allocated_assembly && strstr(allocated_assembly, "call make_point") != NULL);
    CHECK(allocated_assembly && strstr(allocated_assembly, "call sum_points") != NULL);

    const char *spill_path = getenv("BIR_X86_STRUCT_ARR_SPILL_ASM_OUT");
    if (spill_path && spill_assembly) {
        FILE *file = fopen(spill_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(spill_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    const char *allocated_path = getenv("BIR_X86_STRUCT_ARR_ALLOC_ASM_OUT");
    if (allocated_path && allocated_assembly) {
        FILE *file = fopen(allocated_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(allocated_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    free(spill_assembly);
    free(allocated_assembly);
    mir_allocation_free(&allocation);
    mir_module_free(&mir);
    bir_module_free(&source);
    ast_free(program);
}

/* ------------------------------------------------------------------ */
/* Scalar-list buffers of value-only structs: literal construction,    */
/* append, index reads and writes, ownership-moving calls, free, and   */
/* both native emitters. Pop of non-scalar elements is rejected.       */
/* ------------------------------------------------------------------ */

static void test_x86_native_struct_buffers(void) {
    const char *source_text =
        "struct Point: { x: i64, y: i64 }\n"
        "def struct_buffer_main() -> i64: {\n"
        "  let p1: Point\n"
        "  p1.x = 1\n"
        "  p1.y = 2\n"
        "  let p2: Point\n"
        "  p2.x = 3\n"
        "  p2.y = 4\n"
        "  let pts: list[Point] = [p1, p2]\n"
        "  let q: Point = pts[1]\n"
        "  let sum = q.x + q.y\n"
        "  free(pts)\n"
        "  return sum\n"
        "}\n"
        "def struct_buffer_append() -> i64: {\n"
        "  let p1: Point\n"
        "  p1.x = 5\n"
        "  p1.y = 6\n"
        "  let pts: list[Point] = [p1]\n"
        "  let p2: Point\n"
        "  p2.x = 7\n"
        "  p2.y = 8\n"
        "  append(pts, p2)\n"
        "  let q: Point = pts[1]\n"
        "  let sum = q.x + q.y\n"
        "  free(pts)\n"
        "  return sum\n"
        "}\n"
        "def struct_buffer_write() -> i64: {\n"
        "  let p1: Point\n"
        "  p1.x = 1\n"
        "  p1.y = 1\n"
        "  let pts: list[Point] = [p1, p1]\n"
        "  let p2: Point\n"
        "  p2.x = 9\n"
        "  p2.y = 10\n"
        "  pts[0] = p2\n"
        "  let q: Point = pts[0]\n"
        "  let sum = q.x + q.y\n"
        "  free(pts)\n"
        "  return sum\n"
        "}\n"
        "def take_first(pts: list[Point]) -> Point: {\n"
        "  let q: Point = pts[0]\n"
        "  free(pts)\n"
        "  return q\n"
        "}\n"
        "def struct_buffer_call() -> i64: {\n"
        "  let p1: Point\n"
        "  p1.x = 11\n"
        "  p1.y = 12\n"
        "  let pts: list[Point] = [p1]\n"
        "  let q: Point = take_first(pts)\n"
        "  return q.x + q.y\n"
        "}\n"
        "def struct_buffer_roundtrip() -> i64: {\n"
        "  let p1: Point\n"
        "  p1.x = 20\n"
        "  p1.y = 22\n"
        "  let pts: list[Point] = [p1]\n"
        "  let out: list[Point] = take_all(pts)\n"
        "  let q: Point = out[0]\n"
        "  free(out)\n"
        "  return q.x + q.y\n"
        "}\n"
        "def take_all(pts: list[Point]) -> list[Point]: {\n"
        "  return pts\n"
        "}\n";
    ASTNode *program = parse_program(source_text);
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule source;
    bir_module_init(&source, "<unit:x86-native-struct-buffers>");
    char err[512] = {0};
    CHECK(bir_build_program(&source, program));
    if (source.error[0]) fprintf(stderr, "struct buffers build: %s\n", source.error);
    CHECK(bir_verify(&source, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "struct buffers verify: %s\n", err);
    BirScalarValue result = {0};
    CHECK(bir_eval_function_value(&source, "struct_buffer_main", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 7);
    CHECK(bir_eval_function_value(&source, "struct_buffer_append", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 15);
    CHECK(bir_eval_function_value(&source, "struct_buffer_write", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 19);
    CHECK(bir_eval_function_value(&source, "struct_buffer_call", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 23);
    CHECK(bir_eval_function_value(&source, "struct_buffer_roundtrip", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 42);

    /* Pop of non-scalar buffer elements stays rejected at the HIR boundary. */
    ASTNode *bad = parse_program(
        "struct Point: { x: i64, y: i64 }\n"
        "def bad_pop() -> i64: {\n"
        "  let p1: Point\n"
        "  p1.x = 1\n"
        "  p1.y = 2\n"
        "  let pts: list[Point] = [p1]\n"
        "  let q: Point = pop(pts)\n"
        "  free(pts)\n"
        "  return 0\n"
        "}\n");
    CHECK(bad != NULL);
    if (bad) {
        BackendIrModule bad_module;
        bir_module_init(&bad_module, "<unit:struct-buffer-pop-reject>");
        CHECK(!bir_build_program(&bad_module, bad));
        CHECK(strstr(bad_module.error, "pop of non-scalar") != NULL);
        bir_module_free(&bad_module);
        ast_free(bad);
    }

    /* Buffers of owning structs stay rejected at the isolated boundary. */
    ASTNode *bad_owning = parse_program(
        "struct Owning: { text: string }\n"
        "def bad_owning() -> i64: {\n"
        "  let bad: list[Owning] = []\n"
        "  return 0\n"
        "}\n");
    CHECK(bad_owning != NULL);
    if (bad_owning) {
        BackendIrModule bad_module;
        bir_module_init(&bad_module, "<unit:struct-buffer-owning-reject>");
        CHECK(!bir_build_program(&bad_module, bad_owning));
        CHECK(bad_module.error[0] != '\0');
        bir_module_free(&bad_module);
        ast_free(bad_owning);
    }

    MirModule mir;
    memset(&mir, 0, sizeof(mir));
    memset(err, 0, sizeof(err));
    CHECK(mir_lower_module(&source, &mir, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "struct buffers mir lower: %s\n", err);
    MirAllocation allocation;
    mir_allocation_init(&allocation, &mir);
    memset(err, 0, sizeof(err));
    CHECK(mir_allocate(&mir, &allocation, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "struct buffers mir alloc: %s\n", err);

    char *spill_assembly = NULL;
    size_t spill_length = 0;
    FILE *spill_out = open_memstream(&spill_assembly, &spill_length);
    CHECK(spill_out != NULL);
    memset(err, 0, sizeof(err));
    CHECK(bir_x86_64_emit(&mir, spill_out, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "struct buffers spill emit: %s\n", err);
    fclose(spill_out);
    CHECK(spill_assembly && strstr(spill_assembly, "call malloc@PLT") != NULL);
    CHECK(spill_assembly && strstr(spill_assembly, "call take_first") != NULL);

    char *allocated_assembly = NULL;
    size_t allocated_length = 0;
    FILE *allocated_out = open_memstream(&allocated_assembly, &allocated_length);
    CHECK(allocated_out != NULL);
    CHECK(bir_x86_64_emit_allocated(&mir, &allocation, allocated_out,
                                    err, sizeof(err)));
    if (err[0]) fprintf(stderr, "struct buffers allocated emit: %s\n", err);
    fclose(allocated_out);
    CHECK(allocated_assembly && strstr(allocated_assembly, "call malloc@PLT") != NULL);
    CHECK(allocated_assembly && strstr(allocated_assembly, "call take_first") != NULL);

    const char *spill_path = getenv("BIR_X86_STRUCT_BUF_SPILL_ASM_OUT");
    if (spill_path && spill_assembly) {
        FILE *file = fopen(spill_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(spill_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    const char *allocated_path = getenv("BIR_X86_STRUCT_BUF_ALLOC_ASM_OUT");
    if (allocated_path && allocated_assembly) {
        FILE *file = fopen(allocated_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(allocated_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    free(spill_assembly);
    free(allocated_assembly);

    /* Tampering with a 3-part buffer ABI move (invalid part count) must be
       rejected by the MIR verifier before emission. */
    bool corrupted = false;
    for (size_t i = 1; i < mir.arena.inst_count && !corrupted; i++) {
        MirInst *inst = &mir.arena.insts[i];
        if (inst->op == MIR_OP_ABI_MOVE &&
            mir.arena.regs[inst->result].machine_type == MIR_TYPE_VIEW &&
            inst->abi_locations.count == 3) {
            inst->abi_locations.count = 4;
            corrupted = true;
        }
    }
    CHECK(corrupted);
    memset(err, 0, sizeof(err));
    CHECK(!mir_verify(&mir, err, sizeof(err)));
    CHECK(strstr(err, "ABI move") != NULL);
    mir_allocation_free(&allocation);
    mir_module_free(&mir);
    bir_module_free(&source);
    ast_free(program);
}

static void test_x86_native_dicts(void) {
    const char *source_text =
        "def dict_ops() -> i64: {\n"
        "  let d: dict[string]i64 = {\"a\": 1, \"b\": 2}\n"
        "  let x: i64 = get(d, \"a\", 0)\n"
        "  let y: i64 = get(d, \"z\", 7)\n"
        "  let h: i64 = has(d, \"b\")\n"
        "  let n: i64 = len(d)\n"
        "  set(d, \"c\", 3)\n"
        "  let n2: i64 = len(d)\n"
        "  let c: i64 = get(d, \"c\", 0)\n"
        "  delete(d, \"a\")\n"
        "  let a: i64 = get(d, \"a\", 9)\n"
        "  let p: i64 = pop(d, \"b\", 5)\n"
        "  let b: i64 = get(d, \"b\", 5)\n"
        "  free(d)\n"
        "  return x * 100000 + y * 10000 + h * 1000 + n * 100 + n2 * 10 + c\n"
        "}\n"
        "def dict_index() -> i64: {\n"
        "  let d: dict[string]i64 = {\"a\": 1, \"b\": 2}\n"
        "  let x: i64 = d[\"a\"]\n"
        "  d[\"c\"] = 3\n"
        "  let y: i64 = d[\"c\"]\n"
        "  let n: i64 = len(d)\n"
        "  free(d)\n"
        "  return x * 1000 + y * 100 + n * 10\n"
        "}\n"
        "def dict_pass(d: dict[string]i64) -> i64: {\n"
        "  let x: i64 = get(d, \"a\", 0)\n"
        "  set(d, \"q\", 9)\n"
        "  let n: i64 = len(d)\n"
        "  free(d)\n"
        "  return x * 100 + n\n"
        "}\n"
        "def dict_call() -> i64: {\n"
        "  let d: dict[string]i64 = {\"a\": 1, \"b\": 2}\n"
        "  let r: i64 = dict_pass(d)\n"
        "  return r\n"
        "}\n"
        "def dict_rehash() -> i64: {\n"
        "  let d: dict[string]i64 = {\"k0\": 0, \"k1\": 1, \"k2\": 2, \"k3\": 3, \"k4\": 4, \"k5\": 5, \"k6\": 6, \"k7\": 7, \"k8\": 8}\n"
        "  let s: i64 = d[\"k0\"] + d[\"k1\"] + d[\"k2\"] + d[\"k3\"] + d[\"k4\"]\n"
        "  s = s + d[\"k5\"] + d[\"k6\"] + d[\"k7\"] + d[\"k8\"]\n"
        "  free(d)\n"
        "  return s\n"
        "}\n";
    ASTNode *program = parse_program(source_text);
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule source;
    bir_module_init(&source, "<unit:x86-native-dicts>");
    char err[512] = {0};
    CHECK(bir_build_program(&source, program));
    if (source.error[0]) fprintf(stderr, "dicts build: %s\n", source.error);
    CHECK(bir_verify(&source, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "dicts verify: %s\n", err);
    BirScalarValue result = {0};
    CHECK(bir_eval_function_value(&source, "dict_ops", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 171233);
    CHECK(bir_eval_function_value(&source, "dict_index", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 1330);
    CHECK(bir_eval_function_value(&source, "dict_call", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 103);
    CHECK(bir_eval_function_value(&source, "dict_rehash", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 36);

    /* A non-literal dict key is rejected at the HIR boundary. */
    ASTNode *bad_key = parse_program(
        "def bad_key(d: dict[string]i64) -> i64: {\n"
        "  let k: string = \"a\"\n"
        "  let x: i64 = get(d, k, 0)\n"
        "  free(d)\n"
        "  return x\n"
        "}\n");
    CHECK(bad_key != NULL);
    if (bad_key) {
        BackendIrModule bad_module;
        bir_module_init(&bad_module, "<unit:dict-bad-key>");
        CHECK(!bir_build_program(&bad_module, bad_key));
        CHECK(strstr(bad_module.error, "string literals") != NULL);
        bir_module_free(&bad_module);
        ast_free(bad_key);
    }

    /* Dicts observe the same ownership rules as owned slices. */
    unit_rejected("owned dict double free",
        "def main() -> i64: {\n"
        "  let d: dict[string]i64 = {\"a\": 1}\n"
        "  free(d)\n"
        "  free(d)\n"
        "  return 0\n"
        "}\n");
    unit_rejected("owned dict use after free",
        "def main() -> i64: {\n"
        "  let d: dict[string]i64 = {\"a\": 1}\n"
        "  free(d)\n"
        "  return len(d)\n"
        "}\n");
    unit_rejected("owned dict get after free",
        "def main() -> i64: {\n"
        "  let d: dict[string]i64 = {\"a\": 1}\n"
        "  free(d)\n"
        "  return get(d, \"a\", 0)\n"
        "}\n");
    unit_rejected("owned dict set after free",
        "def main() -> i64: {\n"
        "  let d: dict[string]i64 = {\"a\": 1}\n"
        "  free(d)\n"
        "  set(d, \"b\", 2)\n"
        "  return 0\n"
        "}\n");

    MirModule mir;
    memset(&mir, 0, sizeof(mir));
    memset(err, 0, sizeof(err));
    CHECK(mir_lower_module(&source, &mir, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "dicts mir lower: %s\n", err);
    MirAllocation allocation;
    mir_allocation_init(&allocation, &mir);
    memset(err, 0, sizeof(err));
    CHECK(mir_allocate(&mir, &allocation, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "dicts mir alloc: %s\n", err);

    char *spill_assembly = NULL;
    size_t spill_length = 0;
    FILE *spill_out = open_memstream(&spill_assembly, &spill_length);
    CHECK(spill_out != NULL);
    memset(err, 0, sizeof(err));
    CHECK(bir_x86_64_emit(&mir, spill_out, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "dicts spill emit: %s\n", err);
    fclose(spill_out);
    CHECK(spill_assembly && strstr(spill_assembly, "call cobra_dict_set_i64@PLT") != NULL);
    CHECK(spill_assembly && strstr(spill_assembly, "call cobra_dict_get_i64@PLT") != NULL);
    CHECK(spill_assembly && strstr(spill_assembly, "call cobra_dict_free@PLT") != NULL);

    char *allocated_assembly = NULL;
    size_t allocated_length = 0;
    FILE *allocated_out = open_memstream(&allocated_assembly, &allocated_length);
    CHECK(allocated_out != NULL);
    CHECK(bir_x86_64_emit_allocated(&mir, &allocation, allocated_out,
                                    err, sizeof(err)));
    if (err[0]) fprintf(stderr, "dicts allocated emit: %s\n", err);
    fclose(allocated_out);
    CHECK(allocated_assembly && strstr(allocated_assembly, "call cobra_dict_set_i64@PLT") != NULL);
    CHECK(allocated_assembly && strstr(allocated_assembly, "call cobra_dict_pop@PLT") != NULL);

    /* Tampering with a dict op's key metadata must be rejected by the MIR
       verifier before emission. */
    bool corrupted = false;
    for (size_t i = 1; i < mir.arena.inst_count && !corrupted; i++) {
        MirInst *inst = &mir.arena.insts[i];
        if (inst->op == MIR_OP_DICT_GET) {
            inst->dict_key[0] = '\0';
            corrupted = true;
        }
    }
    CHECK(corrupted);
    memset(err, 0, sizeof(err));
    CHECK(!mir_verify(&mir, err, sizeof(err)));
    CHECK(strstr(err, "dict get") != NULL);

    const char *spill_path = getenv("BIR_X86_DICT_SPILL_ASM_OUT");
    if (spill_path && spill_assembly) {
        FILE *file = fopen(spill_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(spill_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    const char *allocated_path = getenv("BIR_X86_DICT_ALLOC_ASM_OUT");
    if (allocated_path && allocated_assembly) {
        FILE *file = fopen(allocated_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(allocated_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    free(spill_assembly);
    free(allocated_assembly);
    mir_allocation_free(&allocation);
    mir_module_free(&mir);
    bir_module_free(&source);
    ast_free(program);
}

static void test_source_struct_sum_payloads(void) {
    const char *source_text =
        "struct Point: { x: i64, y: i64 }\n"
        "def make_point() -> Point: {\n"
        "  let p: Point\n"
        "  p.x = 3\n"
        "  p.y = 4\n"
        "  return p\n"
        "}\n"
        "def sum_struct() -> i64: {\n"
        "  let p: Point = make_point()\n"
        "  let o: Option[Point] = some(p)\n"
        "  if is_some(o): {} else: { return 999 }\n"
        "  let q: Point = unwrap(o)\n"
        "  return q.x * 100 + q.y * 10\n"
        "}\n"
        "def sum_result() -> i64: {\n"
        "  let p: Point\n"
        "  p.x = 5\n"
        "  p.y = 6\n"
        "  let r: Result[Point, i64] = ok(p)\n"
        "  if is_ok(r): {} else: { return 999 }\n"
        "  let q: Point = unwrap_ok(r)\n"
        "  return q.x * 10 + q.y\n"
        "}\n"
        "def sum_none() -> i64: {\n"
        "  let o: Option[Point] = none\n"
        "  if is_some(o): { return 1 }\n"
        "  return 0\n"
        "}\n"
        "def sum_param(o: Option[Point]) -> i64: {\n"
        "  if is_some(o): {} else: { return 999 }\n"
        "  let q: Point = unwrap(o)\n"
        "  return q.x * 10 + q.y\n"
        "}\n"
        "def sum_call() -> i64: {\n"
        "  let p: Point\n"
        "  p.x = 7\n"
        "  p.y = 8\n"
        "  let o: Option[Point] = some(p)\n"
        "  return sum_param(o)\n"
        "}\n"
        "def sum_err() -> i64: {\n"
        "  let p: Point\n"
        "  p.x = 1\n"
        "  p.y = 2\n"
        "  let r: Result[Point, i64] = err(77)\n"
        "  if is_ok(r): { return 999 }\n"
        "  let e: i64 = unwrap_err(r)\n"
        "  return e\n"
        "}\n"
        "def sum_nested() -> i64: {\n"
        "  let p: Point\n"
        "  p.x = 1\n"
        "  p.y = 2\n"
        "  let o: Option[Option[Point]] = some(some(p))\n"
        "  if is_some(o): {} else: { return 999 }\n"
        "  let inner: Option[Point] = unwrap(o)\n"
        "  if is_some(inner): {} else: { return 999 }\n"
        "  let q: Point = unwrap(inner)\n"
        "  return q.x + q.y\n"
        "}\n";
    ASTNode *program = parse_program(source_text);
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule source;
    bir_module_init(&source, "<unit:source-struct-sum-payloads>");
    char err[512] = {0};
    CHECK(bir_build_program(&source, program));
    if (source.error[0]) fprintf(stderr, "struct sum build: %s\n", source.error);
    CHECK(bir_verify(&source, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "struct sum verify: %s\n", err);
    BirScalarValue result = {0};
    CHECK(bir_eval_function_value(&source, "sum_struct", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 340);
    CHECK(bir_eval_function_value(&source, "sum_result", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 56);
    CHECK(bir_eval_function_value(&source, "sum_none", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 0);
    CHECK(bir_eval_function_value(&source, "sum_call", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 78);
    CHECK(bir_eval_function_value(&source, "sum_err", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 77);
    CHECK(bir_eval_function_value(&source, "sum_nested", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 3);

    /* Struct-payload sums with unsupported field contracts stay rejected at
       the HIR boundary: tensors are not yet backend values. */
    ASTNode *bad = parse_program(
        "struct TensorBox: { tensor: tensor[3]f32 }\n"
        "def bad() -> i64: {\n"
        "  let o: Option[TensorBox] = none\n"
        "  return 0\n"
        "}\n");
    CHECK(bad != NULL);
    if (bad) {
        BackendIrModule bad_module;
        bir_module_init(&bad_module, "<unit:struct-sum-owning-reject>");
        CHECK(!bir_build_program(&bad_module, bad));
        CHECK(bad_module.error[0] != '\0');
        bir_module_free(&bad_module);
        ast_free(bad);
    }

    MirModule mir;
    memset(&mir, 0, sizeof(mir));
    memset(err, 0, sizeof(err));
    CHECK(mir_lower_module(&source, &mir, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "struct sum mir lower: %s\n", err);
    MirAllocation allocation;
    mir_allocation_init(&allocation, &mir);
    memset(err, 0, sizeof(err));
    CHECK(mir_allocate(&mir, &allocation, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "struct sum mir alloc: %s\n", err);

    char *spill_assembly = NULL;
    size_t spill_length = 0;
    FILE *spill_out = open_memstream(&spill_assembly, &spill_length);
    CHECK(spill_out != NULL);
    memset(err, 0, sizeof(err));
    CHECK(bir_x86_64_emit(&mir, spill_out, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "struct sum spill emit: %s\n", err);
    fclose(spill_out);
    CHECK(spill_assembly && strstr(spill_assembly, "sum_fail") != NULL);
    CHECK(spill_assembly && strstr(spill_assembly, "make_point") != NULL);

    char *allocated_assembly = NULL;
    size_t allocated_length = 0;
    FILE *allocated_out = open_memstream(&allocated_assembly, &allocated_length);
    CHECK(allocated_out != NULL);
    CHECK(bir_x86_64_emit_allocated(&mir, &allocation, allocated_out,
                                    err, sizeof(err)));
    if (err[0]) fprintf(stderr, "struct sum allocated emit: %s\n", err);
    fclose(allocated_out);
    CHECK(allocated_assembly && strstr(allocated_assembly, "sum_fail") != NULL);
    CHECK(allocated_assembly && strstr(allocated_assembly, "make_point") != NULL);

    const char *spill_path = getenv("BIR_X86_SUM_STRUCT_SPILL_ASM_OUT");
    if (spill_path && spill_assembly) {
        FILE *file = fopen(spill_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(spill_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    const char *allocated_path = getenv("BIR_X86_SUM_STRUCT_ALLOC_ASM_OUT");
    if (allocated_path && allocated_assembly) {
        FILE *file = fopen(allocated_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(allocated_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    free(spill_assembly);
    free(allocated_assembly);
    mir_allocation_free(&allocation);
    mir_module_free(&mir);
    bir_module_free(&source);
    ast_free(program);
}

static void test_source_owning_sum_payloads(void) {
    const char *source_text =
        "struct Owning: { text: string, value: i64 }\n"
        "def make_owning() -> Owning: {\n"
        "  let o: Owning\n"
        "  o.text = concat(\"co\", \"bra\")\n"
        "  o.value = 40\n"
        "  return o\n"
        "}\n"
        "def sum_owning() -> i64: {\n"
        "  let o: Option[Owning] = some(make_owning())\n"
        "  if is_some(o): {} else: { return 999 }\n"
        "  let q: Owning = unwrap(o)\n"
        "  let n: i64 = len(q.text)\n"
        "  let v: i64 = q.value\n"
        "  free(q)\n"
        "  free(o)\n"
        "  return n + v\n"
        "}\n"
        "def sum_owning_param() -> i64: {\n"
        "  let o: Owning\n"
        "  o.text = concat(\"ab\", \"cd\")\n"
        "  o.value = 7\n"
        "  let s: Option[Owning] = some(o)\n"
        "  return consume(s)\n"
        "}\n"
        "def consume(o: Option[Owning]) -> i64: {\n"
        "  if is_some(o): {} else: { return 999 }\n"
        "  let q: Owning = unwrap(o)\n"
        "  let n: i64 = len(q.text)\n"
        "  let v: i64 = q.value\n"
        "  free(q)\n"
        "  free(o)\n"
        "  return n + v\n"
        "}\n"
        "def sum_result_owning() -> i64: {\n"
        "  let o: Owning\n"
        "  o.text = concat(\"x\", \"yz\")\n"
        "  let r: Result[Owning, i64] = ok(o)\n"
        "  if is_ok(r): {} else: { return 999 }\n"
        "  let q: Owning = unwrap_ok(r)\n"
        "  let n: i64 = len(q.text)\n"
        "  free(q)\n"
        "  free(r)\n"
        "  return n + 1\n"
        "}\n"
        "def sum_owning_none() -> i64: {\n"
        "  let o: Option[Owning] = none\n"
        "  if is_some(o): { return 1 }\n"
        "  return 0\n"
        "}\n"
        "def sum_nested_owning() -> i64: {\n"
        "  let o: Owning\n"
        "  o.text = concat(\"a\", \"b\")\n"
        "  let inner: Option[Owning] = some(o)\n"
        "  let outer: Option[Option[Owning]] = some(inner)\n"
        "  if is_some(outer): {} else: { return 999 }\n"
        "  let mid: Option[Owning] = unwrap(outer)\n"
        "  if is_some(mid): {} else: { return 999 }\n"
        "  let q: Owning = unwrap(mid)\n"
        "  let n: i64 = len(q.text)\n"
        "  free(q)\n"
        "  free(mid)\n"
        "  free(outer)\n"
        "  return n\n"
        "}\n";
    ASTNode *program = parse_program(source_text);
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule source;
    bir_module_init(&source, "<unit:source-owning-sum-payloads>");
    char err[512] = {0};
    CHECK(bir_build_program(&source, program));
    if (source.error[0]) fprintf(stderr, "owning sum build: %s\n", source.error);
    CHECK(bir_verify(&source, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "owning sum verify: %s\n", err);
    BirScalarValue result = {0};
    CHECK(bir_eval_function_value(&source, "sum_owning", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 45);
    CHECK(bir_eval_function_value(&source, "sum_owning_param", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 11);
    CHECK(bir_eval_function_value(&source, "sum_result_owning", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 4);
    CHECK(bir_eval_function_value(&source, "sum_owning_none", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 0);
    CHECK(bir_eval_function_value(&source, "sum_nested_owning", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 2);

    /* Unsupported field contracts inside owning struct payloads stay
       rejected at the HIR boundary: tensors are not backend values. */
    ASTNode *bad = parse_program(
        "struct TensorBox: { tensor: tensor[3]f32 }\n"
        "def bad() -> i64: {\n"
        "  let o: Option[TensorBox] = none\n"
        "  return 0\n"
        "}\n");
    CHECK(bad != NULL);
    if (bad) {
        BackendIrModule bad_module;
        bir_module_init(&bad_module, "<unit:owning-sum-tensor-reject>");
        CHECK(!bir_build_program(&bad_module, bad));
        CHECK(bad_module.error[0] != '\0');
        bir_module_free(&bad_module);
        ast_free(bad);
    }

    /* A second unwrap after the payload moved out sees `none` and fails:
       double extraction is rejected in the evaluator. */
    ASTNode *double_unwrap = parse_program(
        "struct Owning: { text: string }\n"
        "def bad() -> i64: {\n"
        "  let o: Owning\n"
        "  o.text = concat(\"a\", \"b\")\n"
        "  let s: Option[Owning] = some(o)\n"
        "  let q1: Owning = unwrap(s)\n"
        "  let q2: Owning = unwrap(s)\n"
        "  free(q1)\n"
        "  free(q2)\n"
        "  free(s)\n"
        "  return 0\n"
        "}\n");
    CHECK(double_unwrap != NULL);
    if (double_unwrap) {
        BackendIrModule bad_module;
        bir_module_init(&bad_module, "<unit:owning-sum-double-unwrap>");
        char bad_err[512] = {0};
        if (bir_build_program(&bad_module, double_unwrap)) {
            CHECK(!bir_verify(&bad_module, bad_err, sizeof(bad_err)) ||
                  !bir_eval_function_value(&bad_module, "bad", &result));
        } else {
            CHECK(bad_module.error[0] != '\0');
        }
        bir_module_free(&bad_module);
        ast_free(double_unwrap);
    }

    MirModule mir;
    memset(&mir, 0, sizeof(mir));
    memset(err, 0, sizeof(err));
    CHECK(mir_lower_module(&source, &mir, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "owning sum mir lower: %s\n", err);
    MirAllocation allocation;
    mir_allocation_init(&allocation, &mir);
    memset(err, 0, sizeof(err));
    CHECK(mir_allocate(&mir, &allocation, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "owning sum mir alloc: %s\n", err);

    char *spill_assembly = NULL;
    size_t spill_length = 0;
    FILE *spill_out = open_memstream(&spill_assembly, &spill_length);
    CHECK(spill_out != NULL);
    memset(err, 0, sizeof(err));
    CHECK(bir_x86_64_emit(&mir, spill_out, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "owning sum spill emit: %s\n", err);
    fclose(spill_out);
    CHECK(spill_assembly && strstr(spill_assembly, "sum_fail") != NULL);
    CHECK(spill_assembly && strstr(spill_assembly, "make_owning") != NULL);

    char *allocated_assembly = NULL;
    size_t allocated_length = 0;
    FILE *allocated_out = open_memstream(&allocated_assembly, &allocated_length);
    CHECK(allocated_out != NULL);
    CHECK(bir_x86_64_emit_allocated(&mir, &allocation, allocated_out,
                                    err, sizeof(err)));
    if (err[0]) fprintf(stderr, "owning sum allocated emit: %s\n", err);
    fclose(allocated_out);
    CHECK(allocated_assembly && strstr(allocated_assembly, "sum_fail") != NULL);
    CHECK(allocated_assembly && strstr(allocated_assembly, "make_owning") != NULL);

    const char *spill_path = getenv("BIR_X86_OWNING_SUM_SPILL_ASM_OUT");
    if (spill_path && spill_assembly) {
        FILE *file = fopen(spill_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(spill_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    const char *allocated_path = getenv("BIR_X86_OWNING_SUM_ALLOC_ASM_OUT");
    if (allocated_path && allocated_assembly) {
        FILE *file = fopen(allocated_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(allocated_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    free(spill_assembly);
    free(allocated_assembly);
    mir_allocation_free(&allocation);
    mir_module_free(&mir);
    bir_module_free(&source);
    ast_free(program);
}

static void test_source_nested_owning_structs(void) {
    const char *source_text =
        "struct Inner: { text: string }\n"
        "struct Mid: { inner: Inner, tag: i64 }\n"
        "struct Box: { payload: Option[Inner], value: i64 }\n"
        "struct Outer: { inner: Inner }\n"
        "def make_inner() -> Inner: {\n"
        "  let i: Inner\n"
        "  i.text = concat(\"a\", \"bc\")\n"
        "  return i\n"
        "}\n"
        "def consume_mid(m: Mid) -> i64: {\n"
        "  let n: i64 = len(m.inner.text)\n"
        "  let t: i64 = m.tag\n"
        "  free(m)\n"
        "  return n + t\n"
        "}\n"
        "def nested_struct() -> i64: {\n"
        "  let m: Mid\n"
        "  m.inner = make_inner()\n"
        "  m.tag = 7\n"
        "  return consume_mid(m)\n"
        "}\n"
        "def consume_box(b: Box) -> i64: {\n"
        "  if is_some(b.payload): {} else: { return 999 }\n"
        "  let q: Inner = unwrap(b.payload)\n"
        "  let n: i64 = len(q.text)\n"
        "  let v: i64 = b.value\n"
        "  free(q)\n"
        "  free(b)\n"
        "  return n + v\n"
        "}\n"
        "def nested_sum_struct() -> i64: {\n"
        "  let i: Inner\n"
        "  i.text = concat(\"x\", \"yz\")\n"
        "  let b: Box\n"
        "  b.payload = some(i)\n"
        "  b.value = 5\n"
        "  return consume_box(b)\n"
        "}\n"
        "def nested_field_store() -> i64: {\n"
        "  let o: Outer\n"
        "  o.inner = make_inner()\n"
        "  let n: i64 = len(o.inner.text)\n"
        "  free(o)\n"
        "  return n\n"
        "}\n"
        "def struct_return() -> i64: {\n"
        "  let o: Inner = make_inner()\n"
        "  let n: i64 = len(o.text)\n"
        "  free(o)\n"
        "  return n\n"
        "}\n";
    ASTNode *program = parse_program(source_text);
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule source;
    bir_module_init(&source, "<unit:source-nested-owning-structs>");
    char err[512] = {0};
    CHECK(bir_build_program(&source, program));
    if (source.error[0]) fprintf(stderr, "nested owning struct build: %s\n", source.error);
    CHECK(bir_verify(&source, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "nested owning struct verify: %s\n", err);
    BirScalarValue result = {0};
    CHECK(bir_eval_function_value(&source, "nested_struct", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 10);
    CHECK(bir_eval_function_value(&source, "nested_sum_struct", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 8);
    CHECK(bir_eval_function_value(&source, "nested_field_store", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 3);
    CHECK(bir_eval_function_value(&source, "struct_return", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 3);

    /* A predicate on a member sum reads the tag in place: `is_some` does
       not consume `b.payload`, so the following `unwrap` still owns it. */
    ASTNode *predicate = parse_program(
        "struct Inner: { text: string }\n"
        "struct Box: { payload: Option[Inner] }\n"
        "def predicate_borrow() -> i64: {\n"
        "  let i: Inner\n"
        "  i.text = concat(\"a\", \"b\")\n"
        "  let b: Box\n"
        "  b.payload = some(i)\n"
        "  if is_some(b.payload): {} else: { return 999 }\n"
        "  let q: Inner = unwrap(b.payload)\n"
        "  let n: i64 = len(q.text)\n"
        "  free(q)\n"
        "  free(b)\n"
        "  return n\n"
        "}\n");
    CHECK(predicate != NULL);
    if (predicate) {
        BackendIrModule pred_module;
        bir_module_init(&pred_module, "<unit:nested-predicate-borrow>");
        char pred_err[512] = {0};
        CHECK(bir_build_program(&pred_module, predicate));
        CHECK(bir_verify(&pred_module, pred_err, sizeof(pred_err)));
        BirScalarValue pred_result = {0};
        CHECK(bir_eval_function_value(&pred_module, "predicate_borrow", &pred_result));
        CHECK(pred_result.kind == BIR_SCALAR_I64 && pred_result.payload.i64 == 2);
        bir_module_free(&pred_module);
        ast_free(predicate);
    }

    /* Nested owning structs keep the ownership edges: use-after-free,
       double free, post-call use, and payload reads after a move are all
       rejected. */
    const char *rejections[] = {
        "struct Inner: { text: string }\n"
        "struct Outer: { inner: Inner }\n"
        "def bad() -> i64: {\n"
        "  let i: Inner\n"
        "  i.text = concat(\"a\", \"b\")\n"
        "  let o: Outer\n"
        "  o.inner = i\n"
        "  free(o)\n"
        "  let n: i64 = len(o.inner.text)\n"
        "  return n\n"
        "}\n",
        "struct Inner: { text: string }\n"
        "struct Outer: { inner: Inner }\n"
        "def bad() -> i64: {\n"
        "  let i: Inner\n"
        "  i.text = concat(\"a\", \"b\")\n"
        "  let o: Outer\n"
        "  o.inner = i\n"
        "  free(o)\n"
        "  free(o)\n"
        "  return 0\n"
        "}\n",
        "struct Inner: { text: string }\n"
        "def consume(o: Inner) -> i64: {\n"
        "  let n: i64 = len(o.text)\n"
        "  free(o)\n"
        "  return n\n"
        "}\n"
        "def bad() -> i64: {\n"
        "  let i: Inner\n"
        "  i.text = concat(\"a\", \"b\")\n"
        "  let r: i64 = consume(i)\n"
        "  let n: i64 = len(i.text)\n"
        "  return r + n\n"
        "}\n",
        "struct Inner: { text: string }\n"
        "struct Outer: { inner: Inner }\n"
        "def bad() -> i64: {\n"
        "  let i: Inner\n"
        "  i.text = concat(\"a\", \"b\")\n"
        "  let o: Outer\n"
        "  o.inner = i\n"
        "  let n: i64 = len(i.text)\n"
        "  free(o)\n"
        "  return n\n"
        "}\n",
    };
    for (size_t r = 0; r < sizeof(rejections) / sizeof(rejections[0]); r++) {
        ASTNode *bad = parse_program(rejections[r]);
        CHECK(bad != NULL);
        if (!bad) continue;
        BackendIrModule bad_module;
        bir_module_init(&bad_module, "<unit:nested-owning-struct-reject>");
        char bad_err[512] = {0};
        if (bir_build_program(&bad_module, bad)) {
            CHECK(!bir_verify(&bad_module, bad_err, sizeof(bad_err)) ||
                  !bir_eval_function_value(&bad_module, "bad", &result));
        } else {
            CHECK(bad_module.error[0] != '\0');
        }
        bir_module_free(&bad_module);
        ast_free(bad);
    }

    MirModule mir;
    memset(&mir, 0, sizeof(mir));
    memset(err, 0, sizeof(err));
    CHECK(mir_lower_module(&source, &mir, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "nested owning struct mir lower: %s\n", err);
    MirAllocation allocation;
    mir_allocation_init(&allocation, &mir);
    memset(err, 0, sizeof(err));
    CHECK(mir_allocate(&mir, &allocation, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "nested owning struct mir alloc: %s\n", err);

    char *spill_assembly = NULL;
    size_t spill_length = 0;
    FILE *spill_out = open_memstream(&spill_assembly, &spill_length);
    CHECK(spill_out != NULL);
    memset(err, 0, sizeof(err));
    CHECK(bir_x86_64_emit(&mir, spill_out, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "nested owning struct spill emit: %s\n", err);
    fclose(spill_out);
    CHECK(spill_assembly && strstr(spill_assembly, "nested_struct") != NULL);
    CHECK(spill_assembly && strstr(spill_assembly, "nested_sum_struct") != NULL);

    char *allocated_assembly = NULL;
    size_t allocated_length = 0;
    FILE *allocated_out = open_memstream(&allocated_assembly, &allocated_length);
    CHECK(allocated_out != NULL);
    CHECK(bir_x86_64_emit_allocated(&mir, &allocation, allocated_out,
                                    err, sizeof(err)));
    if (err[0]) fprintf(stderr, "nested owning struct allocated emit: %s\n", err);
    fclose(allocated_out);
    CHECK(allocated_assembly && strstr(allocated_assembly, "nested_struct") != NULL);
    CHECK(allocated_assembly && strstr(allocated_assembly, "nested_sum_struct") != NULL);

    const char *spill_path = getenv("BIR_X86_NESTED_STRUCT_SPILL_ASM_OUT");
    if (spill_path && spill_assembly) {
        FILE *file = fopen(spill_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(spill_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    const char *allocated_path = getenv("BIR_X86_NESTED_STRUCT_ALLOC_ASM_OUT");
    if (allocated_path && allocated_assembly) {
        FILE *file = fopen(allocated_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(allocated_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    free(spill_assembly);
    free(allocated_assembly);
    mir_allocation_free(&allocation);
    mir_module_free(&mir);
    bir_module_free(&source);
    ast_free(program);
}

static void test_source_sum_match(void) {
    const char *source_text =
        "struct Owning: { text: string, value: i64 }\n"
        "def match_some() -> i64: {\n"
        "  let o: Option[i64] = some(7)\n"
        "  let result: i64 = 0\n"
        "  match o: {\n"
        "    case some(x): { result = x + 1 }\n"
        "    case none: { result = 999 }\n"
        "  }\n"
        "  return result\n"
        "}\n"
        "def match_none() -> i64: {\n"
        "  let o: Option[i64] = none\n"
        "  let result: i64 = 0\n"
        "  match o: {\n"
        "    case some(x): { result = x }\n"
        "    case none: { result = 42 }\n"
        "  }\n"
        "  return result\n"
        "}\n"
        "def match_err() -> i64: {\n"
        "  let r: Result[i64, i64] = err(5)\n"
        "  let result: i64 = 0\n"
        "  match r: {\n"
        "    case ok(v): { result = v }\n"
        "    case err(e): { result = e * 10 }\n"
        "  }\n"
        "  return result\n"
        "}\n"
        "def match_owning() -> i64: {\n"
        "  let o: Owning\n"
        "  o.text = concat(\"co\", \"bra\")\n"
        "  o.value = 40\n"
        "  let s: Option[Owning] = some(o)\n"
        "  let result: i64 = 0\n"
        "  match s: {\n"
        "    case some(x): {\n"
        "      result = len(x.text) + x.value\n"
        "      free(x)\n"
        "    }\n"
        "    case none: { result = 999 }\n"
        "  }\n"
        "  return result\n"
        "}\n"
        "def match_else() -> i64: {\n"
        "  let o: Option[i64] = some(3)\n"
        "  let result: i64 = 0\n"
        "  match o: {\n"
        "    case some(x): { result = x }\n"
        "    else: { result = 99 }\n"
        "  }\n"
        "  return result\n"
        "}\n";
    ASTNode *program = parse_program(source_text);
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule source;
    bir_module_init(&source, "<unit:source-sum-match>");
    char err[512] = {0};
    CHECK(bir_build_program(&source, program));
    if (source.error[0]) fprintf(stderr, "sum match build: %s\n", source.error);
    CHECK(bir_verify(&source, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "sum match verify: %s\n", err);
    BirScalarValue result = {0};
    CHECK(bir_eval_function_value(&source, "match_some", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 8);
    CHECK(bir_eval_function_value(&source, "match_none", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 42);
    CHECK(bir_eval_function_value(&source, "match_err", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 50);
    CHECK(bir_eval_function_value(&source, "match_owning", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 45);
    CHECK(bir_eval_function_value(&source, "match_else", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 3);

    /* Malformed sum matches stay rejected at the HIR boundary: missing
       patterns without else, duplicate patterns, patterns of the wrong kind,
       binding rules, and non-sum targets. */
    const char *rejections[] = {
        "def bad() -> i64: {\n"
        "  let o: Option[i64] = some(1)\n"
        "  let result: i64 = 0\n"
        "  match o: {\n"
        "    case some(x): { result = x }\n"
        "  }\n"
        "  return result\n"
        "}\n",
        "def bad() -> i64: {\n"
        "  let o: Option[i64] = some(1)\n"
        "  let result: i64 = 0\n"
        "  match o: {\n"
        "    case some(x): { result = x }\n"
        "    case some(y): { result = y }\n"
        "    case none: { result = 0 }\n"
        "  }\n"
        "  return result\n"
        "}\n",
        "def bad() -> i64: {\n"
        "  let r: Result[i64, i64] = ok(1)\n"
        "  let result: i64 = 0\n"
        "  match r: {\n"
        "    case some(x): { result = x }\n"
        "    case none: { result = 0 }\n"
        "  }\n"
        "  return result\n"
        "}\n",
        "def bad() -> i64: {\n"
        "  let o: Option[i64] = none\n"
        "  let result: i64 = 0\n"
        "  match o: {\n"
        "    case some(x): { result = x }\n"
        "    case none(y): { result = y }\n"
        "  }\n"
        "  return result\n"
        "}\n",
        "def bad() -> i64: {\n"
        "  let o: Option[i64] = some(1)\n"
        "  let result: i64 = 0\n"
        "  match o: {\n"
        "    case some: { result = 1 }\n"
        "    case none: { result = 0 }\n"
        "  }\n"
        "  return result\n"
        "}\n",
        "def bad() -> i64: {\n"
        "  let v: i64 = 3\n"
        "  let result: i64 = 0\n"
        "  match v: {\n"
        "    case some(x): { result = x }\n"
        "    case none: { result = 0 }\n"
        "  }\n"
        "  return result\n"
        "}\n",
    };
    for (size_t r = 0; r < sizeof(rejections) / sizeof(rejections[0]); r++) {
        ASTNode *bad = parse_program(rejections[r]);
        CHECK(bad != NULL);
        if (!bad) continue;
        BackendIrModule bad_module;
        bir_module_init(&bad_module, "<unit:sum-match-reject>");
        if (bir_build_program(&bad_module, bad)) {
            CHECK(bad_module.error[0] == '\0');
        } else {
            CHECK(bad_module.error[0] != '\0');
        }
        bir_module_free(&bad_module);
        ast_free(bad);
    }

    MirModule mir;
    memset(&mir, 0, sizeof(mir));
    memset(err, 0, sizeof(err));
    CHECK(mir_lower_module(&source, &mir, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "sum match mir lower: %s\n", err);
    MirAllocation allocation;
    mir_allocation_init(&allocation, &mir);
    memset(err, 0, sizeof(err));
    CHECK(mir_allocate(&mir, &allocation, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "sum match mir alloc: %s\n", err);

    char *spill_assembly = NULL;
    size_t spill_length = 0;
    FILE *spill_out = open_memstream(&spill_assembly, &spill_length);
    CHECK(spill_out != NULL);
    memset(err, 0, sizeof(err));
    CHECK(bir_x86_64_emit(&mir, spill_out, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "sum match spill emit: %s\n", err);
    fclose(spill_out);
    CHECK(spill_assembly && strstr(spill_assembly, "match_some") != NULL);
    CHECK(spill_assembly && strstr(spill_assembly, "match_owning") != NULL);

    char *allocated_assembly = NULL;
    size_t allocated_length = 0;
    FILE *allocated_out = open_memstream(&allocated_assembly, &allocated_length);
    CHECK(allocated_out != NULL);
    CHECK(bir_x86_64_emit_allocated(&mir, &allocation, allocated_out,
                                    err, sizeof(err)));
    if (err[0]) fprintf(stderr, "sum match allocated emit: %s\n", err);
    fclose(allocated_out);
    CHECK(allocated_assembly && strstr(allocated_assembly, "match_some") != NULL);
    CHECK(allocated_assembly && strstr(allocated_assembly, "match_owning") != NULL);

    const char *spill_path = getenv("BIR_X86_SUM_MATCH_SPILL_ASM_OUT");
    if (spill_path && spill_assembly) {
        FILE *file = fopen(spill_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(spill_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    const char *allocated_path = getenv("BIR_X86_SUM_MATCH_ALLOC_ASM_OUT");
    if (allocated_path && allocated_assembly) {
        FILE *file = fopen(allocated_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(allocated_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    free(spill_assembly);
    free(allocated_assembly);
    mir_allocation_free(&allocation);
    mir_module_free(&mir);
    bir_module_free(&source);
    ast_free(program);
}

static void test_source_enum_payloads(void) {
    const char *source_text =
        "enum Shape: { Circle(f32), Rect(i64, i64), Line }\n"
        "def circle_case() -> i64: {\n"
        "  let s: Shape = Shape.Circle(2.5)\n"
        "  let r: i64 = 0\n"
        "  match s: {\n"
        "    case Shape.Circle(rad): { r = 100 }\n"
        "    case Shape.Rect(w, h): { r = (w + h) }\n"
        "    case Shape.Line: { r = 300 }\n"
        "  }\n"
        "  return r\n"
        "}\n"
        "def rect_case() -> i64: {\n"
        "  let s: Shape = Shape.Rect(3, 4)\n"
        "  let r: i64 = 0\n"
        "  match s: {\n"
        "    case Shape.Circle(rad): { r = 100 }\n"
        "    case Shape.Rect(w, h): { r = (w + h) }\n"
        "    case Shape.Line: { r = 300 }\n"
        "  }\n"
        "  return r\n"
        "}\n"
        "def line_case() -> i64: {\n"
        "  let s: Shape = Shape.Line\n"
        "  let r: i64 = 0\n"
        "  match s: {\n"
        "    case Shape.Circle(rad): { r = 100 }\n"
        "    case Shape.Rect(w, h): { r = (w + h) }\n"
        "    case Shape.Line: { r = 300 }\n"
        "  }\n"
        "  return r\n"
        "}\n"
        "def shape_param(s: Shape) -> i64: {\n"
        "  let r: i64 = 0\n"
        "  match s: {\n"
        "    case Shape.Circle(rad): { r = 100 }\n"
        "    case Shape.Rect(w, h): { r = 200 }\n"
        "    case Shape.Line: { r = 300 }\n"
        "  }\n"
        "  return r\n"
        "}\n"
        "def call_param() -> i64: {\n"
        "  return shape_param(Shape.Circle(1.5))\n"
        "}\n"
        "def else_case() -> i64: {\n"
        "  let s: Shape = Shape.Rect(1, 2)\n"
        "  let r: i64 = 0\n"
        "  match s: {\n"
        "    case Shape.Circle(rad): { r = 100 }\n"
        "    case Shape.Line: { r = 300 }\n"
        "    else: { r = 777 }\n"
        "  }\n"
        "  return r\n"
        "}\n";
    ASTNode *program = parse_program(source_text);
    CHECK(program != NULL);
    BackendIrModule source;
    bir_module_init(&source, "<unit:enum-payloads>");
    char err[512] = {0};
    CHECK(bir_build_program(&source, program));
    if (err[0]) fprintf(stderr, "enum payloads build: %s\n", err);
    CHECK(bir_verify(&source, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "enum payloads verify: %s\n", err);
    BirScalarValue result = {0};
    CHECK(bir_eval_function_value(&source, "circle_case", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 100);
    CHECK(bir_eval_function_value(&source, "rect_case", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 7);
    CHECK(bir_eval_function_value(&source, "line_case", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 300);
    CHECK(bir_eval_function_value(&source, "call_param", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 100);
    CHECK(bir_eval_function_value(&source, "else_case", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 777);

    const char *rejections[] = {
        /* Tensors and other out-of-subset payloads stay rejected. */
        "enum E: { A(tensor[4]f32) }\n"
        "def bad() -> i64: {\n"
        "  let e: E = E.A(1)\n"
        "  return 1\n"
        "}\n",
        /* Non-exhaustive match on an owning-payload enum. */
        "enum E: { A(string), B(string) }\n"
        "def bad() -> i64: {\n"
        "  let e: E = E.A(concat(\"a\", \"b\"))\n"
        "  let r: i64 = 0\n"
        "  match e: {\n"
        "    case E.A(s): { r = 1 }\n"
        "  }\n"
        "  return r\n"
        "}\n",
        /* Unknown variant in match. */
        "enum E: { A(i64) }\n"
        "def bad() -> i64: {\n"
        "  let e: E = E.A(1)\n"
        "  let r: i64 = 0\n"
        "  match e: {\n"
        "    case E.B(x): { r = 1 }\n"
        "  }\n"
        "  return r\n"
        "}\n",
        /* Payload variant matched without a binding. */
        "enum E: { A(i64) }\n"
        "def bad() -> i64: {\n"
        "  let e: E = E.A(1)\n"
        "  let r: i64 = 0\n"
        "  match e: {\n"
        "    case E.A: { r = 1 }\n"
        "  }\n"
        "  return r\n"
        "}\n",
        /* Unit variant matched with a binding. */
        "enum E: { A, B(i64) }\n"
        "def bad() -> i64: {\n"
        "  let e: E = E.A\n"
        "  let r: i64 = 0\n"
        "  match e: {\n"
        "    case E.A(x): { r = 1 }\n"
        "    case E.B(y): { r = 2 }\n"
        "  }\n"
        "  return r\n"
        "}\n",
        /* Wrong binding count for a multi-field payload. */
        "enum E: { A(i64, i64) }\n"
        "def bad() -> i64: {\n"
        "  let e: E = E.A(1, 2)\n"
        "  let r: i64 = 0\n"
        "  match e: {\n"
        "    case E.A(x): { r = 1 }\n"
        "  }\n"
        "  return r\n"
        "}\n",
        /* Wrong construction arity. */
        "enum E: { A(i64, i64) }\n"
        "def bad() -> i64: {\n"
        "  let e: E = E.A(1)\n"
        "  return 1\n"
        "}\n",
        /* Payload variant constructed without arguments. */
        "enum E: { A(i64) }\n"
        "def bad() -> i64: {\n"
        "  let e: E = E.A\n"
        "  return 1\n"
        "}\n",
    };
    for (size_t r = 0; r < sizeof(rejections) / sizeof(rejections[0]); r++) {
        ASTNode *bad = parse_program(rejections[r]);
        CHECK(bad != NULL);
        if (!bad) continue;
        BackendIrModule bad_module;
        bir_module_init(&bad_module, "<unit:enum-payload-reject>");
        CHECK(!bir_build_program(&bad_module, bad));
        bir_module_free(&bad_module);
        ast_free(bad);
    }

    /* Both native emitters lower payload enums through the tagged-sum
       aggregate lanes. */
    MirModule mir;
    memset(&mir, 0, sizeof(mir));
    memset(err, 0, sizeof(err));
    CHECK(mir_lower_module(&source, &mir, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "enum payloads mir lower: %s\n", err);
    MirAllocation allocation;
    mir_allocation_init(&allocation, &mir);
    memset(err, 0, sizeof(err));
    CHECK(mir_allocate(&mir, &allocation, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "enum payloads mir alloc: %s\n", err);

    char *spill_assembly = NULL;
    size_t spill_length = 0;
    FILE *spill_out = open_memstream(&spill_assembly, &spill_length);
    CHECK(spill_out != NULL);
    memset(err, 0, sizeof(err));
    CHECK(bir_x86_64_emit(&mir, spill_out, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "enum payloads spill emit: %s\n", err);
    fclose(spill_out);
    CHECK(spill_assembly && strstr(spill_assembly, "circle_case") != NULL);
    CHECK(spill_assembly && strstr(spill_assembly, "shape_param") != NULL);

    char *allocated_assembly = NULL;
    size_t allocated_length = 0;
    FILE *allocated_out = open_memstream(&allocated_assembly, &allocated_length);
    CHECK(allocated_out != NULL);
    CHECK(bir_x86_64_emit_allocated(&mir, &allocation, allocated_out,
                                    err, sizeof(err)));
    if (err[0]) fprintf(stderr, "enum payloads allocated emit: %s\n", err);
    fclose(allocated_out);
    CHECK(allocated_assembly && strstr(allocated_assembly, "circle_case") != NULL);
    CHECK(allocated_assembly && strstr(allocated_assembly, "shape_param") != NULL);

    const char *spill_path = getenv("BIR_X86_ENUM_PAYLOAD_SPILL_ASM_OUT");
    if (spill_path && spill_assembly) {
        FILE *file = fopen(spill_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(spill_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    const char *allocated_path = getenv("BIR_X86_ENUM_PAYLOAD_ALLOC_ASM_OUT");
    if (allocated_path && allocated_assembly) {
        FILE *file = fopen(allocated_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(allocated_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    free(spill_assembly);
    free(allocated_assembly);
    mir_allocation_free(&allocation);
    mir_module_free(&mir);
    bir_module_free(&source);
    ast_free(program);
}

static void test_source_owning_enum_payloads(void) {
    const char *source_text =
        "enum Shape: { Circle(f32), Rect(i64, i64), Tag(string) }\n"
        "enum S: { A([]i64), B }\n"
        "struct P: { x: i64, s: string }\n"
        "enum E: { A(P), B }\n"
        "def circle_area(s: Shape) -> i64: {\n"
        "  let r: i64 = 0\n"
        "  match s: {\n"
        "    case Shape.Circle(rad): { r = 100 }\n"
        "    case Shape.Rect(w, h): { r = (w + h) }\n"
        "    case Shape.Tag(t): { r = len(t) }\n"
        "  }\n"
        "  return r\n"
        "}\n"
        "def circle_main() -> i64: {\n"
        "  return circle_area(Shape.Circle(1.5))\n"
        "}\n"
        "def rect_main() -> i64: {\n"
        "  return circle_area(Shape.Rect(3, 4))\n"
        "}\n"
        "def tag_main() -> i64: {\n"
        "  let s: Shape = Shape.Tag(concat(\"ab\", \"cd\"))\n"
        "  let r: i64 = 0\n"
        "  match s: {\n"
        "    case Shape.Circle(rad): { r = 100 }\n"
        "    case Shape.Rect(w, h): { r = 200 }\n"
        "    case Shape.Tag(t): {\n"
        "      r = len(t)\n"
        "      string_free(t)\n"
        "    }\n"
        "  }\n"
        "  return r\n"
        "}\n"
        "def make_tag() -> Shape: {\n"
        "  return Shape.Tag(concat(\"x\", \"y\"))\n"
        "}\n"
        "def call_tag() -> i64: {\n"
        "  let s: Shape = make_tag()\n"
        "  let r: i64 = 0\n"
        "  match s: {\n"
        "    case Shape.Circle(rad): { r = 100 }\n"
        "    case Shape.Rect(w, h): { r = 200 }\n"
        "    case Shape.Tag(t): { r = len(t) }\n"
        "  }\n"
        "  return r\n"
        "}\n"
        "def slice_main() -> i64: {\n"
        "  let v: []i64 = alloc_i64(3)\n"
        "  v[0] = 10\n"
        "  v[1] = 20\n"
        "  v[2] = 30\n"
        "  let s: S = S.A(v)\n"
        "  let r: i64 = 0\n"
        "  match s: {\n"
        "    case S.A(vv): { r = (vv[0] + vv[1] + vv[2]) }\n"
        "    case S.B: { r = 1 }\n"
        "  }\n"
        "  return r\n"
        "}\n"
        "def struct_main() -> i64: {\n"
        "  let p: P\n"
        "  p.x = 5\n"
        "  p.s = concat(\"ab\", \"c\")\n"
        "  let e: E = E.A(p)\n"
        "  let r: i64 = 0\n"
        "  match e: {\n"
        "    case E.A(pp): { r = (pp.x + len(pp.s)) }\n"
        "    case E.B: { r = 0 }\n"
        "  }\n"
        "  return r\n"
        "}\n";
    ASTNode *program = parse_program(source_text);
    CHECK(program != NULL);
    BackendIrModule source;
    bir_module_init(&source, "<unit:owning-enum-payloads>");
    char err[512] = {0};
    CHECK(bir_build_program(&source, program));
    if (err[0]) fprintf(stderr, "owning enum payloads build: %s\n", err);
    CHECK(bir_verify(&source, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "owning enum payloads verify: %s\n", err);
    BirScalarValue result = {0};
    CHECK(bir_eval_function_value(&source, "circle_main", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 100);
    CHECK(bir_eval_function_value(&source, "rect_main", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 7);
    CHECK(bir_eval_function_value(&source, "tag_main", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 4);
    CHECK(bir_eval_function_value(&source, "call_tag", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 2);
    CHECK(bir_eval_function_value(&source, "slice_main", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 60);
    CHECK(bir_eval_function_value(&source, "struct_main", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 8);

    const char *rejections[] = {
        /* Non-exhaustive match on an owning-payload enum. */
        "enum E: { A(string), B(string) }\n"
        "def bad() -> i64: {\n"
        "  let e: E = E.A(concat(\"a\", \"b\"))\n"
        "  let r: i64 = 0\n"
        "  match e: {\n"
        "    case E.A(s): { r = 1 }\n"
        "  }\n"
        "  return r\n"
        "}\n",
        /* Double free of an extracted owning payload. */
        "enum E: { A(string) }\n"
        "def bad() -> i64: {\n"
        "  let e: E = E.A(concat(\"a\", \"b\"))\n"
        "  match e: {\n"
        "    case E.A(s): {\n"
        "      string_free(s)\n"
        "      string_free(s)\n"
        "    }\n"
        "  }\n"
        "  return 0\n"
        "}\n",
        /* Use of an extracted owning payload after free. */
        "enum E: { A(string) }\n"
        "def bad() -> i64: {\n"
        "  let e: E = E.A(concat(\"a\", \"b\"))\n"
        "  let r: i64 = 0\n"
        "  match e: {\n"
        "    case E.A(s): {\n"
        "      string_free(s)\n"
        "      r = len(s)\n"
        "    }\n"
        "  }\n"
        "  return r\n"
        "}\n",
        /* Tensors and other out-of-subset payloads stay rejected. */
        "enum E: { A(tensor[4]f32) }\n"
        "def bad() -> i64: {\n"
        "  let e: E = E.A(1)\n"
        "  return 1\n"
        "}\n",
        /* Wrong declared arity. */
        "enum E: { A(string, i64) }\n"
        "def bad() -> i64: {\n"
        "  let e: E = E.A(concat(\"a\", \"b\"))\n"
        "  return 0\n"
        "}\n",
    };
    for (size_t r = 0; r < sizeof(rejections) / sizeof(rejections[0]); r++) {
        ASTNode *bad = parse_program(rejections[r]);
        CHECK(bad != NULL);
        if (!bad) continue;
        BackendIrModule bad_module;
        bir_module_init(&bad_module, "<unit:owning-enum-reject>");
        bool built = bir_build_program(&bad_module, bad);
        bool rejected = !built || !bir_verify(&bad_module, err, sizeof(err));
        CHECK(rejected);
        if (built && !rejected)
            fprintf(stderr, "owning enum rejection %zu accepted\n", r);
        bir_module_free(&bad_module);
        ast_free(bad);
    }

    /* Both native emitters lower owning enum payloads through the tagged-sum
       aggregate lanes. */
    MirModule mir;
    memset(&mir, 0, sizeof(mir));
    memset(err, 0, sizeof(err));
    CHECK(mir_lower_module(&source, &mir, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "owning enum payloads mir lower: %s\n", err);
    MirAllocation allocation;
    mir_allocation_init(&allocation, &mir);
    memset(err, 0, sizeof(err));
    CHECK(mir_allocate(&mir, &allocation, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "owning enum payloads mir alloc: %s\n", err);

    char *spill_assembly = NULL;
    size_t spill_length = 0;
    FILE *spill_out = open_memstream(&spill_assembly, &spill_length);
    CHECK(spill_out != NULL);
    memset(err, 0, sizeof(err));
    CHECK(bir_x86_64_emit(&mir, spill_out, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "owning enum payloads spill emit: %s\n", err);
    fclose(spill_out);
    CHECK(spill_assembly && strstr(spill_assembly, "circle_main") != NULL);
    CHECK(spill_assembly && strstr(spill_assembly, "tag_main") != NULL);

    char *allocated_assembly = NULL;
    size_t allocated_length = 0;
    FILE *allocated_out = open_memstream(&allocated_assembly, &allocated_length);
    CHECK(allocated_out != NULL);
    memset(err, 0, sizeof(err));
    CHECK(bir_x86_64_emit_allocated(&mir, &allocation, allocated_out,
                                    err, sizeof(err)));
    if (err[0]) fprintf(stderr, "owning enum payloads allocated emit: %s\n", err);
    fclose(allocated_out);
    CHECK(allocated_assembly && strstr(allocated_assembly, "circle_main") != NULL);
    CHECK(allocated_assembly && strstr(allocated_assembly, "struct_main") != NULL);

    const char *spill_path = getenv("BIR_X86_OWNING_ENUM_SPILL_ASM_OUT");
    if (spill_path && spill_assembly) {
        FILE *file = fopen(spill_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(spill_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    const char *allocated_path = getenv("BIR_X86_OWNING_ENUM_ALLOC_ASM_OUT");
    if (allocated_path && allocated_assembly) {
        FILE *file = fopen(allocated_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(allocated_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    free(spill_assembly);
    free(allocated_assembly);
    mir_allocation_free(&allocation);
    mir_module_free(&mir);
    bir_module_free(&source);
    ast_free(program);
}

static void test_source_callee_saved_registers(void) {
    /* Five locals stay live across five sequential calls, so the linear-scan
       allocator cannot fit them in the caller-saved argument registers and
       must hand out callee-saved registers instead of spilling. An unused
       early return exercises the callee-saved restore on more than one
       return site. */
    const char *source_text =
        "def bump(x: i64) -> i64: { return x + 1 }\n"
        "def stress_main() -> i64: {\n"
        "  a = 1\n"
        "  b = 2\n"
        "  c = 3\n"
        "  d = 4\n"
        "  e = 5\n"
        "  t1 = bump(a)\n"
        "  if t1 > 100: { return t1 }\n"
        "  t2 = bump(b)\n"
        "  t3 = bump(c)\n"
        "  t4 = bump(d)\n"
        "  t5 = bump(e)\n"
        "  return a + b + c + d + e + t1 + t2 + t3 + t4 + t5\n"
        "}\n";
    ASTNode *program = parse_program(source_text);
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule source;
    bir_module_init(&source, "<unit:callee-saved>");
    char err[512] = {0};
    CHECK(bir_build_program(&source, program));

    MirModule mir;
    memset(&mir, 0, sizeof(mir));
    memset(err, 0, sizeof(err));
    CHECK(mir_lower_module(&source, &mir, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "callee-saved mir lower: %s\n", err);
    MirAllocation allocation;
    mir_allocation_init(&allocation, &mir);
    memset(err, 0, sizeof(err));
    CHECK(mir_allocate(&mir, &allocation, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "callee-saved mir alloc: %s\n", err);

    bool used_multiple_callee_saved = false;
    for (size_t f = 0; f < allocation.function_count; f++) {
        if (strcmp(allocation.functions[f].name, "stress_main") != 0) continue;
        uint64_t callee_saved_bits = allocation.functions[f].used_gpr_mask >> 6;
        int used = 0;
        for (int i = 0; i < 5; i++) if ((callee_saved_bits >> i) & 1) used++;
        used_multiple_callee_saved = used >= 4;
    }
    CHECK(used_multiple_callee_saved);

    char *spill_assembly = NULL;
    size_t spill_length = 0;
    FILE *spill_out = open_memstream(&spill_assembly, &spill_length);
    CHECK(spill_out != NULL);
    memset(err, 0, sizeof(err));
    CHECK(bir_x86_64_emit(&mir, spill_out, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "callee-saved spill emit: %s\n", err);
    fclose(spill_out);
    CHECK(spill_assembly && strstr(spill_assembly, "stress_main") != NULL);

    char *allocated_assembly = NULL;
    size_t allocated_length = 0;
    FILE *allocated_out = open_memstream(&allocated_assembly, &allocated_length);
    CHECK(allocated_out != NULL);
    memset(err, 0, sizeof(err));
    CHECK(bir_x86_64_emit_allocated(&mir, &allocation, allocated_out,
                                    err, sizeof(err)));
    if (err[0]) fprintf(stderr, "callee-saved allocated emit: %s\n", err);
    fclose(allocated_out);
    CHECK(allocated_assembly && strstr(allocated_assembly, "stress_main") != NULL);
    CHECK(allocated_assembly &&
          (strstr(allocated_assembly, "%rbx") != NULL ||
           strstr(allocated_assembly, "%r12") != NULL ||
           strstr(allocated_assembly, "%r13") != NULL ||
           strstr(allocated_assembly, "%r14") != NULL ||
           strstr(allocated_assembly, "%r15") != NULL));

    const char *spill_path = getenv("BIR_X86_CALLEE_SAVED_SPILL_ASM_OUT");
    if (spill_path && spill_assembly) {
        FILE *file = fopen(spill_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(spill_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    const char *allocated_path = getenv("BIR_X86_CALLEE_SAVED_ALLOC_ASM_OUT");
    if (allocated_path && allocated_assembly) {
        FILE *file = fopen(allocated_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(allocated_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    free(spill_assembly);
    free(allocated_assembly);
    mir_allocation_free(&allocation);
    mir_module_free(&mir);
    bir_module_free(&source);
    ast_free(program);
}

static void test_source_object_emitter_coverage(void) {
    /* Exercises every v1 object-emitter lane in one program: scalar calls,
       flat scalar-field structs, f32 arithmetic and comparison, heap-backed
       readonly-view allocation/indexing/bounds, and a bare `[]i64` parameter
       (borrowed-mutable, matching the direct backend - the callee mutates
       the caller's own allocation through a transient view borrow, released
       once the call returns, and the caller frees it itself afterward).
       bir_x86_64_emit_object writes real machine code straight to an ELF64
       object, unlike the text emitters above, so there is no assembly
       string to inspect here - the Makefile links the resulting .o
       directly and runs it. */
    const char *source_text =
        "def add(a: i64, b: i64) -> i64: { return a + b }\n"
        "struct Point: { x: i64, y: i64 }\n"
        "def sum_point(p: Point) -> i64: { return p.x + p.y }\n"
        "def scale(x: f32, factor: f32) -> f32: { return x * factor }\n"
        "def bump_view(s: []i64) -> i64: {\n"
        "  s[0] = s[0] + 1\n"
        "  return s[0]\n"
        "}\n"
        "def object_main() -> i64: {\n"
        "  let sum: i64 = add(3, 4)\n"
        "  let p: Point\n"
        "  p.x = 3\n"
        "  p.y = 4\n"
        "  let sp: i64 = sum_point(p)\n"
        "  let f: f32 = scale(2.5, 2.0)\n"
        "  let fi: i64 = 0\n"
        "  if f > 4.9: { fi = 1 }\n"
        "  let s: []i64 = alloc_i64(1)\n"
        "  s[0] = 40\n"
        "  let owned: i64 = bump_view(s)\n"
        "  free(s)\n"
        "  return sum + sp + fi + owned\n"
        "}\n";
    ASTNode *program = parse_program(source_text);
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule source;
    bir_module_init(&source, "<unit:object-emitter>");
    char err[512] = {0};
    CHECK(bir_build_program(&source, program));

    MirModule mir;
    memset(&mir, 0, sizeof(mir));
    memset(err, 0, sizeof(err));
    CHECK(mir_lower_module(&source, &mir, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "object emitter mir lower: %s\n", err);
    MirAllocation allocation;
    mir_allocation_init(&allocation, &mir);
    memset(err, 0, sizeof(err));
    CHECK(mir_allocate(&mir, &allocation, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "object emitter mir alloc: %s\n", err);

    const char *object_path = getenv("BIR_X86_OBJECT_OUT");
    if (object_path) {
        memset(err, 0, sizeof(err));
        CHECK(bir_x86_64_emit_object(&mir, &allocation, object_path, err, sizeof(err)));
        if (err[0]) fprintf(stderr, "object emitter emit: %s\n", err);
    }

    mir_allocation_free(&allocation);
    mir_module_free(&mir);
    bir_module_free(&source);
    ast_free(program);
}

static void test_source_object_emitter_coverage2(void) {
    /* Covers the object-emitter lanes added after the first coverage
       fixture above: struct-by-value return (indirect hidden-pointer ABI),
       enum construction and match dispatch, an owning-field struct with
       string_concat, dict[string]i64 (rodata key literals + R_X86_64_PC32 +
       cobra_dict_* via R_X86_64_PLT32), and a dynamic list[i64] buffer
       (alloc/append/pop, always-realloc). */
    const char *source_text =
        "struct Point: { x: i64, y: i64 }\n"
        "def make_point(a: i64, b: i64) -> Point: {\n"
        "  let p: Point\n"
        "  p.x = a\n"
        "  p.y = b\n"
        "  return p\n"
        "}\n"
        "enum Shape: { Circle(f32), Rect(i64, i64), Empty }\n"
        "def area(s: Shape) -> i64: {\n"
        "  let r: i64 = 0\n"
        "  match s: {\n"
        "    case Shape.Circle(rad): { r = 100 }\n"
        "    case Shape.Rect(w, h): { r = w * h }\n"
        "    case Shape.Empty: { r = 0 }\n"
        "  }\n"
        "  return r\n"
        "}\n"
        "struct OwnedBox: { text: string, value: i64 }\n"
        "def make_box() -> OwnedBox: {\n"
        "  let box: OwnedBox\n"
        "  box.text = concat(\"m\", \"e\")\n"
        "  box.value = 1\n"
        "  return box\n"
        "}\n"
        "def object_main2() -> i64: {\n"
        "  let q: Point\n"
        "  q = make_point(5, 6)\n"
        "  let point_total: i64 = q.x * 10 + q.y\n"
        "  let shape_total: i64 = area(Shape.Rect(3, 4))\n"
        "  let box: OwnedBox = make_box()\n"
        "  let box_len: i64 = len(box.text)\n"
        "  let box_val: i64 = box.value\n"
        "  free(box)\n"
        "  let box_total: i64 = box_len + box_val\n"
        "  let d: dict[string]i64 = {\"a\": 1, \"b\": 2}\n"
        "  set(d, \"c\", 3)\n"
        "  let dict_total: i64 = get(d, \"a\", 0) + len(d)\n"
        "  free(d)\n"
        "  let values: list[i64] = [10]\n"
        "  append(values, 20)\n"
        "  append(values, 30)\n"
        "  let popped: i64 = pop(values)\n"
        "  let buf_total: i64 = values[0] + values[1] + popped\n"
        "  free(values)\n"
        "  return point_total + shape_total + box_total + dict_total + buf_total\n"
        "}\n";
    ASTNode *program = parse_program(source_text);
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule source;
    bir_module_init(&source, "<unit:object-emitter-2>");
    char err[512] = {0};
    CHECK(bir_build_program(&source, program));

    MirModule mir;
    memset(&mir, 0, sizeof(mir));
    memset(err, 0, sizeof(err));
    CHECK(mir_lower_module(&source, &mir, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "object emitter 2 mir lower: %s\n", err);
    MirAllocation allocation;
    mir_allocation_init(&allocation, &mir);
    memset(err, 0, sizeof(err));
    CHECK(mir_allocate(&mir, &allocation, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "object emitter 2 mir alloc: %s\n", err);

    const char *object_path = getenv("BIR_X86_OBJECT2_OUT");
    if (object_path) {
        memset(err, 0, sizeof(err));
        CHECK(bir_x86_64_emit_object(&mir, &allocation, object_path, err, sizeof(err)));
        if (err[0]) fprintf(stderr, "object emitter 2 emit: %s\n", err);
    }

    mir_allocation_free(&allocation);
    mir_module_free(&mir);
    bir_module_free(&source);
    ast_free(program);
}

static void test_source_object_emitter_coverage3(void) {
    /* Covers the last three v1 gaps: fixed arrays as struct fields
       (ARRAY_INDEX_ADDR plus the widened struct/stack-slot shape gates),
       an owned sum payload (Option[string], exercising the real
       Option/Result branch of x86obj_emit_drop_owned_value that the first
       two coverage fixtures never reach), and a non-scalar (struct-element)
       list[Point] buffer append plus indexed read. */
    const char *source_text =
        "struct Grid: { data: array[i64, 4], scale: i64 }\n"
        "def make_grid() -> Grid: {\n"
        "  let g: Grid\n"
        "  g.data = [1, 2, 3, 4]\n"
        "  g.scale = 10\n"
        "  return g\n"
        "}\n"
        "def sum_grid(g: Grid) -> i64: {\n"
        "  let copy: array[i64, 4] = g.data\n"
        "  return copy[0] + copy[1] + copy[2] + copy[3] + g.scale\n"
        "}\n"
        "def make_opt(flag: i64) -> Option[string]: {\n"
        "  if flag == 1: {\n"
        "    return some(concat(\"h\", \"i\"))\n"
        "  } else: {\n"
        "    return none\n"
        "  }\n"
        "}\n"
        "struct Point: { x: i64, y: i64 }\n"
        "def object_main3() -> i64: {\n"
        "  let g: Grid = make_grid()\n"
        "  let array_total: i64 = sum_grid(g)\n"
        "  let a: Option[string] = make_opt(1)\n"
        "  let opt_total: i64 = 0\n"
        "  if is_some(a): { opt_total = len(unwrap(a)) }\n"
        "  free(a)\n"
        "  let b: Option[string] = make_opt(0)\n"
        "  if is_some(b): {} else: { opt_total = opt_total + 1 }\n"
        "  free(b)\n"
        "  let p1: Point\n"
        "  p1.x = 5\n"
        "  p1.y = 6\n"
        "  let pts: list[Point] = [p1]\n"
        "  let p2: Point\n"
        "  p2.x = 7\n"
        "  p2.y = 8\n"
        "  append(pts, p2)\n"
        "  let q: Point = pts[1]\n"
        "  let buf_total: i64 = q.x + q.y\n"
        "  free(pts)\n"
        "  return array_total + opt_total + buf_total\n"
        "}\n";
    ASTNode *program = parse_program(source_text);
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule source;
    bir_module_init(&source, "<unit:object-emitter-3>");
    char err[512] = {0};
    CHECK(bir_build_program(&source, program));

    MirModule mir;
    memset(&mir, 0, sizeof(mir));
    memset(err, 0, sizeof(err));
    CHECK(mir_lower_module(&source, &mir, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "object emitter 3 mir lower: %s\n", err);
    MirAllocation allocation;
    mir_allocation_init(&allocation, &mir);
    memset(err, 0, sizeof(err));
    CHECK(mir_allocate(&mir, &allocation, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "object emitter 3 mir alloc: %s\n", err);

    const char *object_path = getenv("BIR_X86_OBJECT3_OUT");
    if (object_path) {
        memset(err, 0, sizeof(err));
        CHECK(bir_x86_64_emit_object(&mir, &allocation, object_path, err, sizeof(err)));
        if (err[0]) fprintf(stderr, "object emitter 3 emit: %s\n", err);
    }

    mir_allocation_free(&allocation);
    mir_module_free(&mir);
    bir_module_free(&source);
    ast_free(program);
}

static void test_source_object_emitter_coverage4(void) {
    /* print (both i64 and string forms) and assert: HIR_STMT_PRINT/ASSERT
       through SSA_OP_PRINT_I64/PRINT_STRING/ASSERT and MIR_OP_PRINT_I64/
       PRINT_STRING/ASSERT. The string print path exercises the putchar
       loop (x86obj emitter) / rbp-scratch loop (text emitter) that reads
       each u8 element's low byte out of its 8-byte canonical slot, since
       printf("%s", ...) cannot be used directly on this memory model - see
       the comment on MIR_OP_PRINT_STRING's handler. assert here is always
       true, so the runner's exit-code check also proves execution reached
       the end rather than trapping. */
    const char *source_text =
        "def object_main4() -> i64: {\n"
        "  print(\"cobra object emitter\")\n"
        "  let a: i64 = 21\n"
        "  print(a)\n"
        "  let b: i64 = a * 2\n"
        "  print(b)\n"
        "  assert b == 42\n"
        "  return a + b\n"
        "}\n";
    ASTNode *program = parse_program(source_text);
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule source;
    bir_module_init(&source, "<unit:object-emitter-4>");
    char err[512] = {0};
    CHECK(bir_build_program(&source, program));

    MirModule mir;
    memset(&mir, 0, sizeof(mir));
    memset(err, 0, sizeof(err));
    CHECK(mir_lower_module(&source, &mir, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "object emitter 4 mir lower: %s\n", err);
    MirAllocation allocation;
    mir_allocation_init(&allocation, &mir);
    memset(err, 0, sizeof(err));
    CHECK(mir_allocate(&mir, &allocation, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "object emitter 4 mir alloc: %s\n", err);

    const char *object_path = getenv("BIR_X86_OBJECT4_OUT");
    if (object_path) {
        memset(err, 0, sizeof(err));
        CHECK(bir_x86_64_emit_object(&mir, &allocation, object_path, err, sizeof(err)));
        if (err[0]) fprintf(stderr, "object emitter 4 emit: %s\n", err);
    }

    mir_allocation_free(&allocation);
    mir_module_free(&mir);
    bir_module_free(&source);
    ast_free(program);
}

static void test_x86_native_owned_slices(void) {
    const char *source_text =
        "struct OwnedBox: { text: string, value: i64 }\n"
        "def buffer_main() -> i64: {\n"
        "  let values: list[i64] = [40]\n"
        "  append(values, 2)\n"
        "  let popped: i64 = pop(values)\n"
        "  if popped == 2: {\n"
        "    free(values)\n"
        "    return 42\n"
        "  } else: {\n"
        "    free(values)\n"
        "    return 0\n"
        "  }\n"
        "}\n"
        "def u8_main() -> i64: {\n"
        "  let value: u8 = 250\n"
        "  value = value + 10\n"
        "  if value == 4: { return 42 }\n"
        "  return 0\n"
        "}\n"
        "def owned_main() -> i64: {\n"
        "  s = alloc_i64(2)\n"
        "  s[0] = 40\n"
        "  s[1] = 2\n"
        "  result = s[0] + s[1]\n"
        "  free(s)\n"
        "  return result\n"
        "}\n"
        "def region_value() -> i64: {\n"
        "  with region scratch: {\n"
        "    a = scratch.alloc_i64(1)\n"
        "    a[0] = 41\n"
        "    result = a[0] + 1\n"
        "  }\n"
        "  return result\n"
        "}\n"
        "def string_main() -> i64: {\n"
        "  let text: string = \"co\" + \"bra\"\n"
        "  result = len(text)\n"
        "  string_free(text)\n"
        "  return result\n"
        "}\n"
        "def take_owned(s: []i64) -> i64: {\n"
        "  value = s[0]\n"
        "  return value\n"
        "}\n"
        "def call_owned() -> i64: {\n"
        "  s = alloc_i64(1)\n"
        "  s[0] = 41\n"
        "  result = take_owned(s) + 1\n"
        "  free(s)\n"
        "  return result\n"
        "}\n"
        "def make_owned() -> []i64: {\n"
        "  s = alloc_i64(1)\n"
        "  s[0] = 40\n"
        "  return s\n"
        "}\n"
        "def return_owned() -> i64: {\n"
        "  s = make_owned()\n"
        "  value = s[0]\n"
        "  free(s)\n"
        "  return value + 2\n"
        "}\n"
        "def identity_owned(s: readonly []i64) -> readonly []i64: {\n"
        "  return s\n"
        "}\n"
        "def roundtrip_owned() -> i64: {\n"
        "  s = alloc_i64(1)\n"
        "  s[0] = 40\n"
        "  let out: readonly []i64 = identity_owned(s)\n"
        "  value = out[0]\n"
        "  free(s)\n"
        "  return value + 2\n"
        "}\n"
        "def make_option() -> Option[string] {\n"
        "  return some(concat(\"o\", \"k\"))\n"
        "}\n"
        "def take_option(value: Option[string]) -> i64 {\n"
        "  let text: string = unwrap(value)\n"
        "  n = len(text)\n"
        "  string_free(text)\n"
        "  free(value)\n"
        "  return n + 40\n"
        "}\n"
        "def option_call() -> i64 {\n"
        "  let value: Option[string] = make_option()\n"
        "  return take_option(value)\n"
        "}\n"
        "def owning_struct() -> i64 {\n"
        "  let box: OwnedBox\n"
        "  box.text = concat(\"o\", \"k\")\n"
        "  box.value = 1\n"
        "  free(box)\n"
        "  return 42\n"
        "}\n"
        "def result_owned() -> i64 {\n"
        "  let value: Result[string, i64] = ok(concat(\"r\", \"s\"))\n"
        "  free(value)\n"
        "  return 42\n"
        "}\n"
        "def make_box() -> OwnedBox {\n"
        "  let box: OwnedBox\n"
        "  box.text = concat(\"m\", \"e\")\n"
        "  box.value = 1\n"
        "  return box\n"
        "}\n"
        "def struct_return() -> i64 {\n"
        "  let box: OwnedBox = make_box()\n"
        "  free(box)\n"
        "  return 42\n"
        "}\n";
    ASTNode *program = parse_program(source_text);
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule source;
    bir_module_init(&source, "<unit:x86-native-owned-slices>");
    char err[512] = {0};
    CHECK(bir_build_program(&source, program));
    CHECK(bir_verify(&source, err, sizeof(err)));
    BirScalarValue result = {0};
    CHECK(bir_eval_function_value(&source, "buffer_main", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 42);
    CHECK(bir_eval_function_value(&source, "u8_main", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 42);
    CHECK(bir_eval_function_value(&source, "owned_main", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 42);
    CHECK(bir_eval_function_value(&source, "region_value", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 42);
    CHECK(bir_eval_function_value(&source, "string_main", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 5);
    CHECK(bir_eval_function_value(&source, "call_owned", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 42);
    CHECK(bir_eval_function_value(&source, "return_owned", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 42);
    CHECK(bir_eval_function_value(&source, "roundtrip_owned", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 42);
    CHECK(bir_eval_function_value(&source, "option_call", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 42);
    CHECK(bir_eval_function_value(&source, "owning_struct", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 42);
    CHECK(bir_eval_function_value(&source, "result_owned", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 42);
    CHECK(bir_eval_function_value(&source, "struct_return", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 42);

    MirModule mir;
    memset(&mir, 0, sizeof(mir));
    CHECK(mir_lower_module(&source, &mir, err, sizeof(err)));
    MirAllocation allocation;
    mir_allocation_init(&allocation, &mir);
    CHECK(mir_allocate(&mir, &allocation, err, sizeof(err)));

    char *spill_assembly = NULL;
    size_t spill_length = 0;
    FILE *spill_out = open_memstream(&spill_assembly, &spill_length);
    CHECK(spill_out != NULL);
    CHECK(bir_x86_64_emit(&mir, spill_out, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "native owned emit: %s\\n", err);
    fclose(spill_out);
    CHECK(spill_assembly && strstr(spill_assembly, "call malloc@PLT") != NULL);
    CHECK(spill_assembly && strstr(spill_assembly, "call free@PLT") != NULL);
    CHECK(spill_assembly && strstr(spill_assembly, "call memcpy@PLT") != NULL);

    char *allocated_assembly = NULL;
    size_t allocated_length = 0;
    FILE *allocated_out = open_memstream(&allocated_assembly, &allocated_length);
    CHECK(allocated_out != NULL);
    CHECK(bir_x86_64_emit_allocated(&mir, &allocation, allocated_out,
                                    err, sizeof(err)));
    if (err[0]) fprintf(stderr, "native owned allocated emit: %s\\n", err);
    fclose(allocated_out);
    CHECK(allocated_assembly && strstr(allocated_assembly, "call malloc@PLT") != NULL);
    CHECK(allocated_assembly && strstr(allocated_assembly, "call free@PLT") != NULL);
    CHECK(allocated_assembly && strstr(allocated_assembly, "call memcpy@PLT") != NULL);

    const char *spill_path = getenv("BIR_X86_OWNED_SPILL_ASM_OUT");
    if (spill_path && spill_assembly) {
        FILE *file = fopen(spill_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(spill_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    const char *allocated_path = getenv("BIR_X86_OWNED_ALLOC_ASM_OUT");
    if (allocated_path && allocated_assembly) {
        FILE *file = fopen(allocated_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(allocated_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    free(spill_assembly);
    free(allocated_assembly);
    mir_allocation_free(&allocation);
    mir_module_free(&mir);
    bir_module_free(&source);
    ast_free(program);
}

static void test_x86_native_scalar_sums(void) {
    const char *source_text =
        "def main() -> i64: {\n"
        "  let value: Option[string] = some(concat(\"a\", \"b\"))\n"
        "  let text: string = unwrap(value)\n"
        "  n = len(text)\n"
        "  string_free(text)\n"
        "  free(value)\n"
        "  return n + 40\n"
        "}\n"
        "def drop_owned() -> i64: {\n"
        "  let value: Option[string] = some(concat(\"x\", \"y\"))\n"
        "  free(value)\n"
        "  return 42\n"
        "}\n";
    ASTNode *program = parse_program(source_text);
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule source;
    bir_module_init(&source, "<unit:x86-native-scalar-sums>");
    char err[512] = {0};
    CHECK(bir_build_program(&source, program));
    CHECK(bir_verify(&source, err, sizeof(err)));
    BirScalarValue result = {0};
    CHECK(bir_eval_function_value(&source, "main", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 42);
    CHECK(bir_eval_function_value(&source, "drop_owned", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 42);

    MirModule mir;
    memset(&mir, 0, sizeof(mir));
    CHECK(mir_lower_module(&source, &mir, err, sizeof(err)));
    MirAllocation allocation;
    mir_allocation_init(&allocation, &mir);
    CHECK(mir_allocate(&mir, &allocation, err, sizeof(err)));
    char *spill_assembly = NULL;
    size_t spill_length = 0;
    FILE *spill_out = open_memstream(&spill_assembly, &spill_length);
    CHECK(spill_out != NULL);
    CHECK(bir_x86_64_emit(&mir, spill_out, err, sizeof(err)));
    fclose(spill_out);
    CHECK(spill_assembly && strstr(spill_assembly, "sum_fail") != NULL);
    char *allocated_assembly = NULL;
    size_t allocated_length = 0;
    FILE *allocated_out = open_memstream(&allocated_assembly, &allocated_length);
    CHECK(allocated_out != NULL);
    CHECK(bir_x86_64_emit_allocated(&mir, &allocation, allocated_out,
                                    err, sizeof(err)));
    fclose(allocated_out);
    CHECK(allocated_assembly && strstr(allocated_assembly, "sum_fail") != NULL);
    const char *spill_path = getenv("BIR_X86_SUM_SPILL_ASM_OUT");
    if (spill_path && spill_assembly) {
        FILE *file = fopen(spill_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(spill_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    const char *allocated_path = getenv("BIR_X86_SUM_ALLOC_ASM_OUT");
    if (allocated_path && allocated_assembly) {
        FILE *file = fopen(allocated_path, "w");
        CHECK(file != NULL);
        if (file) {
            CHECK(fputs(allocated_assembly, file) >= 0);
            CHECK(fclose(file) == 0);
        }
    }
    free(spill_assembly);
    free(allocated_assembly);
    mir_allocation_free(&allocation);
    mir_module_free(&mir);
    bir_module_free(&source);
    ast_free(program);
}

static void test_source_owned_struct_fields(void) {
    const char *source_text =
        "struct Box: { text: string, value: i64 }\n"
        "struct Outer: { inner: Box }\n"
        "struct State: { result: Result[string, i64] }\n"
        "def make_box() -> Box: {\n"
        "  let result: Box\n"
        "  result.text = concat(\"co\", \"bra\")\n"
        "  result.value = 40\n"
        "  return result\n"
        "}\n"
        "def main() -> i64: {\n"
        "  let outer: Outer\n"
        "  outer.inner.text = concat(\"co\", \"bra\")\n"
        "  outer.inner.value = 40\n"
        "  free(outer)\n"
        "  let box: Box\n"
        "  box = make_box()\n"
        "  let moved = box.text\n"
        "  free(moved)\n"
        "  free(box)\n"
        "  let state: State\n"
        "  state.result = ok(concat(\"o\", \"k\"))\n"
        "  let text = unwrap_ok(state.result)\n"
        "  free(text)\n"
        "  free(state)\n"
        "  return 2\n"
        "}\n";
    ASTNode *program = parse_program(source_text);
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule module;
    bir_module_init(&module, "<unit:owned-struct-fields>");
    char err[512] = {0};
    CHECK(bir_build_program(&module, program));
    if (module.error[0]) fprintf(stderr, "owned struct build: %s\\n", module.error);
    CHECK(bir_verify(&module, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "owned struct verify: %s\\n", err);
    BirScalarValue result = {0};
    CHECK(bir_eval_function_value(&module, "main", &result));
    CHECK(result.kind == BIR_SCALAR_I64 && result.payload.i64 == 2);
    bir_module_free(&module);
    ast_free(program);
}

static void test_source_owned_buffers(void) {
    unit_expected("owned list literal, append, index, len, and free",
        "def main() -> i64: {\n"
        "  let values: list[i64] = [1, 2]\n"
        "  append(values, 3)\n"
        "  r = values[0] + values[1] + values[2] + len(values)\n"
        "  free(values)\n"
        "  return r\n"
        "}\n", 9);

    unit_expected("owned list pop mutates the source buffer",
        "def main() -> i64: {\n"
        "  let values: list[i64] = [4, 5, 6]\n"
        "  let last: i64 = pop(values)\n"
        "  r = last * 10 + len(values) + values[1]\n"
        "  free(values)\n"
        "  return r\n"
        "}\n", 67);

    unit_expected("owned list call and returned buffer",
        "def add_item(values: list[i64]) -> list[i64]: {\n"
        "  append(values, 7)\n"
        "  return values\n"
        "}\n"
        "def main() -> i64: {\n"
        "  let values: list[i64] = [1, 2]\n"
        "  let result: list[i64] = add_item(values)\n"
        "  r = result[2] + len(result)\n"
        "  free(result)\n"
        "  return r\n"
        "}\n", 10);

    unit_expected("owned list repeated growth preserves elements",
        "def main() -> i64: {\n"
        "  let values: list[i64] = []\n"
        "  append(values, 10)\n"
        "  append(values, 20)\n"
        "  append(values, 12)\n"
        "  return values[0] + values[1] + values[2]\n"
        "}\n", 42);

    unit_rejected("owned list index out of bounds",
        "def main() -> i64: {\n"
        "  let values: list[i64] = [1, 2]\n"
        "  return values[2]\n"
        "}\n");

    unit_rejected("owned list negative index",
        "def main() -> i64: {\n"
        "  let values: list[i64] = [1, 2]\n"
        "  return values[-1]\n"
        "}\n");

    unit_rejected("pop from an empty owned list",
        "def main() -> i64: {\n"
        "  let values: list[i64] = []\n"
        "  return pop(values)\n"
        "}\n");

    unit_rejected("owned list double free",
        "def main() -> i64: {\n"
        "  let values: list[i64] = [1]\n"
        "  free(values)\n"
        "  free(values)\n"
        "  return 0\n"
        "}\n");

    unit_rejected("owned list use after free",
        "def main() -> i64: {\n"
        "  let values: list[i64] = [1]\n"
        "  free(values)\n"
        "  return len(values)\n"
        "}\n");

    unit_rejected("owned list cannot free while borrowed",
        "def main() -> i64: {\n"
        "  let values: list[i64] = [1, 2]\n"
        "  let view: readonly []i64 = values\n"
        "  free(values)\n"
        "  return len(view)\n"
        "}\n");
}

static void test_source_fixed_arrays(void) {
    unit_expected("fixed array literal, indexing, assignment, and len",
        "def main() -> i64: {\n"
        "  let values: array[i64, 3] = [1, 2, 3]\n"
        "  values[1] = 40\n"
        "  return values[0] + values[1] + values[2] + len(values)\n"
        "}\n", 47);

    unit_expected("fixed array call and indirect return",
        "def sum3(values: array[i64, 3]) -> i64: {\n"
        "  return values[0] + values[1] + values[2]\n"
        "}\n"
        "def make() -> array[i64, 3]: {\n"
        "  return [4, 5, 6]\n"
        "}\n"
        "def main() -> i64: {\n"
        "  let values: array[i64, 3] = make()\n"
        "  return sum3(values)\n"
        "}\n", 15);

    unit_expected("fixed array constructor as aggregate call argument",
        "def sum3(values: array[i64, 3]) -> i64: {\n"
        "  return values[0] + values[1] + values[2]\n"
        "}\n"
        "def main() -> i64: {\n"
        "  return sum3([7, 8, 9])\n"
        "}\n", 24);

    unit_rejected("fixed array length mismatch",
        "def main() -> i64: {\n"
        "  let values: array[i64, 3] = [1, 2]\n"
        "  return 0\n"
        "}\n");

    unit_rejected("fixed array index out of bounds",
        "def main() -> i64: {\n"
        "  let values: array[i64, 2] = [1, 2]\n"
        "  return values[2]\n"
        "}\n");

    unit_rejected("fixed array negative index",
        "def main() -> i64: {\n"
        "  let values: array[i64, 2] = [1, 2]\n"
        "  return values[-1]\n"
        "}\n");

    /* Malformed IR must reject a fixed-array index whose operand is not i64.
       This exercises the verifier contract independently of HIR validation. */
    BackendIrModule module;
    bir_module_init(&module, "<unit:array-index-signature>");
    SsaArena *arena = &module.arena;
    const CobraType *array = bir_array_type(&module, module.type_i64, 2);
    const CobraType *array_pointer = bir_pointer_type(&module, array);
    const CobraType *element_pointer = bir_pointer_type(&module, module.type_i64);
    SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
    SsaInstRef slot = bir_add_stack_slot(arena, array_pointer, array, 0, 8, 0, 1, 1);
    CHECK(array && array_pointer && element_pointer && slot != SSA_INST_NONE);
    CHECK(bir_block_add_inst(arena, entry, slot));
    SsaValueRef base = bir_inst_result(arena, slot, 1, 1);
    SsaValueRef bad_index = bir_add_const(arena,
        bir_scalar_bool(module.type_bool, true), 1, 1);
    SsaInstRef address = bir_add_array_index_addr(arena, element_pointer, array,
                                                   module.type_i64, base,
                                                   bad_index, 1, 1);
    CHECK(bir_block_add_inst(arena, entry, address));
    SsaValueRef value = bir_add_const(arena,
        bir_scalar_i64(module.type_i64, 0), 1, 1);
    CHECK(bir_set_return(arena, entry, value, 1, 1));
    CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                     module.type_i64, true));
    char err[512] = {0};
    CHECK(!bir_verify(&module, err, sizeof(err)));
    CHECK(strstr(err, "fixed array index") != NULL || strstr(err, "operand") != NULL);
    bir_module_free(&module);
}

static void test_buffer_ir_rejected(void) {
    /* BUFFER_ALLOC must carry the canonical element descriptor, not merely a
       scalar-shaped memory type. */
    {
        BackendIrModule module;
        bir_module_init(&module, "<unit:buffer-allocation-metadata>");
        SsaArena *arena = &module.arena;
        const CobraType *buffer = bir_buffer_type(&module, module.type_i64);
        SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
        SsaValueRef length = bir_add_const(arena,
            bir_scalar_i64(module.type_i64, 1), 1, 1);
        SsaInstRef alloc = bir_add_buffer_alloc(arena, buffer, module.type_i64,
                                                length, 1, 1, 1);
        CHECK(buffer && alloc != SSA_INST_NONE);
        CHECK(bir_block_add_inst(arena, entry, alloc));
        arena->insts[alloc].memory_type = module.type_bool;
        SsaValueRef result = bir_add_const(arena,
            bir_scalar_i64(module.type_i64, 0), 1, 1);
        CHECK(bir_set_return(arena, entry, result, 1, 1));
        CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                         module.type_i64, true));
        char err[512] = {0};
        CHECK(!bir_verify(&module, err, sizeof(err)));
        CHECK(strstr(err, "buffer allocation") != NULL ||
              strstr(err, "metadata") != NULL);
        bir_module_free(&module);
    }

    /* BUFFER_APPEND must receive exactly the canonical scalar element type. */
    {
        BackendIrModule module;
        bir_module_init(&module, "<unit:buffer-append-signature>");
        SsaArena *arena = &module.arena;
        const CobraType *buffer = bir_buffer_type(&module, module.type_i64);
        SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
        SsaValueRef length = bir_add_const(arena,
            bir_scalar_i64(module.type_i64, 0), 1, 1);
        SsaInstRef alloc = bir_add_buffer_alloc(arena, buffer, module.type_i64,
                                                length, 1, 1, 1);
        CHECK(buffer && alloc != SSA_INST_NONE);
        CHECK(bir_block_add_inst(arena, entry, alloc));
        SsaValueRef owned = bir_inst_result(arena, alloc, 1, 1);
        SsaValueRef wrong_element = bir_add_const(arena,
            bir_scalar_bool(module.type_bool, true), 1, 1);
        SsaInstRef append = bir_add_buffer_append(arena, buffer, module.type_i64,
                                                  owned, wrong_element, 2, 1, 1);
        CHECK(append != SSA_INST_NONE);
        CHECK(bir_block_add_inst(arena, entry, append));
        SsaValueRef result = bir_add_const(arena,
            bir_scalar_i64(module.type_i64, 0), 1, 1);
        CHECK(bir_set_return(arena, entry, result, 1, 1));
        CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                         module.type_i64, true));
        char err[512] = {0};
        CHECK(!bir_verify(&module, err, sizeof(err)));
        CHECK(strstr(err, "buffer append") != NULL ||
              strstr(err, "wrong type") != NULL);
        bir_module_free(&module);
    }

    /* BUFFER_FREE is an ownership operation and cannot consume a readonly
       borrowed view. */
    {
        BackendIrModule module;
        bir_module_init(&module, "<unit:buffer-free-contract>");
        SsaArena *arena = &module.arena;
        const CobraType *pointer_type = bir_pointer_type(&module, module.type_i64);
        const CobraType *view_type = bir_view_type(&module, module.type_i64);
        SsaBlockRef entry = bir_add_entry_block(arena, "entry", 1, 1);
        SsaInstRef slot = bir_add_stack_slot(arena, pointer_type, module.type_i64,
                                             0, 8, 0, 1, 1);
        CHECK(pointer_type && view_type && slot != SSA_INST_NONE);
        CHECK(bir_block_add_inst(arena, entry, slot));
        SsaValueRef pointer = bir_inst_result(arena, slot, 1, 1);
        SsaValueRef length = bir_add_const(arena,
            bir_scalar_i64(module.type_i64, 1), 1, 1);
        SsaInstRef make = bir_add_view_make(arena, view_type, module.type_i64,
                                            pointer, length, 1, 1);
        CHECK(make != SSA_INST_NONE);
        CHECK(bir_block_add_inst(arena, entry, make));
        SsaValueRef view = bir_inst_result(arena, make, 1, 1);
        SsaInstRef free_inst = bir_add_buffer_free(arena, view, 1, 1);
        CHECK(free_inst != SSA_INST_NONE);
        CHECK(bir_block_add_inst(arena, entry, free_inst));
        SsaValueRef result = bir_add_const(arena,
            bir_scalar_i64(module.type_i64, 0), 1, 1);
        CHECK(bir_set_return(arena, entry, result, 1, 1));
        CHECK(bir_register_function_info(&module, "main", entry, 0, NULL,
                                         module.type_i64, true));
        char err[512] = {0};
        CHECK(!bir_verify(&module, err, sizeof(err)));
        CHECK(strstr(err, "buffer free") != NULL ||
              strstr(err, "owned") != NULL || strstr(err, "contract") != NULL);
        bir_module_free(&module);
    }
}

static void test_f32_scalar_pipeline(void) {
    ASTNode *program = parse_program(
        "def add(a: f32, b: f32) -> f32: { return a + b }\n"
        "def main() -> f32: {\n"
        "  x = add(1.25, 2.5)\n"
        "  if x > 3.0: { return x * 2.0 }\n"
        "  return 0.0\n"
        "}\n");
    CHECK(program != NULL);
    if (!program) return;
    BackendIrModule module;
    bir_module_init(&module, "<unit:f32>");
    char err[512] = {0};
    CHECK(bir_build_program(&module, program));
    CHECK(bir_verify(&module, err, sizeof(err)));
    if (err[0]) fprintf(stderr, "f32 verify: %s\n", err);
    char *dump = NULL;
    size_t dump_len = 0;
    FILE *dump_file = open_memstream(&dump, &dump_len);
    CHECK(dump_file != NULL);
    bir_dump(&module, dump_file);
    fclose(dump_file);
    CHECK(dump && strstr(dump, "f32") != NULL &&
          strstr(dump, "f32bits(0x3fa00000)") != NULL);
    CHECK(dump && strstr(dump, "stack_slot") != NULL);
    CHECK(dump && strstr(dump, "load(v") != NULL);
    CHECK(dump && strstr(dump, "store(v") != NULL);
    free(dump);
    BirScalarValue result = {0};
    CHECK(bir_eval_function_value(&module, "main", &result));
    CHECK(result.kind == BIR_SCALAR_F32);
    CHECK(bir_scalar_as_f32(result) > 7.49f && bir_scalar_as_f32(result) < 7.51f);
    bir_module_free(&module);
    ast_free(program);
}

/* ------------------------------------------------------------------ */

int main(void) {
    test_differential();
    test_unit_subset();
    test_load_store();
    test_aggregate_memory();
    test_typed_memory_rejected();
    test_edge_type_rejected();
    test_return_type_rejected();
    test_opcode_signature_rejected();
    test_parameter_signature_rejected();
    test_call_result_signature_rejected();
    test_unreachable_block();
    test_unreachable_return_type_rejected();
    test_missing_terminator_rejected();
    test_arity_mismatch_rejected();
    test_use_before_def_rejected();
    test_generic_rejected();
    test_source_scalar_generic_boundaries();
    test_source_scalar_generic_structs();
    test_source_scalar_generic_collections();
    test_source_generic_writable_slices();
    test_typed_call_rejected();
    test_nonfinalized_type_rejected();
    test_unknown_callee_rejected();
    test_printer_deterministic();
    test_call_abi_metadata();
    test_mir_lowering();
    test_mir_allocation();
    test_x86_64_emission();
    test_x86_native_matrix();
    test_x86_native_scalar_structs();
    test_x86_native_array_fields();
    test_x86_native_nested_arrays();
    test_x86_native_struct_arrays();
    test_x86_native_struct_buffers();
    test_x86_native_dicts();
    test_source_struct_sum_payloads();
    test_source_owning_sum_payloads();
    test_source_nested_owning_structs();
    test_source_sum_match();
    test_source_enum_payloads();
    test_source_owning_enum_payloads();
    test_source_callee_saved_registers();
    test_source_object_emitter_coverage();
    test_source_object_emitter_coverage2();
    test_source_object_emitter_coverage3();
    test_source_object_emitter_coverage4();
    test_x86_native_readonly_views();
    test_x86_native_writable_views();
    test_x86_native_owned_slices();
    test_x86_native_scalar_sums();
    test_f32_scalar_pipeline();
    test_source_owned_buffers();
    test_buffer_ir_rejected();
    test_source_fixed_arrays();
    test_writable_view_borrow_conflicts();
    test_source_readonly_slice_lowering();
    test_source_readonly_slice_rejections();
    test_source_owned_slices();
    test_source_region_lowering();
    test_source_owned_slice_rejections();
    test_source_sums();
    test_source_owning_sums();
    test_nested_owning_sums();
    test_nested_owning_sum_ir_rejected();
    test_source_strings();
    test_source_enums();
    test_source_scalar_types();
    test_nested_sums();
    test_audit_regressions();
    test_scalar_struct_pipeline();
    test_struct_call_abi();
    test_f32_dump_bit_patterns();
    test_source_owned_struct_fields();
    test_randomized_cfg();
    test_typed_scalar_payload_rejected();
    test_pointer_ownership_contracts();
    test_region_lifetimes();
    test_borrowed_readonly_views();
    test_returned_view_escape_analysis();
    test_owned_slices();
    test_path_sensitive_ownership();

    printf("backend-ir tests: %d checks, %d failures\n", checks, failures);
    if (failures) {
        fprintf(stderr, "backend-ir tests FAILED\n");
        return 1;
    }
    printf("backend-ir tests OK\n");
    return 0;
}
