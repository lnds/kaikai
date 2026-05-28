/*
 * Recursive-descent parser for kaikai-minimal.
 *
 * Consumes the Token stream produced by lexer.c and builds an AST of
 * Node values (see ast.h). Newlines are pervasive and treated as soft
 * separators; the parser skips them wherever continuations are allowed.
 *
 * Errors are reported to stderr and set p->had_error. The parser tries
 * to synchronize at the next top-level declaration boundary or newline
 * so multiple errors can surface in a single run, but for stage 0 even
 * first-error-then-stop is acceptable.
 */

#include "parser.h"

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char  *file;
    const char  *src;
    const Token *toks;
    size_t       n;
    size_t       i;
    int          had_error;
} P;

/* Forward decls */
static Node *parse_decl(P *p);
static Node *parse_fn(P *p, int is_pub, int line, int col);
static Node *parse_type_decl(P *p, int is_pub, int line, int col);
static Node *parse_test(P *p, int line, int col);
static Node *parse_import(P *p, int line, int col);

static Node *parse_stmt(P *p);
static Node *parse_let(P *p);
static Node *parse_assert(P *p);

static Node *parse_expr(P *p);
static Node *parse_pipe(P *p);
static Node *parse_or(P *p);
static Node *parse_and(P *p);
static Node *parse_cmp(P *p);
static Node *parse_add(P *p);
static Node *parse_mul(P *p);
static Node *parse_unary(P *p);
static Node *parse_postfix(P *p);
static Node *parse_primary(P *p);

static Node *parse_if(P *p);
static Node *parse_match(P *p);
static Node *parse_block(P *p);
static Node *parse_lambda_from_ident(P *p, Node *ident);
static Node *parse_list_or_range(P *p);
static Node *parse_record_lit(P *p, Node *type_ident);
static Node *parse_string_literal(P *p);

static Node *parse_pattern(P *p);
static Node *parse_type(P *p);

static int is_uppercase_ident(const Token *t, const char *src);

/* ---------- helpers ---------- */

static const Token *peek_tok(P *p) { return &p->toks[p->i]; }
static TokenKind    peek_kind(P *p) { return p->toks[p->i].kind; }
static TokenKind    peek_kind_n(P *p, size_t off) {
    size_t j = p->i + off;
    if (j >= p->n) return TK_EOF;
    return p->toks[j].kind;
}

static void skip_newlines(P *p) {
    while (peek_kind(p) == TK_NEWLINE) p->i++;
}

static void skip_newlines_and_semis(P *p) {
    while (peek_kind(p) == TK_NEWLINE || peek_kind(p) == TK_SEMI) p->i++;
}

static int at(P *p, TokenKind k) { return peek_kind(p) == k; }

static int match(P *p, TokenKind k) {
    if (peek_kind(p) == k) { p->i++; return 1; }
    return 0;
}

static void error_at(P *p, const Token *t, const char *msg) {
    if (!p->had_error) {
        fprintf(stderr, "%s:%d:%d: error: %s\n", p->file, t->line, t->col, msg);
        /* Print the source line with a caret. */
        /* Find line start. */
        size_t s = t->start;
        while (s > 0 && p->src[s - 1] != '\n') s--;
        size_t e = t->start;
        while (e < t->start + t->length) e++;
        while (p->src[e] && p->src[e] != '\n') e++;
        fprintf(stderr, "  %.*s\n", (int)(e - s), p->src + s);
        fprintf(stderr, "  ");
        for (int32_t k = 0; k < t->col - 1; ++k) fputc(' ', stderr);
        fputc('^', stderr);
        for (size_t k = 1; k < t->length; ++k) fputc('~', stderr);
        fputc('\n', stderr);
    }
    p->had_error = 1;
}

static void error_here(P *p, const char *msg) { error_at(p, peek_tok(p), msg); }

static const Token *expect(P *p, TokenKind k, const char *msg) {
    if (peek_kind(p) == k) return &p->toks[p->i++];
    error_here(p, msg);
    return NULL;
}

static int is_uppercase_ident(const Token *t, const char *src) {
    if (t->kind != TK_IDENT || t->length == 0) return 0;
    char c = src[t->start];
    return c >= 'A' && c <= 'Z';
}

/* Copy the source span of a token into a freshly-allocated Node's name field
   as a (start, length) pair pointing into the original source. */
static void set_name_from_token(Node *n, const Token *t, const char *src) {
    n->name     = src + t->start;
    n->name_len = t->length;
}

/* ---------- decoders for literal values ---------- */

