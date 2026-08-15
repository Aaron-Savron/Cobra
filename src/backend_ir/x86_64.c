/*
 * Cobra isolated Linux x86-64 assembly emitter.
 *
 * This is a correctness-first bridge from MIR to executable assembly. Every
 * virtual register receives a frame spill slot. Register allocation is not
 * performed here, so the emitter uses fixed scratch registers and preserves
 * all live values in memory across calls.
 */
#include "x86_64.h"
#include <limits.h>
#include <stdarg.h>

#define X86_MAX_STACK_SLOTS BIR_MAX_STACK_SLOTS

static void x86_error(char *buffer, size_t capacity, const char *fmt, ...) {
    if (!buffer || capacity == 0 || buffer[0]) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, capacity, fmt, args);
    va_end(args);
}

typedef struct {
    const MirModule *module;
    const MirFunction *function;
    size_t function_index;
    FILE *out;
    int64_t *reg_offsets;
    int64_t *view_length_offsets;
    uint32_t view_fail_labels;
    uint32_t dict_key_labels;
    int64_t memory_offsets[X86_MAX_STACK_SLOTS];
    bool memory_seen[X86_MAX_STACK_SLOTS];
    uint32_t frame_size;
    uint32_t outgoing_stack_size;
    char *errbuf;
    size_t errbuf_size;
} X86Context;

static uint32_t x86_align_up(uint32_t value, uint32_t alignment) {
    if (alignment <= 1) return value;
    uint32_t remainder = value % alignment;
    return remainder ? value + alignment - remainder : value;
}

static MirMachineType x86_machine_type_for_cobra(const CobraType *type);

static bool x86_is_integer(MirMachineType type) {
    return type == MIR_TYPE_I8 || type == MIR_TYPE_I32 || type == MIR_TYPE_U32 ||
           type == MIR_TYPE_I64 || type == MIR_TYPE_U64 || type == MIR_TYPE_BOOL ||
           type == MIR_TYPE_ADDRESS;
}

static bool x86_is_float(MirMachineType type) {
    return type == MIR_TYPE_F32 || type == MIR_TYPE_F64;
}

static bool x86_is_supported_value(MirMachineType type) {
    return x86_is_integer(type) || x86_is_float(type);
}

static bool x86_is_scalar_field_aggregate(const CobraType *type) {
    if (!type || !type->finalized) return false;
    if (type->kind == COBRA_TYPE_STRUCT) {
        if (type->field_count == 0) return false;
        for (size_t i = 0; i < type->field_count; i++) {
            const CobraType *field = type->fields[i].type;
            if (!field ||
                (!x86_is_supported_value(x86_machine_type_for_cobra(field)) &&
                 !bir_type_has_owned_payload(field) &&
                 !x86_is_scalar_field_aggregate(field))) return false;
        }
        return true;
    }
    if (bir_is_sum_type(type)) {
        for (size_t i = 0; i < type->generic_arg_count; i++) {
            const CobraType *component = type->generic_args[i];
            if (!component) continue; /* unit enum variant */
            if (!x86_is_supported_value(x86_machine_type_for_cobra(component)) &&
                !bir_is_owned_slice_type(component) &&
                !x86_is_scalar_field_aggregate(component)) return false;
        }
        return true;
    }
    if (type->kind == COBRA_TYPE_ARRAY) {
        return type->generic_arg_count == 1 && type->array_length > 0 &&
               type->array_length <= COBRA_MAX_ARRAY_ELEMENTS &&
               type->generic_args[0] &&
               (x86_is_supported_value(x86_machine_type_for_cobra(type->generic_args[0])) ||
                x86_is_scalar_field_aggregate(type->generic_args[0]));
    }
    return false;
}

static bool x86_is_supported_memory_type(const CobraType *type) {
    return type && (x86_is_supported_value(x86_machine_type_for_cobra(type)) ||
                    x86_is_scalar_field_aggregate(type));
}

static bool x86_gpr_name(uint16_t index, const char **name) {
    static const char *const names[] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
    if (!name || index >= sizeof(names) / sizeof(names[0])) return false;
    *name = names[index];
    return true;
}

static bool x86_gpr_name32(uint16_t index, const char **name) {
    static const char *const names[] = {"%edi", "%esi", "%edx", "%ecx", "%r8d", "%r9d"};
    if (!name || index >= sizeof(names) / sizeof(names[0])) return false;
    *name = names[index];
    return true;
}

static MirMachineType x86_machine_type_for_cobra(const CobraType *type) {
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
            if (cobra_type_is_slice_kind(type->kind) || bir_is_owned_buffer_type(type))
                return MIR_TYPE_VIEW;
            if (type->kind == COBRA_TYPE_ENUM) return MIR_TYPE_I64;
            return MIR_TYPE_I64;
    }
}

static bool x86_xmm_name(uint16_t index, char *buffer, size_t capacity) {
    if (!buffer || capacity == 0 || index >= BIR_ABI_MAX_XMM_ARGUMENT_REGISTERS) return false;
    snprintf(buffer, capacity, "%%xmm%u", (unsigned)index);
    return true;
}

static int64_t x86_reg_offset(const X86Context *ctx, MirReg reg) {
    return reg < ctx->module->arena.reg_count ? ctx->reg_offsets[reg] : 0;
}

static int64_t x86_view_length_offset(const X86Context *ctx, MirReg reg) {
    return reg < ctx->module->arena.reg_count ? ctx->view_length_offsets[reg] : 0;
}

static int64_t x86_memory_offset(const X86Context *ctx, uint32_t slot) {
    return slot < X86_MAX_STACK_SLOTS ? ctx->memory_offsets[slot] : 0;
}

static void x86_mem_operand(FILE *out, int64_t offset, const char *base) {
    fprintf(out, "%lld(%%%s)", (long long)offset, base);
}

static void x86_reg_mem(FILE *out, const X86Context *ctx, MirReg reg) {
    x86_mem_operand(out, x86_reg_offset(ctx, reg), "rbp");
}

static bool x86_emit_load_integer(const X86Context *ctx, MirReg reg,
                                  const char *target64, const char *target32) {
    if (!ctx || !ctx->out || reg == MIR_REG_NONE || reg >= ctx->module->arena.reg_count)
        return false;
    MirMachineType type = ctx->module->arena.regs[reg].machine_type;
    fprintf(ctx->out, "    ");
    if (type == MIR_TYPE_I8 || type == MIR_TYPE_BOOL) {
        fprintf(ctx->out, "movzbq ");
        x86_reg_mem(ctx->out, ctx, reg);
        fprintf(ctx->out, ", %s\n", target64);
    } else if (type == MIR_TYPE_I32 || type == MIR_TYPE_U32) {
        fprintf(ctx->out, "movl ");
        x86_reg_mem(ctx->out, ctx, reg);
        fprintf(ctx->out, ", %s\n", target32);
    } else {
        fprintf(ctx->out, "movq ");
        x86_reg_mem(ctx->out, ctx, reg);
        fprintf(ctx->out, ", %s\n", target64);
    }
    return true;
}

static bool x86_emit_store_integer(const X86Context *ctx, MirReg reg,
                                   const char *source64, const char *source32) {
    if (!ctx || !ctx->out || reg == MIR_REG_NONE || reg >= ctx->module->arena.reg_count)
        return false;
    MirMachineType type = ctx->module->arena.regs[reg].machine_type;
    fprintf(ctx->out, "    ");
    if (type == MIR_TYPE_I8 || type == MIR_TYPE_BOOL) {
        fprintf(ctx->out, "movl %s, ", source32);
        x86_reg_mem(ctx->out, ctx, reg);
    } else if (type == MIR_TYPE_I32 || type == MIR_TYPE_U32) {
        fprintf(ctx->out, "movl %s, ", source32);
        x86_reg_mem(ctx->out, ctx, reg);
    } else {
        fprintf(ctx->out, "movq %s, ", source64);
        x86_reg_mem(ctx->out, ctx, reg);
    }
    fprintf(ctx->out, "\n");
    return true;
}

static bool x86_emit_load_float(const X86Context *ctx, MirReg reg, const char *xmm) {
    MirMachineType type = ctx->module->arena.regs[reg].machine_type;
    fprintf(ctx->out, "    %s ", type == MIR_TYPE_F32 ? "movss" : "movsd");
    x86_reg_mem(ctx->out, ctx, reg);
    fprintf(ctx->out, ", %s\n", xmm);
    return true;
}

static bool x86_emit_store_view_component(const X86Context *ctx, MirReg reg,
                                           bool length, const char *source64) {
    if (!ctx || !ctx->out || reg == MIR_REG_NONE || reg >= ctx->module->arena.reg_count ||
        ctx->module->arena.regs[reg].machine_type != MIR_TYPE_VIEW) return false;
    fprintf(ctx->out, "    movq %s, ", source64);
    x86_mem_operand(ctx->out, length ? x86_view_length_offset(ctx, reg)
                                   : x86_reg_offset(ctx, reg), "rbp");
    fprintf(ctx->out, "\n");
    return true;
}

static bool x86_emit_load_view_component(const X86Context *ctx, MirReg reg,
                                          bool length, const char *target64) {
    if (!ctx || !ctx->out || reg == MIR_REG_NONE || reg >= ctx->module->arena.reg_count ||
        ctx->module->arena.regs[reg].machine_type != MIR_TYPE_VIEW) return false;
    fprintf(ctx->out, "    movq ");
    x86_mem_operand(ctx->out, length ? x86_view_length_offset(ctx, reg)
                                   : x86_reg_offset(ctx, reg), "rbp");
    fprintf(ctx->out, ", %s\n", target64);
    return true;
}

static bool x86_emit_store_float(const X86Context *ctx, MirReg reg, const char *xmm) {
    MirMachineType type = ctx->module->arena.regs[reg].machine_type;
    fprintf(ctx->out, "    %s %s, ", type == MIR_TYPE_F32 ? "movss" : "movsd", xmm);
    x86_reg_mem(ctx->out, ctx, reg);
    fprintf(ctx->out, "\n");
    return true;
}

