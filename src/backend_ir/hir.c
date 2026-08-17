/*
 * Cobra Backend IR: typed HIR / CFG builder.
 *
 * Consumes the existing parser AST for a small scalar subset and produces a
 * CFG with source-level mutable locals. No SSA is constructed here; the SSA
 * pass (ssa_pass.c) consumes this HIR. See docs/BACKEND_IR.md.
 */
#include "ssa.h"
#include <limits.h>
#include <string.h>

#define BIR_MAX_ARRAY_UNROLL 64

typedef struct {
    char name[COBRA_MAX_IDENT_LEN];
    uint32_t id;
} BirRegionScope;

typedef struct {
    BackendIrModule *module;
    ASTNode *root;
    HirFunction *fn;
    HirBlockRef current;
    int synthetic_seq;
    bool failed;
    BirRegionScope region_stack[BIR_MAX_REGIONS];
    size_t region_depth;
} HirBuilder;

static const CobraType *bir_import_source_struct(BackendIrModule *module,
                                                  const CobraType *source);
static const CobraType *bir_import_sum_component(BackendIrModule *module,
                                                 const CobraType *component);

static int hir_find_region(HirBuilder *b, const char *name) {
    for (size_t i = b->region_depth; i > 0; i--) {
        if (strcmp(b->region_stack[i - 1].name, name) == 0)
            return (int)(i - 1);
    }
    return -1;
}

/* The backend owns its canonical arena. AST descriptors are consulted only
   at the import boundary, then mapped to finalized descriptors in the backend
   arena so HIR/SSA never retain frontend-arena pointers. */
static bool bir_is_scalar_type(const CobraType *type, const BackendIrModule *module) {
    (void)module;
    return type && (type->kind == COBRA_TYPE_I64 ||
                    type->kind == COBRA_TYPE_I32 ||
                    type->kind == COBRA_TYPE_U32 ||
                    type->kind == COBRA_TYPE_U64 ||
                    type->kind == COBRA_TYPE_BOOL ||
                    type->kind == COBRA_TYPE_F32 ||
                    type->kind == COBRA_TYPE_F64 ||
                    type->kind == COBRA_TYPE_U8);
}

static const CobraType *bir_find_aggregate(const BackendIrModule *module,
                                           const char *name) {
    if (!module || !name) return NULL;
    for (size_t i = 0; i < module->aggregate_count; i++) {
        if (strcmp(module->aggregates[i].name, name) == 0)
            return module->aggregates[i].type;
    }
    return NULL;
}

/* Map a canonical fixed-array element into the backend type arena. Scalars
   map to module scalars; nested arrays recurse so array-of-array fields and
   parameters keep canonical inline layout. Owned or borrowed element kinds
   stay unmapped until their ownership rules are defined. */
static const CobraType *bir_import_array_element(BackendIrModule *module,
                                                 const CobraType *element) {
    if (!module || !element) return NULL;
    switch (element->kind) {
        case COBRA_TYPE_I64: return module->type_i64;
        case COBRA_TYPE_I32: return module->type_i32;
        case COBRA_TYPE_U32: return module->type_u32;
        case COBRA_TYPE_U64: return module->type_u64;
        case COBRA_TYPE_BOOL: return module->type_bool;
        case COBRA_TYPE_F32: return module->type_f32;
        case COBRA_TYPE_F64: return module->type_f64;
        case COBRA_TYPE_U8: return module->type_u8;
        case COBRA_TYPE_ARRAY: {
            const CobraType *inner = element->generic_arg_count == 1
                ? element->generic_args[0] : NULL;
            const CobraType *backend_inner = bir_import_array_element(module, inner);
            return backend_inner
                ? bir_array_type(module, backend_inner, element->array_length) : NULL;
        }
        case COBRA_TYPE_STRUCT: {
            const CobraType *imported = bir_import_source_struct(module, element);
            return imported && bir_type_is_value_only_struct(imported)
                ? imported : NULL;
        }
        default:
            return NULL;
    }
}

static const CobraType *bir_import_source_struct(BackendIrModule *module,
                                                  const CobraType *source) {
    if (!module || !source || source->kind != COBRA_TYPE_STRUCT ||
        !source->finalized || !source->name[0] ||
        source->ownership != COBRA_OWNERSHIP_VALUE ||
        source->mutability != COBRA_MUTABILITY_DEFAULT ||
        module->aggregate_count >= BIR_MAX_AGGREGATES) return NULL;
    const CobraType *existing = bir_find_aggregate(module, source->name);
    if (existing) return existing;
    if (source->field_count == 0) return NULL;

    /* Backend layouts may widen ownership-bearing source fields. For example,
       Cobra string syntax becomes an owned pointer-plus-length value here.
       The backend descriptor is therefore authoritative for this lowering. */
    CobraType *copy = cobra_type_make(module->type_arena, COBRA_TYPE_STRUCT,
                                      source->name, NULL, NULL, NULL, NULL,
                                      COBRA_OWNERSHIP_VALUE,
                                      COBRA_MUTABILITY_DEFAULT, -1);
    if (!copy) return NULL;
    if (source->template_origin && source->generic_arg_count == 1) {
        const CobraType *argument = source->generic_args[0];
        const CobraType *backend_argument = NULL;
        switch (argument ? argument->kind : COBRA_TYPE_UNKNOWN) {
            case COBRA_TYPE_I64: backend_argument = module->type_i64; break;
            case COBRA_TYPE_I32: backend_argument = module->type_i32; break;
            case COBRA_TYPE_U32: backend_argument = module->type_u32; break;
            case COBRA_TYPE_U64: backend_argument = module->type_u64; break;
            case COBRA_TYPE_F32: backend_argument = module->type_f32; break;
            case COBRA_TYPE_F64: backend_argument = module->type_f64; break;
            case COBRA_TYPE_U8: backend_argument = module->type_u8; break;
            case COBRA_TYPE_BOOL: backend_argument = module->type_bool; break;
            default: break;
        }
        if (!backend_argument || !cobra_type_add_generic_arg(copy, backend_argument))
            return NULL;
    }
    for (size_t i = 0; i < source->field_count; i++) {
        const CobraTypeField *field = &source->fields[i];
        const CobraType *backend_field = NULL;
        switch (field->type->kind) {
            case COBRA_TYPE_I64: backend_field = module->type_i64; break;
            case COBRA_TYPE_I32: backend_field = module->type_i32; break;
            case COBRA_TYPE_U32: backend_field = module->type_u32; break;
            case COBRA_TYPE_U64: backend_field = module->type_u64; break;
            case COBRA_TYPE_BOOL: backend_field = module->type_bool; break;
            case COBRA_TYPE_F32: backend_field = module->type_f32; break;
            case COBRA_TYPE_F64: backend_field = module->type_f64; break;
            case COBRA_TYPE_U8: backend_field = module->type_u8; break;
            case COBRA_TYPE_ARRAY: {
                const CobraType *element = field->type->generic_arg_count == 1
                    ? field->type->generic_args[0] : NULL;
                const CobraType *backend_element = bir_import_array_element(module, element);
                backend_field = backend_element
                    ? bir_array_type(module, backend_element, field->type->array_length)
                    : NULL;
                break;
            }
            case COBRA_TYPE_STRING:
                backend_field = bir_owned_slice_type(module, module->type_u8);
                break;
            case COBRA_TYPE_SLICE:
            case COBRA_TYPE_SLICE_F32:
            case COBRA_TYPE_SLICE_U8:
                if (field->type->generic_arg_count == 1) {
                    const CobraType *element = field->type->generic_args[0];
                    const CobraType *mapped = NULL;
                    if (element->kind == COBRA_TYPE_I64) mapped = module->type_i64;
                    else if (element->kind == COBRA_TYPE_F32) mapped = module->type_f32;
                    else if (element->kind == COBRA_TYPE_U8) mapped = module->type_u8;
                    if (mapped && field->ownership == COBRA_OWNERSHIP_BORROWED &&
                        field->mutability == COBRA_MUTABILITY_READONLY &&
                        field->region_id == -1) {
                        backend_field = bir_view_type(module, mapped);
                    } else if (mapped && field->ownership == COBRA_OWNERSHIP_VALUE &&
                               field->mutability == COBRA_MUTABILITY_DEFAULT) {
                        backend_field = bir_owned_slice_type(module, mapped);
                    }
                }
                break;
            case COBRA_TYPE_LIST:
                if (field->type->generic_arg_count == 1) {
                    const CobraType *element = field->type->generic_args[0];
                    const CobraType *mapped = NULL;
                    if (element->kind == COBRA_TYPE_I64) mapped = module->type_i64;
                    else if (element->kind == COBRA_TYPE_F32) mapped = module->type_f32;
                    else if (element->kind == COBRA_TYPE_U8) mapped = module->type_u8;
                    else if (element->kind == COBRA_TYPE_STRUCT) {
                        const CobraType *imported =
                            bir_import_source_struct(module, element);
                        if (imported && bir_type_is_value_only_struct(imported))
                            mapped = imported;
                    }
                    backend_field = mapped ? bir_buffer_type(module, mapped) : NULL;
                }
                break;
            case COBRA_TYPE_OPTION:
            case COBRA_TYPE_RESULT:
                backend_field = bir_import_sum_component(module, field->type);
                break;
            case COBRA_TYPE_STRUCT:
                backend_field = bir_import_source_struct(module, field->type);
                break;
            default: break;
        }
        CobraOwnershipKind backend_ownership =
            field->ownership == COBRA_OWNERSHIP_BORROWED
                ? COBRA_OWNERSHIP_BORROWED : COBRA_OWNERSHIP_VALUE;
        CobraMutabilityKind backend_mutability =
            field->ownership == COBRA_OWNERSHIP_BORROWED
                ? field->mutability : COBRA_MUTABILITY_DEFAULT;
        if (!backend_field || !cobra_type_add_field(copy, field->name, backend_field,
                                  backend_ownership, backend_mutability,
                                  field->ownership == COBRA_OWNERSHIP_BORROWED
                                      ? field->region_id : -1)) return NULL;
    }
    if (!cobra_type_finalize(module->type_arena, copy) ||
        copy->field_count != source->field_count) return NULL;
    BirAggregateInfo *info = &module->aggregates[module->aggregate_count++];
    snprintf(info->name, sizeof(info->name), "%s", source->name);
    info->type = copy;
    return copy;
}

static const CobraType *bir_import_ast_type(BackendIrModule *module,
                                             ASTNode *root,
                                             const ASTNode *node,
                                             bool allow_untyped);

/* Import one payload type of a payload-carrying enum variant. A single
   payload imports directly; multiple payloads synthesize a struct whose
   fields are the imported payload types in order (Shape.Rect(f32, f32)
   becomes an anonymous Rect struct). Payloads may be scalars, value-only or
   owning structs, borrowed or owned slices, strings, or nested sums; the
   owning forms ride the same aggregate move/drop machinery as owning
   Option/Result payloads. */
static const CobraType *bir_import_enum_payload(BackendIrModule *module,
                                                const char *enum_name,
                                                ASTNode *variant) {
    if (!module || !variant || variant->child_count == 0) return NULL;
    if (variant->child_count == 1) {
        const CobraType *source = variant->children[0]->canonical_type;
        if (!source) return NULL;
        /* Struct payload annotations are not finalized at enum-import time;
           resolve the canonical layout like bir_import_ast_type does so the
           descriptor has real fields before the aggregate import. */
        if (source->kind == COBRA_TYPE_STRUCT && !source->finalized) {
            ASTNode *root = module->source_root;
            if (!root || !root->canonical_arena || !source->name[0]) return NULL;
            source = cobra_type_struct_layout(root->canonical_arena, root,
                                              source->name);
        }
        return source ? bir_import_sum_component(module, source) : NULL;
    }
    /* Multi-field payload: synthesize a struct in the module arena whose
       fields are the imported payload types in order. Fields admit the same
       payload contracts as single payloads, including owning fields. */
    CobraType *synthetic = cobra_type_new(module->type_arena, COBRA_TYPE_STRUCT);
    if (!synthetic) return NULL;
    snprintf(synthetic->name, sizeof(synthetic->name), "%.46s.%.16s",
             enum_name ? enum_name : "Enum", variant->name);
    synthetic->ownership = COBRA_OWNERSHIP_VALUE;
    synthetic->mutability = COBRA_MUTABILITY_DEFAULT;
    synthetic->region_id = -1;
    for (size_t i = 0; i < variant->child_count; i++) {
        const CobraType *source = variant->children[i]->canonical_type;
        const CobraType *field_type = source
            ? bir_import_sum_component(module, source) : NULL;
        if (!field_type) return NULL;
        char field_name[COBRA_MAX_IDENT_LEN];
        snprintf(field_name, sizeof(field_name), "payload%zu", i);
        if (!cobra_type_add_field(synthetic, field_name, field_type,
                                  COBRA_OWNERSHIP_VALUE,
                                  COBRA_MUTABILITY_DEFAULT, -1)) return NULL;
    }
    if (!cobra_type_finalize(module->type_arena, synthetic)) return NULL;
    return synthetic;
}

/* Register an enum declaration into the backend module: a canonical
   COBRA_TYPE_ENUM descriptor plus its variant discriminants. Payload-carrying
   variants attach one component per variant to the canonical descriptor as
   generic arguments, so the tagged-sum machinery (aggregate copies, match
   extraction, ABI) treats the enum uniformly with Option/Result. */
static bool bir_import_source_enum(BackendIrModule *module, ASTNode *decl) {
    if (!module || !decl || decl->type != AST_ENUM_DECL || !decl->name[0] ||
        decl->child_count == 0 || decl->child_count > COBRA_MAX_ENUM_VARIANTS ||
        module->enum_count >= BIR_MAX_ENUMS) return false;
    if (bir_find_enum(module, decl->name)) return true;
    bool has_payloads = false;
    const CobraType *payloads[COBRA_MAX_ENUM_VARIANTS];
    for (size_t i = 0; i < decl->child_count; i++) {
        ASTNode *variant = decl->children[i];
        if (variant->type != AST_PARAM || !variant->name[0]) return false;
        if (variant->child_count > 0) {
            has_payloads = true;
            payloads[i] = bir_import_enum_payload(module, decl->name, variant);
            if (!payloads[i]) return false;
        } else {
            payloads[i] = NULL;
        }
    }
    /* Build the canonical descriptor. Payload enums get a fresh unfinalized
       node so the variant components attach before ABI finalization; unit
       enums keep the existing interned scalar path. */
    const CobraType *type = NULL;
    if (has_payloads) {
        CobraType *fresh = cobra_type_new(module->type_arena, COBRA_TYPE_ENUM);
        if (!fresh) return false;
        snprintf(fresh->name, sizeof(fresh->name), "%s", decl->name);
        fresh->ownership = COBRA_OWNERSHIP_VALUE;
        fresh->mutability = COBRA_MUTABILITY_DEFAULT;
        fresh->region_id = -1;
        for (size_t i = 0; i < decl->child_count; i++) {
            if (!cobra_type_add_variant_payload(fresh, payloads[i])) return false;
        }
        if (!cobra_type_finalize(module->type_arena, fresh)) return false;
        type = fresh;
    } else {
        type = bir_enum_type(module, decl->name);
        if (!type) return false;
    }
    BirEnumInfo *info = &module->enums[module->enum_count++];
    memset(info, 0, sizeof(*info));
    snprintf(info->name, sizeof(info->name), "%s", decl->name);
    info->type = type;
    info->has_payloads = has_payloads;
    for (size_t i = 0; i < decl->child_count; i++) {
        ASTNode *variant = decl->children[i];
        snprintf(info->variant_names[info->variant_count],
                 sizeof(info->variant_names[info->variant_count]), "%s",
                 variant->name);
        info->variant_values[info->variant_count] = (int)variant->int_val;
        info->variant_payloads[info->variant_count] = payloads[i];
        info->variant_payload_arity[info->variant_count] =
            variant->child_count;
        info->variant_count++;
    }
    return true;
}

/* Recursively import an Option/Result component into the backend arena:
   scalars map to the module scalar descriptors, and nested sums rebuild
   their own backend descriptors so HIR/SSA never retain frontend-arena
   pointers. */
static const CobraType *bir_import_sum_component(BackendIrModule *module,
                                                 const CobraType *component) {
    if (!module || !component) return NULL;
    switch (component->kind) {
        case COBRA_TYPE_I64: return module->type_i64;
        case COBRA_TYPE_I32: return module->type_i32;
        case COBRA_TYPE_U32: return module->type_u32;
        case COBRA_TYPE_U64: return module->type_u64;
        case COBRA_TYPE_F32: return module->type_f32;
        case COBRA_TYPE_F64: return module->type_f64;
        case COBRA_TYPE_U8: return module->type_u8;
        case COBRA_TYPE_BOOL: return module->type_bool;
        case COBRA_TYPE_STRING:
            return bir_owned_slice_type(module, module->type_u8);
        case COBRA_TYPE_STRUCT: {
            /* Owning struct payloads (structs carrying owned strings,
               slices, or sums) and value-only structs both lower through
               the aggregate machinery; the import boundary validates the
               field contract. */
            return bir_import_source_struct(module, component);
        }
        case COBRA_TYPE_SLICE:
        case COBRA_TYPE_SLICE_F32:
        case COBRA_TYPE_SLICE_U8: {
            const CobraType *element = component->generic_arg_count == 1
                ? component->generic_args[0] : NULL;
            const CobraType *backend_element = element
                ? bir_import_sum_component(module, element) : NULL;
            if (!backend_element || bir_is_owned_slice_type(backend_element)) return NULL;
            return bir_owned_slice_type(module, backend_element);
        }
        case COBRA_TYPE_LIST: {
            const CobraType *element = component->generic_arg_count == 1
                ? component->generic_args[0] : NULL;
            const CobraType *backend_element = element
                ? bir_import_sum_component(module, element) : NULL;
            return backend_element ? bir_buffer_type(module, backend_element) : NULL;
        }
        case COBRA_TYPE_OPTION:
        case COBRA_TYPE_RESULT: {
            if (component->generic_arg_count < 1) return NULL;
            const CobraType *element =
                bir_import_sum_component(module, component->generic_args[0]);
            if (!element) return NULL;
            const CobraType *error = NULL;
            if (component->kind == COBRA_TYPE_RESULT) {
                if (component->generic_arg_count < 2) return NULL;
                error = bir_import_sum_component(module, component->generic_args[1]);
                if (!error) return NULL;
            }
            return bir_sum_type(module, component->kind, element, error);
        }
        default: return NULL;
    }
}

static bool bir_is_string_value_type(const CobraType *type) {
    if (bir_is_owned_slice_type(type))
        return type->kind == COBRA_TYPE_SLICE_U8;
    return bir_is_borrowed_view_type(type) &&
           cobra_type_element(type) &&
           cobra_type_element(type)->kind == COBRA_TYPE_U8;
}

static bool bir_fresh_string_syntax(const ASTNode *node) {
    return node &&
           ((node->type == AST_FUNC_CALL && strcmp(node->name, "concat") == 0) ||
            (node->type == AST_BINARY_OP && strcmp(node->name, "+") == 0));
}

static bool bir_function_returns_fresh_string(const ASTNode *node,
                                              bool *saw_return) {
    if (!node) return true;
    if (node->type == AST_RETURN) {
        if (saw_return) *saw_return = true;
        return node->child_count == 1 &&
               bir_fresh_string_syntax(node->children[0]);
    }
    for (size_t i = 0; i < node->child_count; i++) {
        if (!bir_function_returns_fresh_string(node->children[i], saw_return))
            return false;
    }
    return true;
}

static bool bir_function_returns_owned_string(const ASTNode *function) {
    bool saw_return = false;
    return function && function->type == AST_FUNCTION &&
           bir_function_returns_fresh_string(function, &saw_return) && saw_return;
}

static const CobraType *bir_import_ast_type(BackendIrModule *module,
                                             ASTNode *root,
                                             const ASTNode *node,
                                             bool allow_untyped) {
    if (!module || !node) return NULL;
    const CobraType *source = node->canonical_type;
    CobraTypeKind kind = source ? source->kind : node->declared_type;
    if (kind == COBRA_TYPE_UNTYPED && allow_untyped) return module->type_i64;
    /* A bare enum name in type position parses as a struct-typed canonical
       descriptor; resolve it against the registered enum registry, exactly
       as the frontend IR pass does with find_enum. */
    if (kind == COBRA_TYPE_STRUCT && source && source->name[0] &&
        bir_find_enum(module, source->name)) {
        return bir_enum_type(module, source->name);
    }
    if (kind == COBRA_TYPE_ENUM) {
        const char *name = source && source->name[0]
            ? source->name : node->name;
        return name[0] ? bir_enum_type(module, name) : NULL;
    }
    if (kind == COBRA_TYPE_STRUCT) {
        if (!source || !source->name[0] || !root || !root->canonical_arena) return NULL;
        /* Concrete generic struct specializations are already finalized in
           the frontend arena and carry template provenance. Import that
           descriptor directly instead of looking for a parser declaration
           named Box__i64. */
        if (source->finalized && source->template_origin)
            return bir_import_source_struct(module, source);
        source = cobra_type_struct_layout(root->canonical_arena, root, source->name);
        return bir_import_source_struct(module, source);
    }
    if (source && source->kind == COBRA_TYPE_ARRAY) {
        const CobraType *element = source->generic_arg_count == 1
            ? source->generic_args[0] : NULL;
        const CobraType *backend_element = bir_import_array_element(module, element);
        return backend_element
            ? bir_array_type(module, backend_element, source->array_length) : NULL;
    }
    if (source && source->kind == COBRA_TYPE_LIST) {
        const CobraType *element = source->generic_arg_count == 1
            ? source->generic_args[0] : NULL;
        const CobraType *backend_element = NULL;
        if (element && element->kind == COBRA_TYPE_STRUCT) {
            const CobraType *imported = bir_import_source_struct(module, element);
            if (imported && bir_type_is_value_only_struct(imported))
                backend_element = imported;
        } else {
            backend_element = element
                ? bir_import_sum_component(module, element) : NULL;
        }
        return backend_element ? bir_buffer_type(module, backend_element) : NULL;
    }
    if (source && source->kind == COBRA_TYPE_DICT) {
        /* dict[string]T: the backend key is always the readonly u8 string
           view; the value must be a scalar, mirroring the production
           `dict[string]i64` contract. */
        if (source->generic_arg_count != 2 || !source->generic_args[1] ||
            !cobra_type_is_scalar(source->generic_args[1])) return NULL;
        const CobraType *value = NULL;
        switch (source->generic_args[1]->kind) {
            case COBRA_TYPE_I64: value = module->type_i64; break;
            case COBRA_TYPE_F32: value = module->type_f32; break;
            case COBRA_TYPE_U8: value = module->type_u8; break;
            default: return NULL;
        }
        return value ? bir_dict_type(module, value) : NULL;
    }
    if (source && cobra_type_is_slice_kind(source->kind)) {
        const CobraType *element = cobra_type_element(source);
        const CobraType *backend_element = NULL;
        if (element) {
            switch (element->kind) {
                case COBRA_TYPE_I64: backend_element = module->type_i64; break;
                case COBRA_TYPE_F32: backend_element = module->type_f32; break;
                case COBRA_TYPE_U8: backend_element = module->type_u8; break;
                default: break;
            }
        }
        if (!backend_element) return NULL;
        bool is_borrowed_view =
            source->ownership == COBRA_OWNERSHIP_BORROWED ||
            source->mutability == COBRA_MUTABILITY_OUT;
        if (is_borrowed_view &&
            (source->mutability == COBRA_MUTABILITY_READONLY ||
             source->mutability == COBRA_MUTABILITY_OUT)) {
            return source->mutability == COBRA_MUTABILITY_OUT
                ? bir_writable_view_type(module, backend_element)
                : bir_view_type(module, backend_element);
        }
        /* Plain mutable slices (qualifier 0) are owned slice storage. */
        if (source->ownership == COBRA_OWNERSHIP_VALUE &&
            source->mutability == COBRA_MUTABILITY_DEFAULT)
            return bir_owned_slice_type(module, backend_element);
        return NULL;
    }
    if (kind == COBRA_TYPE_STRING) {
        /* Parameters and ordinary string locals are borrowed readonly views.
           A function whose returns are fresh concat expressions uses the
           owned u8-slice representation at its return boundary. */
        if (node->type == AST_FUNCTION && bir_function_returns_owned_string(node))
            return bir_owned_slice_type(module, module->type_u8);
        return bir_view_type(module, module->type_u8);
    }
    if (kind == COBRA_TYPE_OPTION || kind == COBRA_TYPE_RESULT) {
        if (!source || source->generic_arg_count == 0) return NULL;
        const CobraType *backend_element =
            bir_import_sum_component(module, source->generic_args[0]);
        if (!backend_element) return NULL;
        const CobraType *backend_error = NULL;
        if (kind == COBRA_TYPE_RESULT) {
            if (source->generic_arg_count < 2) return NULL;
            backend_error =
                bir_import_sum_component(module, source->generic_args[1]);
            if (!backend_error) return NULL;
        }
        return bir_sum_type(module, kind, backend_element, backend_error);
    }
    switch (kind) {
        case COBRA_TYPE_I64: return module->type_i64;
        case COBRA_TYPE_I32: return module->type_i32;
        case COBRA_TYPE_U32: return module->type_u32;
        case COBRA_TYPE_U64: return module->type_u64;
        case COBRA_TYPE_F32: return module->type_f32;
        case COBRA_TYPE_F64: return module->type_f64;
        case COBRA_TYPE_U8: return module->type_u8;
        case COBRA_TYPE_BOOL: return module->type_bool;
        case COBRA_TYPE_VOID: return module->type_void;
        default: return NULL;
    }
}

