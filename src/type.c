#include "../include/cobra.h"

#include <stdarg.h>

#define COBRA_TYPE_RECURSION_LIMIT 128
#define COBRA_TYPE_EQUAL_PAIRS 128

typedef struct {
    const CobraType *left;
    const CobraType *right;
} TypePair;

static void type_error(CobraTypeArena *arena, const char *format, ...) {
    if (!arena || arena->error[0] != '\0') return;
    va_list args;
    va_start(args, format);
    vsnprintf(arena->error, sizeof(arena->error), format, args);
    va_end(args);
}

static void reset_type(CobraType *type, CobraTypeKind kind) {
    memset(type, 0, sizeof(*type));
    type->kind = kind;
    type->ownership = COBRA_OWNERSHIP_VALUE;
    type->mutability = COBRA_MUTABILITY_DEFAULT;
    type->region_id = -1;
    type->abi = COBRA_ABI_INVALID;
}

void cobra_type_arena_init(CobraTypeArena *arena) {
    if (!arena) return;
    memset(arena, 0, sizeof(*arena));
}

CobraType *cobra_type_new(CobraTypeArena *arena, CobraTypeKind kind) {
    if (!arena || arena->count >= COBRA_MAX_TYPE_NODES) return NULL;
    CobraType *type = &arena->nodes[arena->count++];
    reset_type(type, kind);
    return type;
}

CobraType *cobra_type_named(CobraTypeArena *arena, CobraTypeKind kind, const char *name) {
    CobraType *type = cobra_type_new(arena, kind);
    if (!type) return NULL;
    if (name) snprintf(type->name, sizeof(type->name), "%.63s", name);
    return type;
}

bool cobra_type_add_generic_arg(CobraType *type, const CobraType *argument) {
    if (!type || type->finalized || !argument || type->generic_arg_count >= COBRA_MAX_TYPE_ARGS) return false;
    type->generic_args[type->generic_arg_count++] = argument;
    return true;
}

const CobraType *cobra_type_element(const CobraType *type) {
    if (!type || type->generic_arg_count == 0) return NULL;
    if (type->kind == COBRA_TYPE_OPTION || type->kind == COBRA_TYPE_RESULT ||
        type->kind == COBRA_TYPE_LIST || type->kind == COBRA_TYPE_ARRAY ||
        type->kind == COBRA_TYPE_SLICE || type->kind == COBRA_TYPE_SLICE_F32 ||
        type->kind == COBRA_TYPE_SLICE_U8 || type->kind == COBRA_TYPE_TENSOR_F32)
        return type->generic_args[0];
    return NULL;
}

const CobraType *cobra_type_error(const CobraType *type) {
    if (!type || type->kind != COBRA_TYPE_RESULT || type->generic_arg_count < 2) return NULL;
    return type->generic_args[1];
}

const CobraType *cobra_type_key(const CobraType *type) {
    if (!type || type->kind != COBRA_TYPE_DICT || type->generic_arg_count < 1) return NULL;
    return type->generic_args[0];
}

const CobraType *cobra_type_value(const CobraType *type) {
    if (!type || type->kind != COBRA_TYPE_DICT || type->generic_arg_count < 2) return NULL;
    return type->generic_args[1];
}

bool cobra_type_add_field(CobraType *type, const char *name, const CobraType *field_type,
                          CobraOwnershipKind ownership, CobraMutabilityKind mutability,
                          int region_id) {
    if (!type || type->finalized || type->kind != COBRA_TYPE_STRUCT || !field_type ||
        type->field_count >= COBRA_MAX_TYPE_FIELDS) return false;
    CobraTypeField *field = &type->fields[type->field_count++];
    memset(field, 0, sizeof(*field));
    if (name) snprintf(field->name, sizeof(field->name), "%.63s", name);
    field->type = field_type;
    field->ownership = ownership;
    field->mutability = mutability;
    field->region_id = region_id;
    return true;
}

const char *cobra_type_kind_name(CobraTypeKind kind) {
    switch (kind) {
        case COBRA_TYPE_UNTYPED: return "untyped";
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
        case COBRA_TYPE_LIST: return "list";
        case COBRA_TYPE_DICT: return "dict";
        case COBRA_TYPE_BOOL: return "bool";
        case COBRA_TYPE_NONE: return "none";
        case COBRA_TYPE_OPTION: return "Option";
        case COBRA_TYPE_RESULT: return "Result";
        case COBRA_TYPE_GENERIC_PARAM: return "generic parameter";
        case COBRA_TYPE_ENUM: return "enum";
        case COBRA_TYPE_STRUCT: return "struct";
        case COBRA_TYPE_UNKNOWN: return "unknown";
        default: return "invalid";
    }
}

CobraTypeKind cobra_type_node_kind(const ASTNode *node) {
    if (!node) return COBRA_TYPE_UNKNOWN;
    if (node->canonical_type) return node->canonical_type->kind;
    return node->declared_type != COBRA_TYPE_UNTYPED ? node->declared_type : node->value_type;
}

const CobraType *cobra_type_node_element(const ASTNode *node) {
    return node ? cobra_type_element(node->canonical_type) : NULL;
}

const CobraType *cobra_type_node_error(const ASTNode *node) {
    return node ? cobra_type_error(node->canonical_type) : NULL;
}

