/*
 * Cobra Backend IR: typed HIR/CFG + flat block-argument SSA.
 *
 * Isolated foundation for a future backend. Not linked into the production
 * compiler yet. Consumes the existing AST for a small scalar and immutable
 * scalar-struct subset and produces a flat arena-backed SSA form with stable
 * integer handles,
 * variable-length operands, and explicit edge arguments. See
 * docs/BACKEND_IR.md for the full design.
 */
#ifndef COBRA_BACKEND_IR_H
#define COBRA_BACKEND_IR_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "../../include/cobra.h"

#define BIR_MAX_FUNCTIONS 256
#define BIR_MAX_CALLEE_NAME COBRA_MAX_IDENT_LEN
#define BIR_MAX_PARAMS 32
#define BIR_MAX_SSA_PARAMS (BIR_MAX_PARAMS + 1)
#define BIR_MAX_LOCALS 128
#define BIR_MAX_STEPS 10000000
#define BIR_MAX_CALL_DEPTH 256
#define BIR_MEMORY_SLOTS 8192
#define BIR_STACK_BYTES 65536
#define BIR_MAX_STACK_SLOTS 256
#define BIR_MAX_AGGREGATES 128
#define BIR_MAX_ARRAY_ELEMENTS 64
#define BIR_MAX_REGIONS 128
#define BIR_MAX_ENUMS 32
#define BIR_ABI_MAX_PARTS 4
#define BIR_ABI_MAX_GPR_ARGUMENT_REGISTERS 6
#define BIR_ABI_MAX_XMM_ARGUMENT_REGISTERS 8
#define BIR_ABI_STACK_ALIGNMENT 16
#define BIR_REGION_NONE 0U

/* ABI metadata is target-neutral: register ordinals are abstract positions
   within a register class, not physical register names. The initial profile
   has the register and stack budgets required by the first native target. */
typedef enum {
    BIR_CALLING_CONVENTION_COBRA = 1
} BirCallingConvention;

typedef enum {
    BIR_ABI_STORAGE_NONE = 0,
    BIR_ABI_STORAGE_REGISTER,
    BIR_ABI_STORAGE_STACK
} BirAbiStorageKind;

typedef enum {
    BIR_ABI_PASS_DIRECT = 0,
    BIR_ABI_PASS_INDIRECT
} BirAbiPassMode;

typedef enum {
    BIR_ABI_REGISTER_NONE = 0,
    BIR_ABI_REGISTER_GPR,
    BIR_ABI_REGISTER_XMM
} BirAbiRegisterClass;

typedef struct {
    BirAbiStorageKind storage;
    BirAbiPassMode pass_mode;
    BirAbiRegisterClass register_class;
    uint16_t register_index;
    uint32_t stack_offset;
    uint32_t size;
    uint32_t alignment;
} BirAbiLocation;

typedef struct {
    uint8_t count;
    BirAbiLocation parts[BIR_ABI_MAX_PARTS];
} BirAbiValueLocations;

typedef struct {
    BirCallingConvention convention;
    uint32_t stack_alignment;
    uint32_t stack_size;
    size_t param_count; /* lowered parameters, including hidden storage */
    BirAbiValueLocations params[BIR_MAX_SSA_PARAMS];
    BirAbiValueLocations returns;
    bool variadic;
} BirCallAbi;


/* ------------------------------------------------------------------ */
/* Handles: stable integer identity into flat arena pools.            */
/* ------------------------------------------------------------------ */

typedef uint32_t SsaValueRef;
typedef uint32_t SsaInstRef;
typedef uint32_t SsaBlockRef;
typedef uint32_t HirBlockRef;

/* Typed value payload used by HIR, SSA constants, evaluator slots, calls,
   returns, and typed frame memory. It includes scalar values and the isolated
   borrowed slice view (pointer plus length). Floating-point values are stored
   by IEEE-754 bit pattern so the representation is deterministic and never
   aliases an integer through an incompatible C type. */
typedef enum {
    BIR_SCALAR_INVALID = 0,
    BIR_SCALAR_I64,
    BIR_SCALAR_I32,
    BIR_SCALAR_U32,
    BIR_SCALAR_U64,
    BIR_SCALAR_BOOL,
    BIR_SCALAR_F32,
    BIR_SCALAR_F64,
    BIR_SCALAR_U8,
    BIR_SCALAR_POINTER,
    BIR_SCALAR_VIEW
} BirScalarKind;

/* Static pointer contracts are deliberately separate from canonical
   pointer[T] identity. They describe what a value may do at a particular
   IR point and are refined when a pointer crosses a call boundary. */
typedef enum {
    BIR_POINTER_CONTRACT_UNKNOWN = 0,
    BIR_POINTER_CONTRACT_OWNED_FRAME,
    BIR_POINTER_CONTRACT_OWNED_REGION,
    BIR_POINTER_CONTRACT_OWNED_SLICE,
    BIR_POINTER_CONTRACT_BORROW_READONLY,
    BIR_POINTER_CONTRACT_BORROW_WRITE,
    BIR_POINTER_CONTRACT_CALLER_STORAGE
} BirPointerContract;

typedef enum {
    BIR_POINTER_ORIGIN_UNKNOWN = 0,
    BIR_POINTER_ORIGIN_FRAME,
    BIR_POINTER_ORIGIN_REGION,
    BIR_POINTER_ORIGIN_CALLER
} BirPointerOrigin;