static bool bir_types_equal(const CobraType *left, const CobraType *right) {
    return left && right && (left == right || cobra_type_equal(left, right));
}

/* ------------------------------------------------------------------ */
/* Diagnostics                                                        */
/* ------------------------------------------------------------------ */

static void bir_fail(HirBuilder *b, int line, int col, const char *fmt, ...) {
    if (!b->module || b->module->error[0]) return;
    char message[COBRA_MAX_TOKEN_TEXT];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    snprintf(b->module->error, sizeof(b->module->error),
             "%.500s:%d:%d: %.100s", b->module->source_file, line > 0 ? line : 1,
             col > 0 ? col : 1, message);
    b->failed = true;
}

/* ------------------------------------------------------------------ */
/* HIR building primitives                                            */
/* ------------------------------------------------------------------ */

static HirBlock *hir_new_block(HirBuilder *b, const char *name, int line, int col) {
    HirFunction *fn = b->fn;
    if (fn->block_count == fn->block_cap) {
        size_t next = fn->block_cap ? fn->block_cap * 2 : 16;
        HirBlock *grown = realloc(fn->blocks, sizeof(HirBlock) * next);
        if (!grown) {
            bir_fail(b, line, col, "out of memory building CFG");
            return NULL;
        }
        fn->blocks = grown;
        fn->block_cap = next;
    }
    HirBlock *block = &fn->blocks[fn->block_count];
    memset(block, 0, sizeof(*block));
    block->id = (HirBlockRef)fn->block_count;
    snprintf(block->name, sizeof(block->name), "%s", name);
    block->source_line = line;
    block->source_col = col;
    fn->block_count++;
    return block;
}

static bool hir_add_edge(HirBuilder *b, HirBlockRef from, HirBlockRef to) {
    HirFunction *fn = b->fn;
    if (from >= fn->block_count || to >= fn->block_count) return false;
    HirBlock *source = &fn->blocks[from];
    HirBlock *target = &fn->blocks[to];
    size_t next = source->succ_count + 1;
    if (next > source->succ_cap) {
        size_t cap = source->succ_cap ? source->succ_cap * 2 : 4;
        HirBlockRef *grown = realloc(source->succs, sizeof(HirBlockRef) * cap);
        if (!grown) return false;
        source->succs = grown;
        source->succ_cap = cap;
    }
    next = target->pred_count + 1;
    if (next > target->pred_cap) {
        size_t cap = target->pred_cap ? target->pred_cap * 2 : 4;
        HirBlockRef *grown = realloc(target->preds, sizeof(HirBlockRef) * cap);
        if (!grown) return false;
        target->preds = grown;
        target->pred_cap = cap;
    }
    source->succs[source->succ_count++] = to;
    target->preds[target->pred_count++] = from;
    return true;
}

static bool hir_block_add_stmt(HirBuilder *b, HirBlockRef block, HirStmt stmt) {
    HirBlock *target = &b->fn->blocks[block];
    if (target->term.kind != HIR_TERM_NONE) return false;
    if (target->stmt_count == target->stmt_cap) {
        size_t next = target->stmt_cap ? target->stmt_cap * 2 : 8;
        HirStmt *grown = realloc(target->stmts, sizeof(HirStmt) * next);
        if (!grown) return false;
        target->stmts = grown;
        target->stmt_cap = next;
    }
    target->stmts[target->stmt_count++] = stmt;
    return true;
}

static void hir_expr_free(HirExpr *expr);
static bool hir_emit_assign(HirBuilder *b, uint32_t local, HirExpr *expr);

static bool hir_is_aggregate_value_type(const CobraType *type) {
    return type && (type->kind == COBRA_TYPE_STRUCT ||
                    type->kind == COBRA_TYPE_ARRAY ||
                    bir_is_sum_type(type));
}

static bool hir_set_term(HirBuilder *b, HirBlockRef block, HirTerm term) {
    if (b->fn->blocks[block].term.kind != HIR_TERM_NONE) {
        hir_expr_free(term.cond);
        hir_expr_free(term.ret_expr);
        bir_fail(b, 0, 0, "internal error: block already terminated");
        return false;
    }
    b->fn->blocks[block].term = term;
    return true;
}

static HirExpr *hir_expr_alloc(HirBuilder *b, int line, int col) {
    HirExpr *expr = calloc(1, sizeof(HirExpr));
    if (!expr) {
        bir_fail(b, line, col, "out of memory building expression");
        return NULL;
    }
    expr->source_line = line;
    expr->source_col = col;
    return expr;
}

/* Recursively free an expression tree (expr owns its argument list). */
static void hir_expr_free(HirExpr *expr) {
    if (!expr) return;
    for (size_t i = 0; i < expr->arg_count; i++) hir_expr_free(expr->args[i]);
    if (expr->dict_keys) {
        for (size_t i = 0; i < expr->arg_count; i++) free(expr->dict_keys[i]);
    }
    free(expr->dict_keys);
    free(expr->args);
    free(expr);
}

/* Free a built HIR function: block storage, per-block statement and edge
   arrays, and every owned expression tree (statements and terminators). */
static void hir_function_free(HirFunction *fn) {
    if (!fn) return;
    for (size_t i = 0; i < fn->block_count; i++) {
        HirBlock *block = &fn->blocks[i];
        for (size_t j = 0; j < block->stmt_count; j++) {
            hir_expr_free(block->stmts[j].target);
            hir_expr_free(block->stmts[j].expr);
        }
        free(block->stmts);
        hir_expr_free(block->term.cond);
        hir_expr_free(block->term.ret_expr);
        free(block->preds);
        free(block->succs);
    }
    free(fn->blocks);
    fn->blocks = NULL;
    fn->block_count = 0;
    fn->block_cap = 0;
}

/* ------------------------------------------------------------------ */
/* Locals                                                             */
/* ------------------------------------------------------------------ */

static int hir_find_local(HirBuilder *b, const char *name) {
    for (size_t i = 0; i < b->fn->local_count; i++) {
        if (strcmp(b->fn->locals[i].name, name) == 0) return (int)i;
    }
    return -1;
}

static int hir_add_local(HirBuilder *b, const char *name, bool is_param,
                         const CobraType *type, int line, int col) {
    int existing = hir_find_local(b, name);
    if (existing >= 0) return existing;
    if (b->fn->local_count >= BIR_MAX_LOCALS) {
        bir_fail(b, line, col, "too many locals in backend-IR function");
        return -1;
    }
    HirLocal *local = &b->fn->locals[b->fn->local_count];
    memset(local, 0, sizeof(*local));
    snprintf(local->name, sizeof(local->name), "%s", name);
    local->type = type;
    local->is_param = is_param;
    local->source_line = line;
    local->source_col = col;
    return (int)b->fn->local_count++;
}

static int hir_require_local(HirBuilder *b, const char *name, int line, int col) {
    int existing = hir_find_local(b, name);
    if (existing < 0) {
        bir_fail(b, line, col, "use of unknown local '%s'", name);
        return -1;
    }
    return existing;
}

static int hir_synthetic_local(HirBuilder *b, const char *prefix, int line, int col) {
    char name[COBRA_MAX_IDENT_LEN];
    snprintf(name, sizeof(name), "@%s_%d", prefix, b->synthetic_seq++);
    return hir_add_local(b, name, false, b->module->type_i64, line, col);
}

static bool hir_complete_float_literal(HirBuilder *b, HirExpr *expr,
                                       const CobraType *target);
static bool hir_complete_float_expr(HirBuilder *b, HirExpr *expr,
                                    const CobraType *target);
static HirExpr *hir_coerce_int_const(HirBuilder *b, HirExpr *value,
                                     const CobraType *target);

/* Complete an Option/Result constructor whose type is not fully determined
   by its argument (none, ok, err, or some(none)-style nesting) against the
   declared or parameter type. Completion recurses into an incomplete payload
   constructor first, so some(none) resolves Option[Option[T]] from the
   expected type outward. */
static bool hir_complete_sum_type(HirBuilder *b, HirExpr *expr,
                                  const CobraType *expected) {
    if (!expr || expr->kind != HIR_EXPR_SUM_MAKE) return true;
    if (expr->type) {
        if (!expected) return true;
        /* A fully determined some(x) can still narrow an integer constant
           payload to the declared component: some(-5) in an Option[i32]
           declaration narrows -5 to i32. */
        if (bir_is_sum_type(expected) && expr->arg_count == 1 &&
            expr->args[0] && expr->args[0]->kind == HIR_EXPR_CONST &&
            (expr->args[0]->const_value.kind == BIR_SCALAR_I64 ||
             expr->args[0]->const_value.kind == BIR_SCALAR_U64) &&
            expr->type->generic_arg_count >= 1 &&
            expected->generic_arg_count >= 1 &&
            !bir_types_equal(expr->type->generic_args[0],
                             expected->generic_args[0])) {
            const CobraType *wanted = expected->generic_args[0];
            expr->args[0] = hir_coerce_int_const(b, expr->args[0], wanted);
            if (!expr->args[0]) return false;
            expr->type = bir_sum_type(b->module, expected->kind, wanted,
                                      expected->kind == COBRA_TYPE_RESULT &&
                                      expected->generic_arg_count >= 2
                                      ? expected->generic_args[1] : NULL);
            if (!expr->type) {
                bir_fail(b, expr->source_line, expr->source_col,
                         "sum components are outside the backend-IR subset");
                return false;
            }
        }
        return true;
    }
    if (!expected || !bir_is_sum_type(expected)) {
        bir_fail(b, expr->source_line, expr->source_col,
                 "cannot infer the Option or Result type");
        return false;
    }
    /* Complete an incomplete payload constructor against the component of
       the expected type that this constructor fills. */
    const CobraType *payload_expected = NULL;
    if (expected->generic_arg_count >= 1) {
        payload_expected =
            (expr->sum_variant == 3 && expected->generic_arg_count >= 2)
            ? expected->generic_args[1] : expected->generic_args[0];
    }
    if (expr->arg_count == 1 && expr->args[0] &&
        expr->args[0]->kind == HIR_EXPR_SUM_MAKE && !expr->args[0]->type) {
        if (!hir_complete_sum_type(b, expr->args[0], payload_expected))
            return false;
    }
    if (expr->arg_count == 1 && expr->args[0] &&
        expr->args[0]->kind == HIR_EXPR_FLOAT_LITERAL &&
        !hir_complete_float_literal(b, expr->args[0], payload_expected)) {
        return false;
    }
    if (expr->arg_count == 1 && expr->args[0] &&
        expr->args[0]->kind == HIR_EXPR_BINOP &&
        !hir_complete_float_expr(b, expr->args[0], payload_expected)) {
        return false;
    }
    /* An integer payload constant narrows to the declared component type, so
       some(-5) completes Option[i32] and some(3000000000) completes
       Option[u32]. */
    if (expr->arg_count == 1 && expr->args[0] && payload_expected &&
        expr->args[0]->kind == HIR_EXPR_CONST &&
        (expr->args[0]->const_value.kind == BIR_SCALAR_I64 ||
         expr->args[0]->const_value.kind == BIR_SCALAR_U64) &&
        !bir_types_equal(expr->args[0]->type, payload_expected)) {
        expr->args[0] = hir_coerce_int_const(b, expr->args[0], payload_expected);
        if (!expr->args[0]) return false;
    }
    const CobraType *element = expected->generic_args[0];
    const CobraType *error = expected->kind == COBRA_TYPE_RESULT &&
                             expected->generic_arg_count >= 2
                             ? expected->generic_args[1] : NULL;
    if (expr->sum_variant == 2 || expr->sum_variant == 3) {
        if (expr->arg_count != 1 || !expr->args[0] || !expr->args[0]->type) {
            bir_fail(b, expr->source_line, expr->source_col,
                     "cannot infer the Option or Result type");
            return false;
        }
        if (expr->sum_variant == 2) element = expr->args[0]->type;
        else error = expr->args[0]->type;
    } else if (expr->arg_count == 1 && expr->args[0] && expr->args[0]->type) {
        /* some(payload): the payload type is authoritative. */
        element = expr->args[0]->type;
    }
    expr->type = bir_sum_type(b->module, expected->kind, element, error);
    if (!expr->type) {
        bir_fail(b, expr->source_line, expr->source_col,
                 "sum components are outside the backend-IR subset");
        return false;
    }
    return true;
}

static bool hir_complete_array_type(HirBuilder *b, HirExpr *expr,
                                    const CobraType *expected) {
    if (!expr || expr->kind != HIR_EXPR_ARRAY_LITERAL || !expected) return true;
    if ((expected->kind != COBRA_TYPE_ARRAY && expected->kind != COBRA_TYPE_LIST) ||
        expected->generic_arg_count != 1 ||
        (expected->kind == COBRA_TYPE_ARRAY && expected->array_length != expr->arg_count)) {
        bir_fail(b, expr->source_line, expr->source_col,
                 expected->kind == COBRA_TYPE_LIST
                     ? "buffer length or element type does not match the boundary"
                     : "fixed array length or element type does not match the boundary");
        return false;
    }
    const CobraType *element = expected->generic_args[0];
    for (size_t i = 0; i < expr->arg_count; i++) {
        HirExpr *item = expr->args[i];
        if (!hir_complete_float_expr(b, item, element)) return false;
        if (item->kind == HIR_EXPR_CONST &&
            (item->const_value.kind == BIR_SCALAR_I64 ||
             item->const_value.kind == BIR_SCALAR_U64) &&
            !bir_types_equal(item->type, element)) {
            item = hir_coerce_int_const(b, item, element);
            if (!item) return false;
            expr->args[i] = item;
        }
        if (!item->type || !bir_types_equal(item->type, element)) {
            bir_fail(b, item ? item->source_line : expr->source_line,
                     item ? item->source_col : expr->source_col,
                     "fixed array element has the wrong type");
            return false;
        }
    }
    expr->type = expected;
    return true;
}

/* ------------------------------------------------------------------ */
/* Expressions                                                        */
/* ------------------------------------------------------------------ */

static bool hir_build_expr(HirBuilder *b, ASTNode *node, HirExpr **out);

/* Build a fresh owned u8 string from two readonly string-compatible values.
   The SSA pass performs the allocation and byte-copy operation. */
static bool hir_try_string_concat(HirBuilder *b, ASTNode *node,
                                  HirExpr **out, bool *matched) {
    if (matched) *matched = false;
    if (!node || node->child_count != 2) return true;
    HirExpr *left = NULL;
    HirExpr *right = NULL;
    if (!hir_build_expr(b, node->children[0], &left) ||
        !hir_build_expr(b, node->children[1], &right)) {
        hir_expr_free(left);
        hir_expr_free(right);
        return false;
    }
    bool left_string = bir_is_string_value_type(left->type);
    bool right_string = bir_is_string_value_type(right->type);
    if (!left_string && !right_string) {
        hir_expr_free(left);
        hir_expr_free(right);
        return true;
    }
    if (!left_string || !right_string) {
        hir_expr_free(left);
        hir_expr_free(right);
        bir_fail(b, node->source_line, node->source_col,
                 "string concatenation requires two string values");
        return false;
    }
    HirExpr *expr = hir_expr_alloc(b, node->source_line, node->source_col);
    if (!expr) {
        hir_expr_free(left);
        hir_expr_free(right);
        return false;
    }
    expr->kind = HIR_EXPR_STR_CONCAT;
    expr->type = bir_owned_slice_type(b->module, b->module->type_u8);
    expr->args = calloc(2, sizeof(HirExpr *));
    if (!expr->type || !expr->args) {
        hir_expr_free(left);
        hir_expr_free(right);
        hir_expr_free(expr);
        return false;
    }
    expr->args[0] = left;
    expr->args[1] = right;
    expr->arg_count = 2;
    if (matched) *matched = true;
    if (out) *out = expr;
    return true;
}

/* Integer literals in 0..255 coerce to u8 element types; anything else is
   outside the scalar subset. Returns NULL after failing on out-of-range. */
/* Coerce an i64-typed integer literal into a narrower integer target. The
   literal must be in the target's representable range. */
/* Coerce an integer constant to a target scalar type. Signed i64 constants
   narrow with range checks; unsigned u64 constants (source literals above
   INT64_MAX) narrow with unsigned range checks and are rejected for signed
   targets because their magnitude has no signed representation. */
static HirExpr *hir_coerce_int_const(HirBuilder *b, HirExpr *value,
                                     const CobraType *target) {
    if (!value || !target || value->kind != HIR_EXPR_CONST) return value;
    if (value->const_value.kind == BIR_SCALAR_I64) {
        int64_t raw = value->const_value.payload.i64;
        switch (target->kind) {
            case COBRA_TYPE_U8:
                if (raw < 0 || raw > 255) {
                    bir_fail(b, value->source_line, value->source_col,
                             "u8 value must be an integer literal in 0..255");
                    return NULL;
                }
                value->type = target;
                value->const_value = bir_scalar_u8(target, (uint8_t)raw);
                return value;
            case COBRA_TYPE_I32:
                if (raw < INT32_MIN || raw > INT32_MAX) {
                    bir_fail(b, value->source_line, value->source_col,
                             "i32 value must be an integer literal in the i32 range");
                    return NULL;
                }
                value->type = target;
                value->const_value = bir_scalar_i32(target, (int32_t)raw);
                return value;
            case COBRA_TYPE_U32:
                if (raw < 0 || (uint64_t)raw > UINT32_MAX) {
                    bir_fail(b, value->source_line, value->source_col,
                             "u32 value must be an integer literal in 0..4294967295");
                    return NULL;
                }
                value->type = target;
                value->const_value = bir_scalar_u32(target, (uint32_t)raw);
                return value;
            case COBRA_TYPE_U64:
                if (raw < 0) {
                    bir_fail(b, value->source_line, value->source_col,
                             "u64 value must be a non-negative integer literal");
                    return NULL;
                }
                value->type = target;
                value->const_value = bir_scalar_u64(target, (uint64_t)raw);
                return value;
            default:
                return value;
        }
    }
    if (value->const_value.kind == BIR_SCALAR_U64) {
        uint64_t raw = (uint64_t)value->const_value.payload.i64;
        switch (target->kind) {
            case COBRA_TYPE_U8:
                if (raw > 255) {
                    bir_fail(b, value->source_line, value->source_col,
                             "u8 value must be an integer literal in 0..255");
                    return NULL;
                }
                value->type = target;
                value->const_value = bir_scalar_u8(target, (uint8_t)raw);
                return value;
            case COBRA_TYPE_I32:
                if (raw > (uint64_t)INT32_MAX) {
                    bir_fail(b, value->source_line, value->source_col,
                             "i32 value must be an integer literal in the i32 range");
                    return NULL;
                }
                value->type = target;
                value->const_value = bir_scalar_i32(target, (int32_t)raw);
                return value;
            case COBRA_TYPE_U32:
                if (raw > UINT32_MAX) {
                    bir_fail(b, value->source_line, value->source_col,
                             "u32 value must be an integer literal in 0..4294967295");
                    return NULL;
                }
                value->type = target;
                value->const_value = bir_scalar_u32(target, (uint32_t)raw);
                return value;
            case COBRA_TYPE_I64:
                if (raw > (uint64_t)INT64_MAX) {
                    bir_fail(b, value->source_line, value->source_col,
                             "i64 value cannot hold an unsigned literal above INT64_MAX");
                    return NULL;
                }
                value->type = target;
                value->const_value = bir_scalar_i64(target, (int64_t)raw);
                return value;
            default:
                return value;
        }
    }
    return value;
}

/* Resolve a float literal to f32 or f64 against an expected target. The
   literal keeps its exact double value, so widening never round-trips
   through f32. Without a target (or with a non-float target) the literal
   defaults to f32, matching untyped float-assignment behavior. */
static bool hir_complete_float_literal(HirBuilder *b, HirExpr *expr,
                                       const CobraType *target) {
    if (!expr || expr->kind != HIR_EXPR_FLOAT_LITERAL) return true;
    bool is_f64 = target && bir_types_equal(target, b->module->type_f64);
    expr->kind = HIR_EXPR_CONST;
    if (is_f64) {
        expr->type = b->module->type_f64;
        expr->const_value = bir_scalar_f64(expr->type, expr->float_value);
    } else {
        expr->type = b->module->type_f32;
        expr->const_value = bir_scalar_f32(expr->type, (float)expr->float_value);
    }
    return true;
}

/* Complete a float-typed expression against a boundary target. A plain float
   literal follows the target; a pure float-literal arithmetic expression
   (1.5 + 2.25) also follows it so an f64 return widens both literals instead
   of rounding them through f32 first. Expressions that mix a typed operand
   with a literal were completed at build time and are left alone. */
static bool hir_complete_float_expr(HirBuilder *b, HirExpr *expr,
                                    const CobraType *target) {
    if (!expr) return true;
    if (expr->kind == HIR_EXPR_FLOAT_LITERAL)
        return hir_complete_float_literal(b, expr, target);
    if (expr->kind == HIR_EXPR_BINOP && expr->arg_count == 2 && target &&
        bir_types_equal(target, b->module->type_f64) &&
        expr->args[0]->kind == HIR_EXPR_CONST &&
        expr->args[0]->const_value.kind == BIR_SCALAR_F32 &&
        expr->args[1]->kind == HIR_EXPR_CONST &&
        expr->args[1]->const_value.kind == BIR_SCALAR_F32 &&
        !bir_types_equal(expr->type, b->module->type_bool)) {
        expr->args[0]->type = b->module->type_f64;
        expr->args[0]->const_value =
            bir_scalar_f64(expr->args[0]->type, expr->args[0]->float_value);
        expr->args[1]->type = b->module->type_f64;
        expr->args[1]->const_value =
            bir_scalar_f64(expr->args[1]->type, expr->args[1]->float_value);
        expr->type = expr->args[0]->type;
    }
    return true;
}

static bool hir_is_numeric(const CobraType *type, const BackendIrModule *module) {
    return bir_types_equal(type, module->type_i64) ||
           bir_types_equal(type, module->type_i32) ||
           bir_types_equal(type, module->type_u32) ||
           bir_types_equal(type, module->type_u64) ||
           bir_types_equal(type, module->type_u8) ||
           bir_types_equal(type, module->type_f32) ||
           bir_types_equal(type, module->type_f64);
}

static SsaOpcode hir_map_binop(const char *op) {
    if (strcmp(op, "+") == 0) return SSA_OP_ADD;
    if (strcmp(op, "-") == 0) return SSA_OP_SUB;
    if (strcmp(op, "*") == 0) return SSA_OP_MUL;
    if (strcmp(op, "/") == 0) return SSA_OP_DIV;
    if (strcmp(op, "==") == 0) return SSA_OP_EQ;
    if (strcmp(op, "!=") == 0) return SSA_OP_NE;
    if (strcmp(op, "<") == 0) return SSA_OP_LT;
    if (strcmp(op, ">") == 0) return SSA_OP_GT;
    if (strcmp(op, "<=") == 0) return SSA_OP_LE;
    if (strcmp(op, ">=") == 0) return SSA_OP_GE;
    return SSA_OP_NONE;
}

static bool hir_builtin_outside_subset(const char *name) {
    static const char *const builtins[] = {
        "alloc_i64", "alloc_f32", "alloc_u8", "free", "len", "range",
        "enumerate", "set", "get", "delete", "has", "append",
        "pop", "fill_f32", "sum_f32", "zero_f32", "matmul_f32", "dense_f32",
        "print", NULL
    };
    for (size_t i = 0; builtins[i]; i++) {
        if (strcmp(name, builtins[i]) == 0) return true;
    }
    return false;
}

/* Payload-carrying enum variant construction: Shape.Circle(1.5) lowers to
   a tagged-sum make whose tag is the variant discriminant and whose payload
   is copied into the variant's resident slot. Multi-field payloads
   (Shape.Rect(1.0, 2.0)) synthesize the payload struct into a private
   local first, then reference it. */
