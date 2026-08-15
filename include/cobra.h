/*
 * Cobra Systems Language Compiler Core Header
 *
 * Copyright (c) 2026 The Cobra Project Authors.
 * Direct-to-assembly compiler engine with Scope-Arena memory,
 * AVX2 hardware SIMD vectorization, and multi-target code generation.
 */

#ifndef COBRA_H
#define COBRA_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/wait.h>

#define COBRA_VERSION_STRING "1.0.0"
#define COBRA_MAX_TOKEN_TEXT 128
#define COBRA_MAX_IDENT_LEN  64
#define COBRA_MAX_SOURCE_PATH 512
#define COBRA_MAX_ASM_LEN    512
#define COBRA_MAX_SHAPE_DIMS 8
#define COBRA_MAX_STRUCTS 32
#define COBRA_MAX_STRUCT_FIELDS 32
#define COBRA_MAX_ENUMS 32
#define COBRA_MAX_ENUM_VARIANTS 64
#define COBRA_NATIVE_TENSOR_MAX_RANK 2
/* Native sum storage uses an eight-byte tag and one eight-byte slot for each
   scalar payload component, matching the direct emitter ABI. */
#define COBRA_NATIVE_SUM_TAG_SIZE 8
#define COBRA_NATIVE_SUM_SCALAR_SIZE 8

/* Tensor metadata reserves room for future higher-rank contracts, but the
   current direct native backend intentionally supports rank-1/rank-2 storage
   and indexing only. */
#define COBRA_VIEW_MAX_RANK 8

/* Token Types */
typedef enum {
    TOKEN_EOF = 0,
    TOKEN_DEF,
    TOKEN_LET,
    TOKEN_VAR,
    TOKEN_HEAP,
    TOKEN_STRUCT,
    TOKEN_ENUM,
    TOKEN_MATCH,
    TOKEN_CASE,
    TOKEN_IMPORT,
    TOKEN_COMPTIME,
    TOKEN_COMPUTE,
    TOKEN_PARALLEL,
    TOKEN_GPU_DIRECTIVE,
    TOKEN_CPU_DIRECTIVE,
    TOKEN_FOR,
    TOKEN_IN,
    TOKEN_RETURN,
    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_WHILE,
    TOKEN_ASM,
    TOKEN_PRINT,
    TOKEN_ASSERT,
    TOKEN_NOT,   // not (currently only the 'not in' membership form)
    TOKEN_CONST,
    TOKEN_TRUE,
    TOKEN_FALSE,
    TOKEN_NONE,
    TOKEN_LEN,
    TOKEN_WITH,
    TOKEN_REGION,
    TOKEN_TYPE_I32,     // i32
    TOKEN_TYPE_I64,     // i64
    TOKEN_TYPE_U8,      // u8
    TOKEN_TYPE_U32,     // u32
    TOKEN_TYPE_U64,     // u64
    TOKEN_TYPE_F32,     // f32
    TOKEN_TYPE_F64,     // f64
    TOKEN_TYPE_V256,    // v256
    TOKEN_TYPE_VOID,    // void
    TOKEN_TYPE_STRING,   // string
    TOKEN_TYPE_BOOL,     // bool
    TOKEN_IDENTIFIER,
    TOKEN_INT_LITERAL,
    TOKEN_FLOAT_LITERAL,
    TOKEN_STRING_LITERAL,
    TOKEN_COLON,
    TOKEN_SEMICOLON,
    TOKEN_COMMA,
    TOKEN_ARROW,
    TOKEN_PIPE,
    TOKEN_ASSIGN,
    TOKEN_EQ,
    TOKEN_NEQ,
    TOKEN_LT,
    TOKEN_GT,
    TOKEN_LTE,
    TOKEN_GTE,
    TOKEN_PLUS,
    TOKEN_MINUS,
    TOKEN_STAR,
    TOKEN_SLASH,
    TOKEN_DOT,
    TOKEN_QUESTION,
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_LBRACE,
    TOKEN_RBRACE,
    TOKEN_LBRACKET,
    TOKEN_RBRACKET,
    TOKEN_UNKNOWN
} TokenType;

