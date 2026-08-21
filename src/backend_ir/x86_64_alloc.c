/*
 * Allocated Linux x86-64 emitter.
 *
 * This path shares the bootstrap instruction strategy with x86_64.c but reads
 * virtual-register locations from MirAllocation. It keeps a small call staging
 * area so argument register cycles cannot corrupt values.
 */
#include "x86_64.h"
#include <stdarg.h>

#define X86_ALLOC_TEMP_COUNT MIR_MAX_OPERANDS
#define X86_ALLOC_STACK_SLOTS BIR_MAX_STACK_SLOTS
#define X86_ALLOC_CALLEE_SAVED_COUNT 5
#define X86_ALLOC_CALLEE_SAVED_BASE 6

typedef struct {
    const MirModule *module;
    const MirAllocation *allocation;
    const MirFunction *function;
    size_t function_index;
    FILE *out;
    int64_t *spill_offsets;
    int64_t *view_length_offsets;
    uint32_t view_fail_labels;
    int64_t memory_offsets[X86_ALLOC_STACK_SLOTS];
    bool memory_seen[X86_ALLOC_STACK_SLOTS];
    int64_t temp_offsets[X86_ALLOC_TEMP_COUNT];
    int64_t callee_saved_offsets[X86_ALLOC_CALLEE_SAVED_COUNT];
    uint32_t frame_size;
    uint32_t outgoing_size;
    uint32_t dict_key_labels;
    char *errbuf;
    size_t errbuf_size;
} X86AllocatedContext;

static void x86_alloc_error(char *buffer, size_t capacity, const char *fmt, ...) {
    if (!buffer || capacity == 0 || buffer[0]) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, capacity, fmt, args);
    va_end(args);
}

static uint32_t x86_alloc_align_up(uint32_t value, uint32_t alignment) {
    if (alignment <= 1) return value;
    uint32_t remainder = value % alignment;
    return remainder ? value + alignment - remainder : value;
}

static bool x86_alloc_is_float(MirMachineType type) {
    return type == MIR_TYPE_F32 || type == MIR_TYPE_F64;
}

static bool x86_alloc_is_integer(MirMachineType type) {
    return type == MIR_TYPE_I8 || type == MIR_TYPE_I32 || type == MIR_TYPE_U32 ||
           type == MIR_TYPE_I64 || type == MIR_TYPE_U64 || type == MIR_TYPE_BOOL ||
           type == MIR_TYPE_ADDRESS;
}

static bool x86_alloc_is_scalar(MirMachineType type) {
    return x86_alloc_is_float(type) || x86_alloc_is_integer(type);
}

static MirMachineType x86_alloc_machine_type_for_cobra(const CobraType *type) {
    if (!type) return MIR_TYPE_VOID;
    switch (type->kind) {
        case COBRA_TYPE_I64: return MIR_TYPE_I64;
        case COBRA_TYPE_I32: return MIR_TYPE_I32;
        case COBRA_TYPE_U32: return MIR_TYPE_U32;
        case COBRA_TYPE_U64: return MIR_TYPE_U64;
        case COBRA_TYPE_BOOL: return MIR_TYPE_BOOL;
        case COBRA_TYPE_F32: return MIR_TYPE_F32;
        case COBRA_TYPE_F64: return MIR_TYPE_F64;
        case COBRA_TYPE_U8: return MIR_TYPE_I8;
        case COBRA_TYPE_POINTER: return MIR_TYPE_ADDRESS;
        case COBRA_TYPE_ENUM: return MIR_TYPE_I64;
        default:
            if (cobra_type_is_slice_kind(type->kind) || bir_is_owned_buffer_type(type))
                return MIR_TYPE_VIEW;
            return MIR_TYPE_AGGREGATE;
    }
}

static bool x86_alloc_is_scalar_field_aggregate(const CobraType *type) {
    if (!type || !type->finalized) return false;
    if (type->kind == COBRA_TYPE_STRUCT) {
        if (type->field_count == 0) return false;
        for (size_t i = 0; i < type->field_count; i++) {
            const CobraType *field = type->fields[i].type;
            if (!field ||
                (!x86_alloc_is_scalar(x86_alloc_machine_type_for_cobra(field)) &&
                 !bir_type_has_owned_payload(field) &&
                 !x86_alloc_is_scalar_field_aggregate(field))) return false;
        }
        return true;
    }
    if (bir_is_sum_type(type)) {
        for (size_t i = 0; i < type->generic_arg_count; i++) {
            const CobraType *component = type->generic_args[i];
            if (!component) continue; /* unit enum variant */
            if (!x86_alloc_is_scalar(x86_alloc_machine_type_for_cobra(component)) &&
                !bir_is_owned_slice_type(component) &&
                !x86_alloc_is_scalar_field_aggregate(component)) return false;
        }
        return true;
    }
    if (type->kind == COBRA_TYPE_ARRAY) {
        return type->generic_arg_count == 1 && type->array_length > 0 &&
               type->array_length <= COBRA_MAX_ARRAY_ELEMENTS &&
               type->generic_args[0] &&
               (x86_alloc_is_scalar(x86_alloc_machine_type_for_cobra(type->generic_args[0])) ||
                x86_alloc_is_scalar_field_aggregate(type->generic_args[0]));
    }
    return false;
}

static bool x86_alloc_is_supported_memory_type(const CobraType *type) {
    MirMachineType machine = x86_alloc_machine_type_for_cobra(type);
    return type && (x86_alloc_is_scalar(machine) ||
                    x86_alloc_is_scalar_field_aggregate(type));
}

/* Indices 0..5 are the caller-saved SysV argument registers; indices 6..10
   are the callee-saved registers the allocator may hand out to intervals
   that cross a call. The prologue/epilogue save and restore whichever of
   the latter the allocator actually used for a given function. */
static bool x86_alloc_gpr(uint16_t index, const char **name) {
    static const char *const names[] = {
        "%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9",
        "%rbx", "%r12", "%r13", "%r14", "%r15"};
    if (!name || index >= sizeof(names) / sizeof(names[0])) return false;
    *name = names[index];
    return true;
}

static bool x86_alloc_gpr32(uint16_t index, const char **name) {
    static const char *const names[] = {
        "%edi", "%esi", "%edx", "%ecx", "%r8d", "%r9d",
        "%ebx", "%r12d", "%r13d", "%r14d", "%r15d"};
    if (!name || index >= sizeof(names) / sizeof(names[0])) return false;
    *name = names[index];
    return true;
}

static bool x86_alloc_callee_saved_name(uint16_t index, const char **name) {
    return x86_alloc_gpr(X86_ALLOC_CALLEE_SAVED_BASE + index, name);
}

static bool x86_alloc_xmm(uint16_t index, char *buffer, size_t capacity) {
    if (!buffer || capacity == 0 || index >= BIR_ABI_MAX_XMM_ARGUMENT_REGISTERS) return false;
    snprintf(buffer, capacity, "%%xmm%u", (unsigned)index);
    return true;
}

static void x86_alloc_mem(FILE *out, int64_t offset, const char *base) {
    fprintf(out, "%lld(%%%s)", (long long)offset, base);
}

static int64_t x86_alloc_spill(const X86AllocatedContext *ctx, MirReg reg) {
    return reg < ctx->module->arena.reg_count ? ctx->spill_offsets[reg] : 0;
}

static int64_t x86_alloc_view_length_spill(const X86AllocatedContext *ctx, MirReg reg) {
    return reg < ctx->module->arena.reg_count ? ctx->view_length_offsets[reg] : 0;
}

static const MirRegAllocation *x86_alloc_location(const X86AllocatedContext *ctx,
                                                   MirReg reg) {
    if (!ctx || reg == MIR_REG_NONE || reg >= ctx->allocation->reg_count) return NULL;
    return &ctx->allocation->regs[reg];
}

static bool x86_alloc_load_int(const X86AllocatedContext *ctx, MirReg reg,
                               const char *target64, const char *target32) {
    const MirRegAllocation *location = x86_alloc_location(ctx, reg);
    if (!location) return false;
    MirMachineType type = ctx->module->arena.regs[reg].machine_type;
    if (location->kind == MIR_ALLOC_REGISTER) {
        const char *source = NULL;
        if (!x86_alloc_gpr(location->register_index, &source)) return false;
        fprintf(ctx->out, "    ");
        if (type == MIR_TYPE_I8 || type == MIR_TYPE_I32 || type == MIR_TYPE_U32) {
            const char *source32 = NULL;
            if (!x86_alloc_gpr32(location->register_index, &source32)) return false;
            fprintf(ctx->out, "movl %s, %s\n", source32, target32);
        } else {
            fprintf(ctx->out, "movq %s, %s\n", source, target64);
        }
        return true;
    }
    if (location->kind != MIR_ALLOC_SPILL) return false;
    fprintf(ctx->out, "    ");
    if (type == MIR_TYPE_I8 || type == MIR_TYPE_BOOL) {
        fprintf(ctx->out, "movzbq ");
        x86_alloc_mem(ctx->out, x86_alloc_spill(ctx, reg), "rbp");
        fprintf(ctx->out, ", %s\n", target64);
    } else if (type == MIR_TYPE_I32 || type == MIR_TYPE_U32) {
        fprintf(ctx->out, "movl ");
        x86_alloc_mem(ctx->out, x86_alloc_spill(ctx, reg), "rbp");
        fprintf(ctx->out, ", %s\n", target32);
    } else {
        fprintf(ctx->out, "movq ");
        x86_alloc_mem(ctx->out, x86_alloc_spill(ctx, reg), "rbp");
        fprintf(ctx->out, ", %s\n", target64);
    }
    return true;
}

static bool x86_alloc_store_int(const X86AllocatedContext *ctx, MirReg reg,
                                const char *source64, const char *source32) {
    const MirRegAllocation *location = x86_alloc_location(ctx, reg);
    if (!location) return false;
    MirMachineType type = ctx->module->arena.regs[reg].machine_type;
    if (location->kind == MIR_ALLOC_REGISTER) {
        const char *destination = NULL;
        if (type == MIR_TYPE_I8 || type == MIR_TYPE_I32 || type == MIR_TYPE_U32) {
            if (!x86_alloc_gpr32(location->register_index, &destination)) return false;
            fprintf(ctx->out, "    movl %s, %s\n", source32, destination);
        } else {
            if (!x86_alloc_gpr(location->register_index, &destination)) return false;
            fprintf(ctx->out, "    movq %s, %s\n", source64, destination);
        }
        return true;
    }
    if (location->kind != MIR_ALLOC_SPILL) return false;
    fprintf(ctx->out, "    ");
    if (type == MIR_TYPE_I8 || type == MIR_TYPE_I32 || type == MIR_TYPE_U32) {
        fprintf(ctx->out, "movl %s, ", source32);
        x86_alloc_mem(ctx->out, x86_alloc_spill(ctx, reg), "rbp");
    } else {
        fprintf(ctx->out, "movq %s, ", source64);
        x86_alloc_mem(ctx->out, x86_alloc_spill(ctx, reg), "rbp");
    }
    fprintf(ctx->out, "\n");
    return true;
}

static bool x86_alloc_store_view_component(const X86AllocatedContext *ctx, MirReg reg,
                                                 bool length, const char *source) {
    const MirRegAllocation *location = x86_alloc_location(ctx, reg);
    if (!location || location->kind != MIR_ALLOC_SPILL ||
        ctx->module->arena.regs[reg].machine_type != MIR_TYPE_VIEW) return false;
    fprintf(ctx->out, "    movq %s, ", source);
    x86_alloc_mem(ctx->out, length ? x86_alloc_view_length_spill(ctx, reg)
                                  : x86_alloc_spill(ctx, reg), "rbp");
    fprintf(ctx->out, "\n");
    return true;
}

static bool x86_alloc_load_view_component(const X86AllocatedContext *ctx, MirReg reg,
                                            bool length, const char *target) {
    const MirRegAllocation *location = x86_alloc_location(ctx, reg);
    if (!location || location->kind != MIR_ALLOC_SPILL ||
        ctx->module->arena.regs[reg].machine_type != MIR_TYPE_VIEW) return false;
    fprintf(ctx->out, "    movq ");
    x86_alloc_mem(ctx->out, length ? x86_alloc_view_length_spill(ctx, reg)
                                  : x86_alloc_spill(ctx, reg), "rbp");
    fprintf(ctx->out, ", %s\n", target);
    return true;
}

static bool x86_alloc_load_float(const X86AllocatedContext *ctx, MirReg reg,
                                 const char *target) {
    const MirRegAllocation *location = x86_alloc_location(ctx, reg);
    if (!location) return false;
    MirMachineType type = ctx->module->arena.regs[reg].machine_type;
    const char *op = type == MIR_TYPE_F32 ? "movss" : "movsd";
    fprintf(ctx->out, "    %s ", op);
    if (location->kind == MIR_ALLOC_REGISTER) {
        char source[16];
        if (!x86_alloc_xmm(location->register_index, source, sizeof(source))) return false;
        fprintf(ctx->out, "%s", source);
    } else if (location->kind == MIR_ALLOC_SPILL) {
        x86_alloc_mem(ctx->out, x86_alloc_spill(ctx, reg), "rbp");
    } else return false;
    fprintf(ctx->out, ", %s\n", target);
    return true;
}

static bool x86_alloc_store_float(const X86AllocatedContext *ctx, MirReg reg,
                                  const char *source) {
    const MirRegAllocation *location = x86_alloc_location(ctx, reg);
    if (!location) return false;
    MirMachineType type = ctx->module->arena.regs[reg].machine_type;
    const char *op = type == MIR_TYPE_F32 ? "movss" : "movsd";
    fprintf(ctx->out, "    %s %s, ", op, source);
    if (location->kind == MIR_ALLOC_REGISTER) {
        char destination[16];
        if (!x86_alloc_xmm(location->register_index, destination, sizeof(destination))) return false;
        fprintf(ctx->out, "%s", destination);
    } else if (location->kind == MIR_ALLOC_SPILL) {
        x86_alloc_mem(ctx->out, x86_alloc_spill(ctx, reg), "rbp");
    } else return false;
    fprintf(ctx->out, "\n");
    return true;
}

