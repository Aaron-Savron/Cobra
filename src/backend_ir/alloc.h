/*
 * Cobra MIR register allocation.
 *
 * The allocator consumes verified MIR and assigns each virtual register to an
 * abstract GPR/XMM position or a function-local spill slot. Physical register
 * names and target frame layout remain outside this pass.
 */
#ifndef COBRA_BACKEND_ALLOC_H
#define COBRA_BACKEND_ALLOC_H

#include "mir.h"

typedef enum {
    MIR_ALLOC_NONE = 0,
    MIR_ALLOC_REGISTER,
    MIR_ALLOC_SPILL
} MirAllocationKind;

typedef struct {
    bool live;
    uint32_t start;
    uint32_t end;
    uint64_t forbidden_register_mask;
    BirAbiRegisterClass register_class;
    MirMachineType machine_type;
} MirLiveInterval;

typedef struct {
    MirAllocationKind kind;
    BirAbiRegisterClass register_class;
    uint16_t register_index;
    uint32_t spill_slot;
    uint32_t spill_slot2; /* second component for a spilled view */
    MirLiveInterval interval;
} MirRegAllocation;

typedef struct {
    char name[BIR_MAX_CALLEE_NAME];
    size_t reg_count;
    uint32_t spill_count;
    uint64_t used_gpr_mask;
    uint64_t used_xmm_mask;
} MirFunctionAllocation;

typedef struct {
    const MirModule *source;
    MirRegAllocation *regs;
    size_t reg_count;
    MirFunctionAllocation functions[MIR_MAX_FUNCTIONS];
    size_t function_count;
    char error[COBRA_MAX_TOKEN_TEXT];
} MirAllocation;

void mir_allocation_init(MirAllocation *allocation, const MirModule *source);
void mir_allocation_free(MirAllocation *allocation);
bool mir_allocate(const MirModule *module, MirAllocation *allocation,
                  char *errbuf, size_t errbuf_size);
bool mir_allocation_verify(const MirAllocation *allocation,
                           char *errbuf, size_t errbuf_size);
void mir_allocation_dump(const MirAllocation *allocation, FILE *out);
const char *mir_allocation_kind_name(MirAllocationKind kind);

#endif /* COBRA_BACKEND_ALLOC_H */