static HirExpr *hir_build_enum_make(HirBuilder *b, ASTNode *node,
                                    const BirEnumInfo *info, int variant_index) {
    if (!b || !node || !info || variant_index < 0 ||
        (size_t)variant_index >= info->variant_count) return NULL;
    const CobraType *payload = info->variant_payloads[variant_index];
    if (!payload) {
        bir_fail(b, node->source_line, node->source_col,
                 "unit enum variant '%.20s.%.20s' cannot be called",
                 info->name, info->variant_names[variant_index]);
        return NULL;
    }
    /* The declared arity is authoritative: `A(P)` takes one struct argument
       even when the imported payload type has several fields; `A(i64, i64)`
       takes two. */
    size_t expected = info->variant_payload_arity[variant_index];
    if (expected == 0) expected = payload->kind == COBRA_TYPE_STRUCT
        ? payload->field_count : 1;
    if (node->child_count != expected) {
        bir_fail(b, node->source_line, node->source_col,
                 "enum variant '%.20s.%.20s' expects %zu argument%s, got %zu",
                 info->name, info->variant_names[variant_index],
                 expected, expected == 1 ? "" : "s", node->child_count);
        return NULL;
    }
    /* A declared single payload imports directly and is passed as one
       argument; a multi-field payload synthesizes an inline struct. */
    bool inline_struct = expected > 1;
    HirExpr *payload_expr = NULL;
    if (inline_struct) {
        /* Multi-field payload: build the synthesized struct in place. */
        char tmp_name[COBRA_MAX_IDENT_LEN];
        snprintf(tmp_name, sizeof(tmp_name), "@payload_%d", b->synthetic_seq++);
        int temp = hir_add_local(b, tmp_name, false, payload,
                                 node->source_line, node->source_col);
        if (temp < 0) return NULL;
        for (size_t i = 0; i < payload->field_count; i++) {
            HirExpr *value = NULL;
            if (!hir_build_expr(b, node->children[i], &value)) return NULL;
            if (!hir_complete_float_expr(b, value, payload->fields[i].type) ||
                !hir_complete_array_type(b, value, payload->fields[i].type)) {
                hir_expr_free(value);
                return NULL;
            }
            value = hir_coerce_int_const(b, value, payload->fields[i].type);
            if (!value) return NULL;
            if (!bir_types_equal(value->type, payload->fields[i].type)) {
                bir_fail(b, node->source_line, node->source_col,
                         "enum payload field %zu has the wrong type", i + 1);
                hir_expr_free(value);
                return NULL;
            }
            HirExpr *target = hir_expr_alloc(b, node->source_line,
                                             node->source_col);
            if (!target) {
                hir_expr_free(value);
                return NULL;
            }
            target->kind = HIR_EXPR_MEMBER;
            target->type = payload->fields[i].type;
            target->aggregate_type = payload;
            target->field_offset = payload->fields[i].offset;
            target->args = calloc(1, sizeof(HirExpr *));
            if (!target->args) {
                hir_expr_free(target);
                hir_expr_free(value);
                return NULL;
            }
            target->arg_count = 1;
            target->args[0] = hir_expr_alloc(b, node->source_line,
                                             node->source_col);
            if (!target->args[0]) {
                hir_expr_free(target);
                hir_expr_free(value);
                return NULL;
            }
            target->args[0]->kind = HIR_EXPR_LOCAL;
            target->args[0]->type = payload;
            target->args[0]->local = (uint32_t)temp;
            HirStmt member_store;
            memset(&member_store, 0, sizeof(member_store));
            member_store.kind = HIR_STMT_MEMBER_ASSIGN;
            member_store.target = target;
            member_store.expr = value;
            if (!hir_block_add_stmt(b, b->current, member_store)) {
                hir_expr_free(target);
                hir_expr_free(value);
                return NULL;
            }
        }
        payload_expr = hir_expr_alloc(b, node->source_line, node->source_col);
        if (!payload_expr) return NULL;
        payload_expr->kind = HIR_EXPR_LOCAL;
        payload_expr->type = payload;
        payload_expr->local = (uint32_t)temp;
    } else {
        if (!hir_build_expr(b, node->children[0], &payload_expr)) return NULL;
        if (!hir_complete_float_expr(b, payload_expr, payload) ||
            !hir_complete_array_type(b, payload_expr, payload)) {
            hir_expr_free(payload_expr);
            return NULL;
        }
        payload_expr = hir_coerce_int_const(b, payload_expr, payload);
        if (!payload_expr) return NULL;
        if (!bir_types_equal(payload_expr->type, payload)) {
            bir_fail(b, node->source_line, node->source_col,
                     "enum payload argument has the wrong type");
            hir_expr_free(payload_expr);
            return NULL;
        }
    }
    HirExpr *expr = hir_expr_alloc(b, node->source_line, node->source_col);
    if (!expr) {
        hir_expr_free(payload_expr);
        return NULL;
    }
    expr->kind = HIR_EXPR_SUM_MAKE;
    expr->type = info->type;
    expr->sum_variant = 0; /* variant index rides sum_selector for enums */
    expr->sum_selector = variant_index + 1;
    expr->args = calloc(1, sizeof(HirExpr *));
    if (!expr->args) {
        hir_expr_free(expr);
        hir_expr_free(payload_expr);
        return NULL;
    }
    expr->args[0] = payload_expr;
    expr->arg_count = 1;
    return expr;
}

static bool bir_is_sum_builtin(const char *name) {
    return !strcmp(name, "some") || !strcmp(name, "none") ||
           !strcmp(name, "ok") || !strcmp(name, "err") ||
           !strcmp(name, "is_some") || !strcmp(name, "is_ok") ||
           !strcmp(name, "unwrap") || !strcmp(name, "unwrap_ok") ||
           !strcmp(name, "unwrap_err");
}

/* Option/Result builtins. Constructors carry the payload/error expression and
   a variant id; none() has no operand. Accessors read one component of a
   sum-typed base expression. Constructors that cannot determine their full
   sum type from the argument alone (none, ok, err) leave type NULL; the
   assignment, return, and call-argument boundaries complete the type against
   the declared or parameter type. */
static HirExpr *hir_build_sum_builtin(HirBuilder *b, ASTNode *node) {
    if (strcmp(node->name, "none") == 0) {
        if (node->child_count != 0) {
            bir_fail(b, node->source_line, node->source_col,
                     "none takes no arguments");
            return NULL;
        }
        HirExpr *expr = hir_expr_alloc(b, node->source_line, node->source_col);
        if (!expr) return NULL;
        expr->kind = HIR_EXPR_SUM_MAKE;
        expr->sum_variant = 0;
        expr->type = NULL;
        return expr;
    }
    bool is_constructor = !strcmp(node->name, "some") ||
                          !strcmp(node->name, "ok") ||
                          !strcmp(node->name, "err");
    bool is_accessor = !strcmp(node->name, "is_some") ||
                       !strcmp(node->name, "is_ok") ||
                       !strcmp(node->name, "unwrap") ||
                       !strcmp(node->name, "unwrap_ok") ||
                       !strcmp(node->name, "unwrap_err");
    if (node->child_count != 1) {
        bir_fail(b, node->source_line, node->source_col,
                 "Option and Result operations require one argument");
        return NULL;
    }
    HirExpr *arg = NULL;
    if (!hir_build_expr(b, node->children[0], &arg)) return NULL;
    if (is_constructor) {
        int variant = !strcmp(node->name, "some") ? 1 :
                      !strcmp(node->name, "ok") ? 2 : 3;
        /* Payloads may be scalars, nested sums, or owned slice values; an
           incomplete inner constructor (some(none)) or float literal
           (some(1.5)) defers its type to the boundary. */
        bool incomplete_payload = (arg->kind == HIR_EXPR_SUM_MAKE ||
                                   arg->kind == HIR_EXPR_FLOAT_LITERAL) &&
                                  !arg->type;
        if ((!arg->type && !incomplete_payload) ||
            (arg->type &&
             !bir_is_scalar_type(arg->type, b->module) &&
             !bir_is_sum_type(arg->type) &&
             !bir_is_owned_slice_type(arg->type) &&
             !bir_type_is_value_only_struct(arg->type) &&
             arg->type->kind != COBRA_TYPE_STRUCT)) {
            bir_fail(b, node->source_line, node->source_col,
                     "sum payload must be a scalar, nested sum, owned slice, or struct in the backend-IR subset");
            hir_expr_free(arg);
            return NULL;
        }
        /* An aggregate-returning call as the payload (some(make_point()))
           cannot lower in place: aggregate calls need an explicit destination
           slot. Hoist the call into a fresh temp local so the constructor
           payload is a plain local reference. */
        if (arg->type && hir_is_aggregate_value_type(arg->type) &&
            arg->kind == HIR_EXPR_CALL) {
            char temp_name[64];
            snprintf(temp_name, sizeof(temp_name), "__sum_payload_%zu",
                     b->fn->local_count);
            int temp = hir_add_local(b, temp_name, false, arg->type,
                                     arg->source_line, arg->source_col);
            if (temp < 0) {
                hir_expr_free(arg);
                return NULL;
            }
            if (!hir_emit_assign(b, (uint32_t)temp, arg)) return NULL;
            HirExpr *local = hir_expr_alloc(b, node->source_line, node->source_col);
            if (!local) return NULL;
            local->kind = HIR_EXPR_LOCAL;
            local->local = (uint32_t)temp;
            local->type = arg->type;
            arg = local;
        }
        HirExpr *expr = hir_expr_alloc(b, node->source_line, node->source_col);
        if (!expr) {
            hir_expr_free(arg);
            return NULL;
        }
        expr->kind = HIR_EXPR_SUM_MAKE;
        expr->sum_variant = variant;
        expr->args = calloc(1, sizeof(HirExpr *));
        expr->arg_count = 1;
        if (!expr->args) {
            hir_expr_free(arg);
            hir_expr_free(expr);
            return NULL;
        }
        expr->args[0] = arg;
        if (variant == 1 && arg->type) {
            /* some(x): Option[T] is fully determined. */
            expr->type = bir_sum_type(b->module, COBRA_TYPE_OPTION, arg->type, NULL);
            if (!expr->type) {
                hir_expr_free(expr);
                return NULL;
            }
        }
        return expr;
    }
    if (is_accessor) {
        if (!arg->type || !bir_is_sum_type(arg->type)) {
            bir_fail(b, node->source_line, node->source_col,
                     "'%s' requires an Option or Result value", node->name);
            hir_expr_free(arg);
            return NULL;
        }
        bool option = arg->type->kind == COBRA_TYPE_OPTION;
        bool predicate = !strcmp(node->name, "is_some") ||
                         !strcmp(node->name, "is_ok");
        bool error_value = !strcmp(node->name, "unwrap_err");
        if (predicate && ((option && strcmp(node->name, "is_some") != 0) ||
                          (!option && strcmp(node->name, "is_ok") != 0))) {
            bir_fail(b, node->source_line, node->source_col,
                     "'%s' does not apply to %s", node->name,
                     option ? "an Option" : "a Result");
            hir_expr_free(arg);
            return NULL;
        }
        if (!predicate) {
            bool valid_name = option
                ? strcmp(node->name, "unwrap") == 0
                : (strcmp(node->name, "unwrap_ok") == 0 ||
                   strcmp(node->name, "unwrap_err") == 0);
            if (!valid_name) {
                bir_fail(b, node->source_line, node->source_col,
                         "'%s' does not apply to %s", node->name,
                         option ? "an Option" : "a Result");
                hir_expr_free(arg);
                return NULL;
            }
        }
        HirExpr *expr = hir_expr_alloc(b, node->source_line, node->source_col);
        if (!expr) {
            hir_expr_free(arg);
            return NULL;
        }
        expr->kind = HIR_EXPR_SUM_ACCESS;
        expr->args = calloc(1, sizeof(HirExpr *));
        expr->arg_count = 1;
        if (!expr->args) {
            hir_expr_free(arg);
            hir_expr_free(expr);
            return NULL;
        }
        expr->args[0] = arg;
        expr->aggregate_type = arg->type;
        expr->sum_expected_tag = 1;
        if (predicate) {
            expr->sum_selector = 0; /* tag */
            expr->type = b->module->type_bool;
        } else {
            expr->sum_selector = error_value ? 2 : 1;
            expr->sum_checked = true;
            expr->type = error_value
                ? arg->type->generic_args[1]
                : arg->type->generic_args[0];
            if (!expr->type) {
                hir_expr_free(arg);
                hir_expr_free(expr);
                return NULL;
            }
        }
        return expr;
    }
    hir_expr_free(arg);
    bir_fail(b, node->source_line, node->source_col,
             "unknown Option or Result operation '%s'", node->name);
    return NULL;
}

