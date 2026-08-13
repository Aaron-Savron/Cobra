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

static const CobraType *parser_component_type(Parser *parser, CobraTypeKind kind,
                                                const char *name) {
    if (!parser || !parser->canonical_arena || kind == COBRA_TYPE_UNTYPED ||
        kind == COBRA_TYPE_UNKNOWN) return NULL;
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
                                         char *type_name_out) {
    type_name_out[0] = '\0';
    if (match(parser, TOKEN_IDENTIFIER)) {
        copy_token_text(parser, type_name_out, COBRA_MAX_IDENT_LEN, context);
        advance_token(parser);
        return COBRA_TYPE_STRUCT;
    }
    CobraTypeKind type = token_to_type(parser->current_token.type);
    if (type != COBRA_TYPE_I32 && type != COBRA_TYPE_I64 &&
        type != COBRA_TYPE_U32 && type != COBRA_TYPE_U64 &&
        type != COBRA_TYPE_F32 && type != COBRA_TYPE_BOOL) {
        fprintf(stderr, "%s:%d:%d: error: %s payload must be a scalar or named struct type\n",
                parser->source_file, parser->current_token.line, parser->current_token.col, context);
        exit(1);
    }
    advance_token(parser);
    return type;
}

static CobraTypeKind parse_type_into(Parser *parser, const char *context,
                                     ASTNode *owner, int qualifier) {
    bool slice = false;
    bool tensor = false;
    if (match(parser, TOKEN_IDENTIFIER) &&
        (!strcmp(parser->current_token.text, "Option") ||
         !strcmp(parser->current_token.text, "Result"))) {
        bool is_result = !strcmp(parser->current_token.text, "Result");
        advance_token(parser);
        expect(parser, TOKEN_LBRACKET, "Expected '[' after Option or Result");
        char value_name[COBRA_MAX_IDENT_LEN];
        CobraTypeKind value = parse_sum_component(parser, "Option or Result value", value_name);
        const CobraType *value_type = parser_component_type(parser, value, value_name);
        const CobraType *error_type = NULL;
        if (is_result) {
            expect(parser, TOKEN_COMMA, "Result requires a value and error type");
            char error_name[COBRA_MAX_IDENT_LEN];
            CobraTypeKind error = parse_sum_component(parser, "Result error", error_name);
            error_type = parser_component_type(parser, error, error_name);
        }
        expect(parser, TOKEN_RBRACKET, "Expected ']' after Option or Result type");
        if (owner) parser_set_canonical(parser, owner,
                                        is_result ? COBRA_TYPE_RESULT : COBRA_TYPE_OPTION,
                                        qualifier, value_type, error_type, NULL, NULL);
        return is_result ? COBRA_TYPE_RESULT : COBRA_TYPE_OPTION;
    }
    if (match(parser, TOKEN_IDENTIFIER) && strcmp(parser->current_token.text, "list") == 0) {
        advance_token(parser);
        expect(parser, TOKEN_LBRACKET, "Expected '[' after list in collection type");
        CobraTypeKind element = token_to_type(parser->current_token.type);
        if (element == COBRA_TYPE_UNKNOWN || element == COBRA_TYPE_VOID) {
            fprintf(stderr, "%s:%d:%d: error: list element type must be a scalar type\n",
                    parser->source_file, parser->current_token.line, parser->current_token.col);
            exit(1);
        }
        advance_token(parser);
        expect(parser, TOKEN_RBRACKET, "Expected ']' after list element type");
        if (owner) {
            parser_set_canonical(parser, owner, COBRA_TYPE_LIST, qualifier,
                                 parser_component_type(parser, element, NULL),
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
        /* A plain identifier in type position is a user-defined struct type.
           Its qualified identity is constructed directly from the token; no
           AST type-name mirror is needed. */
        if (owner && parser->canonical_arena) {
            CobraOwnershipKind ownership = qualifier == 1 ? COBRA_OWNERSHIP_BORROWED : COBRA_OWNERSHIP_VALUE;
            CobraMutabilityKind mutability = qualifier == 1 ? COBRA_MUTABILITY_READONLY :
                                             (qualifier == 2 ? COBRA_MUTABILITY_OUT :
                                                              COBRA_MUTABILITY_DEFAULT);
            owner->canonical_type = cobra_type_make(parser->canonical_arena,
                                                    COBRA_TYPE_STRUCT,
                                                    parser->current_token.text,
                                                    NULL, NULL, NULL, NULL,
                                                    ownership, mutability, -1);
        }
        advance_token(parser);
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

static long long parse_integer_magnitude(Parser *parser) {
    errno = 0;
    char *end = NULL;
    long long value = strtoll(parser->current_token.text, &end, 10);
    if (errno == ERANGE || end == parser->current_token.text || *end != '\0' || value < 0) {
        fprintf(stderr, "%s:%d:%d: error: integer literal is out of range\n",
                parser->source_file, parser->current_token.line, parser->current_token.col);
        exit(1);
    }
    return value;
}

static int parse_int_literal(Parser *parser) {
    long long value = parse_integer_magnitude(parser);
    if (value > INT_MAX) {
        fprintf(stderr, "%s:%d:%d: error: integer literal is out of range\n",
                parser->source_file, parser->current_token.line, parser->current_token.col);
        exit(1);
    }
    return (int)value;
}

static ASTNode *parse_primary(Parser *parser) {
    if (match(parser, TOKEN_MINUS)) {
        advance_token(parser);
        if (match(parser, TOKEN_INT_LITERAL)) {
            ASTNode *node = parser_create_node(parser, AST_INT_LITERAL, NULL);
            long long magnitude = parse_integer_magnitude(parser);
            if (magnitude > (long long)INT_MAX + 1) {
                fprintf(stderr, "%s:%d:%d: error: integer literal is out of range\n",
                        parser->source_file, parser->current_token.line, parser->current_token.col);
                exit(1);
            }
            node->int_val = magnitude == (long long)INT_MAX + 1 ? INT_MIN : -(int)magnitude;
            advance_token(parser);
            return node;
        }
        if (match(parser, TOKEN_FLOAT_LITERAL)) {
            ASTNode *node = parser_create_node(parser, AST_FLOAT_LITERAL, NULL);
            node->float_val = -(float)atof(parser->current_token.text);
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
        node->int_val = parse_int_literal(parser);
        advance_token(parser);
        return node;
    }

    if (match(parser, TOKEN_FLOAT_LITERAL)) {
        ASTNode *node = parser_create_node(parser, AST_FLOAT_LITERAL, NULL);
        node->float_val = (float)atof(parser->current_token.text);
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
        advance_token(parser);
        ASTNode *expr = parse_expression(parser);
        expect(parser, TOKEN_RPAREN, "Expected ')' after expression");
        return expr;
    }

    fprintf(stderr, "%s:%d:%d: error: Expected expression, got '%s'\n",
            parser->source_file, parser->current_token.line, parser->current_token.col, parser->current_token.text);
    exit(1);
}

static bool is_multiplicative(Parser *parser) {
    return match(parser, TOKEN_STAR) || match(parser, TOKEN_SLASH);
}

static bool is_additive(Parser *parser) {
    return match(parser, TOKEN_PLUS) || match(parser, TOKEN_MINUS);
}

static bool is_comparison(Parser *parser) {
    return match(parser, TOKEN_EQ) || match(parser, TOKEN_NEQ) ||
           match(parser, TOKEN_LT) || match(parser, TOKEN_GT) ||
           match(parser, TOKEN_LTE) || match(parser, TOKEN_GTE);
}

static ASTNode *make_binary(Parser *parser, ASTNode *left) {
    char op[8];
    Token op_token = parser->current_token;
    strcpy(op, parser->current_token.text);
    advance_token(parser);
    ASTNode *right = parse_primary(parser);
    ASTNode *binary = parser_create_node_at(parser, AST_BINARY_OP, op, op_token);
    ast_add_child(binary, left);
    ast_add_child(binary, right);
    return binary;
}

static ASTNode *parse_multiplicative(Parser *parser) {
    ASTNode *left = parse_primary(parser);
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
                if (!match(parser, TOKEN_IDENTIFIER)) {
                    fprintf(stderr, "%s:%d:%d: error: expected enum name after 'case'\n",
                            parser->source_file, parser->current_token.line, parser->current_token.col);
                    exit(1);
                }
                Token enum_token = parser->current_token;
                ASTNode *case_node = parser_create_node_at(parser, AST_MATCH_CASE, NULL, enum_token);
                copy_token_text(parser, case_node->match_type_name, sizeof(case_node->match_type_name), "match enum name");
                advance_token(parser);
                expect(parser, TOKEN_DOT, "Expected '.' between enum name and variant");
                if (!match(parser, TOKEN_IDENTIFIER)) {
                    fprintf(stderr, "%s:%d:%d: error: expected enum variant after 'case %s.'\n",
                            parser->source_file, parser->current_token.line, parser->current_token.col, case_node->match_type_name);
                    exit(1);
                }
                copy_token_text(parser, case_node->secondary_name,
                                sizeof(case_node->secondary_name), "match variant name");
                advance_token(parser);
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
        char var_name[COBRA_MAX_IDENT_LEN];
        Token variable_token = parser->current_token;
        copy_token_text(parser, var_name, sizeof(var_name), "variable name");
        expect(parser, TOKEN_IDENTIFIER, "Expected variable name");

        ASTNode *var_node = parser_create_node_at(parser, AST_VAR_DECL, var_name, variable_token);
        var_node->is_const = is_const;
        CobraTypeKind declared_type = COBRA_TYPE_UNTYPED;
        if (match(parser, TOKEN_COLON)) {
            advance_token(parser);
            declared_type = parse_type_into(parser, "variable declaration", var_node, 0);
        }

        var_node->declared_type = declared_type;
        if (declared_type == COBRA_TYPE_STRUCT) {
            /* Struct declarations may omit the initializer; the region is
               zero-initialized and fields are assigned individually. */
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
        if (!match(parser, TOKEN_RBRACE) && !match(parser, TOKEN_EOF) &&
            !match(parser, TOKEN_DEF) && !match(parser, TOKEN_ELSE)) {
            ASTNode *expr = parse_expression(parser);
            ast_add_child(ret_node, expr);
        }
        return ret_node;
    }

    if (match(parser, TOKEN_ASM)) {
        advance_token(parser);
        expect(parser, TOKEN_COLON, "Expected ':' after 'asm'");
        
        ASTNode *asm_node = parser_create_node(parser, AST_ASM_BLOCK, NULL);
        
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

static ASTNode *parse_block(Parser *parser) {
    ASTNode *block = parser_create_node(parser, AST_PROGRAM, "Block");

    if (match(parser, TOKEN_LBRACE)) {
        advance_token(parser);
        while (!match(parser, TOKEN_RBRACE) && !match(parser, TOKEN_EOF)) {
            ast_add_child(block, parse_statement(parser));
        }
        expect(parser, TOKEN_RBRACE, "Expected '}' after block");
    } else {
        // Single statement or indented line
        ast_add_child(block, parse_statement(parser));
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

    if (match(parser, TOKEN_COLON)) advance_token(parser);
    expect(parser, TOKEN_LBRACE, "Expected '{' after struct name");

    ASTNode *struct_node = parser_create_node_at(parser, AST_STRUCT_DECL, struct_name, name_token);
    struct_node->declared_type = COBRA_TYPE_STRUCT;
    struct_node->canonical_type = parser_component_type(parser, COBRA_TYPE_STRUCT, struct_name);
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

        if (field->declared_type == COBRA_TYPE_OPTION || field->declared_type == COBRA_TYPE_RESULT) {
            fprintf(stderr, "%s:%d:%d: error: Option and Result fields are not supported yet; keep sum values local or return them\n",
                    parser->source_file, field_token.line, field_token.col);
            exit(1);
        }
        ast_add_child(struct_node, field);
        if (match(parser, TOKEN_COMMA)) advance_token(parser);
    }
    expect(parser, TOKEN_RBRACE, "Expected '}' after struct fields");
    return struct_node;
}

static ASTNode *parse_function(Parser *parser) {
    expect(parser, TOKEN_DEF, "Expected 'def' to start function declaration");
    
    char fn_name[COBRA_MAX_IDENT_LEN];
    Token function_token = parser->current_token;
    copy_token_text(parser, fn_name, sizeof(fn_name), "function name");
    expect(parser, TOKEN_IDENTIFIER, "Expected function name");

    ASTNode *fn_node = parser_create_node_at(parser, AST_FUNCTION, fn_name, function_token);

    expect(parser, TOKEN_LPAREN, "Expected '(' after function name");
    
    // Parse parameters: def foo(a: i64, b: i64) -> i64:
    // Alias contracts use contextual qualifiers (out/readonly) after the
    // colon, so existing code that names a variable `out` keeps working.
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

                    }
                }
            }
        }
    }

    expect(parser, TOKEN_RPAREN, "Expected ')' after function parameters");

    if (match(parser, TOKEN_ARROW)) {
        advance_token(parser);
        fn_node->declared_type = parse_type_into(parser, "return", fn_node, 0);

    }

    if (match(parser, TOKEN_COLON)) {
        advance_token(parser);
    }

    if (match(parser, TOKEN_LBRACE)) {
        advance_token(parser);
        while (!match(parser, TOKEN_RBRACE) && !match(parser, TOKEN_EOF)) {
            ast_add_child(fn_node, parse_statement(parser));
        }
        expect(parser, TOKEN_RBRACE, "Expected '}' at end of function block");
    } else {
        while (!match(parser, TOKEN_EOF) && !match(parser, TOKEN_DEF)) {
            ast_add_child(fn_node, parse_statement(parser));
        }
    }

    return fn_node;
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
    while (!match(parser, TOKEN_EOF)) {
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

            if (!is_c_import && match(parser, TOKEN_IDENTIFIER) &&
                strcmp(parser->current_token.text, "as") == 0) {
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
        } else {
            advance_token(parser);
        }
    }
    return root;
}
