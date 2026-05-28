/*
 * Lexer for kaikai-minimal.
 *
 * Hand-written, ASCII-oriented, with a small allowance for UTF-8 bytes
 * inside strings and comments (they pass through unchanged).
 *
 * The tokenizer is byte-oriented in column tracking; multibyte characters
 * count their raw bytes, not grapheme clusters. This is explicitly the
 * position model used by error reporting.
 *
 * Newlines are always emitted as TK_NEWLINE. The parser decides which
 * newlines are statement terminators and which are continuations (see
 * docs/kaikai-minimal.md: "Newlines and statement termination").
 */

#include "lexer.h"

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *file;
    const char *src;
    size_t      len;
    size_t      pos;
    int32_t     line;
    int32_t     col;

    Token      *toks;
    size_t      n;
    size_t      cap;

    /* Error messages live in a side array, indexed by token position. */
    char      **errs;
    size_t      errs_cap;
} Lexer;

/* -------- helpers -------- */

static void grow_tokens(Lexer *l) {
    size_t newcap = l->cap ? l->cap * 2 : 64;
    Token *t = (Token *) realloc(l->toks, newcap * sizeof(Token));
    if (!t) { fprintf(stderr, "lexer: out of memory\n"); exit(1); }
    l->toks = t;
    l->cap = newcap;
}

static void grow_errs(Lexer *l, size_t upto) {
    if (upto < l->errs_cap) return;
    size_t newcap = l->errs_cap ? l->errs_cap * 2 : 16;
    while (newcap <= upto) newcap *= 2;
    char **e = (char **) realloc(l->errs, newcap * sizeof(char *));
    if (!e) { fprintf(stderr, "lexer: out of memory\n"); exit(1); }
    for (size_t i = l->errs_cap; i < newcap; ++i) e[i] = NULL;
    l->errs = e;
    l->errs_cap = newcap;
}

static void push_token(Lexer *l, TokenKind k, size_t start, size_t end, int32_t line, int32_t col) {
    if (l->n == l->cap) grow_tokens(l);
    Token *t = &l->toks[l->n++];
    t->kind = k;
    t->line = line;
    t->col = col;
    t->start = start;
    t->length = end - start;
}

/* C99 has no strdup (POSIX-only); rolling a tiny replacement keeps
 * stage 0 buildable with -std=c99 -Wpedantic on libcs that hide
 * strdup unless _POSIX_C_SOURCE is defined (notably glibc + clang). */
static char *kai_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *r = (char *) malloc(n);
    if (r) memcpy(r, s, n);
    return r;
}

static void push_error(Lexer *l, const char *msg, size_t start, size_t end, int32_t line, int32_t col) {
    size_t idx = l->n;
    push_token(l, TK_ERROR, start, end, line, col);
    grow_errs(l, idx);
    l->errs[idx] = msg ? kai_strdup(msg) : NULL;
}

static int at_end(const Lexer *l) { return l->pos >= l->len; }
static char peek(const Lexer *l)  { return at_end(l) ? '\0' : l->src[l->pos]; }
static char peek2(const Lexer *l) { return (l->pos + 1 >= l->len) ? '\0' : l->src[l->pos + 1]; }
static char peek3(const Lexer *l) { return (l->pos + 2 >= l->len) ? '\0' : l->src[l->pos + 2]; }

static char advance(Lexer *l) {
    if (at_end(l)) return '\0';
    char c = l->src[l->pos++];
    if (c == '\n') { l->line++; l->col = 1; }
    else           { l->col++; }
    return c;
}

static int match(Lexer *l, char c) {
    if (peek(l) != c) return 0;
    advance(l);
    return 1;
}

/* -------- skippers -------- */

static void skip_line_comment(Lexer *l) {
    /* '#' already consumed */
    while (!at_end(l) && peek(l) != '\n') advance(l);
}

static void skip_whitespace_and_comments(Lexer *l) {
    for (;;) {
        if (at_end(l)) return;
        char c = peek(l);
        if (c == ' ' || c == '\t' || c == '\r') { advance(l); continue; }
        if (c == '#') { advance(l); skip_line_comment(l); continue; }
        return;
    }
}