static bool hir_build_expr(HirBuilder *b, ASTNode *node, HirExpr **out) {
    if (!node) {
        bir_fail(b, 0, 0, "missing expression in backend-IR subset");
        return false;
    }
    HirExpr *expr = NULL;
    switch (node->type) {
        case AST_INT_LITERAL:
            expr = hir_expr_alloc(b, node->source_line, node->source_col);
            if (!expr) return false;
            expr->kind = HIR_EXPR_CONST;
            /* A literal whose magnitude exceeds INT64_MAX is an unsigned u64
               literal. It defaults to u64 and is range-checked at coercion
               boundaries, so 18446744073709551615 fits a u64 context and is
               rejected in i64/i32/u32/u8 contexts. */
            if (node->literal_is_unsigned) {
                expr->type = b->module->type_u64;
                expr->const_value = bir_scalar_u64(expr->type, node->literal_u64);
            } else {
                expr->type = b->module->type_i64;
                expr->const_value = bir_scalar_i64(expr->type, node->literal_i64);
            }
            break;
        case AST_FLOAT_LITERAL:
            /* The literal keeps its exact double value until a boundary
               (declaration, return, binop, call argument, or sum payload)
               decides whether it is f32 or f64. Defaults to f32. */
            expr = hir_expr_alloc(b, node->source_line, node->source_col);
            if (!expr) return false;
            expr->kind = HIR_EXPR_FLOAT_LITERAL;
            expr->type = NULL;
            expr->float_value = node->literal_f64;
            break;
        case AST_BOOL_LITERAL:
            expr = hir_expr_alloc(b, node->source_line, node->source_col);
            if (!expr) return false;
            expr->kind = HIR_EXPR_CONST;
            expr->type = b->module->type_bool;
            expr->const_value = bir_scalar_bool(expr->type, node->int_val != 0);
            break;
        case AST_NONE_LITERAL:
            /* Bare `none` constructs Option[T]; the type is completed at the
               declaration, return, or call-argument boundary. */
            expr = hir_expr_alloc(b, node->source_line, node->source_col);
            if (!expr) return false;
            expr->kind = HIR_EXPR_SUM_MAKE;
            expr->sum_variant = 0;
            expr->type = NULL;
            break;
        case AST_ARRAY_LITERAL: {
            if (node->child_count > BIR_MAX_ARRAY_ELEMENTS) {
                bir_fail(b, node->source_line, node->source_col,
                         "fixed array literal exceeds the %d-element limit",
                         BIR_MAX_ARRAY_ELEMENTS);
                return false;
            }
            HirExpr *array = hir_expr_alloc(b, node->source_line, node->source_col);
            if (!array) return false;
            array->kind = HIR_EXPR_ARRAY_LITERAL;
            array->args = calloc(node->child_count, sizeof(HirExpr *));
            array->arg_count = node->child_count;
            if (!array->args && node->child_count != 0) {
                hir_expr_free(array);
                return false;
            }
            if (node->child_count == 0) {
                /* An empty literal has no inferable element type. It is
                   completed only at a declared list boundary. Fixed arrays
                   still reject it through their required nonzero length. */
                array->type = NULL;
                expr = array;
                break;
            }
            const CobraType *element_type = NULL;
            for (size_t i = 0; i < node->child_count; i++) {
                if (!hir_build_expr(b, node->children[i], &array->args[i])) {
                    hir_expr_free(array);
                    return false;
                }
                if (array->args[i]->kind == HIR_EXPR_FLOAT_LITERAL) {
                    if (!hir_complete_float_literal(b, array->args[i], NULL)) {
                        hir_expr_free(array);
                        return false;
                    }
                }
                if (array->args[i]->kind == HIR_EXPR_CONST &&
                    (array->args[i]->const_value.kind == BIR_SCALAR_I64 ||
                     array->args[i]->const_value.kind == BIR_SCALAR_U64) &&
                    element_type && !bir_types_equal(array->args[i]->type, element_type)) {
                    array->args[i] = hir_coerce_int_const(b, array->args[i], element_type);
                    if (!array->args[i]) {
                        hir_expr_free(array);
                        return false;
                    }
                }
                if (!element_type) element_type = array->args[i]->type;
                const CobraType *item_type = array->args[i]->type;
                bool element_ok = bir_is_scalar_type(item_type, b->module) ||
                                  (item_type && item_type->kind == COBRA_TYPE_ARRAY) ||
                                  (item_type &&
                                   bir_type_is_value_only_struct(item_type));
                if (!element_type || !element_ok ||
                    !bir_types_equal(element_type, item_type)) {
                    bir_fail(b, node->source_line, node->source_col,
                             "array literal elements must have one scalar, nested-array, or value-only struct type");
                    hir_expr_free(array);
                    return false;
                }
            }
            array->type = bir_array_type(b->module, element_type, array->arg_count);
            if (!array->type) {
                hir_expr_free(array);
                bir_fail(b, node->source_line, node->source_col,
                         "fixed array literal type is outside the backend-IR subset");
                return false;
            }
            expr = array;
            break;
        }
        case AST_DICT_LITERAL: {
            if (node->child_count > BIR_MAX_ARRAY_ELEMENTS) {
                bir_fail(b, node->source_line, node->source_col,
                         "dict literal exceeds the %d-entry limit",
                         BIR_MAX_ARRAY_ELEMENTS);
                return false;
            }
            HirExpr *dict = hir_expr_alloc(b, node->source_line, node->source_col);
            if (!dict) return false;
            dict->kind = HIR_EXPR_DICT_LITERAL;
            dict->args = calloc(node->child_count, sizeof(HirExpr *));
            dict->dict_keys = calloc(node->child_count, sizeof(char *));
            dict->arg_count = node->child_count;
            if (!dict->args || (node->child_count && !dict->dict_keys)) {
                hir_expr_free(dict);
                return false;
            }
            const CobraType *value_type = NULL;
            for (size_t i = 0; i < node->child_count; i++) {
                ASTNode *entry = node->children[i];
                if (entry->type != AST_DICT_ENTRY || entry->child_count != 1 ||
                    !entry->name[0]) {
                    bir_fail(b, node->source_line, node->source_col,
                             "dict literal entries require a string key and one value");
                    hir_expr_free(dict);
                    return false;
                }
                size_t key_len = strlen(entry->name);
                dict->dict_keys[i] = malloc(key_len + 1);
                if (!dict->dict_keys[i]) {
                    hir_expr_free(dict);
                    return false;
                }
                memcpy(dict->dict_keys[i], entry->name, key_len + 1);
                if (!hir_build_expr(b, entry->children[0], &dict->args[i])) {
                    hir_expr_free(dict);
                    return false;
                }
                if (!value_type) value_type = dict->args[i]->type;
                if (!bir_types_equal(value_type, dict->args[i]->type)) {
                    bir_fail(b, node->source_line, node->source_col,
                             "dict literal values must have one scalar type");
                    hir_expr_free(dict);
                    return false;
                }
            }
            if (!value_type || !cobra_type_is_scalar(value_type)) {
                bir_fail(b, node->source_line, node->source_col,
                         "dict literal values must be scalars");
                hir_expr_free(dict);
                return false;
            }
            dict->type = bir_dict_type(b->module, value_type);
            if (!dict->type) {
                hir_expr_free(dict);
                bir_fail(b, node->source_line, node->source_col,
                         "dict literal type is outside the backend-IR subset");
                return false;
            }
            expr = dict;
            break;
        }
        case AST_STRING_LITERAL:
            /* A borrowed string literal is a readonly u8 view over the
               literal bytes. The SSA pass materializes the backing storage. */
            expr = hir_expr_alloc(b, node->source_line, node->source_col);
            if (!expr) return false;
            expr->kind = HIR_EXPR_STR_LITERAL;
            expr->type = bir_view_type(b->module, b->module->type_u8);
            if (!expr->type) {
                hir_expr_free(expr);
                return false;
            }
            snprintf(expr->literal, sizeof(expr->literal), "%s",
                     node->string_val);
            break;
        case AST_VAR_REF: {
            int local = hir_require_local(b, node->name,
                                          node->source_line, node->source_col);
            if (local < 0) return false;
            expr = hir_expr_alloc(b, node->source_line, node->source_col);
            if (!expr) return false;
            expr->kind = HIR_EXPR_LOCAL;
            expr->local = (uint32_t)local;
            expr->type = b->fn->locals[local].type;
            break;
        }
        case AST_LEN_EXPR: {
            if (node->child_count != 1) {
                bir_fail(b, node->source_line, node->source_col,
                         "len requires one readonly slice view");
                return false;
            }
            HirExpr *view = NULL;
            if (!hir_build_expr(b, node->children[0], &view)) return false;
            if (view->type && bir_is_owned_dict_type(view->type)) {
                expr = hir_expr_alloc(b, node->source_line, node->source_col);
                if (!expr) {
                    hir_expr_free(view);
                    return false;
                }
                expr->kind = HIR_EXPR_DICT_LEN;
                expr->type = b->module->type_i64;
                expr->args = calloc(1, sizeof(HirExpr *));
                if (!expr->args) {
                    hir_expr_free(view);
                    hir_expr_free(expr);
                    return false;
                }
                expr->args[0] = view;
                expr->arg_count = 1;
                break;
            }
            if (!bir_is_borrowed_view_type(view->type) &&
                !bir_is_owned_slice_type(view->type) &&
                (!view->type || view->type->kind != COBRA_TYPE_ARRAY)) {
                hir_expr_free(view);
                bir_fail(b, node->source_line, node->source_col,
                         "len requires a borrowed slice view, owned slice, or dict");
                return false;
            }
            expr = hir_expr_alloc(b, node->source_line, node->source_col);
            if (!expr) {
                hir_expr_free(view);
                return false;
            }
            expr->kind = HIR_EXPR_LEN;
            expr->type = b->module->type_i64;
            expr->args = calloc(1, sizeof(HirExpr *));
            if (!expr->args) {
                hir_expr_free(view);
                hir_expr_free(expr);
                return false;
            }
            expr->args[0] = view;
            expr->arg_count = 1;
            break;
        }
        case AST_ARRAY_INDEX: {
            if (node->secondary_name[0] || node->child_count != 1) {
                bir_fail(b, node->source_line, node->source_col,
                         "multi-dimensional or struct slice indexing is outside the backend-IR subset");
                return false;
            }
            int local = hir_require_local(b, node->name,
                                          node->source_line, node->source_col);
            if (local < 0) return false;
            const CobraType *view = b->fn->locals[local].type;
            if (view && bir_is_owned_dict_type(view)) {
                /* d["key"] lowers to a dict lookup with a zero fallback,
                   matching the production codegen contract. */
                if (node->children[0]->type != AST_STRING_LITERAL) {
                    bir_fail(b, node->source_line, node->source_col,
                             "dict keys must be string literals in the backend-IR subset");
                    return false;
                }
                HirExpr *key = NULL;
                if (!hir_build_expr(b, node->children[0], &key)) return false;
                HirExpr *fallback = hir_expr_alloc(b, node->source_line,
                                                   node->source_col);
                if (!fallback) {
                    hir_expr_free(key);
                    return false;
                }
                fallback->kind = HIR_EXPR_CONST;
                fallback->type = b->module->type_i64;
                fallback->const_value =
                    bir_scalar_i64(b->module->type_i64, 0);
                expr = hir_expr_alloc(b, node->source_line, node->source_col);
                if (!expr) {
                    hir_expr_free(key);
                    hir_expr_free(fallback);
                    return false;
                }
                expr->kind = HIR_EXPR_DICT_GET;
                expr->type = b->module->type_i64;
                expr->local = (uint32_t)local;
                snprintf(expr->dict_key, sizeof(expr->dict_key), "%s",
                         key->literal);
                hir_expr_free(key);
                expr->args = calloc(2, sizeof(HirExpr *));
                if (!expr->args) {
                    hir_expr_free(fallback);
                    hir_expr_free(expr);
                    return false;
                }
                expr->args[0] = hir_expr_alloc(b, node->source_line,
                                               node->source_col);
                if (!expr->args[0]) {
                    hir_expr_free(fallback);
                    hir_expr_free(expr);
                    return false;
                }
                expr->args[0]->kind = HIR_EXPR_LOCAL;
                expr->args[0]->local = (uint32_t)local;
                expr->args[0]->type = view;
                expr->args[1] = fallback;
                expr->arg_count = 2;
                break;
            }
            if (!bir_is_borrowed_view_type(view) && !bir_is_owned_slice_type(view) &&
                (!view || view->kind != COBRA_TYPE_ARRAY)) {
                bir_fail(b, node->source_line, node->source_col,
                         "indexing requires a borrowed view, owned slice, or fixed array");
                return false;
            }
            HirExpr *index = NULL;
            if (!hir_build_expr(b, node->children[0], &index)) return false;
            if (!bir_types_equal(index->type, b->module->type_i64)) {
                bir_fail(b, node->source_line, node->source_col,
                         "readonly slice index must be i64");
                hir_expr_free(index);
                return false;
            }
            expr = hir_expr_alloc(b, node->source_line, node->source_col);
            if (!expr) {
                hir_expr_free(index);
                return false;
            }
            expr->kind = HIR_EXPR_INDEX;
            expr->args = calloc(1, sizeof(HirExpr *));
            if (!expr->args) {
                hir_expr_free(index);
                hir_expr_free(expr);
                return false;
            }
            expr->args[0] = index;
            expr->arg_count = 1;
            expr->local = (uint32_t)local;
            expr->aggregate_type = view;
            expr->type = cobra_type_element(view);
            if (!expr->type) {
                hir_expr_free(expr);
                bir_fail(b, node->source_line, node->source_col,
                         "readonly slice has no element type");
                return false;
            }
            break;
        }
        case AST_MEMBER_ACCESS: {
            /* A qualified enum variant (Color.Red) is a compile-time
               integer constant: the base is a bare reference to the enum
               type name, not a runtime value. */
            if (node->child_count == 1 &&
                node->children[0]->type == AST_VAR_REF &&
                node->children[0]->name[0]) {
                const BirEnumInfo *enum_info =
                    bir_find_enum(b->module, node->children[0]->name);
                if (enum_info) {
                    int value = bir_enum_variant_value(b->module,
                                                       enum_info->name,
                                                       node->secondary_name);
                    if (value == INT_MIN) {
                        bir_fail(b, node->source_line, node->source_col,
                                 "unknown enum variant '%.20s.%.20s'",
                                 enum_info->name, node->secondary_name);
                        return false;
                    }
                    int variant_index = -1;
                    for (size_t v = 0; v < enum_info->variant_count; v++) {
                        if (enum_info->variant_values[v] == value) {
                            variant_index = (int)v;
                            break;
                        }
                    }
                    if (enum_info->has_payloads) {
                        /* Unit variant of a payload-carrying enum: the value
                           is an aggregate (tag plus resident payload slots),
                           so it lowers to a payload-less tagged make. */
                        if (variant_index >= 0 &&
                            enum_info->variant_payloads[variant_index]) {
                            bir_fail(b, node->source_line, node->source_col,
                                     "enum variant '%.20s.%.20s' requires a payload argument",
                                     enum_info->name, node->secondary_name);
                            return false;
                        }
                        expr = hir_expr_alloc(b, node->source_line,
                                              node->source_col);
                        if (!expr) return false;
                        expr->kind = HIR_EXPR_SUM_MAKE;
                        expr->type = enum_info->type;
                        expr->sum_variant = 0;
                        expr->sum_selector = variant_index + 1;
                        expr->args = NULL;
                        expr->arg_count = 0;
                        break;
                    }
                    expr = hir_expr_alloc(b, node->source_line, node->source_col);
                    if (!expr) return false;
                    expr->kind = HIR_EXPR_CONST;
                    expr->type = enum_info->type;
                    expr->const_value =
                        bir_scalar_i64(enum_info->type, value);
                    break;
                }
            }
            if (node->child_count != 1) {
                bir_fail(b, node->source_line, node->source_col,
                         "member access requires one aggregate base");
                return false;
            }
            HirExpr *base = NULL;
            if (!hir_build_expr(b, node->children[0], &base)) return false;
            if (!base->type || base->type->kind != COBRA_TYPE_STRUCT) {
                bir_fail(b, node->source_line, node->source_col,
                         "member access requires a scalar-field struct");
                hir_expr_free(base);
                return false;
            }
            const CobraTypeField *field = NULL;
            for (size_t i = 0; i < base->type->field_count; i++) {
                if (strcmp(base->type->fields[i].name, node->secondary_name) == 0) {
                    field = &base->type->fields[i];
                    break;
                }
            }
            if (!field || (!bir_is_scalar_type(field->type, b->module) &&
                           !bir_is_borrowed_view_type(field->type) &&
                           !bir_type_has_owned_payload(field->type) &&
                           field->type->kind != COBRA_TYPE_STRUCT &&
                           field->type->kind != COBRA_TYPE_ARRAY)) {
                bir_fail(b, node->source_line, node->source_col,
                         "struct field '%s' is outside the backend aggregate subset",
                         node->secondary_name);
                hir_expr_free(base);
                return false;
            }
            expr = hir_expr_alloc(b, node->source_line, node->source_col);
            if (!expr) {
                hir_expr_free(base);
                return false;
            }
            expr->kind = HIR_EXPR_MEMBER;
            expr->args = calloc(1, sizeof(HirExpr *));
            if (!expr->args) {
                hir_expr_free(base);
                hir_expr_free(expr);
                return false;
            }
            expr->args[0] = base;
            expr->arg_count = 1;
            expr->type = field->type;
            expr->aggregate_type = base->type;
            expr->field_offset = field->offset;
            break;
        }
        case AST_BINARY_OP: {
            if (strcmp(node->name, "+") == 0 && node->child_count == 2) {
                bool matched = false;
                HirExpr *concat = NULL;
                if (!hir_try_string_concat(b, node, &concat, &matched)) return false;
                if (matched) {
                    expr = concat;
                    break;
                }
            }
            SsaOpcode op = hir_map_binop(node->name);
            if (op == SSA_OP_NONE || node->child_count != 2) {
                bir_fail(b, node->source_line, node->source_col,
                         "operator '%s' is outside the backend-IR subset", node->name);
                return false;
            }
            expr = hir_expr_alloc(b, node->source_line, node->source_col);
            if (!expr) return false;
            expr->kind = HIR_EXPR_BINOP;
            expr->binop = op;
            expr->args = calloc(2, sizeof(HirExpr *));
            expr->arg_count = 2;
            if (!expr->args) {
                bir_fail(b, node->source_line, node->source_col, "out of memory");
                hir_expr_free(expr);
                return false;
            }
            if (!hir_build_expr(b, node->children[0], &expr->args[0]) ||
                !hir_build_expr(b, node->children[1], &expr->args[1])) {
                hir_expr_free(expr);
                return false;
            }
            /* A float literal widens to f64 when the sibling operand is f64;
               otherwise it defaults to f32. When both operands are float
               literals (1.5 + 2.25) the first defaults to f32 and the second
               matches it. */
            if (expr->args[0]->kind == HIR_EXPR_FLOAT_LITERAL &&
                expr->args[1]->type &&
                !hir_complete_float_literal(b, expr->args[0], expr->args[1]->type)) {
                hir_expr_free(expr);
                return false;
            }
            if (expr->args[1]->kind == HIR_EXPR_FLOAT_LITERAL &&
                expr->args[0]->type &&
                !hir_complete_float_literal(b, expr->args[1], expr->args[0]->type)) {
                hir_expr_free(expr);
                return false;
            }
            if (expr->args[0]->kind == HIR_EXPR_FLOAT_LITERAL &&
                !hir_complete_float_literal(b, expr->args[0], NULL)) {
                hir_expr_free(expr);
                return false;
            }
            if (expr->args[1]->kind == HIR_EXPR_FLOAT_LITERAL &&
                !hir_complete_float_literal(b, expr->args[1], expr->args[0]->type)) {
                hir_expr_free(expr);
                return false;
            }
            /* An integer constant widens or narrows to the sibling operand's
               type, so `let x: i32 = 5` then `x + 1` uses i32 arithmetic and
               `some_u64 + 1` stays u64. */
            if (expr->args[0]->type && expr->args[0]->kind == HIR_EXPR_CONST &&
                (expr->args[0]->const_value.kind == BIR_SCALAR_I64 ||
                 expr->args[0]->const_value.kind == BIR_SCALAR_U64) &&
                !bir_types_equal(expr->args[0]->type, expr->args[1]->type)) {
                expr->args[0] = hir_coerce_int_const(b, expr->args[0], expr->args[1]->type);
                if (!expr->args[0]) {
                    hir_expr_free(expr);
                    return false;
                }
            }
            if (expr->args[1]->type && expr->args[1]->kind == HIR_EXPR_CONST &&
                (expr->args[1]->const_value.kind == BIR_SCALAR_I64 ||
                 expr->args[1]->const_value.kind == BIR_SCALAR_U64) &&
                !bir_types_equal(expr->args[1]->type, expr->args[0]->type)) {
                expr->args[1] = hir_coerce_int_const(b, expr->args[1], expr->args[0]->type);
                if (!expr->args[1]) {
                    hir_expr_free(expr);
                    return false;
                }
            }
            /* Integer-backed unit enums support comparisons on their
               discriminants; arithmetic stays numeric-only. */
            bool is_comparison = op >= SSA_OP_EQ && op <= SSA_OP_GE;
            bool enum_operands = is_comparison &&
                expr->args[0]->type &&
                expr->args[0]->type->kind == COBRA_TYPE_ENUM;
            if (!expr->args[0]->type || !expr->args[1]->type ||
                !bir_types_equal(expr->args[0]->type, expr->args[1]->type) ||
                (!hir_is_numeric(expr->args[0]->type, b->module) &&
                 !enum_operands)) {
                bir_fail(b, node->source_line, node->source_col,
                         "operator '%s' requires two matching numeric scalar operands", node->name);
                hir_expr_free(expr);
                return false;
            }
            expr->type = is_comparison
                ? b->module->type_bool : expr->args[0]->type;
            break;
        }
        case AST_FUNC_CALL: {
            /* Qualified call whose qualifier names a registered enum is
               payload-variant construction: Shape.Circle(1.5). */
            if (node->qualifier[0]) {
                const BirEnumInfo *enum_info =
                    bir_find_enum(b->module, node->qualifier);
                if (enum_info) {
                    int variant_index = -1;
                    for (size_t v = 0; v < enum_info->variant_count; v++) {
                        if (strcmp(enum_info->variant_names[v], node->name) == 0) {
                            variant_index = (int)v;
                            break;
                        }
                    }
                    if (variant_index < 0) {
                        bir_fail(b, node->source_line, node->source_col,
                                 "enum '%.20s' has no variant '%.20s'",
                                 enum_info->name, node->name);
                        return false;
                    }
                    expr = hir_build_enum_make(b, node, enum_info, variant_index);
                    if (!expr) return false;
                    break;
                }
            }
            if (strcmp(node->name, "concat") == 0) {
                bool matched = false;
                if (!hir_try_string_concat(b, node, &expr, &matched) || !matched) {
                    if (!b->failed)
                        bir_fail(b, node->source_line, node->source_col,
                                 "concat requires two string values");
                    return false;
                }
                break;
            }
            if (strcmp(node->name, "slice_u8") == 0) {
                if (node->child_count != 3) {
                    bir_fail(b, node->source_line, node->source_col,
                             "slice_u8 requires a readonly or owned []u8 view, start, and length");
                    return false;
                }
                HirExpr *base = NULL, *start = NULL, *length = NULL;
                if (!hir_build_expr(b, node->children[0], &base) ||
                    !hir_build_expr(b, node->children[1], &start) ||
                    !hir_build_expr(b, node->children[2], &length)) {
                    hir_expr_free(base);
                    hir_expr_free(start);
                    hir_expr_free(length);
                    return false;
                }
                bool source_u8_view = bir_is_borrowed_view_type(base->type) &&
                                      base->type->kind == COBRA_TYPE_SLICE_U8 &&
                                      base->type->mutability == COBRA_MUTABILITY_READONLY;
                bool source_u8_owned = bir_is_owned_slice_type(base->type) &&
                                       base->type->kind == COBRA_TYPE_SLICE_U8;
                if ((!source_u8_view && !source_u8_owned) ||
                    !bir_types_equal(start->type, b->module->type_i64) ||
                    !bir_types_equal(length->type, b->module->type_i64)) {
                    hir_expr_free(base);
                    hir_expr_free(start);
                    hir_expr_free(length);
                    bir_fail(b, node->source_line, node->source_col,
                             "slice_u8 requires a readonly or owned []u8 view and i64 bounds");
                    return false;
                }
                expr = hir_expr_alloc(b, node->source_line, node->source_col);
                if (!expr) {
                    hir_expr_free(base);
                    hir_expr_free(start);
                    hir_expr_free(length);
                    return false;
                }
                expr->kind = HIR_EXPR_SLICE;
                expr->type = source_u8_owned
                    ? bir_view_type(b->module, b->module->type_u8)
                    : base->type;
                expr->args = calloc(3, sizeof(HirExpr *));
                if (!expr->args) {
                    hir_expr_free(base);
                    hir_expr_free(start);
                    hir_expr_free(length);
                    hir_expr_free(expr);
                    return false;
                }
                expr->args[0] = base;
                expr->args[1] = start;
                expr->args[2] = length;
                expr->arg_count = 3;
                break;
            }
            if (strcmp(node->name, "alloc_i64") == 0 ||
                strcmp(node->name, "alloc_f32") == 0 ||
                strcmp(node->name, "alloc_u8") == 0) {
                if (node->child_count != 1) {
                    bir_fail(b, node->source_line, node->source_col,
                             "slice allocation requires exactly one count argument");
                    return false;
                }
                HirExpr *count = NULL;
                if (!hir_build_expr(b, node->children[0], &count)) return false;
                if (!bir_types_equal(count->type, b->module->type_i64)) {
                    hir_expr_free(count);
                    bir_fail(b, node->source_line, node->source_col,
                             "slice allocation count must be i64");
                    return false;
                }
                CobraTypeKind element_kind =
                    strcmp(node->name, "alloc_f32") == 0 ? COBRA_TYPE_F32 :
                    strcmp(node->name, "alloc_u8") == 0 ? COBRA_TYPE_U8 : COBRA_TYPE_I64;
                const CobraType *element =
                    element_kind == COBRA_TYPE_F32 ? b->module->type_f32 :
                    element_kind == COBRA_TYPE_U8 ? b->module->type_u8 : b->module->type_i64;
                const CobraType *owned = bir_owned_slice_type(b->module, element);
                if (!owned) {
                    hir_expr_free(count);
                    bir_fail(b, node->source_line, node->source_col,
                             "could not construct the owned slice type");
                    return false;
                }
                uint32_t region_id = BIR_REGION_NONE;
                if (node->qualifier[0]) {
                    int found = hir_find_region(b, node->qualifier);
                    if (found < 0) {
                        hir_expr_free(count);
                        bir_fail(b, node->source_line, node->source_col,
                                 "allocation from unknown or inactive region '%s'",
                                 node->qualifier);
                        return false;
                    }
                    region_id = b->region_stack[found].id;
                }
                expr = hir_expr_alloc(b, node->source_line, node->source_col);
                if (!expr) {
                    hir_expr_free(count);
                    return false;
                }
                expr->kind = HIR_EXPR_ALLOC;
                expr->type = owned;
                expr->aggregate_type = element;
                expr->region_id = region_id;
                expr->args = calloc(1, sizeof(HirExpr *));
                if (!expr->args) {
                    hir_expr_free(count);
                    hir_expr_free(expr);
                    return false;
                }
                expr->args[0] = count;
                expr->arg_count = 1;
                break;
            }
            if (strcmp(node->name, "append") == 0) {
                if (node->child_count != 2 || node->children[0]->type != AST_VAR_REF) {
                    bir_fail(b, node->source_line, node->source_col,
                             "append requires a named list and one element");
                    return false;
                }
                int local = hir_require_local(b, node->children[0]->name,
                                              node->source_line, node->source_col);
                if (local < 0 || !bir_is_owned_buffer_type(b->fn->locals[local].type)) {
                    bir_fail(b, node->source_line, node->source_col,
                             "append requires an owned scalar list");
                    return false;
                }
                HirExpr *source = NULL;
                HirExpr *value = NULL;
                if (!hir_build_expr(b, node->children[0], &source) ||
                    !hir_build_expr(b, node->children[1], &value)) {
                    hir_expr_free(source);
                    hir_expr_free(value);
                    return false;
                }
                const CobraType *element = cobra_type_element(b->fn->locals[local].type);
                value = hir_coerce_int_const(b, value, element);
                if (!value || !hir_complete_float_expr(b, value, element) ||
                    !bir_types_equal(value->type, element)) {
                    hir_expr_free(source);
                    hir_expr_free(value);
                    bir_fail(b, node->source_line, node->source_col,
                             "append element has the wrong scalar type");
                    return false;
                }
                expr = hir_expr_alloc(b, node->source_line, node->source_col);
                if (!expr) {
                    hir_expr_free(source);
                    hir_expr_free(value);
                    return false;
                }
                expr->kind = HIR_EXPR_BUFFER_APPEND;
                expr->local = (uint32_t)local;
                expr->type = b->fn->locals[local].type;
                expr->args = calloc(2, sizeof(HirExpr *));
                if (!expr->args) {
                    hir_expr_free(source);
                    hir_expr_free(value);
                    hir_expr_free(expr);
                    return false;
                }
                expr->args[0] = source;
                expr->args[1] = value;
                expr->arg_count = 2;
                break;
            }
            if (strcmp(node->name, "pop") == 0 &&
                node->child_count == 1) {
                if (node->children[0]->type != AST_VAR_REF) {
                    bir_fail(b, node->source_line, node->source_col,
                             "pop requires one named list");
                    return false;
                }
                int local = hir_require_local(b, node->children[0]->name,
                                              node->source_line, node->source_col);
                if (local >= 0 &&
                    bir_is_owned_buffer_type(b->fn->locals[local].type)) {
                    if (!cobra_type_is_scalar(cobra_type_element(b->fn->locals[local].type))) {
                        bir_fail(b, node->source_line, node->source_col,
                                 "pop of non-scalar list elements is outside the backend-IR subset");
                        return false;
                    }
                    expr = hir_expr_alloc(b, node->source_line, node->source_col);
                    if (!expr) return false;
                    expr->kind = HIR_EXPR_BUFFER_POP;
                    expr->local = (uint32_t)local;
                    expr->type = cobra_type_element(b->fn->locals[local].type);
                    expr->args = calloc(1, sizeof(HirExpr *));
                    if (!expr->args) {
                        hir_expr_free(expr);
                        return false;
                    }
                    if (!hir_build_expr(b, node->children[0], &expr->args[0])) {
                        hir_expr_free(expr);
                        return false;
                    }
                    expr->arg_count = 1;
                    break;
                }
            }
            if (strcmp(node->name, "get") == 0 ||
                strcmp(node->name, "has") == 0 ||
                strcmp(node->name, "pop") == 0) {
                bool needs_fallback = strcmp(node->name, "get") == 0 ||
                                      strcmp(node->name, "pop") == 0;
                size_t expected = needs_fallback ? 3 : 2;
                if (node->child_count != expected ||
                    node->children[0]->type != AST_VAR_REF) {
                    bir_fail(b, node->source_line, node->source_col,
                             "%s requires a named dict, string key%s",
                             node->name, needs_fallback ? ", and fallback" : "");
                    return false;
                }
                int local = hir_require_local(b, node->children[0]->name,
                                              node->source_line, node->source_col);
                if (local < 0) return false;
                if (!bir_is_owned_dict_type(b->fn->locals[local].type)) {
                    bir_fail(b, node->source_line, node->source_col,
                             "%s target must be an owned dict", node->name);
                    return false;
                }
                if (node->children[1]->type != AST_STRING_LITERAL) {
                    bir_fail(b, node->source_line, node->source_col,
                             "dict keys must be string literals in the backend-IR subset");
                    return false;
                }
                expr = hir_expr_alloc(b, node->source_line, node->source_col);
                if (!expr) return false;
                expr->kind = strcmp(node->name, "has") == 0
                    ? HIR_EXPR_DICT_HAS
                    : (strcmp(node->name, "pop") == 0
                       ? HIR_EXPR_DICT_POP : HIR_EXPR_DICT_GET);
                expr->type = b->module->type_i64;
                expr->local = (uint32_t)local;
                snprintf(expr->dict_key, sizeof(expr->dict_key), "%s",
                         node->children[1]->string_val);
                expr->args = calloc(2, sizeof(HirExpr *));
                if (!expr->args) {
                    hir_expr_free(expr);
                    return false;
                }
                expr->args[0] = hir_expr_alloc(b, node->source_line,
                                               node->source_col);
                if (!expr->args[0]) {
                    hir_expr_free(expr);
                    return false;
                }
                expr->args[0]->kind = HIR_EXPR_LOCAL;
                expr->args[0]->local = (uint32_t)local;
                expr->args[0]->type = b->fn->locals[local].type;
                if (needs_fallback) {
                    if (!hir_build_expr(b, node->children[2], &expr->args[1])) {
                        hir_expr_free(expr);
                        return false;
                    }
                    expr->args[1] = hir_coerce_int_const(b, expr->args[1],
                                                         b->module->type_i64);
                    if (!expr->args[1] ||
                        !bir_types_equal(expr->args[1]->type, b->module->type_i64)) {
                        hir_expr_free(expr);
                        bir_fail(b, node->source_line, node->source_col,
                                 "%s fallback must be i64", node->name);
                        return false;
                    }
                }
                expr->arg_count = needs_fallback ? 2 : 1;
                break;
            }
            if (bir_is_sum_builtin(node->name)) {
                expr = hir_build_sum_builtin(b, node);
                if (!expr) return false;
                break;
            }
            if (hir_builtin_outside_subset(node->name)) {
                bir_fail(b, node->source_line, node->source_col,
                         "builtin '%s' is outside the backend-IR subset", node->name);
                return false;
            }
            expr = hir_expr_alloc(b, node->source_line, node->source_col);
            if (!expr) return false;
            expr->kind = HIR_EXPR_CALL;
            snprintf(expr->callee, sizeof(expr->callee), "%s", node->name);
            const BirFunctionInfo *callee = bir_find_function(b->module, node->name);
            if (!callee) {
                bir_fail(b, node->source_line, node->source_col,
                         "call to unknown function '%s'", node->name);
                hir_expr_free(expr);
                return false;
            }
            if (callee->is_extern) {
                if (node->child_count > BIR_ABI_MAX_GPR_ARGUMENT_REGISTERS) {
                    bir_fail(b, node->source_line, node->source_col,
                             "call '%s' has %zu arguments; imported C functions "
                             "support at most %d in the backend-IR bridge",
                             node->name, node->child_count,
                             BIR_ABI_MAX_GPR_ARGUMENT_REGISTERS);
                    hir_expr_free(expr);
                    return false;
                }
                if (node->child_count) {
                    expr->args = calloc(node->child_count, sizeof(HirExpr *));
                    expr->arg_count = node->child_count;
                    if (!expr->args) {
                        bir_fail(b, node->source_line, node->source_col, "out of memory");
                        hir_expr_free(expr);
                        return false;
                    }
                    for (size_t i = 0; i < node->child_count; i++) {
                        if (!hir_build_expr(b, node->children[i], &expr->args[i])) {
                            hir_expr_free(expr);
                            return false;
                        }
                    }
                }
                expr->type = b->module->type_i64;
                break;
            }
            if (!bir_validate_function_abi(b->module, callee)) {
                bir_fail(b, node->source_line, node->source_col,
                         "call '%s' has invalid ABI metadata", node->name);
                hir_expr_free(expr);
                return false;
            }
            if (node->child_count != callee->param_count) {
                bir_fail(b, node->source_line, node->source_col,
                         "call '%s' has %zu arguments, expected %zu",
                         node->name, node->child_count, callee->param_count);
                hir_expr_free(expr);
                return false;
            }
            if (node->child_count) {
                expr->args = calloc(node->child_count, sizeof(HirExpr *));
                expr->arg_count = node->child_count;
                if (!expr->args) {
                    bir_fail(b, node->source_line, node->source_col, "out of memory");
                    hir_expr_free(expr);
                    return false;
                }
                for (size_t i = 0; i < node->child_count; i++) {
                    if (!hir_build_expr(b, node->children[i], &expr->args[i])) {
                        hir_expr_free(expr);
                        return false;
                    }
                    /* An aggregate-returning call as an argument cannot lower
                       in place: aggregate calls need an explicit destination
                       slot. Hoist the call into a fresh temp local so the
                       argument is a plain local reference (mirrors the sum
                       payload hoist). */
                    if (expr->args[i]->type &&
                        hir_is_aggregate_value_type(expr->args[i]->type) &&
                        expr->args[i]->kind == HIR_EXPR_CALL) {
                        char temp_name[64];
                        snprintf(temp_name, sizeof(temp_name),
                                 "__call_payload_%zu", b->fn->local_count);
                        int temp = hir_add_local(b, temp_name, false,
                                                 expr->args[i]->type,
                                                 expr->args[i]->source_line,
                                                 expr->args[i]->source_col);
                        if (temp < 0) {
                            hir_expr_free(expr);
                            return false;
                        }
                        if (!hir_emit_assign(b, (uint32_t)temp, expr->args[i]))
                            return false;
                        HirExpr *local = hir_expr_alloc(b, node->source_line,
                                                        node->source_col);
                        if (!local) return false;
                        local->kind = HIR_EXPR_LOCAL;
                        local->local = (uint32_t)temp;
                        local->type = expr->args[i]->type;
                        expr->args[i] = local;
                    }
                    if (!hir_complete_float_expr(b, expr->args[i],
                                                 callee->param_types[i])) {
                        hir_expr_free(expr);
                        return false;
                    }
                    if (!hir_coerce_int_const(b, expr->args[i],
                                              callee->param_types[i])) {
                        hir_expr_free(expr);
                        return false;
                    }
                    if (!hir_complete_sum_type(b, expr->args[i],
                                               callee->param_types[i])) {
                        hir_expr_free(expr);
                        return false;
                    }
                    if (!hir_complete_array_type(b, expr->args[i],
                                                 callee->param_types[i])) {
                        hir_expr_free(expr);
                        return false;
                    }
                    if (!bir_call_arg_type_compatible(expr->args[i]->type,
                                                      callee->param_types[i])) {
                        bir_fail(b, node->source_line, node->source_col,
                                 "argument %zu to '%s' has the wrong type",
                                 i + 1, node->name);
                        hir_expr_free(expr);
                        return false;
                    }
                }
            }
            expr->type = callee->has_return ? callee->return_type : b->module->type_void;
            break;
        }
        case AST_COMPTIME_EXPR:
            return hir_build_expr(b, node->child_count ? node->children[0] : NULL, out);
        default:
            bir_fail(b, node->source_line, node->source_col,
                     "expression form is outside the backend-IR subset");
            return false;
    }
    *out = expr;
    return true;
}