static bool x86_alloc_prepare_frame(X86AllocatedContext *ctx) {
    const MirArena *arena = &ctx->module->arena;
    ctx->spill_offsets = calloc(arena->reg_count ? arena->reg_count : 1,
                                sizeof(*ctx->spill_offsets));
    ctx->view_length_offsets = calloc(arena->reg_count ? arena->reg_count : 1,
                                      sizeof(*ctx->view_length_offsets));
    if (!ctx->spill_offsets || !ctx->view_length_offsets) {
        free(ctx->spill_offsets);
        free(ctx->view_length_offsets);
        x86_alloc_error(ctx->errbuf, ctx->errbuf_size, "out of memory allocating allocated spill slots");
        return false;
    }
    uint32_t cursor = 0;
    for (MirReg reg = 1; reg < arena->reg_count; reg++) {
        if (arena->regs[reg].function_index != ctx->function_index) continue;
        const MirRegAllocation *location = &ctx->allocation->regs[reg];
        if (location->kind != MIR_ALLOC_SPILL) continue;
        cursor = x86_alloc_align_up(cursor, 8);
        cursor += 8;
        ctx->spill_offsets[reg] = -(int64_t)cursor;
        if (arena->regs[reg].machine_type == MIR_TYPE_VIEW) {
            cursor += 8;
            ctx->view_length_offsets[reg] = -(int64_t)cursor;
        }
    }
    for (size_t offset = 0; offset < ctx->function->block_count; offset++) {
        MirBlockRef block_ref = ctx->function->first_block + (MirBlockRef)offset;
        const MirBlock *block = &arena->blocks[block_ref];
        for (size_t i = 0; i < block->inst_count; i++) {
            const MirInst *inst = &arena->insts[block->insts[i]];
            if (inst->op == MIR_OP_CALL) {
                const MirFunction *callee = &ctx->module->functions[inst->callee_index];
                if (callee->call_abi.stack_size > ctx->outgoing_size)
                    ctx->outgoing_size = callee->call_abi.stack_size;
            }
            if (inst->op != MIR_OP_STACK_SLOT) continue;
            if (inst->stack_slot >= X86_ALLOC_STACK_SLOTS || !inst->memory_type ||
                !x86_alloc_is_supported_memory_type(inst->memory_type)) {
                x86_alloc_error(ctx->errbuf, ctx->errbuf_size,
                                "allocated x86-64 emitter supports only scalar and scalar-field aggregate stack slots");
                return false;
            }
            if (ctx->memory_seen[inst->stack_slot]) continue;
            uint32_t alignment = inst->memory_alignment ? inst->memory_alignment : 8;
            uint32_t size = inst->memory_width ? inst->memory_width : 8;
            cursor = x86_alloc_align_up(cursor, alignment > 8 ? 8 : alignment);
            cursor += x86_alloc_align_up(size, 8);
            ctx->memory_seen[inst->stack_slot] = true;
            ctx->memory_offsets[inst->stack_slot] = -(int64_t)cursor;
        }
    }
    for (size_t i = 0; i < X86_ALLOC_TEMP_COUNT; i++) {
        cursor += 8;
        ctx->temp_offsets[i] = -(int64_t)cursor;
    }
    const MirFunctionAllocation *function_allocation =
        &ctx->allocation->functions[ctx->function_index];
    for (size_t i = 0; i < X86_ALLOC_CALLEE_SAVED_COUNT; i++) {
        uint16_t reg_index = X86_ALLOC_CALLEE_SAVED_BASE + (uint16_t)i;
        if (function_allocation->used_gpr_mask & (UINT64_C(1) << reg_index)) {
            cursor += 8;
            ctx->callee_saved_offsets[i] = -(int64_t)cursor;
        } else {
            ctx->callee_saved_offsets[i] = 0;
        }
    }
    ctx->frame_size = x86_alloc_align_up(cursor + ctx->outgoing_size, 16);
    return true;
}

static void x86_alloc_emit_callee_saved_save(X86AllocatedContext *ctx) {
    const MirFunctionAllocation *function_allocation =
        &ctx->allocation->functions[ctx->function_index];
    for (size_t i = 0; i < X86_ALLOC_CALLEE_SAVED_COUNT; i++) {
        uint16_t reg_index = X86_ALLOC_CALLEE_SAVED_BASE + (uint16_t)i;
        if (!(function_allocation->used_gpr_mask & (UINT64_C(1) << reg_index))) continue;
        const char *name = NULL;
        if (!x86_alloc_callee_saved_name((uint16_t)i, &name)) continue;
        fprintf(ctx->out, "    movq %s, ", name);
        x86_alloc_mem(ctx->out, ctx->callee_saved_offsets[i], "rbp");
        fprintf(ctx->out, "\n");
    }
}

static void x86_alloc_emit_callee_saved_restore(X86AllocatedContext *ctx) {
    const MirFunctionAllocation *function_allocation =
        &ctx->allocation->functions[ctx->function_index];
    for (size_t i = 0; i < X86_ALLOC_CALLEE_SAVED_COUNT; i++) {
        uint16_t reg_index = X86_ALLOC_CALLEE_SAVED_BASE + (uint16_t)i;
        if (!(function_allocation->used_gpr_mask & (UINT64_C(1) << reg_index))) continue;
        const char *name = NULL;
        if (!x86_alloc_callee_saved_name((uint16_t)i, &name)) continue;
        fprintf(ctx->out, "    movq ");
        x86_alloc_mem(ctx->out, ctx->callee_saved_offsets[i], "rbp");
        fprintf(ctx->out, ", %s\n", name);
    }
}

static bool x86_alloc_emit_abi_move(X86AllocatedContext *ctx, const MirInst *inst) {
    if (inst->result == MIR_REG_NONE) return false;
    if (inst->machine_type == MIR_TYPE_VIEW) {
        /* Owned buffers are 3-part (ptr, len, cap); the native view model
           stores only ptr and len, and capacity is never read natively
           (append reallocates fresh), so the capacity part is discarded. */
        if (inst->abi_locations.count != 2 && inst->abi_locations.count != 3) return false;
        for (size_t part = 0; part < inst->abi_locations.count; part++) {
            const BirAbiLocation *location = &inst->abi_locations.parts[part];
            if (part >= 2) continue;
            if (location->storage == BIR_ABI_STORAGE_REGISTER) {
                const char *source = NULL;
                if (!x86_alloc_gpr(location->register_index, &source) ||
                    !x86_alloc_store_view_component(ctx, inst->result, part == 1, source))
                    return false;
            } else if (location->storage == BIR_ABI_STORAGE_STACK) {
                fprintf(ctx->out, "    movq ");
                x86_alloc_mem(ctx->out, 16 + location->stack_offset, "rbp");
                fprintf(ctx->out, ", %%r10\n");
                if (!x86_alloc_store_view_component(ctx, inst->result, part == 1, "%r10"))
                    return false;
            } else return false;
        }
        return true;
    }
    if (inst->abi_locations.count != 1 || !x86_alloc_is_scalar(inst->machine_type)) return false;
    const BirAbiLocation *location = &inst->abi_locations.parts[0];
    if (x86_alloc_is_float(inst->machine_type)) {
        if (location->storage == BIR_ABI_STORAGE_REGISTER) {
            char source[16];
            if (!x86_alloc_xmm(location->register_index, source, sizeof(source))) return false;
            return x86_alloc_store_float(ctx, inst->result, source);
        }
        if (location->storage == BIR_ABI_STORAGE_STACK) {
            fprintf(ctx->out, "    %s ", inst->machine_type == MIR_TYPE_F32 ? "movss" : "movsd");
            x86_alloc_mem(ctx->out, 16 + location->stack_offset, "rbp");
            fprintf(ctx->out, ", %%xmm14\n");
            return x86_alloc_store_float(ctx, inst->result, "%xmm14");
        }
        return false;
    }
    if (location->storage == BIR_ABI_STORAGE_REGISTER) {
        const char *source = NULL;
        if (!x86_alloc_gpr(location->register_index, &source)) return false;
        fprintf(ctx->out, "    movq %s, %%r10\n", source);
        return x86_alloc_store_int(ctx, inst->result, "%r10", "%r10d");
    }
    if (location->storage == BIR_ABI_STORAGE_STACK) {
        fprintf(ctx->out, "    movq ");
        x86_alloc_mem(ctx->out, 16 + location->stack_offset, "rbp");
        fprintf(ctx->out, ", %%r10\n");
        return x86_alloc_store_int(ctx, inst->result, "%r10", "%r10d");
    }
    return false;
}

static bool x86_alloc_emit_const(X86AllocatedContext *ctx, const MirInst *inst) {
    if (!inst->has_immediate || inst->result == MIR_REG_NONE) return false;
    if (inst->machine_type == MIR_TYPE_F32) {
        fprintf(ctx->out, "    movl $%u, %%r10d\n    movd %%r10d, %%xmm14\n",
                (unsigned)inst->immediate.payload.f32_bits);
        return x86_alloc_store_float(ctx, inst->result, "%xmm14");
    }
    if (inst->machine_type == MIR_TYPE_F64) {
        fprintf(ctx->out, "    movabsq $0x%llx, %%r10\n    movq %%r10, %%xmm14\n",
                (unsigned long long)inst->immediate.payload.f64_bits);
        return x86_alloc_store_float(ctx, inst->result, "%xmm14");
    }
    fprintf(ctx->out, "    movabsq $%lld, %%r10\n",
            (long long)inst->immediate.payload.i64);
    return x86_alloc_store_int(ctx, inst->result, "%r10", "%r10d");
}

static bool x86_alloc_emit_binary(X86AllocatedContext *ctx, const MirInst *inst) {
    MirReg lhs = ctx->module->arena.operands[inst->operand_start];
    MirReg rhs = ctx->module->arena.operands[inst->operand_start + 1];
    if (x86_alloc_is_float(inst->machine_type)) {
        x86_alloc_load_float(ctx, lhs, "%xmm14");
        x86_alloc_load_float(ctx, rhs, "%xmm15");
        const char *op = NULL;
        if (inst->op == MIR_OP_ADD) op = inst->machine_type == MIR_TYPE_F32 ? "addss" : "addsd";
        if (inst->op == MIR_OP_SUB) op = inst->machine_type == MIR_TYPE_F32 ? "subss" : "subsd";
        if (inst->op == MIR_OP_MUL) op = inst->machine_type == MIR_TYPE_F32 ? "mulss" : "mulsd";
        if (inst->op == MIR_OP_DIV) op = inst->machine_type == MIR_TYPE_F32 ? "divss" : "divsd";
        if (!op) return false;
        fprintf(ctx->out, "    %s %%xmm15, %%xmm14\n", op);
        return x86_alloc_store_float(ctx, inst->result, "%xmm14");
    }
    x86_alloc_load_int(ctx, lhs, "%r10", "%r10d");
    x86_alloc_load_int(ctx, rhs, "%r11", "%r11d");
    bool wide = inst->machine_type != MIR_TYPE_I8 &&
                inst->machine_type != MIR_TYPE_I32 && inst->machine_type != MIR_TYPE_U32;
    const char *op = inst->op == MIR_OP_ADD ? (wide ? "addq" : "addl") :
                     inst->op == MIR_OP_SUB ? (wide ? "subq" : "subl") :
                     inst->op == MIR_OP_MUL ? (wide ? "imulq" : "imull") : NULL;
    if (!op) return false;
    fprintf(ctx->out, "    %s %%r11%s, %%r10%s\n", op,
            wide ? "" : "d", wide ? "" : "d");
    return x86_alloc_store_int(ctx, inst->result, "%r10", "%r10d");
}

static bool x86_alloc_emit_neg(X86AllocatedContext *ctx, const MirInst *inst) {
    MirReg operand = ctx->module->arena.operands[inst->operand_start];
    if (x86_alloc_is_float(inst->machine_type)) {
        x86_alloc_load_float(ctx, operand, "%xmm14");
        if (inst->machine_type == MIR_TYPE_F32) {
            fprintf(ctx->out, "    movd %%xmm14, %%r10d\n    movl $0x80000000, %%r11d\n    xorl %%r11d, %%r10d\n    movd %%r10d, %%xmm14\n");
        } else {
            fprintf(ctx->out, "    movq %%xmm14, %%r10\n    movabsq $0x8000000000000000, %%r11\n    xorq %%r11, %%r10\n    movq %%r10, %%xmm14\n");
        }
        return x86_alloc_store_float(ctx, inst->result, "%xmm14");
    }
    if (!x86_alloc_load_int(ctx, operand, "%r10", "%r10d")) return false;
    bool narrow = inst->machine_type == MIR_TYPE_I8 ||
                  inst->machine_type == MIR_TYPE_I32 ||
                  inst->machine_type == MIR_TYPE_U32;
    fprintf(ctx->out, "    neg%s %%r10%s\n", narrow ? "l" : "q",
            narrow ? "d" : "");
    return x86_alloc_store_int(ctx, inst->result, "%r10", "%r10d");
}

/* See x86_emit_convert_widen in x86_64.c for why this can't reuse
   x86_alloc_load_int: that helper preserves each type's own storage width
   only, but a conversion needs the operand's true 64-bit value so widening
   and truncation both land on the mathematically correct bits. */
static void x86_alloc_convert_widen(X86AllocatedContext *ctx, MirReg reg,
                                    const char *target64, const char *target32) {
    const MirRegAllocation *location = x86_alloc_location(ctx, reg);
    MirMachineType type = ctx->module->arena.regs[reg].machine_type;
    if (!location) return;
    if (location->kind == MIR_ALLOC_REGISTER) {
        const char *source64 = NULL;
        const char *source32 = NULL;
        if (!x86_alloc_gpr(location->register_index, &source64)) return;
        if (!x86_alloc_gpr32(location->register_index, &source32)) return;
        if (type == MIR_TYPE_I8 || type == MIR_TYPE_BOOL) {
            /* Matches x86_alloc_load_int's own register-case precedent: a
               register-resident I8/BOOL is already a clean zero-extended
               32-bit value, so a plain 32-bit move (which zero-extends the
               target's upper 32 bits per the x86-64 ISA) is enough. */
            fprintf(ctx->out, "    movl %s, %s\n", source32, target32);
        } else if (type == MIR_TYPE_I32) {
            fprintf(ctx->out, "    movslq %s, %s\n", source32, target64);
        } else if (type == MIR_TYPE_U32) {
            fprintf(ctx->out, "    movl %s, %s\n", source32, target32);
        } else {
            fprintf(ctx->out, "    movq %s, %s\n", source64, target64);
        }
        return;
    }
    if (location->kind != MIR_ALLOC_SPILL) return;
    int64_t spill = x86_alloc_spill(ctx, reg);
    if (type == MIR_TYPE_I8 || type == MIR_TYPE_BOOL) {
        fprintf(ctx->out, "    movzbq ");
        x86_alloc_mem(ctx->out, spill, "rbp");
        fprintf(ctx->out, ", %s\n", target64);
    } else if (type == MIR_TYPE_I32) {
        fprintf(ctx->out, "    movslq ");
        x86_alloc_mem(ctx->out, spill, "rbp");
        fprintf(ctx->out, ", %s\n", target64);
    } else if (type == MIR_TYPE_U32) {
        fprintf(ctx->out, "    movl ");
        x86_alloc_mem(ctx->out, spill, "rbp");
        fprintf(ctx->out, ", %s\n", target32);
    } else {
        fprintf(ctx->out, "    movq ");
        x86_alloc_mem(ctx->out, spill, "rbp");
        fprintf(ctx->out, ", %s\n", target64);
    }
}

static bool x86_alloc_emit_convert(X86AllocatedContext *ctx, const MirInst *inst) {
    MirReg operand = ctx->module->arena.operands[inst->operand_start];
    MirMachineType from = ctx->module->arena.regs[operand].machine_type;
    MirMachineType to = inst->machine_type;
    if (x86_alloc_is_float(from) && x86_alloc_is_float(to)) {
        x86_alloc_load_float(ctx, operand, "%xmm14");
        if (from != to)
            fprintf(ctx->out, "    %s %%xmm14, %%xmm14\n", to == MIR_TYPE_F64 ? "cvtss2sd" : "cvtsd2ss");
        return x86_alloc_store_float(ctx, inst->result, "%xmm14");
    }
    if (x86_alloc_is_float(from)) {
        x86_alloc_load_float(ctx, operand, "%xmm14");
        if (to == MIR_TYPE_BOOL) {
            fprintf(ctx->out, "    pxor %%xmm15, %%xmm15\n    ucomis%s %%xmm15, %%xmm14\n"
                              "    setne %%al\n    movzbl %%al, %%r10d\n",
                    from == MIR_TYPE_F32 ? "s" : "d");
            return x86_alloc_store_int(ctx, inst->result, "%r10", "%r10d");
        }
        fprintf(ctx->out, "    %s %%xmm14, %%r10\n", from == MIR_TYPE_F32 ? "cvttss2si" : "cvttsd2si");
        return x86_alloc_store_int(ctx, inst->result, "%r10", "%r10d");
    }
    x86_alloc_convert_widen(ctx, operand, "%r10", "%r10d");
    if (x86_alloc_is_float(to)) {
        fprintf(ctx->out, "    %s %%r10, %%xmm14\n", to == MIR_TYPE_F32 ? "cvtsi2ss" : "cvtsi2sd");
        return x86_alloc_store_float(ctx, inst->result, "%xmm14");
    }
    if (to == MIR_TYPE_BOOL) {
        fprintf(ctx->out, "    testq %%r10, %%r10\n    setne %%al\n    movzbl %%al, %%r10d\n");
        return x86_alloc_store_int(ctx, inst->result, "%r10", "%r10d");
    }
    return x86_alloc_store_int(ctx, inst->result, "%r10", "%r10d");
}

