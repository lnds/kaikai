/* Fragment of runtime.h; include runtime.h, never this file. */
/* ---------- bench harness hooks (used by --bench runs) ----------
 * bench v1.x (issue #437): per-iteration timings collected into a
 * sample buffer; on finalize we sort and report median + MAD + mean
 * + range. The emitted bench wrapper runs KAI_BENCH_WARMUP untimed
 * iterations first to take JIT/cache effects out of the timed
 * window, then KAI_BENCH_ITERS timed iterations. Both knobs read
 * from getenv() at startup so the user can override without
 * recompiling. KAI_BENCH_ITERS_DEFAULT is the compile-time fallback.
 *
 * Selfhost-bench (the compiler measuring itself) deferred to v1.y;
 * see issue #40 for the split plan.
 */

#define KAI_BENCH_ITERS_DEFAULT  1000
#define KAI_BENCH_WARMUP_DEFAULT 50

static KAI_TLS int kai_bench_count_total = 0;

static int kai_bench_iters_cached = -1;
static int kai_bench_warmup_cached = -1;

static int kai_bench_parse_int_env(const char *name, int fallback) {
    const char *raw = getenv(name);
    if (!raw || !*raw) return fallback;
    long v = 0;
    const char *p = raw;
    int sign = 1;
    if (*p == '-') { sign = -1; p++; }
    if (!*p) return fallback;
    while (*p) {
        if (*p < '0' || *p > '9') return fallback;
        v = v * 10 + (*p - '0');
        if (v > 100000000L) return fallback;
        p++;
    }
    v *= sign;
    if (v < 0) return fallback;
    return (int)v;
}

static int kai_bench_iters(void) {
    if (kai_bench_iters_cached < 0) {
        int v = kai_bench_parse_int_env("KAI_BENCH_ITERS", KAI_BENCH_ITERS_DEFAULT);
        if (v < 1) v = 1;
        kai_bench_iters_cached = v;
    }
    return kai_bench_iters_cached;
}

static int kai_bench_warmup(void) {
    if (kai_bench_warmup_cached < 0) {
        int v = kai_bench_parse_int_env("KAI_BENCH_WARMUP", KAI_BENCH_WARMUP_DEFAULT);
        if (v < 0) v = 0;
        kai_bench_warmup_cached = v;
    }
    return kai_bench_warmup_cached;
}

static long long kai_bench_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}

/* Per-bench sample buffer. One bench at a time runs to completion
   before the next one starts (the emitted main calls them in
   sequence), so a single shared buffer is enough — we just realloc
   it lazily to the high-water mark. */
static KAI_TLS long long *kai_bench_samples = NULL;
static KAI_TLS int kai_bench_samples_cap = 0;

static void kai_bench_ensure_capacity(int n) {
    if (n <= kai_bench_samples_cap) return;
    long long *r = (long long *)realloc(kai_bench_samples, (size_t)n * sizeof(long long));
    if (!r) {
        kai_eprintf("kai_bench: out of memory reserving %d samples\n", n);
        kai_exit(1);
    }
    kai_bench_samples = r;
    kai_bench_samples_cap = n;
}

static void kai_bench_record(int idx, long long sample_ns) {
    /* Caller has already ensured capacity. Out-of-range index
       silently drops — defensive against emitter bugs that miscount
       iterations. */
    if (idx < 0 || idx >= kai_bench_samples_cap) return;
    kai_bench_samples[idx] = sample_ns;
}

static int kai_bench_ll_cmp(const void *a, const void *b) {
    long long la = *(const long long *)a;
    long long lb = *(const long long *)b;
    if (la < lb) return -1;
    if (la > lb) return 1;
    return 0;
}

static long long kai_bench_median_sorted(const long long *xs, int n) {
    if (n <= 0) return 0;
    return xs[n / 2];
}

