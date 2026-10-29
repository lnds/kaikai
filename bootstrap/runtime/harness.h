/* Fragment of runtime.h; include runtime.h, never this file. */
/* ---------- test harness hooks (used by --test runs) ----------
 *
 * Two report formats share one set of counters. The human format is the
 * default and is byte-for-byte what it has always been; NDJSON is opt-in
 * through KAI_TEST_JSON.
 *
 * Records go to fd 3 when the caller opens it, and only otherwise to
 * stdout: a test body's own `Stdout.print` shares stdout with the report,
 * and interleaved prose makes the stream unparseable. The driver opens
 * fd 3 for `--json`, so the records always arrive on a clean channel.
 *
 * Selection (KAI_TEST_ONLY) filters by the same id the JSON emits,
 * `<file>:<desc>`. A deselected block is not begun, so it is absent from
 * both the records and the counters.
 */

static KAI_TLS int         kai_test_count_total  = 0;
static KAI_TLS int         kai_test_count_passed = 0;
static KAI_TLS const char *kai_test_current      = NULL;
static KAI_TLS const char *kai_test_current_file = NULL;
static KAI_TLS int         kai_test_current_line = 0;
static KAI_TLS long long   kai_test_started_ns   = 0;
static KAI_TLS long long   kai_test_suite_ns     = 0;
static KAI_TLS int         kai_test_reported     = 0;
static KAI_TLS jmp_buf     kai_test_jmp;
static KAI_TLS int         kai_test_in_progress  = 0;
static KAI_TLS uintptr_t   kai_test_frame        = 0;
static KAI_TLS uint32_t    kai_test_unw_mark     = 0;

/* The runtime's cancel paths live in another translation unit when the
 * program links per module, so they reach the running block through this
 * shared pointer, not through this unit's own harness state. */
#if defined(KAI_SEPARATE_COMPILATION)
extern void (*kai_test_escape_hook)(void);
#  if defined(KAI_RUNTIME_OWNER)
void (*kai_test_escape_hook)(void) = NULL;
#  endif
#else
static void (*kai_test_escape_hook)(void) = NULL;
#endif

static long long kai_test_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}

static int kai_test_json_mode(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *raw = getenv("KAI_TEST_JSON");
        cached = (raw && *raw && strcmp(raw, "0") != 0) ? 1 : 0;
    }
    return cached;
}

/* Where records land. fd 3 when the caller opened it, else stdout. */
static FILE *kai_test_json_out(void) {
    static FILE *cached = NULL;
    if (!cached) {
        if (fcntl(3, F_GETFD) != -1) cached = fdopen(3, "w");
        if (!cached) cached = stdout;
    }
    return cached;
}

/* The file the running blocks were declared in. One test binary is built
   per source file, so this is per-process, not per-block. The driver
   supplies the user-facing path: the native paths compile a COPY of the
   entry, so the path baked in at compile time would name a scratch file
   the user cannot click. */
static const char *kai_test_file(void) {
    if (!kai_test_current_file) {
        const char *env = getenv("KAI_TEST_FILE");
        kai_test_current_file = (env && *env) ? env : "";
    }
    return kai_test_current_file;
}

static void kai_test_json_bytes(FILE *o, const char *s) {
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        switch (*p) {
            case '"':  fputs("\\\"", o); break;
            case '\\': fputs("\\\\", o); break;
            case '\n': fputs("\\n", o);  break;
            case '\r': fputs("\\r", o);  break;
            case '\t': fputs("\\t", o);  break;
            default:
                if (*p < 0x20) fprintf(o, "\\u%04x", *p);
                else           fputc((char)*p, o);
        }
    }
}

static void kai_test_json_str(FILE *o, const char *s) {
    fputc('"', o);
    kai_test_json_bytes(o, s);
    fputc('"', o);
}

/* `<file>:<desc>` — stable across runs for an unchanged block, and the
   exact spelling KAI_TEST_ONLY matches against. */
static void kai_test_json_id(FILE *o) {
    fputc('"', o);
    kai_test_json_bytes(o, kai_test_file());
    fputc(':', o);
    kai_test_json_bytes(o, kai_test_current ? kai_test_current : "");
    fputc('"', o);
}

/* ---------- per-block line coverage (`kai test --coverage`) ----------
 *
 * A coverage build opens each region of the package's code with
 * kai_cov_hit(site): `site` is a static "<line>,...:<file>" string whose
 * address is the probe's identity. The sites reached since a block began
 * are reported, as lines per file, right after its test record. */