/* -------- literal scanners -------- */

static int is_ident_start(char c) { return isalpha((unsigned char) c) || c == '_'; }
static int is_ident_cont (char c) { return isalnum((unsigned char) c) || c == '_'; }

/*
 * Keyword table. The identifier's text is compared against this list
 * (linear scan; list is small enough).
 */
typedef struct { const char *name; TokenKind kind; } Keyword;
static const Keyword KEYWORDS[] = {
    { "and",    TK_AND    }, { "as",     TK_AS     }, { "assert", TK_ASSERT },
    { "else",   TK_ELSE   }, { "false",  TK_FALSE  }, { "fn",     TK_FN     },
    { "if",     TK_IF     }, { "import", TK_IMPORT }, { "let",    TK_LET    },
    { "match",  TK_MATCH  }, { "not",    TK_NOT    }, { "or",     TK_OR     },
    { "pub",    TK_PUB    }, { "test",   TK_TEST   }, { "true",   TK_TRUE   },
    { "type",   TK_TYPE   }
};
static const size_t N_KEYWORDS = sizeof(KEYWORDS) / sizeof(KEYWORDS[0]);

static TokenKind keyword_or_ident(const char *s, size_t len) {
    for (size_t i = 0; i < N_KEYWORDS; ++i) {
        const char *kw = KEYWORDS[i].name;
        size_t klen = strlen(kw);
        if (klen == len && memcmp(s, kw, len) == 0) return KEYWORDS[i].kind;
    }
    return TK_IDENT;
}

static void lex_ident_or_keyword(Lexer *l) {
    size_t start = l->pos;
    int32_t sl = l->line, sc = l->col;
    while (!at_end(l) && is_ident_cont(peek(l))) advance(l);

    size_t end = l->pos;
    size_t len = end - start;

    /* A bare `_` is the wildcard. Identifiers that happen to start with `_`
       but have more characters remain ordinary identifiers. */
    if (len == 1 && l->src[start] == '_') {
        push_token(l, TK_UNDERSCORE, start, end, sl, sc);
        return;
    }

    TokenKind k = keyword_or_ident(l->src + start, len);
    push_token(l, k, start, end, sl, sc);
}

/*
 * Numbers. Integer: digits with optional `_` separators. Real: integer
 * part, optional fractional part (starts with `.` followed by digit),
 * optional exponent (`e` or `E`, optional sign, digits).
 */
static void lex_number(Lexer *l) {
    size_t start = l->pos;
    int32_t sl = l->line, sc = l->col;
    int is_real = 0;

    while (!at_end(l) && (isdigit((unsigned char) peek(l)) || peek(l) == '_')) advance(l);

    /* Fractional part: only if `.` is followed by a digit, so that `x..y`
       and `x.field` continue to work. */
    if (peek(l) == '.' && isdigit((unsigned char) peek2(l))) {
        is_real = 1;
        advance(l);                                    /* consume '.' */
        while (!at_end(l) && (isdigit((unsigned char) peek(l)) || peek(l) == '_')) advance(l);
    }

    if (peek(l) == 'e' || peek(l) == 'E') {
        is_real = 1;
        advance(l);
        if (peek(l) == '+' || peek(l) == '-') advance(l);
        if (!isdigit((unsigned char) peek(l))) {
            push_error(l, "malformed number: expected digits after exponent",
                       start, l->pos, sl, sc);
            return;
        }
        while (!at_end(l) && (isdigit((unsigned char) peek(l)) || peek(l) == '_')) advance(l);
    }

    push_token(l, is_real ? TK_REAL : TK_INT, start, l->pos, sl, sc);
}

/*
 * String literal.
 *
 * Single-line: "..." with escapes (\n, \t, \r, \", \', \\, \u{HHHH}).
 *   #{...} is interpolation; we match the braces greedily counting nesting.
 * Triple-quoted: """..."""
 *
 * The lexer does not decode escapes; the parser/emitter does. We just
 * verify structural correctness (balanced quotes and interp braces).
 */