static bool x86_prepare_frame(X86Context *ctx) {
    const MirArena *arena = &ctx->module->arena;
    ctx->reg_offsets = calloc(arena->reg_count ? arena->reg_count : 1,
                              sizeof(*ctx->reg_offsets));
    ctx->view_length_offsets = calloc(arena->reg_count ? arena->reg_count : 1,
                                      sizeof(*ctx->view_length_offsets));
    if (!ctx->reg_offsets || !ctx->view_length_offsets) {
        free(ctx->reg_offsets);
        free(ctx->view_length_offsets);
        x86_error(ctx->errbuf, ctx->errbuf_size, "out of memory allocating MIR spill slots");
        return false;
    }
    uint32_t cursor = 0;
    for (size_t reg = 1; reg < arena->reg_count; reg++) {
        if (arena->regs[reg].function_index != ctx->function_index) continue;
        cursor = x86_align_up(cursor, 8);
        if (cursor > UINT32_MAX - 16) {
            x86_error(ctx->errbuf, ctx->errbuf_size, "MIR function frame is too large");
            return false;
        }
        cursor += 8;
        ctx->reg_offsets[reg] = -(int64_t)cursor;
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
                if (callee->call_abi.stack_size > ctx->outgoing_stack_size)
                    ctx->outgoing_stack_size = callee->call_abi.stack_size;
            }
            if (inst->op != MIR_OP_STACK_SLOT) continue;
            if (inst->stack_slot >= X86_MAX_STACK_SLOTS || !inst->memory_type ||
                !x86_is_supported_memory_type(inst->memory_type)) {
                x86_error(ctx->errbuf, ctx->errbuf_size,
                          "x86-64 emitter supports only scalar and scalar-field aggregate stack slots");
                return false;
            }
            if (ctx->memory_seen[inst->stack_slot]) continue;
            uint32_t alignment = inst->memory_alignment ? inst->memory_alignment : 8;
            uint32_t size = inst->memory_width ? inst->memory_width : 8;
            cursor = x86_align_up(cursor, alignment > 8 ? 8 : alignment);
            if (cursor > UINT32_MAX - x86_align_up(size, 8)) {
                x86_error(ctx->errbuf, ctx->errbuf_size, "MIR function frame is too large");
                return false;
            }
            cursor += x86_align_up(size, 8);
            ctx->memory_seen[inst->stack_slot] = true;
            ctx->memory_offsets[inst->stack_slot] = -(int64_t)cursor;
        }
    }
    uint64_t total = (uint64_t)cursor + ctx->outgoing_stack_size;
    if (total > UINT32_MAX) {
        x86_error(ctx->errbuf, ctx->errbuf_size, "MIR outgoing call area is too large");
        return false;
    }
    ctx->frame_size = x86_align_up((uint32_t)total, 16);
    return true;
}

static bool x86_emit_prologue(X86Context *ctx) {
    fprintf(ctx->out, "    pushq %%rbp\n    movq %%rsp, %%rbp\n");
    if (ctx->frame_size)
        fprintf(ctx->out, "    subq $%u, %%rsp\n", (unsigned)ctx->frame_size);
    return true;
}

static void x86_emit_epilogue(X86Context *ctx) {
    fprintf(ctx->out, "    leave\n    ret\n");
}

static bool x86_emit_const(X86Context *ctx, const MirInst *inst) {
    if (!inst->has_immediate || inst->result == MIR_REG_NONE) return false;
    MirMachineType type = inst->machine_type;
    if (type == MIR_TYPE_F32) {
        fprintf(ctx->out, "    movl $%u, %%r10d\n", (unsigned)inst->immediate.payload.f32_bits);
        return x86_emit_store_integer(ctx, inst->result, "%r10", "%r10d");
    }
    if (type == MIR_TYPE_F64) {
        fprintf(ctx->out, "    movabsq $0x%llx, %%r10\n",
                (unsigned long long)inst->immediate.payload.f64_bits);
        return x86_emit_store_integer(ctx, inst->result, "%r10", "%r10d");
    }
    fprintf(ctx->out, "    movabsq $%lld, %%r10\n",
            (long long)inst->immediate.payload.i64);
    return x86_emit_store_integer(ctx, inst->result, "%r10", "%r10d");
}

static bool x86_emit_abi_move(X86Context *ctx, const MirInst *inst) {
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
                if (!x86_gpr_name(location->register_index, &source) ||
                    !x86_emit_store_view_component(ctx, inst->result, part == 1, source))
                    return false;
            } else if (location->storage == BIR_ABI_STORAGE_STACK) {
                fprintf(ctx->out, "    movq ");
                x86_mem_operand(ctx->out, 16 + location->stack_offset, "rbp");
                fprintf(ctx->out, ", %%r10\n");
                if (!x86_emit_store_view_component(ctx, inst->result, part == 1, "%r10"))
                    return false;
            } else return false;
        }
        return true;
    }
    if (inst->abi_locations.count != 1 || !x86_is_supported_value(inst->machine_type)) return false;
    const BirAbiLocation *location = &inst->abi_locations.parts[0];
    if (x86_is_float(inst->machine_type)) {
        char xmm[16];
        if (location->storage == BIR_ABI_STORAGE_REGISTER &&
            !x86_xmm_name(location->register_index, xmm, sizeof(xmm))) return false;
        if (location->storage == BIR_ABI_STORAGE_REGISTER) {
            fprintf(ctx->out, "    %s %s, ", inst->machine_type == MIR_TYPE_F32 ? "movss" : "movsd", xmm);
            x86_reg_mem(ctx->out, ctx, inst->result);
            fprintf(ctx->out, "\n");
        } else if (location->storage == BIR_ABI_STORAGE_STACK) {
            const char *scratch = "%xmm14";
            fprintf(ctx->out, "    %s ", inst->machine_type == MIR_TYPE_F32 ? "movss" : "movsd");
            x86_mem_operand(ctx->out, 16 + location->stack_offset, "rbp");
            fprintf(ctx->out, ", %s\n", scratch);
            x86_emit_store_float(ctx, inst->result, scratch);
        } else return false;
        return true;
    }
    const char *gpr = NULL;
    if (location->storage == BIR_ABI_STORAGE_REGISTER &&
        !x86_gpr_name(location->register_index, &gpr)) return false;
    if (location->storage == BIR_ABI_STORAGE_REGISTER) {
        if (inst->machine_type == MIR_TYPE_I8 ||
            inst->machine_type == MIR_TYPE_I32 || inst->machine_type == MIR_TYPE_U32) {
            const char *gpr32 = NULL;
            if (!x86_gpr_name32(location->register_index, &gpr32)) return false;
            fprintf(ctx->out, "    movl %s, ", gpr32);
            x86_reg_mem(ctx->out, ctx, inst->result);
            fprintf(ctx->out, "\n");
        } else if (inst->machine_type == MIR_TYPE_I8 || inst->machine_type == MIR_TYPE_BOOL) {
            fprintf(ctx->out, "    movq %s, ", gpr);
            x86_reg_mem(ctx->out, ctx, inst->result);
            fprintf(ctx->out, "\n");
        } else {
            fprintf(ctx->out, "    movq %s, ", gpr);
            x86_reg_mem(ctx->out, ctx, inst->result);
            fprintf(ctx->out, "\n");
        }
    } else if (location->storage == BIR_ABI_STORAGE_STACK) {
        fprintf(ctx->out, "    movq ");
        x86_mem_operand(ctx->out, 16 + location->stack_offset, "rbp");
        fprintf(ctx->out, ", %%r10\n");
        x86_emit_store_integer(ctx, inst->result, "%r10", "%r10d");
    } else return false;
    return true;
}

static bool x86_emit_binary(X86Context *ctx, const MirInst *inst) {
    MirReg lhs = ctx->module->arena.operands[inst->operand_start];
    MirReg rhs = ctx->module->arena.operands[inst->operand_start + 1];
    if (x86_is_float(inst->machine_type)) {
        x86_emit_load_float(ctx, lhs, "%xmm14");
        x86_emit_load_float(ctx, rhs, "%xmm15");
        const char *mnemonic = NULL;
        if (inst->op == MIR_OP_ADD) mnemonic = inst->machine_type == MIR_TYPE_F32 ? "addss" : "addsd";
        if (inst->op == MIR_OP_SUB) mnemonic = inst->machine_type == MIR_TYPE_F32 ? "subss" : "subsd";
        if (inst->op == MIR_OP_MUL) mnemonic = inst->machine_type == MIR_TYPE_F32 ? "mulss" : "mulsd";
        if (inst->op == MIR_OP_DIV) mnemonic = inst->machine_type == MIR_TYPE_F32 ? "divss" : "divsd";
        if (!mnemonic) return false;
        fprintf(ctx->out, "    %s %%xmm15, %%xmm14\n", mnemonic);
        return x86_emit_store_float(ctx, inst->result, "%xmm14");
    }
    if (!x86_emit_load_integer(ctx, lhs, "%r10", "%r10d") ||
        !x86_emit_load_integer(ctx, rhs, "%r11", "%r11d")) return false;
    const char *mnemonic = NULL;
    bool wide = inst->machine_type != MIR_TYPE_I8 &&
                inst->machine_type != MIR_TYPE_I32 && inst->machine_type != MIR_TYPE_U32;
    if (inst->op == MIR_OP_ADD) mnemonic = wide ? "addq" : "addl";
    if (inst->op == MIR_OP_SUB) mnemonic = wide ? "subq" : "subl";
    if (inst->op == MIR_OP_MUL) mnemonic = wide ? "imulq" : "imull";
    if (!mnemonic) return false;
    fprintf(ctx->out, "    %s %%r11%s, %%r10%s\n", mnemonic,
            wide ? "" : "d", wide ? "" : "d");
    return x86_emit_store_integer(ctx, inst->result, "%r10", "%r10d");
}

static bool x86_emit_div_rem(X86Context *ctx, const MirInst *inst) {
    MirReg lhs = ctx->module->arena.operands[inst->operand_start];
    MirReg rhs = ctx->module->arena.operands[inst->operand_start + 1];
    bool wide = inst->machine_type != MIR_TYPE_I8 &&
                inst->machine_type != MIR_TYPE_I32 && inst->machine_type != MIR_TYPE_U32;
    x86_emit_load_integer(ctx, lhs, "%rax", "%eax");
    x86_emit_load_integer(ctx, rhs, "%r10", "%r10d");
    bool unsigned_op = inst->machine_type == MIR_TYPE_I8 ||
                       inst->machine_type == MIR_TYPE_U32 ||
                       inst->machine_type == MIR_TYPE_U64;
    if (wide) {
        if (unsigned_op) fprintf(ctx->out, "    xorq %%rdx, %%rdx\n");
        else fprintf(ctx->out, "    cqto\n");
        fprintf(ctx->out, "    %sq %%r10%s\n", unsigned_op ? "div" : "idiv",
                "");
    } else {
        if (unsigned_op) fprintf(ctx->out, "    xorl %%edx, %%edx\n");
        else fprintf(ctx->out, "    cltd\n");
        fprintf(ctx->out, "    %sl %%r10d\n", unsigned_op ? "div" : "idiv");
    }
    if (inst->op == MIR_OP_REM)
        return x86_emit_store_integer(ctx, inst->result, "%rdx", "%edx");
    return x86_emit_store_integer(ctx, inst->result, "%rax", "%eax");
}

