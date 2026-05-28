/*
 * Expression, primary, and pattern parsing for kaikai-minimal.
 *
 * Split out of parser.c (which keeps declarations, types, statements, the
 * token-cursor helpers, and the literal decoders) so each translation unit
 * stays a readable size. Shared state and helpers come from
 * parser_internal.h. See parser.c for the overall parser contract.
 */

#include "parser.h"
#include "parser_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward decls — precedence ladder and helpers internal to this unit. */
static Node *parse_pipe(P *p);
static Node *parse_or(P *p);
static Node *parse_and(P *p);
static Node *parse_cmp(P *p);
static Node *parse_add(P *p);
static Node *parse_mul(P *p);
static Node *parse_unary(P *p);
static Node *parse_postfix(P *p);
static Node *parse_if(P *p);
static Node *parse_match(P *p);
static Node *parse_lambda_from_ident(P *p, Node *ident);
static Node *parse_list_or_range(P *p);
static Node *parse_string_literal(P *p);

/* ---------- expressions (by precedence) ---------- */

/*
 * Precedence (high to low):
 *   postfix:    . [] ()
 *   unary:      - not
 *   multiplicative: * / // %
 *   additive:   + -
 *   comparison: == != < > <= >=
 *   and
 *   or
 *   pipe:       |>
 */

Node *parse_expr(P *p) { return parse_pipe(p); }

static Node *parse_pipe(P *p) {
    Node *lhs = parse_or(p);
    if (!lhs) return NULL;
    for (;;) {
        if (peek_kind(p) == TK_NEWLINE && peek_kind_n(p, 1) == TK_PIPE_APPLY) {
            /* allow newlines before |> continuation */
            p->i++;
        }
        if (!at(p, TK_PIPE_APPLY)) break;
        int line = peek_tok(p)->line, col = peek_tok(p)->col;
        p->i++;
        skip_newlines(p);
        Node *rhs = parse_or(p);
        if (!rhs) { kai_free_node(lhs); return NULL; }
        Node *pipe = kai_new_node(N_PIPE, line, col);
        kai_node_push(pipe, lhs);
        kai_node_push(pipe, rhs);
        lhs = pipe;
    }
    return lhs;
}

static Node *binop(int op, Node *lhs, Node *rhs, int line, int col) {
    Node *n = kai_new_node(N_BINOP, line, col);
    n->v.op = op;
    kai_node_push(n, lhs);
    kai_node_push(n, rhs);
    return n;
}

static Node *parse_or(P *p) {
    Node *lhs = parse_and(p);
    if (!lhs) return NULL;
    while (at(p, TK_OR)) {
        int line = peek_tok(p)->line, col = peek_tok(p)->col;
        p->i++; skip_newlines(p);
        Node *rhs = parse_and(p);
        if (!rhs) { kai_free_node(lhs); return NULL; }
        lhs = binop(TK_OR, lhs, rhs, line, col);
    }
    return lhs;
}

static Node *parse_and(P *p) {
    Node *lhs = parse_cmp(p);
    if (!lhs) return NULL;
    while (at(p, TK_AND)) {
        int line = peek_tok(p)->line, col = peek_tok(p)->col;
        p->i++; skip_newlines(p);
        Node *rhs = parse_cmp(p);
        if (!rhs) { kai_free_node(lhs); return NULL; }
        lhs = binop(TK_AND, lhs, rhs, line, col);
    }
    return lhs;
}

static int is_cmp(TokenKind k) {
    return k == TK_EQEQ || k == TK_NEQ || k == TK_LT || k == TK_GT ||
           k == TK_LE   || k == TK_GE;
}

static Node *parse_cmp(P *p) {
    Node *lhs = parse_add(p);
    if (!lhs) return NULL;
    if (is_cmp(peek_kind(p))) {
        int op = peek_kind(p);
        int line = peek_tok(p)->line, col = peek_tok(p)->col;
        p->i++; skip_newlines(p);
        Node *rhs = parse_add(p);
        if (!rhs) { kai_free_node(lhs); return NULL; }
        lhs = binop(op, lhs, rhs, line, col);
    }
    return lhs;
}

