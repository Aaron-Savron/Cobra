/*
 * Cobra MIR linear-scan allocator.
 *
 * This pass is target independent. Register ordinals are abstract positions
 * in the GPR and XMM classes. Calls conservatively invalidate the clobber
 * masks carried by MIR call instructions, and all other values may spill to
 * function-local slots.
 */
#include "alloc.h"
#include <stdarg.h>

static void alloc_error(char *buffer, size_t capacity, const char *fmt, ...) {
    if (!buffer || capacity == 0 || buffer[0]) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, capacity, fmt, args);
    va_end(args);
}

void mir_allocation_init(MirAllocation *allocation, const MirModule *source) {
    if (!allocation) return;
    memset(allocation, 0, sizeof(*allocation));
    allocation->source = source;
}

void mir_allocation_free(MirAllocation *allocation) {
    if (!allocation) return;
    free(allocation->regs);
    memset(allocation, 0, sizeof(*allocation));
}

const char *mir_allocation_kind_name(MirAllocationKind kind) {
    switch (kind) {
        case MIR_ALLOC_NONE: return "none";
        case MIR_ALLOC_REGISTER: return "register";
        case MIR_ALLOC_SPILL: return "spill";
    }
    return "?";
}

static BirAbiRegisterClass alloc_class_for_type(MirMachineType type) {
    if (type == MIR_TYPE_F32 || type == MIR_TYPE_F64) return BIR_ABI_REGISTER_XMM;
    if (type == MIR_TYPE_I8 || type == MIR_TYPE_I32 || type == MIR_TYPE_U32 ||
        type == MIR_TYPE_I64 || type == MIR_TYPE_U64 || type == MIR_TYPE_BOOL ||
        type == MIR_TYPE_ADDRESS) return BIR_ABI_REGISTER_GPR;
    return BIR_ABI_REGISTER_NONE;
}

/* Abstract GPR indices 0..BIR_ABI_MAX_GPR_ARGUMENT_REGISTERS-1 are the Linux
   x86-64 System V caller-saved argument registers. Indices at and above that
   are callee-saved registers (rbx, r12-r15) available only to the allocator,
   never to the ABI argument model. Calls do not clobber them, so intervals
   that cross a call may still land in a physical register instead of always
   spilling. SysV has no callee-saved XMM registers, so the XMM class keeps
   its argument-register width. */
#define ALLOC_GPR_CALLEE_SAVED_REGISTERS 5
#define ALLOC_GPR_TOTAL_REGISTERS \
    (BIR_ABI_MAX_GPR_ARGUMENT_REGISTERS + ALLOC_GPR_CALLEE_SAVED_REGISTERS)

static uint64_t alloc_class_mask(BirAbiRegisterClass register_class) {
    if (register_class == BIR_ABI_REGISTER_GPR)
        return (UINT64_C(1) << ALLOC_GPR_TOTAL_REGISTERS) - 1;
    if (register_class == BIR_ABI_REGISTER_XMM)
        return (UINT64_C(1) << BIR_ABI_MAX_XMM_ARGUMENT_REGISTERS) - 1;
    return 0;
}

static uint32_t alloc_reg_position(const MirModule *module, MirReg reg,
                                   const uint32_t *inst_positions,
                                   const uint32_t *block_positions) {
    const MirRegInfo *info = &module->arena.regs[reg];
    if (info->block_parameter)
        return block_positions[info->def_block];
    if (info->def_inst != MIR_INST_NONE && info->def_inst < module->arena.inst_count)
        return inst_positions[info->def_inst];
    return block_positions[info->def_block];
}

typedef struct {
    MirReg reg;
    uint32_t start;
} AllocSortItem;

static int alloc_sort_item(const void *left, const void *right) {
    const AllocSortItem *a = (const AllocSortItem *)left;
    const AllocSortItem *b = (const AllocSortItem *)right;
    if (a->start < b->start) return -1;
    if (a->start > b->start) return 1;
    return a->reg < b->reg ? -1 : a->reg > b->reg;
}