typedef struct {
    uint32_t frame_id;
    uint32_t allocation_id;
    uint32_t region_id;
    int64_t offset;
    int64_t allocation_base_offset;
    uint32_t allocation_size;
    int64_t view_base_offset;
    int64_t view_length;
    uint32_t view_element_width;
    BirPointerContract contract;
    BirPointerOrigin origin;
} BirPointerValue;

typedef struct {
    BirPointerValue pointer;
    int64_t length;
    int64_t capacity;
} BirViewValue;

typedef struct {
    uint32_t id;
    uint32_t parent_id;
    bool declared;
} BirRegionInfo;

typedef struct {
    const CobraType *type;
    BirScalarKind kind;
    union {
        int64_t i64;
        uint32_t f32_bits;
        uint64_t f64_bits;
        BirPointerValue pointer;
        BirViewValue view;
    } payload;
} BirScalarValue;

BirScalarValue bir_scalar_i64(const CobraType *type, int64_t value);
BirScalarValue bir_scalar_i32(const CobraType *type, int32_t value);
BirScalarValue bir_scalar_u32(const CobraType *type, uint32_t value);
BirScalarValue bir_scalar_u64(const CobraType *type, uint64_t value);
BirScalarValue bir_scalar_bool(const CobraType *type, bool value);
BirScalarValue bir_scalar_f32(const CobraType *type, float value);
BirScalarValue bir_scalar_f64(const CobraType *type, double value);
BirScalarValue bir_scalar_u8(const CobraType *type, uint8_t value);
BirScalarValue bir_scalar_pointer(const CobraType *type, uint32_t frame_id, int64_t offset);
BirScalarValue bir_scalar_pointer_with_contract(const CobraType *type, uint32_t frame_id,
                                                int64_t offset, BirPointerContract contract);
float bir_scalar_as_f32(BirScalarValue value);
double bir_scalar_as_f64(BirScalarValue value);
BirScalarValue bir_scalar_view(const CobraType *type, BirPointerValue pointer,
                               int64_t length);
BirScalarValue bir_scalar_buffer(const CobraType *type, BirPointerValue pointer,
                                 int64_t length, int64_t capacity);
BirPointerValue bir_scalar_as_pointer(BirScalarValue value);
bool bir_scalar_is_zero(BirScalarValue value);

#define HIR_BLOCK_NONE ((HirBlockRef)0)

#define SSA_VALUE_NONE ((SsaValueRef)0)
#define SSA_INST_NONE  ((SsaInstRef)0)
#define SSA_BLOCK_NONE ((SsaBlockRef)0)

typedef enum {
    SSA_VALUE_INVALID = 0,
    SSA_VALUE_PARAM,       /* function parameter                      */
    SSA_VALUE_BLOCK_PARAM, /* block argument (join value)             */
    SSA_VALUE_CONST,       /* typed scalar constant                  */
    SSA_VALUE_INST         /* instruction result                      */
} SsaValueKind;

typedef enum {
    SSA_OP_NONE = 0,
    SSA_OP_CONST,
    SSA_OP_PARAM,          /* implicit function parameter value       */
    SSA_OP_BLOCK_ARG,      /* implicit block parameter value          */
    SSA_OP_ADD,
    SSA_OP_SUB,
    SSA_OP_MUL,
    SSA_OP_DIV,
    SSA_OP_REM,
    SSA_OP_NEG,
    SSA_OP_CONVERT,        /* scalar numeric/bool -> scalar numeric/bool, runtime conversion */
    SSA_OP_EQ,
    SSA_OP_NE,
    SSA_OP_LT,
    SSA_OP_LE,
    SSA_OP_GT,
    SSA_OP_GE,
    SSA_OP_STACK_SLOT,      /* typed pointer to a frame-local slot       */
    SSA_OP_PTR_ADD,         /* pointer plus signed byte offset            */
    SSA_OP_FIELD_ADDR,      /* address of a canonical aggregate field      */
    SSA_OP_ARRAY_INDEX_ADDR,/* bounds-checked address of a fixed-array item */
    SSA_OP_LOAD,
    SSA_OP_STORE,
    SSA_OP_AGG_COPY,        /* byte-for-byte copy of a value-owned aggregate */
    SSA_OP_REGION_ENTER,    /* begin a region lifetime                      */
    SSA_OP_REGION_EXIT,     /* destroy all allocations owned by a region    */
    SSA_OP_TRANSFER,        /* move one owned allocation into a region      */
    SSA_OP_DESTROY,         /* explicitly destroy one owned allocation      */
    SSA_OP_VIEW_MAKE,       /* pointer + length -> borrowed slice view       */
    SSA_OP_VIEW_PTR,        /* borrowed slice view -> borrowed pointer       */
    SSA_OP_VIEW_LEN,        /* borrowed slice view -> length                 */
    SSA_OP_SLICE_ALLOC,     /* length -> owned slice view (frame allocation)  */
    SSA_OP_SLICE_FREE,      /* owned slice view -> none (destroy allocation)  */
    SSA_OP_BUFFER_ALLOC,    /* length -> owned growable list buffer           */
    SSA_OP_BUFFER_APPEND,   /* buffer, element -> moved grown buffer          */
    SSA_OP_BUFFER_POP,      /* buffer -> element, reducing logical length    */
    SSA_OP_BUFFER_FREE,     /* owned growable buffer -> none                  */
    SSA_OP_DICT_ALLOC,      /* capacity -> owned string-key dict (empty table) */
    SSA_OP_DICT_SET,        /* dict, key view, value -> moved grown dict      */
    SSA_OP_DICT_GET,        /* dict, key view -> value or fallback           */
    SSA_OP_DICT_HAS,        /* dict, key view -> bool                        */
    SSA_OP_DICT_DELETE,     /* dict, key view -> moved dict                   */
    SSA_OP_DICT_POP,        /* dict, key view, fallback -> value, moved dict */
    SSA_OP_DICT_LEN,        /* dict -> logical entry count                    */
    SSA_OP_DICT_FREE,       /* owned dict -> none                             */
    SSA_OP_STRING_CONCAT,    /* two readonly u8 views -> owned string          */
    SSA_OP_STRING_EQ,        /* two readonly u8 views -> bool byte equality    */
    SSA_OP_SUM_PAYLOAD_STORE,/* move an owned payload into a sum field         */
    SSA_OP_SUM_PAYLOAD_LOAD, /* move an owned payload out of a sum field       */
    SSA_OP_SUM_MOVE,         /* move an owning sum between aggregate slots     */
    SSA_OP_SUM_DROP,         /* drop an active owning sum payload              */
    SSA_OP_FIELD_PAYLOAD_STORE, /* move an owned slice into a struct field      */
    SSA_OP_FIELD_PAYLOAD_LOAD,  /* move an owned slice out of a struct field    */
    SSA_OP_AGG_MOVE,         /* move an ownership-bearing aggregate             */
    SSA_OP_AGG_DROP,         /* drop all owned fields in an aggregate           */
    SSA_OP_SUM_CHECK,       /* tag -> none, runtime failure on wrong tag      */
    SSA_OP_PRINT_I64,       /* i64/bool -> none, prints "%ld\n" to stdout     */
    SSA_OP_PRINT_STRING,    /* readonly u8 view -> none, prints "%s\n"        */
    SSA_OP_ASSERT,          /* i64/bool -> none, runtime failure if zero      */
    SSA_OP_CALL,
    SSA_OP_JUMP,           /* terminator                              */
    SSA_OP_BRANCH,         /* terminator                              */
    SSA_OP_RETURN          /* terminator                              */
} SsaOpcode;