static Node *parse_add(P *p) {
    Node *lhs = parse_mul(p);
    if (!lhs) return NULL;
    while (at(p, TK_PLUS) || at(p, TK_MINUS)) {
        int op = peek_kind(p);
        int line = peek_tok(p)->line, col = peek_tok(p)->col;
        p->i++; skip_newlines(p);
        Node *rhs = parse_mul(p);
        if (!rhs) { kai_free_node(lhs); return NULL; }
        lhs = binop(op, lhs, rhs, line, col);
    }
    return lhs;
}

static Node *parse_mul(P *p) {
    Node *lhs = parse_unary(p);
    if (!lhs) return NULL;
    while (at(p, TK_STAR) || at(p, TK_SLASH) ||
           at(p, TK_SLASH_SLASH) || at(p, TK_PERCENT)) {
        int op = peek_kind(p);
        int line = peek_tok(p)->line, col = peek_tok(p)->col;
        p->i++; skip_newlines(p);
        Node *rhs = parse_unary(p);
        if (!rhs) { kai_free_node(lhs); return NULL; }
        lhs = binop(op, lhs, rhs, line, col);
    }
    return lhs;
}

static Node *parse_unary(P *p) {
    if (at(p, TK_MINUS) || at(p, TK_NOT)) {
        int op = peek_kind(p);
        int line = peek_tok(p)->line, col = peek_tok(p)->col;
        p->i++; skip_newlines(p);
        Node *rhs = parse_unary(p);
        if (!rhs) return NULL;
        Node *n = kai_new_node(N_UNOP, line, col);
        n->v.op = op;
        kai_node_push(n, rhs);
        return n;
    }
    return parse_postfix(p);
}

/* Call suffix `lhs( arg, ... )`. The `(` token `t` is for line/col; on
   entry it has not yet been consumed. Returns the N_CALL, or NULL on error
   (and frees `lhs`). */
static Node *parse_call_suffix(P *p, Node *lhs, const Token *t) {
    p->i++;
    skip_newlines(p);
    Node *call = kai_new_node(N_CALL, t->line, t->col);
    kai_node_push(call, lhs);
    if (!at(p, TK_RPAREN)) {
        for (;;) {
            skip_newlines(p);
            Node *arg = parse_expr(p);
            if (!arg) { kai_free_node(call); return NULL; }
            kai_node_push(call, arg);
            skip_newlines(p);
            if (!match(p, TK_COMMA)) break;
        }
    }
    if (!expect(p, TK_RPAREN, "expected `)` after call arguments")) {
        kai_free_node(call); return NULL;
    }
    return call;
}

/* Index suffix `lhs[ idx ]`. The `[` token `t` is for line/col; on entry it
   has not yet been consumed. Returns the N_INDEX, or NULL on error (and
   frees `lhs`). */
static Node *parse_index_suffix(P *p, Node *lhs, const Token *t) {
    p->i++;
    skip_newlines(p);
    Node *idx = parse_expr(p);
    if (!idx) { kai_free_node(lhs); return NULL; }
    if (!expect(p, TK_RBRACKET, "expected `]` after index")) {
        kai_free_node(idx); kai_free_node(lhs); return NULL;
    }
    Node *n = kai_new_node(N_INDEX, t->line, t->col);
    kai_node_push(n, lhs);
    kai_node_push(n, idx);
    return n;
}

static Node *parse_postfix(P *p) {
    Node *lhs = parse_primary(p);
    if (!lhs) return NULL;
    for (;;) {
        const Token *t = peek_tok(p);
        if (t->kind == TK_LPAREN) {
            lhs = parse_call_suffix(p, lhs, t);
            if (!lhs) return NULL;
        } else if (t->kind == TK_DOT && peek_kind_n(p, 1) == TK_IDENT) {
            p->i++;
            const Token *fn = &p->toks[p->i++];
            Node *f = kai_new_node(N_FIELD, t->line, t->col);
            set_name_from_token(f, fn, p->src);
            kai_node_push(f, lhs);
            lhs = f;
        } else if (t->kind == TK_LBRACKET) {
            lhs = parse_index_suffix(p, lhs, t);
            if (!lhs) return NULL;
        } else {
            break;
        }
    }
    return lhs;
}

