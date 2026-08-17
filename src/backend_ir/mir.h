/*
 * Cobra target-independent machine IR.
 *
 * MIR is built only from verified backend SSA. It keeps control flow explicit,
 * replaces SSA value handles with virtual registers, preserves canonical type
 * identity for diagnostics, and records ABI locations without naming physical
 * registers. The isolated x86-64 emitters consume it without changing the
 * production compiler.
 */
#ifndef COBRA_BACKEND_MIR_H
#define COBRA_BACKEND_MIR_H

#include "ssa.h"

#define MIR_MAX_FUNCTIONS BIR_MAX_FUNCTIONS
#define MIR_MAX_OPERANDS BIR_MAX_SSA_PARAMS
#define MIR_MAX_REGS_PER_FUNCTION 65535

#define MIR_REG_NONE ((MirReg)0)
#define MIR_INST_NONE ((MirInstRef)0)
#define MIR_BLOCK_NONE ((MirBlockRef)0)

typedef uint32_t MirReg;
typedef uint32_t MirInstRef;
typedef uint32_t MirBlockRef;

typedef enum {
    MIR_TYPE_VOID = 0,
    MIR_TYPE_I8,
    MIR_TYPE_I32,
    MIR_TYPE_U32,
    MIR_TYPE_I64,
    MIR_TYPE_U64,
    MIR_TYPE_BOOL,
    MIR_TYPE_F32,
    MIR_TYPE_F64,
    MIR_TYPE_ADDRESS,
    MIR_TYPE_VIEW,
    MIR_TYPE_AGGREGATE
} MirMachineType;

typedef enum {
    MIR_OP_NONE = 0,
    MIR_OP_CONST,
    MIR_OP_ABI_MOVE,
    MIR_OP_BLOCK_ARG,
    MIR_OP_ADD,
    MIR_OP_SUB,
    MIR_OP_MUL,
    MIR_OP_DIV,
    MIR_OP_REM,
    MIR_OP_NEG,
    MIR_OP_EQ,
    MIR_OP_NE,
    MIR_OP_LT,
    MIR_OP_LE,
    MIR_OP_GT,
    MIR_OP_GE,
    MIR_OP_STACK_SLOT,
    MIR_OP_PTR_ADD,
    MIR_OP_FIELD_ADDR,
    MIR_OP_ARRAY_INDEX_ADDR,
    MIR_OP_LOAD,
    MIR_OP_STORE,
    MIR_OP_AGG_COPY,
    MIR_OP_SUM_PAYLOAD_STORE,
    MIR_OP_SUM_PAYLOAD_LOAD,
    MIR_OP_SUM_MOVE,
    MIR_OP_SUM_DROP,
    MIR_OP_FIELD_PAYLOAD_STORE,
    MIR_OP_FIELD_PAYLOAD_LOAD,
    MIR_OP_AGG_MOVE,
    MIR_OP_AGG_DROP,
    MIR_OP_REGION_ENTER,
    MIR_OP_REGION_EXIT,
    MIR_OP_TRANSFER,
    MIR_OP_DESTROY,
    MIR_OP_VIEW_MAKE,
    MIR_OP_VIEW_PTR,
    MIR_OP_VIEW_LEN,
    MIR_OP_SLICE_ALLOC,
    MIR_OP_SLICE_FREE,
    MIR_OP_BUFFER_ALLOC,
    MIR_OP_BUFFER_APPEND,
    MIR_OP_BUFFER_POP,
    MIR_OP_BUFFER_FREE,
    MIR_OP_DICT_ALLOC,
    MIR_OP_DICT_SET,
    MIR_OP_DICT_GET,
    MIR_OP_DICT_HAS,
    MIR_OP_DICT_DELETE,
    MIR_OP_DICT_POP,
    MIR_OP_DICT_LEN,
    MIR_OP_DICT_FREE,
    MIR_OP_STRING_CONCAT,
    MIR_OP_STRING_EQ,
    MIR_OP_SUM_CHECK,
    MIR_OP_PRINT_I64,
    MIR_OP_PRINT_STRING,
    MIR_OP_ASSERT,
    MIR_OP_CALL,
    MIR_OP_JUMP,
    MIR_OP_BRANCH,
    MIR_OP_RETURN
} MirOpcode;