/* ------------------------------------------------------------------ */
/* Statements and structured control flow                             */
/* ------------------------------------------------------------------ */

static bool hir_emit_assign(HirBuilder *b, uint32_t local, HirExpr *expr) {
    if (!expr || local >= b->fn->local_count ||
        !bir_types_equal(b->fn->locals[local].type, expr->type)) {
        bir_fail(b, expr ? expr->source_line : 0, expr ? expr->source_col : 0,
                 "assignment type does not match local '%s'",
                 local < b->fn->local_count ? b->fn->locals[local].name : "<invalid>");
        hir_expr_free(expr);
        return false;
    }
    HirStmt stmt;
    memset(&stmt, 0, sizeof(stmt));
    stmt.kind = HIR_STMT_ASSIGN;
    stmt.local = local;
    stmt.expr = expr;
    if (!hir_block_add_stmt(b, b->current, stmt)) {
        hir_expr_free(expr);
        return false;
    }
    return true;
}

static bool hir_build_stmt_list(HirBuilder *b, ASTNode **stmts, size_t stmt_count,
                                 bool *terminated);

/* `with region NAME(capacity): { body }`. The region is entered at the start
   of the body and exited on the body's fall-through continuation. Returns
   inside the body are outside the subset because region release on early
   return is not yet emitted. The capacity expression is accepted but not
   enforced by the isolated evaluator. */
static bool hir_build_with_region(HirBuilder *b, ASTNode *stmt,
                                  HirBlockRef *continue_block) {
    if (stmt->child_count < 2 || !stmt->children[1]) {
        bir_fail(b, stmt->source_line, stmt->source_col,
                 "with region requires a body");
        return false;
    }
    if (b->region_depth >= BIR_MAX_REGIONS ||
        b->module->region_count >= BIR_MAX_REGIONS) {
        bir_fail(b, stmt->source_line, stmt->source_col,
                 "too many nested or declared regions");
        return false;
    }
    uint32_t parent_id = b->region_depth
        ? b->region_stack[b->region_depth - 1].id : BIR_REGION_NONE;
    uint32_t id = (uint32_t)b->module->region_count + 1;
    if (id == BIR_REGION_NONE || id >= BIR_MAX_REGIONS) {
        bir_fail(b, stmt->source_line, stmt->source_col,
                 "region identifier space exhausted");
        return false;
    }
    if (!bir_declare_region(b->module, id, parent_id)) return false;
    b->region_stack[b->region_depth].id = id;
    snprintf(b->region_stack[b->region_depth].name,
             sizeof(b->region_stack[b->region_depth].name), "%s", stmt->name);
    b->region_depth++;

    HirStmt enter;
    memset(&enter, 0, sizeof(enter));
    enter.kind = HIR_STMT_REGION_ENTER;
    enter.local = id;
    if (!hir_block_add_stmt(b, b->current, enter)) {
        b->region_depth--;
        return false;
    }
    bool body_terminated = false;
    if (!hir_build_stmt_list(b, stmt->children[1]->children,
                             stmt->children[1]->child_count, &body_terminated)) {
        b->region_depth--;
        return false;
    }
    b->region_depth--;
    if (body_terminated) {
        bir_fail(b, stmt->source_line, stmt->source_col,
                 "return inside a with region body is outside the backend-IR subset");
        return false;
    }
    HirStmt exit;
    memset(&exit, 0, sizeof(exit));
    exit.kind = HIR_STMT_REGION_EXIT;
    exit.local = id;
    if (!hir_block_add_stmt(b, b->current, exit)) return false;
    if (continue_block) *continue_block = b->current;
    return true;
}

static bool hir_emit_simple(HirBuilder *b, HirStmtKind kind, HirExpr *expr) {
    HirStmt stmt;
    memset(&stmt, 0, sizeof(stmt));
    stmt.kind = kind;
    stmt.expr = expr;
    return hir_block_add_stmt(b, b->current, stmt);
}

static bool hir_build_if(HirBuilder *b, ASTNode *stmt, HirBlockRef *continue_block) {
    if (stmt->child_count < 2) {
        bir_fail(b, stmt->source_line, stmt->source_col,
                 "if statement is missing its body");
        return false;
    }
    HirExpr *cond = NULL;
    if (!hir_build_expr(b, stmt->children[0], &cond)) return false;
    if (!bir_types_equal(cond->type, b->module->type_bool)) {
        bir_fail(b, stmt->source_line, stmt->source_col,
                 "if condition must have bool type");
        return false;
    }

    HirBlock *then_block = hir_new_block(b, "then", stmt->source_line, stmt->source_col);
    HirBlock *else_block = stmt->child_count > 2
        ? hir_new_block(b, "else", stmt->source_line, stmt->source_col) : NULL;
    HirBlock *merge = hir_new_block(b, "merge", stmt->source_line, stmt->source_col);
    if (!then_block || !merge || (stmt->child_count > 2 && !else_block)) return false;

    HirBlockRef pre = b->current;
    HirTerm term;
    memset(&term, 0, sizeof(term));
    term.kind = HIR_TERM_BRANCH;
    term.cond = cond;
    term.target = then_block->id;
    term.target2 = else_block ? else_block->id : merge->id;
    if (!hir_set_term(b, pre, term)) return false;
    if (!hir_add_edge(b, pre, then_block->id) ||
        !hir_add_edge(b, pre, term.target2)) return false;

    b->current = then_block->id;
    bool then_terminated = false;
    if (!hir_build_stmt_list(b, stmt->children[1]->children,
                             stmt->children[1]->child_count, &then_terminated)) {
        return false;
    }
    if (!then_terminated) {
        HirTerm jump;
        memset(&jump, 0, sizeof(jump));
        jump.kind = HIR_TERM_JUMP;
        jump.target = merge->id;
        if (!hir_set_term(b, b->current, jump) ||
            !hir_add_edge(b, b->current, merge->id)) return false;
    }

    if (else_block) {
        b->current = else_block->id;
        bool else_terminated = false;
        if (!hir_build_stmt_list(b, stmt->children[2]->children,
                                 stmt->children[2]->child_count, &else_terminated)) {
            return false;
        }
        if (!else_terminated) {
            HirTerm jump;
            memset(&jump, 0, sizeof(jump));
            jump.kind = HIR_TERM_JUMP;
            jump.target = merge->id;
            if (!hir_set_term(b, b->current, jump) ||
                !hir_add_edge(b, b->current, merge->id)) return false;
        }
    }

    /* The merge block receives its terminator from the continuation: the
       statement list that follows the if, or the function's fall-through
       return. A merge that both branches returned from is unreachable and is
       given a bare return terminator by the function-level fixup. */
    b->current = merge->id;
    *continue_block = merge->id;
    return true;
}

/* match value: { case Enum.Variant: { ... } else: { ... } }

   The target is evaluated once into a synthetic local, then the arms lower
   to an if/else-if chain comparing the local against each variant's
   discriminant, with the else arm (or the merge block) as the fallback.
   Duplicate arms and non-exhaustive matches without an else arm are
   rejected here, mirroring the frontend IR validation. */
static bool hir_build_match(HirBuilder *b, ASTNode *stmt, HirBlockRef *continue_block) {
    if (stmt->child_count < 2) {
        bir_fail(b, stmt->source_line, stmt->source_col,
                 "match requires a value and at least one arm");
        return false;
    }
    HirExpr *target = NULL;
    if (!hir_build_expr(b, stmt->children[0], &target)) return false;
    const bool is_enum = target->type && target->type->kind == COBRA_TYPE_ENUM;
    const bool is_sum = target->type && bir_is_sum_type(target->type);
    if (!is_enum && !is_sum) {
        bir_fail(b, stmt->source_line, stmt->source_col,
                 "match currently requires an enum or Option/Result value");
        hir_expr_free(target);
        return false;
    }
    const BirEnumInfo *enum_info = is_enum
        ? bir_find_enum(b->module, target->type->name) : NULL;
    if (is_enum && !enum_info) {
        bir_fail(b, stmt->source_line, stmt->source_col,
                 "match target enum is not registered");
        hir_expr_free(target);
        return false;
    }
    const bool is_option = is_sum && target->type->kind == COBRA_TYPE_OPTION;

    /* Validate every arm up front: arm shape, pattern ownership, variant or
       keyword existence, binding rules, duplicates, and exhaustiveness. */
    bool seen[COBRA_MAX_ENUM_VARIANTS] = {0};
    bool has_default = false;
    int covered = 0;
    size_t non_default[COBRA_MAX_ENUM_VARIANTS];
    size_t non_default_count = 0;
    int variant_total = is_enum ? (int)enum_info->variant_count : 2;
    for (size_t i = 1; i < stmt->child_count; i++) {
        ASTNode *arm = stmt->children[i];
        if (arm->type != AST_MATCH_CASE || arm->child_count == 0) {
            bir_fail(b, arm ? arm->source_line : stmt->source_line,
                     arm ? arm->source_col : stmt->source_col,
                     "invalid match arm");
            hir_expr_free(target);
            return false;
        }
        if (arm->is_default_case) {
            if (has_default) {
                bir_fail(b, arm->source_line, arm->source_col,
                         "match may contain only one else arm");
                hir_expr_free(target);
                return false;
            }
            has_default = true;
            continue;
        }
        int variant_index = -1;
        if (is_enum) {
            if (strcmp(arm->match_type_name, enum_info->name) != 0) {
                bir_fail(b, arm->source_line, arm->source_col,
                         "match case belongs to a different enum");
                hir_expr_free(target);
                return false;
            }
            int value = bir_enum_variant_value(b->module, enum_info->name,
                                               arm->secondary_name);
            if (value == INT_MIN) {
                bir_fail(b, arm->source_line, arm->source_col,
                         "unknown enum variant '%.20s.%.20s'",
                         enum_info->name, arm->secondary_name);
                hir_expr_free(target);
                return false;
            }
            for (size_t j = 0; j < enum_info->variant_count; j++) {
                if (enum_info->variant_values[j] == value) { variant_index = (int)j; break; }
            }
            /* Payload binding rules: a payload variant binds exactly its
               declared payload arity (`A(P)` binds one struct, `A(i64, i64)`
               binds two); a unit variant binds nothing. */
            const CobraType *payload = variant_index >= 0
                ? enum_info->variant_payloads[variant_index] : NULL;
            size_t binding_count = arm->child_count - 1;
            if (payload) {
                size_t expected = variant_index >= 0
                    ? enum_info->variant_payload_arity[variant_index] : 1;
                if (expected == 0) expected = payload->kind == COBRA_TYPE_STRUCT
                    ? payload->field_count : 1;
                if (binding_count != expected) {
                    bir_fail(b, arm->source_line, arm->source_col,
                             "enum variant '%.20s.%.20s' binds %zu value%s, expected %zu",
                             enum_info->name, arm->secondary_name, binding_count,
                             binding_count == 1 ? "" : "s", expected);
                    hir_expr_free(target);
                    return false;
                }
            } else if (binding_count != 0) {
                bir_fail(b, arm->source_line, arm->source_col,
                         "unit enum variant '%.20s.%.20s' cannot bind a payload",
                         enum_info->name, arm->secondary_name);
                hir_expr_free(target);
                return false;
            }
            arm->int_val = value;
        } else {
            /* Sum patterns: some/none for Option, ok/err for Result. The
               expected tag is 1 for some/ok and 0 for none/err. */
            const char *keyword = arm->match_type_name;
            int expected_tag;
            bool payload_arm;
            if (is_option && strcmp(keyword, "some") == 0) {
                expected_tag = 1; payload_arm = true; variant_index = 0;
            } else if (is_option && strcmp(keyword, "none") == 0) {
                expected_tag = 0; payload_arm = false; variant_index = 1;
            } else if (!is_option && strcmp(keyword, "ok") == 0) {
                expected_tag = 1; payload_arm = true; variant_index = 0;
            } else if (!is_option && strcmp(keyword, "err") == 0) {
                expected_tag = 0; payload_arm = true; variant_index = 1;
            } else {
                bir_fail(b, arm->source_line, arm->source_col,
                         "match pattern '%.20s' does not apply to %s",
                         keyword, is_option ? "an Option" : "a Result");
                hir_expr_free(target);
                return false;
            }
            if (payload_arm && arm->secondary_name[0] == '\0') {
                bir_fail(b, arm->source_line, arm->source_col,
                         "match pattern '%.20s' requires a payload binding",
                         keyword);
                hir_expr_free(target);
                return false;
            }
            if (!payload_arm && arm->secondary_name[0] != '\0') {
                bir_fail(b, arm->source_line, arm->source_col,
                         "match pattern '%.20s' cannot bind a payload", keyword);
                hir_expr_free(target);
                return false;
            }
            arm->int_val = expected_tag;
        }
        if (variant_index >= 0 && seen[variant_index]) {
            bir_fail(b, arm->source_line, arm->source_col,
                     is_enum ? "duplicate enum variant in match"
                             : "duplicate match pattern in match");
            hir_expr_free(target);
            return false;
        }
        if (variant_index >= 0) {
            seen[variant_index] = true;
            covered++;
        }
        if (non_default_count < COBRA_MAX_ENUM_VARIANTS)
            non_default[non_default_count++] = i;
    }
    if (!has_default && covered != variant_total) {
        bir_fail(b, stmt->source_line, stmt->source_col,
                 "non-exhaustive match requires an else arm");
        hir_expr_free(target);
        return false;
    }

    /* Evaluate the target once into a synthetic local. */
    char tmp_name[COBRA_MAX_IDENT_LEN];
    snprintf(tmp_name, sizeof(tmp_name), "@match_%d", b->synthetic_seq++);
    int tmp_local = hir_add_local(b, tmp_name, false, target->type,
                                  stmt->source_line, stmt->source_col);
    if (tmp_local < 0 || !hir_emit_assign(b, (uint32_t)tmp_local, target))
        return false;

    /* Block layout: a comparison block per arm holds the discriminant
       branch; each arm has its own body block. The chain falls out at the
       else block (or straight into the merge when there is no else arm). */
    HirBlock *body_blocks[COBRA_MAX_ENUM_VARIANTS];
    for (size_t i = 0; i < non_default_count; i++) {
        body_blocks[i] = hir_new_block(b, "match_arm", stmt->source_line,
                                       stmt->source_col);
        if (!body_blocks[i]) return false;
    }
    HirBlock *cmp_blocks[COBRA_MAX_ENUM_VARIANTS];
    size_t cmp_count = non_default_count > 0 ? non_default_count - 1 : 0;
    for (size_t i = 0; i < cmp_count; i++) {
        cmp_blocks[i] = hir_new_block(b, "match_cmp", stmt->source_line,
                                      stmt->source_col);
        if (!cmp_blocks[i]) return false;
    }
    HirBlock *else_block = has_default
        ? hir_new_block(b, "match_else", stmt->source_line, stmt->source_col)
        : NULL;
    HirBlock *merge = hir_new_block(b, "match_merge", stmt->source_line,
                                    stmt->source_col);
    if (!merge || (has_default && !else_block)) return false;

    /* The pre block (which holds the target assign) is the first comparison
       block; later comparisons live in cmp_blocks. */
    HirBlockRef chain = b->current;
    for (size_t i = 0; i < non_default_count; i++) {
        ASTNode *arm = stmt->children[non_default[i]];
        HirBlockRef body_blk = body_blocks[i]->id;
        HirBlockRef next = (i + 1 < non_default_count)
            ? cmp_blocks[i]->id
            : (else_block ? else_block->id : merge->id);

        HirExpr *cond = hir_expr_alloc(b, stmt->source_line, stmt->source_col);
        if (!cond) return false;
        if (is_enum && enum_info->has_payloads) {
            /* Payload enum: the arm predicate is a tag read in place.
               Only the discriminant (i64 at offset 0) is compared. */
            cond->kind = HIR_EXPR_SUM_ACCESS;
            cond->type = b->module->type_bool;
            cond->aggregate_type = enum_info->type;
            cond->sum_selector = 0;
            cond->sum_expected_tag = arm->int_val;
            cond->args = calloc(1, sizeof(HirExpr *));
            if (!cond->args) {
                hir_expr_free(cond);
                return false;
            }
            cond->arg_count = 1;
            cond->args[0] = hir_expr_alloc(b, stmt->source_line,
                                           stmt->source_col);
            if (!cond->args[0]) {
                hir_expr_free(cond);
                return false;
            }
            cond->args[0]->kind = HIR_EXPR_LOCAL;
            cond->args[0]->type = enum_info->type;
            cond->args[0]->local = (uint32_t)tmp_local;
        } else if (is_enum) {
            cond->kind = HIR_EXPR_BINOP;
            cond->binop = SSA_OP_EQ;
            cond->type = b->module->type_bool;
            cond->args = calloc(2, sizeof(HirExpr *));
            if (!cond->args) {
                hir_expr_free(cond);
                return false;
            }
            cond->arg_count = 2;
            cond->args[0] = hir_expr_alloc(b, stmt->source_line, stmt->source_col);
            cond->args[1] = hir_expr_alloc(b, stmt->source_line, stmt->source_col);
            if (!cond->args[0] || !cond->args[1]) {
                hir_expr_free(cond);
                return false;
            }
            cond->args[0]->kind = HIR_EXPR_LOCAL;
            cond->args[0]->type = enum_info->type;
            cond->args[0]->local = (uint32_t)tmp_local;
            cond->args[1]->kind = HIR_EXPR_CONST;
            cond->args[1]->type = enum_info->type;
            cond->args[1]->const_value =
                bir_scalar_i64(enum_info->type, arm->int_val);
        } else {
            /* Sum arm: predicate on the synthetic local's tag. */
            cond->kind = HIR_EXPR_SUM_ACCESS;
            cond->type = b->module->type_bool;
            cond->aggregate_type = target->type;
            cond->sum_selector = 0;
            cond->sum_expected_tag = arm->int_val;
            cond->args = calloc(1, sizeof(HirExpr *));
            if (!cond->args) {
                hir_expr_free(cond);
                return false;
            }
            cond->arg_count = 1;
            cond->args[0] = hir_expr_alloc(b, stmt->source_line, stmt->source_col);
            if (!cond->args[0]) {
                hir_expr_free(cond);
                return false;
            }
            cond->args[0]->kind = HIR_EXPR_LOCAL;
            cond->args[0]->type = target->type;
            cond->args[0]->local = (uint32_t)tmp_local;
        }

        b->current = chain;
        HirTerm term;
        memset(&term, 0, sizeof(term));
        term.kind = HIR_TERM_BRANCH;
        term.cond = cond;
        term.target = body_blk;
        term.target2 = next;
        if (!hir_set_term(b, b->current, term) ||
            !hir_add_edge(b, b->current, body_blk) ||
            !hir_add_edge(b, b->current, next)) return false;

        /* Arm body: bind the payload first (sum arms), then the body falls
           through to the merge unless it returns. */
        b->current = body_blk;
        if (is_sum && !is_enum && arm->secondary_name[0] != '\0') {
            const CobraType *payload_type = NULL;
            int selector = 1;
            if (target->type->generic_arg_count >= 2 &&
                strcmp(arm->match_type_name, "err") == 0) {
                /* err(e): bind the error component. */
                payload_type = target->type->generic_args[1];
                selector = 2;
            } else if (target->type->generic_arg_count >= 1) {
                payload_type = target->type->generic_args[0];
            }
            if (!payload_type) {
                bir_fail(b, arm->source_line, arm->source_col,
                         "match pattern cannot resolve its payload type");
                return false;
            }
            int binding = hir_add_local(b, arm->secondary_name, false,
                                        payload_type, arm->source_line,
                                        arm->source_col);
            if (binding < 0) return false;
            HirExpr *access = hir_expr_alloc(b, arm->source_line, arm->source_col);
            if (!access) return false;
            access->kind = HIR_EXPR_SUM_ACCESS;
            access->type = payload_type;
            access->aggregate_type = target->type;
            access->sum_selector = selector;
            access->sum_checked = true;
            access->sum_expected_tag = arm->int_val;
            access->args = calloc(1, sizeof(HirExpr *));
            if (!access->args) {
                hir_expr_free(access);
                return false;
            }
            access->arg_count = 1;
            access->args[0] = hir_expr_alloc(b, arm->source_line, arm->source_col);
            if (!access->args[0]) {
                hir_expr_free(access);
                return false;
            }
            access->args[0]->kind = HIR_EXPR_LOCAL;
            access->args[0]->type = target->type;
            access->args[0]->local = (uint32_t)tmp_local;
            if (!hir_emit_assign(b, (uint32_t)binding, access)) return false;
        }
        if (is_enum && arm->child_count > 1) {
            /* Payload binding: extract the variant's resident payload and
               bind the pattern names. Single payloads bind directly through
               the checked sum-access path; multi-field payloads extract the
               whole payload struct into a private local, then bind each
               field. */
            int variant_index = -1;
            for (size_t j = 0; j < enum_info->variant_count; j++) {
                if (enum_info->variant_values[j] == arm->int_val) {
                    variant_index = (int)j;
                    break;
                }
            }
            const CobraType *payload = variant_index >= 0
                ? enum_info->variant_payloads[variant_index] : NULL;
            size_t binding_count = arm->child_count - 1;
            /* A multi-field payload (arity > 1) splits into per-field
               bindings; a declared single payload binds the whole value. */
            size_t declared_arity = variant_index >= 0
                ? enum_info->variant_payload_arity[variant_index] : 1;
            bool split_fields = declared_arity > 1;
            if (payload && binding_count > 0) {
                int temp = -1;
                if (split_fields) {
                    char tmp_name[COBRA_MAX_IDENT_LEN];
                    snprintf(tmp_name, sizeof(tmp_name), "@match_payload_%d",
                             b->synthetic_seq++);
                    temp = hir_add_local(b, tmp_name, false, payload,
                                         arm->source_line, arm->source_col);
                    if (temp < 0) return false;
                    HirExpr *extract = hir_expr_alloc(b, arm->source_line,
                                                       arm->source_col);
                    if (!extract) return false;
                    extract->kind = HIR_EXPR_SUM_ACCESS;
                    extract->type = payload;
                    extract->aggregate_type = target->type;
                    extract->sum_selector = variant_index + 1;
                    extract->sum_checked = true;
                    extract->sum_expected_tag = arm->int_val;
                    extract->args = calloc(1, sizeof(HirExpr *));
                    if (!extract->args) {
                        hir_expr_free(extract);
                        return false;
                    }
                    extract->arg_count = 1;
                    extract->args[0] = hir_expr_alloc(b, arm->source_line,
                                                       arm->source_col);
                    if (!extract->args[0]) {
                        hir_expr_free(extract);
                        return false;
                    }
                    extract->args[0]->kind = HIR_EXPR_LOCAL;
                    extract->args[0]->type = target->type;
                    extract->args[0]->local = (uint32_t)tmp_local;
                    if (!hir_emit_assign(b, (uint32_t)temp, extract))
                        return false;
                }
                for (size_t k = 0; k < binding_count; k++) {
                    ASTNode *binding_node = arm->children[k];
                    const CobraType *field_type = NULL;
                    size_t field_offset = 0;
                    if (split_fields) {
                        field_type = payload->fields[k].type;
                        field_offset = payload->fields[k].offset;
                    } else {
                        field_type = payload;
                    }
                    if (!field_type) return false;
                    int binding = hir_add_local(b, binding_node->name, false,
                                                field_type, binding_node->source_line,
                                                binding_node->source_col);
                    if (binding < 0) return false;
                    HirExpr *value = hir_expr_alloc(b, binding_node->source_line,
                                                    binding_node->source_col);
                    if (!value) return false;
                    if (split_fields) {
                        /* member read of the extracted payload struct */
                        value->kind = HIR_EXPR_MEMBER;
                        value->type = field_type;
                        value->aggregate_type = payload;
                        value->field_offset = field_offset;
                        value->args = calloc(1, sizeof(HirExpr *));
                        if (!value->args) {
                            hir_expr_free(value);
                            return false;
                        }
                        value->arg_count = 1;
                        value->args[0] = hir_expr_alloc(b, binding_node->source_line,
                                                        binding_node->source_col);
                        if (!value->args[0]) {
                            hir_expr_free(value);
                            return false;
                        }
                        value->args[0]->kind = HIR_EXPR_LOCAL;
                        value->args[0]->type = payload;
                        value->args[0]->local = (uint32_t)temp;
                    } else {
                        value->kind = HIR_EXPR_SUM_ACCESS;
                        value->type = field_type;
                        value->aggregate_type = target->type;
                        value->sum_selector = variant_index + 1;
                        value->sum_checked = true;
                        value->sum_expected_tag = arm->int_val;
                        value->args = calloc(1, sizeof(HirExpr *));
                        if (!value->args) {
                            hir_expr_free(value);
                            return false;
                        }
                        value->arg_count = 1;
                        value->args[0] = hir_expr_alloc(b, binding_node->source_line,
                                                        binding_node->source_col);
                        if (!value->args[0]) {
                            hir_expr_free(value);
                            return false;
                        }
                        value->args[0]->kind = HIR_EXPR_LOCAL;
                        value->args[0]->type = target->type;
                        value->args[0]->local = (uint32_t)tmp_local;
                    }
                    if (!hir_emit_assign(b, (uint32_t)binding, value))
                        return false;
                }
            }
        }
        bool terminated = false;
        ASTNode *arm_body = arm->children[arm->child_count - 1];
        if (!hir_build_stmt_list(b, arm_body->children, arm_body->child_count,
                                 &terminated)) {
            return false;
        }
        if (!terminated) {
            HirTerm jump;
            memset(&jump, 0, sizeof(jump));
            jump.kind = HIR_TERM_JUMP;
            jump.target = merge->id;
            if (!hir_set_term(b, b->current, jump) ||
                !hir_add_edge(b, b->current, merge->id)) return false;
        }
        chain = next;
    }

    /* The chain falls out at the else block or the merge. */
    b->current = chain;
    if (else_block) {
        bool terminated = false;
        ASTNode *else_arm = NULL;
        for (size_t i = 1; i < stmt->child_count; i++) {
            if (stmt->children[i]->is_default_case) { else_arm = stmt->children[i]; break; }
        }
        if (else_arm &&
            !hir_build_stmt_list(b, else_arm->children[0]->children,
                                 else_arm->children[0]->child_count,
                                 &terminated)) {
            return false;
        }
        if (!terminated) {
            HirTerm jump;
            memset(&jump, 0, sizeof(jump));
            jump.kind = HIR_TERM_JUMP;
            jump.target = merge->id;
            if (!hir_set_term(b, b->current, jump) ||
                !hir_add_edge(b, b->current, merge->id)) return false;
        }
        b->current = merge->id;
    }
    *continue_block = merge->id;
    return true;
}