/* ---------- primary expressions ---------- */

/* Lookahead (no consume): is the run from `p->i` shaped `IDENT (, IDENT)* )
   =>`, i.e. a parameter-list lambda? */
static int scan_paren_is_lambda(P *p) {
    if (!at(p, TK_IDENT)) return 0;
    size_t j = p->i;
    for (;;) {
        if (p->toks[j].kind != TK_IDENT) return 0;
        j++;
        if (p->toks[j].kind == TK_RPAREN) {
            return p->toks[j + 1].kind == TK_FAT_ARROW;
        }
        if (p->toks[j].kind != TK_COMMA) return 0;
        j++;
        while (p->toks[j].kind == TK_NEWLINE) j++;
    }
}

/* Build a parameter-list lambda `(a, b) => body`. On entry `p->i` is at the
   first parameter IDENT; `t` carries the `(` line/col. */
static Node *parse_paren_lambda(P *p, const Token *t) {
    Node *lam = kai_new_node(N_LAMBDA, t->line, t->col);
    kai_node_push(lam, NULL); /* body placeholder */
    for (;;) {
        const Token *pn = expect(p, TK_IDENT, "expected parameter name");
        if (!pn) { kai_free_node(lam); return NULL; }
        Node *id = kai_new_node(N_IDENT, pn->line, pn->col);
        set_name_from_token(id, pn, p->src);
        kai_node_push(lam, id);
        if (!match(p, TK_COMMA)) break;
        skip_newlines(p);
    }
    if (!expect(p, TK_RPAREN, "expected `)` after lambda parameters")) {
        kai_free_node(lam); return NULL;
    }
    if (!expect(p, TK_FAT_ARROW, "expected `=>`")) {
        kai_free_node(lam); return NULL;
    }
    skip_newlines(p);
    Node *body = parse_expr(p);
    if (!body) { kai_free_node(lam); return NULL; }
    lam->children[0] = body;
    return lam;
}

static Node *parse_paren_expr(P *p, const Token *t) {
    p->i++;
    /* `()` Unit, or `() => expr` zero-arg lambda. */
    if (at(p, TK_RPAREN)) {
        p->i++;
        if (at(p, TK_FAT_ARROW)) {
            p->i++; skip_newlines(p);
            Node *body = parse_expr(p);
            if (!body) return NULL;
            Node *lam = kai_new_node(N_LAMBDA, t->line, t->col);
            kai_node_push(lam, body);
            return lam;
        }
        return kai_new_node(N_UNIT, t->line, t->col);
    }
    if (scan_paren_is_lambda(p)) return parse_paren_lambda(p, t);
    /* Otherwise: plain parenthesized expression. */
    skip_newlines(p);
    Node *e = parse_expr(p);
    if (!e) return NULL;
    if (!expect(p, TK_RPAREN, "expected `)`")) { kai_free_node(e); return NULL; }
    return e;
}

