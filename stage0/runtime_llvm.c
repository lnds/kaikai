/*
 * LLVM-backend shim over the header-only kaikai-minimal runtime.
 *
 * `runtime.h` defines the whole runtime as `static inline` so the
 * stage 0 / stage 1 / stage 2 C backends can emit one big translation
 * unit and not worry about duplicate-symbol linkage. That trick does
 * not work for the LLVM backend: `llc` produces an object file that
 * needs externally linkable runtime symbols, and `static` functions
 * live inside a different TU with no visible name.
 *
 * This file is that visible name. It includes the header once, then
 * re-exports each runtime entry point the LLVM backend calls into as
 * a thin `kaix_*` wrapper. It also provides `main`, wired to the
 * `kai_main` symbol the LLVM output defines.
 *
 * Link line:
 *   clang -I stage0 foo.ll stage0/runtime_llvm.c -o foo
 */

#include "runtime.h"

/* ---------- value constructors ---------- */
KaiValue *kaix_str(const char *s)              { return kai_str(s); }
KaiValue *kaix_int(int64_t i)                  { return kai_int(i); }
KaiValue *kaix_bool(int b)                     { return kai_bool(b); }

/* ---------- binops ---------- */
KaiValue *kaix_add(KaiValue *a, KaiValue *b)   { return kai_add(a, b); }
KaiValue *kaix_sub(KaiValue *a, KaiValue *b)   { return kai_sub(a, b); }
KaiValue *kaix_mul(KaiValue *a, KaiValue *b)   { return kai_mul(a, b); }
KaiValue *kaix_div(KaiValue *a, KaiValue *b)   { return kai_div(a, b); }
KaiValue *kaix_idiv(KaiValue *a, KaiValue *b)  { return kai_idiv(a, b); }
KaiValue *kaix_mod(KaiValue *a, KaiValue *b)   { return kai_mod(a, b); }
KaiValue *kaix_eq(KaiValue *a, KaiValue *b)    { return kai_eq_v(a, b); }
KaiValue *kaix_ne(KaiValue *a, KaiValue *b)    { return kai_ne_v(a, b); }
KaiValue *kaix_lt(KaiValue *a, KaiValue *b)    { return kai_lt(a, b); }
KaiValue *kaix_gt(KaiValue *a, KaiValue *b)    { return kai_gt(a, b); }
KaiValue *kaix_le(KaiValue *a, KaiValue *b)    { return kai_le(a, b); }
KaiValue *kaix_ge(KaiValue *a, KaiValue *b)    { return kai_ge(a, b); }
KaiValue *kaix_neg(KaiValue *a)                { return kai_neg(a); }
KaiValue *kaix_not(KaiValue *a)                { return kai_bool(!kai_truthy(a)); }

/* ---------- control helpers ---------- */
int kaix_truthy(KaiValue *v)                   { return kai_truthy(v); }

/* ---------- prelude subset used by M3b ---------- */
KaiValue *kaix_prelude_print(KaiValue *v)          { return kai_prelude_print(v); }
KaiValue *kaix_prelude_int_to_string(KaiValue *v)  { return kai_prelude_int_to_string(v); }

/* ---------- M3c: strings, ranges, higher-order prelude ---------- */
KaiValue *kaix_string_concat(KaiValue *a, KaiValue *b)         { return kai_string_concat(a, b); }
KaiValue *kaix_to_string(KaiValue *v)                           { return kai_to_string(v); }

KaiValue *kaix_range(KaiValue *from, KaiValue *to)              { return kai_range(from, to); }
KaiValue *kaix_range_step(KaiValue *f, KaiValue *t, KaiValue *s){ return kai_range_step(f, t, s); }

KaiValue *kaix_prelude_map(KaiValue *xs, KaiValue *f)          { return kai_prelude_map(xs, f); }
KaiValue *kaix_prelude_each(KaiValue *xs, KaiValue *f)         { return kai_prelude_each(xs, f); }
KaiValue *kaix_prelude_filter(KaiValue *xs, KaiValue *p)       { return kai_prelude_filter(xs, p); }
KaiValue *kaix_prelude_reduce(KaiValue *xs, KaiValue *i, KaiValue *f) { return kai_prelude_reduce(xs, i, f); }

/* Closure construction. Accepts a KaiFn-compatible function pointer
   (passed as a void* from the LLVM IR for opaque-pointer mode). */
KaiValue *kaix_closure(KaiFn fn, int arity, int n_captures, KaiValue **captures) {
    return kai_closure(fn, arity, n_captures, captures);
}

/* Prelude thunks exported as regular symbols so LLVM IR can take
   their address to build closures passed into higher-order prelude
   calls. One per prelude entry we may reference as a value. */
KaiValue *kaix_print_thunk(KaiValue *s, KaiValue **a, int n)            { (void)s; (void)n; return kai_prelude_print(a[0]); }
KaiValue *kaix_eprint_thunk(KaiValue *s, KaiValue **a, int n)           { (void)s; (void)n; return kai_prelude_eprint(a[0]); }
KaiValue *kaix_int_to_string_thunk(KaiValue *s, KaiValue **a, int n)    { (void)s; (void)n; return kai_prelude_int_to_string(a[0]); }

/* ---------- M3d: variants + match ---------- */
KaiValue *kaix_variant(int32_t tag, const char *name, int32_t n, KaiValue **args) {
    return kai_variant(tag, name, n, args);
}

/* 1 iff `v` is a KAI_VARIANT whose tag name matches `name`. Used as
   the discriminator in match arms. */
int kaix_is_variant(KaiValue *v, const char *name) {
    return v && v->tag == KAI_VARIANT && strcmp(v->as.var.variant_name, name) == 0;
}

/* Read the i-th argument of a variant. incref so the caller owns it;
   the C backend passes variant args as borrowed references, but the
   LLVM path keeps ownership uniform. */
KaiValue *kaix_variant_arg(KaiValue *v, int i) {
    return kai_incref(v->as.var.args[i]);
}

/* kai_eq returns an int; wrap for direct use from the IR in match
   guards or literal patterns that want a KaiValue* Bool. */
KaiValue *kaix_eq_raw(KaiValue *a, KaiValue *b) { return kai_bool(kai_eq(a, b)); }

/* Panic with a message. The LLVM match lowering calls this in the
   fall-through block of the last arm when no pattern matches, so the
   generated IR then drops into `unreachable`. */
KaiValue *kaix_panic(KaiValue *msg) { return kai_prelude_panic(msg); }

/* Entry point: the LLVM output defines kai_main. Match what the C
   backend's emit_main_wrapper does. */
extern KaiValue *kai_main(void);

int main(int argc, char **argv) {
    kai_set_args(argc, argv);
    KaiValue *result = kai_main();
    kai_decref(result);
    return 0;
}
