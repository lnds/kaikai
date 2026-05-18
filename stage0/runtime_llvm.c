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
/* Lane 4 (#473) Byte literal constructor. Mirrors `kaix_int` but
 * for the nominal `Byte` (KAI_BYTE) tag — needed when the LLVM
 * EInt emit targets `[Byte]` literal context (`let bs: [Byte] =
 * [65]`) and emits `@kaix_byte(i8 65)` instead of `kaix_int`. */
KaiValue *kaix_byte(uint8_t b)                 { return kai_byte(b); }
KaiValue *kaix_bool(int b)                     { return kai_bool(b); }

/* ---------- binops ---------- */
KaiValue *kaix_add(KaiValue *a, KaiValue *b)   { return kai_op_add(a, b); }
KaiValue *kaix_sub(KaiValue *a, KaiValue *b)   { return kai_op_sub(a, b); }
KaiValue *kaix_mul(KaiValue *a, KaiValue *b)   { return kai_op_mul(a, b); }
KaiValue *kaix_div(KaiValue *a, KaiValue *b)   { return kai_op_div(a, b); }
KaiValue *kaix_idiv(KaiValue *a, KaiValue *b)  { return kai_op_idiv(a, b); }
KaiValue *kaix_mod(KaiValue *a, KaiValue *b)   { return kai_op_mod(a, b); }
KaiValue *kaix_eq(KaiValue *a, KaiValue *b)    { return kai_op_eq_v(a, b); }
KaiValue *kaix_ne(KaiValue *a, KaiValue *b)    { return kai_op_ne_v(a, b); }
KaiValue *kaix_lt(KaiValue *a, KaiValue *b)    { return kai_op_lt(a, b); }
KaiValue *kaix_gt(KaiValue *a, KaiValue *b)    { return kai_op_gt(a, b); }
KaiValue *kaix_le(KaiValue *a, KaiValue *b)    { return kai_op_le(a, b); }
KaiValue *kaix_ge(KaiValue *a, KaiValue *b)    { return kai_op_ge(a, b); }
KaiValue *kaix_neg(KaiValue *a)                { return kai_op_neg(a); }
KaiValue *kaix_pow_int(KaiValue *a, KaiValue *b) { return kai_op_pow_int(a, b); }
/* Route through kai_op_boolnot so the LLVM backend's `not` matches the
 * C backend's `!` operator: same type-check + consuming semantics. */
KaiValue *kaix_not(KaiValue *a)                { return kai_op_boolnot(a); }

/* ---------- control helpers ---------- */
int kaix_truthy(KaiValue *v)                   { return kai_op_truthy(v); }

/* ---------- m13 bit ops ----------
 * The C backend (stage2/compiler.kai emit_call_expr ~line 12151)
 * lowers `bit_and(a, b)` etc. to an inline GNU statement-expression
 * that reads `_a->as.i & _b->as.i`, boxes via `kai_int`, and decrefs
 * the operands. The LLVM backend cannot use statement-expressions,
 * so these mirror wrappers do the same operation in a stable
 * external symbol callable from IR. Caller hands us owned refs and
 * must release them exactly once — we do that here, matching the C
 * path's refcount discipline. */
