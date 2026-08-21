/*
 * Cobra Backend IR: flat arena-backed SSA construction.
 *
 * Stable integer handles into growable pools. Operands and edge arguments
 * are variable-length windows into flat pools. See docs/BACKEND_IR.md.
 */
#include "ssa.h"
#include <limits.h>

BirScalarValue bir_scalar_i64(const CobraType *type, int64_t value) {
    BirScalarValue result;
    memset(&result, 0, sizeof(result));
    result.type = type;
    result.kind = BIR_SCALAR_I64;
    result.payload.i64 = value;
    return result;
}

BirScalarValue bir_scalar_i32(const CobraType *type, int32_t value) {
    BirScalarValue result;
    memset(&result, 0, sizeof(result));
    result.type = type;
    result.kind = BIR_SCALAR_I32;
    result.payload.i64 = value;
    return result;
}

BirScalarValue bir_scalar_u32(const CobraType *type, uint32_t value) {
    BirScalarValue result;
    memset(&result, 0, sizeof(result));
    result.type = type;
    result.kind = BIR_SCALAR_U32;
    result.payload.i64 = value;
    return result;
}

BirScalarValue bir_scalar_u64(const CobraType *type, uint64_t value) {
    BirScalarValue result;
    memset(&result, 0, sizeof(result));
    result.type = type;
    result.kind = BIR_SCALAR_U64;
    result.payload.i64 = (int64_t)value;
    return result;
}

BirScalarValue bir_scalar_bool(const CobraType *type, bool value) {
    BirScalarValue result;
    memset(&result, 0, sizeof(result));
    result.type = type;
    result.kind = BIR_SCALAR_BOOL;
    result.payload.i64 = value ? 1 : 0;
    return result;
}

BirScalarValue bir_scalar_f32(const CobraType *type, float value) {
    BirScalarValue result;
    memset(&result, 0, sizeof(result));
    result.type = type;
    result.kind = BIR_SCALAR_F32;
    memcpy(&result.payload.f32_bits, &value, sizeof(result.payload.f32_bits));
    return result;
}

BirScalarValue bir_scalar_f64(const CobraType *type, double value) {
    BirScalarValue result;
    memset(&result, 0, sizeof(result));
    result.type = type;
    result.kind = BIR_SCALAR_F64;
    memcpy(&result.payload.f64_bits, &value, sizeof(result.payload.f64_bits));
    return result;
}

BirScalarValue bir_scalar_u8(const CobraType *type, uint8_t value) {
    BirScalarValue result;
    memset(&result, 0, sizeof(result));
    result.type = type;
    result.kind = BIR_SCALAR_U8;
    result.payload.i64 = value;
    return result;
}

BirScalarValue bir_scalar_pointer(const CobraType *type, uint32_t frame_id,
                                 int64_t offset) {
    return bir_scalar_pointer_with_contract(type, frame_id, offset,
                                            BIR_POINTER_CONTRACT_UNKNOWN);
}

BirScalarValue bir_scalar_pointer_with_contract(const CobraType *type,
                                                uint32_t frame_id,
                                                int64_t offset,
                                                BirPointerContract contract) {
    BirScalarValue result;
    memset(&result, 0, sizeof(result));
    result.type = type;
    result.kind = BIR_SCALAR_POINTER;
    result.payload.pointer.frame_id = frame_id;
    result.payload.pointer.allocation_id = 0;
    result.payload.pointer.region_id = 0;
    result.payload.pointer.offset = offset;
    result.payload.pointer.allocation_base_offset = 0;
    result.payload.pointer.allocation_size = 0;
    result.payload.pointer.view_base_offset = 0;
    result.payload.pointer.view_length = 0;
    result.payload.pointer.view_element_width = 0;
    result.payload.pointer.contract = contract;
    result.payload.pointer.origin = frame_id ? BIR_POINTER_ORIGIN_FRAME
                                             : BIR_POINTER_ORIGIN_UNKNOWN;
    return result;
}

float bir_scalar_as_f32(BirScalarValue value) {
    float result = 0.0f;
    memcpy(&result, &value.payload.f32_bits, sizeof(result));
    return result;
}

double bir_scalar_as_f64(BirScalarValue value) {
    double result = 0.0;
    memcpy(&result, &value.payload.f64_bits, sizeof(result));
    return result;
}

BirScalarValue bir_scalar_view(const CobraType *type, BirPointerValue pointer,
                               int64_t length) {
    return bir_scalar_buffer(type, pointer, length, length);
}

BirScalarValue bir_scalar_buffer(const CobraType *type, BirPointerValue pointer,
                                 int64_t length, int64_t capacity) {
    BirScalarValue result;
    memset(&result, 0, sizeof(result));
    result.type = type;
    result.kind = BIR_SCALAR_VIEW;
    result.payload.view.pointer = pointer;
    result.payload.view.length = length;
    result.payload.view.capacity = capacity;
    return result;
}

BirPointerValue bir_scalar_as_pointer(BirScalarValue value) {
    return value.payload.pointer;
}

bool bir_scalar_is_zero(BirScalarValue value) {
    if (value.kind == BIR_SCALAR_F32) return bir_scalar_as_f32(value) == 0.0f;
    if (value.kind == BIR_SCALAR_POINTER) {
        return value.payload.pointer.frame_id == 0 && value.payload.pointer.offset == 0 &&
               value.payload.pointer.allocation_id == 0;
    }
    if (value.kind == BIR_SCALAR_VIEW) {
        return value.payload.view.length == 0 &&
               value.payload.view.pointer.frame_id == 0 &&
               value.payload.view.pointer.allocation_id == 0;
    }
    return value.payload.i64 == 0;
}

static void *grow_pool(void *ptr, size_t elem, size_t *cap, size_t needed) {
    if (needed <= *cap) return ptr;
    size_t next = *cap ? *cap * 2 : 16;
    while (next < needed) next *= 2;
    void *grown = realloc(ptr, elem * next);
    if (!grown) return NULL;
    *cap = next;
    return grown;
}

void bir_arena_init(SsaArena *arena) {
    memset(arena, 0, sizeof(*arena));
    /* Reserve pool slot 0 as the invalid-handle sentinel so SSA_VALUE_NONE /
       SSA_INST_NONE / SSA_BLOCK_NONE (all 0) never collide with a real,
       first-created entry. Real handles are therefore 1-based. */
    SsaValue *values = grow_pool(arena->values, sizeof(SsaValue), &arena->value_cap, 1);
    SsaInst *insts = grow_pool(arena->insts, sizeof(SsaInst), &arena->inst_cap, 1);
    SsaBlock *blocks = grow_pool(arena->blocks, sizeof(SsaBlock), &arena->block_cap, 1);
    if (values) {
        arena->values = values;
        memset(&arena->values[0], 0, sizeof(SsaValue));
        arena->values[0].kind = SSA_VALUE_INVALID;
        arena->value_count = 1;
    }
    if (insts) {
        arena->insts = insts;
        memset(&arena->insts[0], 0, sizeof(SsaInst));
        arena->insts[0].op = SSA_OP_NONE;
        arena->inst_count = 1;
    }
    if (blocks) {
        arena->blocks = blocks;
        memset(&arena->blocks[0], 0, sizeof(SsaBlock));
        arena->blocks[0].terminator = SSA_INST_NONE;
        arena->block_count = 1;
    }
}

void bir_arena_free(SsaArena *arena) {
    if (!arena) return;
    for (size_t i = 0; i < arena->block_count; i++) {
        free(arena->blocks[i].insts);
        free(arena->blocks[i].params);
        free(arena->blocks[i].preds);
        free(arena->blocks[i].succs);
    }
    free(arena->blocks);
    free(arena->insts);
    free(arena->values);
    free(arena->operands);
    free(arena->edges);
    memset(arena, 0, sizeof(*arena));
}

SsaBlockRef bir_add_block(SsaArena *arena, const char *name, int line, int col) {
    if (!arena) return SSA_BLOCK_NONE;
    SsaBlock *grown = grow_pool(arena->blocks, sizeof(SsaBlock), &arena->block_cap,
                                arena->block_count + 1);
    if (!grown) return SSA_BLOCK_NONE;
    arena->blocks = grown;
    SsaBlock *block = &arena->blocks[arena->block_count];
    memset(block, 0, sizeof(*block));
    if (name) snprintf(block->name, sizeof(block->name), "%s", name);
    block->source_line = line;
    block->source_col = col;
    block->terminator = SSA_INST_NONE;
    return (SsaBlockRef)arena->block_count++;
}

SsaBlockRef bir_add_entry_block(SsaArena *arena, const char *name, int line, int col) {
    SsaBlockRef block = bir_add_block(arena, name, line, col);
    if (block != SSA_BLOCK_NONE) arena->blocks[block].is_entry = true;
    return block;
}

SsaValueRef bir_add_value(SsaArena *arena, SsaValueKind kind, const CobraType *type,
                          int line, int col) {
    if (!arena || kind == SSA_VALUE_INVALID) return SSA_VALUE_NONE;
    SsaValue *grown = grow_pool(arena->values, sizeof(SsaValue), &arena->value_cap,
                                arena->value_count + 1);
    if (!grown) return SSA_VALUE_NONE;
    arena->values = grown;
    SsaValue *value = &arena->values[arena->value_count];
    memset(value, 0, sizeof(*value));
    value->kind = kind;
    value->pointer_contract = BIR_POINTER_CONTRACT_UNKNOWN;
    value->pointer_origin = BIR_POINTER_ORIGIN_UNKNOWN;
    value->region_id = 0;
    value->allocation_id = 0;
    value->type = type;
    value->source_line = line;
    value->source_col = col;
    return (SsaValueRef)arena->value_count++;
}

SsaValueRef bir_add_const(SsaArena *arena, BirScalarValue value,
                          int line, int col) {
    SsaValueRef ref = bir_add_value(arena, SSA_VALUE_CONST, value.type, line, col);
    if (ref != SSA_VALUE_NONE) arena->values[ref].const_value = value;
    return ref;
}

const CobraType *bir_pointer_type(BackendIrModule *module, const CobraType *pointee) {
    if (!module || !module->type_arena || !pointee) return NULL;
    for (size_t i = 0; i < module->type_arena->count; i++) {
        CobraType *candidate = &module->type_arena->nodes[i];
        if (candidate->kind == COBRA_TYPE_POINTER &&
            candidate->generic_arg_count == 1 &&
            (candidate->generic_args[0] == pointee ||
             cobra_type_equal(candidate->generic_args[0], pointee))) {
            return candidate;
        }
    }
    CobraType *pointer = cobra_type_make(module->type_arena, COBRA_TYPE_POINTER,
                                         NULL, pointee, NULL, NULL, NULL,
                                         COBRA_OWNERSHIP_VALUE,
                                         COBRA_MUTABILITY_DEFAULT, -1);
    if (!pointer || !cobra_type_finalize(module->type_arena, pointer)) return NULL;
    return pointer;
}

SsaInstRef bir_add_inst(SsaArena *arena, SsaOpcode op, const CobraType *type,
                        const SsaValueRef *operands, size_t operand_count,
                        int line, int col) {
    if (!arena || op == SSA_OP_NONE) return SSA_INST_NONE;
    SsaInst *inst_grown = grow_pool(arena->insts, sizeof(SsaInst), &arena->inst_cap,
                                    arena->inst_count + 1);
    if (!inst_grown) return SSA_INST_NONE;
    arena->insts = inst_grown;
    if (operand_count) {
        SsaValueRef *op_grown = grow_pool(arena->operands, sizeof(SsaValueRef),
                                          &arena->operand_cap,
                                          arena->operand_used + operand_count);
        if (!op_grown) return SSA_INST_NONE;
        arena->operands = op_grown;
    }
    SsaInst *inst = &arena->insts[arena->inst_count];
    memset(inst, 0, sizeof(*inst));
    inst->op = op;
    inst->type = type;
    inst->result = SSA_VALUE_NONE;
    inst->pointer_contract = BIR_POINTER_CONTRACT_UNKNOWN;
    inst->pointer_origin = BIR_POINTER_ORIGIN_UNKNOWN;
    inst->region_id = 0;
    inst->allocation_id = 0;
    inst->parent_region_id = 0;
    inst->view_length = 0;
    inst->operand_start = (uint32_t)arena->operand_used;
    inst->operand_count = (uint32_t)operand_count;
    for (size_t i = 0; i < operand_count; i++)
        arena->operands[arena->operand_used + i] = operands[i];
    arena->operand_used += operand_count;
    inst->target = SSA_BLOCK_NONE;
    inst->target2 = SSA_BLOCK_NONE;
    if (op == SSA_OP_LOAD || op == SSA_OP_STORE || op == SSA_OP_STACK_SLOT) {
        inst->address_kind = SSA_ADDRESS_TYPED_POINTER;
        inst->memory_width = 0;
        inst->memory_alignment = 0;
        inst->address_space = 0;
    }
    inst->source_line = line;
    inst->source_col = col;
    return (SsaInstRef)arena->inst_count++;
}

