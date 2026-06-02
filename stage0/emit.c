/*
 * C emitter for kaikai-minimal.
 *
 * Covers everything the four canonical examples need: literals, binops
 * and unops, if/else chains, match with list and variant patterns, let
 * bindings, blocks, records (literals + field access), ranges and list
 * literals, pipes, user fns and prelude fns as first-class values, and
 * named lambdas (x => body) with capture analysis.
 *
 * String literals with #{...} interpolation are re-parsed and emitted
 * as kai_string_concat chains that wrap each inner expression with
 * kai_to_string.
 *
 * The placeholder `.` lambda shorthand is deliberately not supported
 * in stage 0 — users must spell out `x => expr`. This keeps the emitter
 * simpler; the restriction is listed in docs/stage0-design.md.
 */

#include "emit.h"
#include "ast.h"
#include "lexer.h"
#include "parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- tables ---------- */

/* Atom-style global variant tag base — first user-variant tag.
 * Tags 0..10 are reserved for builtin constructors per the convention
 * pinned in docs/variant-tags.md. Stage 0 only knows about the four
 * builtins it can emit (Some, None, Ok, Err); the Signal and ProcessExit
 * constructors are runtime-only and never built by stage-0-emitted code,
 * so they do not appear in the table here but they still consume tags
 * 4 through 10 in the global numbering. */
#define KAI_USER_VARIANT_TAG_BASE 11

typedef struct { const char *name; size_t len; int arity; int tag; } SymEntry;

typedef struct {
    const char *name;       /* capture name (borrowed from source) */
    size_t      len;
} Capture;

typedef struct {
    int    id;
    Node  *lam;             /* the N_LAMBDA node */
    Capture *caps;
    int    n_caps;
    int    cap_caps;
} LamInfo;

/* Local-binding scope used by both capture analysis and emission. A
   flat stack of names (borrowed from source) with a parallel stack
   of "marks" — each mark is the size of the name stack at the time
   a scope was entered. push_mark records the size; pop_mark
   truncates back to it. Lookups walk the name stack from the end,
   so inner scopes shadow outer ones.

   Each entry carries the declaring AST node (`N_PARAM` for fn
   params, `N_PAT_BIND` for let bindings and match-arm binds, NULL
   for shadow-only entries like lambda params and captures). The
   decl is looked up via `ls_resolve` at emit time so use-count
   queries can identify the specific binding being read (two
   bindings sharing a name in disjoint scopes don't alias).

   During `collect_all_lambdas` the stack tracks which names are
   introduced by lets / match patterns inside a lambda body — those
   bindings should never end up as phantom captures.

   During emission the stack tracks the same thing, plus function
   parameters and lambda captures, so `emit_ident_value` /
   `emit_ident_callee` can prefer a local binding over a colliding
   prelude name or user function. */
typedef struct {
    char **names;
    size_t *lens;
    Node  **decls;
    size_t  n, cap;
    size_t *marks;
    size_t  n_marks, cap_marks;
} LocalScope;

/* m5.x flip Phase 3 closeout — Step D refinement (2026-04-29):
 * use-counter for the current fn body. Each entry is keyed by the
 * declaring AST node — `N_PARAM` for fn parameters, `N_PAT_BIND`
 * for let bindings and match-arm pattern binds. `count_local_uses`
 * walks the body once before emit, maintaining its own scope stack
 * to resolve each `N_IDENT` reference back to the specific binding
 * it shadows. `emit_ident_value` then queries by decl pointer: a
 * binding read exactly once, never from inside a lambda body, owns
 * the only live ref and can be emitted raw (`kai_<name>`) — the
 * consuming primitive takes the binding's only ref directly, no
 * leak. Multi-use, captured, or unresolved bindings (lambda
 * params, captures) keep the brute-force eager-dup that preserves
 * the slot at the cost of one leaked ref per read. */
typedef struct {
    Node       *decl;          /* binding identity: N_PARAM or N_PAT_BIND */
    const char *name;
    size_t      name_len;
    int         total_count;
    int         lambda_count;  /* > 0 means at least one read sits inside a lambda body */
} LocalUse;

typedef struct {
    LocalUse *items;
    size_t    n, cap;
} LocalUseTable;

typedef struct {
    FILE *out;
    int   had_error;
    int   test_mode;

    /* Active description for an enclosing `test "…" { ... }`, used so
       `assert` can report the owning test on failure. NULL when not
       inside a test block. */
    const char *cur_test_desc_start;   /* raw span, includes quotes */
    size_t      cur_test_desc_len;

    SymEntry *fns;      size_t n_fns, cap_fns;
    SymEntry *variants; size_t n_variants, cap_variants;
    LamInfo  *lams;     size_t n_lams, cap_lams;

    LocalScope    locals;
    LocalUseTable uses;     /* current fn's local-binding use counts; cleared between fns */

    /* Issue #42 — TCO state. `cur_fn` is the N_FN node currently
     * being emitted, so emit_call can reach the parameter list
     * when lowering a marked tail-self-call to a rebind+goto block.
     * NULL outside emit_fn_body. */
    Node *cur_fn;
} E;

/* ---------- local scope ---------- */

static void ls_push_mark(E *e) {
    LocalScope *s = &e->locals;
    if (s->n_marks == s->cap_marks) {
        s->cap_marks = s->cap_marks ? s->cap_marks * 2 : 8;
        s->marks = (size_t *) realloc(s->marks, s->cap_marks * sizeof(size_t));
    }
    s->marks[s->n_marks++] = s->n;
}

static void ls_pop_mark(E *e) {
    LocalScope *s = &e->locals;
    if (s->n_marks == 0) return;
    s->n = s->marks[--s->n_marks];
}

static void ls_add(E *e, const char *name, size_t len, Node *decl) {
    LocalScope *s = &e->locals;
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->names = (char **) realloc(s->names, s->cap * sizeof(char *));
        s->lens  = (size_t *) realloc(s->lens,  s->cap * sizeof(size_t));
        s->decls = (Node **)  realloc(s->decls, s->cap * sizeof(Node *));
    }
    s->names[s->n] = (char *) name;
    s->lens[s->n]  = len;
    s->decls[s->n] = decl;
    s->n++;
}

static int ls_has(E *e, const char *name, size_t len) {
    LocalScope *s = &e->locals;
    for (size_t i = s->n; i > 0;) {
        --i;
        if (s->lens[i] == len && memcmp(s->names[i], name, len) == 0) return 1;
    }
    return 0;
}

/* Innermost matching declaration, or NULL if no entry has a decl
 * (e.g. lambda params or captures, which use_lookup will miss and
 * fall through to the eager-dup path). */
static Node *ls_resolve(E *e, const char *name, size_t len) {
    LocalScope *s = &e->locals;
    for (size_t i = s->n; i > 0;) {
        --i;
        if (s->lens[i] == len && memcmp(s->names[i], name, len) == 0) {
            return s->decls[i];
        }
    }
    return NULL;
}

/* ---------- local-binding use counter ---------- */

static void lu_clear(E *e) {
    e->uses.n = 0;
}

static void lu_add(E *e, Node *decl, const char *name, size_t len) {
    LocalUseTable *t = &e->uses;
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 16;
        t->items = (LocalUse *) realloc(t->items, t->cap * sizeof(LocalUse));
    }
    t->items[t->n].decl = decl;
    t->items[t->n].name = name;
    t->items[t->n].name_len = len;
    t->items[t->n].total_count = 0;
    t->items[t->n].lambda_count = 0;
    t->n++;
}

static LocalUse *lu_lookup(E *e, Node *decl) {
    if (!decl) return NULL;
    LocalUseTable *t = &e->uses;
    for (size_t i = 0; i < t->n; ++i) {
        if (t->items[i].decl == decl) return &t->items[i];
    }
    return NULL;
}

/* Walk a pattern, adding every PAT_BIND name to the local scope. Used
   by both collect_free_vars and emit_pat_binds so pattern bindings
   are never counted as captures and are never redirected to a
   same-named global at emit time. The PAT_BIND node itself is the
   binding identity used by the use counter. */
static void pat_add_locals(E *e, Node *pat) {
    if (!pat) return;
    switch (pat->kind) {
        case N_PAT_WILD:
        case N_PAT_LIT:
            return;
        case N_PAT_BIND:
            ls_add(e, pat->name, pat->name_len, pat);
            return;
        case N_PAT_LIST:
        case N_PAT_VARIANT:
        case N_PAT_RECORD:
            for (size_t i = 0; i < pat->n_children; ++i) pat_add_locals(e, pat->children[i]);
            return;
        case N_PAT_FIELD:
            if (pat->n_children >= 1) pat_add_locals(e, pat->children[0]);
            return;
        default:
            return;
    }
}

/* Variant of `pat_add_locals` that also registers a `LocalUse` entry
 * for each PAT_BIND keyed by the pat node, so subsequent IDENT reads
 * inside the binding's scope can be counted against the right
 * binding identity. Used only by `count_local_uses` when not
 * descending into a lambda body — lambda-local lets are tracked for
 * shadowing only and stay on the safe eager-dup path. */
static void pat_register_uses(E *e, Node *pat) {
    if (!pat) return;
    switch (pat->kind) {
        case N_PAT_WILD:
        case N_PAT_LIT:
            return;
        case N_PAT_BIND:
            lu_add(e, pat, pat->name, pat->name_len);
            return;
        case N_PAT_LIST:
        case N_PAT_VARIANT:
        case N_PAT_RECORD:
            for (size_t i = 0; i < pat->n_children; ++i) pat_register_uses(e, pat->children[i]);
            return;
        case N_PAT_FIELD:
            if (pat->n_children >= 1) pat_register_uses(e, pat->children[0]);
            return;
        default:
            return;
    }
}

/* ---------- forwards (use counter) ---------- */

static void count_local_uses(E *e, Node *n, int in_lambda);
static void count_local_uses_in_string(E *e, Node *s, int in_lambda);

/* Walk an AST node, incrementing each tracked binding's use count
 * for every `N_IDENT` reference resolving to it. Maintains its own
 * scope by piggy-backing on `e->locals` (push_mark / pop_mark
 * balanced), so a `let x` inside a block shadows an outer `x`
 * correctly. `in_lambda` flips to 1 on descent into an `N_LAMBDA`
 * so `lambda_count` flags bindings that survive past the outer
 * statement and need to keep their slot live for the closure.
 *
 * Lambda params and captures, and lets declared inside lambda
 * bodies, are pushed onto the scope without a `LocalUse` entry —
 * they shadow outer entries during count, and `emit_ident_value`
 * sees `lu_lookup == NULL` for them and stays on eager-dup.
 *
 * String literals carry interpolations that are re-parsed at emit
 * time. Counting has to mirror that or captured names referenced
 * only from inside `#{...}` would be undercounted. */
