#include "elf64.h"
#include <elf.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void elf_error(char *buffer, size_t capacity, const char *fmt, ...) {
    if (!buffer || capacity == 0 || buffer[0]) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, capacity, fmt, args);
    va_end(args);
}

static uint64_t elf_align_up(uint64_t value, uint64_t alignment) {
    uint64_t remainder = value % alignment;
    return remainder ? value + (alignment - remainder) : value;
}

/* Section indices in the fixed layout this writer always produces. .rodata
   and .rela.text are always present (possibly empty) to keep the layout
   static regardless of whether a given program uses them. */
enum { SEC_NULL = 0, SEC_TEXT, SEC_RODATA, SEC_RELA_TEXT, SEC_SYMTAB, SEC_STRTAB, SEC_SHSTRTAB, SEC_COUNT };

/* Local symbol table indices. Index 0 is the mandatory null entry; index 1
   is the local .rodata section symbol every Elf64ObjectRodataReloc targets.
   Global symbols (defined functions, then externs) start at index 2. */
enum { SYM_NULL = 0, SYM_RODATA_SECTION, SYM_FIRST_GLOBAL };

bool elf64_write_object(const char *path, const uint8_t *text, size_t text_size,
                        const uint8_t *rodata, size_t rodata_size,
                        const Elf64ObjectSymbol *symbols, size_t symbol_count,
                        const char *const *externs, size_t extern_count,
                        const Elf64ObjectReloc *relocations, size_t relocation_count,
                        const Elf64ObjectRodataReloc *rodata_relocations, size_t rodata_relocation_count,
                        char *errbuf, size_t errbuf_size) {
    if (errbuf && errbuf_size) errbuf[0] = '\0';
    if (!path || (!text && text_size) || (!rodata && rodata_size)) {
        elf_error(errbuf, errbuf_size, "elf64 writer requires a path and code buffer");
        return false;
    }
    for (size_t i = 0; i < relocation_count; i++) {
        if (relocations[i].extern_index >= extern_count) {
            elf_error(errbuf, errbuf_size, "relocation references an unknown external symbol");
            return false;
        }
    }

    /* .strtab: leading NUL, then one NUL-terminated name per defined symbol,
       then one per external symbol. The local .rodata section symbol has no
       name (STT_SECTION conventionally uses an empty/zero name). */
    size_t strtab_size = 1;
    for (size_t i = 0; i < symbol_count; i++) strtab_size += strlen(symbols[i].name) + 1;
    for (size_t i = 0; i < extern_count; i++) strtab_size += strlen(externs[i]) + 1;
    uint8_t *strtab = calloc(strtab_size ? strtab_size : 1, 1);
    size_t *name_offsets = calloc(symbol_count ? symbol_count : 1, sizeof(*name_offsets));
    size_t *extern_name_offsets = calloc(extern_count ? extern_count : 1, sizeof(*extern_name_offsets));
    if (!strtab || !name_offsets || !extern_name_offsets) {
        free(strtab); free(name_offsets); free(extern_name_offsets);
        elf_error(errbuf, errbuf_size, "out of memory building .strtab");
        return false;
    }
    size_t cursor = 1;
    for (size_t i = 0; i < symbol_count; i++) {
        size_t len = strlen(symbols[i].name);
        memcpy(strtab + cursor, symbols[i].name, len);
        name_offsets[i] = cursor;
        cursor += len + 1;
    }
    for (size_t i = 0; i < extern_count; i++) {
        size_t len = strlen(externs[i]);
        memcpy(strtab + cursor, externs[i], len);
        extern_name_offsets[i] = cursor;
        cursor += len + 1;
    }

    /* .symtab: null entry, the local .rodata section symbol, one
       STT_FUNC/STB_GLOBAL entry per defined symbol, then one
       STT_NOTYPE/STB_GLOBAL/SHN_UNDEF entry per external symbol. Local
       symbols must precede global ones, so sh_info (first global index) is
       SYM_FIRST_GLOBAL. Extern symbol indices (used by relocations) are
       SYM_FIRST_GLOBAL+symbol_count.. */
    size_t symtab_count = SYM_FIRST_GLOBAL + symbol_count + extern_count;
    Elf64_Sym *symtab = calloc(symtab_count, sizeof(*symtab));
    if (!symtab) {
        free(strtab); free(name_offsets); free(extern_name_offsets);
        elf_error(errbuf, errbuf_size, "out of memory building .symtab");
        return false;
    }
    symtab[SYM_RODATA_SECTION].st_info = ELF64_ST_INFO(STB_LOCAL, STT_SECTION);
    symtab[SYM_RODATA_SECTION].st_other = STV_DEFAULT;
    symtab[SYM_RODATA_SECTION].st_shndx = SEC_RODATA;
    for (size_t i = 0; i < symbol_count; i++) {
        Elf64_Sym *sym = &symtab[SYM_FIRST_GLOBAL + i];
        sym->st_name = (Elf64_Word)name_offsets[i];
        sym->st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
        sym->st_other = STV_DEFAULT;
        sym->st_shndx = SEC_TEXT;
        sym->st_value = symbols[i].offset;
        sym->st_size = symbols[i].size;
    }
    for (size_t i = 0; i < extern_count; i++) {
        Elf64_Sym *sym = &symtab[SYM_FIRST_GLOBAL + symbol_count + i];
        sym->st_name = (Elf64_Word)extern_name_offsets[i];
        sym->st_info = ELF64_ST_INFO(STB_GLOBAL, STT_NOTYPE);
        sym->st_other = STV_DEFAULT;
        sym->st_shndx = SHN_UNDEF;
    }
    free(name_offsets);
    free(extern_name_offsets);

    size_t total_relocs = relocation_count + rodata_relocation_count;
    Elf64_Rela *relas = calloc(total_relocs ? total_relocs : 1, sizeof(*relas));
    if (!relas) {
        free(strtab); free(symtab);
        elf_error(errbuf, errbuf_size, "out of memory building .rela.text");
        return false;
    }
    for (size_t i = 0; i < relocation_count; i++) {
        relas[i].r_offset = relocations[i].text_offset;
        relas[i].r_info = ELF64_R_INFO(SYM_FIRST_GLOBAL + symbol_count + relocations[i].extern_index, R_X86_64_PLT32);
        relas[i].r_addend = relocations[i].addend;
    }
    for (size_t i = 0; i < rodata_relocation_count; i++) {
        Elf64_Rela *rela = &relas[relocation_count + i];
        rela->r_offset = rodata_relocations[i].text_offset;
        rela->r_info = ELF64_R_INFO(SYM_RODATA_SECTION, R_X86_64_PC32);
        rela->r_addend = rodata_relocations[i].addend;
    }

    static const char shstrtab_bytes[] = "\0.text\0.rodata\0.rela.text\0.symtab\0.strtab\0.shstrtab";
    const uint32_t name_text = 1, name_rodata = 7, name_rela = 15, name_symtab = 26, name_strtab = 34, name_shstrtab = 42;

    uint64_t off = sizeof(Elf64_Ehdr);
    uint64_t text_off = off;
    off += text_size;
    off = elf_align_up(off, 8);
    uint64_t rodata_off = off;
    off += rodata_size;
    off = elf_align_up(off, 8);
    uint64_t rela_off = off;
    off += total_relocs * sizeof(Elf64_Rela);
    uint64_t symtab_off = off;
    off += symtab_count * sizeof(Elf64_Sym);
    uint64_t strtab_off = off;
    off += strtab_size;
    uint64_t shstrtab_off = off;
    off += sizeof(shstrtab_bytes);
    off = elf_align_up(off, 8);
    uint64_t shdr_off = off;

    Elf64_Ehdr ehdr;
    memset(&ehdr, 0, sizeof(ehdr));
    ehdr.e_ident[EI_MAG0] = ELFMAG0;
    ehdr.e_ident[EI_MAG1] = ELFMAG1;
    ehdr.e_ident[EI_MAG2] = ELFMAG2;
    ehdr.e_ident[EI_MAG3] = ELFMAG3;
    ehdr.e_ident[EI_CLASS] = ELFCLASS64;
    ehdr.e_ident[EI_DATA] = ELFDATA2LSB;
    ehdr.e_ident[EI_VERSION] = EV_CURRENT;
    ehdr.e_ident[EI_OSABI] = ELFOSABI_NONE;
    ehdr.e_type = ET_REL;
    ehdr.e_machine = EM_X86_64;
    ehdr.e_version = EV_CURRENT;
    ehdr.e_shoff = shdr_off;
    ehdr.e_ehsize = sizeof(Elf64_Ehdr);
    ehdr.e_shentsize = sizeof(Elf64_Shdr);
    ehdr.e_shnum = SEC_COUNT;
    ehdr.e_shstrndx = SEC_SHSTRTAB;

    Elf64_Shdr shdrs[SEC_COUNT];
    memset(shdrs, 0, sizeof(shdrs));

    shdrs[SEC_TEXT].sh_name = name_text;
    shdrs[SEC_TEXT].sh_type = SHT_PROGBITS;
    shdrs[SEC_TEXT].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
    shdrs[SEC_TEXT].sh_offset = text_off;
    shdrs[SEC_TEXT].sh_size = text_size;
    shdrs[SEC_TEXT].sh_addralign = 16;

    shdrs[SEC_RODATA].sh_name = name_rodata;
    shdrs[SEC_RODATA].sh_type = SHT_PROGBITS;
    shdrs[SEC_RODATA].sh_flags = SHF_ALLOC;
    shdrs[SEC_RODATA].sh_offset = rodata_off;
    shdrs[SEC_RODATA].sh_size = rodata_size;
    shdrs[SEC_RODATA].sh_addralign = 1;

    shdrs[SEC_RELA_TEXT].sh_name = name_rela;
    shdrs[SEC_RELA_TEXT].sh_type = SHT_RELA;
    shdrs[SEC_RELA_TEXT].sh_offset = rela_off;
    shdrs[SEC_RELA_TEXT].sh_size = total_relocs * sizeof(Elf64_Rela);
    shdrs[SEC_RELA_TEXT].sh_link = SEC_SYMTAB;
    shdrs[SEC_RELA_TEXT].sh_info = SEC_TEXT;
    shdrs[SEC_RELA_TEXT].sh_addralign = 8;
    shdrs[SEC_RELA_TEXT].sh_entsize = sizeof(Elf64_Rela);

    shdrs[SEC_SYMTAB].sh_name = name_symtab;
    shdrs[SEC_SYMTAB].sh_type = SHT_SYMTAB;
    shdrs[SEC_SYMTAB].sh_offset = symtab_off;
    shdrs[SEC_SYMTAB].sh_size = symtab_count * sizeof(Elf64_Sym);
    shdrs[SEC_SYMTAB].sh_link = SEC_STRTAB;
    shdrs[SEC_SYMTAB].sh_info = SYM_FIRST_GLOBAL;
    shdrs[SEC_SYMTAB].sh_addralign = 8;
    shdrs[SEC_SYMTAB].sh_entsize = sizeof(Elf64_Sym);

    shdrs[SEC_STRTAB].sh_name = name_strtab;
    shdrs[SEC_STRTAB].sh_type = SHT_STRTAB;
    shdrs[SEC_STRTAB].sh_offset = strtab_off;
    shdrs[SEC_STRTAB].sh_size = strtab_size;
    shdrs[SEC_STRTAB].sh_addralign = 1;

    shdrs[SEC_SHSTRTAB].sh_name = name_shstrtab;
    shdrs[SEC_SHSTRTAB].sh_type = SHT_STRTAB;
    shdrs[SEC_SHSTRTAB].sh_offset = shstrtab_off;
    shdrs[SEC_SHSTRTAB].sh_size = sizeof(shstrtab_bytes);
    shdrs[SEC_SHSTRTAB].sh_addralign = 1;

    FILE *out = fopen(path, "wb");
    if (!out) {
        elf_error(errbuf, errbuf_size, "could not open '%s' for writing", path);
        free(strtab); free(symtab); free(relas);
        return false;
    }
    bool ok = true;
    ok &= fwrite(&ehdr, sizeof(ehdr), 1, out) == 1;
    ok &= text_size == 0 || fwrite(text, text_size, 1, out) == 1;
    for (uint64_t p = sizeof(ehdr) + text_size; ok && p < rodata_off; p++) ok &= fputc(0, out) != EOF;
    ok &= rodata_size == 0 || fwrite(rodata, rodata_size, 1, out) == 1;
    for (uint64_t p = rodata_off + rodata_size; ok && p < rela_off; p++) ok &= fputc(0, out) != EOF;
    ok &= total_relocs == 0 || fwrite(relas, sizeof(Elf64_Rela), total_relocs, out) == total_relocs;
    ok &= fwrite(symtab, sizeof(Elf64_Sym), symtab_count, out) == symtab_count;
    ok &= fwrite(strtab, strtab_size, 1, out) == 1;
    ok &= fwrite(shstrtab_bytes, sizeof(shstrtab_bytes), 1, out) == 1;
    for (uint64_t p = shstrtab_off + sizeof(shstrtab_bytes); ok && p < shdr_off; p++) ok &= fputc(0, out) != EOF;
    ok &= fwrite(shdrs, sizeof(shdrs), 1, out) == 1;
    if (fclose(out) != 0) ok = false;

    free(strtab);
    free(symtab);
    free(relas);
    if (!ok) elf_error(errbuf, errbuf_size, "failed writing object bytes to '%s'", path);
    return ok;
}
