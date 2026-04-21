/*
 * C emitter for kaikai-minimal.
 *
 * Milestone 7 scope: every expression form that fizzbuzz.kai needs —
 * if/else chains, binary and unary operators, let bindings, pipes,
 * range and list literals, and first-class references to user functions
 * and prelude functions (map, filter, each, reduce). String
 * interpolation (#{...}) still errors out; that is milestone 8.
 */

#include "emit.h"
#include "ast.h"
#include "lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- emitter state ---------- */

typedef struct {
    const char *name;
    size_t      len;
    int         arity;
} FnEntry;

typedef struct {
    FILE *out;
    int   had_error;

    FnEntry *fns;
    size_t   n_fns;
    size_t   cap_fns;
} E;

/* ---------- forwards ---------- */

static void emit_expr(E *e, Node *n);
static void emit_stmt(E *e, Node *n);

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
    const char *c;            /* C symbol for direct call */
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
    { "list_length",    "kai_prelude_list_length",    1 },
    { "list_append",    "kai_prelude_list_append",    2 },
    { "list_reverse",   "kai_prelude_list_reverse",   1 },
    { "map",            "kai_prelude_map",            2 },
    { "filter",         "kai_prelude_filter",         2 },
    { "reduce",         "kai_prelude_reduce",         3 },
    { "each",           "kai_prelude_each",           2 }
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

/* ---------- user fn table ---------- */

static void register_fn(E *e, const char *name, size_t len, int arity) {
    if (e->n_fns == e->cap_fns) {
        e->cap_fns = e->cap_fns ? e->cap_fns * 2 : 16;
        e->fns = (FnEntry *) realloc(e->fns, e->cap_fns * sizeof(FnEntry));
    }
    e->fns[e->n_fns].name = name;
    e->fns[e->n_fns].len = len;
    e->fns[e->n_fns].arity = arity;
    e->n_fns++;
}

static int find_user_fn(E *e, const char *name, size_t len, int *out_arity) {
    for (size_t i = 0; i < e->n_fns; ++i) {
        if (e->fns[i].len == len && memcmp(e->fns[i].name, name, len) == 0) {
            if (out_arity) *out_arity = e->fns[i].arity;
            return 1;
        }
    }
    return 0;
}

/* ---------- identifier emission modes ---------- */

/* As a direct callee (in a call position). Writes the C symbol; the caller
   then writes arguments in parens. Returns 1 if this was a known callable
   (prelude or user fn) — in which case a direct call is appropriate — or
   0 otherwise. */
static int emit_ident_callee(E *e, const char *name, size_t len) {
    const char *mapped = NULL;
    int arity = 0;
    if (find_prelude(name, len, &arity, &mapped)) {
        fputs(mapped, e->out);
        return 1;
    }
    if (find_user_fn(e, name, len, &arity)) {
        fprintf(e->out, "kai_%.*s", (int) len, name);
        return 1;
    }
    /* Fall back: treat as a value that should be a closure (use kai_apply). */
    fprintf(e->out, "kai_%.*s", (int) len, name);
    return 0;
}

/* As a value (expression position). Functions are wrapped in closures;
   variables are emitted as-is. */
static void emit_ident_value(E *e, const char *name, size_t len) {
    const char *mapped = NULL;
    int arity = 0;
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

/* ---------- string literal ---------- */

static void emit_string_inner(E *e, const Node *s) {
    const char *src = s->name;
    size_t len = s->name_len;
    int triple = (s->v.flags & 0x1) != 0;
    size_t start = triple ? 3 : 1;
    size_t end   = triple ? len - 3 : len - 1;
    fputc('"', e->out);
    for (size_t i = start; i < end; ++i) {
        unsigned char c = (unsigned char) src[i];
        if (c == '#' && i + 1 < end && src[i + 1] == '{') {
            bug(e, (Node *) s, "string interpolation (#{...}) — arrives in M8");
            break;
        }
        fputc(c, e->out);
    }
    fputc('"', e->out);
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

/* ---------- if / call / pipe / range / list ---------- */

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

static void emit_args(E *e, Node *call_or_null, Node *prepended) {
    int first = 1;
    fputc('(', e->out);
    if (prepended) { emit_expr(e, prepended); first = 0; }
    if (call_or_null) {
        for (size_t i = 1; i < call_or_null->n_children; ++i) {
            if (!first) fputs(", ", e->out);
            first = 0;
            emit_expr(e, call_or_null->children[i]);
        }
    }
    fputc(')', e->out);
}

static void emit_call(E *e, Node *n) {
    Node *callee = n->children[0];
    if (callee && callee->kind == N_IDENT) {
        int handled = emit_ident_callee(e, callee->name, callee->name_len);
        if (handled) {
            emit_args(e, n, NULL);
        } else {
            /* Fallback via kai_apply on a closure stored in a variable. */
            fprintf(e->out, "kai_apply(%.*s, %d, (KaiValue *[]){",
                    (int) callee->name_len + 4, "kai_", /* prefix */
                    (int) (n->n_children - 1));
            for (size_t i = 1; i < n->n_children; ++i) {
                if (i > 1) fputs(", ", e->out);
                emit_expr(e, n->children[i]);
            }
            fputs("})", e->out);
        }
        return;
    }
    /* Non-ident callee: treat as value that should be a closure. */
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
            int handled = emit_ident_callee(e, callee->name, callee->name_len);
            if (handled) {
                emit_args(e, rhs, lhs);
                return;
            }
        }
        /* Fallback: apply rhs's callee as a closure with lhs prepended. */
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
        /* Apply closure stored in variable. */
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
        emit_expr(e, n->children[0]);
        fputs(", ", e->out);
        emit_expr(e, n->children[1]);
        fputs(", ", e->out);
        emit_expr(e, n->children[2]);
        fputc(')', e->out);
    } else {
        fputs("kai_range(", e->out);
        emit_expr(e, n->children[0]);
        fputs(", ", e->out);
        emit_expr(e, n->children[1]);
        fputc(')', e->out);
    }
}