static bool hir_build_while(HirBuilder *b, ASTNode *stmt, HirBlockRef *continue_block) {
    if (stmt->child_count < 2) {
        bir_fail(b, stmt->source_line, stmt->source_col,
                 "while statement is missing its body");
        return false;
    }
    HirBlock *header = hir_new_block(b, "while_header", stmt->source_line, stmt->source_col);
    HirBlock *body = hir_new_block(b, "while_body", stmt->source_line, stmt->source_col);
    HirBlock *exit_block = hir_new_block(b, "while_exit", stmt->source_line, stmt->source_col);
    if (!header || !body || !exit_block) return false;

    HirTerm pre;
    memset(&pre, 0, sizeof(pre));
    pre.kind = HIR_TERM_JUMP;
    pre.target = header->id;
    if (!hir_set_term(b, b->current, pre) || !hir_add_edge(b, b->current, header->id)) {
        return false;
    }

    b->current = header->id;
    HirExpr *cond = NULL;
    if (!hir_build_expr(b, stmt->children[0], &cond)) return false;
    if (!bir_types_equal(cond->type, b->module->type_bool)) {
        bir_fail(b, stmt->source_line, stmt->source_col,
                 "while condition must have bool type");
        return false;
    }
    HirTerm head;
    memset(&head, 0, sizeof(head));
    head.kind = HIR_TERM_BRANCH;
    head.cond = cond;
    head.target = body->id;
    head.target2 = exit_block->id;
    if (!hir_set_term(b, header->id, head) ||
        !hir_add_edge(b, header->id, body->id) ||
        !hir_add_edge(b, header->id, exit_block->id)) {
        return false;
    }

    b->current = body->id;
    bool body_terminated = false;
    if (!hir_build_stmt_list(b, stmt->children[1]->children,
                             stmt->children[1]->child_count, &body_terminated)) {
        return false;
    }
    if (!body_terminated) {
        HirTerm back;
        memset(&back, 0, sizeof(back));
        back.kind = HIR_TERM_JUMP;
        back.target = header->id;
        if (!hir_set_term(b, b->current, back) ||
            !hir_add_edge(b, b->current, header->id)) return false;
    }

    b->current = exit_block->id;
    *continue_block = exit_block->id;
    return true;
}

static bool hir_build_for(HirBuilder *b, ASTNode *stmt, HirBlockRef *continue_block) {
    if (stmt->secondary_name[0]) {
        bir_fail(b, stmt->source_line, stmt->source_col,
                 "for index,value form is outside the backend-IR subset");
        return false;
    }
    if (stmt->child_count < 2) {
        bir_fail(b, stmt->source_line, stmt->source_col,
                 "for statement is missing its body");
        return false;
    }
    ASTNode *target = stmt->children[0];
    ASTNode *body = stmt->children[1];
    int loop_local = hir_add_local(b, stmt->name, false, b->module->type_i64,
                                   stmt->source_line, stmt->source_col);
    if (loop_local < 0) return false;

    /* Constant array literal: unroll each element into a straight-line copy
       of the body. The loop variable is reassigned at the top of every copy,
       which matches the host interpreter's per-iteration rebinding. */
    if (target->type == AST_ARRAY_LITERAL) {
        if (target->child_count > BIR_MAX_ARRAY_UNROLL) {
            bir_fail(b, stmt->source_line, stmt->source_col,
                     "array literal for-loop exceeds the %d-element unroll limit",
                     BIR_MAX_ARRAY_UNROLL);
            return false;
        }
        for (size_t i = 0; i < target->child_count && !b->failed; i++) {
            HirExpr *element = hir_expr_alloc(b, target->source_line, target->source_col);
            if (!element) return false;
            element->kind = HIR_EXPR_CONST;
            element->type = b->module->type_i64;
            element->const_value = bir_scalar_i64(element->type,
                target->children[i]->type == AST_INT_LITERAL
                    ? target->children[i]->literal_i64 : 0);
            if (!hir_emit_assign(b, (uint32_t)loop_local, element)) return false;
            bool body_terminated = false;
            if (!hir_build_stmt_list(b, body->children, body->child_count,
                                     &body_terminated)) {
                return false;
            }
            if (body_terminated) {
                bir_fail(b, stmt->source_line, stmt->source_col,
                         "return inside a for-loop body is outside the backend-IR subset");
                return false;
            }
        }
        *continue_block = b->current;
        return true;
    }

    /* Scalar-bound and range forms lower to the same CFG as a while loop:
       i = start; while i < bound { body; i = i + 1 }. */
    bool is_range = target->type == AST_FUNC_CALL &&
                    strcmp(target->name, "range") == 0;
    if (is_range && (target->child_count < 1 || target->child_count > 2)) {
        bir_fail(b, stmt->source_line, stmt->source_col,
                 "range() requires one or two arguments");
        return false;
    }
    int start_local = hir_synthetic_local(b, "for_start", stmt->source_line, stmt->source_col);
    int bound_local = hir_synthetic_local(b, "for_bound", stmt->source_line, stmt->source_col);
    if (start_local < 0 || bound_local < 0) return false;

    /* Only pre-allocate the constant start; the range form builds its start
       expression directly so nothing is leaked by an overwrite. */
    HirExpr *start_expr = NULL;
    if (is_range && target->child_count == 2) {
        if (!hir_build_expr(b, target->children[0], &start_expr)) return false;
    } else {
        start_expr = hir_expr_alloc(b, stmt->source_line, stmt->source_col);
        if (!start_expr) return false;
        start_expr->kind = HIR_EXPR_CONST;
        start_expr->type = b->module->type_i64;
        start_expr->const_value = bir_scalar_i64(start_expr->type, 0);
    }
    HirExpr *bound_expr = NULL;
    if (is_range) {
        if (!hir_build_expr(b, target->children[target->child_count - 1], &bound_expr)) {
            return false;
        }
    } else {
        if (!hir_build_expr(b, target, &bound_expr)) return false;
        /* A bare `for i in x:` with x not a range()/array-literal only has a
           defined scalar-bound meaning when x is itself an i64 (loop from 0
           to x). Iterating the elements of a slice/list/dict this way needs
           real container-iteration lowering (length read, per-element load,
           writeback) that this backend does not implement yet; without this
           check the code below silently assigns the container value itself
           into an i64 bound local, which is not a type error the type
           checker catches this deep, so guard it explicitly here. */
        if (bound_expr->type != b->module->type_i64) {
            bir_fail(b, stmt->source_line, stmt->source_col,
                     "for-loop over a slice, list, or dict value is outside the backend-IR subset");
            return false;
        }
    }
    if (!hir_emit_assign(b, (uint32_t)start_local, start_expr) ||
        !hir_emit_assign(b, (uint32_t)bound_local, bound_expr)) {
        return false;
    }
    /* i = start; the induction variable is a separate local so the loop body
       can read and assign it without touching the fixed start value. */
    HirExpr *start_ref = hir_expr_alloc(b, stmt->source_line, stmt->source_col);
    if (!start_ref) return false;
    start_ref->kind = HIR_EXPR_LOCAL;
    start_ref->local = (uint32_t)start_local;
    start_ref->type = b->module->type_i64;
    if (!hir_emit_assign(b, (uint32_t)loop_local, start_ref)) return false;

    HirBlock *header = hir_new_block(b, "for_header", stmt->source_line, stmt->source_col);
    HirBlock *body_block = hir_new_block(b, "for_body", stmt->source_line, stmt->source_col);
    HirBlock *latch = hir_new_block(b, "for_latch", stmt->source_line, stmt->source_col);
    HirBlock *exit_block = hir_new_block(b, "for_exit", stmt->source_line, stmt->source_col);
    if (!header || !body_block || !latch || !exit_block) return false;

    HirTerm pre;
    memset(&pre, 0, sizeof(pre));
    pre.kind = HIR_TERM_JUMP;
    pre.target = header->id;
    if (!hir_set_term(b, b->current, pre) || !hir_add_edge(b, b->current, header->id)) {
        return false;
    }

    /* header: i < bound -> body : exit */
    HirExpr *index_ref = hir_expr_alloc(b, stmt->source_line, stmt->source_col);
    HirExpr *bound_ref = hir_expr_alloc(b, stmt->source_line, stmt->source_col);
    if (!index_ref || !bound_ref) return false;
    index_ref->kind = HIR_EXPR_LOCAL;
    index_ref->local = (uint32_t)loop_local;
    index_ref->type = b->module->type_i64;
    bound_ref->kind = HIR_EXPR_LOCAL;
    bound_ref->local = (uint32_t)bound_local;
    bound_ref->type = b->module->type_i64;
    HirExpr *cond = hir_expr_alloc(b, stmt->source_line, stmt->source_col);
    if (!cond) return false;
    cond->kind = HIR_EXPR_BINOP;
    cond->binop = SSA_OP_LT;
    cond->args = calloc(2, sizeof(HirExpr *));
    if (!cond->args) return false;
    cond->args[0] = index_ref;
    cond->args[1] = bound_ref;
    cond->arg_count = 2;
    cond->type = b->module->type_bool;

    b->current = header->id;
    HirTerm head;
    memset(&head, 0, sizeof(head));
    head.kind = HIR_TERM_BRANCH;
    head.cond = cond;
    head.target = body_block->id;
    head.target2 = exit_block->id;
    if (!hir_set_term(b, header->id, head) ||
        !hir_add_edge(b, header->id, body_block->id) ||
        !hir_add_edge(b, header->id, exit_block->id)) {
        return false;
    }

    b->current = body_block->id;
    bool body_terminated = false;
    if (!hir_build_stmt_list(b, body->children, body->child_count,
                             &body_terminated)) {
        return false;
    }
    if (body_terminated) {
        bir_fail(b, stmt->source_line, stmt->source_col,
                 "return inside a for-loop body is outside the backend-IR subset");
        return false;
    }
    HirTerm to_latch;
    memset(&to_latch, 0, sizeof(to_latch));
    to_latch.kind = HIR_TERM_JUMP;
    to_latch.target = latch->id;
    if (!hir_set_term(b, b->current, to_latch) ||
        !hir_add_edge(b, b->current, latch->id)) return false;

    /* latch: i = i + 1; jump header */
    b->current = latch->id;
    HirExpr *one = hir_expr_alloc(b, stmt->source_line, stmt->source_col);
    HirExpr *index_again = hir_expr_alloc(b, stmt->source_line, stmt->source_col);
    HirExpr *plus = hir_expr_alloc(b, stmt->source_line, stmt->source_col);
    if (!one || !index_again || !plus) return false;
    one->kind = HIR_EXPR_CONST;
    one->type = b->module->type_i64;
    one->const_value = bir_scalar_i64(one->type, 1);
    index_again->kind = HIR_EXPR_LOCAL;
    index_again->local = (uint32_t)loop_local;
    index_again->type = b->module->type_i64;
    plus->kind = HIR_EXPR_BINOP;
    plus->binop = SSA_OP_ADD;
    plus->args = calloc(2, sizeof(HirExpr *));
    if (!plus->args) return false;
    plus->args[0] = index_again;
    plus->args[1] = one;
    plus->arg_count = 2;
    plus->type = b->module->type_i64;
    if (!hir_emit_assign(b, (uint32_t)loop_local, plus)) return false;
    HirTerm back;
    memset(&back, 0, sizeof(back));
    back.kind = HIR_TERM_JUMP;
    back.target = header->id;
    if (!hir_set_term(b, latch->id, back) || !hir_add_edge(b, latch->id, header->id)) {
        return false;
    }

    b->current = exit_block->id;
    *continue_block = exit_block->id;
    return true;
}

static bool hir_has_borrowed_view_field(const CobraType *type) {
    if (!type || type->kind != COBRA_TYPE_STRUCT) return false;
    for (size_t i = 0; i < type->field_count; i++) {
        const CobraTypeField *field = &type->fields[i];
        if (bir_is_borrowed_view_type(field->type) ||
            hir_has_borrowed_view_field(field->type)) return true;
    }
    return false;
}

static bool hir_borrowed_view_aggregate_return_is_safe(HirBuilder *b,
                                                        const HirExpr *value) {
    if (!b || !value || !hir_has_borrowed_view_field(b->fn->return_type)) return true;
    /* A generic borrowed-view aggregate may be forwarded from a parameter.
       A local-built aggregate has no lifetime proof at this boundary and must
       not escape its defining frame. */
    return value->kind == HIR_EXPR_LOCAL &&
           value->local < b->fn->local_count &&
           b->fn->locals[value->local].is_param;
}