static bool x86_emit_neg(X86Context *ctx, const MirInst *inst) {
    MirReg operand = ctx->module->arena.operands[inst->operand_start];
    if (x86_is_float(inst->machine_type)) {
        x86_emit_load_float(ctx, operand, "%xmm14");
        if (inst->machine_type == MIR_TYPE_F32) {
            fprintf(ctx->out, "    movd %%xmm14, %%r10d\n    movl $0x80000000, %%r11d\n    xorl %%r11d, %%r10d\n    movd %%r10d, %%xmm14\n");
        } else {
            fprintf(ctx->out, "    movq %%xmm14, %%r10\n    movabsq $0x8000000000000000, %%r11\n    xorq %%r11, %%r10\n    movq %%r10, %%xmm14\n");
        }
        return x86_emit_store_float(ctx, inst->result, "%xmm14");
    }
    x86_emit_load_integer(ctx, operand, "%r10", "%r10d");
    fprintf(ctx->out, "    neg%s %%r10%s\n",
            inst->machine_type == MIR_TYPE_I8 ||
            inst->machine_type == MIR_TYPE_I32 || inst->machine_type == MIR_TYPE_U32 ? "l" : "q",
            inst->machine_type == MIR_TYPE_I8 ||
            inst->machine_type == MIR_TYPE_I32 || inst->machine_type == MIR_TYPE_U32 ? "d" : "");
    return x86_emit_store_integer(ctx, inst->result, "%r10", "%r10d");
}

static bool x86_emit_compare(X86Context *ctx, const MirInst *inst) {
    MirReg lhs = ctx->module->arena.operands[inst->operand_start];
    MirReg rhs = ctx->module->arena.operands[inst->operand_start + 1];
    if (x86_is_float(ctx->module->arena.regs[lhs].machine_type)) {
        x86_emit_load_float(ctx, lhs, "%xmm14");
        x86_emit_load_float(ctx, rhs, "%xmm15");
        fprintf(ctx->out, "    ucomis%s %%xmm15, %%xmm14\n",
                ctx->module->arena.regs[lhs].machine_type == MIR_TYPE_F32 ? "s" : "d");
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
        if (inst->op == MIR_OP_NE) fprintf(ctx->out, "    orb %%dl, %%al\n");
        else fprintf(ctx->out, "    andb %%dl, %%al\n");
        fprintf(ctx->out, "    movzbq %%al, %%r10\n");
        return x86_emit_store_integer(ctx, inst->result, "%r10", "%r10d");
    }
    if (!x86_emit_load_integer(ctx, lhs, "%r10", "%r10d") ||
        !x86_emit_load_integer(ctx, rhs, "%r11", "%r11d")) return false;
    bool wide = ctx->module->arena.regs[lhs].machine_type != MIR_TYPE_I8 &&
                ctx->module->arena.regs[lhs].machine_type != MIR_TYPE_I32 &&
                ctx->module->arena.regs[lhs].machine_type != MIR_TYPE_U32;
    fprintf(ctx->out, "    cmp%s %%r11%s, %%r10%s\n", wide ? "q" : "l",
            wide ? "" : "d", wide ? "" : "d");
    bool unsigned_op = ctx->module->arena.regs[lhs].machine_type == MIR_TYPE_I8 ||
                       ctx->module->arena.regs[lhs].machine_type == MIR_TYPE_U32 ||
                       ctx->module->arena.regs[lhs].machine_type == MIR_TYPE_U64;
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
    fprintf(ctx->out, "    %s %%al\n    movzbq %%al, %%r10\n", condition);
    return x86_emit_store_integer(ctx, inst->result, "%r10", "%r10d");
}

static bool x86_emit_view_make(X86Context *ctx, const MirInst *inst) {
    if (inst->machine_type != MIR_TYPE_VIEW || inst->operand_count != 2 ||
        inst->result == MIR_REG_NONE || !inst->memory_type ||
        inst->memory_type->size == 0) return false;
    MirReg pointer = ctx->module->arena.operands[inst->operand_start];
    MirReg length = ctx->module->arena.operands[inst->operand_start + 1];
    MirReg source = inst->view_source;
    char fail[64], done[64];
    uint32_t label = ctx->view_fail_labels++;
    snprintf(fail, sizeof(fail), ".Lcobra_view_fail_%zu_%u", ctx->function_index, label);
    snprintf(done, sizeof(done), ".Lcobra_view_done_%zu_%u", ctx->function_index, label);
    if (!x86_emit_load_integer(ctx, pointer, "%r10", "%r10d") ||
        !x86_emit_load_integer(ctx, length, "%r11", "%r11d")) return false;
    fprintf(ctx->out, "    cmpq $0, %%r11\n    jl %s\n", fail);
    if (source != MIR_REG_NONE) {
        if (!x86_emit_load_view_component(ctx, source, false, "%rax") ||
            !x86_emit_load_view_component(ctx, source, true, "%rdx")) return false;
        fprintf(ctx->out, "    cmpq %%rax, %%r10\n    jb %s\n", fail);
        fprintf(ctx->out, "    subq %%rax, %%r10\n    imulq $%u, %%rdx\n",
                (unsigned)inst->memory_type->size);
        fprintf(ctx->out, "    movq %%r11, %%rcx\n    imulq $%u, %%rcx\n    addq %%rcx, %%r10\n    cmpq %%rdx, %%r10\n    ja %s\n",
                (unsigned)inst->memory_type->size, fail);
    }
    if (!x86_emit_load_integer(ctx, pointer, "%r10", "%r10d") ||
        !x86_emit_store_view_component(ctx, inst->result, false, "%r10") ||
        !x86_emit_store_view_component(ctx, inst->result, true, "%r11")) return false;
    fprintf(ctx->out, "    jmp %s\n%s:\n    ud2\n%s:\n", done, fail, done);
    return true;
}

static bool x86_emit_ptr_add(X86Context *ctx, const MirInst *inst) {
    MirReg pointer = ctx->module->arena.operands[inst->operand_start];
    MirReg offset = ctx->module->arena.operands[inst->operand_start + 1];
    if (!x86_emit_load_integer(ctx, pointer, "%r10", "%r10d") ||
        !x86_emit_load_integer(ctx, offset, "%r11", "%r11d")) return false;
    if (inst->view_source != MIR_REG_NONE) {
        const CobraType *view_type = ctx->module->arena.regs[inst->view_source].type;
        const CobraType *element = view_type ? cobra_type_element(view_type) : NULL;
        if (!element || element->size == 0 ||
            !x86_emit_load_view_component(ctx, inst->view_source, true, "%rdx")) return false;
        char fail[64], done[64];
        uint32_t label = ctx->view_fail_labels++;
        snprintf(fail, sizeof(fail), ".Lcobra_view_fail_%zu_%u", ctx->function_index, label);
        snprintf(done, sizeof(done), ".Lcobra_view_done_%zu_%u", ctx->function_index, label);
        fprintf(ctx->out, "    cmpq $0, %%r11\n    jl %s\n    imulq $%u, %%rdx\n    cmpq %%rdx, %%r11\n    ja %s\n    jmp %s\n%s:\n    ud2\n%s:\n",
                fail, (unsigned)element->size, fail, done, fail, done);
    }
    fprintf(ctx->out, "    addq %%r11, %%r10\n");
    return x86_emit_store_integer(ctx, inst->result, "%r10", "%r10d");
}

static bool x86_emit_array_index_addr(X86Context *ctx, const MirInst *inst) {
    MirReg base = ctx->module->arena.operands[inst->operand_start];
    MirReg index = ctx->module->arena.operands[inst->operand_start + 1];
    if (!inst->memory_type || inst->memory_type->kind != COBRA_TYPE_ARRAY ||
        !inst->memory_type->generic_args[0] ||
        inst->memory_type->array_length == 0 ||
        inst->memory_width != inst->memory_type->generic_args[0]->size ||
        !x86_emit_load_integer(ctx, base, "%r10", "%r10d") ||
        !x86_emit_load_integer(ctx, index, "%r11", "%r11d")) return false;
    char fail[64], done[64];
    uint32_t label = ctx->view_fail_labels++;
    snprintf(fail, sizeof(fail), ".Lcobra_array_fail_%zu_%u", ctx->function_index, label);
    snprintf(done, sizeof(done), ".Lcobra_array_done_%zu_%u", ctx->function_index, label);
    fprintf(ctx->out, "    cmpq $0, %%r11\n    jl %s\n    cmpq $%llu, %%r11\n    jae %s\n    imulq $%u, %%r11\n    addq %%r11, %%r10\n    jmp %s\n%s:\n    ud2\n%s:\n",
            fail, (unsigned long long)inst->memory_type->array_length,
            fail, (unsigned)inst->memory_width, done, fail, done);
    return x86_emit_store_integer(ctx, inst->result, "%r10", "%r10d");
}

static bool x86_emit_memory(X86Context *ctx, const MirInst *inst) {
    MirReg pointer = ctx->module->arena.operands[inst->operand_start];
    x86_emit_load_integer(ctx, pointer, "%r10", "%r10d");
    if ((inst->op == MIR_OP_LOAD || inst->op == MIR_OP_STORE) &&
        inst->view_source != MIR_REG_NONE) {
        const CobraType *view_type = ctx->module->arena.regs[inst->view_source].type;
        const CobraType *element = view_type ? cobra_type_element(view_type) : NULL;
        if (!element || element->size == 0 ||
            !x86_emit_load_view_component(ctx, inst->view_source, false, "%rax") ||
            !x86_emit_load_view_component(ctx, inst->view_source, true, "%rdx")) return false;
        char fail[64], done[64];
        uint32_t label = ctx->view_fail_labels++;
        snprintf(fail, sizeof(fail), ".Lcobra_view_fail_%zu_%u", ctx->function_index, label);
        snprintf(done, sizeof(done), ".Lcobra_view_done_%zu_%u", ctx->function_index, label);
        fprintf(ctx->out, "    cmpq %%rax, %%r10\n    jb %s\n    subq %%rax, %%r10\n    imulq $%u, %%rdx\n    addq $%u, %%r10\n    cmpq %%rdx, %%r10\n    ja %s\n    jmp %s\n%s:\n    ud2\n%s:\n",
                fail, (unsigned)element->size, (unsigned)inst->memory_width,
                fail, done, fail, done);
        x86_emit_load_integer(ctx, pointer, "%r10", "%r10d");
    }
    if (inst->op == MIR_OP_LOAD) {
        MirMachineType type = ctx->module->arena.regs[inst->result].machine_type;
        if (x86_is_float(type)) {
            fprintf(ctx->out, "    mov%s (%%r10), %%xmm14\n", type == MIR_TYPE_F32 ? "ss" : "sd");
            return x86_emit_store_float(ctx, inst->result, "%xmm14");
        }
        const char *suffix = type == MIR_TYPE_I32 || type == MIR_TYPE_U32 ? "l" : "q";
        if (type == MIR_TYPE_I8 || type == MIR_TYPE_BOOL) {
            fprintf(ctx->out, "    movzbq (%%r10), %%r11\n");
            return x86_emit_store_integer(ctx, inst->result, "%r11", "%r11d");
        }
        fprintf(ctx->out, "    mov%s (%%r10), %%r11%s\n", suffix, suffix[0] == 'l' ? "d" : "");
        return x86_emit_store_integer(ctx, inst->result, "%r11", "%r11d");
    }
    MirReg value = ctx->module->arena.operands[inst->operand_start + 1];
    MirMachineType type = ctx->module->arena.regs[value].machine_type;
    if (x86_is_float(type)) {
        x86_emit_load_float(ctx, value, "%xmm14");
        fprintf(ctx->out, "    mov%s %%xmm14, (%%r10)\n", type == MIR_TYPE_F32 ? "ss" : "sd");
        return true;
    }
    x86_emit_load_integer(ctx, value, "%r11", "%r11d");
    if (type == MIR_TYPE_I8 || type == MIR_TYPE_BOOL)
        fprintf(ctx->out, "    movb %%r11b, (%%r10)\n");
    else if (type == MIR_TYPE_I32 || type == MIR_TYPE_U32)
        fprintf(ctx->out, "    movl %%r11d, (%%r10)\n");
    else fprintf(ctx->out, "    movq %%r11, (%%r10)\n");
    return true;
}