static bool x86_alloc_emit_div(X86AllocatedContext *ctx, const MirInst *inst) {
    MirReg lhs = ctx->module->arena.operands[inst->operand_start];
    MirReg rhs = ctx->module->arena.operands[inst->operand_start + 1];
    bool wide = inst->machine_type != MIR_TYPE_I8 &&
                inst->machine_type != MIR_TYPE_I32 && inst->machine_type != MIR_TYPE_U32;
    bool unsigned_op = inst->machine_type == MIR_TYPE_I8 ||
                       inst->machine_type == MIR_TYPE_U32 ||
                       inst->machine_type == MIR_TYPE_U64;
    x86_alloc_load_int(ctx, lhs, "%rax", "%eax");
    x86_alloc_load_int(ctx, rhs, "%r10", "%r10d");
    if (wide) fprintf(ctx->out, unsigned_op ? "    xorq %%rdx, %%rdx\n" : "    cqto\n");
    else fprintf(ctx->out, unsigned_op ? "    xorl %%edx, %%edx\n" : "    cltd\n");
    fprintf(ctx->out, "    %s%c %%r10%s\n", unsigned_op ? "div" : "idiv",
            wide ? 'q' : 'l', wide ? "" : "d");
    return inst->op == MIR_OP_REM
        ? x86_alloc_store_int(ctx, inst->result, "%rdx", "%edx")
        : x86_alloc_store_int(ctx, inst->result, "%rax", "%eax");
}

static bool x86_alloc_emit_compare(X86AllocatedContext *ctx, const MirInst *inst) {
    MirReg lhs = ctx->module->arena.operands[inst->operand_start];
    MirReg rhs = ctx->module->arena.operands[inst->operand_start + 1];
    MirMachineType type = ctx->module->arena.regs[lhs].machine_type;
    if (x86_alloc_is_float(type)) {
        x86_alloc_load_float(ctx, lhs, "%xmm14");
        x86_alloc_load_float(ctx, rhs, "%xmm15");
        fprintf(ctx->out, "    ucomis%s %%xmm15, %%xmm14\n", type == MIR_TYPE_F32 ? "s" : "d");
        const char *ordered = NULL;
        const char *unordered = NULL;
        switch (inst->op) {
            case MIR_OP_EQ: ordered = "sete"; unordered = "setnp"; break;
            case MIR_OP_NE: ordered = "setne"; unordered = "setp"; break;
            case MIR_OP_LT: ordered = "setb"; unordered = "setnp"; break;
            case MIR_OP_LE: ordered = "setbe"; unordered = "setnp"; break;
            case MIR_OP_GT: ordered = "seta"; unordered = "setnp"; break;
            case MIR_OP_GE: ordered = "setae"; unordered = "setnp"; break;
            default: return false;
        }
        fprintf(ctx->out, "    %s %%al\n    %s %%dl\n", ordered, unordered);
        fprintf(ctx->out, inst->op == MIR_OP_NE ? "    orb %%dl, %%al\n" : "    andb %%dl, %%al\n");
    } else {
        x86_alloc_load_int(ctx, lhs, "%r10", "%r10d");
        x86_alloc_load_int(ctx, rhs, "%r11", "%r11d");
        bool wide = type != MIR_TYPE_I8 && type != MIR_TYPE_I32 && type != MIR_TYPE_U32;
        bool unsigned_op = type == MIR_TYPE_I8 || type == MIR_TYPE_U32 || type == MIR_TYPE_U64;
        fprintf(ctx->out, "    cmp%s %%r11%s, %%r10%s\n", wide ? "q" : "l",
                wide ? "" : "d", wide ? "" : "d");
        const char *condition = NULL;
        switch (inst->op) {
            case MIR_OP_EQ: condition = "sete"; break;
            case MIR_OP_NE: condition = "setne"; break;
            case MIR_OP_LT: condition = unsigned_op ? "setb" : "setl"; break;
            case MIR_OP_LE: condition = unsigned_op ? "setbe" : "setle"; break;
            case MIR_OP_GT: condition = unsigned_op ? "seta" : "setg"; break;
            case MIR_OP_GE: condition = unsigned_op ? "setae" : "setge"; break;
            default: return false;
        }
        fprintf(ctx->out, "    %s %%al\n", condition);
    }
    fprintf(ctx->out, "    movzbq %%al, %%r10\n");
    return x86_alloc_store_int(ctx, inst->result, "%r10", "%r10d");
}

static bool x86_alloc_emit_view_make(X86AllocatedContext *ctx, const MirInst *inst) {
    if (inst->machine_type != MIR_TYPE_VIEW || inst->operand_count != 2 ||
        inst->result == MIR_REG_NONE || !inst->memory_type ||
        inst->memory_type->size == 0) return false;
    MirReg pointer = ctx->module->arena.operands[inst->operand_start];
    MirReg length = ctx->module->arena.operands[inst->operand_start + 1];
    MirReg source = inst->view_source;
    char fail[64], done[64];
    uint32_t label = ctx->view_fail_labels++;
    snprintf(fail, sizeof(fail), ".Lcobra_alloc_view_fail_%zu_%u", ctx->function_index, label);
    snprintf(done, sizeof(done), ".Lcobra_alloc_view_done_%zu_%u", ctx->function_index, label);
    if (!x86_alloc_load_int(ctx, pointer, "%r10", "%r10d") ||
        !x86_alloc_load_int(ctx, length, "%r11", "%r11d")) return false;
    fprintf(ctx->out, "    movq %%r10, ");
    x86_alloc_mem(ctx->out, ctx->temp_offsets[0], "rbp");
    fprintf(ctx->out, "\n    cmpq $0, %%r11\n    jl %s\n", fail);
    if (source != MIR_REG_NONE) {
        if (!x86_alloc_load_view_component(ctx, source, false, "%rax") ||
            !x86_alloc_load_view_component(ctx, source, true, "%rdx")) return false;
        fprintf(ctx->out, "    cmpq %%rax, %%r10\n    jb %s\n", fail);
        fprintf(ctx->out, "    subq %%rax, %%r10\n    imulq $%u, %%rdx\n    movq %%r11, %%rcx\n    imulq $%u, %%rcx\n    addq %%rcx, %%r10\n    cmpq %%rdx, %%r10\n    ja %s\n",
                (unsigned)inst->memory_type->size,
                (unsigned)inst->memory_type->size, fail);
    }
    fprintf(ctx->out, "    movq ");
    x86_alloc_mem(ctx->out, ctx->temp_offsets[0], "rbp");
    fprintf(ctx->out, ", %%r10\n");
    if (!x86_alloc_store_view_component(ctx, inst->result, false, "%r10") ||
        !x86_alloc_store_view_component(ctx, inst->result, true, "%r11")) return false;
    fprintf(ctx->out, "    jmp %s\n%s:\n    ud2\n%s:\n", done, fail, done);
    return true;
}

static bool x86_alloc_emit_ptr_add(X86AllocatedContext *ctx, const MirInst *inst) {
    MirReg pointer = ctx->module->arena.operands[inst->operand_start];
    MirReg offset = ctx->module->arena.operands[inst->operand_start + 1];
    if (!x86_alloc_load_int(ctx, pointer, "%r10", "%r10d") ||
        !x86_alloc_load_int(ctx, offset, "%r11", "%r11d")) return false;
    if (inst->view_source != MIR_REG_NONE) {
        const CobraType *view_type = ctx->module->arena.regs[inst->view_source].type;
        const CobraType *element = view_type ? cobra_type_element(view_type) : NULL;
        if (!element || element->size == 0 ||
            !x86_alloc_load_view_component(ctx, inst->view_source, true, "%rdx")) return false;
        char fail[64], done[64];
        uint32_t label = ctx->view_fail_labels++;
        snprintf(fail, sizeof(fail), ".Lcobra_alloc_view_fail_%zu_%u", ctx->function_index, label);
        snprintf(done, sizeof(done), ".Lcobra_alloc_view_done_%zu_%u", ctx->function_index, label);
        fprintf(ctx->out, "    cmpq $0, %%r11\n    jl %s\n    imulq $%u, %%rdx\n    cmpq %%rdx, %%r11\n    ja %s\n    jmp %s\n%s:\n    ud2\n%s:\n",
                fail, (unsigned)element->size, fail, done, fail, done);
    }
    fprintf(ctx->out, "    addq %%r11, %%r10\n");
    return x86_alloc_store_int(ctx, inst->result, "%r10", "%r10d");
}

static bool x86_alloc_emit_array_index_addr(X86AllocatedContext *ctx, const MirInst *inst) {
    MirReg base = ctx->module->arena.operands[inst->operand_start];
    MirReg index = ctx->module->arena.operands[inst->operand_start + 1];
    if (!inst->memory_type || inst->memory_type->kind != COBRA_TYPE_ARRAY ||
        !inst->memory_type->generic_args[0] ||
        inst->memory_type->array_length == 0 ||
        inst->memory_width != inst->memory_type->generic_args[0]->size ||
        !x86_alloc_load_int(ctx, base, "%r10", "%r10d") ||
        !x86_alloc_load_int(ctx, index, "%r11", "%r11d")) return false;
    char fail[64], done[64];
    uint32_t label = ctx->view_fail_labels++;
    snprintf(fail, sizeof(fail), ".Lcobra_alloc_array_fail_%zu_%u", ctx->function_index, label);
    snprintf(done, sizeof(done), ".Lcobra_alloc_array_done_%zu_%u", ctx->function_index, label);
    fprintf(ctx->out, "    cmpq $0, %%r11\n    jl %s\n    cmpq $%llu, %%r11\n    jae %s\n    imulq $%u, %%r11\n    addq %%r11, %%r10\n    jmp %s\n%s:\n    ud2\n%s:\n",
            fail, (unsigned long long)inst->memory_type->array_length,
            fail, (unsigned)inst->memory_width, done, fail, done);
    return x86_alloc_store_int(ctx, inst->result, "%r10", "%r10d");
}

static bool x86_alloc_emit_memory(X86AllocatedContext *ctx, const MirInst *inst) {
    MirReg pointer = ctx->module->arena.operands[inst->operand_start];
    if (!x86_alloc_load_int(ctx, pointer, "%r10", "%r10d")) return false;
    if ((inst->op == MIR_OP_LOAD || inst->op == MIR_OP_STORE) &&
        inst->view_source != MIR_REG_NONE) {
        fprintf(ctx->out, "    movq %%r10, ");
        x86_alloc_mem(ctx->out, ctx->temp_offsets[0], "rbp");
        fprintf(ctx->out, "\n");
        const CobraType *view_type = ctx->module->arena.regs[inst->view_source].type;
        const CobraType *element = view_type ? cobra_type_element(view_type) : NULL;
        if (!element || element->size == 0 ||
            !x86_alloc_load_view_component(ctx, inst->view_source, false, "%rax") ||
            !x86_alloc_load_view_component(ctx, inst->view_source, true, "%rdx")) return false;
        char fail[64], done[64];
        uint32_t label = ctx->view_fail_labels++;
        snprintf(fail, sizeof(fail), ".Lcobra_alloc_view_fail_%zu_%u", ctx->function_index, label);
        snprintf(done, sizeof(done), ".Lcobra_alloc_view_done_%zu_%u", ctx->function_index, label);
        fprintf(ctx->out, "    cmpq %%rax, %%r10\n    jb %s\n    subq %%rax, %%r10\n    imulq $%u, %%rdx\n    addq $%u, %%r10\n    cmpq %%rdx, %%r10\n    ja %s\n    jmp %s\n%s:\n    ud2\n%s:\n",
                fail, (unsigned)element->size, (unsigned)inst->memory_width,
                fail, done, fail, done);
        fprintf(ctx->out, "    movq ");
        x86_alloc_mem(ctx->out, ctx->temp_offsets[0], "rbp");
        fprintf(ctx->out, ", %%r10\n");
    }
    if (inst->op == MIR_OP_LOAD) {
        MirReg result = inst->result;
        MirMachineType type = ctx->module->arena.regs[result].machine_type;
        if (x86_alloc_is_float(type)) {
            fprintf(ctx->out, "    mov%s (%%r10), %%xmm14\n", type == MIR_TYPE_F32 ? "ss" : "sd");
            return x86_alloc_store_float(ctx, result, "%xmm14");
        }
        if (type == MIR_TYPE_I8 || type == MIR_TYPE_BOOL)
            fprintf(ctx->out, "    movzbq (%%r10), %%r11\n");
        else if (type == MIR_TYPE_I32 || type == MIR_TYPE_U32)
            fprintf(ctx->out, "    movl (%%r10), %%r11d\n");
        else fprintf(ctx->out, "    movq (%%r10), %%r11\n");
        return x86_alloc_store_int(ctx, result, "%r11", "%r11d");
    }
    MirReg value = ctx->module->arena.operands[inst->operand_start + 1];
    MirMachineType type = ctx->module->arena.regs[value].machine_type;
    if (x86_alloc_is_float(type)) {
        x86_alloc_load_float(ctx, value, "%xmm14");
        fprintf(ctx->out, "    mov%s %%xmm14, (%%r10)\n", type == MIR_TYPE_F32 ? "ss" : "sd");
    } else {
        x86_alloc_load_int(ctx, value, "%r11", "%r11d");
        if (type == MIR_TYPE_I8 || type == MIR_TYPE_BOOL)
            fprintf(ctx->out, "    movb %%r11b, (%%r10)\n");
        else if (type == MIR_TYPE_I32 || type == MIR_TYPE_U32)
            fprintf(ctx->out, "    movl %%r11d, (%%r10)\n");
        else fprintf(ctx->out, "    movq %%r11, (%%r10)\n");
    }
    return true;
}

static bool x86_alloc_stage_call_arg(X86AllocatedContext *ctx, MirReg value, size_t index) {
    if (index >= X86_ALLOC_TEMP_COUNT) return false;
    MirMachineType type = ctx->module->arena.regs[value].machine_type;
    if (x86_alloc_is_float(type)) {
        x86_alloc_load_float(ctx, value, "%xmm14");
        fprintf(ctx->out, "    %s %%xmm14, ", type == MIR_TYPE_F32 ? "movss" : "movsd");
    } else {
        x86_alloc_load_int(ctx, value, "%r10", "%r10d");
        fprintf(ctx->out, "    movq %%r10, ");
    }
    x86_alloc_mem(ctx->out, ctx->temp_offsets[index], "rbp");
    fprintf(ctx->out, "\n");
    return true;
}

static bool x86_alloc_emit_aggregate_copy(X86AllocatedContext *ctx, const MirInst *inst) {
    if (!inst->memory_type || !x86_alloc_is_scalar_field_aggregate(inst->memory_type) ||
        inst->operand_count != 2 || inst->memory_width == 0) return false;
    MirReg destination = ctx->module->arena.operands[inst->operand_start];
    MirReg source = ctx->module->arena.operands[inst->operand_start + 1];
    if (!x86_alloc_load_int(ctx, destination, "%r10", "%r10d") ||
        !x86_alloc_load_int(ctx, source, "%r11", "%r11d")) return false;
    uint32_t offset = 0;
    while (inst->memory_width - offset >= 8) {
        fprintf(ctx->out, "    movq %u(%%r11), %%rax\n    movq %%rax, %u(%%r10)\n",
                offset, offset);
        offset += 8;
    }
    if (inst->memory_width - offset >= 4) {
        fprintf(ctx->out, "    movl %u(%%r11), %%eax\n    movl %%eax, %u(%%r10)\n",
                offset, offset);
        offset += 4;
    }
    while (offset < inst->memory_width) {
        fprintf(ctx->out, "    movb %u(%%r11), %%al\n    movb %%al, %u(%%r10)\n",
                offset, offset);
        offset++;
    }
    return true;
}

static bool x86_alloc_zero_view(X86AllocatedContext *ctx, MirReg view);