const CobraType *cobra_type_node_key(const ASTNode *node) {
    return node ? cobra_type_key(node->canonical_type) : NULL;
}

const CobraType *cobra_type_node_value(const ASTNode *node) {
    return node ? cobra_type_value(node->canonical_type) : NULL;
}

const char *cobra_type_node_name(const ASTNode *node) {
    if (!node || !node->canonical_type) return "";
    const CobraType *type = node->canonical_type;
    if ((type->kind == COBRA_TYPE_STRUCT || type->kind == COBRA_TYPE_ENUM) && type->name[0])
        return type->name;
    const CobraType *element = cobra_type_element(type);
    if (element && (element->kind == COBRA_TYPE_STRUCT || element->kind == COBRA_TYPE_ENUM))
        return element->name;
    return type->name;
}

const char *cobra_type_node_error_name(const ASTNode *node) {
    const CobraType *error = cobra_type_node_error(node);
    return error && error->kind == COBRA_TYPE_STRUCT ? error->name : "";
}

int cobra_type_abi_slots(const CobraType *type) {
    /* Call-convention slot count is a property of the canonical type identity,
       not the value-storage ABI class. A tensor descriptor occupies 16 storage
       bytes but is passed as a single pointer slot, so this derives from kind. */
    if (!type) return 0;
    switch (type->kind) {
        case COBRA_TYPE_F32:
        case COBRA_TYPE_F64:
            return 0; /* floating-point register class, zero GPR slots */
        case COBRA_TYPE_SLICE:
        case COBRA_TYPE_SLICE_F32:
        case COBRA_TYPE_SLICE_U8:
        case COBRA_TYPE_V256: /* 256-bit vector: passed as a pointer+length view */
            return 2;
        case COBRA_TYPE_LIST:
            return 3; /* pointer, length, capacity */
        case COBRA_TYPE_DICT:
            return 2; /* table pointer, logical length */
        case COBRA_TYPE_OPTION:
        case COBRA_TYPE_RESULT:
            return 1; /* pointer to caller-owned sum storage */
        default:
            return 1; /* scalars, strings, bools, enums, structs, tensors */
    }
}

static bool is_integer_type(CobraTypeKind kind) {
    return kind == COBRA_TYPE_I32 || kind == COBRA_TYPE_I64 ||
           kind == COBRA_TYPE_U8 || kind == COBRA_TYPE_U32 || kind == COBRA_TYPE_U64;
}

static size_t scalar_size(CobraTypeKind kind, CobraAbiKind *abi, size_t *alignment) {
    if (abi) *abi = COBRA_ABI_GPR;
    if (alignment) *alignment = 8;
    if (kind == COBRA_TYPE_VOID) {
        if (abi) *abi = COBRA_ABI_VOID;
        return 0;
    }
    if (kind == COBRA_TYPE_GENERIC_PARAM) {
        if (abi) *abi = COBRA_ABI_INVALID;
        return 0;
    }
    if (kind == COBRA_TYPE_F32) {
        if (abi) *abi = COBRA_ABI_XMM;
        if (alignment) *alignment = 4;
        return 4;
    }
    if (kind == COBRA_TYPE_SLICE || kind == COBRA_TYPE_SLICE_F32 ||
        kind == COBRA_TYPE_SLICE_U8 || kind == COBRA_TYPE_TENSOR_F32) {
        if (abi) *abi = COBRA_ABI_SLICE;
        return 16;
    }
    if (kind == COBRA_TYPE_STRING || kind == COBRA_TYPE_LIST ||
        kind == COBRA_TYPE_DICT) {
        if (abi) *abi = COBRA_ABI_REFERENCE;
        return 8;
    }
    if (is_integer_type(kind) || kind == COBRA_TYPE_BOOL ||
        kind == COBRA_TYPE_F64 || kind == COBRA_TYPE_ENUM) return 8;
    return 0;
}

static bool in_stack(const CobraType *const stack[], size_t depth, const CobraType *type) {
    for (size_t i = 0; i < depth; i++) if (stack[i] == type) return true;
    return false;
}

