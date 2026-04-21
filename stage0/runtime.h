/*
 * kaikai-minimal runtime for stage 0.
 *
 * Header-only. Generated C files #include this and compile together in
 * one translation unit, so static storage and linkage are fine.
 *
 * All values are heap-allocated KaiValues with a reference-count header
 * and a discriminating tag. Uniform boxing per docs/stage0-design.md;
 * primitives (Int, Real, Bool, Char, Unit) are wrapped in the same
 * struct so the generated code does not branch on shape.
 *
 * Calling convention for functions emitted by stage 0:
 *   static KaiValue *kai_<name>(KaiValue *arg0, KaiValue *arg1, ...);
 * Arguments are handed to the callee as owned references (callee must
 * decref them or keep them alive as part of the returned value). The
 * return value is owned by the caller.
 *
 * Closures and higher-order prelude helpers (map/filter/reduce/each)
 * go through a dynamic dispatch path `kai_apply(closure, argc, argv)`
 * to keep the generated code uniform.
 */

#ifndef KAI_RUNTIME_H
#define KAI_RUNTIME_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Not every program uses every prelude function; silence the
   unused-function warnings that would otherwise pile up in `cc` output
   when stage 0 links only the parts a given program needs. */
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-function"
#endif

/* ---------- types ---------- */

typedef enum {
    KAI_UNIT,
    KAI_BOOL,
    KAI_INT,
    KAI_REAL,
    KAI_CHAR,
    KAI_STR,
    KAI_NIL,
    KAI_CONS,
    KAI_RECORD,
    KAI_VARIANT,
    KAI_CLOSURE
} KaiTag;

typedef struct KaiValue KaiValue;

/* Dynamic-dispatch signature used for closures and higher-order calls. */
typedef KaiValue *(*KaiFn)(KaiValue *self, KaiValue **args, int n_args);

struct KaiValue {
    int32_t rc;
    int32_t tag;
    union {
        int      b;                                 /* KAI_BOOL */
        int64_t  i;                                 /* KAI_INT */
        double   r;                                 /* KAI_REAL */
        uint32_t c;                                 /* KAI_CHAR */
        struct { size_t len; char *bytes; } s;      /* KAI_STR (heap, not NUL-terminated but we always allocate +1 byte for safety) */
        struct { KaiValue *head; KaiValue *tail; } cons;
        struct {
            int          n_fields;
            KaiValue   **fields;
            const char **names;                     /* static strings, not freed */
        } rec;
        struct {
            int32_t     variant_tag;                /* index into the sum type */
            const char *variant_name;               /* static string */
            int32_t     n_args;
            KaiValue  **args;
        } var;
        struct {
            KaiFn       fn;
            int32_t     arity;
            int32_t     n_captures;
            KaiValue  **captures;
        } clo;
    } as;
};

/* ---------- allocation and refcounting ---------- */

/* Forward declarations used across sections. */
static int       kai_truthy(KaiValue *v);

static KaiValue *kai_alloc(KaiTag tag) {
    KaiValue *v = (KaiValue *) calloc(1, sizeof(KaiValue));
    if (!v) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    v->rc = 1;
    v->tag = (int32_t) tag;
    return v;
}

static KaiValue *kai_incref(KaiValue *v) { if (v) v->rc++; return v; }
static void       kai_decref(KaiValue *v);

static void kai_free_value(KaiValue *v) {
    switch ((KaiTag) v->tag) {
        case KAI_STR:
            free(v->as.s.bytes);
            break;
        case KAI_CONS:
            kai_decref(v->as.cons.head);
            kai_decref(v->as.cons.tail);
            break;
        case KAI_RECORD:
            for (int i = 0; i < v->as.rec.n_fields; ++i) kai_decref(v->as.rec.fields[i]);
            free(v->as.rec.fields);
            free((void *) v->as.rec.names);
            break;
        case KAI_VARIANT:
            for (int i = 0; i < v->as.var.n_args; ++i) kai_decref(v->as.var.args[i]);
            free(v->as.var.args);
            break;
        case KAI_CLOSURE:
            for (int i = 0; i < v->as.clo.n_captures; ++i) kai_decref(v->as.clo.captures[i]);
            free(v->as.clo.captures);
            break;
        default: break;
    }
    free(v);
}

