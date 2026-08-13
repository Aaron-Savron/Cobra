#include "../include/cobra.h"
#include <limits.h>

typedef struct {
    char name[COBRA_MAX_IDENT_LEN];
    CobraTypeKind type;
    bool owned;
    bool freed;
    bool moved;
    bool is_const;
    bool is_parameter;
    CobraMutabilityKind flow_mutability;
    /* Buffers allocated from a `with region` backing store. They are released
       by the region at scope exit, so libc free must reject them. */
    bool region_backed;
    bool region_expired;
    int region_id;
    char borrowed_from[COBRA_MAX_IDENT_LEN];
    /* Borrow metadata is per struct field. A single struct-level edge is not
       enough when a request carries more than one independent byte view. */
    int struct_field_borrow_owner[COBRA_MAX_STRUCT_FIELDS];
    int struct_field_region_id[COBRA_MAX_STRUCT_FIELDS];
    unsigned long long struct_field_initialized;
    char type_name[COBRA_MAX_IDENT_LEN];
    char error_type_name[COBRA_MAX_IDENT_LEN];
    CobraTypeKind element_type;
    CobraTypeKind key_type;
    CobraTypeKind collection_value_type;
    const CobraType *canonical_type;
    int shape_rank;
    char shape_dims[COBRA_MAX_SHAPE_DIMS][COBRA_MAX_IDENT_LEN];
} IRLocal;

typedef struct {
    char name[COBRA_MAX_IDENT_LEN];
    int field_count;
    int total_size;
    struct {
        char name[COBRA_MAX_IDENT_LEN];
        CobraTypeKind type;
        char type_name[COBRA_MAX_IDENT_LEN];
        int offset;
        CobraFieldOwnership ownership;
    } fields[COBRA_MAX_STRUCT_FIELDS];
    bool invalid_layout;
    bool has_borrowed_fields;
    bool has_owned_fields;
    char layout_error[COBRA_MAX_TOKEN_TEXT];
} IRStruct;

typedef struct {
    char name[COBRA_MAX_IDENT_LEN];
    int variant_count;
    struct {
        char name[COBRA_MAX_IDENT_LEN];
        int value;
    } variants[COBRA_MAX_ENUM_VARIANTS];
} IREnum;

typedef struct {
    IRLocal locals[128];
    size_t count;
    CobraTypeKind return_type;
    CobraTypeKind return_payload_type;
    CobraTypeKind return_error_type;
    char return_type_name[COBRA_MAX_IDENT_LEN];
    char return_error_type_name[COBRA_MAX_IDENT_LEN];
    size_t errors;
    ASTNode *root;
    IRStruct structs[COBRA_MAX_STRUCTS];
    int struct_count;
    IREnum enums[COBRA_MAX_ENUMS];
    int enum_count;
    /* Active `with region NAME:` scopes. A region-qualified alloc_f32 call is
       only legal while its qualifier names an enclosing region. */
    char regions[16][COBRA_MAX_IDENT_LEN];
    int region_ids[16];
    int next_region_id;
    int region_depth;
    CobraTypeArena *canonical_arena;
    ASTNode *current_function;
} IRContext;

static const char *canonical_type_name(const CobraType *type) {
    if (!type) return "";
    if ((type->kind == COBRA_TYPE_STRUCT || type->kind == COBRA_TYPE_ENUM) && type->name[0])
        return type->name;
    const CobraType *element = cobra_type_element(type);
    if (element && (element->kind == COBRA_TYPE_STRUCT || element->kind == COBRA_TYPE_ENUM))
        return element->name;
    return type->name;
}

static CobraTypeKind canonical_element_kind(const CobraType *type) {
    const CobraType *element = cobra_type_element(type);
    return element ? element->kind : COBRA_TYPE_UNTYPED;
}

static CobraTypeKind canonical_error_kind(const CobraType *type) {
    const CobraType *error = cobra_type_error(type);
    return error ? error->kind : COBRA_TYPE_UNTYPED;
}

static const char *type_name(CobraTypeKind type) {
    switch (type) {
        case COBRA_TYPE_I32: return "i32";
        case COBRA_TYPE_I64: return "i64";
        case COBRA_TYPE_U8: return "u8";
        case COBRA_TYPE_U32: return "u32";
        case COBRA_TYPE_U64: return "u64";
        case COBRA_TYPE_F32: return "f32";
        case COBRA_TYPE_F64: return "f64";
        case COBRA_TYPE_V256: return "v256";
        case COBRA_TYPE_VOID: return "void";
        case COBRA_TYPE_STRING: return "string";
        case COBRA_TYPE_ARRAY: return "array";
        case COBRA_TYPE_SLICE: return "[]i64";
        case COBRA_TYPE_SLICE_F32: return "[]f32";
        case COBRA_TYPE_SLICE_U8: return "[]u8";
        case COBRA_TYPE_TENSOR_F32: return "tensor[...]f32";
        case COBRA_TYPE_LIST: return "list[...]";
        case COBRA_TYPE_DICT: return "dict[string]i64";
        case COBRA_TYPE_BOOL: return "bool";
        case COBRA_TYPE_NONE: return "none";
        case COBRA_TYPE_OPTION: return "Option[T]";
        case COBRA_TYPE_RESULT: return "Result[T,E]";
        case COBRA_TYPE_GENERIC_PARAM: return "T";
        case COBRA_TYPE_ENUM: return "enum";
        case COBRA_TYPE_STRUCT: return "struct";
        case COBRA_TYPE_UNTYPED: return "untyped";
        default: return "unknown";
    }
}

static void ir_error(IRContext *ctx, ASTNode *node, const char *message) {
    const char *file = (node && node->source_file[0]) ? node->source_file : "<source>";
    int line = node && node->source_line > 0 ? node->source_line : 1;
    int col = node && node->source_col > 0 ? node->source_col : 1;
    fprintf(stderr, "%s:%d:%d: error: %s\n", file, line, col, message);
    ctx->errors++;
}

static IREnum *find_enum(IRContext *ctx, const char *name) {
    if (!ctx || !name) return NULL;
    for (int i = 0; i < ctx->enum_count; i++) {
        if (strcmp(ctx->enums[i].name, name) == 0) return &ctx->enums[i];
    }
    return NULL;
}

static IRStruct *find_struct(IRContext *ctx, const char *name) {
    if (!ctx || !name) return NULL;
    for (int i = 0; i < ctx->struct_count; i++) {
        if (strcmp(ctx->structs[i].name, name) == 0) return &ctx->structs[i];
    }
    return NULL;
}

static int find_enum_variant(IRContext *ctx, const char *enum_name, const char *variant_name) {
    IREnum *decl = find_enum(ctx, enum_name);
    if (!decl) return INT_MIN;
    for (int i = 0; i < decl->variant_count; i++) {
        if (strcmp(decl->variants[i].name, variant_name) == 0) return decl->variants[i].value;
    }
    return INT_MIN;
}

static bool register_enum_decl(IRContext *ctx, ASTNode *node) {
    if (!ctx || !node || node->type != AST_ENUM_DECL) return false;
    if (find_enum(ctx, node->name)) {
        ir_error(ctx, node, "duplicate enum declaration");
        return false;
    }
    if (ctx->enum_count >= COBRA_MAX_ENUMS || node->child_count > COBRA_MAX_ENUM_VARIANTS) {
        ir_error(ctx, node, "enum table capacity exceeded");
        return false;
    }
    IREnum *decl = &ctx->enums[ctx->enum_count++];
    memset(decl, 0, sizeof(*decl));
    snprintf(decl->name, sizeof(decl->name), "%.63s", node->name);
    for (size_t i = 0; i < node->child_count; i++) {
        ASTNode *variant = node->children[i];
        for (int j = 0; j < decl->variant_count; j++) {
            if (decl->variants[j].value == variant->int_val ||
                strcmp(decl->variants[j].name, variant->name) == 0) {
                ir_error(ctx, variant, "enum variants must have unique names and values");
            }
        }
        snprintf(decl->variants[decl->variant_count].name,
                 sizeof(decl->variants[decl->variant_count].name), "%.63s", variant->name);
        decl->variants[decl->variant_count].value = variant->int_val;
        decl->variant_count++;
    }
    return true;
}

static bool find_local(IRContext *ctx, const char *name, CobraTypeKind *out_type) {
    for (size_t i = 0; i < ctx->count; i++) {
        if (strcmp(ctx->locals[i].name, name) == 0) {
            if (out_type) *out_type = ctx->locals[i].type;
            return true;
        }
    }
    return false;
}

static IRLocal *find_local_entry(IRContext *ctx, const char *name) {
    for (size_t i = 0; i < ctx->count; i++) {
        if (strcmp(ctx->locals[i].name, name) == 0) return &ctx->locals[i];
    }
    return NULL;
}

static void init_local_metadata(IRLocal *local) {
    if (!local) return;
    for (int i = 0; i < COBRA_MAX_STRUCT_FIELDS; i++) {
        local->struct_field_borrow_owner[i] = -1;
        local->struct_field_region_id[i] = -1;
    }
    local->region_id = -1;
}

static bool add_local(IRContext *ctx, const char *name, CobraTypeKind type, const ASTNode *shape_source) {
    CobraTypeKind existing;
    if (find_local(ctx, name, &existing)) return false;
    if (ctx->count >= 128) return false;
    IRLocal *local = &ctx->locals[ctx->count];
    memset(local, 0, sizeof(*local));
    snprintf(local->name, sizeof(local->name), "%s", name);
    local->type = type;
    if (shape_source) {
        local->canonical_type = shape_source->canonical_type;
        local->is_const = shape_source->is_const;
        local->flow_mutability = local->canonical_type
            ? local->canonical_type->mutability : COBRA_MUTABILITY_DEFAULT;
        snprintf(local->type_name, sizeof(local->type_name), "%.63s",
                 canonical_type_name(local->canonical_type));
        const CobraType *local_error = cobra_type_error(local->canonical_type);
        snprintf(local->error_type_name, sizeof(local->error_type_name), "%.63s",
                 local_error && local_error->kind == COBRA_TYPE_STRUCT ? local_error->name : "");
        local->element_type = canonical_element_kind(local->canonical_type);
        const CobraType *local_key = cobra_type_key(local->canonical_type);
        const CobraType *local_value = cobra_type_value(local->canonical_type);
        local->key_type = local_key ? local_key->kind : COBRA_TYPE_UNTYPED;
        local->collection_value_type = local_error ? local_error->kind :
                                       (local_value ? local_value->kind : COBRA_TYPE_UNTYPED);
        local->shape_rank = shape_source->shape_rank;
        for (int i = 0; i < local->shape_rank; i++) {
            snprintf(local->shape_dims[i], sizeof(local->shape_dims[i]), "%.63s", shape_source->shape_dims[i]);
        }
    }
    init_local_metadata(local);
    ctx->count++;
    return true;
}

/* Preserve borrow edges from block-local aliases after the block's locals are
   discarded. The first scope model is function-wide, so a source free after a
   conditional must remain conservative even if the alias was declared there. */
static void merge_branch_borrows(IRLocal *saved_locals, size_t *saved_count,
                                 const IRLocal *branch_locals, size_t branch_count,
                                 size_t branch_base) {
    if (!saved_locals || !saved_count || !branch_locals) return;
    for (size_t i = branch_base; i < branch_count; i++) {
        const IRLocal *branch = &branch_locals[i];
        /* Tensor views share the borrow field but have explicit native
           lifetimes; only everyday string aliases belong in this synthetic
           function-wide record set. */
        if (branch->type != COBRA_TYPE_STRING || !branch->borrowed_from[0]) continue;
        bool already_recorded = false;
        for (size_t j = 0; j < *saved_count; j++) {
            if (strcmp(saved_locals[j].borrowed_from, branch->borrowed_from) == 0 &&
                !saved_locals[j].freed) {
                already_recorded = true;
                break;
            }
        }
        if (already_recorded || *saved_count >= 128) continue;
        IRLocal *record = &saved_locals[(*saved_count)++];
        memset(record, 0, sizeof(*record));
        init_local_metadata(record);
        snprintf(record->name, sizeof(record->name), "__cobra_branch_borrow_%zu", *saved_count);
        record->type = COBRA_TYPE_STRING;
        snprintf(record->borrowed_from, sizeof(record->borrowed_from), "%.63s", branch->borrowed_from);
    }
}

static bool is_integer(CobraTypeKind type) {
    return type == COBRA_TYPE_I32 || type == COBRA_TYPE_I64 ||
           type == COBRA_TYPE_U8 || type == COBRA_TYPE_U32 || type == COBRA_TYPE_U64;
}

/* Scalar sums use an indirect pointer ABI for parameters. Keep this first
   parameter milestone deliberately narrow: no nested sums, views, strings,
   collections, or ownership-bearing structs cross the boundary yet. */
static bool is_scalar_sum_component(CobraTypeKind type) {
    return is_integer(type) || type == COBRA_TYPE_F32 || type == COBRA_TYPE_BOOL;
}

static bool is_slice_type(CobraTypeKind type) {
    return type == COBRA_TYPE_SLICE || type == COBRA_TYPE_SLICE_F32 ||
           type == COBRA_TYPE_SLICE_U8 || type == COBRA_TYPE_TENSOR_F32;
}

static bool is_collection_type(CobraTypeKind type) {
    return type == COBRA_TYPE_LIST || type == COBRA_TYPE_DICT;
}

/* Inferred expressions have no parser declaration to attach to. Build their
   canonical descriptor from already-canonical children and explicit inference
   results, rather than reconstructing a type through AST legacy fields. */
static const CobraType *canonical_inferred_type(IRContext *ctx, ASTNode *node);

static const CobraType *canonical_inferred_type(IRContext *ctx, ASTNode *node) {
    if (!ctx || !node || !ctx->canonical_arena) return NULL;
    if (node->canonical_type) return node->canonical_type;
    CobraTypeKind kind = node->declared_type != COBRA_TYPE_UNTYPED ?
                         node->declared_type : node->value_type;
    if (kind == COBRA_TYPE_UNTYPED || kind == COBRA_TYPE_UNKNOWN) return NULL;
    const CobraType *element = NULL;
    const CobraType *error = NULL;
    const CobraType *key = NULL;
    const CobraType *value = NULL;
    if ((kind == COBRA_TYPE_ARRAY || kind == COBRA_TYPE_LIST) && node->child_count > 0) {
        ASTNode *source = node->type == AST_ARRAY_LITERAL ? node : node->children[0];
        if (source && source->type == AST_ARRAY_LITERAL && source->child_count > 0)
            element = source->children[0]->canonical_type;
    }
    const CobraType *node_element = cobra_type_node_element(node);
    if (!element && node_element) element = node_element;
    const CobraType *node_error = cobra_type_node_error(node);
    if (kind == COBRA_TYPE_RESULT) {
        if (node_error) error = node_error;
    } else if (kind == COBRA_TYPE_DICT) {
        key = cobra_type_node_key(node);
        value = cobra_type_node_value(node);
    }
    node->canonical_type = cobra_type_make(ctx->canonical_arena, kind,
                                           cobra_type_node_name(node),
                                           element, error, key, value,
                                           COBRA_OWNERSHIP_VALUE,
                                           COBRA_MUTABILITY_DEFAULT, -1);
    return node->canonical_type;
}

static bool is_f32_buffer_type(CobraTypeKind type) {
    return type == COBRA_TYPE_SLICE_F32 || type == COBRA_TYPE_TENSOR_F32;
}

static CobraTypeKind slice_element_type(CobraTypeKind type) {
    if (type == COBRA_TYPE_SLICE_F32) return COBRA_TYPE_F32;
    if (type == COBRA_TYPE_SLICE_U8) return COBRA_TYPE_U8;
    if (type == COBRA_TYPE_SLICE) return COBRA_TYPE_I64;
    return COBRA_TYPE_UNTYPED;
}

static bool is_tensor_view_builtin(const char *name) {
    return strcmp(name, "reshape_view") == 0 ||
           strcmp(name, "transpose_view") == 0 ||
           strcmp(name, "slice_view") == 0;
}

static bool declared_compatible(CobraTypeKind declared, CobraTypeKind inferred) {
    if (declared == inferred) return true;
    /* alloc_f32 is the storage constructor for a shaped tensor; the
       declaration adds the rank/shape contract without adding a copy. */
    if (declared == COBRA_TYPE_TENSOR_F32 && inferred == COBRA_TYPE_SLICE_F32) return true;
    if (declared == COBRA_TYPE_LIST && inferred == COBRA_TYPE_ARRAY) return true;
    if (declared == COBRA_TYPE_DICT && inferred == COBRA_TYPE_DICT) return true;
    /* none fills any slot (default/empty value) without adding an object. */
    if (inferred == COBRA_TYPE_NONE) return true;
    return is_integer(declared) && is_integer(inferred);
}

static bool shape_dim_is_integer(const char *dim) {
    if (!dim || !*dim) return false;
    for (const char *p = dim; *p; p++) if (!isdigit((unsigned char)*p)) return false;
    return true;
}

static bool shape_product(const ASTNode *node, long long *out) {
    long long product = 1;
    if (!node || node->shape_rank == 0 || !out) return false;
    for (int i = 0; i < node->shape_rank; i++) {
        if (!shape_dim_is_integer(node->shape_dims[i])) return false;
        long long dim = atoll(node->shape_dims[i]);
        if (dim < 0 || (dim != 0 && product > 0x7fffffffffffffffLL / dim)) return false;
        product *= dim;
    }
    *out = product;
    return true;
}

static bool local_shape(IRContext *ctx, ASTNode *node, int *rank,
                        char dims[COBRA_MAX_SHAPE_DIMS][COBRA_MAX_IDENT_LEN]) {
    if (!node || node->type != AST_VAR_REF) return false;
    IRLocal *local = find_local_entry(ctx, node->name);
    if (!local || local->shape_rank == 0) return false;
    if (rank) *rank = local->shape_rank;
    if (dims) for (int i = 0; i < local->shape_rank; i++) snprintf(dims[i], COBRA_MAX_IDENT_LEN, "%.63s", local->shape_dims[i]);
    return true;
}

static CobraTypeKind infer_expr(ASTNode *node, IRContext *ctx);

static bool expression_is_const_zero(ASTNode *node) {
    return node && node->type == AST_INT_LITERAL && node->int_val == 0;
}

/* Resolve only compile-time integer expressions. Dynamic dimensions remain
   valid and simply defer their checks to the native kernel's existing bounds
   validation. */
static bool const_int_expr(ASTNode *node, long long *out) {
    if (!node || !out) return false;
    if (node->type == AST_INT_LITERAL) {
        *out = node->int_val;
        return true;
    }
    if (node->type == AST_COMPTIME_EXPR && node->child_count == 1) {
        return const_int_expr(node->children[0], out);
    }
    if (node->type == AST_BINARY_OP && node->child_count == 2) {
        long long left = 0, right = 0;
        if (!const_int_expr(node->children[0], &left) || !const_int_expr(node->children[1], &right)) return false;
        if (strcmp(node->name, "+") == 0) *out = left + right;
        else if (strcmp(node->name, "-") == 0) *out = left - right;
        else if (strcmp(node->name, "*") == 0) *out = left * right;
        else if (strcmp(node->name, "/") == 0 && right != 0) *out = left / right;
        else return false;
        return true;
    }
    return false;
}

static bool shape_dim_matches(const char *expected, const char *actual) {
    /* If either side is symbolic, its runtime value is not known here. Keep
       the contract valid and let the native buffer checks enforce it later. */
    if (!shape_dim_is_integer(expected) || !shape_dim_is_integer(actual)) return true;
    return atoll(expected) == atoll(actual);
}

static bool shape_dims_known_incompatible(const char *left, const char *right) {
    /* For operator contracts, distinct symbols represent distinct dimensions;
       numeric/symbolic pairs remain dynamic and are checked at runtime. */
    if (shape_dim_is_integer(left) && shape_dim_is_integer(right)) return atoll(left) != atoll(right);
    if (!shape_dim_is_integer(left) && !shape_dim_is_integer(right)) return strcmp(left, right) != 0;
    return false;
}

static void validate_shape_contracts(ASTNode *call, ASTNode *function, IRContext *ctx) {
    char bindings[COBRA_MAX_SHAPE_DIMS][COBRA_MAX_IDENT_LEN] = {{0}};
    char binding_values[COBRA_MAX_SHAPE_DIMS][COBRA_MAX_IDENT_LEN] = {{0}};
    int binding_count = 0;

    size_t argument_index = 0;
    for (size_t i = 0; i < function->child_count && argument_index < call->child_count; i++) {
        ASTNode *param = function->children[i];
        if (param->type != AST_PARAM) continue;
        ASTNode *argument = call->children[argument_index];
        CobraTypeKind argument_type = infer_expr(argument, ctx);
        if (!is_f32_buffer_type(param->declared_type) || param->shape_rank == 0 ||
            !is_f32_buffer_type(argument_type)) {
            argument_index++;
            continue;
        }

        int actual_rank = 0;
        char actual_dims[COBRA_MAX_SHAPE_DIMS][COBRA_MAX_IDENT_LEN] = {{0}};
        if (!local_shape(ctx, argument, &actual_rank, actual_dims)) {
            char message[220];
            snprintf(message, sizeof(message), "argument %zu for '%.31s' requires tensor shape metadata; use tensor[...]f32 instead of []f32",
                     argument_index + 1, function->name);
            ir_error(ctx, call, message);
            argument_index++;
            continue;
        }
        if (actual_rank != param->shape_rank) {
            char message[180];
            snprintf(message, sizeof(message), "argument %zu for '%.31s' has rank %d, expected rank %d",
                     argument_index + 1, function->name, actual_rank, param->shape_rank);
            ir_error(ctx, call, message);
            argument_index++;
            continue;
        }

        for (int dimension = 0; dimension < param->shape_rank; dimension++) {
            const char *expected = param->shape_dims[dimension];
            const char *actual = actual_dims[dimension];
            if (!shape_dim_matches(expected, actual)) {
                char message[220];
                snprintf(message, sizeof(message), "argument %zu for '%.31s' has dimension %d='%.15s', expected '%.15s'",
                         argument_index + 1, function->name, dimension, actual, expected);
                ir_error(ctx, call, message);
            }
            if (!shape_dim_is_integer(expected) && shape_dim_is_integer(actual)) {
                int binding = -1;
                for (int j = 0; j < binding_count; j++) {
                    if (strcmp(bindings[j], expected) == 0) { binding = j; break; }
                }
                if (binding < 0) {
                    if (binding_count < COBRA_MAX_SHAPE_DIMS) {
                        snprintf(bindings[binding_count], COBRA_MAX_IDENT_LEN, "%.63s", expected);
                        snprintf(binding_values[binding_count], COBRA_MAX_IDENT_LEN, "%.63s", actual);
                        binding_count++;
                    }
                } else if ((shape_dim_is_integer(binding_values[binding]) && shape_dim_is_integer(actual) &&
                            strcmp(binding_values[binding], actual) != 0) ||
                           (!shape_dim_is_integer(binding_values[binding]) && !shape_dim_is_integer(actual) &&
                            strcmp(binding_values[binding], actual) != 0)) {
                    char message[220];
                    snprintf(message, sizeof(message), "argument %zu for '%.31s' violates repeated dimension '%.31s'",
                             argument_index + 1, function->name, expected);
                    ir_error(ctx, call, message);
                }
            }
        }
        argument_index++;
    }
}