static bool reserve_edges(SsaArena *arena, size_t extra) {
    if (!extra) return true;
    SsaValueRef *grown = grow_pool(arena->edges, sizeof(SsaValueRef), &arena->edge_cap,
                                   arena->edge_used + extra);
    if (!grown) return false;
    arena->edges = grown;
    return true;
}

SsaInstRef bir_add_stack_slot(SsaArena *arena, const CobraType *pointer_type,
                              const CobraType *memory_type, int64_t offset,
                              uint32_t alignment, uint32_t slot, int line, int col) {
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_STACK_SLOT, pointer_type,
                                   NULL, 0, line, col);
    if (inst == SSA_INST_NONE) return SSA_INST_NONE;
    arena->insts[inst].memory_type = memory_type;
    arena->insts[inst].memory_offset = offset;
    arena->insts[inst].memory_width = memory_type ? (uint32_t)memory_type->size : 0;
    arena->insts[inst].memory_alignment = alignment;
    arena->insts[inst].stack_slot = slot;
    arena->insts[inst].pointer_contract = BIR_POINTER_CONTRACT_OWNED_FRAME;
    arena->insts[inst].pointer_origin = BIR_POINTER_ORIGIN_FRAME;
    arena->insts[inst].allocation_id = slot + 1U;
    return inst;
}

SsaInstRef bir_add_region_stack_slot(SsaArena *arena, const CobraType *pointer_type,
                                     const CobraType *memory_type, int64_t offset,
                                     uint32_t alignment, uint32_t slot,
                                     uint32_t region_id, int line, int col) {
    SsaInstRef inst = bir_add_stack_slot(arena, pointer_type, memory_type, offset,
                                         alignment, slot, line, col);
    if (inst != SSA_INST_NONE) {
        arena->insts[inst].pointer_contract = BIR_POINTER_CONTRACT_OWNED_REGION;
        arena->insts[inst].pointer_origin = BIR_POINTER_ORIGIN_REGION;
        arena->insts[inst].region_id = region_id;
    }
    return inst;
}

SsaInstRef bir_add_region_enter(SsaArena *arena, uint32_t region_id,
                                uint32_t parent_region_id, int line, int col) {
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_REGION_ENTER, NULL,
                                   NULL, 0, line, col);
    if (inst != SSA_INST_NONE) {
        arena->insts[inst].region_id = region_id;
        arena->insts[inst].parent_region_id = parent_region_id;
    }
    return inst;
}

SsaInstRef bir_add_region_exit(SsaArena *arena, uint32_t region_id,
                               int line, int col) {
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_REGION_EXIT, NULL,
                                   NULL, 0, line, col);
    if (inst != SSA_INST_NONE) arena->insts[inst].region_id = region_id;
    return inst;
}

SsaInstRef bir_add_transfer(SsaArena *arena, const CobraType *pointer_type,
                            SsaValueRef pointer, uint32_t destination_region_id,
                            int line, int col) {
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_TRANSFER, pointer_type,
                                   &pointer, 1, line, col);
    if (inst == SSA_INST_NONE) return SSA_INST_NONE;
    arena->insts[inst].pointer_contract = BIR_POINTER_CONTRACT_OWNED_REGION;
    arena->insts[inst].pointer_origin = BIR_POINTER_ORIGIN_REGION;
    arena->insts[inst].region_id = destination_region_id;
    arena->insts[inst].allocation_id = pointer < arena->value_count
        ? arena->values[pointer].allocation_id : 0;
    return inst;
}

SsaInstRef bir_add_destroy(SsaArena *arena, SsaValueRef pointer,
                           int line, int col) {
    return bir_add_inst(arena, SSA_OP_DESTROY, NULL, &pointer, 1, line, col);
}

SsaInstRef bir_add_view_make(SsaArena *arena, const CobraType *view_type,
                             const CobraType *element_type, SsaValueRef pointer,
                             SsaValueRef length, int line, int col) {
    const SsaValueRef operands[2] = {pointer, length};
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_VIEW_MAKE, view_type,
                                   operands, 2, line, col);
    if (inst == SSA_INST_NONE) return SSA_INST_NONE;
    arena->insts[inst].memory_type = element_type;
    arena->insts[inst].pointer_contract = bir_view_is_writable(view_type)
        ? BIR_POINTER_CONTRACT_BORROW_WRITE
        : BIR_POINTER_CONTRACT_BORROW_READONLY;
    if (pointer < arena->value_count) {
        arena->insts[inst].pointer_origin = arena->values[pointer].pointer_origin;
        arena->insts[inst].region_id = arena->values[pointer].region_id;
        arena->insts[inst].allocation_id = arena->values[pointer].allocation_id;
    }
    return inst;
}

SsaInstRef bir_add_view_ptr(SsaArena *arena, const CobraType *pointer_type,
                            const CobraType *element_type, SsaValueRef view,
                            int line, int col) {
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_VIEW_PTR, pointer_type,
                                   &view, 1, line, col);
    if (inst == SSA_INST_NONE) return SSA_INST_NONE;
    arena->insts[inst].memory_type = element_type;
    if (view < arena->value_count &&
        bir_is_owned_slice_type(arena->values[view].type)) {
        arena->insts[inst].pointer_contract = BIR_POINTER_CONTRACT_OWNED_SLICE;
    } else {
        arena->insts[inst].pointer_contract = view < arena->value_count &&
            bir_view_is_writable(arena->values[view].type)
                ? BIR_POINTER_CONTRACT_BORROW_WRITE
                : BIR_POINTER_CONTRACT_BORROW_READONLY;
    }
    if (view < arena->value_count) {
        arena->insts[inst].pointer_origin = arena->values[view].pointer_origin;
        arena->insts[inst].region_id = arena->values[view].region_id;
        arena->insts[inst].allocation_id = arena->values[view].allocation_id;
    }
    return inst;
}

SsaInstRef bir_add_view_len(SsaArena *arena, const CobraType *i64_type,
                            const CobraType *view, SsaValueRef value,
                            int line, int col) {
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_VIEW_LEN, i64_type,
                                   &value, 1, line, col);
    if (inst != SSA_INST_NONE) arena->insts[inst].memory_type = view;
    return inst;
}

SsaInstRef bir_add_slice_alloc(SsaArena *arena, const CobraType *owned_type,
                               const CobraType *element_type, SsaValueRef length,
                               uint32_t allocation_id, int line, int col) {
    const SsaValueRef operands[1] = {length};
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_SLICE_ALLOC, owned_type,
                                   operands, 1, line, col);
    if (inst == SSA_INST_NONE) return SSA_INST_NONE;
    arena->insts[inst].memory_type = element_type;
    arena->insts[inst].pointer_contract = BIR_POINTER_CONTRACT_OWNED_SLICE;
    arena->insts[inst].pointer_origin = BIR_POINTER_ORIGIN_FRAME;
    arena->insts[inst].allocation_id = allocation_id;
    arena->insts[inst].region_id = BIR_REGION_NONE;
    return inst;
}

SsaInstRef bir_add_region_slice_alloc(SsaArena *arena, const CobraType *owned_type,
                                      const CobraType *element_type, SsaValueRef length,
                                      uint32_t allocation_id, uint32_t region_id,
                                      int line, int col) {
    SsaInstRef inst = bir_add_slice_alloc(arena, owned_type, element_type, length,
                                          allocation_id, line, col);
    if (inst != SSA_INST_NONE) {
        arena->insts[inst].pointer_origin = BIR_POINTER_ORIGIN_REGION;
        arena->insts[inst].region_id = region_id;
    }
    return inst;
}

SsaInstRef bir_add_slice_free(SsaArena *arena, SsaValueRef slice,
                              int line, int col) {
    return bir_add_inst(arena, SSA_OP_SLICE_FREE, NULL, &slice, 1, line, col);
}

SsaInstRef bir_add_print_i64(SsaArena *arena, SsaValueRef value, int line, int col) {
    return bir_add_inst(arena, SSA_OP_PRINT_I64, NULL, &value, 1, line, col);
}

SsaInstRef bir_add_print_string(SsaArena *arena, SsaValueRef view, int line, int col) {
    return bir_add_inst(arena, SSA_OP_PRINT_STRING, NULL, &view, 1, line, col);
}

SsaInstRef bir_add_assert(SsaArena *arena, SsaValueRef cond, int line, int col) {
    return bir_add_inst(arena, SSA_OP_ASSERT, NULL, &cond, 1, line, col);
}

SsaInstRef bir_add_buffer_alloc(SsaArena *arena, const CobraType *buffer_type,
                                const CobraType *element_type, SsaValueRef length,
                                uint32_t allocation_id, int line, int col) {
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_BUFFER_ALLOC, buffer_type,
                                   &length, 1, line, col);
    if (inst == SSA_INST_NONE) return SSA_INST_NONE;
    arena->insts[inst].memory_type = element_type;
    arena->insts[inst].pointer_contract = BIR_POINTER_CONTRACT_OWNED_SLICE;
    arena->insts[inst].pointer_origin = BIR_POINTER_ORIGIN_FRAME;
    arena->insts[inst].allocation_id = allocation_id;
    arena->insts[inst].region_id = BIR_REGION_NONE;
    return inst;
}

SsaInstRef bir_add_buffer_append(SsaArena *arena, const CobraType *buffer_type,
                                 const CobraType *element_type, SsaValueRef buffer,
                                 SsaValueRef value, uint32_t allocation_id,
                                 int line, int col) {
    const SsaValueRef operands[2] = {buffer, value};
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_BUFFER_APPEND, buffer_type,
                                   operands, 2, line, col);
    if (inst == SSA_INST_NONE) return SSA_INST_NONE;
    arena->insts[inst].memory_type = element_type;
    arena->insts[inst].pointer_contract = BIR_POINTER_CONTRACT_OWNED_SLICE;
    arena->insts[inst].pointer_origin = BIR_POINTER_ORIGIN_FRAME;
    arena->insts[inst].allocation_id = allocation_id;
    arena->insts[inst].effect = SSA_EFFECT_READWRITE;
    return inst;
}

SsaInstRef bir_add_buffer_pop(SsaArena *arena, const CobraType *element_type,
                              SsaValueRef buffer, SsaValueRef fallback,
                              int line, int col) {
    const SsaValueRef operands[2] = {buffer, fallback};
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_BUFFER_POP, element_type,
                                   operands, 2, line, col);
    if (inst == SSA_INST_NONE) return SSA_INST_NONE;
    arena->insts[inst].memory_type = element_type;
    arena->insts[inst].effect = SSA_EFFECT_READWRITE;
    return inst;
}

SsaInstRef bir_add_buffer_free(SsaArena *arena, SsaValueRef buffer,
                               int line, int col) {
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_BUFFER_FREE, NULL,
                                   &buffer, 1, line, col);
    if (inst != SSA_INST_NONE) arena->insts[inst].effect = SSA_EFFECT_NONE;
    return inst;
}

SsaInstRef bir_add_dict_alloc(SsaArena *arena, const CobraType *dict_type,
                              const CobraType *value_type,
                              SsaValueRef capacity, uint32_t allocation_id,
                              int line, int col) {
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_DICT_ALLOC, dict_type,
                                   &capacity, 1, line, col);
    if (inst == SSA_INST_NONE) return SSA_INST_NONE;
    arena->insts[inst].memory_type = value_type;
    arena->insts[inst].pointer_contract = BIR_POINTER_CONTRACT_OWNED_SLICE;
    arena->insts[inst].pointer_origin = BIR_POINTER_ORIGIN_FRAME;
    arena->insts[inst].allocation_id = allocation_id;
    arena->insts[inst].effect = SSA_EFFECT_WRITE;
    return inst;
}