static bool finalize_type(CobraTypeArena *arena, CobraType *type,
                          const CobraType *const stack[], size_t depth) {
    if (!arena || !type) return false;
    if (type->finalized) return true;
    if (depth >= COBRA_TYPE_RECURSION_LIMIT || in_stack(stack, depth, type)) {
        type_error(arena, "recursive by-value type cycle includes '%s'",
                   type->name[0] ? type->name : cobra_type_kind_name(type->kind));
        return false;
    }

    const CobraType *next_stack[COBRA_TYPE_RECURSION_LIMIT];
    memcpy(next_stack, stack, depth * sizeof(*stack));
    next_stack[depth] = type;

    for (size_t i = 0; i < type->generic_arg_count; i++) {
        if (!finalize_type(arena, (CobraType *)type->generic_args[i], next_stack, depth + 1)) return false;
    }
    for (size_t i = 0; i < type->field_count; i++) {
        if (!finalize_type(arena, (CobraType *)type->fields[i].type, next_stack, depth + 1)) return false;
    }

    CobraAbiKind abi = COBRA_ABI_INVALID;
    size_t alignment = 8;
    size_t size = scalar_size(type->kind, &abi, &alignment);
    if (type->kind == COBRA_TYPE_OPTION || type->kind == COBRA_TYPE_RESULT) {
        size_t required = type->kind == COBRA_TYPE_RESULT ? 2 : 1;
        if (type->generic_arg_count != required) {
            type_error(arena, "%s requires %zu generic argument%s",
                       cobra_type_kind_name(type->kind), required, required == 1 ? "" : "s");
            return false;
        }
        size = COBRA_NATIVE_SUM_TAG_SIZE;
        for (size_t i = 0; i < required; i++) {
            const CobraType *argument = type->generic_args[i];
            size += argument->kind == COBRA_TYPE_STRUCT ? argument->size : COBRA_NATIVE_SUM_SCALAR_SIZE;
        }
        abi = COBRA_ABI_SUM_INDIRECT;
        alignment = 8;
    } else if (type->kind == COBRA_TYPE_STRUCT) {
        if (type->field_count == 0) {
            type_error(arena, "struct '%s' has no fields", type->name[0] ? type->name : "<unnamed>");
            return false;
        }
        size_t offset = 0;
        for (size_t i = 0; i < type->field_count; i++) {
            CobraTypeField *field = &type->fields[i];
            field->offset = offset;
            offset += field->type->size;
        }
        abi = COBRA_ABI_STRUCT_VALUE;
        size = (offset + 7U) & ~7U;
        alignment = 8;
    } else if (type->kind == COBRA_TYPE_LIST || type->kind == COBRA_TYPE_DICT) {
        if (type->generic_arg_count == 0 ||
            (type->kind == COBRA_TYPE_DICT && type->generic_arg_count != 2)) {
            type_error(arena, "%s has incomplete generic arguments", cobra_type_kind_name(type->kind));
            return false;
        }
        abi = COBRA_ABI_REFERENCE;
        size = 8;
        alignment = 8;
    } else if (size == 0 && type->kind != COBRA_TYPE_UNTYPED) {
        if (type->kind == COBRA_TYPE_GENERIC_PARAM)
            type_error(arena, "generic parameter '%s' must be instantiated before ABI lowering",
                       type->name[0] ? type->name : "<unnamed>");
        else
            type_error(arena, "type '%s' has no ABI representation", cobra_type_kind_name(type->kind));
        return false;
    }

    type->abi = abi;
    type->size = size;
    type->alignment = alignment;
    type->finalized = true;
    return true;
}

bool cobra_type_finalize(CobraTypeArena *arena, CobraType *type) {
    if (!arena || !type) return false;
    arena->error[0] = '\0';
    const CobraType *stack[COBRA_TYPE_RECURSION_LIMIT] = {0};
    return finalize_type(arena, type, stack, 0);
}

static bool equal_type(const CobraType *left, const CobraType *right,
                       TypePair pairs[], size_t *pair_count) {
    if (left == right) return true;
    if (!left || !right || left->kind != right->kind ||
        left->ownership != right->ownership || left->mutability != right->mutability ||
        left->region_id != right->region_id ||
        left->generic_arg_count != right->generic_arg_count ||
        left->field_count != right->field_count) return false;

    for (size_t i = 0; i < *pair_count; i++) {
        if (pairs[i].left == left && pairs[i].right == right) return true;
    }
    if (*pair_count >= COBRA_TYPE_EQUAL_PAIRS) return false;
    pairs[(*pair_count)++] = (TypePair){left, right};

    if (left->template_origin && right->template_origin) {
        if (!equal_type(left->template_origin, right->template_origin, pairs, pair_count)) return false;
    } else if (!left->template_origin && !right->template_origin) {
        if (strcmp(left->name, right->name) != 0) return false;
    } else if (left->kind == COBRA_TYPE_STRUCT || right->kind == COBRA_TYPE_STRUCT) {
        /* Struct specialization identity is provenance-sensitive. A generated
           name must never make a struct equal to an unrelated declaration. */
        return false;
    }

    for (size_t i = 0; i < left->generic_arg_count; i++) {
        if (!equal_type(left->generic_args[i], right->generic_args[i], pairs, pair_count)) return false;
    }
    for (size_t i = 0; i < left->field_count; i++) {
        const CobraTypeField *a = &left->fields[i];
        const CobraTypeField *b = &right->fields[i];
        if (strcmp(a->name, b->name) != 0 || a->ownership != b->ownership ||
            a->mutability != b->mutability || a->region_id != b->region_id ||
            !equal_type(a->type, b->type, pairs, pair_count)) return false;
    }
    return true;
}

bool cobra_type_equal(const CobraType *left, const CobraType *right) {
    TypePair pairs[COBRA_TYPE_EQUAL_PAIRS] = {{0}};
    size_t pair_count = 0;
    return equal_type(left, right, pairs, &pair_count);
}

static bool is_identity_component(const CobraType *type) {
    /* A pure type-identity node never carries a declaration's ownership,
       mutability, or region origin. Value-level nodes (for example out or
       readonly parameters) are excluded from interning by this check, so two
       lifetime-different types can never collapse into one interned node. */
    return type->ownership == COBRA_OWNERSHIP_VALUE &&
           type->mutability == COBRA_MUTABILITY_DEFAULT &&
           type->region_id == -1;
}