KAI_SCHED_FN void kai_cov_hit(const char *site);
KAI_SCHED_FN void kai_cov_reset(void);
KAI_SCHED_FN void kai_cov_take(FILE *o);

#if !KAI_SCHED_DECL_ONLY
static pthread_mutex_t kai_cov_mu = PTHREAD_MUTEX_INITIALIZER;
/* The sites reached: open addressing by address, NULL a free slot. */
static const char **kai_cov_set;
static size_t kai_cov_cap;
static size_t kai_cov_len;
/* Sites known to be in the set, by hash, so a repeat hit skips the lock.
 * Cleared with the set, under the lock. */
#define KAI_COV_RECENT 4096
static const char *kai_cov_recent[KAI_COV_RECENT];

static size_t kai_cov_hash(const char *site) {
    uint64_t x = (uint64_t) (uintptr_t) site;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return (size_t) x;
}

static void kai_cov_insert(const char *site) {
    size_t mask = kai_cov_cap - 1;
    for (size_t i = kai_cov_hash(site) & mask; ; i = (i + 1) & mask) {
        if (kai_cov_set[i] == site) return;
        if (!kai_cov_set[i]) { kai_cov_set[i] = site; kai_cov_len++; return; }
    }
}

static void kai_cov_grow(void) {
    const char **old = kai_cov_set;
    size_t n = kai_cov_cap;
    kai_cov_cap = n ? n * 2 : 1024;
    kai_cov_set = (const char **) calloc(kai_cov_cap, sizeof *kai_cov_set);
    if (!kai_cov_set) { kai_eprintf("kai: out of memory recording coverage\n"); kai_exit(2); }
    kai_cov_len = 0;
    for (size_t i = 0; i < n; i++) if (old[i]) kai_cov_insert(old[i]);
    free(old);
}

KAI_SCHED_FN void kai_cov_hit(const char *site) {
    const char **r = &kai_cov_recent[kai_cov_hash(site) % KAI_COV_RECENT];
    if (__atomic_load_n(r, __ATOMIC_RELAXED) == site) return;
    pthread_mutex_lock(&kai_cov_mu);
    if (2 * (kai_cov_len + 1) > kai_cov_cap) kai_cov_grow();
    kai_cov_insert(site);
    __atomic_store_n(r, site, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&kai_cov_mu);
}

static void kai_cov_clear_locked(void) {
    if (kai_cov_set) memset(kai_cov_set, 0, kai_cov_cap * sizeof *kai_cov_set);
    kai_cov_len = 0;
    for (size_t i = 0; i < KAI_COV_RECENT; i++) __atomic_store_n(&kai_cov_recent[i], NULL, __ATOMIC_RELAXED);
}

KAI_SCHED_FN void kai_cov_reset(void) {
    pthread_mutex_lock(&kai_cov_mu);
    kai_cov_clear_locked();
    pthread_mutex_unlock(&kai_cov_mu);
}

typedef struct { const char *file; long line; } KaiCovLine;

static int kai_cov_line_cmp(const void *a, const void *b) {
    const KaiCovLine *x = (const KaiCovLine *) a, *y = (const KaiCovLine *) b;
    int c = strcmp(x->file, y->file);
    return c ? c : (x->line > y->line) - (x->line < y->line);
}

static size_t kai_cov_site_count(const char *site) {
    size_t n = 1;
    for (const char *p = site; *p && *p != ':'; p++) n += *p == ',';
    return n;
}

/* The lines of `site` appended at `out[n]`; the new count. */
static size_t kai_cov_site_lines(const char *site, KaiCovLine *out, size_t n) {
    const char *file = strchr(site, ':');
    file = file ? file + 1 : "";
    for (const char *p = site; *p && *p != ':'; ) {
        char *end;
        long line = strtol(p, &end, 10);
        out[n].file = file;
        out[n].line = line;
        n++;
        p = *end == ',' ? end + 1 : end;
    }
    return n;
}