static void validate_declared_shape(ASTNode *declaration, ASTNode *initializer, IRContext *ctx) {
    if (!declaration || declaration->shape_rank == 0 || !initializer || initializer->type != AST_FUNC_CALL ||
        (strcmp(initializer->name, "alloc_f32") != 0 && strcmp(initializer->name, "alloc_i64") != 0)) return;

    long long expected = 0;
    if (!shape_product(declaration, &expected)) {
        bool has_symbolic_dim = false;
        for (int i = 0; i < declaration->shape_rank; i++) {
            if (!shape_dim_is_integer(declaration->shape_dims[i])) { has_symbolic_dim = true; break; }
        }
        if (!has_symbolic_dim) ir_error(ctx, declaration, "tensor shape is too large for compile-time size validation");
        return;
    }
    long long actual = 0;
    if (initializer->child_count == 1 && const_int_expr(initializer->children[0], &actual) && expected != actual) {
        char message[220];
        snprintf(message, sizeof(message), "tensor '%s' has shape size %lld but allocates %lld elements",
                 declaration->name, expected, actual);
        ir_error(ctx, declaration, message);
    }
}

static void validate_matrix_shapes(ASTNode *node, IRContext *ctx, int dense) {
    int count = dense ? 4 : 3;
    /* Compare the symbolic layout itself, not only M/N/K literals. This catches
       [m,k] x [q,n] when k and q are known distinct symbols, while numeric/
       symbolic pairs remain dynamic and are left to runtime bounds checks. */
    int relation_count = dense ? 4 : 3;
    int relation_left[4] = {0, 0, 1, 2};
    int relation_left_dim[4] = {0, 1, 1, 0};
    int relation_right[4] = {0, 1, 0, 1};
    int relation_right_dim[4] = {0, 0, 1, 1};
    if (dense) {
        relation_right[0] = 3;
        relation_right[1] = 1;
        relation_right[2] = 3;
        relation_right[3] = 3;
    } else {
        relation_right[0] = 2;
        relation_right[1] = 1;
        relation_right[2] = 2;
    }
    for (int relation = 0; relation < relation_count; relation++) {
        int left_rank = 0, right_rank = 0;
        char left_dims[COBRA_MAX_SHAPE_DIMS][COBRA_MAX_IDENT_LEN] = {{0}};
        char right_dims[COBRA_MAX_SHAPE_DIMS][COBRA_MAX_IDENT_LEN] = {{0}};
        if (!local_shape(ctx, node->children[relation_left[relation]], &left_rank, left_dims) ||
            !local_shape(ctx, node->children[relation_right[relation]], &right_rank, right_dims)) continue;
        if (relation_left_dim[relation] >= left_rank || relation_right_dim[relation] >= right_rank) continue;
        if (shape_dims_known_incompatible(left_dims[relation_left_dim[relation]], right_dims[relation_right_dim[relation]])) {
            char message[220];
            snprintf(message, sizeof(message), "%.63s tensor dimensions are incompatible at contract %d",
                     node->name, relation + 1);
            ir_error(ctx, node, message);
        }
    }


    int dimension_offset = dense ? 4 : 3;
    if (node->child_count < (size_t)(dimension_offset + 3)) return;

    long long m = 0, n = 0, k = 0;
    bool known_m = const_int_expr(node->children[dimension_offset], &m);
    bool known_n = const_int_expr(node->children[dimension_offset + 1], &n);
    bool known_k = const_int_expr(node->children[dimension_offset + 2], &k);
    (void)count;

    for (int buffer = 0; buffer < count; buffer++) {
        int rank = 0;
        char dims[COBRA_MAX_SHAPE_DIMS][COBRA_MAX_IDENT_LEN] = {{0}};
        if (!local_shape(ctx, node->children[buffer], &rank, dims)) continue;
        int expected_rank = dense && buffer == 2 ? 1 : 2;
        if (rank != expected_rank) {
            char message[180];
            snprintf(message, sizeof(message), "%.63s tensor argument %d must be rank %d, got rank %d",
                     node->name, buffer + 1, expected_rank, rank);
            ir_error(ctx, node, message);
            continue;
        }
        long long rows = 0, cols = 0;
        bool known_rows = shape_dim_is_integer(dims[0]);
        bool known_cols = rank > 1 && shape_dim_is_integer(dims[1]);
        if (known_rows) rows = atoll(dims[0]);
        if (known_cols) cols = atoll(dims[1]);

        bool mismatch = false;
        if (!dense) {
            if (buffer == 0) mismatch = (known_m && known_rows && rows != m) || (known_k && known_cols && cols != k);
            else if (buffer == 1) mismatch = (known_k && known_rows && rows != k) || (known_n && known_cols && cols != n);
            else mismatch = (known_m && known_rows && rows != m) || (known_n && known_cols && cols != n);
        } else {
            if (buffer == 0) mismatch = (known_m && known_rows && rows != m) || (known_k && known_cols && cols != k);
            else if (buffer == 1) mismatch = (known_k && known_rows && rows != k) || (known_n && known_cols && cols != n);
            else if (buffer == 2) mismatch = known_n && known_rows && rows != n;
            else mismatch = (known_m && known_rows && rows != m) || (known_n && known_cols && cols != n);
        }
        if (mismatch) {
            char message[220];
            snprintf(message, sizeof(message), "%s tensor shape does not match its M/N/K dimensions",
                     node->name);
            ir_error(ctx, node, message);
        }
    }
}

static void reject_illegal_float_context(ASTNode *node, IRContext *ctx, const char *message) {
    /* Scalar f32 expressions are now a supported local value type. Keep this
       helper as a compatibility seam for contexts that still need an ABI rule. */
    (void)node;
    (void)ctx;
    (void)message;
}

static bool is_tensor_builtin(const char *name) {
    return strcmp(name, "fill_f32") == 0 || strcmp(name, "relu_f32") == 0 ||
           strcmp(name, "matmul_f32") == 0 || strcmp(name, "dense_f32") == 0 ||
           strcmp(name, "sum_f32") == 0 || strcmp(name, "mean_f32") == 0 ||
           strcmp(name, "max_f32") == 0 || strcmp(name, "exp_f32") == 0 ||
           strcmp(name, "sqrt_f32") == 0 || strcmp(name, "tanh_f32") == 0 ||
           strcmp(name, "log_f32") == 0 || strcmp(name, "pow_f32") == 0 ||
           is_tensor_view_builtin(name);
}

static bool is_string_builtin(const char *name) {
    return strcmp(name, "concat") == 0 || strcmp(name, "starts_with") == 0 ||
           strcmp(name, "ends_with") == 0 || strcmp(name, "contains") == 0 ||
           strcmp(name, "char_at") == 0;
}

static bool is_string_free_builtin(const char *name) {
    return strcmp(name, "string_free") == 0;
}

static bool is_sum_builtin(const char *name) {
    return !strcmp(name, "some") || !strcmp(name, "none") ||
           !strcmp(name, "ok") || !strcmp(name, "err") ||
           !strcmp(name, "is_some") || !strcmp(name, "unwrap") ||
           !strcmp(name, "is_ok") || !strcmp(name, "unwrap_ok") ||
           !strcmp(name, "unwrap_err");
}

static bool is_python_aggregate_builtin(const char *name) {
    return strcmp(name, "sum") == 0 || strcmp(name, "min") == 0 ||
           strcmp(name, "max") == 0 || strcmp(name, "any") == 0 ||
           strcmp(name, "all") == 0;
}

static bool is_string_comparison(const char *op) {
    return strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
           strcmp(op, "<") == 0 || strcmp(op, ">") == 0 ||
           strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0;
}

static bool local_is_type(IRContext *ctx, ASTNode *node, CobraTypeKind type) {
    CobraTypeKind actual = COBRA_TYPE_UNKNOWN;
    if (!node || node->type != AST_VAR_REF || !find_local(ctx, node->name, &actual)) return false;
    if (type == COBRA_TYPE_SLICE_F32) return is_f32_buffer_type(actual);
    return actual == type;
}

static void reject_gemm_direct_alias(ASTNode *node, IRContext *ctx, int output_index, int input_count) {
    if (!node || node->child_count <= (size_t)output_index ||
        node->children[output_index]->type != AST_VAR_REF) return;
    const char *output = node->children[output_index]->name;
    for (int i = 0; i < input_count && i < (int)node->child_count; i++) {
        if (node->children[i]->type == AST_VAR_REF &&
            strcmp(output, node->children[i]->name) == 0) {
            ir_error(ctx, node, "GEMM output buffer must not alias an input buffer");
            return;
        }
    }
}

static bool is_readonly_f32_buffer_expr(ASTNode *node, IRContext *ctx) {
    if (!node || node->type != AST_VAR_REF) return false;
    IRLocal *local = find_local_entry(ctx, node->name);
    if (!local) return false;
    if (is_f32_buffer_type(local->type)) return true;
    return local->type == COBRA_TYPE_ARRAY && local->element_type == COBRA_TYPE_F32;
}

static void validate_tensor_builtin(ASTNode *node, IRContext *ctx) {
    if (is_tensor_view_builtin(node->name)) {
        if (strcmp(node->name, "transpose_view") == 0) {
            if (node->child_count != 1 || !local_is_type(ctx, node->children[0], COBRA_TYPE_TENSOR_F32)) {
                ir_error(ctx, node, "transpose_view requires one f32 tensor view");
            }
        } else if (strcmp(node->name, "reshape_view") == 0) {
            if (node->child_count != 3 || !local_is_type(ctx, node->children[0], COBRA_TYPE_TENSOR_F32) ||
                !is_integer(infer_expr(node->children[1], ctx)) ||
                !is_integer(infer_expr(node->children[2], ctx))) {
                ir_error(ctx, node, "reshape_view requires (tensor, rows, cols)");
            }
        } else if (node->child_count != 3 || !local_is_type(ctx, node->children[0], COBRA_TYPE_TENSOR_F32) ||
                   !is_integer(infer_expr(node->children[1], ctx)) ||
                   !is_integer(infer_expr(node->children[2], ctx))) {
            ir_error(ctx, node, "slice_view requires (tensor, start, length)");
        }
        return;
    }
    if (strcmp(node->name, "fill_f32") == 0) {
        if (node->child_count != 2 ||
            !is_f32_buffer_type(node->children[0]->type == AST_VAR_REF ?
                                (find_local_entry(ctx, node->children[0]->name) ?
                                 find_local_entry(ctx, node->children[0]->name)->type : COBRA_TYPE_UNKNOWN) : COBRA_TYPE_UNKNOWN) ||
            (node->children[1]->type != AST_INT_LITERAL && node->children[1]->type != AST_FLOAT_LITERAL)) {
            ir_error(ctx, node, "fill_f32 requires ([]f32, numeric f32 scalar)");
        }
    } else if (strcmp(node->name, "relu_f32") == 0) {
        if (node->child_count != 1 ||
            !is_f32_buffer_type(node->children[0]->type == AST_VAR_REF ?
                                (find_local_entry(ctx, node->children[0]->name) ?
                                 find_local_entry(ctx, node->children[0]->name)->type : COBRA_TYPE_UNKNOWN) : COBRA_TYPE_UNKNOWN)) {
            char message[120];
            snprintf(message, sizeof(message), "%s requires one mutable []f32 buffer", node->name);
            ir_error(ctx, node, message);
        }
    } else if (strcmp(node->name, "sum_f32") == 0 || strcmp(node->name, "mean_f32") == 0 ||
               strcmp(node->name, "max_f32") == 0) {
        if (node->child_count != 1 || !is_readonly_f32_buffer_expr(node->children[0], ctx)) {
            char message[120];
            snprintf(message, sizeof(message), "%s requires one readonly []f32 buffer", node->name);
            ir_error(ctx, node, message);
        }
    } else if (strcmp(node->name, "exp_f32") == 0 || strcmp(node->name, "sqrt_f32") == 0 ||
               strcmp(node->name, "tanh_f32") == 0 || strcmp(node->name, "log_f32") == 0) {
        if (node->child_count != 1) {
            ir_error(ctx, node, "math intrinsic requires one f32 argument");
        } else {
            CobraTypeKind arg = infer_expr(node->children[0], ctx);
            if (arg != COBRA_TYPE_F32 && arg != COBRA_TYPE_UNKNOWN) {
                ir_error(ctx, node, "math intrinsic requires an f32 argument");
            }
        }
    } else if (strcmp(node->name, "pow_f32") == 0) {
        if (node->child_count != 2) {
            ir_error(ctx, node, "pow_f32 requires two f32 arguments");
        } else {
            CobraTypeKind a = infer_expr(node->children[0], ctx);
            CobraTypeKind b = infer_expr(node->children[1], ctx);
            if ((a != COBRA_TYPE_F32 && a != COBRA_TYPE_UNKNOWN) ||
                (b != COBRA_TYPE_F32 && b != COBRA_TYPE_UNKNOWN)) {
                ir_error(ctx, node, "pow_f32 requires two f32 arguments");
            }
        }
    } else if (strcmp(node->name, "matmul_f32") == 0) {
        reject_gemm_direct_alias(node, ctx, 2, 2);
        if (node->child_count != 6 ||
            !local_is_type(ctx, node->children[0], COBRA_TYPE_SLICE_F32) ||
            !local_is_type(ctx, node->children[1], COBRA_TYPE_SLICE_F32) ||
            !local_is_type(ctx, node->children[2], COBRA_TYPE_SLICE_F32) ||
            !is_integer(infer_expr(node->children[3], ctx)) ||
            !is_integer(infer_expr(node->children[4], ctx)) ||
            !is_integer(infer_expr(node->children[5], ctx))) {
            ir_error(ctx, node, "matmul_f32 requires (A, B, C: []f32, M, N, K: integer)");
        }
    } else if (strcmp(node->name, "dense_f32") == 0) {
        reject_gemm_direct_alias(node, ctx, 3, 3);
        if (node->child_count != 7 ||
            !local_is_type(ctx, node->children[0], COBRA_TYPE_SLICE_F32) ||
            !local_is_type(ctx, node->children[1], COBRA_TYPE_SLICE_F32) ||
            !local_is_type(ctx, node->children[2], COBRA_TYPE_SLICE_F32) ||
            !local_is_type(ctx, node->children[3], COBRA_TYPE_SLICE_F32) ||
            !is_integer(infer_expr(node->children[4], ctx)) ||
            !is_integer(infer_expr(node->children[5], ctx)) ||
            !is_integer(infer_expr(node->children[6], ctx))) {
            ir_error(ctx, node, "dense_f32 requires (input, weights, bias, output: []f32, M, N, K: integer)");
        }
    }
}

static ASTNode *find_function(IRContext *ctx, const char *name) {
    if (!ctx->root) return NULL;
    for (size_t i = 0; i < ctx->root->child_count; i++) {
        ASTNode *child = ctx->root->children[i];
        if (child->type == AST_FUNCTION && strcmp(child->name, name) == 0) return child;
    }
    return NULL;
}

static bool generic_scalar_argument(const CobraType *type) {
    return cobra_type_is_scalar(type);
}

static bool generic_slice_kind(CobraTypeKind kind) {
    return cobra_type_is_slice_kind(kind);
}

static bool bind_generic_type(const CobraType *pattern, const CobraType *actual,
                              const CobraType *parameter,
                              const CobraType **binding) {
    /* Generic identity, including template origin and borrowed-view contracts,
       belongs to the canonical type graph. IR only supplies the call-site
       pattern and receives the inferred binding. */
    return cobra_type_bind_generic(pattern, actual, parameter, binding);
}

static bool type_contains_generic_type(const CobraType *type,
                                       const CobraType *parameter);
static const CobraType *specialize_struct_reference(IRContext *ctx,
                                                     const CobraType *requested,
                                                     const CobraType *parameter,
                                                     const CobraType *argument,
                                                     ASTNode *use_site);

static const CobraType *specialize_canonical_type(IRContext *ctx,
                                                   const CobraType *type,
                                                   const CobraType *parameter,
                                                   const CobraType *argument) {
    if (!type) return NULL;
    if (type == parameter) return argument;
    if ((type->kind == COBRA_TYPE_OPTION || type->kind == COBRA_TYPE_RESULT ||
         generic_slice_kind(type->kind)) &&
        type_contains_generic_type(type, parameter)) {
        return cobra_type_instantiate(ctx->canonical_arena, type, parameter, argument);
    }
    if (type->kind == COBRA_TYPE_STRUCT && type->generic_arg_count > 0 &&
        type_contains_generic_type(type, parameter)) {
        return specialize_struct_reference(ctx, type, parameter, argument, NULL);
    }
    return type;
}

static bool type_contains_generic_type(const CobraType *type,
                                       const CobraType *parameter) {
    if (!type || !parameter) return false;
    if (type == parameter) return true;
    for (size_t i = 0; i < type->generic_arg_count; i++) {
        if (type_contains_generic_type(type->generic_args[i], parameter)) return true;
    }
    return false;
}

static void specialize_ast_tree(IRContext *ctx, ASTNode *node,
                                const CobraType *parameter,
                                const CobraType *argument) {
    if (!node) return;
    if (node->canonical_type) {
        bool contains_parameter = type_contains_generic_type(node->canonical_type, parameter);
        node->canonical_type = specialize_canonical_type(ctx, node->canonical_type,
                                                         parameter, argument);
        if (node->canonical_type) {
            if (contains_parameter && node->declared_type != COBRA_TYPE_UNTYPED)
                node->declared_type = node->canonical_type->kind;
            if (contains_parameter && node->value_type != COBRA_TYPE_UNTYPED)
                node->value_type = node->canonical_type->kind;
            if (!cobra_type_validate(ctx->canonical_arena, node->canonical_type))
                ir_error(ctx, node, "generic specialization has no valid ABI representation");
        }
    }
    for (size_t i = 0; i < node->child_count; i++)
        specialize_ast_tree(ctx, node->children[i], parameter, argument);
}

static ASTNode *clone_ast_tree(const ASTNode *source) {
    if (!source) return NULL;
    ASTNode *copy = ast_create_node(source->type, source->name);
    if (!copy) return NULL;
    *copy = *source;
    copy->children = NULL;
    copy->child_count = 0;
    copy->child_capacity = 0;
    for (size_t i = 0; i < source->child_count; i++) {
        ASTNode *child = clone_ast_tree(source->children[i]);
        if (!child) {
            ast_free(copy);
            return NULL;
        }
        ast_add_child(copy, child);
    }
    return copy;
}

static ASTNode *find_specialization(IRContext *ctx, ASTNode *generic,
                                    const CobraType *argument) {
    if (!ctx || !ctx->root || !generic || !argument) return NULL;
    for (size_t i = 0; i < ctx->root->child_count; i++) {
        ASTNode *candidate = ctx->root->children[i];
        if (candidate->type == AST_FUNCTION &&
            candidate->specialized_from == generic &&
            candidate->specialization_arg_count == 1 &&
            candidate->specialization_args[0] &&
            cobra_type_equal(candidate->specialization_args[0], argument)) return candidate;
    }
    return NULL;
}

static ASTNode *specialize_generic_function(IRContext *ctx, ASTNode *generic,
                                             const CobraType *argument) {
    if (!ctx || !generic || generic->generic_param_count != 1 ||
        !generic->generic_param_types[0] || !generic_scalar_argument(argument)) return NULL;
    if (ctx->current_function &&
        (ctx->current_function == generic ||
         ctx->current_function->specialized_from == generic)) {
        ir_error(ctx, generic, "recursive generic instantiation is not supported");
        return NULL;
    }
    char specialized_name[COBRA_MAX_IDENT_LEN];
    snprintf(specialized_name, sizeof(specialized_name), "%.47s__%s",
             generic->name, cobra_type_kind_name(argument->kind));
    ASTNode *existing = find_specialization(ctx, generic, argument);
    if (existing) return existing;
    ASTNode *name_collision = find_function(ctx, specialized_name);
    if (name_collision) {
        /* Reuse was already decided by canonical specialization arguments.
           Reaching this branch means the symbol spelling collides with a
           different specialization or user function. */
        ir_error(ctx, generic, "generic specialization name collides with an existing function");
        return NULL;
    }
    ASTNode *specialized = clone_ast_tree(generic);
    if (!specialized) {
        ir_error(ctx, generic, "could not clone generic function specialization");
        return NULL;
    }
    snprintf(specialized->name, sizeof(specialized->name), "%.63s", specialized_name);
    specialized->generic_param_count = 0;
    memset(specialized->generic_param_names, 0, sizeof(specialized->generic_param_names));
    memset(specialized->generic_param_types, 0, sizeof(specialized->generic_param_types));
    specialized->specialized_from = generic;
    specialized->specialization_arg_count = 1;
    specialized->specialization_args[0] = argument;
    specialize_ast_tree(ctx, specialized, generic->generic_param_types[0], argument);
    for (size_t i = 0; i < specialized->child_count; i++) {
        ASTNode *param = specialized->children[i];
        if (param->type == AST_PARAM &&
            (param->declared_type == COBRA_TYPE_GENERIC_PARAM || !param->canonical_type)) {
            ir_error(ctx, param, "generic specialization left an unresolved parameter type");
        }
    }
    ast_add_child(ctx->root, specialized);
    return specialized;
}

static ASTNode *find_struct_declaration(IRContext *ctx, const char *name) {
    if (!ctx || !ctx->root || !name || !name[0]) return NULL;
    for (size_t i = 0; i < ctx->root->child_count; i++) {
        ASTNode *node = ctx->root->children[i];
        if (node->type == AST_STRUCT_DECL && strcmp(node->name, name) == 0)
            return node;
    }
    return NULL;
}

