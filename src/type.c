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
        left->region_id != right->region_id || strcmp(left->name, right->name) != 0 ||
        left->generic_arg_count != right->generic_arg_count ||
        left->field_count != right->field_count) return false;

    for (size_t i = 0; i < *pair_count; i++) {
        if (pairs[i].left == left && pairs[i].right == right) return true;
    }
    if (*pair_count >= COBRA_TYPE_EQUAL_PAIRS) return false;
    pairs[(*pair_count)++] = (TypePair){left, right};

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

static bool type_contains_parameter(const CobraType *type, const CobraType *parameter) {
    if (!type || !parameter) return false;
    if (type == parameter) return true;
    for (size_t i = 0; i < type->generic_arg_count; i++) {
        if (type_contains_parameter(type->generic_args[i], parameter)) return true;
    }
    return false;
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
    if (parameter->kind != COBRA_TYPE_GENERIC_PARAM) {
        type_error(arena, "generic substitution requires a generic parameter placeholder");
        return NULL;
    }
    if (!scalar_generic_argument(argument)) {
        type_error(arena, "this generic slice only accepts scalar arguments");
        return NULL;
    }
    if (template_type->kind != COBRA_TYPE_OPTION &&
        template_type->kind != COBRA_TYPE_RESULT) {
        type_error(arena, "this generic slice only instantiates Option or Result");
        return NULL;
    }
    size_t required = template_type->kind == COBRA_TYPE_RESULT ? 2 : 1;
    if (template_type->generic_arg_count != required ||
        !type_contains_parameter(template_type, parameter)) {
        type_error(arena, "generic parameter is not present in the Option or Result template");
        return NULL;
    }

    const CobraType *components[2] = {0};
    for (size_t i = 0; i < required; i++) {
        const CobraType *component = template_type->generic_args[i];
        components[i] = component == parameter ? argument : component;
        if (components[i]->kind == COBRA_TYPE_GENERIC_PARAM) {
            type_error(arena, "generic instantiation leaves an unresolved parameter");
            return NULL;
        }
    }
    CobraType *candidate = cobra_type_make(
        arena, template_type->kind, template_type->name,
        components[0], required == 2 ? components[1] : NULL,
        NULL, NULL,
        template_type->ownership, template_type->mutability,
        template_type->region_id);
    if (!candidate) {
        type_error(arena, "could not construct instantiated generic descriptor");
        return NULL;
    }
    if (!cobra_type_validate(arena, candidate)) return NULL;
    return intern_finalized_type(arena, candidate);
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

static const CobraType *cobra_type_struct_layout_depth(CobraTypeArena *arena, ASTNode *root,
                                                       const char *name, int depth) {
    if (!arena || !root || !name || !name[0]) return NULL;
    ASTNode *decl = NULL;
    for (size_t i = 0; i < root->child_count; i++) {
        ASTNode *node = root->children[i];
        if (node->type == AST_STRUCT_DECL && strcmp(node->name, name) == 0) { decl = node; break; }
    }
    if (!decl) return NULL;

    CobraType *type = canonical_component_node(arena, COBRA_TYPE_STRUCT, name);
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
            if (field_kind == COBRA_TYPE_SLICE || field_kind == COBRA_TYPE_SLICE_F32) {
                type_error(arena,
                           "struct slice field '%s' is not supported; use a qualified []u8 view",
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