static int64_t decode_int(const char *s, size_t len) {
    int64_t v = 0;
    int neg = 0;
    size_t i = 0;
    if (len > 0 && s[0] == '-') { neg = 1; i = 1; }
    for (; i < len; ++i) {
        char c = s[i];
        if (c == '_') continue;
        if (c < '0' || c > '9') break;
        v = v * 10 + (c - '0');
    }
    return neg ? -v : v;
}

static double decode_real(const char *s, size_t len) {
    /* Copy to a temporary buffer removing underscores, then strtod. */
    char buf[128];
    size_t j = 0;
    for (size_t i = 0; i < len && j + 1 < sizeof(buf); ++i) {
        if (s[i] != '_') buf[j++] = s[i];
    }
    buf[j] = '\0';
    return strtod(buf, NULL);
}

/* Decode a `\u{...}` escape body. On entry `p` points at the `u`; reads the
   hex digits between the braces (lenient: stops at `}` or `end`). Returns the
   code point, or 0 if the `{` is missing. */
static uint32_t decode_unicode_escape(const char *p, const char *end) {
    if (p + 1 >= end || p[1] != '{') return 0;
    p += 2;
    uint32_t v = 0;
    while (p < end && *p != '}') {
        char c = *p++;
        v *= 16;
        if      (c >= '0' && c <= '9') v += c - '0';
        else if (c >= 'a' && c <= 'f') v += 10 + c - 'a';
        else if (c >= 'A' && c <= 'F') v += 10 + c - 'A';
    }
    return v;
}

/* Decode a single (possibly multi-byte UTF-8) character starting at `p`.
   Lenient: a malformed lead byte is returned as-is. */
static uint32_t decode_utf8_char(const char *p, const char *end) {
    unsigned char b0 = (unsigned char) *p++;
    if (b0 < 0x80) return b0;
    uint32_t v = 0;
    int extra = 0;
    if      ((b0 & 0xE0) == 0xC0) { v = b0 & 0x1F; extra = 1; }
    else if ((b0 & 0xF0) == 0xE0) { v = b0 & 0x0F; extra = 2; }
    else if ((b0 & 0xF8) == 0xF0) { v = b0 & 0x07; extra = 3; }
    else                          { v = b0; }
    while (extra-- > 0 && p < end) {
        unsigned char b = (unsigned char) *p++;
        v = (v << 6) | (b & 0x3F);
    }
    return v;
}

static uint32_t decode_char(const char *s, size_t len) {
    /* s points at the opening quote; len includes both quotes. */
    if (len < 2) return 0;
    const char *p = s + 1;
    const char *end = s + len - 1;
    if (*p == '\\') {
        p++;
        if (p >= end) return 0;
        switch (*p) {
            case 'n':  return '\n';
            case 't':  return '\t';
            case 'r':  return '\r';
            case '\\': return '\\';
            case '\'': return '\'';
            case '"':  return '"';
            case '0':  return 0;
            case 'u':  return decode_unicode_escape(p, end);
            default:   return (unsigned char) *p;
        }
    }
    /* Otherwise, single byte or UTF-8 (decoded leniently). */
    return decode_utf8_char(p, end);
}

/* ---------- entry ---------- */

Node *kai_parse_expr_standalone(const char *file, const char *src,
                                const Token *toks, size_t n) {
    P p;
    p.file      = file;
    p.src       = src;
    p.toks      = toks;
    p.n         = n;
    p.i         = 0;
    p.had_error = 0;
    skip_newlines(&p);
    Node *e = parse_expr(&p);
    if (p.had_error) { kai_free_node(e); return NULL; }
    return e;
}

Node *kai_parse(const char *file, const char *src,
                const Token *toks, size_t n) {
    P p;
    p.file      = file;
    p.src       = src;
    p.toks      = toks;
    p.n         = n;
    p.i         = 0;
    p.had_error = 0;

    Node *prog = kai_new_node(N_PROGRAM, 1, 1);

    skip_newlines_and_semis(&p);
    while (!at(&p, TK_EOF)) {
        Node *d = parse_decl(&p);
        if (!d) {
            /* Synchronize: skip to next newline + attempt to continue. */
            while (!at(&p, TK_EOF) && !at(&p, TK_NEWLINE)) p.i++;
            skip_newlines_and_semis(&p);
            continue;
        }
        kai_node_push(prog, d);
        skip_newlines_and_semis(&p);
    }

    if (p.had_error) {
        kai_free_node(prog);
        return NULL;
    }
    return prog;
}

/* ---------- declarations ---------- */

