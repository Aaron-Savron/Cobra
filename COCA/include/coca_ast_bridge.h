/*
 * COCA AST Bridge & Continuous Code Geometry Ingestion (coca_ast_bridge.h)
 *
 * Implements:
 *   1. Conversion of Cobra AST nodes into continuous T^2048 phase tensors
 *   2. Recursive structural binding:
 *      \Psi_{AST} = Node_Type (*) Identifier (*) (Left (+) Right)
 *   3. Scope & Variable Binding with positional role coordinates
 *
 * Strict 32-byte alignment, C11 native, zero heap allocations.
 */

#ifndef COCA_AST_BRIDGE_H
#define COCA_AST_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include "coca_lattice.h"

typedef enum {
    COBRA_NODE_UNKNOWN       = 0,
    COBRA_NODE_FUNCTION      = 1,
    COBRA_NODE_BLOCK         = 2,
    COBRA_NODE_LET           = 3,
    COBRA_NODE_VAR           = 4,
    COBRA_NODE_ASSIGN        = 5,
    COBRA_NODE_ALLOC_STACK   = 6,
    COBRA_NODE_ALLOC_HEAP    = 7,
    COBRA_NODE_LOOP_FOR      = 8,
    COBRA_NODE_LOOP_WHILE    = 9,
    COBRA_NODE_CALL          = 10,
    COBRA_NODE_RETURN        = 11,
    COBRA_NODE_BINARY_OP     = 12,
    COBRA_NODE_ARRAY_INDEX   = 13
} cobra_ast_node_type_t;

typedef struct CobraASTNode {
    cobra_ast_node_type_t node_type;
    char identifier[64];
    uint32_t variable_id;
    uint32_t scope_depth;
    bool escapes_scope;
    bool has_loop_carried_dependency;
    uint32_t iteration_count;
    struct CobraASTNode* left;
    struct CobraASTNode* right;
} CobraASTNode;

typedef struct {
    coca_phasor_t code_tensor;
    uint64_t lifetime_bounds;    // Calculated clock-cycle lifetime
    int requires_heap;           // 0 if COCA proves it lives in registers/stack (Zero-GC)
    int is_vectorizable;         // 1 if Hamiltonian gradient proves loop independence
    uint32_t simd_vector_width;  // e.g. 8 (256-bit AVX2) or 16 (512-bit AVX-512)
} CognitiveCodeBlock;

// Recursively walks the Cobra AST and binds it into a continuous phase tensor
void coca_ingest_ast(const CobraASTNode* root, CognitiveCodeBlock* out_block);

#endif // COCA_AST_BRIDGE_H
