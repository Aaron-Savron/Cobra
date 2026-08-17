#include "../include/cobra.h"
#include <errno.h>
#include <limits.h>

static void advance_token(Parser *parser) {
    parser->current_token = lexer_next_token(&parser->lexer);
}

static ASTNode *parser_create_node_at(Parser *parser, ASTNodeType type,
                                      const char *name, Token location) {
    ASTNode *node = ast_create_node(type, name);
    node->source_line = location.line;
    node->source_col = location.col;
    snprintf(node->source_file, sizeof(node->source_file), "%s",
             parser->source_file[0] ? parser->source_file : "<source>");
    return node;
}

static ASTNode *parser_create_node(Parser *parser, ASTNodeType type, const char *name) {
    return parser_create_node_at(parser, type, name, parser->current_token);
}

static bool match(Parser *parser, TokenType type) {
    return parser->current_token.type == type;
}

static void copy_token_text(Parser *parser, char *destination, size_t capacity, const char *context) {
    size_t length = strlen(parser->current_token.text);
    if (length >= capacity) {
        fprintf(stderr, "%s:%d:%d: error: %s exceeds %zu characters\n",
                parser->source_file, parser->current_token.line, parser->current_token.col, context, capacity - 1);
        exit(1);
    }
    memcpy(destination, parser->current_token.text, length + 1);
}

static CobraTypeKind token_to_type(TokenType type) {
    switch (type) {
        case TOKEN_TYPE_I32: return COBRA_TYPE_I32;
        case TOKEN_TYPE_I64: return COBRA_TYPE_I64;
        case TOKEN_TYPE_U8: return COBRA_TYPE_U8;
        case TOKEN_TYPE_U32: return COBRA_TYPE_U32;
        case TOKEN_TYPE_U64: return COBRA_TYPE_U64;
        case TOKEN_TYPE_F32: return COBRA_TYPE_F32;
        case TOKEN_TYPE_F64: return COBRA_TYPE_F64;
        case TOKEN_TYPE_V256: return COBRA_TYPE_V256;
        case TOKEN_TYPE_VOID: return COBRA_TYPE_VOID;
        case TOKEN_TYPE_STRING: return COBRA_TYPE_STRING;
        case TOKEN_TYPE_BOOL: return COBRA_TYPE_BOOL;
        default: return COBRA_TYPE_UNKNOWN;
    }
}

static bool expect(Parser *parser, TokenType type, const char *err_msg);

static const CobraType *parser_generic_param(Parser *parser, const char *name) {
    if (!parser || !name || !name[0]) return NULL;
    for (size_t i = 0; i < parser->generic_param_count; i++) {
        if (strcmp(parser->generic_param_names[i], name) == 0)
            return parser->generic_param_types[i];
    }
    return NULL;
}

static const CobraType *parser_component_type(Parser *parser, CobraTypeKind kind,
                                                const char *name) {
    if (!parser || !parser->canonical_arena || kind == COBRA_TYPE_UNTYPED ||
        kind == COBRA_TYPE_UNKNOWN) return NULL;
    if (kind == COBRA_TYPE_STRUCT && name && name[0]) {
        const CobraType *generic = parser_generic_param(parser, name);
        if (generic) return generic;
    }
    return cobra_type_make(parser->canonical_arena, kind,
                           name && name[0] ? name : NULL,
                           NULL, NULL, NULL, NULL,
                           COBRA_OWNERSHIP_VALUE,
                           COBRA_MUTABILITY_DEFAULT, -1);
}

static void parser_set_canonical(Parser *parser, ASTNode *owner, CobraTypeKind kind,
                                 int qualifier, const CobraType *element,
                                 const CobraType *error, const CobraType *key,
                                 const CobraType *value) {
    if (!parser || !owner || !parser->canonical_arena ||
        kind == COBRA_TYPE_UNTYPED || kind == COBRA_TYPE_UNKNOWN) return;
    CobraOwnershipKind ownership = COBRA_OWNERSHIP_VALUE;
    CobraMutabilityKind mutability = COBRA_MUTABILITY_DEFAULT;
    if (qualifier == 1) {
        ownership = COBRA_OWNERSHIP_BORROWED;
        mutability = COBRA_MUTABILITY_READONLY;
    } else if (qualifier == 2) {
        mutability = COBRA_MUTABILITY_OUT;
    }
    owner->canonical_type = cobra_type_make(parser->canonical_arena, kind,
                                            NULL,
                                            element, error, key, value,
                                            ownership, mutability, -1);
    if (!owner->canonical_type) {
        fprintf(stderr, "%s:%d:%d: error: could not construct canonical type metadata\n",
                parser->source_file, parser->current_token.line,
                parser->current_token.col);
        exit(EXIT_FAILURE);
    }
}

static CobraTypeKind parse_sum_component(Parser *parser, const char *context,
                                         char *type_name_out,
                                         const CobraType **canonical_out) {
    type_name_out[0] = '\0';
    if (canonical_out) *canonical_out = NULL;
    /* Nested Option/Result components: Option[Option[i64]],
       Result[Option[i64], bool], and so on. The component canonical is the
       recursively built sum descriptor. */
    if (match(parser, TOKEN_IDENTIFIER) &&
        (!strcmp(parser->current_token.text, "Option") ||
         !strcmp(parser->current_token.text, "Result"))) {
        bool is_result = !strcmp(parser->current_token.text, "Result");
        advance_token(parser);
        expect(parser, TOKEN_LBRACKET, "Expected '[' after Option or Result");
        char value_name[COBRA_MAX_IDENT_LEN];
        const CobraType *value_canonical = NULL;
        CobraTypeKind value = parse_sum_component(parser,
                                                  "Option or Result value",
                                                  value_name, &value_canonical);
        const CobraType *value_type = value_canonical
            ? value_canonical
            : parser_component_type(parser, value, value_name);
        const CobraType *error_type = NULL;
        if (is_result) {
            expect(parser, TOKEN_COMMA, "Result requires a value and error type");
            char error_name[COBRA_MAX_IDENT_LEN];
            const CobraType *error_canonical = NULL;
            CobraTypeKind error = parse_sum_component(parser, "Result error",
                                                      error_name, &error_canonical);
            error_type = error_canonical
                ? error_canonical
                : parser_component_type(parser, error, error_name);
        }
        expect(parser, TOKEN_RBRACKET, "Expected ']' after Option or Result type");
        if (canonical_out && parser->canonical_arena && value_type) {
            *canonical_out = cobra_type_make(parser->canonical_arena,
                                             is_result ? COBRA_TYPE_RESULT : COBRA_TYPE_OPTION,
                                             NULL, value_type, error_type, NULL, NULL,
                                             COBRA_OWNERSHIP_VALUE,
                                             COBRA_MUTABILITY_DEFAULT, -1);
        }
        return is_result ? COBRA_TYPE_RESULT : COBRA_TYPE_OPTION;
    }
    if (match(parser, TOKEN_IDENTIFIER)) {
        copy_token_text(parser, type_name_out, COBRA_MAX_IDENT_LEN, context);
        advance_token(parser);
        return COBRA_TYPE_STRUCT;
    }
    CobraTypeKind type = token_to_type(parser->current_token.type);
    if (type != COBRA_TYPE_I32 && type != COBRA_TYPE_I64 &&
        type != COBRA_TYPE_U8 && type != COBRA_TYPE_U32 && type != COBRA_TYPE_U64 &&
        type != COBRA_TYPE_F32 && type != COBRA_TYPE_F64 &&
        type != COBRA_TYPE_BOOL && type != COBRA_TYPE_STRING) {
        fprintf(stderr, "%s:%d:%d: error: %s payload must be a scalar, string, or named struct type\n",
                parser->source_file, parser->current_token.line, parser->current_token.col, context);
        exit(1);
    }
    advance_token(parser);
    return type;
}

/* Tuples currently support scalar elements only (see ROADMAP.md phase 14):
   value-owned aggregates raise the same ownership questions as list[T]'s
   owned-field work in progress elsewhere, so they stay out of scope here. */
static bool is_tuple_element_kind(CobraTypeKind kind) {
    switch (kind) {
        case COBRA_TYPE_I32: case COBRA_TYPE_I64: case COBRA_TYPE_U8:
        case COBRA_TYPE_U32: case COBRA_TYPE_U64: case COBRA_TYPE_F32:
        case COBRA_TYPE_F64: case COBRA_TYPE_BOOL:
            return true;
        default:
            return false;
    }
}

/* Deterministic name for a scalar tuple shape, e.g. "__tuple_i64_f32". Two
   tuple types with the same element kinds in the same order always produce
   the same name, so cobra_type_equal (name-based for COBRA_TYPE_STRUCT) and
   the synthetic-struct-decl cache below both treat them as one type. */
static void tuple_struct_name(char *out, size_t out_size,
                              const CobraTypeKind *elem_kinds, size_t count) {
    size_t used = snprintf(out, out_size, "__tuple");
    for (size_t i = 0; i < count && used < out_size; i++) {
        used += snprintf(out + used, out_size - used, "_%s", cobra_type_kind_name(elem_kinds[i]));
    }
}

/* Registers (once per distinct shape) a synthetic AST_STRUCT_DECL with
   positional fields "_0".."_N-1" so the rest of the compiler - layout,
   sret-style struct return, field access - treats a tuple as an ordinary
   struct without any dedicated tuple machinery in type.c/ir.c/codegen.c. */
static void parser_ensure_tuple_struct(Parser *parser, const char *tuple_name,
                                       const CobraType *const *elem_types,
                                       const CobraTypeKind *elem_kinds, size_t count) {
    if (!parser->root) return;
    for (size_t i = 0; i < parser->root->child_count; i++) {
        if (parser->root->children[i]->type == AST_STRUCT_DECL &&
            !strcmp(parser->root->children[i]->name, tuple_name)) return;
    }
    ASTNode *decl = parser_create_node(parser, AST_STRUCT_DECL, tuple_name);
    decl->declared_type = COBRA_TYPE_STRUCT;
    decl->canonical_type = parser_component_type(parser, COBRA_TYPE_STRUCT, tuple_name);
    for (size_t i = 0; i < count; i++) {
        char field_name[16];
        snprintf(field_name, sizeof(field_name), "_%zu", i);
        ASTNode *field = parser_create_node(parser, AST_PARAM, field_name);
        field->declared_type = elem_kinds[i];
        field->canonical_type = elem_types[i];
        ast_add_child(decl, field);
    }
    ast_add_child(parser->root, decl);
}

/* A `let (a, b) = ...` destructure desugars to several sibling statements
   (see parse_statement), but parse_statement can only return one node. It
   marks that bundle with this sentinel name on an AST_PROGRAM wrapper;
   every statement-list loop must splice the wrapper's children in place of
   itself rather than append it as one nested statement - nesting it would
   read fine at parse time but IR validation scopes an AST_PROGRAM
   encountered as a plain statement like an if/while body, so locals
   declared inside it would not be visible to the statements that follow. */
#define TUPLE_DESTRUCTURE_SPLICE_MARKER "__tuple_destructure__"