static void kai_decref(KaiValue *v) {
    if (!v) return;
    if (--v->rc == 0) kai_free_value(v);
}

/* ---------- constructors ---------- */

static KaiValue *kai_unit(void) { return kai_alloc(KAI_UNIT); }

static KaiValue *kai_bool(int b) {
    KaiValue *v = kai_alloc(KAI_BOOL);
    v->as.b = b ? 1 : 0;
    return v;
}

static KaiValue *kai_int(int64_t i) {
    KaiValue *v = kai_alloc(KAI_INT);
    v->as.i = i;
    return v;
}

static KaiValue *kai_real(double r) {
    KaiValue *v = kai_alloc(KAI_REAL);
    v->as.r = r;
    return v;
}

static KaiValue *kai_char(uint32_t c) {
    KaiValue *v = kai_alloc(KAI_CHAR);
    v->as.c = c;
    return v;
}

static KaiValue *kai_str_from_bytes(const char *bytes, size_t len) {
    KaiValue *v = kai_alloc(KAI_STR);
    v->as.s.len = len;
    v->as.s.bytes = (char *) malloc(len + 1);
    if (!v->as.s.bytes) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    if (len > 0) memcpy(v->as.s.bytes, bytes, len);
    v->as.s.bytes[len] = '\0';
    return v;
}

static KaiValue *kai_str(const char *cstr) {
    return kai_str_from_bytes(cstr, strlen(cstr));
}

static KaiValue *kai_nil(void) { return kai_alloc(KAI_NIL); }

static KaiValue *kai_cons(KaiValue *head, KaiValue *tail) {
    KaiValue *v = kai_alloc(KAI_CONS);
    v->as.cons.head = head;
    v->as.cons.tail = tail;
    return v;
}

static KaiValue *kai_record(int n, KaiValue **fields, const char **names) {
    KaiValue *v = kai_alloc(KAI_RECORD);
    v->as.rec.n_fields = n;
    v->as.rec.fields = (KaiValue **) malloc(n * sizeof(KaiValue *));
    v->as.rec.names  = (const char **) malloc(n * sizeof(const char *));
    for (int i = 0; i < n; ++i) {
        v->as.rec.fields[i] = fields[i];
        v->as.rec.names[i]  = names[i];
    }
    return v;
}

static KaiValue *kai_variant(int32_t tag, const char *name, int n, KaiValue **args) {
    KaiValue *v = kai_alloc(KAI_VARIANT);
    v->as.var.variant_tag = tag;
    v->as.var.variant_name = name;
    v->as.var.n_args = n;
    if (n > 0) {
        v->as.var.args = (KaiValue **) malloc(n * sizeof(KaiValue *));
        for (int i = 0; i < n; ++i) v->as.var.args[i] = args[i];
    } else {
        v->as.var.args = NULL;
    }
    return v;
}

static KaiValue *kai_closure(KaiFn fn, int arity, int n_captures, KaiValue **captures) {
    KaiValue *v = kai_alloc(KAI_CLOSURE);
    v->as.clo.fn = fn;
    v->as.clo.arity = arity;
    v->as.clo.n_captures = n_captures;
    if (n_captures > 0) {
        v->as.clo.captures = (KaiValue **) malloc(n_captures * sizeof(KaiValue *));
        for (int i = 0; i < n_captures; ++i) v->as.clo.captures[i] = captures[i];
    } else {
        v->as.clo.captures = NULL;
    }
    return v;
}

/* Invoke a closure dynamically. */
static KaiValue *kai_apply(KaiValue *clo, int argc, KaiValue **argv) {
    if (!clo || clo->tag != KAI_CLOSURE) {
        fprintf(stderr, "kai: attempted to call a non-callable value\n");
        exit(1);
    }
    return clo->as.clo.fn(clo, argv, argc);
}

/* ---------- field access helpers ---------- */

static KaiValue *kai_field(KaiValue *rec, const char *name) {
    if (!rec || rec->tag != KAI_RECORD) {
        fprintf(stderr, "kai: field access on non-record\n"); exit(1);
    }
    for (int i = 0; i < rec->as.rec.n_fields; ++i) {
        if (strcmp(rec->as.rec.names[i], name) == 0) {
            return kai_incref(rec->as.rec.fields[i]);
        }
    }
    fprintf(stderr, "kai: no such field `%s`\n", name); exit(1);
}