static bool x86_emit_aggregate_copy(X86Context *ctx, const MirInst *inst) {
    if (!inst->memory_type || !x86_is_scalar_field_aggregate(inst->memory_type) ||
        inst->operand_count != 2 || inst->memory_width == 0) return false;
    MirReg destination = ctx->module->arena.operands[inst->operand_start];
    MirReg source = ctx->module->arena.operands[inst->operand_start + 1];
    if (!x86_emit_load_integer(ctx, destination, "%r10", "%r10d") ||
        !x86_emit_load_integer(ctx, source, "%r11", "%r11d")) return false;
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

static bool x86_zero_view(X86Context *ctx, MirReg view);

static bool x86_emit_owned_payload_store(X86Context *ctx, const MirInst *inst) {
    if (inst->operand_count != 2 || inst->memory_offset < 0) return false;
    MirReg destination = ctx->module->arena.operands[inst->operand_start];
    MirReg payload = ctx->module->arena.operands[inst->operand_start + 1];
    if (ctx->module->arena.regs[destination].machine_type != MIR_TYPE_ADDRESS ||
        ctx->module->arena.regs[payload].machine_type != MIR_TYPE_VIEW) return false;
    if (!x86_emit_load_integer(ctx, destination, "%r10", "%r10d") ||
        !x86_emit_load_view_component(ctx, payload, false, "%r11") ||
        !x86_emit_load_view_component(ctx, payload, true, "%rax")) return false;
    if (inst->memory_offset != 0)
        fprintf(ctx->out, "    addq $%lld, %%r10\n", (long long)inst->memory_offset);
    fprintf(ctx->out, "    movq %%r11, (%%r10)\n    movq %%rax, 8(%%r10)\n");
    return x86_zero_view(ctx, payload);
}

static bool x86_emit_owned_payload_load(X86Context *ctx, const MirInst *inst) {
    if (inst->operand_count != 1 || inst->result == MIR_REG_NONE ||
        inst->memory_offset < 0) return false;
    MirReg source = ctx->module->arena.operands[inst->operand_start];
    if (ctx->module->arena.regs[source].machine_type != MIR_TYPE_ADDRESS ||
        ctx->module->arena.regs[inst->result].machine_type != MIR_TYPE_VIEW) return false;
    if (!x86_emit_load_integer(ctx, source, "%r10", "%r10d")) return false;
    if (inst->memory_offset != 0)
        fprintf(ctx->out, "    addq $%lld, %%r10\n", (long long)inst->memory_offset);
    fprintf(ctx->out, "    movq (%%r10), %%r11\n    movq 8(%%r10), %%rax\n");
    if (!x86_emit_store_view_component(ctx, inst->result, false, "%r11") ||
        !x86_emit_store_view_component(ctx, inst->result, true, "%rax")) return false;
    fprintf(ctx->out, "    movq $0, (%%r10)\n    movq $0, 8(%%r10)\n");
    return true;
}

static bool x86_emit_copy_bytes(X86Context *ctx, MirReg destination,
                                MirReg source, uint32_t width) {
    if (width == 0 || !x86_emit_load_integer(ctx, destination, "%r10", "%r10d") ||
        !x86_emit_load_integer(ctx, source, "%r11", "%r11d")) return false;
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

static bool x86_emit_zero_bytes(X86Context *ctx, MirReg source, uint32_t width) {
    if (width == 0 || !x86_emit_load_integer(ctx, source, "%r10", "%r10d")) return false;
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

static bool x86_emit_drop_owned_value(X86Context *ctx, MirReg source,
                                       const CobraType *type, size_t base_offset) {
    if (!type || !bir_type_has_owned_payload(type)) return true;
    if (bir_is_owned_slice_type(type)) {
        if (!x86_emit_load_integer(ctx, source, "%r10", "%r10d")) return false;
        if (base_offset != 0)
            fprintf(ctx->out, "    addq $%zu, %%r10\n", base_offset);
        char done[64];
        uint32_t label = ctx->view_fail_labels++;
        snprintf(done, sizeof(done), ".Lcobra_drop_done_%zu_%u", ctx->function_index, label);
        fprintf(ctx->out, "    movq (%%r10), %%r11\n    testq %%r11, %%r11\n    je %s\n    movq $0, (%%r10)\n    movq $0, 8(%%r10)\n    movq %%r11, %%rdi\n    call free@PLT\n%s:\n", done, done);
        return true;
    }
    if (type->kind == COBRA_TYPE_STRUCT) {
        for (size_t i = 0; i < type->field_count; i++) {
            if (!x86_emit_drop_owned_value(ctx, source, type->fields[i].type,
                                            base_offset + type->fields[i].offset)) return false;
        }
        return true;
    }
    if (bir_is_sum_type(type)) {
        char none[64], done[64], second[64];
        uint32_t label = ctx->view_fail_labels++;
        snprintf(none, sizeof(none), ".Lcobra_sum_drop_none_%zu_%u", ctx->function_index, label);
        snprintf(done, sizeof(done), ".Lcobra_sum_drop_done_%zu_%u", ctx->function_index, label);
        snprintf(second, sizeof(second), ".Lcobra_sum_drop_second_%zu_%u", ctx->function_index, label);
        if (!x86_emit_load_integer(ctx, source, "%r10", "%r10d")) return false;
        if (base_offset != 0)
            fprintf(ctx->out, "    addq $%zu, %%r10\n", base_offset);
        fprintf(ctx->out, "    movq (%%r10), %%r11\n");
        if (type->kind == COBRA_TYPE_OPTION) {
            const CobraType *some = type->generic_args[0];
            fprintf(ctx->out, "    cmpq $1, %%r11\n    jne %s\n", none);
            if (!x86_emit_drop_owned_value(ctx, source, some,
                                           base_offset + bir_sum_component_offset(type, 1))) return false;
            fprintf(ctx->out, "    jmp %s\n%s:\n", done, none);
        } else {
            const CobraType *ok = type->generic_args[0];
            const CobraType *error = type->generic_args[1];
            fprintf(ctx->out, "    cmpq $1, %%r11\n    jne %s\n", second);
            if (!x86_emit_drop_owned_value(ctx, source, ok,
                                           base_offset + bir_sum_component_offset(type, 1))) return false;
            fprintf(ctx->out, "    jmp %s\n%s:\n", done, second);
            if (!x86_emit_drop_owned_value(ctx, source, error,
                                           base_offset + bir_sum_component_offset(type, 2))) return false;
        }
        fprintf(ctx->out, "%s:\n", done);
        return true;
    }
    return true;
}

static bool x86_emit_owned_aggregate_move(X86Context *ctx, const MirInst *inst) {
    if (inst->operand_count != 2 || !inst->memory_type ||
        inst->memory_width == 0) return false;
    MirReg destination = ctx->module->arena.operands[inst->operand_start];
    MirReg source = ctx->module->arena.operands[inst->operand_start + 1];
    return x86_emit_copy_bytes(ctx, destination, source, inst->memory_width) &&
           x86_emit_zero_bytes(ctx, source, inst->memory_width);
}

static bool x86_emit_owned_aggregate_drop(X86Context *ctx, const MirInst *inst) {
    if (inst->operand_count != 1 || !inst->memory_type) return false;
    MirReg source = ctx->module->arena.operands[inst->operand_start];
    if (!x86_emit_drop_owned_value(ctx, source, inst->memory_type, 0)) return false;
    return x86_emit_zero_bytes(ctx, source, (uint32_t)inst->memory_type->size);
}

static bool x86_emit_sum_check(X86Context *ctx, const MirInst *inst) {
    if (inst->operand_count != 1) return false;
    MirReg tag = ctx->module->arena.operands[inst->operand_start];
    int64_t expected = inst->sum_check_kind == 2 ? 0
        : (inst->sum_check_kind == 3 ? inst->sum_check_expected : 1);
    char fail[64], done[64];
    uint32_t label = ctx->view_fail_labels++;
    snprintf(fail, sizeof(fail), ".Lcobra_sum_fail_%zu_%u", ctx->function_index, label);
    snprintf(done, sizeof(done), ".Lcobra_sum_done_%zu_%u", ctx->function_index, label);
    if (!x86_emit_load_integer(ctx, tag, "%r10", "%r10d")) return false;
    fprintf(ctx->out, "    cmpq $%lld, %%r10\n    jne %s\n    jmp %s\n%s:\n    ud2\n%s:\n",
            (long long)expected, fail, done, fail, done);
    return true;
}

static bool x86_emit_print_i64(X86Context *ctx, const MirInst *inst) {
    if (inst->operand_count != 1) return false;
    MirReg value = ctx->module->arena.operands[inst->operand_start];
    char label[64];
    uint32_t id = ctx->dict_key_labels++;
    snprintf(label, sizeof(label), ".Lcobra_fmt_i64_%zu_%u", ctx->function_index, id);
    fprintf(ctx->out, "    .section .rodata\n%s:\n    .string \"%%ld\\n\"\n    .text\n", label);
    if (!x86_emit_load_integer(ctx, value, "%rsi", "%esi")) return false;
    fprintf(ctx->out, "    leaq %s(%%rip), %%rdi\n    xorl %%eax, %%eax\n    call printf@PLT\n", label);
    return true;
}

static bool x86_emit_print_string(X86Context *ctx, const MirInst *inst) {
    /* Not supported on the spill-all reference path: printing a string
       correctly needs a byte-at-a-time loop (see x86_alloc_emit_print_string
       for why - u8 elements are not packed bytes in this memory model), and
       this emitter has no generic scratch-slot pool to hold loop state in.
       The allocated emitter is what bir_backend_compile_program actually
       uses when allocation succeeds (always, for supported programs), so
       this is not on the production path. */
    (void)ctx; (void)inst;
    return false;
}

static bool x86_emit_assert(X86Context *ctx, const MirInst *inst) {
    if (inst->operand_count != 1) return false;
    MirReg cond = ctx->module->arena.operands[inst->operand_start];
    if (!x86_emit_load_integer(ctx, cond, "%r10", "%r10d")) return false;
    char ok[64], label[64];
    uint32_t id = ctx->dict_key_labels++;
    snprintf(ok, sizeof(ok), ".Lcobra_assert_ok_%zu_%u", ctx->function_index, id);
    snprintf(label, sizeof(label), ".Lcobra_assert_msg_%zu_%u", ctx->function_index, id);
    fprintf(ctx->out, "    testq %%r10, %%r10\n    jne %s\n", ok);
    fprintf(ctx->out, "    .section .rodata\n%s:\n    .string \"[cobra] assertion failed\"\n    .text\n", label);
    fprintf(ctx->out, "    leaq %s(%%rip), %%rdi\n    call puts@PLT\n    movl $1, %%edi\n    call exit@PLT\n", label);
    fprintf(ctx->out, "%s:\n", ok);
    return true;
}

static bool x86_emit_slice_alloc(X86Context *ctx, const MirInst *inst) {
    if (inst->result == MIR_REG_NONE || inst->operand_count != 1 ||
        inst->machine_type != MIR_TYPE_VIEW || !inst->memory_type ||
        inst->memory_type->size == 0) return false;
    MirReg length = ctx->module->arena.operands[inst->operand_start];
    char fail[64], done[64];
    uint32_t label = ctx->view_fail_labels++;
    snprintf(fail, sizeof(fail), ".Lcobra_alloc_fail_%zu_%u", ctx->function_index, label);
    snprintf(done, sizeof(done), ".Lcobra_alloc_done_%zu_%u", ctx->function_index, label);
    if (!x86_emit_load_integer(ctx, length, "%r10", "%r10d") ||
        !x86_emit_store_view_component(ctx, inst->result, true, "%r10")) return false;
    fprintf(ctx->out, "    cmpq $0, %%r10\n    jl %s\n", fail);
    fprintf(ctx->out, "    imulq $%u, %%r10\n    movq %%r10, %%rdi\n    call malloc@PLT\n    testq %%rax, %%rax\n    je %s\n",
            (unsigned)inst->memory_type->size, fail);
    if (!x86_emit_store_view_component(ctx, inst->result, false, "%rax")) return false;
    fprintf(ctx->out, "    jmp %s\n%s:\n    ud2\n%s:\n", done, fail, done);
    return true;
}

static bool x86_zero_view(X86Context *ctx, MirReg view);

static bool x86_emit_buffer_alloc(X86Context *ctx, const MirInst *inst) {
    if (inst->result == MIR_REG_NONE || inst->operand_count != 1 ||
        inst->machine_type != MIR_TYPE_VIEW || !inst->memory_type ||
        inst->memory_type->size == 0) return false;
    MirReg length = ctx->module->arena.operands[inst->operand_start];
    char fail[64], done[64], nonzero[64];
    uint32_t label = ctx->view_fail_labels++;
    snprintf(fail, sizeof(fail), ".Lcobra_buffer_alloc_fail_%zu_%u", ctx->function_index, label);
    snprintf(done, sizeof(done), ".Lcobra_buffer_alloc_done_%zu_%u", ctx->function_index, label);
    snprintf(nonzero, sizeof(nonzero), ".Lcobra_buffer_alloc_nonzero_%zu_%u", ctx->function_index, label);
    if (!x86_emit_load_integer(ctx, length, "%r10", "%r10d") ||
        !x86_emit_store_view_component(ctx, inst->result, true, "%r10")) return false;
    fprintf(ctx->out, "    cmpq $0, %%r10\n    jl %s\n    jne %s\n    movq $1, %%r10\n%s:\n",
            fail, nonzero, nonzero);
    fprintf(ctx->out, "    imulq $%u, %%r10\n    movq %%r10, %%rdi\n    call malloc@PLT\n    testq %%rax, %%rax\n    je %s\n",
            (unsigned)inst->memory_type->size, fail);
    if (!x86_emit_store_view_component(ctx, inst->result, false, "%rax")) return false;
    fprintf(ctx->out, "    jmp %s\n%s:\n    ud2\n%s:\n", done, fail, done);
    return true;
}

static bool x86_emit_buffer_append(X86Context *ctx, const MirInst *inst) {
    if (inst->operand_count != 2 || inst->result == MIR_REG_NONE ||
        !inst->memory_type || inst->memory_type->size == 0) return false;
    MirReg source = ctx->module->arena.operands[inst->operand_start];
    MirReg value = ctx->module->arena.operands[inst->operand_start + 1];
    char fail[64], done[64];
    uint32_t label = ctx->view_fail_labels++;
    snprintf(fail, sizeof(fail), ".Lcobra_buffer_append_fail_%zu_%u", ctx->function_index, label);
    snprintf(done, sizeof(done), ".Lcobra_buffer_append_done_%zu_%u", ctx->function_index, label);
    if (!x86_emit_load_view_component(ctx, source, false, "%r10") ||
        !x86_emit_load_view_component(ctx, source, true, "%r11")) return false;
    fprintf(ctx->out, "    cmpq $0, %%r11\n    jl %s\n    movq %%r11, %%rax\n    incq %%rax\n    jo %s\n    movq %%rax, %%rcx\n", fail, fail);
    if (!x86_emit_store_view_component(ctx, inst->result, true, "%rcx")) return false;
    fprintf(ctx->out, "    imulq $%u, %%rcx\n    movq %%rcx, %%rdi\n    call malloc@PLT\n    testq %%rax, %%rax\n    je %s\n",
            (unsigned)inst->memory_type->size, fail);
    if (!x86_emit_store_view_component(ctx, inst->result, false, "%rax")) return false;
    if (!x86_emit_load_view_component(ctx, inst->result, false, "%rdi") ||
        !x86_emit_load_view_component(ctx, source, false, "%rsi") ||
        !x86_emit_load_view_component(ctx, source, true, "%rdx")) return false;
    fprintf(ctx->out, "    imulq $%u, %%rdx\n    call memcpy@PLT\n",
            (unsigned)inst->memory_type->size);
    if (!x86_emit_load_view_component(ctx, inst->result, false, "%r10") ||
        !x86_emit_load_view_component(ctx, source, true, "%r11")) return false;
    fprintf(ctx->out, "    imulq $%u, %%r11\n    addq %%r11, %%r10\n",
            (unsigned)inst->memory_type->size);
    if (bir_type_is_value_only_struct(inst->memory_type)) {
        if (!x86_emit_load_integer(ctx, value, "%r11", "%r11d")) return false;
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
    } else if (x86_is_float(x86_machine_type_for_cobra(inst->memory_type))) {
        if (!x86_emit_load_float(ctx, value, "%xmm14")) return false;
        fprintf(ctx->out, "    mov%s %%xmm14, (%%r10)\n",
                inst->memory_type->kind == COBRA_TYPE_F32 ? "ss" : "sd");
    } else {
        if (!x86_emit_load_integer(ctx, value, "%r8", "%r8d")) return false;
        if (inst->memory_type->kind == COBRA_TYPE_U8 ||
            inst->memory_type->kind == COBRA_TYPE_BOOL)
            fprintf(ctx->out, "    movb %%r8b, (%%r10)\n");
        else if (inst->memory_type->size <= 4)
            fprintf(ctx->out, "    movl %%r8d, (%%r10)\n");
        else fprintf(ctx->out, "    movq %%r8, (%%r10)\n");
    }
    if (!x86_emit_load_view_component(ctx, source, false, "%rdi")) return false;
    fprintf(ctx->out, "    call free@PLT\n");
    if (!x86_zero_view(ctx, source)) return false;
    fprintf(ctx->out, "    jmp %s\n%s:\n    ud2\n%s:\n", done, fail, done);
    return true;
}

static bool x86_emit_buffer_pop(X86Context *ctx, const MirInst *inst) {
    if (inst->operand_count != 1 || inst->result == MIR_REG_NONE ||
        !inst->memory_type || inst->memory_type->size == 0) return false;
    MirReg source = ctx->module->arena.operands[inst->operand_start];
    char fail[64], done[64];
    uint32_t label = ctx->view_fail_labels++;
    snprintf(fail, sizeof(fail), ".Lcobra_buffer_pop_fail_%zu_%u", ctx->function_index, label);
    snprintf(done, sizeof(done), ".Lcobra_buffer_pop_done_%zu_%u", ctx->function_index, label);
    if (!x86_emit_load_view_component(ctx, source, false, "%r10") ||
        !x86_emit_load_view_component(ctx, source, true, "%r11")) return false;
    fprintf(ctx->out, "    cmpq $1, %%r11\n    jl %s\n    decq %%r11\n", fail);
    if (!x86_emit_store_view_component(ctx, source, true, "%r11")) return false;
    fprintf(ctx->out, "    imulq $%u, %%r11\n    addq %%r11, %%r10\n",
            (unsigned)inst->memory_type->size);
    if (x86_is_float(x86_machine_type_for_cobra(inst->memory_type))) {
        fprintf(ctx->out, "    mov%s (%%r10), %%xmm14\n",
                inst->memory_type->kind == COBRA_TYPE_F32 ? "ss" : "sd");
        if (!x86_emit_store_float(ctx, inst->result, "%xmm14")) return false;
    } else {
        if (inst->memory_type->kind == COBRA_TYPE_U8 ||
            inst->memory_type->kind == COBRA_TYPE_BOOL)
            fprintf(ctx->out, "    movzbq (%%r10), %%r8\n");
        else if (inst->memory_type->size <= 4)
            fprintf(ctx->out, "    movl (%%r10), %%r8d\n");
        else fprintf(ctx->out, "    movq (%%r10), %%r8\n");
        if (!x86_emit_store_integer(ctx, inst->result, "%r8", "%r8d")) return false;
    }
    fprintf(ctx->out, "    jmp %s\n%s:\n    ud2\n%s:\n", done, fail, done);
    return true;
}

static bool x86_zero_view(X86Context *ctx, MirReg view) {
    if (!x86_emit_store_view_component(ctx, view, false, "$0") ||
        !x86_emit_store_view_component(ctx, view, true, "$0")) return false;
    return true;
}

/* Emit a .rodata .string for a dict literal key and return its label. */
static void x86_emit_dict_key_rodata(X86Context *ctx, const char *key, char *label,
                                     size_t label_size) {
    uint32_t id = ctx->dict_key_labels++;
    snprintf(label, label_size, ".Lcobra_dictkey_%zu_%u", ctx->function_index, id);
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

/* The native dict value is the production 2-part contract: a table pointer
   (dict ptr slot) plus the entry count (dict len slot). All operations call
   the production cobra_dict_* runtime, which is linked into native test
   binaries exactly like the direct emitter's dict support. */
static bool x86_emit_dict_alloc(X86Context *ctx, const MirInst *inst) {
    if (inst->result == MIR_REG_NONE || inst->machine_type != MIR_TYPE_VIEW ||
        !inst->type || !bir_is_owned_dict_type(inst->type)) return false;
    return x86_zero_view(ctx, inst->result);
}

static bool x86_emit_dict_set(X86Context *ctx, const MirInst *inst) {
    if (inst->operand_count != 2 || inst->result == MIR_REG_NONE ||
        inst->dict_key[0] == '\0') return false;
    MirReg source = ctx->module->arena.operands[inst->operand_start];
    MirReg value = ctx->module->arena.operands[inst->operand_start + 1];
    char label[64];
    x86_emit_dict_key_rodata(ctx, inst->dict_key, label, sizeof(label));
    /* dict may rehash: pass the address of the source dict pointer slot. */
    fprintf(ctx->out, "    leaq ");
    x86_mem_operand(ctx->out, x86_reg_offset(ctx, source), "rbp");
    fprintf(ctx->out, ", %%rdi\n    leaq %s(%%rip), %%rsi\n", label);
    if (!x86_emit_load_integer(ctx, value, "%rdx", "%edx")) return false;
    fprintf(ctx->out, "    call cobra_dict_set_i64@PLT\n");
    /* Reload the possibly-rehashed pointer and the fresh entry count. The
       table may have been rehashed through the source slot, so the new
       pointer is read from there; ownership then moves to the result. */
    if (!x86_emit_load_view_component(ctx, source, false, "%r10") ||
        !x86_emit_store_view_component(ctx, inst->result, false, "%r10")) return false;
    fprintf(ctx->out, "    movq %%r10, %%rdi\n    call cobra_dict_len@PLT\n");
    if (!x86_emit_store_view_component(ctx, inst->result, true, "%rax")) return false;
    return x86_zero_view(ctx, source);
}

static bool x86_emit_dict_get(X86Context *ctx, const MirInst *inst) {
    if (inst->operand_count != 2 || inst->result == MIR_REG_NONE ||
        inst->dict_key[0] == '\0') return false;
    MirReg source = ctx->module->arena.operands[inst->operand_start];
    MirReg fallback = ctx->module->arena.operands[inst->operand_start + 1];
    char label[64];
    x86_emit_dict_key_rodata(ctx, inst->dict_key, label, sizeof(label));
    if (!x86_emit_load_view_component(ctx, source, false, "%rdi") ||
        !x86_emit_load_integer(ctx, fallback, "%rdx", "%edx")) return false;
    fprintf(ctx->out, "    leaq %s(%%rip), %%rsi\n    call cobra_dict_get_i64@PLT\n", label);
    return x86_emit_store_integer(ctx, inst->result, "%rax", "%eax");
}

static bool x86_emit_dict_has(X86Context *ctx, const MirInst *inst) {
    if (inst->operand_count != 1 || inst->result == MIR_REG_NONE ||
        inst->dict_key[0] == '\0') return false;
    MirReg source = ctx->module->arena.operands[inst->operand_start];
    char label[64];
    x86_emit_dict_key_rodata(ctx, inst->dict_key, label, sizeof(label));
    if (!x86_emit_load_view_component(ctx, source, false, "%rdi")) return false;
    fprintf(ctx->out, "    leaq %s(%%rip), %%rsi\n    call cobra_dict_has@PLT\n", label);
    return x86_emit_store_integer(ctx, inst->result, "%rax", "%eax");
}

static bool x86_emit_dict_delete(X86Context *ctx, const MirInst *inst) {
    if (inst->operand_count != 1 || inst->result == MIR_REG_NONE ||
        inst->dict_key[0] == '\0') return false;
    MirReg source = ctx->module->arena.operands[inst->operand_start];
    char label[64];
    x86_emit_dict_key_rodata(ctx, inst->dict_key, label, sizeof(label));
    fprintf(ctx->out, "    leaq ");
    x86_mem_operand(ctx->out, x86_reg_offset(ctx, source), "rbp");
    fprintf(ctx->out, ", %%rdi\n    leaq %s(%%rip), %%rsi\n    call cobra_dict_delete@PLT\n", label);
    if (!x86_emit_load_view_component(ctx, source, false, "%r10") ||
        !x86_emit_store_view_component(ctx, inst->result, false, "%r10")) return false;
    fprintf(ctx->out, "    movq %%r10, %%rdi\n    call cobra_dict_len@PLT\n");
    if (!x86_emit_store_view_component(ctx, inst->result, true, "%rax")) return false;
    return x86_zero_view(ctx, source);
}

static bool x86_emit_dict_pop(X86Context *ctx, const MirInst *inst) {
    if (inst->operand_count != 2 || inst->result == MIR_REG_NONE ||
        inst->dict_key[0] == '\0') return false;
    MirReg source = ctx->module->arena.operands[inst->operand_start];
    MirReg fallback = ctx->module->arena.operands[inst->operand_start + 1];
    char label[64];
    x86_emit_dict_key_rodata(ctx, inst->dict_key, label, sizeof(label));
    fprintf(ctx->out, "    leaq ");
    x86_mem_operand(ctx->out, x86_reg_offset(ctx, source), "rbp");
    fprintf(ctx->out, ", %%rdi\n    leaq %s(%%rip), %%rsi\n", label);
    if (!x86_emit_load_integer(ctx, fallback, "%rdx", "%edx")) return false;
    fprintf(ctx->out, "    call cobra_dict_pop@PLT\n");
    if (!x86_emit_store_integer(ctx, inst->result, "%rax", "%eax")) return false;
    /* Pop reduces the entry count in place; refresh the source length. */
    if (!x86_emit_load_view_component(ctx, source, false, "%rdi")) return false;
    fprintf(ctx->out, "    call cobra_dict_len@PLT\n");
    return x86_emit_store_view_component(ctx, source, true, "%rax");
}

static bool x86_emit_dict_len(X86Context *ctx, const MirInst *inst) {
    if (inst->operand_count != 1 || inst->result == MIR_REG_NONE) return false;
    MirReg source = ctx->module->arena.operands[inst->operand_start];
    if (!x86_emit_load_view_component(ctx, source, false, "%rdi")) return false;
    fprintf(ctx->out, "    call cobra_dict_len@PLT\n");
    return x86_emit_store_integer(ctx, inst->result, "%rax", "%eax");
}

static bool x86_emit_dict_free(X86Context *ctx, const MirInst *inst) {
    if (inst->operand_count != 1 || inst->result != MIR_REG_NONE) return false;
    MirReg source = ctx->module->arena.operands[inst->operand_start];
    fprintf(ctx->out, "    leaq ");
    x86_mem_operand(ctx->out, x86_reg_offset(ctx, source), "rbp");
    fprintf(ctx->out, ", %%rdi\n    leaq ");
    x86_mem_operand(ctx->out, x86_view_length_offset(ctx, source), "rbp");
    fprintf(ctx->out, ", %%rsi\n    call cobra_dict_free@PLT\n");
    return x86_zero_view(ctx, source);
}

static bool x86_emit_slice_free(X86Context *ctx, const MirInst *inst);

static bool x86_emit_transfer(X86Context *ctx, const MirInst *inst) {
    if (inst->operand_count != 1 || inst->result == MIR_REG_NONE) return false;
    MirReg source = ctx->module->arena.operands[inst->operand_start];
    if (ctx->module->arena.regs[source].machine_type == MIR_TYPE_VIEW) {
        if (!x86_emit_load_view_component(ctx, source, false, "%r10") ||
            !x86_emit_load_view_component(ctx, source, true, "%r11") ||
            !x86_emit_store_view_component(ctx, inst->result, false, "%r10") ||
            !x86_emit_store_view_component(ctx, inst->result, true, "%r11")) return false;
        return true;
    }
    if (!x86_emit_load_integer(ctx, source, "%r10", "%r10d")) return false;
    return x86_emit_store_integer(ctx, inst->result, "%r10", "%r10d");
}

static bool x86_emit_destroy(X86Context *ctx, const MirInst *inst) {
    if (inst->operand_count != 1) return false;
    MirReg source = ctx->module->arena.operands[inst->operand_start];
    MirMachineType type = ctx->module->arena.regs[source].machine_type;
    if (type == MIR_TYPE_VIEW) return x86_emit_slice_free(ctx, inst);
    if (type == MIR_TYPE_ADDRESS) return true;
    return false;
}

static bool x86_emit_slice_free(X86Context *ctx, const MirInst *inst) {
    if (inst->operand_count != 1) return false;
    MirReg view = ctx->module->arena.operands[inst->operand_start];
    if (ctx->module->arena.regs[view].machine_type != MIR_TYPE_VIEW) return false;
    char done[64];
    uint32_t label = ctx->view_fail_labels++;
    snprintf(done, sizeof(done), ".Lcobra_free_done_%zu_%u", ctx->function_index, label);
    if (!x86_emit_load_view_component(ctx, view, false, "%r10")) return false;
    fprintf(ctx->out, "    testq %%r10, %%r10\n    je %s\n    movq %%r10, %%rdi\n    call free@PLT\n%s:\n", done, done);
    return x86_zero_view(ctx, view);
}

static bool x86_emit_string_concat(X86Context *ctx, const MirInst *inst) {
    if (inst->operand_count != 2 || inst->result == MIR_REG_NONE ||
        inst->machine_type != MIR_TYPE_VIEW || !inst->memory_type ||
        inst->memory_type->kind != COBRA_TYPE_U8 || inst->memory_type->size == 0) return false;
    MirReg left = ctx->module->arena.operands[inst->operand_start];
    MirReg right = ctx->module->arena.operands[inst->operand_start + 1];
    if (ctx->module->arena.regs[left].machine_type != MIR_TYPE_VIEW ||
        ctx->module->arena.regs[right].machine_type != MIR_TYPE_VIEW) return false;
    char fail[64], done[64];
    uint32_t label = ctx->view_fail_labels++;
    snprintf(fail, sizeof(fail), ".Lcobra_string_concat_fail_%zu_%u", ctx->function_index, label);
    snprintf(done, sizeof(done), ".Lcobra_string_concat_done_%zu_%u", ctx->function_index, label);
    if (!x86_emit_load_view_component(ctx, left, true, "%r10") ||
        !x86_emit_load_view_component(ctx, right, true, "%r11")) return false;
    fprintf(ctx->out, "    addq %%r11, %%r10\n    jc %s\n", fail);
    if (!x86_emit_store_view_component(ctx, inst->result, true, "%r10")) return false;
    fprintf(ctx->out, "    imulq $%u, %%r10\n    movq %%r10, %%rdi\n    call malloc@PLT\n    testq %%rax, %%rax\n    je %s\n",
            (unsigned)inst->memory_type->size, fail);
    if (!x86_emit_store_view_component(ctx, inst->result, false, "%rax")) return false;
    if (!x86_emit_load_view_component(ctx, inst->result, false, "%rdi") ||
        !x86_emit_load_view_component(ctx, left, false, "%rsi") ||
        !x86_emit_load_view_component(ctx, left, true, "%rdx")) return false;
    fprintf(ctx->out, "    imulq $%u, %%rdx\n    call memcpy@PLT\n",
            (unsigned)inst->memory_type->size);
    if (!x86_emit_load_view_component(ctx, inst->result, false, "%rdi") ||
        !x86_emit_load_view_component(ctx, left, true, "%r10") ||
        !x86_emit_load_view_component(ctx, right, false, "%rsi") ||
        !x86_emit_load_view_component(ctx, right, true, "%rdx")) return false;
    fprintf(ctx->out, "    imulq $%u, %%r10\n    addq %%r10, %%rdi\n    imulq $%u, %%rdx\n    call memcpy@PLT\n    jmp %s\n%s:\n    ud2\n%s:\n",
            (unsigned)inst->memory_type->size,
            (unsigned)inst->memory_type->size, done, fail, done);
    return true;
}

static bool x86_emit_region_cleanup(X86Context *ctx, uint32_t region_id) {
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
            if (!x86_emit_load_view_component(ctx, alloc->result, false, "%r10")) return false;
            fprintf(ctx->out, "    testq %%r10, %%r10\n    je %s\n    movq %%r10, %%rdi\n    call free@PLT\n%s:\n", done, done);
            if (!x86_zero_view(ctx, alloc->result)) return false;
        }
    }
    return true;
}

static bool x86_initialize_owned_views(X86Context *ctx) {
    for (size_t offset = 0; offset < ctx->function->block_count; offset++) {
        MirBlockRef block_ref = ctx->function->first_block + (MirBlockRef)offset;
        const MirBlock *block = &ctx->module->arena.blocks[block_ref];
        for (size_t i = 0; i < block->inst_count; i++) {
            const MirInst *inst = &ctx->module->arena.insts[block->insts[i]];
            if ((inst->op != MIR_OP_SLICE_ALLOC && inst->op != MIR_OP_BUFFER_ALLOC) ||
                inst->result == MIR_REG_NONE) continue;
            if (!x86_zero_view(ctx, inst->result)) return false;
        }
    }
    return true;
}

static bool x86_emit_frame_cleanup(X86Context *ctx, MirReg protected_view) {
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
                    BIR_POINTER_CONTRACT_OWNED_SLICE ||
                (owner->op != MIR_OP_SLICE_ALLOC && owner->op != MIR_OP_BUFFER_ALLOC &&
                 owner->op != MIR_OP_BUFFER_APPEND && owner->op != MIR_OP_STRING_CONCAT &&
                 owner->op != MIR_OP_CALL)) continue;
            uint32_t owner_id = ctx->module->arena.regs[owner->result].allocation_id;
            char done[64], skip[64];
            uint32_t label = ctx->view_fail_labels++;
            snprintf(done, sizeof(done), ".Lcobra_frame_free_%zu_%u", ctx->function_index, label);
            snprintf(skip, sizeof(skip), ".Lcobra_frame_skip_%zu_%u", ctx->function_index, label);
            if (!x86_emit_load_view_component(ctx, owner->result, false, "%r10")) return false;
            fprintf(ctx->out, "    testq %%r10, %%r10\n    je %s\n", done);
            if (protected_view != MIR_REG_NONE) {
                if (protected_allocation_id != 0 && owner_id == protected_allocation_id) {
                    fprintf(ctx->out, "    jmp %s\n", skip);
                } else if (protected_allocation_id == 0 || owner_id == 0) {
                    if (!x86_emit_load_view_component(ctx, protected_view, false, "%r11")) return false;
                    fprintf(ctx->out, "    cmpq %%r11, %%r10\n    je %s\n", skip);
                }
            }
            fprintf(ctx->out, "    movq %%r10, %%rdi\n    call free@PLT\n");
            if (!x86_zero_view(ctx, owner->result)) return false;
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
        snprintf(done, sizeof(done), ".Lcobra_param_free_%zu_%u", ctx->function_index, label);
        snprintf(skip, sizeof(skip), ".Lcobra_param_skip_%zu_%u", ctx->function_index, label);
        if (!x86_emit_load_view_component(ctx, reg, false, "%r10")) return false;
        fprintf(ctx->out, "    testq %%r10, %%r10\n    je %s\n", done);
        if (protected_view != MIR_REG_NONE && protected_allocation_id != 0 &&
            owner_id == protected_allocation_id) {
            fprintf(ctx->out, "    jmp %s\n", skip);
        } else if (bir_is_owned_dict_type(info->type)) {
            fprintf(ctx->out, "    leaq -%lld(%%rbp), %%rdi\n    leaq -%lld(%%rbp), %%rsi\n    call cobra_dict_free@PLT\n",
                    (long long)x86_reg_offset(ctx, reg),
                    (long long)x86_view_length_offset(ctx, reg));
            if (!x86_zero_view(ctx, reg)) return false;
        } else {
            fprintf(ctx->out, "    movq %%r10, %%rdi\n    call free@PLT\n");
            if (!x86_zero_view(ctx, reg)) return false;
        }
        fprintf(ctx->out, "%s:\n%s:\n", skip, done);
    }
    return true;
}

