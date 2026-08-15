/*
 * Cobra isolated Linux x86-64 assembly emitter.
 *
 * This emitter consumes verified MIR and is intentionally separate from the
 * production direct emitter. It exposes deterministic spill-all and allocated
 * scalar native lowering paths.
 */
#ifndef COBRA_BACKEND_X86_64_H
#define COBRA_BACKEND_X86_64_H

#include "mir.h"
#include "alloc.h"

bool bir_x86_64_emit(const MirModule *module, FILE *out,
                     char *errbuf, size_t errbuf_size);
bool bir_x86_64_emit_allocated(const MirModule *module,
                               const MirAllocation *allocation,
                               FILE *out, char *errbuf, size_t errbuf_size);

#endif /* COBRA_BACKEND_X86_64_H */
