#ifndef KAI_PARSER_H
#define KAI_PARSER_H

#include "ast.h"
#include "lexer.h"

/*
 * Parse a token stream into an AST.
 *
 * On success, returns a newly allocated N_PROGRAM node (caller owns).
 * On any parse error, prints the error(s) to stderr and returns NULL.
 */
Node *kai_parse(const char *file, const char *src,
                const Token *toks, size_t n);

#endif /* KAI_PARSER_H */
