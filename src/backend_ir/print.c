/*
 * Cobra Backend IR: deterministic textual dump.
 *
 * Blocks print in arena order, values in definition order, operands in order.
 * The output is stable across runs for identical input and is used by tests
 * for golden assertions.
 */
#include "ssa.h"

static const char *type_label(const BackendIrModule *module, const CobraType *type) {
    if (type == module->type_i64) return "i64";
    if (type == module->type_i32) return "i32";
    if (type == module->type_u32) return "u32";
    if (type == module->type_u64) return "u64";
    if (type == module->type_bool) return "bool";
    if (type == module->type_f32) return "f32";
    if (type == module->type_f64) return "f64";
    if (type == module->type_void) return "void";
    if (type && type->kind == COBRA_TYPE_POINTER) {
        return "ptr";
    }
    return cobra_type_kind_name(type ? type->kind : COBRA_TYPE_UNTYPED);
}

static void print_scalar(FILE *out, BirScalarValue value) {
    if (value.kind == BIR_SCALAR_F32) {
        /* Decimal formatting can lose signed zero, infinities, and NaN
           payloads. The raw 32-bit word is the canonical dump form. */
        fprintf(out, "f32bits(0x%08x)", (unsigned)value.payload.f32_bits);
    } else if (value.kind == BIR_SCALAR_F64) {
        fprintf(out, "f64bits(0x%016llx)",
                (unsigned long long)value.payload.f64_bits);
    } else if (value.kind == BIR_SCALAR_U32 || value.kind == BIR_SCALAR_U64) {
        fprintf(out, "%llu", (unsigned long long)value.payload.i64);
    } else if (value.kind == BIR_SCALAR_POINTER) {
        fprintf(out, "ptr(frame=%u,alloc=%u,region=%u,offset=%lld,%s,%s)",
                value.payload.pointer.frame_id,
                value.payload.pointer.allocation_id,
                value.payload.pointer.region_id,
                (long long)value.payload.pointer.offset,
                bir_pointer_origin_name(value.payload.pointer.origin),
                bir_pointer_contract_name(value.payload.pointer.contract));
    } else if (value.kind == BIR_SCALAR_VIEW) {
        fprintf(out, "view(frame=%u,alloc=%u,region=%u,offset=%lld,len=%lld,%s,%s)",
                value.payload.view.pointer.frame_id,
                value.payload.view.pointer.allocation_id,
                value.payload.view.pointer.region_id,
                (long long)value.payload.view.pointer.offset,
                (long long)value.payload.view.length,
                bir_pointer_origin_name(value.payload.view.pointer.origin),
                bir_pointer_contract_name(value.payload.view.pointer.contract));
    } else {
        fprintf(out, "%lld", (long long)value.payload.i64);
    }
}

