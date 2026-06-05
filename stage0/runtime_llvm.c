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
KaiValue *kaix_prelude_write_stdout(KaiValue *v)   { return kai_prelude_write_stdout(v); }
KaiValue *kaix_prelude_int_to_string(KaiValue *v)  { return kai_prelude_int_to_string(v); }

/* ---------- M3c: strings, ranges, higher-order prelude ---------- */
KaiValue *kaix_string_concat(KaiValue *a, KaiValue *b)         { return kai_string_concat(a, b); }
KaiValue *kaix_to_string(KaiValue *v)                           { return kai_to_string(v); }

/* Issue #86: contract / plain asserts under the LLVM backend. The C
   backend has always lowered `SAssert` to `kai_assert_check`; the LLVM
   arm used to be a no-op (cond evaluated for effects, bool discarded),
   so contracts never fired under LLVM. These forwarders wire the same
   runtime entry points, with `kaix_assert_check_with_value` carrying
   the offending binding's runtime value (issue #86 piece 2). */
void      kaix_assert_check(KaiValue *cond, const char *msg)    { kai_assert_check(cond, msg); }
void      kaix_assert_check_with_value(KaiValue *cond, const char *base_msg,
                                       const char *ident_name, KaiValue *val) {
    kai_assert_check_with_value(cond, base_msg, ident_name, val);
}

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
KaiValue *kaix_write_stdout_thunk(KaiValue *s, KaiValue **a, int n)     { (void)s; (void)n; return kai_prelude_write_stdout(a[0]); }
KaiValue *kaix_int_to_string_thunk(KaiValue *s, KaiValue **a, int n)    { (void)s; (void)n; return kai_prelude_int_to_string(a[0]); }

/* ---------- M3d: variants + match ---------- */
KaiValue *kaix_variant(int32_t tag, const char *name, int32_t n, KaiValue **args) {
    /* LLVM backend still passes args as KaiValue **; bridge to the
     * slot-mask shape by stamping each as a pointer slot. mask==0
     * means "all slots boxed", which is what the legacy boxed path
     * always emitted. */
    if (n <= 0) return kai_variant_u(tag, name, 0, 0, NULL);
    KaiVarSlot stack_slots[16];
    KaiVarSlot *slots = stack_slots;
    KaiVarSlot *heap = NULL;
    if (n > (int)(sizeof(stack_slots) / sizeof(stack_slots[0]))) {
        heap = (KaiVarSlot *) malloc((size_t) n * sizeof(KaiVarSlot));
        slots = heap;
    }
    for (int i = 0; i < n; i++) slots[i].ptr = args[i];
    KaiValue *r = kai_variant_u(tag, name, n, 0, slots);
    if (heap) free(heap);
    return r;
}

/* 1 iff `v` is a KAI_VARIANT whose tag name matches `name`. Legacy
   match discriminator — kept as a fallback for emit paths that do
   not resolve a constructor name to a tag at compile time (synthetic
   AST nodes that slip past `evar_find_tag_opt`). The fast path is
   `kaix_is_variant_tag` below. */
int kaix_is_variant(KaiValue *v, const char *name) {
    return v && v->tag == KAI_VARIANT && strcmp(kai_variant_name_of(v->variant_tag), name) == 0;
}

/* 1 iff `v` is a KAI_VARIANT whose variant_tag equals `tag`. The
   primary match discriminator under issue #688 — the LLVM backend
   emits this when the constructor name resolves through the
   atom-style global table in docs/variant-tags.md. Constant-folds to
   a single i32 compare under -O2, eliminating the strcmp call that
   the legacy `kaix_is_variant` path retains for fallback. */
int kaix_is_variant_tag(KaiValue *v, int32_t tag) {
    return v && v->tag == KAI_VARIANT && v->variant_tag == tag;
}

/* Read the i-th argument of a variant. incref so the caller owns it;
   the C backend passes variant args as borrowed references, but the
   LLVM path keeps ownership uniform. */