/* Write the reached lines as `{"<file>":[<line>,...],...}`, sorted, and start over. */
KAI_SCHED_FN void kai_cov_take(FILE *o) {
    pthread_mutex_lock(&kai_cov_mu);
    size_t total = 0, n = 0;
    for (size_t i = 0; i < kai_cov_cap; i++) if (kai_cov_set[i]) total += kai_cov_site_count(kai_cov_set[i]);
    KaiCovLine *ls = (KaiCovLine *) malloc((total ? total : 1) * sizeof *ls);
    if (!ls) { kai_eprintf("kai: out of memory reporting coverage\n"); kai_exit(2); }
    for (size_t i = 0; i < kai_cov_cap; i++) if (kai_cov_set[i]) n = kai_cov_site_lines(kai_cov_set[i], ls, n);
    kai_cov_clear_locked();
    pthread_mutex_unlock(&kai_cov_mu);
    qsort(ls, n, sizeof *ls, kai_cov_line_cmp);
    fputc('{', o);
    for (size_t i = 0; i < n; i++) {
        int same_file = i > 0 && strcmp(ls[i].file, ls[i - 1].file) == 0;
        if (same_file && ls[i].line == ls[i - 1].line) continue;
        if (same_file) {
            fputc(',', o);
        } else {
            if (i > 0) fputs("],", o);
            kai_test_json_str(o, ls[i].file);
            fputs(":[", o);
        }
        fprintf(o, "%ld", ls[i].line);
    }
    if (n > 0) fputc(']', o);
    fputc('}', o);
    free(ls);
}
#endif

static int kai_test_coverage_mode(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *raw = getenv("KAI_TEST_COVERAGE");
        cached = (raw && *raw && strcmp(raw, "0") != 0) ? 1 : 0;
    }
    return cached;
}

static void kai_test_json_record(const char *status, const char *msg) {
    long long ms = (kai_test_now_ns() - kai_test_started_ns) / 1000000LL;
    FILE *o = kai_test_json_out();
    fputs("{\"type\":\"test\",\"id\":", o);
    kai_test_json_id(o);
    fputs(",\"file\":", o);
    kai_test_json_str(o, kai_test_file());
    fprintf(o, ",\"line\":%d,\"status\":", kai_test_current_line);
    kai_test_json_str(o, status);
    fprintf(o, ",\"duration_ms\":%lld", ms < 0 ? 0 : ms);
    if (msg) {
        fputs(",\"message\":", o);
        kai_test_json_str(o, msg);
    }
    fputs("}\n", o);
    if (kai_test_coverage_mode()) {
        fputs("{\"type\":\"coverage\",\"id\":", o);
        kai_test_json_id(o);
        fputs(",\"lines\":", o);
        kai_cov_take(o);
        fputs("}\n", o);
    }
    fflush(o);
}

/* Whether a block runs at all. KAI_TEST_ONLY is a newline-separated list
   of ids; unset means every block runs. The needle is matched whole, so
   one id is never a prefix of another's match. */
static int kai_test_selected(const char *file, const char *desc) {
    const char *only = getenv("KAI_TEST_ONLY");
    if (!only || !*only) return 1;
    size_t flen = strlen(file ? file : "");
    size_t dlen = strlen(desc ? desc : "");
    for (const char *p = only; *p; ) {
        const char *nl = strchr(p, '\n');
        size_t seg = nl ? (size_t)(nl - p) : strlen(p);
        if (seg == flen + 1 + dlen &&
            strncmp(p, file ? file : "", flen) == 0 &&
            p[flen] == ':' &&
            strncmp(p + flen + 1, desc ? desc : "", dlen) == 0) {
            return 1;
        }
        if (!nl) break;
        p = nl + 1;
    }
    return 0;
}

static void kai_test_cancel_escape_here(void);

static void kai_test_begin(const char *desc) {
    kai_test_count_total++;
    kai_test_current = desc;
    kai_test_escape_hook = kai_test_cancel_escape_here;
    kai_test_reported = 0;
    if (kai_test_json_mode() && kai_test_coverage_mode()) kai_cov_reset();
    kai_test_started_ns = kai_test_now_ns();
}

/* The declaration line of the block about to run, stamped by both
   emitters right before the block's begin/run_one. */
static void kai_test_line(int line) { kai_test_current_line = line; }

/* A longjmp out of a test body that reported nothing came from a failure
   path carrying no message of its own; the record must still exist. */
static void kai_test_unreported_fail(void) {
    if (kai_test_json_mode() && !kai_test_reported) {
        kai_test_json_record("fail", "assertion failed");
        kai_test_reported = 1;
    }
}

static void kai_test_pass(void) {
    kai_test_count_passed++;
    if (kai_test_json_mode()) { kai_test_json_record("pass", NULL); kai_test_reported = 1; return; }
    kai_eprintf("  ok   %s\n", kai_test_current ? kai_test_current : "");
}

