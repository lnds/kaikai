/*
 * Name resolution and minimal checks for kaikai-minimal.
 *
 * Strategy:
 *   1. Seed a global scope with the prelude (print, map, filter, Some, ...).
 *   2. First pass over top-level decls: register function names, type
 *      names, and sum-type variant constructor names. This gives forward
 *      references between declarations.
 *   3. Second pass: visit each declaration body. Function bodies get a
 *      scope with their parameters; match arms bind their pattern names;
 *      lambda bodies get a scope with their parameters.
 *   4. Any N_IDENT that cannot be resolved to a name in scope is
 *      reported as an error.
 *
 * Type positions (N_TY_*) are not checked here; their names are only
 * meaningful against the type declarations the parser saw, and for
 * stage 0 we rely on the emitter's type erasure.
 */

#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    size_t      len;
} Entry;

typedef struct Scope {
    struct Scope *parent;
    Entry       *entries;
    size_t       n;
    size_t       cap;
} Scope;

typedef struct {
    const char *file;
    const char *src;
    Scope      *scope;
    int         had_error;
} C;

/* ---------- scope machinery ---------- */

static void scope_push(C *c) {
    Scope *s = (Scope *) calloc(1, sizeof(Scope));
    if (!s) { fprintf(stderr, "check: out of memory\n"); exit(1); }
    s->parent = c->scope;
    c->scope  = s;
}

static void scope_pop(C *c) {
    Scope *s = c->scope;
    c->scope = s->parent;
    free(s->entries);
    free(s);
}

static void scope_add(C *c, const char *name, size_t len) {
    Scope *s = c->scope;
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->entries = (Entry *) realloc(s->entries, s->cap * sizeof(Entry));
        if (!s->entries) { fprintf(stderr, "check: out of memory\n"); exit(1); }
    }
    s->entries[s->n].name = name;
    s->entries[s->n].len  = len;
    s->n++;
}

