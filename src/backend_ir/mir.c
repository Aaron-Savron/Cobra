/*
 * Cobra target-independent machine IR.
 *
 * This file deliberately stops before instruction selection and register
 * allocation. MIR retains explicit virtual registers, CFG edges, ABI moves,
 * memory metadata, and source locations so later target passes do not infer
 * semantics from the AST or from legacy codegen state.
 */
#include "mir.h"
#include <stdarg.h>

static void mir_set_error(char *buffer, size_t capacity, const char *fmt, ...) {
    if (!buffer || capacity == 0 || buffer[0]) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, capacity, fmt, args);
    va_end(args);
}

static void *mir_grow(void *pointer, size_t element_size, size_t *capacity,
                     size_t needed) {
    if (needed <= *capacity) return pointer;
    size_t next = *capacity ? *capacity * 2 : 16;
    while (next < needed) {
        if (next > SIZE_MAX / 2) return NULL;
        next *= 2;
    }
    if (element_size != 0 && next > SIZE_MAX / element_size) return NULL;
    void *grown = realloc(pointer, next * element_size);
    if (!grown) return NULL;
    *capacity = next;
    return grown;
}

static void mir_arena_init(MirArena *arena) {
    memset(arena, 0, sizeof(*arena));
    MirRegInfo *regs = mir_grow(NULL, sizeof(*regs), &arena->reg_cap, 1);
    MirInst *insts = mir_grow(NULL, sizeof(*insts), &arena->inst_cap, 1);
    MirBlock *blocks = mir_grow(NULL, sizeof(*blocks), &arena->block_cap, 1);
    if (regs) {
        arena->regs = regs;
        memset(&arena->regs[0], 0, sizeof(*regs));
        arena->reg_count = 1;
    }
    if (insts) {
        arena->insts = insts;
        memset(&arena->insts[0], 0, sizeof(*insts));
        arena->insts[0].op = MIR_OP_NONE;
        arena->inst_count = 1;
    }
    if (blocks) {
        arena->blocks = blocks;
        memset(&arena->blocks[0], 0, sizeof(*blocks));
        arena->blocks[0].terminator = MIR_INST_NONE;
        arena->block_count = 1;
    }
}

static void mir_arena_free(MirArena *arena) {
    if (!arena) return;
    for (size_t i = 0; i < arena->block_count; i++) {
        free(arena->blocks[i].insts);
        free(arena->blocks[i].params);
        free(arena->blocks[i].preds);
        free(arena->blocks[i].succs);
    }
    free(arena->regs);
    free(arena->insts);
    free(arena->blocks);
    free(arena->operands);
    free(arena->edges);
    memset(arena, 0, sizeof(*arena));
}

void mir_module_init(MirModule *module, const BackendIrModule *source) {
    if (!module) return;
    memset(module, 0, sizeof(*module));
    module->source = source;
    if (source) snprintf(module->source_file, sizeof(module->source_file),
                         "%s", source->source_file);
    mir_arena_init(&module->arena);
}

void mir_module_free(MirModule *module) {
    if (!module) return;
    mir_arena_free(&module->arena);
    memset(module, 0, sizeof(*module));
}

static MirMachineType mir_type_for_cobra(const CobraType *type) {
    if (!type) return MIR_TYPE_VOID;
    switch (type->kind) {
        case COBRA_TYPE_VOID: return MIR_TYPE_VOID;
        case COBRA_TYPE_I32: return MIR_TYPE_I32;
        case COBRA_TYPE_U32: return MIR_TYPE_U32;
        case COBRA_TYPE_U64: return MIR_TYPE_U64;
        case COBRA_TYPE_BOOL: return MIR_TYPE_BOOL;
        case COBRA_TYPE_F32: return MIR_TYPE_F32;
        case COBRA_TYPE_F64: return MIR_TYPE_F64;
        case COBRA_TYPE_U8: return MIR_TYPE_I8;
        case COBRA_TYPE_POINTER: return MIR_TYPE_ADDRESS;
        case COBRA_TYPE_OPTION:
        case COBRA_TYPE_RESULT:
        case COBRA_TYPE_STRUCT: return MIR_TYPE_AGGREGATE;
        default:
            if (cobra_type_is_slice_kind(type->kind) ||
                bir_is_owned_buffer_type(type) || bir_is_owned_dict_type(type))
                return MIR_TYPE_VIEW;
            if (type->kind == COBRA_TYPE_ENUM) return MIR_TYPE_I64;
            return MIR_TYPE_I64;
    }
}

static MirEffect mir_effect_for_ssa(SsaEffect effect) {
    switch (effect) {
        case SSA_EFFECT_READ: return MIR_EFFECT_READ;
        case SSA_EFFECT_WRITE: return MIR_EFFECT_WRITE;
        case SSA_EFFECT_READWRITE: return MIR_EFFECT_READWRITE;
        case SSA_EFFECT_CALL: return MIR_EFFECT_CALL;
        case SSA_EFFECT_NONE: default: return MIR_EFFECT_NONE;
    }
}

static MirOpcode mir_opcode_for_ssa(SsaOpcode op) {
    switch (op) {
        case SSA_OP_ADD: return MIR_OP_ADD;
        case SSA_OP_SUB: return MIR_OP_SUB;
        case SSA_OP_MUL: return MIR_OP_MUL;
        case SSA_OP_DIV: return MIR_OP_DIV;
        case SSA_OP_REM: return MIR_OP_REM;
        case SSA_OP_NEG: return MIR_OP_NEG;
        case SSA_OP_CONVERT: return MIR_OP_CONVERT;
        case SSA_OP_EQ: return MIR_OP_EQ;
        case SSA_OP_NE: return MIR_OP_NE;
        case SSA_OP_LT: return MIR_OP_LT;
        case SSA_OP_LE: return MIR_OP_LE;
        case SSA_OP_GT: return MIR_OP_GT;
        case SSA_OP_GE: return MIR_OP_GE;
        case SSA_OP_STACK_SLOT: return MIR_OP_STACK_SLOT;
        case SSA_OP_PTR_ADD: return MIR_OP_PTR_ADD;
        case SSA_OP_FIELD_ADDR: return MIR_OP_FIELD_ADDR;
        case SSA_OP_ARRAY_INDEX_ADDR: return MIR_OP_ARRAY_INDEX_ADDR;
        case SSA_OP_LOAD: return MIR_OP_LOAD;
        case SSA_OP_STORE: return MIR_OP_STORE;
        case SSA_OP_AGG_COPY: return MIR_OP_AGG_COPY;
        case SSA_OP_SUM_PAYLOAD_STORE: return MIR_OP_SUM_PAYLOAD_STORE;
        case SSA_OP_SUM_PAYLOAD_LOAD: return MIR_OP_SUM_PAYLOAD_LOAD;
        case SSA_OP_SUM_MOVE: return MIR_OP_SUM_MOVE;
        case SSA_OP_SUM_DROP: return MIR_OP_SUM_DROP;
        case SSA_OP_FIELD_PAYLOAD_STORE: return MIR_OP_FIELD_PAYLOAD_STORE;
        case SSA_OP_FIELD_PAYLOAD_LOAD: return MIR_OP_FIELD_PAYLOAD_LOAD;
        case SSA_OP_AGG_MOVE: return MIR_OP_AGG_MOVE;
        case SSA_OP_AGG_DROP: return MIR_OP_AGG_DROP;
        case SSA_OP_REGION_ENTER: return MIR_OP_REGION_ENTER;
        case SSA_OP_REGION_EXIT: return MIR_OP_REGION_EXIT;
        case SSA_OP_TRANSFER: return MIR_OP_TRANSFER;
        case SSA_OP_DESTROY: return MIR_OP_DESTROY;
        case SSA_OP_VIEW_MAKE: return MIR_OP_VIEW_MAKE;
        case SSA_OP_VIEW_PTR: return MIR_OP_VIEW_PTR;
        case SSA_OP_VIEW_LEN: return MIR_OP_VIEW_LEN;
        case SSA_OP_SLICE_ALLOC: return MIR_OP_SLICE_ALLOC;
        case SSA_OP_SLICE_FREE: return MIR_OP_SLICE_FREE;
        case SSA_OP_BUFFER_ALLOC: return MIR_OP_BUFFER_ALLOC;
        case SSA_OP_BUFFER_APPEND: return MIR_OP_BUFFER_APPEND;
        case SSA_OP_BUFFER_POP: return MIR_OP_BUFFER_POP;
        case SSA_OP_BUFFER_FREE: return MIR_OP_BUFFER_FREE;
        case SSA_OP_DICT_ALLOC: return MIR_OP_DICT_ALLOC;
        case SSA_OP_DICT_SET: return MIR_OP_DICT_SET;
        case SSA_OP_DICT_GET: return MIR_OP_DICT_GET;
        case SSA_OP_DICT_HAS: return MIR_OP_DICT_HAS;
        case SSA_OP_DICT_DELETE: return MIR_OP_DICT_DELETE;
        case SSA_OP_DICT_POP: return MIR_OP_DICT_POP;
        case SSA_OP_DICT_LEN: return MIR_OP_DICT_LEN;
        case SSA_OP_DICT_FREE: return MIR_OP_DICT_FREE;
        case SSA_OP_STRING_CONCAT: return MIR_OP_STRING_CONCAT;
        case SSA_OP_STRING_EQ: return MIR_OP_STRING_EQ;
        case SSA_OP_SUM_CHECK: return MIR_OP_SUM_CHECK;
        case SSA_OP_PRINT_I64: return MIR_OP_PRINT_I64;
        case SSA_OP_PRINT_STRING: return MIR_OP_PRINT_STRING;
        case SSA_OP_ASSERT: return MIR_OP_ASSERT;
        case SSA_OP_CALL: return MIR_OP_CALL;
        case SSA_OP_JUMP: return MIR_OP_JUMP;
        case SSA_OP_BRANCH: return MIR_OP_BRANCH;
        case SSA_OP_RETURN: return MIR_OP_RETURN;
        default: return MIR_OP_NONE;
    }
}