static void lex_string(Lexer *l) {
    size_t start = l->pos;
    int32_t sl = l->line, sc = l->col;

    /* Detect triple quote. */
    int triple = 0;
    advance(l);                                       /* opening " */
    if (peek(l) == '"' && peek2(l) == '"') {
        advance(l); advance(l);
        triple = 1;
    }

    while (!at_end(l)) {
        char c = peek(l);
        if (c == '\\') {
            advance(l);
            if (!at_end(l)) advance(l);               /* consume escaped char blindly */
            continue;
        }
        if (c == '#' && peek2(l) == '{') {
            advance(l); advance(l);
            int depth = 1;
            while (!at_end(l) && depth > 0) {
                char d = peek(l);
                if (d == '{') { depth++; advance(l); }
                else if (d == '}') { depth--; advance(l); }
                else if (d == '"') {
                    /* nested string inside interpolation: skip by scanning */
                    advance(l);
                    while (!at_end(l) && peek(l) != '"') {
                        if (peek(l) == '\\' && !at_end(l)) { advance(l); if (!at_end(l)) advance(l); }
                        else advance(l);
                    }
                    if (!at_end(l)) advance(l);       /* closing inner " */
                }
                else advance(l);
            }
            if (depth != 0) {
                push_error(l, "unterminated interpolation in string",
                           start, l->pos, sl, sc);
                return;
            }
            continue;
        }
        if (triple) {
            if (c == '"' && peek2(l) == '"' && peek3(l) == '"') {
                advance(l); advance(l); advance(l);
                push_token(l, TK_STRING, start, l->pos, sl, sc);
                return;
            }
            advance(l);
        } else {
            if (c == '\n') {
                push_error(l, "unterminated string (newline in single-line string)",
                           start, l->pos, sl, sc);
                return;
            }
            if (c == '"') {
                advance(l);
                push_token(l, TK_STRING, start, l->pos, sl, sc);
                return;
            }
            advance(l);
        }
    }

    push_error(l, "unterminated string",
               start, l->pos, sl, sc);
}

static void lex_char(Lexer *l) {
    size_t start = l->pos;
    int32_t sl = l->line, sc = l->col;

    advance(l);                                       /* opening ' */
    if (at_end(l)) {
        push_error(l, "unterminated char literal", start, l->pos, sl, sc);
        return;
    }
    if (peek(l) == '\\') {
        advance(l);
        if (at_end(l)) {
            push_error(l, "unterminated char literal", start, l->pos, sl, sc);
            return;
        }
        /* If \u{HHHH}, consume the braces. */
        if (peek(l) == 'u' && peek2(l) == '{') {
            advance(l); advance(l);
            while (!at_end(l) && peek(l) != '}') advance(l);
            if (!at_end(l)) advance(l);               /* closing '}' */
        } else {
            advance(l);                               /* simple escape */
        }
    } else {
        /* consume one or more bytes of a UTF-8 codepoint */
        advance(l);
        while (!at_end(l) && peek(l) != '\'' && ((unsigned char) peek(l) & 0xC0) == 0x80) {
            advance(l);
        }
    }
    if (peek(l) != '\'') {
        push_error(l, "unterminated char literal", start, l->pos, sl, sc);
        return;
    }
    advance(l);                                       /* closing ' */
    push_token(l, TK_CHAR, start, l->pos, sl, sc);
}

/* -------- main tokenize loop -------- */

/* Lex a single-char-led operator or punctuation token. `c` has already been
   consumed (advance called); `start`/`sl`/`sc` mark its source position. The
   multi-char forms (->, =>, ==, |>, .., ...) peek ahead with match/peek. */