static bool x86_alloc_emit_owned_payload_store(X86AllocatedContext *ctx, const MirInst *inst) {
    if (inst->operand_count != 2 || inst->memory_offset < 0) return false;
    MirReg destination = ctx->module->arena.operands[inst->operand_start];
    MirReg payload = ctx->module->arena.operands[inst->operand_start + 1];
    if (ctx->module->arena.regs[destination].machine_type != MIR_TYPE_ADDRESS ||
        ctx->module->arena.regs[payload].machine_type != MIR_TYPE_VIEW) return false;
    if (!x86_alloc_load_int(ctx, destination, "%r10", "%r10d") ||
        !x86_alloc_load_view_component(ctx, payload, false, "%r11") ||
        !x86_alloc_load_view_component(ctx, payload, true, "%rax")) return false;
    if (inst->memory_offset != 0)
        fprintf(ctx->out, "    addq $%lld, %%r10\n", (long long)inst->memory_offset);
    fprintf(ctx->out, "    movq %%r11, (%%r10)\n    movq %%rax, 8(%%r10)\n");
    return x86_alloc_zero_view(ctx, payload);
}

static bool x86_alloc_emit_owned_payload_load(X86AllocatedContext *ctx, const MirInst *inst) {
    if (inst->operand_count != 1 || inst->result == MIR_REG_NONE ||
        inst->memory_offset < 0) return false;
    MirReg source = ctx->module->arena.operands[inst->operand_start];
    if (ctx->module->arena.regs[source].machine_type != MIR_TYPE_ADDRESS ||
        ctx->module->arena.regs[inst->result].machine_type != MIR_TYPE_VIEW) return false;
    if (!x86_alloc_load_int(ctx, source, "%r10", "%r10d")) return false;
    if (inst->memory_offset != 0)
        fprintf(ctx->out, "    addq $%lld, %%r10\n", (long long)inst->memory_offset);
    fprintf(ctx->out, "    movq (%%r10), %%r11\n    movq 8(%%r10), %%rax\n");
    if (!x86_alloc_store_view_component(ctx, inst->result, false, "%r11") ||
        !x86_alloc_store_view_component(ctx, inst->result, true, "%rax")) return false;
    fprintf(ctx->out, "    movq $0, (%%r10)\n    movq $0, 8(%%r10)\n");
    return true;
}

static bool x86_alloc_emit_copy_bytes(X86AllocatedContext *ctx, MirReg destination,
                                      MirReg source, uint32_t width) {
    if (width == 0 || !x86_alloc_load_int(ctx, destination, "%r10", "%r10d") ||
        !x86_alloc_load_int(ctx, source, "%r11", "%r11d")) return false;
    uint32_t offset = 0;
    while (width - offset >= 8) {
        fprintf(ctx->out, "    movq %u(%%r11), %%rax\n    movq %%rax, %u(%%r10)\n",
                offset, offset);
        offset += 8;
    }
    if (width - offset >= 4) {
        fprintf(ctx->out, "    movl %u(%%r11), %%eax\n    movl %%eax, %u(%%r10)\n",
                offset, offset);
        offset += 4;
    }
    while (offset < width) {
        fprintf(ctx->out, "    movb %u(%%r11), %%al\n    movb %%al, %u(%%r10)\n",
                offset, offset);
        offset++;
    }
    return true;
}

static bool x86_alloc_emit_zero_bytes(X86AllocatedContext *ctx, MirReg source,
                                      uint32_t width) {
    if (width == 0 || !x86_alloc_load_int(ctx, source, "%r10", "%r10d")) return false;
    uint32_t offset = 0;
    while (width - offset >= 8) {
        fprintf(ctx->out, "    movq $0, %u(%%r10)\n", offset);
        offset += 8;
    }
    if (width - offset >= 4) {
        fprintf(ctx->out, "    movl $0, %u(%%r10)\n", offset);
        offset += 4;
    }
    while (offset < width) {
        fprintf(ctx->out, "    movb $0, %u(%%r10)\n", offset);
        offset++;
    }
    return true;
}

static bool x86_alloc_emit_drop_owned_value(X86AllocatedContext *ctx, MirReg source,
                                             const CobraType *type, size_t base_offset) {
    if (!type || !bir_type_has_owned_payload(type)) return true;
    if (bir_is_owned_slice_type(type)) {
        if (!x86_alloc_load_int(ctx, source, "%r10", "%r10d")) return false;
        if (base_offset != 0)
            fprintf(ctx->out, "    addq $%zu, %%r10\n", base_offset);
        char done[64];
        uint32_t label = ctx->view_fail_labels++;
        snprintf(done, sizeof(done), ".Lcobra_alloc_drop_done_%zu_%u", ctx->function_index, label);
        fprintf(ctx->out, "    movq (%%r10), %%r11\n    testq %%r11, %%r11\n    je %s\n    movq $0, (%%r10)\n    movq $0, 8(%%r10)\n    movq %%r11, %%rdi\n    call free@PLT\n%s:\n", done, done);
        return true;
    }
    if (type->kind == COBRA_TYPE_STRUCT) {
        for (size_t i = 0; i < type->field_count; i++) {
            if (!x86_alloc_emit_drop_owned_value(ctx, source, type->fields[i].type,
                                                 base_offset + type->fields[i].offset)) return false;
        }
        return true;
    }
    if (bir_is_sum_type(type)) {
        char none[64], done[64], second[64];
        uint32_t label = ctx->view_fail_labels++;
        snprintf(none, sizeof(none), ".Lcobra_alloc_sum_drop_none_%zu_%u", ctx->function_index, label);
        snprintf(done, sizeof(done), ".Lcobra_alloc_sum_drop_done_%zu_%u", ctx->function_index, label);
        snprintf(second, sizeof(second), ".Lcobra_alloc_sum_drop_second_%zu_%u", ctx->function_index, label);
        if (!x86_alloc_load_int(ctx, source, "%r10", "%r10d")) return false;
        if (base_offset != 0)
            fprintf(ctx->out, "    addq $%zu, %%r10\n", base_offset);
        fprintf(ctx->out, "    movq (%%r10), %%r11\n");
        if (type->kind == COBRA_TYPE_OPTION) {
            const CobraType *some = type->generic_args[0];
            fprintf(ctx->out, "    cmpq $1, %%r11\n    jne %s\n", none);
            if (!x86_alloc_emit_drop_owned_value(ctx, source, some,
                                                 base_offset + bir_sum_component_offset(type, 1))) return false;
            fprintf(ctx->out, "    jmp %s\n%s:\n", done, none);
        } else {
            const CobraType *ok = type->generic_args[0];
            const CobraType *error = type->generic_args[1];
            fprintf(ctx->out, "    cmpq $1, %%r11\n    jne %s\n", second);
            if (!x86_alloc_emit_drop_owned_value(ctx, source, ok,
                                                 base_offset + bir_sum_component_offset(type, 1))) return false;
            fprintf(ctx->out, "    jmp %s\n%s:\n", done, second);
            if (!x86_alloc_emit_drop_owned_value(ctx, source, error,
                                                 base_offset + bir_sum_component_offset(type, 2))) return false;
        }
        fprintf(ctx->out, "%s:\n", done);
        return true;
    }
    return true;
}

static bool x86_alloc_emit_owned_aggregate_move(X86AllocatedContext *ctx,
                                                const MirInst *inst) {
    if (inst->operand_count != 2 || !inst->memory_type ||
        inst->memory_width == 0) return false;
    MirReg destination = ctx->module->arena.operands[inst->operand_start];
    MirReg source = ctx->module->arena.operands[inst->operand_start + 1];
    return x86_alloc_emit_copy_bytes(ctx, destination, source, inst->memory_width) &&
           x86_alloc_emit_zero_bytes(ctx, source, inst->memory_width);
}

static bool x86_alloc_emit_owned_aggregate_drop(X86AllocatedContext *ctx,
                                                const MirInst *inst) {
    if (inst->operand_count != 1 || !inst->memory_type) return false;
    MirReg source = ctx->module->arena.operands[inst->operand_start];
    if (!x86_alloc_emit_drop_owned_value(ctx, source, inst->memory_type, 0)) return false;
    return x86_alloc_emit_zero_bytes(ctx, source, (uint32_t)inst->memory_type->size);
}

static bool x86_alloc_emit_sum_check(X86AllocatedContext *ctx, const MirInst *inst) {
    if (inst->operand_count != 1) return false;
    MirReg tag = ctx->module->arena.operands[inst->operand_start];
    int64_t expected = inst->sum_check_kind == 2 ? 0
        : (inst->sum_check_kind == 3 ? inst->sum_check_expected : 1);
    char fail[64], done[64];
    uint32_t label = ctx->view_fail_labels++;
    snprintf(fail, sizeof(fail), ".Lcobra_sum_fail_%zu_%u", ctx->function_index, label);
    snprintf(done, sizeof(done), ".Lcobra_sum_done_%zu_%u", ctx->function_index, label);
    if (!x86_alloc_load_int(ctx, tag, "%r10", "%r10d")) return false;
    fprintf(ctx->out, "    cmpq $%lld, %%r10\n    jne %s\n    jmp %s\n%s:\n    ud2\n%s:\n",
            (long long)expected, fail, done, fail, done);
    return true;
}

static bool x86_alloc_zero_view(X86AllocatedContext *ctx, MirReg view) {
    if (!x86_alloc_store_view_component(ctx, view, false, "$0") ||
        !x86_alloc_store_view_component(ctx, view, true, "$0")) return false;
    return true;
}

static bool x86_alloc_emit_slice_alloc(X86AllocatedContext *ctx, const MirInst *inst) {
    if (inst->result == MIR_REG_NONE || inst->operand_count != 1 ||
        inst->machine_type != MIR_TYPE_VIEW || !inst->memory_type ||
        inst->memory_type->size == 0) return false;
    MirReg length = ctx->module->arena.operands[inst->operand_start];
    char fail[64], done[64];
    uint32_t label = ctx->view_fail_labels++;
    snprintf(fail, sizeof(fail), ".Lcobra_alloc_fail_%zu_%u", ctx->function_index, label);
    snprintf(done, sizeof(done), ".Lcobra_alloc_done_%zu_%u", ctx->function_index, label);
    if (!x86_alloc_load_int(ctx, length, "%r10", "%r10d") ||
        !x86_alloc_store_view_component(ctx, inst->result, true, "%r10")) return false;
    fprintf(ctx->out, "    cmpq $0, %%r10\n    jl %s\n", fail);
    fprintf(ctx->out, "    imulq $%u, %%r10\n    movq %%r10, %%rdi\n    call malloc@PLT\n    testq %%rax, %%rax\n    je %s\n",
            (unsigned)inst->memory_type->size, fail);
    if (!x86_alloc_store_view_component(ctx, inst->result, false, "%rax")) return false;
    fprintf(ctx->out, "    jmp %s\n%s:\n    ud2\n%s:\n", done, fail, done);
    return true;
}

static bool x86_alloc_emit_buffer_alloc(X86AllocatedContext *ctx, const MirInst *inst) {
    if (inst->result == MIR_REG_NONE || inst->operand_count != 1 ||
        inst->machine_type != MIR_TYPE_VIEW || !inst->memory_type ||
        inst->memory_type->size == 0) return false;
    MirReg length = ctx->module->arena.operands[inst->operand_start];
    char fail[64], done[64], nonzero[64];
    uint32_t label = ctx->view_fail_labels++;
    snprintf(fail, sizeof(fail), ".Lcobra_alloc_buffer_fail_%zu_%u", ctx->function_index, label);
    snprintf(done, sizeof(done), ".Lcobra_alloc_buffer_done_%zu_%u", ctx->function_index, label);
    snprintf(nonzero, sizeof(nonzero), ".Lcobra_alloc_buffer_nonzero_%zu_%u", ctx->function_index, label);
    if (!x86_alloc_load_int(ctx, length, "%r10", "%r10d") ||
        !x86_alloc_store_view_component(ctx, inst->result, true, "%r10")) return false;
    fprintf(ctx->out, "    cmpq $0, %%r10\n    jl %s\n    jne %s\n    movq $1, %%r10\n%s:\n",
            fail, nonzero, nonzero);
    fprintf(ctx->out, "    imulq $%u, %%r10\n    movq %%r10, %%rdi\n    call malloc@PLT\n    testq %%rax, %%rax\n    je %s\n",
            (unsigned)inst->memory_type->size, fail);
    if (!x86_alloc_store_view_component(ctx, inst->result, false, "%rax")) return false;
    fprintf(ctx->out, "    jmp %s\n%s:\n    ud2\n%s:\n", done, fail, done);
    return true;
}

static bool x86_alloc_emit_buffer_append(X86AllocatedContext *ctx, const MirInst *inst) {
    if (inst->operand_count != 2 || inst->result == MIR_REG_NONE ||
        !inst->memory_type || inst->memory_type->size == 0) return false;
    MirReg source = ctx->module->arena.operands[inst->operand_start];
    MirReg value = ctx->module->arena.operands[inst->operand_start + 1];
    char fail[64], done[64];
    uint32_t label = ctx->view_fail_labels++;
    snprintf(fail, sizeof(fail), ".Lcobra_alloc_buffer_append_fail_%zu_%u", ctx->function_index, label);
    snprintf(done, sizeof(done), ".Lcobra_alloc_buffer_append_done_%zu_%u", ctx->function_index, label);
    if (!x86_alloc_load_view_component(ctx, source, false, "%r10") ||
        !x86_alloc_load_view_component(ctx, source, true, "%r11")) return false;
    fprintf(ctx->out, "    cmpq $0, %%r11\n    jl %s\n    movq %%r11, %%rax\n    incq %%rax\n    jo %s\n    movq %%rax, %%rcx\n", fail, fail);
    if (!x86_alloc_store_view_component(ctx, inst->result, true, "%rcx")) return false;
    fprintf(ctx->out, "    imulq $%u, %%rcx\n    movq %%rcx, %%rdi\n    call malloc@PLT\n    testq %%rax, %%rax\n    je %s\n", (unsigned)inst->memory_type->size, fail);
    if (!x86_alloc_store_view_component(ctx, inst->result, false, "%rax")) return false;
    if (!x86_alloc_load_view_component(ctx, inst->result, false, "%rdi") ||
        !x86_alloc_load_view_component(ctx, source, false, "%rsi") ||
        !x86_alloc_load_view_component(ctx, source, true, "%rdx")) return false;
    fprintf(ctx->out, "    imulq $%u, %%rdx\n    call memcpy@PLT\n", (unsigned)inst->memory_type->size);
    if (!x86_alloc_load_view_component(ctx, inst->result, false, "%r10") ||
        !x86_alloc_load_view_component(ctx, source, true, "%r11")) return false;
    fprintf(ctx->out, "    imulq $%u, %%r11\n    addq %%r11, %%r10\n", (unsigned)inst->memory_type->size);
    if (bir_type_is_value_only_struct(inst->memory_type)) {
        if (!x86_alloc_load_int(ctx, value, "%r11", "%r11d")) return false;
        uint32_t offset = 0;
        while (inst->memory_type->size - offset >= 8) {
            fprintf(ctx->out, "    movq %u(%%r11), %%rax\n    movq %%rax, %u(%%r10)\n",
                    offset, offset);
            offset += 8;
        }
        if (inst->memory_type->size - offset >= 4) {
            fprintf(ctx->out, "    movl %u(%%r11), %%eax\n    movl %%eax, %u(%%r10)\n",
                    offset, offset);
            offset += 4;
        }
        while (offset < inst->memory_type->size) {
            fprintf(ctx->out, "    movb %u(%%r11), %%al\n    movb %%al, %u(%%r10)\n",
                    offset, offset);
            offset++;
        }
    } else if (x86_alloc_is_float(x86_alloc_machine_type_for_cobra(inst->memory_type))) {
        if (!x86_alloc_load_float(ctx, value, "%xmm14")) return false;
        fprintf(ctx->out, "    mov%s %%xmm14, (%%r10)\n", inst->memory_type->kind == COBRA_TYPE_F32 ? "ss" : "sd");
    } else {
        if (!x86_alloc_load_int(ctx, value, "%r8", "%r8d")) return false;
        if (inst->memory_type->kind == COBRA_TYPE_U8 || inst->memory_type->kind == COBRA_TYPE_BOOL)
            fprintf(ctx->out, "    movb %%r8b, (%%r10)\n");
        else if (inst->memory_type->size <= 4)
            fprintf(ctx->out, "    movl %%r8d, (%%r10)\n");
        else fprintf(ctx->out, "    movq %%r8, (%%r10)\n");
    }
    if (!x86_alloc_load_view_component(ctx, source, false, "%rdi")) return false;
    fprintf(ctx->out, "    call free@PLT\n");
    if (!x86_alloc_zero_view(ctx, source)) return false;
    fprintf(ctx->out, "    jmp %s\n%s:\n    ud2\n%s:\n", done, fail, done);
    return true;
}