Node *parse_primary(P *p) {
    const Token *t = peek_tok(p);

    switch (t->kind) {
        case TK_INT: {
            p->i++;
            Node *n = kai_new_node(N_INT, t->line, t->col);
            n->v.i = decode_int(p->src + t->start, t->length);
            n->name = p->src + t->start; n->name_len = t->length;
            return n;
        }
        case TK_REAL: {
            p->i++;
            Node *n = kai_new_node(N_REAL, t->line, t->col);
            n->v.r = decode_real(p->src + t->start, t->length);
            n->name = p->src + t->start; n->name_len = t->length;
            return n;
        }
        case TK_CHAR: {
            p->i++;
            Node *n = kai_new_node(N_CHAR, t->line, t->col);
            n->v.c = decode_char(p->src + t->start, t->length);
            n->name = p->src + t->start; n->name_len = t->length;
            return n;
        }
        case TK_STRING: {
            return parse_string_literal(p);
        }
        case TK_TRUE: {
            p->i++;
            Node *n = kai_new_node(N_BOOL, t->line, t->col);
            n->v.b = 1;
            return n;
        }
        case TK_FALSE: {
            p->i++;
            Node *n = kai_new_node(N_BOOL, t->line, t->col);
            n->v.b = 0;
            return n;
        }
        case TK_UNDERSCORE: {
            /* Underscore in expression position is not valid in minimal;
               it's for patterns only. Emit an error. */
            error_here(p, "`_` is only valid inside patterns");
            return NULL;
        }
        case TK_DOT: {
            p->i++;
            return kai_new_node(N_PLACEHOLDER, t->line, t->col);
        }
        case TK_LBRACKET: {
            return parse_list_or_range(p);
        }
        case TK_LBRACE: {
            return parse_block(p);
        }
        case TK_IF:    return parse_if(p);
        case TK_MATCH: return parse_match(p);

        case TK_LPAREN:
            /* `()` Unit, `(expr)` grouping, or `(a, ...) => body` lambda. */
            return parse_paren_expr(p, t);

        case TK_IDENT: {
            /* Might be: ident, ident => body (single-param lambda),
                        IDENT{...} record literal (if uppercase and next is `{`). */
            const Token *id = &p->toks[p->i++];
            /* Lambda shape: IDENT `=>` */
            if (at(p, TK_FAT_ARROW)) {
                Node *idn = kai_new_node(N_IDENT, id->line, id->col);
                set_name_from_token(idn, id, p->src);
                return parse_lambda_from_ident(p, idn);
            }
            /* Record literal: only if name is uppercase AND next token is `{`
               — but we must avoid grabbing block braces of if/while. In minimal
               only record-literal use `TypeName {`. */
            if (is_uppercase_ident(id, p->src) && at(p, TK_LBRACE)) {
                Node *tn = kai_new_node(N_IDENT, id->line, id->col);
                set_name_from_token(tn, id, p->src);
                return parse_record_lit(p, tn);
            }
            Node *n = kai_new_node(N_IDENT, id->line, id->col);
            set_name_from_token(n, id, p->src);
            return n;
        }

        default:
            error_here(p, "expected expression");
            return NULL;
    }
}

static Node *parse_lambda_from_ident(P *p, Node *ident) {
    int line = ident->line, col = ident->col;
    if (!expect(p, TK_FAT_ARROW, "expected `=>`")) { kai_free_node(ident); return NULL; }
    skip_newlines(p);
    Node *body = parse_expr(p);
    if (!body) { kai_free_node(ident); return NULL; }
    Node *lam = kai_new_node(N_LAMBDA, line, col);
    kai_node_push(lam, body);
    kai_node_push(lam, ident);
    return lam;
}

/* ---------- lists and ranges ---------- */

/* Range tail `.. b` or `.. b .. step`, after `first` and the first `..`
   marker. The `..` has been consumed; `open` carries the `[` line/col. */
static Node *parse_range_tail(P *p, const Token *open, Node *first) {
    skip_newlines(p);
    Node *to = parse_expr(p);
    if (!to) { kai_free_node(first); return NULL; }
    Node *step = NULL;
    if (match(p, TK_DOTDOT)) {
        skip_newlines(p);
        step = parse_expr(p);
        if (!step) { kai_free_node(first); kai_free_node(to); return NULL; }
    }
    if (!expect(p, TK_RBRACKET, "expected `]` after range")) {
        kai_free_node(first); kai_free_node(to); if (step) kai_free_node(step);
        return NULL;
    }
    Node *r = kai_new_node(N_RANGE_LIT, open->line, open->col);
    if (step) r->v.flags |= 0x1;
    kai_node_push(r, first);
    kai_node_push(r, to);
    if (step) kai_node_push(r, step);
    return r;
}

/* Append `expr` to `list`, wrapping it in N_SPREAD when `is_spread`. */
static void push_list_elt(Node *list, Node *expr, int is_spread) {
    if (is_spread) {
        Node *sp = kai_new_node(N_SPREAD, expr->line, expr->col);
        kai_node_push(sp, expr);
        kai_node_push(list, sp);
    } else {
        kai_node_push(list, expr);
    }
}