static CobraTypeKind parse_type_into(Parser *parser, const char *context,
                                     ASTNode *owner, int qualifier) {
    bool slice = false;
    bool tensor = false;
    if (match(parser, TOKEN_LPAREN)) {
        /* Tuple type: (T1, T2, ...). At least two elements - a single
           parenthesized type isn't meaningful and stays reserved for a
           possible future grouping use. */
        advance_token(parser);
        const CobraType *elem_types[8];
        CobraTypeKind elem_kinds[8];
        size_t elem_count = 0;
        for (;;) {
            if (elem_count >= 8) {
                fprintf(stderr, "%s:%d:%d: error: tuple types support at most 8 elements\n",
                        parser->source_file, parser->current_token.line, parser->current_token.col);
                exit(1);
            }
            ASTNode elem_owner;
            memset(&elem_owner, 0, sizeof(elem_owner));
            CobraTypeKind elem_kind = parse_type_into(parser, "tuple element", &elem_owner, 0);
            if (!is_tuple_element_kind(elem_kind)) {
                fprintf(stderr, "%s:%d:%d: error: tuple elements must be scalar values\n",
                        parser->source_file, parser->current_token.line, parser->current_token.col);
                exit(1);
            }
            elem_types[elem_count] = elem_owner.canonical_type
                ? elem_owner.canonical_type
                : parser_component_type(parser, elem_kind, NULL);
            elem_kinds[elem_count] = elem_kind;
            elem_count++;
            if (match(parser, TOKEN_COMMA)) { advance_token(parser); continue; }
            break;
        }
        expect(parser, TOKEN_RPAREN, "Expected ')' after tuple type");
        if (elem_count < 2) {
            fprintf(stderr, "%s:%d:%d: error: a tuple type requires at least two elements\n",
                    parser->source_file, parser->current_token.line, parser->current_token.col);
            exit(1);
        }
        char name[COBRA_MAX_IDENT_LEN];
        tuple_struct_name(name, sizeof(name), elem_kinds, elem_count);
        parser_ensure_tuple_struct(parser, name, elem_types, elem_kinds, elem_count);
        if (owner) {
            owner->declared_type = COBRA_TYPE_STRUCT;
            owner->canonical_type = parser_component_type(parser, COBRA_TYPE_STRUCT, name);
        }
        return COBRA_TYPE_STRUCT;
    }
    if (match(parser, TOKEN_IDENTIFIER) &&
        (!strcmp(parser->current_token.text, "Option") ||
         !strcmp(parser->current_token.text, "Result"))) {
        bool is_result = !strcmp(parser->current_token.text, "Result");
        advance_token(parser);
        expect(parser, TOKEN_LBRACKET, "Expected '[' after Option or Result");
        char value_name[COBRA_MAX_IDENT_LEN];
        const CobraType *value_canonical = NULL;
        CobraTypeKind value = parse_sum_component(parser, "Option or Result value",
                                                  value_name, &value_canonical);
        const CobraType *value_type = value_canonical
            ? value_canonical
            : parser_component_type(parser, value, value_name);
        const CobraType *error_type = NULL;
        if (is_result) {
            expect(parser, TOKEN_COMMA, "Result requires a value and error type");
            char error_name[COBRA_MAX_IDENT_LEN];
            const CobraType *error_canonical = NULL;
            CobraTypeKind error = parse_sum_component(parser, "Result error",
                                                      error_name, &error_canonical);
            error_type = error_canonical
                ? error_canonical
                : parser_component_type(parser, error, error_name);
        }
        expect(parser, TOKEN_RBRACKET, "Expected ']' after Option or Result type");
        if (owner) parser_set_canonical(parser, owner,
                                        is_result ? COBRA_TYPE_RESULT : COBRA_TYPE_OPTION,
                                        qualifier, value_type, error_type, NULL, NULL);
        return is_result ? COBRA_TYPE_RESULT : COBRA_TYPE_OPTION;
    }
    if (match(parser, TOKEN_IDENTIFIER) && strcmp(parser->current_token.text, "array") == 0) {
        /* Fixed value arrays use an explicit bounded form: array[T, N].
           This is intentionally separate from list[T], which is a runtime
           growable collection with its own ownership contract. */
        advance_token(parser);
        expect(parser, TOKEN_LBRACKET, "Expected '[' after array in fixed-array type");
        const CobraType *element_type = NULL;
        if (match(parser, TOKEN_IDENTIFIER) &&
            strcmp(parser->current_token.text, "array") == 0) {
            /* Nested fixed array element: array[array[T, N], M]. The inner
               descriptor is arena-owned and stays valid without joining the
               AST tree. Only scalar-element arrays may nest. */
            ASTNode nested;
            memset(&nested, 0, sizeof(nested));
            CobraTypeKind nested_kind = parse_type_into(parser,
                                                        "fixed-array element",
                                                        &nested, 0);
            if (nested_kind != COBRA_TYPE_ARRAY || !nested.canonical_type) {
                fprintf(stderr, "%s:%d:%d: error: nested fixed-array elements must be fixed arrays\n",
                        parser->source_file, parser->current_token.line,
                        parser->current_token.col);
                exit(1);
            }
            element_type = nested.canonical_type;
        } else if (match(parser, TOKEN_IDENTIFIER)) {
            /* Named value-owned struct element: array[Point, N]. The type
               resolves against declared structs exactly like Option[Point]
               payloads; backend import rejects owning or view-bearing
               structs at the isolated boundary. */
            char element_name[COBRA_MAX_IDENT_LEN];
            copy_token_text(parser, element_name, sizeof(element_name),
                            "fixed-array element");
            element_type = parser_component_type(parser, COBRA_TYPE_STRUCT,
                                                 element_name);
            if (!element_type) {
                fprintf(stderr, "%s:%d:%d: error: unknown fixed-array element struct '%s'\n",
                        parser->source_file, parser->current_token.line,
                        parser->current_token.col, element_name);
                exit(1);
            }
            advance_token(parser);
        } else {
            CobraTypeKind element_kind = token_to_type(parser->current_token.type);
            if (element_kind == COBRA_TYPE_UNKNOWN || element_kind == COBRA_TYPE_VOID ||
                element_kind == COBRA_TYPE_STRING || element_kind == COBRA_TYPE_V256) {
                fprintf(stderr, "%s:%d:%d: error: fixed array elements must be scalar values\n",
                        parser->source_file, parser->current_token.line,
                        parser->current_token.col);
                exit(1);
            }
            element_type = parser_component_type(parser, element_kind, NULL);
            advance_token(parser);
        }
        expect(parser, TOKEN_COMMA, "Expected ',' after fixed-array element type");
        if (!match(parser, TOKEN_INT_LITERAL)) {
            fprintf(stderr, "%s:%d:%d: error: fixed array bound must be an integer literal\n",
                    parser->source_file, parser->current_token.line,
                    parser->current_token.col);
            exit(1);
        }
        errno = 0;
        char *end = NULL;
        unsigned long long bound = strtoull(parser->current_token.text, &end, 10);
        if (errno == ERANGE || end == parser->current_token.text || *end != '\0' ||
            bound == 0 || bound > COBRA_MAX_ARRAY_ELEMENTS) {
            fprintf(stderr, "%s:%d:%d: error: fixed array bound must be in 1..%d\n",
                    parser->source_file, parser->current_token.line,
                    parser->current_token.col, COBRA_MAX_ARRAY_ELEMENTS);
            exit(1);
        }
        size_t array_length = (size_t)bound;
        advance_token(parser);
        expect(parser, TOKEN_RBRACKET, "Expected ']' after fixed-array bound");
        if (owner) {
            owner->array_length = array_length;
            owner->canonical_type = cobra_type_make(parser->canonical_arena,
                                                     COBRA_TYPE_ARRAY, NULL,
                                                     element_type, NULL, NULL, NULL,
                                                     qualifier == 1 ? COBRA_OWNERSHIP_BORROWED
                                                                     : COBRA_OWNERSHIP_VALUE,
                                                     qualifier == 1 ? COBRA_MUTABILITY_READONLY
                                                                     : COBRA_MUTABILITY_DEFAULT,
                                                     -1);
            if (!owner->canonical_type) {
                fprintf(stderr, "%s:%d:%d: error: could not construct fixed-array type metadata\n",
                        parser->source_file, parser->current_token.line,
                        parser->current_token.col);
                exit(EXIT_FAILURE);
            }
            ((CobraType *)owner->canonical_type)->array_length = array_length;
        }
        return COBRA_TYPE_ARRAY;
    }
    if (match(parser, TOKEN_IDENTIFIER) && strcmp(parser->current_token.text, "dyn") == 0) {
        /* Trait-object type: dyn TraitName. Reuses the fn(...)->... single-
           pointer ABI (declared_type COBRA_TYPE_FUNC) since a trait object
           is, at the value level, one pointer to a heap-allocated dispatch
           block; the trait name is stashed on the owner node so IR/codegen
           can tell this apart from an ordinary function value. See
           emit_dyn_trait_call/emit_dyn_dispatch_call in src/codegen.c. */
        advance_token(parser);
        char trait_name[COBRA_MAX_IDENT_LEN];
        copy_token_text(parser, trait_name, sizeof(trait_name), "dyn trait name");
        expect(parser, TOKEN_IDENTIFIER, "Expected trait name after 'dyn'");
        if (owner) {
            snprintf(owner->dyn_trait_name, sizeof(owner->dyn_trait_name), "%.63s", trait_name);
            /* A dummy zero-param i64-returning func type satisfies the
               "typed declaration has canonical metadata" invariant; nothing
               reads its param/return shape since dyn-typed values are only
               ever called through the qualifier (method-call) codegen path,
               never as an ordinary fn(...)->... indirect call. */
            owner->canonical_type = cobra_type_make_func(parser->canonical_arena, NULL, 0,
                                                          parser_component_type(parser, COBRA_TYPE_I64, NULL));
        }
        return COBRA_TYPE_FUNC;
    }
    if (match(parser, TOKEN_IDENTIFIER) && strcmp(parser->current_token.text, "fn") == 0) {
        /* Non-capturing function-value type: fn(T1, T2, ...) -> R. Phase 1
           (see ROADMAP.md) restricts every component to a scalar or void
           return - no slices, structs, or other aggregates yet. */
        advance_token(parser);
        expect(parser, TOKEN_LPAREN, "Expected '(' after fn in function-value type");
        const CobraType *params[COBRA_MAX_TYPE_ARGS];
        size_t param_count = 0;
        if (!match(parser, TOKEN_RPAREN)) {
            for (;;) {
                CobraTypeKind param_kind = token_to_type(parser->current_token.type);
                if (param_kind == COBRA_TYPE_UNKNOWN || param_kind == COBRA_TYPE_VOID ||
                    param_kind == COBRA_TYPE_STRING || param_kind == COBRA_TYPE_V256) {
                    fprintf(stderr, "%s:%d:%d: error: fn(...) parameters must be scalar values\n",
                            parser->source_file, parser->current_token.line,
                            parser->current_token.col);
                    exit(1);
                }
                if (param_count >= COBRA_MAX_TYPE_ARGS - 1) {
                    fprintf(stderr, "%s:%d:%d: error: fn(...) has too many parameters\n",
                            parser->source_file, parser->current_token.line,
                            parser->current_token.col);
                    exit(1);
                }
                params[param_count++] = parser_component_type(parser, param_kind, NULL);
                advance_token(parser);
                if (match(parser, TOKEN_COMMA)) { advance_token(parser); continue; }
                break;
            }
        }
        expect(parser, TOKEN_RPAREN, "Expected ')' after fn(...) parameters");
        expect(parser, TOKEN_ARROW, "Expected '->' after fn(...) parameters");
        CobraTypeKind return_kind = token_to_type(parser->current_token.type);
        if (return_kind == COBRA_TYPE_UNKNOWN || return_kind == COBRA_TYPE_STRING ||
            return_kind == COBRA_TYPE_V256) {
            fprintf(stderr, "%s:%d:%d: error: fn(...) return type must be scalar or void\n",
                    parser->source_file, parser->current_token.line,
                    parser->current_token.col);
            exit(1);
        }
        const CobraType *return_type = parser_component_type(parser, return_kind, NULL);
        advance_token(parser);
        if (owner) {
            owner->canonical_type = cobra_type_make_func(parser->canonical_arena, params,
                                                          param_count, return_type);
            if (!owner->canonical_type) {
                fprintf(stderr, "%s:%d:%d: error: could not construct function-value type metadata\n",
                        parser->source_file, parser->current_token.line,
                        parser->current_token.col);
                exit(EXIT_FAILURE);
            }
        }
        return COBRA_TYPE_FUNC;
    }
    if (match(parser, TOKEN_IDENTIFIER) && strcmp(parser->current_token.text, "list") == 0) {
        advance_token(parser);
        expect(parser, TOKEN_LBRACKET, "Expected '[' after list in collection type");
        const CobraType *generic_element = NULL;
        const CobraType *struct_element = NULL;
        CobraTypeKind element = token_to_type(parser->current_token.type);
        if (match(parser, TOKEN_IDENTIFIER)) {
            generic_element = parser_generic_param(parser, parser->current_token.text);
            if (!generic_element) {
                /* Named value-owned struct element: list[Point]. The type
                   resolves against declared structs; backend import rejects
                   owning or view-bearing structs at the isolated boundary. */
                char element_name[COBRA_MAX_IDENT_LEN];
                copy_token_text(parser, element_name, sizeof(element_name),
                                "list element");
                struct_element = parser_component_type(parser, COBRA_TYPE_STRUCT,
                                                       element_name);
                if (!struct_element) element = COBRA_TYPE_UNKNOWN;
            }
        }
        if ((!generic_element && !struct_element && element == COBRA_TYPE_UNKNOWN) ||
            element == COBRA_TYPE_VOID) {
            fprintf(stderr, "%s:%d:%d: error: list element type must be a scalar type, generic parameter, or named struct\n",
                    parser->source_file, parser->current_token.line, parser->current_token.col);
            exit(1);
        }
        if (generic_element && qualifier != 0) {
            fprintf(stderr, "%s:%d:%d: error: generic owned lists cannot carry a borrowed qualifier\n",
                    parser->source_file, parser->current_token.line,
                    parser->current_token.col);
            exit(1);
        }
        advance_token(parser);
        expect(parser, TOKEN_RBRACKET, "Expected ']' after list element type");
        if (owner) {
            parser_set_canonical(parser, owner, COBRA_TYPE_LIST, qualifier,
                                 generic_element ? generic_element
                                     : (struct_element ? struct_element
                                                       : parser_component_type(parser, element, NULL)),
                                 NULL, NULL, NULL);
        }
        return COBRA_TYPE_LIST;
    }
    if (match(parser, TOKEN_IDENTIFIER) && strcmp(parser->current_token.text, "dict") == 0) {
        advance_token(parser);
        expect(parser, TOKEN_LBRACKET, "Expected '[' after dict in collection type");
        if (!match(parser, TOKEN_TYPE_STRING)) {
            fprintf(stderr, "%s:%d:%d: error: dict keys currently require string\n",
                    parser->source_file, parser->current_token.line, parser->current_token.col);
            exit(1);
        }
        advance_token(parser);
        expect(parser, TOKEN_RBRACKET, "Expected ']' after dict key type");
        CobraTypeKind value = token_to_type(parser->current_token.type);
        if (value != COBRA_TYPE_I64) {
            fprintf(stderr, "%s:%d:%d: error: dict values currently require i64\n",
                    parser->source_file, parser->current_token.line, parser->current_token.col);
            exit(1);
        }
        advance_token(parser);
        if (owner) {
            parser_set_canonical(parser, owner, COBRA_TYPE_DICT, qualifier, NULL, NULL,
                                 parser_component_type(parser, COBRA_TYPE_STRING, NULL),
                                 parser_component_type(parser, value, NULL));
        }
        return COBRA_TYPE_DICT;
    }
    if (match(parser, TOKEN_IDENTIFIER) && strcmp(parser->current_token.text, "tensor") == 0) {
        tensor = true;
        advance_token(parser);
        if (!match(parser, TOKEN_LBRACKET)) {
            fprintf(stderr, "%s:%d:%d: error: Expected '[' after tensor in %s type\n",
                    parser->source_file, parser->current_token.line, parser->current_token.col, context);
            exit(1);
        }
        advance_token(parser);
        if (match(parser, TOKEN_RBRACKET)) {
            fprintf(stderr, "%s:%d:%d: error: tensor shape cannot be empty\n",
                    parser->source_file, parser->current_token.line, parser->current_token.col);
            exit(1);
        }
        while (!match(parser, TOKEN_RBRACKET)) {
            if (owner && owner->shape_rank >= COBRA_MAX_SHAPE_DIMS) {
                fprintf(stderr, "%s:%d:%d: error: tensor rank exceeds %d\n",
                        parser->source_file, parser->current_token.line, parser->current_token.col, COBRA_MAX_SHAPE_DIMS);
                exit(1);
            }
            if (!match(parser, TOKEN_IDENTIFIER) && !match(parser, TOKEN_INT_LITERAL)) {
                fprintf(stderr, "%s:%d:%d: error: expected a tensor dimension name or integer\n",
                        parser->source_file, parser->current_token.line, parser->current_token.col);
                exit(1);
            }
            if (owner) {
                snprintf(owner->shape_dims[owner->shape_rank], COBRA_MAX_IDENT_LEN, "%.63s", parser->current_token.text);
                owner->shape_rank++;
            }
            advance_token(parser);
            if (match(parser, TOKEN_COMMA)) advance_token(parser);
            else if (!match(parser, TOKEN_RBRACKET)) {
                fprintf(stderr, "%s:%d:%d: error: expected ',' or ']' in tensor shape\n",
                        parser->source_file, parser->current_token.line, parser->current_token.col);
                exit(1);
            }
        }
        if (owner && owner->shape_rank > COBRA_NATIVE_TENSOR_MAX_RANK) {
            fprintf(stderr, "%s:%d:%d: error: native tensors currently support rank 1 or 2; rank %d requires a future backend\n",
                    parser->source_file, parser->current_token.line, parser->current_token.col, owner->shape_rank);
            exit(1);
        }
        advance_token(parser);
    } else if (match(parser, TOKEN_LBRACKET)) {
        advance_token(parser);
        if (!match(parser, TOKEN_RBRACKET)) {
            fprintf(stderr, "%s:%d:%d: error: Expected ']' in %s slice type\n",
                    parser->source_file, parser->current_token.line, parser->current_token.col, context);
            exit(1);
        }
        advance_token(parser);
        slice = true;
    }

    if (match(parser, TOKEN_IDENTIFIER)) {
        char named_type[COBRA_MAX_IDENT_LEN];
        copy_token_text(parser, named_type, sizeof(named_type), "type name");
        const CobraType *generic = parser_generic_param(parser, named_type);
        advance_token(parser);
        if (generic) {
            if (tensor) {
                fprintf(stderr, "%s:%d:%d: error: scalar generic parameters cannot be used as tensor element types\n",
                        parser->source_file, parser->current_token.line, parser->current_token.col);
                exit(1);
            }
            if (slice) {
                if (qualifier != 1 && qualifier != 2) {
                    fprintf(stderr, "%s:%d:%d: error: generic slices require an explicit readonly or out qualifier\n",
                            parser->source_file, parser->current_token.line, parser->current_token.col);
                    exit(1);
                }
                if (owner) {
                    parser_set_canonical(parser, owner, COBRA_TYPE_SLICE, qualifier,
                                         generic, NULL, NULL, NULL);
                }
                return COBRA_TYPE_SLICE;
            }
            if (qualifier != 0) {
                fprintf(stderr, "%s:%d:%d: error: generic parameters cannot carry ownership qualifiers\n",
                        parser->source_file, parser->current_token.line, parser->current_token.col);
                exit(1);
            }
            if (owner) owner->canonical_type = generic;
            return COBRA_TYPE_GENERIC_PARAM;
        }

        const CobraType *generic_argument = NULL;
        if (match(parser, TOKEN_LBRACKET)) {
            advance_token(parser);
            if (match(parser, TOKEN_IDENTIFIER)) {
                char argument_name[COBRA_MAX_IDENT_LEN];
                copy_token_text(parser, argument_name, sizeof(argument_name), "generic type argument");
                generic_argument = parser_generic_param(parser, argument_name);
                if (!generic_argument) {
                    fprintf(stderr, "%s:%d:%d: error: generic struct arguments must be scalar types or parameters\n",
                            parser->source_file, parser->current_token.line, parser->current_token.col);
                    exit(1);
                }
                advance_token(parser);
            } else {
                CobraTypeKind argument_kind = token_to_type(parser->current_token.type);
                if (argument_kind == COBRA_TYPE_UNKNOWN ||
                    argument_kind == COBRA_TYPE_VOID ||
                    argument_kind == COBRA_TYPE_F64) {
                    fprintf(stderr, "%s:%d:%d: error: generic struct arguments must be scalar types\n",
                            parser->source_file, parser->current_token.line, parser->current_token.col);
                    exit(1);
                }
                generic_argument = parser_component_type(parser, argument_kind, NULL);
                advance_token(parser);
            }
            expect(parser, TOKEN_RBRACKET, "Expected ']' after generic struct argument");
        }
        /* A named generic struct is represented by its base name plus one
           canonical argument until IR materializes the packed specialization. */
        if (owner && parser->canonical_arena) {
            CobraOwnershipKind ownership = qualifier == 1 ? COBRA_OWNERSHIP_BORROWED : COBRA_OWNERSHIP_VALUE;
            CobraMutabilityKind mutability = qualifier == 1 ? COBRA_MUTABILITY_READONLY :
                                             (qualifier == 2 ? COBRA_MUTABILITY_OUT :
                                                              COBRA_MUTABILITY_DEFAULT);
            owner->canonical_type = cobra_type_make(parser->canonical_arena,
                                                    COBRA_TYPE_STRUCT,
                                                    named_type,
                                                    generic_argument, NULL, NULL, NULL,
                                                    ownership, mutability, -1);
        }
        return COBRA_TYPE_STRUCT;
    }
    CobraTypeKind type = token_to_type(parser->current_token.type);
    if (type == COBRA_TYPE_UNKNOWN) {
        fprintf(stderr, "%s:%d:%d: error: Expected %s type, got '%s'\n",
                parser->source_file, parser->current_token.line, parser->current_token.col, context, parser->current_token.text);
        exit(1);
    }
    advance_token(parser);
    if (tensor) {
        if (type != COBRA_TYPE_F32) {
            fprintf(stderr, "%s:%d:%d: error: tensor views currently require f32 elements\n",
                    parser->source_file, parser->current_token.line, parser->current_token.col);
            exit(1);
        }
        if (owner) parser_set_canonical(parser, owner, COBRA_TYPE_TENSOR_F32, qualifier,
                                        parser_component_type(parser, COBRA_TYPE_F32, NULL),
                                        NULL, NULL, NULL);
        return COBRA_TYPE_TENSOR_F32;
    }
    if (slice) {
        if (type == COBRA_TYPE_I64) {
            if (owner) parser_set_canonical(parser, owner, COBRA_TYPE_SLICE, qualifier,
                                            parser_component_type(parser, type, NULL),
                                            NULL, NULL, NULL);
            return COBRA_TYPE_SLICE;
        }
        if (type == COBRA_TYPE_F32) {
            if (owner) parser_set_canonical(parser, owner, COBRA_TYPE_SLICE_F32, qualifier,
                                            parser_component_type(parser, type, NULL),
                                            NULL, NULL, NULL);
            return COBRA_TYPE_SLICE_F32;
        }
        if (type == COBRA_TYPE_U8) {
            if (owner) parser_set_canonical(parser, owner, COBRA_TYPE_SLICE_U8, qualifier,
                                            parser_component_type(parser, type, NULL),
                                            NULL, NULL, NULL);
            return COBRA_TYPE_SLICE_U8;
        }
        fprintf(stderr, "%s:%d:%d: error: only []i64, []f32, and []u8 slices are supported\n",
                parser->source_file, parser->current_token.line, parser->current_token.col);
        exit(1);
    }
    if (owner) parser_set_canonical(parser, owner, type, qualifier, NULL, NULL, NULL, NULL);
    return type;
}

