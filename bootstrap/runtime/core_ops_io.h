/* Fragment of runtime.h; include runtime.h, never this file. */
/* ---------- binary and unary operators ---------- */
/*
 * Linear-consumption primitives (m5.x-flip Phase 3, 2026-04-28):
 * each op reads ALL relevant fields of its arguments BEFORE decref'ing,
 * then returns a freshly-allocated result. Aliasing-safe: `kai_op_eq_v(x, x)`
 * decrefs `a` and `b` separately, but the read-then-decref ordering means
 * both reads complete on a still-alive value. This pairs with the dup
 * pass + exit drops + producer-side incref-on-extract (Steps A + B) so
 * every value is released exactly once. NOT flipped: `kai_op_field` (already
 * increfs its return; introspection rather than consumer), `kai_op_eq` (C-int
 * returning, used inside non-consuming match tests), `kai_apply` (closure
 * invocation; lifecycle managed at the call lowering).
 */

/* Int add, sub, and mul compute in uint64_t and cast back. kaikai's Int is a
 * wrapping two's-complement int64_t by design (CLAUDE.md Tier 1 + the
 * Int contract in docs); the polynomial/FNV hash mixing in
 * stdlib/protocols.kai relies on the wrap. Signed overflow is UB in
 * C99 (6.5) even when the hardware wraps, so doing the math directly
 * on int64_t trips `-fsanitize=undefined` (tier1-asan) the moment a
 * value overflows — which `impl Hash for Real` makes trivial (a
 * Real's bit-cast is a large Int, and `acc * 31` overflows). The
 * unsigned compute is well-defined modular arithmetic (6.3.1.3) and,
 * on every target kaikai supports (two's-complement clang/gcc),
 * produces the byte-identical result the signed form did — so this
 * silences the UB without changing any emitted output. Division and
 * comparison stay signed (their overflow is not part of the contract).
 */
/* Fixed-width boxed arithmetic (numeric lane A). `+`/`-`/`*` over two
 * same-tag fixed-width boxes land here on both backends. Each computes in
 * the unsigned of the width (defined two's-complement wrap) and re-boxes.
 * Returns NULL when the pair is not a matching fixed-width pair, so the
 * caller falls through to its Int/Real path. Byte also has a raw path;
 * this arm is its boxed border (a boxed operand keeps the binop boxed). */
KAI_RT_SHARED KaiValue *kai_fixed_arith(KaiValue *a, KaiValue *b, char op) {
    if (!kai_is_ptr(a) || !kai_is_ptr(b) || a->tag != b->tag) return NULL;
    switch ((KaiTag) a->tag) {
        case KAI_BYTE: { uint8_t x = a->as.byte_val, y = b->as.byte_val;
            uint8_t r = op == '+' ? (uint8_t)(x + y) : op == '-' ? (uint8_t)(x - y) : (uint8_t)(x * y); return kai_byte(r); }
        case KAI_INT32: { uint32_t x = (uint32_t) a->as.i32, y = (uint32_t) b->as.i32;
            uint32_t r = op == '+' ? x + y : op == '-' ? x - y : x * y; return kai_int32((int32_t) r); }
        case KAI_UINT32: { uint32_t x = a->as.u32, y = b->as.u32;
            uint32_t r = op == '+' ? x + y : op == '-' ? x - y : x * y; return kai_uint32(r); }
        case KAI_UINT64: { uint64_t x = a->as.u64, y = b->as.u64;
            uint64_t r = op == '+' ? x + y : op == '-' ? x - y : x * y; return kai_uint64(r); }
        case KAI_INT128: { unsigned __int128 x = (unsigned __int128) kai_i128_load(a), y = (unsigned __int128) kai_i128_load(b);
            unsigned __int128 r = op == '+' ? x + y : op == '-' ? x - y : x * y; return kai_int128((__int128) r); }
        default: return NULL;
    }
}

/* Division for two same-tag fixed-width boxes. Unlike the wrapping
 * add/sub/mul, the result depends on signedness, so each width divides
 * in its own signed/unsigned domain (truncated toward zero, C
 * semantics). A zero divisor aborts. NULL on a non-fixed-width pair. */
static KaiValue *kai_fixed_div(KaiValue *a, KaiValue *b) {
    if (!kai_is_ptr(a) || !kai_is_ptr(b) || a->tag != b->tag) return NULL;
    switch ((KaiTag) a->tag) {
        case KAI_BYTE:   { uint8_t  y = b->as.byte_val; if (y == 0) { kai_trap_abort("divide by zero"); } return kai_byte((uint8_t)(a->as.byte_val / y)); }
        /* i64 domain so INT32_MIN / -1 wraps back to INT32_MIN on the
         * truncating narrow, matching the raw widen-divide-narrow path. */
        case KAI_INT32:  { int32_t  y = b->as.i32;  if (y == 0) { kai_trap_abort("divide by zero"); } return kai_int32((int32_t)((int64_t) a->as.i32 / (int64_t) y)); }
        case KAI_UINT32: { uint32_t y = b->as.u32;  if (y == 0) { kai_trap_abort("divide by zero"); } return kai_uint32(a->as.u32 / y); }
        case KAI_UINT64: { uint64_t y = b->as.u64;  if (y == 0) { kai_trap_abort("divide by zero"); } return kai_uint64(a->as.u64 / y); }
        /* `/ -1` negates in the unsigned domain: I128_MIN / -1 is UB direct. */
        case KAI_INT128: { __int128 y = kai_i128_load(b); if (y == 0) { kai_trap_abort("divide by zero"); } return kai_int128(y == -1 ? (__int128)(0 - (unsigned __int128) kai_i128_load(a)) : kai_i128_load(a) / y); }
        default: return NULL;
    }
}