KaiValue *kaix_bit_and(KaiValue *a, KaiValue *b) {
    KaiValue *r = kai_int(a->as.i & b->as.i);
    kai_decref(a); kai_decref(b);
    return r;
}
KaiValue *kaix_bit_or(KaiValue *a, KaiValue *b) {
    KaiValue *r = kai_int(a->as.i | b->as.i);
    kai_decref(a); kai_decref(b);
    return r;
}
KaiValue *kaix_bit_xor(KaiValue *a, KaiValue *b) {
    KaiValue *r = kai_int(a->as.i ^ b->as.i);
    kai_decref(a); kai_decref(b);
    return r;
}
KaiValue *kaix_bit_not(KaiValue *a) {
    KaiValue *r = kai_int(~ a->as.i);
    kai_decref(a);
    return r;
}
KaiValue *kaix_bit_shl(KaiValue *a, KaiValue *b) {
    KaiValue *r = kai_int(a->as.i << b->as.i);
    kai_decref(a); kai_decref(b);
    return r;
}
KaiValue *kaix_bit_shr(KaiValue *a, KaiValue *b) {
    /* Arithmetic shift: signed `>>` preserves the sign bit. */
    KaiValue *r = kai_int(a->as.i >> b->as.i);
    kai_decref(a); kai_decref(b);
    return r;
}
KaiValue *kaix_bit_ushr(KaiValue *a, KaiValue *b) {
    /* Logical shift: cast through uint64_t to zero-fill. */
    KaiValue *r = kai_int((int64_t)(((uint64_t) a->as.i) >> b->as.i));
    kai_decref(a); kai_decref(b);
    return r;
}
KaiValue *kaix_bit_count(KaiValue *a) {
    KaiValue *r = kai_int((int64_t) __builtin_popcountll((uint64_t) a->as.i));
    kai_decref(a);
    return r;
}
KaiValue *kaix_bit_test(KaiValue *a, KaiValue *b) {
    KaiValue *r = kai_bool(((a->as.i >> b->as.i) & 1) != 0);
    kai_decref(a); kai_decref(b);
    return r;
}
KaiValue *kaix_bit_set(KaiValue *a, KaiValue *b) {
    KaiValue *r = kai_int(a->as.i | ((int64_t)1 << b->as.i));
    kai_decref(a); kai_decref(b);
    return r;
}
KaiValue *kaix_bit_clear(KaiValue *a, KaiValue *b) {
    KaiValue *r = kai_int(a->as.i & ~((int64_t)1 << b->as.i));
    kai_decref(a); kai_decref(b);
    return r;
}
KaiValue *kaix_bit_toggle(KaiValue *a, KaiValue *b) {
    KaiValue *r = kai_int(a->as.i ^ ((int64_t)1 << b->as.i));
    kai_decref(a); kai_decref(b);
    return r;
}

/* ---------- issue #87 — Phase 2 unbox boundary readers ----------
 * Mirror of the C backend's `(boxed)->as.i` / `->as.b` / `->as.r` /
 * `->as.c` field reads used by `emit_expr_raw`'s MBoxed boundary
 * tactic (docs/unboxing-phase2-design.md §3). Borrowing readers —
 * they do NOT decref `v` — because the C backend's `->as.*` is also
 * a borrow. Caller is responsible for the boxed value's lifetime.
 * Used at the MBoxed → MUnboxed boundary inside the LLVM emitter
 * (raw operand of an MUnboxed binop, raw scrutinee of a switch). */
int64_t  kaix_to_int(KaiValue *v)              { return v->as.i; }
int      kaix_to_bool(KaiValue *v)             { return v->as.b; }
double   kaix_to_real(KaiValue *v)             { return v->as.r; }
/* m12.7.x FFI v1: read the raw C-string pointer out of a boxed
 * KaiValue. Used by the LLVM-side FFI shim to forward `String`
 * params straight to the extern. The pointer aliases the boxed
 * value's storage, so the caller must not free it. */
const char *kaix_to_str_data(KaiValue *v)      { return v->as.s.bytes; }
uint32_t kaix_to_char(KaiValue *v)             { return v->as.c; }

/* ---------- m5 #4 — Perceus dup/drop wrappers for LLVM backend ---------- */
KaiValue *kaix_internal_dup(KaiValue *v)       { return kai_internal_dup(v); }
KaiValue *kaix_internal_drop(KaiValue *v)      { return kai_internal_drop(v); }

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
KaiValue *kaix_prelude_flat_map(KaiValue *xs, KaiValue *f)     { return kai_prelude_flat_map(xs, f); }
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
    /* Issue #440 — variant slot. Phase 1: every slot is a pointer
     * (mask=0), so `.ptr` is identical to the pre-#440 `args[i]`. */
    return kai_incref(v->as.var.slots[i].ptr);
}

/* kai_op_eq returns an int; wrap for direct use from the IR in match
   guards or literal patterns that want a KaiValue* Bool. */
KaiValue *kaix_eq_raw(KaiValue *a, KaiValue *b) { return kai_bool(kai_op_eq(a, b)); }

/* Panic with a message. The LLVM match lowering calls this in the
   fall-through block of the last arm when no pattern matches, so the
   generated IR then drops into `unreachable`. Named distinctly from
   `kaix_prelude_panic` below to keep the prelude-dispatch symbol
   free for user calls to the `panic` prelude fn. */
