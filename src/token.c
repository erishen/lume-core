#include "lume.h"

static const char *const TOKEN_NAMES[] = {
    "end-of-file", "error",
    "identifier", "number", "string",
    "server", "route", "tool", "func", "return",
    "if", "else", "while", "for", "in", "break", "continue", "let",
    "import", "export", "as",
    "true", "false", "null", "and", "or", "not",
    "type", "int", "float", "string", "bool", "Result",
    "get", "head", "post", "put", "patch", "delete", "options", "verbs",
    "(", ")", "{", "}", "[", "]",
    ",", ";", ".", ":", "?",
    "=", "==", "!=", "<", "<=", ">", ">=", "=>",
    "+", "-", "*", "/", "%"
};

const char *token_type_name(TokenType t) {
    int i = (int)t;
    return (i >= 0 && i < (int)(sizeof(TOKEN_NAMES) / sizeof(TOKEN_NAMES[0])))
               ? TOKEN_NAMES[i]
               : "?";
}