/* Memory/effect metadata carried on memory-touching instructions. */
typedef enum {
    SSA_EFFECT_NONE = 0,
    SSA_EFFECT_READ,
    SSA_EFFECT_WRITE,
    SSA_EFFECT_READWRITE,
    SSA_EFFECT_CALL
} SsaEffect;

/* The isolated memory model uses canonical pointer[T] values carrying a frame,
   allocation, region identity, and signed byte offset. Readonly slice views
   add a logical element count without taking ownership. Stack slots live in a
   per-frame byte arena;
   native machine-pointer lowering and global addresses are later milestones. */
typedef enum {
    SSA_ADDRESS_TYPED_POINTER = 0,
    SSA_ADDRESS_NATIVE_POINTER
} SsaAddressKind;

typedef struct {
    SsaValueKind kind;
    const CobraType *type;      /* canonical, finalized                */
    SsaInstRef def_inst;        /* for SSA_VALUE_INST                  */
    uint32_t param_index;       /* for SSA_VALUE_PARAM                 */
    SsaBlockRef block;          /* for SSA_VALUE_BLOCK_PARAM           */
    BirScalarValue const_value; /* for SSA_VALUE_CONST                 */
    BirPointerContract pointer_contract; /* static pointer permission     */
    BirPointerOrigin pointer_origin;     /* static provenance origin       */
    uint32_t region_id;                  /* region owner, or none          */
    uint32_t allocation_id;              /* storage identity, or none      */
    int source_line;
    int source_col;
} SsaValue;

typedef struct {
    SsaOpcode op;
    const CobraType *type;      /* result type; NULL for void insts    */
    SsaValueRef result;         /* defining SSA_VALUE_INST, or NONE    */
    uint32_t operand_start;
    uint32_t operand_count;
    SsaBlockRef target;         /* jump: target; branch: then-target   */
    SsaBlockRef target2;        /* branch: else-target                 */
    uint32_t edge_start;        /* edge args for target                */
    uint32_t edge_count;
    uint32_t edge2_start;       /* edge args for target2               */
    uint32_t edge2_count;
    SsaEffect effect;
    SsaAddressKind address_kind; /* load/store address interpretation   */
    uint32_t memory_width;       /* typed access width in bytes           */
    uint32_t memory_alignment;  /* required typed access alignment         */
    uint32_t address_space;     /* 0 = frame-local memory               */
    const CobraType *memory_type; /* pointee/aggregate type for memory ops  */
    int64_t memory_offset;      /* slot or field byte offset                */
    uint32_t stack_slot;        /* stable slot identity for diagnostics    */
    BirPointerContract pointer_contract; /* result pointer permission       */
    BirPointerOrigin pointer_origin;     /* result provenance origin         */
    uint32_t region_id;                  /* region metadata                   */
    uint32_t allocation_id;              /* allocation identity               */
    uint32_t parent_region_id;           /* region_enter parent               */
    int sum_check_kind;                  /* sum_check expected-tag variant    */
    int64_t sum_check_expected;          /* sum_check explicit expected tag   */
    int64_t view_length;                 /* view result length, if applicable  */
    uint32_t payload_allocation_id;       /* owning sum payload identity         */
    const CobraType *aggregate_type;      /* owning field or aggregate contract  */
    SsaValueRef view_source;              /* source view for native bounds checks */
    bool transient_borrow;                /* view_make: release right after the
                                              call argument it was built for,
                                              instead of living to function end */
    char callee[BIR_MAX_CALLEE_NAME]; /* for SSA_OP_CALL               */
    char dict_key[COBRA_MAX_TOKEN_TEXT]; /* for SSA_OP_DICT_* literal key    */
    int source_line;
    int source_col;
} SsaInst;