static Node *parse_decl(P *p) {
    const Token *t = peek_tok(p);
    int is_pub = 0;
    if (match(p, TK_PUB)) is_pub = 1;

    const Token *head = peek_tok(p);
    switch (head->kind) {
        case TK_FN:        p->i++; return parse_fn(p, is_pub, t->line, t->col);
        case TK_TYPE:      p->i++; return parse_type_decl(p, is_pub, t->line, t->col);
        case TK_TEST:
            if (is_pub) { error_here(p, "`pub` not allowed on `test`"); return NULL; }
            p->i++; return parse_test(p, t->line, t->col);
        case TK_IMPORT:
            if (is_pub) { error_here(p, "`pub` not allowed on `import`"); return NULL; }
            p->i++; return parse_import(p, t->line, t->col);
        default:
            error_here(p, "expected top-level declaration (fn, type, test, or import)");
            return NULL;
    }
}

static Node *parse_fn(P *p, int is_pub, int line, int col) {
    Node *fn = kai_new_node(N_FN, line, col);
    fn->v.flags = is_pub ? 0x1 : 0;

    const Token *name = expect(p, TK_IDENT, "expected function name after `fn`");
    if (!name) { kai_free_node(fn); return NULL; }
    set_name_from_token(fn, name, p->src);

    if (!expect(p, TK_LPAREN, "expected `(` after function name")) {
        kai_free_node(fn); return NULL;
    }

    /* Placeholders: [0]=return_type, [1]=body; params append after.
       We fill them later; push NULL stubs for now. */
    kai_node_push(fn, NULL); /* return_type */
    kai_node_push(fn, NULL); /* body */

    skip_newlines(p);
    if (!at(p, TK_RPAREN)) {
        for (;;) {
            skip_newlines(p);
            const Token *pn = expect(p, TK_IDENT, "expected parameter name");
            if (!pn) { kai_free_node(fn); return NULL; }
            if (!expect(p, TK_COLON, "expected `:` after parameter name")) {
                kai_free_node(fn); return NULL;
            }
            Node *ptype = parse_type(p);
            if (!ptype) { kai_free_node(fn); return NULL; }
            Node *param = kai_new_node(N_PARAM, pn->line, pn->col);
            set_name_from_token(param, pn, p->src);
            kai_node_push(param, ptype);
            kai_node_push(fn, param);
            skip_newlines(p);
            if (!match(p, TK_COMMA)) break;
        }
    }
    if (!expect(p, TK_RPAREN, "expected `)` or `,` after parameter")) {
        kai_free_node(fn); return NULL;
    }

    /* Optional return type. */
    Node *ret_ty;
    if (match(p, TK_COLON)) {
        ret_ty = parse_type(p);
        if (!ret_ty) { kai_free_node(fn); return NULL; }
    } else {
        /* When omitted, synthesize N_TY_NAME "Unit". */
        ret_ty = kai_new_node(N_TY_NAME, line, col);
        ret_ty->name = "Unit"; ret_ty->name_len = 4;
        fn->v.flags |= 0x4;        /* ret_omitted */
    }
    fn->children[0] = ret_ty;

    /* Body: `= expr`  OR  `{ block }`. */
    skip_newlines(p);
    Node *body;
    if (match(p, TK_EQ)) {
        fn->v.flags |= 0x2;        /* body_is_expr */
        skip_newlines(p);
        body = parse_expr(p);
    } else if (at(p, TK_LBRACE)) {
        body = parse_block(p);
    } else {
        error_here(p, "expected `=` or `{` for function body");
        kai_free_node(fn); return NULL;
    }
    if (!body) { kai_free_node(fn); return NULL; }
    fn->children[1] = body;

    return fn;
}

/* Parse the optional `[a, b, c]` type-parameter list of a type decl,
   pushing one N_IDENT per parameter onto `td`. On entry the `[` has not
   yet been consumed; this is called only when the caller has matched it.
   Returns 1 on success, 0 on error (caller frees `td`). */
static int parse_type_params(P *p, Node *td) {
    skip_newlines(p);
    for (;;) {
        const Token *tp = expect(p, TK_IDENT, "expected type parameter name");
        if (!tp) return 0;
        Node *id = kai_new_node(N_IDENT, tp->line, tp->col);
        set_name_from_token(id, tp, p->src);
        kai_node_push(td, id);
        skip_newlines(p);
        if (!match(p, TK_COMMA)) break;
        skip_newlines(p);
    }
    if (!expect(p, TK_RBRACKET, "expected `]` after type parameters")) return 0;
    skip_newlines(p);
    return 1;
}

/* Parse a record type body `{ field: T, ... }` into an N_TY_RECORD node.
   On entry the opening `{` has already been consumed. Returns the body
   node, or NULL on error (caller frees `td`). */