static bool alloc_function(MirAllocation *allocation, size_t function_index,
                           const uint32_t *inst_positions,
                           const uint32_t *block_positions,
                           char *errbuf, size_t errbuf_size) {
    const MirModule *module = allocation->source;
    const MirFunction *function = &module->functions[function_index];
    size_t reg_count = module->arena.reg_count;
    MirLiveInterval *intervals = calloc(reg_count ? reg_count : 1, sizeof(*intervals));
    AllocSortItem *items = calloc(reg_count ? reg_count : 1, sizeof(*items));
    MirReg *active = calloc(reg_count ? reg_count : 1, sizeof(*active));
    if (!intervals || !items || !active) {
        free(intervals);
        free(items);
        free(active);
        alloc_error(errbuf, errbuf_size, "out of memory building MIR live intervals");
        return false;
    }

    size_t item_count = 0;
    for (MirReg reg = 1; reg < reg_count; reg++) {
        const MirRegInfo *info = &module->arena.regs[reg];
        if (info->function_index != function_index) continue;
        MirLiveInterval *interval = &intervals[reg];
        interval->live = true;
        interval->register_class = alloc_class_for_type(info->machine_type);
        interval->machine_type = info->machine_type;
        interval->start = alloc_reg_position(module, reg, inst_positions, block_positions);
        interval->end = interval->start;
        items[item_count].reg = reg;
        items[item_count].start = interval->start;
        item_count++;
    }

    for (size_t offset = 0; offset < function->block_count; offset++) {
        MirBlockRef block_ref = function->first_block + (MirBlockRef)offset;
        const MirBlock *block = &module->arena.blocks[block_ref];
        for (size_t i = 0; i < block->inst_count; i++) {
            MirInstRef inst_ref = block->insts[i];
            const MirInst *inst = &module->arena.insts[inst_ref];
            uint32_t position = inst_positions[inst_ref];
            for (size_t o = 0; o < inst->operand_count; o++) {
                MirReg reg = module->arena.operands[inst->operand_start + o];
                if (reg < reg_count && intervals[reg].live && intervals[reg].end < position)
                    intervals[reg].end = position;
            }
            if (inst->op == MIR_OP_JUMP || inst->op == MIR_OP_BRANCH) {
                for (size_t e = 0; e < inst->edge_count; e++) {
                    MirReg reg = module->arena.edges[inst->edge_start + e];
                    if (reg < reg_count && intervals[reg].live && intervals[reg].end < position)
                        intervals[reg].end = position;
                }
                for (size_t e = 0; e < inst->edge2_count; e++) {
                    MirReg reg = module->arena.edges[inst->edge2_start + e];
                    if (reg < reg_count && intervals[reg].live && intervals[reg].end < position)
                        intervals[reg].end = position;
                }
            }
        }
    }

    size_t cells = module->arena.block_count * reg_count;
    uint8_t *block_use = calloc(cells ? cells : 1, sizeof(*block_use));
    uint8_t *block_def = calloc(cells ? cells : 1, sizeof(*block_def));
    uint8_t *live_in = calloc(cells ? cells : 1, sizeof(*live_in));
    uint8_t *live_out = calloc(cells ? cells : 1, sizeof(*live_out));
    if (!block_use || !block_def || !live_in || !live_out) {
        free(block_use);
        free(block_def);
        free(live_in);
        free(live_out);
        free(intervals);
        free(items);
        free(active);
        alloc_error(errbuf, errbuf_size, "out of memory building MIR CFG liveness");
        return false;
    }
    uint32_t function_last_position = 0;
    for (size_t offset = 0; offset < function->block_count; offset++) {
        MirBlockRef block_ref = function->first_block + (MirBlockRef)offset;
        const MirBlock *block = &module->arena.blocks[block_ref];
        for (size_t i = 0; i < block->param_count; i++)
            block_def[(size_t)block_ref * reg_count + block->params[i]] = 1;
        for (size_t i = 0; i < block->inst_count; i++) {
            MirInstRef inst_ref = block->insts[i];
            const MirInst *inst = &module->arena.insts[inst_ref];
            function_last_position = inst_positions[inst_ref];
            for (size_t o = 0; o < inst->operand_count; o++)
                block_use[(size_t)block_ref * reg_count +
                          module->arena.operands[inst->operand_start + o]] = 1;
            if (inst->op == MIR_OP_JUMP || inst->op == MIR_OP_BRANCH) {
                for (size_t e = 0; e < inst->edge_count; e++)
                    block_use[(size_t)block_ref * reg_count +
                              module->arena.edges[inst->edge_start + e]] = 1;
                for (size_t e = 0; e < inst->edge2_count; e++)
                    block_use[(size_t)block_ref * reg_count +
                              module->arena.edges[inst->edge2_start + e]] = 1;
            }
            if (inst->result != MIR_REG_NONE)
                block_def[(size_t)block_ref * reg_count + inst->result] = 1;
        }
    }
    bool liveness_changed = true;
    while (liveness_changed) {
        liveness_changed = false;
        for (size_t offset = function->block_count; offset-- > 0;) {
            MirBlockRef block_ref = function->first_block + (MirBlockRef)offset;
            const MirBlock *block = &module->arena.blocks[block_ref];
            for (size_t reg = 1; reg < reg_count; reg++) {
                uint8_t next_out = 0;
                for (size_t s = 0; s < block->succ_count; s++) {
                    MirBlockRef successor = block->succs[s];
                    next_out |= live_in[(size_t)successor * reg_count + reg];
                }
                uint8_t next_in = block_use[(size_t)block_ref * reg_count + reg] ||
                    (next_out && !block_def[(size_t)block_ref * reg_count + reg]);
                if (live_out[(size_t)block_ref * reg_count + reg] != next_out ||
                    live_in[(size_t)block_ref * reg_count + reg] != next_in) {
                    live_out[(size_t)block_ref * reg_count + reg] = next_out;
                    live_in[(size_t)block_ref * reg_count + reg] = next_in;
                    liveness_changed = true;
                }
            }
        }
    }
    for (size_t offset = 0; offset < function->block_count; offset++) {
        MirBlockRef block_ref = function->first_block + (MirBlockRef)offset;
        for (MirReg reg = 1; reg < reg_count; reg++) {
            if (live_out[(size_t)block_ref * reg_count + reg] &&
                intervals[reg].live && intervals[reg].end < function_last_position)
                intervals[reg].end = function_last_position;
        }
    }
    free(block_use);
    free(block_def);
    free(live_in);
    free(live_out);

    for (size_t i = 0; i < item_count; i++) {
        MirReg reg = items[i].reg;
        MirLiveInterval *interval = &intervals[reg];
        uint64_t forbidden = 0;
        for (size_t offset = 0; offset < function->block_count; offset++) {
            MirBlockRef block_ref = function->first_block + (MirBlockRef)offset;
            const MirBlock *block = &module->arena.blocks[block_ref];
            for (size_t j = 0; j < block->inst_count; j++) {
                MirInstRef inst_ref = block->insts[j];
                const MirInst *inst = &module->arena.insts[inst_ref];
                uint32_t position = inst_positions[inst_ref];
                bool view_register_constraint =
                    (inst->op == MIR_OP_VIEW_MAKE ||
                     ((inst->op == MIR_OP_PTR_ADD || inst->op == MIR_OP_LOAD ||
                       inst->op == MIR_OP_STORE) &&
                      inst->view_source != MIR_REG_NONE));
                /* View bounds checks clobber abstract rdx/rcx. The check for
                   a STORE runs before the stored value is read, so include
                   endpoint uses here; otherwise a value such as s[0] + 1
                   could be allocated to a clobbered register. */
                if (view_register_constraint && interval->start <= position &&
                    interval->end >= position)
                    forbidden |= (UINT64_C(1) << 2) | (UINT64_C(1) << 3);
                if (!(interval->start < position && interval->end > position)) continue;
                if (inst->op == MIR_OP_CALL) {
                    forbidden |= interval->register_class == BIR_ABI_REGISTER_XMM
                        ? inst->clobbers.xmm_mask : inst->clobbers.gpr_mask;
                }
                /* Every opcode that emits malloc/memcpy/free internally
                   clobbers all caller-saved registers. Intervals that
                   overlap such an opcode must live in spill storage. */
                if (inst->op == MIR_OP_SLICE_ALLOC || inst->op == MIR_OP_SLICE_FREE ||
                    inst->op == MIR_OP_BUFFER_ALLOC || inst->op == MIR_OP_BUFFER_APPEND ||
                    inst->op == MIR_OP_BUFFER_FREE || inst->op == MIR_OP_STRING_CONCAT ||
                    inst->op == MIR_OP_STRING_EQ ||
                    inst->op == MIR_OP_DICT_SET || inst->op == MIR_OP_DICT_GET ||
                    inst->op == MIR_OP_DICT_HAS || inst->op == MIR_OP_DICT_DELETE ||
                    inst->op == MIR_OP_DICT_POP || inst->op == MIR_OP_DICT_LEN ||
                    inst->op == MIR_OP_DICT_FREE ||
                    inst->op == MIR_OP_SUM_DROP || inst->op == MIR_OP_AGG_DROP ||
                    inst->op == MIR_OP_DESTROY || inst->op == MIR_OP_REGION_EXIT) {
                    forbidden |= alloc_class_mask(interval->register_class);
                }
                if ((inst->op == MIR_OP_DIV || inst->op == MIR_OP_REM) &&
                    interval->register_class == BIR_ABI_REGISTER_GPR)
                    forbidden |= UINT64_C(1) << 2; /* abstract rdx is fixed scratch */
            }
        }
        interval->forbidden_register_mask = forbidden;
    }

    qsort(items, item_count, sizeof(*items), alloc_sort_item);
    MirFunctionAllocation *function_allocation = &allocation->functions[function_index];
    snprintf(function_allocation->name, sizeof(function_allocation->name), "%s", function->name);
    function_allocation->reg_count = item_count;
    size_t active_count = 0;
    uint32_t next_spill = 0;

    for (size_t item = 0; item < item_count; item++) {
        MirReg reg = items[item].reg;
        MirLiveInterval *interval = &intervals[reg];
        MirRegAllocation *result = &allocation->regs[reg];
        result->interval = *interval;
        result->register_class = interval->register_class;
        for (size_t i = 0; i < active_count;) {
            MirReg active_reg = active[i];
            if (allocation->regs[active_reg].interval.end < interval->start) {
                active[i] = active[--active_count];
                continue;
            }
            i++;
        }

        uint64_t available = alloc_class_mask(interval->register_class) &
                             ~interval->forbidden_register_mask;
        for (size_t i = 0; i < active_count; i++) {
            const MirRegAllocation *active_result = &allocation->regs[active[i]];
            if (active_result->register_class == interval->register_class &&
                active_result->kind == MIR_ALLOC_REGISTER)
                available &= ~(UINT64_C(1) << active_result->register_index);
        }
        if (interval->machine_type == MIR_TYPE_VIEW) {
            /* Views need two paired spill components (pointer and length)
               that the emitter always reads and writes together, so they
               stay spill-only regardless of register availability. */
            result->kind = MIR_ALLOC_SPILL;
            result->register_class = BIR_ABI_REGISTER_NONE;
            result->spill_slot = next_spill++;
            result->spill_slot2 = next_spill++;
            continue;
        }
        if (interval->register_class == BIR_ABI_REGISTER_NONE) available = 0;
        if (available) {
            uint16_t index = 0;
            while (((available >> index) & 1U) == 0) index++;
            result->kind = MIR_ALLOC_REGISTER;
            result->register_index = index;
            result->register_class = interval->register_class;
            if (interval->register_class == BIR_ABI_REGISTER_GPR)
                function_allocation->used_gpr_mask |= UINT64_C(1) << index;
            else function_allocation->used_xmm_mask |= UINT64_C(1) << index;
            active[active_count++] = reg;
            continue;
        }

        size_t victim_index = active_count;
        uint32_t victim_end = 0;
        for (size_t i = 0; i < active_count; i++) {
            MirReg active_reg = active[i];
            MirRegAllocation *active_result = &allocation->regs[active_reg];
            if (active_result->register_class != interval->register_class ||
                active_result->kind != MIR_ALLOC_REGISTER) continue;
            if (active_result->interval.end > victim_end) {
                victim_end = active_result->interval.end;
                victim_index = i;
            }
        }
        if (victim_index < active_count && victim_end > interval->end) {
            MirReg victim = active[victim_index];
            MirRegAllocation *victim_result = &allocation->regs[victim];
            result->kind = MIR_ALLOC_REGISTER;
            result->register_class = interval->register_class;
            result->register_index = victim_result->register_index;
            victim_result->kind = MIR_ALLOC_SPILL;
            victim_result->spill_slot = next_spill++;
            active[victim_index] = reg;
        } else {
            result->kind = MIR_ALLOC_SPILL;
            result->spill_slot = next_spill++;
        }
    }
    function_allocation->spill_count = next_spill;
    free(intervals);
    free(items);
    free(active);
    return true;
}

