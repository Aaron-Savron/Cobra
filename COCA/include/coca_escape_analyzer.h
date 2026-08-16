/*
 * COCA Sheaf Escape Analyzer & Zero-GC Memory Lifetime Prover (coca_escape_analyzer.h)
 *
 * Mathematical Foundations:
 *   1. Evaluates Čech Cohomology Invariant: H^1(U, F) = 0
 *   2. Proves deterministic pointer lifetime bounds across lexical scopes:
 *      - If H^1 == 0: Pointer strictly bounded to current activation frame -> requires_heap = 0 (Stack / Reg)
 *      - If H^1 > 0: Pointer escapes activation frame into return/global -> requires_heap = 1 (Heap)
 *
 * C11 native, AVX2 accelerated.
 */

#ifndef COCA_ESCAPE_ANALYZER_H
#define COCA_ESCAPE_ANALYZER_H

#include "coca_ast_bridge.h"

typedef struct {
    float sheaf_h1_obstruction;  // 0.000 = zero obstruction (no escape)
    bool escapes_current_frame;
    uint32_t live_range_cycles;
    char recommendation[128];
} coca_escape_verdict_t;

// Analyzes code block tensor against the Sheaf Cohomology invariant
void coca_analyze_escape(const CobraASTNode* ast_root, const CognitiveCodeBlock* block, coca_escape_verdict_t* out_verdict);

#endif // COCA_ESCAPE_ANALYZER_H