static bool x86_alloc_emit_buffer_pop(X86AllocatedContext *ctx, const MirInst *inst) {
    if (inst->operand_count != 2 || inst->result == MIR_REG_NONE ||
        !inst->memory_type || inst->memory_type->size == 0) return false;
    MirReg source = ctx->module->arena.operands[inst->operand_start];
    MirReg fallback = ctx->module->arena.operands[inst->operand_start + 1];
    char fail[64], done[64];
    uint32_t label = ctx->view_fail_labels++;
    snprintf(fail, sizeof(fail), ".Lcobra_alloc_buffer_pop_fail_%zu_%u", ctx->function_index, label);
    snprintf(done, sizeof(done), ".Lcobra_alloc_buffer_pop_done_%zu_%u", ctx->function_index, label);
    if (!x86_alloc_load_view_component(ctx, source, false, "%r10") ||
        !x86_alloc_load_view_component(ctx, source, true, "%r11")) return false;
    fprintf(ctx->out, "    cmpq $1, %%r11\n    jl %s\n    decq %%r11\n", fail);
    if (!x86_alloc_store_view_component(ctx, source, true, "%r11")) return false;
    fprintf(ctx->out, "    imulq $%u, %%r11\n    addq %%r11, %%r10\n", (unsigned)inst->memory_type->size);
    if (x86_alloc_is_float(x86_alloc_machine_type_for_cobra(inst->memory_type))) {
        fprintf(ctx->out, "    mov%s (%%r10), %%xmm14\n", inst->memory_type->kind == COBRA_TYPE_F32 ? "ss" : "sd");
        if (!x86_alloc_store_float(ctx, inst->result, "%xmm14")) return false;
    } else {
        if (inst->memory_type->kind == COBRA_TYPE_U8 || inst->memory_type->kind == COBRA_TYPE_BOOL)
            fprintf(ctx->out, "    movzbq (%%r10), %%r8\n");
        else if (inst->memory_type->size <= 4)
            fprintf(ctx->out, "    movl (%%r10), %%r8d\n");
        else fprintf(ctx->out, "    movq (%%r10), %%r8\n");
        if (!x86_alloc_store_int(ctx, inst->result, "%r8", "%r8d")) return false;
    }
    fprintf(ctx->out, "    jmp %s\n%s:\n", done, fail);
    if (x86_alloc_is_float(x86_alloc_machine_type_for_cobra(inst->memory_type))) {
        if (!x86_alloc_load_float(ctx, fallback, "%xmm14")) return false;
        if (!x86_alloc_store_float(ctx, inst->result, "%xmm14")) return false;
    } else {
        if (!x86_alloc_load_int(ctx, fallback, "%r8", "%r8d")) return false;
        if (!x86_alloc_store_int(ctx, inst->result, "%r8", "%r8d")) return false;
    }
    fprintf(ctx->out, "%s:\n", done);
    return true;
}

static bool x86_alloc_emit_slice_free(X86AllocatedContext *ctx, const MirInst *inst) {
    if (inst->operand_count != 1) return false;
    MirReg view = ctx->module->arena.operands[inst->operand_start];
    if (ctx->module->arena.regs[view].machine_type != MIR_TYPE_VIEW) return false;
    char done[64];
    uint32_t label = ctx->view_fail_labels++;
    snprintf(done, sizeof(done), ".Lcobra_free_done_%zu_%u", ctx->function_index, label);
    if (!x86_alloc_load_view_component(ctx, view, false, "%r10")) return false;
    fprintf(ctx->out, "    testq %%r10, %%r10\n    je %s\n    movq %%r10, %%rdi\n    call free@PLT\n%s:\n", done, done);
    return x86_alloc_zero_view(ctx, view);
}

static bool x86_alloc_emit_transfer(X86AllocatedContext *ctx, const MirInst *inst) {
    if (inst->operand_count != 1 || inst->result == MIR_REG_NONE) return false;
    MirReg source = ctx->module->arena.operands[inst->operand_start];
    if (ctx->module->arena.regs[source].machine_type == MIR_TYPE_VIEW) {
        if (!x86_alloc_load_view_component(ctx, source, false, "%r10") ||
            !x86_alloc_load_view_component(ctx, source, true, "%r11") ||
            !x86_alloc_store_view_component(ctx, inst->result, false, "%r10") ||
            !x86_alloc_store_view_component(ctx, inst->result, true, "%r11")) return false;
        return true;
    }
    if (!x86_alloc_load_int(ctx, source, "%r10", "%r10d")) return false;
    return x86_alloc_store_int(ctx, inst->result, "%r10", "%r10d");
}

static bool x86_alloc_emit_destroy(X86AllocatedContext *ctx, const MirInst *inst) {
    if (inst->operand_count != 1) return false;
    MirReg source = ctx->module->arena.operands[inst->operand_start];
    MirMachineType type = ctx->module->arena.regs[source].machine_type;
    if (type == MIR_TYPE_VIEW) return x86_alloc_emit_slice_free(ctx, inst);
    if (type == MIR_TYPE_ADDRESS) return true;
    return false;
}

/* See x86_emit_inline_byte_copy in x86_64.c for why this avoids
   `call memcpy@PLT`: lib/mem.cb's libc import shadows the real symbol
   with a local, often body-less, definition of the same name. Copies
   %rdx bytes from %rsi to %rdi; clobbers %rax and %rcx. */
static void x86_alloc_emit_inline_byte_copy(X86AllocatedContext *ctx, const char *tag, uint32_t label) {
    fprintf(ctx->out,
            "    xorq %%rax, %%rax\n"
            ".Lcobra_alloc_%s_copy_loop_%zu_%u:\n"
            "    cmpq %%rdx, %%rax\n"
            "    je .Lcobra_alloc_%s_copy_done_%zu_%u\n"
            "    movb (%%rsi,%%rax), %%cl\n"
            "    movb %%cl, (%%rdi,%%rax)\n"
            "    incq %%rax\n"
            "    jmp .Lcobra_alloc_%s_copy_loop_%zu_%u\n"
            ".Lcobra_alloc_%s_copy_done_%zu_%u:\n",
            tag, ctx->function_index, label, tag, ctx->function_index, label,
            tag, ctx->function_index, label, tag, ctx->function_index, label);
}

static bool x86_alloc_emit_string_concat(X86AllocatedContext *ctx, const MirInst *inst) {
    if (inst->operand_count != 2 || inst->result == MIR_REG_NONE ||
        inst->machine_type != MIR_TYPE_VIEW || !inst->memory_type ||
        inst->memory_type->kind != COBRA_TYPE_U8 || inst->memory_type->size == 0) return false;
    MirReg left = ctx->module->arena.operands[inst->operand_start];
    MirReg right = ctx->module->arena.operands[inst->operand_start + 1];
    if (ctx->module->arena.regs[left].machine_type != MIR_TYPE_VIEW ||
        ctx->module->arena.regs[right].machine_type != MIR_TYPE_VIEW) return false;
    char fail[64], done[64];
    uint32_t label = ctx->view_fail_labels++;
    snprintf(fail, sizeof(fail), ".Lcobra_alloc_string_concat_fail_%zu_%u", ctx->function_index, label);
    snprintf(done, sizeof(done), ".Lcobra_alloc_string_concat_done_%zu_%u", ctx->function_index, label);
    if (!x86_alloc_load_view_component(ctx, left, true, "%r10") ||
        !x86_alloc_load_view_component(ctx, right, true, "%r11")) return false;
    fprintf(ctx->out, "    addq %%r11, %%r10\n    jc %s\n", fail);
    if (!x86_alloc_store_view_component(ctx, inst->result, true, "%r10")) return false;
    fprintf(ctx->out, "    imulq $%u, %%r10\n    movq %%r10, %%rdi\n    call malloc@PLT\n    testq %%rax, %%rax\n    je %s\n",
            (unsigned)inst->memory_type->size, fail);
    if (!x86_alloc_store_view_component(ctx, inst->result, false, "%rax")) return false;
    if (!x86_alloc_load_view_component(ctx, inst->result, false, "%rdi") ||
        !x86_alloc_load_view_component(ctx, left, false, "%rsi") ||
        !x86_alloc_load_view_component(ctx, left, true, "%rdx")) return false;
    fprintf(ctx->out, "    imulq $%u, %%rdx\n", (unsigned)inst->memory_type->size);
    x86_alloc_emit_inline_byte_copy(ctx, "string_concat_left", label);
    if (!x86_alloc_load_view_component(ctx, inst->result, false, "%rdi") ||
        !x86_alloc_load_view_component(ctx, left, true, "%r10") ||
        !x86_alloc_load_view_component(ctx, right, false, "%rsi") ||
        !x86_alloc_load_view_component(ctx, right, true, "%rdx")) return false;
    fprintf(ctx->out, "    imulq $%u, %%r10\n    addq %%r10, %%rdi\n    imulq $%u, %%rdx\n",
            (unsigned)inst->memory_type->size, (unsigned)inst->memory_type->size);
    x86_alloc_emit_inline_byte_copy(ctx, "string_concat_right", label);
    fprintf(ctx->out, "    jmp %s\n%s:\n    ud2\n%s:\n", done, fail, done);
    return true;
}

/* Inline byte-compare loop, not a memcmp@PLT call: lib/mem.cb imports
   memcmp from libc.so.6 for the allocator, giving every program object a
   local (often body-less, dead-stripped) symbol named memcmp that a bare
   `call memcmp@PLT` would resolve to instead of the real libc function. */
static bool x86_alloc_emit_string_eq(X86AllocatedContext *ctx, const MirInst *inst) {
    if (inst->operand_count != 2 || inst->result == MIR_REG_NONE ||
        inst->machine_type != MIR_TYPE_BOOL || !inst->memory_type ||
        inst->memory_type->kind != COBRA_TYPE_U8 || inst->memory_type->size == 0) return false;
    MirReg left = ctx->module->arena.operands[inst->operand_start];
    MirReg right = ctx->module->arena.operands[inst->operand_start + 1];
    if (ctx->module->arena.regs[left].machine_type != MIR_TYPE_VIEW ||
        ctx->module->arena.regs[right].machine_type != MIR_TYPE_VIEW) return false;
    char mismatch[64], match[64], loop[64], done[64];
    uint32_t label = ctx->view_fail_labels++;
    snprintf(mismatch, sizeof(mismatch), ".Lcobra_alloc_string_eq_mismatch_%zu_%u", ctx->function_index, label);
    snprintf(match, sizeof(match), ".Lcobra_alloc_string_eq_match_%zu_%u", ctx->function_index, label);
    snprintf(loop, sizeof(loop), ".Lcobra_alloc_string_eq_loop_%zu_%u", ctx->function_index, label);
    snprintf(done, sizeof(done), ".Lcobra_alloc_string_eq_done_%zu_%u", ctx->function_index, label);
    if (!x86_alloc_load_view_component(ctx, left, true, "%r10") ||
        !x86_alloc_load_view_component(ctx, right, true, "%r11")) return false;
    fprintf(ctx->out, "    cmpq %%r11, %%r10\n    jne %s\n", mismatch);
    if (!x86_alloc_load_view_component(ctx, left, false, "%r8") ||
        !x86_alloc_load_view_component(ctx, right, false, "%r9")) return false;
    fprintf(ctx->out,
            "    xorq %%rax, %%rax\n"
            "%s:\n"
            "    cmpq %%r10, %%rax\n"
            "    je %s\n"
            "    movq %%rax, %%rcx\n"
            "    imulq $%u, %%rcx\n"
            "    movzbl (%%r8,%%rcx), %%edx\n"
            "    movzbl (%%r9,%%rcx), %%esi\n"
            "    cmpl %%esi, %%edx\n"
            "    jne %s\n"
            "    incq %%rax\n"
            "    jmp %s\n"
            "%s:\n"
            "    movq $1, %%r10\n"
            "    jmp %s\n"
            "%s:\n"
            "    movq $0, %%r10\n"
            "%s:\n",
            loop, match, (unsigned)inst->memory_type->size, mismatch, loop,
            match, done, mismatch, done);
    return x86_alloc_store_int(ctx, inst->result, "%r10", "%r10d");
}

/* Emit a .rodata .string for a dict literal key and return its label. */
static void x86_alloc_emit_dict_key_rodata(X86AllocatedContext *ctx, const char *key,
                                           char *label, size_t label_size) {
    uint32_t id = ctx->dict_key_labels++;
    snprintf(label, label_size, ".Lcobra_alloc_dictkey_%zu_%u", ctx->function_index, id);
    fprintf(ctx->out, "    .section .rodata\n%s:\n    .string \"", label);
    for (const unsigned char *p = (const unsigned char *)key; p && *p; p++) {
        if (*p == '\\') fprintf(ctx->out, "\\\\");
        else if (*p == '\"') fprintf(ctx->out, "\\\"");
        else if (*p == 10) fprintf(ctx->out, "\\n");
        else if (*p == 13) fprintf(ctx->out, "\\r");
        else if (*p == 9) fprintf(ctx->out, "\\t");
        else if (*p < 32 || *p > 126) fprintf(ctx->out, "\\%03o", *p);
        else fputc(*p, ctx->out);
    }
    fprintf(ctx->out, "\"\n    .text\n");
}

static bool x86_alloc_emit_dict_alloc(X86AllocatedContext *ctx, const MirInst *inst) {
    if (inst->result == MIR_REG_NONE || inst->machine_type != MIR_TYPE_VIEW ||
        !inst->type || !bir_is_owned_dict_type(inst->type)) return false;
    return x86_alloc_zero_view(ctx, inst->result);
}

static bool x86_alloc_emit_dict_set(X86AllocatedContext *ctx, const MirInst *inst) {
    if (inst->operand_count != 2 || inst->result == MIR_REG_NONE ||
        inst->dict_key[0] == '\0') return false;
    MirReg source = ctx->module->arena.operands[inst->operand_start];
    MirReg value = ctx->module->arena.operands[inst->operand_start + 1];
    char label[64];
    x86_alloc_emit_dict_key_rodata(ctx, inst->dict_key, label, sizeof(label));
    /* Read the value into %rdx before clobbering %rdi/%rsi with the
       source-address and key setup below; the value may live in any
       caller-saved register. */
    if (!x86_alloc_load_int(ctx, value, "%rdx", "%edx")) return false;
    fprintf(ctx->out, "    leaq ");
    x86_alloc_mem(ctx->out, x86_alloc_spill(ctx, source), "rbp");
    fprintf(ctx->out, ", %%rdi\n    leaq %s(%%rip), %%rsi\n", label);
    fprintf(ctx->out, "    call cobra_dict_set_i64@PLT\n");
    if (!x86_alloc_load_view_component(ctx, source, false, "%r10") ||
        !x86_alloc_store_view_component(ctx, inst->result, false, "%r10")) return false;
    fprintf(ctx->out, "    movq %%r10, %%rdi\n    call cobra_dict_len@PLT\n");
    if (!x86_alloc_store_view_component(ctx, inst->result, true, "%rax")) return false;
    return x86_alloc_zero_view(ctx, source);
}