SsaInstRef bir_add_dict_set(SsaArena *arena, const CobraType *dict_type,
                            const CobraType *value_type,
                            SsaValueRef dict, SsaValueRef value,
                            const char *key, uint32_t allocation_id,
                            int line, int col) {
    const SsaValueRef operands[2] = {dict, value};
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_DICT_SET, dict_type,
                                   operands, 2, line, col);
    if (inst == SSA_INST_NONE) return SSA_INST_NONE;
    arena->insts[inst].memory_type = value_type;
    if (key) snprintf(arena->insts[inst].dict_key, sizeof(arena->insts[inst].dict_key),
                      "%s", key);
    arena->insts[inst].pointer_contract = BIR_POINTER_CONTRACT_OWNED_SLICE;
    arena->insts[inst].pointer_origin = BIR_POINTER_ORIGIN_FRAME;
    arena->insts[inst].allocation_id = allocation_id;
    arena->insts[inst].effect = SSA_EFFECT_READWRITE;
    return inst;
}

SsaInstRef bir_add_dict_get(SsaArena *arena, const CobraType *value_type,
                            SsaValueRef dict, SsaValueRef fallback,
                            const char *key, int line, int col) {
    const SsaValueRef operands[2] = {dict, fallback};
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_DICT_GET, value_type,
                                   operands, 2, line, col);
    if (inst == SSA_INST_NONE) return SSA_INST_NONE;
    if (key) snprintf(arena->insts[inst].dict_key, sizeof(arena->insts[inst].dict_key),
                      "%s", key);
    arena->insts[inst].memory_type = value_type;
    arena->insts[inst].effect = SSA_EFFECT_READ;
    return inst;
}

SsaInstRef bir_add_dict_has(SsaArena *arena, const CobraType *bool_type,
                            SsaValueRef dict, const char *key,
                            int line, int col) {
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_DICT_HAS, bool_type,
                                   &dict, 1, line, col);
    if (inst == SSA_INST_NONE) return SSA_INST_NONE;
    if (key) snprintf(arena->insts[inst].dict_key, sizeof(arena->insts[inst].dict_key),
                      "%s", key);
    arena->insts[inst].effect = SSA_EFFECT_READ;
    return inst;
}

SsaInstRef bir_add_dict_delete(SsaArena *arena, const CobraType *dict_type,
                               SsaValueRef dict, const char *key,
                               uint32_t allocation_id, int line, int col) {
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_DICT_DELETE, dict_type,
                                   &dict, 1, line, col);
    if (inst == SSA_INST_NONE) return SSA_INST_NONE;
    if (key) snprintf(arena->insts[inst].dict_key, sizeof(arena->insts[inst].dict_key),
                      "%s", key);
    arena->insts[inst].memory_type = dict_type;
    arena->insts[inst].pointer_contract = BIR_POINTER_CONTRACT_OWNED_SLICE;
    arena->insts[inst].pointer_origin = BIR_POINTER_ORIGIN_FRAME;
    arena->insts[inst].allocation_id = allocation_id;
    arena->insts[inst].effect = SSA_EFFECT_READWRITE;
    return inst;
}

SsaInstRef bir_add_dict_pop(SsaArena *arena, const CobraType *dict_type,
                            const CobraType *value_type,
                            SsaValueRef dict, SsaValueRef fallback,
                            const char *key, int line, int col) {
    const SsaValueRef operands[2] = {dict, fallback};
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_DICT_POP, value_type,
                                   operands, 2, line, col);
    if (inst == SSA_INST_NONE) return SSA_INST_NONE;
    if (key) snprintf(arena->insts[inst].dict_key, sizeof(arena->insts[inst].dict_key),
                      "%s", key);
    arena->insts[inst].memory_type = dict_type;
    arena->insts[inst].effect = SSA_EFFECT_READWRITE;
    return inst;
}

SsaInstRef bir_add_dict_len(SsaArena *arena, const CobraType *i64_type,
                            SsaValueRef dict, int line, int col) {
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_DICT_LEN, i64_type,
                                   &dict, 1, line, col);
    if (inst != SSA_INST_NONE) arena->insts[inst].effect = SSA_EFFECT_READ;
    return inst;
}

SsaInstRef bir_add_dict_free(SsaArena *arena, SsaValueRef dict,
                             int line, int col) {
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_DICT_FREE, NULL,
                                   &dict, 1, line, col);
    if (inst != SSA_INST_NONE) arena->insts[inst].effect = SSA_EFFECT_NONE;
    return inst;
}

SsaInstRef bir_add_string_concat(SsaArena *arena, const CobraType *owned_type,
                                 SsaValueRef left, SsaValueRef right,
                                 int line, int col) {
    const SsaValueRef operands[2] = {left, right};
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_STRING_CONCAT, owned_type,
                                   operands, 2, line, col);
    if (inst != SSA_INST_NONE) {
        arena->insts[inst].pointer_contract = BIR_POINTER_CONTRACT_OWNED_SLICE;
        arena->insts[inst].pointer_origin = BIR_POINTER_ORIGIN_FRAME;
    }
    return inst;
}

SsaInstRef bir_add_string_eq(SsaArena *arena, const CobraType *bool_type,
                             const CobraType *element_type,
                             SsaValueRef left, SsaValueRef right,
                             int line, int col) {
    const SsaValueRef operands[2] = {left, right};
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_STRING_EQ, bool_type,
                                   operands, 2, line, col);
    if (inst != SSA_INST_NONE) arena->insts[inst].memory_type = element_type;
    return inst;
}

SsaInstRef bir_add_sum_payload_store(SsaArena *arena, const CobraType *sum_type,
                                     const CobraType *payload_type,
                                     SsaValueRef destination, SsaValueRef payload,
                                     size_t payload_offset, int line, int col) {
    const SsaValueRef operands[2] = {destination, payload};
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_SUM_PAYLOAD_STORE, NULL,
                                   operands, 2, line, col);
    if (inst != SSA_INST_NONE) {
        arena->insts[inst].memory_type = payload_type;
        arena->insts[inst].memory_offset = (int64_t)payload_offset;
        arena->insts[inst].view_length = (int64_t)(sum_type ? sum_type->size : 0);
        arena->insts[inst].effect = SSA_EFFECT_READWRITE;
    }
    return inst;
}

SsaInstRef bir_add_sum_payload_load(SsaArena *arena, const CobraType *payload_type,
                                    const CobraType *sum_type,
                                    SsaValueRef source, size_t payload_offset,
                                    int line, int col) {
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_SUM_PAYLOAD_LOAD, payload_type,
                                   &source, 1, line, col);
    if (inst != SSA_INST_NONE) {
        arena->insts[inst].memory_type = payload_type;
        arena->insts[inst].memory_offset = (int64_t)payload_offset;
        arena->insts[inst].view_length = (int64_t)(sum_type ? sum_type->size : 0);
        arena->insts[inst].pointer_contract = BIR_POINTER_CONTRACT_OWNED_SLICE;
        arena->insts[inst].effect = SSA_EFFECT_READWRITE;
    }
    return inst;
}

SsaInstRef bir_add_sum_move(SsaArena *arena, const CobraType *sum_type,
                            SsaValueRef destination, SsaValueRef source,
                            int line, int col) {
    const SsaValueRef operands[2] = {destination, source};
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_SUM_MOVE, NULL,
                                   operands, 2, line, col);
    if (inst != SSA_INST_NONE) {
        arena->insts[inst].memory_type = sum_type;
        arena->insts[inst].memory_width = sum_type ? (uint32_t)sum_type->size : 0;
        arena->insts[inst].memory_alignment = sum_type ? (uint32_t)sum_type->alignment : 0;
        arena->insts[inst].effect = SSA_EFFECT_READWRITE;
    }
    return inst;
}

SsaInstRef bir_add_sum_drop(SsaArena *arena, const CobraType *sum_type,
                            SsaValueRef source, int line, int col) {
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_SUM_DROP, NULL,
                                   &source, 1, line, col);
    if (inst != SSA_INST_NONE) {
        arena->insts[inst].memory_type = sum_type;
        arena->insts[inst].memory_width = sum_type ? (uint32_t)sum_type->size : 0;
        arena->insts[inst].memory_alignment = sum_type ? (uint32_t)sum_type->alignment : 0;
        arena->insts[inst].effect = SSA_EFFECT_READWRITE;
    }
    return inst;
}

SsaInstRef bir_add_field_payload_store(SsaArena *arena, const CobraType *aggregate_type,
                                       const CobraType *payload_type,
                                       SsaValueRef destination, SsaValueRef payload,
                                       size_t field_offset, int line, int col) {
    const SsaValueRef operands[2] = {destination, payload};
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_FIELD_PAYLOAD_STORE, NULL,
                                   operands, 2, line, col);
    if (inst != SSA_INST_NONE) {
        arena->insts[inst].aggregate_type = aggregate_type;
        arena->insts[inst].memory_type = payload_type;
        arena->insts[inst].memory_offset = (int64_t)field_offset;
        arena->insts[inst].memory_width = payload_type ? (uint32_t)payload_type->size : 0;
        arena->insts[inst].memory_alignment = payload_type ? (uint32_t)payload_type->alignment : 0;
        arena->insts[inst].effect = SSA_EFFECT_READWRITE;
    }
    return inst;
}

SsaInstRef bir_add_field_payload_load(SsaArena *arena, const CobraType *aggregate_type,
                                      const CobraType *payload_type,
                                      SsaValueRef source, size_t field_offset,
                                      int line, int col) {
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_FIELD_PAYLOAD_LOAD, payload_type,
                                   &source, 1, line, col);
    if (inst != SSA_INST_NONE) {
        arena->insts[inst].aggregate_type = aggregate_type;
        arena->insts[inst].memory_type = payload_type;
        arena->insts[inst].memory_offset = (int64_t)field_offset;
        arena->insts[inst].memory_width = payload_type ? (uint32_t)payload_type->size : 0;
        arena->insts[inst].memory_alignment = payload_type ? (uint32_t)payload_type->alignment : 0;
        arena->insts[inst].pointer_contract = BIR_POINTER_CONTRACT_OWNED_SLICE;
        arena->insts[inst].effect = SSA_EFFECT_READWRITE;
    }
    return inst;
}

SsaInstRef bir_add_aggregate_move(SsaArena *arena, const CobraType *aggregate_type,
                                  SsaValueRef destination, SsaValueRef source,
                                  int line, int col) {
    const SsaValueRef operands[2] = {destination, source};
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_AGG_MOVE, NULL,
                                   operands, 2, line, col);
    if (inst != SSA_INST_NONE) {
        arena->insts[inst].aggregate_type = aggregate_type;
        arena->insts[inst].memory_type = aggregate_type;
        arena->insts[inst].memory_width = aggregate_type ? (uint32_t)aggregate_type->size : 0;
        arena->insts[inst].memory_alignment = aggregate_type ? (uint32_t)aggregate_type->alignment : 0;
        arena->insts[inst].effect = SSA_EFFECT_READWRITE;
    }
    return inst;
}

SsaInstRef bir_add_aggregate_drop(SsaArena *arena, const CobraType *aggregate_type,
                                  SsaValueRef source, int line, int col) {
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_AGG_DROP, NULL,
                                   &source, 1, line, col);
    if (inst != SSA_INST_NONE) {
        arena->insts[inst].aggregate_type = aggregate_type;
        arena->insts[inst].memory_type = aggregate_type;
        arena->insts[inst].memory_width = aggregate_type ? (uint32_t)aggregate_type->size : 0;
        arena->insts[inst].memory_alignment = aggregate_type ? (uint32_t)aggregate_type->alignment : 0;
        arena->insts[inst].effect = SSA_EFFECT_READWRITE;
    }
    return inst;
}

SsaInstRef bir_add_sum_check(SsaArena *arena, SsaValueRef tag,
                             int check_kind, int line, int col) {
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_SUM_CHECK, NULL, &tag, 1, line, col);
    if (inst != SSA_INST_NONE) arena->insts[inst].sum_check_kind = check_kind;
    return inst;
}