/* Regular list literal tail, after the already-parsed `first` element. */
static Node *parse_list_tail(P *p, const Token *open, Node *first, int first_is_spread) {
    Node *list = kai_new_node(N_LIST_LIT, open->line, open->col);
    push_list_elt(list, first, first_is_spread);
    while (match(p, TK_COMMA)) {
        skip_newlines(p);
        if (at(p, TK_RBRACKET)) break;
        int spread = match(p, TK_ELLIPSIS);
        Node *e = parse_expr(p);
        if (!e) { kai_free_node(list); return NULL; }
        push_list_elt(list, e, spread);
    }
    skip_newlines(p);
    if (!expect(p, TK_RBRACKET, "expected `]` to close list")) {
        kai_free_node(list); return NULL;
    }
    return list;
}

static Node *parse_list_or_range(P *p) {
    const Token *open = expect(p, TK_LBRACKET, "expected `[`");
    if (!open) return NULL;
    skip_newlines(p);
    /* Empty list. */
    if (match(p, TK_RBRACKET)) {
        return kai_new_node(N_LIST_LIT, open->line, open->col);
    }

    /* Parse the first expression. Could be elem of list, or start of range. */
    int first_is_spread = match(p, TK_ELLIPSIS) ? 1 : 0;
    Node *first = parse_expr(p);
    if (!first) return NULL;

    if (at(p, TK_DOTDOT) && !first_is_spread) {
        p->i++;
        return parse_range_tail(p, open, first);
    }
    return parse_list_tail(p, open, first, first_is_spread);
}

/* ---------- record literal ---------- */

Node *parse_record_lit(P *p, Node *type_ident) {
    int line = type_ident->line, col = type_ident->col;
    if (!expect(p, TK_LBRACE, "expected `{` after record type name")) {
        kai_free_node(type_ident); return NULL;
    }
    Node *rec = kai_new_node(N_RECORD_LIT, line, col);
    rec->name     = type_ident->name;
    rec->name_len = type_ident->name_len;
    kai_free_node(type_ident); /* we copied the span */

    skip_newlines(p);
    if (!at(p, TK_RBRACE)) {
        for (;;) {
            skip_newlines(p);
            const Token *fn = expect(p, TK_IDENT, "expected field name");
            if (!fn) { kai_free_node(rec); return NULL; }
            if (!expect(p, TK_COLON, "expected `:` after field name")) {
                kai_free_node(rec); return NULL;
            }
            Node *val = parse_expr(p);
            if (!val) { kai_free_node(rec); return NULL; }
            Node *fi = kai_new_node(N_FIELD_INIT, fn->line, fn->col);
            set_name_from_token(fi, fn, p->src);
            kai_node_push(fi, val);
            kai_node_push(rec, fi);
            skip_newlines(p);
            if (!match(p, TK_COMMA)) break;
        }
    }
    skip_newlines(p);
    if (!expect(p, TK_RBRACE, "expected `}` to close record")) {
        kai_free_node(rec); return NULL;
    }
    return rec;
}

/* ---------- string literal ---------- */

static Node *parse_string_literal(P *p) {
    const Token *t = &p->toks[p->i++];
    Node *s = kai_new_node(N_STRING, t->line, t->col);
    set_name_from_token(s, t, p->src);
    if (t->length >= 3 && p->src[t->start] == '"' && p->src[t->start + 1] == '"') {
        s->v.flags |= 0x1; /* triple-quoted */
    }
    return s;
}

/* ---------- if / match / block ---------- */