static void kai_bench_finalize(const char *desc, int iters) {
    /* Samples already populated by kai_bench_record(0..iters-1). */
    if (iters <= 0) {
        kai_eprintf("  %s: 0 iter / median 0 ns / MAD 0 ns / mean 0 ns / range [0, 0]\n",
                desc ? desc : "(unnamed)");
        kai_bench_count_total++;
        return;
    }
    long long *xs = kai_bench_samples;
    long long total = 0;
    long long mn = xs[0];
    long long mx = xs[0];
    for (int i = 0; i < iters; i++) {
        long long s = xs[i];
        total += s;
        if (s < mn) mn = s;
        if (s > mx) mx = s;
    }
    long long mean = total / (long long)iters;
    qsort(xs, (size_t)iters, sizeof(long long), kai_bench_ll_cmp);
    long long median = kai_bench_median_sorted(xs, iters);
    /* MAD: median(|x_i - median|). Reuse the sample buffer for the
       deviations — we're done with the sorted samples. */
    for (int i = 0; i < iters; i++) {
        long long d = xs[i] - median;
        if (d < 0) d = -d;
        xs[i] = d;
    }
    qsort(xs, (size_t)iters, sizeof(long long), kai_bench_ll_cmp);
    long long mad = kai_bench_median_sorted(xs, iters);
    kai_eprintf(
            "  %s: %d iter / median %lld ns / MAD %lld ns / mean %lld ns / range [%lld, %lld]\n",
            desc ? desc : "(unnamed)", iters, median, mad, mean, mn, mx);
    kai_bench_count_total++;
}

static int kai_bench_summary(void) {
    kai_eprintf("\n%d benches\n", kai_bench_count_total);
    if (kai_bench_samples) {
        free(kai_bench_samples);
        kai_bench_samples = NULL;
        kai_bench_samples_cap = 0;
    }
    return 0;
}

/* Run one bench body through warmup + timed iterations. The C-direct
   backend weaves this loop inline into each `_kai_bench_<id>`; the
   native backend emits each body as a fn returning the block's final
   boxed value and drives them here, decref'ing each result like the
   C-direct runner. No setjmp pad — an assertion inside a bench should
   panic, since timing aborted code is meaningless (`kai_assert_check`
   with `kai_test_in_progress == 0` panics, which is the wanted
   behaviour). */
static void kai_bench_run_one(const char *desc, KaiValue *(*body)(void)) {
    int iters  = kai_bench_iters();
    int warmup = kai_bench_warmup();
    int i;
    kai_bench_ensure_capacity(iters);
    for (i = 0; i < warmup; i++) kai_decref(body());
    for (i = 0; i < iters; i++) {
        long long a = kai_bench_now_ns();
        KaiValue *r = body();
        long long b = kai_bench_now_ns();
        kai_decref(r);
        kai_bench_record(i, b - a);
    }
    kai_bench_finalize(desc, iters);
}

/* ---------- check harness hooks (used by --check runs) ----------
 * check v1 (issue #44): property-based testing. Each `check "..."
 * [with p: T, ...] { body }` block is executed KAI_CHECK_ITERS
 * times. Each iteration generates random values for the with-clause
 * params via kai_arbitrary_<T>() and runs the body. The body must
 * produce a Bool — true means the property held for that input,
 * false records a counterexample and exits early.
 *
 * v1 reports the FIRST counterexample only (no shrinking). Shrinking
 * + median-based outlier-counterexample selection deferred to v1.x;
 * protocol-based generators (impl Arbitrary for T) deferred to v2
 * post-protocols maturity (see issue #44 split plan).
 */

#define KAI_CHECK_ITERS 100

static KAI_TLS int kai_check_count_total = 0;
static KAI_TLS int kai_check_count_passed = 0;
static KAI_TLS const char *kai_check_current_desc = NULL;

#define KAI_CHECK_CX_BUF 1024
static KAI_TLS char kai_check_cx_buf[KAI_CHECK_CX_BUF];
static KAI_TLS size_t kai_check_cx_len = 0;

/* xorshift64* PRNG. Seeded once per process with a constant so
   counterexample reports are reproducible run-to-run; reseed via
   KAI_CHECK_SEED env var lands in v1.x. */
static KAI_TLS uint64_t kai_check_seed = 0xC0DECAFE12345678ULL;

static uint64_t kai_check_rand_u64(void) {
    kai_check_seed ^= kai_check_seed >> 12;
    kai_check_seed ^= kai_check_seed << 25;
    kai_check_seed ^= kai_check_seed >> 27;
    return kai_check_seed * 0x2545F4914F6CDD1DULL;
}

static void kai_check_cx_reset(void) {
    kai_check_cx_buf[0] = '\0';
    kai_check_cx_len = 0;
}