const char *mir_opcode_name(MirOpcode op) {
    switch (op) {
        case MIR_OP_NONE: return "none";
        case MIR_OP_CONST: return "const";
        case MIR_OP_ABI_MOVE: return "abi_move";
        case MIR_OP_BLOCK_ARG: return "block_arg";
        case MIR_OP_ADD: return "add";
        case MIR_OP_SUB: return "sub";
        case MIR_OP_MUL: return "mul";
        case MIR_OP_DIV: return "div";
        case MIR_OP_REM: return "rem";
        case MIR_OP_NEG: return "neg";
        case MIR_OP_CONVERT: return "convert";
        case MIR_OP_EQ: return "eq";
        case MIR_OP_NE: return "ne";
        case MIR_OP_LT: return "lt";
        case MIR_OP_LE: return "le";
        case MIR_OP_GT: return "gt";
        case MIR_OP_GE: return "ge";
        case MIR_OP_STACK_SLOT: return "stack_slot";
        case MIR_OP_PTR_ADD: return "ptr_add";
        case MIR_OP_FIELD_ADDR: return "field_addr";
        case MIR_OP_ARRAY_INDEX_ADDR: return "array_index_addr";
        case MIR_OP_LOAD: return "load";
        case MIR_OP_STORE: return "store";
        case MIR_OP_AGG_COPY: return "agg_copy";
        case MIR_OP_SUM_PAYLOAD_STORE: return "sum_payload_store";
        case MIR_OP_SUM_PAYLOAD_LOAD: return "sum_payload_load";
        case MIR_OP_SUM_MOVE: return "sum_move";
        case MIR_OP_SUM_DROP: return "sum_drop";
        case MIR_OP_FIELD_PAYLOAD_STORE: return "field_payload_store";
        case MIR_OP_FIELD_PAYLOAD_LOAD: return "field_payload_load";
        case MIR_OP_AGG_MOVE: return "agg_move";
        case MIR_OP_AGG_DROP: return "agg_drop";
        case MIR_OP_REGION_ENTER: return "region_enter";
        case MIR_OP_REGION_EXIT: return "region_exit";
        case MIR_OP_TRANSFER: return "transfer";
        case MIR_OP_DESTROY: return "destroy";
        case MIR_OP_VIEW_MAKE: return "view_make";
        case MIR_OP_VIEW_PTR: return "view_ptr";
        case MIR_OP_VIEW_LEN: return "view_len";
        case MIR_OP_SLICE_ALLOC: return "slice_alloc";
        case MIR_OP_SLICE_FREE: return "slice_free";
        case MIR_OP_BUFFER_ALLOC: return "buffer_alloc";
        case MIR_OP_BUFFER_APPEND: return "buffer_append";
        case MIR_OP_BUFFER_POP: return "buffer_pop";
        case MIR_OP_BUFFER_FREE: return "buffer_free";
        case MIR_OP_DICT_ALLOC: return "dict_alloc";
        case MIR_OP_DICT_SET: return "dict_set";
        case MIR_OP_DICT_GET: return "dict_get";
        case MIR_OP_DICT_HAS: return "dict_has";
        case MIR_OP_DICT_DELETE: return "dict_delete";
        case MIR_OP_DICT_POP: return "dict_pop";
        case MIR_OP_DICT_LEN: return "dict_len";
        case MIR_OP_DICT_FREE: return "dict_free";
        case MIR_OP_STRING_CONCAT: return "string_concat";
        case MIR_OP_STRING_EQ: return "string_eq";
        case MIR_OP_SUM_CHECK: return "sum_check";
        case MIR_OP_PRINT_I64: return "print_i64";
        case MIR_OP_PRINT_STRING: return "print_string";
        case MIR_OP_ASSERT: return "assert";
        case MIR_OP_CALL: return "call";
        case MIR_OP_JUMP: return "jump";
        case MIR_OP_BRANCH: return "branch";
        case MIR_OP_RETURN: return "return";
    }
    return "?";
}

const char *mir_machine_type_name(MirMachineType type) {
    switch (type) {
        case MIR_TYPE_VOID: return "void";
        case MIR_TYPE_I8: return "i8";
        case MIR_TYPE_I32: return "i32";
        case MIR_TYPE_U32: return "u32";
        case MIR_TYPE_I64: return "i64";
        case MIR_TYPE_U64: return "u64";
        case MIR_TYPE_BOOL: return "bool";
        case MIR_TYPE_F32: return "f32";
        case MIR_TYPE_F64: return "f64";
        case MIR_TYPE_ADDRESS: return "addr";
        case MIR_TYPE_VIEW: return "view";
        case MIR_TYPE_AGGREGATE: return "aggregate";
    }
    return "?";
}

static MirReg mir_add_reg(MirArena *arena, const MirRegInfo *info) {
    MirRegInfo *grown = mir_grow(arena->regs, sizeof(*grown), &arena->reg_cap,
                                 arena->reg_count + 1);
    if (!grown) return MIR_REG_NONE;
    arena->regs = grown;
    arena->regs[arena->reg_count] = *info;
    return (MirReg)arena->reg_count++;
}

static MirReg mir_add_source_reg(MirArena *arena, const SsaValue *source,
                                 SsaValueRef source_ref, uint32_t function_index,
                                 MirBlockRef def_block, bool entry_defined,
                                 bool block_parameter) {
    MirRegInfo info;
    memset(&info, 0, sizeof(info));
    info.machine_type = mir_type_for_cobra(source ? source->type : NULL);
    info.type = source ? source->type : NULL;
    info.function_index = function_index;
    info.source_value = source_ref;
    info.def_block = def_block;
    info.entry_defined = entry_defined;
    info.block_parameter = block_parameter;
    info.pointer_contract = source ? source->pointer_contract
                                   : BIR_POINTER_CONTRACT_UNKNOWN;
    info.pointer_origin = source ? source->pointer_origin
                                 : BIR_POINTER_ORIGIN_UNKNOWN;
    info.region_id = source ? source->region_id : BIR_REGION_NONE;
    info.allocation_id = source ? source->allocation_id : 0;
    info.source_line = source ? source->source_line : 0;
    info.source_col = source ? source->source_col : 0;
    return mir_add_reg(arena, &info);
}

static MirBlockRef mir_add_block(MirArena *arena, const char *name,
                                 int line, int col, bool entry) {
    MirBlock *grown = mir_grow(arena->blocks, sizeof(*grown), &arena->block_cap,
                               arena->block_count + 1);
    if (!grown) return MIR_BLOCK_NONE;
    arena->blocks = grown;
    MirBlock *block = &arena->blocks[arena->block_count];
    memset(block, 0, sizeof(*block));
    if (name) snprintf(block->name, sizeof(block->name), "%s", name);
    block->terminator = MIR_INST_NONE;
    block->is_entry = entry;
    block->source_line = line;
    block->source_col = col;
    return (MirBlockRef)arena->block_count++;
}

static MirInstRef mir_add_inst(MirArena *arena, const MirInst *source,
                               const MirReg *operands, size_t operand_count) {
    if (operand_count > MIR_MAX_OPERANDS) return MIR_INST_NONE;
    MirInst *grown = mir_grow(arena->insts, sizeof(*grown), &arena->inst_cap,
                              arena->inst_count + 1);
    if (!grown) return MIR_INST_NONE;
    arena->insts = grown;
    if (operand_count) {
        MirReg *op_grown = mir_grow(arena->operands, sizeof(*op_grown),
                                    &arena->operand_cap,
                                    arena->operand_used + operand_count);
        if (!op_grown) return MIR_INST_NONE;
        arena->operands = op_grown;
    }
    MirInst *inst = &arena->insts[arena->inst_count];
    *inst = *source;
    inst->operand_start = (uint32_t)arena->operand_used;
    inst->operand_count = (uint32_t)operand_count;
    if (operand_count) {
        memcpy(&arena->operands[arena->operand_used], operands,
               operand_count * sizeof(*operands));
        arena->operand_used += operand_count;
    }
    return (MirInstRef)arena->inst_count++;
}

static bool mir_block_add_inst(MirArena *arena, MirBlockRef block,
                               MirInstRef inst) {
    if (!arena || block == MIR_BLOCK_NONE || block >= arena->block_count ||
        inst == MIR_INST_NONE || inst >= arena->inst_count) return false;
    MirBlock *target = &arena->blocks[block];
    MirInstRef *grown = mir_grow(target->insts, sizeof(*grown), &target->inst_cap,
                                 target->inst_count + 1);
    if (!grown) return false;
    target->insts = grown;
    target->insts[target->inst_count++] = inst;
    return true;
}

static bool mir_append_unique_block_ref(MirBlockRef **items, size_t *count,
                                        size_t *capacity, MirBlockRef value) {
    for (size_t i = 0; i < *count; i++) if ((*items)[i] == value) return true;
    MirBlockRef *grown = mir_grow(*items, sizeof(*grown), capacity, *count + 1);
    if (!grown) return false;
    *items = grown;
    (*items)[(*count)++] = value;
    return true;
}

static uint32_t mir_append_edges(MirArena *arena, const MirReg *edges,
                                 size_t count) {
    if (count > UINT32_MAX || arena->edge_used > UINT32_MAX - count) return UINT32_MAX;
    if (count) {
        MirReg *grown = mir_grow(arena->edges, sizeof(*grown), &arena->edge_cap,
                                 arena->edge_used + count);
        if (!grown) return UINT32_MAX;
        arena->edges = grown;
        memcpy(&arena->edges[arena->edge_used], edges, count * sizeof(*edges));
    }
    uint32_t start = (uint32_t)arena->edge_used;
    arena->edge_used += count;
    return start;
}

static int source_function_index(const BackendIrModule *source,
                                 const BirFunctionInfo *info) {
    if (!source || !info) return -1;
    for (size_t i = 0; i < source->function_count; i++) {
        if (&source->functions[i] == info) return (int)i;
    }
    return -1;
}

static int source_callee_index(const BackendIrModule *source, const char *name) {
    const BirFunctionInfo *info = bir_find_function(source, name);
    return source_function_index(source, info);
}