/* ---------- equality ---------- */

static int kai_eq(KaiValue *a, KaiValue *b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->tag != b->tag) return 0;
    switch ((KaiTag) a->tag) {
        case KAI_UNIT: return 1;
        case KAI_BOOL: return a->as.b == b->as.b;
        case KAI_INT:  return a->as.i == b->as.i;
        case KAI_REAL: return a->as.r == b->as.r;
        case KAI_CHAR: return a->as.c == b->as.c;
        case KAI_STR:  return a->as.s.len == b->as.s.len &&
                              memcmp(a->as.s.bytes, b->as.s.bytes, a->as.s.len) == 0;
        case KAI_NIL:  return 1;
        case KAI_CONS:
            return kai_eq(a->as.cons.head, b->as.cons.head) &&
                   kai_eq(a->as.cons.tail, b->as.cons.tail);
        case KAI_VARIANT:
            if (a->as.var.variant_tag != b->as.var.variant_tag) return 0;
            if (a->as.var.n_args != b->as.var.n_args) return 0;
            for (int i = 0; i < a->as.var.n_args; ++i) {
                if (!kai_eq(a->as.var.args[i], b->as.var.args[i])) return 0;
            }
            return 1;
        case KAI_RECORD:
            if (a->as.rec.n_fields != b->as.rec.n_fields) return 0;
            for (int i = 0; i < a->as.rec.n_fields; ++i) {
                if (!kai_eq(a->as.rec.fields[i], b->as.rec.fields[i])) return 0;
            }
            return 1;
        case KAI_CLOSURE: return 0;      /* closures are not equatable */
    }
    return 0;
}

/* ---------- to-string ---------- */

static KaiValue *kai_string_concat(KaiValue *a, KaiValue *b);

static KaiValue *kai_to_string(KaiValue *v);

static KaiValue *kai_list_to_string(KaiValue *v) {
    KaiValue *acc = kai_str("[");
    int first = 1;
    while (v && v->tag == KAI_CONS) {
        if (!first) {
            KaiValue *c = kai_string_concat(acc, kai_str(", "));
            kai_decref(acc); acc = c;
        }
        first = 0;
        KaiValue *s = kai_to_string(v->as.cons.head);
        KaiValue *c = kai_string_concat(acc, s);
        kai_decref(acc); kai_decref(s); acc = c;
        v = v->as.cons.tail;
    }
    KaiValue *c = kai_string_concat(acc, kai_str("]"));
    kai_decref(acc); return c;
}

static KaiValue *kai_to_string(KaiValue *v) {
    if (!v) return kai_str("<null>");
    char buf[64];
    switch ((KaiTag) v->tag) {
        case KAI_UNIT: return kai_str("()");
        case KAI_BOOL: return kai_str(v->as.b ? "true" : "false");
        case KAI_INT:
            snprintf(buf, sizeof(buf), "%lld", (long long) v->as.i);
            return kai_str(buf);
        case KAI_REAL:
            snprintf(buf, sizeof(buf), "%g", v->as.r);
            return kai_str(buf);
        case KAI_CHAR:
            snprintf(buf, sizeof(buf), "%c", (char) v->as.c);
            return kai_str(buf);
        case KAI_STR:    return kai_incref(v);
        case KAI_NIL:    return kai_str("[]");
        case KAI_CONS:   return kai_list_to_string(v);
        case KAI_VARIANT: {
            KaiValue *acc = kai_str(v->as.var.variant_name);
            if (v->as.var.n_args > 0) {
                KaiValue *lp = kai_string_concat(acc, kai_str("(")); kai_decref(acc); acc = lp;
                for (int i = 0; i < v->as.var.n_args; ++i) {
                    if (i) { KaiValue *sep = kai_string_concat(acc, kai_str(", ")); kai_decref(acc); acc = sep; }
                    KaiValue *s = kai_to_string(v->as.var.args[i]);
                    KaiValue *c = kai_string_concat(acc, s); kai_decref(acc); kai_decref(s); acc = c;
                }
                KaiValue *rp = kai_string_concat(acc, kai_str(")")); kai_decref(acc); acc = rp;
            }
            return acc;
        }
        case KAI_RECORD: {
            KaiValue *acc = kai_str("{");
            for (int i = 0; i < v->as.rec.n_fields; ++i) {
                if (i) { KaiValue *sep = kai_string_concat(acc, kai_str(", ")); kai_decref(acc); acc = sep; }
                KaiValue *nm = kai_str(v->as.rec.names[i]);
                KaiValue *a1 = kai_string_concat(acc, nm); kai_decref(acc); kai_decref(nm); acc = a1;
                KaiValue *a2 = kai_string_concat(acc, kai_str(": ")); kai_decref(acc); acc = a2;
                KaiValue *fs = kai_to_string(v->as.rec.fields[i]);
                KaiValue *a3 = kai_string_concat(acc, fs); kai_decref(acc); kai_decref(fs); acc = a3;
            }
            KaiValue *cl = kai_string_concat(acc, kai_str("}")); kai_decref(acc);
            return cl;
        }
        case KAI_CLOSURE: return kai_str("<closure>");
    }
    return kai_str("?");
}

