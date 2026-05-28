#ifndef KAI_PARSER_INTERNAL_H
#define KAI_PARSER_INTERNAL_H

/*
 * Private shared surface of the kaikai-minimal parser.
 *
 * NOT an installed/public header: it exists only so parser.c and
 * parser_expr.c can share the parser state struct, the small token-cursor
 * helpers, the literal decoders, and the mutually-recursive parse_*
 * prototypes. The file was split purely to keep each translation unit a
 * readable size; the helpers below were `static` in the original
 * single-file parser and are kept internal to stage 0.
 */

#include "ast.h"
#include "lexer.h"

#include <stddef.h>
#include <stdint.h>

/* Parser state: a cursor over the token stream plus error tracking. */
typedef struct {
    const char  *file;
    const char  *src;
    const Token *toks;
    size_t       n;
    size_t       i;
    int          had_error;
} P;

/* ---------- token cursor helpers (parser.c) ---------- */

const Token *peek_tok(P *p);
TokenKind    peek_kind(P *p);
TokenKind    peek_kind_n(P *p, size_t off);
void         skip_newlines(P *p);
void         skip_newlines_and_semis(P *p);
int          at(P *p, TokenKind k);
int          match(P *p, TokenKind k);
void         error_at(P *p, const Token *t, const char *msg);
void         error_here(P *p, const char *msg);
const Token *expect(P *p, TokenKind k, const char *msg);
int          is_uppercase_ident(const Token *t, const char *src);
void         set_name_from_token(Node *n, const Token *t, const char *src);

/* ---------- literal decoders (parser.c) ---------- */

int64_t  decode_int(const char *s, size_t len);
double   decode_real(const char *s, size_t len);
uint32_t decode_char(const char *s, size_t len);

/* ---------- parse entry points shared across the two units ---------- */

/* Declarations / types / statements live in parser.c. */
Node *parse_type(P *p);
Node *parse_stmt(P *p);

/* Expressions / primaries / patterns live in parser_expr.c. */
Node *parse_expr(P *p);
Node *parse_primary(P *p);
Node *parse_block(P *p);
Node *parse_pattern(P *p);
Node *parse_record_lit(P *p, Node *type_ident);

#endif /* KAI_PARSER_INTERNAL_H */