static void emit_list_tail(E *e, Node *lit, size_t i) {
    if (i >= lit->n_children) { fputs("kai_nil()", e->out); return; }
    Node *elt = lit->children[i];
    if (elt && elt->kind == N_SPREAD) {
        fputs("kai_prelude_list_append(", e->out);
        emit_expr(e, elt->children[0]);
        fputs(", ", e->out);
        emit_list_tail(e, lit, i + 1);
        fputc(')', e->out);
    } else {
        fputs("kai_cons(", e->out);
        emit_expr(e, elt);
        fputs(", ", e->out);
        emit_list_tail(e, lit, i + 1);
        fputc(')', e->out);
    }
}

static void emit_list(E *e, Node *n) {
    emit_list_tail(e, n, 0);
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
        case N_STRING:
            fputs("kai_str(", e->out);
            emit_string_inner(e, n);
            fputc(')', e->out);
            return;

        case N_IDENT:       emit_ident_value(e, n->name, n->name_len); return;
        case N_CALL:        emit_call(e, n);     return;
        case N_PIPE:        emit_pipe(e, n);     return;
        case N_BINOP:       emit_binop(e, n);    return;
        case N_UNOP:        emit_unop(e, n);     return;
        case N_IF:          emit_if(e, n);       return;
        case N_RANGE_LIT:   emit_range(e, n);    return;
        case N_LIST_LIT:    emit_list(e, n);     return;

        case N_BLOCK: {
            size_t n_stmts = (n->n_children > 0) ? n->n_children - 1 : 0;
            Node *value = (n->n_children > 0) ? n->children[n->n_children - 1] : NULL;
            int has_value = (n->v.flags & 0x1) != 0;
            if (n_stmts == 0 && has_value) { emit_expr(e, value); return; }
            fputs("({ ", e->out);
            for (size_t i = 0; i < n_stmts; ++i) { emit_stmt(e, n->children[i]); fputc(' ', e->out); }
            if (has_value) { emit_expr(e, value); fputs("; ", e->out); }
            else            { fputs("kai_unit(); ", e->out); }
            fputs("})", e->out);
            return;
        }

        case N_FIELD:
        case N_INDEX:
        case N_MATCH:
        case N_LAMBDA:
        case N_RECORD_LIT:
        case N_SPREAD:
        case N_PLACEHOLDER:
            bug(e, n, nk_name(n->kind));
            fputs("kai_unit()", e->out);
            return;

        default:
            bug(e, n, nk_name(n->kind));
            fputs("kai_unit()", e->out);
            return;
    }
}

/* ---------- statements ---------- */

static void emit_stmt(E *e, Node *n) {
    if (!n) return;
    switch (n->kind) {
        case N_LET: {
            Node *pat = n->children[0];
            Node *val = n->children[2];
            if (pat->kind != N_PAT_BIND) {
                bug(e, n, "let with destructuring pattern (arrives in M8)");
                return;
            }
            fprintf(e->out, "KaiValue *kai_%.*s = ", (int) pat->name_len, pat->name);
            emit_expr(e, val);
            fputs(";", e->out);
            return;
        }
        case N_ASSERT: {
            fputs("{ KaiValue *_c = ", e->out);
            emit_expr(e, n->children[0]);
            fputs("; if (!kai_truthy(_c)) { kai_prelude_panic(kai_str(\"assertion failed\")); } }", e->out);
            return;
        }
        case N_EXPR_STMT: {
            fputs("{ KaiValue *_ = ", e->out);
            emit_expr(e, n->children[0]);
            fputs("; (void) _; }", e->out);
            return;
        }
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
    emit_expr(e, fn->children[1]);
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
            d->name_len == 4 && memcmp(d->name, "main", 4) == 0) {
            return 1;
        }
    }
    return 0;
}

int kai_emit(Node *program, FILE *out) {
    E e;
    memset(&e, 0, sizeof(e));
    e.out = out;

    /* Pre-register all top-level fns so forward references resolve. */
    for (size_t i = 0; i < program->n_children; ++i) {
        Node *d = program->children[i];
        if (d && d->kind == N_FN) {
            int arity = 0;
            for (size_t j = 2; j < d->n_children; ++j) {
                Node *p = d->children[j];
                if (p && p->kind == N_PARAM) arity++;
            }
            register_fn(&e, d->name, d->name_len, arity);
        }
    }

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

    /* Thunk forward declarations (for first-class fn refs). */
    for (size_t i = 0; i < program->n_children; ++i) {
        Node *d = program->children[i];
        if (d && d->kind == N_FN) {
            fprintf(out, "static KaiValue *_kai_%.*s_thunk(KaiValue *, KaiValue **, int);\n",
                    (int) d->name_len, d->name);
        }
    }
    fputc('\n', out);

    /* Function bodies and thunks. */
    for (size_t i = 0; i < program->n_children; ++i) {
        Node *d = program->children[i];
        if (!d) continue;
        if (d->kind == N_FN) {
            emit_fn_body(&e, d);
            emit_fn_thunk(&e, d);
        }
    }

    if (has_main(program)) {
        fputs("int main(void) {\n"
              "    KaiValue *_result = kai_main();\n"
              "    kai_decref(_result);\n"
              "    return 0;\n"
              "}\n", out);
    }

    free(e.fns);
    return e.had_error ? 1 : 0;
}