/* N_IDENT use: bump the resolved binding's counters. */
static void count_ident_use(E *e, Node *n, int in_lambda) {
    Node *decl = ls_resolve(e, n->name, n->name_len);
    LocalUse *u = lu_lookup(e, decl);
    if (u) {
        u->total_count++;
        if (in_lambda) u->lambda_count++;
    }
}

/* N_LAMBDA: parameters shadow outer locals; the body is counted in-lambda. */
static void count_uses_in_lambda(E *e, Node *n) {
    ls_push_mark(e);
    for (size_t i = 1; i < n->n_children; ++i) {
        Node *p = n->children[i];
        if (p && p->kind == N_IDENT) ls_add(e, p->name, p->name_len, NULL); /* shadow only */
    }
    if (n->n_children >= 1) count_local_uses(e, n->children[0], 1);
    ls_pop_mark(e);
}

/* N_LET: the RHS sees the outer scope; the pattern then binds new locals. */
static void count_uses_in_let(E *e, Node *n, int in_lambda) {
    if (n->n_children >= 3) count_local_uses(e, n->children[2], in_lambda);
    if (n->n_children >= 1) {
        Node *pat = n->children[0];
        if (!in_lambda) pat_register_uses(e, pat);
        pat_add_locals(e, pat);
    }
}

/* N_ARM: the pattern binds locals visible to the guard and body. */
static void count_uses_in_arm(E *e, Node *n, int in_lambda) {
    int has_guard = (n->v.flags & 0x1) != 0;
    Node *pat   = n->children[0];
    Node *body  = n->children[has_guard ? 2 : 1];
    Node *guard = has_guard ? n->children[1] : NULL;
    ls_push_mark(e);
    if (!in_lambda) pat_register_uses(e, pat);
    pat_add_locals(e, pat);
    if (guard) count_local_uses(e, guard, in_lambda);
    if (body)  count_local_uses(e, body,  in_lambda);
    ls_pop_mark(e);
}

static void count_local_uses(E *e, Node *n, int in_lambda) {
    if (!n) return;
    switch (n->kind) {
        case N_IDENT:  count_ident_use(e, n, in_lambda);               return;
        case N_STRING: count_local_uses_in_string(e, n, in_lambda);    return;
        case N_LAMBDA: count_uses_in_lambda(e, n);                     return;
        case N_LET:    count_uses_in_let(e, n, in_lambda);             return;
        case N_ARM:    count_uses_in_arm(e, n, in_lambda);             return;
        case N_BLOCK:
            ls_push_mark(e);
            for (size_t i = 0; i < n->n_children; ++i)
                count_local_uses(e, n->children[i], in_lambda);
            ls_pop_mark(e);
            return;
        default:
            for (size_t i = 0; i < n->n_children; ++i)
                count_local_uses(e, n->children[i], in_lambda);
            return;
    }
}

/* Scan a `#{...}` interpolation body. On entry `i` points just past the
 * opening `#{`; advances `i` to the matching `}` (the one closing depth 0,
 * tracking nested braces) or to `end` if unterminated, and returns the
 * index where the expression text starts. Shared by the three places that
 * walk interpolations: count_local_uses_in_string, collect_free_vars_in_string,
 * and emit_string_expr. */
static size_t scan_interp_expr(const char *src, size_t *i, size_t end) {
    size_t expr_start = *i;
    int depth = 1;
    while (*i < end && depth > 0) {
        if      (src[*i] == '{') depth++;
        else if (src[*i] == '}') { depth--; if (depth == 0) break; }
        ++(*i);
    }
    return expr_start;
}

/* Mirror of `collect_free_vars_in_string`: re-parse `#{...}` chunks
 * and walk them so identifiers referenced only from interpolations
 * are still counted against the binding they read. Keep in
 * lock-step with `emit_string_expr` and `collect_free_vars_in_string`. */
static void count_local_uses_in_string(E *e, Node *s, int in_lambda) {
    const char *src = s->name;
    if (!src || s->name_len < 2) return;
    int triple = (s->v.flags & 0x1) != 0;
    size_t start = triple ? 3 : 1;
    size_t end   = triple ? s->name_len - 3 : s->name_len - 1;
    size_t i = start;
    while (i < end) {
        if (src[i] == '#' && i + 1 < end && src[i + 1] == '{') {
            i += 2;
            size_t expr_start = scan_interp_expr(src, &i, end);
            size_t ntk = 0;
            Token *toks = kai_lex("<interp>", src + expr_start, i - expr_start, &ntk);
            Node *en = kai_parse_expr_standalone("<interp>", src + expr_start, toks, ntk);
            if (en) {
                count_local_uses(e, en, in_lambda);
                kai_free_node(en);
            }
            free(toks);
            if (i < end) i++;                 /* past } */
        } else {
            ++i;
        }
    }
}

/* ---------- forwards ---------- */

/* Issue #42 — TCO bit on N_CALL: set by tco_mark when a call sits in
 * tail position of the enclosing fn and targets the same fn with the
 * same arity. emit_call recognises the bit and emits a rebind+goto
 * block. The label `_kai_<name>_entry:;` is planted by emit_fn_body. */
#define TCO_TAIL_CALL 0x1

static void emit_expr(E *e, Node *n);
static void emit_stmt(E *e, Node *n);
static void emit_tco_goto(E *e, Node *call);
static void emit_pat_test(E *e, Node *pat, const char *scr);
/* `is_alias` = the scrutinee shares storage with its producer (variant args,
 * cons head/tail). PBind/PAs emit `kai_incref(scr)` so the binding has its
 * own owned reference, decoupled from the producer's slot. When false, the
 * scrutinee is already owned (top-level _scr / _letv from a transferred RHS,
 * or kai_op_field which already increfs its return) and the binding takes the
 * ref directly. */
static void emit_pat_binds(E *e, Node *pat, const char *scr, int is_alias);
static void emit_string_expr(E *e, Node *s);
static void emit_lambda_ref(E *e, Node *lam);

/* ---------- diagnostics ---------- */

static void bug(E *e, Node *n, const char *what) {
    e->had_error = 1;
    int line = n ? n->line : 0;
    int col  = n ? n->col  : 0;
    fprintf(stderr, "emit: %d:%d: unsupported construct: %s\n", line, col, what);
}

/* ---------- prelude table ---------- */

static const struct {
    const char *k;
    const char *c;
    int         arity;
} PRELUDE[] = {
    { "print",          "kai_prelude_print",          1 },
    { "eprint",         "kai_prelude_eprint",         1 },
    { "write_stdout",   "kai_prelude_write_stdout",   1 },
    { "panic",          "kai_prelude_panic",          1 },
    { "exit",           "kai_prelude_exit",           1 },
    { "int_to_string",  "kai_prelude_int_to_string",  1 },
    { "real_to_string", "kai_prelude_real_to_string", 1 },
    { "int_to_real",    "kai_prelude_int_to_real",    1 },
    { "real_to_int",    "kai_prelude_real_to_int",    1 },
    /* libm bindings (issue #343) — pass-through NaN/Inf semantics. */
    { "real_sqrt",      "kai_prelude_real_sqrt",      1 },
    { "real_cbrt",      "kai_prelude_real_cbrt",      1 },
    { "real_exp",       "kai_prelude_real_exp",       1 },
    { "real_log",       "kai_prelude_real_log",       1 },
    { "real_log2",      "kai_prelude_real_log2",      1 },
    { "real_log10",     "kai_prelude_real_log10",     1 },
    { "real_sin",       "kai_prelude_real_sin",       1 },
    { "real_cos",       "kai_prelude_real_cos",       1 },
    { "real_tan",       "kai_prelude_real_tan",       1 },
    { "real_asin",      "kai_prelude_real_asin",      1 },
    { "real_acos",      "kai_prelude_real_acos",      1 },
    { "real_atan",      "kai_prelude_real_atan",      1 },
    { "real_sinh",      "kai_prelude_real_sinh",      1 },
    { "real_cosh",      "kai_prelude_real_cosh",      1 },
    { "real_tanh",      "kai_prelude_real_tanh",      1 },
    { "real_signum",    "kai_prelude_real_signum",    1 },
    { "real_is_nan",    "kai_prelude_real_is_nan",    1 },
    { "real_is_inf",    "kai_prelude_real_is_inf",    1 },
    { "real_pow",       "kai_prelude_real_pow",       2 },
    { "real_atan2",     "kai_prelude_real_atan2",     2 },
    { "string_length",  "kai_prelude_string_length",  1 },
    { "string_concat",  "kai_prelude_string_concat",  2 },
    { "string_concat_all", "kai_prelude_string_concat_all", 1 },
    { "string_join",    "kai_prelude_string_join",    2 },
    { "list_length",    "kai_prelude_list_length",    1 },
    { "list_append",    "kai_prelude_list_append",    2 },
    { "list_reverse",   "kai_prelude_list_reverse",   1 },
    { "map",            "kai_prelude_map",            2 },
    { "flat_map",       "kai_prelude_flat_map",       2 },
    { "filter",         "kai_prelude_filter",         2 },
    { "reduce",         "kai_prelude_reduce",         3 },
    { "each",           "kai_prelude_each",           2 },
    { "args",           "kai_prelude_args",           0 },
    { "read_file",      "kai_prelude_read_file",      1 },
    { "write_file",     "kai_prelude_write_file",     2 },
    { "read_line",      "kai_prelude_read_line",      0 },
    { "read_bytes",     "kai_prelude_read_bytes",     1 },
    { "string_to_int",  "kai_prelude_string_to_int",  1 },
    { "string_to_real", "kai_prelude_string_to_real", 1 },
    { "char_at",        "kai_prelude_char_at",        2 },
    { "string_split",   "kai_prelude_string_split",   2 },
    { "string_contains","kai_prelude_string_contains",2 },
    { "string_slice",   "kai_prelude_string_slice",   3 },
    { "char_to_int",    "kai_prelude_char_to_int",    1 },
    { "int_to_char",    "kai_prelude_int_to_char",    1 },
    { "int_to_byte_string", "kai_prelude_int_to_byte_string", 1 },
    { "string_byte_at_int", "kai_prelude_string_byte_at_int", 2 },
    { "array_make",     "kai_prelude_array_make",     2 },
    { "array_length",   "kai_prelude_array_length",   1 },
    { "array_get",      "kai_prelude_array_get",      2 },
    { "array_set",      "kai_prelude_array_set",      3 },
    { "array_grow",     "kai_prelude_array_grow",     3 }
};
static const size_t N_PRELUDE = sizeof(PRELUDE) / sizeof(PRELUDE[0]);