static CobraType *canonical_component_node(CobraTypeArena *arena, CobraTypeKind kind,
                                           const char *name) {
    /* Intern type-identity components only. Ownership, mutability, region
       origin, generic arguments, struct fields, and ABI class all live on the
       top-level declaration node, never on these shared component nodes, so a
       structurally identical node is safe to reuse. Top-level declarations use
       fresh nodes and are never interned here. */
    bool named = name && name[0];
    for (size_t i = 0; i < arena->count; i++) {
        CobraType *type = &arena->nodes[i];
        if (type->kind != kind || !is_identity_component(type)) continue;
        if (named) {
            if (type->generic_arg_count == 0 && strcmp(type->name, name) == 0) return type;
        } else if (type->name[0] == '\0' && type->field_count == 0 &&
                   type->generic_arg_count == 0) {
            return type;
        }
    }
    return cobra_type_named(arena, kind, named ? name : NULL);
}

CobraType *cobra_type_make(CobraTypeArena *arena, CobraTypeKind kind, const char *name,
                           const CobraType *element, const CobraType *error,
                           const CobraType *key, const CobraType *value,
                           CobraOwnershipKind ownership, CobraMutabilityKind mutability,
                           int region_id) {
    if (!arena || kind == COBRA_TYPE_UNTYPED || kind == COBRA_TYPE_UNKNOWN) return NULL;
    CobraType *type = NULL;
    bool identity = ownership == COBRA_OWNERSHIP_VALUE &&
                    mutability == COBRA_MUTABILITY_DEFAULT && region_id == -1;
    if (identity && (kind == COBRA_TYPE_STRUCT || kind == COBRA_TYPE_ENUM ||
                     (element == NULL && error == NULL && key == NULL && value == NULL))) {
        type = canonical_component_node(arena, kind, name && name[0] ? name : NULL);
    } else {
        type = cobra_type_named(arena, kind, name && name[0] ? name : NULL);
    }
    if (!type) return NULL;
    if (type->finalized) {
        bool same_contract = type->ownership == ownership &&
                             type->mutability == mutability &&
                             type->region_id == region_id &&
                             !element && !error && !key && !value;
        return same_contract ? type : NULL;
    }
    type->ownership = ownership;
    type->mutability = mutability;
    type->region_id = region_id;
    if (element && !cobra_type_add_generic_arg(type, element)) return NULL;
    if (error && !cobra_type_add_generic_arg(type, error)) return NULL;
    if (key && !cobra_type_add_generic_arg(type, key)) return NULL;
    if (value && !cobra_type_add_generic_arg(type, value)) return NULL;
    return type;
}

static bool scalar_generic_argument(const CobraType *type) {
    if (!type) return false;
    switch (type->kind) {
        case COBRA_TYPE_I32:
        case COBRA_TYPE_I64:
        case COBRA_TYPE_U8:
        case COBRA_TYPE_U32:
        case COBRA_TYPE_U64:
        case COBRA_TYPE_F32:
        case COBRA_TYPE_BOOL:
        case COBRA_TYPE_ENUM:
            return true;
        default:
            return false;
    }
}

bool cobra_type_is_scalar(const CobraType *type) {
    return scalar_generic_argument(type);
}

bool cobra_type_is_slice_kind(CobraTypeKind kind) {
    return kind == COBRA_TYPE_SLICE || kind == COBRA_TYPE_SLICE_F32 ||
           kind == COBRA_TYPE_SLICE_U8;
}

bool cobra_type_is_borrowed_view(const CobraType *type) {
    if (!type || type->kind != COBRA_TYPE_STRUCT || !type->finalized ||
        type->generic_arg_count != 1 || type->field_count != 1) return false;
    const CobraTypeField *field = &type->fields[0];
    return field->ownership == COBRA_OWNERSHIP_BORROWED &&
           field->mutability == COBRA_MUTABILITY_READONLY &&
           field->region_id == -1 && field->type &&
           cobra_type_is_slice_kind(field->type->kind) &&
           field->type->generic_arg_count == 1 &&
           scalar_generic_argument(field->type->generic_args[0]) &&
           scalar_generic_argument(type->generic_args[0]) &&
           field->type->generic_args[0]->kind == type->generic_args[0]->kind;
}

static bool generic_template_identity(const CobraType *pattern,
                                      const CobraType *origin) {
    if (!pattern || !origin || pattern->kind != COBRA_TYPE_STRUCT ||
        origin->kind != COBRA_TYPE_STRUCT || strcmp(pattern->name, origin->name) != 0 ||
        pattern->generic_arg_count != origin->generic_arg_count ||
        pattern->ownership != origin->ownership ||
        pattern->mutability != origin->mutability || pattern->region_id != origin->region_id)
        return false;
    for (size_t i = 0; i < pattern->generic_arg_count; i++) {
        const CobraType *left = pattern->generic_args[i];
        const CobraType *right = origin->generic_args[i];
        if (left->kind == COBRA_TYPE_GENERIC_PARAM && right->kind == COBRA_TYPE_GENERIC_PARAM) {
            if (strcmp(left->name, right->name) != 0) return false;
        } else if (!cobra_type_equal(left, right)) {
            return false;
        }
    }
    return true;
}