static Node *parse_record_type_body(P *p, int line, int col) {
    Node *body = kai_new_node(N_TY_RECORD, line, col);
    skip_newlines(p);
    if (!at(p, TK_RBRACE)) {
        for (;;) {
            skip_newlines(p);
            const Token *fn_ = expect(p, TK_IDENT, "expected field name");
            if (!fn_) { kai_free_node(body); return NULL; }
            if (!expect(p, TK_COLON, "expected `:` after field name")) {
                kai_free_node(body); return NULL;
            }
            Node *ftype = parse_type(p);
            if (!ftype) { kai_free_node(body); return NULL; }
            Node *fd = kai_new_node(N_FIELD_DECL, fn_->line, fn_->col);
            set_name_from_token(fd, fn_, p->src);
            kai_node_push(fd, ftype);
            kai_node_push(body, fd);
            skip_newlines(p);
            if (!match(p, TK_COMMA)) break;
        }
    }
    skip_newlines(p);
    if (!expect(p, TK_RBRACE, "expected `}` at end of record type")) {
        kai_free_node(body); return NULL;
    }
    return body;
}

/* Decide whether the tokens after `=` introduce a sum type rather than an
   alias. A bare `Uppercase(` unambiguously marks a variant with arguments;
   otherwise the sum form requires a `|` at brace depth 0 before the decl
   ends (a single TypeName is indistinguishable from an alias). Does not
   advance `p`. */
static int looks_like_sum_ahead(P *p) {
    const Token *head = peek_tok(p);
    if (!is_uppercase_ident(head, p->src)) return 0;

    const Token *after_head = (p->i + 1 < p->n) ? &p->toks[p->i + 1] : NULL;
    if (after_head && after_head->kind == TK_LPAREN) return 1;

    /* Scan for a `|` before the decl ends, tracking bracket depth so a `|`
       inside a function type is not mistaken for the variant separator. */
    size_t j = p->i;
    int depth = 0;
    while (j < p->n) {
        TokenKind k = p->toks[j].kind;
        if (k == TK_EOF) break;
        if (k == TK_LPAREN || k == TK_LBRACKET || k == TK_LBRACE) depth++;
        else if (k == TK_RPAREN || k == TK_RBRACKET || k == TK_RBRACE) depth--;
        else if (depth == 0) {
            if (k == TK_PIPE) return 1;
            if (k == TK_NEWLINE) {
                /* Newline between variants is allowed: peek past newlines
                   and treat it as sum only if a `|` follows. */
                size_t m = j + 1;
                while (m < p->n && p->toks[m].kind == TK_NEWLINE) m++;
                return (m < p->n && p->toks[m].kind == TK_PIPE) ? 1 : 0;
            }
        }
        j++;
    }
    return 0;
}

/* Parse a sum type body `Ctor | Ctor(T, ...) | ...` into an N_TY_SUM node.
   Returns the body node, or NULL on error (caller frees `td`). */
static Node *parse_sum_type_body(P *p, int line, int col) {
    Node *body = kai_new_node(N_TY_SUM, line, col);
    skip_newlines(p);
    for (;;) {
        skip_newlines(p);
        const Token *cn = expect(p, TK_IDENT, "expected variant constructor name");
        if (!cn) { kai_free_node(body); return NULL; }
        if (!is_uppercase_ident(cn, p->src)) {
            error_at(p, cn, "variant constructor must start with uppercase");
            kai_free_node(body); return NULL;
        }
        Node *var = kai_new_node(N_VARIANT, cn->line, cn->col);
        set_name_from_token(var, cn, p->src);
        if (match(p, TK_LPAREN)) {
            skip_newlines(p);
            if (!at(p, TK_RPAREN)) {
                for (;;) {
                    Node *ty = parse_type(p);
                    if (!ty) { kai_free_node(var); kai_free_node(body); return NULL; }
                    kai_node_push(var, ty);
                    skip_newlines(p);
                    if (!match(p, TK_COMMA)) break;
                    skip_newlines(p);
                }
            }
            if (!expect(p, TK_RPAREN, "expected `)` after variant fields")) {
                kai_free_node(var); kai_free_node(body); return NULL;
            }
        }
        kai_node_push(body, var);
        skip_newlines(p);
        if (!match(p, TK_PIPE)) break;
        skip_newlines(p);
    }
    return body;
}

