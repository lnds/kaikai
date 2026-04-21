/*
 * C emitter for kaikai-minimal.
 *
 * Milestone 6 scope: just enough to produce a compilable C file for
 * programs of the shape `fn main() { print("...") }`. Later milestones
 * extend this to expressions, matches, lists, and closures. The file is
 * written with that expansion in mind so growth is additive.
 */

#include "emit.h"
#include "ast.h"
#include "lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    FILE *out;
    int   had_error;
} E;

/* ---------- helpers ---------- */

static void emit_expr(E *e, Node *n);
static void emit_stmt(E *e, Node *n);

static void bug(E *e, Node *n, const char *what) {
    e->had_error = 1;
    int line = n ? n->line : 0;
    int col  = n ? n->col  : 0;
    fprintf(stderr, "emit: %d:%d: unsupported construct: %s\n",
            line, col, what);
}

/*
 * Prelude name mapping. Identifiers that appear in the program and
 * resolve to the minimal prelude become their `kai_prelude_*` runtime
 * counterparts. Everything else gets a `kai_` prefix.
 */
static const struct { const char *k; const char *v; } PRELUDE_MAP[] = {
    { "print",          "kai_prelude_print"          },
    { "eprint",         "kai_prelude_eprint"         },
    { "panic",          "kai_prelude_panic"          },
    { "exit",           "kai_prelude_exit"           },
    { "int_to_string",  "kai_prelude_int_to_string"  },
    { "real_to_string", "kai_prelude_real_to_string" },
    { "string_length",  "kai_prelude_string_length"  },
    { "string_concat",  "kai_prelude_string_concat"  },
    { "list_length",    "kai_prelude_list_length"    },
    { "list_append",    "kai_prelude_list_append"    },
    { "list_reverse",   "kai_prelude_list_reverse"   },
    { "map",            "kai_prelude_map"            },
    { "filter",         "kai_prelude_filter"         },
    { "reduce",         "kai_prelude_reduce"         },
    { "each",           "kai_prelude_each"           },
};
static const size_t N_PRELUDE = sizeof(PRELUDE_MAP) / sizeof(PRELUDE_MAP[0]);

static const char *map_prelude(const char *name, size_t len) {
    for (size_t i = 0; i < N_PRELUDE; ++i) {
        size_t kl = strlen(PRELUDE_MAP[i].k);
        if (kl == len && memcmp(PRELUDE_MAP[i].k, name, len) == 0) {
            return PRELUDE_MAP[i].v;
        }
    }
    return NULL;
}

static void emit_ident_ref(E *e, const char *name, size_t len) {
    const char *mapped = map_prelude(name, len);
    if (mapped) { fputs(mapped, e->out); return; }
    fprintf(e->out, "kai_%.*s", (int) len, name);
}

/* Emit the text between quotes of a kaikai string literal as a C string
   literal. For M6 we don't handle #{...} interpolation — that arrives in
   M7. Escapes like \n, \t pass through because C accepts them too. */
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
            bug((E *) e, (Node *) s,
                "string interpolation (#{...}) not yet supported in M6");
            break;
        }
        fputc(c, e->out);
    }
    fputc('"', e->out);
}

/* ---------- expressions ---------- */