bool bir_call_arg_type_compatible(const CobraType *actual,
                                  const CobraType *expected) {
    if (actual && expected &&
        (actual == expected || cobra_type_equal(actual, expected))) return true;
    if (!bir_is_borrowed_view_type(expected))
        return false;
    if (!bir_is_owned_slice_type(actual) && (!actual || actual->kind != COBRA_TYPE_ARRAY))
        return false;
    const CobraType *actual_element = cobra_type_element(actual);
    const CobraType *expected_element = cobra_type_element(expected);
    return actual_element && expected_element &&
           cobra_type_equal(actual_element, expected_element);
}

bool bir_is_owned_buffer_type(const CobraType *type) {
    return type && type->kind == COBRA_TYPE_LIST && type->finalized &&
           type->ownership == COBRA_OWNERSHIP_OWNED &&
           type->mutability == COBRA_MUTABILITY_MUTABLE &&
           type->region_id == -1 && type->generic_arg_count == 1 &&
           (cobra_type_is_scalar(type->generic_args[0]) ||
            bir_type_is_value_only_struct(type->generic_args[0]));
}

bool bir_is_owned_slice_type(const CobraType *type) {
    return (type && cobra_type_is_slice_kind(type->kind) && type->finalized &&
            type->ownership == COBRA_OWNERSHIP_OWNED && type->region_id == -1 &&
            type->generic_arg_count == 1 && cobra_type_is_scalar(type->generic_args[0])) ||
           bir_is_owned_buffer_type(type);
}

static bool bir_dict_key_is_string(const CobraType *key) {
    /* The production parser keys dicts by string. The backend imports the
       source string type as a borrowed readonly u8 view. */
    return key && (key->kind == COBRA_TYPE_STRING ||
                   (key->kind == COBRA_TYPE_SLICE_U8 &&
                    key->ownership == COBRA_OWNERSHIP_BORROWED));
}

bool bir_is_owned_dict_type(const CobraType *type) {
    /* An owned string-key scalar-value map, matching the production
       `dict[string]T` contract. */
    return type && type->kind == COBRA_TYPE_DICT && type->finalized &&
           type->ownership == COBRA_OWNERSHIP_OWNED &&
           type->mutability == COBRA_MUTABILITY_MUTABLE &&
           type->region_id == -1 && type->generic_arg_count == 2 &&
           bir_dict_key_is_string(type->generic_args[0]) &&
           type->generic_args[1] && cobra_type_is_scalar(type->generic_args[1]);
}

const CobraType *bir_dict_type(BackendIrModule *module,
                               const CobraType *value_type) {
    /* Owned string-key dicts admit scalar values only. Owning or
       view-bearing values stay deferred until their destruction and growth
       contracts are defined. */
    if (!module || !module->type_arena || !value_type ||
        !cobra_type_is_scalar(value_type)) return NULL;
    for (size_t i = 0; i < module->type_arena->count; i++) {
        CobraType *candidate = &module->type_arena->nodes[i];
        if (bir_is_owned_dict_type(candidate) &&
            candidate->generic_args[1] == value_type) return candidate;
    }
    const CobraType *string_type = bir_view_type(module, module->type_u8);
    if (!string_type) return NULL;
    CobraType *dict = cobra_type_make(module->type_arena, COBRA_TYPE_DICT, NULL,
                                      NULL, NULL, string_type, value_type,
                                      COBRA_OWNERSHIP_OWNED,
                                      COBRA_MUTABILITY_MUTABLE, -1);
    if (!dict || !cobra_type_finalize(module->type_arena, dict)) return NULL;
    return dict;
}

static const CobraType *bir_make_owned_slice_type(BackendIrModule *module,
                                                   const CobraType *element_type) {
    if (!module || !module->type_arena || !element_type ||
        !cobra_type_is_scalar(element_type)) return NULL;
    CobraTypeKind kind = element_type->kind == COBRA_TYPE_F32 ? COBRA_TYPE_SLICE_F32 :
                         element_type->kind == COBRA_TYPE_U8 ? COBRA_TYPE_SLICE_U8 :
                         element_type->kind == COBRA_TYPE_I64 ? COBRA_TYPE_SLICE :
                         COBRA_TYPE_UNKNOWN;
    if (kind == COBRA_TYPE_UNKNOWN) return NULL;
    for (size_t i = 0; i < module->type_arena->count; i++) {
        CobraType *candidate = &module->type_arena->nodes[i];
        if (candidate->kind == kind && candidate->generic_arg_count == 1 &&
            candidate->generic_args[0] == element_type &&
            candidate->ownership == COBRA_OWNERSHIP_OWNED &&
            candidate->region_id == -1) return candidate;
    }
    CobraType *slice = cobra_type_make(module->type_arena, kind, NULL,
                                       element_type, NULL, NULL, NULL,
                                       COBRA_OWNERSHIP_OWNED,
                                       COBRA_MUTABILITY_MUTABLE, -1);
    if (!slice || !cobra_type_finalize(module->type_arena, slice)) return NULL;
    return slice;
}

const CobraType *bir_owned_slice_type(BackendIrModule *module,
                                      const CobraType *element_type) {
    return bir_make_owned_slice_type(module, element_type);
}

const CobraType *bir_buffer_type(BackendIrModule *module,
                                 const CobraType *element_type) {
    /* Buffer elements are scalars or value-only structs. Owning or
       view-bearing elements stay deferred until their destruction and
       growth contracts are defined. */
    bool element_ok = element_type &&
        (cobra_type_is_scalar(element_type) ||
         bir_type_is_value_only_struct(element_type));
    if (!module || !module->type_arena || !element_ok) return NULL;
    for (size_t i = 0; i < module->type_arena->count; i++) {
        CobraType *candidate = &module->type_arena->nodes[i];
        if (bir_is_owned_buffer_type(candidate) &&
            candidate->generic_args[0] == element_type) return candidate;
    }
    CobraType *buffer = cobra_type_make(module->type_arena, COBRA_TYPE_LIST, NULL,
                                        element_type, NULL, NULL, NULL,
                                        COBRA_OWNERSHIP_OWNED,
                                        COBRA_MUTABILITY_MUTABLE, -1);
    if (!buffer || !cobra_type_finalize(module->type_arena, buffer)) return NULL;
    return buffer;
}

const CobraType *bir_array_type(BackendIrModule *module,
                                const CobraType *element_type, size_t length) {
    /* Value-owned array elements: scalars, nested fixed arrays, and
       value-only structs. Owned or borrowed elements (slices, strings,
       sums, view-bearing structs) stay deferred until their ownership and
       literal-construction rules are defined. */
    bool element_ok = element_type &&
        (cobra_type_is_scalar(element_type) ||
         element_type->kind == COBRA_TYPE_ARRAY ||
         bir_type_is_value_only_struct(element_type));
    if (!module || !module->type_arena || !element_ok || length == 0 ||
        length > COBRA_MAX_ARRAY_ELEMENTS) return NULL;
    for (size_t i = 0; i < module->type_arena->count; i++) {
        CobraType *candidate = &module->type_arena->nodes[i];
        if (candidate->kind == COBRA_TYPE_ARRAY &&
            candidate->generic_arg_count == 1 &&
            candidate->generic_args[0] == element_type &&
            candidate->array_length == length &&
            candidate->ownership == COBRA_OWNERSHIP_VALUE &&
            candidate->mutability == COBRA_MUTABILITY_DEFAULT &&
            candidate->region_id == -1) return candidate;
    }
    CobraType *array = cobra_type_make(module->type_arena, COBRA_TYPE_ARRAY, NULL,
                                       element_type, NULL, NULL, NULL,
                                       COBRA_OWNERSHIP_VALUE,
                                       COBRA_MUTABILITY_DEFAULT, -1);
    if (!array) return NULL;
    array->array_length = length;
    if (!cobra_type_finalize(module->type_arena, array)) return NULL;
    return array;
}

bool bir_is_sum_type(const CobraType *type);

const CobraType *bir_sum_type(BackendIrModule *module, CobraTypeKind kind,
                              const CobraType *element, const CobraType *error) {
    if (!module || !module->type_arena || !element ||
        (kind != COBRA_TYPE_OPTION && kind != COBRA_TYPE_RESULT) ||
        (!cobra_type_is_scalar(element) && !bir_is_sum_type(element) &&
         !bir_is_owned_slice_type(element) &&
         !bir_type_is_value_only_struct(element) &&
         element->kind != COBRA_TYPE_STRUCT)) return NULL;
    if (kind == COBRA_TYPE_RESULT &&
        (!error || (!cobra_type_is_scalar(error) && !bir_is_sum_type(error) &&
                    !bir_is_owned_slice_type(error) &&
                    !bir_type_is_value_only_struct(error) &&
                    error->kind != COBRA_TYPE_STRUCT))) return NULL;
    for (size_t i = 0; i < module->type_arena->count; i++) {
        CobraType *candidate = &module->type_arena->nodes[i];
        if (candidate->kind == kind && candidate->generic_arg_count ==
                (kind == COBRA_TYPE_RESULT ? 2 : 1) &&
            candidate->generic_args[0] == element &&
            (kind == COBRA_TYPE_OPTION || candidate->generic_args[1] == error) &&
            candidate->ownership == COBRA_OWNERSHIP_VALUE &&
            candidate->mutability == COBRA_MUTABILITY_DEFAULT &&
            candidate->region_id == -1) return candidate;
    }
    CobraType *sum = cobra_type_make(module->type_arena, kind, NULL,
                                     element, error, NULL, NULL,
                                     COBRA_OWNERSHIP_VALUE,
                                     COBRA_MUTABILITY_DEFAULT, -1);
    if (!sum || !cobra_type_finalize(module->type_arena, sum)) return NULL;
    return sum;
}

const BirEnumInfo *bir_find_enum(const BackendIrModule *module,
                                 const char *name) {
    if (!module || !name) return NULL;
    for (size_t i = 0; i < module->enum_count; i++) {
        if (strcmp(module->enums[i].name, name) == 0) return &module->enums[i];
    }
    return NULL;
}

int bir_enum_variant_value(const BackendIrModule *module,
                           const char *enum_name, const char *variant_name) {
    const BirEnumInfo *info = bir_find_enum(module, enum_name);
    if (!info || !variant_name) return INT_MIN;
    for (size_t i = 0; i < info->variant_count; i++) {
        if (strcmp(info->variant_names[i], variant_name) == 0)
            return info->variant_values[i];
    }
    return INT_MIN;
}

const CobraType *bir_enum_type(BackendIrModule *module, const char *name) {
    if (!module || !module->type_arena || !name || !name[0]) return NULL;
    /* Registered enums always resolve through the registry so a payload
       descriptor (with generic arguments) is never shadowed by an interned
       unit node created during reference resolution. */
    const BirEnumInfo *registered = bir_find_enum(module, name);
    if (registered && registered->type) return registered->type;
    for (size_t i = 0; i < module->type_arena->count; i++) {
        CobraType *candidate = &module->type_arena->nodes[i];
        if (candidate->kind == COBRA_TYPE_ENUM &&
            strcmp(candidate->name, name) == 0 &&
            candidate->ownership == COBRA_OWNERSHIP_VALUE &&
            candidate->mutability == COBRA_MUTABILITY_DEFAULT &&
            candidate->region_id == -1) return candidate;
    }
    CobraType *enum_type = cobra_type_make(module->type_arena, COBRA_TYPE_ENUM,
                                           name, NULL, NULL, NULL, NULL,
                                           COBRA_OWNERSHIP_VALUE,
                                           COBRA_MUTABILITY_DEFAULT, -1);
    if (!enum_type || !cobra_type_finalize(module->type_arena, enum_type))
        return NULL;
    return enum_type;
}

bool bir_is_sum_type(const CobraType *type) {
    /* Payload-carrying enums share the tagged-sum layout and ride the same
       aggregate machinery. Unit enums stay integer-backed scalars. */
    return type && (type->kind == COBRA_TYPE_OPTION ||
                    type->kind == COBRA_TYPE_RESULT ||
                    (type->kind == COBRA_TYPE_ENUM &&
                     type->generic_arg_count > 0));
}

bool bir_is_payload_enum_type(const CobraType *type) {
    return type && type->kind == COBRA_TYPE_ENUM &&
           type->generic_arg_count > 0;
}