static Node *parse_type_decl(P *p, int is_pub, int line, int col) {
    Node *td = kai_new_node(N_TYPE_DECL, line, col);
    td->v.flags = is_pub ? 0x1 : 0;

    const Token *name = expect(p, TK_IDENT, "expected type name after `type`");
    if (!name) { kai_free_node(td); return NULL; }
    if (!is_uppercase_ident(name, p->src)) {
        error_at(p, name, "type name must start with an uppercase letter");
        kai_free_node(td); return NULL;
    }
    set_name_from_token(td, name, p->src);

    /* Placeholder for body. */
    kai_node_push(td, NULL);

    /* After the name, newline is whitespace because the declaration
       cannot be complete here: it must be followed by `[` (type params)
       or `=`. */
    skip_newlines(p);

    /* Optional type parameters: [a, b, c] */
    if (match(p, TK_LBRACKET)) {
        if (!parse_type_params(p, td)) { kai_free_node(td); return NULL; }
    }

    if (!expect(p, TK_EQ, "expected `=` in type declaration")) {
        kai_free_node(td); return NULL;
    }
    skip_newlines(p);

    Node *body;
    if (at(p, TK_LBRACE)) {
        /* Record: `{ ... }` */
        p->i++;
        body = parse_record_type_body(p, line, col);
        if (!body) { kai_free_node(td); return NULL; }
    } else {
        /* Either a sum type or an alias — decide with bounded lookahead
           that does not consume tokens. */
        size_t save = p->i;
        skip_newlines(p);
        int is_sum = looks_like_sum_ahead(p);
        p->i = save;

        if (is_sum) {
            skip_newlines(p);
            body = parse_sum_type_body(p, line, col);
            if (!body) { kai_free_node(td); return NULL; }
        } else {
            /* Alias to a regular Type expression. */
            Node *aliased = parse_type(p);
            if (!aliased) { kai_free_node(td); return NULL; }
            body = kai_new_node(N_TY_ALIAS, line, col);
            kai_node_push(body, aliased);
        }
    }

    td->children[0] = body;
    return td;
}

static Node *parse_test(P *p, int line, int col) {
    Node *t = kai_new_node(N_TEST, line, col);
    const Token *desc = expect(p, TK_STRING, "expected description string after `test`");
    if (!desc) { kai_free_node(t); return NULL; }
    set_name_from_token(t, desc, p->src);
    skip_newlines(p);
    if (!at(p, TK_LBRACE)) {
        error_here(p, "expected `{` for test body");
        kai_free_node(t); return NULL;
    }
    Node *body = parse_block(p);
    if (!body) { kai_free_node(t); return NULL; }
    kai_node_push(t, body);
    return t;
}

static Node *parse_import(P *p, int line, int col) {
    Node *imp = kai_new_node(N_IMPORT, line, col);
    /* Parse dotted module path. */
    const Token *first = expect(p, TK_IDENT, "expected module name after `import`");
    if (!first) { kai_free_node(imp); return NULL; }

    size_t start = first->start;
    size_t end   = first->start + first->length;
    while (at(p, TK_DOT) && peek_kind_n(p, 1) == TK_IDENT) {
        p->i++;
        const Token *n = &p->toks[p->i++];
        end = n->start + n->length;
    }
    imp->name     = p->src + start;
    imp->name_len = end - start;

    /* Optional alias or selection. */
    if (match(p, TK_AS)) {
        const Token *alias = expect(p, TK_IDENT, "expected alias name after `as`");
        if (!alias) { kai_free_node(imp); return NULL; }
        imp->v.flags |= 0x1;
        Node *a = kai_new_node(N_IDENT, alias->line, alias->col);
        set_name_from_token(a, alias, p->src);
        kai_node_push(imp, a);
    } else if (match(p, TK_DOT)) {
        if (!expect(p, TK_LBRACE, "expected `{` after `.`")) {
            kai_free_node(imp); return NULL;
        }
        skip_newlines(p);
        for (;;) {
            const Token *s = expect(p, TK_IDENT, "expected imported name");
            if (!s) { kai_free_node(imp); return NULL; }
            Node *n = kai_new_node(N_IDENT, s->line, s->col);
            set_name_from_token(n, s, p->src);
            kai_node_push(imp, n);
            skip_newlines(p);
            if (!match(p, TK_COMMA)) break;
            skip_newlines(p);
        }
        if (!expect(p, TK_RBRACE, "expected `}` to close import selection")) {
            kai_free_node(imp); return NULL;
        }
    }
    return imp;
}

/* ---------- types ---------- */

