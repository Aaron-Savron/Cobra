/*
 * Cobra Lexer Implementation
 */

#include "../include/cobra.h"

void lexer_init(Lexer *lexer, const char *source) {
    lexer->source = source;
    lexer->length = strlen(source);
    lexer->cursor = 0;
    lexer->line = 1;
    lexer->col = 1;
    lexer->at_line_start = true;
    lexer->brace_depth = 0;
    lexer->paren_depth = 0;
    lexer->bracket_depth = 0;
    lexer->indent_depth = 0;
    lexer->indent_stack[0] = 0;
    lexer->pending_dedents = 0;
    lexer->emitted_eof_dedents = false;
}

static char peek(Lexer *lexer) {
    if (lexer->cursor >= lexer->length) return '\0';
    return lexer->source[lexer->cursor];
}

static char advance(Lexer *lexer) {
    if (lexer->cursor >= lexer->length) return '\0';
    char c = lexer->source[lexer->cursor++];
    if (c == '\n') {
        lexer->line++;
        lexer->col = 1;
        lexer->at_line_start = true;
    } else {
        lexer->col++;
        lexer->at_line_start = false;
    }
    return c;
}

static Token lexer_layout_token(Lexer *lexer, TokenType type, const char *text) {
    Token token;
    memset(&token, 0, sizeof(token));
    token.type = type;
    token.line = lexer->line;
    token.col = lexer->col;
    snprintf(token.text, sizeof(token.text), "%s", text);
    return token;
}

/* At line start, outside any bracket context, measure the leading
   indentation and emit INDENT/DEDENT tokens so indented blocks carry
   structure. Indentation inside braces is ignored. */
static bool lexer_prepare_layout(Lexer *lexer, Token *layout) {
    if (lexer->pending_dedents > 0) {
        lexer->pending_dedents--;
        *layout = lexer_layout_token(lexer, TOKEN_DEDENT, "DEDENT");
        return true;
    }

    while (lexer->at_line_start) {
        int indent = 0;
        while (peek(lexer) == ' ' || peek(lexer) == '\t') {
            indent += peek(lexer) == '\t' ? 4 : 1;
            advance(lexer);
        }
        if (peek(lexer) == '#') {
            while (peek(lexer) != '\n' && peek(lexer) != '\0') advance(lexer);
            continue;
        }
        if (peek(lexer) == '\n') {
            advance(lexer);
            continue;
        }
        if (peek(lexer) == '\0') {
            if (lexer->brace_depth == 0 && lexer->indent_depth > 0 &&
                !lexer->emitted_eof_dedents) {
                lexer->pending_dedents = lexer->indent_depth;
                lexer->indent_depth = 0;
                lexer->emitted_eof_dedents = true;
                return lexer_prepare_layout(lexer, layout);
            }
            return false;
        }

        lexer->at_line_start = false;
        if (lexer->brace_depth != 0 || lexer->paren_depth != 0 ||
            lexer->bracket_depth != 0) return false;
        int current = lexer->indent_stack[lexer->indent_depth];
        if (indent > current) {
            if (lexer->indent_depth + 1 >= COBRA_MAX_INDENT_LEVELS) {
                *layout = lexer_layout_token(lexer, TOKEN_UNKNOWN, "indentation too deep");
                return true;
            }
            lexer->indent_stack[++lexer->indent_depth] = indent;
            *layout = lexer_layout_token(lexer, TOKEN_INDENT, "INDENT");
            return true;
        }
        if (indent < current) {
            int levels = 0;
            while (lexer->indent_depth > 0 &&
                   indent < lexer->indent_stack[lexer->indent_depth]) {
                lexer->indent_depth--;
                levels++;
            }
            if (indent != lexer->indent_stack[lexer->indent_depth]) {
                *layout = lexer_layout_token(lexer, TOKEN_UNKNOWN, "inconsistent indentation");
                return true;
            }
            lexer->pending_dedents = levels > 0 ? levels - 1 : 0;
            *layout = lexer_layout_token(lexer, TOKEN_DEDENT, "DEDENT");
            return true;
        }
    }
    return false;
}