static void kai_check_cx_append_raw(const char *s, size_t n) {
    if (!s || n == 0) return;
    if (kai_check_cx_len + n + 1 >= KAI_CHECK_CX_BUF) {
        n = KAI_CHECK_CX_BUF - 1 - kai_check_cx_len;
        if (n == 0) return;
    }
    memcpy(kai_check_cx_buf + kai_check_cx_len, s, n);
    kai_check_cx_len += n;
    kai_check_cx_buf[kai_check_cx_len] = '\0';
}

static void kai_check_cx_append(const char *s) {
    if (!s) return;
    kai_check_cx_append_raw(s, strlen(s));
}

/* Records "name=<repr>" in the counterexample buffer. Called by the
   emitted check fn after each kai_arbitrary_<T> generator inside the
   per-iter loop, before evaluating the body, so that on failure the
   buffer already holds every input the predicate saw. v repr uses
   kai_to_string (borrow). */
static void kai_check_record_param(const char *name, KaiValue *v) {
    if (kai_check_cx_len > 0) kai_check_cx_append(", ");
    kai_check_cx_append(name);
    kai_check_cx_append("=");
    KaiValue *s = kai_to_string(v);
    if (kai_is_ptr(s) && s->tag == KAI_STR) {
        kai_check_cx_append_raw(s->as.s.bytes, s->as.s.len);
    }
    kai_decref(s);
}

static void kai_check_begin(const char *desc) {
    kai_check_count_total++;
    kai_check_current_desc = desc;
}

static void kai_check_pass(int iters) {
    kai_check_count_passed++;
    kai_eprintf("  %s: %d iter, OK\n",
            kai_check_current_desc ? kai_check_current_desc : "(unnamed)",
            iters);
}

static void kai_check_fail(int iter_at) {
    kai_eprintf("  %s: counterexample at iter %d: %s\n",
            kai_check_current_desc ? kai_check_current_desc : "(unnamed)",
            iter_at, kai_check_cx_buf);
}

static int kai_check_summary(void) {
    kai_eprintf("\n%d/%d checks passed\n",
            kai_check_count_passed, kai_check_count_total);
    return (kai_check_count_passed == kai_check_count_total) ? 0 : 1;
}

/* Intrinsic generators (issue #44 path b). Each returns a fresh
   KaiValue * with rc=1 ready for the body to consume or drop. The
   ranges are kept small so counterexamples are human-readable
   (Int in [-50, 50], String len in [0, 10], list len in [0, 7]). */

static KaiValue *kai_arbitrary_int(void) {
    int64_t r = (int64_t)(kai_check_rand_u64() % 101) - 50;
    return kai_int(r);
}

static KaiValue *kai_arbitrary_bool(void) {
    return kai_bool((int)(kai_check_rand_u64() & 1));
}

static KaiValue *kai_arbitrary_char(void) {
    /* printable ASCII ('!' .. '~') */
    uint32_t c = (uint32_t)(0x21 + (kai_check_rand_u64() % 0x5E));
    return kai_char(c);
}

static KaiValue *kai_arbitrary_string(void) {
    int len = (int)(kai_check_rand_u64() % 11);
    char buf[16];
    for (int i = 0; i < len; i++) {
        buf[i] = (char)(0x21 + (kai_check_rand_u64() % 0x5E));
    }
    return kai_str_from_bytes(buf, (size_t) len);
}

/* List generators by element kind. v1 supports primitive elements
   only. Structural sum/record/list-of-non-primitive lands in v1.x
   together with the typer-side derivation. */
#define KAI_DEFINE_ARBITRARY_LIST(NAME, ELEM)                          \
    static KaiValue *NAME(void) {                                      \
        int len = (int)(kai_check_rand_u64() % 8);                     \
        KaiValue *acc = kai_nil();                                     \
        for (int i = 0; i < len; i++) acc = kai_cons(ELEM(), acc);     \
        return acc;                                                    \
    }
KAI_DEFINE_ARBITRARY_LIST(kai_arbitrary_list_int,    kai_arbitrary_int)
KAI_DEFINE_ARBITRARY_LIST(kai_arbitrary_list_bool,   kai_arbitrary_bool)
KAI_DEFINE_ARBITRARY_LIST(kai_arbitrary_list_char,   kai_arbitrary_char)
KAI_DEFINE_ARBITRARY_LIST(kai_arbitrary_list_string, kai_arbitrary_string)
#undef KAI_DEFINE_ARBITRARY_LIST

