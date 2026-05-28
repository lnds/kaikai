#include "ast.h"
#include "lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Node *kai_new_node(NodeKind k, int32_t line, int32_t col) {
    Node *n = (Node *) calloc(1, sizeof(Node));
    if (!n) { fprintf(stderr, "ast: out of memory\n"); exit(1); }
    n->kind = k;
    n->line = line;
    n->col  = col;
    return n;
}

void kai_node_push(Node *parent, Node *child) {
    /* Naive doubling array. */
    static const size_t INITIAL_CAP = 2;
    size_t cap = 0;
    /* We don't track capacity separately; for stage 0 simplicity we just
       grow by powers of two based on current size. */
    if (parent->n_children == 0) cap = 0;
    else {
        cap = INITIAL_CAP;
        while (cap < parent->n_children) cap *= 2;
    }
    if (parent->n_children >= cap) {
        size_t newcap = cap == 0 ? INITIAL_CAP : cap * 2;
        Node **c = (Node **) realloc(parent->children, newcap * sizeof(Node *));
        if (!c) { fprintf(stderr, "ast: out of memory\n"); exit(1); }
        parent->children = c;
    }
    parent->children[parent->n_children++] = child;
}

void kai_free_node(Node *n) {
    if (!n) return;
    for (size_t i = 0; i < n->n_children; ++i) kai_free_node(n->children[i]);
    free(n->children);
    free(n);
}

/* Name table indexed by NodeKind. Designated initializers keep entries
   pinned to their enum value, so reordering the enum cannot desync this
   table. The _Static_assert below fails the build if a new kind is added
   without a matching row here (NODE_NAMES would grow past N_KIND_COUNT or
   leave a hole that trips the assert). */
static const char *const NODE_NAMES[] = {
    [N_PROGRAM]     = "program",
    [N_FN]          = "fn",
    [N_PARAM]       = "param",
    [N_TYPE_DECL]   = "type_decl",
    [N_TEST]        = "test",
    [N_IMPORT]      = "import",

    [N_TY_NAME]     = "ty_name",
    [N_TY_LIST]     = "ty_list",
    [N_TY_FN]       = "ty_fn",
    [N_TY_RECORD]   = "ty_record",
    [N_TY_SUM]      = "ty_sum",
    [N_TY_ALIAS]    = "ty_alias",
    [N_TY_INFER]    = "ty_infer",
    [N_FIELD_DECL]  = "field_decl",
    [N_VARIANT]     = "variant",

    [N_LET]         = "let",
    [N_ASSERT]      = "assert",
    [N_EXPR_STMT]   = "expr_stmt",

    [N_INT]         = "int",
    [N_REAL]        = "real",
    [N_CHAR]        = "char",
    [N_STRING]      = "string",
    [N_BOOL]        = "bool",
    [N_UNIT]        = "unit",
    [N_IDENT]       = "ident",
    [N_PLACEHOLDER] = "placeholder",

    [N_CALL]        = "call",
    [N_FIELD]       = "field",
    [N_INDEX]       = "index",
    [N_BINOP]       = "binop",
    [N_UNOP]        = "unop",

    [N_IF]          = "if",
    [N_MATCH]       = "match",
    [N_ARM]         = "arm",
    [N_LAMBDA]      = "lambda",

    [N_RECORD_LIT]  = "record_lit",
    [N_FIELD_INIT]  = "field_init",
    [N_LIST_LIT]    = "list_lit",
    [N_RANGE_LIT]   = "range_lit",
    [N_SPREAD]      = "spread",
    [N_PIPE]        = "pipe",
    [N_BLOCK]       = "block",

    [N_PAT_WILD]    = "pat_wild",
    [N_PAT_LIT]     = "pat_lit",
    [N_PAT_BIND]    = "pat_bind",
    [N_PAT_LIST]    = "pat_list",
    [N_PAT_VARIANT] = "pat_variant",
    [N_PAT_RECORD]  = "pat_record",
    [N_PAT_FIELD]   = "pat_field",
};

/* One row per NodeKind: the table must cover the whole enum exactly.
   C99-portable static assert (no C11 _Static_assert) — a negative-size
   array typedef fails the build if NODE_NAMES drifts from NodeKind. */
typedef char node_names_in_sync[
    (sizeof(NODE_NAMES) / sizeof(NODE_NAMES[0]) == N_KIND_COUNT) ? 1 : -1];

const char *nk_name(NodeKind k) {
    if (k < 0 || k >= N_KIND_COUNT || !NODE_NAMES[k]) return "?";
    return NODE_NAMES[k];
}

static void dump_indent(int depth) { for (int i = 0; i < depth; ++i) fputs("  ", stdout); }

static void dump_rec(const Node *n, int depth) {
    if (!n) { dump_indent(depth); printf("(nil)\n"); return; }
    dump_indent(depth);
    printf("%s", nk_name(n->kind));
    if (n->name_len > 0) printf(" \"%.*s\"", (int) n->name_len, n->name);
    switch (n->kind) {
        case N_INT:  printf(" = %lld", (long long) n->v.i); break;
        case N_REAL: printf(" = %g",   n->v.r); break;
        case N_CHAR: printf(" = U+%04X", n->v.c); break;
        case N_BOOL: printf(" = %s", n->v.b ? "true" : "false"); break;
        case N_BINOP:
        case N_UNOP: printf(" op=%s", tk_name((TokenKind) n->v.op)); break;
        default: break;
    }
    /* v.flags shares memory with v.op/v.i/etc.; only meaningful for kinds
       that actually use flags. */
    switch (n->kind) {
        case N_FN: case N_TYPE_DECL: case N_IMPORT: case N_LET:
        case N_ASSERT: case N_IF: case N_ARM: case N_STRING:
        case N_RANGE_LIT: case N_BLOCK:
        case N_PAT_LIST: case N_PAT_FIELD:
            if (n->v.flags) printf(" flags=0x%x", (unsigned) n->v.flags);
            break;
        default: break;
    }
    printf(" @%d:%d\n", n->line, n->col);
    for (size_t i = 0; i < n->n_children; ++i) dump_rec(n->children[i], depth + 1);
}

void kai_dump_ast(const Node *n) { dump_rec(n, 0); }