bool cobra_type_bind_generic(const CobraType *pattern, const CobraType *actual,
                             const CobraType *parameter, const CobraType **binding) {
    if (!pattern || !actual || !parameter || !binding) return false;
    if (pattern == parameter) {
        if (!scalar_generic_argument(actual)) return false;
        if (!*binding) {
            *binding = actual;
            return true;
        }
        return cobra_type_equal(*binding, actual);
    }
    if (cobra_type_is_slice_kind(pattern->kind) && cobra_type_is_slice_kind(actual->kind)) {
        if (pattern->generic_arg_count != actual->generic_arg_count) return false;
    } else if (pattern->kind == COBRA_TYPE_STRUCT && actual->kind == COBRA_TYPE_STRUCT) {
        /* A concrete specialization carries its originating canonical template.
           Never infer identity from the generated View__i64 spelling: a spoofed
           named struct has no template_origin and cannot bind here. */
        if (!generic_template_identity(pattern, actual->template_origin) ||
            !cobra_type_is_borrowed_view(actual) ||
            pattern->generic_arg_count != actual->generic_arg_count) return false;
    } else if (pattern->kind != actual->kind ||
               pattern->generic_arg_count != actual->generic_arg_count) {
        return false;
    }
    for (size_t i = 0; i < pattern->generic_arg_count; i++) {
        if (!cobra_type_bind_generic(pattern->generic_args[i], actual->generic_args[i],
                                     parameter, binding)) return false;
    }
    return true;
}

static bool slice_type_kind(CobraTypeKind kind) {
    return cobra_type_is_slice_kind(kind);
}

static CobraTypeKind slice_kind_for_element(CobraTypeKind element) {
    if (element == COBRA_TYPE_F32) return COBRA_TYPE_SLICE_F32;
    if (element == COBRA_TYPE_U8) return COBRA_TYPE_SLICE_U8;
    if (element == COBRA_TYPE_I64) return COBRA_TYPE_SLICE;
    return COBRA_TYPE_UNKNOWN;
}

static const CobraType *binding_value(const CobraType *type,
                                      const CobraTypeBinding *bindings,
                                      size_t binding_count) {
    if (!type || !bindings) return NULL;
    for (size_t i = 0; i < binding_count; i++) {
        if (bindings[i].parameter == type) return bindings[i].argument;
    }
    return NULL;
}

static bool type_has_binding(const CobraType *type,
                             const CobraTypeBinding *bindings,
                             size_t binding_count) {
    if (!type) return false;
    if (binding_value(type, bindings, binding_count)) return true;
    for (size_t i = 0; i < type->generic_arg_count; i++) {
        if (type_has_binding(type->generic_args[i], bindings, binding_count)) return true;
    }
    for (size_t i = 0; i < type->field_count; i++) {
        if (type_has_binding(type->fields[i].type, bindings, binding_count)) return true;
    }
    return false;
}

static bool immutable_nested_struct(const CobraType *type) {
    if (!type || type->kind != COBRA_TYPE_STRUCT || !type->finalized) return false;
    for (size_t i = 0; i < type->field_count; i++) {
        const CobraTypeField *field = &type->fields[i];
        if (field->ownership != COBRA_OWNERSHIP_VALUE ||
            field->mutability != COBRA_MUTABILITY_DEFAULT || field->region_id != -1)
            return false;
        if (!scalar_generic_argument(field->type) && !immutable_nested_struct(field->type))
            return false;
    }
    return true;
}

static bool substituted_field_allowed(const CobraTypeField *source,
                                      const CobraType *field_type,
                                      const CobraTypeBinding *bindings,
                                      size_t binding_count) {
    if (!source || !field_type) return false;
    if (source->ownership == COBRA_OWNERSHIP_VALUE &&
        source->mutability == COBRA_MUTABILITY_DEFAULT && source->region_id == -1) {
        return scalar_generic_argument(field_type) || immutable_nested_struct(field_type);
    }
    return source->ownership == COBRA_OWNERSHIP_BORROWED &&
           source->mutability == COBRA_MUTABILITY_READONLY && source->region_id == -1 &&
           /* A borrowed field is part of this generic lane only when its
              element contract actually carries the substituted parameter. */
           type_has_binding(source->type, bindings, binding_count) &&
           slice_type_kind(field_type->kind) && field_type->generic_arg_count == 1 &&
           scalar_generic_argument(field_type->generic_args[0]);
}

static void derived_specialization_name(const CobraType *type,
                                         const CobraType *argument,
                                         char *out, size_t capacity) {
    if (!out || capacity == 0) return;
    if (!type || type->kind != COBRA_TYPE_STRUCT || !type->name[0] || !argument) {
        if (out && capacity) out[0] = '\0';
        return;
    }
    snprintf(out, capacity, "%.46s__%.15s", type->name, cobra_type_kind_name(argument->kind));
}

static CobraType *intern_finalized_type(CobraTypeArena *arena, CobraType *candidate);