static int find_prelude(const char *name, size_t len, int *out_arity, const char **out_c) {
    for (size_t i = 0; i < N_PRELUDE; ++i) {
        size_t kl = strlen(PRELUDE[i].k);
        if (kl == len && memcmp(PRELUDE[i].k, name, len) == 0) {
            if (out_arity) *out_arity = PRELUDE[i].arity;
            if (out_c)     *out_c     = PRELUDE[i].c;
            return 1;
        }
    }
    return 0;
}

/* ---------- user fns + variants tables ---------- */

static void reg_entry(SymEntry **arr, size_t *n, size_t *cap,
                      const char *name, size_t len, int arity, int tag) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *arr = (SymEntry *) realloc(*arr, *cap * sizeof(SymEntry));
    }
    (*arr)[*n].name = name;
    (*arr)[*n].len  = len;
    (*arr)[*n].arity = arity;
    (*arr)[*n].tag = tag;
    (*n)++;
}

static int find_entry(SymEntry *arr, size_t n,
                      const char *name, size_t len,
                      int *out_arity, int *out_tag) {
    for (size_t i = 0; i < n; ++i) {
        if (arr[i].len == len && memcmp(arr[i].name, name, len) == 0) {
            if (out_arity) *out_arity = arr[i].arity;
            if (out_tag)   *out_tag   = arr[i].tag;
            return 1;
        }
    }
    return 0;
}

static int find_user_fn(E *e, const char *name, size_t len, int *out_arity) {
    return find_entry(e->fns, e->n_fns, name, len, out_arity, NULL);
}
static int find_variant(E *e, const char *name, size_t len,
                        int *out_arity, int *out_tag) {
    return find_entry(e->variants, e->n_variants, name, len, out_arity, out_tag);
}

/* ---------- identifier emission modes ---------- */

static int emit_ident_callee(E *e, const char *name, size_t len) {
    const char *mapped = NULL;
    int arity = 0;
    /* Local bindings beat every global. Before this check, a parameter
       or pattern-bound name that happened to share a prelude / user-fn
       name was silently redirected to the global — the cause of many
       phantom bugs in early stage-1/stage-2 bootstraps. */
    if (ls_has(e, name, len)) {
        /* Fall through to the non-handled path so the caller emits
           `kai_apply(kai_<name>, ...)` against the local closure. */
        return 0;
    }
    if (find_prelude(name, len, &arity, &mapped)) {
        fputs(mapped, e->out);
        return 1;
    }
    if (find_user_fn(e, name, len, &arity)) {
        fprintf(e->out, "kai_%.*s", (int) len, name);
        return 1;
    }
    fprintf(e->out, "kai_%.*s", (int) len, name);
    return 0;
}

static void emit_ident_value(E *e, const char *name, size_t len) {
    int arity = 0;
    const char *mapped = NULL;
    /* Locals shadow everything, same rationale as emit_ident_callee. */
    if (ls_has(e, name, len)) {
        /* m5.x-flip Phase 3 — Step D refinement (2026-04-29, deeper
         * 2026-04-29): resolve the IDENT to its declaring node via
         * the scope stack, then look up the use count. A binding
         * read exactly once and never referenced from inside a
         * lambda body owns the only live ref; the consuming
         * primitive takes that ref directly, no leak. Multi-use,
         * captured, or unresolved bindings (lambda params and
         * captures use decl=NULL → lu_lookup misses) fall through
         * to the brute-force eager-dup so the slot stays live for
         * later reads. */
        Node *decl = ls_resolve(e, name, len);
        LocalUse *u = lu_lookup(e, decl);
        if (u && u->total_count == 1 && u->lambda_count == 0) {
            fprintf(e->out, "kai_%.*s", (int) len, name);
            return;
        }
        /* m5.x-flip Phase 3 — Step D eager-dup retrofit. Wrap every
         * other local read in kai_internal_dup so the binding's
         * reference is preserved (the caller of the dup sees its own
         * +1 ref; the binding's original ref stays alive). Leaks one
         * ref per read (no exit drops in stage 0) but never UAFs. */
        fprintf(e->out, "kai_internal_dup(kai_%.*s)", (int) len, name);
        return;
    }
    {
        int vtag = 0;
        if (find_variant(e, name, len, &arity, &vtag)) {
            fprintf(e->out, "kai_variant_u(%d, \"%.*s\", 0, 0, NULL)", vtag, (int) len, name);
            return;
        }
    }
    if (find_prelude(name, len, &arity, &mapped)) {
        fprintf(e->out, "kai_closure(&_%s_thunk, %d, 0, NULL)", mapped, arity);
        return;
    }
    if (find_user_fn(e, name, len, &arity)) {
        fprintf(e->out, "kai_closure(&_kai_%.*s_thunk, %d, 0, NULL)",
                (int) len, name, arity);
        return;
    }
    fprintf(e->out, "kai_%.*s", (int) len, name);
}

/* ---------- lambda registration and capture analysis ---------- */

static int is_global_name(E *e, const char *name, size_t len) {
    int a;
    const char *c;
    return find_prelude(name, len, &a, &c) ||
           find_user_fn(e, name, len, &a) ||
           find_variant(e, name, len, &a, NULL);
}

static int is_lambda_param(Node *lam, const char *name, size_t len) {
    for (size_t i = 1; i < lam->n_children; ++i) {
        Node *p = lam->children[i];
        if (p && p->kind == N_IDENT &&
            p->name_len == len && memcmp(p->name, name, len) == 0) return 1;
    }
    return 0;
}

static void add_capture(LamInfo *info, const char *name, size_t len) {
    for (int i = 0; i < info->n_caps; ++i) {
        if (info->caps[i].len == len &&
            memcmp(info->caps[i].name, name, len) == 0) return;
    }
    if (info->n_caps == info->cap_caps) {
        info->cap_caps = info->cap_caps ? info->cap_caps * 2 : 4;
        info->caps = (Capture *) realloc(info->caps, info->cap_caps * sizeof(Capture));
    }
    info->caps[info->n_caps].name = name;
    info->caps[info->n_caps].len  = len;
    info->n_caps++;
}

static void collect_free_vars(E *e, Node *n, Node *lam, LamInfo *info);

/* Interpolations inside string literals are not part of the main AST:
   they are re-parsed at emit time. Free-variable collection therefore
   has to re-parse them too, or closures written as e.g.
       (t) => eprint("#{outer}: ...")
   will fail to capture `outer`. Keep this in lock-step with the logic
   in emit_string_expr. */
static void collect_free_vars_in_string(E *e, Node *s, Node *lam, LamInfo *info) {
    const char *src = s->name;
    if (!src || s->name_len < 2) return;
    int triple = (s->v.flags & 0x1) != 0;
    size_t start = triple ? 3 : 1;
    size_t end   = triple ? s->name_len - 3 : s->name_len - 1;
    size_t i = start;
    while (i < end) {
        if (src[i] == '#' && i + 1 < end && src[i + 1] == '{') {
            i += 2;
            size_t expr_start = scan_interp_expr(src, &i, end);
            size_t ntk = 0;
            Token *toks = kai_lex("<interp>", src + expr_start, i - expr_start, &ntk);
            Node *en = kai_parse_expr_standalone("<interp>", src + expr_start, toks, ntk);
            if (en) {
                collect_free_vars(e, en, lam, info);
                kai_free_node(en);
            }
            free(toks);
            if (i < end) i++;                 /* past } */
        } else {
            ++i;
        }
    }
}

/* N_IDENT: capture it unless it's a lambda param, a global, or a local. */
static void collect_ident_free_var(E *e, Node *n, Node *lam, LamInfo *info) {
    if (!is_lambda_param(lam, n->name, n->name_len) &&
        !is_global_name(e, n->name, n->name_len) &&
        !ls_has(e, n->name, n->name_len)) {
        add_capture(info, n->name, n->name_len);
    }
}

/* N_ARM: the pattern binds locals (own scope) visible to guard and body. */
static void collect_arm_free_vars(E *e, Node *n, Node *lam, LamInfo *info) {
    int has_guard = (n->v.flags & 0x1) != 0;
    Node *pat      = n->children[0];
    Node *body_idx = n->children[has_guard ? 2 : 1];
    Node *guard    = has_guard ? n->children[1] : NULL;
    ls_push_mark(e);
    pat_add_locals(e, pat);
    if (guard)    collect_free_vars(e, guard,    lam, info);
    if (body_idx) collect_free_vars(e, body_idx, lam, info);
    ls_pop_mark(e);
}

static void collect_free_vars(E *e, Node *n, Node *lam, LamInfo *info) {
    if (!n) return;
    switch (n->kind) {
        case N_LAMBDA: return;                 /* inner lambdas have their own */
        case N_IDENT:  collect_ident_free_var(e, n, lam, info);        return;
        case N_STRING: collect_free_vars_in_string(e, n, lam, info);   return;
        case N_FIELD:
            if (n->n_children >= 1) collect_free_vars(e, n->children[0], lam, info);
            return;
        case N_ARM:    collect_arm_free_vars(e, n, lam, info);         return;
        case N_LET:
            /* The RHS is walked before the pattern binds new locals, so
               `let x = expr` cannot see x in expr. */
            if (n->n_children >= 3) collect_free_vars(e, n->children[2], lam, info);
            if (n->n_children >= 1) pat_add_locals(e, n->children[0]);
            return;
        case N_BLOCK:
            /* Own scope so a nested `let` doesn't leak. */
            ls_push_mark(e);
            for (size_t i = 0; i < n->n_children; ++i)
                collect_free_vars(e, n->children[i], lam, info);
            ls_pop_mark(e);
            return;
        default:
            for (size_t i = 0; i < n->n_children; ++i)
                collect_free_vars(e, n->children[i], lam, info);
            return;
    }
}