static MirClobberSet mir_default_call_clobbers(void) {
    MirClobberSet clobbers;
    clobbers.gpr_mask = (UINT64_C(1) << BIR_ABI_MAX_GPR_ARGUMENT_REGISTERS) - 1;
    clobbers.xmm_mask = (UINT64_C(1) << BIR_ABI_MAX_XMM_ARGUMENT_REGISTERS) - 1;
    return clobbers;
}

static void mir_copy_inst_metadata(MirInst *target, const SsaInst *source) {
    target->machine_type = mir_type_for_cobra(source->type);
    target->type = source->type;
    target->effect = mir_effect_for_ssa(source->effect);
    target->address_kind = source->address_kind;
    target->memory_width = source->memory_width;
    target->memory_alignment = source->memory_alignment;
    target->address_space = source->address_space;
    target->memory_type = source->memory_type;
    target->aggregate_type = source->aggregate_type;
    target->memory_offset = source->memory_offset;
    target->stack_slot = source->stack_slot;
    target->pointer_contract = source->pointer_contract;
    target->pointer_origin = source->pointer_origin;
    target->region_id = source->region_id;
    target->allocation_id = source->allocation_id;
    target->parent_region_id = source->parent_region_id;
    target->sum_check_kind = source->sum_check_kind;
    target->sum_check_expected = source->sum_check_expected;
    target->view_length = source->view_length;
    target->source_line = source->source_line;
    target->source_col = source->source_col;
}

static bool mir_map_value(const SsaValueRef *value_map, size_t value_count,
                          SsaValueRef source, MirReg *out) {
    if (!value_map || !out || source == SSA_VALUE_NONE || source >= value_count ||
        value_map[source] == MIR_REG_NONE) return false;
    *out = value_map[source];
    return true;
}

static bool mir_lower_function(const BackendIrModule *source, MirModule *module,
                               const BirFunctionInfo *info, size_t function_index) {
    const SsaArena *ssa = &source->arena;
    MirArena *arena = &module->arena;
    if (!info) return false;
    if (info->is_extern) {
        /* Extern functions have no SSA body to lower; keep a skeleton entry
           so this function's index in module->functions still lines up
           with its index in source->functions (callee_index indexes both
           arrays identically - see source_callee_index / x86_emit_call). */
        MirFunction *extern_function = &module->functions[module->function_count++];
        memset(extern_function, 0, sizeof(*extern_function));
        snprintf(extern_function->name, sizeof(extern_function->name), "%s", info->name);
        extern_function->entry = MIR_BLOCK_NONE;
        extern_function->first_block = MIR_BLOCK_NONE;
        extern_function->return_type = info->return_type;
        extern_function->has_return = info->has_return;
        return true;
    }
    if (info->first_block == SSA_BLOCK_NONE || info->block_count == 0 ||
        info->first_block >= ssa->block_count ||
        info->block_count > ssa->block_count - info->first_block) return false;

    MirFunction *function = &module->functions[module->function_count++];
    memset(function, 0, sizeof(*function));
    snprintf(function->name, sizeof(function->name), "%s", info->name);
    function->param_count = info->param_count;
    function->ssa_param_count = info->ssa_param_count;
    function->return_type = info->return_type;
    function->has_return = info->has_return;
    function->has_hidden_return_storage = info->has_hidden_return_storage;
    function->call_abi = info->call_abi;

    SsaValueRef *value_map = calloc(ssa->value_count ? ssa->value_count : 1,
                                    sizeof(*value_map));
    MirBlockRef *block_map = calloc(ssa->block_count ? ssa->block_count : 1,
                                    sizeof(*block_map));
    bool *constant_used = calloc(ssa->value_count ? ssa->value_count : 1,
                                 sizeof(*constant_used));
    if (!value_map || !block_map || !constant_used) {
        free(value_map);
        free(block_map);
        free(constant_used);
        return false;
    }

    function->first_block = (MirBlockRef)arena->block_count;
    for (size_t offset = 0; offset < info->block_count; offset++) {
        SsaBlockRef source_block = info->first_block + (SsaBlockRef)offset;
        const SsaBlock *block = &ssa->blocks[source_block];
        MirBlockRef target = mir_add_block(arena, block->name,
                                           block->source_line, block->source_col,
                                           block->is_entry);
        if (target == MIR_BLOCK_NONE) goto fail;
        block_map[source_block] = target;
    }
    function->entry = block_map[info->entry];
    function->block_count = info->block_count;

    for (size_t offset = 0; offset < info->block_count; offset++) {
        SsaBlockRef source_block = info->first_block + (SsaBlockRef)offset;
        MirBlockRef target = block_map[source_block];
        const SsaBlock *block = &ssa->blocks[source_block];
        for (size_t p = 0; p < block->pred_count; p++) {
            SsaBlockRef pred = block->preds[p];
            if (pred < ssa->block_count && block_map[pred] != MIR_BLOCK_NONE &&
                !mir_append_unique_block_ref(&arena->blocks[target].preds,
                                             &arena->blocks[target].pred_count,
                                             &arena->blocks[target].pred_cap,
                                             block_map[pred])) goto fail;
        }
        for (size_t s = 0; s < block->succ_count; s++) {
            SsaBlockRef succ = block->succs[s];
            if (succ < ssa->block_count && block_map[succ] != MIR_BLOCK_NONE &&
                !mir_append_unique_block_ref(&arena->blocks[target].succs,
                                             &arena->blocks[target].succ_count,
                                             &arena->blocks[target].succ_cap,
                                             block_map[succ])) goto fail;
        }
    }

    for (size_t offset = 0; offset < info->block_count; offset++) {
        SsaBlockRef source_block = info->first_block + (SsaBlockRef)offset;
        const SsaBlock *block = &ssa->blocks[source_block];
        for (size_t i = 0; i < block->inst_count; i++) {
            const SsaInst *inst = &ssa->insts[block->insts[i]];
            for (size_t o = 0; o < inst->operand_count; o++) {
                SsaValueRef value = ssa->operands[inst->operand_start + o];
                if (value < ssa->value_count &&
                    ssa->values[value].kind == SSA_VALUE_CONST)
                    constant_used[value] = true;
            }
            if (inst->op == SSA_OP_JUMP || inst->op == SSA_OP_BRANCH) {
                for (size_t e = 0; e < inst->edge_count; e++) {
                    SsaValueRef value = ssa->edges[inst->edge_start + e];
                    if (value < ssa->value_count &&
                        ssa->values[value].kind == SSA_VALUE_CONST)
                        constant_used[value] = true;
                }
                for (size_t e = 0; e < inst->edge2_count; e++) {
                    SsaValueRef value = ssa->edges[inst->edge2_start + e];
                    if (value < ssa->value_count &&
                        ssa->values[value].kind == SSA_VALUE_CONST)
                        constant_used[value] = true;
                }
            }
        }
    }

    for (size_t offset = 0; offset < info->block_count; offset++) {
        SsaBlockRef source_block = info->first_block + (SsaBlockRef)offset;
        MirBlockRef target_block = block_map[source_block];
        const SsaBlock *block = &ssa->blocks[source_block];
        for (size_t p = 0; p < block->param_count; p++) {
            SsaValueRef source_ref = block->params[p];
            const SsaValue *source_value = &ssa->values[source_ref];
            MirReg reg = mir_add_source_reg(arena, source_value, source_ref,
                                            (uint32_t)function_index, target_block,
                                            false, true);
            if (reg == MIR_REG_NONE) goto fail;
            value_map[source_ref] = reg;
            MirReg *grown = mir_grow(arena->blocks[target_block].params,
                                      sizeof(*grown),
                                      &arena->blocks[target_block].param_cap,
                                      arena->blocks[target_block].param_count + 1);
            if (!grown) goto fail;
            arena->blocks[target_block].params = grown;
            arena->blocks[target_block].params[
                arena->blocks[target_block].param_count++] = reg;
        }
    }

    for (size_t p = 0; p < info->ssa_param_count; p++) {
        SsaValueRef source_ref = info->params[p];
        if (source_ref == SSA_VALUE_NONE || source_ref >= ssa->value_count) goto fail;
        const SsaValue *source_value = &ssa->values[source_ref];
        MirReg reg = mir_add_source_reg(arena, source_value, source_ref,
                                        (uint32_t)function_index, function->entry,
                                        true, false);
        if (reg == MIR_REG_NONE) goto fail;
        value_map[source_ref] = reg;
        MirInst synthetic;
        memset(&synthetic, 0, sizeof(synthetic));
        synthetic.op = MIR_OP_ABI_MOVE;
        synthetic.machine_type = mir_type_for_cobra(source_value->type);
        synthetic.type = source_value->type;
        synthetic.result = reg;
        synthetic.abi_locations = info->call_abi.params[p];
        synthetic.source_line = source_value->source_line;
        synthetic.source_col = source_value->source_col;
        MirInstRef inst = mir_add_inst(arena, &synthetic, NULL, 0);
        if (inst == MIR_INST_NONE || !mir_block_add_inst(arena, function->entry, inst)) goto fail;
        arena->regs[reg].def_inst = inst;
    }

    for (size_t source_ref = 1; source_ref < ssa->value_count; source_ref++) {
        const SsaValue *source_value = &ssa->values[source_ref];
        if (source_value->kind != SSA_VALUE_CONST || !constant_used[source_ref] ||
            value_map[source_ref] != MIR_REG_NONE)
            continue;
        MirReg reg = mir_add_source_reg(arena, source_value, (SsaValueRef)source_ref,
                                        (uint32_t)function_index, function->entry,
                                        true, false);
        if (reg == MIR_REG_NONE) goto fail;
        value_map[source_ref] = reg;
        MirInst synthetic;
        memset(&synthetic, 0, sizeof(synthetic));
        synthetic.op = MIR_OP_CONST;
        synthetic.machine_type = mir_type_for_cobra(source_value->type);
        synthetic.type = source_value->type;
        synthetic.result = reg;
        synthetic.immediate = source_value->const_value;
        synthetic.has_immediate = true;
        synthetic.source_line = source_value->source_line;
        synthetic.source_col = source_value->source_col;
        MirInstRef inst = mir_add_inst(arena, &synthetic, NULL, 0);
        if (inst == MIR_INST_NONE || !mir_block_add_inst(arena, function->entry, inst)) goto fail;
        arena->regs[reg].def_inst = inst;
    }

    for (size_t offset = 0; offset < info->block_count; offset++) {
        SsaBlockRef source_block = info->first_block + (SsaBlockRef)offset;
        MirBlockRef target_block = block_map[source_block];
        const SsaBlock *block = &ssa->blocks[source_block];
        for (size_t index = 0; index < block->inst_count; index++) {
            SsaInstRef source_inst_ref = block->insts[index];
            const SsaInst *source_inst = &ssa->insts[source_inst_ref];
            MirInst lowered;
            memset(&lowered, 0, sizeof(lowered));
            lowered.op = mir_opcode_for_ssa(source_inst->op);
            lowered.machine_type = mir_type_for_cobra(source_inst->type);
            lowered.type = source_inst->type;
            lowered.effect = mir_effect_for_ssa(source_inst->effect);
            lowered.source_line = source_inst->source_line;
            lowered.source_col = source_inst->source_col;
            mir_copy_inst_metadata(&lowered, source_inst);
            if (lowered.op == MIR_OP_NONE) {
                if (getenv("BIR_MIR_DEBUG")) fprintf(stderr, "mir debug: fn '%s' op %d (%s) unmapped to MIR\n", info->name, (int)source_inst->op, bir_opcode_name(source_inst->op));
                goto fail;
            }

            MirReg operands[MIR_MAX_OPERANDS];
            if (source_inst->operand_count > MIR_MAX_OPERANDS) goto fail;
            for (size_t o = 0; o < source_inst->operand_count; o++) {
                if (!mir_map_value(value_map, ssa->value_count,
                                   ssa->operands[source_inst->operand_start + o],
                                   &operands[o])) {
                    if (getenv("BIR_MIR_DEBUG")) fprintf(stderr, "mir debug: fn '%s' op %d (%s) operand %zu unmapped\n", info->name, (int)source_inst->op, bir_opcode_name(source_inst->op), o);
                    goto fail;
                }
            }
            if (source_inst->view_source != SSA_VALUE_NONE &&
                !mir_map_value(value_map, ssa->value_count,
                               source_inst->view_source,
                               &lowered.view_source)) {
                if (getenv("BIR_MIR_DEBUG")) fprintf(stderr, "mir debug: fn '%s' op %d (%s) view_source unmapped\n", info->name, (int)source_inst->op, bir_opcode_name(source_inst->op));
                goto fail;
            }
            if (source_inst->result != SSA_VALUE_NONE) {
                const SsaValue *result_value = &ssa->values[source_inst->result];
                MirReg result = mir_add_source_reg(arena, result_value,
                                                   source_inst->result,
                                                   (uint32_t)function_index,
                                                   target_block, false, false);
                if (result == MIR_REG_NONE) {
                    if (getenv("BIR_MIR_DEBUG")) fprintf(stderr, "mir debug: fn '%s' op %d (%s) result reg failed\n", info->name, (int)source_inst->op, bir_opcode_name(source_inst->op));
                    goto fail;
                }
                value_map[source_inst->result] = result;
                lowered.result = result;
            }
            if (source_inst->op == SSA_OP_CALL) {
                int callee_index = source_callee_index(source, source_inst->callee);
                if (callee_index < 0) {
                    if (getenv("BIR_MIR_DEBUG")) fprintf(stderr, "mir debug: fn '%s' call to unknown callee '%s'\n", info->name, source_inst->callee);
                    goto fail;
                }
                lowered.callee_index = (uint32_t)callee_index;
                lowered.clobbers = mir_default_call_clobbers();
                snprintf(lowered.callee, sizeof(lowered.callee), "%s",
                         source_inst->callee);
            }
            if (source_inst->op >= SSA_OP_DICT_ALLOC &&
                source_inst->op <= SSA_OP_DICT_FREE) {
                snprintf(lowered.dict_key, sizeof(lowered.dict_key), "%s",
                         source_inst->dict_key);
            }
            if (source_inst->op == SSA_OP_RETURN) {
                lowered.abi_locations = function->call_abi.returns;
            }
            MirInstRef lowered_ref = mir_add_inst(arena, &lowered, operands,
                                                  source_inst->operand_count);
            if (lowered_ref == MIR_INST_NONE ||
                !mir_block_add_inst(arena, target_block, lowered_ref)) goto fail;
            if (lowered.result != MIR_REG_NONE) {
                arena->regs[lowered.result].def_inst = lowered_ref;
            }
            if (bir_is_terminator(source_inst->op)) {
                MirBlock *mir_block = &arena->blocks[target_block];
                mir_block->terminator = lowered_ref;
                if (source_inst->op == SSA_OP_JUMP || source_inst->op == SSA_OP_BRANCH) {
                    if (source_inst->target >= ssa->block_count ||
                        block_map[source_inst->target] == MIR_BLOCK_NONE) goto fail;
                    arena->insts[lowered_ref].target = block_map[source_inst->target];
                    MirReg edge_values[MIR_MAX_OPERANDS];
                    if (source_inst->edge_count > MIR_MAX_OPERANDS) goto fail;
                    for (size_t e = 0; e < source_inst->edge_count; e++) {
                        if (!mir_map_value(value_map, ssa->value_count,
                                           ssa->edges[source_inst->edge_start + e],
                                           &edge_values[e])) goto fail;
                    }
                    uint32_t start = mir_append_edges(arena, edge_values,
                                                      source_inst->edge_count);
                    if (start == UINT32_MAX) goto fail;
                    arena->insts[lowered_ref].edge_start = start;
                    arena->insts[lowered_ref].edge_count = source_inst->edge_count;
                    if (source_inst->op == SSA_OP_BRANCH) {
                        if (source_inst->target2 >= ssa->block_count ||
                            block_map[source_inst->target2] == MIR_BLOCK_NONE ||
                            source_inst->edge2_count > MIR_MAX_OPERANDS) goto fail;
                        arena->insts[lowered_ref].target2 = block_map[source_inst->target2];
                        for (size_t e = 0; e < source_inst->edge2_count; e++) {
                            if (!mir_map_value(value_map, ssa->value_count,
                                               ssa->edges[source_inst->edge2_start + e],
                                               &edge_values[e])) goto fail;
                        }
                        start = mir_append_edges(arena, edge_values,
                                                 source_inst->edge2_count);
                        if (start == UINT32_MAX) goto fail;
                        arena->insts[lowered_ref].edge2_start = start;
                        arena->insts[lowered_ref].edge2_count = source_inst->edge2_count;
                    }
                }
            }
        }
    }

    free(value_map);
    free(block_map);
    free(constant_used);
    return true;

fail:
    free(value_map);
    free(block_map);
    free(constant_used);
    return false;
}

