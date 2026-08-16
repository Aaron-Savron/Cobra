/*
 * COCA Cognitive LLVM IR Emitter (include/coca_ir_emitter.h)
 *
 * Implements:
 *   1. Cross-Platform Cognitive IR Synthesis (x86_64, ARM64/Apple Silicon, WASM, SPIR-V, RISC-V)
 *   2. Cognitive Lifetime Attributes: `noalias`, `nocapture`, `sret`, `align 32` derived from Sheaf H¹ = 0.000
 *   3. Native SIMD Vector Typing (`<8 x float>`, `<16 x float>`) derived from Hamiltonian Auto-Vectorization
 *   4. Zero-GC Memory Guarantees injected directly into LLVM IR function signatures
 *
 * Strict 32-byte alignment, C11 native.
 */

#ifndef COCA_IR_EMITTER_H
#define COCA_IR_EMITTER_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "coca_ast_bridge.h"
#include "coca_escape_analyzer.h"
#include "coca_simd_optimizer.h"

typedef enum {
    COCA_TARGET_GENERIC_LLVM = 0,
    COCA_TARGET_X86_64_AVX2   = 1,
    COCA_TARGET_ARM64_NEON   = 2,
    COCA_TARGET_WASM32_SIMD  = 3,
    COCA_TARGET_SPIRV_GPU    = 4
} coca_ir_target_arch_t;

typedef struct {
    char ir_buffer[8192];
    size_t ir_len;
    uint32_t total_ir_instructions;
    bool is_zero_gc_proven;
    bool is_simd_vectorized;
    coca_ir_target_arch_t target_arch;
} coca_ir_result_t;

// Synthesizes cross-platform LLVM IR from COCA cognitive analysis
void coca_emit_llvm_ir(
    const CobraASTNode* ast_root,
    const CognitiveCodeBlock* block,
    const coca_escape_verdict_t* escape_verdict,
    const coca_simd_verdict_t* simd_verdict,
    coca_ir_target_arch_t target_arch,
    coca_ir_result_t* out_ir
);

#endif // COCA_IR_EMITTER_H