static LamInfo *register_lambda(E *e, Node *lam) {
    if (e->n_lams == e->cap_lams) {
        e->cap_lams = e->cap_lams ? e->cap_lams * 2 : 8;
        e->lams = (LamInfo *) realloc(e->lams, e->cap_lams * sizeof(LamInfo));
    }
    LamInfo *info = &e->lams[e->n_lams];
    memset(info, 0, sizeof(LamInfo));
    info->id  = (int) e->n_lams;
    info->lam = lam;
    e->n_lams++;
    if (lam->n_children >= 1) {
        /* The body starts in a fresh local scope. Lambda params are
           already covered by is_lambda_param, but we still push a mark
           so any let/pattern binds inside the body are tracked against
           this lambda's scope and popped cleanly afterwards. */
        ls_push_mark(e);
        collect_free_vars(e, lam->children[0], lam, info);
        ls_pop_mark(e);
    }
    return info;
}

static void collect_lambdas(E *e, Node *n) {
    if (!n) return;
    if (n->kind == N_LAMBDA) {
        register_lambda(e, n);
        /* Recurse into the body for nested lambdas. */
        if (n->n_children >= 1) collect_lambdas(e, n->children[0]);
        return;
    }
    for (size_t i = 0; i < n->n_children; ++i) collect_lambdas(e, n->children[i]);
}

static LamInfo *find_lam_info(E *e, Node *lam) {
    for (size_t i = 0; i < e->n_lams; ++i) {
        if (e->lams[i].lam == lam) return &e->lams[i];
    }
    return NULL;
}

/* ---------- operator helpers ---------- */

static const char *binop_fn(int op) {
    switch (op) {
        case TK_PLUS:        return "kai_op_add";
        case TK_MINUS:       return "kai_op_sub";
        case TK_STAR:        return "kai_op_mul";
        case TK_SLASH:       return "kai_op_div";
        case TK_SLASH_SLASH: return "kai_op_idiv";
        case TK_PERCENT:     return "kai_op_mod";
        case TK_LT:          return "kai_op_lt";
        case TK_GT:          return "kai_op_gt";
        case TK_LE:          return "kai_op_le";
        case TK_GE:          return "kai_op_ge";
        case TK_EQEQ:        return "kai_op_eq_v";
        case TK_NEQ:         return "kai_op_ne_v";
        default:             return NULL;
    }
}

static void emit_binop(E *e, Node *n) {
    int op = n->v.op;
    Node *l = n->children[0], *r = n->children[1];
    if (op == TK_AND) {
        fputs("({ KaiValue *_a = ", e->out); emit_expr(e, l);
        fputs("; kai_op_truthy(_a) ? ", e->out); emit_expr(e, r);
        fputs(" : kai_bool(0); })", e->out);
        return;
    }
    if (op == TK_OR) {
        fputs("({ KaiValue *_a = ", e->out); emit_expr(e, l);
        fputs("; kai_op_truthy(_a) ? kai_bool(1) : ", e->out); emit_expr(e, r);
        fputs("; })", e->out);
        return;
    }
    const char *fn = binop_fn(op);
    if (!fn) { bug(e, n, "unknown binop"); return; }
    fprintf(e->out, "%s(", fn);
    emit_expr(e, l);
    fputs(", ", e->out);
    emit_expr(e, r);
    fputc(')', e->out);
}

static void emit_unop(E *e, Node *n) {
    int op = n->v.op;
    const char *fn = (op == TK_MINUS) ? "kai_op_neg"
                    : (op == TK_NOT)   ? "kai_op_boolnot"
                    : NULL;
    if (!fn) { bug(e, n, "unknown unop"); return; }
    fprintf(e->out, "%s(", fn);
    emit_expr(e, n->children[0]);
    fputc(')', e->out);
}

/* ---------- if / call / pipe / range / list / record / field ---------- */

static void emit_if(E *e, Node *n) {
    Node *cond = n->children[0];
    Node *then_br = n->children[1];
    Node *else_br = (n->v.flags & 0x1) ? n->children[2] : NULL;
    fputs("(kai_op_truthy(", e->out); emit_expr(e, cond); fputs(") ? ", e->out);
    emit_expr(e, then_br);
    fputs(" : ", e->out);
    if (else_br) emit_expr(e, else_br);
    else         fputs("kai_unit()", e->out);
    fputc(')', e->out);
}

static void emit_args_with_prepend(E *e, Node *call, Node *prepended) {
    int first = 1;
    fputc('(', e->out);
    if (prepended) { emit_expr(e, prepended); first = 0; }
    if (call) {
        for (size_t i = 1; i < call->n_children; ++i) {
            if (!first) fputs(", ", e->out);
            first = 0;
            emit_expr(e, call->children[i]);
        }
    }
    fputc(')', e->out);
}

static void emit_variant_construction(E *e, const char *name, size_t name_len,
                                       Node *call_or_null) {
    /* Emits kai_variant_u(<tag>, "<Name>", n_args, 0, <slot array>)
     * where <tag> comes from the global variants table (builtins
     * 0..10, users 11.., see docs/variant-tags.md) and <slot array>
     * is a KaiVarSlot[] with each slot's .ptr filled. Stage 0 only
     * emits boxed args, so the mask is always 0; the typed payload
     * path (Phase 2 #440) lives in stage 2's emitter. */
    size_t n_args = call_or_null ? (call_or_null->n_children - 1) : 0;
    int vtag = 0;
    find_variant(e, name, name_len, NULL, &vtag);
    fprintf(e->out, "kai_variant_u(%d, \"%.*s\", %d, 0, ", vtag, (int) name_len, name, (int) n_args);
    if (n_args == 0) {
        fputs("NULL)", e->out);
    } else {
        fputs("(KaiVarSlot[]){", e->out);
        for (size_t i = 1; i < call_or_null->n_children; ++i) {
            if (i > 1) fputs(", ", e->out);
            fputs("{.ptr = ", e->out);
            emit_expr(e, call_or_null->children[i]);
            fputc('}', e->out);
        }
        fputs("})", e->out);
    }
}

static void emit_call(E *e, Node *n) {
    if ((n->v.flags & TCO_TAIL_CALL) && e->cur_fn) {
        emit_tco_goto(e, n);
        return;
    }
    Node *callee = n->children[0];
    if (callee && callee->kind == N_IDENT) {
        int arity = 0;
        if (find_variant(e, callee->name, callee->name_len, &arity, NULL)) {
            emit_variant_construction(e, callee->name, callee->name_len, n);
            return;
        }
        int handled = emit_ident_callee(e, callee->name, callee->name_len);
        if (handled) {
            emit_args_with_prepend(e, n, NULL);
            return;
        }
        /* Variable holding a closure. Issue #298: kai_apply consumes
         * its closure argument; route the callee identifier through
         * emit_ident_value so multi-use bindings get the eager-dup
         * wrapper and single-use bindings transfer their original ref. */
        fputs("kai_apply(", e->out);
        emit_ident_value(e, callee->name, callee->name_len);
        fprintf(e->out, ", %d, (KaiValue *[]){",
                (int) (n->n_children - 1));
        for (size_t i = 1; i < n->n_children; ++i) {
            if (i > 1) fputs(", ", e->out);
            emit_expr(e, n->children[i]);
        }
        fputs("})", e->out);
        return;
    }
    /* Non-ident callee. */
    fputs("kai_apply(", e->out);
    emit_expr(e, callee);
    fprintf(e->out, ", %d, (KaiValue *[]){", (int) (n->n_children - 1));
    for (size_t i = 1; i < n->n_children; ++i) {
        if (i > 1) fputs(", ", e->out);
        emit_expr(e, n->children[i]);
    }
    fputs("})", e->out);
}

/* Emit `lhs |> rhs(...)` where rhs is a call: a variant constructor with the
   piped value as its first arg, a known callee taking a prepended arg, or a
   general kai_apply with lhs spliced in front of the call's args. */
static void emit_pipe_into_call(E *e, Node *lhs, Node *rhs) {
    Node *callee = rhs->children[0];
    if (callee && callee->kind == N_IDENT) {
        int arity = 0;
        int vtag = 0;
        if (find_variant(e, callee->name, callee->name_len, &arity, &vtag)) {
            /* Variant constructor with piped first arg. Stage 0
             * only emits boxed args, mask is always 0. */
            fprintf(e->out, "kai_variant_u(%d, \"%.*s\", %d, 0, (KaiVarSlot[]){",
                    vtag, (int) callee->name_len, callee->name,
                    (int) rhs->n_children);
            fputs("{.ptr = ", e->out);
            emit_expr(e, lhs);
            fputc('}', e->out);
            for (size_t i = 1; i < rhs->n_children; ++i) {
                fputs(", {.ptr = ", e->out);
                emit_expr(e, rhs->children[i]);
                fputc('}', e->out);
            }
            fputs("})", e->out);
            return;
        }
        int handled = emit_ident_callee(e, callee->name, callee->name_len);
        if (handled) { emit_args_with_prepend(e, rhs, lhs); return; }
    }
    fputs("kai_apply(", e->out);
    emit_expr(e, callee);
    fprintf(e->out, ", %d, (KaiValue *[]){", (int) rhs->n_children);
    emit_expr(e, lhs);
    for (size_t i = 1; i < rhs->n_children; ++i) {
        fputs(", ", e->out);
        emit_expr(e, rhs->children[i]);
    }
    fputs("})", e->out);
}

static void emit_pipe(E *e, Node *n) {
    Node *lhs = n->children[0];
    Node *rhs = n->children[1];
    if (rhs && rhs->kind == N_CALL) {
        emit_pipe_into_call(e, lhs, rhs);
        return;
    }
    if (rhs && rhs->kind == N_IDENT) {
        int handled = emit_ident_callee(e, rhs->name, rhs->name_len);
        if (handled) {
            fputc('(', e->out);
            emit_expr(e, lhs);
            fputc(')', e->out);
            return;
        }
        fputs("kai_apply(", e->out);
        emit_expr(e, rhs);
        fputs(", 1, (KaiValue *[]){", e->out);
        emit_expr(e, lhs);
        fputs("})", e->out);
        return;
    }
    bug(e, n, "|> right side must be a call or function name");
}

static void emit_range(E *e, Node *n) {
    int has_step = (n->v.flags & 0x1) != 0;
    if (has_step) {
        fputs("kai_range_step(", e->out);
        emit_expr(e, n->children[0]); fputs(", ", e->out);
        emit_expr(e, n->children[1]); fputs(", ", e->out);
        emit_expr(e, n->children[2]); fputc(')', e->out);
    } else {
        fputs("kai_range(", e->out);
        emit_expr(e, n->children[0]); fputs(", ", e->out);
        emit_expr(e, n->children[1]); fputc(')', e->out);
    }
}