static bool x86_alloc_emit_dict_get(X86AllocatedContext *ctx, const MirInst *inst) {
    if (inst->operand_count != 2 || inst->result == MIR_REG_NONE ||
        inst->dict_key[0] == '\0') return false;
    MirReg source = ctx->module->arena.operands[inst->operand_start];
    MirReg fallback = ctx->module->arena.operands[inst->operand_start + 1];
    char label[64];
    x86_alloc_emit_dict_key_rodata(ctx, inst->dict_key, label, sizeof(label));
    /* Read the fallback into %rdx first; it may live in %rdi, which the
       source-pointer load below clobbers. */
    if (!x86_alloc_load_int(ctx, fallback, "%rdx", "%edx")) return false;
    if (!x86_alloc_load_view_component(ctx, source, false, "%rdi")) return false;
    fprintf(ctx->out, "    leaq %s(%%rip), %%rsi\n    call cobra_dict_get_i64@PLT\n", label);
    return x86_alloc_store_int(ctx, inst->result, "%rax", "%eax");
}

static bool x86_alloc_emit_dict_has(X86AllocatedContext *ctx, const MirInst *inst) {
    if (inst->operand_count != 1 || inst->result == MIR_REG_NONE ||
        inst->dict_key[0] == '\0') return false;
    MirReg source = ctx->module->arena.operands[inst->operand_start];
    char label[64];
    x86_alloc_emit_dict_key_rodata(ctx, inst->dict_key, label, sizeof(label));
    if (!x86_alloc_load_view_component(ctx, source, false, "%rdi")) return false;
    fprintf(ctx->out, "    leaq %s(%%rip), %%rsi\n    call cobra_dict_has@PLT\n", label);
    return x86_alloc_store_int(ctx, inst->result, "%rax", "%eax");
}

static bool x86_alloc_emit_dict_delete(X86AllocatedContext *ctx, const MirInst *inst) {
    if (inst->operand_count != 1 || inst->result == MIR_REG_NONE ||
        inst->dict_key[0] == '\0') return false;
    MirReg source = ctx->module->arena.operands[inst->operand_start];
    char label[64];
    x86_alloc_emit_dict_key_rodata(ctx, inst->dict_key, label, sizeof(label));
    fprintf(ctx->out, "    leaq ");
    x86_alloc_mem(ctx->out, x86_alloc_spill(ctx, source), "rbp");
    fprintf(ctx->out, ", %%rdi\n    leaq %s(%%rip), %%rsi\n    call cobra_dict_delete@PLT\n", label);
    if (!x86_alloc_load_view_component(ctx, source, false, "%r10") ||
        !x86_alloc_store_view_component(ctx, inst->result, false, "%r10")) return false;
    fprintf(ctx->out, "    movq %%r10, %%rdi\n    call cobra_dict_len@PLT\n");
    if (!x86_alloc_store_view_component(ctx, inst->result, true, "%rax")) return false;
    return x86_alloc_zero_view(ctx, source);
}

static bool x86_alloc_emit_dict_pop(X86AllocatedContext *ctx, const MirInst *inst) {
    if (inst->operand_count != 2 || inst->result == MIR_REG_NONE ||
        inst->dict_key[0] == '\0') return false;
    MirReg source = ctx->module->arena.operands[inst->operand_start];
    MirReg fallback = ctx->module->arena.operands[inst->operand_start + 1];
    char label[64];
    x86_alloc_emit_dict_key_rodata(ctx, inst->dict_key, label, sizeof(label));
    /* Read the fallback into %rdx before clobbering %rdi/%rsi. */
    if (!x86_alloc_load_int(ctx, fallback, "%rdx", "%edx")) return false;
    fprintf(ctx->out, "    leaq ");
    x86_alloc_mem(ctx->out, x86_alloc_spill(ctx, source), "rbp");
    fprintf(ctx->out, ", %%rdi\n    leaq %s(%%rip), %%rsi\n", label);
    fprintf(ctx->out, "    call cobra_dict_pop@PLT\n");
    if (!x86_alloc_store_int(ctx, inst->result, "%rax", "%eax")) return false;
    if (!x86_alloc_load_view_component(ctx, source, false, "%rdi")) return false;
    fprintf(ctx->out, "    call cobra_dict_len@PLT\n");
    return x86_alloc_store_view_component(ctx, source, true, "%rax");
}

static bool x86_alloc_emit_dict_len(X86AllocatedContext *ctx, const MirInst *inst) {
    if (inst->operand_count != 1 || inst->result == MIR_REG_NONE) return false;
    MirReg source = ctx->module->arena.operands[inst->operand_start];
    if (!x86_alloc_load_view_component(ctx, source, false, "%rdi")) return false;
    fprintf(ctx->out, "    call cobra_dict_len@PLT\n");
    return x86_alloc_store_int(ctx, inst->result, "%rax", "%eax");
}

static bool x86_alloc_emit_dict_free(X86AllocatedContext *ctx, const MirInst *inst) {
    if (inst->operand_count != 1 || inst->result != MIR_REG_NONE) return false;
    MirReg source = ctx->module->arena.operands[inst->operand_start];
    fprintf(ctx->out, "    leaq ");
    x86_alloc_mem(ctx->out, x86_alloc_spill(ctx, source), "rbp");
    fprintf(ctx->out, ", %%rdi\n    leaq ");
    x86_alloc_mem(ctx->out, x86_alloc_view_length_spill(ctx, source), "rbp");
    fprintf(ctx->out, ", %%rsi\n    call cobra_dict_free@PLT\n");
    return x86_alloc_zero_view(ctx, source);
}

static bool x86_alloc_emit_print_i64(X86AllocatedContext *ctx, const MirInst *inst) {
    if (inst->operand_count != 1) return false;
    MirReg value = ctx->module->arena.operands[inst->operand_start];
    char label[64];
    uint32_t id = ctx->dict_key_labels++;
    snprintf(label, sizeof(label), ".Lcobra_alloc_fmt_i64_%zu_%u", ctx->function_index, id);
    fprintf(ctx->out, "    .section .rodata\n%s:\n    .string \"%%ld\\n\"\n    .text\n", label);
    if (!x86_alloc_load_int(ctx, value, "%rsi", "%esi")) return false;
    fprintf(ctx->out, "    leaq %s(%%rip), %%rdi\n    xorl %%eax, %%eax\n    call printf@PLT\n", label);
    return true;
}

static bool x86_alloc_emit_print_string(X86AllocatedContext *ctx, const MirInst *inst) {
    /* Cobra's canonical memory model gives every scalar (including u8) an
       8-byte slot, so a "string" (a u8 view) is not packed, NUL-terminated
       bytes the way printf("%s", ...) expects - each character occupies an
       8-byte stride with the value in its low byte. Print it as a
       length-bounded byte loop through putchar instead, which needs no
       packing/termination assumption and matches the view's exact length. */
    if (inst->operand_count != 1) return false;
    MirReg value = ctx->module->arena.operands[inst->operand_start];
    if (ctx->module->arena.regs[value].machine_type != MIR_TYPE_VIEW) return false;
    if (!x86_alloc_load_view_component(ctx, value, false, "%r10") ||
        !x86_alloc_load_view_component(ctx, value, true, "%r11")) return false;
    fprintf(ctx->out, "    movq %%r10, ");
    x86_alloc_mem(ctx->out, ctx->temp_offsets[0], "rbp");
    fprintf(ctx->out, "\n    movq %%r11, ");
    x86_alloc_mem(ctx->out, ctx->temp_offsets[1], "rbp");
    fprintf(ctx->out, "\n    movq $0, ");
    x86_alloc_mem(ctx->out, ctx->temp_offsets[2], "rbp");
    fprintf(ctx->out, "\n");
    char top[64], done[64];
    uint32_t id = ctx->dict_key_labels++;
    snprintf(top, sizeof(top), ".Lcobra_alloc_print_str_top_%zu_%u", ctx->function_index, id);
    snprintf(done, sizeof(done), ".Lcobra_alloc_print_str_done_%zu_%u", ctx->function_index, id);
    fprintf(ctx->out, "%s:\n    movq ", top);
    x86_alloc_mem(ctx->out, ctx->temp_offsets[2], "rbp");
    fprintf(ctx->out, ", %%rax\n    cmpq ");
    x86_alloc_mem(ctx->out, ctx->temp_offsets[1], "rbp");
    fprintf(ctx->out, ", %%rax\n    jae %s\n", done);
    fprintf(ctx->out, "    movq ");
    x86_alloc_mem(ctx->out, ctx->temp_offsets[0], "rbp");
    fprintf(ctx->out, ", %%r10\n    movq ");
    x86_alloc_mem(ctx->out, ctx->temp_offsets[2], "rbp");
    fprintf(ctx->out, ", %%r11\n    imulq $8, %%r11\n    addq %%r11, %%r10\n");
    fprintf(ctx->out, "    movzbl (%%r10), %%edi\n    call putchar@PLT\n    movq ");
    x86_alloc_mem(ctx->out, ctx->temp_offsets[2], "rbp");
    fprintf(ctx->out, ", %%rax\n    incq %%rax\n    movq %%rax, ");
    x86_alloc_mem(ctx->out, ctx->temp_offsets[2], "rbp");
    fprintf(ctx->out, "\n    jmp %s\n%s:\n    movl $10, %%edi\n    call putchar@PLT\n", top, done);
    return true;
}

static bool x86_alloc_emit_assert(X86AllocatedContext *ctx, const MirInst *inst) {
    if (inst->operand_count != 1) return false;
    MirReg cond = ctx->module->arena.operands[inst->operand_start];
    if (!x86_alloc_load_int(ctx, cond, "%r10", "%r10d")) return false;
    char ok[64], label[64];
    uint32_t id = ctx->dict_key_labels++;
    snprintf(ok, sizeof(ok), ".Lcobra_alloc_assert_ok_%zu_%u", ctx->function_index, id);
    snprintf(label, sizeof(label), ".Lcobra_alloc_assert_msg_%zu_%u", ctx->function_index, id);
    fprintf(ctx->out, "    testq %%r10, %%r10\n    jne %s\n", ok);
    fprintf(ctx->out, "    .section .rodata\n%s:\n    .string \"[cobra] assertion failed\"\n    .text\n", label);
    fprintf(ctx->out, "    leaq %s(%%rip), %%rdi\n    call puts@PLT\n    movl $1, %%edi\n    call exit@PLT\n", label);
    fprintf(ctx->out, "%s:\n", ok);
    return true;
}

static bool x86_alloc_emit_region_cleanup(X86AllocatedContext *ctx, uint32_t region_id) {
    for (size_t offset = 0; offset < ctx->function->block_count; offset++) {
        MirBlockRef block_ref = ctx->function->first_block + (MirBlockRef)offset;
        const MirBlock *block = &ctx->module->arena.blocks[block_ref];
        for (size_t i = 0; i < block->inst_count; i++) {
            const MirInst *alloc = &ctx->module->arena.insts[block->insts[i]];
            if ((alloc->op != MIR_OP_SLICE_ALLOC && alloc->op != MIR_OP_BUFFER_ALLOC) ||
                alloc->region_id != region_id ||
                alloc->result == MIR_REG_NONE) continue;
            char done[64];
            uint32_t label = ctx->view_fail_labels++;
            snprintf(done, sizeof(done), ".Lcobra_region_free_%zu_%u", ctx->function_index, label);
            if (!x86_alloc_load_view_component(ctx, alloc->result, false, "%r10")) return false;
            fprintf(ctx->out, "    testq %%r10, %%r10\n    je %s\n    movq %%r10, %%rdi\n    call free@PLT\n%s:\n", done, done);
            if (!x86_alloc_zero_view(ctx, alloc->result)) return false;
        }
    }
    return true;
}

static bool x86_alloc_initialize_owned_views(X86AllocatedContext *ctx) {
    for (size_t offset = 0; offset < ctx->function->block_count; offset++) {
        MirBlockRef block_ref = ctx->function->first_block + (MirBlockRef)offset;
        const MirBlock *block = &ctx->module->arena.blocks[block_ref];
        for (size_t i = 0; i < block->inst_count; i++) {
            const MirInst *inst = &ctx->module->arena.insts[block->insts[i]];
            if ((inst->op != MIR_OP_SLICE_ALLOC && inst->op != MIR_OP_BUFFER_ALLOC) ||
                inst->result == MIR_REG_NONE) continue;
            if (!x86_alloc_zero_view(ctx, inst->result)) return false;
        }
    }
    return true;
}

static bool x86_alloc_emit_frame_cleanup(X86AllocatedContext *ctx, MirReg protected_view) {
    uint32_t protected_allocation_id =
        protected_view != MIR_REG_NONE
            ? ctx->module->arena.regs[protected_view].allocation_id : 0;
    for (size_t offset = 0; offset < ctx->function->block_count; offset++) {
        MirBlockRef block_ref = ctx->function->first_block + (MirBlockRef)offset;
        const MirBlock *block = &ctx->module->arena.blocks[block_ref];
        for (size_t i = 0; i < block->inst_count; i++) {
            const MirInst *owner = &ctx->module->arena.insts[block->insts[i]];
            if (owner->result == MIR_REG_NONE ||
                ctx->module->arena.regs[owner->result].machine_type != MIR_TYPE_VIEW ||
                ctx->module->arena.regs[owner->result].pointer_contract !=
                    BIR_POINTER_CONTRACT_OWNED_SLICE ||                (owner->op != MIR_OP_SLICE_ALLOC && owner->op != MIR_OP_BUFFER_ALLOC &&
                 owner->op != MIR_OP_BUFFER_APPEND && owner->op != MIR_OP_STRING_CONCAT &&
                 owner->op != MIR_OP_CALL)) continue;
            uint32_t owner_id = ctx->module->arena.regs[owner->result].allocation_id;
            char done[64], skip[64];
            uint32_t label = ctx->view_fail_labels++;
            snprintf(done, sizeof(done), ".Lcobra_alloc_frame_free_%zu_%u", ctx->function_index, label);
            snprintf(skip, sizeof(skip), ".Lcobra_alloc_frame_skip_%zu_%u", ctx->function_index, label);
            if (!x86_alloc_load_view_component(ctx, owner->result, false, "%r10")) return false;
            fprintf(ctx->out, "    testq %%r10, %%r10\n    je %s\n", done);
            if (protected_view != MIR_REG_NONE) {
                if (protected_allocation_id != 0 && owner_id == protected_allocation_id) {
                    fprintf(ctx->out, "    jmp %s\n", skip);
                } else if (protected_allocation_id == 0 || owner_id == 0) {
                    if (!x86_alloc_load_view_component(ctx, protected_view, false, "%r11")) return false;
                    fprintf(ctx->out, "    cmpq %%r11, %%r10\n    je %s\n", skip);
                }
            }
            fprintf(ctx->out, "    movq %%r10, %%rdi\n    call free@PLT\n");
            if (!x86_alloc_zero_view(ctx, owner->result)) return false;
            fprintf(ctx->out, "%s:\n%s:\n", skip, done);
        }
    }
    for (MirReg reg = 1; reg < ctx->module->arena.reg_count; reg++) {
        const MirRegInfo *info = &ctx->module->arena.regs[reg];
        if (info->function_index != ctx->function_index || !info->entry_defined ||
            info->machine_type != MIR_TYPE_VIEW ||
            info->pointer_contract != BIR_POINTER_CONTRACT_OWNED_SLICE) continue;
        uint32_t owner_id = info->allocation_id;
        char done[64], skip[64];
        uint32_t label = ctx->view_fail_labels++;
        snprintf(done, sizeof(done), ".Lcobra_alloc_param_free_%zu_%u", ctx->function_index, label);
        snprintf(skip, sizeof(skip), ".Lcobra_alloc_param_skip_%zu_%u", ctx->function_index, label);
        if (!x86_alloc_load_view_component(ctx, reg, false, "%r10")) return false;
        fprintf(ctx->out, "    testq %%r10, %%r10\n    je %s\n", done);
        if (protected_view != MIR_REG_NONE && protected_allocation_id != 0 &&
            owner_id == protected_allocation_id) {
            fprintf(ctx->out, "    jmp %s\n", skip);
        } else {
            fprintf(ctx->out, "    movq %%r10, %%rdi\n    call free@PLT\n");
            if (!x86_alloc_zero_view(ctx, reg)) return false;
        }
        fprintf(ctx->out, "%s:\n%s:\n", skip, done);
    }
    return true;
}