static Node *parse_if(P *p) {
    const Token *t = expect(p, TK_IF, "expected `if`");
    if (!t) return NULL;
    skip_newlines(p);
    Node *cond = parse_expr(p);
    if (!cond) return NULL;
    skip_newlines(p);
    if (!at(p, TK_LBRACE)) {
        error_here(p, "expected `{` after if condition");
        kai_free_node(cond); return NULL;
    }
    Node *then_block = parse_block(p);
    if (!then_block) { kai_free_node(cond); return NULL; }

    Node *if_n = kai_new_node(N_IF, t->line, t->col);
    kai_node_push(if_n, cond);
    kai_node_push(if_n, then_block);

    skip_newlines(p);
    if (at(p, TK_ELSE)) {
        p->i++;
        skip_newlines(p);
        if_n->v.flags |= 0x1;
        Node *else_branch;
        if (at(p, TK_IF)) {
            else_branch = parse_if(p);
        } else {
            if (!at(p, TK_LBRACE)) {
                error_here(p, "expected `{` or `if` after `else`");
                kai_free_node(if_n); return NULL;
            }
            else_branch = parse_block(p);
        }
        if (!else_branch) { kai_free_node(if_n); return NULL; }
        kai_node_push(if_n, else_branch);
    }
    return if_n;
}

static Node *parse_match(P *p) {
    const Token *t = expect(p, TK_MATCH, "expected `match`");
    if (!t) return NULL;
    skip_newlines(p);
    Node *scrutinee = parse_expr(p);
    if (!scrutinee) return NULL;
    skip_newlines(p);
    if (!expect(p, TK_LBRACE, "expected `{` after match scrutinee")) {
        kai_free_node(scrutinee); return NULL;
    }
    Node *m = kai_new_node(N_MATCH, t->line, t->col);
    kai_node_push(m, scrutinee);

    skip_newlines(p);
    while (!at(p, TK_RBRACE) && !at(p, TK_EOF)) {
        int line = peek_tok(p)->line, col = peek_tok(p)->col;
        Node *pat = parse_pattern(p);
        if (!pat) { kai_free_node(m); return NULL; }
        Node *arm = kai_new_node(N_ARM, line, col);
        kai_node_push(arm, pat);
        if (match(p, TK_IF)) {
            arm->v.flags |= 0x1;
            Node *guard = parse_expr(p);
            if (!guard) { kai_free_node(arm); kai_free_node(m); return NULL; }
            kai_node_push(arm, guard);
        }
        if (!expect(p, TK_ARROW, "expected `->` in match arm")) {
            kai_free_node(arm); kai_free_node(m); return NULL;
        }
        skip_newlines(p);
        Node *body;
        if (at(p, TK_LBRACE)) body = parse_block(p);
        else                  body = parse_expr(p);
        if (!body) { kai_free_node(arm); kai_free_node(m); return NULL; }
        kai_node_push(arm, body);
        kai_node_push(m, arm);
        skip_newlines_and_semis(p);
    }
    if (!expect(p, TK_RBRACE, "expected `}` to close match")) {
        kai_free_node(m); return NULL;
    }
    return m;
}

/* Parse one block item into `b`. A `let`/`assert` is a statement; any other
   expression is the block's final value when followed (past newlines/semis)
   by `}`, otherwise an expr-statement. Returns 1 to continue, 0 to stop the
   block loop (final value seen, via *last_expr), or -1 on error. */
static int parse_block_item(P *p, Node *b, Node **last_expr) {
    if (at(p, TK_LET) || at(p, TK_ASSERT)) {
        Node *s = parse_stmt(p);
        if (!s) return -1;
        kai_node_push(b, s);
        return 1;
    }
    int line = peek_tok(p)->line, col = peek_tok(p)->col;
    Node *e = parse_expr(p);
    if (!e) return -1;
    /* Skip newlines/semis to see what follows. */
    size_t save = p->i;
    while (peek_kind(p) == TK_NEWLINE || peek_kind(p) == TK_SEMI) p->i++;
    if (at(p, TK_RBRACE)) {
        *last_expr = e;
        return 0;                 /* final value — stop the loop */
    }
    /* Not final — wrap as an expr statement. */
    p->i = save;
    Node *s = kai_new_node(N_EXPR_STMT, line, col);
    kai_node_push(s, e);
    kai_node_push(b, s);
    return 1;
}