static KaiValue *kai_op_add(KaiValue *a, KaiValue *b) {
    KaiValue *r;
    if (kai_is_int(a)  && kai_is_int(b))       r = kai_int((int64_t)((uint64_t) kai_intf(a) + (uint64_t) kai_intf(b)));
    else if (kai_is_ptr(a) && a->tag == KAI_REAL && kai_is_ptr(b) && b->tag == KAI_REAL) r = kai_real(a->as.r + b->as.r);
    else if ((r = kai_fixed_arith(a, b, '+')) != NULL) { /* boxed fixed-width */ }
    else { kai_eprintf("kai: type mismatch in +\n"); kai_exit(1); }
    kai_decref(a); kai_decref(b);
    return r;
}

static KaiValue *kai_op_sub(KaiValue *a, KaiValue *b) {
    KaiValue *r;
    if (kai_is_int(a)  && kai_is_int(b))       r = kai_int((int64_t)((uint64_t) kai_intf(a) - (uint64_t) kai_intf(b)));
    else if (kai_is_ptr(a) && a->tag == KAI_REAL && kai_is_ptr(b) && b->tag == KAI_REAL) r = kai_real(a->as.r - b->as.r);
    else if ((r = kai_fixed_arith(a, b, '-')) != NULL) { /* boxed fixed-width */ }
    else { kai_eprintf("kai: type mismatch in -\n"); kai_exit(1); }
    kai_decref(a); kai_decref(b);
    return r;
}

static KaiValue *kai_op_mul(KaiValue *a, KaiValue *b) {
    KaiValue *r;
    if (kai_is_int(a)  && kai_is_int(b))       r = kai_int((int64_t)((uint64_t) kai_intf(a) * (uint64_t) kai_intf(b)));
    else if (kai_is_ptr(a) && a->tag == KAI_REAL && kai_is_ptr(b) && b->tag == KAI_REAL) r = kai_real(a->as.r * b->as.r);
    else if ((r = kai_fixed_arith(a, b, '*')) != NULL) { /* boxed fixed-width */ }
    else { kai_eprintf("kai: type mismatch in *\n"); kai_exit(1); }
    kai_decref(a); kai_decref(b);
    return r;
}

static KaiValue *kai_op_div(KaiValue *a, KaiValue *b) {
    KaiValue *r;
    if (kai_is_int(a) && kai_is_int(b)) {
        /* Same trap discipline as the raw kai_idiv_chk: a boxed and a
         * raw `/` on the same operands must agree. */
        if (kai_intf(b) == 0) { kai_trap_abort("divide by zero"); }
        if (kai_intf(a) == INT64_MIN && kai_intf(b) == -1) { kai_trap_abort("divide overflow"); }
        r = kai_int(kai_intf(a) / kai_intf(b));
    } else if (kai_is_ptr(a) && a->tag == KAI_REAL && kai_is_ptr(b) && b->tag == KAI_REAL) {
        r = kai_real(a->as.r / b->as.r);
    } else if ((r = kai_fixed_div(a, b)) != NULL) { /* boxed fixed-width */ }
    else { kai_eprintf("kai: type mismatch in /\n"); kai_exit(1); }
    kai_decref(a); kai_decref(b);
    return r;
}

static KaiValue *kai_op_mod(KaiValue *a, KaiValue *b) {
    KaiValue *r;
    if (kai_is_int(a) && kai_is_int(b)) {
        if (kai_intf(b) == 0) { kai_trap_abort("mod by zero"); }
        if (kai_intf(a) == INT64_MIN && kai_intf(b) == -1) { kai_trap_abort("divide overflow"); }
        r = kai_int(kai_intf(a) % kai_intf(b));
    } else if (kai_is_ptr(a) && a->tag == KAI_BYTE && kai_is_ptr(b) && b->tag == KAI_BYTE) {
        if (b->as.byte_val == 0) { kai_trap_abort("mod by zero"); }
        r = kai_byte((uint8_t)(a->as.byte_val % b->as.byte_val));
    } else if (kai_is_ptr(a) && a->tag == KAI_INT32 && kai_is_ptr(b) && b->tag == KAI_INT32) {
        if (b->as.i32 == 0) { kai_trap_abort("mod by zero"); }
        /* i64 domain so INT32_MIN % -1 is 0, matching the raw widen path. */
        r = kai_int32((int32_t)((int64_t) a->as.i32 % (int64_t) b->as.i32));
    } else if (kai_is_ptr(a) && a->tag == KAI_UINT32 && kai_is_ptr(b) && b->tag == KAI_UINT32) {
        if (b->as.u32 == 0) { kai_trap_abort("mod by zero"); }
        r = kai_uint32(a->as.u32 % b->as.u32);
    } else if (kai_is_ptr(a) && a->tag == KAI_UINT64 && kai_is_ptr(b) && b->tag == KAI_UINT64) {
        if (b->as.u64 == 0) { kai_trap_abort("mod by zero"); }
        r = kai_uint64(a->as.u64 % b->as.u64);
    } else if (kai_is_ptr(a) && a->tag == KAI_INT128 && kai_is_ptr(b) && b->tag == KAI_INT128) {
        __int128 y = kai_i128_load(b); if (y == 0) { kai_trap_abort("mod by zero"); }
        /* `x % -1` is 0 by definition; the direct op is UB at I128_MIN. */
        r = kai_int128(y == -1 ? (__int128) 0 : kai_i128_load(a) % y);
    } else { kai_eprintf("kai: type mismatch in %%\n"); kai_exit(1); }
    kai_decref(a); kai_decref(b);
    return r;
}

