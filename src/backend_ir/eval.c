/*
 * Cobra Backend IR: typed scalar and memory evaluator.
 *
 * Pointer values identify a frame-local byte offset. Each call frame owns a
 * byte-backed stack arena, so loads and stores exercise width, alignment,
 * offsets, and pointer provenance rather than an integer-slot shortcut.
 */
#include "ssa.h"
#include <limits.h>

#define BIR_MAX_SUM_HANDLES 1024

typedef struct {
    bool live;
    BirScalarValue payload;
} EvalSumHandle;

typedef struct {
    bool live;
    BirScalarValue payload;
} EvalViewHandle;

typedef struct {
    BirScalarValue *slots;
    uint8_t *memory;
    uint8_t *slice_memory;      /* owned-slice byte arena (bump allocated)  */
    uint32_t frame_id;
    bool region_active[BIR_MAX_REGIONS];
    bool allocation_live[BIR_MAX_STACK_SLOTS + 1];
    uint32_t allocation_regions[BIR_MAX_STACK_SLOTS + 1];
    bool slice_allocations[BIR_MAX_STACK_SLOTS + 1];
    size_t next_slice_offset;
    uint16_t readonly_borrows[BIR_MAX_STACK_SLOTS + 1];
    uint16_t writable_borrows[BIR_MAX_STACK_SLOTS + 1];
    SsaBlockRef return_block;
    size_t return_inst_index;
    SsaValueRef call_result_slot;
    BirScalarValue call_storage;
    bool has_call_storage;
    uint32_t moved_allocations[BIR_MAX_PARAMS];
    size_t moved_count;
} EvalFrame;

typedef struct {
    const BackendIrModule *module;
    size_t slot_count;
    EvalFrame stack[BIR_MAX_CALL_DEPTH];
    size_t depth;
    BirScalarValue *current_slots;
    uint8_t *current_memory;
    uint8_t *slice_memory;      /* owned-slice arena of the current frame   */
    bool slice_allocations[BIR_MAX_STACK_SLOTS + 1];
    size_t next_slice_offset;
    uint32_t current_frame_id;
    uint32_t next_frame_id;
    bool region_active[BIR_MAX_REGIONS];
    bool allocation_live[BIR_MAX_STACK_SLOTS + 1];
    uint32_t allocation_regions[BIR_MAX_STACK_SLOTS + 1];
    uint16_t readonly_borrows[BIR_MAX_STACK_SLOTS + 1];
    uint16_t writable_borrows[BIR_MAX_STACK_SLOTS + 1];
    uint64_t steps;
    bool failed;
    EvalSumHandle sum_handles[BIR_MAX_SUM_HANDLES];
    EvalViewHandle view_handles[BIR_MAX_SUM_HANDLES];
} SsaEval;

static void eval_fail(SsaEval *ev, const char *fmt, ...) {
    if (ev->failed) return;
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "backend-IR eval: ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
    ev->failed = true;
}

static BirScalarValue scalar_zero_for_type(const CobraType *type) {
    if (!type || type->kind == COBRA_TYPE_VOID) {
        BirScalarValue result = {0};
        result.type = type;
        return result;
    }
    if (type->kind == COBRA_TYPE_I32) return bir_scalar_i32(type, 0);
    if (type->kind == COBRA_TYPE_U32) return bir_scalar_u32(type, 0);
    if (type->kind == COBRA_TYPE_U64) return bir_scalar_u64(type, 0);
    if (type->kind == COBRA_TYPE_F32) return bir_scalar_f32(type, 0.0f);
    if (type->kind == COBRA_TYPE_F64) return bir_scalar_f64(type, 0.0);
    if (type->kind == COBRA_TYPE_U8) return bir_scalar_u8(type, 0);
    if (type->kind == COBRA_TYPE_BOOL) return bir_scalar_bool(type, false);
    if (type->kind == COBRA_TYPE_POINTER) return bir_scalar_pointer(type, 0, 0);
    if (cobra_type_is_slice_kind(type->kind) || bir_is_owned_buffer_type(type)) {
        BirPointerValue pointer = bir_scalar_as_pointer(bir_scalar_pointer(type, 0, 0));
        return bir_scalar_buffer(type, pointer, 0, 0);
    }
    return bir_scalar_i64(type, 0);
}

static bool scalar_is_zero(BirScalarValue value) {
    return bir_scalar_is_zero(value);
}

static BirScalarValue eval_value(SsaEval *ev, SsaValueRef ref) {
    if (ref == SSA_VALUE_NONE || ref >= ev->slot_count) {
        return scalar_zero_for_type(ev->module->type_i64);
    }
    const SsaValue *value = &ev->module->arena.values[ref];
    if (value->kind == SSA_VALUE_CONST) return value->const_value;
    return ev->current_slots[ref];
}

/* Resolve the byte buffer backing a pointer: owned-slice allocations live in
   the per-frame slice arena, everything else in the typed slot arena. */
static bool eval_resolve_memory(SsaEval *ev, BirPointerValue pointer,
                                uint8_t **memory_out) {
    if (pointer.frame_id == ev->current_frame_id) {
        bool slice_backed = pointer.allocation_id != 0 &&
            pointer.allocation_id <= BIR_MAX_STACK_SLOTS &&
            ev->slice_allocations[pointer.allocation_id];
        *memory_out = slice_backed ? ev->slice_memory : ev->current_memory;
        return true;
    }
    for (size_t i = ev->depth; i > 0; i--) {
        EvalFrame *frame = &ev->stack[i - 1];
        if (frame->frame_id == pointer.frame_id) {
            bool slice_backed = pointer.allocation_id != 0 &&
                pointer.allocation_id <= BIR_MAX_STACK_SLOTS &&
                frame->slice_allocations[pointer.allocation_id];
            *memory_out = slice_backed ? frame->slice_memory : frame->memory;
            return true;
        }
    }
    return false;
}

static bool eval_bind_function_params(SsaEval *ev, const BirFunctionInfo *info,
                                      const BirScalarValue *args, size_t arg_count) {
    for (size_t k = 0; k < info->ssa_param_count; k++) {
        SsaValueRef param = info->params[k];
        const CobraType *type = info->has_hidden_return_storage && k == 0
            ? info->return_value_type
            : info->param_value_types[k -
                (info->has_hidden_return_storage ? 1U : 0U)];
        if (param == SSA_VALUE_NONE || param >= ev->slot_count) {
            eval_fail(ev, "function '%s' has an invalid parameter slot", info->name);
            return false;
        }
        BirScalarValue incoming = k < arg_count
            ? args[k] : scalar_zero_for_type(type);
        if (type->kind == COBRA_TYPE_POINTER || cobra_type_is_slice_kind(type->kind) ||
            bir_is_owned_buffer_type(type)) {
            BirPointerContract expected = info->has_hidden_return_storage && k == 0
                ? BIR_POINTER_CONTRACT_CALLER_STORAGE
                : info->param_pointer_contract[k -
                    (info->has_hidden_return_storage ? 1U : 0U)];
            bool valid_pointer = type->kind == COBRA_TYPE_POINTER &&
                                 incoming.kind == BIR_SCALAR_POINTER;
            bool valid_view = (cobra_type_is_slice_kind(type->kind) ||
                               bir_is_owned_buffer_type(type)) &&
                              incoming.kind == BIR_SCALAR_VIEW;
            BirPointerContract actual = valid_view
                ? incoming.payload.view.pointer.contract
                : valid_pointer ? incoming.payload.pointer.contract
                                : BIR_POINTER_CONTRACT_UNKNOWN;
            if ((!valid_pointer && !valid_view) ||
                !bir_pointer_contract_compatible(actual, expected)) {
                eval_fail(ev, "function '%s' received a value that violates its %s contract",
                          info->name, bir_pointer_contract_name(expected));
                return false;
            }
            incoming.type = type;
            if (valid_view) incoming.payload.view.pointer.contract = expected;
            else incoming.payload.pointer.contract = expected;
        }
        ev->current_slots[param] = incoming;
    }
    return true;
}

static bool eval_bind_block_params(SsaEval *ev, SsaBlockRef block,
                                   const SsaValueRef *edge_args, size_t edge_count) {
    const SsaBlock *target = &ev->module->arena.blocks[block];
    if (edge_count != target->param_count) {
        eval_fail(ev, "block b%u param/edge arity mismatch (%zu vs %u)",
                  block, edge_count, (unsigned)target->param_count);
        return false;
    }
    for (size_t k = 0; k < edge_count; k++) {
        if (target->params[k] >= ev->slot_count) {
            eval_fail(ev, "invalid block parameter slot");
            return false;
        }
        ev->current_slots[target->params[k]] = eval_value(ev, edge_args[k]);
    }
    return true;
}

static bool eval_is_aggregate(const CobraType *type) {
    if (!type || !type->finalized || type->ownership != COBRA_OWNERSHIP_VALUE ||
        type->mutability != COBRA_MUTABILITY_DEFAULT) return false;
    if (type->kind == COBRA_TYPE_ARRAY) {
        return type->generic_arg_count == 1 && type->array_length > 0 &&
               type->array_length <= COBRA_MAX_ARRAY_ELEMENTS &&
               type->generic_args[0] &&
               (cobra_type_is_scalar(type->generic_args[0]) ||
                type->generic_args[0]->kind == COBRA_TYPE_ARRAY ||
                bir_type_is_value_only_struct(type->generic_args[0])) &&
               type->size == type->generic_args[0]->size * type->array_length;
    }
    if (type->kind == COBRA_TYPE_OPTION || type->kind == COBRA_TYPE_RESULT ||
        (type->kind == COBRA_TYPE_ENUM && type->generic_arg_count > 0)) {
        size_t required = type->kind == COBRA_TYPE_RESULT ? 2 : 1;
        if (type->generic_arg_count < required) return false;
        for (size_t i = 0; i < type->generic_arg_count; i++) {
            const CobraType *component = type->generic_args[i];
            if (!component) continue; /* unit enum variant */
            if (component->kind != COBRA_TYPE_I64 &&
                component->kind != COBRA_TYPE_I32 &&
                component->kind != COBRA_TYPE_U32 &&
                component->kind != COBRA_TYPE_U64 &&
                component->kind != COBRA_TYPE_BOOL &&
                component->kind != COBRA_TYPE_F32 &&
                component->kind != COBRA_TYPE_F64 &&
                component->kind != COBRA_TYPE_U8 &&
                component->kind != COBRA_TYPE_OPTION &&
                component->kind != COBRA_TYPE_RESULT &&
                !bir_is_owned_slice_type(component) &&
                !bir_type_is_value_only_struct(component) &&
                component->kind != COBRA_TYPE_STRUCT) return false;
        }
        return true;
    }
    if (type->kind != COBRA_TYPE_STRUCT || type->field_count == 0) return false;
    for (size_t i = 0; i < type->field_count; i++) {
        const CobraTypeField *field = &type->fields[i];
        bool borrowed_view_field =
            bir_is_borrowed_view_type(field->type) &&
            field->ownership == COBRA_OWNERSHIP_BORROWED &&
            field->mutability == COBRA_MUTABILITY_READONLY &&
            field->region_id == -1;
        if ((!borrowed_view_field &&
             (field->ownership != COBRA_OWNERSHIP_VALUE ||
              field->mutability != COBRA_MUTABILITY_DEFAULT)) ||
            !field->type ||             (field->type->kind != COBRA_TYPE_I64 &&
              field->type->kind != COBRA_TYPE_I32 &&
              field->type->kind != COBRA_TYPE_U32 &&
              field->type->kind != COBRA_TYPE_U64 &&
              field->type->kind != COBRA_TYPE_BOOL &&
              field->type->kind != COBRA_TYPE_F32 &&
              field->type->kind != COBRA_TYPE_F64 &&
              field->type->kind != COBRA_TYPE_U8 &&
              field->type->kind != COBRA_TYPE_STRUCT &&
              field->type->kind != COBRA_TYPE_ARRAY &&
              field->type->kind != COBRA_TYPE_OPTION &&
              field->type->kind != COBRA_TYPE_RESULT &&
              !bir_is_owned_slice_type(field->type) &&
              !bir_is_borrowed_view_type(field->type))) return false;
    }
    return true;
}

static bool eval_validate_memory(SsaEval *ev, const SsaInst *inst) {
    const CobraType *type = inst->memory_type;
    bool aggregate = inst->op == SSA_OP_AGG_COPY ||
                     (inst->op == SSA_OP_STACK_SLOT && eval_is_aggregate(type));
    bool valid = inst->address_kind == SSA_ADDRESS_TYPED_POINTER && type &&
                 type->finalized && inst->address_space == 0 &&
                 inst->memory_alignment != 0 &&
                 (inst->memory_alignment & (inst->memory_alignment - 1)) == 0;
    if (inst->op == SSA_OP_FIELD_ADDR) {
        valid = valid && eval_is_aggregate(type) && inst->memory_offset >= 0;
        for (size_t i = 0; valid && i < type->field_count; i++) {
            const CobraTypeField *field = &type->fields[i];
            if (field->offset == (size_t)inst->memory_offset) {
                valid = inst->memory_width == field->type->size &&
                        inst->memory_alignment == field->type->alignment;
                break;
            }
        }
    } else {
        valid = valid && (aggregate ? eval_is_aggregate(type) :
                          (type->kind == COBRA_TYPE_I64 ||
                           type->kind == COBRA_TYPE_I32 ||
                           type->kind == COBRA_TYPE_U32 ||
                           type->kind == COBRA_TYPE_U64 ||
                           type->kind == COBRA_TYPE_BOOL ||
                           type->kind == COBRA_TYPE_F32 ||
                           type->kind == COBRA_TYPE_F64 ||
                           type->kind == COBRA_TYPE_U8 ||
                           type->kind == COBRA_TYPE_ENUM ||
                           bir_is_borrowed_view_type(type))) &&
                inst->memory_width == type->size &&
                inst->memory_alignment == type->alignment;
    }
    valid = valid &&
            (inst->op == SSA_OP_LOAD ? inst->effect == SSA_EFFECT_READ :
             inst->op == SSA_OP_STORE ? inst->effect == SSA_EFFECT_WRITE :
             inst->op == SSA_OP_AGG_COPY ? inst->effect == SSA_EFFECT_READWRITE :
             inst->effect == SSA_EFFECT_NONE);
    if (!valid) {
        eval_fail(ev, "%s violates its width/alignment contract",
                  bir_opcode_name(inst->op));
        return false;
    }
    return true;
}

static bool eval_pointer_live(SsaEval *ev, BirScalarValue pointer) {
    if (pointer.kind != BIR_SCALAR_POINTER || pointer.payload.pointer.frame_id == 0) {
        eval_fail(ev, "pointer is null");
        return false;
    }
    bool *region_active = ev->region_active;
    bool *allocation_live = ev->allocation_live;
    if (pointer.payload.pointer.frame_id != ev->current_frame_id) {
        EvalFrame *owner = NULL;
        for (size_t i = ev->depth; i > 0; i--) {
            if (ev->stack[i - 1].frame_id == pointer.payload.pointer.frame_id) {
                owner = &ev->stack[i - 1];
                break;
            }
        }
        if (!owner) {
            eval_fail(ev, "pointer refers to an inactive stack frame");
            return false;
        }
        region_active = owner->region_active;
        allocation_live = owner->allocation_live;
    }
    if (pointer.payload.pointer.origin == BIR_POINTER_ORIGIN_REGION) {
        if (pointer.payload.pointer.region_id == BIR_REGION_NONE ||
            pointer.payload.pointer.region_id >= BIR_MAX_REGIONS ||
            !region_active[pointer.payload.pointer.region_id]) {
            eval_fail(ev, "pointer refers to an inactive region");
            return false;
        }
    }
    if (pointer.payload.pointer.allocation_size != 0 &&
        (pointer.payload.pointer.offset < pointer.payload.pointer.allocation_base_offset ||
         pointer.payload.pointer.offset - pointer.payload.pointer.allocation_base_offset >
             (int64_t)pointer.payload.pointer.allocation_size)) {
        eval_fail(ev, "pointer is outside its allocation bounds");
        return false;
    }
    if (pointer.payload.pointer.view_element_width != 0 &&
        pointer.payload.pointer.view_length >= 0 &&
        (pointer.payload.pointer.offset < pointer.payload.pointer.view_base_offset ||
         pointer.payload.pointer.offset > pointer.payload.pointer.view_base_offset +
             pointer.payload.pointer.view_length * pointer.payload.pointer.view_element_width)) {
        eval_fail(ev, "pointer is outside its readonly view bounds");
        return false;
    }
    if (pointer.payload.pointer.allocation_id != 0 &&
        pointer.payload.pointer.allocation_id <= BIR_MAX_STACK_SLOTS &&
        pointer.payload.pointer.contract != BIR_POINTER_CONTRACT_OWNED_FRAME &&
        !allocation_live[pointer.payload.pointer.allocation_id]) {
        eval_fail(ev, "pointer refers to a destroyed allocation");
        return false;
    }
    return true;
}

static uint32_t eval_fresh_allocation(SsaEval *ev, uint32_t requested) {
    if (requested != 0 && requested <= BIR_MAX_STACK_SLOTS &&
        !ev->allocation_live[requested]) return requested;
    for (uint32_t candidate = 1; candidate <= BIR_MAX_STACK_SLOTS; candidate++) {
        if (!ev->allocation_live[candidate]) return candidate;
    }
    return 0;
}

static bool eval_check_borrow_conflict(SsaEval *ev, BirPointerValue pointer,
                                        BirPointerContract contract,
                                        const char *use) {
    uint32_t allocation = pointer.allocation_id;
    if (allocation == 0) return true;
    if (allocation > BIR_MAX_STACK_SLOTS) {
        eval_fail(ev, "%s references an invalid allocation identity", use);
        return false;
    }
    uint16_t readers = ev->readonly_borrows[allocation];
    uint16_t writers = ev->writable_borrows[allocation];
    if (contract == BIR_POINTER_CONTRACT_BORROW_WRITE &&
        (readers || writers > 1)) {
        eval_fail(ev, "%s conflicts with an active borrow", use);
        return false;
    }
    if ((contract == BIR_POINTER_CONTRACT_OWNED_FRAME ||
         contract == BIR_POINTER_CONTRACT_OWNED_REGION ||
         contract == BIR_POINTER_CONTRACT_OWNED_SLICE ||
         contract == BIR_POINTER_CONTRACT_CALLER_STORAGE) &&
        (readers || writers)) {
        eval_fail(ev, "%s writes while a borrowed view is active", use);
        return false;
    }
    if (contract == BIR_POINTER_CONTRACT_BORROW_READONLY && writers) {
        eval_fail(ev, "%s conflicts with an active writer", use);
        return false;
    }
    return true;
}