bool bir_type_has_owned_payload(const CobraType *type) {
    if (!type) return false;
    if (bir_is_owned_slice_type(type)) return true;
    if (bir_is_sum_type(type)) {
        for (size_t i = 0; i < type->generic_arg_count; i++) {
            if (bir_type_has_owned_payload(type->generic_args[i])) return true;
        }
        return false;
    }
    if (type->kind == COBRA_TYPE_STRUCT) {
        for (size_t i = 0; i < type->field_count; i++) {
            if (bir_type_has_owned_payload(type->fields[i].type)) return true;
        }
    }
    return false;
}

bool bir_sum_has_owned_payload(const CobraType *type) {
    return bir_is_sum_type(type) && bir_type_has_owned_payload(type);
}

/* A value-only struct is one whose fields are scalars, fixed arrays of
   value-only elements, or nested value-only structs. It carries no owned
   strings, slices, sums, or borrowed views, so it is safe to copy by value
   as a fixed-array element. */
bool bir_type_is_value_only_struct(const CobraType *type) {
    if (!type || type->kind != COBRA_TYPE_STRUCT) return false;
    if (bir_type_has_owned_payload(type)) return false;
    for (size_t i = 0; i < type->field_count; i++) {
        const CobraType *field = type->fields[i].type;
        if (!field) return false;
        if (bir_is_borrowed_view_type(field)) return false;
        if (field->kind == COBRA_TYPE_ARRAY) {
            const CobraType *element = field->generic_arg_count == 1
                ? field->generic_args[0] : NULL;
            if (!element ||
                (!cobra_type_is_scalar(element) &&
                 element->kind != COBRA_TYPE_ARRAY &&
                 !bir_type_is_value_only_struct(element))) return false;
        } else if (field->kind == COBRA_TYPE_STRUCT) {
            if (!bir_type_is_value_only_struct(field)) return false;
        } else if (!cobra_type_is_scalar(field)) {
            return false;
        }
    }
    return true;
}

/* The resident slot width of a sum component. Scalar components occupy the
   fixed COBRA_NATIVE_SUM_SCALAR_SIZE slot; aggregates occupy their real
   canonical size. This must match finalize_type's layout exactly so
   bir_sum_component_offset stays consistent with allocation. */
size_t bir_sum_component_slot_size(const CobraType *component) {
    if (!component) return 0;
    if (component->kind == COBRA_TYPE_STRUCT ||
        component->kind == COBRA_TYPE_OPTION ||
        component->kind == COBRA_TYPE_RESULT ||
        cobra_type_is_slice_kind(component->kind) ||
        component->kind == COBRA_TYPE_LIST ||
        component->kind == COBRA_TYPE_DICT ||
        (component->kind == COBRA_TYPE_ENUM &&
         component->generic_arg_count > 0)) return component->size;
    return COBRA_NATIVE_SUM_SCALAR_SIZE;
}

size_t bir_sum_component_offset(const CobraType *sum, int selector) {
    if (!bir_is_sum_type(sum)) return 0;
    if (selector == 0) return 0;
    size_t payload_offset = COBRA_NATIVE_SUM_TAG_SIZE;
    /* Selector k (1-based) addresses the k-th component slot; every variant
       payload is resident, so offsets are the sum of earlier slot widths.
       This covers Option/Result and N-variant payload enums uniformly. */
    for (int i = 1; i < selector && (size_t)i <= sum->generic_arg_count; i++) {
        payload_offset +=
            bir_sum_component_slot_size(sum->generic_args[i - 1]);
    }
    return payload_offset;
}

bool bir_is_borrowed_view_type(const CobraType *type) {
    return type && cobra_type_is_slice_kind(type->kind) && type->finalized &&
           type->ownership == COBRA_OWNERSHIP_BORROWED &&
           (type->mutability == COBRA_MUTABILITY_READONLY ||
            type->mutability == COBRA_MUTABILITY_OUT) &&
           type->region_id == -1 && type->generic_arg_count == 1 &&
           cobra_type_is_scalar(type->generic_args[0]);
}

bool bir_view_is_writable(const CobraType *type) {
    return bir_is_borrowed_view_type(type) &&
           type->mutability == COBRA_MUTABILITY_OUT;
}

static const CobraType *bir_make_view_type(BackendIrModule *module,
                                            const CobraType *element_type,
                                            CobraMutabilityKind mutability) {
    if (!module || !module->type_arena || !element_type ||
        !cobra_type_is_scalar(element_type)) return NULL;
    CobraTypeKind kind = element_type->kind == COBRA_TYPE_F32 ? COBRA_TYPE_SLICE_F32 :
                         element_type->kind == COBRA_TYPE_U8 ? COBRA_TYPE_SLICE_U8 :
                         element_type->kind == COBRA_TYPE_I64 ? COBRA_TYPE_SLICE :
                         COBRA_TYPE_UNKNOWN;
    if (kind == COBRA_TYPE_UNKNOWN) return NULL;
    for (size_t i = 0; i < module->type_arena->count; i++) {
        CobraType *candidate = &module->type_arena->nodes[i];
        if (candidate->kind == kind && candidate->generic_arg_count == 1 &&
            candidate->generic_args[0] == element_type &&
            candidate->ownership == COBRA_OWNERSHIP_BORROWED &&
            candidate->mutability == mutability &&
            candidate->region_id == -1) return candidate;
    }
    CobraType *view = cobra_type_make(module->type_arena, kind, NULL,
                                      element_type, NULL, NULL, NULL,
                                      COBRA_OWNERSHIP_BORROWED,
                                      mutability, -1);
    if (!view || !cobra_type_finalize(module->type_arena, view)) return NULL;
    return view;
}

const CobraType *bir_view_type(BackendIrModule *module, const CobraType *element_type) {
    return bir_make_view_type(module, element_type, COBRA_MUTABILITY_READONLY);
}

const CobraType *bir_writable_view_type(BackendIrModule *module,
                                        const CobraType *element_type) {
    return bir_make_view_type(module, element_type, COBRA_MUTABILITY_OUT);
}

SsaInstRef bir_add_ptr_add(SsaArena *arena, const CobraType *pointer_type,
                           SsaValueRef pointer, SsaValueRef offset,
                           int line, int col) {
    const SsaValueRef operands[2] = {pointer, offset};
    (void)pointer_type;
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_PTR_ADD, pointer_type,
                                   operands, 2, line, col);
    if (inst != SSA_INST_NONE && pointer < arena->value_count) {
        arena->insts[inst].pointer_contract = arena->values[pointer].pointer_contract;
        arena->insts[inst].pointer_origin = arena->values[pointer].pointer_origin;
        arena->insts[inst].region_id = arena->values[pointer].region_id;
        arena->insts[inst].allocation_id = arena->values[pointer].allocation_id;
    }
    return inst;
}

SsaInstRef bir_add_field_addr(SsaArena *arena, const CobraType *pointer_type,
                              const CobraType *aggregate_type, const CobraType *field_type,
                              SsaValueRef base, size_t field_offset,
                              int line, int col) {
    if (!arena || !pointer_type || !aggregate_type || !field_type) return SSA_INST_NONE;
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_FIELD_ADDR, pointer_type,
                                   &base, 1, line, col);
    if (inst == SSA_INST_NONE) return SSA_INST_NONE;
    arena->insts[inst].memory_type = aggregate_type;
    arena->insts[inst].memory_offset = (int64_t)field_offset;
    arena->insts[inst].memory_width = (uint32_t)field_type->size;
    arena->insts[inst].memory_alignment = (uint32_t)field_type->alignment;
    arena->insts[inst].address_kind = SSA_ADDRESS_TYPED_POINTER;
    arena->insts[inst].address_space = 0;
    arena->insts[inst].pointer_contract =
        base < arena->value_count ? arena->values[base].pointer_contract
                                  : BIR_POINTER_CONTRACT_UNKNOWN;
    arena->insts[inst].pointer_origin =
        base < arena->value_count ? arena->values[base].pointer_origin
                                  : BIR_POINTER_ORIGIN_UNKNOWN;
    arena->insts[inst].region_id = base < arena->value_count
        ? arena->values[base].region_id : 0;
    arena->insts[inst].allocation_id = base < arena->value_count
        ? arena->values[base].allocation_id : 0;
    return inst;
}

SsaInstRef bir_add_array_index_addr(SsaArena *arena, const CobraType *pointer_type,
                                    const CobraType *array_type, const CobraType *element_type,
                                    SsaValueRef base, SsaValueRef index,
                                    int line, int col) {
    const SsaValueRef operands[2] = {base, index};
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_ARRAY_INDEX_ADDR, pointer_type,
                                   operands, 2, line, col);
    if (inst == SSA_INST_NONE) return SSA_INST_NONE;
    arena->insts[inst].memory_type = array_type;
    arena->insts[inst].memory_width = element_type ? (uint32_t)element_type->size : 0;
    arena->insts[inst].memory_alignment = element_type ? (uint32_t)element_type->alignment : 0;
    arena->insts[inst].address_kind = SSA_ADDRESS_TYPED_POINTER;
    arena->insts[inst].address_space = 0;
    arena->insts[inst].pointer_contract = base < arena->value_count
        ? arena->values[base].pointer_contract : BIR_POINTER_CONTRACT_UNKNOWN;
    arena->insts[inst].pointer_origin = base < arena->value_count
        ? arena->values[base].pointer_origin : BIR_POINTER_ORIGIN_UNKNOWN;
    arena->insts[inst].region_id = base < arena->value_count
        ? arena->values[base].region_id : BIR_REGION_NONE;
    arena->insts[inst].allocation_id = base < arena->value_count
        ? arena->values[base].allocation_id : 0;
    return inst;
}

SsaInstRef bir_add_aggregate_copy(SsaArena *arena, const CobraType *aggregate_type,
                                  SsaValueRef destination, SsaValueRef source,
                                  int line, int col) {
    if (!arena || !aggregate_type) return SSA_INST_NONE;
    const SsaValueRef operands[2] = {destination, source};
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_AGG_COPY, NULL,
                                   operands, 2, line, col);
    if (inst == SSA_INST_NONE) return SSA_INST_NONE;
    arena->insts[inst].memory_type = aggregate_type;
    arena->insts[inst].memory_width = (uint32_t)aggregate_type->size;
    arena->insts[inst].memory_alignment = (uint32_t)aggregate_type->alignment;
    arena->insts[inst].address_kind = SSA_ADDRESS_TYPED_POINTER;
    arena->insts[inst].address_space = 0;
    arena->insts[inst].effect = SSA_EFFECT_READWRITE;
    return inst;
}

SsaInstRef bir_add_typed_load(SsaArena *arena, const CobraType *value_type,
                              const CobraType *pointer_type, SsaValueRef pointer,
                              uint32_t width, uint32_t alignment, int line, int col) {
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_LOAD, value_type,
                                   &pointer, 1, line, col);
    if (inst == SSA_INST_NONE) return SSA_INST_NONE;
    arena->insts[inst].memory_type = value_type;
    arena->insts[inst].memory_width = width;
    arena->insts[inst].memory_alignment = alignment;
    arena->insts[inst].address_kind = SSA_ADDRESS_TYPED_POINTER;
    arena->insts[inst].address_space = 0;
    if (bir_is_borrowed_view_type(value_type)) {
        arena->insts[inst].pointer_contract = bir_view_is_writable(value_type)
            ? BIR_POINTER_CONTRACT_BORROW_WRITE
            : BIR_POINTER_CONTRACT_BORROW_READONLY;
        arena->insts[inst].pointer_origin = BIR_POINTER_ORIGIN_FRAME;
        arena->insts[inst].region_id = BIR_REGION_NONE;
    }
    (void)pointer_type;
    arena->insts[inst].effect = SSA_EFFECT_READ;
    return inst;
}

SsaInstRef bir_add_typed_store(SsaArena *arena, const CobraType *value_type,
                               const CobraType *pointer_type, SsaValueRef pointer,
                               SsaValueRef value, uint32_t width,
                               uint32_t alignment, int line, int col) {
    const SsaValueRef operands[2] = {pointer, value};
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_STORE, NULL,
                                   operands, 2, line, col);
    if (inst == SSA_INST_NONE) return SSA_INST_NONE;
    arena->insts[inst].memory_type = value_type;
    arena->insts[inst].memory_width = width;
    arena->insts[inst].memory_alignment = alignment;
    arena->insts[inst].address_kind = SSA_ADDRESS_TYPED_POINTER;
    arena->insts[inst].address_space = 0;
    (void)pointer_type;
    arena->insts[inst].effect = SSA_EFFECT_WRITE;
    return inst;
}