/* Raw (unboxed) i64 `/` and `%` with the same trap discipline as the boxed
 * path and array indexing: a zero divisor, or the one signed overflow
 * `INT64_MIN / -1` (whose two's-complement result is unrepresentable), is a
 * clean `kai_trap_abort`, not the hardware UB `sdiv`/`srem` would invoke.
 * Both backends route raw Int div/mod here so the fault is deterministic
 * across them. */
static int64_t kai_idiv_chk(int64_t a, int64_t b) {
    if (b == 0) { kai_trap_abort("divide by zero"); }
    if (a == INT64_MIN && b == -1) { kai_trap_abort("divide overflow"); }
    return a / b;
}
static int64_t kai_imod_chk(int64_t a, int64_t b) {
    if (b == 0) { kai_trap_abort("mod by zero"); }
    if (a == INT64_MIN && b == -1) { kai_trap_abort("divide overflow"); }
    return a % b;
}

/* Unsigned `/` and `%` with the zero-divisor trap. UInt32/UInt64 widen
 * their operands into u64 and divide unsigned; there is no INT_MIN/-1
 * overflow in the unsigned domain, so only the zero divisor traps. */
static uint64_t kai_udiv_chk(uint64_t a, uint64_t b) {
    if (b == 0) { kai_trap_abort("divide by zero"); }
    return a / b;
}
static uint64_t kai_umod_chk(uint64_t a, uint64_t b) {
    if (b == 0) { kai_trap_abort("mod by zero"); }
    return a % b;
}

/* Less-than for two same-tag fixed-width boxes; signedness per the
 * width. Sets `*out` and returns 1 on a match, 0 otherwise. */
static int kai_fixed_lt(KaiValue *a, KaiValue *b, int *out) {
    if (!kai_is_ptr(a) || !kai_is_ptr(b) || a->tag != b->tag) return 0;
    switch ((KaiTag) a->tag) {
        case KAI_BYTE:   *out = a->as.byte_val < b->as.byte_val; return 1;
        case KAI_INT32:  *out = a->as.i32  < b->as.i32;  return 1;
        case KAI_UINT32: *out = a->as.u32  < b->as.u32;  return 1;
        case KAI_UINT64: *out = a->as.u64  < b->as.u64;  return 1;
        case KAI_INT128: *out = kai_i128_load(a) < kai_i128_load(b); return 1;
        default: return 0;
    }
}

/* Greater-than for two same-tag fixed-width boxes; signedness per the
 * width. Mirror of `kai_fixed_lt`. */
static int kai_fixed_gt(KaiValue *a, KaiValue *b, int *out) {
    if (!kai_is_ptr(a) || !kai_is_ptr(b) || a->tag != b->tag) return 0;
    switch ((KaiTag) a->tag) {
        case KAI_BYTE:   *out = a->as.byte_val > b->as.byte_val; return 1;
        case KAI_INT32:  *out = a->as.i32  > b->as.i32;  return 1;
        case KAI_UINT32: *out = a->as.u32  > b->as.u32;  return 1;
        case KAI_UINT64: *out = a->as.u64  > b->as.u64;  return 1;
        case KAI_INT128: *out = kai_i128_load(a) > kai_i128_load(b); return 1;
        default: return 0;
    }
}

KAI_RT_SHARED KaiValue *kai_op_lt(KaiValue *a, KaiValue *b) {
    KaiValue *r;
    int _flt;
    if (kai_is_int(a)  && kai_is_int(b))       r = kai_bool(kai_intf(a) < kai_intf(b));
    else if (kai_is_ptr(a) && a->tag == KAI_REAL && kai_is_ptr(b) && b->tag == KAI_REAL) r = kai_bool(a->as.r < b->as.r);
    else if (kai_is_ptr(a) && a->tag == KAI_CHAR && kai_is_ptr(b) && b->tag == KAI_CHAR) r = kai_bool(a->as.c < b->as.c);
    else if (kai_fixed_lt(a, b, &_flt)) r = kai_bool(_flt);
    else if (kai_is_ptr(a) && a->tag == KAI_STR  && kai_is_ptr(b) && b->tag == KAI_STR) {
        size_t n = a->as.s.len < b->as.s.len ? a->as.s.len : b->as.s.len;
        int c = memcmp(a->as.s.bytes, b->as.s.bytes, n);
        if (c != 0) r = kai_bool(c < 0);
        else        r = kai_bool(a->as.s.len < b->as.s.len);
    } else {
        /* Ord dispatch: route to custom impl Ord.cmp when present (root and
         * nested). kai_op_lt CONSUMES a/b, so incref for the impl (which also
         * consumes) AND decref a/b at the end. cmp result < 0 means a < b. */
        int32_t _o_head = kai_head_tag(a);
        void *_o_fn = kai_lookup_impl(KAI_PROTO_ORD, KAI_OP_ORD_CMP, _o_head);
        if (_o_fn) {
            kai_incref(a); kai_incref(b);
            KaiValue *_o_c = ((KaiValue *(*)(KaiValue *, KaiValue *)) _o_fn)(a, b);
            int _o_r = kai_intf(_o_c) < 0;
            kai_decref(_o_c);
            kai_decref(a); kai_decref(b);
            return kai_bool(_o_r);
        }
        kai_eprintf("kai: type mismatch in <\n"); kai_exit(1);
    }
    kai_decref(a); kai_decref(b);
    return r;
}

