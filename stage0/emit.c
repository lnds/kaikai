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

typedef struct { const char *name; size_t len; int arity; } SymEntry;

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
    size_t  n, cap;
    size_t *marks;
    size_t  n_marks, cap_marks;
} LocalScope;

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

    LocalScope locals;
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

static void ls_add(E *e, const char *name, size_t len) {
    LocalScope *s = &e->locals;
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->names = (char **) realloc(s->names, s->cap * sizeof(char *));
        s->lens  = (size_t *) realloc(s->lens,  s->cap * sizeof(size_t));
    }
    s->names[s->n] = (char *) name;
    s->lens[s->n]  = len;
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

/* Walk a pattern, adding every PAT_BIND name to the local scope. Used
   by both collect_free_vars and emit_pat_binds so pattern bindings
   are never counted as captures and are never redirected to a
   same-named global at emit time. */
static void pat_add_locals(E *e, Node *pat) {
    if (!pat) return;
    switch (pat->kind) {
        case N_PAT_WILD:
        case N_PAT_LIT:
            return;
        case N_PAT_BIND:
            ls_add(e, pat->name, pat->name_len);
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

/* ---------- forwards ---------- */

static void emit_expr(E *e, Node *n);
static void emit_stmt(E *e, Node *n);
static void emit_pat_test(E *e, Node *pat, const char *scr);
/* `is_alias` = the scrutinee shares storage with its producer (variant args,
 * cons head/tail). PBind/PAs emit `kai_incref(scr)` so the binding has its
 * own owned reference, decoupled from the producer's slot. When false, the
 * scrutinee is already owned (top-level _scr / _letv from a transferred RHS,
 * or kai_field which already increfs its return) and the binding takes the
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
    { "panic",          "kai_prelude_panic",          1 },
    { "exit",           "kai_prelude_exit",           1 },
    { "int_to_string",  "kai_prelude_int_to_string",  1 },
    { "real_to_string", "kai_prelude_real_to_string", 1 },
    { "string_length",  "kai_prelude_string_length",  1 },
    { "string_concat",  "kai_prelude_string_concat",  2 },
    { "string_concat_all", "kai_prelude_string_concat_all", 1 },
    { "string_join",    "kai_prelude_string_join",    2 },
    { "list_length",    "kai_prelude_list_length",    1 },
    { "list_append",    "kai_prelude_list_append",    2 },
    { "list_reverse",   "kai_prelude_list_reverse",   1 },
    { "map",            "kai_prelude_map",            2 },
    { "filter",         "kai_prelude_filter",         2 },
    { "reduce",         "kai_prelude_reduce",         3 },
    { "each",           "kai_prelude_each",           2 },
    { "args",           "kai_prelude_args",           0 },
    { "read_file",      "kai_prelude_read_file",      1 },
    { "write_file",     "kai_prelude_write_file",     2 },
    { "read_line",      "kai_prelude_read_line",      0 },
    { "string_to_int",  "kai_prelude_string_to_int",  1 },
    { "string_to_real", "kai_prelude_string_to_real", 1 },
    { "char_at",        "kai_prelude_char_at",        2 },
    { "string_split",   "kai_prelude_string_split",   2 },
    { "string_contains","kai_prelude_string_contains",2 },
    { "string_slice",   "kai_prelude_string_slice",   3 },
    { "char_to_int",    "kai_prelude_char_to_int",    1 },
    { "int_to_char",    "kai_prelude_int_to_char",    1 },
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
                      const char *name, size_t len, int arity) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *arr = (SymEntry *) realloc(*arr, *cap * sizeof(SymEntry));
    }
    (*arr)[*n].name = name;
    (*arr)[*n].len  = len;
    (*arr)[*n].arity = arity;
    (*n)++;
}

static int find_entry(SymEntry *arr, size_t n,
                      const char *name, size_t len, int *out_arity) {
    for (size_t i = 0; i < n; ++i) {
        if (arr[i].len == len && memcmp(arr[i].name, name, len) == 0) {
            if (out_arity) *out_arity = arr[i].arity;
            return 1;
        }
    }
    return 0;
}

static int find_user_fn(E *e, const char *name, size_t len, int *out_arity) {
    return find_entry(e->fns, e->n_fns, name, len, out_arity);
}
static int find_variant(E *e, const char *name, size_t len, int *out_arity) {
    return find_entry(e->variants, e->n_variants, name, len, out_arity);
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
        /* m5.x-flip Phase 3 — Step D eager-dup retrofit. Stage 0 has no
         * perceus pass; under the linear-consumption runtime every read
         * of a local that flows into a consuming primitive would free the
         * binding mid-flight. Wrap every local read in kai_internal_dup
         * so the binding's reference is preserved (the caller of the dup
         * sees its own +1 ref; the binding's original ref stays alive).
         * This brute-force eager-dup leaks one ref per binding (no exit
         * drops in stage 0) but never UAFs. The leak baseline matches
         * pre-flip behaviour where everything also leaked. */
        fprintf(e->out, "kai_internal_dup(kai_%.*s)", (int) len, name);
        return;
    }
    if (find_variant(e, name, len, &arity)) {
        fprintf(e->out, "kai_variant(0, \"%.*s\", 0, NULL)", (int) len, name);
        return;
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
           find_variant(e, name, len, &a);
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
            size_t expr_start = i;
            int depth = 1;
            while (i < end && depth > 0) {
                if      (src[i] == '{') depth++;
                else if (src[i] == '}') { depth--; if (depth == 0) break; }
                ++i;
            }
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

static void collect_free_vars(E *e, Node *n, Node *lam, LamInfo *info) {
    if (!n) return;
    if (n->kind == N_LAMBDA) return;         /* inner lambdas have their own */
    if (n->kind == N_IDENT) {
        if (!is_lambda_param(lam, n->name, n->name_len) &&
            !is_global_name(e, n->name, n->name_len) &&
            !ls_has(e, n->name, n->name_len)) {
            add_capture(info, n->name, n->name_len);
        }
        return;
    }
    if (n->kind == N_STRING) {
        collect_free_vars_in_string(e, n, lam, info);
        return;
    }
    if (n->kind == N_FIELD) {
        if (n->n_children >= 1) collect_free_vars(e, n->children[0], lam, info);
        return;
    }
    if (n->kind == N_RECORD_LIT) {
        for (size_t i = 0; i < n->n_children; ++i)
            collect_free_vars(e, n->children[i], lam, info);
        return;
    }
    /* N_LET adds pattern binds into scope *after* its RHS, so the RHS
       doesn't see the new name (correct: `let x = expr` where expr
       cannot refer to x). */
    if (n->kind == N_LET) {
        if (n->n_children >= 3) collect_free_vars(e, n->children[2], lam, info);
        if (n->n_children >= 1) pat_add_locals(e, n->children[0]);
        return;
    }
    /* Each match arm is its own scope: walk the pattern for binds,
       push, walk guard + body, pop. */
    if (n->kind == N_ARM) {
        Node *pat = n->children[0];
        int has_guard = (n->v.flags & 0x1) != 0;
        Node *body_idx = n->children[has_guard ? 2 : 1];
        Node *guard    = has_guard ? n->children[1] : NULL;
        ls_push_mark(e);
        pat_add_locals(e, pat);
        if (guard)    collect_free_vars(e, guard,    lam, info);
        if (body_idx) collect_free_vars(e, body_idx, lam, info);
        ls_pop_mark(e);
        return;
    }
    /* Blocks get their own scope so a nested `let` doesn't leak. */
    if (n->kind == N_BLOCK) {
        ls_push_mark(e);
        for (size_t i = 0; i < n->n_children; ++i)
            collect_free_vars(e, n->children[i], lam, info);
        ls_pop_mark(e);
        return;
    }
    for (size_t i = 0; i < n->n_children; ++i)
        collect_free_vars(e, n->children[i], lam, info);
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
        case TK_PLUS:        return "kai_add";
        case TK_MINUS:       return "kai_sub";
        case TK_STAR:        return "kai_mul";
        case TK_SLASH:       return "kai_div";
        case TK_SLASH_SLASH: return "kai_idiv";
        case TK_PERCENT:     return "kai_mod";
        case TK_LT:          return "kai_lt";
        case TK_GT:          return "kai_gt";
        case TK_LE:          return "kai_le";
        case TK_GE:          return "kai_ge";
        case TK_EQEQ:        return "kai_eq_v";
        case TK_NEQ:         return "kai_ne_v";
        default:             return NULL;
    }
}

static void emit_binop(E *e, Node *n) {
    int op = n->v.op;
    Node *l = n->children[0], *r = n->children[1];
    if (op == TK_AND) {
        fputs("({ KaiValue *_a = ", e->out); emit_expr(e, l);
        fputs("; kai_truthy(_a) ? ", e->out); emit_expr(e, r);
        fputs(" : kai_bool(0); })", e->out);
        return;
    }
    if (op == TK_OR) {
        fputs("({ KaiValue *_a = ", e->out); emit_expr(e, l);
        fputs("; kai_truthy(_a) ? kai_bool(1) : ", e->out); emit_expr(e, r);
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
    const char *fn = (op == TK_MINUS) ? "kai_neg"
                    : (op == TK_NOT)   ? "kai_boolnot"
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
    fputs("(kai_truthy(", e->out); emit_expr(e, cond); fputs(") ? ", e->out);
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
    /* Emits kai_variant(0, "<Name>", n_args, <args array>). */
    size_t n_args = call_or_null ? (call_or_null->n_children - 1) : 0;
    fprintf(e->out, "kai_variant(0, \"%.*s\", %d, ", (int) name_len, name, (int) n_args);
    if (n_args == 0) {
        fputs("NULL)", e->out);
    } else {
        fputs("(KaiValue *[]){", e->out);
        for (size_t i = 1; i < call_or_null->n_children; ++i) {
            if (i > 1) fputs(", ", e->out);
            emit_expr(e, call_or_null->children[i]);
        }
        fputs("})", e->out);
    }
}

static void emit_call(E *e, Node *n) {
    Node *callee = n->children[0];
    if (callee && callee->kind == N_IDENT) {
        int arity = 0;
        if (find_variant(e, callee->name, callee->name_len, &arity)) {
            emit_variant_construction(e, callee->name, callee->name_len, n);
            return;
        }
        int handled = emit_ident_callee(e, callee->name, callee->name_len);
        if (handled) {
            emit_args_with_prepend(e, n, NULL);
            return;
        }
        /* Variable holding a closure. */
        fputs("kai_apply(", e->out);
        fprintf(e->out, "kai_%.*s, %d, (KaiValue *[]){",
                (int) callee->name_len, callee->name,
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

static void emit_pipe(E *e, Node *n) {
    Node *lhs = n->children[0];
    Node *rhs = n->children[1];
    if (rhs && rhs->kind == N_CALL) {
        Node *callee = rhs->children[0];
        if (callee && callee->kind == N_IDENT) {
            int arity = 0;
            if (find_variant(e, callee->name, callee->name_len, &arity)) {
                /* Variant constructor with piped first arg. */
                fprintf(e->out, "kai_variant(0, \"%.*s\", %d, (KaiValue *[]){",
                        (int) callee->name_len, callee->name,
                        (int) rhs->n_children);
                emit_expr(e, lhs);
                for (size_t i = 1; i < rhs->n_children; ++i) {
                    fputs(", ", e->out);
                    emit_expr(e, rhs->children[i]);
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
    fputs("kai_field(", e->out);
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
            fprintf(e->out, "kai_eq(%s, ", scr);
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
            fprintf(e->out, "(%s && %s->tag == KAI_VARIANT && strcmp(%s->as.var.variant_name, \"%.*s\") == 0",
                    scr, scr, scr, (int) pat->name_len, pat->name);
            for (size_t i = 0; i < pat->n_children; ++i) {
                fputs(" && ", e->out);
                char sub[512];
                snprintf(sub, sizeof(sub), "%s->as.var.args[%zu]", scr, i);
                emit_pat_test(e, pat->children[i], sub);
            }
            fputs(")", e->out);
            return;
        }

        case N_PAT_RECORD: {
            fprintf(e->out, "(%s && %s->tag == KAI_RECORD", scr, scr);
            for (size_t i = 0; i < pat->n_children; ++i) {
                Node *pf = pat->children[i];
                char tmp[512];
                snprintf(tmp, sizeof(tmp),
                         "kai_field(%s, \"%.*s\")", scr,
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
                snprintf(sub, sizeof(sub), "%s->as.var.args[%zu]", scr, i);
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
                         "kai_field(%s, \"%.*s\")", scr,
                         (int) pf->name_len, pf->name);
                /* kai_field already incrs its return; child is owned. */
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
            fputs("if (kai_truthy(", e->out);
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
    fputs("} while (0); _r; })", e->out);
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
            size_t expr_start = i;
            int depth = 1;
            while (i < end && depth > 0) {
                if      (src[i] == '{') depth++;
                else if (src[i] == '}') { depth--; if (depth == 0) break; }
                ++i;
            }
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
    for (int i = 0; i < n_params; ++i) {
        Node *p = lam->children[1 + i];
        fprintf(e->out, "    KaiValue *kai_%.*s = args[%d];\n",
                (int) p->name_len, p->name, i);
        ls_add(e, p->name, p->name_len);
    }
    for (int i = 0; i < info->n_caps; ++i) {
        fprintf(e->out, "    KaiValue *kai_%.*s = self->as.clo.captures[%d];\n",
                (int) info->caps[i].len, info->caps[i].name, i);
        ls_add(e, info->caps[i].name, info->caps[i].len);
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
           not see the new name. */
        ls_add(e, pat->name, pat->name_len);
        return;
    }
    fputs("KaiValue *_letv = ", e->out);
    emit_expr(e, val);
    fputs("; ", e->out);
    /* _letv holds the RHS's owned ref; not aliased from any other producer. */
    emit_pat_binds(e, pat, "_letv", 0);
    pat_add_locals(e, pat);
}

static void emit_stmt(E *e, Node *n) {
    if (!n) return;
    switch (n->kind) {
        case N_LET:       emit_let(e, n); return;
        case N_ASSERT: {
            const char *msg_expr = NULL;
            if (n->v.flags & 0x1) {
                /* assert cond, msg — msg is an expression; stringify and
                   pass to kai_test_fail / kai_prelude_panic. */
                msg_expr = "";
            }
            if (e->test_mode) {
                fputs("{ KaiValue *_c = ", e->out);
                emit_expr(e, n->children[0]);
                if (n->v.flags & 0x1) {
                    fputs("; if (!kai_truthy(_c)) { KaiValue *_m = ", e->out);
                    emit_expr(e, n->children[1]);
                    fputs("; kai_test_fail(", e->out);
                    if (e->cur_test_desc_start) {
                        fprintf(e->out, "%.*s", (int) e->cur_test_desc_len, e->cur_test_desc_start);
                    } else {
                        fputs("\"\"", e->out);
                    }
                    fputs(", (_m && _m->tag == KAI_STR) ? _m->as.s.bytes : \"assertion failed\"); return; } }", e->out);
                } else {
                    fputs("; if (!kai_truthy(_c)) { kai_test_fail(", e->out);
                    if (e->cur_test_desc_start) {
                        fprintf(e->out, "%.*s", (int) e->cur_test_desc_len, e->cur_test_desc_start);
                    } else {
                        fputs("\"\"", e->out);
                    }
                    fputs(", \"assertion failed\"); return; } }", e->out);
                }
            } else {
                fputs("{ KaiValue *_c = ", e->out);
                emit_expr(e, n->children[0]);
                fputs("; if (!kai_truthy(_c)) { kai_prelude_panic(kai_str(\"assertion failed\")); } }", e->out);
            }
            (void) msg_expr;
            return;
        }
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
    fputs(" {\n    return ", e->out);
    ls_push_mark(e);
    for (size_t i = 2; i < fn->n_children; ++i) {
        Node *p = fn->children[i];
        if (p && p->kind == N_PARAM) ls_add(e, p->name, p->name_len);
    }
    emit_expr(e, fn->children[1]);
    ls_pop_mark(e);
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
            reg_entry(&e->fns, &e->n_fns, &e->cap_fns, d->name, d->name_len, arity);
        }
    }
}

static void register_builtin_variants(E *e) {
    static const struct { const char *n; int a; } B[] = {
        { "Some", 1 }, { "None", 0 }, { "Ok", 1 }, { "Err", 1 }
    };
    for (size_t i = 0; i < sizeof(B) / sizeof(B[0]); ++i) {
        reg_entry(&e->variants, &e->n_variants, &e->cap_variants,
                  B[i].n, strlen(B[i].n), B[i].a);
    }
}

static void register_user_variants(E *e, Node *prog) {
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
                          v->name, v->name_len, (int) v->n_children);
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
    return e.had_error ? 1 : 0;
}