static bool x86_call_arg_moves_ownership(const X86Context *ctx,
                                         const MirInst *inst, size_t arg) {
    if (!ctx || !ctx->module || !ctx->module->source ||
        inst->callee_index >= ctx->module->source->function_count) return false;
    const BirFunctionInfo *callee = &ctx->module->source->functions[inst->callee_index];
    if (callee->has_hidden_return_storage && arg == 0) return false;
    size_t user_arg = arg - (callee->has_hidden_return_storage ? 1U : 0U);
    return user_arg < callee->param_count &&
           bir_is_owned_slice_type(callee->param_value_types[user_arg]);
}

static bool x86_emit_call(X86Context *ctx, const MirInst *inst) {
    const MirFunction *callee = &ctx->module->functions[inst->callee_index];
    const BirCallAbi *abi = &callee->call_abi;
    for (size_t arg = 0; arg < inst->operand_count; arg++) {
        MirReg value = ctx->module->arena.operands[inst->operand_start + arg];
        MirMachineType type = ctx->module->arena.regs[value].machine_type;
        if (type == MIR_TYPE_VIEW) {
            /* Buffers pass 3 parts (ptr, len, cap). The callee discards the
               capacity part, so any value works; pass the length again. */
            if (abi->params[arg].count != 2 && abi->params[arg].count != 3) return false;
            for (size_t part = 0; part < abi->params[arg].count; part++) {
                const BirAbiLocation *location = &abi->params[arg].parts[part];
                if (!x86_emit_load_view_component(ctx, value, part == 1 || part == 2, "%r10")) return false;
                if (location->storage == BIR_ABI_STORAGE_REGISTER) {
                    const char *destination = NULL;
                    if (!x86_gpr_name(location->register_index, &destination)) return false;
                    fprintf(ctx->out, "    movq %%r10, %s\n", destination);
                } else if (location->storage == BIR_ABI_STORAGE_STACK) {
                    fprintf(ctx->out, "    movq %%r10, ");
                    x86_mem_operand(ctx->out, location->stack_offset, "rsp");
                    fprintf(ctx->out, "\n");
                } else return false;
            }
            continue;
        }
        if (abi->params[arg].count != 1) return false;
        const BirAbiLocation *location = &abi->params[arg].parts[0];
        if (x86_is_float(type)) {
            char xmm[16];
            if (location->storage == BIR_ABI_STORAGE_REGISTER) {
                if (!x86_xmm_name(location->register_index, xmm, sizeof(xmm))) return false;
                x86_emit_load_float(ctx, value, "%xmm14");
                fprintf(ctx->out, "    mov%s %%xmm14, %s\n", type == MIR_TYPE_F32 ? "ss" : "sd", xmm);
            } else if (location->storage == BIR_ABI_STORAGE_STACK) {
                x86_emit_load_float(ctx, value, "%xmm14");
                fprintf(ctx->out, "    mov%s %%xmm14, ", type == MIR_TYPE_F32 ? "ss" : "sd");
                x86_mem_operand(ctx->out, location->stack_offset, "rsp");
                fprintf(ctx->out, "\n");
            } else return false;
        } else {
            const char *gpr = NULL;
            if (location->storage == BIR_ABI_STORAGE_REGISTER &&
                !x86_gpr_name(location->register_index, &gpr)) return false;
            x86_emit_load_integer(ctx, value, "%r10", "%r10d");
            if (location->storage == BIR_ABI_STORAGE_REGISTER) {
                if (type == MIR_TYPE_I32 || type == MIR_TYPE_U32) {
                    const char *gpr32 = NULL;
                    if (!x86_gpr_name32(location->register_index, &gpr32)) return false;
                    fprintf(ctx->out, "    movl %%r10d, %s\n", gpr32);
                } else fprintf(ctx->out, "    movq %%r10, %s\n", gpr);
            } else if (location->storage == BIR_ABI_STORAGE_STACK) {
                fprintf(ctx->out, "    movq %%r10, ");
                x86_mem_operand(ctx->out, location->stack_offset, "rsp");
                fprintf(ctx->out, "\n");
            } else return false;
        }
    }
    for (size_t arg = 0; arg < inst->operand_count; arg++) {
        MirReg value = ctx->module->arena.operands[inst->operand_start + arg];
        if (ctx->module->arena.regs[value].machine_type == MIR_TYPE_VIEW &&
            x86_call_arg_moves_ownership(ctx, inst, arg) &&
            !x86_zero_view(ctx, value)) return false;
    }
    fprintf(ctx->out, "    call %s\n", inst->callee);
    if (inst->result != MIR_REG_NONE) {
        MirMachineType type = ctx->module->arena.regs[inst->result].machine_type;
        if (type == MIR_TYPE_VIEW) {
            if (!x86_emit_store_view_component(ctx, inst->result, false, "%rax") ||
                !x86_emit_store_view_component(ctx, inst->result, true, "%rdx")) return false;
        } else if (x86_is_float(type)) {
            x86_emit_store_float(ctx, inst->result, "%xmm0");
        } else {
            x86_emit_store_integer(ctx, inst->result, "%rax", "%eax");
        }
    }
    return true;
}