static bool expect(Parser *parser, TokenType type, const char *err_msg) {
    if (match(parser, type)) {
        advance_token(parser);
        return true;
    }
    fprintf(stderr, "%s:%d:%d: error: %s (got '%s', type=%d, expected=%d)\n",
            parser->source_file, parser->current_token.line, parser->current_token.col, err_msg, parser->current_token.text, parser->current_token.type, type);
    exit(1);
}

void parser_init_with_file(Parser *parser, const char *source, const char *source_file) {
    memset(parser, 0, sizeof(*parser));
    lexer_init(&parser->lexer, source);
    snprintf(parser->source_file, sizeof(parser->source_file), "%s",
             source_file && *source_file ? source_file : "<source>");
    advance_token(parser);
}

void parser_init(Parser *parser, const char *source) {
    parser_init_with_file(parser, source, "<source>");
}

static ASTNode *parse_expression(Parser *parser);

/* Parse a non-negative integer literal magnitude. When the magnitude fits a
   signed long long, *is_unsigned is false and the signed value is returned.
   Larger magnitudes that still fit uint64 are accepted as unsigned literals
   (u64 contexts), so 18446744073709551615 parses where a signed read would
   overflow. */
static uint64_t parse_integer_magnitude(Parser *parser, bool *is_unsigned) {
    errno = 0;
    char *end = NULL;
    long long value = strtoll(parser->current_token.text, &end, 10);
    if (errno != ERANGE && end != parser->current_token.text && *end == '\0' && value >= 0) {
        if (is_unsigned) *is_unsigned = false;
        return (uint64_t)value;
    }
    errno = 0;
    unsigned long long uvalue = strtoull(parser->current_token.text, &end, 10);
    if (errno != ERANGE && end != parser->current_token.text && *end == '\0') {
        if (is_unsigned) *is_unsigned = true;
        return (uint64_t)uvalue;
    }
    fprintf(stderr, "%s:%d:%d: error: integer literal is out of range\n",
            parser->source_file, parser->current_token.line, parser->current_token.col);
    exit(1);
}

static int parse_int_literal(Parser *parser) {
    bool is_unsigned = false;
    uint64_t magnitude = parse_integer_magnitude(parser, &is_unsigned);
    if (is_unsigned || magnitude > INT_MAX) {
        fprintf(stderr, "%s:%d:%d: error: integer literal is out of range\n",
                parser->source_file, parser->current_token.line, parser->current_token.col);
        exit(1);
    }
    return (int)magnitude;
}

static ASTNode *parse_closure_literal(Parser *parser);

static ASTNode *parse_primary(Parser *parser) {
    if (match(parser, TOKEN_DEF)) {
        return parse_closure_literal(parser);
    }

    if (match(parser, TOKEN_MINUS)) {
        advance_token(parser);
        if (match(parser, TOKEN_INT_LITERAL)) {
            ASTNode *node = parser_create_node(parser, AST_INT_LITERAL, NULL);
            /* Negative literals are signed even when the magnitude itself
               exceeds INT64_MAX: -9223372036854775808 is INT64_MIN, while a
               magnitude above 2^63 has no representable negation. */
            bool is_unsigned = false;
            uint64_t magnitude = parse_integer_magnitude(parser, &is_unsigned);
            if (magnitude > (uint64_t)INT64_MAX + 1) {
                fprintf(stderr, "%s:%d:%d: error: integer literal is out of range\n",
                        parser->source_file, parser->current_token.line, parser->current_token.col);
                exit(1);
            }
            node->literal_is_unsigned = false;
            node->literal_u64 = magnitude;
            node->literal_i64 = magnitude == (uint64_t)INT64_MAX + 1
                ? INT64_MIN : -(int64_t)magnitude;
            node->int_val = (int)(int32_t)node->literal_i64;
            advance_token(parser);
            return node;
        }
        if (match(parser, TOKEN_FLOAT_LITERAL)) {
            ASTNode *node = parser_create_node(parser, AST_FLOAT_LITERAL, NULL);
            node->literal_f64 = -atof(parser->current_token.text);
            node->float_val = (float)node->literal_f64;
            advance_token(parser);
            return node;
        }
        fprintf(stderr, "%s:%d:%d: error: unary '-' requires a numeric literal\n",
                parser->source_file, parser->current_token.line, parser->current_token.col);
        exit(1);
    }

    if (match(parser, TOKEN_TRUE) || match(parser, TOKEN_FALSE)) {
        ASTNode *node = parser_create_node(parser, AST_BOOL_LITERAL, NULL);
        node->int_val = match(parser, TOKEN_TRUE) ? 1 : 0;
        advance_token(parser);
        return node;
    }

    if (match(parser, TOKEN_NONE)) {
        ASTNode *node = parser_create_node(parser, AST_NONE_LITERAL, NULL);
        advance_token(parser);
        return node;
    }

    if (match(parser, TOKEN_COMPTIME)) {
        advance_token(parser);
        ASTNode *ct_node = parser_create_node(parser, AST_COMPTIME_EXPR, NULL);
        ASTNode *inner_expr = parse_expression(parser);
        ast_add_child(ct_node, inner_expr);
        return ct_node;
    }

    if (match(parser, TOKEN_LBRACE)) {
        advance_token(parser);
        ASTNode *dict_node = parser_create_node(parser, AST_DICT_LITERAL, NULL);
        while (!match(parser, TOKEN_RBRACE)) {
            if (!match(parser, TOKEN_STRING_LITERAL)) {
                fprintf(stderr, "%s:%d:%d: error: dictionary keys must be string literals\n",
                        parser->source_file, parser->current_token.line, parser->current_token.col);
                exit(1);
            }
            ASTNode *entry = parser_create_node(parser, AST_DICT_ENTRY, parser->current_token.text);
            advance_token(parser);
            expect(parser, TOKEN_COLON, "Expected ':' after dictionary key");
            ast_add_child(entry, parse_expression(parser));
            ast_add_child(dict_node, entry);
            if (match(parser, TOKEN_COMMA)) advance_token(parser);
            else break;
        }
        expect(parser, TOKEN_RBRACE, "Expected '}' after dictionary entries");
        return dict_node;
    }

    if (match(parser, TOKEN_LBRACKET)) {
        advance_token(parser);
        ASTNode *arr_node = parser_create_node(parser, AST_ARRAY_LITERAL, NULL);
        if (!match(parser, TOKEN_RBRACKET)) {
            ASTNode *first = parse_expression(parser);
            if (match(parser, TOKEN_FOR)) {
                /* List comprehension: [expr for target in source (if guard)?] */
                Token for_token = parser->current_token;
                advance_token(parser);
                ASTNode *comp_node = parser_create_node_at(parser, AST_COMPREHENSION, NULL, for_token);
                if (!match(parser, TOKEN_IDENTIFIER)) {
                    fprintf(stderr, "%s:%d:%d: error: expected comprehension target name after 'for'\n",
                            parser->source_file, parser->current_token.line, parser->current_token.col);
                    exit(1);
                }
                copy_token_text(parser, comp_node->name, sizeof(comp_node->name), "comprehension target");
                advance_token(parser);
                if (!match(parser, TOKEN_IN)) {
                    fprintf(stderr, "%s:%d:%d: error: expected 'in' in list comprehension\n",
                            parser->source_file, parser->current_token.line, parser->current_token.col);
                    exit(1);
                }
                advance_token(parser);
                ast_add_child(comp_node, first);
                ast_add_child(comp_node, parse_expression(parser));
                if (match(parser, TOKEN_IF)) {
                    advance_token(parser);
                    ast_add_child(comp_node, parse_expression(parser));
                }
                expect(parser, TOKEN_RBRACKET, "Expected ']' after comprehension");
                return comp_node;
            }
            ast_add_child(arr_node, first);
            while (match(parser, TOKEN_COMMA)) {
                advance_token(parser);
                ast_add_child(arr_node, parse_expression(parser));
            }
        }
        expect(parser, TOKEN_RBRACKET, "Expected ']' after array elements");
        return arr_node;
    }

    if (match(parser, TOKEN_INT_LITERAL)) {
        ASTNode *node = parser_create_node(parser, AST_INT_LITERAL, NULL);
        bool is_unsigned = false;
        uint64_t magnitude = parse_integer_magnitude(parser, &is_unsigned);
        node->literal_is_unsigned = is_unsigned;
        node->literal_u64 = magnitude;
        node->literal_i64 = (int64_t)magnitude;
        node->int_val = (int)(int32_t)node->literal_i64;
        advance_token(parser);
        return node;
    }

    if (match(parser, TOKEN_FLOAT_LITERAL)) {
        ASTNode *node = parser_create_node(parser, AST_FLOAT_LITERAL, NULL);
        node->literal_f64 = atof(parser->current_token.text);
        node->float_val = (float)node->literal_f64;
        advance_token(parser);
        return node;
    }

    if (match(parser, TOKEN_STRING_LITERAL)) {
        ASTNode *node = parser_create_node(parser, AST_STRING_LITERAL, NULL);
        snprintf(node->string_val, sizeof(node->string_val), "%s", parser->current_token.text);
        advance_token(parser);
        return node;
    }

    if (match(parser, TOKEN_LEN)) {
        advance_token(parser);
        expect(parser, TOKEN_LPAREN, "Expected '(' after 'len'");
        ASTNode *len_node = parser_create_node(parser, AST_LEN_EXPR, NULL);
        ASTNode *arg = parse_expression(parser);
        ast_add_child(len_node, arg);
        expect(parser, TOKEN_RPAREN, "Expected ')' after len argument");
        return len_node;
    }

    if (match(parser, TOKEN_IDENTIFIER)) {
        char name[COBRA_MAX_IDENT_LEN];
        Token identifier_token = parser->current_token;
        copy_token_text(parser, name, sizeof(name), "identifier");
        advance_token(parser);

        // Check for array indexing: arr[index]
        if (match(parser, TOKEN_LBRACKET)) {
            advance_token(parser);
            ASTNode *idx_node = parser_create_node_at(parser, AST_ARRAY_INDEX, name, identifier_token);
            /* A tensor index may address more than one logical axis:
               x[row, col]. Ordinary arrays continue to use one index. */
            ast_add_child(idx_node, parse_expression(parser));
            while (match(parser, TOKEN_COMMA)) {
                advance_token(parser);
                ast_add_child(idx_node, parse_expression(parser));
            }
            expect(parser, TOKEN_RBRACKET, "Expected ']' after array index");
            return idx_node;
        }

        /* Qualified calls use one dot and remain calls to the underlying
           unqualified native symbol: alias.function(...). */
        if (match(parser, TOKEN_DOT)) {
            advance_token(parser);
            if (!match(parser, TOKEN_IDENTIFIER)) {
                fprintf(stderr, "%s:%d:%d: error: expected function name after module qualifier '%s.'\n",
                        parser->source_file, parser->current_token.line, parser->current_token.col, name);
                exit(1);
            }
            char qualified_name[COBRA_MAX_IDENT_LEN];
            copy_token_text(parser, qualified_name, sizeof(qualified_name), "qualified function name");
            advance_token(parser);
            if (!match(parser, TOKEN_LPAREN)) {
                /* Struct member access, with optional byte-view indexing such
                   as request.body[0]. */
                if (match(parser, TOKEN_LBRACKET)) {
                    advance_token(parser);
                    ASTNode *index_node = parser_create_node_at(parser, AST_ARRAY_INDEX, name, identifier_token);
                    snprintf(index_node->secondary_name, sizeof(index_node->secondary_name), "%.63s", qualified_name);
                    ast_add_child(index_node, parse_expression(parser));
                    while (match(parser, TOKEN_COMMA)) {
                        advance_token(parser);
                        ast_add_child(index_node, parse_expression(parser));
                    }
                    expect(parser, TOKEN_RBRACKET, "Expected ']' after struct byte-view index");
                    return index_node;
                }
                ASTNode *member_node = parser_create_node_at(parser, AST_MEMBER_ACCESS, name, identifier_token);
                snprintf(member_node->secondary_name, sizeof(member_node->secondary_name), "%.63s", qualified_name);
                ASTNode *base_ref = parser_create_node_at(parser, AST_VAR_REF, name, identifier_token);
                ast_add_child(member_node, base_ref);
                /* Continue a field chain such as pair.left.x. Methods and
                   calls remain limited to the first qualified segment. */
                while (match(parser, TOKEN_DOT)) {
                    advance_token(parser);
                    if (!match(parser, TOKEN_IDENTIFIER)) {
                        fprintf(stderr, "%s:%d:%d: error: expected field name after '.'\n",
                                parser->source_file, parser->current_token.line, parser->current_token.col);
                        exit(1);
                    }
                    Token nested_token = parser->current_token;
                    char nested_name[COBRA_MAX_IDENT_LEN];
                    copy_token_text(parser, nested_name, sizeof(nested_name), "nested field name");
                    advance_token(parser);
                    if (match(parser, TOKEN_LPAREN)) {
                        fprintf(stderr, "%s:%d:%d: error: nested struct methods are not supported yet\n",
                                parser->source_file, nested_token.line, nested_token.col);
                        exit(1);
                    }
                    ASTNode *nested = parser_create_node_at(parser, AST_MEMBER_ACCESS, name, nested_token);
                    snprintf(nested->secondary_name, sizeof(nested->secondary_name), "%.63s", nested_name);
                    ast_add_child(nested, member_node);
                    member_node = nested;
                }
                return member_node;
            }
            advance_token(parser);
            ASTNode *call_node = parser_create_node_at(parser, AST_FUNC_CALL, qualified_name, identifier_token);
            snprintf(call_node->qualifier, sizeof(call_node->qualifier), "%s", name);
            if (!match(parser, TOKEN_RPAREN)) {
                ast_add_child(call_node, parse_expression(parser));
                while (match(parser, TOKEN_COMMA)) {
                    advance_token(parser);
                    ast_add_child(call_node, parse_expression(parser));
                }
            }
            expect(parser, TOKEN_RPAREN, "Expected ')' after qualified function call arguments");
            if (match(parser, TOKEN_QUESTION)) {
                advance_token(parser);
                call_node->propagate_error = true;
            }
            return call_node;
        }

        // Check for function call: func_name(...)
        if (match(parser, TOKEN_LPAREN)) {
            advance_token(parser); // skip (
            ASTNode *call_node = parser_create_node_at(parser, AST_FUNC_CALL, name, identifier_token);
            if (!match(parser, TOKEN_RPAREN)) {
                ast_add_child(call_node, parse_expression(parser));
                while (match(parser, TOKEN_COMMA)) {
                    advance_token(parser);
                    ast_add_child(call_node, parse_expression(parser));
                }
            }
            expect(parser, TOKEN_RPAREN, "Expected ')' after function call arguments");
            /* Postfix `?` is reserved for status propagation after calls.
               The existing `name?` inspector remains unchanged. */
            if (match(parser, TOKEN_QUESTION)) {
                advance_token(parser);
                call_node->propagate_error = true;
            }
            return call_node;
        }

        // Check for ? quick inspector: x?
        if (match(parser, TOKEN_QUESTION)) {
            advance_token(parser); // skip ?
            ASTNode *inspect_node = parser_create_node_at(parser, AST_INSPECT_STMT, name, identifier_token);
            return inspect_node;
        }

        ASTNode *node = parser_create_node_at(parser, AST_VAR_REF, name, identifier_token);
        return node;
    }

    if (match(parser, TOKEN_LPAREN)) {
        Token paren_token = parser->current_token;
        advance_token(parser);
        ASTNode *expr = parse_expression(parser);
        if (match(parser, TOKEN_COMMA)) {
            /* (a, b, ...): a tuple literal, not a grouping paren. Only
               `return (...)` and a direct `let (a, b) = (x, y)` destructure
               give this node anywhere to go; see the comment on AST_TUPLE. */
            ASTNode *tuple = parser_create_node_at(parser, AST_TUPLE, NULL, paren_token);
            ast_add_child(tuple, expr);
            while (match(parser, TOKEN_COMMA)) {
                advance_token(parser);
                ast_add_child(tuple, parse_expression(parser));
            }
            expect(parser, TOKEN_RPAREN, "Expected ')' after tuple literal");
            return tuple;
        }
        expect(parser, TOKEN_RPAREN, "Expected ')' after expression");
        return expr;
    }

    fprintf(stderr, "%s:%d:%d: error: Expected expression, got '%s'\n",
            parser->source_file, parser->current_token.line, parser->current_token.col, parser->current_token.text);
    exit(1);
}