KAI_RT_SHARED KaiValue *kai_op_gt(KaiValue *a, KaiValue *b) {
    KaiValue *r;
    int _fgt;
    if (kai_is_int(a)  && kai_is_int(b))       r = kai_bool(kai_intf(a) > kai_intf(b));
    else if (kai_is_ptr(a) && a->tag == KAI_REAL && kai_is_ptr(b) && b->tag == KAI_REAL) r = kai_bool(a->as.r > b->as.r);
    else if (kai_is_ptr(a) && a->tag == KAI_CHAR && kai_is_ptr(b) && b->tag == KAI_CHAR) r = kai_bool(a->as.c > b->as.c);
    else if (kai_fixed_gt(a, b, &_fgt)) r = kai_bool(_fgt);
    else if (kai_is_ptr(a) && a->tag == KAI_STR  && kai_is_ptr(b) && b->tag == KAI_STR) {
        size_t n = a->as.s.len < b->as.s.len ? a->as.s.len : b->as.s.len;
        int c = memcmp(a->as.s.bytes, b->as.s.bytes, n);
        if (c != 0) r = kai_bool(c > 0);
        else        r = kai_bool(a->as.s.len > b->as.s.len);
    } else {
        /* Ord dispatch (mirror of kai_op_lt). cmp result > 0 means a > b. */
        int32_t _o_head = kai_head_tag(a);
        void *_o_fn = kai_lookup_impl(KAI_PROTO_ORD, KAI_OP_ORD_CMP, _o_head);
        if (_o_fn) {
            kai_incref(a); kai_incref(b);
            KaiValue *_o_c = ((KaiValue *(*)(KaiValue *, KaiValue *)) _o_fn)(a, b);
            int _o_r = kai_intf(_o_c) > 0;
            kai_decref(_o_c);
            kai_decref(a); kai_decref(b);
            return kai_bool(_o_r);
        }
        kai_eprintf("kai: type mismatch in >\n"); kai_exit(1);
    }
    kai_decref(a); kai_decref(b);
    return r;
}

/* Two `Real`s compare with the C operator itself: a NaN is unordered, so
 * `<=` is not the negation of `>`. Sets `*out` and consumes both on a
 * match; returns 0, consuming neither, otherwise. */
static int kai_real_cmp(KaiValue *a, KaiValue *b, int le, int *out) {
    if (!kai_is_ptr(a) || a->tag != KAI_REAL || !kai_is_ptr(b) || b->tag != KAI_REAL) return 0;
    *out = le ? a->as.r <= b->as.r : a->as.r >= b->as.r;
    kai_decref(a); kai_decref(b);
    return 1;
}

/* Any other type is totally ordered, so `kai_op_le` and `kai_op_ge` are
 * layered over the consuming `kai_op_gt` / `kai_op_lt`. The inner call
 * consumes `a` and `b` already; we must not decref them again here. The
 * intermediate bool result is consumed locally to read the inverted
 * truth value. */
static KaiValue *kai_op_le(KaiValue *a, KaiValue *b) {
    int le;
    if (kai_real_cmp(a, b, 1, &le)) return kai_bool(le);
    KaiValue *g = kai_op_gt(a, b);
    KaiValue *r = kai_bool(!g->as.b);
    kai_decref(g);
    return r;
}

static KaiValue *kai_op_ge(KaiValue *a, KaiValue *b) {
    int ge;
    if (kai_real_cmp(a, b, 0, &ge)) return kai_bool(ge);
    KaiValue *l = kai_op_lt(a, b);
    KaiValue *r = kai_bool(!l->as.b);
    kai_decref(l);
    return r;
}

/* `kai_op_eq` does NOT consume — it is the C-int returning equality used by
 * pattern tests and other non-consuming sites. `kai_op_eq_v` / `kai_op_ne_v`
 * are the value-level wrappers used for `==` / `!=` expressions; those
 * DO consume per the m5.x flip. Self-aliasing (`kai_op_eq_v(x, x)`) is safe
 * because `kai_op_eq` reads both pointers before either decref runs.  */
static KaiValue *kai_op_eq_v(KaiValue *a, KaiValue *b) {
    KaiValue *r = kai_bool(kai_op_eq(a, b));
    kai_decref(a); kai_decref(b);
    return r;
}

static KaiValue *kai_op_ne_v(KaiValue *a, KaiValue *b) {
    KaiValue *r = kai_bool(!kai_op_eq(a, b));
    kai_decref(a); kai_decref(b);
    return r;
}

/* `kai_op_pow_int(a, b)` — integer-exponent power for the `^` operator.
 * Lives in runtime so the operator works without requiring the
 * Numeric protocol in scope. The unit of a dimensioned operand is
 * metadata on the type only; the runtime value carries no unit, so
 * the same code path serves `Real` and `Real<u>` alike.
 *
 * For Int: negative exponents truncate to 0 (no integer reciprocal).
 * For Real: negative exponents compute `1.0 / base^|e|` so unit
 * lifting at the type level (`r : Real<m>`, `r ^ -1 : Real<m^-1>`)
 * matches a meaningful runtime result. This is intentionally more
 * lenient than the `Numeric.pow_int` stdlib impl, which clamps
 * negatives to 0; the discrepancy is acceptable because `^`
 * dispatches through this helper, never through the protocol. */