static void emit_expr(E *e, Node *n) {
    if (!n) { fputs("kai_unit()", e->out); return; }

    switch (n->kind) {
        case N_UNIT: fputs("kai_unit()", e->out); return;

        case N_BOOL:
            fprintf(e->out, "kai_bool(%d)", n->v.b ? 1 : 0);
            return;

        case N_INT:
            fprintf(e->out, "kai_int(%lldLL)", (long long) n->v.i);
            return;

        case N_REAL:
            fprintf(e->out, "kai_real(%.17g)", n->v.r);
            return;

        case N_CHAR:
            fprintf(e->out, "kai_char(0x%08X)", (unsigned) n->v.c);
            return;

        case N_STRING:
            fputs("kai_str(", e->out);
            emit_string_inner(e, n);
            fputc(')', e->out);
            return;

        case N_IDENT:
            emit_ident_ref(e, n->name, n->name_len);
            return;

        case N_CALL: {
            Node *callee = n->children[0];
            if (callee && callee->kind == N_IDENT) {
                emit_ident_ref(e, callee->name, callee->name_len);
                fputc('(', e->out);
                for (size_t i = 1; i < n->n_children; ++i) {
                    if (i > 1) fputs(", ", e->out);
                    emit_expr(e, n->children[i]);
                }
                fputc(')', e->out);
            } else {
                bug(e, n, "call with non-ident callee");
            }
            return;
        }

        case N_BLOCK: {
            size_t n_stmts = (n->n_children > 0) ? n->n_children - 1 : 0;
            Node *value = (n->n_children > 0) ? n->children[n->n_children - 1] : NULL;
            int has_value = (n->v.flags & 0x1) != 0;
            if (n_stmts == 0 && has_value) {
                emit_expr(e, value);
                return;
            }
            /* Statement expression (GCC/clang). */
            fputs("({ ", e->out);
            for (size_t i = 0; i < n_stmts; ++i) {
                emit_stmt(e, n->children[i]);
                fputc(' ', e->out);
            }
            if (has_value) {
                emit_expr(e, value);
                fputs("; ", e->out);
            } else {
                fputs("kai_unit(); ", e->out);
            }
            fputs("})", e->out);
            return;
        }

        /* ---- constructs not yet implemented (will be filled in M7-M8) ---- */
        case N_BINOP:
        case N_UNOP:
        case N_IF:
        case N_MATCH:
        case N_LAMBDA:
        case N_RECORD_LIT:
        case N_LIST_LIT:
        case N_RANGE_LIT:
        case N_SPREAD:
        case N_PIPE:
        case N_FIELD:
        case N_INDEX:
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
            /* children = [pattern, type, expr] */
            Node *pat = n->children[0];
            Node *val = n->children[2];
            if (pat->kind != N_PAT_BIND) {
                bug(e, n, "let with non-ident pattern (M6)");
                return;
            }
            fprintf(e->out, "KaiValue *kai_%.*s = ",
                    (int) pat->name_len, pat->name);
            emit_expr(e, val);
            fputs(";", e->out);
            return;
        }
        case N_ASSERT: {
            /* Wrapped panic-on-false. Used by tests (M9). */
            fputs("{ KaiValue *_cond = ", e->out);
            emit_expr(e, n->children[0]);
            fputs("; if (!(_cond && _cond->tag == KAI_BOOL && _cond->as.b)) {"
                  " kai_prelude_panic(kai_str(\"assertion failed\")); } }",
                  e->out);
            return;
        }
        case N_EXPR_STMT: {
            /* Evaluate for side effects, discard. */
            fputs("{ KaiValue *_ = ", e->out);
            emit_expr(e, n->children[0]);
            fputs("; (void) _; }", e->out);
            return;
        }
        default:
            /* Treat any other node as an expression statement. */
            fputs("{ KaiValue *_ = ", e->out);
            emit_expr(e, n);
            fputs("; (void) _; }", e->out);
            return;
    }
}

/* ---------- function and program emission ---------- */

static void emit_fn_signature(E *e, Node *fn) {
    fprintf(e->out, "static KaiValue *kai_%.*s(", (int) fn->name_len, fn->name);
    int first = 1;
    for (size_t i = 2; i < fn->n_children; ++i) {
        Node *param = fn->children[i];
        if (!param || param->kind != N_PARAM) continue;
        if (!first) fputs(", ", e->out);
        first = 0;
        fprintf(e->out, "KaiValue *kai_%.*s",
                (int) param->name_len, param->name);
    }
    if (first) fputs("void", e->out);
    fputc(')', e->out);
}

static void emit_fn(E *e, Node *fn) {
    emit_fn_signature(e, fn);
    fputs(" {\n    return ", e->out);
    Node *body = fn->children[1];
    emit_expr(e, body);
    fputs(";\n}\n\n", e->out);
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
    e.out = out;
    e.had_error = 0;

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

    /* Function bodies. */
    for (size_t i = 0; i < program->n_children; ++i) {
        Node *d = program->children[i];
        if (!d) continue;
        if (d->kind == N_FN) {
            emit_fn(&e, d);
        }
        /* Other top-level forms (type_decl, test, import) are no-ops at
           this milestone — they'll be handled in later milestones. */
    }

    if (has_main(program)) {
        fputs("int main(void) {\n"
              "    KaiValue *_result = kai_main();\n"
              "    kai_decref(_result);\n"
              "    return 0;\n"
              "}\n", out);
    }

    return e.had_error ? 1 : 0;
}