KaiValue *kaix_match_panic(KaiValue *msg) { return kai_prelude_panic(msg); }

KaiValue *kaix_prelude_panic(KaiValue *msg) { return kai_prelude_panic(msg); }

/* ---------- refcounting ---------- */
/* Used by the LLVM backend's tail-spread fast path: `[x, ...xs]` emits
   kai_cons(x, kaix_incref(xs)) instead of the quadratic
   list_append(xs, nil()). */
KaiValue *kaix_incref(KaiValue *v)                        { return kai_incref(v); }

/* m5 #3 LLVM mirror: drop unused fresh-alloc let-bindings. The IR
   emits `call void @kaix_decref(%KaiValue* <reg>)` immediately after
   the binding when last-use analysis proves the name is unread. */
void      kaix_decref(KaiValue *v)                        { kai_decref(v); }

/* ---------- M3e: lists + closures-with-captures ---------- */
KaiValue *kaix_cons(KaiValue *h, KaiValue *t)            { return kai_cons(h, t); }
KaiValue *kaix_nil(void)                                  { return kai_nil(); }
int       kaix_is_cons(KaiValue *v)                       { return v && v->tag == KAI_CONS; }
int       kaix_is_nil(KaiValue *v)                        { return !v || v->tag == KAI_NIL; }
KaiValue *kaix_cons_head(KaiValue *v)                     { return kai_incref(v->as.cons.head); }
KaiValue *kaix_cons_tail(KaiValue *v)                     { return kai_incref(v->as.cons.tail); }

/* issue #118 — LLVM mirror of the C-side reuse-in-place runtime.
 * Stage 2's recogniser emits `__perceus_reuse_cons` calls inside
 * match arms; both backends lower to the same `kai_reuse_or_alloc_cons`
 * helper, so the shape mirrors the existing `kaix_cons` thin wrapper. */
KaiValue *kaix_reuse_or_alloc_cons(KaiValue *scr, KaiValue *h, KaiValue *t) {
  return kai_reuse_or_alloc_cons(scr, h, t);
}

/* issue #210 — record + variant reuse-in-place. Same shape as the
 * cons wrapper: the LLVM emit synthesises a call with the same
 * argument layout as `kai_record` / `kai_variant`, with the consumed
 * scrutinee as the leading parameter. */
KaiValue *kaix_reuse_or_alloc_record(KaiValue *scr, int n,
                                     KaiValue **fields, const char **names) {
  return kai_reuse_or_alloc_record(scr, n, fields, names);
}
KaiValue *kaix_reuse_or_alloc_variant(KaiValue *scr, int32_t tag,
                                      const char *name, int n, KaiValue **args) {
  return kai_reuse_or_alloc_variant(scr, tag, name, n, args);
}

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

KaiValue *kaix_field(KaiValue *rec, const char *name)        { return kai_op_field(rec, name); }
KaiValue *kaix_field_borrow(KaiValue *rec, const char *name) { return kai_op_field_borrow(rec, name); }

/* Full prelude set — anything the compiler (stage 2's own source)
   calls directly when compiled through the LLVM backend. */
KaiValue *kaix_prelude_args(void)                           { return kai_prelude_args(); }
KaiValue *kaix_prelude_program_name(void)                   { return kai_prelude_program_name(); }
KaiValue *kaix_prelude_stdlib_path(void)                    { return kai_prelude_stdlib_path(); }
KaiValue *kaix_prelude_abspath(KaiValue *p)                 { return kai_prelude_abspath(p); }
KaiValue *kaix_prelude_exit(KaiValue *v)                    { return kai_prelude_exit(v); }
/* kaix_prelude_panic is defined above near kaix_match_panic. */
KaiValue *kaix_prelude_read_file(KaiValue *p)               { return kai_prelude_read_file(p); }
KaiValue *kaix_prelude_write_file(KaiValue *p, KaiValue *c) { return kai_prelude_write_file(p, c); }
KaiValue *kaix_prelude_file_exists(KaiValue *p)             { return kai_prelude_file_exists(p); }
KaiValue *kaix_prelude_file_delete(KaiValue *p)             { return kai_prelude_file_delete(p); }
KaiValue *kaix_prelude_file_rename(KaiValue *f, KaiValue *t){ return kai_prelude_file_rename(f, t); }
/* Issue #513: Array[Byte] file round-trip. Same shape as the
 * exists/delete/rename wrappers above — the C-side statics live
 * in runtime.h; the LLVM emit references the `kaix_` names. */