static const char *generic_struct_suffix(const CobraType *argument) {
    if (!argument) return "unknown";
    return argument->name[0] && argument->kind == COBRA_TYPE_ENUM
        ? argument->name : cobra_type_kind_name(argument->kind);
}

/* Materialize an immutable generic struct as an ordinary declaration before IR
   registration. This keeps the existing by-value struct lowering unchanged:
   after substitution, codegen sees only a finalized named struct with a real
   canonical layout and never needs to understand generic placeholders. */
static const CobraType *specialize_struct_reference(IRContext *ctx,
                                                     const CobraType *requested,
                                                     const CobraType *parameter,
                                                     const CobraType *argument,
                                                     ASTNode *use_site) {
    if (!ctx || !requested || requested->kind != COBRA_TYPE_STRUCT ||
        requested->generic_arg_count == 0) return requested;
    if (requested->generic_arg_count != 1 ||
        requested->ownership != COBRA_OWNERSHIP_VALUE ||
        requested->mutability != COBRA_MUTABILITY_DEFAULT ||
        requested->region_id != -1) {
        ir_error(ctx, use_site, "generic struct values must be immutable scalar by-value types");
        return NULL;
    }

    ASTNode *template_decl = find_struct_declaration(ctx, requested->name);
    if (!template_decl || template_decl->generic_param_count != 1 ||
        !template_decl->generic_param_types[0]) {
        ir_error(ctx, use_site, "generic struct template is not declared with one type parameter");
        return NULL;
    }
    const CobraType *requested_argument = requested->generic_args[0];
    if (parameter && requested_argument == parameter) requested_argument = argument;
    if (!generic_scalar_argument(requested_argument)) {
        ir_error(ctx, use_site, "generic struct arguments must be scalar types");
        return NULL;
    }
    const CobraType *template_type = cobra_type_struct_layout(
        ctx->canonical_arena, ctx->root, template_decl->name);
    if (!template_type) {
        ir_error(ctx, use_site, "generic struct template has an invalid layout");
        return NULL;
    }

    char specialized_name[COBRA_MAX_IDENT_LEN];
    snprintf(specialized_name, sizeof(specialized_name), "%.46s__%.15s",
             template_decl->name, generic_struct_suffix(requested_argument));
    CobraTypeBinding binding = {template_decl->generic_param_types[0], requested_argument};
    const CobraType *specialized = cobra_type_substitute(
        ctx->canonical_arena, template_type, &binding, 1, specialized_name);
    if (!specialized) {
        const char *reason = ctx->canonical_arena->error[0]
            ? ctx->canonical_arena->error : "generic struct specialization is unsupported";
        ir_error(ctx, use_site, reason);
        return NULL;
    }

    /* Add one synthetic, non-generic declaration for the specialized layout.
       It is intentionally an AST declaration rather than a side table entry,
       so canonical field lookup, IR registration, and codegen all share the
       existing declaration-driven path. */
    ASTNode *special_decl = find_struct_declaration(ctx, specialized->name);
    if (!special_decl) {
        special_decl = ast_create_node(AST_STRUCT_DECL, specialized->name);
        if (!special_decl) {
            ir_error(ctx, use_site, "could not allocate generic struct specialization declaration");
            return NULL;
        }
        special_decl->declared_type = COBRA_TYPE_STRUCT;
        special_decl->canonical_type = specialized;
        special_decl->source_line = use_site ? use_site->source_line : template_decl->source_line;
        special_decl->source_col = use_site ? use_site->source_col : template_decl->source_col;
        snprintf(special_decl->source_file, sizeof(special_decl->source_file), "%.511s",
                 template_decl->source_file);
        for (size_t i = 0; i < specialized->field_count; i++) {
            const CobraTypeField *field_type = &specialized->fields[i];
            ASTNode *field = ast_create_node(AST_PARAM, field_type->name);
            if (!field) {
                ast_free(special_decl);
                ir_error(ctx, use_site, "could not allocate generic struct field declaration");
                return NULL;
            }
            field->declared_type = field_type->type->kind;
            field->canonical_type = field_type->type;
            ast_add_child(special_decl, field);
        }
        ast_add_child(ctx->root, special_decl);
    }
    return specialized;
}

static void specialize_struct_types_in_tree(IRContext *ctx, ASTNode *node) {
    if (!ctx || !node) return;
    /* A generic declaration is a template, not a use site. Its placeholder
       fields remain unresolved until a concrete Box[T] reference is seen. */
    if (node->type == AST_STRUCT_DECL && node->generic_param_count > 0) return;
    if (node->canonical_type && node->canonical_type->kind == COBRA_TYPE_STRUCT &&
        node->canonical_type->generic_arg_count > 0 &&
        !node->canonical_type->finalized) {
        const CobraType *specialized = specialize_struct_reference(ctx,
                                                                    node->canonical_type,
                                                                    NULL, NULL, node);
        if (specialized) {
            node->canonical_type = specialized;
            node->declared_type = COBRA_TYPE_STRUCT;
            node->value_type = node->type == AST_PARAM || node->type == AST_VAR_DECL
                ? COBRA_TYPE_STRUCT : node->value_type;
        }
    }
    for (size_t i = 0; i < node->child_count; i++)
        specialize_struct_types_in_tree(ctx, node->children[i]);
}

static bool canonical_call_compatible(const CobraType *expected, const CobraType *actual) {
    if (!expected || !actual) return true;
    if (cobra_type_equal(expected, actual)) return true;
    if (expected->kind == COBRA_TYPE_SLICE || expected->kind == COBRA_TYPE_SLICE_F32 ||
        expected->kind == COBRA_TYPE_SLICE_U8) {
        const CobraType *expected_element = cobra_type_element(expected);
        const CobraType *actual_element = cobra_type_element(actual);
        if (!expected_element || !actual_element ||
            expected_element->kind != actual_element->kind) return false;
        if (actual->kind == COBRA_TYPE_ARRAY) {
            return expected->mutability != COBRA_MUTABILITY_OUT;
        }
        if (actual->kind == COBRA_TYPE_SLICE || actual->kind == COBRA_TYPE_SLICE_F32 ||
            actual->kind == COBRA_TYPE_SLICE_U8) {
            /* A writable view may be borrowed by readonly code. An out
               parameter still rejects a readonly source, but does not require
               the caller's local to carry an out qualifier. */
            return expected->mutability != COBRA_MUTABILITY_OUT ||
                   actual->mutability != COBRA_MUTABILITY_READONLY;
        }
    }
    if (expected->mutability == COBRA_MUTABILITY_OUT &&
        actual->kind == expected->kind &&
        strcmp(expected->name, actual->name) == 0 &&
        actual->mutability != COBRA_MUTABILITY_READONLY) return true;
    return is_integer(expected->kind) && is_integer(actual->kind);
}

/* IR ownership checks use the canonical parameter contract. Flow-sensitive
   rebinding is tracked separately on IRLocal::flow_mutability. */
static bool param_is_out(const ASTNode *param) {
    return param && param->canonical_type &&
           param->canonical_type->mutability == COBRA_MUTABILITY_OUT;
}

/* Map a canonical field mutability to the historical 0/1/2 qualifier the
   direct emitter and IR flow state use. */
static int qualifier_from_mutability(CobraMutabilityKind mutability) {
    if (mutability == COBRA_MUTABILITY_READONLY) return 1;
    if (mutability == COBRA_MUTABILITY_OUT) return 2;
    return 0;
}

/* Resolve a struct field's alias contract from canonical metadata. The
   descriptor is authoritative; IR flow state records only later rebinding. */
static const CobraType *canonical_field_value(IRContext *ctx, const char *struct_name,
                                               const char *field_name) {
    if (!ctx || !ctx->canonical_arena || !struct_name || !field_name) return NULL;
    const CobraType *st = cobra_type_struct_layout(ctx->canonical_arena, ctx->root, struct_name);
    if (!st) return NULL;
    for (size_t i = 0; i < st->field_count; i++) {
        const CobraTypeField *field = &st->fields[i];
        if (strcmp(field->name, field_name) != 0) continue;
        const CobraType *base = field->type;
        return cobra_type_make(ctx->canonical_arena, base->kind,
                               base->name[0] ? base->name : NULL,
                               cobra_type_element(base), cobra_type_error(base),
                               cobra_type_key(base), cobra_type_value(base),
                               field->ownership, field->mutability,
                               field->region_id);
    }
    return NULL;
}

static int canonical_field_qualifier(IRContext *ctx, const char *struct_name,
                                     const char *field_name) {
    if (!ctx || !struct_name || !struct_name[0] || !field_name || !field_name[0])
        return 0;
    const CobraType *st = cobra_type_struct_layout(ctx->canonical_arena, ctx->root, struct_name);
    if (!st) return 0;
    for (size_t i = 0; i < st->field_count; i++) {
        if (strcmp(st->fields[i].name, field_name) == 0)
            return qualifier_from_mutability(st->fields[i].mutability);
    }
    return 0;
}

static bool node_is_typed_declaration(const ASTNode *node) {
    if (!node) return false;
    return (node->type == AST_PARAM || node->type == AST_VAR_DECL ||
            node->type == AST_HEAP_DECL || node->type == AST_FUNCTION) &&
           node->declared_type != COBRA_TYPE_UNTYPED;
}

static void check_canonical_tree(IRContext *ctx, ASTNode *node) {
    if (!ctx || !node) return;
    if (node_is_typed_declaration(node) && !node->canonical_type) {
        /* Every explicitly typed declaration must carry canonical metadata
           after inference. Losing it is a compiler invariant violation, not a
           user error, so it aborts rather than falling back to legacy fields. */
        fprintf(stderr,
                "%s:%d:%d: internal error: typed declaration '%s' is missing canonical type metadata\n",
                node->source_file[0] ? node->source_file : "<source>",
                node->source_line > 0 ? node->source_line : 1,
                node->source_col > 0 ? node->source_col : 1,
                node->name[0] ? node->name : "<anonymous>");
        exit(EXIT_FAILURE);
    }
    if (node->canonical_type && node->declared_type != COBRA_TYPE_UNTYPED) {
        CobraTypeKind expected = node->declared_type;
        bool enum_promotion = expected == COBRA_TYPE_STRUCT &&
                              node->canonical_type->kind == COBRA_TYPE_ENUM;
        if (node->canonical_type->kind != expected && !enum_promotion) {
            fprintf(stderr,
                    "%s:%d:%d: internal error: canonical declaration kind mismatch for '%s' (expected %s, got %s)\n",
                    node->source_file[0] ? node->source_file : "<source>",
                    node->source_line > 0 ? node->source_line : 1,
                    node->source_col > 0 ? node->source_col : 1,
                    node->name[0] ? node->name : "<anonymous>",
                    cobra_type_kind_name(expected),
                    cobra_type_kind_name(node->canonical_type->kind));
            exit(EXIT_FAILURE);
        }
    }
    for (size_t i = 0; i < node->child_count; i++) {
        check_canonical_tree(ctx, node->children[i]);
    }
}

static bool is_imported_function(IRContext *ctx, const char *name) {
    if (!ctx->root) return false;
    for (size_t i = 0; i < ctx->root->child_count; i++) {
        ASTNode *decl = ctx->root->children[i];
        if (decl->type != AST_IMPORT_DECL) continue;
        for (size_t j = 0; j < decl->child_count; j++) {
            ASTNode *ref = decl->children[j];
            if (ref->type == AST_VAR_REF && strcmp(ref->name, name) == 0) return true;
        }
    }
    return false;
}

/* Register the shared layout contract once from the canonical descriptor.
   Codegen reads the same canonical sizes and offsets, so the IR table is a
   convenience mirror of cobra_type_struct_layout, not a second layout. */
static void register_struct_decl(IRContext *ctx, ASTNode *decl, bool report_errors) {
    if (!ctx || !decl) return;
    /* Generic declarations are templates. Register only materialized
       specializations so no unresolved placeholder can influence ABI checks. */
    if (decl->generic_param_count > 0) return;
    if (ctx->struct_count >= COBRA_MAX_STRUCTS) {
        if (report_errors) ir_error(ctx, decl, "too many struct types");
        return;
    }
    IRStruct *existing = find_struct(ctx, decl->name);
    if (existing) {
        if (report_errors) {
            if (existing->invalid_layout && existing->layout_error[0]) ir_error(ctx, decl, existing->layout_error);
            else if (!existing->invalid_layout) ir_error(ctx, decl, "duplicate struct declaration");
        }
        return;
    }

    const CobraType *canonical = ctx->canonical_arena
        ? cobra_type_struct_layout(ctx->canonical_arena, ctx->root, decl->name)
        : NULL;
    if (!canonical) {
        /* Keep a marker so later parameter checks do not replace the useful
           cycle or ownership diagnostic with a misleading unknown-type error. */
        IRStruct *invalid = &ctx->structs[ctx->struct_count++];
        memset(invalid, 0, sizeof(*invalid));
        snprintf(invalid->name, sizeof(invalid->name), "%.63s", decl->name);
        invalid->invalid_layout = true;
        const char *reason = (ctx->canonical_arena && ctx->canonical_arena->error[0])
            ? ctx->canonical_arena->error : "invalid struct layout";
        snprintf(invalid->layout_error, sizeof(invalid->layout_error), "%.127s", reason);
        if (report_errors) ir_error(ctx, decl, invalid->layout_error);
        return;
    }

    IRStruct *type = &ctx->structs[ctx->struct_count++];
    memset(type, 0, sizeof(*type));
    snprintf(type->name, sizeof(type->name), "%.63s",
             canonical->name[0] ? canonical->name : decl->name);
    type->field_count = (int)canonical->field_count;
    type->total_size = (int)canonical->size;
    type->invalid_layout = false;
    for (size_t i = 0; i < canonical->field_count; i++) {
        const CobraTypeField *field = &canonical->fields[i];
        type->fields[i].type = field->type->kind;
        type->fields[i].offset = (int)field->offset;
        if (field->ownership == COBRA_OWNERSHIP_OWNED) {
            type->fields[i].ownership = COBRA_FIELD_OWNED_VALUE;
            type->has_owned_fields = true;
        } else if (field->ownership == COBRA_OWNERSHIP_BORROWED ||
                   field->mutability != COBRA_MUTABILITY_DEFAULT) {
            type->fields[i].ownership = COBRA_FIELD_BORROWED_VIEW;
            type->has_borrowed_fields = true;
        } else {
            type->fields[i].ownership = COBRA_FIELD_SCALAR;
        }
        snprintf(type->fields[i].name, sizeof(type->fields[i].name), "%.63s", field->name);
        if (field->type->name[0])
            snprintf(type->fields[i].type_name, sizeof(type->fields[i].type_name), "%.63s", field->type->name);
    }
}

static bool is_source_module_alias(IRContext *ctx, const char *alias) {
    if (!ctx->root || !alias || !*alias) return false;
    for (size_t i = 0; i < ctx->root->child_count; i++) {
        ASTNode *decl = ctx->root->children[i];
        if (decl->type == AST_IMPORT_DECL && decl->source_import &&
            strcmp(decl->module_alias, alias) == 0) return true;
    }
    return false;
}

static bool function_visible_from(ASTNode *caller, ASTNode *callee) {
    if (!callee || !callee->has_visibility || callee->is_public) return true;
    if (!caller || !caller->source_file[0] || !callee->source_file[0]) return true;
    return strcmp(caller->source_file, callee->source_file) == 0;
}

static bool is_active_region(IRContext *ctx, const char *name) {
    if (!name || !*name) return false;
    for (int i = ctx->region_depth - 1; i >= 0; i--) {
        if (strcmp(ctx->regions[i], name) == 0) return true;
    }
    return false;
}

static int struct_field_index(IRStruct *type, const char *name) {
    if (!type || !name) return -1;
    for (int i = 0; i < type->field_count; i++) {
        if (strcmp(type->fields[i].name, name) == 0) return i;
    }
    return -1;
}

static int local_index(IRContext *ctx, const IRLocal *local) {
    if (!ctx || !local) return -1;
    for (size_t i = 0; i < ctx->count; i++) if (&ctx->locals[i] == local) return (int)i;
    return -1;
}

static bool local_has_live_struct_borrow(IRContext *ctx, const char *owner_name) {
    if (!ctx || !owner_name || !*owner_name) return false;
    IRLocal *owner = find_local_entry(ctx, owner_name);
    int owner_index = local_index(ctx, owner);
    if (owner_index < 0) return false;
    for (size_t i = 0; i < ctx->count; i++) {
        IRLocal *local = &ctx->locals[i];
        for (int field = 0; field < COBRA_MAX_STRUCT_FIELDS; field++) {
            if (local->struct_field_borrow_owner[field] == owner_index && !local->freed) return true;
        }
    }
    return false;
}

static void expire_region_borrows(IRContext *ctx, int region_id) {
    if (!ctx || region_id < 0) return;
    for (size_t i = 0; i < ctx->count; i++) {
        IRLocal *local = &ctx->locals[i];
        if (local->region_id == region_id) local->region_expired = true;
        for (int field = 0; field < COBRA_MAX_STRUCT_FIELDS; field++) {
            if (local->struct_field_region_id[field] == region_id) local->region_expired = true;
        }
    }
}

static void copy_struct_borrow_metadata(IRLocal *destination, const IRLocal *source) {
    if (!destination || !source) return;
    memcpy(destination->struct_field_borrow_owner, source->struct_field_borrow_owner,
           sizeof(destination->struct_field_borrow_owner));
    memcpy(destination->struct_field_region_id, source->struct_field_region_id,
           sizeof(destination->struct_field_region_id));
    destination->struct_field_initialized = source->struct_field_initialized;
    destination->region_expired = source->region_expired;
}

static void set_struct_field_borrow(IRContext *ctx, IRLocal *base, int field_index,
                                    ASTNode *source_node) {
    if (!ctx || !base || field_index < 0 || field_index >= COBRA_MAX_STRUCT_FIELDS || !source_node) return;
    base->struct_field_borrow_owner[field_index] = -1;
    base->struct_field_region_id[field_index] = -1;
    if (source_node->type == AST_VAR_REF) {
        IRLocal *source = find_local_entry(ctx, source_node->name);
        if (!source) return;
        IRLocal *owner = source;
        if (source->borrowed_from[0]) {
            IRLocal *ultimate = find_local_entry(ctx, source->borrowed_from);
            if (ultimate) owner = ultimate;
        }
        base->struct_field_borrow_owner[field_index] = local_index(ctx, owner);
        base->struct_field_region_id[field_index] = source->region_id;
        return;
    }
    if (source_node->type == AST_MEMBER_ACCESS && source_node->child_count > 0 &&
        source_node->children[0]->type == AST_VAR_REF) {
        IRLocal *source_base = find_local_entry(ctx, source_node->children[0]->name);
        IRStruct *source_type = source_base ? find_struct(ctx, source_base->type_name) : NULL;
        int source_field = struct_field_index(source_type, source_node->secondary_name);
        if (source_base && source_field >= 0) {
            base->struct_field_borrow_owner[field_index] =
                source_base->struct_field_borrow_owner[source_field] >= 0 ?
                source_base->struct_field_borrow_owner[source_field] : local_index(ctx, source_base);
            base->struct_field_region_id[field_index] = source_base->struct_field_region_id[source_field];
        }
    }
}

static void propagate_member_view_metadata(IRContext *ctx, IRLocal *view, ASTNode *member) {
    if (!ctx || !view || !member || member->type != AST_MEMBER_ACCESS ||
        member->child_count == 0 || member->children[0]->type != AST_VAR_REF) return;
    IRLocal *source_base = find_local_entry(ctx, member->children[0]->name);
    IRStruct *source_type = source_base ? find_struct(ctx, source_base->type_name) : NULL;
    int source_field = struct_field_index(source_type, member->secondary_name);
    if (!source_base || source_field < 0) return;
    int owner_index = source_base->struct_field_borrow_owner[source_field];
    if (owner_index < 0) owner_index = local_index(ctx, source_base);
    IRLocal *owner = owner_index >= 0 && owner_index < (int)ctx->count ?
                     &ctx->locals[owner_index] : source_base;
    snprintf(view->borrowed_from, sizeof(view->borrowed_from), "%.63s", owner->name);
    view->region_id = source_base->struct_field_region_id[source_field];
    if (source_base->region_expired || view->region_id < 0) {
        view->region_expired = source_base->region_expired;
    }
}

static bool is_region_alloc_call(IRContext *ctx, ASTNode *node) {
    return node && node->type == AST_FUNC_CALL && node->qualifier[0] != '\0' &&
           (strcmp(node->name, "alloc_i64") == 0 ||
            strcmp(node->name, "alloc_f32") == 0 ||
            strcmp(node->name, "alloc_u8") == 0) &&
           is_active_region(ctx, node->qualifier);
}

static bool scan_owned_string_returns(ASTNode *node, bool *saw_return) {
    if (!node) return true;
    if (node->type == AST_RETURN) {
        *saw_return = true;
        if (node->child_count == 0) return false;
        ASTNode *value = node->children[0];
        /* This proof is intentionally syntax-driven so it is independent of
           source declaration order. The enclosing function's declared string
           return type and normal type validation reject non-string '+' returns. */
        if (!((value->type == AST_FUNC_CALL && strcmp(value->name, "concat") == 0) ||
              (value->type == AST_BINARY_OP && strcmp(value->name, "+") == 0))) return false;
    }
    for (size_t i = 0; i < node->child_count; i++) {
        if (!scan_owned_string_returns(node->children[i], saw_return)) return false;
    }
    return true;
}

static bool function_returns_owned_string(IRContext *ctx, ASTNode *function) {
    (void)ctx;
    if (!function || function->declared_type != COBRA_TYPE_STRING) return false;
    bool saw_return = false;
    return scan_owned_string_returns(function, &saw_return) && saw_return;
}

static bool expression_is_fresh_string(ASTNode *node, IRContext *ctx) {
    if (!node) return false;
    if (node->type == AST_BINARY_OP && strcmp(node->name, "+") == 0 &&
        node->value_type == COBRA_TYPE_STRING) return true;
    if (node->type != AST_FUNC_CALL) return false;
    if (strcmp(node->name, "concat") == 0) return true;
    ASTNode *function = find_function(ctx, node->name);
    return function && function_returns_owned_string(ctx, function);
}

static void set_scalar_canonical(IRContext *ctx, ASTNode *node, CobraTypeKind kind) {
    if (!ctx || !node || !ctx->canonical_arena || node->canonical_type) return;
    node->canonical_type = cobra_type_make(ctx->canonical_arena, kind, NULL,
                                           NULL, NULL, NULL, NULL,
                                           COBRA_OWNERSHIP_VALUE,
                                           COBRA_MUTABILITY_DEFAULT, -1);
}