static KaiValue *kai_string_concat(KaiValue *a, KaiValue *b) {
    size_t la = (a && a->tag == KAI_STR) ? a->as.s.len : 0;
    size_t lb = (b && b->tag == KAI_STR) ? b->as.s.len : 0;
    KaiValue *v = kai_alloc(KAI_STR);
    v->as.s.len = la + lb;
    v->as.s.bytes = (char *) malloc(la + lb + 1);
    if (la) memcpy(v->as.s.bytes, a->as.s.bytes, la);
    if (lb) memcpy(v->as.s.bytes + la, b->as.s.bytes, lb);
    v->as.s.bytes[la + lb] = '\0';
    return v;
}

/* ---------- prelude: IO ---------- */

/*
 * Calling convention for the prelude: borrow semantics. Arguments are
 * borrowed from the caller (no incref / no decref on them). Return
 * values are owned by the caller.
 */

static KaiValue *kai_prelude_print(KaiValue *arg) {
    if (!arg) { fputc('\n', stdout); return kai_unit(); }
    if (arg->tag == KAI_STR) {
        fwrite(arg->as.s.bytes, 1, arg->as.s.len, stdout);
    } else {
        KaiValue *s = kai_to_string(arg);
        fwrite(s->as.s.bytes, 1, s->as.s.len, stdout);
        kai_decref(s);
    }
    fputc('\n', stdout);
    return kai_unit();
}

static KaiValue *kai_prelude_eprint(KaiValue *arg) {
    if (!arg) { fputc('\n', stderr); return kai_unit(); }
    if (arg->tag == KAI_STR) {
        fwrite(arg->as.s.bytes, 1, arg->as.s.len, stderr);
    } else {
        KaiValue *s = kai_to_string(arg);
        fwrite(s->as.s.bytes, 1, s->as.s.len, stderr);
        kai_decref(s);
    }
    fputc('\n', stderr);
    return kai_unit();
}

static KaiValue *kai_prelude_panic(KaiValue *msg) {
    fprintf(stderr, "panic: ");
    if (msg && msg->tag == KAI_STR) {
        fwrite(msg->as.s.bytes, 1, msg->as.s.len, stderr);
    }
    fputc('\n', stderr);
    exit(1);
    return kai_unit();
}

static KaiValue *kai_prelude_exit(KaiValue *code) {
    int c = (code && code->tag == KAI_INT) ? (int) code->as.i : 0;
    exit(c);
    return kai_unit();
}

/* ---------- prelude: conversions ---------- */

static KaiValue *kai_prelude_int_to_string(KaiValue *v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long) v->as.i);
    return kai_str(buf);
}

static KaiValue *kai_prelude_real_to_string(KaiValue *v) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", v->as.r);
    return kai_str(buf);
}

/* ---------- prelude: strings ---------- */

static KaiValue *kai_prelude_string_length(KaiValue *s) {
    int64_t n = (s && s->tag == KAI_STR) ? (int64_t) s->as.s.len : 0;
    return kai_int(n);
}

static KaiValue *kai_prelude_string_concat(KaiValue *a, KaiValue *b) {
    return kai_string_concat(a, b);
}

/* ---------- prelude: lists ---------- */

static KaiValue *kai_prelude_list_length(KaiValue *xs) {
    int64_t n = 0;
    KaiValue *p = xs;
    while (p && p->tag == KAI_CONS) { n++; p = p->as.cons.tail; }
    return kai_int(n);
}