static bool x86_alloc_call_arg_moves_ownership(const X86AllocatedContext *ctx,
                                               const MirInst *inst, size_t arg) {
    if (!ctx || !ctx->module || !ctx->module->source ||
        inst->callee_index >= ctx->module->source->function_count) return false;
    const BirFunctionInfo *callee = &ctx->module->source->functions[inst->callee_index];
    if (callee->has_hidden_return_storage && arg == 0) return false;
    size_t user_arg = arg - (callee->has_hidden_return_storage ? 1U : 0U);
    return user_arg < callee->param_count &&
           bir_is_owned_slice_type(callee->param_value_types[user_arg]);
}

/* Cobra's canonical memory model gives every u8 element an 8-byte slot (see
   x86_alloc_emit_print_string), so a "string" or []u8 view's bytes are not
   the packed, NUL-terminated bytes a C function expects - each element
   occupies an 8-byte stride with the value in its low byte. Passing the raw
   view base straight through to an extern call hands the callee a buffer it
   will misread (open() sees a one-character path, write() sees mostly zero
   padding, read() into a []u8 output buffer never reaches the caller's real
   storage, and so on). Allocate a real packed buffer at runtime, copy each
   element's low byte into it contiguously, NUL-terminate it, and stage that
   pointer as the call argument instead; x86_alloc_writeback_extern_view_arg
   copies it back element-by-element after the call returns, so this is safe
   whether the callee reads the buffer, writes it, or both.

   Bookkeeping lives in dedicated per-argument temps starting right after the
   argument-staging temps (index BIR_ABI_MAX_GPR_ARGUMENT_REGISTERS), two per
   argument (source pointer, then length); the malloc'd packed buffer pointer
   is stashed in the argument's own staging temp, since that is exactly the
   value the call needs. A shared loop-counter temp at the very end is safe
   to reuse across arguments because packing and writeback both run one
   argument at a time, never interleaved. */
#define X86_ALLOC_PACK_BOOKKEEPING_BASE BIR_ABI_MAX_GPR_ARGUMENT_REGISTERS

static bool x86_alloc_view_element_is_u8(const X86AllocatedContext *ctx, MirReg value) {
    const CobraType *type = ctx->module->arena.regs[value].type;
    const CobraType *element = type ? cobra_type_element(type) : NULL;
    return element && element->kind == COBRA_TYPE_U8;
}

static bool x86_alloc_pack_extern_view_arg(X86AllocatedContext *ctx, MirReg value, size_t arg) {
    size_t src_index = X86_ALLOC_PACK_BOOKKEEPING_BASE + arg * 2;
    size_t len_index = src_index + 1;
    if (arg >= X86_ALLOC_TEMP_COUNT || len_index >= X86_ALLOC_TEMP_COUNT - 1) return false;
    int64_t src_off = ctx->temp_offsets[src_index];
    int64_t len_off = ctx->temp_offsets[len_index];
    int64_t idx_off = ctx->temp_offsets[X86_ALLOC_TEMP_COUNT - 1];

    if (!x86_alloc_load_view_component(ctx, value, false, "%r10")) return false;
    fprintf(ctx->out, "    movq %%r10, ");
    x86_alloc_mem(ctx->out, src_off, "rbp");
    fprintf(ctx->out, "\n");
    if (!x86_alloc_load_view_component(ctx, value, true, "%r10")) return false;
    fprintf(ctx->out, "    movq %%r10, ");
    x86_alloc_mem(ctx->out, len_off, "rbp");
    fprintf(ctx->out, "\n    movq ");
    x86_alloc_mem(ctx->out, len_off, "rbp");
    fprintf(ctx->out, ", %%rdi\n    incq %%rdi\n    call malloc@PLT\n    movq %%rax, ");
    x86_alloc_mem(ctx->out, ctx->temp_offsets[arg], "rbp");
    fprintf(ctx->out, "\n    movq $0, ");
    x86_alloc_mem(ctx->out, idx_off, "rbp");
    fprintf(ctx->out, "\n");

    char top[64], done[64];
    uint32_t id = ctx->dict_key_labels++;
    snprintf(top, sizeof(top), ".Lcobra_alloc_pack_top_%zu_%u", ctx->function_index, id);
    snprintf(done, sizeof(done), ".Lcobra_alloc_pack_done_%zu_%u", ctx->function_index, id);
    fprintf(ctx->out, "%s:\n    movq ", top);
    x86_alloc_mem(ctx->out, idx_off, "rbp");
    fprintf(ctx->out, ", %%rax\n    cmpq ");
    x86_alloc_mem(ctx->out, len_off, "rbp");
    fprintf(ctx->out, ", %%rax\n    jae %s\n    movq ", done);
    x86_alloc_mem(ctx->out, src_off, "rbp");
    fprintf(ctx->out, ", %%r10\n    movq ");
    x86_alloc_mem(ctx->out, idx_off, "rbp");
    fprintf(ctx->out, ", %%r11\n    imulq $8, %%r11\n    addq %%r11, %%r10\n"
                       "    movzbl (%%r10), %%eax\n    movq ");
    x86_alloc_mem(ctx->out, ctx->temp_offsets[arg], "rbp");
    fprintf(ctx->out, ", %%r11\n    addq ");
    x86_alloc_mem(ctx->out, idx_off, "rbp");
    fprintf(ctx->out, ", %%r11\n    movb %%al, (%%r11)\n    movq ");
    x86_alloc_mem(ctx->out, idx_off, "rbp");
    fprintf(ctx->out, ", %%rax\n    incq %%rax\n    movq %%rax, ");
    x86_alloc_mem(ctx->out, idx_off, "rbp");
    fprintf(ctx->out, "\n    jmp %s\n%s:\n    movq ", top, done);
    x86_alloc_mem(ctx->out, ctx->temp_offsets[arg], "rbp");
    fprintf(ctx->out, ", %%r10\n    movq ");
    x86_alloc_mem(ctx->out, len_off, "rbp");
    fprintf(ctx->out, ", %%r11\n    addq %%r11, %%r10\n    movb $0, (%%r10)\n");
    return true;
}

/* Copies the packed scratch buffer staged for argument `arg` back into the
   original strided view storage, one byte-widened element at a time. Must
   run after the extern call so a callee that writes through the buffer
   (read() into a []u8 destination) is observed by the caller. */
static bool x86_alloc_writeback_extern_view_arg(X86AllocatedContext *ctx, size_t arg,
                                                 int64_t buf_off) {
    size_t src_index = X86_ALLOC_PACK_BOOKKEEPING_BASE + arg * 2;
    size_t len_index = src_index + 1;
    if (len_index >= X86_ALLOC_TEMP_COUNT - 1) return false;
    int64_t src_off = ctx->temp_offsets[src_index];
    int64_t len_off = ctx->temp_offsets[len_index];
    int64_t idx_off = ctx->temp_offsets[X86_ALLOC_TEMP_COUNT - 1];

    fprintf(ctx->out, "    movq $0, ");
    x86_alloc_mem(ctx->out, idx_off, "rbp");
    fprintf(ctx->out, "\n");
    char top[64], done[64];
    uint32_t id = ctx->dict_key_labels++;
    snprintf(top, sizeof(top), ".Lcobra_alloc_unpack_top_%zu_%u", ctx->function_index, id);
    snprintf(done, sizeof(done), ".Lcobra_alloc_unpack_done_%zu_%u", ctx->function_index, id);
    fprintf(ctx->out, "%s:\n    movq ", top);
    x86_alloc_mem(ctx->out, idx_off, "rbp");
    fprintf(ctx->out, ", %%rax\n    cmpq ");
    x86_alloc_mem(ctx->out, len_off, "rbp");
    fprintf(ctx->out, ", %%rax\n    jae %s\n    movq ", done);
    x86_alloc_mem(ctx->out, buf_off, "rbp");
    fprintf(ctx->out, ", %%r10\n    addq ");
    x86_alloc_mem(ctx->out, idx_off, "rbp");
    fprintf(ctx->out, ", %%r10\n    movzbl (%%r10), %%eax\n    movq ");
    x86_alloc_mem(ctx->out, src_off, "rbp");
    fprintf(ctx->out, ", %%r10\n    movq ");
    x86_alloc_mem(ctx->out, idx_off, "rbp");
    fprintf(ctx->out, ", %%r11\n    imulq $8, %%r11\n    addq %%r11, %%r10\n"
                       "    movb %%al, (%%r10)\n    movq ");
    x86_alloc_mem(ctx->out, idx_off, "rbp");
    fprintf(ctx->out, ", %%rax\n    incq %%rax\n    movq %%rax, ");
    x86_alloc_mem(ctx->out, idx_off, "rbp");
    fprintf(ctx->out, "\n    jmp %s\n%s:\n", top, done);
    return true;
}

static bool x86_alloc_emit_extern_call(X86AllocatedContext *ctx, const MirInst *inst) {
    if (inst->operand_count > BIR_ABI_MAX_GPR_ARGUMENT_REGISTERS) return false;
    /* Stage every argument through a stack temp before touching any ABI
       argument register. A later argument's current physical register can
       be the very register an earlier argument must be moved into (e.g.
       argument 1 lives in %rdi while argument 0 is about to be written
       there); moving arguments straight into their destination registers in
       one forward pass silently overwrites that still-unread source and
       hands the callee a clobbered value. Staging through memory first,
       then loading every ABI register from the stable temps, removes the
       ordering hazard entirely.

       Scalars are staged before any view is packed: packing a view calls
       malloc@PLT, which like any call clobbers the caller-saved registers,
       and a later scalar argument can still be sitting unread in one of
       those registers at that point. Reading every scalar out to its
       stable temp first removes that hazard too. */
    bool packed[BIR_ABI_MAX_GPR_ARGUMENT_REGISTERS] = {0};
    for (size_t arg = 0; arg < inst->operand_count; arg++) {
        if (arg >= X86_ALLOC_TEMP_COUNT) return false;
        MirReg value = ctx->module->arena.operands[inst->operand_start + arg];
        MirMachineType type = ctx->module->arena.regs[value].machine_type;
        if (type == MIR_TYPE_VIEW) continue;
        if (x86_alloc_is_float(type)) return false;
        if (!x86_alloc_load_int(ctx, value, "%r10", "%r10d")) return false;
        fprintf(ctx->out, "    movq %%r10, ");
        x86_alloc_mem(ctx->out, ctx->temp_offsets[arg], "rbp");
        fprintf(ctx->out, "\n");
    }
    for (size_t arg = 0; arg < inst->operand_count; arg++) {
        if (arg >= X86_ALLOC_TEMP_COUNT) return false;
        MirReg value = ctx->module->arena.operands[inst->operand_start + arg];
        MirMachineType type = ctx->module->arena.regs[value].machine_type;
        if (type != MIR_TYPE_VIEW) continue;
        if (x86_alloc_view_element_is_u8(ctx, value)) {
            if (!x86_alloc_pack_extern_view_arg(ctx, value, arg)) return false;
            packed[arg] = true;
        } else {
            /* A view over any other element type (e.g. []i64, []f32) already
               has real element width equal to its canonical stride, so the
               raw base pointer is already a valid packed C buffer. */
            if (!x86_alloc_load_view_component(ctx, value, false, "%r10")) return false;
            fprintf(ctx->out, "    movq %%r10, ");
            x86_alloc_mem(ctx->out, ctx->temp_offsets[arg], "rbp");
            fprintf(ctx->out, "\n");
        }
    }
    for (size_t arg = 0; arg < inst->operand_count; arg++) {
        const char *gpr = NULL;
        if (!x86_alloc_gpr((uint16_t)arg, &gpr)) return false;
        fprintf(ctx->out, "    movq ");
        x86_alloc_mem(ctx->out, ctx->temp_offsets[arg], "rbp");
        fprintf(ctx->out, ", %s\n", gpr);
    }
    fprintf(ctx->out, "    xorl %%eax, %%eax\n    call %s@PLT\n", inst->callee);
    bool needs_writeback = false;
    for (size_t arg = 0; arg < inst->operand_count; arg++) {
        if (packed[arg]) needs_writeback = true;
    }
    if (needs_writeback) {
        /* The writeback loops below clobber %rax as scratch; the call's
           return value has to survive them, so stash it before running
           any writeback and restore it just before storing the result. */
        if (X86_ALLOC_TEMP_COUNT == 0) return false;
        int64_t result_off = ctx->temp_offsets[X86_ALLOC_TEMP_COUNT - 2];
        fprintf(ctx->out, "    movq %%rax, ");
        x86_alloc_mem(ctx->out, result_off, "rbp");
        fprintf(ctx->out, "\n");
        for (size_t arg = 0; arg < inst->operand_count; arg++) {
            if (!packed[arg]) continue;
            if (!x86_alloc_writeback_extern_view_arg(ctx, arg, ctx->temp_offsets[arg])) return false;
        }
        fprintf(ctx->out, "    movq ");
        x86_alloc_mem(ctx->out, result_off, "rbp");
        fprintf(ctx->out, ", %%rax\n");
    }
    if (inst->result != MIR_REG_NONE) {
        x86_alloc_store_int(ctx, inst->result, "%rax", "%eax");
    }
    return true;
}