static CobraTypeKind infer_expr(ASTNode *node, IRContext *ctx) {
    if (!node) return COBRA_TYPE_UNKNOWN;

    switch (node->type) {
        case AST_INT_LITERAL:
            node->value_type = COBRA_TYPE_I64;
            set_scalar_canonical(ctx, node, node->value_type);
            return node->value_type;
        case AST_FLOAT_LITERAL:
            node->value_type = COBRA_TYPE_F32;
            set_scalar_canonical(ctx, node, node->value_type);
            return node->value_type;
        case AST_STRING_LITERAL:
            node->value_type = COBRA_TYPE_STRING;
            set_scalar_canonical(ctx, node, node->value_type);
            return node->value_type;
        case AST_BOOL_LITERAL:
            node->value_type = COBRA_TYPE_BOOL;
            set_scalar_canonical(ctx, node, node->value_type);
            return node->value_type;
        case AST_NONE_LITERAL:
            node->value_type = COBRA_TYPE_NONE;
            return node->value_type;
        case AST_MEMBER_ACCESS: {
            /* Qualified enum variants are compile-time integer constants, not
               runtime member objects. */
            if (node->child_count > 0 && node->children[0]->type == AST_VAR_REF) {
                IREnum *enum_decl = find_enum(ctx, node->children[0]->name);
                if (enum_decl) {
                    int value = find_enum_variant(ctx, enum_decl->name, node->secondary_name);
                    if (value == INT_MIN) {
                        ir_error(ctx, node, "unknown enum variant");
                        return COBRA_TYPE_UNKNOWN;
                    }
                    node->value_type = COBRA_TYPE_ENUM;
                    node->int_val = value;
                    node->canonical_type = cobra_type_make(ctx->canonical_arena, COBRA_TYPE_ENUM,
                                                       enum_decl->name, NULL, NULL, NULL, NULL,
                                                       COBRA_OWNERSHIP_VALUE,
                                                       COBRA_MUTABILITY_DEFAULT, -1);
                    return node->value_type;
                }
            }
            /* The base may be a local struct or another member that resolves
               to a nested by-value struct. Each node records its own offset,
               while type_name carries the next layout lookup name. */
            if (node->child_count == 0) {
                ir_error(ctx, node, "member access requires a struct value");
                return COBRA_TYPE_UNKNOWN;
            }
            ASTNode *base_expr = node->children[0];
            CobraTypeKind base_type = infer_expr(base_expr, ctx);
            if (base_type != COBRA_TYPE_STRUCT) {
                ir_error(ctx, node, "member access requires a struct value");
                return COBRA_TYPE_UNKNOWN;
            }
            const char *base_type_name = cobra_type_node_name(base_expr);
            IRLocal *base = base_expr->type == AST_VAR_REF ?
                            find_local_entry(ctx, base_expr->name) : NULL;
            if (base && base->region_expired) {
                ir_error(ctx, node, "struct byte-view is used after its region ended");
            }
            IRStruct *type = find_struct(ctx, base_type_name);
            if (!type) {
                ir_error(ctx, node, "struct type is not registered");
                return COBRA_TYPE_UNKNOWN;
            }
            for (int i = 0; i < type->field_count; i++) {
                if (strcmp(type->fields[i].name, node->secondary_name) == 0) {
                    node->canonical_type = canonical_field_value(ctx, base_type_name,
                                                                  node->secondary_name);
                    node->value_type = type->fields[i].type;
                    return node->value_type;
                }
            }
            char message[180];
            snprintf(message, sizeof(message), "struct '%s' has no field '%s'", base_type_name, node->secondary_name);
            ir_error(ctx, node, message);
            return COBRA_TYPE_UNKNOWN;
        }
        case AST_ARRAY_LITERAL: {
            node->value_type = COBRA_TYPE_ARRAY;
            CobraTypeKind literal_element = COBRA_TYPE_UNTYPED;
            for (size_t i = 0; i < node->child_count; i++) {
                CobraTypeKind element = infer_expr(node->children[i], ctx);
                if (i == 0) {
                    literal_element = element;
                } else if (element != COBRA_TYPE_UNKNOWN &&
                           literal_element != COBRA_TYPE_UNKNOWN &&
                           element != literal_element &&
                           !(is_integer(element) && is_integer(literal_element))) {
                    ir_error(ctx, node, "array literal elements must have one compatible type");
                }
            }
            canonical_inferred_type(ctx, node);
            return node->value_type;
        }
        case AST_DICT_LITERAL:
            node->value_type = COBRA_TYPE_DICT;
            for (size_t i = 0; i < node->child_count; i++) {
                if (node->children[i]->child_count == 1) {
                    CobraTypeKind value = infer_expr(node->children[i]->children[0], ctx);
                    if (value != COBRA_TYPE_I64 && value != COBRA_TYPE_UNKNOWN)
                        ir_error(ctx, node, "dict values currently require i64");
                }
            }
            node->canonical_type = cobra_type_make(ctx->canonical_arena, COBRA_TYPE_DICT, NULL,
                                                   NULL, NULL,
                                                   cobra_type_make(ctx->canonical_arena, COBRA_TYPE_STRING,
                                                                   NULL, NULL, NULL, NULL, NULL,
                                                                   COBRA_OWNERSHIP_VALUE,
                                                                   COBRA_MUTABILITY_DEFAULT, -1),
                                                   cobra_type_make(ctx->canonical_arena, COBRA_TYPE_I64,
                                                                   NULL, NULL, NULL, NULL, NULL,
                                                                   COBRA_OWNERSHIP_VALUE,
                                                                   COBRA_MUTABILITY_DEFAULT, -1),
                                                   COBRA_OWNERSHIP_VALUE,
                                                   COBRA_MUTABILITY_DEFAULT, -1);
            return node->value_type;
        case AST_VAR_REF: {
            CobraTypeKind type = COBRA_TYPE_UNKNOWN;
            IRLocal *local = find_local_entry(ctx, node->name);
            if (!local) {
                char message[160];
                snprintf(message, sizeof(message), "undefined variable '%s'", node->name);
                ir_error(ctx, node, message);
            } else {
                type = local->type;
                node->canonical_type = local->canonical_type;
                if (local->freed || local->moved) {
                    char message[180];
                    snprintf(message, sizeof(message), "use of %s value '%s'", local->moved ? "moved" : "freed", node->name);
                    ir_error(ctx, node, message);
                } else if (local->region_expired) {
                    char message[200];
                    snprintf(message, sizeof(message), "use of region-backed value '%s' after its region ended", node->name);
                    ir_error(ctx, node, message);
                }
            }
            node->value_type = type;
            return type;
        }
        case AST_COMPTIME_EXPR:
            if (node->child_count == 0) return COBRA_TYPE_UNKNOWN;
            node->value_type = infer_expr(node->children[0], ctx);
            return node->value_type;
        case AST_BINARY_OP: {
            CobraTypeKind left = infer_expr(node->children[0], ctx);
            CobraTypeKind right = infer_expr(node->children[1], ctx);
            if (strcmp(node->name, "/") == 0 && expression_is_const_zero(node->children[1])) {
                ir_error(ctx, node, "division by zero");
            }
            if (left == COBRA_TYPE_STRING || right == COBRA_TYPE_STRING) {
                if ((strcmp(node->name, "+") == 0 && left == COBRA_TYPE_STRING && right == COBRA_TYPE_STRING) ||
                    (is_string_comparison(node->name) && left == COBRA_TYPE_STRING && right == COBRA_TYPE_STRING)) {
                    node->value_type = strcmp(node->name, "+") == 0 ? COBRA_TYPE_STRING : COBRA_TYPE_I64;
                    node->fresh_string_result = strcmp(node->name, "+") == 0;
                    return node->value_type;
                }
                ir_error(ctx, node, "string operators require two strings and support '+' or comparison");
                return COBRA_TYPE_UNKNOWN;
            }
            if (left == COBRA_TYPE_UNTYPED) left = right;
            if (right == COBRA_TYPE_UNTYPED) right = left;
            /* Mixed int/float arithmetic coerces the integer operand to f32,
               matching the numeric semantics model math expects. */
            if (left == COBRA_TYPE_F32 && is_integer(right)) right = COBRA_TYPE_F32;
            else if (right == COBRA_TYPE_F32 && is_integer(left)) left = COBRA_TYPE_F32;
            /* bool is an integer-ordered scalar: comparisons with integer
               literals/values are allowed and produce an i64 result. */
            bool bool_int_pair = (left == COBRA_TYPE_BOOL && (is_integer(right) || right == COBRA_TYPE_BOOL)) ||
                                 (right == COBRA_TYPE_BOOL && (is_integer(left) || left == COBRA_TYPE_BOOL));
            if (left != right && !(is_integer(left) && is_integer(right)) && !bool_int_pair) {
                char message[180];
                snprintf(message, sizeof(message), "operator '%s' cannot combine %s and %s",
                         node->name, type_name(left), type_name(right));
                ir_error(ctx, node, message);
                return COBRA_TYPE_UNKNOWN;
            }
            bool comparison = strcmp(node->name, "==") == 0 || strcmp(node->name, "!=") == 0 ||
                              strcmp(node->name, "<") == 0 || strcmp(node->name, ">") == 0 ||
                              strcmp(node->name, "<=") == 0 || strcmp(node->name, ">=") == 0;
            node->value_type = comparison ? COBRA_TYPE_I64 :
                (left == COBRA_TYPE_UNTYPED ? COBRA_TYPE_I64 : left);
            return node->value_type;
        }
        case AST_LEN_EXPR: {
            CobraTypeKind target_type = node->child_count > 0 ? infer_expr(node->children[0], ctx) : COBRA_TYPE_UNKNOWN;
            if (target_type != COBRA_TYPE_ARRAY && target_type != COBRA_TYPE_SLICE &&
                target_type != COBRA_TYPE_SLICE_F32 && target_type != COBRA_TYPE_SLICE_U8 &&
                target_type != COBRA_TYPE_STRING &&
                target_type != COBRA_TYPE_LIST && target_type != COBRA_TYPE_DICT &&
                target_type != COBRA_TYPE_UNKNOWN) {
                ir_error(ctx, node, "len requires an array, slice, or string");
            }
            node->value_type = COBRA_TYPE_I64;
            return node->value_type;
        }
        case AST_MEMBERSHIP: {
            if (node->child_count != 2) {
                ir_error(ctx, node, "membership requires (element, collection)");
            } else {
                if (node->children[1]->type != AST_VAR_REF) {
                    ir_error(ctx, node, "membership requires a named collection variable");
                }
                CobraTypeKind element = infer_expr(node->children[0], ctx);
                CobraTypeKind container = infer_expr(node->children[1], ctx);
                if (container == COBRA_TYPE_DICT) {
                    if (element != COBRA_TYPE_STRING && element != COBRA_TYPE_UNKNOWN)
                        ir_error(ctx, node, "dict membership requires a string key");
                } else if                (container == COBRA_TYPE_LIST || container == COBRA_TYPE_ARRAY ||
                           container == COBRA_TYPE_SLICE || container == COBRA_TYPE_SLICE_U8 ||
                           container == COBRA_TYPE_UNKNOWN) {

                    if (element != COBRA_TYPE_UNKNOWN && !is_integer(element) && element != COBRA_TYPE_F32)
                        ir_error(ctx, node, "collection membership requires a scalar value");
                } else {
                    ir_error(ctx, node, "membership requires a list, array, slice, or dict");
                }
            }
            node->value_type = COBRA_TYPE_I64;
            return node->value_type;
        }
        case AST_COMPREHENSION: {
            /* [expr for target in source (if guard)?] lowers to a native list
               build: the source must be an iterable collection and the target
               becomes an element local while the output and guard validate. */
            if (node->child_count < 2) {
                ir_error(ctx, node, "comprehension requires an output expression and a source");
                return COBRA_TYPE_UNKNOWN;
            }
            CobraTypeKind source = infer_expr(node->children[1], ctx);
            if (source != COBRA_TYPE_ARRAY && source != COBRA_TYPE_SLICE &&
                source != COBRA_TYPE_SLICE_F32 && source != COBRA_TYPE_SLICE_U8 &&
                source != COBRA_TYPE_LIST &&
                source != COBRA_TYPE_UNKNOWN) {
                ir_error(ctx, node, "comprehension source must be an array, slice, or list");
            }
            CobraTypeKind element_type = COBRA_TYPE_I64;
            if (node->children[1]->type == AST_VAR_REF) {
                IRLocal *src = find_local_entry(ctx, node->children[1]->name);
                if (src && ((src->type == COBRA_TYPE_LIST || src->type == COBRA_TYPE_ARRAY) &&
                            src->element_type == COBRA_TYPE_F32))
                    element_type = COBRA_TYPE_F32;
                else if (src && src->type == COBRA_TYPE_SLICE_F32)
                    element_type = COBRA_TYPE_F32;
            }
            /* The target is scoped to the comprehension, exactly like a loop
               iterator, so it never leaks into the enclosing function. */
            IRLocal saved_comp_locals[128];
            size_t saved_comp_count = ctx->count;
            memcpy(saved_comp_locals, ctx->locals, sizeof(saved_comp_locals));
            if (node->name[0] != '\0' && !add_local(ctx, node->name, element_type, NULL)) {
                char message[160];
                snprintf(message, sizeof(message), "duplicate comprehension target '%s'", node->name);
                ir_error(ctx, node, message);
            }
            (void)infer_expr(node->children[0], ctx);
            if (node->child_count > 2) (void)infer_expr(node->children[2], ctx);
            memcpy(ctx->locals, saved_comp_locals, sizeof(saved_comp_locals));
            ctx->count = saved_comp_count;
            node->value_type = COBRA_TYPE_LIST;
            node->canonical_type = cobra_type_make(ctx->canonical_arena, COBRA_TYPE_LIST, NULL,
                                                   cobra_type_make(ctx->canonical_arena, element_type, NULL,
                                                                   NULL, NULL, NULL, NULL,
                                                                   COBRA_OWNERSHIP_VALUE,
                                                                   COBRA_MUTABILITY_DEFAULT, -1),
                                                   NULL, NULL, NULL,
                                                   COBRA_OWNERSHIP_VALUE,
                                                   COBRA_MUTABILITY_DEFAULT, -1);
            return node->value_type;
        }
        case AST_ARRAY_INDEX: {
            CobraTypeKind base_type = COBRA_TYPE_UNKNOWN;
            if (node->secondary_name[0] != '\0') {
                IRLocal *owner = find_local_entry(ctx, node->name);
                IRStruct *owner_type = owner ? find_struct(ctx, owner->type_name) : NULL;
                if (owner && owner->region_expired)
                    ir_error(ctx, node, "struct byte-view is used after its region ended");
                if (!owner || owner->type != COBRA_TYPE_STRUCT || !owner_type) {
                    ir_error(ctx, node, "struct byte-view index requires a struct value");
                } else {
                    for (int field_index = 0; field_index < owner_type->field_count; field_index++) {
                        if (strcmp(owner_type->fields[field_index].name, node->secondary_name) == 0) {
                            base_type = owner_type->fields[field_index].type;
                            (void)canonical_field_qualifier(ctx, owner->type_name, owner_type->fields[field_index].name);
                            break;
                        }
                    }
                    if (base_type != COBRA_TYPE_SLICE_U8)
                        ir_error(ctx, node, "only readonly or out []u8 struct fields are indexable");
                }
            } else if (!find_local(ctx, node->name, &base_type)) {
                char message[160];
                snprintf(message, sizeof(message), "undefined indexed value '%s'", node->name);
                ir_error(ctx, node, message);
            } else if (base_type != COBRA_TYPE_ARRAY && !is_slice_type(base_type) &&
                       base_type != COBRA_TYPE_LIST && base_type != COBRA_TYPE_DICT &&
                       base_type != COBRA_TYPE_UNKNOWN) {
                char message[160];
                snprintf(message, sizeof(message), "'%s' is not indexable", node->name);
                ir_error(ctx, node, message);
            }
            IRLocal *array_local = find_local_entry(ctx, node->name);
            if (array_local && array_local->freed) {
                char message[180];
                snprintf(message, sizeof(message), "indexing freed buffer '%s'", node->name);
                ir_error(ctx, node, message);
            } else if (array_local && array_local->region_expired) {
                char message[200];
                snprintf(message, sizeof(message), "indexing region-backed value '%s' after its region ended", node->name);
                ir_error(ctx, node, message);
            }
            if (node->child_count == 0 || node->child_count > COBRA_MAX_SHAPE_DIMS) {
                ir_error(ctx, node, "tensor indexing requires between one and eight indices");
            }
            for (size_t i = 0; i < node->child_count; i++) {
                CobraTypeKind index_type = infer_expr(node->children[i], ctx);
                if (base_type == COBRA_TYPE_DICT) {
                    if (index_type != COBRA_TYPE_STRING && index_type != COBRA_TYPE_UNKNOWN)
                        ir_error(ctx, node, "dict index must be a string");
                } else if (index_type != COBRA_TYPE_UNKNOWN && !is_integer(index_type)) {
                    ir_error(ctx, node, "collection index must be an integer");
                }
            }
            if (base_type == COBRA_TYPE_DICT && node->child_count != 1)
                ir_error(ctx, node, "dict indexing requires one string key");
            if (base_type == COBRA_TYPE_TENSOR_F32) {
                IRLocal *tensor = find_local_entry(ctx, node->name);
                if (tensor && tensor->shape_rank > 0 && node->child_count == 2 && tensor->shape_rank != 2) {
                    ir_error(ctx, node, "two-index access requires a rank-2 tensor");
                }
                if (tensor && tensor->shape_rank > 0 && node->child_count == 1 && tensor->shape_rank != 1 && tensor->shape_rank != 2) {
                    ir_error(ctx, node, "flat tensor access requires rank-1 or rank-2 storage");
                }
            }
            IRLocal *indexed_local = find_local_entry(ctx, node->name);
            CobraTypeKind indexed_element = indexed_local ? indexed_local->element_type : COBRA_TYPE_UNTYPED;
            node->value_type = (base_type == COBRA_TYPE_DICT) ? COBRA_TYPE_I64 :
                               (is_f32_buffer_type(base_type) || indexed_element == COBRA_TYPE_F32 ? COBRA_TYPE_F32 :
                                (indexed_element != COBRA_TYPE_UNTYPED ? indexed_element : COBRA_TYPE_I64));
            return node->value_type;
        }
        case AST_INDEX_ASSIGN: {
            CobraTypeKind base_type = COBRA_TYPE_UNKNOWN;
            if (node->secondary_name[0] != '\0') {
                IRLocal *owner = find_local_entry(ctx, node->name);
                IRStruct *owner_type = owner ? find_struct(ctx, owner->type_name) : NULL;
                if (owner && owner->region_expired)
                    ir_error(ctx, node, "struct byte-view is used after its region ended");
                if (!owner || owner->type != COBRA_TYPE_STRUCT || !owner_type) {
                    ir_error(ctx, node, "struct byte-view write requires a struct value");
                } else {
                    int field_qualifier = 0;
                    for (int field_index = 0; field_index < owner_type->field_count; field_index++) {
                        if (strcmp(owner_type->fields[field_index].name, node->secondary_name) == 0) {
                            base_type = owner_type->fields[field_index].type;
                            field_qualifier = canonical_field_qualifier(ctx, owner->type_name,
                                                                         owner_type->fields[field_index].name);
                            break;
                        }
                    }
                    if (base_type != COBRA_TYPE_SLICE &&
                        base_type != COBRA_TYPE_SLICE_F32 &&
                        base_type != COBRA_TYPE_SLICE_U8)
                        ir_error(ctx, node, "only slice struct fields are writable by index");
                    if (field_qualifier != 2)
                        ir_error(ctx, node, "cannot write readonly borrowed slice field");
                }
            } else if (!find_local(ctx, node->name, &base_type) ||
                (base_type != COBRA_TYPE_ARRAY && base_type != COBRA_TYPE_SLICE &&
                 base_type != COBRA_TYPE_SLICE_U8 && !is_f32_buffer_type(base_type) &&
                 base_type != COBRA_TYPE_LIST &&
                 base_type != COBRA_TYPE_DICT)) {
                ir_error(ctx, node, "writable indexing requires a list, dict, slice, or tensor buffer");
            }
            IRLocal *written = find_local_entry(ctx, node->name);
            if (written && written->flow_mutability == COBRA_MUTABILITY_READONLY) {
                ir_error(ctx, node, "readonly buffer cannot be written");
            }
            size_t index_count = node->child_count > 0 ? node->child_count - 1 : 0;
            if (base_type == COBRA_TYPE_TENSOR_F32 && (index_count < 1 || index_count > 2)) {
                ir_error(ctx, node, "native tensor indexing currently supports rank-1 and rank-2 accesses");
            }
            for (size_t i = 0; i < index_count; i++) {
                CobraTypeKind index_type = infer_expr(node->children[i], ctx);
                if (base_type == COBRA_TYPE_DICT) {
                    if (index_type != COBRA_TYPE_STRING && index_type != COBRA_TYPE_UNKNOWN)
                        ir_error(ctx, node, "dict index must be a string");
                } else if (!is_integer(index_type)) {
                    ir_error(ctx, node, "collection index must be an integer");
                }
            }
            if (base_type == COBRA_TYPE_DICT && index_count != 1)
                ir_error(ctx, node, "dict assignment requires one string key");
            if (base_type == COBRA_TYPE_TENSOR_F32) {
                IRLocal *tensor = find_local_entry(ctx, node->name);
                if (tensor && tensor->shape_rank > 0 && index_count == 2 && tensor->shape_rank != 2) {
                    ir_error(ctx, node, "two-index assignment requires a rank-2 tensor");
                }
            }
            if (node->child_count > 0) {
                CobraTypeKind value_type = infer_expr(node->children[node->child_count - 1], ctx);
                /* Integer literals and variables coerce to f32 on assignment,
                   matching the mixed-numeric semantics of arithmetic. */
                if (base_type == COBRA_TYPE_DICT) {
                    if (value_type != COBRA_TYPE_I64 && value_type != COBRA_TYPE_UNKNOWN)
                        ir_error(ctx, node, "dict values currently require i64");
                } else if (base_type == COBRA_TYPE_LIST) {
                    if (value_type != COBRA_TYPE_I64 && value_type != COBRA_TYPE_F32 && value_type != COBRA_TYPE_UNKNOWN)
                        ir_error(ctx, node, "list assignment requires a scalar value");
                } else if (value_type != COBRA_TYPE_F32 && value_type != COBRA_TYPE_UNKNOWN &&
                           !is_integer(value_type)) {
                    ir_error(ctx, node, "[]f32 assignment requires an f32 value");
                }
            }
            node->value_type = COBRA_TYPE_VOID;
            return node->value_type;
        }
        case AST_FUNC_CALL: {
            ASTNode *called_function = find_function(ctx, node->name);
            if (called_function && called_function->generic_param_count > 0) {
                if (called_function->generic_param_count != 1) {
                    ir_error(ctx, node, "generic functions currently require exactly one type parameter");
                    called_function = NULL;
                } else {
                    const CobraType *binding = NULL;
                    bool valid = true;
                    size_t argument_index = 0;
                    for (size_t i = 0; i < called_function->child_count; i++) {
                        ASTNode *param = called_function->children[i];
                        if (param->type != AST_PARAM) continue;
                        if (argument_index >= node->child_count) {
                            valid = false;
                            break;
                        }
                        ASTNode *argument = node->children[argument_index++];
                        infer_expr(argument, ctx);
                        if (!bind_generic_type(param->canonical_type,
                                               argument->canonical_type,
                                               called_function->generic_param_types[0],
                                               &binding)) {
                            valid = false;
                            break;
                        }
                    }
                    if (argument_index != node->child_count || !binding) valid = false;
                    if (!valid) {
                        ir_error(ctx, node, "generic call has an ambiguous, mismatched, or unsupported type argument");
                        called_function = NULL;
                    } else {
                        ASTNode *specialized = specialize_generic_function(ctx, called_function, binding);
                        if (!specialized) {
                            called_function = NULL;
                        } else {
                            snprintf(node->name, sizeof(node->name), "%.63s", specialized->name);
                            called_function = specialized;
                        }
                    }
                }
            }
            if (called_function && !function_visible_from(node, called_function)) {
                char message[180];
                snprintf(message, sizeof(message), "function '%s' is private to its module", node->name);
                ir_error(ctx, node, message);
            }
            if (node->qualifier[0] != '\0') {
                if (is_active_region(ctx, node->qualifier)) {
                    /* Region-qualified calls are the arena allocator surface.
                       Only the typed allocation helpers are valid members. */
                    if (strcmp(node->name, "alloc_i64") != 0 &&
                        strcmp(node->name, "alloc_f32") != 0 &&
                        strcmp(node->name, "alloc_u8") != 0) {
                        char message[180];
                        snprintf(message, sizeof(message), "region '%s' has no function '%s'",
                                 node->qualifier, node->name);
                        ir_error(ctx, node, message);
                    }
                } else if (!is_source_module_alias(ctx, node->qualifier)) {
                    char message[180];
                    snprintf(message, sizeof(message), "unknown source module alias '%s'", node->qualifier);
                    ir_error(ctx, node, message);
                } else if (!find_function(ctx, node->name)) {
                    char message[180];
                    snprintf(message, sizeof(message), "module alias '%s' has no function '%s'",
                             node->qualifier, node->name);
                    ir_error(ctx, node, message);
                }
            }
            if (is_string_free_builtin(node->name)) {
                if (node->child_count != 1) {
                    ir_error(ctx, node, "string_free requires exactly one string value");
                } else {
                    CobraTypeKind arg = infer_expr(node->children[0], ctx);
                    if (arg != COBRA_TYPE_STRING && arg != COBRA_TYPE_UNKNOWN) {
                        ir_error(ctx, node, "string_free requires a string value");
                    } else if (node->children[0]->type == AST_VAR_REF) {
                        IRLocal *local = find_local_entry(ctx, node->children[0]->name);
                        if (local && !local->owned) {
                            ir_error(ctx, node, "string_free requires an owned string");
                        } else if (local && local->freed) {
                            ir_error(ctx, node, "double string free detected");
                        } else if (local) {
                            for (size_t i = 0; i < ctx->count; i++) {
                                if (strcmp(ctx->locals[i].borrowed_from, local->name) == 0 && !ctx->locals[i].freed) {
                                    ir_error(ctx, node, "cannot free a string source while a forwarded alias is live");
                                }
                            }
                            local->owned = false;
                            local->freed = true;
                            local->moved = false;
                        }
                    }
                }
                node->value_type = COBRA_TYPE_VOID;
                return node->value_type;
            }
            if (is_sum_builtin(node->name)) {
                bool constructor = !strcmp(node->name, "some") || !strcmp(node->name, "none") ||
                                   !strcmp(node->name, "ok") || !strcmp(node->name, "err");
                bool option = !strcmp(node->name, "some") || !strcmp(node->name, "none") ||
                              !strcmp(node->name, "is_some") || !strcmp(node->name, "unwrap");
                if (!strcmp(node->name, "none")) {
                    if (node->child_count != 0) ir_error(ctx, node, "none takes no arguments");
                    node->value_type = COBRA_TYPE_OPTION;
                                        return node->value_type;
                }
                if (node->child_count != 1) {
                    ir_error(ctx, node, "Option and Result operations require one argument");
                }
                CobraTypeKind arg = node->child_count ? infer_expr(node->children[0], ctx) : COBRA_TYPE_UNKNOWN;
                if (constructor) {
                    const CobraType *payload_type = node->child_count ? node->children[0]->canonical_type : NULL;
                    if (!strcmp(node->name, "some")) {
                        node->value_type = COBRA_TYPE_OPTION;
                                                node->canonical_type = cobra_type_make(ctx->canonical_arena,
                                                               COBRA_TYPE_OPTION, NULL,
                                                               payload_type, NULL, NULL, NULL,
                                                               COBRA_OWNERSHIP_VALUE,
                                                               COBRA_MUTABILITY_DEFAULT, -1);
                    } else if (!strcmp(node->name, "ok")) {
                        node->value_type = COBRA_TYPE_RESULT;
                                                node->canonical_type = cobra_type_make(ctx->canonical_arena,
                                                               COBRA_TYPE_RESULT, NULL,
                                                               payload_type, NULL, NULL, NULL,
                                                               COBRA_OWNERSHIP_VALUE,
                                                               COBRA_MUTABILITY_DEFAULT, -1);
                    } else {
                        node->value_type = COBRA_TYPE_RESULT;
                                                node->canonical_type = cobra_type_make(ctx->canonical_arena,
                                                               COBRA_TYPE_RESULT, NULL,
                                                               NULL, payload_type, NULL, NULL,
                                                               COBRA_OWNERSHIP_VALUE,
                                                               COBRA_MUTABILITY_DEFAULT, -1);
                    }
                    return node->value_type;
                }
                if (option) {
                    if (arg != COBRA_TYPE_OPTION && arg != COBRA_TYPE_UNKNOWN)
                        ir_error(ctx, node, "Option operation requires an Option value");
                    node->value_type = !strcmp(node->name, "is_some") ? COBRA_TYPE_BOOL :
                                       (node->children[0]->type == AST_VAR_REF ?
                                        (find_local_entry(ctx, node->children[0]->name) ?
                                         canonical_element_kind(find_local_entry(ctx, node->children[0]->name)->canonical_type) : COBRA_TYPE_UNKNOWN) : COBRA_TYPE_UNKNOWN);
                    if (node->children[0]->type == AST_VAR_REF && !strcmp(node->name, "unwrap")) {
                        IRLocal *sum_local = find_local_entry(ctx, node->children[0]->name);
                        if (sum_local) node->canonical_type = sum_local->canonical_type
                            ? cobra_type_element(sum_local->canonical_type) : NULL;
                    }
                } else {
                    if (arg != COBRA_TYPE_RESULT && arg != COBRA_TYPE_UNKNOWN)
                        ir_error(ctx, node, "Result operation requires a Result value");
                    if (!strcmp(node->name, "is_ok")) node->value_type = COBRA_TYPE_BOOL;
                    else if (!strcmp(node->name, "unwrap_ok"))
                        node->value_type = node->children[0]->type == AST_VAR_REF && find_local_entry(ctx, node->children[0]->name) ?
                            canonical_element_kind(find_local_entry(ctx, node->children[0]->name)->canonical_type) : COBRA_TYPE_UNKNOWN;
                    else node->value_type = node->children[0]->type == AST_VAR_REF && find_local_entry(ctx, node->children[0]->name) ?
                            canonical_error_kind(find_local_entry(ctx, node->children[0]->name)->canonical_type) : COBRA_TYPE_UNKNOWN;
                    if (node->children[0]->type == AST_VAR_REF) {
                        IRLocal *sum_local = find_local_entry(ctx, node->children[0]->name);
                        if (sum_local) {
                            if (!strcmp(node->name, "unwrap_ok"))
                                node->canonical_type = sum_local->canonical_type
                            ? cobra_type_element(sum_local->canonical_type) : NULL;
                            else if (!strcmp(node->name, "unwrap_err"))
                                node->canonical_type = sum_local->canonical_type
                                ? cobra_type_error(sum_local->canonical_type) : NULL;
                        }
                    }
                }
                return node->value_type;
            }
            if (strcmp(node->name, "range") == 0) {
                if (node->child_count < 1 || node->child_count > 3) {
                    ir_error(ctx, node, "range requires stop, or start, stop, and optional step");
                } else {
                    for (size_t i = 0; i < node->child_count; i++) {
                        if (!is_integer(infer_expr(node->children[i], ctx)))
                            ir_error(ctx, node, "range arguments must be integers");
                    }
                    if (node->child_count == 3 && expression_is_const_zero(node->children[2]))
                        ir_error(ctx, node, "range step cannot be zero");
                }
                node->value_type = COBRA_TYPE_I64;
                return node->value_type;
            }
            if (strcmp(node->name, "enumerate") == 0) {
                if (node->child_count != 1 || node->children[0]->type != AST_VAR_REF) {
                    ir_error(ctx, node, "enumerate requires one named collection");
                } else {
                    CobraTypeKind target = infer_expr(node->children[0], ctx);
                    if (target != COBRA_TYPE_ARRAY && target != COBRA_TYPE_SLICE &&
                        target != COBRA_TYPE_SLICE_F32 && target != COBRA_TYPE_SLICE_U8 &&
                        target != COBRA_TYPE_LIST &&
                        target != COBRA_TYPE_UNKNOWN) {
                        ir_error(ctx, node, "enumerate requires an array, slice, or list");
                    }
                }
                node->value_type = COBRA_TYPE_I64;
                return node->value_type;
            }
            if (is_python_aggregate_builtin(node->name)) {
                if (node->child_count != 1 || node->children[0]->type != AST_VAR_REF) {
                    ir_error(ctx, node, "aggregate builtin requires one named collection");
                } else {
                    CobraTypeKind target = infer_expr(node->children[0], ctx);
                    if (target != COBRA_TYPE_ARRAY && target != COBRA_TYPE_SLICE &&
                        target != COBRA_TYPE_SLICE_U8 && target != COBRA_TYPE_LIST && target != COBRA_TYPE_UNKNOWN) {
                        ir_error(ctx, node, "aggregate builtin requires an integer collection");
                    }
                    if (node->children[0]->type == AST_VAR_REF) {
                        IRLocal *source = find_local_entry(ctx, node->children[0]->name);
                        CobraTypeKind element = source && source->canonical_type
                            ? canonical_element_kind(source->canonical_type) : COBRA_TYPE_UNTYPED;
                        bool unsigned_element = element == COBRA_TYPE_U32 || element == COBRA_TYPE_U64;
                        bool float_element = element == COBRA_TYPE_F32;
                        if (unsigned_element || float_element) {
                            ir_error(ctx, node, "aggregate builtin currently requires signed integer collections");
                        }
                    }
                }
                node->value_type = COBRA_TYPE_I64;
                return node->value_type;
            }
            if (strcmp(node->name, "append") == 0) {
                if (node->child_count != 2) ir_error(ctx, node, "append requires (list, value)");
                else {
                    CobraTypeKind target = infer_expr(node->children[0], ctx);
                    if (target != COBRA_TYPE_LIST && target != COBRA_TYPE_UNKNOWN) ir_error(ctx, node, "append target must be a list");
                    CobraTypeKind value = infer_expr(node->children[1], ctx);
                    if (node->children[0]->type == AST_VAR_REF) {
                        IRLocal *list = find_local_entry(ctx, node->children[0]->name);
                        if (list && list->element_type != COBRA_TYPE_UNTYPED &&
                            value != COBRA_TYPE_UNKNOWN && value != list->element_type &&
                            !(is_integer(list->element_type) && is_integer(value))) {
                            ir_error(ctx, node, "append value does not match the list element type");
                        }
                    }
                }
                node->value_type = COBRA_TYPE_VOID;
                return node->value_type;
            }
            if (strcmp(node->name, "set") == 0 || strcmp(node->name, "get") == 0 ||
                strcmp(node->name, "has") == 0 || strcmp(node->name, "delete") == 0 ||
                strcmp(node->name, "pop") == 0) {
                size_t expected = strcmp(node->name, "set") == 0 ? 3 :
                                  (strcmp(node->name, "get") == 0 || strcmp(node->name, "pop") == 0) ? 3 : 2;
                if (node->child_count != expected) ir_error(ctx, node, "dict operation received the wrong number of arguments");
                if (node->child_count > 0) {
                    CobraTypeKind target = infer_expr(node->children[0], ctx);
                    if (target != COBRA_TYPE_DICT && target != COBRA_TYPE_UNKNOWN) ir_error(ctx, node, "dict operation target must be a dict");
                }
                if (node->child_count > 1) {
                    CobraTypeKind key = infer_expr(node->children[1], ctx);
                    if (key != COBRA_TYPE_STRING && key != COBRA_TYPE_UNKNOWN) ir_error(ctx, node, "dict key must be a string");
                }
                if (!strcmp(node->name, "set") && node->child_count > 2) {
                    CobraTypeKind value = infer_expr(node->children[2], ctx);
                    if (value != COBRA_TYPE_I64 && value != COBRA_TYPE_UNKNOWN) ir_error(ctx, node, "dict value must be i64");
                }
                node->value_type = (!strcmp(node->name, "set") || !strcmp(node->name, "delete")) ? COBRA_TYPE_VOID : COBRA_TYPE_I64;
                return node->value_type;
            }
            if (is_string_builtin(node->name)) {
                size_t expected = strcmp(node->name, "char_at") == 0 ? 2 : 2;
                if (node->child_count != expected) {
                    ir_error(ctx, node, "string builtin received the wrong number of arguments");
                }
                for (size_t i = 0; i < node->child_count; i++) {
                    CobraTypeKind arg = infer_expr(node->children[i], ctx);
                    bool valid = (i == 0 || (strcmp(node->name, "char_at") == 0 && i == 1)) ?
                                 (arg == COBRA_TYPE_STRING || arg == COBRA_TYPE_UNKNOWN) :
                                 (arg == COBRA_TYPE_STRING || arg == COBRA_TYPE_UNKNOWN);
                    if (strcmp(node->name, "char_at") == 0 && i == 1) valid = is_integer(arg) || arg == COBRA_TYPE_UNKNOWN;
                    if (!valid) ir_error(ctx, node, "string builtin argument has the wrong type");
                }
                node->value_type = strcmp(node->name, "concat") == 0 ? COBRA_TYPE_STRING : COBRA_TYPE_I64;
                node->fresh_string_result = strcmp(node->name, "concat") == 0;
                return node->value_type;
            }
            if (strcmp(node->name, "alloc_i64") == 0 || strcmp(node->name, "alloc_f32") == 0 ||
                strcmp(node->name, "alloc_u8") == 0) {
                if (node->child_count != 1) ir_error(ctx, node, "buffer allocation requires exactly one count argument");
                if (node->child_count > 0 && !is_integer(infer_expr(node->children[0], ctx))) {
                    ir_error(ctx, node, "buffer allocation count must be an integer");
                }
                node->value_type = strcmp(node->name, "alloc_f32") == 0 ? COBRA_TYPE_SLICE_F32 :
                                   (strcmp(node->name, "alloc_u8") == 0 ? COBRA_TYPE_SLICE_U8 : COBRA_TYPE_SLICE);
                CobraTypeKind element_kind = strcmp(node->name, "alloc_f32") == 0 ? COBRA_TYPE_F32 :
                                             (strcmp(node->name, "alloc_u8") == 0 ? COBRA_TYPE_U8 : COBRA_TYPE_I64);
                node->canonical_type = cobra_type_make(ctx->canonical_arena, node->value_type, NULL,
                                                       cobra_type_make(ctx->canonical_arena, element_kind,
                                                                       NULL, NULL, NULL, NULL, NULL,
                                                                       COBRA_OWNERSHIP_VALUE,
                                                                       COBRA_MUTABILITY_DEFAULT, -1),
                                                       NULL, NULL, NULL,
                                                       COBRA_OWNERSHIP_VALUE,
                                                       COBRA_MUTABILITY_DEFAULT, -1);
                return node->value_type;
            }
            if (strcmp(node->name, "slice_u8") == 0) {
                if (node->child_count != 3) {
                    ir_error(ctx, node, "slice_u8 requires a buffer, start, and length");
                } else {
                    CobraTypeKind source = infer_expr(node->children[0], ctx);
                    CobraTypeKind start = infer_expr(node->children[1], ctx);
                    CobraTypeKind length = infer_expr(node->children[2], ctx);
                    if (source != COBRA_TYPE_SLICE_U8 && source != COBRA_TYPE_UNKNOWN)
                        ir_error(ctx, node, "slice_u8 source must be a []u8 buffer");
                    if (!is_integer(start) && start != COBRA_TYPE_UNKNOWN)
                        ir_error(ctx, node, "slice_u8 start must be an integer");
                    if (!is_integer(length) && length != COBRA_TYPE_UNKNOWN)
                        ir_error(ctx, node, "slice_u8 length must be an integer");
                }
                node->value_type = COBRA_TYPE_SLICE_U8;
                return node->value_type;
            }
            if (strcmp(node->name, "free") == 0) {
                if (node->child_count != 1) ir_error(ctx, node, "free requires exactly one collection or buffer");
                else if (node->children[0]->type == AST_VAR_REF) {
                    CobraTypeKind target = infer_expr(node->children[0], ctx);
                    if (!is_slice_type(target) && !is_collection_type(target) && target != COBRA_TYPE_UNKNOWN) {
                        ir_error(ctx, node, "free requires an owned collection or buffer");
                    } else {
                        IRLocal *local = find_local_entry(ctx, node->children[0]->name);
                        if (local && local->region_backed) {
                            ir_error(ctx, node, "cannot free a region-backed buffer; the region releases it");
                        } else if (local &&
                                   (local->flow_mutability == COBRA_MUTABILITY_READONLY ||
                                    (local->canonical_type &&
                                     local->canonical_type->ownership == COBRA_OWNERSHIP_BORROWED))) {
                            ir_error(ctx, node, "cannot free a borrowed readonly buffer");
                        } else {
                            if (local && (local->type == COBRA_TYPE_LIST || local->type == COBRA_TYPE_DICT) && !local->owned)
                                ir_error(ctx, node, "cannot free a borrowed collection parameter");
                            if (local) { local->owned = false; local->freed = true; local->moved = false; }
                        }
                    }
                }
                node->value_type = COBRA_TYPE_VOID;
                return node->value_type;
            }
            if (is_tensor_builtin(node->name)) {
                validate_tensor_builtin(node, ctx);
            if (strcmp(node->name, "matmul_f32") == 0) {
                validate_matrix_shapes(node, ctx, 0);
            } else if (strcmp(node->name, "dense_f32") == 0) {
                validate_matrix_shapes(node, ctx, 1);
            }
            if (is_tensor_view_builtin(node->name)) {
                node->value_type = COBRA_TYPE_TENSOR_F32;
            } else if (strcmp(node->name, "fill_f32") == 0 || strcmp(node->name, "relu_f32") == 0 ||
                strcmp(node->name, "matmul_f32") == 0 || strcmp(node->name, "dense_f32") == 0) {
                    node->value_type = COBRA_TYPE_VOID;
                } else {
                    /* Reductions and math intrinsics return f32. */
                    node->value_type = COBRA_TYPE_F32;
                }
                return node->value_type;
            }
            ASTNode *function = find_function(ctx, node->name);
            if (function) {
                size_t argument_index = 0;
                for (size_t i = 0; i < function->child_count; i++) {
                    ASTNode *param = function->children[i];
                    if (param->type != AST_PARAM) continue;
                    if (argument_index >= node->child_count) {
                        ir_error(ctx, node, "function call has too few arguments");
                        break;
                    }
                    CobraTypeKind argument_type = infer_expr(node->children[argument_index], ctx);
                    CobraTypeKind expected = param->declared_type;
                    if (param->canonical_type && node->children[argument_index]->canonical_type &&
                        !canonical_call_compatible(param->canonical_type,
                                                   node->children[argument_index]->canonical_type)) {
                        ir_error(ctx, node, "canonical type metadata disagrees at function call boundary");
                    }
                    bool compatible = argument_type == COBRA_TYPE_UNKNOWN || expected == COBRA_TYPE_UNTYPED ||
                        expected == argument_type ||
                        (is_integer(expected) && is_integer(argument_type)) ||
                        (expected == COBRA_TYPE_F32 && is_integer(argument_type)) ||
                        (expected == COBRA_TYPE_F64 && is_integer(argument_type));
                    if (expected == COBRA_TYPE_V256 && argument_type == COBRA_TYPE_ARRAY) {
                        compatible = true;
                    }
                    if (expected == COBRA_TYPE_ENUM) {
                        compatible = argument_type == COBRA_TYPE_ENUM &&
                                     cobra_type_node_name(param)[0] != '\0' &&
                                     cobra_type_node_name(node->children[argument_index])[0] != '\0' &&
                                     strcmp(cobra_type_node_name(param), cobra_type_node_name(node->children[argument_index])) == 0;
                    }
                    if (expected == COBRA_TYPE_OPTION || expected == COBRA_TYPE_RESULT) {
                        ASTNode *argument = node->children[argument_index];
                        bool sum_compatible = argument_type == expected &&
                            canonical_element_kind(param->canonical_type) == canonical_element_kind(argument->canonical_type) &&
                            (expected != COBRA_TYPE_RESULT ||
                             canonical_error_kind(param->canonical_type) == canonical_error_kind(argument->canonical_type));
                        if (sum_compatible && cobra_type_node_name(argument)[0] != '\0' && cobra_type_node_name(param)[0] != '\0')
                            sum_compatible = strcmp(cobra_type_node_name(param), cobra_type_node_name(argument)) == 0;
                        if (sum_compatible && expected == COBRA_TYPE_RESULT &&
                            cobra_type_node_error_name(argument)[0] != '\0' && cobra_type_node_error_name(param)[0] != '\0')
                            sum_compatible = strcmp(cobra_type_node_error_name(param), cobra_type_node_error_name(argument)) == 0;
                        if (argument->type != AST_VAR_REF) {
                            ir_error(ctx, node, "Option and Result arguments must be named values");
                        } else if (!sum_compatible && argument_type != COBRA_TYPE_UNKNOWN) {
                            char expected_sig[160];
                            char actual_sig[160];
                            const char *expected_value = cobra_type_node_name(param)[0] ? cobra_type_node_name(param) : type_name(canonical_element_kind(param->canonical_type));
                            const char *actual_value = cobra_type_node_name(argument)[0] ? cobra_type_node_name(argument) : type_name(canonical_element_kind(argument->canonical_type));
                            if (expected == COBRA_TYPE_RESULT) {
                                const char *expected_error = cobra_type_node_error_name(param)[0] ? cobra_type_node_error_name(param) : type_name(canonical_error_kind(param->canonical_type));
                                const char *actual_error = cobra_type_node_error_name(argument)[0] ? cobra_type_node_error_name(argument) : type_name(canonical_error_kind(argument->canonical_type));
                                snprintf(expected_sig, sizeof(expected_sig), "Result[%s, %s]", expected_value, expected_error);
                                snprintf(actual_sig, sizeof(actual_sig), "Result[%s, %s]", actual_value, actual_error);
                            } else {
                                snprintf(expected_sig, sizeof(expected_sig), "Option[%s]", expected_value);
                                snprintf(actual_sig, sizeof(actual_sig), "Option[%s]", actual_value);
                            }
                            char message[512];
                            snprintf(message, sizeof(message),
                                     "argument %zu to '%.*s' has type %.*s, expected %.*s",
                                     argument_index + 1, COBRA_MAX_IDENT_LEN - 1, function->name,
                                     79, actual_sig, 79, expected_sig);
                            ir_error(ctx, node, message);
                        }
                        /* The sum-specific diagnostics above replace the generic
                           scalar type mismatch below. */
                        compatible = true;
                    }
                    if (expected == COBRA_TYPE_STRUCT && argument_type != COBRA_TYPE_UNKNOWN) {
                        compatible = argument_type == COBRA_TYPE_STRUCT &&
                                     cobra_type_node_name(param)[0] != '\0' &&
                                     cobra_type_node_name(node->children[argument_index])[0] != '\0' &&
                                     strcmp(cobra_type_node_name(param), cobra_type_node_name(node->children[argument_index])) == 0;
                        if (compatible && param_is_out(param)) {
                            ASTNode *argument = node->children[argument_index];
                            if (argument->type != AST_VAR_REF) {
                                ir_error(ctx, node, "out struct arguments must be named local values");
                                compatible = false;
                            } else {
                                IRLocal *out_local = find_local_entry(ctx, argument->name);
                                if (!out_local || out_local->type != COBRA_TYPE_STRUCT ||
                                    !out_local->canonical_type ||
                                    !param->canonical_type ||
                                    strcmp(canonical_type_name(out_local->canonical_type),
                                           cobra_type_node_name(param)) != 0) {
                                    ir_error(ctx, node, "out struct argument must match the declared struct type");
                                    compatible = false;
                                }
                            }
                        }
                    }
                    if (expected == COBRA_TYPE_SLICE || expected == COBRA_TYPE_SLICE_F32 ||
                        expected == COBRA_TYPE_SLICE_U8) {
                        /* A fixed array is a borrowed pointer+length view only
                           for readonly parameters. No allocation or copy is
                           emitted, and mutable out parameters stay explicit. */
                        if (argument_type == COBRA_TYPE_ARRAY) {
                            const CobraType *expected_element_type = cobra_type_node_element(param);
                            CobraTypeKind expected_element = expected_element_type
                                ? expected_element_type->kind : slice_element_type(expected);
                            const CobraType *actual_element_type = cobra_type_node_element(node->children[argument_index]);
                            CobraTypeKind actual_element = actual_element_type
                                ? actual_element_type->kind : COBRA_TYPE_UNTYPED;
                        bool element_matches = expected_element == COBRA_TYPE_UNTYPED ||
                                actual_element == expected_element ||
                                (is_integer(actual_element) && is_integer(expected_element));
                            /* Unqualified slices retain Cobra's established
                               mutable local-buffer behavior. Explicit readonly
                               slices use the borrow-only contract; out slices
                               still reject fixed-array conversion because they
                               require an explicit writable target ABI. */
                            compatible = !param_is_out(param) && element_matches;
                            if (!compatible && param_is_out(param)) {
                                ir_error(ctx, node, "fixed arrays can only coerce to readonly slice parameters");
                            } else if (!compatible && element_matches) {
                                ir_error(ctx, node, "fixed array element type does not match the readonly slice");
                            }
                        } else {
                            compatible = argument_type == expected || argument_type == COBRA_TYPE_UNKNOWN;
                        }
                    } else if (expected == COBRA_TYPE_TENSOR_F32) {
                        compatible = argument_type == COBRA_TYPE_TENSOR_F32 || argument_type == COBRA_TYPE_UNKNOWN;
                    }
                    if (!compatible) {
                        char message[200];
                        snprintf(message, sizeof(message), "argument %zu to '%s' has type %s, expected %s",
                                 argument_index + 1, function->name, type_name(argument_type), type_name(expected));
                        ir_error(ctx, node, message);
                    }
                    argument_index++;
                }
                if (argument_index < node->child_count) ir_error(ctx, node, "function call has too many arguments");
                validate_shape_contracts(node, function, ctx);
            } else {
                for (size_t i = 0; i < node->child_count; i++) (void)infer_expr(node->children[i], ctx);
            }
            /* Untyped function declarations retain the historical integer ABI;
               typed f32/tensor returns flow through their native return class. */
            node->value_type = function ? function->declared_type : COBRA_TYPE_I64;
            if (function) node->canonical_type = function->canonical_type;
            if (node->value_type == COBRA_TYPE_UNTYPED || node->value_type == COBRA_TYPE_UNKNOWN) {
                node->value_type = COBRA_TYPE_I64;
            }
            /* The canonical function descriptor already carries the complete
               sum payload contract; keep it attached to the call expression. */
            /* A typed postfix `?` consumes the successful tag and exposes the
               payload as the expression value. The failing tag is returned by
               the caller as the same native sum value, so no exception object or
               runtime dispatch is involved. */
            if (function && node->propagate_error &&
                (function->declared_type == COBRA_TYPE_OPTION ||
                 function->declared_type == COBRA_TYPE_RESULT)) {
                bool same_sum = ctx->return_type == function->declared_type;
                bool same_value = ctx->return_payload_type == canonical_element_kind(function->canonical_type);
                bool same_error = function->declared_type != COBRA_TYPE_RESULT ||
                                  ctx->return_error_type == canonical_error_kind(function->canonical_type);
                bool same_value_name = canonical_element_kind(function->canonical_type) != COBRA_TYPE_STRUCT ||
                    !ctx->return_type_name[0] || !cobra_type_node_name(function)[0] ||
                    strcmp(ctx->return_type_name, cobra_type_node_name(function)) == 0;
                bool same_error_name = function->declared_type != COBRA_TYPE_RESULT ||
                    canonical_error_kind(function->canonical_type) != COBRA_TYPE_STRUCT ||
                    !ctx->return_error_type_name[0] || !cobra_type_node_error_name(function)[0] ||
                    strcmp(ctx->return_error_type_name, cobra_type_node_error_name(function)) == 0;
                if (!same_sum || !same_value || !same_error || !same_value_name || !same_error_name) {
                    ir_error(ctx, node, "postfix '?' requires the callee and current function to use the same sum type");
                }
                node->value_type = canonical_element_kind(function->canonical_type);
                if (function->canonical_type) {
                    node->canonical_type = cobra_type_element(function->canonical_type);
                }
            }
            node->fresh_string_result = function && function_returns_owned_string(ctx, function);
            return node->value_type;
        }
        default:
            return COBRA_TYPE_UNKNOWN;
    }
}