__attribute__((noinline))
static void kai_test_fail(const char *desc, const char *msg) {
    if (kai_test_json_mode()) {
        kai_test_json_record("fail", msg ? msg : "assertion failed");
        kai_test_reported = 1;
        return;
    }
    kai_eprintf("  FAIL %s : %s\n",
            desc ? desc : "",
            msg  ? msg  : "assertion failed");
}

static void kai_test_land(void *arg) {
    (void) arg;
    longjmp(kai_test_jmp, 1);
}

KAI_RT_SHARED void kai_unw_release_to(KaiFiber *f, uint32_t mark);

/* A failed assertion leaves the test body through the unwinder, releasing
 * the body's unwind entries first, while the frames they point into live. */
__attribute__((noreturn))
static void kai_test_unwind(void) {
    kai_unw_release_to(kai_current_fiber(), kai_test_unw_mark);
    kai_trap_unwind(kai_test_frame, NULL, kai_test_land, NULL);
}

static int kai_test_summary(void) {
    int failed = kai_test_count_total - kai_test_count_passed;
    if (kai_test_json_mode()) {
        FILE *o = kai_test_json_out();
        fprintf(o, "{\"type\":\"summary\",\"passed\":%d,\"failed\":%d,\"duration_ms\":%lld}\n",
                kai_test_count_passed, failed, kai_test_suite_ns / 1000000LL);
        fflush(o);
    } else {
        kai_eprintf("\n%d/%d tests passed\n",
                kai_test_count_passed, kai_test_count_total);
    }
    return failed == 0 ? 0 : 1;
}

/* Run one test body through the begin/setjmp/pass landing pad. The
   C-direct backend weaves this same shape inline into each emitted
   `_kai_test_<id>`; the native backend cannot emit a generated `int
   main`, so it emits each test body as a plain fn and drives them
   through this helper. The setjmp landing lives HERE, not in `body` —
   so `kai_assert_check`'s longjmp on a failed assertion unwinds into a
   frame still on the stack, and the body fn itself stays
   mem2reg-promotable (no setjmp in its IR). `body` returns the block's
   final value (a boxed `KaiValue *`), decref'd here exactly as the
   C-direct runner's `kai_decref(_body)`. */
static void kai_test_run_one(const char *desc, KaiValue *(*body)(void)) {
    if (!kai_test_selected(kai_test_file(), desc)) return;
    long long t0 = kai_test_now_ns();
    kai_test_begin(desc);
    kai_test_frame = (uintptr_t) __builtin_frame_address(0);
    kai_test_unw_mark = kai_current_fiber()->unw_top;
    if (setjmp(kai_test_jmp) == 0) {
        kai_test_in_progress = 1;
        KaiValue *r = body();
        kai_decref(r);
        kai_test_in_progress = 0;
        kai_test_pass();
    } else {
        kai_test_in_progress = 0;
        kai_test_unreported_fail();
    }
    kai_test_suite_ns += kai_test_now_ns() - t0;
}

/* Test blocks register before any runs. A grouped build links several test
   files into one binary and runs it once per file: KAI_TEST_HOMES then names,
   in run order, the modules whose blocks that file's own build would run.
   Unset, every block runs in declaration order. The C backend registers its
   self-contained `_kai_test_<id>`; the native one a body for run_one. */
typedef struct {
    void (*run)(void);
    const char *desc;
    KaiValue *(*body)(void);
    int line;
    const char *home;
} KaiTestEntry;

static KAI_TLS KaiTestEntry *kai_test_entries = NULL;
static KAI_TLS int kai_test_nentries = 0;

static void kai_test_register(void (*run)(void), const char *desc,
                              KaiValue *(*body)(void), int line, const char *home) {
    if ((kai_test_nentries & (kai_test_nentries - 1)) == 0) {
        int cap = kai_test_nentries == 0 ? 16 : kai_test_nentries * 2;
        KaiTestEntry *grown = (KaiTestEntry *)realloc(kai_test_entries, (size_t)cap * sizeof(KaiTestEntry));
        if (!grown) { kai_eprintf("kai: out of memory registering tests\n"); kai_exit(2); }
        kai_test_entries = grown;
    }
    KaiTestEntry e = { run, desc, body, line, home ? home : "" };
    kai_test_entries[kai_test_nentries++] = e;
}

static void kai_test_run_entry(const KaiTestEntry *e) {
    if (e->run) { e->run(); return; }
    kai_test_line(e->line);
    kai_test_run_one(e->desc, e->body);
}