static bool is_multiplicative(Parser *parser) {
    return match(parser, TOKEN_STAR) || match(parser, TOKEN_SLASH) || match(parser, TOKEN_PERCENT);
}

static bool is_additive(Parser *parser) {
    return match(parser, TOKEN_PLUS) || match(parser, TOKEN_MINUS);
}

static bool is_comparison(Parser *parser) {
    return match(parser, TOKEN_EQ) || match(parser, TOKEN_NEQ) ||
           match(parser, TOKEN_LT) || match(parser, TOKEN_GT) ||
           match(parser, TOKEN_LTE) || match(parser, TOKEN_GTE);
}

static ASTNode *parse_cast(Parser *parser);

static ASTNode *make_binary(Parser *parser, ASTNode *left) {
    char op[8];
    Token op_token = parser->current_token;
    strcpy(op, parser->current_token.text);
    advance_token(parser);
    ASTNode *right = parse_cast(parser);
    ASTNode *binary = parser_create_node_at(parser, AST_BINARY_OP, op, op_token);
    ast_add_child(binary, left);
    ast_add_child(binary, right);
    return binary;
}

/* expr as Type binds tighter than the arithmetic operators (so `a + b as f32`
   reads as `a + (b as f32)`) but looser than a primary, matching how most
   C-like languages place an explicit cast. */
static ASTNode *parse_cast(Parser *parser) {
    ASTNode *left = parse_primary(parser);
    while (match(parser, TOKEN_AS)) {
        Token op_token = parser->current_token;
        advance_token(parser);
        CobraTypeKind target = token_to_type(parser->current_token.type);
        if (target != COBRA_TYPE_I32 && target != COBRA_TYPE_I64 &&
            target != COBRA_TYPE_U8 && target != COBRA_TYPE_U32 && target != COBRA_TYPE_U64 &&
            target != COBRA_TYPE_F32 && target != COBRA_TYPE_F64 && target != COBRA_TYPE_BOOL) {
            fprintf(stderr, "%s:%d:%d: error: 'as' requires a scalar numeric or bool type\n",
                    parser->source_file, parser->current_token.line, parser->current_token.col);
            exit(1);
        }
        advance_token(parser);
        ASTNode *cast = parser_create_node_at(parser, AST_CAST_EXPR, "as", op_token);
        cast->declared_type = target;
        ast_add_child(cast, left);
        left = cast;
    }
    return left;
}

static ASTNode *parse_multiplicative(Parser *parser) {
    ASTNode *left = parse_cast(parser);
    while (is_multiplicative(parser)) left = make_binary(parser, left);
    return left;
}

static ASTNode *parse_additive(Parser *parser) {
    ASTNode *left = parse_multiplicative(parser);
    while (is_additive(parser)) {
        char op[8];
        Token op_token = parser->current_token;
        strcpy(op, parser->current_token.text);
        advance_token(parser);
        ASTNode *right = parse_multiplicative(parser);
        ASTNode *binary = parser_create_node_at(parser, AST_BINARY_OP, op, op_token);
        ast_add_child(binary, left);
        ast_add_child(binary, right);
        left = binary;
    }
    return left;
}

static ASTNode *parse_expression(Parser *parser) {
    ASTNode *left = parse_additive(parser);
    while (is_comparison(parser) || match(parser, TOKEN_IN) || match(parser, TOKEN_NOT)) {
        if (match(parser, TOKEN_NOT)) {
            /* The 'not in' form is the only supported 'not' expression. */
            Token op_token = parser->current_token;
            advance_token(parser);
            if (!match(parser, TOKEN_IN)) {
                fprintf(stderr, "%s:%d:%d: error: expected 'in' after 'not'\n",
                        parser->source_file, parser->current_token.line, parser->current_token.col);
                exit(1);
            }
            advance_token(parser);
            ASTNode *right = parse_additive(parser);
            ASTNode *membership = parser_create_node_at(parser, AST_MEMBERSHIP, "not in", op_token);
            ast_add_child(membership, left);
            ast_add_child(membership, right);
            left = membership;
            continue;
        }
        if (match(parser, TOKEN_IN)) {
            Token op_token = parser->current_token;
            advance_token(parser);
            ASTNode *right = parse_additive(parser);
            ASTNode *membership = parser_create_node_at(parser, AST_MEMBERSHIP, "in", op_token);
            ast_add_child(membership, left);
            ast_add_child(membership, right);
            left = membership;
            continue;
        }
        char op[8];
        Token op_token = parser->current_token;
        strcpy(op, parser->current_token.text);
        advance_token(parser);
        ASTNode *right = parse_additive(parser);
        ASTNode *binary = parser_create_node_at(parser, AST_BINARY_OP, op, op_token);
        ast_add_child(binary, left);
        ast_add_child(binary, right);
        left = binary;
    }
    return left;
}

static ASTNode *parse_block(Parser *parser);