static KaiValue *kai_op_pow_int(KaiValue *a, KaiValue *b) {
    if (!kai_is_int(b)) {
        kai_eprintf("kai: type mismatch in ^ (exponent must be Int)\n"); kai_exit(1);
    }
    int64_t e = kai_intf(b);
    KaiValue *r;
    if (kai_is_int(a)) {
        if (e < 0) { r = kai_int(0); }
        else {
            /* Unsigned accumulate: an overflowing power wraps like `*`. */
            uint64_t base = (uint64_t) kai_intf(a);
            uint64_t acc = 1;
            for (int64_t i = 0; i < e; i++) { acc *= base; }
            r = kai_int((int64_t) acc);
        }
    } else if (kai_is_ptr(a) && a->tag == KAI_REAL) {
        double base = a->as.r;
        int64_t k = e < 0 ? -e : e;
        double acc = 1.0;
        for (int64_t i = 0; i < k; i++) { acc *= base; }
        if (e < 0) { acc = 1.0 / acc; }
        r = kai_real(acc);
    } else { kai_eprintf("kai: type mismatch in ^\n"); kai_exit(1); }
    kai_decref(a); kai_decref(b);
    return r;
}

static KaiValue *kai_op_neg(KaiValue *a) {
    KaiValue *r;
    if (kai_is_int(a))       r = kai_int((int64_t)(0 - (uint64_t) kai_intf(a)));
    else if (kai_is_ptr(a) && a->tag == KAI_REAL) r = kai_real(-a->as.r);
    else if (kai_is_ptr(a) && a->tag == KAI_BYTE)   r = kai_byte((uint8_t)(0u - a->as.byte_val));
    else if (kai_is_ptr(a) && a->tag == KAI_INT32)  r = kai_int32((int32_t)(-(uint32_t) a->as.i32));
    else if (kai_is_ptr(a) && a->tag == KAI_UINT32) r = kai_uint32((uint32_t)(-a->as.u32));
    else if (kai_is_ptr(a) && a->tag == KAI_UINT64) r = kai_uint64((uint64_t)(-a->as.u64));
    else if (kai_is_ptr(a) && a->tag == KAI_INT128) r = kai_int128((__int128)(-(unsigned __int128) kai_i128_load(a)));
    else { kai_eprintf("kai: type mismatch in unary -\n"); kai_exit(1); }
    kai_decref(a);
    return r;
}

static KaiValue *kai_op_boolnot(KaiValue *a) {
    KaiValue *r;
    if (a->tag == KAI_BOOL) r = kai_bool(!a->as.b);
    else { kai_eprintf("kai: type mismatch in `not`\n"); kai_exit(1); }
    kai_decref(a);
    return r;
}

/* `kai_op_truthy` is a non-consuming C-int predicate used inside
 * `if (kai_op_truthy(...))`, ternary lowerings of `if`/`and`/`or`, and
 * `kai_assert_check`. It is intentionally NOT flipped under the
 * m5.x runtime flip: the LLVM short-circuit lowering's phi node
 * returns `lhs` itself in the early-exit branch, so consuming `lhs`
 * inside the truthiness probe would alias-free a value still
 * referenced downstream. The emitted-C path leaks the temporary
 * argument; tracked in issue #82 as future cleanup (predicate
 * consumes + emit-side incref before short-circuit). */
static int kai_op_truthy(KaiValue *v) {
    return v && v->tag == KAI_BOOL && v->as.b;
}

/* ---------- range construction ---------- */

/* O(1): the list is represented by its generator (KAI_RANGE), never
 * built. Consumers iterate it; match borders normalise one cell at a
 * time (kai_seq_norm). */
static KaiValue *kai_range(KaiValue *from, KaiValue *to) {
    return kai_range_new(kai_intf(from), kai_intf(to), 1);
}

static KaiValue *kai_range_step(KaiValue *from, KaiValue *to, KaiValue *step) {
    int64_t s = kai_intf(step);
    if (s == 0) { kai_eprintf("kai: zero step in range\n"); kai_exit(1); }
    return kai_range_new(kai_intf(from), kai_intf(to), s);
}

/* ---------- core: args (set by generated `int main`) ---------- */

/* Argv snapshot is written once by the generated main (the owner TU) and
 * read by os_args()/exe_name() anywhere, so it is shared — a per-TU copy
 * would leave every non-owner TU reading a NULL argv. */
#if defined(KAI_SEPARATE_COMPILATION)
extern int    kai_g_argc;
extern char **kai_g_argv;
#  if defined(KAI_RUNTIME_OWNER)
int    kai_g_argc = 0;
char **kai_g_argv = NULL;
#  endif
#else
static int          kai_g_argc = 0;
static char       **kai_g_argv = NULL;
#endif

static void kai_set_args(int argc, char **argv) {
    /* Bind this thread's worker before any fiber runs. */
    (void) kai_worker_here();
    kai_g_argc = argc;
    kai_g_argv = argv;
    /* Block-buffered off a TTY (one write per buffer, not per line); the
     * runtime flushes before it blocks, writes stderr, or exits. */
    if (isatty(STDOUT_FILENO)) setvbuf(stdout, NULL, _IOLBF, 0);
    /* Lazy registration: kai_set_args runs once at program start from
       the emitted main wrapper. atexit fires only when the env var
       gates a report, so the side effect is harmless when tracing is
       off. Keeps the emitter unchanged — no new emit site needed. */
    kai_slab_register_teardown();
    kai_rc_register_once();
    kai_rc_ledger_register();
#ifdef KAI_TRACE_RC
    kai_rc_strict_register_once();
#endif
}