bool mir_allocate(const MirModule *module, MirAllocation *allocation,
                  char *errbuf, size_t errbuf_size) {
    if (errbuf && errbuf_size) errbuf[0] = '\0';
    if (!module || !allocation) {
        alloc_error(errbuf, errbuf_size, "MIR allocation requires a module and destination");
        return false;
    }
    if (!mir_verify(module, errbuf, errbuf_size)) return false;
    mir_allocation_init(allocation, module);
    allocation->reg_count = module->arena.reg_count;
    allocation->function_count = module->function_count;
    allocation->regs = calloc(allocation->reg_count ? allocation->reg_count : 1,
                               sizeof(*allocation->regs));
    if (!allocation->regs) {
        alloc_error(errbuf, errbuf_size, "out of memory allocating MIR allocation table");
        mir_allocation_free(allocation);
        return false;
    }

    uint32_t *inst_positions = calloc(module->arena.inst_count ? module->arena.inst_count : 1,
                                      sizeof(*inst_positions));
    uint32_t *block_positions = calloc(module->arena.block_count ? module->arena.block_count : 1,
                                       sizeof(*block_positions));
    if (!inst_positions || !block_positions) {
        free(inst_positions);
        free(block_positions);
        alloc_error(errbuf, errbuf_size, "out of memory assigning MIR positions");
        mir_allocation_free(allocation);
        return false;
    }
    for (size_t f = 0; f < module->function_count; f++) {
        const MirFunction *function = &module->functions[f];
        uint32_t position = 1;
        for (size_t offset = 0; offset < function->block_count; offset++) {
            MirBlockRef block_ref = function->first_block + (MirBlockRef)offset;
            block_positions[block_ref] = position;
            const MirBlock *block = &module->arena.blocks[block_ref];
            for (size_t i = 0; i < block->inst_count; i++)
                inst_positions[block->insts[i]] = position++;
        }
    }
    for (size_t f = 0; f < module->function_count; f++) {
        if (!alloc_function(allocation, f, inst_positions, block_positions,
                            errbuf, errbuf_size)) {
            free(inst_positions);
            free(block_positions);
            mir_allocation_free(allocation);
            return false;
        }
    }
    free(inst_positions);
    free(block_positions);
    return mir_allocation_verify(allocation, errbuf, errbuf_size);
}

