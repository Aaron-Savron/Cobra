/*
 * Direct-to-machine-code x86-64 object emitter.
 *
 * Encodes verified, allocated MIR straight to bytes and writes a real
 * ELF64 relocatable object (via elf64.c) without invoking the system
 * assembler. This is a v1 lane: 64-bit integer/boolean/address scalars
 * only, register or six-GPR-argument calls only, no floats, views, or
 * aggregates. Anything outside that subset is rejected with a diagnostic
 * instead of being silently miscompiled.
 */
#ifndef COBRA_BACKEND_X86_64_OBJ_H
#define COBRA_BACKEND_X86_64_OBJ_H

#include "mir.h"
#include "alloc.h"

bool bir_x86_64_emit_object(const MirModule *module, const MirAllocation *allocation,
                            const char *output_object_path,
                            char *errbuf, size_t errbuf_size);

#endif /* COBRA_BACKEND_X86_64_OBJ_H */