typedef enum {
    MIR_EFFECT_NONE = 0,
    MIR_EFFECT_READ,
    MIR_EFFECT_WRITE,
    MIR_EFFECT_READWRITE,
    MIR_EFFECT_CALL
} MirEffect;

typedef struct {
    MirMachineType machine_type;
    const CobraType *type;
    uint32_t function_index;
    SsaValueRef source_value;
    MirBlockRef def_block;
    MirInstRef def_inst;
    bool entry_defined;
    bool block_parameter;
    BirPointerContract pointer_contract;
    BirPointerOrigin pointer_origin;
    uint32_t region_id;
    uint32_t allocation_id;
    int source_line;
    int source_col;
} MirRegInfo;

typedef struct {
    uint64_t gpr_mask;
    uint64_t xmm_mask;
} MirClobberSet;

typedef struct {
    MirOpcode op;
    MirMachineType machine_type;
    const CobraType *type;
    MirReg result;
    uint32_t operand_start;
    uint32_t operand_count;
    MirBlockRef target;
    MirBlockRef target2;
    uint32_t edge_start;
    uint32_t edge_count;
    uint32_t edge2_start;
    uint32_t edge2_count;
    MirEffect effect;
    BirScalarValue immediate;
    bool has_immediate;
    BirAbiValueLocations abi_locations;
    MirClobberSet clobbers;
    uint32_t callee_index;
    char callee[BIR_MAX_CALLEE_NAME];
    char dict_key[COBRA_MAX_TOKEN_TEXT];
    SsaAddressKind address_kind;
    uint32_t memory_width;
    uint32_t memory_alignment;
    uint32_t address_space;
    const CobraType *memory_type;
    const CobraType *aggregate_type;
    int64_t memory_offset;
    uint32_t stack_slot;
    BirPointerContract pointer_contract;
    BirPointerOrigin pointer_origin;
    uint32_t region_id;
    uint32_t allocation_id;
    uint32_t parent_region_id;
    int sum_check_kind;
    int64_t sum_check_expected;
    int64_t view_length;
    MirReg view_source;
    int source_line;
    int source_col;
} MirInst;

typedef struct {
    char name[32];
    MirInstRef *insts;
    size_t inst_count;
    size_t inst_cap;
    MirInstRef terminator;
    MirReg *params;
    size_t param_count;
    size_t param_cap;
    MirBlockRef *preds;
    size_t pred_count;
    size_t pred_cap;
    MirBlockRef *succs;
    size_t succ_count;
    size_t succ_cap;
    bool is_entry;
    int source_line;
    int source_col;
} MirBlock;

typedef struct {
    char name[BIR_MAX_CALLEE_NAME];
    MirBlockRef entry;
    MirBlockRef first_block;
    size_t block_count;
    size_t param_count;
    size_t ssa_param_count;
    const CobraType *return_type;
    bool has_return;
    bool has_hidden_return_storage;
    BirCallAbi call_abi;
} MirFunction;

typedef struct {
    MirRegInfo *regs;
    size_t reg_count;
    size_t reg_cap;
    MirInst *insts;
    size_t inst_count;
    size_t inst_cap;
    MirBlock *blocks;
    size_t block_count;
    size_t block_cap;
    MirReg *operands;
    size_t operand_used;
    size_t operand_cap;
    MirReg *edges;
    size_t edge_used;
    size_t edge_cap;
} MirArena;

typedef struct {
    const BackendIrModule *source;
    MirArena arena;
    MirFunction functions[MIR_MAX_FUNCTIONS];
    size_t function_count;
    char source_file[COBRA_MAX_SOURCE_PATH];
    char error[COBRA_MAX_TOKEN_TEXT];
} MirModule;

void mir_module_init(MirModule *module, const BackendIrModule *source);
void mir_module_free(MirModule *module);
bool mir_lower_module(const BackendIrModule *source, MirModule *module,
                     char *errbuf, size_t errbuf_size);
bool mir_verify(const MirModule *module, char *errbuf, size_t errbuf_size);
void mir_dump(const MirModule *module, FILE *out);
const char *mir_opcode_name(MirOpcode op);
const char *mir_machine_type_name(MirMachineType type);

#endif /* COBRA_BACKEND_MIR_H */