SsaValueRef bir_inst_result(SsaArena *arena, SsaInstRef inst, int line, int col) {
    if (!arena || inst == SSA_INST_NONE || inst >= arena->inst_count) return SSA_VALUE_NONE;
    SsaInst *source = &arena->insts[inst];
    if (!bir_op_has_result(source->op)) return SSA_VALUE_NONE;
    SsaValueRef ref = bir_add_value(arena, SSA_VALUE_INST, source->type, line, col);
    if (ref == SSA_VALUE_NONE) return SSA_VALUE_NONE;
    arena->values[ref].def_inst = inst;
    arena->values[ref].pointer_contract = source->pointer_contract;
    arena->values[ref].pointer_origin = source->pointer_origin;
    arena->values[ref].region_id = source->region_id;
    arena->values[ref].allocation_id = source->allocation_id;
    source->result = ref;
    return ref;
}

bool bir_block_add_inst(SsaArena *arena, SsaBlockRef block, SsaInstRef inst) {
    if (!arena || block == SSA_BLOCK_NONE || block >= arena->block_count ||
        inst == SSA_INST_NONE || inst >= arena->inst_count)
        return false;
    SsaBlock *target = &arena->blocks[block];
    if (target->terminator != SSA_INST_NONE) return false; /* nothing after a terminator */
    SsaInstRef *grown = grow_pool(target->insts, sizeof(SsaInstRef), &target->inst_cap,
                                  target->inst_count + 1);
    if (!grown) return false;
    target->insts = grown;
    target->insts[target->inst_count++] = inst;
    return true;
}

bool bir_set_terminator(SsaArena *arena, SsaBlockRef block, SsaInstRef term) {
    if (!arena || block == SSA_BLOCK_NONE || block >= arena->block_count ||
        term == SSA_INST_NONE || term >= arena->inst_count)
        return false;
    SsaBlock *target = &arena->blocks[block];
    if (!bir_is_terminator(arena->insts[term].op)) return false;
    if (!bir_block_add_inst(arena, block, term)) return false;
    target->terminator = term;
    return true;
}

SsaValueRef bir_add_block_param(SsaArena *arena, SsaBlockRef block,
                                const CobraType *type, int line, int col) {
    if (!arena || block == SSA_BLOCK_NONE || block >= arena->block_count)
        return SSA_VALUE_NONE;
    SsaBlock *target = &arena->blocks[block];
    SsaValueRef *grown = grow_pool(target->params, sizeof(SsaValueRef), &target->param_cap,
                                   target->param_count + 1);
    if (!grown) return SSA_VALUE_NONE;
    target->params = grown;
    SsaValueRef ref = bir_add_value(arena, SSA_VALUE_BLOCK_PARAM, type, line, col);
    if (ref == SSA_VALUE_NONE) return SSA_VALUE_NONE;
    arena->values[ref].block = block;
    target->params[target->param_count++] = ref;
    return ref;
}

bool bir_add_edge(SsaArena *arena, SsaBlockRef pred, SsaBlockRef succ) {
    if (!arena || pred == SSA_BLOCK_NONE || pred >= arena->block_count ||
        succ == SSA_BLOCK_NONE || succ >= arena->block_count)
        return false;
    SsaBlock *from = &arena->blocks[pred];
    SsaBlock *to = &arena->blocks[succ];
    SsaBlockRef *succ_grown = grow_pool(from->succs, sizeof(SsaBlockRef),
                                        &from->succ_cap, from->succ_count + 1);
    if (!succ_grown) return false;
    from->succs = succ_grown;
    SsaBlockRef *pred_grown = grow_pool(to->preds, sizeof(SsaBlockRef),
                                        &to->pred_cap, to->pred_count + 1);
    if (!pred_grown) return false;
    to->preds = pred_grown;
    from->succs[from->succ_count++] = succ;
    to->preds[to->pred_count++] = pred;
    return true;
}

static bool append_edge_args(SsaArena *arena, SsaInst *inst, bool second,
                             const SsaValueRef *args, size_t count) {
    if (!reserve_edges(arena, count)) return false;
    uint32_t start = (uint32_t)arena->edge_used;
    for (size_t i = 0; i < count; i++) arena->edges[arena->edge_used++] = args[i];
    if (second) {
        inst->edge2_start = start;
        inst->edge2_count = (uint32_t)count;
    } else {
        inst->edge_start = start;
        inst->edge_count = (uint32_t)count;
    }
    return true;
}

bool bir_set_jump(SsaArena *arena, SsaBlockRef block, SsaBlockRef target,
                  const SsaValueRef *edge_args, size_t edge_count, int line, int col) {
    if (!arena || block == SSA_BLOCK_NONE || block >= arena->block_count ||
        target == SSA_BLOCK_NONE || target >= arena->block_count)
        return false;
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_JUMP, NULL, NULL, 0, line, col);
    if (inst == SSA_INST_NONE) return false;
    SsaInst *jump = &arena->insts[inst];
    jump->target = target;
    if (!append_edge_args(arena, jump, false, edge_args, edge_count)) return false;
    return bir_set_terminator(arena, block, inst);
}

bool bir_set_branch(SsaArena *arena, SsaBlockRef block, SsaValueRef cond,
                    SsaBlockRef then_block, SsaBlockRef else_block,
                    const SsaValueRef *then_args, size_t then_count,
                    const SsaValueRef *else_args, size_t else_count,
                    int line, int col) {
    if (!arena || block == SSA_BLOCK_NONE || block >= arena->block_count ||
        cond == SSA_VALUE_NONE || cond >= arena->value_count ||
        then_block == SSA_BLOCK_NONE || then_block >= arena->block_count ||
        else_block == SSA_BLOCK_NONE || else_block >= arena->block_count)
        return false;
    const SsaValueRef operands[1] = {cond};
    SsaInstRef inst = bir_add_inst(arena, SSA_OP_BRANCH, NULL, operands, 1, line, col);
    if (inst == SSA_INST_NONE) return false;
    SsaInst *branch = &arena->insts[inst];
    branch->target = then_block;
    branch->target2 = else_block;
    if (!append_edge_args(arena, branch, false, then_args, then_count) ||
        !append_edge_args(arena, branch, true, else_args, else_count))
        return false;
    return bir_set_terminator(arena, block, inst);
}

bool bir_set_return(SsaArena *arena, SsaBlockRef block, SsaValueRef value,
                    int line, int col) {
    if (!arena || block == SSA_BLOCK_NONE || block >= arena->block_count)
        return false;
    SsaInstRef inst;
    if (value != SSA_VALUE_NONE) {
        const SsaValueRef operands[1] = {value};
        inst = bir_add_inst(arena, SSA_OP_RETURN, NULL, operands, 1, line, col);
    } else {
        inst = bir_add_inst(arena, SSA_OP_RETURN, NULL, NULL, 0, line, col);
    }
    if (inst == SSA_INST_NONE) return false;
    return bir_set_terminator(arena, block, inst);
}

typedef struct {
    uint16_t gpr_count;
    uint16_t xmm_count;
    uint32_t stack_size;
} BirAbiCursor;

static uint32_t bir_abi_align_up(uint32_t value, uint32_t alignment) {
    if (alignment <= 1) return value;
    uint32_t remainder = value % alignment;
    return remainder ? value + alignment - remainder : value;
}

static BirAbiRegisterClass bir_abi_register_class(const CobraType *type) {
    return type && type->abi == COBRA_ABI_XMM
        ? BIR_ABI_REGISTER_XMM : BIR_ABI_REGISTER_GPR;
}

static void bir_abi_location_init(BirAbiLocation *location,
                                  BirAbiPassMode pass_mode,
                                  BirAbiRegisterClass register_class,
                                  uint32_t size, uint32_t alignment) {
    memset(location, 0, sizeof(*location));
    location->storage = BIR_ABI_STORAGE_NONE;
    location->pass_mode = pass_mode;
    location->register_class = register_class;
    location->size = size;
    location->alignment = alignment ? alignment : 1;
}

static bool bir_abi_place_part(BirAbiLocation *location, BirAbiCursor *cursor) {
    if (location->register_class == BIR_ABI_REGISTER_XMM &&
        cursor->xmm_count < BIR_ABI_MAX_XMM_ARGUMENT_REGISTERS) {
        location->storage = BIR_ABI_STORAGE_REGISTER;
        location->register_index = cursor->xmm_count++;
        return true;
    }
    if (location->register_class == BIR_ABI_REGISTER_GPR &&
        cursor->gpr_count < BIR_ABI_MAX_GPR_ARGUMENT_REGISTERS) {
        location->storage = BIR_ABI_STORAGE_REGISTER;
        location->register_index = cursor->gpr_count++;
        return true;
    }
    uint32_t slot_size = location->size < 8 ? 8 : location->size;
    uint32_t slot_alignment = location->alignment < 8 ? 8 : location->alignment;
    cursor->stack_size = bir_abi_align_up(cursor->stack_size, slot_alignment);
    location->storage = BIR_ABI_STORAGE_STACK;
    location->stack_offset = cursor->stack_size;
    cursor->stack_size += slot_size;
    return true;
}

static bool bir_abi_is_view(const CobraType *type) {
    return type && (bir_is_borrowed_view_type(type) ||
                    bir_is_owned_dict_type(type) ||
                    (type->abi == COBRA_ABI_SLICE && !bir_is_owned_buffer_type(type)));
}

static bool bir_abi_fill_value(BirAbiValueLocations *locations,
                               const CobraType *type, bool indirect,
                               BirAbiCursor *cursor) {
    if (!locations || !type) return false;
    memset(locations, 0, sizeof(*locations));
    BirAbiRegisterClass register_class = indirect
        ? BIR_ABI_REGISTER_GPR : bir_abi_register_class(type);
    uint32_t size = indirect ? 8U : (uint32_t)(type->size ? type->size : 8U);
    uint32_t alignment = indirect ? 8U : (uint32_t)(type->alignment ? type->alignment : 8U);
    size_t part_count = !indirect && bir_is_owned_buffer_type(type) ? 3 :
                        bir_abi_is_view(type) && !indirect ? 2 : 1;
    if (part_count > BIR_ABI_MAX_PARTS) return false;
    for (size_t part = 0; part < part_count; part++) {
        BirAbiLocation *location = &locations->parts[part];
        bir_abi_location_init(location,
                              indirect ? BIR_ABI_PASS_INDIRECT : BIR_ABI_PASS_DIRECT,
                              register_class,
                              part_count > 1 ? 8U : size,
                              part_count > 1 ? 8U : alignment);
        if (!bir_abi_place_part(location, cursor)) return false;
    }
    locations->count = (uint8_t)part_count;
    return true;
}

static bool bir_abi_fill_return(BirAbiValueLocations *locations,
                                const CobraType *type, bool indirect) {
    if (!locations || !type) return false;
    memset(locations, 0, sizeof(*locations));
    if (type->kind == COBRA_TYPE_VOID) return true;
    bool buffer = !indirect && bir_is_owned_buffer_type(type);
    bool view = bir_abi_is_view(type) && !indirect;
    size_t part_count = buffer ? 3 : view ? 2 : 1;
    if (part_count > BIR_ABI_MAX_PARTS) return false;
    for (size_t part = 0; part < part_count; part++) {
        BirAbiLocation *location = &locations->parts[part];
        bir_abi_location_init(location,
                              indirect ? BIR_ABI_PASS_INDIRECT : BIR_ABI_PASS_DIRECT,
                              indirect ? BIR_ABI_REGISTER_GPR
                                       : bir_abi_register_class(type),
                              indirect || view || buffer ? 8U : (uint32_t)(type->size ? type->size : 8U),
                              indirect || view || buffer ? 8U : (uint32_t)(type->alignment ? type->alignment : 8U));
        if (!indirect) {
            location->storage = BIR_ABI_STORAGE_REGISTER;
            location->register_index = (uint16_t)part;
        }
    }
    locations->count = (uint8_t)part_count;
    return true;
}