static bool x86_emit_instruction(X86Context *ctx, const MirInst *inst) {
    switch (inst->op) {
        case MIR_OP_CONST: return x86_emit_const(ctx, inst);
        case MIR_OP_ABI_MOVE: return x86_emit_abi_move(ctx, inst);
        case MIR_OP_ADD:
        case MIR_OP_SUB:
        case MIR_OP_MUL: return x86_emit_binary(ctx, inst);
        case MIR_OP_DIV:
        case MIR_OP_REM:
            if (x86_is_float(inst->machine_type)) return x86_emit_binary(ctx, inst);
            return x86_emit_div_rem(ctx, inst);
        case MIR_OP_NEG: return x86_emit_neg(ctx, inst);
        case MIR_OP_EQ:
        case MIR_OP_NE:
        case MIR_OP_LT:
        case MIR_OP_LE:
        case MIR_OP_GT:
        case MIR_OP_GE: return x86_emit_compare(ctx, inst);
        case MIR_OP_STACK_SLOT: {
            if (inst->result == MIR_REG_NONE || inst->stack_slot >= X86_MAX_STACK_SLOTS ||
                !ctx->memory_seen[inst->stack_slot]) return false;
            fprintf(ctx->out, "    leaq ");
            x86_mem_operand(ctx->out, x86_memory_offset(ctx, inst->stack_slot), "rbp");
            fprintf(ctx->out, ", %%r10\n");
            return x86_emit_store_integer(ctx, inst->result, "%r10", "%r10d");
        }
        case MIR_OP_PTR_ADD: return x86_emit_ptr_add(ctx, inst);
        case MIR_OP_VIEW_MAKE: return x86_emit_view_make(ctx, inst);
        case MIR_OP_VIEW_PTR: {
            MirReg view = ctx->module->arena.operands[inst->operand_start];
            if (!x86_emit_load_view_component(ctx, view, false, "%r10")) return false;
            return x86_emit_store_integer(ctx, inst->result, "%r10", "%r10d");
        }
        case MIR_OP_VIEW_LEN: {
            MirReg view = ctx->module->arena.operands[inst->operand_start];
            if (!x86_emit_load_view_component(ctx, view, true, "%r10")) return false;
            return x86_emit_store_integer(ctx, inst->result, "%r10", "%r10d");
        }
        case MIR_OP_FIELD_ADDR: {
            x86_emit_load_integer(ctx, ctx->module->arena.operands[inst->operand_start], "%r10", "%r10d");
            fprintf(ctx->out, "    addq $%lld, %%r10\n", (long long)inst->memory_offset);
            return x86_emit_store_integer(ctx, inst->result, "%r10", "%r10d");
        }
        case MIR_OP_ARRAY_INDEX_ADDR: return x86_emit_array_index_addr(ctx, inst);
        case MIR_OP_LOAD:
        case MIR_OP_STORE: return x86_emit_memory(ctx, inst);
        case MIR_OP_AGG_COPY: return x86_emit_aggregate_copy(ctx, inst);
        case MIR_OP_SUM_PAYLOAD_STORE:
        case MIR_OP_FIELD_PAYLOAD_STORE: return x86_emit_owned_payload_store(ctx, inst);
        case MIR_OP_SUM_PAYLOAD_LOAD:
        case MIR_OP_FIELD_PAYLOAD_LOAD: return x86_emit_owned_payload_load(ctx, inst);
        case MIR_OP_SUM_MOVE:
        case MIR_OP_AGG_MOVE: return x86_emit_owned_aggregate_move(ctx, inst);
        case MIR_OP_SUM_DROP:
        case MIR_OP_AGG_DROP: return x86_emit_owned_aggregate_drop(ctx, inst);
        case MIR_OP_SLICE_ALLOC: return x86_emit_slice_alloc(ctx, inst);
        case MIR_OP_SLICE_FREE: return x86_emit_slice_free(ctx, inst);
        case MIR_OP_BUFFER_ALLOC: return x86_emit_buffer_alloc(ctx, inst);
        case MIR_OP_BUFFER_APPEND: return x86_emit_buffer_append(ctx, inst);
        case MIR_OP_BUFFER_POP: return x86_emit_buffer_pop(ctx, inst);
        case MIR_OP_BUFFER_FREE: return x86_emit_slice_free(ctx, inst);
        case MIR_OP_DICT_ALLOC: return x86_emit_dict_alloc(ctx, inst);
        case MIR_OP_DICT_SET: return x86_emit_dict_set(ctx, inst);
        case MIR_OP_DICT_GET: return x86_emit_dict_get(ctx, inst);
        case MIR_OP_DICT_HAS: return x86_emit_dict_has(ctx, inst);
        case MIR_OP_DICT_DELETE: return x86_emit_dict_delete(ctx, inst);
        case MIR_OP_DICT_POP: return x86_emit_dict_pop(ctx, inst);
        case MIR_OP_DICT_LEN: return x86_emit_dict_len(ctx, inst);
        case MIR_OP_DICT_FREE: return x86_emit_dict_free(ctx, inst);
        case MIR_OP_STRING_CONCAT: return x86_emit_string_concat(ctx, inst);
        case MIR_OP_SUM_CHECK: return x86_emit_sum_check(ctx, inst);
        case MIR_OP_PRINT_I64: return x86_emit_print_i64(ctx, inst);
        case MIR_OP_PRINT_STRING: return x86_emit_print_string(ctx, inst);
        case MIR_OP_ASSERT: return x86_emit_assert(ctx, inst);
        case MIR_OP_REGION_ENTER: return true;
        case MIR_OP_REGION_EXIT: return x86_emit_region_cleanup(ctx, inst->region_id);
        case MIR_OP_TRANSFER: return x86_emit_transfer(ctx, inst);
        case MIR_OP_DESTROY: return x86_emit_destroy(ctx, inst);
        case MIR_OP_CALL: return x86_emit_call(ctx, inst);
        case MIR_OP_JUMP:
            fprintf(ctx->out, "    jmp .Lcobra_%zu_b%u\n", ctx->function_index, inst->target);
            return true;
        case MIR_OP_BRANCH:
            x86_emit_load_integer(ctx, ctx->module->arena.operands[inst->operand_start], "%r10", "%r10d");
            fprintf(ctx->out, "    testq %%r10, %%r10\n    jne .Lcobra_%zu_b%u\n    jmp .Lcobra_%zu_b%u\n",
                    ctx->function_index, inst->target,
                    ctx->function_index, inst->target2);
            return true;
        case MIR_OP_RETURN: {
            MirReg value = inst->operand_count == 1
                ? ctx->module->arena.operands[inst->operand_start] : MIR_REG_NONE;
            if (!x86_emit_frame_cleanup(ctx,
                                        value != MIR_REG_NONE &&
                                        ctx->module->arena.regs[value].machine_type == MIR_TYPE_VIEW
                                            ? value : MIR_REG_NONE)) return false;
            if (value != MIR_REG_NONE) {
                MirMachineType type = ctx->module->arena.regs[value].machine_type;
                if (type == MIR_TYPE_VIEW) {
                    if (!x86_emit_load_view_component(ctx, value, false, "%rax") ||
                        !x86_emit_load_view_component(ctx, value, true, "%rdx")) return false;
                } else if (x86_is_float(type)) x86_emit_load_float(ctx, value, "%xmm0");
                else if (!x86_emit_load_integer(ctx, value, "%rax", "%eax")) return false;
            }
            x86_emit_epilogue(ctx);
            return true;
        }
        case MIR_OP_BLOCK_ARG:
        case MIR_OP_NONE:
            return false;
    }
    return false;
}

