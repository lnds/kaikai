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
    "string_slice", "char_to_int", "int_to_char",
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

static void check_node(C *c, Node *n) {
    if (!n) return;
    switch (n->kind) {

    /* -------- declarations visited from program pass -------- */
    case N_FN: {
        /* children = [return_type, body, params...] */
        scope_push(c);
        for (size_t i = 2; i < n->n_children; ++i) {
            Node *param = n->children[i];
            if (param && param->kind == N_PARAM) {
                scope_add(c, param->name, param->name_len);
            }
        }
        Node *body = n->children[1];
        check_node(c, body);
        scope_pop(c);
        break;
    }
    case N_TEST: {
        /* A test's body runs like a function body with no params. */
        scope_push(c);
        if (n->n_children >= 1) check_node(c, n->children[0]);
        scope_pop(c);
        break;
    }
    case N_TYPE_DECL:
    case N_IMPORT:
        /* Nothing to check in the body for stage 0. */
        break;

    /* -------- statements -------- */
    case N_LET: {
        /* children = [pattern, type, expr]; evaluate expr in current
           scope, then bind pattern names in current scope. */
        if (n->n_children >= 3) check_node(c, n->children[2]);
        if (n->n_children >= 1) bind_pattern(c, n->children[0]);
        break;
    }
    case N_ASSERT:
    case N_EXPR_STMT: {
        for (size_t i = 0; i < n->n_children; ++i) check_node(c, n->children[i]);
        break;
    }

    /* -------- expressions -------- */
    case N_IDENT: {
        if (!scope_has(c, n->name, n->name_len)) {
            error_at(c, n->line, n->col, "undefined name", n->name, n->name_len);
        }
        break;
    }
    case N_FIELD: {
        /* Only check the base; the field name is resolved structurally,
           not against the scope. */
        if (n->n_children >= 1) check_node(c, n->children[0]);
        break;
    }
    case N_RECORD_LIT: {
        /* record type name is a type; we don't resolve type names here.
           Visit the field initializers' values. */
        for (size_t i = 0; i < n->n_children; ++i) check_node(c, n->children[i]);
        break;
    }
    case N_FIELD_INIT: {
        if (n->n_children >= 1) check_node(c, n->children[0]);
        break;
    }
    case N_LAMBDA: {
        /* children = [body, param_idents...] */
        scope_push(c);
        for (size_t i = 1; i < n->n_children; ++i) {
            Node *p = n->children[i];
            if (p && p->kind == N_IDENT) {
                scope_add(c, p->name, p->name_len);
            }
        }
        /* Also allow `.` placeholder usage inside lambda bodies — nothing
           special to bind; we just let it through during check. */
        if (n->n_children >= 1) check_node(c, n->children[0]);
        scope_pop(c);
        break;
    }
    case N_BLOCK: {
        /* A block introduces a new scope so `let` bindings are local. */
        scope_push(c);
        for (size_t i = 0; i < n->n_children; ++i) check_node(c, n->children[i]);
        scope_pop(c);
        break;
    }
    case N_MATCH: {
        /* children = [scrutinee, arms...] */
        if (n->n_children >= 1) check_node(c, n->children[0]);
        for (size_t i = 1; i < n->n_children; ++i) check_node(c, n->children[i]);
        break;
    }
    case N_ARM: {
        /* children = [pattern, (guard), body]; each arm scope binds pattern names. */
        scope_push(c);
        Node *pat = n->children[0];
        bind_pattern(c, pat);
        if (n->v.flags & 0x1) {
            /* guard */
            if (n->n_children >= 2) check_node(c, n->children[1]);
            if (n->n_children >= 3) check_node(c, n->children[2]);
        } else {
            if (n->n_children >= 2) check_node(c, n->children[1]);
        }
        scope_pop(c);
        break;
    }
    case N_PLACEHOLDER:
        /* `.` is accepted anywhere; semantic validation (must appear in
           lambda-expected position) is deferred to the emitter. */
        break;

    /* -------- simple recursive cases -------- */
    case N_PROGRAM:
    case N_CALL: case N_BINOP: case N_UNOP: case N_IF:
    case N_LIST_LIT: case N_RANGE_LIT: case N_SPREAD: case N_PIPE:
    case N_INDEX: {
        for (size_t i = 0; i < n->n_children; ++i) check_node(c, n->children[i]);
        break;
    }

    /* -------- leaves / uninteresting -------- */
    case N_INT: case N_REAL: case N_CHAR: case N_STRING:
    case N_BOOL: case N_UNIT:
    case N_TY_NAME: case N_TY_LIST: case N_TY_FN: case N_TY_RECORD:
    case N_TY_SUM: case N_TY_ALIAS: case N_TY_INFER:
    case N_FIELD_DECL: case N_VARIANT: case N_PARAM:
    case N_PAT_WILD: case N_PAT_LIT: case N_PAT_BIND:
    case N_PAT_LIST: case N_PAT_VARIANT: case N_PAT_RECORD:
    case N_PAT_FIELD:
    case N_KIND_COUNT:
        /* nothing to resolve inside these */
        break;
    }
}

/* ---------- first pass: register top-level decls ---------- */

static void register_top_level(C *c, Node *prog) {
    for (size_t i = 0; i < prog->n_children; ++i) {
        Node *d = prog->children[i];
        if (!d) continue;
        if (d->kind == N_FN) {
            scope_add(c, d->name, d->name_len);
        } else if (d->kind == N_TYPE_DECL) {
            /* Register the type name (mostly for completeness; we don't
               check its uses in type positions yet). If the body is a
               sum, register each variant as a constructor name. */
            scope_add(c, d->name, d->name_len);
            if (d->n_children >= 1) {
                Node *body = d->children[0];
                if (body && body->kind == N_TY_SUM) {
                    for (size_t j = 0; j < body->n_children; ++j) {
                        Node *v = body->children[j];
                        if (v && v->kind == N_VARIANT) {
                            scope_add(c, v->name, v->name_len);
                        }
                    }
                }
            }
        } else if (d->kind == N_IMPORT) {
            /* For single-module stage 0, imports introduce at least the
               last segment of the dotted path into scope. Alias and
               selection children are already N_IDENT. */
            if (d->v.flags & 0x1) {
                /* has_alias: first child is the alias ident */
                if (d->n_children >= 1) {
                    Node *a = d->children[0];
                    if (a && a->kind == N_IDENT) scope_add(c, a->name, a->name_len);
                }
            } else if (d->n_children > 0) {
                /* selection list */
                for (size_t j = 0; j < d->n_children; ++j) {
                    Node *sel = d->children[j];
                    if (sel && sel->kind == N_IDENT) scope_add(c, sel->name, sel->name_len);
                }
            } else {
                /* Bare qualified import: bring the last path segment into scope. */
                const char *p = d->name;
                size_t len = d->name_len;
                size_t start = 0;
                for (size_t j = 0; j < len; ++j) if (p[j] == '.') start = j + 1;
                scope_add(c, p + start, len - start);
            }
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
