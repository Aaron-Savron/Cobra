/*
 * Cobra AST Manager Implementation
 */

#include "../include/cobra.h"

ASTNode *ast_create_node(ASTNodeType type, const char *name) {
    /* Zero-initialize the whole node. Several fields are only written on the
       specific syntax that introduces them; relying on malloc would leave
       those fields undefined. */
    ASTNode *node = (ASTNode *)calloc(1, sizeof(ASTNode));
    if (!node) {
        fprintf(stderr, "Fatal Error: Memory allocation failed for ASTNode\n");
        exit(EXIT_FAILURE);
    }
    node->type = type;
    node->target_device = TARGET_DEV_CPU;
    node->value_type = COBRA_TYPE_UNTYPED;
    node->declared_type = COBRA_TYPE_UNTYPED;
    node->bit_width = 64;
    node->shape_rank = 0;
    for (int i = 0; i < COBRA_MAX_SHAPE_DIMS; i++) node->shape_dims[i][0] = '\0';
    
    if (name) {
        snprintf(node->name, sizeof(node->name), "%s", name);
    } else {
        node->name[0] = '\0';
    }
    node->secondary_name[0] = '\0';
    node->int_val = 0;
    node->literal_i64 = 0;
    node->literal_u64 = 0;
    node->literal_is_unsigned = false;
    node->float_val = 0.0f;
    node->literal_f64 = 0.0;
    node->string_val[0] = '\0';
    node->asm_code[0] = '\0';
    node->source_line = 0;
    node->source_col = 0;
    node->source_file[0] = '\0';
    node->match_type_name[0] = '\0';
    node->propagate_error = false;
    node->fresh_string_result = false;
    node->is_public = true;
    node->has_visibility = false;
    node->source_import = false;
    node->module_alias[0] = '\0';
    node->qualifier[0] = '\0';
    node->canonical_type = NULL;
    node->canonical_arena = NULL;
    
    node->children = NULL;
    node->child_count = 0;
    node->child_capacity = 0;
    return node;
}

void ast_add_child(ASTNode *parent, ASTNode *child) {
    if (!parent || !child) return;
    if (parent->child_count >= parent->child_capacity) {
        size_t new_cap = parent->child_capacity == 0 ? 4 : parent->child_capacity * 2;
        ASTNode **new_children = (ASTNode **)realloc(parent->children, sizeof(ASTNode *) * new_cap);
        if (!new_children) {
            fprintf(stderr, "Fatal Error: Memory allocation failed during AST expansion\n");
            exit(EXIT_FAILURE);
        }
        parent->children = new_children;
        parent->child_capacity = new_cap;
    }
    parent->children[parent->child_count++] = child;
}

void ast_free(ASTNode *node) {
    if (!node) return;
    for (size_t i = 0; i < node->child_count; i++) {
        ast_free(node->children[i]);
    }
    if (node->children) free(node->children);
    if (node->canonical_arena) free(node->canonical_arena);
    free(node);
}

void ast_print(ASTNode *node, int indent) {
    if (!node) return;
    for (int i = 0; i < indent; i++) printf("  ");

    const char *dev_str = node->target_device == TARGET_DEV_GPU_VECTOR ? "[GPU_VECTOR]" :
                          node->target_device == TARGET_DEV_NPU ? "[NPU]" : "[CPU]";

    switch (node->type) {
        case AST_PROGRAM: printf("Program\n"); break;
        case AST_FUNCTION: printf("FunctionDecl (%s) %s\n", node->name, dev_str); break;
        case AST_VAR_DECL: printf("VarDecl (%s) [%d-bit]\n", node->name, node->bit_width); break;
        case AST_HEAP_DECL: printf("HeapDecl (%s)\n", node->name); break;
        case AST_STRUCT_DECL: printf("StructDecl (%s)\n", node->name); break;
        case AST_IMPORT_DECL: printf("ImportDecl (%s)%s%s%s\n", node->name,
            node->source_import ? " [source]" : " [c]",
            node->module_alias[0] ? " as " : "", node->module_alias); break;
        case AST_COMPTIME_EXPR: printf("ComptimeExpr\n"); break;
        case AST_COMPUTE_BLOCK: printf("ComputeBlock %s (SIMD Vectorization)\n", dev_str); break;
        case AST_PARALLEL_BLOCK: printf("ParallelBlock %s (native worker chunks)\n", dev_str); break;
        case AST_FOR_LOOP: printf("ForLoop (%s)\n", node->name); break;
        case AST_ARRAY_LITERAL: printf("ArrayLiteral\n"); break;
        case AST_DICT_LITERAL: printf("DictLiteral\n"); break;
        case AST_DICT_ENTRY: printf("DictEntry (%s)\n", node->name); break;
        case AST_ASSIGN: printf("Assign (%s)\n", node->name); break;
        case AST_INDEX_ASSIGN: printf("IndexAssign (%s)\n", node->name); break;
        case AST_RETURN: printf("Return\n"); break;
        case AST_PRINT_STMT: printf("PrintStmt\n"); break;
        case AST_ASSERT_STMT: printf("AssertStmt\n"); break;
        case AST_LEN_EXPR: printf("LenExpr\n"); break;
        case AST_ARRAY_INDEX: printf("ArrayIndex (%s)\n", node->name); break;
        case AST_INSPECT_STMT: printf("InspectStmt (%s)\n", node->name); break;
        case AST_BINARY_OP: printf("BinaryOp (%s)\n", node->name); break;
        case AST_INT_LITERAL: printf("IntLiteral (%d)\n", node->int_val); break;
        case AST_FLOAT_LITERAL: printf("FloatLiteral (%f)\n", node->float_val); break;
        case AST_STRING_LITERAL: printf("StringLiteral (\"%s\")\n", node->string_val); break;
        case AST_VAR_REF: printf("VarRef (%s)\n", node->name); break;
        case AST_ASM_BLOCK: printf("AsmBlock (%s)\n", node->asm_code); break;
        case AST_MEMBERSHIP: printf("Membership (%s)\n", node->name); break;
        case AST_COMPREHENSION: printf("Comprehension (target %s)\n", node->name); break;
        case AST_MEMBER_ACCESS: printf("MemberAccess (%s.%s)\n", node->name, node->secondary_name); break;
        case AST_MEMBER_ASSIGN: printf("MemberAssign (%s.%s)\n", node->name, node->secondary_name); break;
        case AST_BOOL_LITERAL: printf("BoolLiteral (%s)\n", node->int_val ? "true" : "false"); break;
        case AST_NONE_LITERAL: printf("NoneLiteral\n"); break;
        default: printf("UnknownNode\n"); break;
    }

    for (size_t i = 0; i < node->child_count; i++) {
        ast_print(node->children[i], indent + 1);
    }
}