static bool hir_build_stmt_list(HirBuilder *b, ASTNode **stmts, size_t stmt_count,
                                 bool *terminated) {
    HirBlockRef cur = b->current;
    bool block_terminated = false;
    for (size_t i = 0; i < stmt_count && !b->failed; i++) {
        ASTNode *stmt = stmts[i];
        if (block_terminated) continue; /* dead code after a return */
        switch (stmt->type) {
            case AST_VAR_DECL:
            case AST_ASSIGN: {
                int existing = hir_find_local(b, stmt->name);
                const CobraType *declared = NULL;
                if (stmt->type == AST_VAR_DECL) {
                    declared = bir_import_ast_type(b->module, b->root, stmt, true);
                    if (!declared) {
                        bir_fail(b, stmt->source_line, stmt->source_col,
                                 "declaration type is outside the backend-IR subset");
                        return false;
                    }
                    if (stmt->child_count == 0) {
                        if (declared->kind != COBRA_TYPE_STRUCT &&
                            declared->kind != COBRA_TYPE_ARRAY) {
                            bir_fail(b, stmt->source_line, stmt->source_col,
                                     "declaration without initializer is outside the backend-IR subset");
                            return false;
                        }
                        if (hir_add_local(b, stmt->name, false, declared,
                                          stmt->source_line, stmt->source_col) < 0) return false;
                        break;
                    }
                } else if (existing >= 0) {
                    declared = b->fn->locals[existing].type;
                }
                if (stmt->child_count == 0) {
                    bir_fail(b, stmt->source_line, stmt->source_col,
                             "assignment without an expression");
                    return false;
                }
                HirExpr *value = NULL;
                if (!hir_build_expr(b, stmt->children[0], &value)) return false;
                if (!declared ||
                    (stmt->type == AST_VAR_DECL &&
                     stmt->declared_type == COBRA_TYPE_UNTYPED &&
                     !stmt->canonical_type)) declared = value->type;
                /* A fresh concat assigned to a declared string is an owned
                   string local. String literals remain borrowed views. */
                if (value->type && bir_is_owned_slice_type(value->type) &&
                    (stmt->declared_type == COBRA_TYPE_STRING ||
                     (stmt->canonical_type &&
                      stmt->canonical_type->kind == COBRA_TYPE_STRING))) {
                    declared = value->type;
                }
                /* Owned slice values satisfy a borrowed view alias of the
                   same element; the alias borrows the backing allocation. */
                if (declared && bir_is_borrowed_view_type(declared) &&
                    !bir_types_equal(declared, value->type) &&
                    bir_call_arg_type_compatible(value->type, declared)) {
                    HirExpr *borrow = hir_expr_alloc(b, stmt->source_line,
                                                     stmt->source_col);
                    if (!borrow) {
                        hir_expr_free(value);
                        return false;
                    }
                    borrow->kind = HIR_EXPR_BORROW;
                    borrow->type = declared;
                    borrow->aggregate_type = cobra_type_element(declared);
                    borrow->args = calloc(1, sizeof(HirExpr *));
                    if (!borrow->args) {
                        hir_expr_free(value);
                        hir_expr_free(borrow);
                        return false;
                    }
                    borrow->args[0] = value;
                    borrow->arg_count = 1;
                    value = borrow;
                }
                if (!hir_complete_float_expr(b, value, declared)) {
                    hir_expr_free(value);
                    return false;
                }
                if (!declared) declared = value->type;
                HirExpr *coerced = hir_coerce_int_const(b, value, declared);
                if (!coerced) {
                    hir_expr_free(value);
                    return false;
                }
                value = coerced;
                if (!hir_complete_sum_type(b, value, declared) ||
                    !hir_complete_array_type(b, value, declared)) {
                    hir_expr_free(value);
                    return false;
                }
                if (!declared || !bir_types_equal(declared, value->type)) {
                    bir_fail(b, stmt->source_line, stmt->source_col,
                             "assignment type does not match '%s'", stmt->name);
                    hir_expr_free(value);
                    return false;
                }
                int local = existing >= 0 ? existing :
                    hir_add_local(b, stmt->name, false, declared,
                                  stmt->source_line, stmt->source_col);
                if (local < 0) {
                    hir_expr_free(value);
                    return false;
                }
                if (!hir_emit_assign(b, (uint32_t)local, value)) return false;
                break;
            }
            case AST_RETURN: {
                /* A return on any path inside a with region body would skip
                   the region exit. Region release on early return is not yet
                   emitted, so returns inside regions are rejected. */
                if (b->region_depth > 0) {
                    bir_fail(b, stmt->source_line, stmt->source_col,
                             "return inside a with region body is outside the backend-IR subset");
                    return false;
                }
                HirExpr *value = NULL;
                if (stmt->child_count > 0 &&
                    !hir_build_expr(b, stmt->children[0], &value)) {
                    return false;
                }
                /* Returning a view from caller-owned storage is an explicit
                   borrow conversion. It is valid for both a readonly and an
                   writable generic return, but the source must be an owned
                   slice or an existing compatible view. The SSA/evaluator
                   provenance checks reject frame- and region-local escapes. */
                if (value && bir_is_borrowed_view_type(b->fn->return_type) &&
                    !bir_types_equal(value->type, b->fn->return_type) &&
                    bir_call_arg_type_compatible(value->type, b->fn->return_type)) {
                    HirExpr *borrow = hir_expr_alloc(b, stmt->source_line,
                                                     stmt->source_col);
                    if (!borrow) {
                        hir_expr_free(value);
                        return false;
                    }
                    borrow->kind = HIR_EXPR_BORROW;
                    borrow->type = b->fn->return_type;
                    borrow->aggregate_type = cobra_type_element(b->fn->return_type);
                    borrow->args = calloc(1, sizeof(HirExpr *));
                    if (!borrow->args) {
                        hir_expr_free(value);
                        hir_expr_free(borrow);
                        return false;
                    }
                    borrow->args[0] = value;
                    borrow->arg_count = 1;
                    value = borrow;
                }
                if (value && b->fn->return_type == b->module->type_void) {
                    bir_fail(b, stmt->source_line, stmt->source_col,
                             "void function cannot return a value");
                    hir_expr_free(value);
                    return false;
                }
                if (value && !hir_complete_float_expr(b, value,
                                                      b->fn->return_type)) {
                    hir_expr_free(value);
                    return false;
                }
                if (value) {
                    HirExpr *coerced = hir_coerce_int_const(b, value, b->fn->return_type);
                    if (!coerced) {
                        hir_expr_free(value);
                        return false;
                    }
                    value = coerced;
                }
                if (value && (!hir_complete_sum_type(b, value, b->fn->return_type) ||
                               !hir_complete_array_type(b, value, b->fn->return_type))) {
                    hir_expr_free(value);
                    return false;
                }
                if (value && !bir_types_equal(value->type, b->fn->return_type)) {
                    bir_fail(b, stmt->source_line, stmt->source_col,
                             "return expression has the wrong type");
                    hir_expr_free(value);
                    return false;
                }
                if (value && !hir_borrowed_view_aggregate_return_is_safe(b, value)) {
                    bir_fail(b, stmt->source_line, stmt->source_col,
                             "borrowed view aggregate cannot escape its defining frame");
                    hir_expr_free(value);
                    return false;
                }
                HirTerm term;
                memset(&term, 0, sizeof(term));
                term.kind = HIR_TERM_RETURN;
                term.ret_expr = value;
                if (!hir_set_term(b, b->current, term)) return false;
                block_terminated = true;
                break;
            }
            case AST_MEMBER_ASSIGN: {
                if (stmt->child_count != 2) {
                    bir_fail(b, stmt->source_line, stmt->source_col,
                             "member assignment requires a base and value");
                    return false;
                }
                HirExpr *base = NULL;
                HirExpr *value = NULL;
                if (!hir_build_expr(b, stmt->children[0], &base) ||
                    !hir_build_expr(b, stmt->children[1], &value)) {
                    hir_expr_free(base);
                    hir_expr_free(value);
                    return false;
                }
                if (!base->type || base->type->kind != COBRA_TYPE_STRUCT) {
                    bir_fail(b, stmt->source_line, stmt->source_col,
                             "member assignment requires a scalar-field struct");
                    hir_expr_free(base);
                    hir_expr_free(value);
                    return false;
                }
                const CobraTypeField *field = NULL;
                for (size_t f = 0; f < base->type->field_count; f++) {
                    if (strcmp(base->type->fields[f].name, stmt->secondary_name) == 0) {
                        field = &base->type->fields[f];
                        break;
                    }
                }
                if (!field || (!bir_is_scalar_type(field->type, b->module) &&
                               !bir_is_borrowed_view_type(field->type) &&
                               !bir_type_has_owned_payload(field->type) &&
                               field->type->kind != COBRA_TYPE_STRUCT &&
                               field->type->kind != COBRA_TYPE_ARRAY)) {
                    bir_fail(b, stmt->source_line, stmt->source_col,
                             "member assignment has an incompatible aggregate field");
                    hir_expr_free(base);
                    hir_expr_free(value);
                    return false;
                }
                if (!hir_complete_float_expr(b, value, field->type) ||
                    !hir_complete_sum_type(b, value, field->type) ||
                    !hir_complete_array_type(b, value, field->type)) {
                    hir_expr_free(base);
                    hir_expr_free(value);
                    return false;
                }
                value = hir_coerce_int_const(b, value, field->type);
                if (!value) {
                    hir_expr_free(base);
                    return false;
                }
                if (!bir_types_equal(field->type, value->type)) {
                    bir_fail(b, stmt->source_line, stmt->source_col,
                             "member assignment has an incompatible scalar field");
                    hir_expr_free(base);
                    hir_expr_free(value);
                    return false;
                }
                HirExpr *target = hir_expr_alloc(b, stmt->source_line, stmt->source_col);
                if (!target) {
                    hir_expr_free(base);
                    hir_expr_free(value);
                    return false;
                }
                target->kind = HIR_EXPR_MEMBER;
                target->type = field->type;
                target->aggregate_type = base->type;
                target->field_offset = field->offset;
                target->args = calloc(1, sizeof(HirExpr *));
                if (!target->args) {
                    hir_expr_free(base);
                    hir_expr_free(value);
                    hir_expr_free(target);
                    return false;
                }
                target->args[0] = base;
                target->arg_count = 1;
                HirStmt member_store;
                memset(&member_store, 0, sizeof(member_store));
                member_store.kind = HIR_STMT_MEMBER_ASSIGN;
                member_store.target = target;
                member_store.expr = value;
                if (!hir_block_add_stmt(b, b->current, member_store)) {
                    hir_expr_free(target);
                    hir_expr_free(value);
                    return false;
                }
                break;
            }
            case AST_INDEX_ASSIGN: {
                if (stmt->secondary_name[0] || stmt->child_count != 2) {
                    bir_fail(b, stmt->source_line, stmt->source_col,
                             "writable view or owned slice assignment requires one index and one value");
                    return false;
                }
                int local = hir_require_local(b, stmt->name,
                                              stmt->source_line, stmt->source_col);
                if (local < 0) return false;
                const CobraType *view = b->fn->locals[local].type;
                if (view && bir_is_owned_dict_type(view)) {
                    /* d["key"] = value lowers to a dict insert. */
                    if (stmt->children[0]->type != AST_STRING_LITERAL) {
                        bir_fail(b, stmt->source_line, stmt->source_col,
                                 "dict keys must be string literals in the backend-IR subset");
                        return false;
                    }
                    HirExpr *value = NULL;
                    if (!hir_build_expr(b, stmt->children[1], &value)) return false;
                    value = hir_coerce_int_const(b, value, b->module->type_i64);
                    if (!value || !hir_complete_float_expr(b, value, b->module->type_i64) ||
                        !bir_types_equal(value->type, b->module->type_i64)) {
                        hir_expr_free(value);
                        bir_fail(b, stmt->source_line, stmt->source_col,
                                 "dict values must be i64");
                        return false;
                    }
                    HirStmt dict_set;
                    memset(&dict_set, 0, sizeof(dict_set));
                    dict_set.kind = HIR_STMT_DICT_SET;
                    dict_set.local = (uint32_t)local;
                    dict_set.expr = value;
                    snprintf(dict_set.dict_key, sizeof(dict_set.dict_key), "%s",
                             stmt->children[0]->string_val);
                    if (!hir_block_add_stmt(b, b->current, dict_set)) {
                        hir_expr_free(value);
                        return false;
                    }
                    break;
                }
                bool owned = bir_is_owned_slice_type(view);
                bool owned_buffer = bir_is_owned_buffer_type(view);
                bool fixed_array = view && view->kind == COBRA_TYPE_ARRAY;
                bool writable_view = bir_is_borrowed_view_type(view) &&
                                     bir_view_is_writable(view);
                if (!owned && !owned_buffer && !fixed_array && !writable_view) {
                    bir_fail(b, stmt->source_line, stmt->source_col,
                             "cannot write through a readonly borrowed slice view or unsupported value");
                    return false;
                }
                HirExpr *index = NULL;
                HirExpr *value = NULL;
                if (!hir_build_expr(b, stmt->children[0], &index) ||
                    !hir_build_expr(b, stmt->children[1], &value)) {
                    hir_expr_free(index);
                    hir_expr_free(value);
                    return false;
                }
                const CobraType *element = cobra_type_element(view);
                if (!element || !bir_types_equal(index->type, b->module->type_i64)) {
                    hir_expr_free(index);
                    hir_expr_free(value);
                    bir_fail(b, stmt->source_line, stmt->source_col,
                             "writable view or owned slice assignment has an invalid index");
                    return false;
                }
                if (!hir_complete_float_expr(b, value, element)) {
                    hir_expr_free(index);
                    hir_expr_free(value);
                    return false;
                }
                value = hir_coerce_int_const(b, value, element);
                if (!value || !bir_types_equal(value->type, element)) {
                    hir_expr_free(index);
                    hir_expr_free(value);
                    bir_fail(b, stmt->source_line, stmt->source_col,
                             "writable view or owned slice assignment has an incompatible value type");
                    return false;
                }
                HirExpr *target = hir_expr_alloc(b, stmt->source_line, stmt->source_col);
                if (!target) {
                    hir_expr_free(index);
                    hir_expr_free(value);
                    return false;
                }
                target->kind = HIR_EXPR_INDEX;
                target->type = element;
                target->local = (uint32_t)local;
                target->aggregate_type = view;
                target->args = calloc(1, sizeof(HirExpr *));
                if (!target->args) {
                    hir_expr_free(index);
                    hir_expr_free(value);
                    hir_expr_free(target);
                    return false;
                }
                target->args[0] = index;
                target->arg_count = 1;
                HirStmt index_store;
                memset(&index_store, 0, sizeof(index_store));
                index_store.kind = HIR_STMT_INDEX_ASSIGN;
                index_store.target = target;
                index_store.expr = value;
                if (!hir_block_add_stmt(b, b->current, index_store)) {
                    hir_expr_free(target);
                    hir_expr_free(value);
                    return false;
                }
                break;
            }
            case AST_FUNC_CALL: {
                /* set(d, "key", value) / delete(d, "key"): void dict
                   mutations that run in place on the named dict local. */
                if (strcmp(stmt->name, "set") == 0 ||
                    strcmp(stmt->name, "delete") == 0) {
                    size_t expected = strcmp(stmt->name, "set") == 0 ? 3 : 2;
                    if (stmt->child_count != expected ||
                        stmt->children[0]->type != AST_VAR_REF) {
                        bir_fail(b, stmt->source_line, stmt->source_col,
                                 "%s requires a named dict, string key, and value",
                                 stmt->name);
                        return false;
                    }
                    int local = hir_require_local(b, stmt->children[0]->name,
                                                  stmt->source_line, stmt->source_col);
                    if (local < 0) return false;
                    if (!bir_is_owned_dict_type(b->fn->locals[local].type)) {
                        bir_fail(b, stmt->source_line, stmt->source_col,
                                 "%s target must be an owned dict", stmt->name);
                        return false;
                    }
                    if (stmt->children[1]->type != AST_STRING_LITERAL) {
                        bir_fail(b, stmt->source_line, stmt->source_col,
                                 "dict keys must be string literals in the backend-IR subset");
                        return false;
                    }
                    HirStmt dict_stmt;
                    memset(&dict_stmt, 0, sizeof(dict_stmt));
                    dict_stmt.kind = strcmp(stmt->name, "set") == 0
                        ? HIR_STMT_DICT_SET : HIR_STMT_DICT_DELETE;
                    dict_stmt.local = (uint32_t)local;
                    snprintf(dict_stmt.dict_key, sizeof(dict_stmt.dict_key),
                             "%s", stmt->children[1]->string_val);
                    if (strcmp(stmt->name, "set") == 0) {
                        HirExpr *value = NULL;
                        if (!hir_build_expr(b, stmt->children[2], &value)) return false;
                        value = hir_coerce_int_const(b, value, b->module->type_i64);
                        if (!value ||
                            !hir_complete_float_expr(b, value, b->module->type_i64) ||
                            !bir_types_equal(value->type, b->module->type_i64)) {
                            hir_expr_free(value);
                            bir_fail(b, stmt->source_line, stmt->source_col,
                                     "dict values must be i64");
                            return false;
                        }
                        dict_stmt.expr = value;
                    }
                    if (!hir_block_add_stmt(b, b->current, dict_stmt)) {
                        hir_expr_free(dict_stmt.expr);
                        return false;
                    }
                    break;
                }
                /* free/string_free: explicit destruction of an owned slice or
                   fresh owned string local. */
                if (strcmp(stmt->name, "free") == 0 ||
                    strcmp(stmt->name, "string_free") == 0) {
                    if (stmt->child_count != 1 ||
                        stmt->children[0]->type != AST_VAR_REF) {
                        bir_fail(b, stmt->source_line, stmt->source_col,
                                 "free requires one owned slice or string local");
                        return false;
                    }
                    int local = hir_require_local(b, stmt->children[0]->name,
                                                  stmt->source_line, stmt->source_col);
                    if (local < 0) return false;
                    if (!bir_is_owned_slice_type(b->fn->locals[local].type) &&
                        !bir_is_owned_dict_type(b->fn->locals[local].type) &&
                        !bir_type_has_owned_payload(b->fn->locals[local].type)) {
                        bir_fail(b, stmt->source_line, stmt->source_col,
                                 "free requires an owned slice, string, dict, or owning sum local");
                        return false;
                    }
                    HirStmt f;
                    memset(&f, 0, sizeof(f));
                    f.kind = HIR_STMT_FREE;
                    f.local = (uint32_t)local;
                    if (!hir_block_add_stmt(b, b->current, f)) return false;
                    break;
                }
                /* Expression statement: evaluate the call for effect. */
                HirExpr *value = NULL;
                if (!hir_build_expr(b, stmt, &value)) return false;
                if (!hir_emit_simple(b, HIR_STMT_EXPR, value)) return false;
                break;
            }
            case AST_WITH_REGION: {
                HirBlockRef next = cur;
                if (!hir_build_with_region(b, stmt, &next)) return false;
                cur = next;
                b->current = next;
                break;
            }
            case AST_IF_STMT: {
                HirBlockRef next = cur;
                if (!hir_build_if(b, stmt, &next)) return false;
                cur = next;
                b->current = next;
                break;
            }
            case AST_MATCH_STMT: {
                HirBlockRef next = cur;
                if (!hir_build_match(b, stmt, &next)) return false;
                cur = next;
                b->current = next;
                break;
            }
            case AST_WHILE_STMT: {
                HirBlockRef next = cur;
                if (!hir_build_while(b, stmt, &next)) return false;
                cur = next;
                b->current = next;
                break;
            }
            case AST_FOR_LOOP: {
                HirBlockRef next = cur;
                if (!hir_build_for(b, stmt, &next)) return false;
                cur = next;
                b->current = next;
                break;
            }
            case AST_PRINT_STMT: {
                if (stmt->child_count != 1) {
                    bir_fail(b, stmt->source_line, stmt->source_col,
                             "print requires exactly one argument in the backend-IR subset");
                    return false;
                }
                HirExpr *value = NULL;
                if (!hir_build_expr(b, stmt->children[0], &value)) return false;
                /* Cobra strings are u8 views at the backend-IR layer (there is
                   no distinct STRING kind here): a literal is a borrowed u8
                   view, and a `string` local is an owned u8 slice. Both lower
                   to a MIR_TYPE_VIEW ptr+len pair. */
                const CobraType *element = value->type ? cobra_type_element(value->type) : NULL;
                bool is_string = element && element->kind == COBRA_TYPE_U8 &&
                                 (bir_is_borrowed_view_type(value->type) ||
                                  bir_is_owned_slice_type(value->type));
                if (!is_string) {
                    value = hir_coerce_int_const(b, value, b->module->type_i64);
                    if (!value || !hir_complete_float_expr(b, value, b->module->type_i64) ||
                        !bir_types_equal(value->type, b->module->type_i64)) {
                        hir_expr_free(value);
                        bir_fail(b, stmt->source_line, stmt->source_col,
                                 "print requires a string or i64 argument in the backend-IR subset");
                        return false;
                    }
                }
                HirStmt print_stmt;
                memset(&print_stmt, 0, sizeof(print_stmt));
                print_stmt.kind = HIR_STMT_PRINT;
                print_stmt.expr = value;
                print_stmt.local = is_string ? 1 : 0; /* reuses .local as a 0/1 string flag */
                if (!hir_block_add_stmt(b, b->current, print_stmt)) {
                    hir_expr_free(value);
                    return false;
                }
                break;
            }
            case AST_ASSERT_STMT: {
                if (stmt->child_count != 1) {
                    bir_fail(b, stmt->source_line, stmt->source_col,
                             "assert requires exactly one condition in the backend-IR subset");
                    return false;
                }
                HirExpr *cond = NULL;
                if (!hir_build_expr(b, stmt->children[0], &cond)) return false;
                if (!bir_types_equal(cond->type, b->module->type_bool) &&
                    !bir_types_equal(cond->type, b->module->type_i64)) {
                    hir_expr_free(cond);
                    bir_fail(b, stmt->source_line, stmt->source_col,
                             "assert requires a bool or i64 condition in the backend-IR subset");
                    return false;
                }
                HirStmt assert_stmt;
                memset(&assert_stmt, 0, sizeof(assert_stmt));
                assert_stmt.kind = HIR_STMT_ASSERT;
                assert_stmt.expr = cond;
                if (!hir_block_add_stmt(b, b->current, assert_stmt)) {
                    hir_expr_free(cond);
                    return false;
                }
                break;
            }
            case AST_COMPUTE_BLOCK: {
                /* @compute is a vectorization hint for the direct backend
                   (see codegen.c's AST_COMPUTE_BLOCK case); lowering the
                   wrapped block as plain statements matches that behavior. */
                if (stmt->child_count != 1) {
                    bir_fail(b, stmt->source_line, stmt->source_col,
                             "compute block form is outside the backend-IR subset");
                    return false;
                }
                ASTNode *body = stmt->children[0];
                bool inner_terminated = false;
                if (!hir_build_stmt_list(b, body->children, body->child_count,
                                         &inner_terminated)) {
                    return false;
                }
                cur = b->current;
                if (inner_terminated) block_terminated = true;
                break;
            }
            case AST_INSPECT_STMT:
                /* The `x?` quick inspector has no codegen case in the direct
                   backend either (codegen.c's emit_statement falls through
                   its own default to a silent no-op); match that instead of
                   rejecting a construct the production backend also drops. */
                break;
            default:
                bir_fail(b, stmt->source_line, stmt->source_col,
                         "statement form is outside the backend-IR subset");
                return false;
        }
    }
    if (terminated) *terminated = block_terminated;
    b->current = cur;
    return !b->failed;
}

/* ------------------------------------------------------------------ */
/* Function entry                                                     */
/* ------------------------------------------------------------------ */

static bool hir_prepare_signature(BackendIrModule *module, ASTNode *root,
                                  ASTNode *function,
                                  const CobraType **param_types,
                                  size_t *param_count_out,
                                  const CobraType **return_type_out,
                                  bool *has_return_out) {
    size_t count = 0;
    for (size_t i = 0; i < function->child_count; i++) {
        ASTNode *param = function->children[i];
        if (param->type != AST_PARAM) continue;
        if (count >= BIR_MAX_PARAMS) {
            snprintf(module->error, sizeof(module->error),
                     "%s: too many parameters for backend-IR subset", function->name);
            return false;
        }
        param_types[count] = bir_import_ast_type(module, root, param, true);
        if (!param_types[count]) {
            snprintf(module->error, sizeof(module->error),
                     "%.32s: parameter '%.24s' is outside the backend-IR subset",
                     function->name, param->name);
            return false;
        }
        count++;
    }
    const CobraType *return_type = bir_import_ast_type(module, root, function, true);
    if (!return_type) {
        snprintf(module->error, sizeof(module->error),
                 "%s: return type is outside the backend-IR subset", function->name);
        return false;
    }
    *param_count_out = count;
    *return_type_out = return_type;
    *has_return_out = return_type != module->type_void;
    return true;
}

/* ------------------------------------------------------------------ */
/* Scalar generic function monomorphization                           */
/* ------------------------------------------------------------------ */

static ASTNode *bir_find_source_function(ASTNode *root, const char *name,
                                          bool generic_only) {
    if (!root || !name) return NULL;
    for (size_t i = 0; i < root->child_count; i++) {
        ASTNode *node = root->children[i];
        if (node->type != AST_FUNCTION || strcmp(node->name, name) != 0) continue;
        if (!generic_only || node->generic_param_count > 0) return node;
    }
    return NULL;
}

static ASTNode *bir_find_declared_value(ASTNode *node, const char *name) {
    if (!node || !name) return NULL;
    if ((node->type == AST_PARAM || node->type == AST_VAR_DECL) &&
        strcmp(node->name, name) == 0) return node;
    for (size_t i = 0; i < node->child_count; i++) {
        ASTNode *found = bir_find_declared_value(node->children[i], name);
        if (found) return found;
    }
    return NULL;
}

static const CobraType *bir_generic_actual_type(BackendIrModule *module,
                                                 ASTNode *root,
                                                 ASTNode *function,
                                                 ASTNode *argument) {
    if (!module || !root || !function || !argument) return NULL;
    switch (argument->type) {
        case AST_INT_LITERAL:
            return argument->literal_is_unsigned ? module->type_u64 : module->type_i64;
        case AST_FLOAT_LITERAL:
            return module->type_f32;
        case AST_BOOL_LITERAL:
            return module->type_bool;
        case AST_VAR_REF: {
            ASTNode *decl = bir_find_declared_value(function, argument->name);
            if (!decl) return NULL;
            if (decl->canonical_type && !bir_type_has_generic(decl->canonical_type)) {
                return decl->canonical_type;
            }
            switch (decl->declared_type) {
                case COBRA_TYPE_I64: return module->type_i64;
                case COBRA_TYPE_I32: return module->type_i32;
                case COBRA_TYPE_U32: return module->type_u32;
                case COBRA_TYPE_U64: return module->type_u64;
                case COBRA_TYPE_F32: return module->type_f32;
                case COBRA_TYPE_F64: return module->type_f64;
                case COBRA_TYPE_U8: return module->type_u8;
                case COBRA_TYPE_BOOL: return module->type_bool;
                default: return NULL;
            }
        }
        case AST_BINARY_OP:
            if (argument->child_count == 2 &&
                (strcmp(argument->name, "+") == 0 ||
                 strcmp(argument->name, "-") == 0 ||
                 strcmp(argument->name, "*") == 0 ||
                 strcmp(argument->name, "/") == 0)) {
                return bir_generic_actual_type(module, root, function,
                                                argument->children[0]);
            }
            return NULL;
        case AST_FUNC_CALL: {
            ASTNode *callee = bir_find_source_function(root, argument->name, false);
            if (!callee || callee->generic_param_count > 0 ||
                !callee->canonical_type || bir_type_has_generic(callee->canonical_type)) return NULL;
            return callee->canonical_type;
        }
        default:
            return NULL;
    }
}

static bool bir_generic_binding_for_call(BackendIrModule *module,
                                           ASTNode *root,
                                           ASTNode *function,
                                           ASTNode *generic,
                                           ASTNode *call,
                                           const CobraType **binding_out) {
    if (!module || !root || !function || !generic || !call || !binding_out ||
        generic->generic_param_count != 1 || call->child_count == 0) return false;
    ASTNode *parameter_node = NULL;
    for (size_t i = 0; i < generic->child_count; i++) {
        if (generic->children[i]->type == AST_PARAM) {
            parameter_node = generic->children[i];
            break;
        }
    }
    if (!parameter_node || !parameter_node->canonical_type) return false;
    const CobraType *actual = bir_generic_actual_type(
        module, root, function, call->children[0]);
    if (!actual) return false;
    const CobraType *binding = NULL;
    if (!cobra_type_bind_generic(parameter_node->canonical_type, actual,
                                 generic->generic_param_types[0], &binding))
        return false;
    if (!binding || !cobra_type_is_scalar(binding)) return false;
    *binding_out = binding;
    return true;
}

static ASTNode *bir_clone_ast_tree(const ASTNode *source) {
    if (!source) return NULL;
    ASTNode *copy = ast_create_node(source->type, source->name);
    if (!copy) return NULL;
    *copy = *source;
    copy->children = NULL;
    copy->child_count = 0;
    copy->child_capacity = 0;
    for (size_t i = 0; i < source->child_count; i++) {
        ASTNode *child = bir_clone_ast_tree(source->children[i]);
        if (!child) {
            ast_free(copy);
            return NULL;
        }
        ast_add_child(copy, child);
    }
    return copy;
}

static bool bir_specialize_ast_tree(BackendIrModule *module, ASTNode *node,
                                    const CobraType *parameter,
                                    const CobraType *argument) {
    if (!module || !node || !parameter || !argument) return false;
    if (node->canonical_type && bir_type_has_generic(node->canonical_type)) {
        const CobraType *template_type = node->canonical_type;
        if (template_type->kind == COBRA_TYPE_STRUCT &&
            template_type->generic_arg_count == 1 &&
            template_type->field_count == 0 && module->source_root &&
            module->source_root->canonical_arena) {
            const CobraType *resolved = cobra_type_struct_layout(
                module->source_root->canonical_arena, module->source_root,
                template_type->name);
            if (resolved) template_type = resolved;
        }
        CobraTypeBinding binding = {parameter, argument};
        char specialized_name[COBRA_MAX_IDENT_LEN] = "";
        const char *name_override = NULL;
        if (node->canonical_type->kind == COBRA_TYPE_STRUCT &&
            node->canonical_type->name[0]) {
            snprintf(specialized_name, sizeof(specialized_name), "%.46s__%.15s",
                     node->canonical_type->name, cobra_type_kind_name(argument->kind));
            name_override = specialized_name;
        }
        const CobraType *specialized = cobra_type_substitute(
            module->type_arena, template_type, &binding, 1, name_override);
        if (!specialized) {
            snprintf(module->error, sizeof(module->error),
                     "generic specialization of '%.40s' has no valid canonical type",
                     node->name[0] ? node->name : "<anonymous>");
            return false;
        }
        node->canonical_type = specialized;
        if (node->declared_type != COBRA_TYPE_UNTYPED)
            node->declared_type = specialized->kind;
        if (node->value_type != COBRA_TYPE_UNTYPED)
            node->value_type = specialized->kind;
    }
    for (size_t i = 0; i < node->child_count; i++) {
        if (!bir_specialize_ast_tree(module, node->children[i], parameter, argument))
            return false;
    }
    return true;
}

static ASTNode *bir_specialize_generic_function(BackendIrModule *module,
                                                 ASTNode *generic,
                                                 const CobraType *argument) {
    if (!module || !module->source_root || !generic ||
        generic->generic_param_count != 1 || !generic->generic_param_types[0] ||
        !argument) return NULL;
    if (!cobra_type_is_scalar(argument)) {
        snprintf(module->error, sizeof(module->error),
                 "generic function '%.40s' requires a scalar argument",
                 generic->name);
        return NULL;
    }
    for (size_t i = 0; i < module->source_root->child_count; i++) {
        ASTNode *candidate = module->source_root->children[i];
        if (candidate->type == AST_FUNCTION &&
            candidate->specialized_from == generic &&
            candidate->specialization_arg_count == 1 &&
            candidate->specialization_args[0] &&
            cobra_type_equal(candidate->specialization_args[0], argument)) {
            return candidate;
        }
    }
    char specialized_name[COBRA_MAX_IDENT_LEN];
    snprintf(specialized_name, sizeof(specialized_name), "%.47s__%s",
             generic->name, cobra_type_kind_name(argument->kind));
    if (bir_find_source_function(module->source_root, specialized_name, false)) {
        snprintf(module->error, sizeof(module->error),
                 "generic specialization name collides with '%s'", specialized_name);
        return NULL;
    }
    ASTNode *specialized = bir_clone_ast_tree(generic);
    if (!specialized) {
        snprintf(module->error, sizeof(module->error),
                 "could not clone generic function '%s'", generic->name);
        return NULL;
    }
    snprintf(specialized->name, sizeof(specialized->name), "%s", specialized_name);
    specialized->generic_param_count = 0;
    memset(specialized->generic_param_names, 0,
           sizeof(specialized->generic_param_names));
    memset(specialized->generic_param_types, 0,
           sizeof(specialized->generic_param_types));
    specialized->specialized_from = generic;
    specialized->specialization_arg_count = 1;
    specialized->specialization_args[0] = argument;
    if (!bir_specialize_ast_tree(module, specialized,
                                 generic->generic_param_types[0], argument)) {
        ast_free(specialized);
        return NULL;
    }
    ast_add_child(module->source_root, specialized);
    return specialized;
}