static void kai_test_run_home(const char *home, size_t len) {
    for (int i = 0; i < kai_test_nentries; i++) {
        const char *h = kai_test_entries[i].home;
        if (strlen(h) == len && strncmp(h, home, len) == 0) kai_test_run_entry(&kai_test_entries[i]);
    }
}

/* The list is read once and removed, so a program the tests start does not inherit it. */
static void kai_test_drive(void) {
    const char *env = getenv("KAI_TEST_HOMES");
    if (!env || !*env) {
        for (int i = 0; i < kai_test_nentries; i++) kai_test_run_entry(&kai_test_entries[i]);
        return;
    }
    char *homes = strdup(env);
    unsetenv("KAI_TEST_HOMES");
    for (const char *p = homes; *p; ) {
        while (*p == ' ') p++;
        const char *end = p;
        while (*end && *end != ' ') end++;
        if (end > p) kai_test_run_home(p, (size_t)(end - p));
        p = end;
    }
    free(homes);
}

/* Runtime-aware assert: called from every emitted `assert`. Inside an
   active test (kai_test_in_progress) it prints a failure and longjmps
   back to the test harness so the next test can run. Otherwise it
   aborts the process via kai_core_panic, matching stage 0's
   non-test-mode behaviour. */

/* Whether this thread is inside a test block, and which. Out of line: an
 * assert's decref can switch context before the check runs. */
__attribute__((noinline))
static int kai_test_running(const char **desc) {
    *desc = kai_test_current;
    return kai_test_in_progress;
}

/* A cancellation with no handler and no fiber pad fails the running test
 * block like a failed assertion. */
static void kai_test_cancel_escape_here(void) {
    const char *test;
    if (!kai_test_running(&test)) return;
    kai_test_fail(test, "unhandled cancellation (Cancel.raise escaped the test)");
    kai_test_unwind();
}

static void kai_test_cancel_escape(void) {
    if (kai_test_escape_hook) kai_test_escape_hook();
}

static void kai_assert_check(KaiValue *cond, const char *msg) {
    int ok = kai_op_truthy(cond);
    kai_decref(cond);
    if (ok) return;
    const char *test;
    if (kai_test_running(&test)) {
        kai_test_fail(test, msg ? msg : "assertion failed");
        kai_test_unwind();
    } else {
        kai_core_panic(kai_str(msg ? msg : "assertion failed"));
    }
}

/* Issue #86 (piece 2): contract-violation assert that appends the
   runtime value of the offending binding to the panic message. Used
   by the refinement-desugar path when the predicate has the simple
   shape `<ident> <binop> <literal>` and the emitter could statically
   resolve the single ident's local. `base_msg` is the predicate-aware
   piece-1 context ("requires violated in `<fn>`\nrequired: ..."); the
   appended line is "\nargument <ident_name> was: <value>".

   Ownership: `cond` and `val` are both CONSUMED (decref'd on every
   path). The call site passes a fresh owned ref for `val` — a boxed
   local is forwarded via `kai_incref`, a raw-unboxed scalar via a
   fresh `kai_real`/`kai_int` box. `kai_to_string` is a borrow on its
   argument, so we decref `val` ourselves once the string is built.
   Every owned temp (`vs`, `full`, `val`) is released before the
   longjmp so a failure inside an active test block does not leak
   (KAI_TRACE_RC / ASAN gate the lane on this). */
static void kai_assert_check_with_value(KaiValue *cond, const char *base_msg,
                                        const char *ident_name, KaiValue *val) {
    int ok = kai_op_truthy(cond);
    kai_decref(cond);
    if (ok) { kai_decref(val); return; }
    KaiValue *vs   = kai_to_string(val);
    kai_decref(val);
    KaiValue *m0   = kai_str(base_msg ? base_msg : "assertion failed");
    KaiValue *m1   = kai_string_concat(m0, kai_str("\nargument "));
    KaiValue *m2   = kai_string_concat(m1, kai_str(ident_name ? ident_name : "?"));
    KaiValue *m3   = kai_string_concat(m2, kai_str(" was: "));
    KaiValue *full = kai_string_concat(m3, vs);
    kai_decref(m0); kai_decref(m1); kai_decref(m2); kai_decref(m3); kai_decref(vs);
    const char *test;
    if (kai_test_running(&test)) {
        kai_test_fail(test, full->as.s.bytes);
        kai_decref(full);
        kai_test_unwind();
    } else {
        kai_core_panic(full);
    }
}

