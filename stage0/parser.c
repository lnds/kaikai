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
#include "parser_internal.h"

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward decls — declarations / statements internal to this unit. */
static Node *parse_decl(P *p);
static Node *parse_fn(P *p, int is_pub, int line, int col);
static Node *parse_type_decl(P *p, int is_pub, int line, int col);
static Node *parse_test(P *p, int line, int col);
static Node *parse_import(P *p, int line, int col);

static Node *parse_let(P *p);
static Node *parse_assert(P *p);

/* ---------- helpers (shared via parser_internal.h) ---------- */

const Token *peek_tok(P *p) { return &p->toks[p->i]; }
TokenKind    peek_kind(P *p) { return p->toks[p->i].kind; }
TokenKind    peek_kind_n(P *p, size_t off) {
    size_t j = p->i + off;
    if (j >= p->n) return TK_EOF;
    return p->toks[j].kind;
}

void skip_newlines(P *p) {
    while (peek_kind(p) == TK_NEWLINE) p->i++;
}

void skip_newlines_and_semis(P *p) {
    while (peek_kind(p) == TK_NEWLINE || peek_kind(p) == TK_SEMI) p->i++;
}

int at(P *p, TokenKind k) { return peek_kind(p) == k; }

int match(P *p, TokenKind k) {
    if (peek_kind(p) == k) { p->i++; return 1; }
    return 0;
}

void error_at(P *p, const Token *t, const char *msg) {
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

void error_here(P *p, const char *msg) { error_at(p, peek_tok(p), msg); }

const Token *expect(P *p, TokenKind k, const char *msg) {
    if (peek_kind(p) == k) return &p->toks[p->i++];
    error_here(p, msg);
    return NULL;
}

int is_uppercase_ident(const Token *t, const char *src) {
    if (t->kind != TK_IDENT || t->length == 0) return 0;
    char c = src[t->start];
    return c >= 'A' && c <= 'Z';
}

/* Copy the source span of a token into a freshly-allocated Node's name field
   as a (start, length) pair pointing into the original source. */
void set_name_from_token(Node *n, const Token *t, const char *src) {
    n->name     = src + t->start;
    n->name_len = t->length;
}

/* ---------- decoders for literal values ---------- */

int64_t decode_int(const char *s, size_t len) {
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

double decode_real(const char *s, size_t len) {
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

uint32_t decode_char(const char *s, size_t len) {
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

/* Parse the `( name: T, ... )` parameter list, appending one N_PARAM per
   parameter onto `fn`. The `(` has already been consumed. Returns 1 on
   success, 0 on error (caller frees `fn`). */
static int parse_fn_params(P *p, Node *fn) {
    skip_newlines(p);
    if (!at(p, TK_RPAREN)) {
        for (;;) {
            skip_newlines(p);
            const Token *pn = expect(p, TK_IDENT, "expected parameter name");
            if (!pn) return 0;
            if (!expect(p, TK_COLON, "expected `:` after parameter name")) return 0;
            Node *ptype = parse_type(p);
            if (!ptype) return 0;
            Node *param = kai_new_node(N_PARAM, pn->line, pn->col);
            set_name_from_token(param, pn, p->src);
            kai_node_push(param, ptype);
            kai_node_push(fn, param);
            skip_newlines(p);
            if (!match(p, TK_COMMA)) break;
        }
    }
    return expect(p, TK_RPAREN, "expected `)` or `,` after parameter") != NULL;
}

/* Parse the optional `: RetType` and the body (`= expr` or `{ block }`),
   filling fn->children[0] (return type) and [1] (body). The closing `)` of
   the parameter list has already been consumed. Returns 1 on success, 0 on
   error (caller frees `fn`). */
static int parse_fn_ret_and_body(P *p, Node *fn, int line, int col) {
    Node *ret_ty;
    if (match(p, TK_COLON)) {
        ret_ty = parse_type(p);
        if (!ret_ty) return 0;
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
        return 0;
    }
    if (!body) return 0;
    fn->children[1] = body;
    return 1;
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

    /* Placeholders: [0]=return_type, [1]=body; params append after. */
    kai_node_push(fn, NULL); /* return_type */
    kai_node_push(fn, NULL); /* body */

    if (!parse_fn_params(p, fn))               { kai_free_node(fn); return NULL; }
    if (!parse_fn_ret_and_body(p, fn, line, col)) { kai_free_node(fn); return NULL; }
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
/* Parse one sum variant: `Ctor` or `Ctor(T, ...)`. Returns the N_VARIANT,
   or NULL on error (the caller frees the enclosing sum body). */
static Node *parse_variant(P *p) {
    const Token *cn = expect(p, TK_IDENT, "expected variant constructor name");
    if (!cn) return NULL;
    if (!is_uppercase_ident(cn, p->src)) {
        error_at(p, cn, "variant constructor must start with uppercase");
        return NULL;
    }
    Node *var = kai_new_node(N_VARIANT, cn->line, cn->col);
    set_name_from_token(var, cn, p->src);
    if (match(p, TK_LPAREN)) {
        skip_newlines(p);
        if (!at(p, TK_RPAREN)) {
            for (;;) {
                Node *ty = parse_type(p);
                if (!ty) { kai_free_node(var); return NULL; }
                kai_node_push(var, ty);
                skip_newlines(p);
                if (!match(p, TK_COMMA)) break;
                skip_newlines(p);
            }
        }
        if (!expect(p, TK_RPAREN, "expected `)` after variant fields")) {
            kai_free_node(var); return NULL;
        }
    }
    return var;
}

static Node *parse_sum_type_body(P *p, int line, int col) {
    Node *body = kai_new_node(N_TY_SUM, line, col);
    skip_newlines(p);
    for (;;) {
        skip_newlines(p);
        Node *var = parse_variant(p);
        if (!var) { kai_free_node(body); return NULL; }
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

/* Function type `( T, ... ) -> T`. The `(` has already been matched; `t`
   carries its line/col. */
static Node *parse_fn_type(P *p, const Token *t) {
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

/* Named type `IDENT` or generic `IDENT[ T, ... ]`. The leading IDENT has
   not yet been consumed. */
static Node *parse_named_type(P *p) {
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

Node *parse_type(P *p) {
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
    if (match(p, TK_LPAREN)) return parse_fn_type(p, t);
    if (at(p, TK_IDENT))     return parse_named_type(p);
    error_here(p, "expected type");
    return NULL;
}

/* ---------- statements ---------- */

Node *parse_stmt(P *p) {
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