static bool bir_specialize_calls_in_node(BackendIrModule *module,
                                          ASTNode *owner_function,
                                          ASTNode *node) {
    if (!module || !owner_function || !node) return false;
    for (size_t i = 0; i < node->child_count; i++) {
        ASTNode *child = node->children[i];
        if (child->type == AST_FUNC_CALL) {
            ASTNode *generic = bir_find_source_function(module->source_root,
                                                        child->name, true);
            if (generic) {
                if (child->child_count == 0) {
                    snprintf(module->error, sizeof(module->error),
                             "generic call '%.40s' cannot infer its scalar type argument",
                             child->name);
                    return false;
                }
                const CobraType *argument = NULL;
                if (!bir_generic_binding_for_call(module, module->source_root,
                                                  owner_function, generic, child,
                                                  &argument)) {
                    snprintf(module->error, sizeof(module->error),
                             "generic call '%.40s' has incompatible or unknown type arguments",
                             child->name);
                    return false;
                }
                ASTNode *specialized = bir_specialize_generic_function(module,
                                                                        generic,
                                                                        argument);
                if (!specialized) return false;
                snprintf(child->name, sizeof(child->name), "%s", specialized->name);
            }
        }
        if (!bir_specialize_calls_in_node(module, owner_function, child)) return false;
    }
    return true;
}

static bool bir_specialize_calls(BackendIrModule *module, ASTNode *function) {
    return module && function &&
           bir_specialize_calls_in_node(module, function, function);
}

static ASTNode *bir_find_source_struct(ASTNode *root, const char *name) {
    if (!root || !name) return NULL;
    for (size_t i = 0; i < root->child_count; i++) {
        ASTNode *node = root->children[i];
        if (node->type == AST_STRUCT_DECL && strcmp(node->name, name) == 0)
            return node;
    }
    return NULL;
}

static bool bir_generic_struct_scalar_layout(const CobraType *type) {
    if (!type || type->kind != COBRA_TYPE_STRUCT || !type->finalized) return false;
    for (size_t i = 0; i < type->field_count; i++) {
        const CobraTypeField *field = &type->fields[i];
        if (field->ownership == COBRA_OWNERSHIP_BORROWED &&
            field->mutability == COBRA_MUTABILITY_READONLY && field->region_id == -1 &&
            cobra_type_is_slice_kind(field->type ? field->type->kind : COBRA_TYPE_UNKNOWN) &&
            field->type->generic_arg_count == 1 &&
            cobra_type_is_scalar(field->type->generic_args[0])) continue;
        if (field->ownership != COBRA_OWNERSHIP_VALUE ||
            field->mutability != COBRA_MUTABILITY_DEFAULT || field->region_id != -1)
            return false;
        if (bir_is_scalar_type(field->type, NULL)) continue;
        if (!bir_generic_struct_scalar_layout(field->type)) return false;
    }
    return true;
}

static bool bir_specialize_source_struct_node(BackendIrModule *module,
                                              ASTNode *root, ASTNode *node) {
    if (!module || !root || !node) return false;
    if (node->canonical_type && node->canonical_type->kind == COBRA_TYPE_STRUCT &&
        node->canonical_type->generic_arg_count == 1 &&
        !bir_type_has_generic(node->canonical_type)) {
        const CobraType *requested = node->canonical_type;
        const CobraType *argument = requested->generic_args[0];
        ASTNode *template_decl = bir_find_source_struct(root, requested->name);
        if (!template_decl || template_decl->generic_param_count != 1 ||
            !template_decl->generic_param_types[0] || !cobra_type_is_scalar(argument)) {
            snprintf(module->error, sizeof(module->error),
                     "generic struct '%.40s' requires one scalar type argument",
                     requested->name);
            return false;
        }
        const CobraType *template_type = cobra_type_struct_layout(
            root->canonical_arena, root, template_decl->name);
        if (!template_type) {
            snprintf(module->error, sizeof(module->error),
                     "generic struct '%.40s' has an invalid canonical layout",
                     requested->name);
            return false;
        }
        char specialized_name[COBRA_MAX_IDENT_LEN];
        snprintf(specialized_name, sizeof(specialized_name), "%.46s__%.15s",
                 requested->name, cobra_type_kind_name(argument->kind));
        CobraTypeBinding binding = {template_decl->generic_param_types[0], argument};
        const CobraType *specialized = cobra_type_substitute(
            root->canonical_arena, template_type, &binding, 1, specialized_name);
        if (!specialized || !bir_generic_struct_scalar_layout(specialized)) {
            snprintf(module->error, sizeof(module->error),
                     "generic struct '%.40s' has unsupported ownership or field layout",
                     requested->name);
            return false;
        }
        if (!bir_import_source_struct(module, specialized)) {
            snprintf(module->error, sizeof(module->error),
                     "generic struct '%.40s' could not be imported into backend IR",
                     requested->name);
            return false;
        }
        node->canonical_type = specialized;
        node->declared_type = COBRA_TYPE_STRUCT;
        if (node->value_type != COBRA_TYPE_UNTYPED)
            node->value_type = COBRA_TYPE_STRUCT;
    }
    if (node->type == AST_STRUCT_DECL && node->generic_param_count > 0)
        return true;
    for (size_t i = 0; i < node->child_count; i++) {
        if (!bir_specialize_source_struct_node(module, root, node->children[i]))
            return false;
    }
    return true;
}

bool bir_build_function(BackendIrModule *module, ASTNode *function,
                        BirFunctionInfo **out_info) {
    if (!module || !function || function->type != AST_FUNCTION) {
        if (module) snprintf(module->error, sizeof(module->error),
                             "backend-IR builder requires an AST_FUNCTION node");
        return false;
    }
    module->error[0] = '\0';
    if (function->generic_param_count > 0) {
        snprintf(module->error, sizeof(module->error),
                 "%s: generic functions are outside the backend-IR subset",
                 function->name);
        return false;
    }
    const CobraType *signature_params[BIR_MAX_PARAMS] = {0};
    size_t signature_count = 0;
    const CobraType *signature_return = NULL;
    bool has_return = false;
    if (!hir_prepare_signature(module, module->source_root, function, signature_params,
                               &signature_count, &signature_return, &has_return)) {
        return false;
    }
    const BirFunctionInfo *declared = bir_find_function(module, function->name);
    if (!declared && !bir_declare_function(module, function->name, signature_count,
                                           signature_params, signature_return,
                                           has_return)) {
        return false;
    }

    HirBuilder b;
    memset(&b, 0, sizeof(b));
    b.module = module;
    b.root = module->source_root;
    b.current = SSA_BLOCK_NONE;

    HirFunction fn;
    memset(&fn, 0, sizeof(fn));
    snprintf(fn.name, sizeof(fn.name), "%s", function->name);
    b.fn = &fn;

    size_t param_count = signature_count;
    fn.param_count = param_count;
    fn.return_type = signature_return;
    for (size_t i = 0; i < param_count; i++) fn.param_types[i] = signature_params[i];

    HirBlock *entry = hir_new_block(&b, "entry", function->source_line, function->source_col);
    if (!entry) goto fail;
    entry->is_entry = true;
    b.current = entry->id;

    /* Parameters are locals 0..param_count-1. */
    param_count = 0;
    for (size_t i = 0; i < function->child_count; i++) {
        ASTNode *child = function->children[i];
        if (child->type != AST_PARAM) continue;
        const CobraType *param_type = signature_params[param_count];
        int local = hir_add_local(&b, child->name, true, param_type,
                                  child->source_line, child->source_col);
        if (local < 0 || (size_t)local != param_count) {
            bir_fail(&b, child->source_line, child->source_col,
                     "internal error: parameter local ordering");
            goto fail;
        }
        param_count++;
    }

    /* Body statements; a bare fall-through returns 0 for i64 (matching the
       host interpreter) or returns void. */
    bool body_terminated = false;
    size_t body_count = 0;
    for (size_t i = 0; i < function->child_count; i++) {
        if (function->children[i]->type != AST_PARAM) body_count++;
    }
    ASTNode **body_stmts = NULL;
    if (body_count) {
        body_stmts = malloc(sizeof(ASTNode *) * body_count);
        if (!body_stmts) {
            snprintf(module->error, sizeof(module->error), "out of memory");
            goto fail;
        }
        size_t index = 0;
        for (size_t i = 0; i < function->child_count; i++) {
            ASTNode *child = function->children[i];
            if (child->type == AST_PARAM) continue;
            body_stmts[index++] = child;
        }
    }
    if (!hir_build_stmt_list(&b, body_stmts, body_count, &body_terminated)) {
        free(body_stmts);
        goto fail;
    }
    free(body_stmts);
    if (b.failed) goto fail;

    /* Fall-through: i64 -> ret 0; void -> bare ret. */
    if (!body_terminated && b.fn->blocks[b.current].term.kind == HIR_TERM_NONE) {
        HirTerm term;
        memset(&term, 0, sizeof(term));
        term.kind = HIR_TERM_RETURN;
        if (has_return && signature_return->kind != COBRA_TYPE_STRUCT &&
            signature_return->kind != COBRA_TYPE_ARRAY &&
            !bir_is_sum_type(signature_return)) {
            HirExpr *zero = hir_expr_alloc(&b, function->source_line, function->source_col);
            if (!zero) goto fail;
            zero->kind = HIR_EXPR_CONST;
            zero->type = signature_return;
            zero->const_value = signature_return == module->type_f32
                ? bir_scalar_f32(zero->type, 0.0f)
                : bir_scalar_i64(zero->type, 0);
            term.ret_expr = zero;
        }
        if (!hir_set_term(&b, b.current, term)) goto fail;
    }
    /* Every block must carry a terminator. */
    for (size_t i = 0; i < fn.block_count; i++) {
        if (fn.blocks[i].term.kind == HIR_TERM_NONE) {
            HirTerm term;
            memset(&term, 0, sizeof(term));
            term.kind = HIR_TERM_RETURN;
            if (has_return && signature_return->kind != COBRA_TYPE_STRUCT &&
                !bir_is_sum_type(signature_return)) {
                HirExpr *zero = hir_expr_alloc(&b, function->source_line, function->source_col);
                if (!zero) goto fail;
                zero->kind = HIR_EXPR_CONST;
                zero->type = signature_return;
                zero->const_value = signature_return == module->type_f32
                    ? bir_scalar_f32(zero->type, 0.0f)
                    : bir_scalar_i64(zero->type, 0);
                term.ret_expr = zero;
            }
            fn.blocks[i].term = term;
        }
    }

    /* Hand the HIR function to the SSA pass (ssa_pass.c). The pass creates
       the flat SSA blocks and returns the arena entry block for this
       function (block ids are module-global and not per-function). */
    extern bool bir_ssa_lower(BackendIrModule *module, HirFunction *fn,
                              SsaBlockRef *entry_out, SsaValueRef *param_refs_out);
    SsaBlockRef entry_ref = SSA_BLOCK_NONE;
    SsaValueRef param_refs[BIR_MAX_SSA_PARAMS];
    memset(param_refs, 0, sizeof(param_refs));
    if (!bir_ssa_lower(module, &fn, &entry_ref, param_refs)) goto fail;
    if (entry_ref == SSA_BLOCK_NONE ||
        !bir_register_function_info(module, function->name, entry_ref, param_count,
                                    param_refs, signature_return, has_return)) {
        goto fail;
    }

    if (out_info) *out_info = (BirFunctionInfo *)bir_find_function(module, function->name);
    hir_function_free(&fn);
    return true;

fail:
    hir_function_free(&fn);
    return false;
}

/* Collects every AST_FUNC_CALL callee name reachable under `node` into
   `names`, growing it as needed. Used to compute which top-level functions
   a program actually needs, so a function that fails to lower (an
   unsupported construct) only aborts the build when something reachable
   from main actually calls it. */
static void collect_called_names(ASTNode *node, char names[][COBRA_MAX_IDENT_LEN], size_t *count, size_t cap) {
    if (!node) return;
    if (node->type == AST_FUNC_CALL && node->name[0]) {
        bool seen = false;
        for (size_t i = 0; i < *count; i++) {
            if (strcmp(names[i], node->name) == 0) { seen = true; break; }
        }
        if (!seen && *count < cap) {
            snprintf(names[*count], COBRA_MAX_IDENT_LEN, "%s", node->name);
            (*count)++;
        }
    }
    for (size_t i = 0; i < node->child_count; i++) collect_called_names(node->children[i], names, count, cap);
}

/* Marks `reachable[i]` true for every top-level function in `root` that is
   transitively called from "main" (or is main itself). BFS over static
   call names; a name that doesn't resolve to a declared top-level function
   (an FFI/prelude builtin, for instance) is simply not walked further. */
static void mark_reachable_functions(ASTNode *root, size_t original_count, bool *reachable) {
    char worklist[512][COBRA_MAX_IDENT_LEN];
    size_t worklist_count = 0;
    for (size_t i = 0; i < original_count; i++) {
        ASTNode *decl = root->children[i];
        if (decl->type == AST_FUNCTION && strcmp(decl->name, "main") == 0 &&
            worklist_count < 512) {
            snprintf(worklist[worklist_count], COBRA_MAX_IDENT_LEN, "%s", decl->name);
            worklist_count++;
        }
    }
    if (worklist_count == 0) {
        /* No "main" in this compilation unit (a library module, or a unit
           test that lowers a function directly without an entry point).
           There is no safe notion of "unreachable" here, so treat every
           top-level function as reachable and preserve the old
           fail-on-any-error behavior. */
        for (size_t i = 0; i < original_count; i++) {
            if (root->children[i]->type == AST_FUNCTION) reachable[i] = true;
        }
        return;
    }
    for (size_t w = 0; w < worklist_count; w++) {
        for (size_t i = 0; i < original_count; i++) {
            ASTNode *decl = root->children[i];
            if (decl->type != AST_FUNCTION || strcmp(decl->name, worklist[w]) != 0) continue;
            if (reachable[i]) break;
            reachable[i] = true;
            char called[512][COBRA_MAX_IDENT_LEN];
            size_t called_count = 0;
            collect_called_names(decl, called, &called_count, 512);
            for (size_t c = 0; c < called_count; c++) {
                bool already_queued = false;
                for (size_t q = 0; q < worklist_count; q++) {
                    if (strcmp(worklist[q], called[c]) == 0) { already_queued = true; break; }
                }
                if (!already_queued && worklist_count < 512) {
                    snprintf(worklist[worklist_count], COBRA_MAX_IDENT_LEN, "%s", called[c]);
                    worklist_count++;
                }
            }
            break;
        }
    }
}

/* Removes a function's placeholder BirFunctionInfo entry (added by the
   unconditional bir_declare_function predeclaration pass) from the
   module's function table. Used when a function's body fails to lower and
   is being skipped as unreachable: without this, the placeholder's
   SSA_BLOCK_NONE/zero block range stays in module->functions and a later
   pass that walks every declared function trips over it. Safe because
   bir_find_function resolves by name, not index, and nothing reachable
   from main calls this function (already verified by the caller). */
static void remove_declared_function(BackendIrModule *module, const char *name) {
    for (size_t i = 0; i < module->function_count; i++) {
        if (strcmp(module->functions[i].name, name) != 0) continue;
        for (size_t j = i + 1; j < module->function_count; j++) {
            module->functions[j - 1] = module->functions[j];
        }
        module->function_count--;
        return;
    }
}

bool bir_build_program(BackendIrModule *module, ASTNode *root) {
    if (!module || !root) return false;
    module->error[0] = '\0';
    module->source_root = root;

    size_t original_count = root->child_count;

    /* Materialize concrete immutable scalar generic struct references before
       generic call-site binding. Generic struct declarations remain templates
       and are never registered directly. */
    for (size_t i = 0; i < original_count; i++) {
        ASTNode *decl = root->children[i];
        if (decl->type == AST_FUNCTION && decl->generic_param_count > 0) continue;
        if (!bir_specialize_source_struct_node(module, root, decl)) return false;
    }

    /* Materialize generic function calls after concrete aggregate arguments
       exist, so View[T] calls can bind T from a finalized View__scalar type. */
    for (size_t i = 0; i < original_count; i++) {
        ASTNode *decl = root->children[i];
        if (decl->type == AST_FUNCTION && decl->generic_param_count == 0 &&
            !bir_specialize_calls(module, decl)) return false;
    }

    /* Register unit enum declarations before any type resolution so that
       enum type annotations, variant references, and signatures resolve.
       Unit enums carry no ownership or payloads in this lane. */
    for (size_t i = 0; i < root->child_count; i++) {
        ASTNode *decl = root->children[i];
        if (decl->type != AST_ENUM_DECL) continue;
        if (decl->generic_param_count > 0 ||
            !bir_import_source_enum(module, decl)) {
            snprintf(module->error, sizeof(module->error),
                     "%.40s: enum is outside the backend-IR unit-enum subset",
                     decl->name);

            return false;
        }
    }

    /* Resolve and import fixed-layout scalar structs before function
       signatures. The frontend layout is canonical; the backend copies only
       value-owned scalar fields into its own finalized arena. */
    for (size_t i = 0; i < root->child_count; i++) {
        ASTNode *decl = root->children[i];
        if (decl->type != AST_STRUCT_DECL) continue;
        if (decl->generic_param_count > 0) continue;
        if (!bir_import_ast_type(module, root, decl, false)) {
            snprintf(module->error, sizeof(module->error),
                     "%.40s: aggregate is outside the backend-IR scalar-struct subset",
                     decl->name);
            return false;
        }
    }

    /* Register `import c "lib.so" (funcs...)` names before function
       signatures so calls to them resolve like any other predeclared
       function. Imports carry no declared signature (see is_imported_function
       in the direct backend's ir.c); the extern call path below applies the
       same raw i64/pointer SysV convention as emit_import_call there. */
    for (size_t i = 0; i < root->child_count; i++) {
        ASTNode *decl = root->children[i];
        if (decl->type != AST_IMPORT_DECL) continue;
        for (size_t j = 0; j < decl->child_count; j++) {
            ASTNode *ref = decl->children[j];
            if (ref->type != AST_VAR_REF) continue;
            if (!bir_declare_extern_function(module, ref->name)) return false;
        }
    }

    /* Predeclare every signature before lowering bodies. This makes forward,
       recursive, and mutually recursive calls type-checkable in HIR. */
    for (size_t i = 0; i < root->child_count; i++) {
        ASTNode *decl = root->children[i];
        if (decl->type != AST_FUNCTION) continue;
        if (decl->generic_param_count > 0) continue;
        const CobraType *params[BIR_MAX_PARAMS] = {0};
        size_t param_count = 0;
        const CobraType *return_type = NULL;
        bool has_return = false;
        if (!hir_prepare_signature(module, root, decl, params, &param_count,
                                   &return_type, &has_return) ||
            !bir_declare_function(module, decl->name, param_count, params,
                                  return_type, has_return)) {

            return false;
        }
    }

    bool *reachable = (bool *)calloc(root->child_count ? root->child_count : 1, sizeof(bool));
    if (!reachable) {
        snprintf(module->error, sizeof(module->error), "out of memory computing reachability");
        return false;
    }
    mark_reachable_functions(root, root->child_count, reachable);

    for (size_t i = 0; i < root->child_count; i++) {
        ASTNode *decl = root->children[i];
        if (decl->type == AST_FUNCTION) {
            if (decl->generic_param_count > 0) continue;
            /* Snapshot the module's SSA arena before attempting this
               function, so a failed-and-skipped (unreachable) function's
               partially-lowered blocks/values/insts don't leak into later
               passes that walk the module's global SSA pools. */
            size_t snap_values = module->arena.value_count;
            size_t snap_insts = module->arena.inst_count;
            size_t snap_blocks = module->arena.block_count;
            size_t snap_operands = module->arena.operand_used;
            size_t snap_edges = module->arena.edge_used;
            if (!bir_build_function(module, decl, NULL)) {
                /* Only a "construct not supported by this backend yet"
                   failure is eligible to be skipped; a genuine semantic
                   error (ownership escape, type mismatch, etc.) must still
                   fail the whole build even for unreachable functions,
                   since it means the program itself is invalid. */
                bool is_unsupported_gap = strstr(module->error, "outside the backend-IR subset") != NULL;
                if (reachable[i] || !is_unsupported_gap) {
                    free(reachable);
                    return false;
                }
                /* Not reachable from main: this function's body uses a
                   construct the isolated backend doesn't support yet, but
                   nothing the program actually runs needs it, so omit it
                   rather than failing the whole module. Roll back any
                   partial SSA state the failed attempt left behind. */
                module->arena.value_count = snap_values;
                module->arena.inst_count = snap_insts;
                module->arena.block_count = snap_blocks;
                module->arena.operand_used = snap_operands;
                module->arena.edge_used = snap_edges;
                remove_declared_function(module, decl->name);
                continue;
            }
            continue;
        }
        if (decl->type == AST_STRUCT_DECL || decl->type == AST_ENUM_DECL) continue;
        if (decl->type == AST_IMPORT_DECL) continue;
        snprintf(module->error, sizeof(module->error),
                 "%.60s:%d:%d: top-level declaration is outside the backend-IR subset",
                 module->source_file, decl->source_line, decl->source_col);
        free(reachable);
        return false;
    }

    free(reachable);
    return true;
}

const BirFunctionInfo *bir_find_function(const BackendIrModule *module,
                                         const char *name) {
    if (!module) return NULL;
    for (size_t i = 0; i < module->function_count; i++) {
        if (strcmp(module->functions[i].name, name) == 0) {
            return &module->functions[i];
        }
    }
    return NULL;
}

void bir_module_init(BackendIrModule *module, const char *source_file) {
    memset(module, 0, sizeof(*module));
    bir_arena_init(&module->arena);
    if (source_file) {
        snprintf(module->source_file, sizeof(module->source_file), "%s", source_file);
    } else {
        snprintf(module->source_file, sizeof(module->source_file), "<source>");
    }
    module->type_arena = calloc(1, sizeof(CobraTypeArena));
    if (!module->type_arena) {
        /* Allocation failure: leave types unset; callers must check for NULL. */
        return;
    }
    cobra_type_arena_init(module->type_arena);
    module->type_i64 = cobra_type_make(module->type_arena, COBRA_TYPE_I64, NULL,
                                       NULL, NULL, NULL, NULL,
                                       COBRA_OWNERSHIP_VALUE,
                                       COBRA_MUTABILITY_DEFAULT, -1);
    module->type_i32 = cobra_type_make(module->type_arena, COBRA_TYPE_I32, NULL,
                                       NULL, NULL, NULL, NULL,
                                       COBRA_OWNERSHIP_VALUE,
                                       COBRA_MUTABILITY_DEFAULT, -1);
    module->type_u32 = cobra_type_make(module->type_arena, COBRA_TYPE_U32, NULL,
                                       NULL, NULL, NULL, NULL,
                                       COBRA_OWNERSHIP_VALUE,
                                       COBRA_MUTABILITY_DEFAULT, -1);
    module->type_u64 = cobra_type_make(module->type_arena, COBRA_TYPE_U64, NULL,
                                       NULL, NULL, NULL, NULL,
                                       COBRA_OWNERSHIP_VALUE,
                                       COBRA_MUTABILITY_DEFAULT, -1);
    module->type_bool = cobra_type_make(module->type_arena, COBRA_TYPE_BOOL, NULL,
                                        NULL, NULL, NULL, NULL,
                                        COBRA_OWNERSHIP_VALUE,
                                        COBRA_MUTABILITY_DEFAULT, -1);
    module->type_f32 = cobra_type_make(module->type_arena, COBRA_TYPE_F32, NULL,
                                       NULL, NULL, NULL, NULL,
                                       COBRA_OWNERSHIP_VALUE,
                                       COBRA_MUTABILITY_DEFAULT, -1);
    module->type_f64 = cobra_type_make(module->type_arena, COBRA_TYPE_F64, NULL,
                                       NULL, NULL, NULL, NULL,
                                       COBRA_OWNERSHIP_VALUE,
                                       COBRA_MUTABILITY_DEFAULT, -1);
    module->type_u8 = cobra_type_make(module->type_arena, COBRA_TYPE_U8, NULL,
                                      NULL, NULL, NULL, NULL,
                                      COBRA_OWNERSHIP_VALUE,
                                      COBRA_MUTABILITY_DEFAULT, -1);
    module->type_void = cobra_type_make(module->type_arena, COBRA_TYPE_VOID, NULL,
                                        NULL, NULL, NULL, NULL,
                                        COBRA_OWNERSHIP_VALUE,
                                        COBRA_MUTABILITY_DEFAULT, -1);
    const CobraType *module_scalars[] = {
        module->type_i64, module->type_i32, module->type_u32, module->type_u64,
        module->type_bool, module->type_f32, module->type_f64, module->type_u8,
        module->type_void
    };
    for (size_t i = 0; i < sizeof(module_scalars) / sizeof(module_scalars[0]); i++) {
        if (module_scalars[i] && !module_scalars[i]->finalized) {
            cobra_type_finalize(module->type_arena,
                                (CobraType *)module_scalars[i]);
        }
    }
}

void bir_module_free(BackendIrModule *module) {
    if (!module) return;
    bir_arena_free(&module->arena);
    free(module->type_arena);
    module->type_arena = NULL;
    memset(module, 0, sizeof(*module));
}