static Node *parse_type(P *p) {
    /* Forms:
         [T]
         ( T , ... ) -> T          (function type)
         IDENT                      (named type)
         IDENT [ T , ... ]          (generic named type)
    */
    const Token *t = peek_tok(p);
    if (match(p, TK_LBRACKET)) {
        Node *inner = parse_type(p);
        if (!inner) return NULL;
        if (!expect(p, TK_RBRACKET, "expected `]`")) return NULL;
        Node *n = kai_new_node(N_TY_LIST, t->line, t->col);
        kai_node_push(n, inner);
        return n;
    }
    if (match(p, TK_LPAREN)) {
        Node *fn = kai_new_node(N_TY_FN, t->line, t->col);
        kai_node_push(fn, NULL); /* return type placeholder */
        skip_newlines(p);
        if (!at(p, TK_RPAREN)) {
            for (;;) {
                Node *ty = parse_type(p);
                if (!ty) { kai_free_node(fn); return NULL; }
                kai_node_push(fn, ty);
                skip_newlines(p);
                if (!match(p, TK_COMMA)) break;
                skip_newlines(p);
            }
        }
        if (!expect(p, TK_RPAREN, "expected `)`")) { kai_free_node(fn); return NULL; }
        if (!expect(p, TK_ARROW, "expected `->` in function type")) {
            kai_free_node(fn); return NULL;
        }
        Node *ret = parse_type(p);
        if (!ret) { kai_free_node(fn); return NULL; }
        fn->children[0] = ret;
        return fn;
    }
    if (at(p, TK_IDENT)) {
        const Token *id = &p->toks[p->i++];
        Node *n = kai_new_node(N_TY_NAME, id->line, id->col);
        set_name_from_token(n, id, p->src);
        if (match(p, TK_LBRACKET)) {
            skip_newlines(p);
            for (;;) {
                Node *arg = parse_type(p);
                if (!arg) { kai_free_node(n); return NULL; }
                kai_node_push(n, arg);
                skip_newlines(p);
                if (!match(p, TK_COMMA)) break;
                skip_newlines(p);
            }
            if (!expect(p, TK_RBRACKET, "expected `]` after generic arguments")) {
                kai_free_node(n); return NULL;
            }
        }
        return n;
    }
    error_here(p, "expected type");
    return NULL;
}

/* ---------- statements ---------- */

static Node *parse_stmt(P *p) {
    if (at(p, TK_LET))    { p->i++; return parse_let(p); }
    if (at(p, TK_ASSERT)) { p->i++; return parse_assert(p); }
    /* Otherwise, an expression statement. */
    int line = peek_tok(p)->line, col = peek_tok(p)->col;
    Node *e = parse_expr(p);
    if (!e) return NULL;
    Node *s = kai_new_node(N_EXPR_STMT, line, col);
    kai_node_push(s, e);
    return s;
}

static Node *parse_let(P *p) {
    int line = p->toks[p->i - 1].line, col = p->toks[p->i - 1].col;
    Node *let = kai_new_node(N_LET, line, col);
    Node *pat = parse_pattern(p);
    if (!pat) { kai_free_node(let); return NULL; }
    kai_node_push(let, pat);

    /* Optional type annotation. */
    if (match(p, TK_COLON)) {
        let->v.flags |= 0x1;
        Node *ty = parse_type(p);
        if (!ty) { kai_free_node(let); return NULL; }
        kai_node_push(let, ty);
    } else {
        Node *ty = kai_new_node(N_TY_INFER, line, col);
        kai_node_push(let, ty);
    }

    if (!expect(p, TK_EQ, "expected `=` in let binding")) {
        kai_free_node(let); return NULL;
    }
    skip_newlines(p);
    Node *e = parse_expr(p);
    if (!e) { kai_free_node(let); return NULL; }
    kai_node_push(let, e);
    return let;
}

static Node *parse_assert(P *p) {
    int line = p->toks[p->i - 1].line, col = p->toks[p->i - 1].col;
    Node *a = kai_new_node(N_ASSERT, line, col);
    Node *cond = parse_expr(p);
    if (!cond) { kai_free_node(a); return NULL; }
    kai_node_push(a, cond);
    if (match(p, TK_COMMA)) {
        a->v.flags |= 0x1;
        Node *msg = parse_expr(p);
        if (!msg) { kai_free_node(a); return NULL; }
        kai_node_push(a, msg);
    }
    return a;
}

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

static Node *parse_expr(P *p) { return parse_pipe(p); }

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

static Node *parse_postfix(P *p) {
    Node *lhs = parse_primary(p);
    if (!lhs) return NULL;
    for (;;) {
        const Token *t = peek_tok(p);
        if (t->kind == TK_LPAREN) {
            /* Call. */
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
            lhs = call;
        } else if (t->kind == TK_DOT && peek_kind_n(p, 1) == TK_IDENT) {
            p->i++;
            const Token *fn = &p->toks[p->i++];
            Node *f = kai_new_node(N_FIELD, t->line, t->col);
            set_name_from_token(f, fn, p->src);
            kai_node_push(f, lhs);
            lhs = f;
        } else if (t->kind == TK_LBRACKET) {
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
            lhs = n;
        } else {
            break;
        }
    }
    return lhs;
}