typedef struct {
    TokenType type;
    char text[COBRA_MAX_TOKEN_TEXT];
    int line;
    int col;
} Token;

typedef struct {
    const char *source;
    size_t length;
    size_t cursor;
    int line;
    int col;
} Lexer;

void lexer_init(Lexer *lexer, const char *source);
Token lexer_next_token(Lexer *lexer);

/* Hardware Execution Targets & AST Nodes */
typedef enum {
    TARGET_DEV_CPU = 0,
    TARGET_DEV_GPU_VECTOR,
    TARGET_DEV_NPU,
    /* A function under an `@gpu` directive: its body is lowered to a real
       SPIR-V compute kernel (see src/gpu_lower.c), not just tagged and left
       on the CPU path the way TARGET_DEV_GPU_VECTOR currently is. */
    TARGET_DEV_GPU_KERNEL
} TargetDevice;

typedef enum {
    AST_PROGRAM = 0,
    AST_FUNCTION,
    AST_PARAM,
    AST_FUNC_CALL,
    AST_VAR_DECL,
    AST_HEAP_DECL,
    AST_STRUCT_DECL,
    AST_ENUM_DECL,
    AST_MATCH_STMT,
    AST_MATCH_CASE,
    AST_MEMBER_ACCESS,
    AST_IMPORT_DECL,
    AST_COMPTIME_EXPR,
    AST_COMPUTE_BLOCK,
    AST_PARALLEL_BLOCK,
    AST_WITH_REGION,
    AST_FOR_LOOP,
    AST_ARRAY_LITERAL,
    AST_DICT_LITERAL,
    AST_DICT_ENTRY,
    AST_ASSIGN,
    AST_INDEX_ASSIGN,
    AST_IF_STMT,
    AST_WHILE_STMT,
    AST_RETURN,
    AST_PRINT_STMT,
    AST_ASSERT_STMT,
    AST_LEN_EXPR,
    AST_ARRAY_INDEX,
    AST_BINARY_OP,
    AST_INT_LITERAL,
    AST_FLOAT_LITERAL,
    AST_STRING_LITERAL,
    AST_VAR_REF,
    AST_ASM_BLOCK,
    AST_INSPECT_STMT,
    AST_MEMBERSHIP,     // element in collection (dict has / list scan)
    AST_COMPREHENSION,  // [expr for target in source]  (optional if guard)
    AST_BOOL_LITERAL,   // true / false
    AST_NONE_LITERAL,   // none
    AST_MEMBER_ASSIGN   // struct_var.field = value
} ASTNodeType;

typedef enum {
    COBRA_TYPE_UNTYPED = 0,
    COBRA_TYPE_I32,
    COBRA_TYPE_I64,
    COBRA_TYPE_U8,
    COBRA_TYPE_U32,
    COBRA_TYPE_U64,
    COBRA_TYPE_F32,
    COBRA_TYPE_F64,
    COBRA_TYPE_V256,
    COBRA_TYPE_VOID,
    COBRA_TYPE_STRING,
    COBRA_TYPE_POINTER,      /* typed backend/native pointer */
    COBRA_TYPE_ARRAY,
    COBRA_TYPE_SLICE,       /* zero-residency pointer + length view of i64 */
    COBRA_TYPE_SLICE_F32,   /* compatibility pointer + length view of f32 */
    COBRA_TYPE_SLICE_U8,    /* byte pointer + length view of u8 */
    COBRA_TYPE_TENSOR_F32,  /* adaptive f32 tensor/view descriptor */
    COBRA_TYPE_LIST,         /* owned growable typed sequence */
    COBRA_TYPE_DICT,         /* owned string-key scalar map */
    COBRA_TYPE_BOOL,         /* true / false, stored as 0 or 1 */
    COBRA_TYPE_NONE,         /* the none literal */
    COBRA_TYPE_OPTION,       /* native tagged optional scalar value */
    COBRA_TYPE_RESULT,       /* native tagged success/error scalar value */
    COBRA_TYPE_GENERIC_PARAM,/* canonical placeholder used during instantiation */
    COBRA_TYPE_ENUM,         /* integer-backed unit enum (type_name holds name) */
    COBRA_TYPE_STRUCT,       /* user-defined struct type (type_name holds name) */
    COBRA_TYPE_UNKNOWN
} CobraTypeKind;

