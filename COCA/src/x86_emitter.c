/*
 * COCA x86_64 SIMD Assembly Emitter (src/x86_emitter.c)
 */

#include "../include/coca_x86_64_emitter.h"

void coca_emit_x86_64(
    const CobraASTNode* ast_root,
    const CognitiveCodeBlock* block,
    const coca_escape_verdict_t* escape_verdict,
    const coca_simd_verdict_t* simd_verdict,
    coca_asm_result_t* out_asm
) {
    if (!out_asm) return;
    memset(out_asm, 0, sizeof(coca_asm_result_t));

    char* p = out_asm->asm_buffer;
    size_t rem = sizeof(out_asm->asm_buffer);

    int written = 0;

    // Header & Function Prologue
    written += snprintf(p + written, rem - written,
        "; =========================================================================\n"
        "; COCA COGNITIVE COMPILER BACKEND: AUTO-GENERATED x86_64 ASSEMBLY\n"
        "; Sheaf Invariant: H¹ = %.3f | Zero-GC: %s | SIMD Width: %u\n"
        "; =========================================================================\n"
        ".global cobra_kernel_entry\n"
        ".text\n"
        "cobra_kernel_entry:\n"
        "    push rbp\n"
        "    mov rbp, rsp\n",
        escape_verdict ? escape_verdict->sheaf_h1_obstruction : 0.0f,
        (escape_verdict && !escape_verdict->escapes_current_frame) ? "PROVED (Stack/Reg)" : "HEAP",
        simd_verdict ? simd_verdict->vector_width : 1
    );

    // Zero-GC Stack Allocation (if non-escaping)
    if (escape_verdict && !escape_verdict->escapes_current_frame) {
        written += snprintf(p + written, rem - written,
            "    ; [COCA Zero-GC Prover] Bounded scope detected; pre-allocating L1 stack frame\n"
            "    sub rsp, 64\n"
        );
        out_asm->is_zero_gc = true;
    } else {
        written += snprintf(p + written, rem - written,
            "    ; [COCA Heap Fallback] Object escapes frame; invoking allocator\n"
            "    mov rdi, 64\n"
            "    call malloc\n"
        );
        out_asm->is_zero_gc = false;
    }

    // SIMD Vectorized Computation Loop
    if (simd_verdict && simd_verdict->can_vectorize) {
        written += snprintf(p + written, rem - written,
            "    ; [COCA Hamiltonian SIMD Collapse] Vectorizing %s\n"
            "    xor rcx, rcx\n"
            ".L_coca_simd_loop:\n"
            "    vmovups ymm0, [rdi + rcx*4]\n"
            "    vmovups ymm1, [rsi + rcx*4]\n"
            "    vaddps ymm0, ymm0, ymm1\n"
            "    vmovups [rdx + rcx*4], ymm0\n"
            "    add rcx, 8\n"
            "    cmp rcx, %u\n"
            "    jl .L_coca_simd_loop\n"
            "    vzeroupper\n",
            ast_root ? ast_root->identifier : "loop",
            ast_root ? ast_root->iteration_count : 1024
        );
        out_asm->is_vectorized = true;
    } else {
        written += snprintf(p + written, rem - written,
            "    ; [COCA Scalar Loop]\n"
            "    xor rcx, rcx\n"
            ".L_coca_scalar_loop:\n"
            "    movss xmm0, [rdi + rcx*4]\n"
            "    addss xmm0, [rsi + rcx*4]\n"
            "    movss [rdx + rcx*4], xmm0\n"
            "    inc rcx\n"
            "    cmp rcx, %u\n"
            "    jl .L_coca_scalar_loop\n",
            ast_root ? ast_root->iteration_count : 1024
        );
        out_asm->is_vectorized = false;
    }

    // Epilogue
    written += snprintf(p + written, rem - written,
        "    mov rsp, rbp\n"
        "    pop rbp\n"
        "    ret\n"
    );

    out_asm->asm_len = written;
    out_asm->total_instructions = 18;
}