KaiValue *kaix_prelude_file_read_bytes(KaiValue *p)              { return kai_prelude_file_read_bytes(p); }
KaiValue *kaix_prelude_file_write_bytes(KaiValue *p, KaiValue *b){ return kai_prelude_file_write_bytes(p, b); }
KaiValue *kaix_prelude_dir_list_dir(KaiValue *p)            { return kai_prelude_dir_list_dir(p); }
KaiValue *kaix_prelude_dir_create_dir(KaiValue *p)          { return kai_prelude_dir_create_dir(p); }
KaiValue *kaix_prelude_dir_remove_dir(KaiValue *p)          { return kai_prelude_dir_remove_dir(p); }
KaiValue *kaix_prelude_dir_walk(KaiValue *p)                { return kai_prelude_dir_walk(p); }
KaiValue *kaix_prelude_read_line(void)                      { return kai_prelude_read_line(); }
KaiValue *kaix_prelude_read_bytes(KaiValue *n)              { return kai_prelude_read_bytes(n); }
KaiValue *kaix_prelude_real_to_string(KaiValue *v)          { return kai_prelude_real_to_string(v); }
KaiValue *kaix_prelude_int_to_real(KaiValue *v)             { return kai_prelude_int_to_real(v); }
KaiValue *kaix_prelude_real_to_int(KaiValue *v)             { return kai_prelude_real_to_int(v); }
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
KaiValue *kaix_prelude_int_to_byte_string(KaiValue *i)         { return kai_prelude_int_to_byte_string(i); }
KaiValue *kaix_prelude_string_byte_at_int(KaiValue *s, KaiValue *i) { return kai_prelude_string_byte_at_int(s, i); }
KaiValue *kaix_prelude_array_make(KaiValue *n, KaiValue *init)           { return kai_prelude_array_make(n, init); }
KaiValue *kaix_prelude_array_length(KaiValue *a)                         { return kai_prelude_array_length(a); }
KaiValue *kaix_prelude_array_get(KaiValue *a, KaiValue *i)               { return kai_prelude_array_get(a, i); }
KaiValue *kaix_prelude_array_set(KaiValue *a, KaiValue *i, KaiValue *v)  { return kai_prelude_array_set(a, i, v); }
KaiValue *kaix_prelude_array_grow(KaiValue *a, KaiValue *n, KaiValue *init) { return kai_prelude_array_grow(a, n, init); }
/* Issue #364: `impl Rem for Real` in stdlib/protocols.kai delegates
 * to this libm fmod binding. Listed in the LLVM prelude table so the
 * monomorphised __pimpl_Rem_Real_rem body resolves a real symbol. */
KaiValue *kaix_prelude_real_rem(KaiValue *a, KaiValue *b)                { return kai_prelude_real_rem(a, b); }
/* Issue #522: stdlib/math/real libm bindings. Mirrors the C-side
 * prelude table libm block; needed at link time when the LLVM emit
 * references @kaix_prelude_real_<libm-op>. */
