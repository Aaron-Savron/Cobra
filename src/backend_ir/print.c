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
    if (type == module->type_void) return "void";
    return cobra_type_kind_name(type ? type->kind : COBRA_TYPE_UNTYPED);
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
            fprintf(out, "i64");
        }
        fprintf(out, ") -> %s\n", type_label(module, info->return_type));
    }

    for (size_t b = 1; b < arena->block_count; b++) { /* slot 0 is the sentinel */
        const SsaBlock *block = &arena->blocks[b];
        fprintf(out, "block b%u", (unsigned)b);
        if (block->name[0]) fprintf(out, " \"%s\"", block->name);
        fprintf(out, " [%s](", block->is_entry ? "entry" : "block");
        for (size_t k = 0; k < block->param_count; k++) {
            if (k) fprintf(out, ", ");
            fprintf(out, "v%u: i64", block->params[k]);
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
                        inst->effect == SSA_EFFECT_WRITE ? "write" : "call");
            }
            if (inst->op == SSA_OP_CONST && inst->result != SSA_VALUE_NONE) {
                fprintf(out, " %lld", (long long)arena->values[inst->result].const_i64);
            }
            if (inst->source_line > 0) fprintf(out, " ; line %d", inst->source_line);
            fprintf(out, "\n");
        }
    }
}