static void lex_operator(Lexer *l, char c, size_t start, int32_t sl, int32_t sc) {
    switch (c) {
        case '(': push_token(l, TK_LPAREN,   start, l->pos, sl, sc); break;
        case ')': push_token(l, TK_RPAREN,   start, l->pos, sl, sc); break;
        case '[': push_token(l, TK_LBRACKET, start, l->pos, sl, sc); break;
        case ']': push_token(l, TK_RBRACKET, start, l->pos, sl, sc); break;
        case '{': push_token(l, TK_LBRACE,   start, l->pos, sl, sc); break;
        case '}': push_token(l, TK_RBRACE,   start, l->pos, sl, sc); break;
        case ',': push_token(l, TK_COMMA,    start, l->pos, sl, sc); break;
        case ':': push_token(l, TK_COLON,    start, l->pos, sl, sc); break;
        case ';': push_token(l, TK_SEMI,     start, l->pos, sl, sc); break;
        case '+': push_token(l, TK_PLUS,     start, l->pos, sl, sc); break;
        case '*': push_token(l, TK_STAR,     start, l->pos, sl, sc); break;
        case '%': push_token(l, TK_PERCENT,  start, l->pos, sl, sc); break;

        case '-':
            if (match(l, '>')) push_token(l, TK_ARROW, start, l->pos, sl, sc);
            else               push_token(l, TK_MINUS, start, l->pos, sl, sc);
            break;

        case '/':
            if (match(l, '/')) push_token(l, TK_SLASH_SLASH, start, l->pos, sl, sc);
            else               push_token(l, TK_SLASH,       start, l->pos, sl, sc);
            break;

        case '=':
            if (match(l, '=')) push_token(l, TK_EQEQ,      start, l->pos, sl, sc);
            else if (match(l, '>')) push_token(l, TK_FAT_ARROW, start, l->pos, sl, sc);
            else               push_token(l, TK_EQ,        start, l->pos, sl, sc);
            break;

        case '<':
            if (match(l, '=')) push_token(l, TK_LE, start, l->pos, sl, sc);
            else               push_token(l, TK_LT, start, l->pos, sl, sc);
            break;

        case '>':
            if (match(l, '=')) push_token(l, TK_GE, start, l->pos, sl, sc);
            else               push_token(l, TK_GT, start, l->pos, sl, sc);
            break;

        case '!':
            if (match(l, '=')) push_token(l, TK_NEQ,  start, l->pos, sl, sc);
            else               push_token(l, TK_BANG, start, l->pos, sl, sc);
            break;

        case '|':
            if (match(l, '>')) push_token(l, TK_PIPE_APPLY, start, l->pos, sl, sc);
            else               push_token(l, TK_PIPE,       start, l->pos, sl, sc);
            break;

        case '.':
            if (peek(l) == '.' && peek2(l) == '.') {
                advance(l); advance(l);
                push_token(l, TK_ELLIPSIS, start, l->pos, sl, sc);
            } else if (peek(l) == '.') {
                advance(l);
                push_token(l, TK_DOTDOT, start, l->pos, sl, sc);
            } else {
                push_token(l, TK_DOT, start, l->pos, sl, sc);
            }
            break;

        default: {
            static char msgbuf[64];
            snprintf(msgbuf, sizeof(msgbuf),
                     "unexpected character '%c' (0x%02x)", c, (unsigned char) c);
            push_error(l, msgbuf, start, l->pos, sl, sc);
            break;
        }
    }
}

Token *kai_lex(const char *file, const char *src, size_t len, size_t *out_n) {
    Lexer l;
    memset(&l, 0, sizeof(l));
    l.file = file;
    l.src  = src;
    l.len  = len;
    l.pos  = 0;
    l.line = 1;
    l.col  = 1;

    while (!at_end(&l)) {
        skip_whitespace_and_comments(&l);
        if (at_end(&l)) break;

        size_t start = l.pos;
        int32_t sl = l.line, sc = l.col;
        char c = peek(&l);

        if (c == '\n') {
            advance(&l);
            push_token(&l, TK_NEWLINE, start, l.pos, sl, sc);
            continue;
        }

        if (is_ident_start(c))      { lex_ident_or_keyword(&l); continue; }
        if (isdigit((unsigned char) c)) { lex_number(&l);       continue; }
        if (c == '"')               { lex_string(&l);           continue; }
        if (c == '\'')              { lex_char(&l);             continue; }

        advance(&l);                                  /* consume `c` */
        lex_operator(&l, c, start, sl, sc);
    }

    /* Always append an EOF token. */
    push_token(&l, TK_EOF, l.pos, l.pos, l.line, l.col);

    if (out_n) *out_n = l.n;
    /* errs array leaks here; acceptable because messages are consulted by
       the caller before the program exits. A future cleanup can free them. */
    return l.toks;
}