static int scope_has(C *c, const char *name, size_t len) {
    for (Scope *s = c->scope; s; s = s->parent) {
        for (size_t i = 0; i < s->n; ++i) {
            if (s->entries[i].len == len &&
                memcmp(s->entries[i].name, name, len) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

/* ---------- prelude ---------- */

static const char *PRELUDE[] = {
    /* IO */
    "print", "eprint", "read_file", "write_file", "read_line", "read_bytes", "args",
    "exit", "panic",
    /* conversions */
    "int_to_string", "real_to_string", "string_to_int", "string_to_real",
    "int_to_real", "real_to_int",
    /* strings */
    "string_length", "string_concat", "string_concat_all", "string_join",
    "string_split", "string_contains", "char_at",
    "string_slice", "char_to_int", "int_to_char", "int_to_byte_string",
    "string_byte_at_int",
    /* lists */
    "list_length", "list_append", "list_reverse",
    "map", "filter", "reduce", "each",
    /* opaque mutable arrays (used by the stage 2 inferencer) */
    "array_make", "array_length", "array_get", "array_set", "array_grow",
    /* Option / Result constructors */
    "Some", "None", "Ok", "Err",
    NULL
};

static void add_prelude(C *c) {
    for (const char **p = PRELUDE; *p; ++p) {
        scope_add(c, *p, strlen(*p));
    }
}

/* ---------- error reporting ---------- */

static void error_at(C *c, int line, int col, const char *msg,
                     const char *name, size_t name_len) {
    c->had_error = 1;
    if (name_len > 0) {
        fprintf(stderr, "%s:%d:%d: error: %s `%.*s`\n",
                c->file, line, col, msg, (int) name_len, name);
    } else {
        fprintf(stderr, "%s:%d:%d: error: %s\n",
                c->file, line, col, msg);
    }
}

/* ---------- forwards ---------- */

static void check_node(C *c, Node *n);
static void bind_pattern(C *c, Node *pat);

/* ---------- pattern bindings ---------- */

static void bind_pattern(C *c, Node *pat) {
    if (!pat) return;
    switch (pat->kind) {
        case N_PAT_WILD:
            break;
        case N_PAT_LIT:
            /* literal patterns bind nothing */
            break;
        case N_PAT_BIND:
            scope_add(c, pat->name, pat->name_len);
            break;
        case N_PAT_LIST:
            for (size_t i = 0; i < pat->n_children; ++i) bind_pattern(c, pat->children[i]);
            break;
        case N_PAT_VARIANT:
            /* constructor name is not a binding; its children are sub-patterns */
            for (size_t i = 0; i < pat->n_children; ++i) bind_pattern(c, pat->children[i]);
            break;
        case N_PAT_RECORD:
            for (size_t i = 0; i < pat->n_children; ++i) bind_pattern(c, pat->children[i]);
            break;
        case N_PAT_FIELD:
            /* field name is the record field, not a binding by itself.
               Its sub-pattern (children[0]) may introduce bindings. */
            if (pat->n_children >= 1) bind_pattern(c, pat->children[0]);
            break;
        default:
            break;
    }
}

/* ---------- expression checker ---------- */

/* Visit every child in the current scope. Shared by the structural cases
   that introduce no scope or binding of their own (calls, operators, list
   literals, record literals, the program root, etc.). */
static void check_children(C *c, Node *n) {
    for (size_t i = 0; i < n->n_children; ++i) check_node(c, n->children[i]);
}

/* N_FN: children = [return_type, body, params...]. Body sees a scope
   seeded with the parameter names. */
static void check_fn(C *c, Node *n) {
    scope_push(c);
    for (size_t i = 2; i < n->n_children; ++i) {
        Node *param = n->children[i];
        if (param && param->kind == N_PARAM) {
            scope_add(c, param->name, param->name_len);
        }
    }
    check_node(c, n->children[1]);
    scope_pop(c);
}

/* N_TEST: the body runs like a function body with no params. */
static void check_test(C *c, Node *n) {
    scope_push(c);
    if (n->n_children >= 1) check_node(c, n->children[0]);
    scope_pop(c);
}

/* N_LET: children = [pattern, type, expr]. Evaluate the expression in the
   current scope, then bind the pattern names into the current scope. */
static void check_let(C *c, Node *n) {
    if (n->n_children >= 3) check_node(c, n->children[2]);
    if (n->n_children >= 1) bind_pattern(c, n->children[0]);
}

/* N_IDENT: the only case that actually reports an unresolved name. */
static void check_ident(C *c, Node *n) {
    if (!scope_has(c, n->name, n->name_len)) {
        error_at(c, n->line, n->col, "undefined name", n->name, n->name_len);
    }
}

/* N_LAMBDA: children = [body, param_idents...]. Body sees a scope seeded
   with the parameter idents. The `.` placeholder needs no binding. */
static void check_lambda(C *c, Node *n) {
    scope_push(c);
    for (size_t i = 1; i < n->n_children; ++i) {
        Node *p = n->children[i];
        if (p && p->kind == N_IDENT) {
            scope_add(c, p->name, p->name_len);
        }
    }
    if (n->n_children >= 1) check_node(c, n->children[0]);
    scope_pop(c);
}

/* N_BLOCK: introduces a new scope so `let` bindings stay local. */
static void check_block(C *c, Node *n) {
    scope_push(c);
    check_children(c, n);
    scope_pop(c);
}

/* N_ARM: children = [pattern, (guard), body]. Each arm scope binds the
   pattern names; the guard (if present) and body see those bindings. */
static void check_arm(C *c, Node *n) {
    scope_push(c);
    bind_pattern(c, n->children[0]);
    if (n->v.flags & 0x1) {
        if (n->n_children >= 2) check_node(c, n->children[1]);
        if (n->n_children >= 3) check_node(c, n->children[2]);
    } else {
        if (n->n_children >= 2) check_node(c, n->children[1]);
    }
    scope_pop(c);
}

static void check_node(C *c, Node *n) {
    if (!n) return;
    switch (n->kind) {

    /* -------- cases with their own scope / binding logic -------- */
    case N_FN:     check_fn(c, n);     break;
    case N_TEST:   check_test(c, n);   break;
    case N_LET:    check_let(c, n);    break;
    case N_IDENT:  check_ident(c, n);  break;
    case N_LAMBDA: check_lambda(c, n); break;
    case N_BLOCK:  check_block(c, n);  break;
    case N_ARM:    check_arm(c, n);    break;

    /* -------- structural cases: just visit children in scope -------- */
    case N_PROGRAM:
    case N_ASSERT: case N_EXPR_STMT:
    case N_FIELD: case N_RECORD_LIT: case N_FIELD_INIT:
    case N_MATCH:
    case N_CALL: case N_BINOP: case N_UNOP: case N_IF:
    case N_LIST_LIT: case N_RANGE_LIT: case N_SPREAD: case N_PIPE:
    case N_INDEX:
        check_children(c, n);
        break;

    /* -------- leaves / nothing to resolve -------- */
    case N_TYPE_DECL: case N_IMPORT:   /* no body to check in stage 0 */
    case N_PLACEHOLDER:                /* `.` accepted anywhere; emitter validates */
    case N_INT: case N_REAL: case N_CHAR: case N_STRING:
    case N_BOOL: case N_UNIT:
    case N_TY_NAME: case N_TY_LIST: case N_TY_FN: case N_TY_RECORD:
    case N_TY_SUM: case N_TY_ALIAS: case N_TY_INFER:
    case N_FIELD_DECL: case N_VARIANT: case N_PARAM:
    case N_PAT_WILD: case N_PAT_LIT: case N_PAT_BIND:
    case N_PAT_LIST: case N_PAT_VARIANT: case N_PAT_RECORD:
    case N_PAT_FIELD:
    case N_KIND_COUNT:
        break;
    }
}

/* ---------- first pass: register top-level decls ---------- */

/* type X = ... : register the type name, and if the body is a sum, each
   variant constructor name. */
static void register_type_decl(C *c, Node *d) {
    scope_add(c, d->name, d->name_len);
    if (d->n_children < 1) return;
    Node *body = d->children[0];
    if (!body || body->kind != N_TY_SUM) return;
    for (size_t j = 0; j < body->n_children; ++j) {
        Node *v = body->children[j];
        if (v && v->kind == N_VARIANT) scope_add(c, v->name, v->name_len);
    }
}

/* import a.b.c [as x] [.{ sel, ... }] : bring the alias, the selected
   names, or (bare) the last dotted segment into scope. */
static void register_import(C *c, Node *d) {
    if (d->v.flags & 0x1) {
        /* has_alias: first child is the alias ident */
        Node *a = d->n_children >= 1 ? d->children[0] : NULL;
        if (a && a->kind == N_IDENT) scope_add(c, a->name, a->name_len);
        return;
    }
    if (d->n_children > 0) {
        /* selection list */
        for (size_t j = 0; j < d->n_children; ++j) {
            Node *sel = d->children[j];
            if (sel && sel->kind == N_IDENT) scope_add(c, sel->name, sel->name_len);
        }
        return;
    }
    /* Bare qualified import: bring the last path segment into scope. */
    const char *p = d->name;
    size_t len = d->name_len;
    size_t start = 0;
    for (size_t j = 0; j < len; ++j) if (p[j] == '.') start = j + 1;
    scope_add(c, p + start, len - start);
}

static void register_top_level(C *c, Node *prog) {
    for (size_t i = 0; i < prog->n_children; ++i) {
        Node *d = prog->children[i];
        if (!d) continue;
        switch (d->kind) {
            case N_FN:        scope_add(c, d->name, d->name_len); break;
            case N_TYPE_DECL: register_type_decl(c, d);           break;
            case N_IMPORT:    register_import(c, d);              break;
            default: break;
        }
    }
}

/* ---------- entry ---------- */

int kai_check(Node *program, const char *file, const char *src) {
    C c;
    c.file      = file;
    c.src       = src;
    c.scope     = NULL;
    c.had_error = 0;

    scope_push(&c);         /* global */
    add_prelude(&c);
    register_top_level(&c, program);

    for (size_t i = 0; i < program->n_children; ++i) {
        check_node(&c, program->children[i]);
    }

    scope_pop(&c);
    return c.had_error ? 1 : 0;
}