typedef struct {
    char name[32];              /* debug label (from HIR blocks)         */
    SsaInstRef *insts;          /* ordered instructions, terminator last */
    size_t inst_count;
    size_t inst_cap;
    SsaInstRef terminator;      /* SSA_INST_NONE until finalized         */
    SsaValueRef *params;        /* block arguments                       */
    size_t param_count;
    size_t param_cap;
    SsaBlockRef *preds;
    size_t pred_count;
    size_t pred_cap;
    SsaBlockRef *succs;
    size_t succ_count;
    size_t succ_cap;
    bool is_entry;
    int source_line;
    int source_col;
} SsaBlock;

/* Flat pools owned by one module. */
typedef struct {
    SsaValue *values;
    size_t value_count;
    size_t value_cap;
    SsaInst *insts;
    size_t inst_count;
    size_t inst_cap;
    SsaBlock *blocks;
    size_t block_count;
    size_t block_cap;
    SsaValueRef *operands;      /* variable-length operand storage */
    size_t operand_used;
    size_t operand_cap;
    SsaValueRef *edges;         /* variable-length edge-argument storage */
    size_t edge_used;
    size_t edge_cap;
} SsaArena;

/* ------------------------------------------------------------------ */
/* Typed HIR / CFG (mutable source locals).                           */
/* ------------------------------------------------------------------ */

typedef enum {
    HIR_EXPR_NONE = 0,
    HIR_EXPR_CONST,
    HIR_EXPR_LOCAL,
    HIR_EXPR_BINOP,
    HIR_EXPR_CALL,
    HIR_EXPR_MEMBER,
    HIR_EXPR_INDEX,
    HIR_EXPR_LEN,
    HIR_EXPR_SLICE,
    HIR_EXPR_ALLOC,   /* alloc_i64/f32/u8 builtin: owned slice allocation   */
    HIR_EXPR_BORROW,  /* owned slice -> borrowed view alias                 */
    HIR_EXPR_SUM_MAKE,    /* some/none/ok/err construction                  */
    HIR_EXPR_SUM_ACCESS,  /* tag/payload/error read with optional tag check  */
    HIR_EXPR_STR_LITERAL, /* borrowed readonly string literal                */
    HIR_EXPR_STR_CONCAT,   /* fresh owned string from two readonly views     */
    HIR_EXPR_STR_EQ,       /* two readonly u8 views -> bool ==/!= (sum_variant negates) */
    HIR_EXPR_BUFFER_APPEND, /* append an element and move the old buffer      */
    HIR_EXPR_BUFFER_POP,    /* pop the last element from a mutable buffer    */
    HIR_EXPR_ARRAY_LITERAL, /* fixed value array literal                     */
    HIR_EXPR_FLOAT_LITERAL, /* float literal: completes to f32 or f64 at a boundary */
    HIR_EXPR_DICT_LITERAL,  /* string-key dict literal construction          */
    HIR_EXPR_DICT_GET,      /* dict lookup with fallback (d["key"] read)      */
    HIR_EXPR_DICT_HAS,      /* dict membership test                          */
    HIR_EXPR_DICT_POP,      /* dict pop with fallback (moves the dict local) */
    HIR_EXPR_DICT_LEN,      /* dict logical entry count                      */
    HIR_EXPR_CAST           /* expr as Type: runtime scalar/bool conversion   */
} HirExprKind;

typedef struct HirExpr HirExpr;
struct HirExpr {
    HirExprKind kind;
    BirScalarValue const_value;
    uint32_t local;           /* for LOCAL                              */
    const CobraType *type;    /* canonical type owned by the backend module */
    SsaOpcode binop;          /* for BINOP                              */
    size_t field_offset;      /* for MEMBER                           */
    const CobraType *aggregate_type; /* for MEMBER base layout / ALLOC element */
    uint32_t region_id;       /* for ALLOC: owning region, or none      */
    int sum_variant;          /* for SUM_MAKE: 0 none, 1 some, 2 ok, 3 err */
    int sum_selector;         /* for SUM_ACCESS: 0 tag, 1 payload, 2 error */
    int sum_expected_tag;     /* for SUM_ACCESS tag predicate: expected tag */
    bool sum_checked;         /* for SUM_ACCESS: runtime tag check         */
    double float_value;       /* for FLOAT_LITERAL: exact literal value     */
    char literal[COBRA_MAX_TOKEN_TEXT]; /* for STR_LITERAL: literal bytes   */
    char dict_key[COBRA_MAX_TOKEN_TEXT]; /* for DICT_GET/HAS/POP key        */
    char **dict_keys;                     /* for DICT_LITERAL: entry keys     */
    char callee[BIR_MAX_CALLEE_NAME]; /* for CALL                      */
    HirExpr **args;
    size_t arg_count;
    /* for BORROW: true when this alias lives only for the duration of a
       single call argument, not a let-bound view local or a return - lets
       the SSA lowering mark the resulting view_make releasable right after
       the call that consumes it. */
    bool transient_borrow;
    int source_line;
    int source_col;
};