/* Canonical recursive type metadata. Parser declarations attach this graph
   directly; transitional native symbol fields may mirror ABI details, but new
   compiler phases must use this graph instead of inventing parallel type
   identity or ownership metadata. */
#define COBRA_MAX_TYPE_ARGS 8
#define COBRA_MAX_TYPE_FIELDS 32
#define COBRA_MAX_ARRAY_ELEMENTS 64
#define COBRA_MAX_TYPE_NODES 2048

typedef enum {
    COBRA_OWNERSHIP_VALUE = 0,
    COBRA_OWNERSHIP_BORROWED,
    COBRA_OWNERSHIP_OWNED,
    COBRA_OWNERSHIP_REGION
} CobraOwnershipKind;

typedef enum {
    COBRA_MUTABILITY_DEFAULT = 0,
    COBRA_MUTABILITY_READONLY,
    COBRA_MUTABILITY_MUTABLE,
    COBRA_MUTABILITY_OUT
} CobraMutabilityKind;

typedef enum {
    COBRA_ABI_INVALID = 0,
    COBRA_ABI_VOID,
    COBRA_ABI_GPR,
    COBRA_ABI_XMM,
    COBRA_ABI_SLICE,
    COBRA_ABI_SUM_INDIRECT,
    COBRA_ABI_STRUCT_VALUE,
    COBRA_ABI_REFERENCE
} CobraAbiKind;

typedef struct CobraType CobraType;

typedef struct {
    const CobraType *parameter;
    const CobraType *argument;
} CobraTypeBinding;

typedef struct {
    char name[COBRA_MAX_IDENT_LEN];
    const CobraType *type;
    size_t offset;
    CobraOwnershipKind ownership;
    CobraMutabilityKind mutability;
    int region_id;
} CobraTypeField;

struct CobraType {
    CobraTypeKind kind;
    char name[COBRA_MAX_IDENT_LEN];
    /* Generic arguments are authoritative. Accessors below expose the value,
       error, key, and element roles without duplicating mutable pointers. */
    const CobraType *generic_args[COBRA_MAX_TYPE_ARGS];
    size_t generic_arg_count;
    CobraTypeField fields[COBRA_MAX_TYPE_FIELDS];
    size_t field_count;
    /* Fixed value-array length. Meaningful only for COBRA_TYPE_ARRAY. */
    size_t array_length;
    CobraOwnershipKind ownership;
    CobraMutabilityKind mutability;
    int region_id;
    CobraAbiKind abi;
    size_t size;
    size_t alignment;
    bool finalized;
    /* Originating template for a specialization. Generated names are symbols
       only; canonical identity uses this origin plus substituted arguments. */
    const CobraType *template_origin;
    /* Set while cobra_type_struct_layout is populating this node, so a
       by-value self or mutual reference during population is detected as a
       cycle instead of recursing forever. */
    bool populating;
};

typedef struct {
    CobraType nodes[COBRA_MAX_TYPE_NODES];
    size_t count;
    char error[COBRA_MAX_TOKEN_TEXT];
} CobraTypeArena;

void cobra_type_arena_init(CobraTypeArena *arena);
CobraType *cobra_type_new(CobraTypeArena *arena, CobraTypeKind kind);
CobraType *cobra_type_named(CobraTypeArena *arena, CobraTypeKind kind, const char *name);
/* Construct a declaration type directly from parsed canonical components. The
   returned descriptor is not finalized until semantic validation completes. */