static void emit_list_tail(E *e, Node *lit, size_t i) {
    if (i >= lit->n_children) { fputs("kai_nil()", e->out); return; }
    Node *elt = lit->children[i];
    int is_last = (i + 1 == lit->n_children);
    if (elt && elt->kind == N_SPREAD) {
        /* Tail spread `[..., ...xs]`: no wrapper list_append — the
           spread value is the whole remainder. Incref preserves the
           ownership contract (result RC=1, source untouched) that
           list_append gave via spine copy, in O(1) vs O(|xs|). */
        if (is_last) {
            fputs("kai_incref(", e->out);
            emit_expr(e, elt->children[0]);
            fputc(')', e->out);
        } else {
            fputs("kai_prelude_list_append(", e->out);
            emit_expr(e, elt->children[0]);
            fputs(", ", e->out);
            emit_list_tail(e, lit, i + 1);
            fputc(')', e->out);
        }
    } else {
        fputs("kai_cons(", e->out);
        emit_expr(e, elt);
        fputs(", ", e->out);
        emit_list_tail(e, lit, i + 1);
        fputc(')', e->out);
    }
}

static void emit_list(E *e, Node *n) { emit_list_tail(e, n, 0); }

static void emit_record_lit(E *e, Node *n) {
    int nf = (int) n->n_children;
    fprintf(e->out, "kai_record(%d, (KaiValue *[]){", nf);
    for (int i = 0; i < nf; ++i) {
        if (i > 0) fputs(", ", e->out);
        Node *fi = n->children[i];
        emit_expr(e, fi->children[0]);
    }
    fputs("}, (const char *[]){", e->out);
    for (int i = 0; i < nf; ++i) {
        if (i > 0) fputs(", ", e->out);
        Node *fi = n->children[i];
        fprintf(e->out, "\"%.*s\"", (int) fi->name_len, fi->name);
    }
    fputs("})", e->out);
}

static void emit_field_access(E *e, Node *n) {
    fputs("kai_op_field(", e->out);
    emit_expr(e, n->children[0]);
    fprintf(e->out, ", \"%.*s\")", (int) n->name_len, n->name);
}

/* ---------- match ---------- */

static void emit_pat_test(E *e, Node *pat, const char *scr) {
    switch (pat->kind) {
        case N_PAT_WILD:
        case N_PAT_BIND:
            fputs("1", e->out); return;

        case N_PAT_LIT: {
            Node *lit = pat->children[0];
            fprintf(e->out, "kai_op_eq(%s, ", scr);
            emit_expr(e, lit);
            fputc(')', e->out);
            return;
        }

        case N_PAT_LIST: {
            int has_rest = (pat->v.flags & 0x1) != 0;
            size_t n_fixed = has_rest ? (pat->n_children - 1) : pat->n_children;

            if (n_fixed == 0 && !has_rest) {
                fprintf(e->out, "(%s && %s->tag == KAI_NIL)", scr, scr);
                return;
            }

            fputs("(", e->out);
            char cur[512];
            snprintf(cur, sizeof(cur), "%s", scr);
            int first = 1;
            for (size_t i = 0; i < n_fixed; ++i) {
                if (!first) fputs(" && ", e->out);
                first = 0;
                fprintf(e->out, "%s && %s->tag == KAI_CONS && ", cur, cur);
                char head[512];
                snprintf(head, sizeof(head), "%s->as.cons.head", cur);
                emit_pat_test(e, pat->children[i], head);
                char nxt[512];
                snprintf(nxt, sizeof(nxt), "%s->as.cons.tail", cur);
                memcpy(cur, nxt, sizeof(cur));
            }
            if (!has_rest) {
                if (!first) fputs(" && ", e->out);
                fprintf(e->out, "%s && %s->tag == KAI_NIL", cur, cur);
            }
            fputs(")", e->out);
            return;
        }

        case N_PAT_VARIANT: {
            fprintf(e->out, "(%s && %s->tag == KAI_VARIANT && strcmp(kai_variant_name_of(%s->variant_tag), \"%.*s\") == 0",
                    scr, scr, scr, (int) pat->name_len, pat->name);
            for (size_t i = 0; i < pat->n_children; ++i) {
                fputs(" && ", e->out);
                char sub[512];
                snprintf(sub, sizeof(sub), "%s->as.var_slots[%zu].ptr", scr, i);
                emit_pat_test(e, pat->children[i], sub);
            }
            fputs(")", e->out);
            return;
        }

        case N_PAT_RECORD: {
            /* m5.x §4b: pat_test reads field tags / values to decide
               the arm; it does not consume the field. Use the
               borrowing variant so failing arms do not leak the
               field's ref. emit_pat_binds keeps using the incref-ing
               kai_op_field below. */
            fprintf(e->out, "(%s && %s->tag == KAI_RECORD", scr, scr);
            for (size_t i = 0; i < pat->n_children; ++i) {
                Node *pf = pat->children[i];
                char tmp[512];
                snprintf(tmp, sizeof(tmp),
                         "kai_op_field_borrow(%s, \"%.*s\")", scr,
                         (int) pf->name_len, pf->name);
                fputs(" && (", e->out);
                emit_pat_test(e, pf->children[0], tmp);
                fputs(")", e->out);
            }
            fputs(")", e->out);
            return;
        }

        default:
            bug(e, pat, nk_name(pat->kind));
            fputs("0", e->out);
            return;
    }
}

static void emit_pat_binds(E *e, Node *pat, const char *scr, int is_alias) {
    switch (pat->kind) {
        case N_PAT_WILD:
        case N_PAT_LIT:
            return;

        case N_PAT_BIND:
            if (is_alias) {
                fprintf(e->out, "KaiValue *kai_%.*s = kai_incref(%s); ",
                        (int) pat->name_len, pat->name, scr);
            } else {
                fprintf(e->out, "KaiValue *kai_%.*s = %s; ",
                        (int) pat->name_len, pat->name, scr);
            }
            return;

        case N_PAT_LIST: {
            int has_rest = (pat->v.flags & 0x1) != 0;
            size_t n_fixed = has_rest ? (pat->n_children - 1) : pat->n_children;
            char cur[512];
            snprintf(cur, sizeof(cur), "%s", scr);
            for (size_t i = 0; i < n_fixed; ++i) {
                char head[512];
                snprintf(head, sizeof(head), "%s->as.cons.head", cur);
                /* cons.head is alias of cur's storage. */
                emit_pat_binds(e, pat->children[i], head, 1);
                char nxt[512];
                snprintf(nxt, sizeof(nxt), "%s->as.cons.tail", cur);
                memcpy(cur, nxt, sizeof(cur));
            }
            if (has_rest) {
                Node *rest_pat = pat->children[n_fixed];
                /* rest is also alias of cur's storage. */
                emit_pat_binds(e, rest_pat, cur, 1);
            }
            return;
        }

        case N_PAT_VARIANT: {
            for (size_t i = 0; i < pat->n_children; ++i) {
                char sub[512];
                /* Issue #440 — variant slot. Phase 1: pointer-only,
                 * mask=0, so `.ptr` is the boxed child as before. */
                snprintf(sub, sizeof(sub), "%s->as.var_slots[%zu].ptr", scr, i);
                /* variant arg slot is alias of scr's storage. */
                emit_pat_binds(e, pat->children[i], sub, 1);
            }
            return;
        }

        case N_PAT_RECORD: {
            for (size_t i = 0; i < pat->n_children; ++i) {
                Node *pf = pat->children[i];
                char tmp[512];
                snprintf(tmp, sizeof(tmp),
                         "kai_op_field(%s, \"%.*s\")", scr,
                         (int) pf->name_len, pf->name);
                /* kai_op_field already incrs its return; child is owned. */
                emit_pat_binds(e, pf->children[0], tmp, 0);
            }
            return;
        }

        default: return;
    }
}

static void emit_match(E *e, Node *m) {
    fputs("({ KaiValue *_scr = ", e->out);
    emit_expr(e, m->children[0]);
    fputs("; KaiValue *_r = kai_unit(); do {\n", e->out);
    for (size_t i = 1; i < m->n_children; ++i) {
        Node *arm = m->children[i];
        int has_guard = (arm->v.flags & 0x1) != 0;
        Node *pat = arm->children[0];
        Node *guard = has_guard ? arm->children[1] : NULL;
        Node *body = arm->children[has_guard ? 2 : 1];

        fputs("    if (", e->out);
        emit_pat_test(e, pat, "_scr");
        fputs(") { ", e->out);
        /* Each arm opens its own local scope so pattern bindings don't
           leak and don't shadow siblings. */
        ls_push_mark(e);
        /* _scr is shared across all match arms — the next arm reads
         * _scr's slots if this arm's pat_test or guard rejects, and a
         * guard inside this arm may consume the binding (e.g. const-
         * pattern desugar emits `__cv_NAME == NAME()`). Treat the
         * binding as alias so PBind/PAs incref; subsequent arms keep
         * a live _scr to test against. _scr leaks one ref per match,
         * matching the pre-flip baseline. */
        emit_pat_binds(e, pat, "_scr", 1);
        pat_add_locals(e, pat);
        if (has_guard) {
            fputs("if (kai_op_truthy(", e->out);
            emit_expr(e, guard);
            fputs(")) { _r = ", e->out);
            emit_expr(e, body);
            fputs("; break; } } ", e->out);
        } else {
            fputs("_r = ", e->out);
            emit_expr(e, body);
            fputs("; break; }\n", e->out);
        }
        ls_pop_mark(e);
    }
    fputs("    kai_prelude_panic(kai_str(\"non-exhaustive match\"));\n", e->out);
    /* m5.x §4b: consume `_scr` linearly. Stage 0's eager-dup retrofit
       wraps every multi-use local read in kai_internal_dup, so when the
       scrutinee is a bare binding read it arrives as a fresh ref;
       single-use, non-captured reads transfer the binding's own ref
       (whose only consumer is this match). emit_pat_binds with
       is_alias=true increfs each PBind so the bindings hold their own
       refs and stay live past the decref below. */
    fputs("} while (0); kai_decref(_scr); _r; })", e->out);
}

/* ---------- string literal (with interpolation) ---------- */

static void write_c_string_slice(FILE *out, const char *src, size_t start, size_t end) {
    fputc('"', out);
    for (size_t i = start; i < end; ++i) {
        unsigned char c = (unsigned char) src[i];
        fputc(c, out);
    }
    fputc('"', out);
}