typedef enum {
    HIR_STMT_NONE = 0,
    HIR_STMT_ASSIGN,          /* local = expr                          */
    HIR_STMT_MEMBER_ASSIGN,   /* aggregate.field = scalar               */
    HIR_STMT_INDEX_ASSIGN,    /* borrowed view/owned slice[index] = val */
    HIR_STMT_EXPR,            /* evaluate expr, discard result (calls)  */
    HIR_STMT_FREE,            /* free an owned slice local              */
    HIR_STMT_REGION_ENTER,    /* enter a declared region (local = id)   */
    HIR_STMT_REGION_EXIT,     /* exit a declared region (local = id)    */
    HIR_STMT_DICT_SET,        /* insert key/value into a dict local     */
    HIR_STMT_DICT_DELETE,     /* remove a key from a dict local         */
    HIR_STMT_PRINT,           /* print(expr) - string or i64/bool        */
    HIR_STMT_ASSERT           /* assert expr - runtime failure if false  */
} HirStmtKind;

typedef struct {
    HirStmtKind kind;
    uint32_t local;           /* for ASSIGN                            */
    HirExpr *target;          /* for MEMBER_ASSIGN                     */
    HirExpr *expr;
    char dict_key[COBRA_MAX_TOKEN_TEXT]; /* for DICT_SET/DICT_DELETE key */
} HirStmt;

typedef enum {
    HIR_TERM_NONE = 0,
    HIR_TERM_JUMP,
    HIR_TERM_BRANCH,
    HIR_TERM_RETURN
} HirTermKind;

typedef struct {
    HirTermKind kind;
    HirBlockRef target;       /* JUMP target / BRANCH then             */
    HirBlockRef target2;      /* BRANCH else                           */
    HirExpr *cond;            /* BRANCH condition                      */
    HirExpr *ret_expr;        /* RETURN value (NULL = bare return)     */
} HirTerm;

typedef struct {
    HirBlockRef id;
    char name[32];
    HirStmt *stmts;
    size_t stmt_count;
    size_t stmt_cap;
    HirTerm term;
    HirBlockRef *preds;
    size_t pred_count;
    size_t pred_cap;
    HirBlockRef *succs;
    size_t succ_count;
    size_t succ_cap;
    bool is_entry;
    int source_line;
    int source_col;
} HirBlock;

typedef struct {
    char name[BIR_MAX_CALLEE_NAME];
    const CobraType *type;      /* canonical type owned by the HIR module */
    bool is_param;
    int source_line;
    int source_col;
} HirLocal;

typedef struct {
    char name[BIR_MAX_CALLEE_NAME];
    size_t param_count;
    const CobraType *param_types[BIR_MAX_PARAMS];
    const CobraType *return_type; /* supported scalar, aggregate, view, or void */
    HirLocal locals[BIR_MAX_LOCALS];
    size_t local_count;
    HirBlock *blocks;          /* block 0 is entry                     */
    size_t block_count;
    size_t block_cap;
} HirFunction;

/* ------------------------------------------------------------------ */
/* Module: arena + function table + evaluator entry points.           */
/* ------------------------------------------------------------------ */

typedef struct {
    char name[BIR_MAX_CALLEE_NAME];
    SsaBlockRef entry;
    SsaBlockRef first_block;   /* owned contiguous block range           */
    size_t block_count;
    size_t param_count;        /* source-visible parameters               */
    size_t ssa_param_count;    /* includes hidden aggregate return storage */
    const CobraType *param_types[BIR_MAX_PARAMS];
    const CobraType *param_value_types[BIR_MAX_PARAMS];
    CobraAbiKind param_abi[BIR_MAX_PARAMS];
    CobraAbiKind param_value_abi[BIR_MAX_PARAMS];
    BirPointerContract param_pointer_contract[BIR_MAX_PARAMS];
    BirPointerContract return_pointer_contract;
    BirCallAbi call_abi;
    /* A borrowed-view return must be derived from exactly this one view
       parameter. UINT32_MAX means the function does not return a view. */
    uint32_t return_view_param;
    CobraAbiKind return_abi;
    CobraAbiKind return_value_abi;
    /* Function-parameter SSA values, in declaration order. Kept here because
       the module value pool is shared across functions and a value-scan would
       bind the wrong function's parameters. */
    SsaValueRef params[BIR_MAX_SSA_PARAMS];
    SsaValueRef hidden_return_param;
    const CobraType *return_type;
    const CobraType *return_value_type; /* pointer[return_type] for structs */
    bool has_hidden_return_storage;
    bool has_return;           /* false = void                         */
    /* True for a name declared via `import c "lib.so" (...)`: no SSA body
       (entry/first_block stay SSA_BLOCK_NONE) and calls to it bypass the
       normal call_abi/param_types machinery in favor of the raw SysV
       integer-register convention (see x86_emit_extern_call). */
    bool is_extern;
    /* True unless bir_build_program proved this function unreachable from
       main (or there is no main, in which case every function is treated as
       reachable - see mark_reachable_functions). A handful of verify.c's
       whole-module semantic checks (e.g. the owned-slice-parameter liveness
       check) are relaxed for unreachable functions: nothing the program
       actually runs needs them, but they still get fully built and verified
       for structural soundness, and a std.cb helper the program never calls
       must not be able to hard-fail every build that happens to pull it in. */
    bool reachable_from_main;
} BirFunctionInfo;

typedef struct {
    char name[COBRA_MAX_IDENT_LEN];
    const CobraType *type;
} BirAggregateInfo;

/* An enum registered from an AST_ENUM_DECL. Discriminants are the i64
   values of the enum's integer-backed canonical scalar type. Payload-carrying
   enums store one component type per variant (NULL marks a unit variant) in
   the module arena; the canonical descriptor carries the same components as
   generic arguments so the tagged-sum machinery can read them off the type. */
