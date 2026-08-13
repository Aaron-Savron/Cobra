/*
 * Cobra Backend IR: typed HIR/CFG + flat block-argument SSA.
 *
 * Isolated foundation for a future backend. Not linked into the production
 * compiler yet. Consumes the existing AST for a small scalar subset and
 * produces a flat arena-backed SSA form with stable integer handles,
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
#define BIR_MAX_LOCALS 128
#define BIR_MAX_STEPS 10000000
#define BIR_MAX_CALL_DEPTH 256
#define BIR_MEMORY_SLOTS 8192

/* ------------------------------------------------------------------ */
/* Handles: stable integer identity into flat arena pools.            */
/* ------------------------------------------------------------------ */

typedef uint32_t SsaValueRef;
typedef uint32_t SsaInstRef;
typedef uint32_t SsaBlockRef;

#define SSA_VALUE_NONE ((SsaValueRef)0)
#define SSA_INST_NONE  ((SsaInstRef)0)
#define SSA_BLOCK_NONE ((SsaBlockRef)0)

typedef enum {
    SSA_VALUE_INVALID = 0,
    SSA_VALUE_PARAM,       /* function parameter                      */
    SSA_VALUE_BLOCK_PARAM, /* block argument (join value)             */
    SSA_VALUE_CONST,       /* integer constant                        */
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
    SSA_OP_EQ,
    SSA_OP_NE,
    SSA_OP_LT,
    SSA_OP_LE,
    SSA_OP_GT,
    SSA_OP_GE,
    SSA_OP_LOAD,
    SSA_OP_STORE,
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
    SSA_EFFECT_CALL
} SsaEffect;

typedef struct {
    SsaValueKind kind;
    const CobraType *type;      /* canonical, finalized                */
    SsaInstRef def_inst;        /* for SSA_VALUE_INST                  */
    uint32_t param_index;       /* for SSA_VALUE_PARAM                 */
    SsaBlockRef block;          /* for SSA_VALUE_BLOCK_PARAM           */
    int64_t const_i64;          /* for SSA_VALUE_CONST                 */
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
    char callee[BIR_MAX_CALLEE_NAME]; /* for SSA_OP_CALL               */
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
    HIR_EXPR_CALL
} HirExprKind;

typedef struct HirExpr HirExpr;
struct HirExpr {
    HirExprKind kind;
    int64_t const_i64;
    uint32_t local;           /* for LOCAL                              */
    SsaOpcode binop;          /* for BINOP                              */
    char callee[BIR_MAX_CALLEE_NAME]; /* for CALL                      */
    HirExpr **args;
    size_t arg_count;
    int source_line;
    int source_col;
};

typedef enum {
    HIR_STMT_NONE = 0,
    HIR_STMT_ASSIGN,          /* local = expr                          */
    HIR_STMT_EXPR             /* evaluate expr, discard result (calls)  */
} HirStmtKind;

typedef struct {
    HirStmtKind kind;
    uint32_t local;           /* for ASSIGN                            */
    HirExpr *expr;
} HirStmt;

typedef enum {
    HIR_TERM_NONE = 0,
    HIR_TERM_JUMP,
    HIR_TERM_BRANCH,
    HIR_TERM_RETURN
} HirTermKind;

typedef struct {
    HirTermKind kind;
    SsaBlockRef target;       /* JUMP target / BRANCH then             */
    SsaBlockRef target2;      /* BRANCH else                           */
    HirExpr *cond;            /* BRANCH condition                      */
    HirExpr *ret_expr;        /* RETURN value (NULL = bare return)     */
} HirTerm;

typedef struct {
    SsaBlockRef id;
    char name[32];
    HirStmt *stmts;
    size_t stmt_count;
    size_t stmt_cap;
    HirTerm term;
    SsaBlockRef *preds;
    size_t pred_count;
    size_t pred_cap;
    SsaBlockRef *succs;
    size_t succ_count;
    size_t succ_cap;
    bool is_entry;
    int source_line;
    int source_col;
} HirBlock;

typedef struct {
    char name[BIR_MAX_CALLEE_NAME];
    bool is_param;
    int source_line;
    int source_col;
} HirLocal;

typedef struct {
    char name[BIR_MAX_CALLEE_NAME];
    size_t param_count;
    const CobraType *return_type; /* i64 or void                        */
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
    size_t param_count;
    /* Function-parameter SSA values, in declaration order. Kept here because
       the module value pool is shared across functions and a value-scan would
       bind the wrong function's parameters. */
    SsaValueRef params[BIR_MAX_PARAMS];
    const CobraType *return_type;
    bool has_return;           /* false = void                         */
} BirFunctionInfo;

typedef struct {
    SsaArena arena;
    /* Heap-allocated: CobraTypeArena embeds a 2048-entry node array (~6.7 MB),
       far too large to embed by value in a stack-allocated module. */
    CobraTypeArena *type_arena;
    const CobraType *type_i64;
    const CobraType *type_void;
    const CobraType *type_bool;
    BirFunctionInfo functions[BIR_MAX_FUNCTIONS];
    size_t function_count;
    char source_file[COBRA_MAX_SOURCE_PATH];
    char error[COBRA_MAX_TOKEN_TEXT];
} BackendIrModule;

/* Low-level SSA construction. */
void bir_arena_init(SsaArena *arena);
void bir_arena_free(SsaArena *arena);
SsaBlockRef bir_add_block(SsaArena *arena, const char *name, int line, int col);
SsaBlockRef bir_add_entry_block(SsaArena *arena, const char *name, int line, int col);
SsaValueRef bir_add_value(SsaArena *arena, SsaValueKind kind, const CobraType *type,
                          int line, int col);
SsaValueRef bir_add_const(SsaArena *arena, const CobraType *type, int64_t value,
                          int line, int col);
SsaInstRef bir_add_inst(SsaArena *arena, SsaOpcode op, const CobraType *type,
                        const SsaValueRef *operands, size_t operand_count,
                        int line, int col);
SsaValueRef bir_inst_result(SsaArena *arena, SsaInstRef inst, int line, int col);
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
bool bir_register_function_info(BackendIrModule *module, const char *name,
                                SsaBlockRef entry, size_t param_count,
                                const SsaValueRef *params,
                                const CobraType *return_type, bool has_return);

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
bool bir_eval_function(const BackendIrModule *module, const char *name,
                       int64_t *result);

/* Helpers shared by the pipeline. */
const char *bir_opcode_name(SsaOpcode op);
bool bir_is_terminator(SsaOpcode op);
bool bir_op_has_result(SsaOpcode op);
const char *bir_value_kind_name(SsaValueKind kind);
bool bir_type_has_generic(const CobraType *type);

#endif /* COBRA_BACKEND_IR_H */