KaiValue *kaix_prelude_real_sqrt(KaiValue *x)                            { return kai_prelude_real_sqrt(x); }
KaiValue *kaix_prelude_real_cbrt(KaiValue *x)                            { return kai_prelude_real_cbrt(x); }
KaiValue *kaix_prelude_real_exp(KaiValue *x)                             { return kai_prelude_real_exp(x); }
KaiValue *kaix_prelude_real_log(KaiValue *x)                             { return kai_prelude_real_log(x); }
KaiValue *kaix_prelude_real_log2(KaiValue *x)                            { return kai_prelude_real_log2(x); }
KaiValue *kaix_prelude_real_log10(KaiValue *x)                           { return kai_prelude_real_log10(x); }
KaiValue *kaix_prelude_real_sin(KaiValue *x)                             { return kai_prelude_real_sin(x); }
KaiValue *kaix_prelude_real_cos(KaiValue *x)                             { return kai_prelude_real_cos(x); }
KaiValue *kaix_prelude_real_tan(KaiValue *x)                             { return kai_prelude_real_tan(x); }
KaiValue *kaix_prelude_real_asin(KaiValue *x)                            { return kai_prelude_real_asin(x); }
KaiValue *kaix_prelude_real_acos(KaiValue *x)                            { return kai_prelude_real_acos(x); }
KaiValue *kaix_prelude_real_atan(KaiValue *x)                            { return kai_prelude_real_atan(x); }
KaiValue *kaix_prelude_real_sinh(KaiValue *x)                            { return kai_prelude_real_sinh(x); }
KaiValue *kaix_prelude_real_cosh(KaiValue *x)                            { return kai_prelude_real_cosh(x); }
KaiValue *kaix_prelude_real_tanh(KaiValue *x)                            { return kai_prelude_real_tanh(x); }
KaiValue *kaix_prelude_real_signum(KaiValue *x)                          { return kai_prelude_real_signum(x); }
KaiValue *kaix_prelude_real_is_nan(KaiValue *x)                          { return kai_prelude_real_is_nan(x); }
KaiValue *kaix_prelude_real_is_inf(KaiValue *x)                          { return kai_prelude_real_is_inf(x); }
KaiValue *kaix_prelude_real_pow(KaiValue *a, KaiValue *b)                { return kai_prelude_real_pow(a, b); }
KaiValue *kaix_prelude_real_atan2(KaiValue *a, KaiValue *b)              { return kai_prelude_real_atan2(a, b); }
/* Issue #523: m8 mailbox runtime bindings. Mirrors the C-side
 * prelude table mailbox block; needed at link time when the LLVM
 * emit references @kaix_prelude_mailbox_* (e.g. via
 * stdlib/actor.kai's spawn_actor / send / receive surface). */
KaiValue *kaix_prelude_mailbox_alloc(void)                               { return kai_prelude_mailbox_alloc(); }
KaiValue *kaix_prelude_mailbox_alloc_bounded(KaiValue *cap, KaiValue *overflow) { return kai_prelude_mailbox_alloc_bounded(cap, overflow); }
KaiValue *kaix_prelude_mailbox_alloc_unowned(void)                       { return kai_prelude_mailbox_alloc_unowned(); }
KaiValue *kaix_prelude_mailbox_assign_owner(KaiValue *pid, KaiValue *fiber) { return kai_prelude_mailbox_assign_owner(pid, fiber); }
KaiValue *kaix_prelude_mailbox_send(KaiValue *pid, KaiValue *msg)        { return kai_prelude_mailbox_send(pid, msg); }
KaiValue *kaix_prelude_mailbox_recv(KaiValue *pid)                       { return kai_prelude_mailbox_recv(pid); }
KaiValue *kaix_prelude_mailbox_free(KaiValue *pid)                       { return kai_prelude_mailbox_free(pid); }
/* Lane 4 (#473) Byte primitive ops. Mirrors the C-side prelude
 * table; needed at link time when the LLVM emit references
 * @kaix_prelude_byte_* (e.g. via stdlib/protocols.kai's
 * BinSerialize impls calling byte_to_int / int_to_byte). */
KaiValue *kaix_prelude_int_to_byte(KaiValue *v)                          { return kai_prelude_int_to_byte(v); }
KaiValue *kaix_prelude_byte_to_int(KaiValue *v)                          { return kai_prelude_byte_to_int(v); }
KaiValue *kaix_prelude_byte_add(KaiValue *a, KaiValue *b)                { return kai_prelude_byte_add(a, b); }
KaiValue *kaix_prelude_byte_sub(KaiValue *a, KaiValue *b)                { return kai_prelude_byte_sub(a, b); }
KaiValue *kaix_prelude_byte_eq(KaiValue *a, KaiValue *b)                 { return kai_prelude_byte_eq(a, b); }
KaiValue *kaix_prelude_byte_lt(KaiValue *a, KaiValue *b)                 { return kai_prelude_byte_lt(a, b); }
KaiValue *kaix_prelude_byte_to_string(KaiValue *v)                       { return kai_prelude_byte_to_string(v); }
KaiValue *kaix_prelude_ref_make(KaiValue *init)                          { return kai_prelude_ref_make(init); }
KaiValue *kaix_prelude_ref_get(KaiValue *r)                              { return kai_prelude_ref_get(r); }
KaiValue *kaix_prelude_ref_set(KaiValue *r, KaiValue *v)                 { return kai_prelude_ref_set(r, v); }