/* `main`'s value as the process exit status. An Int return is the status
 * (POSIX keeps the low 8 bits); any other return type — Unit, String — is 0.
 * `main` is excluded from unboxing (classify_unbox_sig), so the result is
 * always a KaiValue* here. Both backends' entry points share this rule.
 * Returning the code lets libc run the full exit path, atexit included;
 * `os.process.exit` flushes stdout but skips atexit. */
static int kai_main_exit_status(KaiValue *result) {
    int status = kai_is_int(result) ? (int) (kai_intf(result) & 0xff) : 0;
    atomic_store(&kai_chosen_exit_plus1, status + 1);
    return status;
}

static KaiValue *kai_core_args(void) {
    KaiValue *acc = kai_nil();
    for (int i = kai_g_argc - 1; i >= 1; --i) {
        acc = kai_cons(kai_str(kai_g_argv[i]), acc);
    }
    return acc;
}

/* Issue #127: argv[0] as a kaikai String. Mirrors `kai_core_args`
 * in routing through the `kai_g_argv` snapshot installed by
 * `kai_set_args` at process entry. Returns the empty string when
 * argv was never captured (unit tests linking the runtime without
 * a generated `int main` wrapper) — leaves the surface total. */
static KaiValue *kai_core_program_name(void) {
    if (kai_g_argv == NULL || kai_g_argv[0] == NULL) return kai_str("");
    return kai_str(kai_g_argv[0]);
}

/* Hanga Roa core loader: the path where `core/` lives on this
 * system, hard-coded at compile time via -DKAI_STDLIB_PATH=... in
 * stage1/stage2 Makefiles. Defaults to "stdlib" (relative to the
 * caller's cwd) when the macro is unset, which keeps in-tree
 * unit harnesses happy without the macro. Surface to kaikai code
 * via the `kai_stdlib_path` builtin. */
#ifndef KAI_STDLIB_PATH
#define KAI_STDLIB_PATH "stdlib"
#endif
/* The compile-time `KAI_STDLIB_PATH` macro is set by stage{1,2}/Makefile
 * to `$(abspath ../stdlib)` — an absolute path to the checkout's stdlib
 * directory. After install (brew, tarball, etc.) that path no longer
 * exists, so the env-var override `KAIKAI_STDLIB_PATH` takes precedence
 * when set. `bin/kai` exports this var pointing at the installed
 * `share/kaikai/stdlib` so users get a working compiler without
 * rebuilding from source. Direct `kaic2` invocations from a source
 * checkout leave the env unset and fall back to the macro.
 */
static KaiValue *kai_core_stdlib_path(void) {
    const char *env = getenv("KAIKAI_STDLIB_PATH");
    if (env && *env) {
        return kai_str(env);
    }
    return kai_str(KAI_STDLIB_PATH);
}

/* Canonicalise a filesystem path via realpath(3). Returns the input
 * unchanged when the path does not yet exist (caller's "first_try"
 * candidate path before stat), so the dedup logic at the load site
 * stays correct: only paths that resolve to real files get folded
 * to their canonical form. Backed by POSIX realpath; falls back to
 * the input string when realpath fails (errno != 0). */
static KaiValue *kai_core_abspath(KaiValue *path) {
    if (!path || path->tag != KAI_STR) {
        return path;
    }
    char pbuf[KAI_PATH_BUF];
    size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
    memcpy(pbuf, path->as.s.bytes, plen);
    pbuf[plen] = '\0';
    char resolved[KAI_PATH_BUF];
    if (realpath(pbuf, resolved) == NULL) {
        return path;
    }
    return kai_str_dyn(resolved);
}

/* ---------- core: mailbox runtime (m8 #7) ---------- */

/* User code reaches the mailbox runtime through these core
 * functions. They are wrapped in stdlib/actor.kai's `with_mailbox`
 * helper, which also installs the user-facing Actor[Msg] handler.
 * The polymorphic surface uses Nothing → TyAny so a single set of
 * runtime entries serves every Msg type. */

static KaiValue *kai_core_mailbox_alloc(void) {
    return kai_pid_value(kai_mailbox_alloc());
}

/* Tier 2 spawn_actor — allocate a mailbox without stamping any
 * owner. Pair with kai_core_mailbox_assign_owner to wire the
 * mailbox onto the spawned fiber before its body runs. */
static KaiValue *kai_core_mailbox_alloc_unowned(void) {
    return kai_pid_value(kai_mailbox_alloc_unowned());
}

/* Tier 2 spawn_actor — set `pid->as.mb->owner_fiber = fiber->as.fib`
 * AND `fiber->as.fib->mailbox = pid->as.mb`. Retained for callers that
 * already hold a spawned Fiber; spawn_actor itself uses
 * kai_core_spawn_actor_fiber, which stamps the owner before the fiber is
 * enqueued so Monitor/Link resolve it synchronously with no race. */