static void emit_string_expr(E *e, Node *s) {
    const char *src = s->name;
    size_t len = s->name_len;
    int triple = (s->v.flags & 0x1) != 0;
    size_t start = triple ? 3 : 1;
    size_t end   = triple ? len - 3 : len - 1;

    /* Collect parts in order. */
    typedef struct {
        int        is_expr;
        size_t     lit_start, lit_end;
        const char *expr_src;
        size_t     expr_src_len;
    } Part;
    Part parts[64];
    int  n_parts = 0;

    size_t i = start;
    size_t lit_start = start;
    while (i < end) {
        if (src[i] == '#' && i + 1 < end && src[i + 1] == '{') {
            if (i > lit_start) {
                parts[n_parts].is_expr = 0;
                parts[n_parts].lit_start = lit_start;
                parts[n_parts].lit_end   = i;
                n_parts++;
            }
            i += 2;
            size_t expr_start = scan_interp_expr(src, &i, end);
            parts[n_parts].is_expr      = 1;
            parts[n_parts].expr_src     = src + expr_start;
            parts[n_parts].expr_src_len = i - expr_start;
            n_parts++;
            if (i < end) i++;              /* past } */
            lit_start = i;
        } else {
            ++i;
        }
    }
    if (lit_start < end) {
        parts[n_parts].is_expr = 0;
        parts[n_parts].lit_start = lit_start;
        parts[n_parts].lit_end   = end;
        n_parts++;
    }

    if (n_parts == 0) { fputs("kai_str(\"\")", e->out); return; }

    /* Emit as nested kai_string_concat. */
    for (int k = 0; k < n_parts - 1; ++k) fputs("kai_string_concat(", e->out);
    for (int k = 0; k < n_parts; ++k) {
        if (k > 0) fputs(", ", e->out);
        if (parts[k].is_expr) {
            /* Parse and emit. Interpolations may introduce new lambdas;
               collect and register them before emitting. */
            size_t ntk = 0;
            Token *toks = kai_lex("<interp>", parts[k].expr_src, parts[k].expr_src_len, &ntk);
            Node *en = kai_parse_expr_standalone("<interp>", parts[k].expr_src, toks, ntk);
            if (!en) { bug(e, s, "failed to parse interpolation"); fputs("kai_str(\"\")", e->out); }
            else {
                collect_lambdas(e, en);
                fputs("kai_to_string(", e->out);
                emit_expr(e, en);
                fputc(')', e->out);
                kai_free_node(en);
            }
            free(toks);
        } else {
            fputs("kai_str(", e->out);
            write_c_string_slice(e->out, src, parts[k].lit_start, parts[k].lit_end);
            fputc(')', e->out);
        }
        if (k > 0) fputc(')', e->out);
    }
}

/* ---------- lambda ---------- */

static void emit_lambda_ref(E *e, Node *lam) {
    LamInfo *info = find_lam_info(e, lam);
    if (!info) { bug(e, lam, "unregistered lambda"); return; }
    int n_params = (int) lam->n_children - 1;
    fprintf(e->out, "kai_closure(&_kai_lam_%d, %d, %d, ",
            info->id, n_params, info->n_caps);
    if (info->n_caps == 0) {
        fputs("NULL)", e->out);
    } else {
        fputs("(KaiValue *[]){", e->out);
        for (int i = 0; i < info->n_caps; ++i) {
            if (i > 0) fputs(", ", e->out);
            fprintf(e->out, "kai_%.*s", (int) info->caps[i].len, info->caps[i].name);
        }
        fputs("})", e->out);
    }
}

static void emit_lambda_helper_def(E *e, LamInfo *info) {
    Node *lam = info->lam;
    int n_params = (int) lam->n_children - 1;
    fprintf(e->out, "static KaiValue *_kai_lam_%d(KaiValue *self, KaiValue **args, int n) {\n",
            info->id);
    fputs("    (void) self; (void) n;\n", e->out);
    ls_push_mark(e);
    /* Lambda params and captures use decl=NULL: the lambda body's
     * use counter is not run (per-fn scope only), so a non-NULL
     * decl would still find no LocalUse entry — passing NULL keeps
     * the intent explicit and shaves the lookup. */
    for (int i = 0; i < n_params; ++i) {
        Node *p = lam->children[1 + i];
        fprintf(e->out, "    KaiValue *kai_%.*s = args[%d];\n",
                (int) p->name_len, p->name, i);
        ls_add(e, p->name, p->name_len, NULL);
    }
    for (int i = 0; i < info->n_caps; ++i) {
        fprintf(e->out, "    KaiValue *kai_%.*s = self->as.clo.captures[%d];\n",
                (int) info->caps[i].len, info->caps[i].name, i);
        ls_add(e, info->caps[i].name, info->caps[i].len, NULL);
    }
    fputs("    return ", e->out);
    emit_expr(e, lam->children[0]);
    fputs(";\n}\n\n", e->out);
    ls_pop_mark(e);
}

/* ---------- expression dispatch ---------- */

static void emit_expr(E *e, Node *n) {
    if (!n) { fputs("kai_unit()", e->out); return; }

    switch (n->kind) {
        case N_UNIT: fputs("kai_unit()", e->out); return;
        case N_BOOL: fprintf(e->out, "kai_bool(%d)", n->v.b ? 1 : 0); return;
        case N_INT:  fprintf(e->out, "kai_int(%lldLL)", (long long) n->v.i); return;
        case N_REAL: fprintf(e->out, "kai_real(%.17g)", n->v.r); return;
        case N_CHAR: fprintf(e->out, "kai_char(0x%08X)", (unsigned) n->v.c); return;
        case N_STRING:     emit_string_expr(e, n); return;
        case N_IDENT:      emit_ident_value(e, n->name, n->name_len); return;
        case N_CALL:       emit_call(e, n);     return;
        case N_PIPE:       emit_pipe(e, n);     return;
        case N_BINOP:      emit_binop(e, n);    return;
        case N_UNOP:       emit_unop(e, n);     return;
        case N_IF:         emit_if(e, n);       return;
        case N_RANGE_LIT:  emit_range(e, n);    return;
        case N_LIST_LIT:   emit_list(e, n);     return;
        case N_MATCH:      emit_match(e, n);    return;
        case N_RECORD_LIT: emit_record_lit(e, n); return;
        case N_FIELD:      emit_field_access(e, n); return;
        case N_LAMBDA:     emit_lambda_ref(e, n); return;

        case N_BLOCK: {
            size_t n_stmts = (n->n_children > 0) ? n->n_children - 1 : 0;
            Node *value = (n->n_children > 0) ? n->children[n->n_children - 1] : NULL;
            int has_value = (n->v.flags & 0x1) != 0;
            if (n_stmts == 0 && has_value) { emit_expr(e, value); return; }
            /* Block scope: let bindings inside the block must not leak
               out or shadow across block boundaries. */
            fputs("({ ", e->out);
            ls_push_mark(e);
            for (size_t i = 0; i < n_stmts; ++i) { emit_stmt(e, n->children[i]); fputc(' ', e->out); }
            if (has_value) { emit_expr(e, value); fputs("; ", e->out); }
            else            { fputs("kai_unit(); ", e->out); }
            ls_pop_mark(e);
            fputs("})", e->out);
            return;
        }

        case N_PLACEHOLDER:
            bug(e, n, "placeholder `.` — use explicit `x => expr` in stage 0");
            fputs("kai_unit()", e->out);
            return;

        case N_INDEX:
        case N_SPREAD:
        default:
            bug(e, n, nk_name(n->kind));
            fputs("kai_unit()", e->out);
            return;
    }
}

/* ---------- statements ---------- */

static void emit_let(E *e, Node *n) {
    Node *pat = n->children[0];
    Node *val = n->children[2];
    if (pat->kind == N_PAT_BIND) {
        fprintf(e->out, "KaiValue *kai_%.*s = ", (int) pat->name_len, pat->name);
        emit_expr(e, val);
        fputs(";", e->out);
        /* Record the binding *after* emitting the RHS so the RHS does
           not see the new name. The pat node itself is the binding
           identity used by the use counter. */
        ls_add(e, pat->name, pat->name_len, pat);
        return;
    }
    fputs("KaiValue *_letv = ", e->out);
    emit_expr(e, val);
    fputs("; ", e->out);
    /* _letv holds the RHS's owned ref; not aliased from any other producer. */
    emit_pat_binds(e, pat, "_letv", 0);
    pat_add_locals(e, pat);
}

/* Emit the test-description argument for kai_test_fail (the raw string
   literal span, or "" when outside a `test` block). */
static void emit_test_desc(E *e) {
    if (e->cur_test_desc_start) {
        fprintf(e->out, "%.*s", (int) e->cur_test_desc_len, e->cur_test_desc_start);
    } else {
        fputs("\"\"", e->out);
    }
}

static void emit_assert(E *e, Node *n) {
    int has_msg = (n->v.flags & 0x1) != 0;
    if (!e->test_mode) {
        /* Outside tests, a failed assertion panics. */
        fputs("{ KaiValue *_c = ", e->out);
        emit_expr(e, n->children[0]);
        fputs("; if (!kai_op_truthy(_c)) { kai_prelude_panic(kai_str(\"assertion failed\")); } }", e->out);
        return;
    }
    fputs("{ KaiValue *_c = ", e->out);
    emit_expr(e, n->children[0]);
    if (has_msg) {
        /* assert cond, msg — msg is an expression stringified for the report. */
        fputs("; if (!kai_op_truthy(_c)) { KaiValue *_m = ", e->out);
        emit_expr(e, n->children[1]);
        fputs("; kai_test_fail(", e->out);
        emit_test_desc(e);
        fputs(", (_m && _m->tag == KAI_STR) ? _m->as.s.bytes : \"assertion failed\"); return; } }", e->out);
    } else {
        fputs("; if (!kai_op_truthy(_c)) { kai_test_fail(", e->out);
        emit_test_desc(e);
        fputs(", \"assertion failed\"); return; } }", e->out);
    }
}

static void emit_stmt(E *e, Node *n) {
    if (!n) return;
    switch (n->kind) {
        case N_LET:    emit_let(e, n);    return;
        case N_ASSERT: emit_assert(e, n); return;
        case N_EXPR_STMT:
            fputs("{ KaiValue *_ = ", e->out);
            emit_expr(e, n->children[0]);
            fputs("; (void) _; }", e->out);
            return;
        default:
            fputs("{ KaiValue *_ = ", e->out);
            emit_expr(e, n);
            fputs("; (void) _; }", e->out);
            return;
    }
}

