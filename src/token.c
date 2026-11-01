#include "lume.h"

static const char *const TOKEN_NAMES[] = {
    "end-of-file", "error",
    "identifier", "number", "string", "interpolated string",
    "server", "route", "tool", "func", "return",
    "if", "else", "while", "for", "in", "break", "continue", "let",
    "import", "export", "as",
    "true", "false", "null", "and", "or", "not",
    "type", "int", "float", "string", "bool", "Result", "fn",
    "get", "head", "post", "put", "patch", "delete", "options", "verbs",
    "enum", "match", "|", "&", "^",
    "(", ")", "{", "}", "[", "]",
    ",", ";", ".", ":", "?",
    "=", "==", "!=", "<", "<=", ">", ">=", "=>",
    "+", "-", "*", "/", "%"
};

/* Guard against the TokenType enum and this table drifting apart again:
 * 5117488 inserted TOK_AMP/TOK_CARET after TOK_PIPE without extending the
 * table, so every name from "(" onward shifted by two — parser diagnostics
 * printed "expected }, got [" where they meant "expected ), got {", which
 * made diagnostic-keyed negative tests (trailing-closure-bad) fail even
 * though parsing itself was correct. */
_Static_assert(sizeof(TOKEN_NAMES) / sizeof(TOKEN_NAMES[0]) == TOK_MAX,
               "TOKEN_NAMES out of sync with TokenType (see lume.h)");

const char *token_type_name(TokenType t) {
    int i = (int)t;
    return (i >= 0 && i < (int)(sizeof(TOKEN_NAMES) / sizeof(TOKEN_NAMES[0])))
               ? TOKEN_NAMES[i]
               : "?";
}