/* ---------- primary expressions ---------- */

/* Parse a parenthesized primary: `()` Unit, `(expr)` grouping, or a
   parameter-list lambda `(a, b) => body` (including the zero-arg `() =>`).
   On entry `t` is the `(` token (for line/col); the `(` has NOT been
   consumed yet. */
static Node *parse_paren_expr(P *p, const Token *t) {
    p->i++;
    /* Detect `()` */
    if (at(p, TK_RPAREN)) {
        p->i++;
        /* Could still be `() => expr` (zero-arg lambda). */
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
    /* Save state to try lambda shape first if it starts with IDENT. */
    size_t save = p->i;
    int could_be_lambda = 0;
    if (at(p, TK_IDENT)) {
        /* Scan IDENT (, IDENT)* ) => */
        size_t j = p->i;
        for (;;) {
            if (p->toks[j].kind != TK_IDENT) { could_be_lambda = 0; break; }
            j++;
            if (p->toks[j].kind == TK_RPAREN) {
                if (p->toks[j + 1].kind == TK_FAT_ARROW) could_be_lambda = 1;
                break;
            }
            if (p->toks[j].kind != TK_COMMA) { could_be_lambda = 0; break; }
            j++;
            while (p->toks[j].kind == TK_NEWLINE) j++;
        }
    }
    if (could_be_lambda) {
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
    /* Otherwise: plain parenthesized expression. */
    p->i = save;
    skip_newlines(p);
    Node *e = parse_expr(p);
    if (!e) return NULL;
    if (!expect(p, TK_RPAREN, "expected `)`")) { kai_free_node(e); return NULL; }
    return e;
}

static Node *parse_primary(P *p) {
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

static Node *parse_list_or_range(P *p) {
    const Token *open = expect(p, TK_LBRACKET, "expected `[`");
    if (!open) return NULL;
    skip_newlines(p);
    /* Empty list. */
    if (match(p, TK_RBRACKET)) {
        return kai_new_node(N_LIST_LIT, open->line, open->col);
    }

    /* Parse the first expression. Could be elem of list, or start of range. */
    Node *first = NULL;
    int first_is_spread = 0;
    if (match(p, TK_ELLIPSIS)) {
        first_is_spread = 1;
        first = parse_expr(p);
    } else {
        first = parse_expr(p);
    }
    if (!first) return NULL;

    /* Range: `[a .. b]` or `[a .. b .. step]` */
    if (at(p, TK_DOTDOT) && !first_is_spread) {
        p->i++;
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

    /* Regular list literal. */
    Node *list = kai_new_node(N_LIST_LIT, open->line, open->col);
    if (first_is_spread) {
        Node *sp = kai_new_node(N_SPREAD, first->line, first->col);
        kai_node_push(sp, first);
        kai_node_push(list, sp);
    } else {
        kai_node_push(list, first);
    }
    while (match(p, TK_COMMA)) {
        skip_newlines(p);
        if (at(p, TK_RBRACKET)) break;
        if (match(p, TK_ELLIPSIS)) {
            Node *inner = parse_expr(p);
            if (!inner) { kai_free_node(list); return NULL; }
            Node *sp = kai_new_node(N_SPREAD, inner->line, inner->col);
            kai_node_push(sp, inner);
            kai_node_push(list, sp);
        } else {
            Node *e = parse_expr(p);
            if (!e) { kai_free_node(list); return NULL; }
            kai_node_push(list, e);
        }
    }
    skip_newlines(p);
    if (!expect(p, TK_RBRACKET, "expected `]` to close list")) {
        kai_free_node(list); return NULL;
    }
    return list;
}

/* ---------- record literal ---------- */

static Node *parse_record_lit(P *p, Node *type_ident) {
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

static Node *parse_block(P *p) {
    const Token *open = expect(p, TK_LBRACE, "expected `{`");
    if (!open) return NULL;
    Node *b = kai_new_node(N_BLOCK, open->line, open->col);
    skip_newlines_and_semis(p);
    Node *last_expr = NULL;
    while (!at(p, TK_RBRACE) && !at(p, TK_EOF)) {
        /* A block item is either a let/assert statement or an expression.
           If we're on `let` or `assert`, it's a stmt; otherwise parse expr
           then decide: if followed by newline-then-`}` or just `}`, it's
           the final value; otherwise it's an expr-stmt. */
        if (at(p, TK_LET) || at(p, TK_ASSERT)) {
            Node *s = parse_stmt(p);
            if (!s) { kai_free_node(b); return NULL; }
            kai_node_push(b, s);
        } else {
            int line = peek_tok(p)->line, col = peek_tok(p)->col;
            Node *e = parse_expr(p);
            if (!e) { kai_free_node(b); return NULL; }
            /* Determine if this is the final expression or a statement. */
            /* Skip newlines/semis to see what follows. */
            size_t save = p->i;
            while (peek_kind(p) == TK_NEWLINE || peek_kind(p) == TK_SEMI) p->i++;
            if (at(p, TK_RBRACE)) {
                last_expr = e;
                (void) line; (void) col;
                break;
            }
            /* Not final — wrap as an expr statement. */
            p->i = save;
            Node *s = kai_new_node(N_EXPR_STMT, line, col);
            kai_node_push(s, e);
            kai_node_push(b, s);
        }
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

static Node *parse_pattern(P *p) {
    const Token *t = peek_tok(p);

    if (t->kind == TK_UNDERSCORE) {
        p->i++;
        return kai_new_node(N_PAT_WILD, t->line, t->col);
    }
    if (t->kind == TK_LBRACKET) {
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
    if (t->kind == TK_INT || t->kind == TK_REAL || t->kind == TK_CHAR ||
        t->kind == TK_STRING || t->kind == TK_TRUE || t->kind == TK_FALSE ||
        (t->kind == TK_MINUS && peek_kind_n(p, 1) == TK_INT)) {
        /* Literal (or -number) */
        Node *lit;
        if (t->kind == TK_MINUS) {
            /* negative integer literal */
            int line = t->line, col = t->col;
            p->i++;
            const Token *numtok = &p->toks[p->i++];
            Node *num = kai_new_node(N_INT, numtok->line, numtok->col);
            num->v.i = -decode_int(p->src + numtok->start, numtok->length);
            num->name = p->src + numtok->start; num->name_len = numtok->length;
            lit = num;
            (void) line; (void) col;
        } else {
            lit = parse_primary(p);
            if (!lit) return NULL;
        }
        Node *pl = kai_new_node(N_PAT_LIT, t->line, t->col);
        kai_node_push(pl, lit);
        return pl;
    }
    if (t->kind == TK_IDENT) {
        /* Could be: ConstructorName(...) variant, ConstructorName{...} variant-record,
                    a plain binding, or `{...}` record pattern (no ctor). */
        const Token *id = &p->toks[p->i++];
        if (is_uppercase_ident(id, p->src) && at(p, TK_LPAREN)) {
            p->i++;
            Node *pv = kai_new_node(N_PAT_VARIANT, id->line, id->col);
            set_name_from_token(pv, id, p->src);
            skip_newlines(p);
            if (!at(p, TK_RPAREN)) {
                for (;;) {
                    skip_newlines(p);
                    Node *sub = parse_pattern(p);
                    if (!sub) { kai_free_node(pv); return NULL; }
                    kai_node_push(pv, sub);
                    skip_newlines(p);
                    if (!match(p, TK_COMMA)) break;
                }
            }
            skip_newlines(p);
            if (!expect(p, TK_RPAREN, "expected `)` to close variant pattern")) {
                kai_free_node(pv); return NULL;
            }
            return pv;
        }
        if (is_uppercase_ident(id, p->src) && at(p, TK_LBRACE)) {
            /* Variant-record or plain-record-with-ctor: we tag as variant
               with record children. For minimal we support only record
               destructuring with field names; we use N_PAT_RECORD attached
               to a ctor name via N_PAT_VARIANT (carrying N_PAT_RECORD as
               sole child). */
            p->i++;
            Node *pr = kai_new_node(N_PAT_RECORD, id->line, id->col);
            if (!parse_record_pattern_fields(p, pr)) { kai_free_node(pr); return NULL; }
            Node *pv = kai_new_node(N_PAT_VARIANT, id->line, id->col);
            set_name_from_token(pv, id, p->src);
            kai_node_push(pv, pr);
            return pv;
        }
        /* Bare constructor name: 0-arg variant. */
        if (is_uppercase_ident(id, p->src)) {
            Node *pv = kai_new_node(N_PAT_VARIANT, id->line, id->col);
            set_name_from_token(pv, id, p->src);
            return pv;
        }
        /* Plain identifier binding. */
        Node *b = kai_new_node(N_PAT_BIND, id->line, id->col);
        set_name_from_token(b, id, p->src);
        return b;
    }
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