/* ---------- declarations ---------- */

/* ---------- m37 / TCO — self-tail-call rewrite (issue #42) --------
 *
 * Stage-0 mirror of the rewrite implemented in stage 1 / stage 2's
 * compiler.kai. Goal: kaic1's binary (which kaic0 emits) uses
 * `goto _kai_<name>_entry` for self-tail-recursive fns so the
 * bootstrap chain on Linux's 8 MiB main-thread stack survives
 * lexing the ~33 k tokens of stage2/compiler.kai without the
 * RLIMIT_STACK runtime constructor.
 *
 * Approach: a pre-emit pass walks the fn body, marks every
 * tail-position N_CALL whose callee is the enclosing fn (matched
 * by name, with the right arity, not shadowed by a local
 * binding), and emit_call recognises the marker on its way out.
 * The label `_kai_<name>_entry:;` is planted by emit_fn_body
 * only when at least one call was marked, so non-recursive fns
 * keep their byte-identical output.
 *
 * Tail position propagates through:
 *   - N_IF then / else branches
 *   - N_MATCH arm bodies
 *   - N_BLOCK trailing value
 * It does NOT propagate through N_LAMBDA bodies (different fn),
 * N_CALL args, N_PIPE, N_BINOP, N_UNOP, etc.
 *
 * Refcount discipline at the goto site mirrors the stage 1 / 2
 * port: for each parameter, drop the slot iff the slot would
 * still own a ref at the wrap's natural exit. In stage 0 that
 * is captured by emit_ident_value's single-use predicate
 * (`total_count == 1 && lambda_count == 0` → bare emit, ref
 * already transferred; otherwise → kai_internal_dup wrap, slot
 * retains ref). Drop is therefore the negation of that predicate. */

typedef struct {
    const char **names;   /* borrowed from source, never freed */
    size_t      *lens;
    size_t       n, cap;
} TcoShadows;

static void tcs_push(TcoShadows *s, const char *name, size_t len) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->names = (const char **) realloc(s->names, s->cap * sizeof(*s->names));
        s->lens  = (size_t *)       realloc(s->lens,  s->cap * sizeof(*s->lens));
    }
    s->names[s->n] = name;
    s->lens[s->n]  = len;
    s->n++;
}

static int tcs_has(TcoShadows *s, const char *name, size_t len) {
    for (size_t i = 0; i < s->n; ++i) {
        if (s->lens[i] == len && memcmp(s->names[i], name, len) == 0) return 1;
    }
    return 0;
}

static void tco_collect_pat_binds(TcoShadows *s, Node *pat) {
    if (!pat) return;
    switch (pat->kind) {
        case N_PAT_BIND:
            tcs_push(s, pat->name, pat->name_len);
            break;
        case N_PAT_LIST:
        case N_PAT_VARIANT:
        case N_PAT_RECORD:
            for (size_t i = 0; i < pat->n_children; ++i)
                tco_collect_pat_binds(s, pat->children[i]);
            break;
        case N_PAT_FIELD: {
            int shorthand = (pat->v.flags & 0x1) != 0;
            if (pat->n_children >= 1) tco_collect_pat_binds(s, pat->children[0]);
            else if (shorthand) tcs_push(s, pat->name, pat->name_len);
            break;
        }
        default:
            break;
    }
}

static int tco_walk_tail(TcoShadows *s, Node *body,
                         const char *name, size_t name_len, int arity);

static int tco_walk_block_tail(TcoShadows *s, Node *body,
                               const char *name, size_t name_len, int arity) {
    int has_value = (body->v.flags & 0x1) != 0;
    if (!has_value || body->n_children == 0) return 0;
    size_t mark = s->n;
    for (size_t i = 0; i + 1 < body->n_children; ++i) {
        Node *st = body->children[i];
        if (st && st->kind == N_LET) {
            Node *pat = st->children[0];
            tco_collect_pat_binds(s, pat);
        }
    }
    int hit = tco_walk_tail(s, body->children[body->n_children - 1],
                            name, name_len, arity);
    s->n = mark;
    return hit;
}

static int tco_walk_match_arms(TcoShadows *s, Node *m,
                               const char *name, size_t name_len, int arity) {
    for (size_t i = 1; i < m->n_children; ++i) {
        Node *arm = m->children[i];
        if (!arm || arm->kind != N_ARM || arm->n_children == 0) continue;
        size_t mark = s->n;
        Node *pat  = arm->children[0];
        Node *body = arm->children[arm->n_children - 1];
        tco_collect_pat_binds(s, pat);
        int hit = tco_walk_tail(s, body, name, name_len, arity);
        s->n = mark;
        if (hit) return 1;
    }
    return 0;
}

static int tco_walk_tail(TcoShadows *s, Node *body,
                         const char *name, size_t name_len, int arity) {
    if (!body) return 0;
    switch (body->kind) {
        case N_CALL: {
            if (body->n_children == 0) return 0;
            Node *callee = body->children[0];
            if (!callee || callee->kind != N_IDENT) return 0;
            if (callee->name_len != name_len) return 0;
            if (memcmp(callee->name, name, name_len) != 0) return 0;
            if (tcs_has(s, name, name_len)) return 0;
            if ((int) (body->n_children - 1) != arity) return 0;
            return 1;
        }
        case N_IF: {
            if (body->n_children < 2) return 0;
            int has_else = (body->v.flags & 0x1) != 0;
            if (tco_walk_tail(s, body->children[1], name, name_len, arity)) return 1;
            if (has_else && body->n_children >= 3 &&
                tco_walk_tail(s, body->children[2], name, name_len, arity)) return 1;
            return 0;
        }
        case N_MATCH:
            return tco_walk_match_arms(s, body, name, name_len, arity);
        case N_BLOCK:
            return tco_walk_block_tail(s, body, name, name_len, arity);
        default:
            return 0;
    }
}

static void tco_mark_block_tail(TcoShadows *s, Node *body,
                                const char *name, size_t name_len, int arity);
static void tco_mark_match_arms(TcoShadows *s, Node *m,
                                const char *name, size_t name_len, int arity);

static void tco_mark(TcoShadows *s, Node *body,
                     const char *name, size_t name_len, int arity) {
    if (!body) return;
    switch (body->kind) {
        case N_CALL: {
            if (body->n_children == 0) return;
            Node *callee = body->children[0];
            if (!callee || callee->kind != N_IDENT) return;
            if (callee->name_len != name_len) return;
            if (memcmp(callee->name, name, name_len) != 0) return;
            if (tcs_has(s, name, name_len)) return;
            if ((int) (body->n_children - 1) != arity) return;
            body->v.flags |= TCO_TAIL_CALL;
            return;
        }
        case N_IF: {
            if (body->n_children < 2) return;
            int has_else = (body->v.flags & 0x1) != 0;
            tco_mark(s, body->children[1], name, name_len, arity);
            if (has_else && body->n_children >= 3)
                tco_mark(s, body->children[2], name, name_len, arity);
            return;
        }
        case N_MATCH:
            tco_mark_match_arms(s, body, name, name_len, arity);
            return;
        case N_BLOCK:
            tco_mark_block_tail(s, body, name, name_len, arity);
            return;
        default:
            return;
    }
}

static void tco_mark_match_arms(TcoShadows *s, Node *m,
                                const char *name, size_t name_len, int arity) {
    for (size_t i = 1; i < m->n_children; ++i) {
        Node *arm = m->children[i];
        if (!arm || arm->kind != N_ARM || arm->n_children == 0) continue;
        size_t mark = s->n;
        Node *pat  = arm->children[0];
        Node *body = arm->children[arm->n_children - 1];
        tco_collect_pat_binds(s, pat);
        tco_mark(s, body, name, name_len, arity);
        s->n = mark;
    }
}

static void tco_mark_block_tail(TcoShadows *s, Node *body,
                                const char *name, size_t name_len, int arity) {
    int has_value = (body->v.flags & 0x1) != 0;
    if (!has_value || body->n_children == 0) return;
    size_t mark = s->n;
    for (size_t i = 0; i + 1 < body->n_children; ++i) {
        Node *st = body->children[i];
        if (st && st->kind == N_LET) {
            Node *pat = st->children[0];
            tco_collect_pat_binds(s, pat);
        }
    }
    tco_mark(s, body->children[body->n_children - 1], name, name_len, arity);
    s->n = mark;
}

/* Emit the rebind+goto block for a tail-self-call N_CALL. The
 * statement-expression yields `(KaiValue *)0` after the goto so it
 * can sit inside the `return ({ ... });` shape the surrounding
 * emit produces. */
static void emit_tco_goto(E *e, Node *call) {
    Node *fn = e->cur_fn;
    fputs("({ ", e->out);
    /* 1. Args into _t<i> in declaration order. */
    int i = 0;
    for (size_t a = 1; a < call->n_children; ++a, ++i) {
        fprintf(e->out, "KaiValue *_t%d = ", i);
        emit_expr(e, call->children[a]);
        fputs("; ", e->out);
    }
    /* 2. Drop old params whose slot still owns a ref. */
    for (size_t j = 2; j < fn->n_children; ++j) {
        Node *p = fn->children[j];
        if (!p || p->kind != N_PARAM) continue;
        LocalUse *u = lu_lookup(e, p);
        int single_use = (u && u->total_count == 1 && u->lambda_count == 0);
        if (!single_use) {
            fprintf(e->out,
                    "{ KaiValue *_ = kai_internal_drop(kai_%.*s); (void) _; } ",
                    (int) p->name_len, p->name);
        }
    }
    /* 3. Rebind kai_<pname> = _t<i> in declaration order. */
    int p_idx = 0;
    for (size_t j = 2; j < fn->n_children; ++j) {
        Node *p = fn->children[j];
        if (!p || p->kind != N_PARAM) continue;
        fprintf(e->out, "kai_%.*s = _t%d; ",
                (int) p->name_len, p->name, p_idx);
        ++p_idx;
    }
    /* 4. Goto entry. */
    fprintf(e->out, "goto _kai_%.*s_entry; (KaiValue *)0; })",
            (int) fn->name_len, fn->name);
}