/* ---------- shrinkers (issue #438) ---------------------------------
 * Each kai_shrink_<T>(v) returns a NEW KaiValue* (rc=1) that is one
 * greedy step closer to the canonical minimum, or NULL when no smaller
 * candidate exists. Callers own both the input (untouched, still must
 * be decref'd by the caller as before) and the returned value.
 *
 * Strategy is greedy and per-type:
 *   Int    : halve toward 0 (sign-preserving).
 *   Bool   : true → false; false → NULL.
 *   Char   : bisect toward 'a'; if c == 'a' then NULL.
 *   String : delete first byte; else if non-empty, shrink first non-'a'
 *            byte toward 'a'; else NULL.
 *   List   : delete head element; else shrink first element via the
 *            per-T element shrinker; else NULL.
 *
 * Greedy means each step produces ONE candidate; the runner accepts it
 * only if the predicate still fails. If accepted the runner shrinks
 * again from the new candidate; if rejected the runner asks for the
 * NEXT alternative — encoded by re-calling the shrinker on the same
 * input, which is fine because each shrinker is deterministic and the
 * runner falls through to a different param after one rejection (the
 * only branch we care about for v1.x; integrated multi-strategy
 * backtracking is post-1.0 per #438 out-of-scope). */

static KaiValue *kai_shrink_int(KaiValue *v) {
    if (!kai_is_int(v)) return NULL;
    /* `v` may be a Koka-style tagged immediate (small Int) OR a heap
       KAI_INT, so read through kai_intf — a raw `v->as.i` deref would
       segv on the immediate (which has no header). */
    int64_t x = kai_intf(v);
    if (x == 0) return NULL;
    /* Halve toward zero. Integer division on negatives in C is
       truncation toward zero (C99 §6.5.5/6), so x/2 already does the
       right thing for both signs (e.g. -8/2 = -4, -1/2 = 0). */
    return kai_int(x / 2);
}

static KaiValue *kai_shrink_bool(KaiValue *v) {
    if (!v || v->tag != KAI_BOOL) return NULL;
    if (v->as.b == 0) return NULL;
    return kai_bool(0);
}

static KaiValue *kai_shrink_char(KaiValue *v) {
    if (!v || v->tag != KAI_CHAR) return NULL;
    uint32_t c = v->as.c;
    if (c == (uint32_t) 'a') return NULL;
    /* Bisect toward 'a'. Converges in O(log) steps. */
    uint32_t a = (uint32_t) 'a';
    uint32_t next = (c > a) ? (a + (c - a) / 2) : (c + (a - c) / 2);
    if (next == c) next = a;  /* defensive: ensure progress */
    return kai_char(next);
}

static KaiValue *kai_shrink_string(KaiValue *v) {
    if (!v || v->tag != KAI_STR) return NULL;
    size_t len = v->as.s.len;
    if (len > 0) {
        /* Delete first byte. */
        return kai_str_from_bytes(v->as.s.bytes + 1, len - 1);
    }
    return NULL;
}

/* List shrinker factory. Two strategies:
   A) drop head — biggest single reduction, collapses toward nil.
   B) elem shrink — walks the spine looking for the FIRST element
      that still has a smaller form via ELEM_SHRINK and rebuilds the
      list with that one element replaced. Returns NULL if every
      element is at its own minimum.
   The walk-and-replace pattern gives strategy B a deterministic
   linear sweep across the full list, not just the head, so e.g.
   [0, -30] still has a productive shrink (rebuild to [0, -15]). */