static bool eval_memory_range(SsaEval *ev, BirScalarValue pointer,
                              uint32_t width, uint32_t alignment,
                              uint8_t **memory_out, size_t *offset_out) {
    if (!eval_pointer_live(ev, pointer) ||
        pointer.payload.pointer.offset < 0 ||
        pointer.payload.pointer.offset > BIR_STACK_BYTES - (int64_t)width ||
        pointer.payload.pointer.offset % (int64_t)alignment != 0) {
        eval_fail(ev, "pointer is null, out of bounds, or misaligned");
        return false;
    }
    if (pointer.payload.pointer.view_element_width != 0) {
        int64_t view_end = pointer.payload.pointer.view_base_offset +
            pointer.payload.pointer.view_length * pointer.payload.pointer.view_element_width;
        if (pointer.payload.pointer.offset < pointer.payload.pointer.view_base_offset ||
            pointer.payload.pointer.offset > view_end ||
            (int64_t)width > view_end - pointer.payload.pointer.offset) {
            eval_fail(ev, "memory access exceeds readonly view bounds");
            return false;
        }
    }
    uint8_t *memory = NULL;
    if (!eval_resolve_memory(ev, pointer.payload.pointer, &memory)) {
        eval_fail(ev, "pointer refers to an inactive stack frame");
        return false;
    }
    *memory_out = memory;
    *offset_out = (size_t)pointer.payload.pointer.offset;
    return true;
}

static bool eval_read_memory(SsaEval *ev, const SsaInst *inst,
                             BirScalarValue pointer, BirScalarValue *result) {
    if (!eval_validate_memory(ev, inst)) return false;
    if (pointer.kind != BIR_SCALAR_POINTER ||
        !bir_pointer_contract_readable(pointer.payload.pointer.contract)) {
        eval_fail(ev, "read requires a readable pointer borrow");
        return false;
    }
    if (!eval_check_borrow_conflict(ev, pointer.payload.pointer,
                                    pointer.payload.pointer.contract, "read"))
        return false;
    uint8_t *memory = NULL;
    size_t offset = 0;
    if (!eval_memory_range(ev, pointer, inst->memory_width,
                           inst->memory_alignment, &memory, &offset)) return false;
    const CobraType *type = inst->memory_type;
    if (bir_is_borrowed_view_type(type)) {
        uint64_t handle = 0;
        memcpy(&handle, memory + offset, sizeof(handle));
        if (handle == 0 || handle >= BIR_MAX_SUM_HANDLES ||
            !ev->view_handles[handle].live) {
            eval_fail(ev, "borrowed view field is uninitialized or inactive");
            return false;
        }
        *result = ev->view_handles[handle].payload;
        result->type = type;
        return true;
    }
    if (type->kind == COBRA_TYPE_F32) {
        uint32_t bits = 0;
        memcpy(&bits, memory + offset, sizeof(bits));
        BirScalarValue value = {0};
        value.type = type;
        value.kind = BIR_SCALAR_F32;
        value.payload.f32_bits = bits;
        *result = value;
    } else if (type->kind == COBRA_TYPE_F64) {
        uint64_t bits = 0;
        memcpy(&bits, memory + offset, sizeof(bits));
        BirScalarValue value = {0};
        value.type = type;
        value.kind = BIR_SCALAR_F64;
        value.payload.f64_bits = bits;
        *result = value;
    } else if (type->kind == COBRA_TYPE_I32) {
        int32_t raw = 0;
        memcpy(&raw, memory + offset, sizeof(raw));
        *result = bir_scalar_i32(type, raw);
    } else if (type->kind == COBRA_TYPE_U32) {
        uint32_t raw = 0;
        memcpy(&raw, memory + offset, sizeof(raw));
        *result = bir_scalar_u32(type, raw);
    } else if (type->kind == COBRA_TYPE_U64) {
        uint64_t raw = 0;
        memcpy(&raw, memory + offset, sizeof(raw));
        *result = bir_scalar_u64(type, raw);
    } else if (type->kind == COBRA_TYPE_BOOL) {
        uint8_t raw = 0;
        memcpy(&raw, memory + offset, sizeof(raw));
        *result = bir_scalar_bool(type, raw != 0);
    } else if (type->kind == COBRA_TYPE_U8) {
        uint8_t raw = 0;
        memcpy(&raw, memory + offset, sizeof(raw));
        *result = bir_scalar_u8(type, raw);
    } else {
        int64_t raw = 0;
        memcpy(&raw, memory + offset, sizeof(raw));
        *result = bir_scalar_i64(type, raw);
    }
    return true;
}

static bool eval_write_memory(SsaEval *ev, const SsaInst *inst,
                              BirScalarValue pointer, BirScalarValue value) {
    if (!eval_validate_memory(ev, inst)) return false;
    if (pointer.kind != BIR_SCALAR_POINTER ||
        !bir_pointer_contract_writable(pointer.payload.pointer.contract)) {
        eval_fail(ev, "write requires an owned or writable borrowed pointer");
        return false;
    }
    if (!eval_check_borrow_conflict(ev, pointer.payload.pointer,
                                    pointer.payload.pointer.contract, "write"))
        return false;
    uint8_t *memory = NULL;
    size_t offset = 0;
    if (!eval_memory_range(ev, pointer, inst->memory_width,
                           inst->memory_alignment, &memory, &offset)) return false;
    const CobraType *type = inst->memory_type;
    if (bir_is_borrowed_view_type(type)) {
        if (value.kind != BIR_SCALAR_VIEW || !value.type ||
            !cobra_type_equal(value.type, type) ||
            !eval_pointer_live(ev, (BirScalarValue){
                .type = cobra_type_element(type),
                .kind = BIR_SCALAR_POINTER,
                .payload.pointer = value.payload.view.pointer
            })) {
            if (!ev->failed) eval_fail(ev, "invalid borrowed view field value");
            return false;
        }
        uint32_t handle = 0;
        for (uint32_t candidate = 1; candidate < BIR_MAX_SUM_HANDLES; candidate++) {
            if (!ev->view_handles[candidate].live) {
                handle = candidate;
                break;
            }
        }
        if (handle == 0) {
            eval_fail(ev, "borrowed view field handle table is full");
            return false;
        }
        memset(memory + offset, 0, inst->memory_width);
        memcpy(memory + offset, &handle, sizeof(handle));
        ev->view_handles[handle].live = true;
        ev->view_handles[handle].payload = value;
        return true;
    }
    if (type->kind == COBRA_TYPE_F32) {
        memcpy(memory + offset, &value.payload.f32_bits, sizeof(value.payload.f32_bits));
    } else if (type->kind == COBRA_TYPE_F64) {
        memcpy(memory + offset, &value.payload.f64_bits, sizeof(value.payload.f64_bits));
    } else if (type->kind == COBRA_TYPE_I32) {
        int32_t raw = (int32_t)value.payload.i64;
        memcpy(memory + offset, &raw, sizeof(raw));
    } else if (type->kind == COBRA_TYPE_U32) {
        uint32_t raw = (uint32_t)value.payload.i64;
        memcpy(memory + offset, &raw, sizeof(raw));
    } else if (type->kind == COBRA_TYPE_BOOL || type->kind == COBRA_TYPE_U8) {
        uint8_t raw = (uint8_t)value.payload.i64;
        memcpy(memory + offset, &raw, sizeof(raw));
    } else {
        memcpy(memory + offset, &value.payload.i64, sizeof(value.payload.i64));
    }
    return true;
}

static bool eval_sum_storage(SsaEval *ev, BirScalarValue pointer,
                              const CobraType *sum_type, bool writable,
                              uint8_t **memory_out, size_t *offset_out) {
    if (!sum_type || !bir_sum_has_owned_payload(sum_type) ||
        pointer.kind != BIR_SCALAR_POINTER ||
        !eval_pointer_live(ev, pointer) ||
        (writable && !bir_pointer_contract_writable(pointer.payload.pointer.contract)) ||
        (!writable && !bir_pointer_contract_readable(pointer.payload.pointer.contract)) ||
        pointer.payload.pointer.offset < 0 ||
        pointer.payload.pointer.offset > BIR_STACK_BYTES - (int64_t)sum_type->size) {
        if (!ev->failed) eval_fail(ev, "invalid owning sum storage");
        return false;
    }
    uint8_t *memory = NULL;
    if (!eval_resolve_memory(ev, pointer.payload.pointer, &memory)) {
        eval_fail(ev, "owning sum storage refers to an inactive frame");
        return false;
    }
    *memory_out = memory;
    *offset_out = (size_t)pointer.payload.pointer.offset;
    return true;
}

static uint64_t eval_read_u64(uint8_t *memory, size_t offset);
static void eval_write_u64(uint8_t *memory, size_t offset, uint64_t value);
static bool eval_aggregate_storage(SsaEval *ev, BirScalarValue pointer,
                                    const CobraType *aggregate_type, bool writable,
                                    uint8_t **memory_out, size_t *offset_out) {
    if (!aggregate_type || !aggregate_type->finalized ||
        pointer.kind != BIR_SCALAR_POINTER || !eval_pointer_live(ev, pointer) ||
        (writable && !bir_pointer_contract_writable(pointer.payload.pointer.contract)) ||
        (!writable && !bir_pointer_contract_readable(pointer.payload.pointer.contract)) ||
        pointer.payload.pointer.offset < 0 ||
        pointer.payload.pointer.offset > BIR_STACK_BYTES - (int64_t)aggregate_type->size) {
        if (!ev->failed) eval_fail(ev, "invalid aggregate storage");
        return false;
    }
    if (!eval_resolve_memory(ev, pointer.payload.pointer, memory_out)) {
        eval_fail(ev, "aggregate storage refers to an inactive frame");
        return false;
    }
    *offset_out = (size_t)pointer.payload.pointer.offset;
    return true;
}

static bool eval_transfer_returned_slice(SsaEval *ev, EvalFrame *caller,
                                         uint8_t *callee_slice_memory,
                                         BirScalarValue *value);

static int eval_sum_active_selector(const SsaEval *ev, const CobraType *sum,
                                    uint64_t tag) {
    if (!sum || !bir_is_sum_type(sum)) return 0;
    if (sum->kind == COBRA_TYPE_OPTION) return tag ? 1 : 0;
    if (sum->kind == COBRA_TYPE_RESULT) return tag ? 1 : 2;
    /* Payload enum: the tag is the variant discriminant; the selector is the
       1-based variant index, resolved through the module's variant registry. */
    if (sum->kind == COBRA_TYPE_ENUM) {
        for (size_t i = 0; i < BIR_MAX_ENUMS; i++) {
            const BirEnumInfo *info = &ev->module->enums[i];
            if (!info->name[0] || info->type != sum) continue;
            for (size_t v = 0; v < info->variant_count; v++) {
                if ((uint64_t)info->variant_values[v] == tag) return (int)(v + 1);
            }
            return 0;
        }
        return 0;
    }
    return 0;
}

static bool eval_sum_handle_is_live(const SsaEval *ev, uint64_t handle,
                                    BirScalarValue *payload_out) {
    if (!ev || handle == 0 || handle >= BIR_MAX_SUM_HANDLES ||
        !ev->sum_handles[handle].live) return false;
    if (payload_out) *payload_out = ev->sum_handles[handle].payload;
    return true;
}

static bool eval_sum_storage_occupied(const SsaEval *ev, uint8_t *memory,
                                      size_t base, const CobraType *sum_type);
static bool eval_drop_owned_value(SsaEval *ev, uint8_t *memory, size_t base,
                                  const CobraType *type);
static bool eval_collect_owned_value(const SsaEval *ev, uint8_t *memory,
                                     size_t base, const CobraType *type,
                                     uint32_t *allocations, size_t *count);
static bool eval_transfer_owned_value(SsaEval *ev, EvalFrame *caller,
                                      uint8_t *callee_slice_memory,
                                      uint8_t *memory, size_t base,
                                      const CobraType *type);

/* Drop the active recursively-owned payload in a sum storage slot. Nested
   sums are moved as bytes only after their ownership is represented by the
   same handle table, so recursive drop must walk their active branch. */
static bool eval_drop_sum_payload(SsaEval *ev, uint8_t *memory, size_t base,
                                  const CobraType *sum_type) {
    if (!ev || !memory || !sum_type || !bir_is_sum_type(sum_type)) return true;
    uint64_t tag = eval_read_u64(memory, base);
    int selector = eval_sum_active_selector(ev, sum_type, tag);
    if (selector == 0 || (size_t)selector > sum_type->generic_arg_count) return true;
    const CobraType *component = sum_type->generic_args[selector - 1];
    size_t offset = bir_sum_component_offset(sum_type, selector);
    if (bir_is_owned_slice_type(component)) {
        uint64_t handle = eval_read_u64(memory, base + offset);
        if (handle == 0) return true;
        BirScalarValue payload;
        if (!eval_sum_handle_is_live(ev, handle, &payload)) {
            eval_fail(ev, "owning sum payload is already inactive");
            return false;
        }
        uint32_t allocation = payload.payload.view.pointer.allocation_id;
        if (allocation == 0 || allocation > BIR_MAX_STACK_SLOTS ||
            !ev->allocation_live[allocation]) {
            eval_fail(ev, "owning sum payload allocation is not live");
            return false;
        }
        if (ev->readonly_borrows[allocation] || ev->writable_borrows[allocation]) {
            eval_fail(ev, "owning sum payload is borrowed during drop");
            return false;
        }
        ev->allocation_live[allocation] = false;
        ev->allocation_regions[allocation] = 0;
        ev->readonly_borrows[allocation] = 0;
        ev->writable_borrows[allocation] = 0;
        ev->sum_handles[handle].live = false;
        eval_write_u64(memory, base + offset, 0);
        eval_write_u64(memory, base + offset + sizeof(uint64_t), 0);
        return true;
    }
    if (bir_is_sum_type(component) && bir_sum_has_owned_payload(component)) {
        return eval_drop_sum_payload(ev, memory, base + offset, component);
    }
    if (component->kind == COBRA_TYPE_STRUCT) {
        /* Owning struct payloads drop their owned fields recursively at
           canonical offsets inside the component slot. */
        return eval_drop_owned_value(ev, memory, base + offset, component);
    }
    return true;
}

static bool eval_owned_value_occupied(const SsaEval *ev, uint8_t *memory, size_t base,
                                      const CobraType *type) {
    if (!ev || !memory || !type) return false;
    if (bir_is_owned_slice_type(type))
        return eval_read_u64(memory, base) != 0;
    if (bir_is_sum_type(type) && bir_sum_has_owned_payload(type))
        return eval_sum_storage_occupied(ev, memory, base, type);
    if (type->kind == COBRA_TYPE_STRUCT) {
        for (size_t i = 0; i < type->field_count; i++) {
            if (eval_owned_value_occupied(ev, memory,
                                          base + type->fields[i].offset,
                                          type->fields[i].type)) return true;
        }
    }
    return false;
}

static bool eval_drop_owned_value(SsaEval *ev, uint8_t *memory, size_t base,
                                  const CobraType *type) {
    if (!ev || !memory || !type) return true;
    if (bir_is_owned_slice_type(type)) {
        uint64_t handle = eval_read_u64(memory, base);
        if (handle == 0) return true;
        BirScalarValue payload;
        if (!eval_sum_handle_is_live(ev, handle, &payload)) {
            eval_fail(ev, "owned field payload is already inactive");
            return false;
        }
        uint32_t allocation = payload.payload.view.pointer.allocation_id;
        if (allocation == 0 || allocation > BIR_MAX_STACK_SLOTS ||
            !ev->allocation_live[allocation] ||
            ev->readonly_borrows[allocation] || ev->writable_borrows[allocation]) {
            eval_fail(ev, "owned field payload cannot be destroyed");
            return false;
        }
        ev->allocation_live[allocation] = false;
        ev->allocation_regions[allocation] = 0;
        ev->sum_handles[handle].live = false;
        eval_write_u64(memory, base, 0);
        eval_write_u64(memory, base + sizeof(uint64_t), 0);
        return true;
    }
    if (bir_is_sum_type(type) && bir_sum_has_owned_payload(type))
        return eval_drop_sum_payload(ev, memory, base, type);
    if (type->kind == COBRA_TYPE_STRUCT) {
        for (size_t i = 0; i < type->field_count; i++) {
            if (!eval_drop_owned_value(ev, memory,
                                       base + type->fields[i].offset,
                                       type->fields[i].type)) return false;
        }
    }
    return true;
}

static bool eval_sum_storage_occupied(const SsaEval *ev, uint8_t *memory,
                                      size_t base, const CobraType *sum_type) {
    if (!ev || !memory || !sum_type || !bir_is_sum_type(sum_type)) return false;
    uint64_t tag = eval_read_u64(memory, base);
    int selector = eval_sum_active_selector(ev, sum_type, tag);
    if (selector == 0 || (size_t)selector >= sum_type->generic_arg_count) return false;
    const CobraType *component = sum_type->generic_args[selector - 1];
    size_t offset = bir_sum_component_offset(sum_type, selector);
    if (bir_is_owned_slice_type(component))
        return eval_read_u64(memory, base + offset) != 0;
    if (bir_is_sum_type(component) && bir_sum_has_owned_payload(component))
        return eval_sum_storage_occupied(ev, memory, base + offset, component);
    if (component->kind == COBRA_TYPE_STRUCT)
        return eval_owned_value_occupied(ev, memory, base + offset, component);
    return false;
}

static bool eval_collect_sum_payloads(const SsaEval *ev, uint8_t *memory,
                                      size_t base, const CobraType *sum_type,
                                      uint32_t *allocations, size_t *count) {
    if (!ev || !memory || !sum_type || !allocations || !count ||
        !bir_is_sum_type(sum_type)) return true;
    uint64_t tag = eval_read_u64(memory, base);
    int selector = eval_sum_active_selector(ev, sum_type, tag);
    if (selector == 0 || (size_t)selector > sum_type->generic_arg_count) return true;
    const CobraType *component = sum_type->generic_args[selector - 1];
    size_t offset = bir_sum_component_offset(sum_type, selector);
    if (bir_is_owned_slice_type(component)) {
        uint64_t handle = eval_read_u64(memory, base + offset);
        if (handle == 0) return true;
        BirScalarValue payload;
        if (!eval_sum_handle_is_live(ev, handle, &payload)) return false;
        uint32_t allocation = payload.payload.view.pointer.allocation_id;
        if (allocation == 0 || allocation > BIR_MAX_STACK_SLOTS ||
            !ev->allocation_live[allocation]) return false;
        for (size_t i = 0; i < *count; i++)
            if (allocations[i] == allocation) return false;
        if (*count >= BIR_MAX_PARAMS) return false;
        allocations[(*count)++] = allocation;
        return true;
    }
    if (bir_is_sum_type(component) && bir_sum_has_owned_payload(component))
        return eval_collect_sum_payloads(ev, memory, base + offset, component,
                                         allocations, count);
    if (component->kind == COBRA_TYPE_STRUCT) {
        for (size_t i = 0; i < component->field_count; i++) {
            if (!eval_collect_owned_value(ev, memory,
                                          base + offset + component->fields[i].offset,
                                          component->fields[i].type, allocations,
                                          count)) return false;
        }
    }
    return true;
}