static void validate_statement(ASTNode *node, IRContext *ctx);
static void validate_block(ASTNode *block, IRContext *ctx);

static void validate_match_statement(ASTNode *node, IRContext *ctx) {
    if (!node || node->child_count < 2) {
        ir_error(ctx, node, "match requires a value and at least one arm");
        return;
    }
    ASTNode *target = node->children[0];
    CobraTypeKind target_type = infer_expr(target, ctx);
    if (target_type != COBRA_TYPE_ENUM) {
        ir_error(ctx, node, "match currently requires an enum value");
        return;
    }
    IREnum *enum_decl = find_enum(ctx, cobra_type_node_name(target));
    if (!enum_decl) {
        ir_error(ctx, node, "match target enum is not registered");
        return;
    }
    bool seen[COBRA_MAX_ENUM_VARIANTS] = {0};
    bool has_default = false;
    int covered = 0;
    int case_count = 0;
    for (size_t i = 1; i < node->child_count; i++) {
        ASTNode *arm = node->children[i];
        if (!arm->is_default_case && ++case_count > COBRA_MAX_ENUM_VARIANTS) {
            ir_error(ctx, arm, "match contains too many enum cases");
            continue;
        }
        if (arm->type != AST_MATCH_CASE || arm->child_count == 0) {
            ir_error(ctx, arm, "invalid match arm");
            continue;
        }
        if (arm->is_default_case) {
            if (has_default) ir_error(ctx, arm, "match may contain only one else arm");
            has_default = true;
            validate_block(arm->children[0], ctx);
            continue;
        }
        if (strcmp(arm->match_type_name, enum_decl->name) != 0) {
            ir_error(ctx, arm, "match case belongs to a different enum");
            validate_block(arm->children[0], ctx);
            continue;
        }
        int value = find_enum_variant(ctx, arm->match_type_name, arm->secondary_name);
        if (value == INT_MIN) {
            ir_error(ctx, arm, "unknown enum variant in match case");
        } else {
            arm->int_val = value;
            int variant_index = -1;
            for (int j = 0; j < enum_decl->variant_count; j++) {
                if (enum_decl->variants[j].value == value) { variant_index = j; break; }
            }
            if (variant_index >= 0 && seen[variant_index]) {
                ir_error(ctx, arm, "duplicate enum variant in match");
            } else if (variant_index >= 0) {
                seen[variant_index] = true;
                covered++;
            }
        }
        validate_block(arm->children[0], ctx);
    }
    if (!has_default && covered != enum_decl->variant_count) {
        ir_error(ctx, node, "non-exhaustive match requires an else arm");
    }
}