static ASTNode *parse_statement(Parser *parser) {
    // @compute block for hardware vectorization / SIMD GPU dispatch
    if (match(parser, TOKEN_COMPUTE)) {
        advance_token(parser);
        ASTNode *compute_node = parser_create_node(parser, AST_COMPUTE_BLOCK, NULL);
        compute_node->target_device = TARGET_DEV_GPU_VECTOR; // Tag block for GPU Vectorization!
        
        if (match(parser, TOKEN_COLON)) advance_token(parser);
        ASTNode *block_body = parse_block(parser);
        ast_add_child(compute_node, block_body);
        return compute_node;
    }

    // @parallel block: native chunked execution for proven element-wise loops
    if (match(parser, TOKEN_PARALLEL)) {
        advance_token(parser);
        ASTNode *parallel_node = parser_create_node(parser, AST_PARALLEL_BLOCK, NULL);
        parallel_node->target_device = TARGET_DEV_CPU;
        if (match(parser, TOKEN_COLON)) advance_token(parser);
        ASTNode *block_body = parse_block(parser);
        ast_add_child(parallel_node, block_body);
        return parallel_node;
    }

    // with region scratch(capacity): body - a bump arena whose backing
    // storage is released exactly once at scope exit.
    if (match(parser, TOKEN_WITH)) {
        advance_token(parser);
        expect(parser, TOKEN_REGION, "Expected 'region' after 'with'");
        if (!match(parser, TOKEN_IDENTIFIER)) {
            fprintf(stderr, "%s:%d:%d: error: expected a region name after 'with region'\n",
                    parser->source_file, parser->current_token.line, parser->current_token.col);
            exit(1);
        }
        char region_name[COBRA_MAX_IDENT_LEN];
        copy_token_text(parser, region_name, sizeof(region_name), "region name");
        advance_token(parser);
        ASTNode *with_node = parser_create_node(parser, AST_WITH_REGION, region_name);
        if (match(parser, TOKEN_LPAREN)) {
            advance_token(parser);
            ast_add_child(with_node, parse_expression(parser));
            expect(parser, TOKEN_RPAREN, "Expected ')' after region capacity");
        } else {
            ASTNode *cap = parser_create_node(parser, AST_INT_LITERAL, NULL);
            cap->int_val = 1048576; /* 1 MiB default backing store */
            cap->literal_i64 = 1048576; /* literal_i64 is authoritative for codegen; int_val alone is stale */
            ast_add_child(with_node, cap);
        }
        if (match(parser, TOKEN_COLON)) advance_token(parser);
        ASTNode *body = parse_block(parser);
        ast_add_child(with_node, body);
        return with_node;
    }

    // for loop: for i in numbers:
    if (match(parser, TOKEN_FOR)) {
        advance_token(parser);
        char iter_var[COBRA_MAX_IDENT_LEN];
        Token iterator_token = parser->current_token;
        copy_token_text(parser, iter_var, sizeof(iter_var), "iterator name");
        expect(parser, TOKEN_IDENTIFIER, "Expected iterator variable name after 'for'");
        
        ASTNode *for_node = parser_create_node_at(parser, AST_FOR_LOOP, iter_var, iterator_token);
        if (match(parser, TOKEN_COMMA)) {
            advance_token(parser);
            if (!match(parser, TOKEN_IDENTIFIER)) {
                fprintf(stderr, "%s:%d:%d: error: expected second iterator name after ','\n",
                        parser->source_file, parser->current_token.line, parser->current_token.col);
                exit(1);
            }
            copy_token_text(parser, for_node->secondary_name,
                            sizeof(for_node->secondary_name), "second iterator name");
            advance_token(parser);
        }
        expect(parser, TOKEN_IN, "Expected 'in' after iterator variable in for loop");
        
        ASTNode *target_expr = parse_expression(parser);
        ast_add_child(for_node, target_expr);
        
        if (match(parser, TOKEN_COLON)) advance_token(parser);
        ASTNode *body = parse_block(parser);
        ast_add_child(for_node, body);
        return for_node;
    }

    /* match value: { case Type.Variant: { ... } else: { ... } } */
    if (match(parser, TOKEN_MATCH)) {
        advance_token(parser);
        ASTNode *match_node = parser_create_node(parser, AST_MATCH_STMT, NULL);
        ast_add_child(match_node, parse_expression(parser));
        if (match(parser, TOKEN_COLON)) advance_token(parser);
        expect(parser, TOKEN_LBRACE, "Expected '{' after match expression");
        while (!match(parser, TOKEN_RBRACE) && !match(parser, TOKEN_EOF)) {
            if (match(parser, TOKEN_CASE)) {
                advance_token(parser);
                if (!match(parser, TOKEN_IDENTIFIER) && !match(parser, TOKEN_NONE)) {
                    fprintf(stderr, "%s:%d:%d: error: expected pattern name after 'case'\n",
                            parser->source_file, parser->current_token.line, parser->current_token.col);
                    exit(1);
                }
                Token enum_token = parser->current_token;
                ASTNode *case_node = parser_create_node_at(parser, AST_MATCH_CASE, NULL, enum_token);
                copy_token_text(parser, case_node->match_type_name, sizeof(case_node->match_type_name), "match pattern name");
                advance_token(parser);
                if (match(parser, TOKEN_LPAREN)) {
                    /* Sum payload pattern: case some(x) / ok(x) / err(e).
                       The payload binding is stored in secondary_name. */
                    advance_token(parser);
                    if (!match(parser, TOKEN_IDENTIFIER)) {
                        fprintf(stderr, "%s:%d:%d: error: expected binding name after 'case %s('\n",
                                parser->source_file, parser->current_token.line,
                                parser->current_token.col, case_node->match_type_name);
                        exit(1);
                    }
                    copy_token_text(parser, case_node->secondary_name,
                                    sizeof(case_node->secondary_name), "match binding name");
                    advance_token(parser);
                    expect(parser, TOKEN_RPAREN, "Expected ')' after match binding");
                } else if (match(parser, TOKEN_DOT)) {
                    /* Enum pattern: case Type.Variant, with an optional
                       payload binding list: case Shape.Circle(r). Payload
                       bindings become trailing child AST_PARAM nodes after
                       the arm body, so the body stays child zero. */
                    advance_token(parser);
                    if (!match(parser, TOKEN_IDENTIFIER)) {
                        fprintf(stderr, "%s:%d:%d: error: expected enum variant after 'case %s.'\n",
                                parser->source_file, parser->current_token.line, parser->current_token.col, case_node->match_type_name);
                        exit(1);
                    }
                    copy_token_text(parser, case_node->secondary_name,
                                    sizeof(case_node->secondary_name), "match variant name");
                    advance_token(parser);
                    if (match(parser, TOKEN_LPAREN)) {
                        advance_token(parser);
                        if (match(parser, TOKEN_RPAREN)) {
                            fprintf(stderr, "%s:%d:%d: error: payload pattern 'case %s.%s' requires at least one binding\n",
                                    parser->source_file, parser->current_token.line,
                                    parser->current_token.col, case_node->match_type_name,
                                    case_node->secondary_name);
                            exit(1);
                        }
                        while (true) {
                            if (!match(parser, TOKEN_IDENTIFIER)) {
                                fprintf(stderr, "%s:%d:%d: error: expected binding name in 'case %s.%s(...)'\n",
                                        parser->source_file, parser->current_token.line,
                                        parser->current_token.col, case_node->match_type_name,
                                        case_node->secondary_name);
                                exit(1);
                            }
                            Token binding_token = parser->current_token;
                            ASTNode *binding = parser_create_node_at(parser, AST_PARAM,
                                                                     parser->current_token.text,
                                                                     binding_token);
                            ast_add_child(case_node, binding);
                            advance_token(parser);
                            if (!match(parser, TOKEN_COMMA)) break;
                            advance_token(parser);
                        }
                        expect(parser, TOKEN_RPAREN, "Expected ')' after payload bindings");
                    }
                }
                if (match(parser, TOKEN_COLON)) advance_token(parser);
                ast_add_child(case_node, parse_block(parser));
                ast_add_child(match_node, case_node);
                continue;
            }
            if (match(parser, TOKEN_ELSE)) {
                Token else_token = parser->current_token;
                advance_token(parser);
                ASTNode *case_node = parser_create_node_at(parser, AST_MATCH_CASE, "else", else_token);
                case_node->is_default_case = true;
                if (match(parser, TOKEN_COLON)) advance_token(parser);
                ast_add_child(case_node, parse_block(parser));
                ast_add_child(match_node, case_node);
                continue;
            }
            fprintf(stderr, "%s:%d:%d: error: expected 'case' or 'else' in match body\n",
                    parser->source_file, parser->current_token.line, parser->current_token.col);
            exit(1);
        }
        expect(parser, TOKEN_RBRACE, "Expected '}' after match body");
        return match_node;
    }

    // assert(expr)
    if (match(parser, TOKEN_ASSERT)) {
        advance_token(parser);
        bool has_paren = match(parser, TOKEN_LPAREN);
        if (has_paren) advance_token(parser);
        ASTNode *assert_node = parser_create_node(parser, AST_ASSERT_STMT, NULL);
        ast_add_child(assert_node, parse_expression(parser));
        if (has_paren) expect(parser, TOKEN_RPAREN, "Expected ')' after assert expression");
        return assert_node;
    }

    // print(expr) or print "string"
    if (match(parser, TOKEN_PRINT)) {
        advance_token(parser);
        bool has_paren = match(parser, TOKEN_LPAREN);
        if (has_paren) advance_token(parser);

        ASTNode *print_node = parser_create_node(parser, AST_PRINT_STMT, NULL);
        ASTNode *expr = parse_expression(parser);
        ast_add_child(print_node, expr);

        if (has_paren) expect(parser, TOKEN_RPAREN, "Expected ')' after print expression");
        return print_node;
    }

    // if statement
    if (match(parser, TOKEN_IF)) {
        advance_token(parser);
        ASTNode *if_node = parser_create_node(parser, AST_IF_STMT, NULL);
        
        // Condition
        ASTNode *cond = parse_expression(parser);
        ast_add_child(if_node, cond);

        if (match(parser, TOKEN_COLON)) advance_token(parser);

        // If body
        ASTNode *then_block = parse_block(parser);
        ast_add_child(if_node, then_block);

        // Optional else block
        if (match(parser, TOKEN_ELSE)) {
            advance_token(parser);
            if (match(parser, TOKEN_COLON)) advance_token(parser);
            ASTNode *else_block = parse_block(parser);
            ast_add_child(if_node, else_block);
        }

        return if_node;
    }

    // while statement
    if (match(parser, TOKEN_WHILE)) {
        advance_token(parser);
        ASTNode *while_node = parser_create_node(parser, AST_WHILE_STMT, NULL);

        // Condition
        ASTNode *cond = parse_expression(parser);
        ast_add_child(while_node, cond);

        if (match(parser, TOKEN_COLON)) advance_token(parser);

        // Body
        ASTNode *body_block = parse_block(parser);
        ast_add_child(while_node, body_block);

        return while_node;
    }

    // heap declaration (long-lived global allocation)
    if (match(parser, TOKEN_HEAP)) {
        advance_token(parser);
        char var_name[COBRA_MAX_IDENT_LEN];
        Token variable_token = parser->current_token;
        copy_token_text(parser, var_name, sizeof(var_name), "variable name");
        expect(parser, TOKEN_IDENTIFIER, "Expected variable name after 'heap'");

        ASTNode *heap_node = parser_create_node_at(parser, AST_HEAP_DECL, var_name, variable_token);
        CobraTypeKind declared_type = COBRA_TYPE_UNTYPED;
        if (match(parser, TOKEN_COLON)) {
            advance_token(parser);
            declared_type = parse_type_into(parser, "heap declaration", heap_node, 0);
        }

        expect(parser, TOKEN_ASSIGN, "Expected '=' in heap declaration");
        heap_node->declared_type = declared_type;
        ASTNode *init_expr = parse_expression(parser);
        ast_add_child(heap_node, init_expr);


        return heap_node;
    }

    // let / var / const declaration
    if (match(parser, TOKEN_LET) || match(parser, TOKEN_VAR) || match(parser, TOKEN_CONST)) {
        bool is_const = match(parser, TOKEN_CONST);
        advance_token(parser);

        if (match(parser, TOKEN_LPAREN)) {
            /* let (a, b, ...) = expr - flat tuple destructure. No nested
               patterns, no wildcard `_`: this is intentionally scoped to
               binding every element of a tuple-typed expression to a fresh
               local name (see ROADMAP.md phase 14). */
            Token let_token = parser->current_token;
            advance_token(parser);
            char names[8][COBRA_MAX_IDENT_LEN];
            size_t name_count = 0;
            while (!match(parser, TOKEN_RPAREN)) {
                if (name_count >= 8 || !match(parser, TOKEN_IDENTIFIER)) {
                    fprintf(stderr, "%s:%d:%d: error: expected a name in tuple destructure\n",
                            parser->source_file, parser->current_token.line, parser->current_token.col);
                    exit(1);
                }
                copy_token_text(parser, names[name_count++], COBRA_MAX_IDENT_LEN, "destructure name");
                advance_token(parser);
                if (match(parser, TOKEN_COMMA)) advance_token(parser);
                else if (!match(parser, TOKEN_RPAREN)) {
                    fprintf(stderr, "%s:%d:%d: error: expected ',' or ')' in tuple destructure\n",
                            parser->source_file, parser->current_token.line, parser->current_token.col);
                    exit(1);
                }
            }
            expect(parser, TOKEN_RPAREN, "Expected ')' after tuple destructure names");
            if (name_count < 2) {
                fprintf(stderr, "%s:%d:%d: error: tuple destructure requires at least two names\n",
                        parser->source_file, parser->current_token.line, parser->current_token.col);
                exit(1);
            }
            expect(parser, TOKEN_ASSIGN, "Expected '=' in tuple destructure");
            ASTNode *rhs = parse_expression(parser);

            ASTNode *block = parser_create_node_at(parser, AST_PROGRAM, TUPLE_DESTRUCTURE_SPLICE_MARKER, let_token);
            if (rhs->type == AST_TUPLE) {
                /* Direct tuple literal RHS: no shared value to evaluate once,
                   so this is pure sugar over N independent `let`s - the type
                   system never needs to see a tuple type at all here. */
                if (rhs->child_count != name_count) {
                    fprintf(stderr, "%s:%d:%d: error: tuple destructure expects %zu values, found %zu\n",
                            parser->source_file, let_token.line, let_token.col,
                            name_count, rhs->child_count);
                    exit(1);
                }
                for (size_t i = 0; i < name_count; i++) {
                    ASTNode *decl = parser_create_node_at(parser, AST_VAR_DECL, names[i], let_token);
                    decl->is_const = is_const;
                    decl->declared_type = COBRA_TYPE_UNTYPED;
                    ast_add_child(decl, rhs->children[i]);
                    rhs->children[i] = NULL;
                    ast_add_child(block, decl);
                }
                ast_free(rhs);
            } else if (rhs->type == AST_FUNC_CALL) {
                /* Destructuring a call result requires knowing its tuple
                   struct shape up front, so this form only supports calling
                   a function already declared earlier in the source - the
                   same forward-reference limitation ordinary struct-typed
                   `let` bindings have today. */
                ASTNode *callee = NULL;
                if (parser->root) {
                    for (size_t i = 0; i < parser->root->child_count; i++) {
                        ASTNode *cand = parser->root->children[i];
                        if (cand->type == AST_FUNCTION && !strcmp(cand->name, rhs->name)) { callee = cand; break; }
                    }
                }
                if (!callee || callee->declared_type != COBRA_TYPE_STRUCT || !callee->canonical_type) {
                    fprintf(stderr, "%s:%d:%d: error: tuple destructure requires a call to a previously declared tuple-returning function\n",
                            parser->source_file, let_token.line, let_token.col);
                    exit(1);
                }
                const char *tuple_name = cobra_type_node_name(callee);
                ASTNode *found_decl = NULL;
                for (size_t i = 0; parser->root && i < parser->root->child_count; i++) {
                    ASTNode *cand = parser->root->children[i];
                    if (cand->type == AST_STRUCT_DECL && !strcmp(cand->name, tuple_name)) { found_decl = cand; break; }
                }
                if (!found_decl || found_decl->child_count != name_count) {
                    fprintf(stderr, "%s:%d:%d: error: tuple destructure expects %zu values, callee returns a different arity\n",
                            parser->source_file, let_token.line, let_token.col, name_count);
                    exit(1);
                }
                char temp_name[COBRA_MAX_IDENT_LEN];
                snprintf(temp_name, sizeof(temp_name), "__tuple_tmp_%d_%d", let_token.line, let_token.col);
                ASTNode *temp_decl = parser_create_node_at(parser, AST_VAR_DECL, temp_name, let_token);
                temp_decl->declared_type = COBRA_TYPE_STRUCT;
                temp_decl->canonical_type = callee->canonical_type;
                ast_add_child(temp_decl, rhs);
                ast_add_child(block, temp_decl);
                for (size_t i = 0; i < name_count; i++) {
                    ASTNode *access = parser_create_node_at(parser, AST_MEMBER_ACCESS, temp_name, let_token);
                    snprintf(access->secondary_name, sizeof(access->secondary_name), "_%zu", i);
                    ast_add_child(access, parser_create_node_at(parser, AST_VAR_REF, temp_name, let_token));
                    ASTNode *decl = parser_create_node_at(parser, AST_VAR_DECL, names[i], let_token);
                    decl->is_const = is_const;
                    decl->declared_type = COBRA_TYPE_UNTYPED;
                    ast_add_child(decl, access);
                    ast_add_child(block, decl);
                }
            } else {
                fprintf(stderr, "%s:%d:%d: error: tuple destructure requires a tuple literal or a tuple-returning function call\n",
                        parser->source_file, let_token.line, let_token.col);
                exit(1);
            }
            return block;
        }

        char var_name[COBRA_MAX_IDENT_LEN];
        Token variable_token = parser->current_token;
        copy_token_text(parser, var_name, sizeof(var_name), "variable name");
        expect(parser, TOKEN_IDENTIFIER, "Expected variable name");

        ASTNode *var_node = parser_create_node_at(parser, AST_VAR_DECL, var_name, variable_token);
        var_node->is_const = is_const;
        CobraTypeKind declared_type = COBRA_TYPE_UNTYPED;
        if (match(parser, TOKEN_COLON)) {
            advance_token(parser);
            int alias_qualifier = 0;
            if (match(parser, TOKEN_IDENTIFIER) &&
                (!strcmp(parser->current_token.text, "out") ||
                 !strcmp(parser->current_token.text, "readonly"))) {
                alias_qualifier = !strcmp(parser->current_token.text, "readonly") ? 1 : 2;
                advance_token(parser);
            }
            declared_type = parse_type_into(parser, "variable declaration", var_node,
                                            alias_qualifier);
        }

        var_node->declared_type = declared_type;
        if (declared_type == COBRA_TYPE_STRUCT || declared_type == COBRA_TYPE_ARRAY) {
            /* Struct and fixed-array declarations may omit the initializer;
               the slot is zero-initialized and fields or elements are
               assigned individually. */
            if (match(parser, TOKEN_ASSIGN)) {
                advance_token(parser);
                ASTNode *init_expr = parse_expression(parser);
                ast_add_child(var_node, init_expr);
            }
        } else {
            expect(parser, TOKEN_ASSIGN, "Expected '=' in variable declaration");
            ASTNode *init_expr = parse_expression(parser);
            ast_add_child(var_node, init_expr);
        }


        return var_node;
    }

    // Implicit Pythonic Assignment: x = 10
    if (match(parser, TOKEN_IDENTIFIER)) {
        Token target_token = parser->current_token;
        char id_name[COBRA_MAX_IDENT_LEN];
        copy_token_text(parser, id_name, sizeof(id_name), "identifier");

        ASTNode *primary = parse_primary(parser);
        if ((primary->type == AST_VAR_REF || primary->type == AST_ARRAY_INDEX ||
             primary->type == AST_MEMBER_ACCESS) && match(parser, TOKEN_ASSIGN)) {
            advance_token(parser); // skip '='
            ASTNode *value_expr = parse_expression(parser);
            if (primary->type == AST_MEMBER_ACCESS) {
                ASTNode *member_assign = parser_create_node_at(parser, AST_MEMBER_ASSIGN, primary->name, target_token);
                snprintf(member_assign->secondary_name, sizeof(member_assign->secondary_name), "%s", primary->secondary_name);
                if (primary->child_count > 0) {
                    ast_add_child(member_assign, primary->children[0]);
                    primary->children[0] = NULL;
                }
                ast_add_child(member_assign, value_expr);
                ast_free(primary);
                return member_assign;
            }
            if (primary->type == AST_ARRAY_INDEX) {
                ASTNode *assign_node = parser_create_node_at(parser, AST_INDEX_ASSIGN, primary->name, target_token);
                snprintf(assign_node->secondary_name, sizeof(assign_node->secondary_name),
                         "%.63s", primary->secondary_name);
                /* Transfer every index before appending the assigned value.
                   Multi-axis tensor stores must not silently drop axes. */
                for (size_t i = 0; i < primary->child_count; i++) {
                    ast_add_child(assign_node, primary->children[i]);
                    primary->children[i] = NULL;
                }
                ast_add_child(assign_node, value_expr);
                free(primary->children);
                primary->children = NULL;
                primary->child_count = 0;
                primary->child_capacity = 0;
                ast_free(primary);
                return assign_node;
            }
            ASTNode *assign_node = parser_create_node_at(parser, AST_ASSIGN, id_name, target_token);
            ast_add_child(assign_node, value_expr);
            ast_free(primary);
            return assign_node;
        }
        return primary;
    }

    if (match(parser, TOKEN_RETURN)) {
        advance_token(parser);
        ASTNode *ret_node = parser_create_node(parser, AST_RETURN, NULL);
        /* `return` directly followed by `def` is ambiguous: a brace-less
           function body's implicit end (`return` with no value, next `def`
           starts the following top-level function) vs. `return def(...)...`
           returning a closure literal. Disambiguate with one token of
           lookahead: `def(` is always a closure literal (named declarations
           require `def <identifier>`), so only swallow TOKEN_DEF as "no
           value" when it isn't immediately followed by '('. */
        bool def_starts_closure = false;
        if (match(parser, TOKEN_DEF)) {
            Lexer saved_lexer = parser->lexer;
            Token saved_token = parser->current_token;
            advance_token(parser);
            def_starts_closure = match(parser, TOKEN_LPAREN);
            parser->lexer = saved_lexer;
            parser->current_token = saved_token;
        }
        if (!match(parser, TOKEN_RBRACE) && !match(parser, TOKEN_EOF) &&
            (!match(parser, TOKEN_DEF) || def_starts_closure) && !match(parser, TOKEN_ELSE)) {
            ASTNode *expr = parse_expression(parser);
            ast_add_child(ret_node, expr);
        }
        return ret_node;
    }

    if (match(parser, TOKEN_ASM)) {
        advance_token(parser);
        ASTNode *asm_node = parser_create_node(parser, AST_ASM_BLOCK, NULL);

        /* Optional operand binding: asm(in a, b out result): { ... } loads
           named i64 locals into the fixed SysV argument registers before the
           block and stores rax into the output local afterward. */
        if (match(parser, TOKEN_LPAREN)) {
            advance_token(parser);
            if (match(parser, TOKEN_IN)) {
                advance_token(parser);
                while (!match(parser, TOKEN_RPAREN) && !match(parser, TOKEN_EOF) &&
                       strcmp(parser->current_token.text, "out") != 0) {
                    if (asm_node->asm_input_count >= 6) {
                        fprintf(stderr, "%s:%d:%d: error: inline asm supports at most 6 input operands\n",
                                parser->source_file, parser->current_token.line, parser->current_token.col);
                        exit(1);
                    }
                    snprintf(asm_node->asm_inputs[asm_node->asm_input_count],
                             sizeof(asm_node->asm_inputs[0]), "%.63s", parser->current_token.text);
                    asm_node->asm_input_count++;
                    advance_token(parser);
                    if (match(parser, TOKEN_COMMA)) advance_token(parser);
                }
            }
            if (!match(parser, TOKEN_RPAREN) && strcmp(parser->current_token.text, "out") == 0) {
                advance_token(parser);
                if (!match(parser, TOKEN_RPAREN) && !match(parser, TOKEN_EOF)) {
                    snprintf(asm_node->asm_output, sizeof(asm_node->asm_output),
                             "%.63s", parser->current_token.text);
                    asm_node->asm_has_output = true;
                    advance_token(parser);
                }
            }
            expect(parser, TOKEN_RPAREN, "Expected ')' after asm operand list");
        }
        expect(parser, TOKEN_COLON, "Expected ':' after 'asm'");

        if (match(parser, TOKEN_LBRACE)) {
            advance_token(parser); // Skip {
            char buf[512] = "";
            int line = parser->current_token.line;

            while (!match(parser, TOKEN_RBRACE) && !match(parser, TOKEN_EOF)) {
                size_t used = strlen(buf);
                const char *separator = "";
                if (parser->current_token.line > line) {
                    separator = "\n    ";
                    line = parser->current_token.line;
                } else if (used > 0 && buf[used - 1] != '\n' && buf[used - 1] != ' ' &&
                           strcmp(parser->current_token.text, ",") != 0) {
                    separator = " ";
                }
                size_t needed = strlen(separator) + strlen(parser->current_token.text);
                if (used + needed >= sizeof(buf)) {
                    fprintf(stderr, "%s:%d:%d: error: inline asm exceeds %zu bytes\n",
                            parser->source_file, parser->current_token.line, parser->current_token.col, sizeof(buf) - 1);
                    exit(1);
                }
                memcpy(buf + used, separator, strlen(separator));
                used += strlen(separator);
                memcpy(buf + used, parser->current_token.text, strlen(parser->current_token.text) + 1);
                advance_token(parser);
            }
            expect(parser, TOKEN_RBRACE, "Expected '}' after asm block");
            snprintf(asm_node->asm_code, sizeof(asm_node->asm_code), "%s", buf);
        } else {
            snprintf(asm_node->asm_code, sizeof(asm_node->asm_code), "%s", parser->current_token.text);
            advance_token(parser);
        }
        return asm_node;
    }

    return parse_expression(parser);
}