static bool eval_collect_owned_value(const SsaEval *ev, uint8_t *memory,
                                     size_t base, const CobraType *type,
                                     uint32_t *allocations, size_t *count) {
    if (!ev || !memory || !type || !allocations || !count) return true;
    if (bir_is_owned_slice_type(type)) {
        uint64_t handle = eval_read_u64(memory, base);
        if (handle == 0) return true;
        BirScalarValue payload;
        if (!eval_sum_handle_is_live(ev, handle, &payload)) return false;
        uint32_t allocation = payload.payload.view.pointer.allocation_id;
        if (allocation == 0 || allocation > BIR_MAX_STACK_SLOTS ||
            !ev->allocation_live[allocation]) return false;
        for (size_t i = 0; i < *count; i++)
            if (allocations[i] == allocation) return false;
        if (*count >= BIR_MAX_PARAMS) return false;
        allocations[(*count)++] = allocation;
        return true;
    }
    if (bir_is_sum_type(type) && bir_sum_has_owned_payload(type))
        return eval_collect_sum_payloads(ev, memory, base, type,
                                         allocations, count);
    if (type->kind == COBRA_TYPE_STRUCT) {
        for (size_t i = 0; i < type->field_count; i++) {
            if (!eval_collect_owned_value(ev, memory,
                                          base + type->fields[i].offset,
                                          type->fields[i].type, allocations,
                                          count)) return false;
        }
    }
    return true;
}

static bool eval_transfer_sum_payloads(SsaEval *ev, EvalFrame *caller,
                                       uint8_t *callee_slice_memory,
                                       uint8_t *memory, size_t base,
                                       const CobraType *sum_type) {
    if (!ev || !caller || !memory || !sum_type || !bir_is_sum_type(sum_type)) return true;
    uint64_t tag = eval_read_u64(memory, base);
    int selector = eval_sum_active_selector(ev, sum_type, tag);
    if (selector == 0 || (size_t)selector > sum_type->generic_arg_count) return true;
    const CobraType *component = sum_type->generic_args[selector - 1];
    size_t offset = bir_sum_component_offset(sum_type, selector);
    if (bir_is_owned_slice_type(component)) {
        uint64_t handle = eval_read_u64(memory, base + offset);
        if (handle == 0) return true;
        BirScalarValue payload;
        if (!eval_sum_handle_is_live(ev, handle, &payload)) return false;
        if (payload.kind != BIR_SCALAR_VIEW) return false;
        if (!eval_transfer_returned_slice(ev, caller, callee_slice_memory, &payload))
            return false;
        ev->sum_handles[handle].payload = payload;
        return true;
    }
    if (bir_is_sum_type(component) && bir_sum_has_owned_payload(component))
        return eval_transfer_sum_payloads(ev, caller, callee_slice_memory,
                                          memory, base + offset, component);
    if (component->kind == COBRA_TYPE_STRUCT)
        return eval_transfer_owned_value(ev, caller, callee_slice_memory,
                                         memory, base + offset, component);
    return true;
}

/* Move every owned payload nested inside a value at the given storage
   base into the caller arena before the callee frame is released. Walks
   struct fields and owning-sum components at canonical offsets. */
static bool eval_transfer_owned_value(SsaEval *ev, EvalFrame *caller,
                                      uint8_t *callee_slice_memory,
                                      uint8_t *memory, size_t base,
                                      const CobraType *type) {
    if (!ev || !caller || !memory || !type ||
        !bir_type_has_owned_payload(type)) return true;
    if (bir_is_owned_slice_type(type)) {
        uint64_t handle = eval_read_u64(memory, base);
        if (handle == 0) return true;
        BirScalarValue payload;
        if (!eval_sum_handle_is_live(ev, handle, &payload)) return false;
        if (payload.kind != BIR_SCALAR_VIEW) return false;
        if (!eval_transfer_returned_slice(ev, caller, callee_slice_memory, &payload))
            return false;
        ev->sum_handles[handle].payload = payload;
        return true;
    }
    if (bir_is_sum_type(type) && bir_sum_has_owned_payload(type))
        return eval_transfer_sum_payloads(ev, caller, callee_slice_memory,
                                          memory, base, type);
    if (type->kind == COBRA_TYPE_STRUCT) {
        for (size_t i = 0; i < type->field_count; i++) {
            if (!eval_transfer_owned_value(ev, caller, callee_slice_memory,
                                           memory, base + type->fields[i].offset,
                                           type->fields[i].type)) return false;
        }
    }
    return true;
}

static uint64_t eval_read_u64(uint8_t *memory, size_t offset) {
    uint64_t value = 0;
    memcpy(&value, memory + offset, sizeof(value));
    return value;
}

static void eval_write_u64(uint8_t *memory, size_t offset, uint64_t value) {
    memcpy(memory + offset, &value, sizeof(value));
}

static bool eval_call(SsaEval *ev, const SsaInst *inst, SsaBlockRef *next_block,
                      size_t *next_inst) {
    const BirFunctionInfo *callee = bir_find_function(ev->module, inst->callee);
    if (!callee) {
        eval_fail(ev, "call to unknown function '%s'", inst->callee);
        return false;
    }
    BirScalarValue args[BIR_MAX_SSA_PARAMS];
    size_t arg_count = inst->operand_count;
    if (!bir_validate_function_abi(ev->module, callee)) {
        eval_fail(ev, "call '%s' has invalid ABI metadata", inst->callee);
        return false;
    }
    if (arg_count != callee->call_abi.param_count ||
        arg_count > BIR_MAX_SSA_PARAMS) {
        eval_fail(ev, "call '%s' has an invalid lowered argument count", inst->callee);
        return false;
    }
    for (size_t o = 0; o < arg_count; o++) {
        args[o] = eval_value(ev, ev->module->arena.operands[inst->operand_start + o]);
    }
    /* A borrow built solely for this call's argument (transient_borrow on
       its defining view_make, see hir.c's call-argument HIR_EXPR_BORROW
       wrap) is released here, mirroring verify.c's static flow check - it
       lives for exactly this call, not for the rest of the function like a
       let-bound view local. */
    for (size_t o = 0; o < arg_count; o++) {
        SsaValueRef ref = ev->module->arena.operands[inst->operand_start + o];
        if (ref >= ev->module->arena.value_count) continue;
        const SsaValue *val = &ev->module->arena.values[ref];
        if (val->def_inst == SSA_INST_NONE || val->def_inst >= ev->module->arena.inst_count)
            continue;
        const SsaInst *def = &ev->module->arena.insts[val->def_inst];
        if (def->op != SSA_OP_VIEW_MAKE || !def->transient_borrow) continue;
        uint32_t allocation = val->allocation_id;
        if (allocation == 0 || allocation > BIR_MAX_STACK_SLOTS) continue;
        if (val->pointer_contract == BIR_POINTER_CONTRACT_BORROW_WRITE) {
            if (ev->writable_borrows[allocation] > 0) ev->writable_borrows[allocation]--;
        } else if (val->pointer_contract == BIR_POINTER_CONTRACT_BORROW_READONLY) {
            if (ev->readonly_borrows[allocation] > 0) ev->readonly_borrows[allocation]--;
        }
    }
    uint32_t moved_allocations[BIR_MAX_PARAMS];
    size_t moved_count = 0;
    uint32_t sum_storage_allocations[BIR_MAX_PARAMS];
    size_t sum_storage_count = 0;
    for (size_t param = 0; param < callee->param_count; param++) {
        bool owning_slice = bir_is_owned_slice_type(callee->param_types[param]) ||
                            bir_is_owned_dict_type(callee->param_types[param]);
        bool owning_sum = bir_is_sum_type(callee->param_types[param]) &&
                          bir_sum_has_owned_payload(callee->param_types[param]);
        bool owning_struct = callee->param_types[param]->kind == COBRA_TYPE_STRUCT &&
                             bir_type_has_owned_payload(callee->param_types[param]);
        if (!owning_slice && !owning_sum && !owning_struct) continue;
        size_t lowered = param + (callee->has_hidden_return_storage ? 1U : 0U);
        BirScalarValue incoming = args[lowered];
        uint32_t allocation = incoming.kind == BIR_SCALAR_VIEW
            ? incoming.payload.view.pointer.allocation_id
            : incoming.kind == BIR_SCALAR_POINTER
                ? incoming.payload.pointer.allocation_id : 0;
        if (allocation == 0 || allocation > BIR_MAX_STACK_SLOTS ||
            !ev->allocation_live[allocation] ||
            ev->allocation_regions[allocation] != 0 ||
            ev->readonly_borrows[allocation] ||
            ev->writable_borrows[allocation] ||
            moved_count >= BIR_MAX_PARAMS ||
            (owning_slice && incoming.kind != BIR_SCALAR_VIEW) ||
            ((owning_sum || owning_struct) && incoming.kind != BIR_SCALAR_POINTER)) {
            eval_fail(ev, "%s argument to '%s' is not transferable",
                      owning_slice ? "owned slice"
                      : (owning_sum ? "owning sum" : "owning struct"),
                      inst->callee);
            return false;
        }
        if (owning_sum) {
            if (sum_storage_count >= BIR_MAX_PARAMS) {
                eval_fail(ev, "too many owning sum arguments to '%s'", inst->callee);
                return false;
            }
            sum_storage_allocations[sum_storage_count++] = allocation;
            uint8_t *sum_memory = NULL;
            if (incoming.payload.pointer.frame_id == ev->current_frame_id) {
                sum_memory = ev->current_memory;
            } else if (!eval_resolve_memory(ev, incoming.payload.pointer, &sum_memory)) {
                eval_fail(ev, "owning sum argument refers to inactive storage");
                return false;
            }
            size_t sum_base = (size_t)incoming.payload.pointer.offset;
            const CobraType *sum_type = incoming.type->generic_args[0];
            size_t payload_count = moved_count;
            if (!eval_collect_sum_payloads(ev, sum_memory, sum_base, sum_type,
                                           moved_allocations, &payload_count)) {
                eval_fail(ev, "owning sum argument has an inactive or duplicate payload");
                return false;
            }
            moved_count = payload_count;
        } else if (owning_struct) {
            uint8_t *struct_memory = NULL;
            if (incoming.payload.pointer.frame_id == ev->current_frame_id) {
                struct_memory = ev->current_memory;
            } else if (!eval_resolve_memory(ev, incoming.payload.pointer, &struct_memory)) {
                eval_fail(ev, "owning struct argument refers to inactive storage");
                return false;
            }
            size_t struct_base = (size_t)incoming.payload.pointer.offset;
            size_t payload_count = moved_count;
            if (!eval_collect_owned_value(ev, struct_memory, struct_base,
                                          callee->param_types[param],
                                          moved_allocations, &payload_count)) {
                eval_fail(ev, "owning struct argument has an inactive or duplicate payload");
                return false;
            }
            moved_count = payload_count;
        } else {
            moved_allocations[moved_count++] = allocation;
        }
    }
    if (ev->depth >= BIR_MAX_CALL_DEPTH) {
        eval_fail(ev, "call depth exceeded in '%s'", inst->callee);
        return false;
    }
    BirScalarValue *callee_slots = calloc(ev->slot_count ? ev->slot_count : 1,
                                          sizeof(BirScalarValue));
    uint8_t *callee_memory = calloc(BIR_STACK_BYTES, sizeof(uint8_t));
    uint8_t *callee_slice_memory = calloc(BIR_STACK_BYTES, sizeof(uint8_t));
    if (!callee_slots || !callee_memory || !callee_slice_memory) {
        free(callee_slots);
        free(callee_memory);
        free(callee_slice_memory);
        eval_fail(ev, "out of memory in evaluator");
        return false;
    }
    EvalFrame *frame = &ev->stack[ev->depth++];
    frame->slots = ev->current_slots;
    frame->memory = ev->current_memory;
    frame->slice_memory = ev->slice_memory;
    frame->frame_id = ev->current_frame_id;
    memcpy(frame->region_active, ev->region_active, sizeof(ev->region_active));
    memcpy(frame->allocation_live, ev->allocation_live, sizeof(ev->allocation_live));
    memcpy(frame->allocation_regions, ev->allocation_regions, sizeof(ev->allocation_regions));
    memcpy(frame->slice_allocations, ev->slice_allocations, sizeof(ev->slice_allocations));
    frame->next_slice_offset = ev->next_slice_offset;
    memcpy(frame->readonly_borrows, ev->readonly_borrows, sizeof(ev->readonly_borrows));
    memcpy(frame->writable_borrows, ev->writable_borrows, sizeof(ev->writable_borrows));
    frame->return_block = *next_block;
    frame->return_inst_index = *next_inst;
    frame->call_result_slot = inst->result;
    frame->has_call_storage = callee->has_hidden_return_storage;
    frame->call_storage = callee->has_hidden_return_storage ? args[0]
                                                             : (BirScalarValue){0};
    frame->moved_count = moved_count;
    memcpy(frame->moved_allocations, moved_allocations,
           moved_count * sizeof(moved_allocations[0]));
    for (size_t moved = 0; moved < moved_count; moved++)
        ev->allocation_live[moved_allocations[moved]] = false;
    ev->current_slots = callee_slots;
    ev->current_memory = callee_memory;
    ev->slice_memory = callee_slice_memory;
    ev->current_frame_id = ++ev->next_frame_id;
    memset(ev->region_active, 0, sizeof(ev->region_active));
    memset(ev->allocation_live, 0, sizeof(ev->allocation_live));
    memset(ev->allocation_regions, 0, sizeof(ev->allocation_regions));
    memset(ev->slice_allocations, 0, sizeof(ev->slice_allocations));
    for (size_t moved = 0; moved < moved_count; moved++) {
        ev->allocation_live[moved_allocations[moved]] = true;
        ev->slice_allocations[moved_allocations[moved]] = true;
    }
    for (size_t sum = 0; sum < sum_storage_count; sum++) {
        ev->allocation_live[sum_storage_allocations[sum]] = true;
        ev->slice_allocations[sum_storage_allocations[sum]] = false;
    }
    ev->next_slice_offset = 0;
    memset(ev->readonly_borrows, 0, sizeof(ev->readonly_borrows));
    memset(ev->writable_borrows, 0, sizeof(ev->writable_borrows));
    if (!eval_bind_function_params(ev, callee, args, arg_count)) {
        ev->current_slots = frame->slots;
        ev->current_memory = frame->memory;
        ev->slice_memory = frame->slice_memory;
        ev->current_frame_id = frame->frame_id;
        memcpy(ev->region_active, frame->region_active, sizeof(ev->region_active));
        memcpy(ev->allocation_live, frame->allocation_live, sizeof(ev->allocation_live));
        memcpy(ev->allocation_regions, frame->allocation_regions, sizeof(ev->allocation_regions));
        memcpy(ev->slice_allocations, frame->slice_allocations, sizeof(ev->slice_allocations));
        ev->next_slice_offset = frame->next_slice_offset;
        memcpy(ev->readonly_borrows, frame->readonly_borrows, sizeof(ev->readonly_borrows));
        memcpy(ev->writable_borrows, frame->writable_borrows, sizeof(ev->writable_borrows));
        ev->depth--;
        free(callee_slots);
        free(callee_memory);
        free(callee_slice_memory);
        return false;
    }
    *next_block = callee->entry;
    *next_inst = 0;
    return true;
}

/* Move an owned slice result across a call boundary. Parameters keep their
   original frame-backed allocation, while allocations created in the callee
   are copied into the caller's slice arena before the callee frame is freed. */