bool mir_allocation_verify(const MirAllocation *allocation,
                           char *errbuf, size_t errbuf_size) {
    if (errbuf && errbuf_size) errbuf[0] = '\0';
    if (!allocation || !allocation->source ||
        allocation->function_count != allocation->source->function_count ||
        allocation->reg_count != allocation->source->arena.reg_count) {
        alloc_error(errbuf, errbuf_size, "invalid MIR allocation table");
        return false;
    }
    const MirModule *module = allocation->source;
    for (MirReg reg = 1; reg < allocation->reg_count; reg++) {
        const MirRegInfo *info = &module->arena.regs[reg];
        const MirRegAllocation *result = &allocation->regs[reg];
        if (info->function_index >= allocation->function_count ||
            !result->interval.live || result->interval.start > result->interval.end ||
            result->kind == MIR_ALLOC_NONE ||
            result->register_class != alloc_class_for_type(info->machine_type)) {
            alloc_error(errbuf, errbuf_size, "MIR virtual register %u has invalid allocation metadata", reg);
            return false;
        }
        if (result->kind == MIR_ALLOC_REGISTER) {
            uint64_t mask = alloc_class_mask(result->register_class);
            if (result->register_index >= 64 ||
                !(mask & (UINT64_C(1) << result->register_index)) ||
                (result->interval.forbidden_register_mask &
                 (UINT64_C(1) << result->register_index))) {
                alloc_error(errbuf, errbuf_size, "MIR virtual register %u violates a register constraint", reg);
                return false;
            }
        }
    }
    for (size_t left = 1; left < allocation->reg_count; left++) {
        const MirRegInfo *left_info = &module->arena.regs[left];
        const MirRegAllocation *left_result = &allocation->regs[left];
        if (left_result->kind != MIR_ALLOC_REGISTER) continue;
        for (size_t right = left + 1; right < allocation->reg_count; right++) {
            const MirRegInfo *right_info = &module->arena.regs[right];
            const MirRegAllocation *right_result = &allocation->regs[right];
            if (right_result->kind != MIR_ALLOC_REGISTER ||
                left_info->function_index != right_info->function_index ||
                left_result->register_class != right_result->register_class ||
                left_result->register_index != right_result->register_index) continue;
            if (left_result->interval.start <= right_result->interval.end &&
                right_result->interval.start <= left_result->interval.end) {
                alloc_error(errbuf, errbuf_size,
                            "MIR virtual registers %zu and %zu overlap in one register", left, right);
                return false;
            }
        }
    }
    return true;
}