static void print_abi_location(FILE *out, const BirAbiLocation *location) {
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

static void print_abi(FILE *out, const BirCallAbi *abi) {
    fprintf(out, "  abi cobra stack_align=%u stack_size=%u params=[",
            (unsigned)abi->stack_alignment, (unsigned)abi->stack_size);
    for (size_t p = 0; p < abi->param_count; p++) {
        if (p) fprintf(out, ", ");
        fprintf(out, "{");
        for (size_t part = 0; part < abi->params[p].count; part++) {
            if (part) fprintf(out, "+");
            print_abi_location(out, &abi->params[p].parts[part]);
        }
        fprintf(out, "}");
    }
    fprintf(out, "] return=[");
    for (size_t part = 0; part < abi->returns.count; part++) {
        if (part) fprintf(out, "+");
        print_abi_location(out, &abi->returns.parts[part]);
    }
    fprintf(out, "]\n");
}

void bir_dump(const BackendIrModule *module, FILE *out) {
    if (!module || !out) return;
    const SsaArena *arena = &module->arena;
    fprintf(out, "; backend IR: %s\n", module->source_file);

    for (size_t f = 0; f < module->function_count; f++) {
        const BirFunctionInfo *info = &module->functions[f];
        fprintf(out, "fn \"%s\"(", info->name);
        for (size_t k = 0; k < info->param_count; k++) {
            if (k) fprintf(out, ", ");
            fprintf(out, "%s", type_label(module, info->param_types[k]));
        }
        fprintf(out, ") -> %s\n", type_label(module, info->return_type));
        print_abi(out, &info->call_abi);
    }

    /* Constants are values in this prototype, not standalone instructions.
       Print them explicitly so every value definition, including f32 bit
       payloads, is visible in the deterministic dump. */
    for (size_t v = 1; v < arena->value_count; v++) {
        const SsaValue *value = &arena->values[v];
        if (value->kind != SSA_VALUE_CONST) continue;
        fprintf(out, "value v%u: %s = const ", (unsigned)v,
                type_label(module, value->type));
        print_scalar(out, value->const_value);
        fprintf(out, "\n");
    }

    for (size_t b = 1; b < arena->block_count; b++) { /* slot 0 is the sentinel */
        const SsaBlock *block = &arena->blocks[b];
        fprintf(out, "block b%u", (unsigned)b);
        if (block->name[0]) fprintf(out, " \"%s\"", block->name);
        fprintf(out, " [%s](", block->is_entry ? "entry" : "block");
        for (size_t k = 0; k < block->param_count; k++) {
            if (k) fprintf(out, ", ");
            SsaValueRef param = block->params[k];
            fprintf(out, "v%u: %s", param,
                    type_label(module, arena->values[param].type));
        }
        fprintf(out, ")");
        if (block->source_line > 0) fprintf(out, " ; line %d", block->source_line);
        fprintf(out, "\n");

        for (size_t i = 0; i < block->inst_count; i++) {
            const SsaInst *inst = &arena->insts[block->insts[i]];
            fprintf(out, "  ");
            if (inst->result != SSA_VALUE_NONE) {
                fprintf(out, "v%u: %s = ", inst->result,
                        type_label(module, inst->type));
            }
            fprintf(out, "%s", bir_opcode_name(inst->op));
            if (inst->op == SSA_OP_CALL) {
                fprintf(out, " \"%s\"", inst->callee);
            }
            if (inst->operand_count) {
                fprintf(out, "(");
                for (size_t o = 0; o < inst->operand_count; o++) {
                    if (o) fprintf(out, ", ");
                    fprintf(out, "v%u", arena->operands[inst->operand_start + o]);
                }
                fprintf(out, ")");
            }
            if (inst->op == SSA_OP_JUMP) {
                fprintf(out, " -> b%u", inst->target);
                if (inst->edge_count) {
                    fprintf(out, "(");
                    for (size_t e = 0; e < inst->edge_count; e++) {
                        if (e) fprintf(out, ", ");
                        fprintf(out, "v%u", arena->edges[inst->edge_start + e]);
                    }
                    fprintf(out, ")");
                }
            }
            if (inst->op == SSA_OP_BRANCH) {
                fprintf(out, " ? b%u", inst->target);
                if (inst->edge_count) {
                    fprintf(out, "(");
                    for (size_t e = 0; e < inst->edge_count; e++) {
                        if (e) fprintf(out, ", ");
                        fprintf(out, "v%u", arena->edges[inst->edge_start + e]);
                    }
                    fprintf(out, ")");
                }
                fprintf(out, " : b%u", inst->target2);
                if (inst->edge2_count) {
                    fprintf(out, "(");
                    for (size_t e = 0; e < inst->edge2_count; e++) {
                        if (e) fprintf(out, ", ");
                        fprintf(out, "v%u", arena->edges[inst->edge2_start + e]);
                    }
                    fprintf(out, ")");
                }
            }
            if (inst->effect != SSA_EFFECT_NONE) {
                fprintf(out, " [effect:%s]",
                        inst->effect == SSA_EFFECT_READ ? "read" :
                        inst->effect == SSA_EFFECT_WRITE ? "write" :
                        inst->effect == SSA_EFFECT_READWRITE ? "readwrite" : "call");
            }
            if (inst->op == SSA_OP_CONST && inst->result != SSA_VALUE_NONE) {
                fprintf(out, " ");
                print_scalar(out, arena->values[inst->result].const_value);
            }
            if (inst->op == SSA_OP_SUM_CHECK) {
                fprintf(out, " [expect:%s]",
                        inst->sum_check_kind == 0 ? "some" :
                        inst->sum_check_kind == 1 ? "ok" : "err");
            }
            if (inst->source_line > 0) fprintf(out, " ; line %d", inst->source_line);
            fprintf(out, "\n");
        }
    }
}