/* m7c-c / m7c-d — kaix_* wrappers around the static runtime
 * helpers in runtime.h. The LLVM IR can only see externally-
 * linkable symbols, so these thin shims expose every helper the
 * effects ABI needs (push/pop, lookup, cont init/resume,
 * default-handler clauses for the catalog builtins). */
KaiHandlerId kaix_fresh_handler_id(void) { return kai_fresh_handler_id(); }

void kaix_evidence_push(KaiEvidence *node, const char *eff_label, void *handler) {
    kai_evidence_push(node, eff_label, handler);
}

void kaix_evidence_pop(void) { kai_evidence_pop(); }

void *kaix_evidence_lookup_handler(const char *eff_label) {
    KaiEvidence *node = kai_evidence_lookup_node(eff_label);
    if (node == NULL) { return NULL; }
    return node->handler;
}

/* m8 bug #12 follow-up — LLVM mirror of the C emit's per-fiber
 * `in_dispatch_node` save/restore around an op call. The C path
 * captures the node from `kai_evidence_lookup_node(...)` and
 * pokes `current_fiber->in_dispatch_node` directly across the
 * indirect call (stage2/compiler.kai's `emit_call_expr`); the
 * LLVM path only had `kaix_evidence_lookup_handler`, so a
 * self-delegating handler under `--emit=llvm` would re-resolve to
 * the same node and infinite-loop just like bug #12 in the C
 * backend. The pair below (`kaix_in_dispatch_enter` /
 * `kaix_in_dispatch_leave`) lets the LLVM emit set the flag and
 * restore it without exposing the KaiFiber struct in IR. */
void *kaix_evidence_lookup_node(const char *eff_label) {
    return (void *) kai_evidence_lookup_node(eff_label);
}

void *kaix_evidence_node_handler(void *node_v) {
    KaiEvidence *node = (KaiEvidence *) node_v;
    if (node == NULL) { return NULL; }
    return node->handler;
}

void *kaix_in_dispatch_enter(void *node_v) {
    KaiEvidence *node = (KaiEvidence *) node_v;
    KaiFiber *fib = kai_current_fiber();
    KaiEvidence *prev = fib->in_dispatch_node;
    fib->in_dispatch_node = node;
    return (void *) prev;
}

void kaix_in_dispatch_leave(void *prev_v) {
    KaiFiber *fib = kai_current_fiber();
    fib->in_dispatch_node = (KaiEvidence *) prev_v;
}

void kaix_cont_init_identity(KaiCont *k, KaiHandlerId hid) {
    kai_cont_init_identity(k, hid);
}

KaiValue *kaix_cont_resume(KaiCont *k, KaiValue *v) {
    return kai_cont_resume(k, v);
}

/* m7c-d — clause-body helper for the 2-arg `resume(v, ns)` form.
 * Every Ev<Eff> struct begins with `KaiHandlerId` (8 bytes) +
 * `void *env` (8 bytes), so `state` lives at byte offset 16. */
void kaix_clause_state_set(void *self, KaiValue *v) {
    *((KaiValue **)((char *) self + 16)) = v;
}

/* m7c-d — non-static wrappers for the default-handler clause
 * functions defined static in runtime.h. The LLVM emit's
 * `kai_main_install_defaults` references these. Untyped i8*
 * self-args mirror the IR-level signature; the underlying
 * statics use the typed Ev<Eff> *self but the layout is the
 * same KaiHandlerId-leading prefix so the cast is safe. */