bool bir_x86_64_emit(const MirModule *module, FILE *out,
                     char *errbuf, size_t errbuf_size) {
    if (errbuf && errbuf_size) errbuf[0] = '\0';
    if (!module || !out) {
        x86_error(errbuf, errbuf_size, "x86-64 emitter requires a MIR module and output");
        return false;
    }
    if (!mir_verify(module, errbuf, errbuf_size)) return false;
    fprintf(out, ".text\n");
    for (size_t f = 0; f < module->function_count; f++) {
        X86Context ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.module = module;
        ctx.function = &module->functions[f];
        ctx.function_index = f;
        ctx.out = out;
        ctx.errbuf = errbuf;
        ctx.errbuf_size = errbuf_size;
        MirMachineType return_machine = x86_machine_type_for_cobra(ctx.function->return_type);
        if (!ctx.function->has_hidden_return_storage &&
            return_machine != MIR_TYPE_VIEW &&
            (!x86_is_supported_value(return_machine) &&
             ctx.function->return_type->kind != COBRA_TYPE_VOID)) {
            x86_error(errbuf, errbuf_size,
                      "x86-64 emitter currently supports scalar and scalar-field aggregate returns only");
            return false;
        }
        if (!x86_prepare_frame(&ctx)) {
            free(ctx.reg_offsets);
            free(ctx.view_length_offsets);
            return false;
        }
        for (size_t offset = 0; offset < ctx.function->block_count; offset++) {
            MirBlockRef block_ref = ctx.function->first_block + (MirBlockRef)offset;
            const MirBlock *block = &module->arena.blocks[block_ref];
            for (size_t i = 0; i < block->inst_count; i++) {
                const MirInst *inst = &module->arena.insts[block->insts[i]];
                if (inst->machine_type == MIR_TYPE_AGGREGATE) {
                    x86_error(errbuf, errbuf_size,
                              "x86-64 emitter does not support MIR machine type '%s'",
                              mir_machine_type_name(inst->machine_type));
                    free(ctx.reg_offsets);
                    free(ctx.view_length_offsets);
                    return false;
                }
            }
        }
        fprintf(out, ".globl %s\n.type %s, @function\n%s:\n", ctx.function->name,
                ctx.function->name, ctx.function->name);
        x86_emit_prologue(&ctx);
        if (!x86_initialize_owned_views(&ctx)) {
            x86_error(errbuf, errbuf_size, "could not initialize native owned-slice storage");
            free(ctx.reg_offsets);
            free(ctx.view_length_offsets);
            return false;
        }
        for (size_t offset = 0; offset < ctx.function->block_count; offset++) {
            MirBlockRef block_ref = ctx.function->first_block + (MirBlockRef)offset;
            const MirBlock *block = &module->arena.blocks[block_ref];
            fprintf(out, ".Lcobra_%zu_b%u:\n", f, block_ref);
            for (size_t i = 0; i < block->inst_count; i++) {
                const MirInst *inst = &module->arena.insts[block->insts[i]];
                if (!x86_emit_instruction(&ctx, inst)) {
                    x86_error(errbuf, errbuf_size,
                              "x86-64 emitter does not support MIR opcode '%s' in function '%s'",
                              mir_opcode_name(inst->op), ctx.function->name);
                    free(ctx.reg_offsets);
                    free(ctx.view_length_offsets);
                    return false;
                }
            }
        }
        fprintf(out, ".size %s, .-%s\n", ctx.function->name, ctx.function->name);
        free(ctx.reg_offsets);
        free(ctx.view_length_offsets);
    }
    fprintf(out, ".section .note.GNU-stack,\"\",@progbits\n");
    return true;
}
