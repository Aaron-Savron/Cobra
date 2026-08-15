/*
 * Production entry point for the isolated backend IR pipeline.
 *
 * This is the only file the production compiler driver (src/main.c) talks
 * to. It runs the full source -> HIR -> SSA -> verifier -> MIR -> allocator
 * -> x86-64 pipeline and writes GNU AT&T assembly, or fails cleanly with a
 * diagnostic when the program uses a construct the isolated lane does not
 * yet support. It never falls back to the production direct emitter.
 */
#ifndef COBRA_BACKEND_IR_DRIVER_H
#define COBRA_BACKEND_IR_DRIVER_H

#include "../../include/cobra.h"

bool bir_backend_compile_program(ASTNode *root, const char *source_path,
                                 const char *output_asm_path,
                                 char *errbuf, size_t errbuf_size);

/* Same pipeline, but encodes machine code directly and writes a real ELF64
   relocatable object instead of assembly text - no system assembler
   involved. Narrower than bir_backend_compile_program: see x86_64_obj.h. */
bool bir_backend_compile_program_object(ASTNode *root, const char *source_path,
                                        const char *output_object_path,
                                        char *errbuf, size_t errbuf_size);

#endif /* COBRA_BACKEND_IR_DRIVER_H */