static void validate_block(ASTNode *block, IRContext *ctx) {
    if (!block) return;
    for (size_t i = 0; i < block->child_count; i++) {
        validate_statement(block->children[i], ctx);
    }
}

static void validate_statement(ASTNode *node, IRContext *ctx) {
    if (!node) return;

    switch (node->type) {
        case AST_ENUM_DECL:
            register_enum_decl(ctx, node);
            return;
        case AST_STRUCT_DECL:
            register_struct_decl(ctx, node, true);
            return;
        case AST_MATCH_STMT:
            validate_match_statement(node, ctx);
            return;
        case AST_MEMBER_ASSIGN: {
            /* A nested member assignment keeps the base member expression as
               child zero. Scalar nested fields are safe; borrowed view fields
               still require the direct owner-aware path below. */
            if (node->child_count != 2 ||
                (node->children[0]->type != AST_VAR_REF &&
                 node->children[0]->type != AST_MEMBER_ACCESS)) {
                ir_error(ctx, node, "member assignment requires a struct value");
                return;
            }
            ASTNode *base_expr = node->children[0];
            CobraTypeKind base_type = infer_expr(base_expr, ctx);
            if (base_type != COBRA_TYPE_STRUCT) {
                ir_error(ctx, node, "member assignment requires a struct value");
                return;
            }
            IRLocal *base = base_expr->type == AST_VAR_REF ?
                            find_local_entry(ctx, base_expr->name) : NULL;
            if (base && base->is_const) {
                char message[160];
                snprintf(message, sizeof(message), "cannot assign to const struct '%s'", base->name);
                ir_error(ctx, node, message);
            }
            const char *base_type_name = cobra_type_node_name(base_expr);
            IRStruct *type = find_struct(ctx, base_type_name);
            if (!type) { ir_error(ctx, node, "struct type is not registered"); return; }
            CobraTypeKind field_type = COBRA_TYPE_UNKNOWN;
            char field_type_name[COBRA_MAX_IDENT_LEN] = "";
            int field_index = -1;
            int field_qualifier = 0;
            for (int i = 0; i < type->field_count; i++) {
                if (strcmp(type->fields[i].name, node->secondary_name) == 0) {
                    field_index = i;
                    field_type = type->fields[i].type;
                    snprintf(field_type_name, sizeof(field_type_name), "%.63s", type->fields[i].type_name);
                    field_qualifier = canonical_field_qualifier(ctx, base_type_name,
                                                                  type->fields[i].name);
                    node->canonical_type = canonical_field_value(ctx, base_type_name,
                                                                  type->fields[i].name);
                    node->declared_type = field_type;
                    break;
                }
            }
            if (field_type == COBRA_TYPE_UNKNOWN) {
                char message[180];
                snprintf(message, sizeof(message), "struct '%s' has no field '%s'", base_type_name, node->secondary_name);
                ir_error(ctx, node, message);
                return;
            }
            bool borrowed_view_field = field_index >= 0 &&
                type->fields[field_index].ownership == COBRA_FIELD_BORROWED_VIEW &&
                (field_type == COBRA_TYPE_SLICE || field_type == COBRA_TYPE_SLICE_F32 ||
                 field_type == COBRA_TYPE_SLICE_U8);
            if (borrowed_view_field && base && field_qualifier == 1 &&
                (base->is_parameter || (field_index >= 0 &&
                 (base->struct_field_initialized & (1ULL << field_index)) != 0))) {
                ir_error(ctx, node, "cannot reassign readonly borrowed slice field");
            }
            CobraTypeKind value = infer_expr(node->children[1], ctx);
            if (borrowed_view_field && value != COBRA_TYPE_UNKNOWN &&
                value != COBRA_TYPE_SLICE && value != COBRA_TYPE_SLICE_F32 &&
                value != COBRA_TYPE_SLICE_U8) {
                ir_error(ctx, node, "borrowed slice field assignment requires a slice view");
            }
            if (borrowed_view_field && value != COBRA_TYPE_UNKNOWN &&
                node->children[1]->canonical_type && node->canonical_type) {
                const CobraType *expected_element = cobra_type_element(node->canonical_type);
                const CobraType *actual_element = cobra_type_element(node->children[1]->canonical_type);
                if (expected_element && actual_element &&
                    expected_element->kind != actual_element->kind) {
                    ir_error(ctx, node, "borrowed slice field element type does not match");
                }
            }
            if (field_type == COBRA_TYPE_ENUM && value == COBRA_TYPE_ENUM &&
                field_type_name[0] != '\0' && cobra_type_node_name(node->children[1])[0] != '\0' &&
                strcmp(field_type_name, cobra_type_node_name(node->children[1])) != 0) {
                ir_error(ctx, node, "cannot assign a value from a different enum to this field");
            }
            if (borrowed_view_field) {
                if (!base) {
                    ir_error(ctx, node, "nested borrowed view assignment requires an explicit owner contract");
                } else {
                    set_struct_field_borrow(ctx, base, field_index, node->children[1]);
                    if (field_index >= 0) base->struct_field_initialized |= (1ULL << field_index);
                    if (!base->borrowed_from[0] && node->children[1]->type == AST_VAR_REF)
                        snprintf(base->borrowed_from, sizeof(base->borrowed_from), "%.63s", node->children[1]->name);
                }
            }
            if (!borrowed_view_field && value != COBRA_TYPE_UNKNOWN && value != COBRA_TYPE_UNTYPED &&
                !(field_type == value) && !(is_integer(field_type) && is_integer(value)) &&
                !(field_type == COBRA_TYPE_F32 && is_integer(value)) &&
                !(field_type == COBRA_TYPE_BOOL && (value == COBRA_TYPE_BOOL || is_integer(value)))) {
                char message[180];
                snprintf(message, sizeof(message), "field '%s' requires %s, got %s",
                         node->secondary_name, type_name(field_type), type_name(value));
                ir_error(ctx, node, message);
            }
            node->value_type = COBRA_TYPE_VOID;
            return;
        }
        case AST_VAR_DECL:
        case AST_HEAP_DECL: {
            reject_illegal_float_context(node->child_count > 0 ? node->children[0] : NULL, ctx,
                                         "scalar f32 values are not yet supported; use a float literal directly with fill_f32");
            CobraTypeKind inferred = node->child_count > 0 ? infer_expr(node->children[0], ctx) : COBRA_TYPE_UNTYPED;
            if (node->child_count > 0 && node->children[0]->type == AST_FUNC_CALL &&
                (strcmp(node->children[0]->name, "fill_f32") == 0 ||
                 strcmp(node->children[0]->name, "relu_f32") == 0 ||
                 strcmp(node->children[0]->name, "matmul_f32") == 0)) {
                ir_error(ctx, node, "void tensor intrinsic cannot initialize a variable");
            }
            CobraTypeKind declared = node->declared_type == COBRA_TYPE_UNTYPED ? inferred : node->declared_type;
            if (declared == COBRA_TYPE_STRUCT && find_enum(ctx, cobra_type_node_name(node))) {
                declared = COBRA_TYPE_ENUM;
                node->declared_type = COBRA_TYPE_ENUM;
                /* Refresh the canonical descriptor too, so a bare enum name in
                   declaration position does not stay tagged as a struct. */
                node->canonical_type = cobra_type_make(ctx->canonical_arena,
                                                       COBRA_TYPE_ENUM, cobra_type_node_name(node),
                                                       NULL, NULL, NULL, NULL,
                                                       COBRA_OWNERSHIP_VALUE,
                                                       COBRA_MUTABILITY_DEFAULT, -1);
            }
            node->value_type = declared;
            if (node->type == AST_HEAP_DECL && declared == COBRA_TYPE_STRUCT) {
                IRStruct *heap_struct = find_struct(ctx, cobra_type_node_name(node));
                if (heap_struct && heap_struct->has_borrowed_fields) {
                    ir_error(ctx, node,
                             "cannot store a borrowed generic struct in heap storage; its owner is not part of the value");
                }
            }
            if (!node->canonical_type) canonical_inferred_type(ctx, node);
            CobraTypeKind declared_element = canonical_element_kind(node->canonical_type);
            if ((declared == COBRA_TYPE_ARRAY || declared == COBRA_TYPE_LIST) &&
                declared_element == COBRA_TYPE_UNTYPED &&
                node->child_count > 0 && node->children[0]->type == AST_ARRAY_LITERAL &&
                node->children[0]->child_count > 0) {
                infer_expr(node->children[0]->children[0], ctx);
                declared_element = canonical_element_kind(node->children[0]->children[0]->canonical_type);
            }
            if (declared == COBRA_TYPE_LIST && node->child_count > 0 &&
                node->children[0]->type == AST_ARRAY_LITERAL) {
                for (size_t i = 0; i < node->children[0]->child_count; i++) {
                    CobraTypeKind element = infer_expr(node->children[0]->children[i], ctx);
                    if (declared_element != COBRA_TYPE_UNTYPED && element != COBRA_TYPE_UNKNOWN &&
                        element != declared_element && !(is_integer(declared_element) && is_integer(element)))
                        ir_error(ctx, node, "list literal element does not match its element type");
                }
            }
            if (declared == COBRA_TYPE_F64) {
                ir_error(ctx, node, "f64 is reserved until native double-precision lowering is implemented");
            }
            validate_declared_shape(node, node->child_count > 0 ? node->children[0] : NULL, ctx);
            if ((declared == COBRA_TYPE_OPTION || declared == COBRA_TYPE_RESULT) &&
                node->child_count > 0 && node->children[0]->type == AST_FUNC_CALL) {
                ASTNode *constructor = node->children[0];
                bool has_option_payload = canonical_element_kind(constructor->canonical_type) != COBRA_TYPE_UNKNOWN &&
                                           canonical_element_kind(constructor->canonical_type) != COBRA_TYPE_UNTYPED;
                bool has_result_value = has_option_payload;
                bool has_result_error = canonical_error_kind(constructor->canonical_type) != COBRA_TYPE_UNKNOWN &&
                                        canonical_error_kind(constructor->canonical_type) != COBRA_TYPE_UNTYPED;
                if (declared == COBRA_TYPE_OPTION && has_option_payload &&
                    canonical_element_kind(node->canonical_type) != COBRA_TYPE_UNTYPED && canonical_element_kind(constructor->canonical_type) != canonical_element_kind(node->canonical_type) &&
                    !(is_integer(canonical_element_kind(constructor->canonical_type)) && is_integer(canonical_element_kind(node->canonical_type)))) {
                    ir_error(ctx, node, "Option payload does not match its declared type");
                }
                if (declared == COBRA_TYPE_RESULT && has_result_value &&
                    canonical_element_kind(node->canonical_type) != COBRA_TYPE_UNTYPED && canonical_element_kind(constructor->canonical_type) != canonical_element_kind(node->canonical_type) &&
                    !(is_integer(canonical_element_kind(constructor->canonical_type)) && is_integer(canonical_element_kind(node->canonical_type)))) {
                    ir_error(ctx, node, "Result value payload does not match its declared type");
                }
                if (declared == COBRA_TYPE_RESULT && has_result_error &&
                    canonical_error_kind(node->canonical_type) != COBRA_TYPE_UNTYPED &&
                    canonical_error_kind(constructor->canonical_type) != canonical_error_kind(node->canonical_type) &&
                    !(is_integer(canonical_error_kind(constructor->canonical_type)) && is_integer(canonical_error_kind(node->canonical_type)))) {
                    ir_error(ctx, node, "Result error payload does not match its declared type");
                }
                if (has_option_payload && canonical_element_kind(node->canonical_type) == COBRA_TYPE_STRUCT &&
                    cobra_type_node_name(constructor)[0] && cobra_type_node_name(node)[0] &&
                    strcmp(cobra_type_node_name(constructor), cobra_type_node_name(node)) != 0) {
                    ir_error(ctx, node, "struct payload does not match its declared type");
                }
                if (declared == COBRA_TYPE_RESULT && has_result_error &&
                    canonical_error_kind(node->canonical_type) == COBRA_TYPE_STRUCT &&
                    cobra_type_node_error_name(constructor)[0] && cobra_type_node_error_name(node)[0] &&
                    strcmp(cobra_type_node_error_name(constructor), cobra_type_node_error_name(node)) != 0) {
                    ir_error(ctx, node, "struct error payload does not match its declared type");
                }
            }
            if (declared == COBRA_TYPE_ENUM && inferred == COBRA_TYPE_ENUM &&
                cobra_type_node_name(node)[0] != '\0' && cobra_type_node_name(node->children[0])[0] != '\0' &&
                strcmp(cobra_type_node_name(node), cobra_type_node_name(node->children[0])) != 0) {
                ir_error(ctx, node, "enum initializer belongs to a different enum");
            }
            if (node->child_count > 0 && declared == COBRA_TYPE_STRUCT && inferred == COBRA_TYPE_STRUCT &&
                cobra_type_node_name(node)[0] && cobra_type_node_name(node->children[0])[0] &&
                strcmp(cobra_type_node_name(node), cobra_type_node_name(node->children[0])) != 0) {
                ir_error(ctx, node, "struct initializer belongs to a different type");
            }
            if (node->declared_type != COBRA_TYPE_UNTYPED && inferred != COBRA_TYPE_UNKNOWN &&
                inferred != COBRA_TYPE_UNTYPED && !declared_compatible(declared, inferred)) {
                char message[180];
                snprintf(message, sizeof(message), "'%s' declared as %s but initialized with %s",
                         node->name, type_name(declared), type_name(inferred));
                ir_error(ctx, node, message);
            }
            node->value_type = declared;
            canonical_inferred_type(ctx, node);
            if (!add_local(ctx, node->name, declared, node)) {
                char message[160];
                snprintf(message, sizeof(message), "duplicate variable '%s'", node->name);
                ir_error(ctx, node, message);
            }
            IRLocal *declared_local = find_local_entry(ctx, node->name);
            if (declared_local) node->canonical_type = declared_local->canonical_type;
            if (declared_local && declared == COBRA_TYPE_STRUCT &&
                node->child_count > 0 && node->children[0]->type == AST_VAR_REF) {
                IRLocal *source_struct = find_local_entry(ctx, node->children[0]->name);
                if (source_struct && source_struct->type == COBRA_TYPE_STRUCT)
                    copy_struct_borrow_metadata(declared_local, source_struct);
            }
            if (node->child_count > 0 && is_region_alloc_call(ctx, node->children[0])) {
                IRLocal *region_local = find_local_entry(ctx, node->name);
                if (region_local) {
                    region_local->region_backed = true;
                    region_local->region_id = -1;
                    for (int region = ctx->region_depth - 1; region >= 0; region--) {
                        if (strcmp(ctx->regions[region], node->children[0]->qualifier) == 0) {
                            region_local->region_id = ctx->region_ids[region];
                            break;
                        }
                    }
                }
            }
            if (node->child_count > 0 && declared == COBRA_TYPE_SLICE_U8 &&
                node->children[0]->type == AST_MEMBER_ACCESS) {
                IRLocal *view_local = find_local_entry(ctx, node->name);
                ASTNode *member_base = node->children[0]->child_count > 0 ?
                                       node->children[0]->children[0] : NULL;
                if (view_local && member_base && member_base->type == AST_VAR_REF) {
                    propagate_member_view_metadata(ctx, view_local, node->children[0]);
                }
            }
            if (declared == COBRA_TYPE_LIST || declared == COBRA_TYPE_DICT ||
                (node->child_count > 0 && expression_is_fresh_string(node->children[0], ctx)) ||
                (node->child_count > 0 && node->children[0]->type == AST_FUNC_CALL &&
                 (strcmp(node->children[0]->name, "alloc_i64") == 0 ||
                  strcmp(node->children[0]->name, "alloc_f32") == 0 ||
                  strcmp(node->children[0]->name, "alloc_u8") == 0))) {
                IRLocal *local = find_local_entry(ctx, node->name);
                if (local && !local->region_backed) local->owned = true;
            } else if (declared == COBRA_TYPE_STRING && node->child_count > 0 &&
                       node->children[0]->type == AST_FUNC_CALL &&
                       node->children[0]->fresh_string_result == false &&
                       node->children[0]->child_count > 0 &&
                       node->children[0]->children[0]->type == AST_VAR_REF) {
                IRLocal *local = find_local_entry(ctx, node->name);
                if (local) {
                    snprintf(local->borrowed_from, sizeof(local->borrowed_from), "%.63s",
                             node->children[0]->children[0]->name);
                }
            } else if (declared == COBRA_TYPE_TENSOR_F32 && node->child_count > 0 &&
                       node->children[0]->type == AST_FUNC_CALL &&
                       is_tensor_view_builtin(node->children[0]->name)) {
                IRLocal *local = find_local_entry(ctx, node->name);
                ASTNode *source = node->children[0]->child_count ? node->children[0]->children[0] : NULL;
                if (local && source && source->type == AST_VAR_REF) {
                    snprintf(local->borrowed_from, sizeof(local->borrowed_from), "%.63s", source->name);
                }
            } else if (declared == COBRA_TYPE_LIST || declared == COBRA_TYPE_DICT) {
                IRLocal *local = find_local_entry(ctx, node->name);
                if (local) {
                    local->owned = true;
                }
            } else if (declared == COBRA_TYPE_SLICE_U8 && node->child_count > 0 &&
                       node->children[0]->type == AST_FUNC_CALL &&
                       strcmp(node->children[0]->name, "slice_u8") == 0) {
                IRLocal *local = find_local_entry(ctx, node->name);
                ASTNode *source = node->children[0]->child_count ? node->children[0]->children[0] : NULL;
                if (local && source && source->type == AST_VAR_REF) {
                    snprintf(local->borrowed_from, sizeof(local->borrowed_from), "%.63s", source->name);
                }
            } else if (declared == COBRA_TYPE_SLICE || declared == COBRA_TYPE_SLICE_F32 ||
                       declared == COBRA_TYPE_SLICE_U8 || declared == COBRA_TYPE_TENSOR_F32) {
                bool valid_view = node->child_count && node->children[0]->type == AST_FUNC_CALL &&
                                  is_tensor_view_builtin(node->children[0]->name);
                bool valid_tensor_return = node->child_count && node->children[0]->type == AST_FUNC_CALL &&
                                           find_function(ctx, node->children[0]->name) &&
                                           find_function(ctx, node->children[0]->name)->declared_type == COBRA_TYPE_TENSOR_F32;
                bool valid_slice_source = declared == COBRA_TYPE_SLICE || declared == COBRA_TYPE_SLICE_F32 ||
                                           declared == COBRA_TYPE_SLICE_U8;
                if (!valid_view && !valid_tensor_return && !valid_slice_source &&
                    declared != COBRA_TYPE_LIST && declared != COBRA_TYPE_DICT) {
                    ir_error(ctx, node, "slice declarations require direct allocation, a zero-copy view, or a typed tensor-returning function");
                }
                if (valid_tensor_return) {
                    IRLocal *local = find_local_entry(ctx, node->name);
                    ASTNode *call = node->children[0];
                    for (size_t arg = 0; local && arg < call->child_count; arg++) {
                        ASTNode *value = call->children[arg];
                        if (value->type == AST_VAR_REF) {
                            IRLocal *source = find_local_entry(ctx, value->name);
                            if (source && source->type == COBRA_TYPE_TENSOR_F32) {
                                snprintf(local->borrowed_from, sizeof(local->borrowed_from), "%.63s", value->name);
                                break;
                            }
                        }
                    }
                }
            }
            break;
        }
        case AST_INDEX_ASSIGN: {
            CobraTypeKind base_type = COBRA_TYPE_UNKNOWN;
            if (node->secondary_name[0] != '\0') {
                IRLocal *owner = find_local_entry(ctx, node->name);
                IRStruct *owner_type = owner ? find_struct(ctx, owner->type_name) : NULL;
                if (owner && owner->region_expired)
                    ir_error(ctx, node, "struct byte-view is used after its region ended");
                if (!owner || owner->type != COBRA_TYPE_STRUCT || !owner_type) {
                    ir_error(ctx, node, "struct byte-view write requires a struct value");
                } else {
                    int field_qualifier = 0;
                    for (int field_index = 0; field_index < owner_type->field_count; field_index++) {
                        if (strcmp(owner_type->fields[field_index].name, node->secondary_name) == 0) {
                            base_type = owner_type->fields[field_index].type;
                            field_qualifier = canonical_field_qualifier(ctx, owner->type_name,
                                                                         owner_type->fields[field_index].name);
                            break;
                        }
                    }
                    if (base_type != COBRA_TYPE_SLICE &&
                        base_type != COBRA_TYPE_SLICE_F32 &&
                        base_type != COBRA_TYPE_SLICE_U8)
                        ir_error(ctx, node, "only slice struct fields are writable by index");
                    if (field_qualifier != 2)
                        ir_error(ctx, node, "cannot write readonly borrowed slice field");
                }
            } else if (!find_local(ctx, node->name, &base_type) ||
                (base_type != COBRA_TYPE_ARRAY && base_type != COBRA_TYPE_SLICE &&
                 base_type != COBRA_TYPE_SLICE_U8 && !is_f32_buffer_type(base_type) &&
                 base_type != COBRA_TYPE_LIST &&
                 base_type != COBRA_TYPE_DICT)) {
                ir_error(ctx, node, "writable indexing requires a list, dict, slice, or tensor buffer");
            }
            IRLocal *written = find_local_entry(ctx, node->name);
            if (written && written->flow_mutability == COBRA_MUTABILITY_READONLY) {
                ir_error(ctx, node, "readonly buffer cannot be written");
            }
            size_t index_count = node->child_count > 0 ? node->child_count - 1 : 0;
            if (base_type == COBRA_TYPE_DICT ? index_count != 1 :
                index_count < 1 || index_count > (base_type == COBRA_TYPE_TENSOR_F32 ? 2 : 1)) {
                ir_error(ctx, node, "indexed assignment requires one index, or two for a rank-2 tensor");
                break;
            }
            for (size_t i = 0; i < index_count; i++) {
                CobraTypeKind index_type = infer_expr(node->children[i], ctx);
                if (base_type == COBRA_TYPE_DICT) {
                    if (index_type != COBRA_TYPE_STRING && index_type != COBRA_TYPE_UNKNOWN)
                        ir_error(ctx, node, "dict index must be a string");
                } else if (!is_integer(index_type)) {
                    ir_error(ctx, node, "collection index must be an integer");
                }
            }
            CobraTypeKind assigned = infer_expr(node->children[node->child_count - 1], ctx);
            if (base_type == COBRA_TYPE_DICT) {
                if (assigned != COBRA_TYPE_I64 && assigned != COBRA_TYPE_UNKNOWN)
                    ir_error(ctx, node, "dict values currently require i64");            } else if (base_type == COBRA_TYPE_ARRAY) {
                IRLocal *array = find_local_entry(ctx, node->name);
                CobraTypeKind element = array ? array->element_type : COBRA_TYPE_UNTYPED;
                bool compatible = assigned == COBRA_TYPE_UNKNOWN || element == COBRA_TYPE_UNTYPED ||
                    assigned == element || (is_integer(element) && is_integer(assigned));
                if (element == COBRA_TYPE_F32) compatible = assigned == COBRA_TYPE_F32 ||
                    is_integer(assigned) || assigned == COBRA_TYPE_UNKNOWN;
                if (!compatible) ir_error(ctx, node, "array assignment does not match its element type");
            } else if (is_f32_buffer_type(base_type)) {
                /* Integer literals and variables coerce to f32 on assignment,
                   matching the mixed-numeric semantics of arithmetic. */
                if (assigned != COBRA_TYPE_F32 && assigned != COBRA_TYPE_UNKNOWN &&
                    !is_integer(assigned)) {
                    ir_error(ctx, node, "[]f32 assignment requires an f32 value");
                }
            } else if (base_type == COBRA_TYPE_LIST) {
                IRLocal *list = find_local_entry(ctx, node->name);
                CobraTypeKind element = list ? list->element_type : COBRA_TYPE_UNTYPED;
                bool compatible = assigned == COBRA_TYPE_UNKNOWN ||
                    element == COBRA_TYPE_UNTYPED || assigned == element ||
                    (is_integer(element) && is_integer(assigned));
                if (element == COBRA_TYPE_F32) compatible = assigned == COBRA_TYPE_F32 ||
                    is_integer(assigned) || assigned == COBRA_TYPE_UNKNOWN;
                if (!compatible) ir_error(ctx, node, "list assignment does not match its element type");
            } else if (assigned != COBRA_TYPE_UNKNOWN && !is_integer(assigned)) {
                ir_error(ctx, node, "[]i64 assignment requires an integer value");
            }
            break;
        }
        case AST_ASSIGN: {
            reject_illegal_float_context(node->child_count > 0 ? node->children[0] : NULL, ctx,
                                         "scalar f32 values are not yet supported; use a float literal directly with fill_f32");
            IRLocal *assigned_local = find_local_entry(ctx, node->name);
            if (assigned_local && assigned_local->is_const) {
                char message[160];
                snprintf(message, sizeof(message), "cannot assign to const '%s'", node->name);
                ir_error(ctx, node, message);
            }
            CobraTypeKind existing = COBRA_TYPE_UNKNOWN;
            bool exists = find_local(ctx, node->name, &existing);
            CobraTypeKind value = node->child_count > 0 ? infer_expr(node->children[0], ctx) : COBRA_TYPE_UNKNOWN;
            IRLocal *source_local = (node->child_count > 0 && node->children[0]->type == AST_VAR_REF) ?
                                    find_local_entry(ctx, node->children[0]->name) : NULL;
            if (exists && existing == COBRA_TYPE_ENUM && value == COBRA_TYPE_ENUM &&
                assigned_local && assigned_local->canonical_type &&
                canonical_type_name(assigned_local->canonical_type)[0] != '\0' &&
                cobra_type_node_name(node->children[0])[0] != '\0' &&
                strcmp(canonical_type_name(assigned_local->canonical_type),
                       cobra_type_node_name(node->children[0])) != 0) {
                ir_error(ctx, node, "cannot assign a value from a different enum");
            }
            if (value == COBRA_TYPE_F64 || existing == COBRA_TYPE_F64) {
                ir_error(ctx, node, "f64 is reserved until native double-precision lowering is implemented");
            }
            if (value == COBRA_TYPE_VOID) {
                ir_error(ctx, node, "void tensor intrinsic cannot be assigned to a variable");
            }
            if (exists && existing == COBRA_TYPE_F32 && value == COBRA_TYPE_F32) {
                node->value_type = COBRA_TYPE_F32;
            }
            node->value_type = value;
            if (!exists) {
                /* Cobra's concise `name = value` form declares on first use. */
                if (value == COBRA_TYPE_UNKNOWN) value = COBRA_TYPE_UNTYPED;
                const ASTNode *source_shape =
                    (node->child_count > 0 && node->children[0]->canonical_type)
                        ? node->children[0] : NULL;
                if (!add_local(ctx, node->name, value, source_shape)) {
                    char message[160];
                    snprintf(message, sizeof(message), "could not declare '%s'", node->name);
                    ir_error(ctx, node, message);
                }
                if (node->child_count > 0 && is_region_alloc_call(ctx, node->children[0])) {
                    IRLocal *region_local = find_local_entry(ctx, node->name);
                    if (region_local) {
                        region_local->region_backed = true;
                        region_local->region_id = -1;
                        for (int region = ctx->region_depth - 1; region >= 0; region--) {
                            if (strcmp(ctx->regions[region], node->children[0]->qualifier) == 0) {
                                region_local->region_id = ctx->region_ids[region];
                                break;
                            }
                        }
                    }
                }
                if (node->child_count > 0 && node->children[0]->type == AST_MEMBER_ACCESS) {
                    IRLocal *view = find_local_entry(ctx, node->name);
                    if (view) propagate_member_view_metadata(ctx, view, node->children[0]);
                }
                if (node->child_count > 0 && node->children[0]->type == AST_FUNC_CALL &&
                    strcmp(node->children[0]->name, "slice_u8") == 0 &&
                    node->children[0]->child_count > 0 &&
                    node->children[0]->children[0]->type == AST_VAR_REF) {
                    IRLocal *view = find_local_entry(ctx, node->name);
                    if (view) {
                        snprintf(view->borrowed_from, sizeof(view->borrowed_from), "%.63s",
                                 node->children[0]->children[0]->name);
                    }
                }
                if (value == COBRA_TYPE_ARRAY && node->child_count > 0 &&
                    node->children[0]->type == AST_ARRAY_LITERAL &&
                    node->children[0]->child_count > 0) {
                    IRLocal *local = find_local_entry(ctx, node->name);
                    if (local) local->element_type = infer_expr(node->children[0]->children[0], ctx);
                }
                if (node->child_count > 0 && node->children[0]->type == AST_FUNC_CALL &&
                    (strcmp(node->children[0]->name, "alloc_i64") == 0 ||
                 strcmp(node->children[0]->name, "alloc_f32") == 0 ||
                 strcmp(node->children[0]->name, "alloc_u8") == 0)) {
                    IRLocal *local = find_local_entry(ctx, node->name);
                    if (local) local->owned = true;
            } else if (value == COBRA_TYPE_SLICE || value == COBRA_TYPE_SLICE_F32 ||
                       value == COBRA_TYPE_SLICE_U8 || value == COBRA_TYPE_TENSOR_F32) {
                ir_error(ctx, node, "implicit tensor aliases require an explicit tensor[...]f32 declaration");
                }
            } else if (value != COBRA_TYPE_UNKNOWN && value != COBRA_TYPE_UNTYPED &&
                       existing != value && !(is_integer(existing) && is_integer(value))) {
                char message[160];
                snprintf(message, sizeof(message), "assignment to '%s' changes its type", node->name);
                ir_error(ctx, node, message);
            }
            /* Reassigning an existing buffer local borrows the source's
               qualifier: a readonly source makes writes through the alias
               fail exactly like writes through the parameter, and a fresh
               allocation (or any non-readonly source) resets it, so a local
               that once aliased readonly storage can be rebound to writable
               storage again. Region-backed and plain owned locals both follow
               this rule. */
            if (exists && assigned_local && is_slice_type(assigned_local->type)) {
                assigned_local->flow_mutability =
                    (source_local && source_local->flow_mutability == COBRA_MUTABILITY_READONLY)
                        ? COBRA_MUTABILITY_READONLY : COBRA_MUTABILITY_DEFAULT;
            }
            if (exists && assigned_local && existing == COBRA_TYPE_STRUCT &&
                value == COBRA_TYPE_STRUCT && source_local &&
                strcmp(assigned_local->type_name, source_local->type_name) == 0) {
                copy_struct_borrow_metadata(assigned_local, source_local);
            }
            if (source_local && source_local->owned &&
                (source_local->type == COBRA_TYPE_LIST || source_local->type == COBRA_TYPE_DICT ||
                 source_local->type == COBRA_TYPE_STRING)) {
                if (exists && strcmp(source_local->name, node->name) != 0) {
                    IRLocal *destination = find_local_entry(ctx, node->name);
                    if (destination && destination->owned) {
                        ir_error(ctx, node, "cannot overwrite an owned value without freeing it first");
                    } else if (destination) {
                        ir_error(ctx, node, "owned value aliases are not supported yet; use it in place or free it before reassignment");
                    }
                } else if (!exists) {
                    ir_error(ctx, node, "owned values require an explicit type declaration before moving");
                }
            }
            if ((exists && node->child_count > 0 && node->children[0]->type == AST_FUNC_CALL &&
                 (strcmp(node->children[0]->name, "alloc_i64") == 0 ||
                  strcmp(node->children[0]->name, "alloc_f32") == 0 ||
                  strcmp(node->children[0]->name, "concat") == 0)) ||
                (exists && node->child_count > 0 && node->children[0]->type == AST_BINARY_OP &&
                 strcmp(node->children[0]->name, "+") == 0 &&
                 node->children[0]->value_type == COBRA_TYPE_STRING)) {
                IRLocal *local = find_local_entry(ctx, node->name);
                if (local) {
                    local->owned = true;
                    local->freed = false;
                    local->moved = false;
                }
            }
            break;
        }
        case AST_RETURN: {
            ASTNode *return_value = node->child_count > 0 ? node->children[0] : NULL;
            CobraTypeKind actual = return_value ? infer_expr(return_value, ctx) : COBRA_TYPE_VOID;
            /* `return fallible()?` returns the original failure sum and wraps
               the successful payload in the current function's sum type. */
            if (return_value && return_value->type == AST_FUNC_CALL &&
                return_value->propagate_error) {
                ASTNode *callee = find_function(ctx, return_value->name);
                if (callee && (callee->declared_type == COBRA_TYPE_OPTION ||
                               callee->declared_type == COBRA_TYPE_RESULT) &&
                    callee->declared_type == ctx->return_type) {
                    actual = ctx->return_type;
                }
            }
            if (actual == COBRA_TYPE_TENSOR_F32 && return_value) {
                if (return_value->type == AST_VAR_REF) {
                    IRLocal *returned = find_local_entry(ctx, return_value->name);
                    if (returned && returned->borrowed_from[0]) {
                        ir_error(ctx, node, "returning a borrowed tensor view would escape its owner");
                    }
                } else if (return_value->type == AST_FUNC_CALL) {
                    ASTNode *callee = find_function(ctx, return_value->name);
                    if (!callee || callee->declared_type != COBRA_TYPE_TENSOR_F32) {
                        ir_error(ctx, node, "tensor return expression must be tensor-valued");
                    }
                }
            }
            if (actual == COBRA_TYPE_F32 && ctx->return_type != COBRA_TYPE_F32) {
                ir_error(ctx, node, "an f32 return requires an explicit -> f32 function declaration");
            }
            if (actual == COBRA_TYPE_STRING && return_value && return_value->type == AST_VAR_REF) {
                IRLocal *returned = find_local_entry(ctx, return_value->name);
                if (returned && returned->owned) {
                    ir_error(ctx, node, "returning an owned local string is not supported yet; return a fresh concatenation or a borrowed parameter");
                } else if (returned && returned->borrowed_from[0]) {
                    ir_error(ctx, node, "returning a forwarded string alias would escape its owner");
                }
            }
            if (is_slice_type(actual) && return_value && return_value->type == AST_VAR_REF) {
                IRLocal *returned = find_local_entry(ctx, return_value->name);
                if (returned && returned->region_backed) {
                    ir_error(ctx, node, "returning a region-backed buffer would dangle after the region releases it");
                }
                if (returned && returned->borrowed_from[0]) {
                    ir_error(ctx, node, "returning a borrowed buffer would escape its owner");
                }
            }
            if (actual == COBRA_TYPE_STRUCT && return_value &&
                return_value->type == AST_VAR_REF) {
                IRLocal *returned = find_local_entry(ctx, return_value->name);
                IRStruct *returned_type = returned ? find_struct(ctx, canonical_type_name(returned->canonical_type)) : NULL;
                if (returned && returned->borrowed_from[0]) {
                    ir_error(ctx, node, "returning a borrowed struct would escape its owner");
                } else if (returned_type) {
                    if (returned && returned->region_expired) {
                        ir_error(ctx, node, "returning a struct with a region-backed borrowed slice would escape its region");
                    } else if (returned_type->has_borrowed_fields) {
                        ir_error(ctx, node, "returning a struct with borrowed slice fields is not supported yet");
                    }
                }
            }
            if (ctx->return_type == COBRA_TYPE_ENUM && actual == COBRA_TYPE_ENUM &&
                ctx->return_type_name[0] != '\0' && return_value && cobra_type_node_name(return_value)[0] != '\0' &&
                strcmp(ctx->return_type_name, cobra_type_node_name(return_value)) != 0) {
                ir_error(ctx, node, "cannot return a value from a different enum");
            }
            if (ctx->return_type != COBRA_TYPE_UNTYPED && ctx->return_type != COBRA_TYPE_UNKNOWN &&
                actual != COBRA_TYPE_UNKNOWN && ctx->return_type != actual &&
                !(actual == COBRA_TYPE_NONE && ctx->return_type == COBRA_TYPE_OPTION) &&
                !(is_integer(ctx->return_type) && is_integer(actual))) {
                char message[180];
                snprintf(message, sizeof(message), "return type is %s, expected %s",
                         type_name(actual), type_name(ctx->return_type));
                ir_error(ctx, node, message);
            }
            break;
        }
        case AST_WITH_REGION: {
            /* `with region NAME(capacity): body` bumps allocations out of one
               backing store and releases it exactly once after the body. */
            if (node->name[0] == '\0') {
                ir_error(ctx, node, "region requires a name");
                break;
            }
            if (ctx->region_depth >= 16) {
                ir_error(ctx, node, "too many nested regions");
                break;
            }
            if (node->child_count < 1) break;
            CobraTypeKind cap_type = infer_expr(node->children[0], ctx);
            if (!is_integer(cap_type) && cap_type != COBRA_TYPE_UNKNOWN)
                ir_error(ctx, node, "region capacity must be an integer");
            snprintf(ctx->regions[ctx->region_depth], COBRA_MAX_IDENT_LEN, "%.63s", node->name);
            ctx->region_ids[ctx->region_depth] = ctx->next_region_id++;
            ctx->region_depth++;
            if (node->child_count > 1) validate_block(node->children[1], ctx);
            expire_region_borrows(ctx, ctx->region_ids[ctx->region_depth - 1]);
            ctx->region_depth--;
            break;
        }
        case AST_IF_STMT:
        case AST_WHILE_STMT: {
            if (node->child_count > 0) (void)infer_expr(node->children[0], ctx);
            IRLocal saved_locals[128];
            size_t saved_count = ctx->count;
            memcpy(saved_locals, ctx->locals, sizeof(saved_locals));
            if (node->child_count > 1) {
                size_t branch_base = saved_count;
                validate_block(node->children[1], ctx);
                merge_branch_borrows(saved_locals, &saved_count, ctx->locals, ctx->count, branch_base);
                memcpy(ctx->locals, saved_locals, sizeof(saved_locals));
                ctx->count = saved_count;
            }
            if (node->child_count > 2) {
                size_t branch_base = saved_count;
                validate_block(node->children[2], ctx);
                merge_branch_borrows(saved_locals, &saved_count, ctx->locals, ctx->count, branch_base);
                memcpy(ctx->locals, saved_locals, sizeof(saved_locals));
                ctx->count = saved_count;
            }
            break;
        }
        case AST_FOR_LOOP:
            /* []f32/list element loops, Python-style range loops, and
               enumerate loops all lower to native counters. */
            IRLocal saved_locals[128];
            size_t saved_count = ctx->count;
            memcpy(saved_locals, ctx->locals, sizeof(saved_locals));
            CobraTypeKind iterator_type = COBRA_TYPE_I64;
            ASTNode *target = node->child_count > 0 ? node->children[0] : NULL;
            if (target) {
                CobraTypeKind target_type = infer_expr(target, ctx);
                if (target->type == AST_FUNC_CALL && strcmp(target->name, "range") == 0) {
                    if (target_type != COBRA_TYPE_I64) ir_error(ctx, node, "range loop source must be integer-valued");
                } else if (target->type == AST_FUNC_CALL && strcmp(target->name, "enumerate") == 0) {
                    if (target->child_count != 1 || target->children[0]->type != AST_VAR_REF) {
                        ir_error(ctx, node, "enumerate loop source must be a named collection");
                    } else {
                        IRLocal *source = find_local_entry(ctx, target->children[0]->name);
                        if (source && ((source->type == COBRA_TYPE_LIST || source->type == COBRA_TYPE_ARRAY) &&
                                       canonical_element_kind(source->canonical_type) == COBRA_TYPE_F32))
                            iterator_type = COBRA_TYPE_F32;
                        else if (source && source->type == COBRA_TYPE_SLICE_F32)
                            iterator_type = COBRA_TYPE_F32;
                    }
                } else if (target->type == AST_VAR_REF) {
                    IRLocal *source = find_local_entry(ctx, target->name);
                    if (source && ((source->type == COBRA_TYPE_LIST || source->type == COBRA_TYPE_ARRAY) &&
                                   canonical_element_kind(source->canonical_type) == COBRA_TYPE_F32))
                        iterator_type = COBRA_TYPE_F32;
                    else if (source && source->type == COBRA_TYPE_SLICE_F32)
                        iterator_type = COBRA_TYPE_F32;
                }
            }            if (node->secondary_name[0] != '\0' &&
                !(target && target->type == AST_FUNC_CALL && strcmp(target->name, "enumerate") == 0)) {
                ir_error(ctx, node, "two loop targets require enumerate(collection)");
            }
            /* enumerate has two semantic locals: the primary target is always the
               integer index, while the secondary target carries the source element. */
            CobraTypeKind primary_iterator_type = node->secondary_name[0] != '\0' ?
                COBRA_TYPE_I64 : iterator_type;
      if (!add_local(ctx, node->name, primary_iterator_type, NULL)) {
          char message[160];
          snprintf(message, sizeof(message), "duplicate iterator '%s'", node->name);
          ir_error(ctx, node, message);
      }
      if (node->secondary_name[0] != '\0' &&
          !add_local(ctx, node->secondary_name, iterator_type, NULL)) {
                char message[160];
                snprintf(message, sizeof(message), "duplicate iterator '%s'", node->secondary_name);
                ir_error(ctx, node, message);
            }
            if (node->child_count > 1) {
                size_t branch_base = ctx->count;
                validate_block(node->children[1], ctx);
                merge_branch_borrows(saved_locals, &saved_count, ctx->locals, ctx->count, branch_base);
            }
            memcpy(ctx->locals, saved_locals, sizeof(saved_locals));
            ctx->count = saved_count;
            break;
        case AST_COMPUTE_BLOCK:
        case AST_PARALLEL_BLOCK: {
            IRLocal saved_locals[128];
            size_t saved_count = ctx->count;
            memcpy(saved_locals, ctx->locals, sizeof(saved_locals));
            if (node->child_count > 0) {
                size_t branch_base = saved_count;
                validate_block(node->children[0], ctx);
                merge_branch_borrows(saved_locals, &saved_count, ctx->locals, ctx->count, branch_base);
            }
            memcpy(ctx->locals, saved_locals, sizeof(saved_locals));
            ctx->count = saved_count;
            break;
        }
        case AST_PRINT_STMT:
        case AST_ASSERT_STMT:
            if (node->child_count > 0) {
                reject_illegal_float_context(node->children[0], ctx,
                                             "scalar f32 expressions are not yet supported");
                (void)infer_expr(node->children[0], ctx);
            }
            break;
        case AST_FUNC_CALL:
            if (node->propagate_error) {
                ASTNode *propagated_function = find_function(ctx, node->name);
                bool typed_sum = propagated_function &&
                    (propagated_function->declared_type == COBRA_TYPE_OPTION ||
                     propagated_function->declared_type == COBRA_TYPE_RESULT);
                bool integer_status = propagated_function &&
                    (propagated_function->declared_type == COBRA_TYPE_UNTYPED ||
                     propagated_function->declared_type == COBRA_TYPE_UNKNOWN ||
                     is_integer(propagated_function->declared_type));
                /* Imported C calls use the existing integer/pointer bridge and
                   therefore have the same native status contract. */
                if (!integer_status && !typed_sum && is_imported_function(ctx, node->name)) integer_status = true;
                if (!integer_status && !typed_sum) {
                    ir_error(ctx, node, "postfix '?' requires an integer status, Option, or Result call");
                }
                if (typed_sum) {
                    bool same_sum = ctx->return_type == propagated_function->declared_type;
                    bool same_value = ctx->return_payload_type == canonical_element_kind(propagated_function->canonical_type);
                    bool same_error = propagated_function->declared_type != COBRA_TYPE_RESULT ||
                                      ctx->return_error_type == canonical_error_kind(propagated_function->canonical_type);
                    if (!same_sum || !same_value || !same_error) {
                        ir_error(ctx, node, "postfix '?' requires the callee and current function to use the same sum type");
                    }
                } else if (!is_integer(ctx->return_type)) {
                    ir_error(ctx, node, "postfix '?' requires the current function to return an integer status");
                }
            }
            if (is_string_free_builtin(node->name)) {
                if (node->child_count == 1 && node->children[0]->type == AST_VAR_REF) {
                    IRLocal *local = find_local_entry(ctx, node->children[0]->name);
                    if (!local || local->type != COBRA_TYPE_STRING || !local->owned) {
                        ir_error(ctx, node, "string_free requires an owned string");
                    } else if (local->freed) {
                        ir_error(ctx, node, "double string free detected");
                    } else {
                        for (size_t i = 0; i < ctx->count; i++) {
                            if (strcmp(ctx->locals[i].borrowed_from, local->name) == 0 && !ctx->locals[i].freed) {
                                ir_error(ctx, node, "cannot free a string source while a forwarded alias is live");
                            }
                        }
                        local->freed = true;
                    }
                } else {
                    ir_error(ctx, node, "string_free requires a named string variable");
                }
            } else if (strcmp(node->name, "free") == 0) {
                if (node->child_count == 1 && node->children[0]->type == AST_VAR_REF) {
                    IRLocal *local = find_local_entry(ctx, node->children[0]->name);
                    if (local && local->region_backed) {
                        ir_error(ctx, node, "cannot free a region-backed buffer; the region releases it");
                    } else if (local &&
                               (local->flow_mutability == COBRA_MUTABILITY_READONLY ||
                                (local->canonical_type &&
                                 local->canonical_type->ownership == COBRA_OWNERSHIP_BORROWED))) {
                        ir_error(ctx, node, "cannot free a borrowed readonly buffer");
                    } else if (!local || (!is_slice_type(local->type) && !is_collection_type(local->type)) ||
                        (!local->owned && !local->borrowed_from[0])) {
                        ir_error(ctx, node, "free requires an owned collection, buffer, or a named tensor view");
                    } else if (local->freed) {
                        ir_error(ctx, node, "double free detected");
                    } else if (local_has_live_struct_borrow(ctx, local->name)) {
                        ir_error(ctx, node, "cannot free a buffer while a struct byte-view field is live");
                    } else if (local->borrowed_from[0]) {
                        /* Releasing a view ends the borrow; it must never call
                           libc free on the source allocation. */
                        local->freed = true;
                    } else {
                        for (size_t i = 0; i < ctx->count; i++) {
                            if (strcmp(ctx->locals[i].borrowed_from, local->name) == 0 && !ctx->locals[i].freed) {
                                ir_error(ctx, node, "cannot free a tensor source while a derived view is live");
                            }
                        }
                        local->freed = true;
                    }
                } else {
                    ir_error(ctx, node, "free requires a named owned buffer");
                }
            } else {
                (void)infer_expr(node, ctx);
                if (node->fresh_string_result) {
                    ir_error(ctx, node, "discarded fresh string result would leak; assign it or explicitly release it");
                }
            }
            break;
        case AST_BINARY_OP:
            (void)infer_expr(node, ctx);
            if (node->fresh_string_result) {
                ir_error(ctx, node, "discarded fresh string result would leak; assign it or explicitly release it");
            }
            break;
        case AST_ARRAY_LITERAL:
            for (size_t i = 0; i < node->child_count; i++) {
                (void)infer_expr(node->children[i], ctx);
            }
            break;
        case AST_INSPECT_STMT:
        case AST_ASM_BLOCK:
        case AST_COMPTIME_EXPR:
            for (size_t i = 0; i < node->child_count; i++) {
                (void)infer_expr(node->children[i], ctx);
            }
            break;
        default:
            break;
    }
}