KaiValue *kaix_default_stdout_print(void *self, KaiValue *s, KaiCont *k) {
    return kai_default_stdout_print(self, s, k);
}
KaiValue *kaix_default_stderr_eprint(void *self, KaiValue *s, KaiCont *k) {
    return kai_default_stderr_eprint(self, s, k);
}
KaiValue *kaix_default_fail_fail(void *self, KaiValue *msg, KaiCont *k) {
    return kai_default_fail_fail(self, msg, k);
}
KaiValue *kaix_default_mutable_array_make(void *self, KaiValue *n, KaiValue *init, KaiCont *k) {
    return kai_default_mutable_array_make(self, n, init, k);
}
KaiValue *kaix_default_mutable_array_length(void *self, KaiValue *a, KaiCont *k) {
    return kai_default_mutable_array_length(self, a, k);
}
KaiValue *kaix_default_mutable_array_get(void *self, KaiValue *a, KaiValue *i, KaiCont *k) {
    return kai_default_mutable_array_get(self, a, i, k);
}
KaiValue *kaix_default_mutable_array_set(void *self, KaiValue *a, KaiValue *i, KaiValue *v, KaiCont *k) {
    return kai_default_mutable_array_set(self, a, i, v, k);
}
KaiValue *kaix_default_mutable_array_grow(void *self, KaiValue *a, KaiValue *n, KaiValue *init, KaiCont *k) {
    return kai_default_mutable_array_grow(self, a, n, init, k);
}
KaiValue *kaix_default_mutable_ref_make(void *self, KaiValue *init, KaiCont *k) {
    return kai_default_mutable_ref_make(self, init, k);
}
KaiValue *kaix_default_mutable_ref_get(void *self, KaiValue *r, KaiCont *k) {
    return kai_default_mutable_ref_get(self, r, k);
}
KaiValue *kaix_default_mutable_ref_set(void *self, KaiValue *r, KaiValue *v, KaiCont *k) {
    return kai_default_mutable_ref_set(self, r, v, k);
}
KaiValue *kaix_default_random_int_range(void *self, KaiValue *lo, KaiValue *hi, KaiCont *k) {
    return kai_default_random_int_range(self, lo, hi, k);
}

/* Clock default handler — LLVM-visible wrappers around the static
 * Clock default handlers in runtime.h. */
KaiValue *kaix_default_clock_wall_now(void *self, KaiCont *k) {
    return kai_default_clock_wall_now(self, k);
}
KaiValue *kaix_default_clock_monotonic_now(void *self, KaiCont *k) {
    return kai_default_clock_monotonic_now(self, k);
}
KaiValue *kaix_default_clock_sleep_ns(void *self, KaiValue *ns, KaiCont *k) {
    return kai_default_clock_sleep_ns(self, ns, k);
}

/* net-tcp-v1 — LLVM-visible wrappers around the static NetTcp
 * default handlers in runtime.h. The LLVM emitter installs these
 * by name from `kai_main_install_defaults`. */
KaiValue *kaix_default_nettcp_connect(void *self, KaiValue *host, KaiValue *port, KaiCont *k) {
    return kai_default_nettcp_connect(self, host, port, k);
}
KaiValue *kaix_default_nettcp_listen(void *self, KaiValue *host, KaiValue *port, KaiCont *k) {
    return kai_default_nettcp_listen(self, host, port, k);
}
KaiValue *kaix_default_nettcp_accept(void *self, KaiValue *l, KaiCont *k) {
    return kai_default_nettcp_accept(self, l, k);
}
KaiValue *kaix_default_nettcp_send(void *self, KaiValue *c, KaiValue *data, KaiCont *k) {
    return kai_default_nettcp_send(self, c, data, k);
}
KaiValue *kaix_default_nettcp_recv(void *self, KaiValue *c, KaiValue *max, KaiCont *k) {
    return kai_default_nettcp_recv(self, c, max, k);
}
KaiValue *kaix_default_nettcp_close(void *self, KaiValue *c, KaiCont *k) {
    return kai_default_nettcp_close(self, c, k);
}

/* Issue #141 — LLVM-visible wrappers around the static Log default
 * handlers in runtime.h. The LLVM emitter installs these by name
 * from `kai_main_install_defaults` when `Log` appears in main's
 * row. */
KaiValue *kaix_default_log_debug(void *self, KaiValue *msg, KaiCont *k) {
    return kai_default_log_debug(self, msg, k);
}
KaiValue *kaix_default_log_info(void *self, KaiValue *msg, KaiCont *k) {
    return kai_default_log_info(self, msg, k);
}
KaiValue *kaix_default_log_warn(void *self, KaiValue *msg, KaiCont *k) {
    return kai_default_log_warn(self, msg, k);
}
KaiValue *kaix_default_log_error(void *self, KaiValue *msg, KaiCont *k) {
    return kai_default_log_error(self, msg, k);
}