static bool bir_compute_function_abi(const BackendIrModule *module,
                                     const BirFunctionInfo *info,
                                     BirCallAbi *out) {
    if (!module || !info || !out) return false;
    memset(out, 0, sizeof(*out));
    out->convention = BIR_CALLING_CONVENTION_COBRA;
    out->stack_alignment = BIR_ABI_STACK_ALIGNMENT;
    out->param_count = info->ssa_param_count;
    BirAbiCursor cursor = {0, 0, 0};
    for (size_t lowered = 0; lowered < info->ssa_param_count; lowered++) {
        bool hidden = info->has_hidden_return_storage && lowered == 0;
        size_t source = lowered - (info->has_hidden_return_storage ? 1U : 0U);
        const CobraType *source_type = hidden ? info->return_type : info->param_types[source];
        const CobraType *value_type = hidden ? info->return_value_type : info->param_value_types[source];
        bool indirect = hidden || (source_type &&                          (source_type->kind == COBRA_TYPE_STRUCT ||
                           source_type->kind == COBRA_TYPE_ARRAY ||
                           bir_is_sum_type(source_type)));

        if (!bir_abi_fill_value(&out->params[lowered], value_type, indirect, &cursor))
            return false;
    }
    out->stack_size = bir_abi_align_up(cursor.stack_size, out->stack_alignment);
    if (!bir_abi_fill_return(&out->returns, info->return_type,
                             info->has_hidden_return_storage)) return false;
    return true;
}

bool bir_validate_function_abi(const BackendIrModule *module,
                               const BirFunctionInfo *info) {
    BirCallAbi expected;
    return bir_compute_function_abi(module, info, &expected) &&
           memcmp(&expected, &info->call_abi, sizeof(expected)) == 0;
}

bool bir_declare_function(BackendIrModule *module, const char *name,
                           size_t param_count,
                           const CobraType *const *param_types,
                           const CobraType *return_type, bool has_return) {
    if (!module || !name || !name[0] || !return_type ||
        param_count > BIR_MAX_PARAMS) {
        if (module) snprintf(module->error, sizeof(module->error),
                             "invalid function declaration");
        return false;
    }
    if (bir_find_function(module, name)) {
        snprintf(module->error, sizeof(module->error),
                 "duplicate function '%s'", name);
        return false;
    }
    if (module->function_count >= BIR_MAX_FUNCTIONS) {
        snprintf(module->error, sizeof(module->error),
                 "too many functions in backend-IR module");
        return false;
    }
    BirFunctionInfo *info = &module->functions[module->function_count++];
    memset(info, 0, sizeof(*info));
    snprintf(info->name, sizeof(info->name), "%s", name);
    /* Conservative default: only bir_build_program's reachability pass ever
       clears this, once it has actually proven the function unreachable from
       main. Every other caller (unit tests building a module without a
       main, imported/extern declarations) keeps full verification. */
    info->reachable_from_main = true;
    info->entry = SSA_BLOCK_NONE;
    info->first_block = SSA_BLOCK_NONE;
    info->block_count = 0;
    info->param_count = param_count;
    info->ssa_param_count = param_count;
    info->has_hidden_return_storage = has_return &&
                                      (return_type->kind == COBRA_TYPE_STRUCT ||
                                       return_type->kind == COBRA_TYPE_ARRAY ||
                                       bir_is_sum_type(return_type));
    if (info->has_hidden_return_storage) info->ssa_param_count++;
    if (info->ssa_param_count > BIR_MAX_SSA_PARAMS) {
        module->function_count--;
        snprintf(module->error, sizeof(module->error),
                 "function '%s' exceeds the lowered parameter limit", name);
        return false;
    }
    for (size_t k = 0; k < param_count; k++) {
        info->param_types[k] = param_types && param_types[k]
            ? param_types[k] : module->type_i64;
        info->param_value_types[k] = info->param_types[k];
        if (info->param_types[k]->kind == COBRA_TYPE_STRUCT ||
            info->param_types[k]->kind == COBRA_TYPE_ARRAY ||
            bir_is_sum_type(info->param_types[k])) {
            info->param_value_types[k] = bir_pointer_type(module, info->param_types[k]);
            if (!info->param_value_types[k]) {
                module->function_count--;
                snprintf(module->error, sizeof(module->error),
                         "function '%s' has an invalid aggregate parameter", name);
                return false;
            }
            info->param_value_abi[k] = info->param_value_types[k]->abi;
        } else {
            info->param_value_abi[k] = info->param_types[k]->abi;
        }
        info->param_abi[k] = info->param_types[k]->abi;
    }
    info->return_type = return_type;
    info->return_abi = return_type->abi;
    info->return_value_type = return_type;
    if (info->has_hidden_return_storage) {
        info->return_value_type = bir_pointer_type(module, return_type);
        if (!info->return_value_type) {
            module->function_count--;
            snprintf(module->error, sizeof(module->error),
                     "function '%s' has invalid aggregate return storage", name);
            return false;
        }
    }
    info->return_value_abi = info->return_value_type->abi;
    if (!bir_compute_function_abi(module, info, &info->call_abi)) {
        module->function_count--;
        snprintf(module->error, sizeof(module->error),
                 "function '%s' has invalid ABI metadata", name);
        return false;
    }
    info->return_view_param = UINT32_MAX;
    info->return_pointer_contract = info->has_hidden_return_storage
        ? BIR_POINTER_CONTRACT_CALLER_STORAGE
        : BIR_POINTER_CONTRACT_UNKNOWN;
    if (bir_is_borrowed_view_type(return_type)) {
        size_t view_count = 0;
        size_t view_param = UINT32_MAX;
        for (size_t k = 0; k < param_count; k++) {
            if (bir_is_borrowed_view_type(info->param_value_types[k])) {
                view_count++;
                view_param = k;
            }
        }
        if (view_count != 1 || view_param == UINT32_MAX ||
            (bir_view_is_writable(return_type) &&
             !bir_view_is_writable(info->param_value_types[view_param]))) {
            module->function_count--;
            snprintf(module->error, sizeof(module->error),
                     "function '%s' borrowed-view return must derive from one compatible view parameter",
                     name);
            return false;
        }
        info->return_view_param = (uint32_t)view_param;
        info->return_pointer_contract = bir_view_is_writable(return_type)
            ? BIR_POINTER_CONTRACT_BORROW_WRITE
            : BIR_POINTER_CONTRACT_BORROW_READONLY;
    }
    for (size_t k = 0; k < param_count; k++) {
        if (info->param_is_out_struct[k]) {
            info->param_pointer_contract[k] = BIR_POINTER_CONTRACT_BORROW_WRITE;
        } else if (info->param_value_types[k]->kind == COBRA_TYPE_POINTER) {
            info->param_pointer_contract[k] = BIR_POINTER_CONTRACT_BORROW_READONLY;
        } else if (bir_is_owned_slice_type(info->param_value_types[k]) ||
                   bir_is_owned_dict_type(info->param_value_types[k])) {
            info->param_pointer_contract[k] = BIR_POINTER_CONTRACT_OWNED_SLICE;
        } else if (bir_is_borrowed_view_type(info->param_value_types[k])) {
            info->param_pointer_contract[k] = bir_view_is_writable(
                info->param_value_types[k])
                    ? BIR_POINTER_CONTRACT_BORROW_WRITE
                    : BIR_POINTER_CONTRACT_BORROW_READONLY;
        } else {
            info->param_pointer_contract[k] = BIR_POINTER_CONTRACT_UNKNOWN;
        }
    }
    info->has_return = has_return;
    return true;
}

bool bir_declare_extern_function(BackendIrModule *module, const char *name) {
    if (!module || !name || !name[0]) {
        if (module) snprintf(module->error, sizeof(module->error),
                             "invalid extern function declaration");
        return false;
    }
    /* `import c` may name the same function twice (or across repeated
       import lines); treat a re-declare as a no-op rather than an error. */
    const BirFunctionInfo *existing = bir_find_function(module, name);
    if (existing) return existing->is_extern;
    if (module->function_count >= BIR_MAX_FUNCTIONS) {
        snprintf(module->error, sizeof(module->error),
                 "too many functions in backend-IR module");
        return false;
    }
    BirFunctionInfo *info = &module->functions[module->function_count++];
    memset(info, 0, sizeof(*info));
    snprintf(info->name, sizeof(info->name), "%s", name);
    /* Conservative default: only bir_build_program's reachability pass ever
       clears this, once it has actually proven the function unreachable from
       main. Every other caller (unit tests building a module without a
       main, imported/extern declarations) keeps full verification. */
    info->reachable_from_main = true;
    info->entry = SSA_BLOCK_NONE;
    info->first_block = SSA_BLOCK_NONE;
    info->block_count = 0;
    info->return_type = module->type_i64;
    info->return_abi = module->type_i64->abi;
    info->return_value_type = module->type_i64;
    info->return_value_abi = module->type_i64->abi;
    info->return_view_param = UINT32_MAX;
    info->has_return = true;
    info->is_extern = true;
    return true;
}