static bool eval_transfer_returned_slice(SsaEval *ev, EvalFrame *caller,
                                         uint8_t *callee_slice_memory,
                                         BirScalarValue *value) {
    if (!value || value->kind != BIR_SCALAR_VIEW ||
        value->payload.view.pointer.contract != BIR_POINTER_CONTRACT_OWNED_SLICE)
        return true;
    BirPointerValue *pointer = &value->payload.view.pointer;
    uint32_t source_allocation = pointer->allocation_id;
    if (source_allocation == 0 || source_allocation > BIR_MAX_STACK_SLOTS ||
        !ev->allocation_live[source_allocation] ||
        pointer->allocation_size == 0 ||
        pointer->allocation_base_offset < 0 ||
        pointer->allocation_base_offset > BIR_STACK_BYTES -
            (int64_t)pointer->allocation_size)
        return false;

    if (pointer->frame_id != ev->current_frame_id) {
        /* The result is a previously transferred parameter. Its backing frame
           remains active, so only restore ownership in the caller snapshot. */
        if (pointer->frame_id != caller->frame_id ||
            !caller->slice_allocations[source_allocation]) {
            bool found = false;
            for (size_t i = ev->depth; i > 0; i--) {
                if (ev->stack[i - 1].frame_id == pointer->frame_id) {
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }
        caller->allocation_live[source_allocation] = true;
        caller->allocation_regions[source_allocation] = 0;
        caller->slice_allocations[source_allocation] = true;
        return true;
    }

    uint32_t destination_allocation = 0;
    for (uint32_t candidate = 1; candidate <= BIR_MAX_STACK_SLOTS; candidate++) {
        if (!caller->allocation_live[candidate]) {
            destination_allocation = candidate;
            break;
        }
    }
    if (destination_allocation == 0) return false;
    size_t alignment = pointer->view_element_width ? pointer->view_element_width : 1;
    if ((alignment & (alignment - 1)) != 0) alignment = 1;
    size_t destination = caller->next_slice_offset;
    size_t remainder = destination % alignment;
    if (remainder) destination += alignment - remainder;
    if (destination > BIR_STACK_BYTES - pointer->allocation_size ||
        pointer->allocation_base_offset < 0 ||
        !callee_slice_memory) return false;
    memcpy(caller->slice_memory + destination,
           callee_slice_memory + pointer->allocation_base_offset,
           pointer->allocation_size);

    int64_t view_delta = pointer->view_base_offset - pointer->allocation_base_offset;
    int64_t pointer_delta = pointer->offset - pointer->allocation_base_offset;
    pointer->frame_id = caller->frame_id;
    pointer->allocation_id = destination_allocation;
    pointer->allocation_base_offset = (int64_t)destination;
    pointer->view_base_offset = (int64_t)destination + view_delta;
    pointer->offset = (int64_t)destination + pointer_delta;
    caller->next_slice_offset = destination + pointer->allocation_size;
    caller->allocation_live[destination_allocation] = true;
    caller->allocation_regions[destination_allocation] = 0;
    caller->slice_allocations[destination_allocation] = true;
    return true;
}

/* An ownership-bearing aggregate returned through hidden caller storage may
   contain payloads allocated in the callee frame. Move every payload into the
   caller arena before the callee frame is released. */
static bool eval_transfer_returned_value(SsaEval *ev, EvalFrame *caller,
                                         uint8_t *callee_slice_memory,
                                         uint8_t *memory, size_t base,
                                         const CobraType *type) {
    if (!type || !bir_type_has_owned_payload(type)) return true;
    if (bir_is_owned_slice_type(type)) {
        uint64_t handle = eval_read_u64(memory, base);
        if (handle == 0) return true;
        BirScalarValue payload;
        if (!eval_sum_handle_is_live(ev, handle, &payload) ||
            !eval_transfer_returned_slice(ev, caller, callee_slice_memory, &payload))
            return false;
        ev->sum_handles[handle].payload = payload;
        return true;
    }
    if (bir_is_sum_type(type))
        return eval_transfer_sum_payloads(ev, caller, callee_slice_memory,
                                          memory, base, type);
    if (type->kind == COBRA_TYPE_STRUCT) {
        for (size_t i = 0; i < type->field_count; i++) {
            if (!eval_transfer_returned_value(ev, caller, callee_slice_memory,
                                              memory, base + type->fields[i].offset,
                                              type->fields[i].type)) return false;
        }
    }
    return true;
}

static bool eval_transfer_returned_aggregate(SsaEval *ev, EvalFrame *caller,
                                             uint8_t *callee_slice_memory) {
    if (!caller || !caller->has_call_storage) return true;
    BirScalarValue storage = caller->call_storage;
    if (storage.kind != BIR_SCALAR_POINTER || !storage.type ||
        storage.type->kind != COBRA_TYPE_POINTER ||
        storage.type->generic_arg_count != 1) return false;
    const CobraType *type = storage.type->generic_args[0];
    if (!bir_type_has_owned_payload(type)) return true;
    if (storage.payload.pointer.frame_id != caller->frame_id ||
        storage.payload.pointer.offset < 0 ||
        storage.payload.pointer.offset > BIR_STACK_BYTES - (int64_t)type->size)
        return false;
    return eval_transfer_returned_value(ev, caller, callee_slice_memory,
                                        caller->memory,
                                        (size_t)storage.payload.pointer.offset,
                                        type);
}

static bool eval_terminator(SsaEval *ev, const SsaInst *term, SsaBlockRef block,
                            SsaBlockRef *next_block, size_t *next_inst,
                            bool *returned, BirScalarValue *result) {
    const SsaArena *arena = &ev->module->arena;
    switch (term->op) {
        case SSA_OP_JUMP:
            if (!eval_bind_block_params(ev, term->target,
                                        &arena->edges[term->edge_start],
                                        term->edge_count)) return false;
            *next_block = term->target;
            *next_inst = 0;
            return true;
        case SSA_OP_BRANCH: {
            BirScalarValue cond = eval_value(ev, arena->operands[term->operand_start]);
            bool taken = !scalar_is_zero(cond);
            SsaBlockRef target = taken ? term->target : term->target2;
            const SsaValueRef *edge_args = taken
                ? &arena->edges[term->edge_start] : &arena->edges[term->edge2_start];
            size_t edge_count = taken ? term->edge_count : term->edge2_count;
            if (!eval_bind_block_params(ev, target, edge_args, edge_count)) return false;
            *next_block = target;
            *next_inst = 0;
            return true;
        }
        case SSA_OP_RETURN: {
            BirScalarValue value = term->operand_count == 1
                ? eval_value(ev, arena->operands[term->operand_start])
                : scalar_zero_for_type(ev->module->type_void);
            if (value.kind == BIR_SCALAR_VIEW &&
                value.payload.view.pointer.contract != BIR_POINTER_CONTRACT_OWNED_SLICE &&
                value.payload.view.pointer.frame_id == ev->current_frame_id) {
                eval_fail(ev, "borrowed view return escapes its owner frame");
                return false;
            }
            if (ev->depth == 0) {
                *result = value;
                *returned = true;
                return true;
            }
            EvalFrame frame = ev->stack[--ev->depth];
            BirScalarValue *callee_slots = ev->current_slots;
            uint8_t *callee_memory = ev->current_memory;
            uint8_t *callee_slice_memory = ev->slice_memory;
            bool transfer_ok = eval_transfer_returned_slice(
                ev, &frame, callee_slice_memory, &value);
            if (transfer_ok && !eval_transfer_returned_aggregate(ev, &frame,
                                                                  callee_slice_memory))
                transfer_ok = false;
            uint32_t returned_allocation =
                value.kind == BIR_SCALAR_VIEW &&
                value.payload.view.pointer.contract == BIR_POINTER_CONTRACT_OWNED_SLICE
                ? value.payload.view.pointer.allocation_id : 0;
            ev->current_slots = frame.slots;
            ev->current_memory = frame.memory;
            ev->slice_memory = frame.slice_memory;
            ev->current_frame_id = frame.frame_id;
            for (size_t moved = 0; moved < frame.moved_count; moved++) {
                if (frame.moved_allocations[moved] != returned_allocation)
                    frame.allocation_live[frame.moved_allocations[moved]] = false;
            }
            memcpy(ev->region_active, frame.region_active, sizeof(ev->region_active));
            memcpy(ev->allocation_live, frame.allocation_live, sizeof(ev->allocation_live));
            memcpy(ev->allocation_regions, frame.allocation_regions, sizeof(ev->allocation_regions));
            memcpy(ev->slice_allocations, frame.slice_allocations, sizeof(ev->slice_allocations));
            ev->next_slice_offset = frame.next_slice_offset;
            memcpy(ev->readonly_borrows, frame.readonly_borrows, sizeof(ev->readonly_borrows));
            memcpy(ev->writable_borrows, frame.writable_borrows, sizeof(ev->writable_borrows));
            if (frame.call_result_slot != SSA_VALUE_NONE &&
                frame.call_result_slot < ev->slot_count)
                ev->current_slots[frame.call_result_slot] = value;
            free(callee_slots);
            free(callee_memory);
            free(callee_slice_memory);
            if (!transfer_ok) {
                eval_fail(ev, "owned slice return could not transfer its allocation");
                return false;
            }
            *next_block = frame.return_block;
            *next_inst = frame.return_inst_index;
            return true;
        }
        default:
            eval_fail(ev, "block b%u terminator is not a terminator", block);
            return false;
    }
}

/* ------------------------------------------------------------------ */
/* Dict tables: an open-addressing FNV-1a table in the owned-slice     */
/* arena, mirroring the production cobra_dict contract. Layout:         */
/*   table+0   capacity (i64)                                          */
/*   table+8   length   (i64, live entries)                            */
/*   table+16  entries[capacity], each BIR_DICT_STRIDE bytes:          */
/*     +0  used (0 empty, 1 live, 2 tombstone)                        */
/*     +8  value (i64)                                                 */
/*     +16 key bytes (COBRA_MAX_TOKEN_TEXT, NUL-terminated)            */
/* ------------------------------------------------------------------ */
#define BIR_DICT_HEADER 16
#define BIR_DICT_STRIDE (16 + COBRA_MAX_TOKEN_TEXT)
#define BIR_DICT_KEY_OFFSET 16
#define BIR_DICT_VALUE_OFFSET 8

static uint64_t eval_dict_hash(const char *key) {
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char *p = (const unsigned char *)key; *p; p++) {
        hash ^= *p;
        hash *= 1099511628211ULL;
    }
    return hash;
}

/* Find a live entry for key; returns its slot index or -1. Probing stops
   at an empty slot; tombstones are skipped so in-place pop never breaks
   a probe chain. */
static int64_t eval_dict_find(uint8_t *table, int64_t capacity, const char *key) {
    if (!table || capacity <= 0 || !key) return -1;
    uint64_t h = eval_dict_hash(key);
    for (int64_t i = 0; i < capacity; i++) {
        int64_t slot = (int64_t)((h + (uint64_t)i) % (uint64_t)capacity);
        uint8_t *entry = table + BIR_DICT_HEADER + slot * BIR_DICT_STRIDE;
        uint8_t used = entry[0];
        if (used == 0) return -1;
        if (used == 1 && strcmp((char *)(entry + BIR_DICT_KEY_OFFSET), key) == 0)
            return slot;
    }
    return -1;
}

/* Insert or update key/value in a table with no tombstones. Returns the
   slot index or -1 if the table has no room. */
static int64_t eval_dict_insert(uint8_t *table, int64_t capacity,
                                const char *key, int64_t value) {
    if (!table || capacity <= 0 || !key) return -1;
    uint64_t h = eval_dict_hash(key);
    for (int64_t i = 0; i < capacity; i++) {
        int64_t slot = (int64_t)((h + (uint64_t)i) % (uint64_t)capacity);
        uint8_t *entry = table + BIR_DICT_HEADER + slot * BIR_DICT_STRIDE;
        uint8_t used = entry[0];
        if (used == 0 || used == 2) {
            entry[0] = 1;
            snprintf((char *)(entry + BIR_DICT_KEY_OFFSET),
                     COBRA_MAX_TOKEN_TEXT, "%s", key);
            memcpy(entry + BIR_DICT_VALUE_OFFSET, &value, sizeof(value));
            return slot;
        }
        if (used == 1 &&
            strcmp((char *)(entry + BIR_DICT_KEY_OFFSET), key) == 0) {
            memcpy(entry + BIR_DICT_VALUE_OFFSET, &value, sizeof(value));
            return slot;
        }
    }
    return -1;
}

/* Copy every live entry from source into a fresh empty table. Returns the
   number of copied entries. */
static int64_t eval_dict_copy(uint8_t *source_table, int64_t source_capacity,
                              uint8_t *destination, int64_t destination_capacity) {
    int64_t copied = 0;
    if (!source_table || !destination || source_capacity <= 0 ||
        destination_capacity <= 0) return 0;
    for (int64_t slot = 0; slot < source_capacity; slot++) {
        uint8_t *entry = source_table + BIR_DICT_HEADER +
                         slot * BIR_DICT_STRIDE;
        if (entry[0] != 1) continue;
        int64_t value = 0;
        memcpy(&value, entry + BIR_DICT_VALUE_OFFSET, sizeof(value));
        if (eval_dict_insert(destination, destination_capacity,
                             (char *)(entry + BIR_DICT_KEY_OFFSET), value) >= 0)
            copied++;
    }
    return copied;
}

bool bir_eval_function_value(const BackendIrModule *module, const char *name,
                             BirScalarValue *result) {
    if (!module || !name || !result) return false;
    const BirFunctionInfo *info = bir_find_function(module, name);
    if (!info) {
        fprintf(stderr, "backend-IR eval: unknown function '%s'\n", name);
        return false;
    }
    const SsaArena *arena = &module->arena;
    if (info->entry == SSA_BLOCK_NONE || info->entry >= arena->block_count) {
        fprintf(stderr, "backend-IR eval: invalid entry for '%s'\n", name);
        return false;
    }
    if (info->has_hidden_return_storage) {
        fprintf(stderr, "backend-IR eval: aggregate return '%s' requires caller storage\n", name);
        return false;
    }

    SsaEval *ev = calloc(1, sizeof(*ev));
    if (!ev) return false;
    ev->module = module;
    ev->slot_count = arena->value_count;
    ev->current_slots = calloc(ev->slot_count ? ev->slot_count : 1,
                                sizeof(BirScalarValue));
    ev->current_memory = calloc(BIR_STACK_BYTES, sizeof(uint8_t));
    ev->slice_memory = calloc(BIR_STACK_BYTES, sizeof(uint8_t));
    ev->current_frame_id = 1;
    ev->next_frame_id = 1;
    if (!ev->current_slots || !ev->current_memory || !ev->slice_memory) {
        free(ev->current_slots);
        free(ev->current_memory);
        free(ev->slice_memory);
        free(ev);
        return false;
    }

    if (!eval_bind_function_params(ev, info, NULL, 0)) {
        fprintf(stderr, "backend-IR eval: cannot bind params for '%s'\n", name);
        free(ev->current_slots);
        free(ev->current_memory);
        free(ev->slice_memory);
        free(ev);
        return false;
    }

    SsaBlockRef current = info->entry;
    size_t inst_index = 0;
    bool returned = false;
    BirScalarValue result_value = scalar_zero_for_type(info->return_type);

    while (!returned && !ev->failed) {
        if (++ev->steps > BIR_MAX_STEPS) {
            eval_fail(ev, "step limit exceeded");
            break;
        }
        const SsaBlock *block = &arena->blocks[current];
        if (inst_index >= block->inst_count) {
            eval_fail(ev, "block b%u ran past its terminator", current);
            break;
        }
        const SsaInst *inst = &arena->insts[block->insts[inst_index]];
        SsaBlockRef next_block = current;
        size_t next_inst = inst_index + 1;
        switch (inst->op) {
            case SSA_OP_CONST:
                if (inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count)
                    ev->current_slots[inst->result] = arena->values[inst->result].const_value;
                break;
            case SSA_OP_PARAM:
            case SSA_OP_BLOCK_ARG:
                break;
            case SSA_OP_STACK_SLOT:
                if (inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count) {
                    BirScalarValue pointer = bir_scalar_pointer_with_contract(
                        inst->type, ev->current_frame_id, inst->memory_offset,
                        inst->pointer_contract);
                    pointer.payload.pointer.allocation_id = inst->allocation_id;
                    pointer.payload.pointer.allocation_base_offset = inst->memory_offset;
                    pointer.payload.pointer.allocation_size = inst->memory_width;
                    pointer.payload.pointer.region_id = inst->region_id;
                    pointer.payload.pointer.origin = inst->pointer_origin;
                    if (inst->allocation_id != 0 && inst->allocation_id <= BIR_MAX_STACK_SLOTS) {
                        ev->allocation_live[inst->allocation_id] = true;
                        ev->allocation_regions[inst->allocation_id] = inst->region_id;
                    }
                    ev->current_slots[inst->result] = pointer;
                }
                break;
            case SSA_OP_REGION_ENTER:
                if (inst->region_id == BIR_REGION_NONE || inst->region_id >= BIR_MAX_REGIONS ||
                    (inst->parent_region_id != 0 && !ev->region_active[inst->parent_region_id])) {
                    eval_fail(ev, "region enter has an inactive parent");
                    break;
                }
                ev->region_active[inst->region_id] = true;
                break;
            case SSA_OP_REGION_EXIT:
                if (inst->region_id == BIR_REGION_NONE || inst->region_id >= BIR_MAX_REGIONS ||
                    !ev->region_active[inst->region_id]) {
                    eval_fail(ev, "region exit is not paired with an active region");
                    break;
                }
                for (size_t child = 1; child < ev->module->region_count; child++) {
                    const BirRegionInfo *child_info = &ev->module->regions[child];
                    if (child_info->declared && ev->region_active[child_info->id] &&
                        child_info->parent_id == inst->region_id) {
                        eval_fail(ev, "region exit has an active child region");
                        break;
                    }
                }
                if (ev->failed) break;
                ev->region_active[inst->region_id] = false;
                for (size_t allocation = 1; allocation <= BIR_MAX_STACK_SLOTS; allocation++) {
                    if (ev->allocation_regions[allocation] == inst->region_id) {
                        ev->allocation_live[allocation] = false;
                        ev->readonly_borrows[allocation] = 0;
                        ev->writable_borrows[allocation] = 0;
                    }
                }
                break;
            case SSA_OP_TRANSFER: {
                BirScalarValue source = eval_value(ev, arena->operands[inst->operand_start]);
                if (!eval_pointer_live(ev, source) ||
                    source.payload.pointer.contract != BIR_POINTER_CONTRACT_OWNED_REGION ||
                    inst->region_id == BIR_REGION_NONE || inst->region_id >= BIR_MAX_REGIONS ||
                    !ev->region_active[inst->region_id]) {
                    if (!ev->failed) eval_fail(ev, "invalid region ownership transfer");
                    break;
                }
                BirScalarValue moved = source;
                moved.type = inst->type;
                moved.payload.pointer.contract = BIR_POINTER_CONTRACT_OWNED_REGION;
                moved.payload.pointer.origin = BIR_POINTER_ORIGIN_REGION;
                moved.payload.pointer.region_id = inst->region_id;
                ev->allocation_regions[moved.payload.pointer.allocation_id] = inst->region_id;
                if (inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count)
                    ev->current_slots[inst->result] = moved;
                break;
            }
            case SSA_OP_VIEW_MAKE: {
                BirScalarValue pointer = eval_value(ev, arena->operands[inst->operand_start]);
                BirScalarValue length = eval_value(ev, arena->operands[inst->operand_start + 1]);
                int64_t available = pointer.payload.pointer.allocation_size != 0
                    ? pointer.payload.pointer.allocation_base_offset +
                      pointer.payload.pointer.allocation_size - pointer.payload.pointer.offset
                    : BIR_STACK_BYTES - pointer.payload.pointer.offset;
                bool within_existing_view = true;
                if (pointer.payload.pointer.view_element_width != 0 && inst->memory_type) {
                    int64_t old_end = pointer.payload.pointer.view_base_offset +
                        pointer.payload.pointer.view_length *
                            pointer.payload.pointer.view_element_width;
                    within_existing_view = pointer.payload.pointer.offset >=
                                               pointer.payload.pointer.view_base_offset &&
                                           pointer.payload.pointer.offset <= old_end &&
                                           length.payload.i64 >= 0 &&
                                           (int64_t)inst->memory_type->size > 0 &&
                                           length.payload.i64 <=
                                               (old_end - pointer.payload.pointer.offset) /
                                                   (int64_t)inst->memory_type->size;
                }
                uint32_t view_allocation = pointer.payload.pointer.allocation_id;
                if (view_allocation != 0 && view_allocation <= BIR_MAX_STACK_SLOTS) {
                    uint16_t active_readers = ev->readonly_borrows[view_allocation];
                    uint16_t active_writers = ev->writable_borrows[view_allocation];
                    if ((inst->pointer_contract == BIR_POINTER_CONTRACT_BORROW_WRITE &&
                         (active_readers || active_writers)) ||
                        (inst->pointer_contract == BIR_POINTER_CONTRACT_BORROW_READONLY &&
                         active_writers)) {
                        eval_fail(ev, "view construction conflicts with an active borrow");
                        break;
                    }
                    if (!eval_check_borrow_conflict(ev, pointer.payload.pointer,
                                                    inst->pointer_contract,
                                                    "view construction")) break;
                    if (inst->pointer_contract == BIR_POINTER_CONTRACT_BORROW_WRITE)
                        ev->writable_borrows[view_allocation]++;
                    else
                        ev->readonly_borrows[view_allocation]++;
                }
                if (pointer.kind != BIR_SCALAR_POINTER || length.payload.i64 < 0 ||
                    !eval_pointer_live(ev, pointer) ||
                    !bir_pointer_contract_readable(pointer.payload.pointer.contract) ||
                    !inst->memory_type || inst->memory_type->size == 0 || available < 0 ||
                    length.payload.i64 > available / (int64_t)inst->memory_type->size ||
                    !within_existing_view) {
                    if (view_allocation != 0 && view_allocation <= BIR_MAX_STACK_SLOTS) {
                        if (inst->pointer_contract == BIR_POINTER_CONTRACT_BORROW_WRITE)
                            ev->writable_borrows[view_allocation]--;
                        else
                            ev->readonly_borrows[view_allocation]--;
                    }
                    eval_fail(ev, "invalid borrowed view bounds or source pointer");
                    break;
                }
                BirPointerValue view_pointer = pointer.payload.pointer;
                view_pointer.contract = inst->pointer_contract;
                view_pointer.view_base_offset = pointer.payload.pointer.offset;
                view_pointer.view_length = length.payload.i64;
                view_pointer.view_element_width = (uint32_t)inst->memory_type->size;
                if (inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count)
                    ev->current_slots[inst->result] = bir_scalar_view(
                        inst->type, view_pointer, length.payload.i64);
                break;
            }
            case SSA_OP_VIEW_PTR: {
                BirScalarValue view = eval_value(ev, arena->operands[inst->operand_start]);
                BirScalarValue view_pointer_value = {0};
                view_pointer_value.type = inst->type;
                view_pointer_value.kind = BIR_SCALAR_POINTER;
                view_pointer_value.payload.pointer = view.payload.view.pointer;
                if (view.kind != BIR_SCALAR_VIEW ||
                    !eval_pointer_live(ev, view_pointer_value)) {
                    eval_fail(ev, "invalid slice pointer extraction");
                    break;
                }
                BirPointerValue pointer = view.payload.view.pointer;
                pointer.contract = inst->pointer_contract;
                pointer.view_base_offset = view.payload.view.pointer.view_base_offset;
                pointer.view_length = view.payload.view.length;
                if (pointer.view_element_width == 0 && inst->memory_type)
                    pointer.view_element_width = (uint32_t)inst->memory_type->size;
                if (inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count)
                    ev->current_slots[inst->result] = bir_scalar_pointer_with_contract(
                        inst->type, pointer.frame_id, pointer.offset, pointer.contract);
                if (inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count) {
                    ev->current_slots[inst->result].payload.pointer = pointer;
                    ev->current_slots[inst->result].type = inst->type;
                }
                break;
            }
            case SSA_OP_VIEW_LEN: {
                BirScalarValue view = eval_value(ev, arena->operands[inst->operand_start]);
                if (view.kind != BIR_SCALAR_VIEW) {
                    eval_fail(ev, "view length requires a readonly view");
                    break;
                }
                if (inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count)
                    ev->current_slots[inst->result] = bir_scalar_i64(inst->type,
                                                                      view.payload.view.length);
                break;
            }
            case SSA_OP_SLICE_ALLOC: {
                BirScalarValue length = eval_value(ev, arena->operands[inst->operand_start]);
        uint32_t allocation = eval_fresh_allocation(ev, inst->allocation_id);
        if (allocation == 0 || allocation > BIR_MAX_STACK_SLOTS ||
            length.payload.i64 < 0 ||
                    !inst->memory_type || inst->memory_type->size == 0 ||
                    length.payload.i64 > (int64_t)BIR_STACK_BYTES /
                                             (int64_t)inst->memory_type->size ||
                    ev->allocation_live[allocation] ||
                    (inst->region_id != BIR_REGION_NONE &&
                     (inst->region_id >= BIR_MAX_REGIONS ||
                      !ev->region_active[inst->region_id]))) {
                    eval_fail(ev, "invalid owned slice allocation");
                    break;
                }
                size_t bytes = (size_t)length.payload.i64 *
                               (size_t)inst->memory_type->size;
                size_t alignment = inst->memory_type->alignment;
                size_t start = ev->next_slice_offset;
                size_t remainder = start % alignment;
                if (remainder) start += alignment - remainder;
                if (bytes > BIR_STACK_BYTES || start > BIR_STACK_BYTES - bytes) {
                    eval_fail(ev, "owned slice allocation exceeds frame memory");
                    break;
                }
                ev->next_slice_offset = start + bytes;
                BirPointerValue pointer = {0};
                pointer.frame_id = ev->current_frame_id;
                pointer.allocation_id = allocation;
                pointer.region_id = inst->region_id;
                pointer.offset = (int64_t)start;
                pointer.allocation_base_offset = (int64_t)start;
                pointer.allocation_size = (uint32_t)bytes;
                pointer.view_base_offset = (int64_t)start;
                pointer.view_length = length.payload.i64;
                pointer.view_element_width = (uint32_t)inst->memory_type->size;
                pointer.contract = BIR_POINTER_CONTRACT_OWNED_SLICE;
                pointer.origin = inst->pointer_origin;
                ev->allocation_live[allocation] = true;
                ev->allocation_regions[allocation] = inst->region_id;
                ev->slice_allocations[allocation] = true;
                if (inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count)
                    ev->current_slots[inst->result] =
                        bir_scalar_buffer(inst->type, pointer, length.payload.i64,
                                          length.payload.i64);
                break;
            }
            case SSA_OP_BUFFER_ALLOC: {
                BirScalarValue length = eval_value(ev, arena->operands[inst->operand_start]);
        uint32_t allocation = eval_fresh_allocation(ev, inst->allocation_id);
        if (allocation == 0 || allocation > BIR_MAX_STACK_SLOTS ||
            length.payload.i64 < 0 || !inst->memory_type ||
                    inst->memory_type->size == 0 ||
                    length.payload.i64 > INT64_MAX / (int64_t)inst->memory_type->size ||
                    ev->allocation_live[allocation] ||
                    (inst->region_id != BIR_REGION_NONE &&
                     (inst->region_id >= BIR_MAX_REGIONS ||
                      !ev->region_active[inst->region_id]))) {
                    eval_fail(ev, "invalid owned buffer allocation");
                    break;
                }
                int64_t logical_length = length.payload.i64;
                int64_t capacity = logical_length < 8 ? 8 : logical_length;
                if ((uint64_t)capacity > BIR_STACK_BYTES / inst->memory_type->size) {
                    eval_fail(ev, "owned buffer allocation exceeds frame memory");
                    break;
                }
                size_t bytes = (size_t)capacity * inst->memory_type->size;
                size_t start = ev->next_slice_offset;
                size_t remainder = start % inst->memory_type->alignment;
                if (remainder) start += inst->memory_type->alignment - remainder;
                if (start > BIR_STACK_BYTES - bytes) {
                    eval_fail(ev, "owned buffer allocation exceeds frame memory");
                    break;
                }
                ev->next_slice_offset = start + bytes;
                BirPointerValue pointer = {0};
                pointer.frame_id = ev->current_frame_id;
                pointer.allocation_id = allocation;
                pointer.offset = (int64_t)start;
                pointer.allocation_base_offset = (int64_t)start;
                pointer.allocation_size = (uint32_t)bytes;
                pointer.view_base_offset = (int64_t)start;
                pointer.view_length = logical_length;
                pointer.view_element_width = (uint32_t)inst->memory_type->size;
                pointer.contract = BIR_POINTER_CONTRACT_OWNED_SLICE;
                pointer.origin = BIR_POINTER_ORIGIN_FRAME;
                ev->allocation_live[allocation] = true;
                ev->allocation_regions[allocation] = BIR_REGION_NONE;
                ev->slice_allocations[allocation] = true;
                if (inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count)
                    ev->current_slots[inst->result] =
                        bir_scalar_buffer(inst->type, pointer, logical_length, capacity);
                break;
            }
            case SSA_OP_BUFFER_APPEND: {
                BirScalarValue source = eval_value(ev, arena->operands[inst->operand_start]);
                BirScalarValue value = eval_value(ev, arena->operands[inst->operand_start + 1]);
                const CobraType *element = inst->memory_type;
                uint32_t source_allocation = source.kind == BIR_SCALAR_VIEW
                    ? source.payload.view.pointer.allocation_id : 0;
                bool aggregate_element = element &&
                    !cobra_type_is_scalar(element) &&
                    bir_type_is_value_only_struct(element);
                bool value_ok = aggregate_element
                    ? (value.kind == BIR_SCALAR_POINTER && value.type &&
                       value.type->generic_arg_count == 1 &&
                       value.type->generic_args[0] == element)
                    : value.type == element;
                if (source.kind != BIR_SCALAR_VIEW || !bir_is_owned_buffer_type(source.type) ||
                    !value_ok || source.payload.view.length < 0 ||
                    source.payload.view.capacity < source.payload.view.length ||
                    source_allocation == 0 || source_allocation > BIR_MAX_STACK_SLOTS ||
                    !ev->allocation_live[source_allocation] ||
                    ev->readonly_borrows[source_allocation] ||
                    ev->writable_borrows[source_allocation] ||
                    !element || element->size == 0) {
                    eval_fail(ev, "invalid owned buffer append");
                    break;
                }
                int64_t old_length = source.payload.view.length;
                int64_t old_capacity = source.payload.view.capacity;
                uint32_t allocation = eval_fresh_allocation(ev, inst->allocation_id);
                if (allocation == 0) {
                    eval_fail(ev, "owned buffer append has no free allocation identity");
                    break;
                }
                int64_t new_capacity = old_capacity >= 8 ? old_capacity * 2 : 8;
                if (new_capacity < old_length + 1) new_capacity = old_length + 1;
                if (new_capacity <= old_capacity ||
                    (uint64_t)new_capacity > BIR_STACK_BYTES / element->size) {
                    eval_fail(ev, "owned buffer append exceeds frame memory");
                    break;
                }
                size_t bytes = (size_t)new_capacity * element->size;
                size_t destination = ev->next_slice_offset;
                size_t remainder = destination % element->alignment;
                if (remainder) destination += element->alignment - remainder;
                if (destination > BIR_STACK_BYTES - bytes) {
                    eval_fail(ev, "owned buffer append exceeds frame memory");
                    break;
                }
                uint8_t *source_memory = NULL;
                if (!eval_resolve_memory(ev, source.payload.view.pointer, &source_memory)) {
                    eval_fail(ev, "owned buffer source is inactive");
                    break;
                }
                size_t old_bytes = (size_t)old_length * element->size;
                memcpy(ev->slice_memory + destination,
                       source_memory + source.payload.view.pointer.offset, old_bytes);
                BirPointerValue pointer = source.payload.view.pointer;
                pointer.frame_id = ev->current_frame_id;
                pointer.allocation_id = allocation;
                pointer.offset = (int64_t)destination;
                pointer.allocation_base_offset = (int64_t)destination;
                pointer.allocation_size = (uint32_t)bytes;
                pointer.view_base_offset = (int64_t)destination;
                pointer.view_length = old_length + 1;
                pointer.view_element_width = (uint32_t)element->size;
                pointer.contract = BIR_POINTER_CONTRACT_OWNED_SLICE;
                pointer.origin = BIR_POINTER_ORIGIN_FRAME;
                BirScalarValue element_pointer = {0};
                element_pointer.type = element;
                element_pointer.kind = BIR_SCALAR_POINTER;
                element_pointer.payload.pointer = pointer;
                element_pointer.payload.pointer.offset += old_length * (int64_t)element->size;
                SsaInst synthetic = *inst;
                synthetic.op = SSA_OP_STORE;
                synthetic.memory_type = element;
                synthetic.memory_width = (uint32_t)element->size;
                synthetic.memory_alignment = (uint32_t)element->alignment;
                synthetic.effect = SSA_EFFECT_WRITE;
                ev->allocation_live[allocation] = true;
                ev->allocation_regions[allocation] = BIR_REGION_NONE;
                ev->slice_allocations[allocation] = true;
                if (aggregate_element) {
                    uint8_t *destination_bytes = NULL;
                    uint8_t *source_bytes = NULL;
                    size_t destination_offset = 0;
                    size_t source_offset = 0;
                    if (!eval_memory_range(ev, element_pointer, element->size,
                                           element->alignment, &destination_bytes,
                                           &destination_offset) ||
                        !eval_memory_range(ev, value, element->size,
                                           element->alignment, &source_bytes,
                                           &source_offset)) break;
                    memcpy(destination_bytes + destination_offset,
                           source_bytes + source_offset, element->size);
                } else if (!eval_write_memory(ev, &synthetic, element_pointer, value)) break;
                ev->allocation_live[source_allocation] = false;
                ev->allocation_live[allocation] = true;
                ev->allocation_regions[allocation] = BIR_REGION_NONE;
                ev->slice_allocations[allocation] = true;
                ev->next_slice_offset = destination + bytes;
                if (inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count)
                    ev->current_slots[inst->result] =
                        bir_scalar_buffer(inst->type, pointer, old_length + 1,
                                          new_capacity);
                break;
            }
            case SSA_OP_BUFFER_POP: {
                BirScalarValue source = eval_value(ev, arena->operands[inst->operand_start]);
                const CobraType *element = inst->memory_type;
                uint32_t allocation = source.kind == BIR_SCALAR_VIEW
                    ? source.payload.view.pointer.allocation_id : 0;
                if (source.kind != BIR_SCALAR_VIEW || !bir_is_owned_buffer_type(source.type) ||
                    source.payload.view.length <= 0 || allocation == 0 ||
                    allocation > BIR_MAX_STACK_SLOTS || !ev->allocation_live[allocation] ||
                    !element) {
                    eval_fail(ev, "pop from an empty or inactive buffer");
                    break;
                }
                BirPointerValue pointer = source.payload.view.pointer;
                pointer.offset += (source.payload.view.length - 1) *
                                  (int64_t)element->size;
                BirScalarValue element_pointer = {0};
                element_pointer.type = element;
                element_pointer.kind = BIR_SCALAR_POINTER;
                element_pointer.payload.pointer = pointer;
                SsaInst synthetic = *inst;
                synthetic.op = SSA_OP_LOAD;
                synthetic.type = element;
                synthetic.memory_type = element;
                synthetic.memory_width = (uint32_t)element->size;
                synthetic.memory_alignment = (uint32_t)element->alignment;
                synthetic.effect = SSA_EFFECT_READ;
                BirScalarValue popped = {0};
                if (!eval_read_memory(ev, &synthetic, element_pointer, &popped)) break;
                SsaValueRef source_ref = arena->operands[inst->operand_start];
                if (source_ref < ev->slot_count) {
                    BirScalarValue updated = source;
                    updated.payload.view.length--;
                    updated.payload.view.pointer.view_length--;
                    ev->current_slots[source_ref] = updated;
                }
                if (inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count)
                    ev->current_slots[inst->result] = popped;
                break;
            }
            case SSA_OP_BUFFER_FREE: {
                BirScalarValue buffer = eval_value(ev, arena->operands[inst->operand_start]);
                uint32_t allocation = buffer.kind == BIR_SCALAR_VIEW
                    ? buffer.payload.view.pointer.allocation_id : 0;
                if (buffer.kind != BIR_SCALAR_VIEW || !bir_is_owned_buffer_type(buffer.type) ||
                    allocation == 0 || allocation > BIR_MAX_STACK_SLOTS ||
                    !ev->allocation_live[allocation] ||
                    ev->allocation_regions[allocation] != BIR_REGION_NONE ||
                    ev->readonly_borrows[allocation] || ev->writable_borrows[allocation]) {
                    eval_fail(ev, "owned buffer free requires a live unborrowed buffer");
                    break;
                }
                ev->allocation_live[allocation] = false;
                ev->slice_allocations[allocation] = false;
                break;
            }
            case SSA_OP_DICT_ALLOC: {
                BirScalarValue capacity_value =
                    eval_value(ev, arena->operands[inst->operand_start]);
                uint32_t allocation = eval_fresh_allocation(ev, inst->allocation_id);
                if (allocation == 0 || allocation > BIR_MAX_STACK_SLOTS ||
                    capacity_value.kind != BIR_SCALAR_I64 ||
                    capacity_value.payload.i64 < 0 ||
                    ev->allocation_live[allocation] ||
                    (inst->region_id != BIR_REGION_NONE &&
                     (inst->region_id >= BIR_MAX_REGIONS ||
                      !ev->region_active[inst->region_id]))) {
                    eval_fail(ev, "invalid owned dict allocation");
                    break;
                }
                int64_t capacity = capacity_value.payload.i64;
                if (capacity < 8) capacity = 8;
                if ((uint64_t)capacity >
                    (BIR_STACK_BYTES - BIR_DICT_HEADER) / BIR_DICT_STRIDE) {
                    eval_fail(ev, "owned dict allocation exceeds frame memory");
                    break;
                }
                size_t bytes = BIR_DICT_HEADER +
                               (size_t)capacity * BIR_DICT_STRIDE;
                size_t start = ev->next_slice_offset;
                size_t remainder = start % 8;
                if (remainder) start += 8 - remainder;
                if (start > BIR_STACK_BYTES - bytes) {
                    eval_fail(ev, "owned dict allocation exceeds frame memory");
                    break;
                }
                ev->next_slice_offset = start + bytes;
                memset(ev->slice_memory + start, 0, bytes);
                memcpy(ev->slice_memory + start, &capacity, sizeof(capacity));
                int64_t zero = 0;
                memcpy(ev->slice_memory + start + 8, &zero, sizeof(zero));
                BirPointerValue pointer = {0};
                pointer.frame_id = ev->current_frame_id;
                pointer.allocation_id = allocation;
                pointer.offset = (int64_t)start;
                pointer.allocation_base_offset = (int64_t)start;
                pointer.allocation_size = (uint32_t)bytes;
                pointer.view_base_offset = (int64_t)start;
                pointer.view_length = capacity;
                pointer.view_element_width = BIR_DICT_STRIDE;
                pointer.contract = BIR_POINTER_CONTRACT_OWNED_SLICE;
                pointer.origin = BIR_POINTER_ORIGIN_FRAME;
                ev->allocation_live[allocation] = true;
                ev->allocation_regions[allocation] = BIR_REGION_NONE;
                ev->slice_allocations[allocation] = true;
                if (inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count)
                    ev->current_slots[inst->result] =
                        bir_scalar_buffer(inst->type, pointer, 0, capacity);
                break;
            }
            case SSA_OP_DICT_SET: {
                BirScalarValue source = eval_value(ev, arena->operands[inst->operand_start]);
                BirScalarValue value = eval_value(ev, arena->operands[inst->operand_start + 1]);
                uint32_t source_allocation = source.kind == BIR_SCALAR_VIEW
                    ? source.payload.view.pointer.allocation_id : 0;
                if (source.kind != BIR_SCALAR_VIEW ||
                    !bir_is_owned_dict_type(source.type) ||
                    !value.type || !cobra_type_equal(value.type,
                                                     inst->memory_type) ||
                    inst->dict_key[0] == '\0' ||
                    source_allocation == 0 || source_allocation > BIR_MAX_STACK_SLOTS ||
                    !ev->allocation_live[source_allocation] ||
                    ev->readonly_borrows[source_allocation] ||
                    ev->writable_borrows[source_allocation] ||
                    source.payload.view.length < 0) {
                    eval_fail(ev, "invalid owned dict set");
                    break;
                }
                uint8_t *source_memory = NULL;
                if (!eval_resolve_memory(ev, source.payload.view.pointer,
                                         &source_memory)) {
                    eval_fail(ev, "owned dict source is inactive");
                    break;
                }
                size_t source_base = (size_t)source.payload.view.pointer.offset;
                int64_t source_capacity = 0;
                int64_t source_length = 0;
                memcpy(&source_capacity, source_memory + source_base, sizeof(source_capacity));
                memcpy(&source_length, source_memory + source_base + 8, sizeof(source_length));
                int64_t capacity = source_capacity;
                if (capacity <= 0 || source_length < 0 ||
                    source_length > source_capacity) {
                    eval_fail(ev, "owned dict source has an invalid table");
                    break;
                }
                /* Grow at a 0.7 load factor, matching the production
                   rehash threshold. The grown table is a fresh allocation. */
                if ((source_length + 1) * 10 >= capacity * 7)
                    capacity *= 2;
                if ((uint64_t)capacity >
                    (BIR_STACK_BYTES - BIR_DICT_HEADER) / BIR_DICT_STRIDE) {
                    eval_fail(ev, "owned dict set exceeds frame memory");
                    break;
                }
                size_t bytes = BIR_DICT_HEADER +
                               (size_t)capacity * BIR_DICT_STRIDE;
                uint32_t allocation = eval_fresh_allocation(ev, inst->allocation_id);
                if (allocation == 0) {
                    eval_fail(ev, "owned dict set has no free allocation identity");
                    break;
                }
                size_t destination = ev->next_slice_offset;
                size_t remainder = destination % 8;
                if (remainder) destination += 8 - remainder;
                if (destination > BIR_STACK_BYTES - bytes) {
                    eval_fail(ev, "owned dict set exceeds frame memory");
                    break;
                }
                memset(ev->slice_memory + destination, 0, bytes);
                memcpy(ev->slice_memory + destination, &capacity, sizeof(capacity));
                int64_t copied = eval_dict_copy(source_memory + source_base,
                                                source_capacity,
                                                ev->slice_memory + destination,
                                                capacity);
                if (value.kind != BIR_SCALAR_I64 &&
                    value.kind != BIR_SCALAR_U64 &&
                    value.kind != BIR_SCALAR_U32 &&
                    value.kind != BIR_SCALAR_I32) {
                    eval_fail(ev, "owned dict values must be integers");
                    break;
                }
                int64_t key_value = value.payload.i64;
                int64_t before = eval_dict_find(ev->slice_memory + destination,
                                                capacity, inst->dict_key) >= 0
                    ? 1 : 0;
                if (eval_dict_insert(ev->slice_memory + destination, capacity,
                                     inst->dict_key, key_value) < 0) {
                    eval_fail(ev, "owned dict set cannot insert its key");
                    break;
                }
                int64_t new_length = copied + (before ? 0 : 1);
                memcpy(ev->slice_memory + destination + 8, &new_length,
                       sizeof(new_length));
                BirPointerValue pointer = source.payload.view.pointer;
                pointer.frame_id = ev->current_frame_id;
                pointer.allocation_id = allocation;
                pointer.offset = (int64_t)destination;
                pointer.allocation_base_offset = (int64_t)destination;
                pointer.allocation_size = (uint32_t)bytes;
                pointer.view_base_offset = (int64_t)destination;
                pointer.view_length = capacity;
                pointer.contract = BIR_POINTER_CONTRACT_OWNED_SLICE;
                pointer.origin = BIR_POINTER_ORIGIN_FRAME;
                ev->allocation_live[source_allocation] = false;
                ev->slice_allocations[source_allocation] = false;
                ev->allocation_live[allocation] = true;
                ev->allocation_regions[allocation] = BIR_REGION_NONE;
                ev->slice_allocations[allocation] = true;
                ev->next_slice_offset = destination + bytes;
                if (inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count)
                    ev->current_slots[inst->result] =
                        bir_scalar_buffer(inst->type, pointer, new_length, capacity);
                break;
            }
            case SSA_OP_DICT_GET: {
                BirScalarValue dict = eval_value(ev, arena->operands[inst->operand_start]);
                BirScalarValue fallback = eval_value(ev, arena->operands[inst->operand_start + 1]);
                uint32_t allocation = dict.kind == BIR_SCALAR_VIEW
                    ? dict.payload.view.pointer.allocation_id : 0;
                if (dict.kind != BIR_SCALAR_VIEW ||
                    !bir_is_owned_dict_type(dict.type) ||
                    inst->dict_key[0] == '\0' ||
                    allocation == 0 || allocation > BIR_MAX_STACK_SLOTS ||
                    !ev->allocation_live[allocation]) {
                    eval_fail(ev, "dict get requires a live owned dict");
                    break;
                }
                uint8_t *memory = NULL;
                if (!eval_resolve_memory(ev, dict.payload.view.pointer, &memory)) {
                    eval_fail(ev, "dict get source is inactive");
                    break;
                }
                size_t base = (size_t)dict.payload.view.pointer.offset;
                int64_t capacity = 0;
                memcpy(&capacity, memory + base, sizeof(capacity));
                int64_t slot = eval_dict_find(memory + base, capacity,
                                              inst->dict_key);
                BirScalarValue result = fallback;
                if (slot >= 0) {
                    int64_t raw = 0;
                    memcpy(&raw, memory + base + BIR_DICT_HEADER +
                           slot * BIR_DICT_STRIDE + BIR_DICT_VALUE_OFFSET,
                           sizeof(raw));
                    result = bir_scalar_i64(inst->type, raw);
                }
                if (inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count)
                    ev->current_slots[inst->result] = result;
                break;
            }
            case SSA_OP_DICT_HAS: {
                BirScalarValue dict = eval_value(ev, arena->operands[inst->operand_start]);
                uint32_t allocation = dict.kind == BIR_SCALAR_VIEW
                    ? dict.payload.view.pointer.allocation_id : 0;
                if (dict.kind != BIR_SCALAR_VIEW ||
                    !bir_is_owned_dict_type(dict.type) ||
                    inst->dict_key[0] == '\0' ||
                    allocation == 0 || allocation > BIR_MAX_STACK_SLOTS ||
                    !ev->allocation_live[allocation]) {
                    eval_fail(ev, "dict has requires a live owned dict");
                    break;
                }
                uint8_t *memory = NULL;
                if (!eval_resolve_memory(ev, dict.payload.view.pointer, &memory)) {
                    eval_fail(ev, "dict has source is inactive");
                    break;
                }
                size_t base = (size_t)dict.payload.view.pointer.offset;
                int64_t capacity = 0;
                memcpy(&capacity, memory + base, sizeof(capacity));
                int64_t found = eval_dict_find(memory + base, capacity,
                                               inst->dict_key) >= 0 ? 1 : 0;
                if (inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count)
                    ev->current_slots[inst->result] =
                        bir_scalar_i64(inst->type, found);
                break;
            }
            case SSA_OP_DICT_DELETE: {
                BirScalarValue source = eval_value(ev, arena->operands[inst->operand_start]);
                uint32_t source_allocation = source.kind == BIR_SCALAR_VIEW
                    ? source.payload.view.pointer.allocation_id : 0;
                if (source.kind != BIR_SCALAR_VIEW ||
                    !bir_is_owned_dict_type(source.type) ||
                    inst->dict_key[0] == '\0' ||
                    source_allocation == 0 || source_allocation > BIR_MAX_STACK_SLOTS ||
                    !ev->allocation_live[source_allocation] ||
                    ev->readonly_borrows[source_allocation] ||
                    ev->writable_borrows[source_allocation] ||
                    source.payload.view.length < 0) {
                    eval_fail(ev, "invalid owned dict delete");
                    break;
                }
                uint8_t *source_memory = NULL;
                if (!eval_resolve_memory(ev, source.payload.view.pointer,
                                         &source_memory)) {
                    eval_fail(ev, "owned dict delete source is inactive");
                    break;
                }
                size_t source_base = (size_t)source.payload.view.pointer.offset;
                int64_t capacity = 0;
                int64_t source_length = 0;
                memcpy(&capacity, source_memory + source_base, sizeof(capacity));
                memcpy(&source_length, source_memory + source_base + 8,
                       sizeof(source_length));
                if (capacity <= 0 || source_length < 0 ||
                    source_length > capacity) {
                    eval_fail(ev, "owned dict delete has an invalid table");
                    break;
                }
                uint32_t allocation = eval_fresh_allocation(ev, inst->allocation_id);
                if (allocation == 0) {
                    eval_fail(ev, "owned dict delete has no free allocation identity");
                    break;
                }
                size_t bytes = BIR_DICT_HEADER +
                               (size_t)capacity * BIR_DICT_STRIDE;
                size_t destination = ev->next_slice_offset;
                size_t remainder = destination % 8;
                if (remainder) destination += 8 - remainder;
                if (destination > BIR_STACK_BYTES - bytes) {
                    eval_fail(ev, "owned dict delete exceeds frame memory");
                    break;
                }
                memset(ev->slice_memory + destination, 0, bytes);
                memcpy(ev->slice_memory + destination, &capacity, sizeof(capacity));
                int64_t copied = 0;
                for (int64_t slot = 0; slot < capacity; slot++) {
                    uint8_t *entry = source_memory + source_base +
                                     BIR_DICT_HEADER + slot * BIR_DICT_STRIDE;
                    if (entry[0] != 1) continue;
                    if (strcmp((char *)(entry + BIR_DICT_KEY_OFFSET),
                               inst->dict_key) == 0) continue;
                    int64_t value = 0;
                    memcpy(&value, entry + BIR_DICT_VALUE_OFFSET, sizeof(value));
                    if (eval_dict_insert(ev->slice_memory + destination, capacity,
                                         (char *)(entry + BIR_DICT_KEY_OFFSET),
                                         value) >= 0) copied++;
                }
                memcpy(ev->slice_memory + destination + 8, &copied,
                       sizeof(copied));
                BirPointerValue pointer = source.payload.view.pointer;
                pointer.frame_id = ev->current_frame_id;
                pointer.allocation_id = allocation;
                pointer.offset = (int64_t)destination;
                pointer.allocation_base_offset = (int64_t)destination;
                pointer.allocation_size = (uint32_t)bytes;
                pointer.view_base_offset = (int64_t)destination;
                pointer.view_length = capacity;
                pointer.contract = BIR_POINTER_CONTRACT_OWNED_SLICE;
                pointer.origin = BIR_POINTER_ORIGIN_FRAME;
                ev->allocation_live[source_allocation] = false;
                ev->slice_allocations[source_allocation] = false;
                ev->allocation_live[allocation] = true;
                ev->allocation_regions[allocation] = BIR_REGION_NONE;
                ev->slice_allocations[allocation] = true;
                ev->next_slice_offset = destination + bytes;
                if (inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count)
                    ev->current_slots[inst->result] =
                        bir_scalar_buffer(inst->type, pointer, copied, capacity);
                break;
            }
            case SSA_OP_DICT_POP: {
                BirScalarValue source = eval_value(ev, arena->operands[inst->operand_start]);
                BirScalarValue fallback = eval_value(ev, arena->operands[inst->operand_start + 1]);
                uint32_t allocation = source.kind == BIR_SCALAR_VIEW
                    ? source.payload.view.pointer.allocation_id : 0;
                if (source.kind != BIR_SCALAR_VIEW ||
                    !bir_is_owned_dict_type(source.type) ||
                    inst->dict_key[0] == '\0' ||
                    allocation == 0 || allocation > BIR_MAX_STACK_SLOTS ||
                    !ev->allocation_live[allocation] ||
                    ev->readonly_borrows[allocation] ||
                    ev->writable_borrows[allocation]) {
                    eval_fail(ev, "invalid owned dict pop");
                    break;
                }
                uint8_t *memory = NULL;
                if (!eval_resolve_memory(ev, source.payload.view.pointer, &memory)) {
                    eval_fail(ev, "owned dict pop source is inactive");
                    break;
                }
                size_t base = (size_t)source.payload.view.pointer.offset;
                int64_t capacity = 0;
                int64_t length = 0;
                memcpy(&capacity, memory + base, sizeof(capacity));
                memcpy(&length, memory + base + 8, sizeof(length));
                int64_t slot = eval_dict_find(memory + base, capacity,
                                              inst->dict_key);
                BirScalarValue result = fallback;
                if (slot >= 0) {
                    uint8_t *entry = memory + base + BIR_DICT_HEADER +
                                     slot * BIR_DICT_STRIDE;
                    int64_t raw = 0;
                    memcpy(&raw, entry + BIR_DICT_VALUE_OFFSET, sizeof(raw));
                    result = bir_scalar_i64(inst->type, raw);
                    entry[0] = 2; /* tombstone: keeps probe chains intact */
                    if (length > 0) {
                        length--;
                        memcpy(memory + base + 8, &length, sizeof(length));
                    }
                    if (inst->operand_start < ev->slot_count) {
                        BirScalarValue updated = source;
                        updated.payload.view.length = length;
                        updated.payload.view.pointer.view_length = length;
                        ev->current_slots[arena->operands[inst->operand_start]] =
                            updated;
                    }
                }
                if (inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count)
                    ev->current_slots[inst->result] = result;
                break;
            }
            case SSA_OP_DICT_LEN: {
                BirScalarValue dict = eval_value(ev, arena->operands[inst->operand_start]);
                uint32_t allocation = dict.kind == BIR_SCALAR_VIEW
                    ? dict.payload.view.pointer.allocation_id : 0;
                if (dict.kind != BIR_SCALAR_VIEW ||
                    !bir_is_owned_dict_type(dict.type) ||
                    allocation == 0 || allocation > BIR_MAX_STACK_SLOTS ||
                    !ev->allocation_live[allocation]) {
                    eval_fail(ev, "dict len requires a live owned dict");
                    break;
                }
                int64_t length = dict.payload.view.length;
                if (length < 0) length = 0;
                if (inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count)
                    ev->current_slots[inst->result] =
                        bir_scalar_i64(inst->type, length);
                break;
            }
            case SSA_OP_DICT_FREE: {
                BirScalarValue dict = eval_value(ev, arena->operands[inst->operand_start]);
                uint32_t allocation = dict.kind == BIR_SCALAR_VIEW
                    ? dict.payload.view.pointer.allocation_id : 0;
                if (dict.kind != BIR_SCALAR_VIEW ||
                    !bir_is_owned_dict_type(dict.type) ||
                    allocation == 0 || allocation > BIR_MAX_STACK_SLOTS ||
                    !ev->allocation_live[allocation] ||
                    ev->allocation_regions[allocation] != BIR_REGION_NONE ||
                    ev->readonly_borrows[allocation] ||
                    ev->writable_borrows[allocation]) {
                    eval_fail(ev, "owned dict free requires a live unborrowed dict");
                    break;
                }
                ev->allocation_live[allocation] = false;
                ev->slice_allocations[allocation] = false;
                break;
            }
            case SSA_OP_STRING_CONCAT: {
                BirScalarValue left = eval_value(ev, arena->operands[inst->operand_start]);
                BirScalarValue right = eval_value(ev, arena->operands[inst->operand_start + 1]);
                const CobraType *element = inst->memory_type;
                if (left.kind != BIR_SCALAR_VIEW || right.kind != BIR_SCALAR_VIEW ||
                    !element || element->size == 0 ||
                    left.payload.view.length < 0 || right.payload.view.length < 0 ||
                    left.payload.view.length > INT64_MAX - right.payload.view.length) {
                    eval_fail(ev, "invalid owned string concatenation");
                    break;
                }
                BirPointerValue sources[2] = {
                    left.payload.view.pointer, right.payload.view.pointer
                };
                int64_t lengths[2] = {
                    left.payload.view.length, right.payload.view.length
                };
                uint64_t total = (uint64_t)lengths[0] + (uint64_t)lengths[1];
                uint32_t allocation = eval_fresh_allocation(ev, inst->allocation_id);
                if (total > BIR_STACK_BYTES / element->size || allocation == 0) {
                    eval_fail(ev, "owned string allocation exceeds evaluator limits");
                    break;
                }
                size_t destination = ev->next_slice_offset;
                size_t remainder = destination % element->alignment;
                if (remainder) destination += element->alignment - remainder;
                size_t bytes = (size_t)total * element->size;
                if (destination > BIR_STACK_BYTES - bytes) {
                    eval_fail(ev, "owned string allocation exceeds frame memory");
                    break;
                }
                bool valid = true;
                for (size_t source_index = 0; source_index < 2; source_index++) {
                    BirScalarValue source_value = {0};
                    source_value.type = element;
                    source_value.kind = BIR_SCALAR_POINTER;
                    source_value.payload.pointer = sources[source_index];
                    if (!eval_pointer_live(ev, source_value) ||
                        !bir_pointer_contract_readable(sources[source_index].contract) ||
                        !eval_check_borrow_conflict(ev, sources[source_index],
                                                    sources[source_index].contract,
                                                    "string concatenation") ||
                        (lengths[source_index] > 0 &&
                         (sources[source_index].view_element_width != element->size ||
                          sources[source_index].offset < sources[source_index].view_base_offset ||
                          lengths[source_index] >
                              sources[source_index].view_length -
                              (sources[source_index].offset -
                               sources[source_index].view_base_offset) /
                                  (int64_t)element->size))) {
                        valid = false;
                        break;
                    }
                }
                if (!valid) {
                    if (!ev->failed) eval_fail(ev, "invalid owned string source view");
                    break;
                }
                size_t copied = 0;
                for (size_t source_index = 0; source_index < 2; source_index++) {
                    if (lengths[source_index] == 0) continue;
                    uint8_t *source_memory = NULL;
                    if (!eval_resolve_memory(ev, sources[source_index], &source_memory)) {
                        eval_fail(ev, "owned string source frame is inactive");
                        break;
                    }
                    size_t source_bytes = (size_t)lengths[source_index] * element->size;
                    memcpy(ev->slice_memory + destination + copied,
                           source_memory + sources[source_index].offset,
                           source_bytes);
                    copied += source_bytes;
                }
                if (ev->failed) break;
                BirPointerValue pointer = {0};
                pointer.frame_id = ev->current_frame_id;
                pointer.allocation_id = allocation;
                pointer.offset = (int64_t)destination;
                pointer.allocation_base_offset = (int64_t)destination;
                pointer.allocation_size = (uint32_t)bytes;
                pointer.view_base_offset = (int64_t)destination;
                pointer.view_length = (int64_t)total;
                pointer.view_element_width = (uint32_t)element->size;
                pointer.contract = BIR_POINTER_CONTRACT_OWNED_SLICE;
                pointer.origin = BIR_POINTER_ORIGIN_FRAME;
                ev->next_slice_offset = destination + bytes;
                ev->allocation_live[allocation] = true;
                ev->allocation_regions[allocation] = 0;
                ev->slice_allocations[allocation] = true;
                if (inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count)
                    ev->current_slots[inst->result] =
                        bir_scalar_view(inst->type, pointer, (int64_t)total);
                break;
            }
            case SSA_OP_STRING_EQ: {
                BirScalarValue left = eval_value(ev, arena->operands[inst->operand_start]);
                BirScalarValue right = eval_value(ev, arena->operands[inst->operand_start + 1]);
                if (left.kind != BIR_SCALAR_VIEW || right.kind != BIR_SCALAR_VIEW ||
                    left.payload.view.length < 0 || right.payload.view.length < 0) {
                    eval_fail(ev, "invalid string equality operands");
                    break;
                }
                bool equal = left.payload.view.length == right.payload.view.length;
                if (equal && left.payload.view.length > 0) {
                    uint8_t *left_memory = NULL;
                    uint8_t *right_memory = NULL;
                    BirScalarValue left_pointer_value = {0};
                    left_pointer_value.kind = BIR_SCALAR_POINTER;
                    left_pointer_value.payload.pointer = left.payload.view.pointer;
                    BirScalarValue right_pointer_value = {0};
                    right_pointer_value.kind = BIR_SCALAR_POINTER;
                    right_pointer_value.payload.pointer = right.payload.view.pointer;
                    if (!eval_pointer_live(ev, left_pointer_value) ||
                        !eval_pointer_live(ev, right_pointer_value) ||
                        !bir_pointer_contract_readable(left.payload.view.pointer.contract) ||
                        !bir_pointer_contract_readable(right.payload.view.pointer.contract) ||
                        !eval_resolve_memory(ev, left.payload.view.pointer, &left_memory) ||
                        !eval_resolve_memory(ev, right.payload.view.pointer, &right_memory)) {
                        eval_fail(ev, "invalid string equality source view");
                        break;
                    }
                    size_t bytes = (size_t)left.payload.view.length;
                    equal = memcmp(left_memory + left.payload.view.pointer.offset,
                                   right_memory + right.payload.view.pointer.offset,
                                   bytes) == 0;
                }
                if (ev->failed) break;
                if (inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count)
                    ev->current_slots[inst->result] = bir_scalar_bool(inst->type, equal);
                break;
            }
            case SSA_OP_SUM_PAYLOAD_STORE: {
                BirScalarValue destination = eval_value(ev, arena->operands[inst->operand_start]);
                BirScalarValue payload = eval_value(ev, arena->operands[inst->operand_start + 1]);
                const CobraType *sum_type = destination.type &&
                    destination.type->kind == COBRA_TYPE_POINTER &&
                    destination.type->generic_arg_count == 1
                    ? destination.type->generic_args[0] : NULL;
                uint8_t *memory = NULL;
                size_t base = 0;
                BirScalarValue payload_pointer = {0};
                payload_pointer.type = inst->memory_type;
                payload_pointer.kind = BIR_SCALAR_POINTER;
                payload_pointer.payload.pointer = payload.payload.view.pointer;
                if (payload.kind != BIR_SCALAR_VIEW ||
                    payload.payload.view.pointer.contract != BIR_POINTER_CONTRACT_OWNED_SLICE ||
                    !eval_pointer_live(ev, payload_pointer) ||
                    !eval_sum_storage(ev, destination, sum_type, true, &memory, &base) ||
                    inst->memory_offset < 0 ||
                    (size_t)inst->memory_offset + sizeof(uint64_t) * 2 > sum_type->size) {
                    if (!ev->failed) eval_fail(ev, "invalid owned sum payload store");
                    break;
                }
                size_t slot = base + (size_t)inst->memory_offset;
                uint64_t tag = eval_read_u64(memory, base);
                int active_selector =
                    eval_sum_active_selector(ev, sum_type, tag);
                if ((size_t)inst->memory_offset !=
                    bir_sum_component_offset(sum_type, active_selector)) {
                    eval_fail(ev, "owned sum payload store targets an inactive variant");
                    break;
                }
                if (eval_read_u64(memory, slot) != 0) {
                    eval_fail(ev, "owned sum payload slot is already occupied");
                    break;
                }
                uint32_t handle = 0;
                for (uint32_t candidate = 1; candidate < BIR_MAX_SUM_HANDLES; candidate++) {
                    if (!ev->sum_handles[candidate].live) {
                        handle = candidate;
                        break;
                    }
                }
                if (handle == 0) {
                    eval_fail(ev, "owned sum payload handle table is full");
                    break;
                }
                ev->sum_handles[handle].live = true;
                ev->sum_handles[handle].payload = payload;
                eval_write_u64(memory, slot, handle);
                eval_write_u64(memory, slot + sizeof(uint64_t),
                               (uint64_t)payload.payload.view.length);
                break;
            }
            case SSA_OP_SUM_PAYLOAD_LOAD: {
                BirScalarValue source = eval_value(ev, arena->operands[inst->operand_start]);
                const CobraType *sum_type = source.type &&
                    source.type->kind == COBRA_TYPE_POINTER &&
                    source.type->generic_arg_count == 1
                    ? source.type->generic_args[0] : NULL;
                uint8_t *memory = NULL;
                size_t base = 0;
                if (!eval_sum_storage(ev, source, sum_type, false, &memory, &base) ||
                    inst->memory_offset < 0 ||
                    (size_t)inst->memory_offset + sizeof(uint64_t) * 2 > sum_type->size) break;
                size_t slot = base + (size_t)inst->memory_offset;
                uint64_t tag = eval_read_u64(memory, base);
                int active_selector =
                    eval_sum_active_selector(ev, sum_type, tag);
                if ((size_t)inst->memory_offset !=
                    bir_sum_component_offset(sum_type, active_selector)) {
                    eval_fail(ev, "owned sum payload is inactive");
                    break;
                }
                uint64_t handle = eval_read_u64(memory, slot);
                if (handle == 0 || handle >= BIR_MAX_SUM_HANDLES ||
                    !ev->sum_handles[handle].live) {
                    eval_fail(ev, "owned sum payload is inactive");
                    break;
                }
                BirScalarValue payload = ev->sum_handles[handle].payload;
                ev->sum_handles[handle].live = false;
                eval_write_u64(memory, slot, 0);
                eval_write_u64(memory, slot + sizeof(uint64_t), 0);
                if (inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count)
                    ev->current_slots[inst->result] = payload;
                break;
            }
            case SSA_OP_SUM_MOVE: {
                BirScalarValue destination = eval_value(ev, arena->operands[inst->operand_start]);
                BirScalarValue source = eval_value(ev, arena->operands[inst->operand_start + 1]);
                const CobraType *sum_type = source.type &&
                    source.type->kind == COBRA_TYPE_POINTER &&
                    source.type->generic_arg_count == 1
                    ? source.type->generic_args[0] : NULL;
                uint8_t *destination_memory = NULL;
                uint8_t *source_memory = NULL;
                size_t destination_base = 0, source_base = 0;
                if (!eval_sum_storage(ev, destination, sum_type, true,
                                      &destination_memory, &destination_base) ||
                    !eval_sum_storage(ev, source, sum_type, false,
                                      &source_memory, &source_base)) break;
                if (bir_sum_has_owned_payload(sum_type) &&
                    eval_sum_storage_occupied(ev, destination_memory,
                                              destination_base, sum_type)) {
                    eval_fail(ev, "owning sum move overwrites a live payload");
                    break;
                }
                memcpy(destination_memory + destination_base,
                       source_memory + source_base, sum_type->size);
                /* The payload handle is intentionally preserved in the
                   destination and the complete source storage is cleared.
                   This transfers nested ownership without duplicating it. */
                memset(source_memory + source_base, 0, sum_type->size);
                break;
            }
            case SSA_OP_SUM_DROP: {
                BirScalarValue source = eval_value(ev, arena->operands[inst->operand_start]);
                const CobraType *sum_type = source.type &&
                    source.type->kind == COBRA_TYPE_POINTER &&
                    source.type->generic_arg_count == 1
                    ? source.type->generic_args[0] : NULL;
                uint8_t *memory = NULL;
                size_t base = 0;
                uint32_t sum_allocation = source.kind == BIR_SCALAR_POINTER
                    ? source.payload.pointer.allocation_id : 0;
                if (sum_allocation != 0 &&
                    !ev->allocation_live[sum_allocation]) {
                    eval_fail(ev, "double drop of owning sum");
                    break;
                }
                if (!eval_sum_storage(ev, source, sum_type, true, &memory, &base)) break;
                if (!eval_drop_sum_payload(ev, memory, base, sum_type)) break;
                memset(memory + base, 0, sum_type->size);
                if (sum_allocation != 0) ev->allocation_live[sum_allocation] = false;
                break;
            }
            case SSA_OP_FIELD_PAYLOAD_STORE: {
                BirScalarValue destination = eval_value(ev, arena->operands[inst->operand_start]);
                BirScalarValue payload = eval_value(ev, arena->operands[inst->operand_start + 1]);
                uint8_t *memory = NULL;
                size_t base = 0;
                BirScalarValue payload_pointer = {0};
                payload_pointer.type = inst->memory_type;
                payload_pointer.kind = BIR_SCALAR_POINTER;
                payload_pointer.payload.pointer = payload.payload.view.pointer;
                if (payload.kind != BIR_SCALAR_VIEW ||
                    !eval_pointer_live(ev, payload_pointer) ||
                    !eval_aggregate_storage(ev, destination, inst->aggregate_type, true,
                                             &memory, &base) ||
                    inst->memory_offset < 0 ||
                    (size_t)inst->memory_offset + sizeof(uint64_t) * 2 > inst->aggregate_type->size ||
                    eval_read_u64(memory, base + (size_t)inst->memory_offset) != 0 ||
                    ev->readonly_borrows[payload.payload.view.pointer.allocation_id] ||
                    ev->writable_borrows[payload.payload.view.pointer.allocation_id]) {
                    if (!ev->failed) eval_fail(ev, "invalid owned struct field store");
                    break;
                }
                uint32_t handle = 0;
                for (uint32_t candidate = 1; candidate < BIR_MAX_SUM_HANDLES; candidate++) {
                    if (!ev->sum_handles[candidate].live) {
                        handle = candidate;
                        break;
                    }
                }
                if (!handle) {
                    eval_fail(ev, "owned field handle table is full");
                    break;
                }
                ev->sum_handles[handle].live = true;
                ev->sum_handles[handle].payload = payload;
                size_t slot = base + (size_t)inst->memory_offset;
                eval_write_u64(memory, slot, handle);
                eval_write_u64(memory, slot + sizeof(uint64_t),
                               (uint64_t)payload.payload.view.length);
                break;
            }
            case SSA_OP_FIELD_PAYLOAD_LOAD: {
                BirScalarValue source = eval_value(ev, arena->operands[inst->operand_start]);
                uint8_t *memory = NULL;
                size_t base = 0;
                if (!eval_aggregate_storage(ev, source, inst->aggregate_type, false,
                                            &memory, &base) ||
                    inst->memory_offset < 0 ||
                    (size_t)inst->memory_offset + sizeof(uint64_t) * 2 > inst->aggregate_type->size) break;
                size_t slot = base + (size_t)inst->memory_offset;
                uint64_t handle = eval_read_u64(memory, slot);
                if (!handle || handle >= BIR_MAX_SUM_HANDLES ||
                    !ev->sum_handles[handle].live) {
                    eval_fail(ev, "owned struct field is inactive");
                    break;
                }
                BirScalarValue payload = ev->sum_handles[handle].payload;
                ev->sum_handles[handle].live = false;
                eval_write_u64(memory, slot, 0);
                eval_write_u64(memory, slot + sizeof(uint64_t), 0);
                if (inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count)
                    ev->current_slots[inst->result] = payload;
                break;
            }
            case SSA_OP_AGG_MOVE: {
                BirScalarValue destination = eval_value(ev, arena->operands[inst->operand_start]);
                BirScalarValue source = eval_value(ev, arena->operands[inst->operand_start + 1]);
                uint8_t *destination_memory = NULL;
                uint8_t *source_memory = NULL;
                size_t destination_base = 0, source_base = 0;
                if (!eval_aggregate_storage(ev, destination, inst->aggregate_type, true,
                                            &destination_memory, &destination_base) ||
                    !eval_aggregate_storage(ev, source, inst->aggregate_type, false,
                                            &source_memory, &source_base)) break;
                if (eval_owned_value_occupied(ev, destination_memory, destination_base,
                                              inst->aggregate_type)) {
                    eval_fail(ev, "aggregate move overwrites a live payload");
                    break;
                }
                memmove(destination_memory + destination_base,
                        source_memory + source_base, inst->aggregate_type->size);
                memset(source_memory + source_base, 0, inst->aggregate_type->size);
                break;
            }
            case SSA_OP_AGG_DROP: {
                BirScalarValue source = eval_value(ev, arena->operands[inst->operand_start]);
                uint8_t *memory = NULL;
                size_t base = 0;
                uint32_t allocation = source.kind == BIR_SCALAR_POINTER
                    ? source.payload.pointer.allocation_id : 0;
                if (allocation != 0 && !ev->allocation_live[allocation]) {
                    eval_fail(ev, "double drop of aggregate");
                    break;
                }
                if (!eval_aggregate_storage(ev, source, inst->aggregate_type, true,
                                             &memory, &base) ||
                    !eval_drop_owned_value(ev, memory, base, inst->aggregate_type)) break;
                memset(memory + base, 0, inst->aggregate_type->size);
                if (allocation != 0) ev->allocation_live[allocation] = false;
                break;
            }
            case SSA_OP_SLICE_FREE: {
                BirScalarValue slice = eval_value(ev, arena->operands[inst->operand_start]);
                uint32_t allocation = slice.payload.view.pointer.allocation_id;
                if (slice.kind != BIR_SCALAR_VIEW ||
                    slice.payload.view.pointer.contract != BIR_POINTER_CONTRACT_OWNED_SLICE ||
                    allocation == 0 || allocation > BIR_MAX_STACK_SLOTS ||
                    !ev->allocation_live[allocation]) {
                    eval_fail(ev, "owned slice free requires a live owned slice");
                    break;
                }
                if (ev->allocation_regions[allocation] != 0) {
                    eval_fail(ev, "cannot free a region-backed slice; the region releases it");
                    break;
                }
                if (ev->readonly_borrows[allocation] || ev->writable_borrows[allocation]) {
                    eval_fail(ev, "owned slice free while a borrowed view is active");
                    break;
                }
                ev->allocation_live[allocation] = false;
                ev->allocation_regions[allocation] = 0;
                break;
            }
            case SSA_OP_DESTROY: {
                BirScalarValue pointer = eval_value(ev, arena->operands[inst->operand_start]);
                if (!eval_pointer_live(ev, pointer) ||
                    (pointer.payload.pointer.contract != BIR_POINTER_CONTRACT_OWNED_FRAME &&
                     pointer.payload.pointer.contract != BIR_POINTER_CONTRACT_OWNED_REGION)) {
                    if (!ev->failed) eval_fail(ev, "destroy requires a live owned allocation");
                    break;
                }
                if (pointer.payload.pointer.allocation_id == 0 ||
                    pointer.payload.pointer.allocation_id > BIR_MAX_STACK_SLOTS ||
                    !ev->allocation_live[pointer.payload.pointer.allocation_id]) {
                    eval_fail(ev, "double destroy of allocation");
                    break;
                }
                ev->allocation_live[pointer.payload.pointer.allocation_id] = false;
                ev->readonly_borrows[pointer.payload.pointer.allocation_id] = 0;
                ev->writable_borrows[pointer.payload.pointer.allocation_id] = 0;
                break;
            }
            case SSA_OP_SUM_CHECK: {
                BirScalarValue tag = eval_value(ev, arena->operands[inst->operand_start]);
                if (tag.kind != BIR_SCALAR_I64) {
                    eval_fail(ev, "sum check requires an i64 tag value");
                    break;
                }
                int64_t expected = inst->sum_check_kind == 2 ? 0
                    : (inst->sum_check_kind == 3 ? inst->sum_check_expected : 1);
                if (tag.payload.i64 != expected) {
                    if (inst->sum_check_kind == 0)
                        eval_fail(ev, "unwrap called on none");
                    else if (inst->sum_check_kind == 1)
                        eval_fail(ev, "unwrap_ok called on err");
                    else if (inst->sum_check_kind == 2)
                        eval_fail(ev, "unwrap_err called on ok");
                    else
                        eval_fail(ev, "enum variant extraction failed");
                }
                break;
            }
            case SSA_OP_FIELD_ADDR: {
                BirScalarValue pointer = eval_value(ev, arena->operands[inst->operand_start]);
                if (!eval_validate_memory(ev, inst) || !eval_pointer_live(ev, pointer) ||
                    pointer.kind != BIR_SCALAR_POINTER ||
                    !bir_pointer_contract_readable(pointer.payload.pointer.contract) ||
                    pointer.payload.pointer.offset < 0 ||
                    pointer.payload.pointer.offset > BIR_STACK_BYTES - inst->memory_offset) {
                    if (!ev->failed) eval_fail(ev, "invalid aggregate field address");
                    break;
                }
                if (inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count) {
                    BirScalarValue field_pointer = bir_scalar_pointer_with_contract(
                        inst->type, pointer.payload.pointer.frame_id,
                        pointer.payload.pointer.offset + inst->memory_offset,
                        inst->pointer_contract);
                    field_pointer.payload.pointer.allocation_id = pointer.payload.pointer.allocation_id;
                    field_pointer.payload.pointer.allocation_base_offset = pointer.payload.pointer.allocation_base_offset;
                    field_pointer.payload.pointer.allocation_size = pointer.payload.pointer.allocation_size;
                    field_pointer.payload.pointer.region_id = pointer.payload.pointer.region_id;
                    field_pointer.payload.pointer.origin = pointer.payload.pointer.origin;
                    ev->current_slots[inst->result] = field_pointer;
                }
                break;
            }
            case SSA_OP_ARRAY_INDEX_ADDR: {
                BirScalarValue pointer = eval_value(ev, arena->operands[inst->operand_start]);
                BirScalarValue index_value = eval_value(ev, arena->operands[inst->operand_start + 1]);
                const CobraType *array_type = inst->memory_type;
                const CobraType *element = array_type && array_type->generic_arg_count == 1
                    ? array_type->generic_args[0] : NULL;
                int64_t index = index_value.payload.i64;
                if (pointer.kind != BIR_SCALAR_POINTER || !array_type ||
                    array_type->kind != COBRA_TYPE_ARRAY || !element ||
                    index < 0 || (uint64_t)index >= array_type->array_length ||
                    !eval_pointer_live(ev, pointer) ||
                    pointer.payload.pointer.contract == BIR_POINTER_CONTRACT_UNKNOWN ||
                    (uint64_t)index > (uint64_t)INT64_MAX / element->size ||
                    pointer.payload.pointer.offset >
                        INT64_MAX - index * (int64_t)element->size) {
                    eval_fail(ev, "fixed array index is out of bounds");
                    break;
                }
                if (inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count) {
                    BirScalarValue element_pointer = bir_scalar_pointer_with_contract(
                        inst->type, pointer.payload.pointer.frame_id,
                        pointer.payload.pointer.offset + index * (int64_t)element->size,
                        inst->pointer_contract);
                    element_pointer.payload.pointer.allocation_id = pointer.payload.pointer.allocation_id;
                    element_pointer.payload.pointer.allocation_base_offset = pointer.payload.pointer.allocation_base_offset;
                    element_pointer.payload.pointer.allocation_size = pointer.payload.pointer.allocation_size;
                    element_pointer.payload.pointer.region_id = pointer.payload.pointer.region_id;
                    element_pointer.payload.pointer.origin = pointer.payload.pointer.origin;
                    ev->current_slots[inst->result] = element_pointer;
                }
                break;
            }
            case SSA_OP_PTR_ADD: {
                BirScalarValue pointer = eval_value(ev, arena->operands[inst->operand_start]);
                int64_t delta = eval_value(ev, arena->operands[inst->operand_start + 1]).payload.i64;
                int64_t base = pointer.payload.pointer.offset;
                if (!eval_pointer_live(ev, pointer) || pointer.kind != BIR_SCALAR_POINTER ||
                    pointer.payload.pointer.contract == BIR_POINTER_CONTRACT_UNKNOWN ||
                    (delta > 0 && base > INT64_MAX - delta) ||
                    (delta < 0 && base < INT64_MIN - delta)) {
                    eval_fail(ev, "invalid pointer arithmetic");
                    break;
                }
                if (inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count) {
                    BirScalarValue offset_pointer = bir_scalar_pointer_with_contract(
                        inst->type, pointer.payload.pointer.frame_id, base + delta,
                        inst->pointer_contract);
                    offset_pointer.payload.pointer.allocation_id = pointer.payload.pointer.allocation_id;
                    offset_pointer.payload.pointer.allocation_base_offset = pointer.payload.pointer.allocation_base_offset;
                    offset_pointer.payload.pointer.allocation_size = pointer.payload.pointer.allocation_size;
                    offset_pointer.payload.pointer.region_id = pointer.payload.pointer.region_id;
                    offset_pointer.payload.pointer.origin = pointer.payload.pointer.origin;
                    offset_pointer.payload.pointer.view_base_offset = pointer.payload.pointer.view_base_offset;
                    offset_pointer.payload.pointer.view_length = pointer.payload.pointer.view_length;
                    offset_pointer.payload.pointer.view_element_width = pointer.payload.pointer.view_element_width;
                    ev->current_slots[inst->result] = offset_pointer;
                }
                break;
            }
            case SSA_OP_ADD:
            case SSA_OP_SUB:
            case SSA_OP_MUL:
            case SSA_OP_DIV:
            case SSA_OP_REM: {
                BirScalarValue lhs = eval_value(ev, arena->operands[inst->operand_start]);
                BirScalarValue rhs = eval_value(ev, arena->operands[inst->operand_start + 1]);
                BirScalarValue value = {0};
                if (lhs.kind == BIR_SCALAR_F32) {
                    float a = bir_scalar_as_f32(lhs), b = bir_scalar_as_f32(rhs), r = 0.0f;
                    if ((inst->op == SSA_OP_DIV || inst->op == SSA_OP_REM) && b == 0.0f) {
                        eval_fail(ev, "floating-point division by zero");
                        break;
                    }
                    if (inst->op == SSA_OP_ADD) r = a + b;
                    else if (inst->op == SSA_OP_SUB) r = a - b;
                    else if (inst->op == SSA_OP_MUL) r = a * b;
                    else if (inst->op == SSA_OP_DIV) r = a / b;
                    else { eval_fail(ev, "remainder is not supported for f32"); break; }
                    value = bir_scalar_f32(inst->type, r);
                } else if (lhs.kind == BIR_SCALAR_F64) {
                    double a = bir_scalar_as_f64(lhs), b = bir_scalar_as_f64(rhs), r = 0.0;
                    if ((inst->op == SSA_OP_DIV || inst->op == SSA_OP_REM) && b == 0.0) {
                        eval_fail(ev, "floating-point division by zero");
                        break;
                    }
                    if (inst->op == SSA_OP_ADD) r = a + b;
                    else if (inst->op == SSA_OP_SUB) r = a - b;
                    else if (inst->op == SSA_OP_MUL) r = a * b;
                    else if (inst->op == SSA_OP_DIV) r = a / b;
                    else { eval_fail(ev, "remainder is not supported for f64"); break; }
                    value = bir_scalar_f64(inst->type, r);
                } else {
                    /* Integer arithmetic is computed in unsigned domain so
                       wrapping is defined; the result is renormalized to the
                       operand kind (i64, i32 wrap, u8 wrap, u32 wrap, u64 wrap). */
                    uint64_t a = (uint64_t)lhs.payload.i64;
                    uint64_t b = (uint64_t)rhs.payload.i64;
                    uint64_t r = 0;
                    bool unsigned_div = lhs.kind == BIR_SCALAR_U8 ||
                                        lhs.kind == BIR_SCALAR_U32 ||
                                        lhs.kind == BIR_SCALAR_U64;
                    if ((inst->op == SSA_OP_DIV || inst->op == SSA_OP_REM) && b == 0) {
                        eval_fail(ev, "integer division by zero");
                        break;
                    }
                    if (inst->op == SSA_OP_ADD) r = a + b;
                    else if (inst->op == SSA_OP_SUB) r = a - b;
                    else if (inst->op == SSA_OP_MUL) r = a * b;
                    else if (unsigned_div) r = b ? a / b : 0;
                    else r = (uint64_t)((int64_t)a / (int64_t)b);
                    if (inst->op == SSA_OP_REM) {
                        r = unsigned_div ? (b ? a % b : 0)
                                         : (uint64_t)((int64_t)a % (int64_t)b);
                    }
                    if (lhs.kind == BIR_SCALAR_U8)
                        value = bir_scalar_u8(inst->type, (uint8_t)r);
                    else if (lhs.kind == BIR_SCALAR_I32)
                        value = bir_scalar_i32(inst->type, (int32_t)(uint32_t)r);
                    else if (lhs.kind == BIR_SCALAR_U32)
                        value = bir_scalar_u32(inst->type, (uint32_t)r);
                    else if (lhs.kind == BIR_SCALAR_U64)
                        value = bir_scalar_u64(inst->type, r);
                    else
                        value = bir_scalar_i64(inst->type, (int64_t)r);
                }
                if (!ev->failed && inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count)
                    ev->current_slots[inst->result] = value;
                break;
            }
            case SSA_OP_NEG: {
                BirScalarValue operand = eval_value(ev, arena->operands[inst->operand_start]);
                BirScalarValue value;
                if (operand.kind == BIR_SCALAR_F32)
                    value = bir_scalar_f32(inst->type, -bir_scalar_as_f32(operand));
                else if (operand.kind == BIR_SCALAR_F64)
                    value = bir_scalar_f64(inst->type, -bir_scalar_as_f64(operand));
                else if (operand.kind == BIR_SCALAR_I32)
                    value = bir_scalar_i32(inst->type, (int32_t)-(int32_t)operand.payload.i64);
                else if (operand.kind == BIR_SCALAR_U8)
                    value = bir_scalar_u8(inst->type, (uint8_t)-(uint8_t)operand.payload.i64);
                else if (operand.kind == BIR_SCALAR_U32)
                    value = bir_scalar_u32(inst->type, (uint32_t)-(uint32_t)operand.payload.i64);
                else if (operand.kind == BIR_SCALAR_U64)
                    value = bir_scalar_u64(inst->type, (uint64_t)-(uint64_t)operand.payload.i64);
                else
                    value = bir_scalar_i64(inst->type, -operand.payload.i64);
                if (inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count)
                    ev->current_slots[inst->result] = value;
                break;
            }
            case SSA_OP_CONVERT: {
                BirScalarValue operand = eval_value(ev, arena->operands[inst->operand_start]);
                bool from_float = operand.kind == BIR_SCALAR_F32 || operand.kind == BIR_SCALAR_F64;
                /* Two distinct integer readings are kept: as_signed matches
                   every non-u64 kind's true value (payload.i64 already holds
                   it, sign-extended by the constructors above), while
                   as_unsigned re-reads the same bits as unsigned so a u64
                   operand's magnitude survives when it doesn't fit int64_t. */
                double as_double = 0.0;
                int64_t as_signed = 0;
                uint64_t as_unsigned = 0;
                if (from_float) {
                    as_double = operand.kind == BIR_SCALAR_F32
                        ? (double)bir_scalar_as_f32(operand)
                        : bir_scalar_as_f64(operand);
                } else {
                    as_signed = operand.payload.i64;
                    as_unsigned = operand.kind == BIR_SCALAR_U64
                        ? (uint64_t)operand.payload.i64
                        : (uint64_t)as_signed;
                }
                bool from_u64 = operand.kind == BIR_SCALAR_U64;
                CobraTypeKind to = inst->type ? inst->type->kind : COBRA_TYPE_UNKNOWN;
                BirScalarValue value;
                switch (to) {
                    case COBRA_TYPE_BOOL:
                        value = bir_scalar_bool(inst->type, from_float ? as_double != 0.0 : as_signed != 0);
                        break;
                    case COBRA_TYPE_F32:
                        value = bir_scalar_f32(inst->type, from_float ? (float)as_double :
                                              (from_u64 ? (float)as_unsigned : (float)as_signed));
                        break;
                    case COBRA_TYPE_F64:
                        value = bir_scalar_f64(inst->type, from_float ? as_double :
                                              (from_u64 ? (double)as_unsigned : (double)as_signed));
                        break;
                    case COBRA_TYPE_I32:
                        value = bir_scalar_i32(inst->type, from_float ? (int32_t)(int64_t)as_double : (int32_t)as_signed);
                        break;
                    case COBRA_TYPE_U32:
                        value = bir_scalar_u32(inst->type, from_float ? (uint32_t)(int64_t)as_double : (uint32_t)as_signed);
                        break;
                    case COBRA_TYPE_U8:
                        value = bir_scalar_u8(inst->type, from_float ? (uint8_t)(int64_t)as_double : (uint8_t)as_signed);
                        break;
                    case COBRA_TYPE_U64:
                        value = bir_scalar_u64(inst->type, from_float ? (uint64_t)(int64_t)as_double : as_unsigned);
                        break;
                    case COBRA_TYPE_I64:
                    default:
                        value = bir_scalar_i64(inst->type, from_float ? (int64_t)as_double : as_signed);
                        break;
                }
                if (inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count)
                    ev->current_slots[inst->result] = value;
                break;
            }
            case SSA_OP_EQ:
            case SSA_OP_NE:
            case SSA_OP_LT:
            case SSA_OP_LE:
            case SSA_OP_GT:
            case SSA_OP_GE: {
                BirScalarValue lhs = eval_value(ev, arena->operands[inst->operand_start]);
                BirScalarValue rhs = eval_value(ev, arena->operands[inst->operand_start + 1]);
                bool cmp;
                if (lhs.kind == BIR_SCALAR_F32) {
                    float a = bir_scalar_as_f32(lhs), b = bir_scalar_as_f32(rhs);
                    if (inst->op == SSA_OP_EQ) cmp = a == b;
                    else if (inst->op == SSA_OP_NE) cmp = a != b;
                    else if (inst->op == SSA_OP_LT) cmp = a < b;
                    else if (inst->op == SSA_OP_LE) cmp = a <= b;
                    else if (inst->op == SSA_OP_GT) cmp = a > b;
                    else cmp = a >= b;
                } else if (lhs.kind == BIR_SCALAR_F64) {
                    double a = bir_scalar_as_f64(lhs), b = bir_scalar_as_f64(rhs);
                    if (inst->op == SSA_OP_EQ) cmp = a == b;
                    else if (inst->op == SSA_OP_NE) cmp = a != b;
                    else if (inst->op == SSA_OP_LT) cmp = a < b;
                    else if (inst->op == SSA_OP_LE) cmp = a <= b;
                    else if (inst->op == SSA_OP_GT) cmp = a > b;
                    else cmp = a >= b;
                } else if (lhs.kind == BIR_SCALAR_U8 || lhs.kind == BIR_SCALAR_U32 ||
                           lhs.kind == BIR_SCALAR_U64) {
                    uint64_t a = (uint64_t)lhs.payload.i64, b = (uint64_t)rhs.payload.i64;
                    if (inst->op == SSA_OP_EQ) cmp = a == b;
                    else if (inst->op == SSA_OP_NE) cmp = a != b;
                    else if (inst->op == SSA_OP_LT) cmp = a < b;
                    else if (inst->op == SSA_OP_LE) cmp = a <= b;
                    else if (inst->op == SSA_OP_GT) cmp = a > b;
                    else cmp = a >= b;
                } else {
                    /* Signed comparisons: i32 payloads are sign-extended. */
                    int64_t a = lhs.payload.i64, b = rhs.payload.i64;
                    if (inst->op == SSA_OP_EQ) cmp = a == b;
                    else if (inst->op == SSA_OP_NE) cmp = a != b;
                    else if (inst->op == SSA_OP_LT) cmp = a < b;
                    else if (inst->op == SSA_OP_LE) cmp = a <= b;
                    else if (inst->op == SSA_OP_GT) cmp = a > b;
                    else cmp = a >= b;
                }
                if (inst->result != SSA_VALUE_NONE && inst->result < ev->slot_count)
                    ev->current_slots[inst->result] = bir_scalar_bool(inst->type, cmp);
                break;
            }
            case SSA_OP_LOAD: {
                BirScalarValue value = {0};
                if (eval_read_memory(ev, inst,
                                     eval_value(ev, arena->operands[inst->operand_start]),
                                     &value) && inst->result != SSA_VALUE_NONE &&
                    inst->result < ev->slot_count)
                    ev->current_slots[inst->result] = value;
                break;
            }
            case SSA_OP_STORE:
                eval_write_memory(ev, inst,
                                  eval_value(ev, arena->operands[inst->operand_start]),
                                  eval_value(ev, arena->operands[inst->operand_start + 1]));
                break;
            case SSA_OP_AGG_COPY: {
                if (!eval_validate_memory(ev, inst)) break;
                uint8_t *destination = NULL;
                uint8_t *source = NULL;
                size_t destination_offset = 0;
                size_t source_offset = 0;
                BirScalarValue dst = eval_value(ev, arena->operands[inst->operand_start]);
                BirScalarValue src = eval_value(ev, arena->operands[inst->operand_start + 1]);
                if (dst.kind != BIR_SCALAR_POINTER ||
                    !bir_pointer_contract_writable(dst.payload.pointer.contract) ||
                    src.kind != BIR_SCALAR_POINTER ||
                    !bir_pointer_contract_readable(src.payload.pointer.contract)) {
                    eval_fail(ev, "aggregate copy violates source/destination borrow contracts");
                    break;
                }
                if (!eval_memory_range(ev, dst, inst->memory_width,
                                       inst->memory_alignment, &destination,
                                       &destination_offset) ||
                    !eval_memory_range(ev, src, inst->memory_width,
                                       inst->memory_alignment, &source,
                                       &source_offset)) break;
                memmove(destination + destination_offset, source + source_offset,
                        inst->memory_width);
                break;
            }
            case SSA_OP_CALL:
                if (!eval_call(ev, inst, &next_block, &next_inst)) ev->failed = true;
                break;
            case SSA_OP_JUMP:
            case SSA_OP_BRANCH:
            case SSA_OP_RETURN:
                if (!eval_terminator(ev, inst, current, &next_block, &next_inst,
                                     &returned, &result_value)) ev->failed = true;
                break;
            default:
                eval_fail(ev, "unknown opcode in evaluator");
                break;
        }
        if (ev->failed) break;
        current = next_block;
        inst_index = next_inst;
    }

    for (size_t i = 0; i < ev->depth; i++) {
        free(ev->stack[i].slots);
        free(ev->stack[i].memory);
        free(ev->stack[i].slice_memory);
    }
    bool ok = !ev->failed && returned;
    if (ok) *result = result_value;
    free(ev->current_slots);
    free(ev->current_memory);
    free(ev->slice_memory);
    free(ev);
    return ok;
}