static KaiValue *kai_prelude_list_append(KaiValue *xs, KaiValue *ys) {
    if (!xs || xs->tag == KAI_NIL) return kai_incref(ys);
    KaiValue *rest = kai_prelude_list_append(xs->as.cons.tail, ys);
    return kai_cons(kai_incref(xs->as.cons.head), rest);
}

static KaiValue *kai_prelude_list_reverse(KaiValue *xs) {
    KaiValue *acc = kai_nil();
    KaiValue *p   = xs;
    while (p && p->tag == KAI_CONS) {
        acc = kai_cons(kai_incref(p->as.cons.head), acc);
        p   = p->as.cons.tail;
    }
    return acc;
}

/* ---------- prelude: higher order ---------- */

static KaiValue *kai_prelude_map(KaiValue *xs, KaiValue *f) {
    if (!xs || xs->tag == KAI_NIL) return kai_nil();
    KaiValue *arg0 = xs->as.cons.head;
    KaiValue *head = kai_apply(f, 1, &arg0);
    KaiValue *rest = kai_prelude_map(xs->as.cons.tail, f);
    return kai_cons(head, rest);
}

static KaiValue *kai_prelude_filter(KaiValue *xs, KaiValue *p) {
    if (!xs || xs->tag == KAI_NIL) return kai_nil();
    KaiValue *arg0 = xs->as.cons.head;
    KaiValue *keep = kai_apply(p, 1, &arg0);
    int yes = kai_truthy(keep);
    kai_decref(keep);
    KaiValue *rest = kai_prelude_filter(xs->as.cons.tail, p);
    if (yes) return kai_cons(kai_incref(xs->as.cons.head), rest);
    return rest;
}

static KaiValue *kai_prelude_reduce(KaiValue *xs, KaiValue *init, KaiValue *f) {
    KaiValue *acc = kai_incref(init);
    KaiValue *p   = xs;
    while (p && p->tag == KAI_CONS) {
        KaiValue *args[2];
        args[0] = acc;
        args[1] = p->as.cons.head;
        KaiValue *next = kai_apply(f, 2, args);
        kai_decref(acc);
        acc = next;
        p = p->as.cons.tail;
    }
    return acc;
}

static KaiValue *kai_prelude_each(KaiValue *xs, KaiValue *f) {
    KaiValue *p = xs;
    while (p && p->tag == KAI_CONS) {
        KaiValue *arg0 = p->as.cons.head;
        KaiValue *r = kai_apply(f, 1, &arg0);
        kai_decref(r);
        p = p->as.cons.tail;
    }
    return kai_unit();
}

/* ---------- binary and unary operators ---------- */

static KaiValue *kai_add(KaiValue *a, KaiValue *b) {
    if (a->tag == KAI_INT  && b->tag == KAI_INT)  return kai_int(a->as.i + b->as.i);
    if (a->tag == KAI_REAL && b->tag == KAI_REAL) return kai_real(a->as.r + b->as.r);
    fprintf(stderr, "kai: type mismatch in +\n"); exit(1);
}

static KaiValue *kai_sub(KaiValue *a, KaiValue *b) {
    if (a->tag == KAI_INT  && b->tag == KAI_INT)  return kai_int(a->as.i - b->as.i);
    if (a->tag == KAI_REAL && b->tag == KAI_REAL) return kai_real(a->as.r - b->as.r);
    fprintf(stderr, "kai: type mismatch in -\n"); exit(1);
}

static KaiValue *kai_mul(KaiValue *a, KaiValue *b) {
    if (a->tag == KAI_INT  && b->tag == KAI_INT)  return kai_int(a->as.i * b->as.i);
    if (a->tag == KAI_REAL && b->tag == KAI_REAL) return kai_real(a->as.r * b->as.r);
    fprintf(stderr, "kai: type mismatch in *\n"); exit(1);
}

static KaiValue *kai_div(KaiValue *a, KaiValue *b) {
    if (a->tag == KAI_INT && b->tag == KAI_INT) {
        if (b->as.i == 0) { fprintf(stderr, "kai: divide by zero\n"); exit(1); }
        return kai_int(a->as.i / b->as.i);
    }
    if (a->tag == KAI_REAL && b->tag == KAI_REAL) return kai_real(a->as.r / b->as.r);
    fprintf(stderr, "kai: type mismatch in /\n"); exit(1);
}