static CobraType *substitute_recursive(CobraTypeArena *arena,
                                       const CobraType *type,
                                       const CobraTypeBinding *bindings,
                                       size_t binding_count,
                                       const char *name_override,
                                       bool top_level) {
    if (!arena || !type) return NULL;
    const CobraType *bound = binding_value(type, bindings, binding_count);
    if (bound) return (CobraType *)bound;
    if (type->kind == COBRA_TYPE_GENERIC_PARAM) {
        type_error(arena, "generic parameter '%s' remains unresolved", type->name);
        return NULL;
    }
    if (!type_has_binding(type, bindings, binding_count)) return (CobraType *)type;

    const CobraType *arguments[COBRA_MAX_TYPE_ARGS] = {0};
    for (size_t i = 0; i < type->generic_arg_count; i++) {
        arguments[i] = substitute_recursive(arena, type->generic_args[i], bindings,
                                            binding_count, NULL, false);
        if (!arguments[i]) return NULL;
    }

    CobraTypeKind kind = type->kind;
    if (slice_type_kind(kind)) {
        if (type->ownership != COBRA_OWNERSHIP_BORROWED ||
            type->mutability != COBRA_MUTABILITY_READONLY ||
            type->generic_arg_count != 1 ||
            !scalar_generic_argument(arguments[0])) {
            type_error(arena, "generic slices must remain borrowed readonly scalar views");
            return NULL;
        }
        CobraTypeKind inferred = slice_kind_for_element(arguments[0]->kind);
        if (inferred == COBRA_TYPE_UNKNOWN ||
            (kind != COBRA_TYPE_SLICE && kind != inferred)) {
            type_error(arena, "generic slice element does not match its ABI kind");
            return NULL;
        }
        kind = inferred;
    }

    char nested_name[COBRA_MAX_IDENT_LEN] = "";
    const char *name = type->name[0] ? type->name : NULL;
    if (type->kind == COBRA_TYPE_STRUCT) {
        if (top_level && name_override && name_override[0]) name = name_override;
        else if (!top_level) {
            derived_specialization_name(type, arguments[0], nested_name, sizeof(nested_name));
            if (nested_name[0]) name = nested_name;
        }
    }
    CobraType *candidate = cobra_type_named(arena, kind, name);
    if (!candidate) {
        type_error(arena, "could not allocate canonical generic specialization");
        return NULL;
    }
    candidate->template_origin = type->template_origin ? type->template_origin : type;
    candidate->ownership = type->ownership;
    candidate->mutability = type->mutability;
    candidate->region_id = type->region_id;
    for (size_t i = 0; i < type->generic_arg_count; i++) {
        if (!cobra_type_add_generic_arg(candidate, arguments[i])) {
            type_error(arena, "generic specialization has too many type arguments");
            return NULL;
        }
    }
    for (size_t i = 0; i < type->field_count; i++) {
        const CobraTypeField *source = &type->fields[i];
        const CobraType *field_type = substitute_recursive(arena, source->type, bindings,
                                                            binding_count, NULL, false);
        if (!field_type || !substituted_field_allowed(source, field_type,
                                                        bindings, binding_count)) {
            type_error(arena, "generic field '%s' has an unsupported ownership or ABI contract",
                       source->name);
            return NULL;
        }
        if (!cobra_type_add_field(candidate, source->name, field_type,
                                  source->ownership, source->mutability,
                                  source->region_id)) return NULL;
    }
    if (!cobra_type_validate(arena, candidate)) return NULL;
    return intern_finalized_type(arena, candidate);
}

CobraType *cobra_type_substitute(CobraTypeArena *arena,
                                 const CobraType *template_type,
                                 const CobraTypeBinding *bindings,
                                 size_t binding_count,
                                 const char *specialized_name) {
    if (!arena || !template_type || !bindings || binding_count == 0) return NULL;
    for (size_t i = 0; i < binding_count; i++) {
        if (!bindings[i].parameter || bindings[i].parameter->kind != COBRA_TYPE_GENERIC_PARAM ||
            !bindings[i].argument || !scalar_generic_argument(bindings[i].argument)) {
            type_error(arena, "generic substitution requires scalar arguments and placeholders");
            return NULL;
        }
    }
    arena->error[0] = '\0';
    return substitute_recursive(arena, template_type, bindings, binding_count,
                                specialized_name, true);
}

static CobraType *intern_finalized_type(CobraTypeArena *arena, CobraType *candidate) {
    if (!arena || !candidate) return NULL;
    for (size_t i = 0; i < arena->count; i++) {
        CobraType *existing = &arena->nodes[i];
        if (existing == candidate || !existing->finalized) continue;
        if (cobra_type_equal(existing, candidate)) return existing;
    }
    return candidate;
}

CobraType *cobra_type_instantiate(CobraTypeArena *arena,
                                  const CobraType *template_type,
                                  const CobraType *parameter,
                                  const CobraType *argument) {
    if (!arena || !template_type || !parameter || !argument) return NULL;
    CobraTypeBinding binding = {parameter, argument};
    return cobra_type_substitute(arena, template_type, &binding, 1, NULL);
}

CobraType *cobra_type_instantiate_struct(CobraTypeArena *arena,
                                         const CobraType *template_type,
                                         const CobraType *parameter,
                                         const CobraType *argument,
                                         const char *specialized_name) {
    if (!arena || !template_type || !parameter || !argument ||
        !specialized_name || !specialized_name[0]) return NULL;
    CobraTypeBinding binding = {parameter, argument};
    return cobra_type_substitute(arena, template_type, &binding, 1, specialized_name);
}