typedef struct {
    char name[COBRA_MAX_IDENT_LEN];
    const CobraType *type;   /* canonical COBRA_TYPE_ENUM in the module arena */
    char variant_names[COBRA_MAX_ENUM_VARIANTS][COBRA_MAX_IDENT_LEN];
    int variant_values[COBRA_MAX_ENUM_VARIANTS];
    const CobraType *variant_payloads[COBRA_MAX_ENUM_VARIANTS];
    /* Declared payload arity per variant (AST children). A single struct
       payload (`A(P)`) declares one argument even though its synthetic or
       imported payload type has several fields. */
    size_t variant_payload_arity[COBRA_MAX_ENUM_VARIANTS];
    size_t variant_count;
    bool has_payloads;
} BirEnumInfo;

typedef struct {
    SsaArena arena;
    /* Heap-allocated: CobraTypeArena embeds a 2048-entry node array (~6.7 MB),
       far too large to embed by value in a stack-allocated module. */
    /* The backend owns this arena and imports only the supported canonical
       scalar descriptors from the frontend AST; HIR and SSA never retain
       pointers into the parser's arena. */
    CobraTypeArena *type_arena;
    const CobraType *type_i64;
    const CobraType *type_i32;
    const CobraType *type_u32;
    const CobraType *type_u64;
    const CobraType *type_void;
    const CobraType *type_f32;
    const CobraType *type_f64;
    const CobraType *type_u8;
    const CobraType *type_bool;
    BirAggregateInfo aggregates[BIR_MAX_AGGREGATES];
    size_t aggregate_count;
    BirEnumInfo enums[BIR_MAX_ENUMS];
    size_t enum_count;
    BirRegionInfo regions[BIR_MAX_REGIONS];
    size_t region_count;
    ASTNode *source_root;      /* frontend root during type import only */
    BirFunctionInfo functions[BIR_MAX_FUNCTIONS];
    size_t function_count;
    char source_file[COBRA_MAX_SOURCE_PATH];
    char error[COBRA_MAX_TOKEN_TEXT + COBRA_MAX_SOURCE_PATH];
} BackendIrModule;

/* Low-level SSA construction. */
void bir_arena_init(SsaArena *arena);
void bir_arena_free(SsaArena *arena);
SsaBlockRef bir_add_block(SsaArena *arena, const char *name, int line, int col);
SsaBlockRef bir_add_entry_block(SsaArena *arena, const char *name, int line, int col);
SsaValueRef bir_add_value(SsaArena *arena, SsaValueKind kind, const CobraType *type,
                          int line, int col);
SsaValueRef bir_add_const(SsaArena *arena, BirScalarValue value,
                          int line, int col);
const CobraType *bir_pointer_type(BackendIrModule *module, const CobraType *pointee);
SsaInstRef bir_add_inst(SsaArena *arena, SsaOpcode op, const CobraType *type,
                        const SsaValueRef *operands, size_t operand_count,
                        int line, int col);
SsaValueRef bir_inst_result(SsaArena *arena, SsaInstRef inst, int line, int col);
SsaInstRef bir_add_stack_slot(SsaArena *arena, const CobraType *pointer_type,
                              const CobraType *memory_type, int64_t offset,
                              uint32_t alignment, uint32_t slot, int line, int col);
SsaInstRef bir_add_region_stack_slot(SsaArena *arena, const CobraType *pointer_type,
                                     const CobraType *memory_type, int64_t offset,
                                     uint32_t alignment, uint32_t slot,
                                     uint32_t region_id, int line, int col);
SsaInstRef bir_add_ptr_add(SsaArena *arena, const CobraType *pointer_type,
                           SsaValueRef pointer, SsaValueRef offset,
                           int line, int col);
SsaInstRef bir_add_field_addr(SsaArena *arena, const CobraType *pointer_type,
                              const CobraType *aggregate_type, const CobraType *field_type,
                              SsaValueRef base, size_t field_offset,
                              int line, int col);
SsaInstRef bir_add_array_index_addr(SsaArena *arena, const CobraType *pointer_type,
                                    const CobraType *array_type, const CobraType *element_type,
                                    SsaValueRef base, SsaValueRef index,
                                    int line, int col);
SsaInstRef bir_add_aggregate_copy(SsaArena *arena, const CobraType *aggregate_type,
                                  SsaValueRef destination, SsaValueRef source,
                                  int line, int col);
SsaInstRef bir_add_region_enter(SsaArena *arena, uint32_t region_id,
                                uint32_t parent_region_id, int line, int col);
SsaInstRef bir_add_region_exit(SsaArena *arena, uint32_t region_id,
                               int line, int col);
SsaInstRef bir_add_transfer(SsaArena *arena, const CobraType *pointer_type,
                            SsaValueRef pointer, uint32_t destination_region_id,
                            int line, int col);
SsaInstRef bir_add_destroy(SsaArena *arena, SsaValueRef pointer,
                           int line, int col);
SsaInstRef bir_add_view_make(SsaArena *arena, const CobraType *view_type,
                             const CobraType *element_type, SsaValueRef pointer,
                             SsaValueRef length, int line, int col);
SsaInstRef bir_add_view_ptr(SsaArena *arena, const CobraType *pointer_type,
                            const CobraType *element_type, SsaValueRef view,
                            int line, int col);
SsaInstRef bir_add_view_len(SsaArena *arena, const CobraType *i64_type,
                            const CobraType *view, SsaValueRef value,
                            int line, int col);
SsaInstRef bir_add_slice_alloc(SsaArena *arena, const CobraType *owned_type,
                               const CobraType *element_type, SsaValueRef length,
                               uint32_t allocation_id, int line, int col);