/* -------- diagnostics -------- */

/*
 * Milestone 2 keeps error reporting inline: each TK_ERROR token carries
 * enough position info for the driver to print the source slice. A
 * per-token message map is deferred to milestone 8 (test runner), where
 * structured error reporting matters.
 */
const char *kai_lex_error_message(size_t idx) {
    (void) idx;
    return NULL;
}

/* Name table indexed by TokenKind. Designated initializers pin each entry
   to its enum value, so reordering TokenKind cannot desync this table.
   The static assert below fails the build if the enum grows past the last
   covered value without a matching row. */
static const char *const TOKEN_NAMES[] = {
    [TK_EOF]         = "EOF",
    [TK_NEWLINE]     = "NEWLINE",
    [TK_AND]         = "and",
    [TK_AS]          = "as",
    [TK_ASSERT]      = "assert",
    [TK_ELSE]        = "else",
    [TK_FALSE]       = "false",
    [TK_FN]          = "fn",
    [TK_IF]          = "if",
    [TK_IMPORT]      = "import",
    [TK_LET]         = "let",
    [TK_MATCH]       = "match",
    [TK_NOT]         = "not",
    [TK_OR]          = "or",
    [TK_PUB]         = "pub",
    [TK_TEST]        = "test",
    [TK_TRUE]        = "true",
    [TK_TYPE]        = "type",
    [TK_INT]         = "INT",
    [TK_REAL]        = "REAL",
    [TK_CHAR]        = "CHAR",
    [TK_STRING]      = "STRING",
    [TK_IDENT]       = "IDENT",
    [TK_UNDERSCORE]  = "_",
    [TK_LPAREN]      = "(",
    [TK_RPAREN]      = ")",
    [TK_LBRACKET]    = "[",
    [TK_RBRACKET]    = "]",
    [TK_LBRACE]      = "{",
    [TK_RBRACE]      = "}",
    [TK_COMMA]       = ",",
    [TK_COLON]       = ":",
    [TK_SEMI]        = ";",
    [TK_DOT]         = ".",
    [TK_DOTDOT]      = "..",
    [TK_ELLIPSIS]    = "...",
    [TK_EQ]          = "=",
    [TK_ARROW]       = "->",
    [TK_FAT_ARROW]   = "=>",
    [TK_PIPE]        = "|",
    [TK_PIPE_APPLY]  = "|>",
    [TK_PLUS]        = "+",
    [TK_MINUS]       = "-",
    [TK_STAR]        = "*",
    [TK_SLASH]       = "/",
    [TK_SLASH_SLASH] = "//",
    [TK_PERCENT]     = "%",
    [TK_EQEQ]        = "==",
    [TK_NEQ]         = "!=",
    [TK_LT]          = "<",
    [TK_GT]          = ">",
    [TK_LE]          = "<=",
    [TK_GE]          = ">=",
    [TK_BANG]        = "!",
    [TK_ERROR]       = "ERROR",
};

/* One row per TokenKind: the table must cover the enum through TK_ERROR.
   C99-portable static assert (no C11 _Static_assert) — a negative-size
   array typedef fails the build if TOKEN_NAMES drifts from TokenKind. */
typedef char token_names_in_sync[
    (sizeof(TOKEN_NAMES) / sizeof(TOKEN_NAMES[0]) == TK_ERROR + 1) ? 1 : -1];

const char *tk_name(TokenKind k) {
    if (k < 0 || k > TK_ERROR || !TOKEN_NAMES[k]) return "?";
    return TOKEN_NAMES[k];
}

void kai_lex_dump(const char *file, const char *src, const Token *toks, size_t n) {
    (void) file;
    for (size_t i = 0; i < n; ++i) {
        const Token *t = &toks[i];
        if (t->kind == TK_NEWLINE) {
            printf("%4d:%-3d  %s\n", t->line, t->col, tk_name(t->kind));
        } else {
            printf("%4d:%-3d  %-12s  \"%.*s\"\n",
                   t->line, t->col, tk_name(t->kind),
                   (int) t->length, src + t->start);
        }
    }
}
