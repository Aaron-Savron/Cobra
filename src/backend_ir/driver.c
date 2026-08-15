#include "driver.h"
#include "mir.h"
#include "alloc.h"
#include "x86_64.h"
#include "x86_64_obj.h"

bool bir_backend_compile_program(ASTNode *root, const char *source_path,
                                 const char *output_asm_path,
                                 char *errbuf, size_t errbuf_size) {
    if (errbuf && errbuf_size) errbuf[0] = '\0';
    if (!root || !output_asm_path) {
        if (errbuf && errbuf_size) snprintf(errbuf, errbuf_size, "isolated backend requires a program and output path");
        return false;
    }

    BackendIrModule source;
    bir_module_init(&source, source_path ? source_path : output_asm_path);
    if (!bir_build_program(&source, root)) {
        if (errbuf && errbuf_size)
            snprintf(errbuf, errbuf_size, "isolated backend: %s", source.error[0] ? source.error : "program uses an unsupported construct");
        bir_module_free(&source);
        return false;
    }

    MirModule mir;
    mir_module_init(&mir, &source);
    if (!mir_lower_module(&source, &mir, errbuf, errbuf_size)) {
        mir_module_free(&mir);
        bir_module_free(&source);
        return false;
    }

    MirAllocation allocation;
    mir_allocation_init(&allocation, &mir);
    bool allocated_ok = mir_allocate(&mir, &allocation, errbuf, errbuf_size);

    FILE *out = fopen(output_asm_path, "w");
    if (!out) {
        if (errbuf && errbuf_size) snprintf(errbuf, errbuf_size, "isolated backend: could not open '%s' for writing", output_asm_path);
        mir_allocation_free(&allocation);
        mir_module_free(&mir);
        bir_module_free(&source);
        return false;
    }

    bool emit_ok;
    if (allocated_ok) {
        emit_ok = bir_x86_64_emit_allocated(&mir, &allocation, out, errbuf, errbuf_size);
    } else {
        /* Register allocation failed to produce a verified allocation
           (should not happen for a verified MIR module, but the spill-all
           emitter is a correct fallback that needs no allocation at all). */
        emit_ok = bir_x86_64_emit(&mir, out, errbuf, errbuf_size);
    }

    fclose(out);
    mir_allocation_free(&allocation);
    mir_module_free(&mir);
    bir_module_free(&source);
    return emit_ok;
}

bool bir_backend_compile_program_object(ASTNode *root, const char *source_path,
                                        const char *output_object_path,
                                        char *errbuf, size_t errbuf_size) {
    if (errbuf && errbuf_size) errbuf[0] = '\0';
    if (!root || !output_object_path) {
        if (errbuf && errbuf_size) snprintf(errbuf, errbuf_size, "isolated backend requires a program and output path");
        return false;
    }

    BackendIrModule source;
    bir_module_init(&source, source_path ? source_path : output_object_path);
    if (!bir_build_program(&source, root)) {
        if (errbuf && errbuf_size)
            snprintf(errbuf, errbuf_size, "isolated backend: %s", source.error[0] ? source.error : "program uses an unsupported construct");
        bir_module_free(&source);
        return false;
    }

    MirModule mir;
    mir_module_init(&mir, &source);
    if (!mir_lower_module(&source, &mir, errbuf, errbuf_size)) {
        mir_module_free(&mir);
        bir_module_free(&source);
        return false;
    }

    MirAllocation allocation;
    mir_allocation_init(&allocation, &mir);
    if (!mir_allocate(&mir, &allocation, errbuf, errbuf_size)) {
        mir_allocation_free(&allocation);
        mir_module_free(&mir);
        bir_module_free(&source);
        return false;
    }

    bool emit_ok = bir_x86_64_emit_object(&mir, &allocation, output_object_path, errbuf, errbuf_size);

    mir_allocation_free(&allocation);
    mir_module_free(&mir);
    bir_module_free(&source);
    return emit_ok;
}