SsaInstRef bir_add_region_slice_alloc(SsaArena *arena, const CobraType *owned_type,
                                      const CobraType *element_type, SsaValueRef length,
                                      uint32_t allocation_id, uint32_t region_id,
                                      int line, int col);
SsaInstRef bir_add_slice_free(SsaArena *arena, SsaValueRef slice,
                              int line, int col);
SsaInstRef bir_add_buffer_alloc(SsaArena *arena, const CobraType *buffer_type,
                                const CobraType *element_type, SsaValueRef length,
                                uint32_t allocation_id, int line, int col);
SsaInstRef bir_add_buffer_append(SsaArena *arena, const CobraType *buffer_type,
                                 const CobraType *element_type, SsaValueRef buffer,
                                 SsaValueRef value, uint32_t allocation_id,
                                 int line, int col);
SsaInstRef bir_add_buffer_pop(SsaArena *arena, const CobraType *element_type,
                              SsaValueRef buffer, int line, int col);
SsaInstRef bir_add_buffer_free(SsaArena *arena, SsaValueRef buffer,
                               int line, int col);
SsaInstRef bir_add_dict_alloc(SsaArena *arena, const CobraType *dict_type,
                              const CobraType *value_type,
                              SsaValueRef capacity, uint32_t allocation_id,
                              int line, int col);
SsaInstRef bir_add_dict_set(SsaArena *arena, const CobraType *dict_type,
                            const CobraType *value_type,
                            SsaValueRef dict, SsaValueRef value,
                            const char *key, uint32_t allocation_id,
                            int line, int col);
SsaInstRef bir_add_dict_get(SsaArena *arena, const CobraType *value_type,
                            SsaValueRef dict, SsaValueRef fallback,
                            const char *key, int line, int col);
SsaInstRef bir_add_dict_has(SsaArena *arena, const CobraType *bool_type,
                            SsaValueRef dict, const char *key,
                            int line, int col);
SsaInstRef bir_add_dict_delete(SsaArena *arena, const CobraType *dict_type,
                               SsaValueRef dict, const char *key,
                               uint32_t allocation_id, int line, int col);
SsaInstRef bir_add_dict_pop(SsaArena *arena, const CobraType *dict_type,
                            const CobraType *value_type,
                            SsaValueRef dict, SsaValueRef fallback,
                            const char *key, int line, int col);
SsaInstRef bir_add_dict_len(SsaArena *arena, const CobraType *i64_type,
                            SsaValueRef dict, int line, int col);
SsaInstRef bir_add_dict_free(SsaArena *arena, SsaValueRef dict,
                             int line, int col);
SsaInstRef bir_add_string_concat(SsaArena *arena, const CobraType *owned_type,
                                 SsaValueRef left, SsaValueRef right,
                                 int line, int col);
SsaInstRef bir_add_string_eq(SsaArena *arena, const CobraType *bool_type,
                             const CobraType *element_type,
                             SsaValueRef left, SsaValueRef right,
                             int line, int col);
SsaInstRef bir_add_sum_payload_store(SsaArena *arena, const CobraType *sum_type,
                                     const CobraType *payload_type,
                                     SsaValueRef destination, SsaValueRef payload,
                                     size_t payload_offset, int line, int col);
SsaInstRef bir_add_sum_payload_load(SsaArena *arena, const CobraType *payload_type,
                                    const CobraType *sum_type,
                                    SsaValueRef source, size_t payload_offset,
                                    int line, int col);
SsaInstRef bir_add_sum_move(SsaArena *arena, const CobraType *sum_type,
                            SsaValueRef destination, SsaValueRef source,
                            int line, int col);
SsaInstRef bir_add_sum_drop(SsaArena *arena, const CobraType *sum_type,
                            SsaValueRef source, int line, int col);
SsaInstRef bir_add_field_payload_store(SsaArena *arena, const CobraType *aggregate_type,
                                       const CobraType *payload_type,
                                       SsaValueRef destination, SsaValueRef payload,
                                       size_t field_offset, int line, int col);
SsaInstRef bir_add_field_payload_load(SsaArena *arena, const CobraType *aggregate_type,
                                      const CobraType *payload_type,
                                      SsaValueRef source, size_t field_offset,
                                      int line, int col);
SsaInstRef bir_add_aggregate_move(SsaArena *arena, const CobraType *aggregate_type,
                                  SsaValueRef destination, SsaValueRef source,
                                  int line, int col);
SsaInstRef bir_add_aggregate_drop(SsaArena *arena, const CobraType *aggregate_type,
                                  SsaValueRef source, int line, int col);
SsaInstRef bir_add_sum_check(SsaArena *arena, SsaValueRef tag,
                             int check_kind, int line, int col);
SsaInstRef bir_add_print_i64(SsaArena *arena, SsaValueRef value, int line, int col);
SsaInstRef bir_add_print_string(SsaArena *arena, SsaValueRef view, int line, int col);
SsaInstRef bir_add_assert(SsaArena *arena, SsaValueRef cond, int line, int col);

/* An owned slice argument satisfies a borrowed readonly or writable view
   parameter of the same element type; the call boundary retags the pointer
   to the callee's borrow contract. */
bool bir_call_arg_type_compatible(const CobraType *actual,
                                  const CobraType *expected);
const CobraType *bir_owned_slice_type(BackendIrModule *module,
                                      const CobraType *element_type);
const CobraType *bir_array_type(BackendIrModule *module,
                                const CobraType *element_type, size_t length);