Node *parse_block(P *p) {
    const Token *open = expect(p, TK_LBRACE, "expected `{`");
    if (!open) return NULL;
    Node *b = kai_new_node(N_BLOCK, open->line, open->col);
    skip_newlines_and_semis(p);
    Node *last_expr = NULL;
    while (!at(p, TK_RBRACE) && !at(p, TK_EOF)) {
        int r = parse_block_item(p, b, &last_expr);
        if (r < 0) { kai_free_node(b); return NULL; }
        if (r == 0) break;
        skip_newlines_and_semis(p);
    }
    if (!expect(p, TK_RBRACE, "expected `}` to close block")) {
        kai_free_node(b); return NULL;
    }
    if (last_expr) {
        b->v.flags |= 0x1;
        kai_node_push(b, last_expr);
    } else {
        /* Synthesize a trailing Unit value. */
        Node *u = kai_new_node(N_UNIT, open->line, open->col);
        kai_node_push(b, u);
    }
    return b;
}

/* ---------- patterns ---------- */

/* Parse the body of a record pattern — the `field, field: subpat, ...`
   sequence — appending an N_PAT_FIELD per field to `pr`. On entry the
   opening `{` has already been consumed; on success the closing `}` has
   been consumed too. Returns 1 on success, 0 on error (caller frees `pr`).
   Shared by the bare `{...}` record pattern and the `Ctor{...}` form. */
static int parse_record_pattern_fields(P *p, Node *pr) {
    skip_newlines(p);
    if (!at(p, TK_RBRACE)) {
        for (;;) {
            skip_newlines(p);
            const Token *fn = expect(p, TK_IDENT, "expected field name in record pattern");
            if (!fn) return 0;
            Node *pf = kai_new_node(N_PAT_FIELD, fn->line, fn->col);
            set_name_from_token(pf, fn, p->src);
            if (match(p, TK_COLON)) {
                Node *sub = parse_pattern(p);
                if (!sub) { kai_free_node(pf); return 0; }
                kai_node_push(pf, sub);
            } else {
                /* shorthand: bind field value to its own name */
                pf->v.flags |= 0x1;
                Node *bind = kai_new_node(N_PAT_BIND, fn->line, fn->col);
                set_name_from_token(bind, fn, p->src);
                kai_node_push(pf, bind);
            }
            kai_node_push(pr, pf);
            skip_newlines(p);
            if (!match(p, TK_COMMA)) break;
        }
    }
    skip_newlines(p);
    if (!expect(p, TK_RBRACE, "expected `}` in record pattern")) return 0;
    return 1;
}

/* `[ p, ..., ...rest ]` list pattern. The `[` token `t` is for line/col;
   on entry it has not yet been consumed. */
static Node *parse_list_pattern(P *p, const Token *t) {
    p->i++;
    Node *pl = kai_new_node(N_PAT_LIST, t->line, t->col);
    skip_newlines(p);
    if (!at(p, TK_RBRACKET)) {
        for (;;) {
            skip_newlines(p);
            if (match(p, TK_ELLIPSIS)) {
                pl->v.flags |= 0x1;
                const Token *rn = expect(p, TK_IDENT, "expected identifier after `...` in list pattern");
                if (!rn) { kai_free_node(pl); return NULL; }
                Node *b = kai_new_node(N_PAT_BIND, rn->line, rn->col);
                set_name_from_token(b, rn, p->src);
                kai_node_push(pl, b);
                break; /* rest must be last */
            }
            Node *elt = parse_pattern(p);
            if (!elt) { kai_free_node(pl); return NULL; }
            kai_node_push(pl, elt);
            skip_newlines(p);
            if (!match(p, TK_COMMA)) break;
        }
    }
    skip_newlines(p);
    if (!expect(p, TK_RBRACKET, "expected `]` at end of list pattern")) {
        kai_free_node(pl); return NULL;
    }
    return pl;
}

/* A literal pattern, including the `-number` negative-int form. `t` is the
   first token; on entry it has not yet been consumed. */