bool mir_lower_module(const BackendIrModule *source, MirModule *module,
                     char *errbuf, size_t errbuf_size) {
    if (errbuf && errbuf_size) errbuf[0] = '\0';
    if (!source || !module) {
        mir_set_error(errbuf, errbuf_size, "MIR lowering requires a source and destination module");
        return false;
    }
    char ssa_error[COBRA_MAX_TOKEN_TEXT] = {0};
    if (!bir_verify(source, ssa_error, sizeof(ssa_error))) {
        mir_set_error(errbuf, errbuf_size, "cannot lower unverified SSA: %s", ssa_error);
        return false;
    }
    mir_module_init(module, source);
    for (size_t f = 0; f < source->function_count; f++) {
        if (module->function_count >= MIR_MAX_FUNCTIONS ||
            !mir_lower_function(source, module, &source->functions[f], f)) {
            mir_set_error(errbuf, errbuf_size,
                          "could not lower SSA function '%s' to MIR",
                          source->functions[f].name);
            mir_module_free(module);
            return false;
        }
    }
    if (!mir_verify(module, errbuf, errbuf_size)) {
        mir_module_free(module);
        return false;
    }
    return true;
}

static bool mir_valid_reg(const MirModule *module, MirReg reg) {
    return module && reg != MIR_REG_NONE && reg < module->arena.reg_count;
}

static bool mir_valid_block(const MirModule *module, MirBlockRef block) {
    return module && block != MIR_BLOCK_NONE && block < module->arena.block_count;
}

static bool mir_valid_inst(const MirModule *module, MirInstRef inst) {
    return module && inst != MIR_INST_NONE && inst < module->arena.inst_count;
}

static bool mir_types_equal(const CobraType *left, const CobraType *right) {
    return left && right && (left == right || cobra_type_equal(left, right));
}

static bool mir_type_is_numeric(MirMachineType type) {
    return type == MIR_TYPE_I8 || type == MIR_TYPE_I32 || type == MIR_TYPE_U32 ||
           type == MIR_TYPE_I64 || type == MIR_TYPE_U64 || type == MIR_TYPE_F32 ||
           type == MIR_TYPE_F64;
}

static bool mir_type_is_convert_scalar(MirMachineType type) {
    return mir_type_is_numeric(type) || type == MIR_TYPE_BOOL;
}

static bool mir_check_edge(const MirModule *module, MirBlockRef target,
                           uint32_t start, uint32_t count, char *err,
                           size_t err_size) {
    if (!mir_valid_block(module, target) ||
        (size_t)start > module->arena.edge_used ||
        (size_t)count > module->arena.edge_used - start) {
        mir_set_error(err, err_size, "MIR edge has an invalid target or argument window");
        return false;
    }
    const MirBlock *block = &module->arena.blocks[target];
    if (count != block->param_count) {
        mir_set_error(err, err_size, "MIR edge argument count does not match block parameters");
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        MirReg argument = module->arena.edges[start + i];
        if (!mir_valid_reg(module, argument) ||
            module->arena.regs[argument].machine_type !=
                module->arena.regs[block->params[i]].machine_type) {
            mir_set_error(err, err_size, "MIR edge argument has the wrong machine type");
            return false;
        }
    }
    return true;
}

static bool mir_block_in_function(const MirFunction *function, MirBlockRef block);

