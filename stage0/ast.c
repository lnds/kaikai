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

const char *nk_name(NodeKind k) {
    switch (k) {
        case N_PROGRAM:     return "program";
        case N_FN:          return "fn";
        case N_PARAM:       return "param";
        case N_TYPE_DECL:   return "type_decl";
        case N_TEST:        return "test";
        case N_IMPORT:      return "import";

        case N_TY_NAME:     return "ty_name";
        case N_TY_LIST:     return "ty_list";
        case N_TY_FN:       return "ty_fn";
        case N_TY_RECORD:   return "ty_record";
        case N_TY_SUM:      return "ty_sum";
        case N_TY_ALIAS:    return "ty_alias";
        case N_TY_INFER:    return "ty_infer";
        case N_FIELD_DECL:  return "field_decl";
        case N_VARIANT:     return "variant";

        case N_LET:         return "let";
        case N_ASSERT:      return "assert";
        case N_EXPR_STMT:   return "expr_stmt";

        case N_INT:         return "int";
        case N_REAL:        return "real";
        case N_CHAR:        return "char";
        case N_STRING:      return "string";
        case N_BOOL:        return "bool";
        case N_UNIT:        return "unit";
        case N_IDENT:       return "ident";
        case N_PLACEHOLDER: return "placeholder";

        case N_CALL:        return "call";
        case N_FIELD:       return "field";
        case N_INDEX:       return "index";
        case N_BINOP:       return "binop";
        case N_UNOP:        return "unop";

        case N_IF:          return "if";
        case N_MATCH:       return "match";
        case N_ARM:         return "arm";
        case N_LAMBDA:      return "lambda";

        case N_RECORD_LIT:  return "record_lit";
        case N_FIELD_INIT:  return "field_init";
        case N_LIST_LIT:    return "list_lit";
        case N_RANGE_LIT:   return "range_lit";
        case N_SPREAD:      return "spread";
        case N_PIPE:        return "pipe";
        case N_BLOCK:       return "block";

        case N_PAT_WILD:    return "pat_wild";
        case N_PAT_LIT:     return "pat_lit";
        case N_PAT_BIND:    return "pat_bind";
        case N_PAT_LIST:    return "pat_list";
        case N_PAT_VARIANT: return "pat_variant";
        case N_PAT_RECORD:  return "pat_record";
        case N_PAT_FIELD:   return "pat_field";

        case N_KIND_COUNT:  return "?";
    }
    return "?";
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