static KaiValue *kai_core_mailbox_assign_owner(KaiValue *pid, KaiValue *fiber) {
    if (pid && pid->tag == KAI_PID && pid->as.mb &&
        fiber && fiber->tag == KAI_FIBER && fiber->as.fib) {
        pid->as.mb->owner_fiber = fiber->as.fib;
        pid->as.mb->outer       = fiber->as.fib->mailbox;
        fiber->as.fib->mailbox  = pid->as.mb;
        if (!pid->as.pid.owner) pid->as.pid.owner = kai_fiber_exit_ref(fiber->as.fib);
    }
    /* m5.x flip Phase 3 closeout (issue #82): consume input refs. */
    if (pid)   kai_decref(pid);
    if (fiber) kai_decref(fiber);
    return kai_unit();
}

/* Spawn a fiber for `thunk` and stamp `pid`'s mailbox onto it — owner
 * wiring and enqueue happen together, before the fiber can be stolen, so
 * there is no window where the child runs with an unwired mailbox and no
 * cross-thread write to owner_fiber. Because the stamp completes before
 * this returns, Monitor/Link in the spawning fiber resolve owner_fiber
 * synchronously. Consumes `pid` and `thunk`: the fiber holds its own
 * reference to the thunk, the caller keeps its own Pid handle. */
static KAI_RC_NOINLINE KaiValue *kai_spawn_fiber_stamped(KaiValue *thunk, KaiValue *stamp_pid);
KAI_SCHED_FN KaiValue *kai_core_spawn_actor_fiber(KaiValue *pid, KaiValue *thunk)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    KaiValue *f = kai_spawn_fiber_stamped(thunk, (pid && pid->tag == KAI_PID) ? pid : NULL);
    if (pid) kai_decref(pid);
    kai_decref(thunk);
    return f;
}
#endif

static KaiValue *kai_core_mailbox_alloc_bounded(KaiValue *cap, KaiValue *overflow) {
    int c = (kai_is_int(cap)) ? (int) kai_intf(cap) : 0;
    int o = (kai_is_int(overflow)) ? (int) kai_intf(overflow) : 0;
    KaiValue *r = kai_pid_value(kai_mailbox_alloc_bounded(c, o));
    /* m5.x flip Phase 3 closeout (issue #82): consume input refs. */
    if (cap)      kai_decref(cap);
    if (overflow) kai_decref(overflow);
    return r;
}

/* Issue #763 spawn_actor_policy — bounded alloc without stamping an
 * owner. Pair with kai_core_mailbox_assign_owner, same protocol
 * as kai_core_mailbox_alloc_unowned. */
static KaiValue *kai_core_mailbox_alloc_bounded_unowned(KaiValue *cap, KaiValue *overflow) {
    int c = (kai_is_int(cap)) ? (int) kai_intf(cap) : 0;
    int o = (kai_is_int(overflow)) ? (int) kai_intf(overflow) : 0;
    KaiValue *r = kai_pid_value(kai_mailbox_alloc_bounded_unowned(c, o));
    /* Consume input refs — callee-consumes discipline (issue #82). */
    if (cap)      kai_decref(cap);
    if (overflow) kai_decref(overflow);
    return r;
}

KAI_SCHED_FN KaiValue *kai_core_mailbox_send(KaiValue *pid, KaiValue *msg)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    if (!pid || pid->tag != KAI_PID) {
        kai_eprintf("kai: mailbox_send: argument is not a Pid\n");
        kai_exit(1);
    }
    /* A send to an ended mailbox succeeds and the message is dropped: the
     * sender cannot know whether the receiver is still alive. */
    KaiMailbox *mb = kai_mailbox_pin(pid);
    if (!mb) {
        kai_decref(msg);
    } else if (kai_nthreads > 1) {
        /* Copy unconditionally: no point-in-time "same thread?" answer is
         * sound, since a thief can retarget the receiver's home thread
         * under a lock the sender does not hold, and a Perceus dup across
         * the send leaves the sender its own reference to a value stealing
         * would later share across threads. */
        KaiValue *copy = kai_deep_copy_cross(msg);
        kai_decref(msg);
        kai_mailbox_push_cross_thread(mb, copy);
    } else {
        kai_mailbox_push(mb, msg);
    }
    if (mb) kai_mailbox_unpin(mb);
    kai_decref(pid);
    /* A sender parked on a full mailbox that was cancelled unwinds here. */
    kai_cancel_deliver(kai_current_fiber());
    return kai_unit();
}
#endif

KAI_SCHED_FN KaiValue *kai_core_mailbox_recv(KaiValue *pid)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    if (!pid || pid->tag != KAI_PID || !pid->as.mb) {
        kai_eprintf("kai: mailbox_recv: argument is not a Pid\n");
        kai_exit(1);
    }
    /* kai_mailbox_pop returns the stored ref (mailbox transferred its
     * ownership to the caller). Consume the input `pid` ref so the
     * helper is callee-consumes-clean. */
    KaiValue *msg = kai_mailbox_pop(pid->as.mb);
    kai_decref(pid);
    /* A due cancel unwinds from here: the call does not return. */
    if (!msg) kai_cancel_deliver(kai_current_fiber());
    return msg;
}
#endif

/* Receive within a deadline. `timeout_nanos` is a relative nanosecond
 * budget; returns `Some(msg)` if a message arrives in time, `None` if
 * the deadline elapses first. Mirrors `kai_core_mailbox_recv`'s
 * callee-consumes-clean discipline for the input `pid` ref. */