static bool mir_owned_payload_field_matches(const CobraType *aggregate,
                                            int64_t offset,
                                            const CobraType *payload) {
    if (!aggregate || !payload || offset < 0) return false;
    if (aggregate->kind == COBRA_TYPE_STRUCT) {
        for (size_t i = 0; i < aggregate->field_count; i++) {
            const CobraTypeField *field = &aggregate->fields[i];
            if ((int64_t)field->offset == offset &&
                bir_is_owned_slice_type(field->type) &&
                mir_types_equal(field->type, payload)) return true;
        }
        return false;
    }
    if (bir_is_sum_type(aggregate)) {
        for (size_t selector = 1; selector <= aggregate->generic_arg_count; selector++) {
            const CobraType *component = aggregate->generic_args[selector - 1];
            if ((int64_t)bir_sum_component_offset(aggregate, (int)selector) == offset &&
                bir_is_owned_slice_type(component) &&
                mir_types_equal(component, payload)) return true;
        }
    }
    return false;
}

static bool mir_check_instruction(const MirModule *module, size_t function_index,
                                  MirBlockRef block_ref, MirInstRef inst_ref,
                                  char *err, size_t err_size) {
    const MirArena *arena = &module->arena;
    const MirInst *inst = &arena->insts[inst_ref];
    const MirFunction *function = &module->functions[function_index];
    if (inst->op == MIR_OP_NONE) {
        mir_set_error(err, err_size, "MIR block b%u contains an invalid opcode", block_ref);
        return false;
    }
    if ((size_t)inst->operand_start > arena->operand_used ||
        (size_t)inst->operand_count > arena->operand_used - inst->operand_start) {
        mir_set_error(err, err_size, "MIR instruction has an invalid operand window");
        return false;
    }
    for (size_t o = 0; o < inst->operand_count; o++) {
        MirReg reg = arena->operands[inst->operand_start + o];
        if (!mir_valid_reg(module, reg) ||
            arena->regs[reg].function_index != function_index) {
            mir_set_error(err, err_size, "MIR instruction uses a foreign virtual register");
            return false;
        }
    }
    if (inst->result != MIR_REG_NONE) {
        if (!mir_valid_reg(module, inst->result) ||
            arena->regs[inst->result].function_index != function_index ||
            arena->regs[inst->result].def_inst != inst_ref) {
            mir_set_error(err, err_size, "MIR instruction has an invalid result definition");
            return false;
        }
    }
    switch (inst->op) {
        case MIR_OP_CONST:
            if (inst->result == MIR_REG_NONE || !inst->has_immediate ||
                inst->operand_count != 0 || !inst->type ||
                inst->immediate.type != inst->type) {
                mir_set_error(err, err_size, "MIR const has invalid immediate metadata");
                return false;
            }
            break;
        case MIR_OP_ABI_MOVE:
            if (inst->result == MIR_REG_NONE || inst->operand_count != 0 ||
                inst->abi_locations.count == 0 ||
                (arena->regs[inst->result].machine_type == MIR_TYPE_VIEW &&
                 inst->abi_locations.count != 2 && inst->abi_locations.count != 3)) {
                mir_set_error(err, err_size, "MIR ABI move has invalid location metadata");
                return false;
            }
            break;
        case MIR_OP_BLOCK_ARG:
            if (inst->result == MIR_REG_NONE || inst->operand_count != 0) {
                mir_set_error(err, err_size, "MIR block argument has invalid operands");
                return false;
            }
            break;
        case MIR_OP_ADD:
        case MIR_OP_SUB:
        case MIR_OP_MUL:
        case MIR_OP_DIV:
        case MIR_OP_REM:
            if (inst->operand_count != 2 || inst->result == MIR_REG_NONE ||
                !mir_type_is_numeric(inst->machine_type) ||
                arena->regs[arena->operands[inst->operand_start]].machine_type != inst->machine_type ||
                arena->regs[arena->operands[inst->operand_start + 1]].machine_type != inst->machine_type) {
                mir_set_error(err, err_size, "MIR numeric operation has an invalid signature");
                return false;
            }
            break;
        case MIR_OP_NEG:
            if (inst->operand_count != 1 || inst->result == MIR_REG_NONE ||
                !mir_type_is_numeric(inst->machine_type) ||
                arena->regs[arena->operands[inst->operand_start]].machine_type != inst->machine_type) {
                mir_set_error(err, err_size, "MIR negation has an invalid signature");
                return false;
            }
            break;
        case MIR_OP_CONVERT:
            /* Deliberately not the equal-type shape every other unary op
               uses above: convert's operand and result machine types are
               independent, each only constrained to the scalar-convert set. */
            if (inst->operand_count != 1 || inst->result == MIR_REG_NONE ||
                !mir_type_is_convert_scalar(inst->machine_type) ||
                !mir_type_is_convert_scalar(
                    arena->regs[arena->operands[inst->operand_start]].machine_type)) {
                mir_set_error(err, err_size, "MIR convert has an invalid signature");
                return false;
            }
            break;
        case MIR_OP_EQ:
        case MIR_OP_NE:
        case MIR_OP_LT:
        case MIR_OP_LE:
        case MIR_OP_GT:
        case MIR_OP_GE:
            {
                MirMachineType operand_type =
                    arena->regs[arena->operands[inst->operand_start]].machine_type;
                bool operand_ok = mir_type_is_numeric(operand_type) ||
                    ((inst->op == MIR_OP_EQ || inst->op == MIR_OP_NE) &&
                     operand_type == MIR_TYPE_BOOL);
                if (inst->operand_count != 2 || inst->result == MIR_REG_NONE ||
                    inst->machine_type != MIR_TYPE_BOOL || !operand_ok ||
                    arena->regs[arena->operands[inst->operand_start]].machine_type !=
                        arena->regs[arena->operands[inst->operand_start + 1]].machine_type) {
                    mir_set_error(err, err_size, "MIR comparison has an invalid signature");
                    return false;
                }
            }
            break;
        case MIR_OP_LOAD:
            if (inst->operand_count != 1 || inst->result == MIR_REG_NONE ||
                arena->regs[arena->operands[inst->operand_start]].machine_type != MIR_TYPE_ADDRESS ||
                inst->effect != MIR_EFFECT_READ) {
                mir_set_error(err, err_size, "MIR load has an invalid signature");
                return false;
            }
            break;
        case MIR_OP_STORE:
            if (inst->operand_count != 2 || inst->result != MIR_REG_NONE ||
                arena->regs[arena->operands[inst->operand_start]].machine_type != MIR_TYPE_ADDRESS ||
                inst->effect != MIR_EFFECT_WRITE) {
                mir_set_error(err, err_size, "MIR store has an invalid signature");
                return false;
            }
            break;
        case MIR_OP_AGG_COPY:
            if (inst->operand_count != 2 || inst->result != MIR_REG_NONE ||
                inst->effect != MIR_EFFECT_READWRITE) {
                mir_set_error(err, err_size, "MIR aggregate copy has an invalid signature");
                return false;
            }
            break;
        case MIR_OP_ARRAY_INDEX_ADDR:
            if (inst->operand_count != 2 || inst->result == MIR_REG_NONE ||
                inst->machine_type != MIR_TYPE_ADDRESS ||
                arena->regs[arena->operands[inst->operand_start]].machine_type != MIR_TYPE_ADDRESS ||
                arena->regs[arena->operands[inst->operand_start + 1]].machine_type != MIR_TYPE_I64 ||
                !inst->memory_type || inst->memory_type->kind != COBRA_TYPE_ARRAY ||
                !inst->memory_type->generic_args[0] ||
                inst->memory_width != inst->memory_type->generic_args[0]->size ||
                inst->memory_alignment != inst->memory_type->generic_args[0]->alignment) {
                mir_set_error(err, err_size, "MIR fixed-array index address has an invalid signature");
                return false;
            }
            break;
        case MIR_OP_SUM_PAYLOAD_STORE:
        case MIR_OP_FIELD_PAYLOAD_STORE: {
            if (inst->operand_count != 2 || inst->result != MIR_REG_NONE ||
                inst->effect != MIR_EFFECT_READWRITE || !inst->memory_type ||
                !bir_is_owned_slice_type(inst->memory_type) ||
                arena->regs[arena->operands[inst->operand_start]].machine_type != MIR_TYPE_ADDRESS ||
                arena->regs[arena->operands[inst->operand_start + 1]].machine_type != MIR_TYPE_VIEW ||
                inst->memory_offset < 0) {
                mir_set_error(err, err_size, "MIR owned payload store has an invalid signature");
                return false;
            }
            MirReg destination = arena->operands[inst->operand_start];
            const CobraType *aggregate = inst->op == MIR_OP_FIELD_PAYLOAD_STORE
                ? inst->aggregate_type
                : (arena->regs[destination].type &&
                   arena->regs[destination].type->generic_arg_count == 1
                       ? arena->regs[destination].type->generic_args[0] : NULL);
            if (!aggregate || !bir_type_has_owned_payload(aggregate) ||
                !mir_owned_payload_field_matches(aggregate, inst->memory_offset,
                                                 inst->memory_type)) {
                mir_set_error(err, err_size, "MIR owned payload store has invalid aggregate field metadata");
                return false;
            }
            break;
        }
        case MIR_OP_SUM_PAYLOAD_LOAD:
        case MIR_OP_FIELD_PAYLOAD_LOAD: {
            if (inst->operand_count != 1 || inst->result == MIR_REG_NONE ||
                inst->effect != MIR_EFFECT_READWRITE || !inst->memory_type ||
                !bir_is_owned_slice_type(inst->memory_type) ||
                inst->machine_type != MIR_TYPE_VIEW ||
                arena->regs[arena->operands[inst->operand_start]].machine_type != MIR_TYPE_ADDRESS ||
                inst->memory_offset < 0) {
                mir_set_error(err, err_size, "MIR owned payload load has an invalid signature");
                return false;
            }
            MirReg source = arena->operands[inst->operand_start];
            const CobraType *aggregate = inst->op == MIR_OP_FIELD_PAYLOAD_LOAD
                ? inst->aggregate_type
                : (arena->regs[source].type &&
                   arena->regs[source].type->generic_arg_count == 1
                       ? arena->regs[source].type->generic_args[0] : NULL);
            if (!aggregate || !bir_type_has_owned_payload(aggregate) ||
                !mir_owned_payload_field_matches(aggregate, inst->memory_offset,
                                                 inst->memory_type)) {
                mir_set_error(err, err_size, "MIR owned payload load has invalid aggregate field metadata");
                return false;
            }
            break;
        }
        case MIR_OP_SUM_MOVE:
        case MIR_OP_AGG_MOVE: {
            if (inst->operand_count != 2 || inst->result != MIR_REG_NONE ||
                inst->effect != MIR_EFFECT_READWRITE || !inst->memory_type ||
                arena->regs[arena->operands[inst->operand_start]].machine_type != MIR_TYPE_ADDRESS ||
                arena->regs[arena->operands[inst->operand_start + 1]].machine_type != MIR_TYPE_ADDRESS ||
                !bir_type_has_owned_payload(inst->memory_type) ||
                inst->memory_width != inst->memory_type->size) {
                mir_set_error(err, err_size, "MIR aggregate move has an invalid signature");
                return false;
            }
            break;
        }
        case MIR_OP_SUM_DROP:
        case MIR_OP_AGG_DROP: {
            if (inst->operand_count != 1 || inst->result != MIR_REG_NONE ||
                inst->effect != MIR_EFFECT_READWRITE || !inst->memory_type ||
                arena->regs[arena->operands[inst->operand_start]].machine_type != MIR_TYPE_ADDRESS ||
                !bir_type_has_owned_payload(inst->memory_type) ||
                inst->memory_width != inst->memory_type->size) {
                mir_set_error(err, err_size, "MIR aggregate drop has an invalid signature");
                return false;
            }
            break;
        }
        case MIR_OP_TRANSFER: {
            if (inst->operand_count != 1 || inst->result == MIR_REG_NONE ||
                inst->machine_type != MIR_TYPE_ADDRESS ||
                arena->regs[arena->operands[inst->operand_start]].machine_type != MIR_TYPE_ADDRESS ||
                arena->regs[inst->result].machine_type != MIR_TYPE_ADDRESS ||
                inst->pointer_contract != BIR_POINTER_CONTRACT_OWNED_REGION ||
                inst->effect != MIR_EFFECT_NONE) {
                mir_set_error(err, err_size, "MIR transfer has an invalid ownership signature");
                return false;
            }
            break;
        }
        case MIR_OP_DESTROY: {
            if (inst->operand_count != 1 || inst->result != MIR_REG_NONE ||
                inst->effect != MIR_EFFECT_NONE) {
                mir_set_error(err, err_size, "MIR destroy has an invalid signature");
                return false;
            }
            MirReg source = arena->operands[inst->operand_start];
            MirMachineType source_type = arena->regs[source].machine_type;
            if (source_type != MIR_TYPE_ADDRESS && source_type != MIR_TYPE_VIEW) {
                mir_set_error(err, err_size, "MIR destroy requires an address or owned view");
                return false;
            }
            if (source_type == MIR_TYPE_VIEW &&
                arena->regs[source].pointer_contract != BIR_POINTER_CONTRACT_OWNED_SLICE) {
                mir_set_error(err, err_size, "MIR destroy requires an owned view");
                return false;
            }
            break;
        }
        case MIR_OP_BUFFER_ALLOC: {
            if (inst->operand_count != 1 || inst->result == MIR_REG_NONE ||
                inst->machine_type != MIR_TYPE_VIEW || !inst->type ||
                !bir_is_owned_buffer_type(inst->type) || !inst->memory_type ||
                (!cobra_type_is_scalar(inst->memory_type) &&
                 !bir_type_is_value_only_struct(inst->memory_type)) ||
                inst->pointer_contract != BIR_POINTER_CONTRACT_OWNED_SLICE ||
                inst->allocation_id == 0 ||
                arena->regs[arena->operands[inst->operand_start]].machine_type != MIR_TYPE_I64) {
                mir_set_error(err, err_size, "MIR buffer allocation has invalid metadata");
                return false;
            }
            break;
        }
        case MIR_OP_BUFFER_APPEND: {
            if (inst->operand_count != 2 || inst->result == MIR_REG_NONE ||
                inst->machine_type != MIR_TYPE_VIEW || !inst->type ||
                !bir_is_owned_buffer_type(inst->type) || !inst->memory_type ||
                (!cobra_type_is_scalar(inst->memory_type) &&
                 !bir_type_is_value_only_struct(inst->memory_type)) ||
                inst->pointer_contract != BIR_POINTER_CONTRACT_OWNED_SLICE ||
                inst->allocation_id == 0 || inst->effect != MIR_EFFECT_READWRITE ||
                arena->regs[arena->operands[inst->operand_start]].machine_type != MIR_TYPE_VIEW ||
                (cobra_type_is_scalar(inst->memory_type)
                     ? arena->regs[arena->operands[inst->operand_start + 1]].machine_type !=
                           mir_type_for_cobra(inst->memory_type)
                     : arena->regs[arena->operands[inst->operand_start + 1]].machine_type !=
                           MIR_TYPE_ADDRESS)) {
                mir_set_error(err, err_size, "MIR buffer append has invalid metadata");
                return false;
            }
            break;
        }
        case MIR_OP_BUFFER_POP: {
            if (inst->operand_count != 1 || inst->result == MIR_REG_NONE ||
                !inst->type || !cobra_type_is_scalar(inst->type) || !inst->memory_type ||
                !mir_types_equal(inst->type, inst->memory_type) ||
                inst->effect != MIR_EFFECT_READWRITE ||
                arena->regs[arena->operands[inst->operand_start]].machine_type != MIR_TYPE_VIEW) {
                mir_set_error(err, err_size, "MIR buffer pop has invalid metadata");
                return false;
            }
            break;
        }
        case MIR_OP_BUFFER_FREE: {
            if (inst->operand_count != 1 || inst->result != MIR_REG_NONE ||
                inst->effect != MIR_EFFECT_NONE ||
                !arena->regs[arena->operands[inst->operand_start]].type ||
                !bir_is_owned_buffer_type(arena->regs[arena->operands[inst->operand_start]].type) ||
                arena->regs[arena->operands[inst->operand_start]].machine_type != MIR_TYPE_VIEW) {
                mir_set_error(err, err_size, "MIR buffer free has invalid metadata");
                return false;
            }
            break;
        }
        case MIR_OP_DICT_ALLOC: {
            if (inst->operand_count != 1 || inst->result == MIR_REG_NONE ||
                inst->machine_type != MIR_TYPE_VIEW || !inst->type ||
                !bir_is_owned_dict_type(inst->type) || !inst->memory_type ||
                !cobra_type_is_scalar(inst->memory_type) ||
                inst->pointer_contract != BIR_POINTER_CONTRACT_OWNED_SLICE ||
                inst->allocation_id == 0 ||
                arena->regs[arena->operands[inst->operand_start]].machine_type != MIR_TYPE_I64) {
                mir_set_error(err, err_size, "MIR dict allocation has invalid metadata");
                return false;
            }
            break;
        }
        case MIR_OP_DICT_SET: {
            if (inst->operand_count != 2 || inst->result == MIR_REG_NONE ||
                inst->machine_type != MIR_TYPE_VIEW || !inst->type ||
                !bir_is_owned_dict_type(inst->type) || !inst->memory_type ||
                !cobra_type_is_scalar(inst->memory_type) ||
                inst->dict_key[0] == '\0' ||
                inst->pointer_contract != BIR_POINTER_CONTRACT_OWNED_SLICE ||
                inst->allocation_id == 0 || inst->effect != MIR_EFFECT_READWRITE ||
                arena->regs[arena->operands[inst->operand_start]].machine_type != MIR_TYPE_VIEW ||
                arena->regs[arena->operands[inst->operand_start + 1]].machine_type !=
                    mir_type_for_cobra(inst->memory_type)) {
                mir_set_error(err, err_size, "MIR dict set has invalid metadata");
                return false;
            }
            break;
        }
        case MIR_OP_DICT_GET: {
            if (inst->operand_count != 2 || inst->result == MIR_REG_NONE ||
                !inst->type || inst->machine_type != MIR_TYPE_I64 ||
                !inst->memory_type || !cobra_type_is_scalar(inst->memory_type) ||
                inst->dict_key[0] == '\0' || inst->effect != MIR_EFFECT_READ ||
                arena->regs[arena->operands[inst->operand_start]].machine_type != MIR_TYPE_VIEW ||
                arena->regs[arena->operands[inst->operand_start + 1]].machine_type != MIR_TYPE_I64) {
                mir_set_error(err, err_size, "MIR dict get has invalid metadata");
                return false;
            }
            break;
        }
        case MIR_OP_DICT_HAS: {
            if (inst->operand_count != 1 || inst->result == MIR_REG_NONE ||
                !inst->type || inst->machine_type != MIR_TYPE_I64 ||
                inst->dict_key[0] == '\0' || inst->effect != MIR_EFFECT_READ ||
                arena->regs[arena->operands[inst->operand_start]].machine_type != MIR_TYPE_VIEW) {
                mir_set_error(err, err_size, "MIR dict has has invalid metadata");
                return false;
            }
            break;
        }
        case MIR_OP_DICT_DELETE: {
            if (inst->operand_count != 1 || inst->result == MIR_REG_NONE ||
                inst->machine_type != MIR_TYPE_VIEW || !inst->type ||
                !bir_is_owned_dict_type(inst->type) ||
                inst->dict_key[0] == '\0' ||
                inst->pointer_contract != BIR_POINTER_CONTRACT_OWNED_SLICE ||
                inst->allocation_id == 0 || inst->effect != MIR_EFFECT_READWRITE ||
                arena->regs[arena->operands[inst->operand_start]].machine_type != MIR_TYPE_VIEW) {
                mir_set_error(err, err_size, "MIR dict delete has invalid metadata");
                return false;
            }
            break;
        }
        case MIR_OP_DICT_POP: {
            if (inst->operand_count != 2 || inst->result == MIR_REG_NONE ||
                !inst->type || inst->machine_type != MIR_TYPE_I64 ||
                !inst->memory_type || !bir_is_owned_dict_type(inst->memory_type) ||
                inst->dict_key[0] == '\0' || inst->effect != MIR_EFFECT_READWRITE ||
                arena->regs[arena->operands[inst->operand_start]].machine_type != MIR_TYPE_VIEW ||
                arena->regs[arena->operands[inst->operand_start + 1]].machine_type != MIR_TYPE_I64) {
                mir_set_error(err, err_size, "MIR dict pop has invalid metadata");
                return false;
            }
            break;
        }
        case MIR_OP_DICT_LEN: {
            if (inst->operand_count != 1 || inst->result == MIR_REG_NONE ||
                !inst->type || inst->machine_type != MIR_TYPE_I64 ||
                inst->effect != MIR_EFFECT_READ ||
                arena->regs[arena->operands[inst->operand_start]].machine_type != MIR_TYPE_VIEW) {
                mir_set_error(err, err_size, "MIR dict len has invalid metadata");
                return false;
            }
            break;
        }
        case MIR_OP_DICT_FREE: {
            if (inst->operand_count != 1 || inst->result != MIR_REG_NONE ||
                inst->effect != MIR_EFFECT_NONE ||
                !arena->regs[arena->operands[inst->operand_start]].type ||
                !bir_is_owned_dict_type(arena->regs[arena->operands[inst->operand_start]].type) ||
                arena->regs[arena->operands[inst->operand_start]].machine_type != MIR_TYPE_VIEW) {
                mir_set_error(err, err_size, "MIR dict free has invalid metadata");
                return false;
            }
            break;
        }
        case MIR_OP_STRING_CONCAT: {
            if (inst->operand_count != 2 || inst->result == MIR_REG_NONE ||
                inst->machine_type != MIR_TYPE_VIEW || !inst->type ||
                !bir_is_owned_slice_type(inst->type) ||
                !cobra_type_element(inst->type) ||
                cobra_type_element(inst->type)->kind != COBRA_TYPE_U8 ||
                !inst->memory_type || inst->memory_type->kind != COBRA_TYPE_U8 ||
                inst->pointer_contract != BIR_POINTER_CONTRACT_OWNED_SLICE ||
                inst->allocation_id == 0 ||
                arena->regs[arena->operands[inst->operand_start]].machine_type != MIR_TYPE_VIEW ||
                arena->regs[arena->operands[inst->operand_start + 1]].machine_type != MIR_TYPE_VIEW) {
                mir_set_error(err, err_size, "MIR string concatenation has invalid metadata");
                return false;
            }
            break;
        }
        case MIR_OP_STRING_EQ: {
            if (inst->operand_count != 2 || inst->result == MIR_REG_NONE ||
                inst->machine_type != MIR_TYPE_BOOL ||
                arena->regs[arena->operands[inst->operand_start]].machine_type != MIR_TYPE_VIEW ||
                arena->regs[arena->operands[inst->operand_start + 1]].machine_type != MIR_TYPE_VIEW) {
                mir_set_error(err, err_size, "MIR string equality has invalid metadata");
                return false;
            }
            break;
        }
        case MIR_OP_CALL: {
            if (inst->effect != MIR_EFFECT_CALL || inst->callee_index >= module->function_count ||
                (inst->clobbers.gpr_mask == 0 && inst->clobbers.xmm_mask == 0)) {
                mir_set_error(err, err_size, "MIR call has invalid effect, callee, or clobber set");
                return false;
            }
            const MirFunction *callee = &module->functions[inst->callee_index];
            const BirFunctionInfo *source_callee =
                &module->source->functions[inst->callee_index];
            /* Extern (`import c`) callees carry no lowered signature; the
               call site's own operand count is the only thing to check,
               matching x86_emit_call's separate extern-call path below. */
            if (source_callee->is_extern) {
                if (inst->operand_count > BIR_ABI_MAX_GPR_ARGUMENT_REGISTERS) {
                    mir_set_error(err, err_size, "MIR call does not match the callee ABI");
                    return false;
                }
            } else if (inst->operand_count != callee->call_abi.param_count ||
                memcmp(&callee->call_abi, &source_callee->call_abi,
                       sizeof(BirCallAbi)) != 0 ||
                !bir_validate_function_abi(module->source, source_callee)) {
                mir_set_error(err, err_size, "MIR call does not match the callee ABI");
                return false;
            }
            if (!source_callee->is_extern) {
                for (size_t arg = 0; arg < inst->operand_count; arg++) {
                    MirReg reg = arena->operands[inst->operand_start + arg];
                    const CobraType *expected = NULL;
                    if (source_callee->has_hidden_return_storage && arg == 0)
                        expected = source_callee->return_value_type;
                    else {
                        size_t source_arg = arg -
                            (source_callee->has_hidden_return_storage ? 1U : 0U);
                        expected = source_callee->param_value_types[source_arg];
                    }
                    if (!mir_types_equal(arena->regs[reg].type, expected)) {
                        mir_set_error(err, err_size, "MIR call argument type does not match the callee");
                        return false;
                    }
                }
            }
            if (source_callee->has_return && !source_callee->has_hidden_return_storage &&
                (inst->result == MIR_REG_NONE ||
                 !mir_types_equal(arena->regs[inst->result].type,
                                  source_callee->return_type))) {
                mir_set_error(err, err_size, "MIR call result type does not match the callee");
                return false;
            }
            if (callee->has_hidden_return_storage) {
                if (inst->result != MIR_REG_NONE) {
                    mir_set_error(err, err_size, "MIR aggregate call unexpectedly has a result");
                    return false;
                }
            } else if (callee->has_return && inst->result == MIR_REG_NONE) {
                mir_set_error(err, err_size, "MIR value call has no result");
                return false;
            }
            break;
        }
        case MIR_OP_RETURN:
            if (function->has_return && !function->has_hidden_return_storage &&
                (inst->operand_count != 1 ||
                 !mir_types_equal(arena->regs[arena->operands[inst->operand_start]].type,
                                  function->return_type))) {
                mir_set_error(err, err_size, "MIR value return has the wrong arity or type");
                return false;
            }
            if ((!function->has_return || function->has_hidden_return_storage) &&
                inst->operand_count != 0) {
                mir_set_error(err, err_size, "MIR aggregate or void return has an operand");
                return false;
            }
            if (inst->abi_locations.count != function->call_abi.returns.count) {
                mir_set_error(err, err_size, "MIR return ABI locations do not match the function");
                return false;
            }
            break;
        case MIR_OP_JUMP:
            if (inst->operand_count != 0 || !mir_block_in_function(function, inst->target) ||
                !mir_check_edge(module, inst->target, inst->edge_start,
                                inst->edge_count, err, err_size)) return false;
            break;
        case MIR_OP_BRANCH:
            if (inst->operand_count != 1 ||
                arena->regs[arena->operands[inst->operand_start]].machine_type != MIR_TYPE_BOOL ||
                !mir_block_in_function(function, inst->target) ||
                !mir_block_in_function(function, inst->target2) ||
                !mir_check_edge(module, inst->target, inst->edge_start,
                                inst->edge_count, err, err_size) ||
                !mir_check_edge(module, inst->target2, inst->edge2_start,
                                inst->edge2_count, err, err_size)) return false;
            break;
        default:
            break;
    }
    return true;
}

