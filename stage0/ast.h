#ifndef KAI_AST_H
#define KAI_AST_H

#include <stddef.h>
#include <stdint.h>

/*
 * AST for kaikai-minimal.
 *
 * All nodes share a single struct type. The kind field discriminates; the
 * children array holds any sub-nodes; `name` holds a source-buffer span
 * for name-carrying nodes (identifiers, field names, string/int literal
 * text, etc.). Scalar decoded values live in `v`.
 *
 * Ownership: a Node owns its children; kai_free_node recurses. Children
 * pointers are malloc-ed arrays. Name pointers are not owned (they alias
 * the original source buffer kept alive by the caller).
 *
 * For convenience, this header documents, for each NodeKind, the expected
 * layout of children. When a position is "optional", it is either present
 * as an actual sub-node or NULL — the spec says which.
 */

typedef struct Node Node;

typedef enum {
    /* ---------- program and declarations ---------- */
    N_PROGRAM,        /* children = decls (each: FN/TYPE_DECL/TEST/IMPORT) */
    N_FN,             /* name = fn name;
                         v.flags: 0x1 is_pub, 0x2 body_is_expr, 0x4 ret_omitted
                         children = [return_type, body, param0, ..., paramN]
                         (return_type is N_TY_NAME "Unit" when ret_omitted) */
    N_PARAM,          /* name = param name; children = [type] */
    N_TYPE_DECL,      /* name = type name; v.flags: 0x1 is_pub;
                         children = [body, ty_param0, ..., ty_paramN]
                         ty_params are N_IDENT */
    N_TEST,           /* name = raw string literal (including quotes);
                         children = [body_block] */
    N_IMPORT,         /* name = dotted module path, e.g. "math.vector";
                         v.flags: 0x1 has_alias;
                         children:
                           if has_alias: [alias_ident, selected0, ..., selectedN]
                           else        : [selected0, ..., selectedN] */

    /* ---------- type expressions ---------- */
    N_TY_NAME,        /* name = type name; children = [arg0, ..., argN] */
    N_TY_LIST,        /* children = [element_type] */
    N_TY_FN,          /* children = [return_type, param_type0, ..., param_typeN] */
    N_TY_RECORD,      /* children = [N_FIELD_DECL, ...] */
    N_TY_SUM,         /* children = [N_VARIANT, ...] */
    N_TY_ALIAS,       /* children = [the aliased type] */
    N_TY_INFER,       /* placeholder for "infer from RHS"; no children */
    N_FIELD_DECL,     /* name = field name; children = [type] */
    N_VARIANT,        /* name = constructor name;
                         children = [arg_type0, ..., arg_typeN] (may be empty) */

    /* ---------- statements ---------- */
    N_LET,            /* v.flags: 0x1 has_type;
                         children = [pattern, type, expression]
                         type is N_TY_INFER when has_type = 0 */
    N_ASSERT,         /* v.flags: 0x1 has_msg;
                         children = [cond] or [cond, msg] */
    N_EXPR_STMT,      /* children = [expression] */

    /* ---------- expressions ---------- */
    N_INT,            /* v.i */
    N_REAL,           /* v.r */
    N_CHAR,           /* v.c */
    N_STRING,         /* name = raw span including surrounding quotes;
                         v.flags: 0x1 triple */
    N_BOOL,           /* v.b: 0 or 1 */
    N_UNIT,           /* () */
    N_IDENT,          /* name = identifier text */
    N_PLACEHOLDER,    /* the lone `.` in lambda-expected position */

    N_CALL,           /* children = [callee, arg0, ..., argN] */
    N_FIELD,          /* name = field name; children = [base] */
    N_INDEX,          /* children = [base, index] */
    N_BINOP,          /* v.op = TokenKind; children = [lhs, rhs] */
    N_UNOP,           /* v.op = TokenKind; children = [rhs] */

    N_IF,             /* v.flags: 0x1 has_else;
                         children = [cond, then_block] or
                                    [cond, then_block, else_branch]
                         else_branch is another N_IF (for "else if") or block */
    N_MATCH,          /* children = [scrutinee, arm0, ..., armN] */
    N_ARM,            /* v.flags: 0x1 has_guard;
                         children = [pattern, body] or [pattern, guard, body] */
    N_LAMBDA,         /* children = [body, param_ident0, ..., param_identN] */

    N_RECORD_LIT,     /* name = record type name;
                         children = [N_FIELD_INIT, ...] */
    N_FIELD_INIT,     /* name = field name; children = [value] */
    N_LIST_LIT,       /* children = [elt0, ..., eltN]
                         elements may themselves be N_SPREAD */
    N_RANGE_LIT,      /* v.flags: 0x1 has_step;
                         children = [from, to] or [from, to, step] */
    N_SPREAD,         /* children = [inner] — the ... prefix form */
    N_PIPE,           /* children = [lhs, rhs] — the |> apply pipe */
    N_BLOCK,          /* v.flags: 0x1 has_value;
                         children = [stmt0, ..., stmtN, value]
                         `value` is the final expression; absent block content
                         still has a trailing N_UNIT as placeholder value when
                         has_value = 0 */

    /* ---------- patterns ---------- */
    N_PAT_WILD,       /* _ */
    N_PAT_LIT,        /* children = [literal_node] */
    N_PAT_BIND,       /* name = bound identifier */
    N_PAT_LIST,       /* v.flags: 0x1 has_rest;
                         children = [elt0, ..., eltN] and if has_rest,
                         an extra trailing N_PAT_BIND for the ...rest ident */
    N_PAT_VARIANT,    /* name = constructor;
                         children = [sub_pat0, ..., sub_patN] */
    N_PAT_RECORD,     /* children = [N_PAT_FIELD, ...] */
    N_PAT_FIELD,      /* name = field name; v.flags: 0x1 shorthand;
                         children = [sub_pattern]
                         if shorthand: sub_pattern is N_PAT_BIND with same name */

    N_KIND_COUNT
} NodeKind;

struct Node {
    NodeKind kind;
    int32_t  line, col;

    /* Text span into source (not owned). */
    const char *name;
    size_t      name_len;

    /* Children, malloc-ed. */
    Node      **children;
    size_t      n_children;

    /* Scalar payloads — pick the right union member per kind. */
    union {
        int64_t  i;
        double   r;
        uint32_t c;
        int      b;
        int      op;       /* a TokenKind value */
        int      flags;    /* per-kind bitfield, see enum comments */
    } v;
};

/* Allocation helper (all fields zero-initialized). */
Node *kai_new_node(NodeKind k, int32_t line, int32_t col);

/* Appends a child to a node, growing `children` as needed. */
void  kai_node_push(Node *parent, Node *child);

/* Recursively frees a node and all its children. */
void  kai_free_node(Node *n);

/* Debug pretty-printer. Dumps a tree to stdout with indentation. */
void  kai_dump_ast(const Node *n);

/* Human-readable name of a NodeKind. */
const char *nk_name(NodeKind k);

#endif /* KAI_AST_H */
