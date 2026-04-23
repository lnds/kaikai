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
KaiValue *kaix_prelude_eprint(KaiValue *v)         { return kai_prelude_eprint(v); }
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

/* Indirect call for a closure value held in a local binding. The LLVM
   backend emits this when a ECall's head is a fn-typed parameter or
   let binding — we can't lower it as a direct `call @kai_<name>`
   because the name resolves to no global symbol. kai_apply checks the
   tag and dispatches to clo->as.clo.fn(clo, args, n). */
KaiValue *kaix_apply(KaiValue *clo, int32_t n, KaiValue **args) {
    return kai_apply(clo, n, args);
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
   generated IR then drops into `unreachable`. Named distinctly from
   `kaix_prelude_panic` below to keep the prelude-dispatch symbol
   free for user calls to the `panic` prelude fn. */
KaiValue *kaix_match_panic(KaiValue *msg) { return kai_prelude_panic(msg); }

KaiValue *kaix_prelude_panic(KaiValue *msg) { return kai_prelude_panic(msg); }

/* ---------- M3e: lists + closures-with-captures ---------- */
KaiValue *kaix_cons(KaiValue *h, KaiValue *t)            { return kai_cons(h, t); }
KaiValue *kaix_nil(void)                                  { return kai_nil(); }
int       kaix_is_cons(KaiValue *v)                       { return v && v->tag == KAI_CONS; }
int       kaix_is_nil(KaiValue *v)                        { return !v || v->tag == KAI_NIL; }
KaiValue *kaix_cons_head(KaiValue *v)                     { return kai_incref(v->as.cons.head); }
KaiValue *kaix_cons_tail(KaiValue *v)                     { return kai_incref(v->as.cons.tail); }

/* Used by lambda thunks to read their captured values from the
   closure's self parameter. i is the capture's index. */
KaiValue *kaix_capture(KaiValue *self, int i)             { return kai_incref(self->as.clo.captures[i]); }

KaiValue *kaix_prelude_list_length(KaiValue *xs)          { return kai_prelude_list_length(xs); }
KaiValue *kaix_prelude_list_append(KaiValue *a, KaiValue *b) { return kai_prelude_list_append(a, b); }
KaiValue *kaix_prelude_list_reverse(KaiValue *xs)         { return kai_prelude_list_reverse(xs); }

/* ---------- M12: chars, reals, records, fields ---------- */
KaiValue *kaix_char(uint32_t c)                           { return kai_char(c); }
KaiValue *kaix_real(double r)                             { return kai_real(r); }
KaiValue *kaix_unit(void)                                 { return kai_unit(); }

/* Record construction: fields[i] carries the value for names[i].
   `names` must point to string literals with program lifetime —
   the runtime stores them by reference and does not free them. */
KaiValue *kaix_record(int n, KaiValue **fields, const char **names) {
    return kai_record(n, fields, names);
}

KaiValue *kaix_field(KaiValue *rec, const char *name)     { return kai_field(rec, name); }

/* Full prelude set — anything the compiler (stage 2's own source)
   calls directly when compiled through the LLVM backend. */
KaiValue *kaix_prelude_args(void)                           { return kai_prelude_args(); }
KaiValue *kaix_prelude_exit(KaiValue *v)                    { return kai_prelude_exit(v); }
/* kaix_prelude_panic is defined above near kaix_match_panic. */
KaiValue *kaix_prelude_read_file(KaiValue *p)               { return kai_prelude_read_file(p); }
KaiValue *kaix_prelude_write_file(KaiValue *p, KaiValue *c) { return kai_prelude_write_file(p, c); }
KaiValue *kaix_prelude_read_line(void)                      { return kai_prelude_read_line(); }
KaiValue *kaix_prelude_real_to_string(KaiValue *v)          { return kai_prelude_real_to_string(v); }
KaiValue *kaix_prelude_string_to_int(KaiValue *s)           { return kai_prelude_string_to_int(s); }
KaiValue *kaix_prelude_string_to_real(KaiValue *s)          { return kai_prelude_string_to_real(s); }
KaiValue *kaix_prelude_string_length(KaiValue *s)           { return kai_prelude_string_length(s); }
KaiValue *kaix_prelude_string_concat(KaiValue *a, KaiValue *b) { return kai_prelude_string_concat(a, b); }
KaiValue *kaix_prelude_string_concat_all(KaiValue *xs)         { return kai_prelude_string_concat_all(xs); }
KaiValue *kaix_prelude_string_join(KaiValue *xs, KaiValue *sep){ return kai_prelude_string_join(xs, sep); }
KaiValue *kaix_prelude_string_slice(KaiValue *s, KaiValue *i, KaiValue *n) { return kai_prelude_string_slice(s, i, n); }
KaiValue *kaix_prelude_string_split(KaiValue *s, KaiValue *d)  { return kai_prelude_string_split(s, d); }
KaiValue *kaix_prelude_string_contains(KaiValue *s, KaiValue *sub) { return kai_prelude_string_contains(s, sub); }
KaiValue *kaix_prelude_char_at(KaiValue *s, KaiValue *i)       { return kai_prelude_char_at(s, i); }
KaiValue *kaix_prelude_char_to_int(KaiValue *c)                { return kai_prelude_char_to_int(c); }
KaiValue *kaix_prelude_int_to_char(KaiValue *i)                { return kai_prelude_int_to_char(i); }
KaiValue *kaix_prelude_array_make(KaiValue *n, KaiValue *init)           { return kai_prelude_array_make(n, init); }
KaiValue *kaix_prelude_array_length(KaiValue *a)                         { return kai_prelude_array_length(a); }
KaiValue *kaix_prelude_array_get(KaiValue *a, KaiValue *i)               { return kai_prelude_array_get(a, i); }
KaiValue *kaix_prelude_array_set(KaiValue *a, KaiValue *i, KaiValue *v)  { return kai_prelude_array_set(a, i, v); }
KaiValue *kaix_prelude_array_grow(KaiValue *a, KaiValue *n, KaiValue *init) { return kai_prelude_array_grow(a, n, init); }

/* Entry point: the LLVM output defines kai_main. Match what the C
   backend's emit_main_wrapper does. */
extern KaiValue *kai_main(void);

int main(int argc, char **argv) {
    kai_set_args(argc, argv);
    KaiValue *result = kai_main();
    kai_decref(result);
    return 0;
}