static bool mir_block_in_function(const MirFunction *function, MirBlockRef block) {
    return block >= function->first_block &&
           block - function->first_block < function->block_count;
}

static bool mir_compute_dominators(const MirModule *module, size_t function_index,
                                   bool *dom) {
    const MirFunction *function = &module->functions[function_index];
    size_t blocks = module->arena.block_count;
    for (size_t b = function->first_block; b < function->first_block + function->block_count; b++) {
        for (size_t d = 0; d < blocks; d++) dom[b * blocks + d] = false;
        if (b == function->entry) {
            dom[b * blocks + b] = true;
        } else {
            for (size_t d = function->first_block;
                 d < function->first_block + function->block_count; d++)
                dom[b * blocks + d] = true;
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t b = function->first_block;
             b < function->first_block + function->block_count; b++) {
            if (b == function->entry) continue;
            const MirBlock *block = &module->arena.blocks[b];
            bool have_pred = false;
            bool *candidate = calloc(blocks ? blocks : 1, sizeof(bool));
            if (!candidate) return false;
            for (size_t p = 0; p < block->pred_count; p++) {
                MirBlockRef pred = block->preds[p];
                if (!mir_block_in_function(function, pred)) continue;
                if (!have_pred) {
                    for (size_t d = 0; d < blocks; d++) candidate[d] = dom[pred * blocks + d];
                    have_pred = true;
                } else {
                    for (size_t d = 0; d < blocks; d++)
                        candidate[d] = candidate[d] && dom[pred * blocks + d];
                }
            }
            if (have_pred) {
                candidate[b] = true;
                for (size_t d = 0; d < blocks; d++) {
                    if (candidate[d] != dom[b * blocks + d]) {
                        dom[b * blocks + d] = candidate[d];
                        changed = true;
                    }
                }
            }
            free(candidate);
        }
    }
    return true;
}