bool cobra_type_validate(CobraTypeArena *arena, const CobraType *type) {
    if (!arena || !type) return false;
    for (size_t i = 0; i < type->field_count; i++) {
        if (!type->fields[i].type || !type->fields[i].name[0]) {
            type_error(arena, "struct field %zu is incomplete", i);
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (strcmp(type->fields[i].name, type->fields[j].name) == 0) {
                type_error(arena, "duplicate field '%s'", type->fields[i].name);
                return false;
            }
        }
    }
    return cobra_type_finalize(arena, (CobraType *)type);
}

/* --- Canonical struct layout (declaration-driven) -------------------------
   The canonical descriptor is the single layout source: finalize computes the
   packed offsets, sizes, ownership, mutability, and ABI class, and codegen,
   IR, and the type regression all read it. */

static bool is_enum_name(ASTNode *root, const char *name) {
    if (!root || !name || !name[0]) return false;
    for (size_t i = 0; i < root->child_count; i++) {
        ASTNode *node = root->children[i];
        if (node->type == AST_ENUM_DECL && strcmp(node->name, name) == 0) return true;
    }
    return false;
}

static CobraType *canonical_scalar(CobraTypeArena *arena, CobraTypeKind kind) {
    for (size_t i = 0; i < arena->count; i++) {
        CobraType *type = &arena->nodes[i];
        if (type->kind == kind && type->field_count == 0 &&
            type->generic_arg_count == 0 && type->name[0] == '\0' &&
            is_identity_component(type)) return type;
    }
    return cobra_type_named(arena, kind, NULL);
}

static const CobraType *cobra_type_struct_layout_depth(CobraTypeArena *arena, ASTNode *root,
                                                       const char *name, int depth);

static const CobraType *canonical_node(CobraTypeArena *arena, ASTNode *root,
                                       CobraTypeKind kind, const char *name,
                                       int depth) {
    if (kind == COBRA_TYPE_UNTYPED || kind == COBRA_TYPE_UNKNOWN) return NULL;
    if (kind == COBRA_TYPE_STRUCT && is_enum_name(root, name)) kind = COBRA_TYPE_ENUM;
    if (kind == COBRA_TYPE_STRUCT) {
        const CobraType *st = cobra_type_struct_layout_depth(arena, root, name, depth);
        if (!st) return NULL;
        /* A by-value struct used nested must not carry owned string or
           borrowed slice fields. This is checked at the reference site so it
           holds regardless of build order, matching the legacy layout rule. */
        if (depth > 0) {
            for (size_t i = 0; i < st->field_count; i++) {
                CobraTypeKind field_kind = st->fields[i].type->kind;
                if (field_kind == COBRA_TYPE_STRING) {
                    type_error(arena,
                               "nested owned string field '%s' requires an ownership layout",
                               st->fields[i].name);
                    return NULL;
                }
                if (field_kind == COBRA_TYPE_SLICE ||
                    field_kind == COBRA_TYPE_SLICE_F32 ||
                    field_kind == COBRA_TYPE_SLICE_U8) {
                    type_error(arena,
                               "nested borrowed slice field '%s' requires an ownership layout",
                               st->fields[i].name);
                    return NULL;
                }
            }
        }
        return st;
    }
    if (kind == COBRA_TYPE_ENUM) {
        for (size_t i = 0; i < arena->count; i++) {
            CobraType *type = &arena->nodes[i];
            if (type->kind == COBRA_TYPE_ENUM && name &&
                strcmp(type->name, name) == 0) return type;
        }
        return cobra_type_named(arena, COBRA_TYPE_ENUM, name);
    }
    return canonical_scalar(arena, kind);
}

/* The interning key is kind + name + identity-component. Module identity and
   field shape are deliberately NOT part of the key today because duplicate
   struct declarations are rejected at IR registration, so a name can only
   map to one declaration per program. This guard turns a future collision
   (for example once modules or generics introduce qualified names) into a
   loud internal error instead of a silent layout merge. */
static bool struct_shape_matches(ASTNode *decl, const CobraType *type) {
    size_t declared = 0;
    for (size_t i = 0; i < decl->child_count; i++)
        if (decl->children[i]->type == AST_PARAM) declared++;
    if (declared != type->field_count) return false;
    size_t index = 0;
    for (size_t i = 0; i < decl->child_count; i++) {
        ASTNode *field = decl->children[i];
        if (field->type != AST_PARAM) continue;
        if (strcmp(field->name, type->fields[index].name) != 0) return false;
        index++;
    }
    return true;
}

static bool type_contains_generic_param(const CobraType *type) {
    if (!type) return false;
    if (type->kind == COBRA_TYPE_GENERIC_PARAM) return true;
    for (size_t i = 0; i < type->generic_arg_count; i++) {
        if (type_contains_generic_param(type->generic_args[i])) return true;
    }
    for (size_t i = 0; i < type->field_count; i++) {
        if (type_contains_generic_param(type->fields[i].type)) return true;
    }
    return false;
}

