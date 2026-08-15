/*
 * Minimal ELF64 relocatable object writer.
 *
 * Writes a .text section (global function symbol table), an optional
 * .rodata section (e.g. dict-literal key strings), R_X86_64_PLT32
 * relocations against external symbols (malloc, free, cobra_dict_ helpers, etc.), and
 * R_X86_64_PC32 relocations against the local .rodata section symbol, all
 * used by the isolated x86-64 backend to produce a real .o directly from
 * encoded machine code without invoking the system assembler. Every other
 * call and branch in the code buffer must already be a fully resolved
 * PC-relative offset inside .text.
 */
#ifndef COBRA_BACKEND_ELF64_H
#define COBRA_BACKEND_ELF64_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;
    uint64_t offset; /* byte offset into the .text section */
    uint64_t size;
} Elf64ObjectSymbol;

/* A PLT32-style relocation against an external (undefined) symbol, named by
   index into the `externs` array passed to elf64_write_object. `text_offset`
   is the byte offset of the 4-byte field to relocate; the bytes there are
   ignored (conventionally left zero) since the linker computes and writes
   the final value. */
typedef struct {
    size_t text_offset;
    size_t extern_index;
    int64_t addend;
} Elf64ObjectReloc;

/* A PC32-style relocation against the local .rodata section symbol (there is
   only ever one, so no index is needed). Used for `leaq label(%rip), reg`
   references into `rodata`, e.g. a dict key string literal. */
typedef struct {
    size_t text_offset;
    int64_t addend;
} Elf64ObjectRodataReloc;

bool elf64_write_object(const char *path, const uint8_t *text, size_t text_size,
                        const uint8_t *rodata, size_t rodata_size,
                        const Elf64ObjectSymbol *symbols, size_t symbol_count,
                        const char *const *externs, size_t extern_count,
                        const Elf64ObjectReloc *relocations, size_t relocation_count,
                        const Elf64ObjectRodataReloc *rodata_relocations, size_t rodata_relocation_count,
                        char *errbuf, size_t errbuf_size);

#endif /* COBRA_BACKEND_ELF64_H */