KaiValue *kaix_variant_arg(KaiValue *v, int i) {
    /* Issue #126 / #622 (Cluster F) — variant slot must honor the
     * `slot_mask`. The LLVM emitter always *builds* variants with
     * mask==0 (all boxed pointers, see `kaix_variant` above), so for
     * everything the codegen mints `.ptr` is correct. Since #741 moved
     * Int variant slots to the tagged-`kai_int` boxed representation,
     * the C runtime's own constructors (notably `Exited(Int)` /
     * `Signaled(Int)` from the Process default handler,
     * runtime.h §`_kai_process_make_exit_exited`) also mint boxed
     * slots with mask==0, so this accessor's `.ptr` path covers them
     * directly. The mask-honoring branch below stays as a defensive
     * accessor for any future runtime-minted typed slot (mask!=0):
     * reading a typed slot via `.ptr` would reinterpret the raw scalar
     * as a pointer and feed garbage into the match arm.
     * `kai_variant_slot_box` is exactly the boxed-view accessor the C
     * backend uses (`slot_read_for_test` in emit_c.kai); route through
     * it so both backends agree on any runtime-minted typed-slot variant.
     *
     * Ownership: `kai_variant_slot_box` returns a borrowed pointer for
     * pointer slots and a freshly-allocated boxed temporary (rc==1)
     * for typed slots. The LLVM path keeps ownership uniform (caller
     * owns the returned cell), so incref only the borrowed pointer-slot
     * case; the typed-slot temporary already arrives owned. */
    uint32_t k = kai_var_slot_kind(kai_slot_mask_of(v->variant_tag), i);
    if (k == KAI_VAR_SLOT_INT || k == KAI_VAR_SLOT_REAL) {
        return kai_variant_slot_box(v, i);
    }
    return kai_incref(kai_var_slots(v)[i].ptr);
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

/* ---------- issue #120: opt-in Perceus regions ----------
 * The LLVM backend lowers `region { ... }` to kaix_arena_push on entry,
 * kaix_arena_pop on exit, and routes every constructor inside the block
 * through kaix_arena_alloc — the SAME C arena helpers the C backend
 * calls inline (docs/issue-120-regions-design.md: one helper, both
 * backends, no separate LLVM bump-alloc). The arena stack is a runtime
 * global, so push/pop/alloc need no KaiArena* across the IR boundary.
 * P0 links these even though no lowering emits them yet — the
 * build-release.sh LLVM smoke gate verifies the symbols resolve. */
void      kaix_arena_push(void)                          { (void) kai_arena_push(); }
void      kaix_arena_pop(void)                           { kai_arena_pop(); }
KaiValue *kaix_arena_alloc(KaiTag tag) {
    KaiArena *a = kai_arena_current();
    /* Defensive: if codegen ever calls this outside a region (a bug),
     * fall back to the normal RC heap rather than dereference NULL. */
    return a ? kai_arena_alloc(a, tag) : kai_alloc(tag);
}

/* issue #120 — opt-in Perceus regions (Phase P1): arena-backed
 * constructor + deep-copy-out shims, mirroring the C-backend helpers so
 * both emitters call the SAME runtime code (no separate LLVM path). */
KaiValue *kaix_arena_cons(KaiValue *h, KaiValue *t)                       { return kai_arena_cons(h, t); }
KaiValue *kaix_arena_record(int n, KaiValue **fields, const char **names) { return kai_arena_record(n, fields, names); }
KaiValue *kaix_arena_variant(int32_t tag, const char *name, int n, KaiValue **args) {
    /* Same boxed-args bridge as kaix_variant: stamp into slot-mask shape
     * with mask 0 and forward to the arena variant ctor. */
    if (n <= 0) return kai_arena_variant(tag, name, 0, 0, NULL);
    KaiVarSlot stack_slots[16];
    KaiVarSlot *slots = stack_slots;
    KaiVarSlot *heap = NULL;
    if (n > (int)(sizeof(stack_slots) / sizeof(stack_slots[0]))) {
        heap = (KaiVarSlot *) malloc((size_t) n * sizeof(KaiVarSlot));
        slots = heap;
    }
    for (int i = 0; i < n; i++) slots[i].ptr = args[i];
    KaiValue *r = kai_arena_variant(tag, name, n, 0, slots);
    if (heap) free(heap);
    return r;
}
KaiValue *kaix_deep_copy_out(KaiValue *v)                                 { return kai_deep_copy_out(v); }

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
  /* Same bridge as kaix_variant: LLVM's emit path still hands us
   * KaiValue ** for boxed args. Stamp into the slot-mask shape with
   * mask==0 and forward to the unified entry point. */
  if (n <= 0) return kai_reuse_or_alloc_variant(scr, tag, name, 0, 0, NULL);
  KaiVarSlot stack_slots[16];
  KaiVarSlot *slots = stack_slots;
  KaiVarSlot *heap = NULL;
  if (n > (int)(sizeof(stack_slots) / sizeof(stack_slots[0]))) {
    heap = (KaiVarSlot *) malloc((size_t) n * sizeof(KaiVarSlot));
    slots = heap;
  }
  for (int i = 0; i < n; i++) slots[i].ptr = args[i];
  KaiValue *r = kai_reuse_or_alloc_variant(scr, tag, name, n, 0, slots);
  if (heap) free(heap);
  return r;
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
KaiValue *kaix_prelude_string_cp_at(KaiValue *s, KaiValue *off)  { return kai_prelude_string_cp_at(s, off); }
KaiValue *kaix_prelude_string_cp_len(KaiValue *s, KaiValue *off) { return kai_prelude_string_cp_len(s, off); }
KaiValue *kaix_prelude_string_hash(KaiValue *s)                { return kai_prelude_string_hash(s); }
KaiValue *kaix_prelude_real_bits(KaiValue *v)                  { return kai_prelude_real_bits(v); }
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

/* m7c-c (LLVM per-instance dispatch) — mirror of the C emit's
 * `kai_evidence_lookup_node_by_id`. An aliased `with State[T](init)
 * as name` handler captures its `handler_id` at install time; a
 * `@name` read (`name.get()`) must resolve to *that* instance's
 * node, not the innermost State node by name. Without this the LLVM
 * backend collapsed every nested `var` cell to the top-of-stack
 * State handler, so e.g. `@top` read the value of the last-pushed
 * cell. The C backend has always resolved aliased ops by id; this
 * brings the LLVM mirror to parity. (refs #622) */
void *kaix_evidence_lookup_node_by_id(KaiHandlerId id) {
    return (void *) kai_evidence_lookup_node_by_id(id);
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

/* ---------- continuation-discard unwind (parity lane, refs #622) ----------
 *
 * The C backend's `handle` lowering (emit_c.kai ~4025) wraps the body
 * in `if (setjmp(_jmp)==0) { push_with_jmp(...); body; pop; } else {
 * body_result = _discard; }`, and the op-call site (emit_c.kai ~1684)
 * longjmps to that pad when a clause discards `resume` (status stays
 * UNRESUMED). The runtime carries `handle_jmp` + `discard_slot` on the
 * `KaiEvidence` node (runtime.h ~8796) and exposes
 * `kai_evidence_push_with_jmp`.
 *
 * The LLVM backend emits the `setjmp` in the handle frame directly (it
 * MUST run there — a `longjmp` into a returned frame is UB), but the
 * subtle bits — the `jmp_buf` size, the status/handle_jmp read, and the
 * store+pop+longjmp triple — live here so the emitted IR stays
 * straight-line and the one place that knows `sizeof(jmp_buf)` is the C
 * side, never an N hardcoded in IR. */

/* Size + alignment of the platform `jmp_buf`, fetched by the emitter so
 * the handle pad `alloca`s `i8, i64 %size, align 16` without baking a
 * platform-specific N into the IR. macOS arm64's `jmp_buf` is large
 * (sigjmp-capable); glibc's smaller — both fit a generous, 16-aligned
 * runtime-sized buffer. */
int64_t kai_jmpbuf_size(void) { return (int64_t) sizeof(jmp_buf); }

/* Mirror of `kai_evidence_push_with_jmp` (runtime.h) for the LLVM
 * install path: stamps the node's `handle_jmp` + `discard_slot` so the
 * op site can find the landing pad on a discard. `jmp` is the `i8*`
 * cast of the handle's `alloca`'d `jmp_buf`; `discard_slot` is the
 * `i8*` cast of the handle's `%KaiValue*` discard alloca. */
void kaix_evidence_push_with_jmp(KaiEvidence *node, const char *eff_label,
                                 void *handler, void *jmp, void *discard_slot) {
    kai_evidence_push_with_jmp(node, eff_label, handler,
                               (jmp_buf *) jmp, (KaiValue **) discard_slot);
}

/* Op-site discard test, mirroring the C branch condition
 * `_k.status == KAI_CONT_UNRESUMED && _node_op->handle_jmp != NULL`.
 * Returns 1 when the clause discarded `resume` AND a handle pad is in
 * scope to unwind to. `node_v` is the looked-up `KaiEvidence*`; `k_v`
 * is the `KaiCont*` the dispatch passed to the clause. */
int kaix_op_discarded(void *node_v, void *k_v) {
    KaiEvidence *node = (KaiEvidence *) node_v;
    KaiCont *k = (KaiCont *) k_v;
    if (node == NULL || k == NULL) { return 0; }
    return (k->status == KAI_CONT_UNRESUMED && node->handle_jmp != NULL) ? 1 : 0;
}

/* Op-site discard unwind, mirroring the C triple
 * `*discard_slot = op_r; kai_evidence_pop(); longjmp(*handle_jmp, 1);`.
 * Only called when `kaix_op_discarded` returned 1, so `handle_jmp` and
 * `discard_slot` are both live. Does not return. */
void kaix_handle_discard_unwind(void *node_v, KaiValue *op_r) {
    KaiEvidence *node = (KaiEvidence *) node_v;
    *node->discard_slot = op_r;
    kai_evidence_pop();
    longjmp(*node->handle_jmp, 1);
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

/* Parity lane B (#622) — LLVM-visible wrappers around the static
 * File / Stdin / Process / Env / Signal default handlers in runtime.h.
 * The LLVM install path used to drive off a hardcoded effect-name
 * table that omitted these effects entirely; the user-effect default
 * path (issue #558) was also absent. The install is AST-driven now
 * (emit_llvm.kai), so any effect with an all-`$extern_handler` default
 * block — these builtins included — installs its handler at main entry
 * and references the `kaix_*` forwarder below. Without the forwarder
 * the IR's `@kaix_default_*` symbol is undefined and clang rejects it
 * (the #570/#582/#587 failure mode). File/Stdin/Process were the four
 * segfaulting fixtures; Env/Signal share the same gap and are wired
 * here for completeness so the AST walk never re-opens it. */
KaiValue *kaix_default_file_read_file(void *self, KaiValue *path, KaiCont *k) {
    return kai_default_file_read_file(self, path, k);
}
KaiValue *kaix_default_file_write_file(void *self, KaiValue *path, KaiValue *contents, KaiCont *k) {
    return kai_default_file_write_file(self, path, contents, k);
}
KaiValue *kaix_default_stdin_read_line(void *self, KaiCont *k) {
    return kai_default_stdin_read_line(self, k);
}
KaiValue *kaix_default_stdin_read_bytes(void *self, KaiValue *n, KaiCont *k) {
    return kai_default_stdin_read_bytes(self, n, k);
}
KaiValue *kaix_default_process_start(void *self, KaiValue *cmd, KaiValue *args, KaiCont *k) {
    return kai_default_process_start(self, cmd, args, k);
}
KaiValue *kaix_default_process_wait(void *self, KaiValue *child, KaiCont *k) {
    return kai_default_process_wait(self, child, k);
}
KaiValue *kaix_default_process_kill(void *self, KaiValue *child, KaiValue *sig, KaiCont *k) {
    return kai_default_process_kill(self, child, sig, k);
}
KaiValue *kaix_default_process_exit(void *self, KaiValue *code, KaiCont *k) {
    return kai_default_process_exit(self, code, k);
}
KaiValue *kaix_default_env_args(void *self, KaiCont *k) {
    return kai_default_env_args(self, k);
}
KaiValue *kaix_default_env_var(void *self, KaiValue *name, KaiCont *k) {
    return kai_default_env_var(self, name, k);
}
KaiValue *kaix_default_env_set_var(void *self, KaiValue *name, KaiValue *value, KaiCont *k) {
    return kai_default_env_set_var(self, name, value, k);
}
KaiValue *kaix_default_env_unset_var(void *self, KaiValue *name, KaiCont *k) {
    return kai_default_env_unset_var(self, name, k);
}
KaiValue *kaix_default_env_vars(void *self, KaiCont *k) {
    return kai_default_env_vars(self, k);
}
KaiValue *kaix_default_signal_on(void *self, KaiValue *sig_v, KaiCont *k) {
    return kai_default_signal_on(self, sig_v, k);
}
KaiValue *kaix_default_signal_off(void *self, KaiValue *sig_v, KaiCont *k) {
    return kai_default_signal_off(self, sig_v, k);
}
KaiValue *kaix_default_signal_await(void *self, KaiCont *k) {
    return kai_default_signal_await(self, k);
}

/* m7c-d — install/teardown default handlers for builtins that
 * appear in main's row. The LLVM emitter generates the body of
 * these two functions per program (filling in the right
 * push/pop sequence for the row). When main has no builtin
 * effects the LLVM IR still defines them as no-ops. */
extern void kai_main_install_defaults(void);
extern void kai_main_teardown_defaults(void);

/* Option C — protocol dispatch tables. The LLVM emitter generates the
 * body of `_kai_proto_init_llvm` per program: it calls
 * `kaix_register_one_impl` once per impl and
 * `kaix_register_one_variant_head` once per variant. When the program
 * declares no protocols the IR still defines the function as a
 * no-op. */
extern void _kai_proto_init_llvm(void);

/* Variant-tag -> head-tag registration shim. The LLVM IR calls this
 * once per variant entry; we accumulate in a heap array and rebind
 * the runtime pointer + length each time. */
static int32_t *_kaix_v2h_heap     = NULL;
static int32_t  _kaix_v2h_capacity = 0;

void kaix_register_one_variant_head(int32_t variant_tag, int32_t head_tag) {
    if (variant_tag >= _kaix_v2h_capacity) {
        int32_t newcap = _kaix_v2h_capacity == 0 ? 32 : _kaix_v2h_capacity * 2;
        while (variant_tag >= newcap) newcap *= 2;
        _kaix_v2h_heap = (int32_t *) realloc(_kaix_v2h_heap, (size_t) newcap * sizeof(int32_t));
        for (int32_t i = _kaix_v2h_capacity; i < newcap; ++i) _kaix_v2h_heap[i] = 0;
        _kaix_v2h_capacity = newcap;
    }
    _kaix_v2h_heap[variant_tag] = head_tag;
    kai_register_variant_heads(_kaix_v2h_heap, _kaix_v2h_capacity);
}

/* issue #118 layer 3 — reusable-tag registration shim. The LLVM IR
 * calls this once per tag that the Perceus recogniser marked as a
 * reuse target; it stamps the runtime bitset so kai_variant_u stops
 * immortalising those cells (which would saturate rc and block
 * reuse-in-place). Mirrors the C backend's
 * kai_register_reusable_tags(arr, n) but one-at-a-time, matching the
 * per-entry shape of the other kaix_register_one_* shims. */
void kaix_register_one_reusable_tag(int32_t tag) {
    kai_register_reusable_tags(&tag, 1);
}

/* Impl-table registration shim. The LLVM IR calls this once per impl
 * with the resolved function pointer. Builds an in-place hashmap by
 * accumulating + reinserting on each call. Cheap because each
 * register call is O(1) amortised. */
static KaiImplEntry *_kaix_impls_heap     = NULL;
static int32_t       _kaix_impls_capacity = 0;
static int32_t       _kaix_impls_count    = 0;

void kaix_register_one_impl(int32_t proto_id, int32_t head_tag, void *fn) {
    if (_kaix_impls_count >= _kaix_impls_capacity) {
        int32_t newcap = _kaix_impls_capacity == 0 ? 32 : _kaix_impls_capacity * 2;
        _kaix_impls_heap = (KaiImplEntry *) realloc(_kaix_impls_heap, (size_t) newcap * sizeof(KaiImplEntry));
        _kaix_impls_capacity = newcap;
    }
    _kaix_impls_heap[_kaix_impls_count].proto_id = proto_id;
    _kaix_impls_heap[_kaix_impls_count].head_tag = head_tag;
    _kaix_impls_heap[_kaix_impls_count].fn       = fn;
    _kaix_impls_count++;
    kai_register_impls(_kaix_impls_heap, _kaix_impls_count);
}

/* Runtime helpers exposed to LLVM IR via @kaix_* — used by the
 * runtime dispatch shim emitted for every `__proto_<op>` dispatcher. */
int32_t kaix_head_tag(KaiValue *v) {
    return kai_head_tag(v);
}

void *kaix_lookup_impl(int32_t proto_id, int32_t head_tag) {
    return kai_lookup_impl(proto_id, head_tag);
}

void kaix_panic_no_impl(const char *proto, const char *op, int32_t head) {
    fprintf(stderr, "panic: no impl of %s.%s for runtime head %d\n",
            proto, op, (int) head);
    exit(1);
}

/* ---------- TRMC constructor-context (issue #668, LLVM backend) ----------
 *
 * The C backend lowers a modulo-cons tail leaf with the inline
 * `kai_cctx_*` helpers (runtime.h §TRMC), carrying the `KaiCctx`
 * { res, holeptr } struct by value across its goto-loop. The LLVM
 * backend keeps the accumulator as two SSA-stable allocas in the
 * function prologue (`%_kai_acc.res` : %KaiValue*, `%_kai_acc.hole` :
 * %KaiValue**) and calls these non-inline `kaix_*` shims so the
 * `KaiCctx` struct never has to round-trip through LLVM IR by value.
 *
 * `kaix_field_addr(node, holeslot)` returns the address of the boxed
 * recursive slot inside `node` — the open hole the next loop step
 * attaches to. Mirrors `kai_field_addr_create(&kai_var_slots(node)
 * [holeslot].ptr)`.
 *
 * `kaix_cctx_apply(res, hole, child)` plugs `child` into the open hole
 * and returns the spine root. An empty cctx (hole == NULL, first
 * level) returns `child` itself. This is the terminal-leaf lowering
 * (`__kai_trmc_apply`) AND the apply half of extend; identical to
 * `kai_cctx_apply_linear`.
 *
 * `kaix_cctx_extend(res, hole, child)` plugs `child` into the old hole
 * and returns the NEW spine root. The caller then computes the new
 * hole separately via `kaix_field_addr(child, holeslot)` and stores
 * both back into the accumulator allocas. Splitting the result from
 * the new-hole keeps each shim a single C expression with no struct
 * return, which the LLVM emitter consumes as one `call` + two stores.
 */
KaiValue **kaix_field_addr(KaiValue *node, int32_t holeslot) {
    return &kai_var_slots(node)[holeslot].ptr;
}

KaiValue *kaix_cctx_apply(KaiValue *res, KaiValue **hole, KaiValue *child) {
    if (hole != NULL) { *hole = child; return res; }
    return child;
}

KaiValue *kaix_cctx_extend(KaiValue *res, KaiValue **hole, KaiValue *child) {
    return kaix_cctx_apply(res, hole, child);
}

/* Entry point: the LLVM output defines kai_main. Match what the C
   backend's emit_main_wrapper does. */
extern KaiValue *kai_main(void);

int main(int argc, char **argv) {
    kai_set_args(argc, argv);
    _kai_proto_init_llvm();
    kai_main_install_defaults();
    KaiValue *result = kai_main();
    kai_main_teardown_defaults();
    kai_decref(result);
    return 0;
}