static bool mir_check_dominance(const MirModule *module, size_t function_index,
                                const bool *dom, char *err, size_t err_size) {
    const MirFunction *function = &module->functions[function_index];
    size_t blocks = module->arena.block_count;
    for (MirBlockRef block_ref = function->first_block;
         block_ref < function->first_block + function->block_count; block_ref++) {
        const MirBlock *block = &module->arena.blocks[block_ref];
        for (size_t index = 0; index < block->inst_count; index++) {
            const MirInst *inst = &module->arena.insts[block->insts[index]];
            for (size_t o = 0; o < inst->operand_count; o++) {
                MirReg reg = module->arena.operands[inst->operand_start + o];
                const MirRegInfo *info = &module->arena.regs[reg];
                if (info->entry_defined ||
                    (info->block_parameter && info->def_block == block_ref)) continue;
                if (info->def_block == block_ref) {
                    bool found = false;
                    for (size_t d = 0; d < block->inst_count; d++) {
                        if (block->insts[d] == info->def_inst) {
                            found = d < index;
                            break;
                        }
                    }
                    if (!found) {
                        mir_set_error(err, err_size, "MIR virtual register is used before definition");
                        return false;
                    }
                } else if (!mir_valid_block(module, info->def_block) ||
                           !dom[block_ref * blocks + info->def_block]) {
                    mir_set_error(err, err_size, "MIR virtual register definition does not dominate its use");
                    return false;
                }
            }
            const MirInst *term = index + 1 == block->inst_count ? inst : NULL;
            if (!term || (term->op != MIR_OP_JUMP && term->op != MIR_OP_BRANCH)) continue;
            for (size_t e = 0; e < term->edge_count; e++) {
                MirReg reg = module->arena.edges[term->edge_start + e];
                const MirRegInfo *info = &module->arena.regs[reg];
                if (info->entry_defined ||
                    (info->block_parameter && info->def_block == block_ref)) continue;
                if (info->def_block == block_ref) {
                    bool found = false;
                    for (size_t d = 0; d < block->inst_count; d++) {
                        if (block->insts[d] == info->def_inst) {
                            found = d < index;
                            break;
                        }
                    }
                    if (!found) {
                        mir_set_error(err, err_size, "MIR edge uses a value before definition");
                        return false;
                    }
                } else if (!dom[block_ref * blocks + info->def_block]) {
                    mir_set_error(err, err_size, "MIR edge value definition does not dominate its use");
                    return false;
                }
            }
            for (size_t e = 0; e < term->edge2_count; e++) {
                MirReg reg = module->arena.edges[term->edge2_start + e];
                const MirRegInfo *info = &module->arena.regs[reg];
                if (!info->entry_defined && info->def_block != block_ref &&
                    !dom[block_ref * blocks + info->def_block]) {
                    mir_set_error(err, err_size, "MIR branch edge value does not dominate its use");
                    return false;
                }
            }
        }
    }
    return true;
}