const CobraType *bir_sum_type(BackendIrModule *module, CobraTypeKind kind,
                              const CobraType *element, const CobraType *error);
const BirEnumInfo *bir_find_enum(const BackendIrModule *module,
                                 const char *name);
bool bir_is_payload_enum_type(const CobraType *type);
int bir_enum_variant_value(const BackendIrModule *module,
                           const char *enum_name, const char *variant_name);
const CobraType *bir_enum_type(BackendIrModule *module, const char *name);
bool bir_is_sum_type(const CobraType *type);
bool bir_sum_has_owned_payload(const CobraType *type);
bool bir_type_has_owned_payload(const CobraType *type);
bool bir_type_is_value_only_struct(const CobraType *type);
size_t bir_sum_component_slot_size(const CobraType *component);
size_t bir_sum_component_offset(const CobraType *sum, int selector);
bool bir_is_owned_slice_type(const CobraType *type);
bool bir_is_owned_buffer_type(const CobraType *type);
bool bir_is_owned_dict_type(const CobraType *type);
const CobraType *bir_buffer_type(BackendIrModule *module,
                                 const CobraType *element_type);
const CobraType *bir_dict_type(BackendIrModule *module,
                               const CobraType *value_type);
const CobraType *bir_view_type(BackendIrModule *module, const CobraType *element_type);
const CobraType *bir_writable_view_type(BackendIrModule *module, const CobraType *element_type);
bool bir_is_borrowed_view_type(const CobraType *type);
bool bir_view_is_writable(const CobraType *type);
SsaInstRef bir_add_typed_load(SsaArena *arena, const CobraType *value_type,
                              const CobraType *pointer_type, SsaValueRef pointer,
                              uint32_t width, uint32_t alignment, int line, int col);
SsaInstRef bir_add_typed_store(SsaArena *arena, const CobraType *value_type,
                               const CobraType *pointer_type, SsaValueRef pointer,
                               SsaValueRef value, uint32_t width,
                               uint32_t alignment, int line, int col);
bool bir_set_terminator(SsaArena *arena, SsaBlockRef block, SsaInstRef term);
bool bir_block_add_inst(SsaArena *arena, SsaBlockRef block, SsaInstRef inst);
SsaValueRef bir_add_block_param(SsaArena *arena, SsaBlockRef block,
                                const CobraType *type, int line, int col);
bool bir_add_edge(SsaArena *arena, SsaBlockRef pred, SsaBlockRef succ);
bool bir_set_jump(SsaArena *arena, SsaBlockRef block, SsaBlockRef target,
                  const SsaValueRef *edge_args, size_t edge_count, int line, int col);
bool bir_set_branch(SsaArena *arena, SsaBlockRef block, SsaValueRef cond,
                    SsaBlockRef then_block, SsaBlockRef else_block,
                    const SsaValueRef *then_args, size_t then_count,
                    const SsaValueRef *else_args, size_t else_count,
                    int line, int col);
bool bir_set_return(SsaArena *arena, SsaBlockRef block, SsaValueRef value,
                    int line, int col);

/* Low-level function registration (used by the HIR pipeline and by direct
   SSA unit tests). params may be NULL when the function has no parameters. */
bool bir_declare_function(BackendIrModule *module, const char *name,
                           size_t param_count,
                           const CobraType *const *param_types,
                           const CobraType *return_type, bool has_return);
bool bir_validate_function_abi(const BackendIrModule *module,
                               const BirFunctionInfo *info);
/* Registers a name declared via `import c "lib.so" (...)`: no signature, no
   body, callable with up to BIR_ABI_MAX_GPR_ARGUMENT_REGISTERS raw i64
   arguments (matching the direct backend's emit_import_call bridge). */
bool bir_declare_extern_function(BackendIrModule *module, const char *name);
bool bir_register_function_info(BackendIrModule *module, const char *name,
                                SsaBlockRef entry, size_t param_count,
                                const SsaValueRef *params,
                                const CobraType *return_type, bool has_return);
bool bir_declare_region(BackendIrModule *module, uint32_t region_id,
                        uint32_t parent_region_id);

/* HIR + SSA pipeline. */
void bir_module_init(BackendIrModule *module, const char *source_file);
void bir_module_free(BackendIrModule *module);
bool bir_build_function(BackendIrModule *module, ASTNode *function,
                        BirFunctionInfo **out_info);
bool bir_build_program(BackendIrModule *module, ASTNode *root);
const BirFunctionInfo *bir_find_function(const BackendIrModule *module,
                                         const char *name);

/* Verifier. */
bool bir_verify(const BackendIrModule *module, char *errbuf, size_t errbuf_size);

/* Deterministic textual dump. */
void bir_dump(const BackendIrModule *module, FILE *out);

/* Evaluator. */
bool bir_eval_function_value(const BackendIrModule *module, const char *name,
                             BirScalarValue *result);

/* Helpers shared by the pipeline. */
const char *bir_opcode_name(SsaOpcode op);
bool bir_is_terminator(SsaOpcode op);
bool bir_op_has_result(SsaOpcode op);
const char *bir_value_kind_name(SsaValueKind kind);
const char *bir_pointer_contract_name(BirPointerContract contract);
const char *bir_pointer_origin_name(BirPointerOrigin origin);
bool bir_pointer_contract_readable(BirPointerContract contract);
bool bir_pointer_contract_writable(BirPointerContract contract);
bool bir_pointer_contract_compatible(BirPointerContract actual,
                                      BirPointerContract expected);
bool bir_type_has_generic(const CobraType *type);

#endif /* COBRA_BACKEND_IR_H */