CobraType *cobra_type_make(CobraTypeArena *arena, CobraTypeKind kind, const char *name,
                           const CobraType *element, const CobraType *error,
                           const CobraType *key, const CobraType *value,
                           CobraOwnershipKind ownership, CobraMutabilityKind mutability,
                           int region_id);
bool cobra_type_add_generic_arg(CobraType *type, const CobraType *argument);
/* Attach one variant payload to a payload-carrying enum descriptor (NULL marks
   a unit variant). Legal only before finalization. */
bool cobra_type_add_variant_payload(CobraType *type, const CobraType *payload);
/* Recursively substitute canonical generic metadata, preserving ownership,
   mutability, region origin, field layout, ABI finalization, and interning.
   The current contract is exactly one scalar binding; multi-parameter
   substitution is intentionally rejected until the specialization model is
   extended. */
CobraType *cobra_type_substitute(CobraTypeArena *arena,
                                 const CobraType *template_type,
                                 const CobraTypeBinding *bindings,
                                 size_t binding_count,
                                 const char *specialized_name);
const CobraType *cobra_type_element(const CobraType *type);
const CobraType *cobra_type_error(const CobraType *type);
const CobraType *cobra_type_key(const CobraType *type);
const CobraType *cobra_type_value(const CobraType *type);
bool cobra_type_add_field(CobraType *type, const char *name, const CobraType *field_type,
                          CobraOwnershipKind ownership, CobraMutabilityKind mutability,
                          int region_id);
bool cobra_type_equal(const CobraType *left, const CobraType *right);
bool cobra_type_finalize(CobraTypeArena *arena, CobraType *type);
bool cobra_type_validate(CobraTypeArena *arena, const CobraType *type);
const char *cobra_type_kind_name(CobraTypeKind kind);
/* Native calling-convention slot count for a finalized canonical type. This
   mirrors the direct emitter's GPR-slot accounting: XMM-class scalars use zero
   GPR slots, slices use two, lists three, dicts two, and indirect sums one. */
int cobra_type_abi_slots(const CobraType *type);
bool cobra_type_is_scalar(const CobraType *type);
bool cobra_type_is_slice_kind(CobraTypeKind kind);
bool cobra_type_is_borrowed_view(const CobraType *type);
bool cobra_type_bind_generic(const CobraType *pattern, const CobraType *actual,
                             const CobraType *parameter, const CobraType **binding);

/* Recursive Descent Parser */
typedef struct ASTNode ASTNode;

struct ASTNode {
    ASTNodeType type;
    TargetDevice target_device;
    CobraTypeKind value_type;
    CobraTypeKind declared_type;
    int bit_width;
    int shape_rank;
    char shape_dims[COBRA_MAX_SHAPE_DIMS][COBRA_MAX_IDENT_LEN];
    /* Fixed value-array length for the backend source form array[T, N]. */
    size_t array_length;
    