#define KAI_DEFINE_SHRINK_LIST(NAME, ELEM_SHRINK)                          \
    static KaiValue *NAME(KaiValue *v) {                                   \
        if (!v) return NULL;                                               \
        if (v->tag == KAI_NIL) return NULL;                                \
        if (v->tag != KAI_CONS) return NULL;                               \
        KaiValue *tail = v->as.cons.tail;                                  \
        kai_incref(tail);                                                  \
        return tail;                                                       \
    }                                                                      \
    /* Rebuild prefix (in reverse) onto a new tail. Helper for strategy B. \
       Each element in `rev_prefix` is a borrowed pointer and is incref'd  \
       as it lands in the new spine; the new spine takes ownership of the  \
       passed-in `tail` (no extra incref). */                              \
    static KaiValue *NAME##_rebuild(KaiValue **rev_prefix, int n,          \
                                    KaiValue *tail) {                      \
        KaiValue *acc = tail;                                              \
        for (int i = 0; i < n; i++) {                                      \
            KaiValue *h = rev_prefix[i];                                   \
            kai_incref(h);                                                 \
            acc = kai_cons(h, acc);                                        \
        }                                                                  \
        return acc;                                                        \
    }                                                                      \
    static KaiValue *NAME##_head(KaiValue *v) {                            \
        if (!v || v->tag != KAI_CONS) return NULL;                         \
        /* First pass: walk to find the first shrinkable element. */       \
        KaiValue *prefix[64];                                              \
        int prefix_n = 0;                                                  \
        KaiValue *cur = v;                                                 \
        while (cur != NULL && cur->tag == KAI_CONS) {                      \
            KaiValue *h = cur->as.cons.head;                               \
            KaiValue *h2 = ELEM_SHRINK(h);                                 \
            if (h2 != NULL) {                                              \
                /* Build new list: h2 :: cur->tail (incref'd) prepended    \
                   by the walked prefix in reverse. */                     \
                KaiValue *new_tail = cur->as.cons.tail;                    \
                kai_incref(new_tail);                                      \
                KaiValue *acc = kai_cons(h2, new_tail);                    \
                acc = NAME##_rebuild(prefix, prefix_n, acc);               \
                return acc;                                                \
            }                                                              \
            if (prefix_n >= 64) return NULL; /* bound spine walk */         \
            prefix[prefix_n++] = h;                                        \
            cur = cur->as.cons.tail;                                       \
        }                                                                  \
        return NULL;                                                       \
    }
KAI_DEFINE_SHRINK_LIST(kai_shrink_list_int,    kai_shrink_int)
KAI_DEFINE_SHRINK_LIST(kai_shrink_list_bool,   kai_shrink_bool)
KAI_DEFINE_SHRINK_LIST(kai_shrink_list_char,   kai_shrink_char)
KAI_DEFINE_SHRINK_LIST(kai_shrink_list_string, kai_shrink_string)
#undef KAI_DEFINE_SHRINK_LIST

/* Per-process shrink-iteration cap. KAI_CHECK_SHRINK_ITERS in the env
   overrides; default 200 per #438. Read once and memoised so we don't
   pay getenv cost per shrink step. */
#define KAI_CHECK_SHRINK_ITERS_DEFAULT 200
static int kai_check_shrink_iters_cached = -1;
static int kai_check_shrink_iters_limit(void) {
    if (kai_check_shrink_iters_cached >= 0) return kai_check_shrink_iters_cached;
    const char *env = getenv("KAI_CHECK_SHRINK_ITERS");
    int n = KAI_CHECK_SHRINK_ITERS_DEFAULT;
    if (env && *env) {
        int parsed = atoi(env);
        if (parsed >= 0) n = parsed;
    }
    kai_check_shrink_iters_cached = n;
    return n;
}

/* Counterexample-buffer side channel for shrinking. When a failure
   triggers shrinking, the runner first snapshots the original cx
   buffer here so the final report can show "<orig> shrunk to <min>".
   Bounded by the same KAI_CHECK_CX_BUF cap as the live buffer. */
static KAI_TLS char kai_check_orig_cx_buf[KAI_CHECK_CX_BUF];
static KAI_TLS int  kai_check_has_orig_cx = 0;

static void kai_check_cx_save_orig(void) {
    size_t n = kai_check_cx_len;
    if (n >= KAI_CHECK_CX_BUF) n = KAI_CHECK_CX_BUF - 1;
    memcpy(kai_check_orig_cx_buf, kai_check_cx_buf, n);
    kai_check_orig_cx_buf[n] = '\0';
    kai_check_has_orig_cx = 1;
}

/* Reports a shrunk counterexample. If the shrink loop produced a
   strictly smaller value, prints both forms; otherwise (no progress)
   degrades to the v1 single-form output so noise stays low. */
static void kai_check_fail_shrunk(int iter_at) {
    if (kai_check_has_orig_cx
        && strcmp(kai_check_orig_cx_buf, kai_check_cx_buf) != 0) {
        kai_eprintf(
                "  %s: counterexample at iter %d: %s, shrunk to %s\n",
                kai_check_current_desc ? kai_check_current_desc : "(unnamed)",
                iter_at, kai_check_orig_cx_buf, kai_check_cx_buf);
    } else {
        kai_eprintf("  %s: counterexample at iter %d: %s\n",
                kai_check_current_desc ? kai_check_current_desc : "(unnamed)",
                iter_at, kai_check_cx_buf);
    }
    kai_check_has_orig_cx = 0;
}