static const CobraType *cobra_type_struct_layout_depth(CobraTypeArena *arena, ASTNode *root,
                                                       const char *name, int depth) {
    if (!arena || !root || !name || !name[0]) return NULL;
    ASTNode *decl = NULL;
    for (size_t i = 0; i < root->child_count; i++) {
        ASTNode *node = root->children[i];
        if (node->type == AST_STRUCT_DECL && strcmp(node->name, name) == 0) { decl = node; break; }
    }
    if (!decl) return NULL;

    CobraType *type = NULL;
    if (decl->canonical_type && decl->canonical_type->kind == COBRA_TYPE_STRUCT &&
        strcmp(decl->canonical_type->name, name) == 0) {
        type = (CobraType *)decl->canonical_type;
    } else {
        type = canonical_component_node(arena, COBRA_TYPE_STRUCT, name);
    }
    if (!type) return NULL;
    if (type->finalized) {
        if (!struct_shape_matches(decl, type)) {
            type_error(arena,
                       "struct '%s' interning collision: field shape does not match the declaration",
                       name);
            return NULL;
        }
        return type;
    }
    if (type->populating) {
        type_error(arena, "recursive by-value struct cycle includes '%s'", name);
        return NULL;
    }

    if (type->field_count == 0) {
        type->populating = true;
        bool ok = true;
        for (size_t i = 0; i < decl->child_count; i++) {
            ASTNode *field = decl->children[i];
            if (field->type != AST_PARAM) continue;
            const CobraType *declared = field->canonical_type;
            if (!declared) {
                type_error(arena, "struct field '%s' is missing canonical type metadata", field->name);
                ok = false;
                break;
            }
            CobraTypeKind field_kind = declared->kind;
            const char *field_type_name = declared->name;
            if (field_kind == COBRA_TYPE_STRUCT && is_enum_name(root, field_type_name))
                field_kind = COBRA_TYPE_ENUM;
            /* Per-field ownership-layout validation, carried over from the
               removed legacy module. The canonical descriptor is now the only
               source for the field kind, named identity, and qualifier. */
            bool generic_borrowed_slice = decl->generic_param_count == 1 &&
                declared->kind == COBRA_TYPE_SLICE &&
                declared->generic_arg_count == 1 &&
                declared->generic_args[0] == decl->generic_param_types[0] &&
                declared->ownership == COBRA_OWNERSHIP_BORROWED &&
                declared->mutability == COBRA_MUTABILITY_READONLY &&
                declared->region_id == -1;
            if ((field_kind == COBRA_TYPE_SLICE || field_kind == COBRA_TYPE_SLICE_F32) &&
                !generic_borrowed_slice) {
                type_error(arena,
                           "struct slice field '%s' is not supported; use a qualified generic readonly view",
                           field->name);
                ok = false;
                break;
            }
            if (field_kind == COBRA_TYPE_SLICE_U8 &&
                declared->mutability != COBRA_MUTABILITY_READONLY &&
                declared->mutability != COBRA_MUTABILITY_OUT) {
                type_error(arena, "byte-view field '%s' requires readonly or out qualifier",
                           field->name);
                ok = false;
                break;
            }
            const CobraType *field_type = (field_kind == COBRA_TYPE_STRUCT ||
                                           field_kind == COBRA_TYPE_ENUM) ?
                canonical_node(arena, root, field_kind, field_type_name, depth + 1) : declared;
            if (!field_type) {
                type_error(arena, "struct field '%s' has unsupported type", field->name);
                ok = false;
                break;
            }
            CobraOwnershipKind ownership = declared->ownership;
            CobraMutabilityKind mutability = declared->mutability;
            if (field_kind == COBRA_TYPE_STRING && ownership == COBRA_OWNERSHIP_VALUE)
                ownership = COBRA_OWNERSHIP_OWNED;
            if (!cobra_type_add_field(type, field->name, field_type, ownership,
                                      mutability, declared->region_id)) {
                type_error(arena, "struct '%s' has too many fields", name);
                ok = false;
                break;
            }
        }
        type->populating = false;
        if (!ok) return NULL;
    }
    /* Generic templates retain their placeholder fields until a call site
       supplies a scalar argument. They are definitions, not ABI-ready values. */
    if (type_contains_generic_param(type)) return type;
    if (!cobra_type_finalize(arena, type)) return NULL;
    return type;
}

const CobraType *cobra_type_struct_layout(CobraTypeArena *arena, ASTNode *root,
                                          const char *name) {
    return cobra_type_struct_layout_depth(arena, root, name, 0);
}

/* Canonical field offset for (struct, field), derived from the finalize layout
   so codegen never reads per-node offsets written by IR. Returns -1 when the
   struct or field is not found. */
int cobra_type_field_offset(CobraTypeArena *arena, ASTNode *root,
                            const char *struct_name, const char *field_name) {
    if (!arena || !root || !struct_name || !struct_name[0] ||
        !field_name || !field_name[0]) return -1;
    const CobraType *st = cobra_type_struct_layout(arena, root, struct_name);
    if (!st) return -1;
    for (size_t i = 0; i < st->field_count; i++) {
        if (strcmp(st->fields[i].name, field_name) == 0)
            return (int)st->fields[i].offset;
    }
    return -1;
}