static bool x86_alloc_emit_call(X86AllocatedContext *ctx, const MirInst *inst) {
    if (ctx->module->source && inst->callee_index < ctx->module->source->function_count &&
        ctx->module->source->functions[inst->callee_index].is_extern) {
        return x86_alloc_emit_extern_call(ctx, inst);
    }
    const MirFunction *callee = &ctx->module->functions[inst->callee_index];
    if (inst->operand_count > X86_ALLOC_TEMP_COUNT) return false;
    for (size_t arg = 0; arg < inst->operand_count; arg++) {
        MirReg value = ctx->module->arena.operands[inst->operand_start + arg];
        if (ctx->module->arena.regs[value].machine_type == MIR_TYPE_VIEW) continue;
        if (callee->call_abi.params[arg].count != 1 ||
            !x86_alloc_stage_call_arg(ctx, value, arg)) return false;
    }
    for (size_t arg = 0; arg < inst->operand_count; arg++) {
        MirReg value = ctx->module->arena.operands[inst->operand_start + arg];
        MirMachineType type = ctx->module->arena.regs[value].machine_type;
        if (type == MIR_TYPE_VIEW) {
            /* Buffers pass 3 parts (ptr, len, cap). The callee discards the
               capacity part, so any value works; pass the length again. */
            if (callee->call_abi.params[arg].count != 2 &&
                callee->call_abi.params[arg].count != 3) return false;
            for (size_t part = 0; part < callee->call_abi.params[arg].count; part++) {
                const BirAbiLocation *location = &callee->call_abi.params[arg].parts[part];
                if (!x86_alloc_load_view_component(ctx, value, part == 1 || part == 2, "%r10")) return false;
                if (location->storage == BIR_ABI_STORAGE_REGISTER) {
                    const char *destination = NULL;
                    if (!x86_alloc_gpr(location->register_index, &destination)) return false;
                    fprintf(ctx->out, "    movq %%r10, %s\n", destination);
                } else if (location->storage == BIR_ABI_STORAGE_STACK) {
                    fprintf(ctx->out, "    movq %%r10, ");
                    x86_alloc_mem(ctx->out, location->stack_offset, "rsp");
                    fprintf(ctx->out, "\n");
                } else return false;
            }
            continue;
        }
        if (callee->call_abi.params[arg].count != 1) return false;
        const BirAbiLocation *location = &callee->call_abi.params[arg].parts[0];
        if (x86_alloc_is_float(type)) {
            fprintf(ctx->out, "    %s ", type == MIR_TYPE_F32 ? "movss" : "movsd");
            x86_alloc_mem(ctx->out, ctx->temp_offsets[arg], "rbp");
            if (location->storage == BIR_ABI_STORAGE_REGISTER) {
                char destination[16];
                if (!x86_alloc_xmm(location->register_index, destination, sizeof(destination))) return false;
                fprintf(ctx->out, ", %s\n", destination);
            } else if (location->storage == BIR_ABI_STORAGE_STACK) {
                fprintf(ctx->out, ", %%xmm14\n");
                fprintf(ctx->out, "    %s %%xmm14, ", type == MIR_TYPE_F32 ? "movss" : "movsd");
                x86_alloc_mem(ctx->out, location->stack_offset, "rsp");
                fprintf(ctx->out, "\n");
            } else return false;
        } else {
            fprintf(ctx->out, "    movq ");
            x86_alloc_mem(ctx->out, ctx->temp_offsets[arg], "rbp");
            fprintf(ctx->out, ", %%r10\n");
            if (location->storage == BIR_ABI_STORAGE_REGISTER) {
                const char *destination = NULL;
                if (type == MIR_TYPE_I8 || type == MIR_TYPE_I32 || type == MIR_TYPE_U32) {
                    if (!x86_alloc_gpr32(location->register_index, &destination)) return false;
                    fprintf(ctx->out, "    movl %%r10d, %s\n", destination);
                } else {
                    if (!x86_alloc_gpr(location->register_index, &destination)) return false;
                    fprintf(ctx->out, "    movq %%r10, %s\n", destination);
                }
            } else if (location->storage == BIR_ABI_STORAGE_STACK) {
                fprintf(ctx->out, "    movq %%r10, ");
                x86_alloc_mem(ctx->out, location->stack_offset, "rsp");
                fprintf(ctx->out, "\n");
            } else return false;
        }
    }
    for (size_t arg = 0; arg < inst->operand_count; arg++) {
        MirReg value = ctx->module->arena.operands[inst->operand_start + arg];
        if (ctx->module->arena.regs[value].machine_type == MIR_TYPE_VIEW &&
            x86_alloc_call_arg_moves_ownership(ctx, inst, arg) &&
            !x86_alloc_zero_view(ctx, value)) return false;
    }
    fprintf(ctx->out, "    call %s\n", inst->callee);
    if (inst->result != MIR_REG_NONE) {
        MirMachineType type = ctx->module->arena.regs[inst->result].machine_type;
        if (type == MIR_TYPE_VIEW) {
            if (!x86_alloc_store_view_component(ctx, inst->result, false, "%rax") ||
                !x86_alloc_store_view_component(ctx, inst->result, true, "%rdx")) return false;
        } else if (x86_alloc_is_float(type)) x86_alloc_store_float(ctx, inst->result, "%xmm0");
        else x86_alloc_store_int(ctx, inst->result, "%rax", "%eax");
    }
    return true;
}

static bool x86_alloc_emit_inst(X86AllocatedContext *ctx, const MirInst *inst) {
    switch (inst->op) {
        case MIR_OP_CONST: return x86_alloc_emit_const(ctx, inst);
        case MIR_OP_ABI_MOVE: return x86_alloc_emit_abi_move(ctx, inst);
        case MIR_OP_ADD:
        case MIR_OP_SUB:
        case MIR_OP_MUL: return x86_alloc_emit_binary(ctx, inst);
        case MIR_OP_NEG: return x86_alloc_emit_neg(ctx, inst);
        case MIR_OP_CONVERT: return x86_alloc_emit_convert(ctx, inst);
        case MIR_OP_DIV:
        case MIR_OP_REM:
            return x86_alloc_is_float(inst->machine_type)
                ? x86_alloc_emit_binary(ctx, inst) : x86_alloc_emit_div(ctx, inst);
        case MIR_OP_EQ:
        case MIR_OP_NE:
        case MIR_OP_LT:
        case MIR_OP_LE:
        case MIR_OP_GT:
        case MIR_OP_GE: return x86_alloc_emit_compare(ctx, inst);
        case MIR_OP_STACK_SLOT:
            fprintf(ctx->out, "    leaq ");
            x86_alloc_mem(ctx->out, ctx->memory_offsets[inst->stack_slot], "rbp");
            fprintf(ctx->out, ", %%r10\n");
            return x86_alloc_store_int(ctx, inst->result, "%r10", "%r10d");
        case MIR_OP_PTR_ADD: return x86_alloc_emit_ptr_add(ctx, inst);
        case MIR_OP_VIEW_MAKE: return x86_alloc_emit_view_make(ctx, inst);
        case MIR_OP_VIEW_PTR: {
            MirReg view = ctx->module->arena.operands[inst->operand_start];
            if (!x86_alloc_load_view_component(ctx, view, false, "%r10")) return false;
            return x86_alloc_store_int(ctx, inst->result, "%r10", "%r10d");
        }
        case MIR_OP_VIEW_LEN: {
            MirReg view = ctx->module->arena.operands[inst->operand_start];
            if (!x86_alloc_load_view_component(ctx, view, true, "%r10")) return false;
            return x86_alloc_store_int(ctx, inst->result, "%r10", "%r10d");
        }
        case MIR_OP_FIELD_ADDR:
            x86_alloc_load_int(ctx, ctx->module->arena.operands[inst->operand_start], "%r10", "%r10d");
            fprintf(ctx->out, "    addq $%lld, %%r10\n", (long long)inst->memory_offset);
            return x86_alloc_store_int(ctx, inst->result, "%r10", "%r10d");
        case MIR_OP_ARRAY_INDEX_ADDR: return x86_alloc_emit_array_index_addr(ctx, inst);
        case MIR_OP_LOAD:
        case MIR_OP_STORE: return x86_alloc_emit_memory(ctx, inst);
        case MIR_OP_AGG_COPY: return x86_alloc_emit_aggregate_copy(ctx, inst);
        case MIR_OP_SUM_PAYLOAD_STORE:
        case MIR_OP_FIELD_PAYLOAD_STORE: return x86_alloc_emit_owned_payload_store(ctx, inst);
        case MIR_OP_SUM_PAYLOAD_LOAD:
        case MIR_OP_FIELD_PAYLOAD_LOAD: return x86_alloc_emit_owned_payload_load(ctx, inst);
        case MIR_OP_SUM_MOVE:
        case MIR_OP_AGG_MOVE: return x86_alloc_emit_owned_aggregate_move(ctx, inst);
        case MIR_OP_SUM_DROP:
        case MIR_OP_AGG_DROP: return x86_alloc_emit_owned_aggregate_drop(ctx, inst);
        case MIR_OP_SLICE_ALLOC: return x86_alloc_emit_slice_alloc(ctx, inst);
        case MIR_OP_SLICE_FREE: return x86_alloc_emit_slice_free(ctx, inst);
        case MIR_OP_BUFFER_ALLOC: return x86_alloc_emit_buffer_alloc(ctx, inst);
        case MIR_OP_BUFFER_APPEND: return x86_alloc_emit_buffer_append(ctx, inst);
        case MIR_OP_BUFFER_POP: return x86_alloc_emit_buffer_pop(ctx, inst);
        case MIR_OP_BUFFER_FREE: return x86_alloc_emit_slice_free(ctx, inst);
        case MIR_OP_DICT_ALLOC: return x86_alloc_emit_dict_alloc(ctx, inst);
        case MIR_OP_DICT_SET: return x86_alloc_emit_dict_set(ctx, inst);
        case MIR_OP_DICT_GET: return x86_alloc_emit_dict_get(ctx, inst);
        case MIR_OP_DICT_HAS: return x86_alloc_emit_dict_has(ctx, inst);
        case MIR_OP_DICT_DELETE: return x86_alloc_emit_dict_delete(ctx, inst);
        case MIR_OP_DICT_POP: return x86_alloc_emit_dict_pop(ctx, inst);
        case MIR_OP_DICT_LEN: return x86_alloc_emit_dict_len(ctx, inst);
        case MIR_OP_DICT_FREE: return x86_alloc_emit_dict_free(ctx, inst);
        case MIR_OP_STRING_CONCAT: return x86_alloc_emit_string_concat(ctx, inst);
        case MIR_OP_STRING_EQ: return x86_alloc_emit_string_eq(ctx, inst);
        case MIR_OP_SUM_CHECK: return x86_alloc_emit_sum_check(ctx, inst);
        case MIR_OP_PRINT_I64: return x86_alloc_emit_print_i64(ctx, inst);
        case MIR_OP_PRINT_STRING: return x86_alloc_emit_print_string(ctx, inst);
        case MIR_OP_ASSERT: return x86_alloc_emit_assert(ctx, inst);
        case MIR_OP_REGION_ENTER: return true;
        case MIR_OP_REGION_EXIT: return x86_alloc_emit_region_cleanup(ctx, inst->region_id);
        case MIR_OP_TRANSFER: return x86_alloc_emit_transfer(ctx, inst);
        case MIR_OP_DESTROY: return x86_alloc_emit_destroy(ctx, inst);
        case MIR_OP_CALL: return x86_alloc_emit_call(ctx, inst);
        case MIR_OP_JUMP:
            fprintf(ctx->out, "    jmp .Lcobra_alloc_%zu_b%u\n", ctx->function_index, inst->target);
            return true;
        case MIR_OP_BRANCH:
            x86_alloc_load_int(ctx, ctx->module->arena.operands[inst->operand_start], "%r10", "%r10d");
            fprintf(ctx->out, "    testq %%r10, %%r10\n    jne .Lcobra_alloc_%zu_b%u\n    jmp .Lcobra_alloc_%zu_b%u\n",
                    ctx->function_index, inst->target,
                    ctx->function_index, inst->target2);
            return true;
        case MIR_OP_RETURN: {
            MirReg value = inst->operand_count == 1
                ? ctx->module->arena.operands[inst->operand_start] : MIR_REG_NONE;
            MirMachineType type = value != MIR_REG_NONE
                ? ctx->module->arena.regs[value].machine_type : MIR_TYPE_VOID;

            /* Cleanup calls clobber caller-saved registers. Preserve the
               return payload in the emitter-owned staging area before freeing
               compiler-owned allocations, then reload it for the ABI return. */
            if (value != MIR_REG_NONE) {
                if (type == MIR_TYPE_VIEW) {
                    if (!x86_alloc_load_view_component(ctx, value, false, "%r10") ||
                        !x86_alloc_load_view_component(ctx, value, true, "%r11")) return false;
                    fprintf(ctx->out, "    movq %%r10, ");
                    x86_alloc_mem(ctx->out, ctx->temp_offsets[0], "rbp");
                    fprintf(ctx->out, "\n    movq %%r11, ");
                    x86_alloc_mem(ctx->out, ctx->temp_offsets[1], "rbp");
                    fprintf(ctx->out, "\n");
                } else if (x86_alloc_is_float(type)) {
                    x86_alloc_load_float(ctx, value, "%xmm14");
                    fprintf(ctx->out, "    %s %%xmm14, ", type == MIR_TYPE_F32 ? "movss" : "movsd");
                    x86_alloc_mem(ctx->out, ctx->temp_offsets[0], "rbp");
                    fprintf(ctx->out, "\n");
                } else {
                    if (!x86_alloc_load_int(ctx, value, "%r10", "%r10d")) return false;
                    fprintf(ctx->out, "    movq %%r10, ");
                    x86_alloc_mem(ctx->out, ctx->temp_offsets[0], "rbp");
                    fprintf(ctx->out, "\n");
                }
            }
            if (!x86_alloc_emit_frame_cleanup(ctx,
                                              value != MIR_REG_NONE && type == MIR_TYPE_VIEW
                                                  ? value : MIR_REG_NONE)) return false;
            if (value != MIR_REG_NONE) {
                if (type == MIR_TYPE_VIEW) {
                    fprintf(ctx->out, "    movq ");
                    x86_alloc_mem(ctx->out, ctx->temp_offsets[0], "rbp");
                    fprintf(ctx->out, ", %%rax\n    movq ");
                    x86_alloc_mem(ctx->out, ctx->temp_offsets[1], "rbp");
                    fprintf(ctx->out, ", %%rdx\n");
                } else if (x86_alloc_is_float(type)) {
                    fprintf(ctx->out, "    %s ", type == MIR_TYPE_F32 ? "movss" : "movsd");
                    x86_alloc_mem(ctx->out, ctx->temp_offsets[0], "rbp");
                    fprintf(ctx->out, ", %%xmm0\n");
                } else {
                    fprintf(ctx->out, "    movq ");
                    x86_alloc_mem(ctx->out, ctx->temp_offsets[0], "rbp");
                    fprintf(ctx->out, ", %%rax\n");
                }
            }
            x86_alloc_emit_callee_saved_restore(ctx);
            fprintf(ctx->out, "    leave\n    ret\n");
            return true;
        }
        default: return false;
    }
}

bool bir_x86_64_emit_allocated(const MirModule *module,
                               const MirAllocation *allocation,
                               FILE *out, char *errbuf, size_t errbuf_size) {
    if (errbuf && errbuf_size) errbuf[0] = '\0';
    if (!module || !allocation || allocation->source != module || !out ||
        !mir_allocation_verify(allocation, errbuf, errbuf_size)) return false;
    fprintf(out, ".text\n");
    for (size_t f = 0; f < module->function_count; f++) {
        if (module->source->functions[f].is_extern) continue;
        X86AllocatedContext ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.module = module;
        ctx.allocation = allocation;
        ctx.function = &module->functions[f];
        ctx.function_index = f;
        ctx.out = out;
        ctx.errbuf = errbuf;
        ctx.errbuf_size = errbuf_size;
        if (!x86_alloc_prepare_frame(&ctx)) {
            free(ctx.spill_offsets);
            free(ctx.view_length_offsets);
            return false;
        }
        fprintf(out, ".globl %s\n.type %s, @function\n%s:\n",
                ctx.function->name, ctx.function->name, ctx.function->name);
        fprintf(out, "    pushq %%rbp\n    movq %%rsp, %%rbp\n");
        if (ctx.frame_size) fprintf(out, "    subq $%u, %%rsp\n", (unsigned)ctx.frame_size);
        x86_alloc_emit_callee_saved_save(&ctx);
        if (!x86_alloc_initialize_owned_views(&ctx)) {
            x86_alloc_error(errbuf, errbuf_size, "could not initialize native owned-slice storage");
            free(ctx.spill_offsets);
            free(ctx.view_length_offsets);
            return false;
        }
        for (size_t offset = 0; offset < ctx.function->block_count; offset++) {
            MirBlockRef block_ref = ctx.function->first_block + (MirBlockRef)offset;
            const MirBlock *block = &module->arena.blocks[block_ref];
            fprintf(out, ".Lcobra_alloc_%zu_b%u:\n", f, block_ref);
            for (size_t i = 0; i < block->inst_count; i++) {
                const MirInst *inst = &module->arena.insts[block->insts[i]];
                if (!x86_alloc_emit_inst(&ctx, inst)) {
                    x86_alloc_error(errbuf, errbuf_size,
                                    "allocated x86-64 emitter does not support MIR opcode '%s' in function '%s'",
                                    mir_opcode_name(inst->op), ctx.function->name);
                    free(ctx.spill_offsets);
            free(ctx.view_length_offsets);
                    return false;
                }
            }
        }
        fprintf(out, ".size %s, .-%s\n", ctx.function->name, ctx.function->name);
        free(ctx.spill_offsets);
            free(ctx.view_length_offsets);
    }
    fprintf(out, ".section .note.GNU-stack,\"\",@progbits\n");
    return true;
}