    char name[COBRA_MAX_IDENT_LEN];
    /* Optional second loop target for Python-style `for index, value in ...`. */
    char secondary_name[COBRA_MAX_IDENT_LEN];
    /* The variable a comprehension result is stored into (declaration name). */
    char comprehension_target[COBRA_MAX_IDENT_LEN];
    int int_val;
    /* Full-precision literal magnitude. int_val is the low 32 bits for
       compatibility with contexts that need a small integer; literal_i64 is
       authoritative for integer literal values, literal_f64 for float
       literals (float_val remains the f32-truncated form). */
    int64_t literal_i64;
    /* Full-precision unsigned magnitude for literals that exceed INT64_MAX
       (u64 literals such as 18446744073709551615). literal_i64 holds the
       same bit pattern; literal_is_unsigned records that the source form was
       a positive magnitude too large for a signed value. */
    uint64_t literal_u64;
    bool literal_is_unsigned;
    float float_val;
    double literal_f64;
    char string_val[COBRA_MAX_TOKEN_TEXT];
    char asm_code[COBRA_MAX_ASM_LEN];
    /* Optional inline-asm operand binding: `asm(in a, b out result): { ... }`
       loads named i64 locals into the fixed SysV argument registers
       (rdi, rsi, rdx, rcx, r8, r9, in order) before the raw block and stores
       rax into the named output local afterward, so the block can reference
       those registers directly instead of the caller hand-rolling movs. */
    char asm_inputs[6][COBRA_MAX_IDENT_LEN];
    int asm_input_count;
    char asm_output[COBRA_MAX_IDENT_LEN];
    bool asm_has_output;
    /* Compile-time source location; never emitted into native output. */
    int source_line;
    int source_col;
    char source_file[COBRA_MAX_SOURCE_PATH];
    /* Enum names on match arms are syntax, not type metadata. Semantic type
       identity for every other expression lives in canonical_type. */
    char match_type_name[COBRA_MAX_IDENT_LEN];
    /* A call marked with postfix `?` propagates a nonzero native status
       through the current integer-returning function. */
    bool propagate_error;
    /* Set by validation only when a string expression allocates fresh storage
       (concat, string '+', or a function proven to return one). Codegen uses
       this shared fact so borrowed string forwarding stays non-owning. */
    bool fresh_string_result;
    /* const bindings are immutable: the IR rejects later assignment. */
    bool is_const;
    /* Source-module visibility. Unannotated functions remain public for
       compatibility; explicit private declarations are checked at boundaries. */
    bool is_public;
    bool has_visibility;
    /* Match cases use this flag for the single `else` arm. */
    bool is_default_case;
    /* Source imports are compile-time composition; C imports retain linker
       metadata and are handled by the native C bridge. */
    bool source_import;
    /* Optional source-module alias used only for qualified calls such as
       `math.module_add(...)`; native symbol emission remains direct. */
    char module_alias[COBRA_MAX_IDENT_LEN];
    /* Optional qualifier on a function call; empty means an ordinary call. */
    char qualifier[COBRA_MAX_IDENT_LEN];
    /* Canonical type attached during parsing or IR inference. Descriptors are
       immutable after construction; the AST only borrows the arena node. */
    const CobraType *canonical_type;
    /* Source-level generic functions currently support one scalar parameter.
       The placeholder descriptors are owned by the program canonical arena. */
    size_t generic_param_count;
    char generic_param_names[COBRA_MAX_TYPE_ARGS][COBRA_MAX_IDENT_LEN];
    const CobraType *generic_param_types[COBRA_MAX_TYPE_ARGS];
    /* Specialized clones point back to their generic declaration so recursion
       and duplicate specialization checks remain explicit in IR. */
    const ASTNode *specialized_from;
    /* Canonical arguments identify a specialization; the generated function
       name is only an assembly symbol and must not decide reuse. */
    size_t specialization_arg_count;
    const CobraType *specialization_args[COBRA_MAX_TYPE_ARGS];
    /* Only the AST_PROGRAM node owns this arena. Child nodes keep descriptors
       from it but leave this pointer NULL. */
    CobraTypeArena *canonical_arena;
    
    ASTNode **children;
    size_t child_count;
    size_t child_capacity;
};

/* Canonical roles for AST expressions and declarations. These accessors expose
   semantic type identity without reading transitional AST component caches. */
CobraTypeKind cobra_type_node_kind(const ASTNode *node);
const CobraType *cobra_type_node_element(const ASTNode *node);
const CobraType *cobra_type_node_error(const ASTNode *node);
const CobraType *cobra_type_node_key(const ASTNode *node);
const CobraType *cobra_type_node_value(const ASTNode *node);
const char *cobra_type_node_name(const ASTNode *node);
const char *cobra_type_node_error_name(const ASTNode *node);