static void parser_append_statement(ASTNode *block, ASTNode *stmt) {
    if (!stmt) return;
    if (stmt->type == AST_PROGRAM && !strcmp(stmt->name, TUPLE_DESTRUCTURE_SPLICE_MARKER)) {
        for (size_t i = 0; i < stmt->child_count; i++) ast_add_child(block, stmt->children[i]);
        stmt->children = NULL;
        stmt->child_count = 0;
        stmt->child_capacity = 0;
        ast_free(stmt);
        return;
    }
    ast_add_child(block, stmt);
}

static ASTNode *parse_block(Parser *parser) {
    ASTNode *block = parser_create_node(parser, AST_PROGRAM, "Block");

    if (match(parser, TOKEN_LBRACE)) {
        advance_token(parser);
        while (!match(parser, TOKEN_RBRACE) && !match(parser, TOKEN_EOF)) {
            parser_append_statement(block, parse_statement(parser));
        }
        expect(parser, TOKEN_RBRACE, "Expected '}' after block");
    } else {
        // Single statement or indented line
        parser_append_statement(block, parse_statement(parser));
    }

    return block;
}

/* enum Status: { idle, ready, failed } */
static ASTNode *parse_enum_declaration(Parser *parser) {
    advance_token(parser); /* skip enum */
    if (!match(parser, TOKEN_IDENTIFIER)) {
        fprintf(stderr, "%s:%d:%d: error: expected enum name after 'enum'\n",
                parser->source_file, parser->current_token.line, parser->current_token.col);
        exit(1);
    }
    Token name_token = parser->current_token;
    char enum_name[COBRA_MAX_IDENT_LEN];
    copy_token_text(parser, enum_name, sizeof(enum_name), "enum name");
    advance_token(parser);
    if (match(parser, TOKEN_COLON)) advance_token(parser);
    expect(parser, TOKEN_LBRACE, "Expected '{' after enum name");

    ASTNode *enum_node = parser_create_node_at(parser, AST_ENUM_DECL, enum_name, name_token);
    enum_node->declared_type = COBRA_TYPE_ENUM;
    enum_node->canonical_type = parser_component_type(parser, COBRA_TYPE_ENUM, enum_name);
    int next_value = 0;
    while (!match(parser, TOKEN_RBRACE) && !match(parser, TOKEN_EOF)) {
        if (!match(parser, TOKEN_IDENTIFIER)) {
            fprintf(stderr, "%s:%d:%d: error: expected enum variant name in '%s'\n",
                    parser->source_file, parser->current_token.line, parser->current_token.col, enum_name);
            exit(1);
        }
        Token variant_token = parser->current_token;
        ASTNode *variant = parser_create_node_at(parser, AST_PARAM, parser->current_token.text, variant_token);
        variant->declared_type = COBRA_TYPE_I32;
        variant->int_val = next_value;
        advance_token(parser);
        /* Payload-carrying variant: enum Shape: { Circle(f32), Rect(f32, f32) }.
           The payload types become child AST_PARAM nodes of the variant, so a
           non-zero child_count marks a payload variant. The production
           compiler rejects payload variants at IR validation; the isolated
           backend lowers them through the tagged-sum machinery. */
        if (match(parser, TOKEN_LPAREN)) {
            advance_token(parser);
            if (match(parser, TOKEN_RPAREN)) {
                fprintf(stderr, "%s:%d:%d: error: enum variant '%s' requires at least one payload type\n",
                        parser->source_file, variant_token.line, variant_token.col, variant->name);
                exit(1);
            }
            int payload_index = 0;
            while (true) {
                char payload_name[COBRA_MAX_IDENT_LEN];
                snprintf(payload_name, sizeof(payload_name), "payload%d", payload_index);
                ASTNode *payload = parser_create_node_at(parser, AST_PARAM,
                                                         payload_name, parser->current_token);
                payload->declared_type = parse_type_into(parser, "enum variant payload",
                                                         payload, 0);
                if (payload->declared_type == COBRA_TYPE_UNTYPED ||
                    payload->declared_type == COBRA_TYPE_UNKNOWN ||
                    !payload->canonical_type) {
                    fprintf(stderr, "%s:%d:%d: error: invalid payload type in enum variant '%s'\n",
                            parser->source_file, variant_token.line, variant_token.col,
                            variant->name);
                    exit(1);
                }
                ast_add_child(variant, payload);
                payload_index++;
                if (!match(parser, TOKEN_COMMA)) break;
                advance_token(parser);
            }
            expect(parser, TOKEN_RPAREN, "Expected ')' after enum variant payload types");
        }
        if (match(parser, TOKEN_ASSIGN)) {
            advance_token(parser);
            if (!match(parser, TOKEN_INT_LITERAL)) {
                fprintf(stderr, "%s:%d:%d: error: enum values must be integer literals\n",
                        parser->source_file, parser->current_token.line, parser->current_token.col);
                exit(1);
            }
            variant->int_val = parse_int_literal(parser);
            next_value = variant->int_val;
            advance_token(parser);
        }
        next_value = variant->int_val + 1;
        ast_add_child(enum_node, variant);
        if (match(parser, TOKEN_COMMA)) advance_token(parser);
    }
    expect(parser, TOKEN_RBRACE, "Expected '}' after enum variants");
    if (enum_node->child_count == 0) {
        fprintf(stderr, "%s:%d:%d: error: enum '%s' must declare at least one variant\n",
                parser->source_file, name_token.line, name_token.col, enum_name);
        exit(1);
    }
    return enum_node;
}

/* struct Point: { x: i64, y: i64 } */
static ASTNode *parse_struct_declaration(Parser *parser) {
    advance_token(parser); // skip 'struct'
    if (!match(parser, TOKEN_IDENTIFIER)) {
        fprintf(stderr, "%s:%d:%d: error: expected struct name after 'struct'\n",
                parser->source_file, parser->current_token.line, parser->current_token.col);
        exit(1);
    }
    Token name_token = parser->current_token;
    char struct_name[COBRA_MAX_IDENT_LEN];
    copy_token_text(parser, struct_name, sizeof(struct_name), "struct name");
    advance_token(parser);

    size_t previous_generic_count = parser->generic_param_count;
    parser->generic_param_count = 0;
    if (match(parser, TOKEN_LBRACKET)) {
        advance_token(parser);
        while (!match(parser, TOKEN_RBRACKET)) {
            if (parser->generic_param_count >= 1 ||
                !match(parser, TOKEN_IDENTIFIER)) {
                fprintf(stderr, "%s:%d:%d: error: generic structs currently require exactly one identifier type parameter\n",
                        parser->source_file, parser->current_token.line, parser->current_token.col);
                exit(1);
            }
            const char *parameter_name = parser->current_token.text;
            size_t index = parser->generic_param_count++;
            snprintf(parser->generic_param_names[index], COBRA_MAX_IDENT_LEN, "%.63s", parameter_name);
            parser->generic_param_types[index] = cobra_type_make(parser->canonical_arena,
                                                                  COBRA_TYPE_GENERIC_PARAM,
                                                                  parameter_name, NULL, NULL, NULL, NULL,
                                                                  COBRA_OWNERSHIP_VALUE,
                                                                  COBRA_MUTABILITY_DEFAULT, -1);
            advance_token(parser);
            if (match(parser, TOKEN_COMMA)) advance_token(parser);
            else if (!match(parser, TOKEN_RBRACKET)) {
                fprintf(stderr, "%s:%d:%d: error: expected ',' or ']' after generic struct parameter\n",
                        parser->source_file, parser->current_token.line, parser->current_token.col);
                exit(1);
            }
        }
        expect(parser, TOKEN_RBRACKET, "Expected ']' after generic struct parameters");
    }

    if (match(parser, TOKEN_COLON)) advance_token(parser);
    expect(parser, TOKEN_LBRACE, "Expected '{' after struct name");

    ASTNode *struct_node = parser_create_node_at(parser, AST_STRUCT_DECL, struct_name, name_token);
    struct_node->declared_type = COBRA_TYPE_STRUCT;
    struct_node->generic_param_count = parser->generic_param_count;
    for (size_t i = 0; i < parser->generic_param_count; i++) {
        snprintf(struct_node->generic_param_names[i], COBRA_MAX_IDENT_LEN, "%.63s",
                 parser->generic_param_names[i]);
        struct_node->generic_param_types[i] = parser->generic_param_types[i];
    }
    struct_node->canonical_type = parser->generic_param_count == 1
        ? cobra_type_make(parser->canonical_arena, COBRA_TYPE_STRUCT, struct_name,
                          parser->generic_param_types[0], NULL, NULL, NULL,
                          COBRA_OWNERSHIP_VALUE, COBRA_MUTABILITY_DEFAULT, -1)
        : parser_component_type(parser, COBRA_TYPE_STRUCT, struct_name);
    while (!match(parser, TOKEN_RBRACE)) {
        if (!match(parser, TOKEN_IDENTIFIER)) {
            fprintf(stderr, "%s:%d:%d: error: expected field name in struct '%s'\n",
                    parser->source_file, parser->current_token.line, parser->current_token.col, struct_name);
            exit(1);
        }
        char field_name[COBRA_MAX_IDENT_LEN];
        Token field_token = parser->current_token;
        copy_token_text(parser, field_name, sizeof(field_name), "field name");
        advance_token(parser);
        expect(parser, TOKEN_COLON, "Expected ':' after field name");
        ASTNode *field = parser_create_node_at(parser, AST_PARAM, field_name, field_token);
        /* Borrowed byte fields must declare their mutability contract at the
           field site. `readonly` is the normal request-view form; `out` is the
           explicit writable form. */
        int field_qualifier = 0;
        if (match(parser, TOKEN_IDENTIFIER) &&
            (!strcmp(parser->current_token.text, "readonly") ||
             !strcmp(parser->current_token.text, "out"))) {
            field_qualifier = !strcmp(parser->current_token.text, "readonly") ? 1 : 2;
            advance_token(parser);
        }
        field->declared_type = parse_type_into(parser, "struct field", field, field_qualifier);
        /* Struct fields use ownership to distinguish a borrowed writable view
           from an owned value (see direct_struct_field_supported in ir.c and
           the struct slice field check in type.c); function/local `out`
           parameters key off mutability alone and must not be touched here,
           so this stays local to struct field parsing. */
        if (field_qualifier == 2 && field->canonical_type)
            ((CobraType *)field->canonical_type)->ownership = COBRA_OWNERSHIP_BORROWED;

        ast_add_child(struct_node, field);
        if (match(parser, TOKEN_COMMA)) advance_token(parser);
    }
    expect(parser, TOKEN_RBRACE, "Expected '}' after struct fields");
    parser->generic_param_count = previous_generic_count;
    return struct_node;
}