static Node *parse_lit_pattern(P *p, const Token *t) {
    Node *lit;
    if (t->kind == TK_MINUS) {
        /* negative integer literal */
        p->i++;
        const Token *numtok = &p->toks[p->i++];
        Node *num = kai_new_node(N_INT, numtok->line, numtok->col);
        num->v.i = -decode_int(p->src + numtok->start, numtok->length);
        num->name = p->src + numtok->start; num->name_len = numtok->length;
        lit = num;
    } else {
        lit = parse_primary(p);
        if (!lit) return NULL;
    }
    Node *pl = kai_new_node(N_PAT_LIT, t->line, t->col);
    kai_node_push(pl, lit);
    return pl;
}

/* True when `t` opens a literal pattern. */
static int starts_lit_pattern(P *p, const Token *t) {
    if (t->kind == TK_INT || t->kind == TK_REAL || t->kind == TK_CHAR ||
        t->kind == TK_STRING || t->kind == TK_TRUE || t->kind == TK_FALSE)
        return 1;
    return t->kind == TK_MINUS && peek_kind_n(p, 1) == TK_INT;
}

/* Parse `Ctor(sub, ...)` variant args into `pv`. The `(` has not yet been
   consumed. Returns 1 on success, 0 on error (caller frees `pv`). */
static int parse_variant_pattern_args(P *p, Node *pv) {
    p->i++;
    skip_newlines(p);
    if (!at(p, TK_RPAREN)) {
        for (;;) {
            skip_newlines(p);
            Node *sub = parse_pattern(p);
            if (!sub) return 0;
            kai_node_push(pv, sub);
            skip_newlines(p);
            if (!match(p, TK_COMMA)) break;
        }
    }
    skip_newlines(p);
    if (!expect(p, TK_RPAREN, "expected `)` to close variant pattern")) return 0;
    return 1;
}

/* An IDENT-led pattern: Ctor(...), Ctor{...}, bare Ctor (0-arg variant), or
   a plain binding. The leading IDENT has not yet been consumed. */
static Node *parse_ident_pattern(P *p) {
    const Token *id = &p->toks[p->i++];
    int upper = is_uppercase_ident(id, p->src);
    if (upper && at(p, TK_LPAREN)) {
        Node *pv = kai_new_node(N_PAT_VARIANT, id->line, id->col);
        set_name_from_token(pv, id, p->src);
        if (!parse_variant_pattern_args(p, pv)) { kai_free_node(pv); return NULL; }
        return pv;
    }
    if (upper && at(p, TK_LBRACE)) {
        /* Variant-record: an N_PAT_RECORD attached to the ctor name via
           N_PAT_VARIANT (carrying the record as its sole child). */
        p->i++;
        Node *pr = kai_new_node(N_PAT_RECORD, id->line, id->col);
        if (!parse_record_pattern_fields(p, pr)) { kai_free_node(pr); return NULL; }
        Node *pv = kai_new_node(N_PAT_VARIANT, id->line, id->col);
        set_name_from_token(pv, id, p->src);
        kai_node_push(pv, pr);
        return pv;
    }
    if (upper) {
        /* Bare constructor name: 0-arg variant. */
        Node *pv = kai_new_node(N_PAT_VARIANT, id->line, id->col);
        set_name_from_token(pv, id, p->src);
        return pv;
    }
    /* Plain identifier binding. */
    Node *b = kai_new_node(N_PAT_BIND, id->line, id->col);
    set_name_from_token(b, id, p->src);
    return b;
}

Node *parse_pattern(P *p) {
    const Token *t = peek_tok(p);

    if (t->kind == TK_UNDERSCORE) {
        p->i++;
        return kai_new_node(N_PAT_WILD, t->line, t->col);
    }
    if (t->kind == TK_LBRACKET) return parse_list_pattern(p, t);
    if (starts_lit_pattern(p, t)) return parse_lit_pattern(p, t);
    if (t->kind == TK_IDENT)    return parse_ident_pattern(p);
    if (t->kind == TK_LBRACE) {
        /* Pure record pattern without ctor. */
        p->i++;
        Node *pr = kai_new_node(N_PAT_RECORD, t->line, t->col);
        if (!parse_record_pattern_fields(p, pr)) { kai_free_node(pr); return NULL; }
        return pr;
    }

    error_here(p, "expected pattern");
    return NULL;
}