void mir_allocation_dump(const MirAllocation *allocation, FILE *out) {
    if (!allocation || !out) return;
    fprintf(out, "; MIR allocation\n");
    for (size_t f = 0; f < allocation->function_count; f++) {
        const MirFunctionAllocation *function = &allocation->functions[f];
        fprintf(out, "fn \"%s\" spills=%u gpr=0x%llx xmm=0x%llx\n",
                function->name, (unsigned)function->spill_count,
                (unsigned long long)function->used_gpr_mask,
                (unsigned long long)function->used_xmm_mask);
        for (MirReg reg = 1; reg < allocation->reg_count; reg++) {
            const MirRegInfo *info = &allocation->source->arena.regs[reg];
            if (info->function_index != f) continue;
            const MirRegAllocation *result = &allocation->regs[reg];
            fprintf(out, "  r%u [%u,%u] ", reg,
                    (unsigned)result->interval.start,
                    (unsigned)result->interval.end);
            if (result->kind == MIR_ALLOC_REGISTER)
                fprintf(out, "%s%u\n",
                        result->register_class == BIR_ABI_REGISTER_XMM ? "xmm" : "gpr",
                        (unsigned)result->register_index);
            else if (info->machine_type == MIR_TYPE_VIEW)
                fprintf(out, "spill%u,spill%u\n",
                        (unsigned)result->spill_slot,
                        (unsigned)result->spill_slot2);
            else fprintf(out, "spill%u\n", (unsigned)result->spill_slot);
        }
    }
}