/* def foo[](...) (empty brackets, no named type parameter) marks a parameter
   left without a `: type` annotation as implicitly generic instead of the
   ordinary bare-parameter error: it gets the same COBRA_TYPE_GENERIC_PARAM
   placeholder an explicit `x: T` would, so it flows through the existing
   specialize_generic_function/specialize_ast_tree monomorphization pipeline
   unchanged. Each omitted parameter gets its own independent generic slot
   (no unification between distinct parameter names), up to
   COBRA_MAX_TYPE_ARGS. */
static void assign_implicit_generic_param(Parser *parser, ASTNode *fn_node, ASTNode *param,
                                          bool is_implicit_generic_fn) {
    if (!is_implicit_generic_fn) return;
    if (fn_node->generic_param_count >= COBRA_MAX_TYPE_ARGS) {
        fprintf(stderr, "%s:%d:%d: error: too many inferred parameters (max %d)\n",
                parser->source_file, parser->current_token.line, parser->current_token.col,
                COBRA_MAX_TYPE_ARGS);
        exit(1);
    }
    size_t index = fn_node->generic_param_count++;
    char slot_name[COBRA_MAX_IDENT_LEN];
    snprintf(slot_name, sizeof(slot_name), "__auto%zu", index);
    const CobraType *slot_type = cobra_type_make(parser->canonical_arena, COBRA_TYPE_GENERIC_PARAM,
                                             slot_name, NULL, NULL, NULL, NULL,
                                             COBRA_OWNERSHIP_VALUE, COBRA_MUTABILITY_DEFAULT, -1);
    param->declared_type = COBRA_TYPE_GENERIC_PARAM;
    param->canonical_type = slot_type;
    snprintf(fn_node->generic_param_names[index], COBRA_MAX_IDENT_LEN, "%s", slot_name);
    fn_node->generic_param_types[index] = slot_type;
}

/* Shared by named `def foo(...)` declarations and anonymous closure
   literals: parses generics, parameters, return type, and body onto an
   already-created (named or synthesized) AST_FUNCTION node. */
static ASTNode *parse_function_signature_and_body(Parser *parser, ASTNode *fn_node) {
    size_t previous_generic_count = parser->generic_param_count;
    parser->generic_param_count = 0;
    bool saw_generic_brackets = false;
    if (match(parser, TOKEN_LBRACKET)) {
        saw_generic_brackets = true;
        advance_token(parser);
        while (!match(parser, TOKEN_RBRACKET)) {
            if (parser->generic_param_count >= 1 ||
                !match(parser, TOKEN_IDENTIFIER)) {
                fprintf(stderr, "%s:%d:%d: error: generic functions currently require exactly one identifier type parameter\n",
                        parser->source_file, parser->current_token.line, parser->current_token.col);
                exit(1);
            }
            const char *name = parser->current_token.text;
            for (size_t i = 0; i < parser->generic_param_count; i++) {
                if (strcmp(parser->generic_param_names[i], name) == 0) {
                    fprintf(stderr, "%s:%d:%d: error: duplicate generic parameter '%s'\n",
                            parser->source_file, parser->current_token.line,
                            parser->current_token.col, name);
                    exit(1);
                }
            }
            size_t index = parser->generic_param_count++;
            snprintf(parser->generic_param_names[index], COBRA_MAX_IDENT_LEN, "%.63s", name);
            parser->generic_param_types[index] = cobra_type_make(parser->canonical_arena,
                                                                  COBRA_TYPE_GENERIC_PARAM,
                                                                  name, NULL, NULL, NULL, NULL,
                                                                  COBRA_OWNERSHIP_VALUE,
                                                                  COBRA_MUTABILITY_DEFAULT, -1);
            snprintf(fn_node->generic_param_names[index], COBRA_MAX_IDENT_LEN, "%.63s", name);
            fn_node->generic_param_types[index] = parser->generic_param_types[index];
            advance_token(parser);
            if (match(parser, TOKEN_COMMA)) advance_token(parser);
            else if (!match(parser, TOKEN_RBRACKET)) {
                fprintf(stderr, "%s:%d:%d: error: expected ',' or ']' after generic parameter\n",
                        parser->source_file, parser->current_token.line, parser->current_token.col);
                exit(1);
            }
        }
        fn_node->generic_param_count = parser->generic_param_count;
        expect(parser, TOKEN_RBRACKET, "Expected ']' after generic parameters");
    }

    expect(parser, TOKEN_LPAREN, "Expected '(' after function name");
    
    // Parse parameters: def foo(a: i64, b: i64) -> i64:
    // Alias contracts use contextual qualifiers (out/readonly) after the
    // colon, so existing code that names a variable `out` keeps working.
    bool is_implicit_generic_fn = saw_generic_brackets && parser->generic_param_count == 0;
    if (!match(parser, TOKEN_RPAREN)) {
        if (match(parser, TOKEN_IDENTIFIER)) {
            ASTNode *param = parser_create_node(parser, AST_PARAM, parser->current_token.text);
            ast_add_child(fn_node, param);
            advance_token(parser);

            // Optional type annotation: a: i64  or  a: readonly []f32
            if (match(parser, TOKEN_COLON)) {
                advance_token(parser);
                int alias_qualifier = 0;
                if (match(parser, TOKEN_IDENTIFIER) &&
                    (!strcmp(parser->current_token.text, "out") ||
                     !strcmp(parser->current_token.text, "readonly"))) {
                    alias_qualifier = !strcmp(parser->current_token.text, "readonly") ? 1 : 2;
                    advance_token(parser);
                }
                param->declared_type = parse_type_into(parser, "parameter", param, alias_qualifier);

            } else {
                assign_implicit_generic_param(parser, fn_node, param, is_implicit_generic_fn);
            }

            while (match(parser, TOKEN_COMMA)) {
                advance_token(parser);
                if (match(parser, TOKEN_IDENTIFIER)) {
                    ASTNode *p = parser_create_node(parser, AST_PARAM, parser->current_token.text);
                    ast_add_child(fn_node, p);
                    advance_token(parser);

                    if (match(parser, TOKEN_COLON)) {
                        advance_token(parser);
                        int alias_qualifier = 0;
                        if (match(parser, TOKEN_IDENTIFIER) &&
                            (!strcmp(parser->current_token.text, "out") ||
                             !strcmp(parser->current_token.text, "readonly"))) {
                            alias_qualifier = !strcmp(parser->current_token.text, "readonly") ? 1 : 2;
                            advance_token(parser);
                        }
                        p->declared_type = parse_type_into(parser, "parameter", p, alias_qualifier);

                    } else {
                        assign_implicit_generic_param(parser, fn_node, p, is_implicit_generic_fn);
                    }
                }
            }
        }
    }

    expect(parser, TOKEN_RPAREN, "Expected ')' after function parameters");

    if (match(parser, TOKEN_ARROW)) {
        advance_token(parser);
        int return_qualifier = 0;
        if (match(parser, TOKEN_IDENTIFIER) &&
            (!strcmp(parser->current_token.text, "out") ||
             !strcmp(parser->current_token.text, "readonly"))) {
            return_qualifier = !strcmp(parser->current_token.text, "readonly") ? 1 : 2;
            advance_token(parser);
        }
        fn_node->declared_type = parse_type_into(parser, "return", fn_node,
                                                 return_qualifier);

    }

    if (match(parser, TOKEN_COLON)) {
        advance_token(parser);
    }

    if (match(parser, TOKEN_LBRACE)) {
        advance_token(parser);
        while (!match(parser, TOKEN_RBRACE) && !match(parser, TOKEN_EOF)) {
            parser_append_statement(fn_node, parse_statement(parser));
        }
        expect(parser, TOKEN_RBRACE, "Expected '}' at end of function block");
    } else {
        while (!match(parser, TOKEN_EOF) && !match(parser, TOKEN_DEF)) {
            parser_append_statement(fn_node, parse_statement(parser));
        }
    }

    parser->generic_param_count = previous_generic_count;
    return fn_node;
}

static ASTNode *parse_function(Parser *parser) {
    expect(parser, TOKEN_DEF, "Expected 'def' to start function declaration");

    char fn_name[COBRA_MAX_IDENT_LEN];
    Token function_token = parser->current_token;
    copy_token_text(parser, fn_name, sizeof(fn_name), "function name");
    expect(parser, TOKEN_IDENTIFIER, "Expected function name");

    ASTNode *fn_node = parser_create_node_at(parser, AST_FUNCTION, fn_name, function_token);
    if (parser->gpu_directive_active) fn_node->target_device = TARGET_DEV_GPU_KERNEL;

    if (parser->enclosing_depth < 8)
        snprintf(parser->enclosing_stack[parser->enclosing_depth++], COBRA_MAX_IDENT_LEN, "%.63s", fn_name);
    ASTNode *result = parse_function_signature_and_body(parser, fn_node);
    if (parser->enclosing_depth > 0) parser->enclosing_depth--;
    return result;
}

/* Anonymous closure literal in expression position: `def(params) -> ret: { body }`.
   Registered immediately as an ordinary synthesized top-level function (parsing
   finishes fully before cobra_ir_build ever runs, so there is no re-entrancy
   risk), and the literal evaluates to a var-reference naming it. Every
   closure gets an implicit leading `__env` (i64) parameter, whether or not it
   ends up capturing anything - it rides the same {code_ptr, env_ptr} thunk
   convention as plain function values (see ensure_fn_thunk in codegen.c), so
   there is one uniform indirect-call ABI instead of a bifurcated one. Capture
   analysis and the read rewrite to AST_ENV_FIELD_LOAD happen later in ir.c,
   once every function's parameter list is known; only scalar captures of the
   immediately-enclosing named function are supported (see
   collect_closure_captures in ir.c). enclosing_function records that
   function's name so ir.c can look up its parameters. */
static ASTNode *parse_closure_literal(Parser *parser) {
    Token def_token = parser->current_token;
    expect(parser, TOKEN_DEF, "Expected 'def' to start closure literal");

    char closure_name[COBRA_MAX_IDENT_LEN];
    snprintf(closure_name, sizeof(closure_name), "__closure_%d", parser->closure_counter++);

    ASTNode *fn_node = parser_create_node_at(parser, AST_FUNCTION, closure_name, def_token);
    if (parser->gpu_directive_active) fn_node->target_device = TARGET_DEV_GPU_KERNEL;
    fn_node->is_closure = true;
    if (parser->enclosing_depth > 0)
        snprintf(fn_node->enclosing_function, COBRA_MAX_IDENT_LEN, "%.63s", parser->enclosing_stack[parser->enclosing_depth - 1]);

    ASTNode *env_param = parser_create_node_at(parser, AST_PARAM, "__env", def_token);
    env_param->declared_type = COBRA_TYPE_I64;
    parser_set_canonical(parser, env_param, COBRA_TYPE_I64, 0, NULL, NULL, NULL, NULL);
    ast_add_child(fn_node, env_param);

    if (parser->enclosing_depth < 8)
        snprintf(parser->enclosing_stack[parser->enclosing_depth++], COBRA_MAX_IDENT_LEN, "%.63s", closure_name);
    parse_function_signature_and_body(parser, fn_node);
    if (parser->enclosing_depth > 0) parser->enclosing_depth--;

    if (parser->root) ast_add_child(parser->root, fn_node);

    ASTNode *ref = parser_create_node_at(parser, AST_VAR_REF, closure_name, def_token);
    ref->is_closure_instance = true;
    return ref;
}

/* trait Name: { def method(params) -> ret  def method2(...) -> ret2 } --
   signature-only method list, no bodies. Each method becomes an AST_FUNCTION
   node with AST_PARAM children and declared_type set to the return type;
   child_count beyond the params is zero (no body statements), which is how
   ir.c's conformance pass tells a signature apart from a real function. */
static ASTNode *parse_trait_method_signature(Parser *parser) {
    Token def_token = parser->current_token;
    expect(parser, TOKEN_DEF, "Expected 'def' to start trait method signature");
    char method_name[COBRA_MAX_IDENT_LEN];
    copy_token_text(parser, method_name, sizeof(method_name), "trait method name");
    expect(parser, TOKEN_IDENTIFIER, "Expected trait method name");
    ASTNode *sig = parser_create_node_at(parser, AST_FUNCTION, method_name, def_token);
    expect(parser, TOKEN_LPAREN, "Expected '(' after trait method name");
    if (!match(parser, TOKEN_RPAREN)) {
        for (;;) {
            if (!match(parser, TOKEN_IDENTIFIER)) {
                fprintf(stderr, "%s:%d:%d: error: expected parameter name in trait method signature\n",
                        parser->source_file, parser->current_token.line, parser->current_token.col);
                exit(1);
            }
            ASTNode *param = parser_create_node(parser, AST_PARAM, parser->current_token.text);
            ast_add_child(sig, param);
            advance_token(parser);
            if (match(parser, TOKEN_COLON)) {
                advance_token(parser);
                int alias_qualifier = 0;
                if (match(parser, TOKEN_IDENTIFIER) &&
                    (!strcmp(parser->current_token.text, "out") ||
                     !strcmp(parser->current_token.text, "readonly"))) {
                    alias_qualifier = !strcmp(parser->current_token.text, "readonly") ? 1 : 2;
                    advance_token(parser);
                }
                param->declared_type = parse_type_into(parser, "parameter", param, alias_qualifier);
            }
            if (match(parser, TOKEN_COMMA)) { advance_token(parser); continue; }
            break;
        }
    }
    expect(parser, TOKEN_RPAREN, "Expected ')' after trait method parameters");
    if (match(parser, TOKEN_ARROW)) {
        advance_token(parser);
        sig->declared_type = parse_type_into(parser, "return", sig, 0);
    }
    /* Optional default body: def name(...) -> ret: { body }. A default body
       may not reference a receiver (the trait signature never names one -
       each impl's method declares its own receiver param independently), so
       it is restricted to receiver-independent logic. Body statements are
       appended as children after the AST_PARAM children; a signature-only
       method keeps child_count == its param count, which is how callers
       distinguish "has a default" from "signature only" without a separate
       flag. */
    if (match(parser, TOKEN_COLON)) {
        advance_token(parser);
        expect(parser, TOKEN_LBRACE, "Expected '{' to start trait default method body");
        while (!match(parser, TOKEN_RBRACE) && !match(parser, TOKEN_EOF)) {
            ast_add_child(sig, parse_statement(parser));
        }
        expect(parser, TOKEN_RBRACE, "Expected '}' to close trait default method body");
    }
    return sig;
}