KAI_SCHED_FN KaiValue *kai_core_mailbox_recv_timeout(KaiValue *pid, KaiValue *timeout_nanos)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    if (!pid || pid->tag != KAI_PID || !pid->as.mb) {
        kai_eprintf("kai: mailbox_recv_timeout: argument is not a Pid\n");
        kai_exit(1);
    }
    int64_t ns = kai_intf(timeout_nanos);
    kai_decref(timeout_nanos);
    KaiValue *msg = kai_mailbox_pop_timeout(pid->as.mb,
                                            ns < 0 ? 0 : (uint64_t) ns);
    kai_decref(pid);
    if (msg) {
        return kai_variant_u(0, "Some", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
    }
    kai_cancel_deliver(kai_current_fiber());
    return kai_variant_u(1, "None", 0, 0, NULL);
}
#endif

/* Close the mailbox attached to a Pid when its `with_mailbox` /
 * `spawn_actor` scope exits; the Pid value itself is RC-managed
 * independently and reads as ended from then on. */
KAI_SCHED_FN KaiValue *kai_core_mailbox_free(KaiValue *pid)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    if (pid && pid->tag == KAI_PID) {
        _Atomic int *s = kai_pid_stripe_lock(pid);
        KaiMailbox *mb = pid->as.mb;
        pid->as.mb = NULL;
        kai_pid_stripe_unlock(s);
        if (mb) kai_mailbox_close(mb);
    }
    if (pid) kai_decref(pid);
    return kai_unit();
}
#endif

/* ---------- core: file io ---------- */

static KAI_RC_NOINLINE KaiValue *kai_core_read_file(KaiValue *path) {
    KaiValue *r = NULL;
    if (!path || path->tag != KAI_STR) {
        KaiValue *msg = kai_str("read_file: argument is not a String");
        r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
    } else {
        char pbuf[KAI_PATH_BUF];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        FILE *fp = fopen(pbuf, "rb");
        struct stat st;
        if (!fp) {
            KaiValue *msg = kai_str("read_file: cannot open file");
            r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
        } else if (fstat(fileno(fp), &st) == 0 && S_ISDIR(st.st_mode)) {
            /* fopen accepts directories; on Linux ext4 ftell(SEEK_END) then
             * reports the htree EOF sentinel (~2^63), so reject before
             * sizing the buffer. */
            fclose(fp);
            KaiValue *msg = kai_str("read_file: is a directory");
            r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
        } else if (fseek(fp, 0, SEEK_END) != 0) {
            fclose(fp);
            KaiValue *msg = kai_str("read_file: seek failed");
            r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
        } else {
            long n = ftell(fp);
            if (n < 0) {
                fclose(fp);
                KaiValue *msg = kai_str("read_file: tell failed");
                r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
            } else if (fseek(fp, 0, SEEK_SET) != 0) {
                fclose(fp);
                KaiValue *msg = kai_str("read_file: rewind failed");
                r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
            } else {
                KaiValue *v = kai_alloc(KAI_STR);
                v->as.s.len = (size_t) n;
                v->as.s.bytes = (char *) kai_heap_malloc((size_t) n + 1);
                size_t got = fread(v->as.s.bytes, 1, (size_t) n, fp);
                fclose(fp);
                v->as.s.bytes[got] = '\0';
                v->as.s.len = got;
                r = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = v}});
            }
        }
    }
    if (path) kai_decref(path);
    return r;
}

static KaiValue *kai_core_write_file(KaiValue *path, KaiValue *content) {
    KaiValue *r = NULL;
    if (!path || path->tag != KAI_STR) {
        KaiValue *msg = kai_str("write_file: path is not a String");
        r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
    } else if (!content || content->tag != KAI_STR) {
        KaiValue *msg = kai_str("write_file: content is not a String");
        r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
    } else {
        char pbuf[KAI_PATH_BUF];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        FILE *fp = fopen(pbuf, "wb");
        if (!fp) {
            KaiValue *msg = kai_str("write_file: cannot open file");
            r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
        } else {
            size_t wrote = fwrite(content->as.s.bytes, 1, content->as.s.len, fp);
            fclose(fp);
            if (wrote != content->as.s.len) {
                KaiValue *msg = kai_str("write_file: short write");
                r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
            } else {
                KaiValue *u = kai_unit();
                r = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = u}});
            }
        }
    }
    if (path)    kai_decref(path);
    if (content) kai_decref(content);
    return r;
}

/* Appends `content` through one O_APPEND descriptor, so the kernel
 * places each write at the end of the file and only the new bytes are
 * written. Consumes both arguments like write_file. */
static KaiValue *kai_core_file_append(KaiValue *path, KaiValue *content) {
    KaiValue *r = NULL;
    if (!path || path->tag != KAI_STR) {
        KaiValue *msg = kai_str("file_append: path is not a String");
        r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
    } else if (!content || content->tag != KAI_STR) {
        KaiValue *msg = kai_str("file_append: content is not a String");
        r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
    } else {
        char pbuf[KAI_PATH_BUF];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        int fd = open(pbuf, O_WRONLY | O_APPEND | O_CREAT, 0666);
        if (fd < 0) {
            KaiValue *msg = kai_str("file_append: cannot open file");
            r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
        } else {
            const char *p = content->as.s.bytes;
            size_t left = content->as.s.len;
            int ok = 1;
            while (left > 0) {
                ssize_t w = write(fd, p, left);
                if (w < 0) {
                    if (errno == EINTR) continue;
                    ok = 0;
                    break;
                }
                p += w;
                left -= (size_t) w;
            }
            close(fd);
            if (!ok) {
                KaiValue *msg = kai_str("file_append: short write");
                r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
            } else {
                r = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = kai_unit()}});
            }
        }
    }
    if (path)    kai_decref(path);
    if (content) kai_decref(content);
    return r;
}

