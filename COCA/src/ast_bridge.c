/*
 * COCA AST Bridge & Continuous Code Geometry Ingestion (src/ast_bridge.c)
 *
 * Recursively maps Cobra AST nodes into continuous phase space tensors:
 * \Psi_{Node} = \text{TypePhasor} \circledast \text{IdentPhasor} \circledast (\Psi_{\text{Left}} + \Psi_{\text{Right}})
 */

#include "../include/coca_ast_bridge.h"

static void recursive_ast_tensor(const CobraASTNode* node, coca_phasor_t* out_tensor) {
    if (!node) {
        memset(out_tensor, 0, sizeof(coca_phasor_t));
        return;
    }

    // 1. Map Node Type to Orthogonal Basis Phasor
    coca_phasor_t type_phasor;
    coca_phasor_from_seed(&type_phasor, 0xABCDEF00ULL + (uint64_t)node->node_type * 104729ULL);

    // 2. Map Identifier to Subword Phase Vector
    coca_phasor_t ident_phasor;
    coca_encode_identifier(node->identifier, &ident_phasor);

    // 3. Bind Type and Identifier
    coca_phasor_t bound_node;
    coca_phasor_bind(&type_phasor, &ident_phasor, &bound_node);

    // 4. Recursive Left & Right Child Traversal
    coca_phasor_t left_tensor, right_tensor;
    recursive_ast_tensor(node->left, &left_tensor);
    recursive_ast_tensor(node->right, &right_tensor);

    // 5. Superposition of Children: Children = Left + Right
    coca_phasor_t children_sum;
    for (int i = 0; i < COCA_HD_DIM; i++) {
        children_sum.real[i] = left_tensor.real[i] + right_tensor.real[i];
        children_sum.imag[i] = left_tensor.imag[i] + right_tensor.imag[i];
    }
    coca_phasor_normalize(&children_sum);

    // 6. Complete Structural Binding
    coca_phasor_bind(&bound_node, &children_sum, out_tensor);
    coca_phasor_normalize(out_tensor);
}

void coca_ingest_ast(const CobraASTNode* root, CognitiveCodeBlock* out_block) {
    if (!out_block) return;
    memset(out_block, 0, sizeof(CognitiveCodeBlock));

    if (!root) return;

    // Build Continuous Geometric Tensor
    recursive_ast_tensor(root, &out_block->code_tensor);

    // Default initial heuristics (to be refined by Escape & SIMD analyzers)
    out_block->requires_heap = root->escapes_scope ? 1 : 0;
    out_block->is_vectorizable = (!root->has_loop_carried_dependency && root->iteration_count >= 8) ? 1 : 0;
    out_block->simd_vector_width = out_block->is_vectorizable ? 8 : 1;
    out_block->lifetime_bounds = root->iteration_count > 0 ? (uint64_t)root->iteration_count * 4 : 64;
}