bool bir_register_function_info(BackendIrModule *module, const char *name,
                                SsaBlockRef entry, size_t param_count,
                                const SsaValueRef *params,
                                const CobraType *return_type, bool has_return) {
    if (!module || !name || !name[0] || entry == SSA_BLOCK_NONE ||
        entry >= module->arena.block_count || !return_type ||
        param_count > BIR_MAX_PARAMS) {
        if (module) snprintf(module->error, sizeof(module->error),
                             "invalid function registration");
        return false;
    }
    BirFunctionInfo *info = (BirFunctionInfo *)bir_find_function(module, name);
    if (!info) {
        const CobraType *defaults[BIR_MAX_PARAMS] = {0};
        for (size_t k = 0; k < param_count; k++) defaults[k] = module->type_i64;
        if (!bir_declare_function(module, name, param_count, defaults,
                                  return_type, has_return)) return false;
        info = (BirFunctionInfo *)bir_find_function(module, name);
    } else if (info->entry != SSA_BLOCK_NONE) {
        snprintf(module->error, sizeof(module->error),
                 "function '%s' is already registered", name);
        return false;
    } else if (info->param_count != param_count ||
               !(info->return_type == return_type ||
                 cobra_type_equal(info->return_type, return_type)) ||
               info->has_return != has_return) {
        snprintf(module->error, sizeof(module->error),
                 "function '%s' definition does not match its declaration", name);
        return false;
    }
    info->entry = entry;
    info->first_block = entry;
    info->block_count = module->arena.block_count - entry;
    info->hidden_return_param = info->has_hidden_return_storage && params
        ? params[0] : SSA_VALUE_NONE;
    for (size_t k = 0; k < info->ssa_param_count; k++) {
        info->params[k] = params ? params[k] : SSA_VALUE_NONE;
    }
    info->return_type = return_type;
    info->return_abi = return_type->abi;
    info->return_pointer_contract = info->has_hidden_return_storage
        ? BIR_POINTER_CONTRACT_CALLER_STORAGE
        : bir_is_borrowed_view_type(return_type)
            ? (bir_view_is_writable(return_type)
                ? BIR_POINTER_CONTRACT_BORROW_WRITE
                : BIR_POINTER_CONTRACT_BORROW_READONLY)
            : BIR_POINTER_CONTRACT_UNKNOWN;
    info->has_return = has_return;
    for (size_t k = 0; k < param_count; k++) info->param_abi[k] = info->param_types[k]->abi;
    for (size_t k = 0; k < info->ssa_param_count; k++) {
        SsaValueRef ref = info->params[k];
        if (ref == SSA_VALUE_NONE || ref >= module->arena.value_count) continue;
        if (info->has_hidden_return_storage && k == 0) {
            module->arena.values[ref].pointer_contract =
                BIR_POINTER_CONTRACT_CALLER_STORAGE;
            module->arena.values[ref].pointer_origin = BIR_POINTER_ORIGIN_CALLER;
            module->arena.values[ref].region_id = 0;
        } else {
            size_t source = k - (info->has_hidden_return_storage ? 1U : 0U);
            module->arena.values[ref].pointer_contract =
                info->param_pointer_contract[source];
            if (module->arena.values[ref].type->kind == COBRA_TYPE_POINTER ||
                cobra_type_is_slice_kind(module->arena.values[ref].type->kind) ||
                bir_is_owned_buffer_type(module->arena.values[ref].type)) {
                module->arena.values[ref].pointer_origin = BIR_POINTER_ORIGIN_CALLER;
                module->arena.values[ref].region_id = 0;
            }
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

const char *bir_opcode_name(SsaOpcode op) {
    switch (op) {
        case SSA_OP_NONE: return "none";
        case SSA_OP_CONST: return "const";
        case SSA_OP_PARAM: return "param";
        case SSA_OP_BLOCK_ARG: return "block_arg";
        case SSA_OP_ADD: return "add";
        case SSA_OP_SUB: return "sub";
        case SSA_OP_MUL: return "mul";
        case SSA_OP_DIV: return "div";
        case SSA_OP_REM: return "rem";
        case SSA_OP_NEG: return "neg";
        case SSA_OP_CONVERT: return "convert";
        case SSA_OP_EQ: return "eq";
        case SSA_OP_NE: return "ne";
        case SSA_OP_LT: return "lt";
        case SSA_OP_LE: return "le";
        case SSA_OP_GT: return "gt";
        case SSA_OP_GE: return "ge";
        case SSA_OP_STACK_SLOT: return "stack_slot";
        case SSA_OP_PTR_ADD: return "ptr_add";
        case SSA_OP_FIELD_ADDR: return "field_addr";
        case SSA_OP_ARRAY_INDEX_ADDR: return "array_index_addr";
        case SSA_OP_LOAD: return "load";
        case SSA_OP_STORE: return "store";
        case SSA_OP_AGG_COPY: return "agg_copy";
        case SSA_OP_REGION_ENTER: return "region_enter";
        case SSA_OP_REGION_EXIT: return "region_exit";
        case SSA_OP_TRANSFER: return "transfer";
        case SSA_OP_DESTROY: return "destroy";
        case SSA_OP_VIEW_MAKE: return "view_make";
        case SSA_OP_VIEW_PTR: return "view_ptr";
        case SSA_OP_VIEW_LEN: return "view_len";
        case SSA_OP_SLICE_ALLOC: return "slice_alloc";
        case SSA_OP_SLICE_FREE: return "slice_free";
        case SSA_OP_BUFFER_ALLOC: return "buffer_alloc";
        case SSA_OP_BUFFER_APPEND: return "buffer_append";
        case SSA_OP_BUFFER_POP: return "buffer_pop";
        case SSA_OP_BUFFER_FREE: return "buffer_free";
        case SSA_OP_DICT_ALLOC: return "dict_alloc";
        case SSA_OP_DICT_SET: return "dict_set";
        case SSA_OP_DICT_GET: return "dict_get";
        case SSA_OP_DICT_HAS: return "dict_has";
        case SSA_OP_DICT_DELETE: return "dict_delete";
        case SSA_OP_DICT_POP: return "dict_pop";
        case SSA_OP_DICT_LEN: return "dict_len";
        case SSA_OP_DICT_FREE: return "dict_free";
        case SSA_OP_STRING_CONCAT: return "string_concat";
        case SSA_OP_STRING_EQ: return "string_eq";
        case SSA_OP_SUM_PAYLOAD_STORE: return "sum_payload_store";
        case SSA_OP_SUM_PAYLOAD_LOAD: return "sum_payload_load";
        case SSA_OP_SUM_MOVE: return "sum_move";
        case SSA_OP_SUM_DROP: return "sum_drop";
        case SSA_OP_FIELD_PAYLOAD_STORE: return "field_payload_store";
        case SSA_OP_FIELD_PAYLOAD_LOAD: return "field_payload_load";
        case SSA_OP_AGG_MOVE: return "agg_move";
        case SSA_OP_AGG_DROP: return "agg_drop";
        case SSA_OP_SUM_CHECK: return "sum_check";
        case SSA_OP_PRINT_I64: return "print_i64";
        case SSA_OP_PRINT_STRING: return "print_string";
        case SSA_OP_ASSERT: return "assert";
        case SSA_OP_CALL: return "call";
        case SSA_OP_JUMP: return "jump";
        case SSA_OP_BRANCH: return "branch";
        case SSA_OP_RETURN: return "return";
    }
    return "?";
}

bool bir_is_terminator(SsaOpcode op) {
    return op == SSA_OP_JUMP || op == SSA_OP_BRANCH || op == SSA_OP_RETURN;
}

bool bir_op_has_result(SsaOpcode op) {
    switch (op) {
        case SSA_OP_ADD:
        case SSA_OP_SUB:
        case SSA_OP_MUL:
        case SSA_OP_DIV:
        case SSA_OP_REM:
        case SSA_OP_NEG:
        case SSA_OP_CONVERT:
        case SSA_OP_EQ:
        case SSA_OP_NE:
        case SSA_OP_LT:
        case SSA_OP_LE:
        case SSA_OP_GT:
        case SSA_OP_GE:
        case SSA_OP_STACK_SLOT:
        case SSA_OP_PTR_ADD:
        case SSA_OP_FIELD_ADDR:
        case SSA_OP_ARRAY_INDEX_ADDR:
        case SSA_OP_LOAD:
        case SSA_OP_TRANSFER:
        case SSA_OP_VIEW_MAKE:
        case SSA_OP_VIEW_PTR:
        case SSA_OP_VIEW_LEN:
        case SSA_OP_SLICE_ALLOC:
        case SSA_OP_BUFFER_ALLOC:
        case SSA_OP_BUFFER_APPEND:
        case SSA_OP_BUFFER_POP:
        case SSA_OP_DICT_ALLOC:
        case SSA_OP_DICT_SET:
        case SSA_OP_DICT_GET:
        case SSA_OP_DICT_HAS:
        case SSA_OP_DICT_DELETE:
        case SSA_OP_DICT_POP:
        case SSA_OP_DICT_LEN:
        case SSA_OP_STRING_CONCAT:
        case SSA_OP_STRING_EQ:
        case SSA_OP_SUM_PAYLOAD_LOAD:
        case SSA_OP_FIELD_PAYLOAD_LOAD:
        case SSA_OP_CALL:
            return true;
        default:
            return false;
    }
}

const char *bir_pointer_contract_name(BirPointerContract contract) {
    switch (contract) {
        case BIR_POINTER_CONTRACT_UNKNOWN: return "unknown";
        case BIR_POINTER_CONTRACT_OWNED_FRAME: return "owned-frame";
        case BIR_POINTER_CONTRACT_OWNED_REGION: return "owned-region";
        case BIR_POINTER_CONTRACT_OWNED_SLICE: return "owned-slice";
        case BIR_POINTER_CONTRACT_BORROW_READONLY: return "borrow-readonly";
        case BIR_POINTER_CONTRACT_BORROW_WRITE: return "borrow-write";
        case BIR_POINTER_CONTRACT_CALLER_STORAGE: return "caller-storage";
    }
    return "?";
}

const char *bir_pointer_origin_name(BirPointerOrigin origin) {
    switch (origin) {
        case BIR_POINTER_ORIGIN_UNKNOWN: return "unknown";
        case BIR_POINTER_ORIGIN_FRAME: return "frame";
        case BIR_POINTER_ORIGIN_REGION: return "region";
        case BIR_POINTER_ORIGIN_CALLER: return "caller";
    }
    return "?";
}

bool bir_pointer_contract_readable(BirPointerContract contract) {
    return contract == BIR_POINTER_CONTRACT_OWNED_FRAME ||
           contract == BIR_POINTER_CONTRACT_OWNED_REGION ||
           contract == BIR_POINTER_CONTRACT_OWNED_SLICE ||
           contract == BIR_POINTER_CONTRACT_BORROW_READONLY ||
           contract == BIR_POINTER_CONTRACT_BORROW_WRITE ||
           contract == BIR_POINTER_CONTRACT_CALLER_STORAGE;
}

bool bir_pointer_contract_writable(BirPointerContract contract) {
    return contract == BIR_POINTER_CONTRACT_OWNED_FRAME ||
           contract == BIR_POINTER_CONTRACT_OWNED_REGION ||
           contract == BIR_POINTER_CONTRACT_OWNED_SLICE ||
           contract == BIR_POINTER_CONTRACT_BORROW_WRITE ||
           contract == BIR_POINTER_CONTRACT_CALLER_STORAGE;
}

bool bir_pointer_contract_compatible(BirPointerContract actual,
                                      BirPointerContract expected) {
    if (expected == BIR_POINTER_CONTRACT_UNKNOWN) return actual != BIR_POINTER_CONTRACT_UNKNOWN;
    if (expected == BIR_POINTER_CONTRACT_BORROW_READONLY)
        return bir_pointer_contract_readable(actual);
    if (expected == BIR_POINTER_CONTRACT_BORROW_WRITE)
        return bir_pointer_contract_writable(actual);
    if (expected == BIR_POINTER_CONTRACT_CALLER_STORAGE)
        return actual == BIR_POINTER_CONTRACT_OWNED_FRAME ||
               actual == BIR_POINTER_CONTRACT_OWNED_REGION ||
               actual == BIR_POINTER_CONTRACT_OWNED_SLICE ||
               actual == BIR_POINTER_CONTRACT_CALLER_STORAGE ||
               actual == BIR_POINTER_CONTRACT_BORROW_WRITE;
    return actual == expected;
}

const char *bir_value_kind_name(SsaValueKind kind) {
    switch (kind) {
        case SSA_VALUE_INVALID: return "invalid";
        case SSA_VALUE_PARAM: return "param";
        case SSA_VALUE_BLOCK_PARAM: return "block_param";
        case SSA_VALUE_CONST: return "const";
        case SSA_VALUE_INST: return "inst";
    }
    return "?";
}

bool bir_declare_region(BackendIrModule *module, uint32_t region_id,
                        uint32_t parent_region_id) {
    if (!module || region_id == BIR_REGION_NONE || region_id >= BIR_MAX_REGIONS ||
        parent_region_id >= BIR_MAX_REGIONS || module->region_count >= BIR_MAX_REGIONS) {
        if (module) snprintf(module->error, sizeof(module->error),
                             "invalid region declaration");
        return false;
    }
    if (parent_region_id != BIR_REGION_NONE) {
        bool parent_found = false;
        for (size_t i = 0; i < module->region_count; i++) {
            if (module->regions[i].declared && module->regions[i].id == parent_region_id) {
                parent_found = true;
                break;
            }
        }
        if (!parent_found) {
            snprintf(module->error, sizeof(module->error),
                     "region parent %u is not declared", parent_region_id);
            return false;
        }
        uint32_t cursor = parent_region_id;
        for (size_t depth = 0; depth < BIR_MAX_REGIONS && cursor != 0; depth++) {
            if (cursor == region_id) {
                snprintf(module->error, sizeof(module->error),
                         "region declaration would create a cycle");
                return false;
            }
            const BirRegionInfo *parent = NULL;
            for (size_t i = 0; i < module->region_count; i++) {
                if (module->regions[i].declared && module->regions[i].id == cursor) {
                    parent = &module->regions[i];
                    break;
                }
            }
            if (!parent) break;
            cursor = parent->parent_id;
        }
    }
    for (size_t i = 0; i < module->region_count; i++) {
        if (module->regions[i].id == region_id) {
            if (parent_region_id == region_id) {
                snprintf(module->error, sizeof(module->error),
                         "region %u cannot be its own parent", region_id);
                return false;
            }
            module->error[0] = '\0';
            module->regions[i].parent_id = parent_region_id;
            module->regions[i].declared = true;
            return true;
        }
    }
    BirRegionInfo *region = &module->regions[module->region_count++];
    region->id = region_id;
    region->parent_id = parent_region_id;
    region->declared = true;
    module->error[0] = '\0';
    return true;
}

bool bir_type_has_generic(const CobraType *type) {
    if (!type) return false;
    if (type->kind == COBRA_TYPE_GENERIC_PARAM) return true;
    for (size_t i = 0; i < type->generic_arg_count; i++) {
        if (bir_type_has_generic(type->generic_args[i])) return true;
    }
    for (size_t i = 0; i < type->field_count; i++) {
        if (bir_type_has_generic(type->fields[i].type)) return true;
    }
    return false;
}
