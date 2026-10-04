#ifndef LUME_PARSER_INTERNAL_H
#define LUME_PARSER_INTERNAL_H

#include "lume.h"

/* Parser 内部结构与跨文件共享声明(parser.c / parser_stmt.c /
 * parser_expr.c)。Parser 在 parse_program 中一次性构造,语句与表达式
 * 解析共享同一 token 游标与错误缓冲。 */

typedef struct {
    Token *toks;
    int count;
    int pos;
    char errbuf[256];
} Parser;

/* ---- 基础工具(parser.c) ---- */
void perror_at(Parser *p, size_t line, const char *fmt, ...);
Node *nalloc(NodeType type, size_t line);
Token peek(const Parser *p);
Token peek2(const Parser *p);
size_t previous_line(const Parser *p);
bool at_end(const Parser *p);
bool check(const Parser *p, TokenType t);
bool advance(Parser *p);
bool match(Parser *p, TokenType t);
bool expect(Parser *p, TokenType t);
char *ident_name(Parser *p, Token t);
bool is_method_keyword(TokenType t);
const char *method_keyword_name(TokenType t);
bool is_field_token(TokenType t);

/* Release one node the parser abandoned mid-statement: children are NOT
 * walked (the caller frees every node of the batch itself). */
void node_free_orphan(Node *n);

/* Take a node back out of the orphan batch. Every nalloc() node sits in that
 * batch, so a parser file that discards a node of its own -- free()ing just
 * the shell, after moving whatever the node owned over to a node that outlives
 * it -- has to call this first: otherwise the sweep at the end of a failed
 * parse frees the same pointer a second time. */
void node_unjournal(Node *n);

/* ---- 类型 / 块 / 函数声明(parser.c) ---- */
Type *parse_type(Parser *p);
bool parse_params(Parser *p, char ***names_out, Type ***types_out,
                  int *arity_out);
Type *parse_optional_ret(Parser *p);
Node *parse_block(Parser *p);
Node *parse_func_literal(Parser *p, size_t kw_line);
Node *parse_route_tail(Parser *p, Node *n);

/* ---- 语句 / 表达式(parser_stmt.c / parser_expr.c) ---- */
Node *parse_statement(Parser *p);
Node *parse_expression(Parser *p);

#endif /* LUME_PARSER_INTERNAL_H */