static KaiValue *kai_idiv(KaiValue *a, KaiValue *b) {
    int64_t av = 0, bv = 0;
    if      (a->tag == KAI_INT)  av = a->as.i;
    else if (a->tag == KAI_REAL) av = (int64_t) a->as.r;
    else { fprintf(stderr, "kai: type mismatch in //\n"); exit(1); }
    if      (b->tag == KAI_INT)  bv = b->as.i;
    else if (b->tag == KAI_REAL) bv = (int64_t) b->as.r;
    else { fprintf(stderr, "kai: type mismatch in //\n"); exit(1); }
    if (bv == 0) { fprintf(stderr, "kai: divide by zero\n"); exit(1); }
    return kai_int(av / bv);
}

static KaiValue *kai_mod(KaiValue *a, KaiValue *b) {
    if (a->tag == KAI_INT && b->tag == KAI_INT) {
        if (b->as.i == 0) { fprintf(stderr, "kai: mod by zero\n"); exit(1); }
        return kai_int(a->as.i % b->as.i);
    }
    fprintf(stderr, "kai: type mismatch in %%\n"); exit(1);
}

static KaiValue *kai_lt(KaiValue *a, KaiValue *b) {
    if (a->tag == KAI_INT  && b->tag == KAI_INT)  return kai_bool(a->as.i < b->as.i);
    if (a->tag == KAI_REAL && b->tag == KAI_REAL) return kai_bool(a->as.r < b->as.r);
    if (a->tag == KAI_STR  && b->tag == KAI_STR) {
        size_t n = a->as.s.len < b->as.s.len ? a->as.s.len : b->as.s.len;
        int c = memcmp(a->as.s.bytes, b->as.s.bytes, n);
        if (c != 0) return kai_bool(c < 0);
        return kai_bool(a->as.s.len < b->as.s.len);
    }
    fprintf(stderr, "kai: type mismatch in <\n"); exit(1);
}

static KaiValue *kai_gt(KaiValue *a, KaiValue *b) {
    if (a->tag == KAI_INT  && b->tag == KAI_INT)  return kai_bool(a->as.i > b->as.i);
    if (a->tag == KAI_REAL && b->tag == KAI_REAL) return kai_bool(a->as.r > b->as.r);
    if (a->tag == KAI_STR  && b->tag == KAI_STR) {
        size_t n = a->as.s.len < b->as.s.len ? a->as.s.len : b->as.s.len;
        int c = memcmp(a->as.s.bytes, b->as.s.bytes, n);
        if (c != 0) return kai_bool(c > 0);
        return kai_bool(a->as.s.len > b->as.s.len);
    }
    fprintf(stderr, "kai: type mismatch in >\n"); exit(1);
}

static KaiValue *kai_le(KaiValue *a, KaiValue *b) {
    KaiValue *g = kai_gt(a, b);
    KaiValue *r = kai_bool(!g->as.b);
    kai_decref(g);
    return r;
}

static KaiValue *kai_ge(KaiValue *a, KaiValue *b) {
    KaiValue *l = kai_lt(a, b);
    KaiValue *r = kai_bool(!l->as.b);
    kai_decref(l);
    return r;
}

static KaiValue *kai_eq_v(KaiValue *a, KaiValue *b) { return kai_bool(kai_eq(a, b)); }
static KaiValue *kai_ne_v(KaiValue *a, KaiValue *b) { return kai_bool(!kai_eq(a, b)); }

static KaiValue *kai_neg(KaiValue *a) {
    if (a->tag == KAI_INT)  return kai_int(-a->as.i);
    if (a->tag == KAI_REAL) return kai_real(-a->as.r);
    fprintf(stderr, "kai: type mismatch in unary -\n"); exit(1);
}

static KaiValue *kai_boolnot(KaiValue *a) {
    if (a->tag == KAI_BOOL) return kai_bool(!a->as.b);
    fprintf(stderr, "kai: type mismatch in `not`\n"); exit(1);
}

static int kai_truthy(KaiValue *v) {
    return v && v->tag == KAI_BOOL && v->as.b;
}

/* ---------- range construction ---------- */