bool cobra_ir_build(ASTNode *root, CobraIR *ir) {
    if (!root || !ir) return false;
    ir->root = root;
    ir->valid = true;
    ir->error_count = 0;
    IRContext root_context = {0};
    root_context.root = root;
    if (!root->canonical_arena) {
        root->canonical_arena = (CobraTypeArena *)calloc(1, sizeof(CobraTypeArena));
        if (!root->canonical_arena) return false;
        cobra_type_arena_init(root->canonical_arena);
    }
    root_context.canonical_arena = root->canonical_arena;

    /* Resolve every concrete generic-struct use before the shared layout
       prepass. Generic function declarations remain templates and are handled
       by the existing scalar function-specialization path. */
    for (size_t i = 0; i < root->child_count; i++) {
        ASTNode *declaration = root->children[i];
        if (declaration->type == AST_FUNCTION && declaration->generic_param_count > 0)
            continue;
        if (declaration->type == AST_STRUCT_DECL && declaration->generic_param_count > 0)
            continue;
        specialize_struct_types_in_tree(&root_context, declaration);
    }

    /* Source imports are composed into one flat native namespace. Reject
       duplicate aliases and definitions before type validation so resolution
       cannot depend on declaration order and codegen cannot emit duplicate labels. */
    for (size_t i = 0; i < root->child_count; i++) {
        ASTNode *import = root->children[i];
        if (import->type != AST_IMPORT_DECL || !import->source_import || import->module_alias[0] == '\0') continue;
        for (size_t j = 0; j < i; j++) {
            ASTNode *previous = root->children[j];
            if (previous->type == AST_IMPORT_DECL && previous->source_import &&
                previous->module_alias[0] != '\0' &&
                strcmp(previous->module_alias, import->module_alias) == 0) {
                char message[180];
                snprintf(message, sizeof(message), "duplicate source module alias '%s'", import->module_alias);
                ir_error(&root_context, import, message);
                ir->error_count++;
                break;
            }
        }
    }

    for (size_t i = 0; i < root->child_count; i++) {
        ASTNode *function = root->children[i];
        if (function->type != AST_FUNCTION) continue;
        for (size_t j = 0; j < i; j++) {
            ASTNode *previous = root->children[j];
            if (previous->type == AST_FUNCTION && strcmp(previous->name, function->name) == 0) {
                char message[180];
                snprintf(message, sizeof(message), "duplicate function '%s' in composed program", function->name);
                ir_error(&root_context, function, message);
                ir->error_count++;
                break;
            }
        }
    }

    /* Validate top-level struct layouts once. Function-local contexts reuse the
       resulting metadata without repeating the same unsupported-field error for
       every composed library function. */
    for (size_t i = 0; i < root->child_count; i++) {
        if (root->children[i]->type == AST_ENUM_DECL)
            register_enum_decl(&root_context, root->children[i]);
        else if (root->children[i]->type == AST_STRUCT_DECL)
            register_struct_decl(&root_context, root->children[i], true);
    }
    ir->error_count += root_context.errors;

    for (size_t i = 0; i < root->child_count; i++) {
        ASTNode *function = root->children[i];
        if (function->type != AST_FUNCTION || function->generic_param_count > 0) continue;

        IRContext ctx = {0};
        ctx.current_function = function;
        ctx.root = root;
        ctx.canonical_arena = root->canonical_arena;
        ctx.return_type = function->declared_type;
        if (function->declared_type == COBRA_TYPE_F64) {
            ir_error(&ctx, function, "f64 is reserved until native double-precision lowering is implemented");
        }
        /* Register every top-level struct into this function's context so
           member access and struct declarations validate against one layout. */
        for (size_t j = 0; j < root->child_count; j++) {
            if (root->children[j]->type == AST_ENUM_DECL) {
                register_enum_decl(&ctx, root->children[j]);
            } else if (root->children[j]->type == AST_STRUCT_DECL) {
                register_struct_decl(&ctx, root->children[j], false);
            }
        }
        if (function->declared_type == COBRA_TYPE_STRUCT && find_enum(&ctx, cobra_type_node_name(function))) {
            function->declared_type = COBRA_TYPE_ENUM;
            function->canonical_type = cobra_type_make(ctx.canonical_arena,
                                                       COBRA_TYPE_ENUM, cobra_type_node_name(function),
                                                       NULL, NULL, NULL, NULL,
                                                       COBRA_OWNERSHIP_VALUE,
                                                       COBRA_MUTABILITY_DEFAULT, -1);
        }
        for (size_t j = 0; j < function->child_count; j++) {
            ASTNode *param = function->children[j];
            if (param->type == AST_PARAM && param->declared_type == COBRA_TYPE_STRUCT &&
                find_enum(&ctx, cobra_type_node_name(param))) {
                param->declared_type = COBRA_TYPE_ENUM;
                param->canonical_type = cobra_type_make(ctx.canonical_arena,
                                                       COBRA_TYPE_ENUM, cobra_type_node_name(param),
                                                       NULL, NULL, NULL, NULL,
                                                       COBRA_OWNERSHIP_VALUE,
                                                       COBRA_MUTABILITY_DEFAULT, -1);
            }
        }
        ctx.return_type = function->declared_type;
        ctx.return_payload_type = canonical_element_kind(function->canonical_type);
        ctx.return_error_type = canonical_error_kind(function->canonical_type);
        snprintf(ctx.return_type_name, sizeof(ctx.return_type_name), "%.63s", cobra_type_node_name(function));
        snprintf(ctx.return_error_type_name, sizeof(ctx.return_error_type_name), "%.63s", cobra_type_node_error_name(function));
        for (size_t j = 0; j < function->child_count; j++) {
            ASTNode *child = function->children[j];
            if (child->type == AST_PARAM) {
                CobraTypeKind type = child->declared_type == COBRA_TYPE_UNTYPED ? COBRA_TYPE_I64 : child->declared_type;
                if (type == COBRA_TYPE_F64) {
                    ir_error(&ctx, child, "f64 is reserved until native double-precision lowering is implemented");
                }
                if (type == COBRA_TYPE_OPTION || type == COBRA_TYPE_RESULT) {
                    if (!is_scalar_sum_component(canonical_element_kind(child->canonical_type))) {
                        ir_error(&ctx, child, "Option and Result parameters currently require scalar value payloads");
                    }
                    if (type == COBRA_TYPE_RESULT &&
                        !is_scalar_sum_component(canonical_error_kind(child->canonical_type))) {
                        ir_error(&ctx, child, "Result parameters currently require a scalar error payload");
                    }
                }
                if (type == COBRA_TYPE_STRUCT) {
                    IRStruct *struct_type = find_struct(&ctx, cobra_type_node_name(child));
                    if (!struct_type) {
                        char message[180];
                        snprintf(message, sizeof(message), "unknown struct type '%s' for parameter '%s'",
                                 cobra_type_node_name(child), child->name);
                        ir_error(&ctx, child, message);
                    } else if (struct_type->invalid_layout) {
                        /* The layout diagnostic was emitted once during the
                           root prepass. Do not add zero-size or alignment
                           cascades for the same invalid declaration. */
                    } else {
                        for (int field_index = 0; field_index < struct_type->field_count; field_index++) {
                            if (struct_type->fields[field_index].type == COBRA_TYPE_STRING) {
                                char message[220];
                                snprintf(message, sizeof(message),
                                         "struct parameter '%s' contains ownership-bearing field '%s'; "
                                         "only scalar fields are supported",
                                         child->name, struct_type->fields[field_index].name);
                                ir_error(&ctx, child, message);
                                break;
                            }
                        }
                        if (struct_type->total_size == 0) {
                            char message[180];
                            snprintf(message, sizeof(message),
                                     "struct parameter '%s' has a zero-byte layout; add a scalar field",
                                     child->name);
                            ir_error(&ctx, child, message);
                        } else if ((struct_type->total_size & 7) != 0) {
                            char message[220];
                            snprintf(message, sizeof(message),
                                     "struct parameter '%s' has invalid %d-byte alignment; native parameters require 8-byte alignment",
                                     child->name, struct_type->total_size);
                            ir_error(&ctx, child, message);
                        }
                    }
                }
            if (!add_local(&ctx, child->name, type, child)) {
                char message[160];
                    snprintf(message, sizeof(message), "duplicate parameter '%s'", child->name);
                    ir_error(&ctx, child, message);
                } else {
                    IRLocal *parameter_local = find_local_entry(&ctx, child->name);
                    if (parameter_local) {
                        parameter_local->is_parameter = true;
                        if (type == COBRA_TYPE_STRUCT) {
                            IRStruct *parameter_type = find_struct(&ctx, cobra_type_node_name(child));
                            if (parameter_type) {
                                parameter_local->struct_field_initialized =
                                    parameter_type->field_count >= 64 ? ~0ULL :
                                    ((1ULL << parameter_type->field_count) - 1ULL);
                            }
                        }
                    }
                }
            }
        }
        for (size_t j = 0; j < function->child_count; j++) {
            if (function->children[j]->type != AST_PARAM) {
                validate_statement(function->children[j], &ctx);
            }
        }
        check_canonical_tree(&ctx, function);
        ir->error_count += ctx.errors;
    }

    ir->valid = ir->error_count == 0;
    if (!ir->valid) {
        fprintf(stderr, "IR rejected program with %zu error(s)\n", ir->error_count);
    }
    return ir->valid;
}