bool mir_verify(const MirModule *module, char *errbuf, size_t errbuf_size) {
    if (errbuf && errbuf_size) errbuf[0] = '\0';
    if (!module || !module->source || module->function_count == 0 ||
        module->function_count > MIR_MAX_FUNCTIONS) {
        mir_set_error(errbuf, errbuf_size, "invalid MIR module");
        return false;
    }
    size_t blocks = module->arena.block_count;
    bool *owned = calloc(blocks ? blocks : 1, sizeof(bool));
    bool *dom = calloc((blocks ? blocks : 1) * (blocks ? blocks : 1), sizeof(bool));
    if (!owned || !dom) {
        free(owned);
        free(dom);
        mir_set_error(errbuf, errbuf_size, "out of memory verifying MIR");
        return false;
    }
    bool ok = true;
    for (size_t f = 0; f < module->function_count && ok; f++) {
        const MirFunction *function = &module->functions[f];
        if (module->source->functions[f].is_extern) {
            if (function->entry != MIR_BLOCK_NONE || function->first_block != MIR_BLOCK_NONE) {
                mir_set_error(errbuf, errbuf_size, "MIR extern function '%s' unexpectedly has a body", function->name);
                ok = false;
                break;
            }
            continue;
        }
        if (function->entry == MIR_BLOCK_NONE || function->first_block == MIR_BLOCK_NONE ||
            function->block_count == 0 || function->first_block >= blocks ||
            function->block_count > blocks - function->first_block ||
            function->entry != function->first_block ||
            !bir_validate_function_abi(module->source, &module->source->functions[f]) ||
            memcmp(&function->call_abi, &module->source->functions[f].call_abi,
                   sizeof(BirCallAbi)) != 0) {
            mir_set_error(errbuf, errbuf_size, "MIR function '%s' has an invalid signature", function->name);
            ok = false;
            break;
        }
        for (size_t offset = 0; offset < function->block_count && ok; offset++) {
            MirBlockRef block_ref = function->first_block + (MirBlockRef)offset;
            if (owned[block_ref]) {
                mir_set_error(errbuf, errbuf_size, "MIR block belongs to multiple functions");
                ok = false;
                break;
            }
            owned[block_ref] = true;
            const MirBlock *block = &module->arena.blocks[block_ref];
            if (block->terminator == MIR_INST_NONE || block->inst_count == 0 ||
                block->insts[block->inst_count - 1] != block->terminator) {
                mir_set_error(errbuf, errbuf_size, "MIR block has no final terminator");
                ok = false;
                break;
            }
            for (size_t i = 0; i < block->inst_count && ok; i++) {
                MirInstRef inst_ref = block->insts[i];
                if (!mir_valid_inst(module, inst_ref)) {
                    mir_set_error(errbuf, errbuf_size, "MIR block contains an invalid instruction");
                    ok = false;
                    break;
                }
                MirOpcode op = module->arena.insts[inst_ref].op;
                if (i + 1 < block->inst_count &&
                    (op == MIR_OP_JUMP || op == MIR_OP_BRANCH || op == MIR_OP_RETURN)) {
                    mir_set_error(errbuf, errbuf_size, "MIR terminator is not last in its block");
                    ok = false;
                    break;
                }
                if (!mir_check_instruction(module, f, block_ref, inst_ref,
                                           errbuf, errbuf_size)) ok = false;
            }
        }
        if (ok && !mir_compute_dominators(module, f, dom)) {
            mir_set_error(errbuf, errbuf_size, "out of memory computing MIR dominators");
            ok = false;
        }
        if (ok && !mir_check_dominance(module, f, dom, errbuf, errbuf_size)) ok = false;
    }
    for (size_t r = 1; r < module->arena.reg_count && ok; r++) {
        const MirRegInfo *info = &module->arena.regs[r];
        if (info->function_index >= module->function_count || !info->type ||
            !info->type->finalized || info->machine_type != mir_type_for_cobra(info->type)) {
            mir_set_error(errbuf, errbuf_size, "MIR virtual register has invalid type metadata");
            ok = false;
        }
    }
    free(owned);
    free(dom);
    return ok;
}

static void mir_print_abi_location(FILE *out, const BirAbiLocation *location) {
    if (location->pass_mode == BIR_ABI_PASS_INDIRECT) fprintf(out, "indirect:");
    if (location->storage == BIR_ABI_STORAGE_REGISTER) {
        fprintf(out, "%s%u", location->register_class == BIR_ABI_REGISTER_XMM ? "xmm" : "gpr",
                (unsigned)location->register_index);
    } else if (location->storage == BIR_ABI_STORAGE_STACK) {
        fprintf(out, "stack+%u", (unsigned)location->stack_offset);
    } else {
        fprintf(out, "none");
    }
}

void mir_dump(const MirModule *module, FILE *out) {
    if (!module || !out) return;
    fprintf(out, "; MIR: %s\n", module->source_file);
    for (size_t f = 0; f < module->function_count; f++) {
        const MirFunction *function = &module->functions[f];
        fprintf(out, "fn \"%s\" -> %s\n", function->name,
                mir_machine_type_name(mir_type_for_cobra(function->return_type)));
        fprintf(out, "  abi params=%zu stack=%u return=",
                function->call_abi.param_count,
                (unsigned)function->call_abi.stack_size);
        for (size_t p = 0; p < function->call_abi.returns.count; p++) {
            if (p) fprintf(out, "+");
            mir_print_abi_location(out, &function->call_abi.returns.parts[p]);
        }
        fprintf(out, "\n");
        for (size_t offset = 0; offset < function->block_count; offset++) {
            MirBlockRef block_ref = function->first_block + (MirBlockRef)offset;
            const MirBlock *block = &module->arena.blocks[block_ref];
            fprintf(out, "block b%u \"%s\"", block_ref, block->name);
            if (block->param_count) {
                fprintf(out, " params=");
                for (size_t p = 0; p < block->param_count; p++) {
                    if (p) fprintf(out, ",");
                    fprintf(out, "r%u", block->params[p]);
                }
            }
            fprintf(out, "\n");
            for (size_t i = 0; i < block->inst_count; i++) {
                MirInstRef ref = block->insts[i];
                const MirInst *inst = &module->arena.insts[ref];
                fprintf(out, "  ");
                if (inst->result != MIR_REG_NONE)
                    fprintf(out, "r%u:%s = ", inst->result,
                            mir_machine_type_name(inst->machine_type));
                fprintf(out, "%s", mir_opcode_name(inst->op));
                if (inst->op == MIR_OP_CALL) {
                    fprintf(out, " \"%s\" [clobber:gpr=0x%llx,xmm=0x%llx]",
                            inst->callee,
                            (unsigned long long)inst->clobbers.gpr_mask,
                            (unsigned long long)inst->clobbers.xmm_mask);
                }
                if (inst->has_immediate) {
                    fprintf(out, " immediate=");
                    if (inst->immediate.kind == BIR_SCALAR_F32)
                        fprintf(out, "f32bits(0x%08x)", (unsigned)inst->immediate.payload.f32_bits);
                    else if (inst->immediate.kind == BIR_SCALAR_F64)
                        fprintf(out, "f64bits(0x%016llx)",
                                (unsigned long long)inst->immediate.payload.f64_bits);
                    else fprintf(out, "%lld", (long long)inst->immediate.payload.i64);
                }
                if (inst->operand_count) {
                    fprintf(out, "(");
                    for (size_t o = 0; o < inst->operand_count; o++) {
                        if (o) fprintf(out, ",");
                        fprintf(out, "r%u", module->arena.operands[inst->operand_start + o]);
                    }
                    fprintf(out, ")");
                }
                if (inst->op == MIR_OP_JUMP || inst->op == MIR_OP_BRANCH)
                    fprintf(out, " -> b%u", inst->target);
                if (inst->op == MIR_OP_BRANCH) fprintf(out, ", b%u", inst->target2);
                if (inst->op == MIR_OP_RETURN && inst->abi_locations.count) {
                    fprintf(out, " [abi:");
                    for (size_t p = 0; p < inst->abi_locations.count; p++) {
                        if (p) fprintf(out, "+");
                        mir_print_abi_location(out, &inst->abi_locations.parts[p]);
                    }
                    fprintf(out, "]");
                }
                fprintf(out, "\n");
            }
        }
    }
}