static KaiValue *kai_range(KaiValue *from, KaiValue *to) {
    int64_t f = from->as.i, t = to->as.i;
    KaiValue *acc = kai_nil();
    for (int64_t i = t; i >= f; --i) acc = kai_cons(kai_int(i), acc);
    return acc;
}

static KaiValue *kai_range_step(KaiValue *from, KaiValue *to, KaiValue *step) {
    int64_t f = from->as.i, t = to->as.i, s = step->as.i;
    if (s == 0) { fprintf(stderr, "kai: zero step in range\n"); exit(1); }
    int64_t *buf = NULL;
    size_t cap = 0, n = 0;
    if (s > 0) {
        for (int64_t i = f; i <= t; i += s) {
            if (n == cap) { cap = cap ? cap * 2 : 16; buf = (int64_t *) realloc(buf, cap * sizeof(int64_t)); }
            buf[n++] = i;
        }
    } else {
        for (int64_t i = f; i >= t; i += s) {
            if (n == cap) { cap = cap ? cap * 2 : 16; buf = (int64_t *) realloc(buf, cap * sizeof(int64_t)); }
            buf[n++] = i;
        }
    }
    KaiValue *acc = kai_nil();
    for (size_t i = n; i > 0;) { --i; acc = kai_cons(kai_int(buf[i]), acc); }
    free(buf);
    return acc;
}

/* ---------- prelude thunks for first-class function refs ---------- */

static KaiValue *_kai_prelude_print_thunk(KaiValue *s, KaiValue **a, int n)          { (void) s; (void) n; return kai_prelude_print(a[0]); }
static KaiValue *_kai_prelude_eprint_thunk(KaiValue *s, KaiValue **a, int n)         { (void) s; (void) n; return kai_prelude_eprint(a[0]); }
static KaiValue *_kai_prelude_panic_thunk(KaiValue *s, KaiValue **a, int n)          { (void) s; (void) n; return kai_prelude_panic(a[0]); }
static KaiValue *_kai_prelude_exit_thunk(KaiValue *s, KaiValue **a, int n)           { (void) s; (void) n; return kai_prelude_exit(a[0]); }
static KaiValue *_kai_prelude_int_to_string_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_prelude_int_to_string(a[0]); }
static KaiValue *_kai_prelude_real_to_string_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_prelude_real_to_string(a[0]); }
static KaiValue *_kai_prelude_string_length_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_prelude_string_length(a[0]); }
static KaiValue *_kai_prelude_string_concat_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_prelude_string_concat(a[0], a[1]); }
static KaiValue *_kai_prelude_list_length_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_list_length(a[0]); }
static KaiValue *_kai_prelude_list_append_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_list_append(a[0], a[1]); }
static KaiValue *_kai_prelude_list_reverse_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_prelude_list_reverse(a[0]); }
static KaiValue *_kai_prelude_map_thunk(KaiValue *s, KaiValue **a, int n)            { (void) s; (void) n; return kai_prelude_map(a[0], a[1]); }
static KaiValue *_kai_prelude_filter_thunk(KaiValue *s, KaiValue **a, int n)         { (void) s; (void) n; return kai_prelude_filter(a[0], a[1]); }
static KaiValue *_kai_prelude_reduce_thunk(KaiValue *s, KaiValue **a, int n)         { (void) s; (void) n; return kai_prelude_reduce(a[0], a[1], a[2]); }
static KaiValue *_kai_prelude_each_thunk(KaiValue *s, KaiValue **a, int n)           { (void) s; (void) n; return kai_prelude_each(a[0], a[1]); }

/* ---------- test harness hooks (used by --test runs) ---------- */

static int kai_test_count_total   = 0;
static int kai_test_count_passed  = 0;

static void kai_test_begin(const char *desc) {
    (void) desc;
    kai_test_count_total++;
}

static void kai_test_pass(void) {
    kai_test_count_passed++;
}

static void kai_test_fail(const char *desc, const char *msg) {
    fprintf(stderr, "  FAIL: %s\n    %s\n", desc, msg ? msg : "assertion failed");
}

static int kai_test_summary(void) {
    fprintf(stderr, "%d/%d tests passed\n", kai_test_count_passed, kai_test_count_total);
    return (kai_test_count_passed == kai_test_count_total) ? 0 : 1;
}

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

#endif /* KAI_RUNTIME_H */