static void emit_fn_signature(E *e, Node *fn) {
    fprintf(e->out, "static KaiValue *kai_%.*s(", (int) fn->name_len, fn->name);
    int first = 1;
    for (size_t i = 2; i < fn->n_children; ++i) {
        Node *param = fn->children[i];
        if (!param || param->kind != N_PARAM) continue;
        if (!first) fputs(", ", e->out);
        first = 0;
        fprintf(e->out, "KaiValue *kai_%.*s", (int) param->name_len, param->name);
    }
    if (first) fputs("void", e->out);
    fputc(')', e->out);
}

static void emit_fn_body(E *e, Node *fn) {
    emit_fn_signature(e, fn);

    /* Issue #42 — pre-emit pass: detect tail-self-calls in `body`
     * and mark them with TCO_TAIL_CALL so emit_call lowers each
     * one to a rebind+goto block. The label `_kai_<name>_entry:;`
     * is planted only when at least one call was marked, so
     * non-recursive fns keep their byte-identical output. */
    int arity = 0;
    for (size_t i = 2; i < fn->n_children; ++i) {
        Node *p = fn->children[i];
        if (p && p->kind == N_PARAM) arity++;
    }
    Node *body = fn->children[1];
    int has_tco = 0;
    {
        TcoShadows s = { NULL, NULL, 0, 0 };
        if (tco_walk_tail(&s, body, fn->name, fn->name_len, arity)) {
            tco_mark(&s, body, fn->name, fn->name_len, arity);
            has_tco = 1;
        }
        free(s.names);
        free(s.lens);
    }

    if (has_tco) {
        fprintf(e->out, " {\n    _kai_%.*s_entry:;\n    return ",
                (int) fn->name_len, fn->name);
    } else {
        fputs(" {\n    return ", e->out);
    }
    e->cur_fn = fn;
    ls_push_mark(e);
    lu_clear(e);
    for (size_t i = 2; i < fn->n_children; ++i) {
        Node *p = fn->children[i];
        if (p && p->kind == N_PARAM) {
            ls_add(e, p->name, p->name_len, p);
            lu_add(e, p, p->name, p->name_len);
        }
    }
    /* Walk the body once to count IDENT references against every
     * tracked binding (params + lets + match-arm binds). The walker
     * pushes/pops scope marks as it descends through blocks, lets,
     * and match arms, so reads resolve to the innermost binding by
     * declaration identity rather than by name alone. emit_expr
     * below then mirrors the same scope shape as it emits, and
     * emit_ident_value uses the resulting use counts to skip the
     * eager-dup wrap on single-use, non-captured bindings. */
    count_local_uses(e, body, 0);
    emit_expr(e, body);
    ls_pop_mark(e);
    e->cur_fn = NULL;
    fputs(";\n}\n\n", e->out);
}

static void emit_fn_thunk(E *e, Node *fn) {
    int arity = 0;
    for (size_t i = 2; i < fn->n_children; ++i) {
        Node *p = fn->children[i];
        if (p && p->kind == N_PARAM) arity++;
    }
    fprintf(e->out,
            "static KaiValue *_kai_%.*s_thunk(KaiValue *self, KaiValue **args, int n) {\n"
            "    (void) self; (void) n;\n"
            "    return kai_%.*s(",
            (int) fn->name_len, fn->name,
            (int) fn->name_len, fn->name);
    for (int i = 0; i < arity; ++i) {
        if (i > 0) fputs(", ", e->out);
        fprintf(e->out, "args[%d]", i);
    }
    fputs(");\n}\n\n", e->out);
}

static int has_main(Node *prog) {
    for (size_t i = 0; i < prog->n_children; ++i) {
        Node *d = prog->children[i];
        if (d && d->kind == N_FN &&
            d->name_len == 4 && memcmp(d->name, "main", 4) == 0) return 1;
    }
    return 0;
}

static void emit_test_fn(E *e, int id, Node *t) {
    /* t->name is the raw description string literal with quotes. */
    e->cur_test_desc_start = t->name;
    e->cur_test_desc_len   = t->name_len;

    fprintf(e->out, "static void _kai_test_%d(void) {\n", id);
    fprintf(e->out, "    kai_test_begin(%.*s);\n",
            (int) t->name_len, t->name);
    fputs("    KaiValue *_body = ", e->out);
    ls_push_mark(e);
    if (t->n_children >= 1) emit_expr(e, t->children[0]);
    else                    fputs("kai_unit()", e->out);
    ls_pop_mark(e);
    fputs(";\n", e->out);
    fputs("    kai_decref(_body);\n", e->out);
    fputs("    kai_test_pass();\n", e->out);
    fputs("}\n\n", e->out);

    e->cur_test_desc_start = NULL;
    e->cur_test_desc_len   = 0;
}

/* ---------- top-level passes ---------- */

static void register_top_level_fns(E *e, Node *prog) {
    for (size_t i = 0; i < prog->n_children; ++i) {
        Node *d = prog->children[i];
        if (d && d->kind == N_FN) {
            int arity = 0;
            for (size_t j = 2; j < d->n_children; ++j) {
                Node *p = d->children[j];
                if (p && p->kind == N_PARAM) arity++;
            }
            reg_entry(&e->fns, &e->n_fns, &e->cap_fns, d->name, d->name_len, arity, 0);
        }
    }
}

static void register_builtin_variants(E *e) {
    /* Tag assignment from docs/variant-tags.md. Some/None/Ok/Err are
     * the four constructors stage-0-emitted code directly builds;
     * SigInt..Signaled live in tags 4..10 because user code may
     * pattern-match against them and the matcher must look up the
     * reserved tag (the runtime stamps the same literal). */
    static const struct { const char *n; int a; int t; } B[] = {
        { "Some",     1, 0 },
        { "None",     0, 1 },
        { "Ok",       1, 2 },
        { "Err",      1, 3 },
        { "SigInt",   0, 4 },
        { "SigTerm",  0, 5 },
        { "SigHup",   0, 6 },
        { "SigUsr1",  0, 7 },
        { "SigUsr2",  0, 8 },
        { "Exited",   1, 9 },
        { "Signaled", 1, 10 },
    };
    for (size_t i = 0; i < sizeof(B) / sizeof(B[0]); ++i) {
        reg_entry(&e->variants, &e->n_variants, &e->cap_variants,
                  B[i].n, strlen(B[i].n), B[i].a, B[i].t);
    }
}

static void register_user_variants(E *e, Node *prog) {
    int next_tag = KAI_USER_VARIANT_TAG_BASE;
    for (size_t i = 0; i < prog->n_children; ++i) {
        Node *d = prog->children[i];
        if (!d || d->kind != N_TYPE_DECL) continue;
        if (d->n_children < 1) continue;
        Node *body = d->children[0];
        if (!body || body->kind != N_TY_SUM) continue;
        for (size_t j = 0; j < body->n_children; ++j) {
            Node *v = body->children[j];
            if (v && v->kind == N_VARIANT) {
                reg_entry(&e->variants, &e->n_variants, &e->cap_variants,
                          v->name, v->name_len, (int) v->n_children, next_tag);
                next_tag++;
            }
        }
    }
}

static void collect_all_lambdas(E *e, Node *prog) {
    for (size_t i = 0; i < prog->n_children; ++i) {
        Node *d = prog->children[i];
        if (!d) continue;
        if (d->kind == N_FN) collect_lambdas(e, d->children[1]);
        else if (d->kind == N_TEST && d->n_children >= 1)
            collect_lambdas(e, d->children[0]);
    }
}

int kai_emit(Node *program, FILE *out, int test_mode) {
    E e;
    memset(&e, 0, sizeof(e));
    e.out = out;
    e.test_mode = test_mode;

    register_top_level_fns(&e, program);
    register_builtin_variants(&e);
    register_user_variants(&e, program);
    collect_all_lambdas(&e, program);

    fprintf(out, "/* generated by kaikai-minimal stage 0 */\n");
    fprintf(out, "#include \"runtime.h\"\n\n");

    /* Forward declarations. */
    for (size_t i = 0; i < program->n_children; ++i) {
        Node *d = program->children[i];
        if (d && d->kind == N_FN) {
            emit_fn_signature(&e, d);
            fputs(";\n", out);
        }
    }
    fputc('\n', out);

    for (size_t i = 0; i < program->n_children; ++i) {
        Node *d = program->children[i];
        if (d && d->kind == N_FN) {
            fprintf(out, "static KaiValue *_kai_%.*s_thunk(KaiValue *, KaiValue **, int);\n",
                    (int) d->name_len, d->name);
        }
    }
    fputc('\n', out);

    /* Lambda helper forward declarations. */
    for (size_t i = 0; i < e.n_lams; ++i) {
        fprintf(out, "static KaiValue *_kai_lam_%d(KaiValue *, KaiValue **, int);\n",
                e.lams[i].id);
    }
    fputc('\n', out);

    /* Function bodies and their thunks. */
    for (size_t i = 0; i < program->n_children; ++i) {
        Node *d = program->children[i];
        if (!d) continue;
        if (d->kind == N_FN) {
            emit_fn_body(&e, d);
            emit_fn_thunk(&e, d);
        }
    }

    /* Lambda helper bodies. */
    for (size_t i = 0; i < e.n_lams; ++i) {
        emit_lambda_helper_def(&e, &e.lams[i]);
    }

    /* Test functions (always emitted; only wired into main in test_mode). */
    int n_tests = 0;
    for (size_t i = 0; i < program->n_children; ++i) {
        Node *d = program->children[i];
        if (d && d->kind == N_TEST) {
            emit_test_fn(&e, n_tests, d);
            n_tests++;
        }
    }

    if (test_mode) {
        fputs("int main(int argc, char **argv) {\n"
              "    kai_set_args(argc, argv);\n", out);
        for (int i = 0; i < n_tests; ++i) {
            fprintf(out, "    _kai_test_%d();\n", i);
        }
        fputs("    return kai_test_summary();\n"
              "}\n", out);
    } else if (has_main(program)) {
        fputs("int main(int argc, char **argv) {\n"
              "    kai_set_args(argc, argv);\n"
              "    KaiValue *_result = kai_main();\n"
              "    kai_decref(_result);\n"
              "    return 0;\n"
              "}\n", out);
    }

    /* Free tables. */
    free(e.fns);
    free(e.variants);
    for (size_t i = 0; i < e.n_lams; ++i) free(e.lams[i].caps);
    free(e.lams);
    free(e.locals.names);
    free(e.locals.lens);
    free(e.locals.decls);
    free(e.locals.marks);
    free(e.uses.items);
    return e.had_error ? 1 : 0;
}