/* Build (or reuse) and finalize the canonical struct layout for a named struct
   declaration. Returns NULL when the layout is invalid; the arena error string
   carries the reason. The packed size and per-field offsets are authoritative. */
const CobraType *cobra_type_struct_layout(CobraTypeArena *arena, ASTNode *root, const char *name);
/* Canonical field byte offset for (struct, field); -1 when not found. Codegen
   reads offsets here instead of per-node fields written by IR. */
int cobra_type_field_offset(CobraTypeArena *arena, ASTNode *root,
                            const char *struct_name, const char *field_name);

ASTNode *ast_create_node(ASTNodeType type, const char *name);
void ast_add_child(ASTNode *parent, ASTNode *child);
void ast_free(ASTNode *node);
void ast_print(ASTNode *node, int indent);

/* Field ownership classes shared by the IR borrow checks and codegen. */
typedef enum {
    COBRA_FIELD_SCALAR = 0,
    COBRA_FIELD_BORROWED_VIEW,
    COBRA_FIELD_OWNED_VALUE
} CobraFieldOwnership;

/* Compile-Time AST Interpreter */
void interpreter_reset_steps(void);
int interpreter_eval_expr(ASTNode *node);

/* Recursive Descent Parser */
typedef struct {
    Lexer lexer;
    Token current_token;
    char source_file[COBRA_MAX_SOURCE_PATH];
    CobraTypeArena *canonical_arena;
    size_t generic_param_count;
    char generic_param_names[COBRA_MAX_TYPE_ARGS][COBRA_MAX_IDENT_LEN];
    const CobraType *generic_param_types[COBRA_MAX_TYPE_ARGS];
    /* Set by a top-level `@gpu` directive and cleared by `@cpu`; every `def`
       parsed while set is tagged TARGET_DEV_GPU_KERNEL instead of CPU. A
       file whose first directive is `@gpu` therefore targets GPU throughout. */
    bool gpu_directive_active;
} Parser;

void parser_init(Parser *parser, const char *source);
void parser_init_with_file(Parser *parser, const char *source, const char *source_file);
ASTNode *parser_parse_program(Parser *parser);

/* Customizable Terminal Icon Set */
typedef struct {
    const char *cobra;
    const char *bolt;
    const char *inspect;
    const char *test;
    const char *fmt;
    const char *rocket;
    const char *pass;
    const char *pkg;
} CobraIconSet;

/* Target Code Generator */
typedef enum {
    TARGET_LINUX_X86_64 = 0,
    TARGET_WIN64_X86_64,
    TARGET_MACOS_ARM64,
    TARGET_WASM32
} TargetPlatform;

typedef struct {
    ASTNode *root;
    bool valid;
    size_t error_count;
} CobraIR;

bool cobra_ir_build(ASTNode *root, CobraIR *ir);
bool codegen_generate_assembly(ASTNode *root, const char *output_asm_path, TargetPlatform target);
/* Native test assembly omits Cobra's source-level main entry point. */
bool codegen_generate_test_assembly(ASTNode *root, const char *output_asm_path, TargetPlatform target);
/* Enable or disable auto-vectorization of index-pure []f32 loops. */
void codegen_set_vectorize(bool enabled);
/* Mark the build as a portable scalar emission, so unreachable AVX-backed
   library helpers are not copied into the output object. */
void codegen_set_portable(bool enabled);
/* --no-gpu: gpu_available()/gpu_device_count()/gpu_selftest() compile to
   constant 0 and the GPU runtime is never linked, regardless of whether the
   program calls them. For CI without a GPU, reproducible builds, and CPU-
   only benchmarking baselines. */
void codegen_set_gpu_enabled(bool enabled);

/* Test-only host evaluator. It is retained for the lightweight evaluator API. */
bool interpreter_run_function(ASTNode *root, const char *function_name, int *return_code);

#endif /* COBRA_H */