static ASTNode *parse_trait_declaration(Parser *parser) {
    Token trait_token = parser->current_token;
    expect(parser, TOKEN_TRAIT, "Expected 'trait'");
    char trait_name[COBRA_MAX_IDENT_LEN];
    copy_token_text(parser, trait_name, sizeof(trait_name), "trait name");
    expect(parser, TOKEN_IDENTIFIER, "Expected trait name");
    ASTNode *trait_node = parser_create_node_at(parser, AST_TRAIT_DECL, trait_name, trait_token);
    expect(parser, TOKEN_COLON, "Expected ':' after trait name");
    /* Optional supertrait: trait Drawable: Shape: { ... }. An identifier
       before the body's opening brace names a supertrait that any type
       implementing this trait must also satisfy; stashed on secondary_name
       so ir.c's conformance pass can chase the chain without a new field. */
    if (match(parser, TOKEN_IDENTIFIER)) {
        copy_token_text(parser, trait_node->secondary_name, sizeof(trait_node->secondary_name), "supertrait name");
        advance_token(parser);
        expect(parser, TOKEN_COLON, "Expected ':' after supertrait name");
    }
    expect(parser, TOKEN_LBRACE, "Expected '{' to start trait body");
    while (!match(parser, TOKEN_RBRACE) && !match(parser, TOKEN_EOF)) {
        ast_add_child(trait_node, parse_trait_method_signature(parser));
    }
    expect(parser, TOKEN_RBRACE, "Expected '}' to close trait body");
    return trait_node;
}

/* impl Name for Type: { def method(params) -> ret: { body } ... } -- each
   method is registered directly into parser->root as a real top-level
   function named __impl_<Trait>_<Type>_<method>, exactly like closure
   literals are (parsing finishes fully before cobra_ir_build runs, so
   there's no re-entrancy risk). The returned AST_IMPL_DECL is bookkeeping
   only: its children name the implemented methods (name) and their mangled
   target (secondary_name), so ir.c can check every trait method got an impl
   without re-deriving the mangling scheme. */
static ASTNode *parse_impl_declaration(Parser *parser) {
    Token impl_token = parser->current_token;
    expect(parser, TOKEN_IMPL, "Expected 'impl'");
    char first_name[COBRA_MAX_IDENT_LEN];
    copy_token_text(parser, first_name, sizeof(first_name), "trait or type name");
    expect(parser, TOKEN_IDENTIFIER, "Expected trait or type name after 'impl'");

    /* `impl Trait for Type: {...}` vs plain `impl Type: {...}` (no trait, a
       bare method block on a struct) are told apart by whether 'for'
       follows. Plain impls store an empty name as the sentinel meaning "no
       trait" - real trait names come from TOKEN_IDENTIFIER text and can
       never be empty, so this can't collide with a real trait-qualified
       mangled name (see find_impl_method / the conformance pass skip). */
    char trait_name[COBRA_MAX_IDENT_LEN];
    char type_name[COBRA_MAX_IDENT_LEN];
    if (match(parser, TOKEN_FOR)) {
        advance_token(parser);
        snprintf(trait_name, sizeof(trait_name), "%.63s", first_name);
        copy_token_text(parser, type_name, sizeof(type_name), "implementing type name");
        expect(parser, TOKEN_IDENTIFIER, "Expected implementing type name after 'for'");
    } else {
        trait_name[0] = '\0';
        snprintf(type_name, sizeof(type_name), "%.63s", first_name);
    }

    ASTNode *impl_node = parser_create_node_at(parser, AST_IMPL_DECL, trait_name, impl_token);
    snprintf(impl_node->secondary_name, sizeof(impl_node->secondary_name), "%.63s", type_name);

    expect(parser, TOKEN_COLON, "Expected ':' after impl header");
    expect(parser, TOKEN_LBRACE, "Expected '{' to start impl body");
    while (!match(parser, TOKEN_RBRACE) && !match(parser, TOKEN_EOF)) {
        Token def_token = parser->current_token;
        expect(parser, TOKEN_DEF, "Expected 'def' to start impl method");
        char method_name[COBRA_MAX_IDENT_LEN];
        copy_token_text(parser, method_name, sizeof(method_name), "impl method name");
        expect(parser, TOKEN_IDENTIFIER, "Expected impl method name");

        char mangled_name[COBRA_MAX_IDENT_LEN];
        snprintf(mangled_name, sizeof(mangled_name), "__impl_%.31s_%.31s_%.31s",
                 trait_name, type_name, method_name);

        ASTNode *fn_node = parser_create_node_at(parser, AST_FUNCTION, mangled_name, def_token);
        parse_function_signature_and_body(parser, fn_node);
        if (parser->root) ast_add_child(parser->root, fn_node);

        ASTNode *marker = parser_create_node_at(parser, AST_PARAM, method_name, def_token);
        snprintf(marker->secondary_name, sizeof(marker->secondary_name), "%.63s", mangled_name);
        ast_add_child(impl_node, marker);
    }
    expect(parser, TOKEN_RBRACE, "Expected '}' to close impl body");
    return impl_node;
}

/* Deep-clone an AST subtree. Mirrors src/ir.c's generic-specialization
   clone_ast_tree exactly (small enough, and static in that file, to just
   duplicate rather than expose across translation units). */
static ASTNode *parser_clone_ast_tree(const ASTNode *source) {
    if (!source) return NULL;
    ASTNode *copy = ast_create_node(source->type, source->name);
    if (!copy) return NULL;
    *copy = *source;
    copy->children = NULL;
    copy->child_count = 0;
    copy->child_capacity = 0;
    for (size_t i = 0; i < source->child_count; i++) {
        ASTNode *child = parser_clone_ast_tree(source->children[i]);
        if (!child) return NULL;
        ast_add_child(copy, child);
    }
    return copy;
}

/* Trait default methods: for every impl block that omits a method the trait
   declares WITH a default body (child_count beyond its AST_PARAM children,
   per parse_trait_method_signature), synthesize the missing
   __impl_<Trait>_<Type>_<method> function by cloning the default body and
   prepending a receiver parameter typed as the implementing struct - exactly
   the shape a hand-written impl method already has, so no other pass needs
   to know a default was involved. Runs once, after the whole program has
   parsed (so every trait/impl is known regardless of declaration order),
   before cobra_ir_build's per-function loop ever starts. */
static void synthesize_trait_defaults(Parser *parser, ASTNode *root) {
    if (!parser || !root) return;
    for (size_t i = 0; i < root->child_count; i++) {
        ASTNode *impl = root->children[i];
        if (impl->type != AST_IMPL_DECL) continue;
        ASTNode *trait = NULL;
        for (size_t j = 0; j < root->child_count; j++) {
            if (root->children[j]->type == AST_TRAIT_DECL && strcmp(root->children[j]->name, impl->name) == 0) {
                trait = root->children[j];
                break;
            }
        }
        if (!trait) continue;
        for (size_t m = 0; m < trait->child_count; m++) {
            ASTNode *method = trait->children[m];
            size_t param_count = 0;
            while (param_count < method->child_count && method->children[param_count]->type == AST_PARAM) param_count++;
            bool has_default = method->child_count > param_count;
            if (!has_default) continue;

            bool already_implemented = false;
            for (size_t k = 0; k < impl->child_count; k++) {
                if (strcmp(impl->children[k]->name, method->name) == 0) { already_implemented = true; break; }
            }
            if (already_implemented) continue;

            char mangled_name[COBRA_MAX_IDENT_LEN];
            snprintf(mangled_name, sizeof(mangled_name), "__impl_%.31s_%.31s_%.31s",
                     impl->name, impl->secondary_name, method->name);

            ASTNode *fn_node = parser_clone_ast_tree(method);
            if (!fn_node) continue;
            snprintf(fn_node->name, sizeof(fn_node->name), "%.63s", mangled_name);

            ASTNode *receiver = ast_create_node(AST_PARAM, "__self");
            receiver->declared_type = COBRA_TYPE_STRUCT;
            receiver->canonical_type = parser_component_type(parser, COBRA_TYPE_STRUCT, impl->secondary_name);
            /* Prepend: shift every existing child right one slot, then place
               the receiver first, matching the {receiver, ...params, ...body}
               shape find_impl_method's callers already build at call sites. */
            ast_add_child(fn_node, receiver);
            for (size_t shift = fn_node->child_count - 1; shift > 0; shift--)
                fn_node->children[shift] = fn_node->children[shift - 1];
            fn_node->children[0] = receiver;

            ast_add_child(root, fn_node);

            ASTNode *marker = ast_create_node(AST_PARAM, method->name);
            snprintf(marker->secondary_name, sizeof(marker->secondary_name), "%.63s", mangled_name);
            ast_add_child(impl, marker);
        }
    }
}

ASTNode *parser_parse_program(Parser *parser) {
    ASTNode *root = parser_create_node(parser, AST_PROGRAM, "RootProgram");
    root->canonical_arena = (CobraTypeArena *)calloc(1, sizeof(CobraTypeArena));
    if (!root->canonical_arena) {
        fprintf(stderr, "%s: error: failed to allocate canonical type arena\n", parser->source_file);
        exit(EXIT_FAILURE);
    }
    cobra_type_arena_init(root->canonical_arena);
    parser->canonical_arena = root->canonical_arena;
    parser->root = root;
    while (!match(parser, TOKEN_EOF)) {
        if (match(parser, TOKEN_GPU_DIRECTIVE)) {
            advance_token(parser);
            parser->gpu_directive_active = true;
            continue;
        }
        if (match(parser, TOKEN_CPU_DIRECTIVE)) {
            advance_token(parser);
            parser->gpu_directive_active = false;
            continue;
        }
        if (match(parser, TOKEN_UNKNOWN)) {
            fprintf(stderr, "%s:%d:%d: error: unexpected token '%s'\n",
                    parser->source_file, parser->current_token.line, parser->current_token.col, parser->current_token.text);
            exit(1);
        }
        if (match(parser, TOKEN_DEF)) {
            ast_add_child(root, parse_function(parser));
        } else if (match(parser, TOKEN_IDENTIFIER) &&
                   (!strcmp(parser->current_token.text, "pub") ||
                    !strcmp(parser->current_token.text, "private"))) {
            bool is_public = !strcmp(parser->current_token.text, "pub");
            advance_token(parser);
            if (!match(parser, TOKEN_DEF)) {
                fprintf(stderr, "%s:%d:%d: error: visibility applies to a function declaration\n",
                        parser->source_file, parser->current_token.line, parser->current_token.col);
                exit(1);
            }
            ASTNode *function = parse_function(parser);
            function->is_public = is_public;
            function->has_visibility = true;
            ast_add_child(root, function);
        } else if (match(parser, TOKEN_IDENTIFIER) &&
                   !strcmp(parser->current_token.text, "export")) {
            advance_token(parser);
            if (!match(parser, TOKEN_DEF)) {
                fprintf(stderr, "%s:%d:%d: error: export applies to a function declaration\n",
                        parser->source_file, parser->current_token.line, parser->current_token.col);
                exit(1);
            }
            ASTNode *function = parse_function(parser);
            function->is_exported = true;
            ast_add_child(root, function);
        } else if (match(parser, TOKEN_ENUM)) {
            ast_add_child(root, parse_enum_declaration(parser));
        } else if (match(parser, TOKEN_STRUCT)) {
            ast_add_child(root, parse_struct_declaration(parser));
        } else if (match(parser, TOKEN_IMPORT)) {
            advance_token(parser);
            bool is_c_import = false;
            if (match(parser, TOKEN_IDENTIFIER) && strcmp(parser->current_token.text, "c") == 0) {
                is_c_import = true;
                advance_token(parser); // skip 'c'
            }
            if (!match(parser, TOKEN_STRING_LITERAL)) {
                fprintf(stderr, "%s:%d:%d: error: import requires a quoted source path or C library name\n",
                        parser->source_file, parser->current_token.line, parser->current_token.col);
                exit(1);
            }
            char import_name[128] = "";
            copy_token_text(parser, import_name, sizeof(import_name), is_c_import ? "library name" : "module path");
            if (strlen(import_name) >= COBRA_MAX_IDENT_LEN) {
                fprintf(stderr, "%s:%d:%d: error: import path exceeds %d characters\n",
                        parser->source_file, parser->current_token.line, parser->current_token.col, COBRA_MAX_IDENT_LEN - 1);
                exit(1);
            }
            Token import_token = parser->current_token;
            advance_token(parser);

            ASTNode *import_node = parser_create_node_at(parser, AST_IMPORT_DECL, import_name, import_token);
            import_node->source_import = !is_c_import;

            if (!is_c_import && match(parser, TOKEN_AS)) {
                advance_token(parser);
                if (!match(parser, TOKEN_IDENTIFIER)) {
                    fprintf(stderr, "%s:%d:%d: error: expected module alias after 'as'\n",
                            parser->source_file, parser->current_token.line, parser->current_token.col);
                    exit(1);
                }
                copy_token_text(parser, import_node->module_alias,
                                sizeof(import_node->module_alias), "module alias");
                advance_token(parser);
            }

            if (is_c_import && match(parser, TOKEN_LPAREN)) {
                advance_token(parser);
                while (!match(parser, TOKEN_RPAREN) && !match(parser, TOKEN_EOF)) {
                    if (match(parser, TOKEN_IDENTIFIER)) {
                        ASTNode *fn_ref = parser_create_node(parser, AST_VAR_REF, parser->current_token.text);
                        ast_add_child(import_node, fn_ref);
                        advance_token(parser);
                    } else {
                        fprintf(stderr, "%s:%d:%d: error: expected imported C function name\n",
                                parser->source_file, parser->current_token.line, parser->current_token.col);
                        exit(1);
                    }
                    if (match(parser, TOKEN_COMMA)) advance_token(parser);
                }
                expect(parser, TOKEN_RPAREN, "Expected ')' after imported C function list");
            }
            ast_add_child(root, import_node);
        } else if (match(parser, TOKEN_TRAIT)) {
            ast_add_child(root, parse_trait_declaration(parser));
        } else if (match(parser, TOKEN_IMPL)) {
            ast_add_child(root, parse_impl_declaration(parser));
        } else {
            advance_token(parser);
        }
    }
    synthesize_trait_defaults(parser, root);
    return root;
}