Token lexer_next_token(Lexer *lexer) {
    Token layout;
    if (lexer_prepare_layout(lexer, &layout)) return layout;
    while (1) {
        char whitespace = peek(lexer);
        if (whitespace == ' ' || whitespace == '\t' || whitespace == '\r') {
            advance(lexer);
            continue;
        }
        if (whitespace == '#') {
            while (peek(lexer) != '\n' && peek(lexer) != '\0') advance(lexer);
            if (lexer_prepare_layout(lexer, &layout)) return layout;
            continue;
        }
        if (whitespace == '\n') {
            advance(lexer);
            if (lexer_prepare_layout(lexer, &layout)) return layout;
            continue;
        }
        break;
    }

    Token token;
    token.line = lexer->line;
    token.col = lexer->col;
    token.text[0] = '\0';

    if (lexer->cursor >= lexer->length) {
        token.type = TOKEN_EOF;
        strcpy(token.text, "EOF");
        return token;
    }

    char c = peek(lexer);

    if (c == 'f' && lexer->cursor + 1 < lexer->length && lexer->source[lexer->cursor + 1] == '"') {
        /* f"..." captures the raw text between the quotes so the parser can
           split it into literal segments and {expr} holes. Escape pairs are
           kept verbatim; the parser decodes them in literal segments. */
        advance(lexer); /* 'f' */
        advance(lexer); /* opening quote */
        int idx = 0;
        bool too_long = false;
        bool terminated = false;
        while (peek(lexer) != '"' && peek(lexer) != '\0') {
            if (peek(lexer) == '\\') {
                advance(lexer);
                char esc = peek(lexer);
                if (esc == '\0') break;
                if (idx < COBRA_MAX_TOKEN_TEXT - 2) {
                    token.text[idx++] = '\\';
                    token.text[idx++] = esc;
                } else too_long = true;
                advance(lexer);
                continue;
            }
            if (idx < COBRA_MAX_TOKEN_TEXT - 1) token.text[idx++] = advance(lexer);
            else { too_long = true; advance(lexer); }
        }
        if (peek(lexer) == '"') { advance(lexer); terminated = true; }
        token.text[idx] = '\0';
        if (!terminated) {
            fprintf(stderr, "Lexical Error [line %d, col %d]: unterminated f-string literal\n",
                    token.line, token.col);
            token.type = TOKEN_UNKNOWN;
            return token;
        }
        if (too_long) {
            fprintf(stderr, "Lexical Error [line %d, col %d]: f-string literal exceeds %d bytes\n",
                    token.line, token.col, COBRA_MAX_TOKEN_TEXT - 1);
            token.type = TOKEN_UNKNOWN;
            return token;
        }
        token.type = TOKEN_FSTRING;
        return token;
    }

    if (isalpha(c) || c == '_') {
        int idx = 0;
        bool too_long = false;
        while (isalnum(peek(lexer)) || peek(lexer) == '_') {
            if (idx < COBRA_MAX_TOKEN_TEXT - 1) token.text[idx++] = advance(lexer);
            else { too_long = true; advance(lexer); }
            if (idx >= COBRA_MAX_IDENT_LEN) too_long = true;
        }
        token.text[idx] = '\0';
        if (too_long) {
            fprintf(stderr, "Lexical Error [line %d, col %d]: identifier exceeds %d characters\n",
                    token.line, token.col, COBRA_MAX_IDENT_LEN - 1);
            token.type = TOKEN_UNKNOWN;
            return token;
        }

        if (strcmp(token.text, "def") == 0) token.type = TOKEN_DEF;
        else if (strcmp(token.text, "let") == 0) token.type = TOKEN_LET;
        else if (strcmp(token.text, "var") == 0) token.type = TOKEN_VAR;
        else if (strcmp(token.text, "heap") == 0) token.type = TOKEN_HEAP;
        else if (strcmp(token.text, "struct") == 0) token.type = TOKEN_STRUCT;
        else if (strcmp(token.text, "enum") == 0) token.type = TOKEN_ENUM;
        else if (strcmp(token.text, "match") == 0) token.type = TOKEN_MATCH;
        else if (strcmp(token.text, "case") == 0) token.type = TOKEN_CASE;
        else if (strcmp(token.text, "import") == 0) token.type = TOKEN_IMPORT;
        else if (strcmp(token.text, "for") == 0) token.type = TOKEN_FOR;
        else if (strcmp(token.text, "in") == 0) token.type = TOKEN_IN;
        else if (strcmp(token.text, "return") == 0) token.type = TOKEN_RETURN;
        else if (strcmp(token.text, "if") == 0) token.type = TOKEN_IF;
        else if (strcmp(token.text, "else") == 0) token.type = TOKEN_ELSE;
        else if (strcmp(token.text, "elif") == 0) token.type = TOKEN_ELIF;
        else if (strcmp(token.text, "while") == 0) token.type = TOKEN_WHILE;
        else if (strcmp(token.text, "asm") == 0) token.type = TOKEN_ASM;
        else if (strcmp(token.text, "print") == 0) token.type = TOKEN_PRINT;
        else if (strcmp(token.text, "assert") == 0) token.type = TOKEN_ASSERT;
        else if (strcmp(token.text, "not") == 0) token.type = TOKEN_NOT;
        else if (strcmp(token.text, "and") == 0) token.type = TOKEN_AND;
        else if (strcmp(token.text, "or") == 0) token.type = TOKEN_OR;
        else if (strcmp(token.text, "const") == 0) token.type = TOKEN_CONST;
        else if (strcmp(token.text, "true") == 0 || strcmp(token.text, "True") == 0) token.type = TOKEN_TRUE;
        else if (strcmp(token.text, "false") == 0 || strcmp(token.text, "False") == 0) token.type = TOKEN_FALSE;
        else if (strcmp(token.text, "none") == 0 || strcmp(token.text, "None") == 0) token.type = TOKEN_NONE;
        else if (strcmp(token.text, "len") == 0) token.type = TOKEN_LEN;
        else if (strcmp(token.text, "with") == 0) token.type = TOKEN_WITH;
        else if (strcmp(token.text, "region") == 0) token.type = TOKEN_REGION;
        else if (strcmp(token.text, "trait") == 0) token.type = TOKEN_TRAIT;
        else if (strcmp(token.text, "impl") == 0) token.type = TOKEN_IMPL;
        else if (strcmp(token.text, "as") == 0) token.type = TOKEN_AS;
        else if (strcmp(token.text, "i32") == 0) token.type = TOKEN_TYPE_I32;
        else if (strcmp(token.text, "i64") == 0) token.type = TOKEN_TYPE_I64;
        else if (strcmp(token.text, "u8") == 0) token.type = TOKEN_TYPE_U8;
        else if (strcmp(token.text, "u32") == 0) token.type = TOKEN_TYPE_U32;
        else if (strcmp(token.text, "u64") == 0) token.type = TOKEN_TYPE_U64;
        else if (strcmp(token.text, "f32") == 0) token.type = TOKEN_TYPE_F32;
        else if (strcmp(token.text, "f64") == 0) token.type = TOKEN_TYPE_F64;
        else if (strcmp(token.text, "v256") == 0) token.type = TOKEN_TYPE_V256;
        else if (strcmp(token.text, "void") == 0) token.type = TOKEN_TYPE_VOID;
        else if (strcmp(token.text, "string") == 0) token.type = TOKEN_TYPE_STRING;
        else if (strcmp(token.text, "bool") == 0) token.type = TOKEN_TYPE_BOOL;
        else token.type = TOKEN_IDENTIFIER;

        return token;
    }

    if (isdigit(c)) {
        int idx = 0;
        bool is_float = false;
        bool too_long = false;
        while (isdigit(peek(lexer))) {
            if (idx < COBRA_MAX_TOKEN_TEXT - 1) token.text[idx++] = advance(lexer);
            else { too_long = true; advance(lexer); }
        }
        if (peek(lexer) == '.' && isdigit(lexer->source[lexer->cursor + 1])) {
            is_float = true;
            if (idx < COBRA_MAX_TOKEN_TEXT - 1) token.text[idx++] = advance(lexer);
            else { too_long = true; advance(lexer); }
            while (isdigit(peek(lexer))) {
                if (idx < COBRA_MAX_TOKEN_TEXT - 1) token.text[idx++] = advance(lexer);
                else { too_long = true; advance(lexer); }
            }
        }
        token.text[idx] = '\0';
        if (too_long) {
            fprintf(stderr, "Lexical Error [line %d, col %d]: numeric literal exceeds %d characters\n",
                    token.line, token.col, COBRA_MAX_TOKEN_TEXT - 1);
            token.type = TOKEN_UNKNOWN;
            return token;
        }
        token.type = is_float ? TOKEN_FLOAT_LITERAL : TOKEN_INT_LITERAL;
        return token;
    }

    if (c == '"') {
        advance(lexer);
        int idx = 0;
        bool too_long = false;
        bool terminated = false;
        while (peek(lexer) != '"' && peek(lexer) != '\0') {
            if (peek(lexer) == '\\') {
                advance(lexer);
                char escaped = peek(lexer);
                char decoded = escaped;
                bool known_escape = true;
                if (escaped == 'n') decoded = '\n';
                else if (escaped == 'r') decoded = '\r';
                else if (escaped == 't') decoded = '\t';
                else if (escaped == '\\') decoded = '\\';
                else if (escaped != '"') known_escape = false;
                if (escaped != '\0') advance(lexer);
                if (!known_escape) {
                    if (idx < COBRA_MAX_TOKEN_TEXT - 1) token.text[idx++] = '\\';
                    else too_long = true;
                }
                if (escaped != '\0') {
                    if (idx < COBRA_MAX_TOKEN_TEXT - 1) token.text[idx++] = decoded;
                    else too_long = true;
                }
                continue;
            }
            if (idx < COBRA_MAX_TOKEN_TEXT - 1) token.text[idx++] = advance(lexer);
            else { too_long = true; advance(lexer); }
        }
        if (peek(lexer) == '"') { advance(lexer); terminated = true; }
        token.text[idx] = '\0';
        if (!terminated) {
            fprintf(stderr, "Lexical Error [line %d, col %d]: unterminated string literal\n",
                    token.line, token.col);
            token.type = TOKEN_UNKNOWN;
            return token;
        }
        if (too_long) {
            fprintf(stderr, "Lexical Error [line %d, col %d]: string literal exceeds %d bytes\n",
                    token.line, token.col, COBRA_MAX_TOKEN_TEXT - 1);
            token.type = TOKEN_UNKNOWN;
            return token;
        }
        token.type = TOKEN_STRING_LITERAL;
        return token;
    }

    advance(lexer);
    token.text[0] = c;
    token.text[1] = '\0';

    switch (c) {
        case ':': token.type = TOKEN_COLON; break;
        case ';': token.type = TOKEN_SEMICOLON; break;
        case ',': token.type = TOKEN_COMMA; break;
        case '=':
            if (peek(lexer) == '=') {
                advance(lexer);
                token.type = TOKEN_EQ;
                strcpy(token.text, "==");
            } else {
                token.type = TOKEN_ASSIGN;
            }
            break;
        case '!':
            if (peek(lexer) == '=') {
                advance(lexer);
                token.type = TOKEN_NEQ;
                strcpy(token.text, "!=");
            } else {
                token.type = TOKEN_UNKNOWN;
            }
            break;
        case '<':
            if (peek(lexer) == '=') {
                advance(lexer);
                token.type = TOKEN_LTE;
                strcpy(token.text, "<=");
            } else {
                token.type = TOKEN_LT;
            }
            break;
        case '>':
            if (peek(lexer) == '=') {
                advance(lexer);
                token.type = TOKEN_GTE;
                strcpy(token.text, ">=");
            } else {
                token.type = TOKEN_GT;
            }
            break;
        case '+': token.type = TOKEN_PLUS; break;
        case '-':
            if (peek(lexer) == '>') {
                advance(lexer);
                token.type = TOKEN_ARROW;
                strcpy(token.text, "->");
            } else {
                token.type = TOKEN_MINUS;
            }
            break;
        case '*':
            if (peek(lexer) == '*') {
                advance(lexer);
                token.type = TOKEN_POW;
                strcpy(token.text, "**");
            } else {
                token.type = TOKEN_STAR;
            }
            break;
        case '/':
            if (peek(lexer) == '/') {
                advance(lexer);
                token.type = TOKEN_FLOORDIV;
                strcpy(token.text, "//");
            } else {
                token.type = TOKEN_SLASH;
            }
            break;
        case '%': token.type = TOKEN_PERCENT; break;
        case '.': token.type = TOKEN_DOT; break;
        case '@':
            {
                int idx = 0;
                while (isalpha(peek(lexer))) {
                    if (idx < 127) token.text[idx++] = advance(lexer);
                    else advance(lexer);
                }
                token.text[idx] = '\0';
                if (strcmp(token.text, "comptime") == 0) {
                    token.type = TOKEN_COMPTIME;
                } else if (strcmp(token.text, "compute") == 0) {
                    token.type = TOKEN_COMPUTE;
                } else if (strcmp(token.text, "parallel") == 0) {
                    token.type = TOKEN_PARALLEL;
                } else if (strcmp(token.text, "gpu") == 0) {
                    token.type = TOKEN_GPU_DIRECTIVE;
                } else if (strcmp(token.text, "cpu") == 0) {
                    token.type = TOKEN_CPU_DIRECTIVE;
                } else {
                    token.type = TOKEN_UNKNOWN;
                }
            }
            break;
        case '[':
            token.type = TOKEN_LBRACKET;
            lexer->bracket_depth++;
            break;
        case ']':
            token.type = TOKEN_RBRACKET;
            if (lexer->bracket_depth > 0) lexer->bracket_depth--;
            break;
        case '?': token.type = TOKEN_QUESTION; break;
        case '|':
            if (peek(lexer) == '>') {
                advance(lexer);
                token.type = TOKEN_PIPE;
                strcpy(token.text, "|>");
            } else {
                token.type = TOKEN_UNKNOWN;
            }
            break;
        case '(':
            token.type = TOKEN_LPAREN;
            lexer->paren_depth++;
            break;
        case ')':
            token.type = TOKEN_RPAREN;
            if (lexer->paren_depth > 0) lexer->paren_depth--;
            break;
        case '{':
            token.type = TOKEN_LBRACE;
            lexer->brace_depth++;
            break;
        case '}':
            token.type = TOKEN_RBRACE;
            if (lexer->brace_depth > 0) lexer->brace_depth--;
            break;
        default: token.type = TOKEN_UNKNOWN; break;
    }

    return token;
}