/* issue #140 — LLVM-visible wrappers around the static SecureRandom
 * default handlers in runtime.h. */
KaiValue *kaix_default_securerandom_int_range(void *self, KaiValue *min_v, KaiValue *max_v, KaiCont *k) {
    return kai_default_securerandom_int_range(self, min_v, max_v, k);
}
KaiValue *kaix_default_securerandom_bytes(void *self, KaiValue *n_v, KaiCont *k) {
    return kai_default_securerandom_bytes(self, n_v, k);
}

/* Issue #570 — LLVM-visible wrappers around the static Spawn default
 * handlers in runtime.h. The LLVM emitter installs these by name from
 * `kai_main_install_defaults` when `Spawn` appears in main's row.
 * Without these the runtime's `kaix_evidence_lookup_handler("Spawn")`
 * returns NULL and the first op call (typically inside `with_mailbox`
 * → `spawn_actor`) dereferences a null evidence pointer. */
KaiValue *kaix_default_spawn_yield(void *self, KaiCont *k) {
    return kai_default_spawn_yield(self, k);
}
KaiValue *kaix_default_spawn_spawn(void *self, KaiValue *thunk, KaiCont *k) {
    return kai_default_spawn_spawn(self, thunk, k);
}
KaiValue *kaix_default_spawn_await(void *self, KaiValue *fib, KaiCont *k) {
    return kai_default_spawn_await(self, fib, k);
}
KaiValue *kaix_default_spawn_select(void *self, KaiValue *fibs, KaiCont *k) {
    return kai_default_spawn_select(self, fibs, k);
}
KaiValue *kaix_default_spawn_cancel(void *self, KaiValue *fib, KaiCont *k) {
    return kai_default_spawn_cancel(self, fib, k);
}
KaiValue *kaix_default_spawn_set_trap_exit(void *self, KaiValue *on, KaiCont *k) {
    return kai_default_spawn_set_trap_exit(self, on, k);
}

/* Issue #582 — LLVM-visible wrapper around `kai_default_cancel_raise`.
 * The LLVM emitter installs this by name from `kai_main_install_defaults`
 * when `Cancel` appears in main's row. Without it the runtime's
 * `kaix_evidence_lookup_handler("Cancel")` returned NULL and
 * `Cancel.raise()` inside a fiber (e.g. one spawned under a mailboxed
 * parent) dereferenced a null evidence op pointer. */
KaiValue *kaix_default_cancel_raise(void *self, KaiCont *k) {
    return kai_default_cancel_raise(self, k);
}

/* Issue #587 — LLVM-visible wrappers around the static Link / Monitor
 * default handlers in runtime.h. Same gap as #570 (Spawn) and #582
 * (Cancel): without these the LLVM `kaix_evidence_lookup_handler`
 * returned NULL for Link/Monitor and any fiber body that called
 * `Link.link(_)` or `Monitor.monitor(_)` segfaulted on the first op. */
KaiValue *kaix_default_link_link(void *self, KaiValue *peer, KaiCont *k) {
    return kai_default_link_link(self, peer, k);
}
KaiValue *kaix_default_monitor_monitor(void *self, KaiValue *target, KaiCont *k) {
    return kai_default_monitor_monitor(self, target, k);
}
KaiValue *kaix_default_monitor_demonitor(void *self, KaiValue *ref, KaiCont *k) {
    return kai_default_monitor_demonitor(self, ref, k);
}

/* m7c-d — install/teardown default handlers for builtins that
 * appear in main's row. The LLVM emitter generates the body of
 * these two functions per program (filling in the right
 * push/pop sequence for the row). When main has no builtin
 * effects the LLVM IR still defines them as no-ops. */
extern void kai_main_install_defaults(void);
extern void kai_main_teardown_defaults(void);

/* Entry point: the LLVM output defines kai_main. Match what the C
   backend's emit_main_wrapper does. */
extern KaiValue *kai_main(void);

int main(int argc, char **argv) {
    kai_set_args(argc, argv);
    kai_main_install_defaults();
    KaiValue *result = kai_main();
    kai_main_teardown_defaults();
    kai_decref(result);
    return 0;
}
