/* Stack segments driven from C, with no compiled kaikai code.
 *
 *   segment_runtime_test            every check, KAI_THREADS as set
 *   segment_runtime_test bench      round-trip and creation costs
 *   segment_runtime_test twice      resumes one continuation twice (traps)
 *
 * Built like a program: this TU under KAI_SEPARATE_COMPILATION, the runtime
 * owner beside it, so the segment API runs from the owner object.
 */

#include "runtime.h"
#include <pthread.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>

#if defined(KAI_SEG_SWITCH)

static int failed = 0;

static void check(const char *what, int cond) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failed = 1;
    }
}

static KaiValue *mk_body(KaiFn fn, KaiValue *cap) {
    return cap ? kai_closure(fn, 0, 1, &cap) : kai_closure(fn, 0, 0, NULL);
}

static int64_t cap_int(KaiValue *self) {
    return kai_intf(self->as.clo.captures[0]);
}

/* ---------- generator ---------- */

static KaiValue *gen_body(KaiValue *self, KaiValue **args, int n) {
    (void) args; (void) n;
    int64_t count = cap_int(self);
    for (int64_t i = 0; i < count; i++) kai_decref(kai_seg_suspend(kai_int(i)));
    return kai_int(-1);
}

static void test_generator(int64_t count) {
    KaiValue *cap = kai_int(count);
    KaiValue *k = NULL;
    KaiValue *v = kai_seg_start(mk_body(gen_body, cap), &k);
    kai_decref(cap);
    int64_t expect = 0, wrong = 0;
    while (k != NULL) {
        if (kai_intf(v) != expect) wrong++;
        expect++;
        kai_decref(v);
        v = kai_seg_resume(k, kai_unit(), &k);
    }
    check("generator yields every value in order", wrong == 0 && expect == count);
    check("generator returns its result", kai_intf(v) == -1);
    kai_decref(v);
}

/* ---------- discontinue ---------- */

static int cleanups = 0;
static KaiValue *count_cleanup(void *env) { (void) env; cleanups++; return NULL; }

/* Pushes a handler with a `finally`, registers an owned slot across the
 * suspend, and never finishes on its own. */
static KaiValue *held_body(KaiValue *self, KaiValue **args, int n) {
    (void) args; (void) n;
    KaiEvidence node;
    int handler = 0;
    kai_evidence_push(&node, "Res", &handler);
    kai_evidence_set_cleanup(&node, count_cleanup, NULL);
    KaiValue *held[1] = { kai_incref(self->as.clo.captures[0]) };
    KaiFiber *uf = kai_unw_push(held, 1);
    kai_decref(kai_seg_suspend(kai_unit()));
    kai_unw_pop(uf);
    kai_decref(held[0]);
    kai_evidence_run_cleanup(&node);
    kai_evidence_pop();
    return kai_unit();
}

static void test_discontinue(int by_drop) {
    KaiValue *res = kai_str_from_bytes("resource", 8);
    KaiFiber *f = kai_current_fiber();
    KaiEvidence *top = f->evidence_top;
    uint32_t unw = f->unw_top;
    cleanups = 0;
    KaiValue *k = NULL;
    KaiValue *v = kai_seg_start(mk_body(held_body, res), &k);
    kai_decref(v);
    check("held body suspended", k != NULL);
    check("suspend restores the resumer's evidence", f->evidence_top == top);
    check("suspend restores the resumer's unwind stack", f->unw_top == unw);
    check("suspended segment holds its slot", kai_rc_load(res) == 3);
    if (by_drop) kai_decref(k);
    else kai_seg_discontinue(k);
    check("discontinue runs the finally exactly once", cleanups == 1);
    check("discontinue releases the held slot", kai_rc_load(res) == 1);
    check("discontinue leaves the resumer's evidence", f->evidence_top == top);
    check("discontinue leaves the resumer's unwind stack", f->unw_top == unw);
    kai_decref(res);
}

/* ---------- re-parenting ---------- */

static KaiValue *lookup_body(KaiValue *self, KaiValue **args, int n) {
    (void) self; (void) args; (void) n;
    KaiEvidence node;
    int inner = 1;
    kai_evidence_push(&node, "Inner", &inner);
    int64_t seen = 0;
    for (int round = 0; round < 2; round++) {
        kai_decref(kai_seg_suspend(kai_unit()));
        int *outer = (int *) kai_evidence_lookup("Outer");
        seen = seen * 10 + (outer ? *outer : 0);
    }
    check("inner handler still resolves", kai_evidence_lookup("Inner") == &inner);
    kai_evidence_pop();
    return kai_int(seen);
}

/* Each resume hangs the segment's chain from the resumer's current top. */
static void test_reparent(void) {
    KaiValue *k = NULL;
    kai_decref(kai_seg_start(mk_body(lookup_body, NULL), &k));
    KaiEvidence a, b;
    int ha = 7, hb = 8;
    kai_evidence_push(&a, "Outer", &ha);
    kai_decref(kai_seg_resume(k, kai_unit(), &k));
    kai_evidence_pop();
    kai_evidence_push(&b, "Outer", &hb);
    KaiValue *v = kai_seg_resume(k, kai_unit(), &k);
    kai_evidence_pop();
    check("lookups see each resumer's handlers", k == NULL && kai_intf(v) == 78);
    kai_decref(v);
}

static KaiValue *dispatch_body(KaiValue *self, KaiValue **args, int n) {
    (void) self; (void) args; (void) n;
    KaiEvidence *node = kai_evidence_lookup_node("Outer");
    return kai_int(node ? *(int *) node->handler : 0);
}

/* A segment started from inside a clause runs outside that clause's handler. */
static void test_inherits_dispatch(void) {
    KaiFiber *f = kai_current_fiber();
    KaiEvidence outer, inner;
    int ho = 1, hi = 2;
    kai_evidence_push(&outer, "Outer", &ho);
    kai_evidence_push(&inner, "Outer", &hi);
    KaiEvidence *saved = f->in_dispatch_node;
    f->in_dispatch_node = &inner;
    KaiValue *k = NULL;
    KaiValue *v = kai_seg_start(mk_body(dispatch_body, NULL), &k);
    check("a segment skips the handler whose clause started it", k == NULL && kai_intf(v) == 1);
    check("the resumer's in-dispatch node survives", f->in_dispatch_node == &inner);
    f->in_dispatch_node = saved;
    kai_decref(v);
    kai_evidence_pop();
    kai_evidence_pop();
}

/* ---------- abandon to a handle outside the segment ---------- */

static KaiEvidence *abandon_target;

static KaiValue *abandon_body(KaiValue *self, KaiValue **args, int n) {
    (void) self; (void) args; (void) n;
    KaiEvidence node;
    int h = 0;
    kai_evidence_push(&node, "Res", &h);
    kai_evidence_set_cleanup(&node, count_cleanup, NULL);
    /* The op-site discard sequence the backends emit. */
    *abandon_target->discard_slot = kai_int(42);
    kai_evidence_unwind_to(abandon_target);
    _longjmp(*abandon_target->handle_jmp, 1);
}

static void test_abandon_escapes(void) {
    KaiFiber *f = kai_current_fiber();
    KaiEvidence *top = f->evidence_top;
    jmp_buf jb;
    KaiEvidence node;
    KaiValue *volatile discard = NULL;
    int h = 0;
    cleanups = 0;
    if (_setjmp(jb) == 0) {
        kai_evidence_push_with_jmp(&node, "Abort", &h, &jb, (KaiValue **) &discard);
        abandon_target = &node;
        KaiValue *k = NULL;
        (void) kai_seg_start(mk_body(abandon_body, NULL), &k);
        check("abandon does not return through the segment", 0);
    }
    check("abandon lands on the outer handle", discard != NULL && kai_intf(discard) == 42);
    check("abandon runs the segment's finally", cleanups == 1);
    check("abandon unwinds the evidence past the handle", f->evidence_top == top);
}

/* ---------- a handle that keeps its continuation ---------- */

static KaiValue *gen_yield_body(KaiValue *self, KaiValue **args, int n) {
    (void) args; (void) n;
    int64_t count = cap_int(self);
    KaiEvidence node;
    int h = 0;
    kai_evidence_push(&node, "Res", &h);
    kai_evidence_set_cleanup(&node, count_cleanup, NULL);
    for (int64_t i = 0; i < count; i++) {
        KaiValue *a[1] = { kai_int(i) };
        kai_decref(kai_seg_request(0, 1, a));
    }
    kai_evidence_run_cleanup(&node);
    kai_evidence_pop();
    return kai_int(-1);
}

/* yield(x, k) -> [x, k]: the continuation leaves its clause. */
static KaiValue *keep_clause(KaiValue *self, KaiValue **args, int n) {
    (void) self; (void) n;
    KaiValue *pair = kai_array_make(2, kai_unit());
    pair->as.arr.items[0] = args[0];
    pair->as.arr.items[1] = args[1];
    return pair;
}

static KaiValue *identity_clause(KaiValue *self, KaiValue **args, int n) {
    (void) self; (void) n;
    return args[0];
}

static KaiValue *gen_handle(int64_t count) {
    KaiValue *cap = kai_int(count);
    KaiValue *clauses[1] = { kai_closure(keep_clause, 2, 0, NULL) };
    KaiValue *r = kai_seg_handle(mk_body(gen_yield_body, cap), kai_closure(identity_clause, 1, 0, NULL), 1, clauses);
    kai_decref(cap);
    return r;
}

/* Calls each kept continuation from outside the handle, `take` times. */
static KaiValue *gen_take(KaiValue *r, int64_t take, int64_t *seen, int64_t *wrong) {
    while (kai_is_ptr(r) && r->tag == KAI_ARRAY && *seen < take) {
        if (kai_intf(r->as.arr.items[0]) != *seen) (*wrong)++;
        (*seen)++;
        KaiValue *k = kai_incref(r->as.arr.items[1]);
        kai_decref(r);
        KaiValue *argv[1] = { kai_unit() };
        r = kai_apply(k, 1, argv);
    }
    return r;
}

static void test_handle_keeps_continuation(void) {
    int64_t seen = 0, wrong = 0;
    cleanups = 0;
    KaiValue *r = gen_take(gen_handle(1000000), 1000000, &seen, &wrong);
    check("a kept continuation yields every value in order", seen == 1000000 && wrong == 0);
    check("the handle returns through its return clause", !kai_is_ptr(r) || r->tag != KAI_ARRAY);
    check("the finished body ran its finally once", cleanups == 1);
    kai_decref(r);

    seen = 0; wrong = 0; cleanups = 0;
    r = gen_take(gen_handle(1000000), 3, &seen, &wrong);
    check("a dropped generator stopped where it was", seen == 3 && r->tag == KAI_ARRAY);
    kai_decref(r);
    check("dropping a kept continuation runs the finally once", cleanups == 1);
}

/* ---------- double resume ---------- */

static KaiValue *once_body(KaiValue *self, KaiValue **args, int n) {
    (void) self; (void) args; (void) n;
    kai_decref(kai_seg_suspend(kai_unit()));
    kai_decref(kai_seg_suspend(kai_unit()));
    return kai_unit();
}

static int run_twice(void) {
    KaiValue *k = NULL;
    kai_decref(kai_seg_start(mk_body(once_body, NULL), &k));
    kai_incref(k);
    KaiValue *k2 = NULL;
    kai_decref(kai_seg_resume(k, kai_unit(), &k2));
    kai_decref(kai_seg_resume(k, kai_unit(), &k2));
    return 0;
}

static void test_double_resume_traps(const char *self_path) {
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "KAI_THREADS=1 '%s' twice 2>&1", self_path);
    FILE *p = popen(cmd, "r");
    char out[512] = {0};
    size_t got = p ? fread(out, 1, sizeof out - 1, p) : 0;
    int st = p ? pclose(p) : -1;
    (void) got;
    check("a second resume traps", WIFEXITED(st) && WEXITSTATUS(st) != 0
          && strstr(out, "continuation resumed twice") != NULL);
}

/* ---------- M:N: a suspended segment migrates with its fiber ---------- */

#define MN_FIBERS 16
#define MN_ROUNDS 400

static _Atomic long mn_wrong = 0;
static _Atomic int  probe_captured = 0;
static _Atomic int  probe_moved = 0;
static int          probe_started_on = -1;

static KaiValue *tid_body(KaiValue *self, KaiValue **args, int n) {
    (void) self; (void) args; (void) n;
    for (int64_t i = 0; i < MN_ROUNDS; i++) kai_decref(kai_seg_suspend(kai_int(i)));
    return kai_unit();
}

/* Suspends and resumes across yields while other fibers come and go. */
static KaiValue *mn_fiber(KaiValue *self, KaiValue **args, int n) {
    (void) self; (void) args; (void) n;
    KaiValue *k = NULL;
    KaiValue *v = kai_seg_start(mk_body(tid_body, NULL), &k);
    int64_t expect = 0;
    while (k != NULL) {
        if (kai_intf(v) != expect++) atomic_fetch_add(&mn_wrong, 1);
        kai_decref(v);
        KaiCont kc;
        kai_cont_init_identity(&kc, 0);
        kai_default_spawn_yield(NULL, &kc);
        v = kai_seg_resume(k, kai_unit(), &k);
    }
    kai_decref(v);
    return kai_unit();
}

/* glibc declares pthread_self `const`, so a direct call is folded across the
 * suspend; a call through a volatile pointer is not. */
static pthread_t (*volatile current_thread)(void) = pthread_self;

static KaiValue *probe_body(KaiValue *self, KaiValue **args, int n) {
    (void) self; (void) args; (void) n;
    pthread_t before = current_thread();
    kai_decref(kai_seg_suspend(kai_unit()));
    if (!pthread_equal(before, current_thread())) atomic_store(&probe_moved, 1);
    return kai_unit();
}

/* Captures on whichever worker stole it, then pins itself to thread 0 so the
 * yield's enqueue routes it there: the resume runs on another worker. */
static KaiValue *probe_fiber(KaiValue *self, KaiValue **args, int n) {
    (void) self; (void) args; (void) n;
    probe_started_on = kai_thread_id;
    KaiValue *k = NULL;
    kai_decref(kai_seg_start(mk_body(probe_body, NULL), &k));
    KaiFiber *f = kai_current_fiber();
    f->pinned_main = 1;
    atomic_store(&probe_captured, 1);
    KaiCont kc;
    kai_cont_init_identity(&kc, 0);
    kai_default_spawn_yield(NULL, &kc);
    kai_decref(kai_seg_resume(k, kai_unit(), &k));
    f->pinned_main = 0;
    return kai_unit();
}

static KaiValue *mn_spawn(KaiFn fn) {
    KaiCont kc;
    kai_cont_init_identity(&kc, 0);
    KaiValue *thunk = mk_body(fn, NULL);
    KaiValue *fib = kai_default_spawn_spawn(NULL, thunk, &kc);
    kai_decref(thunk);
    return fib;
}

static void mn_await(KaiValue *fib) {
    KaiCont kc;
    kai_cont_init_identity(&kc, 0);
    kai_decref(kai_default_spawn_await(NULL, fib, &kc));
    kai_decref(fib);
}

static double now_ns(void);

static KaiValue *mn_main(void) {
    KaiValue *fibs[MN_FIBERS];
    for (int i = 0; i < MN_FIBERS; i++) fibs[i] = mn_spawn(mn_fiber);
    for (int i = 0; i < MN_FIBERS; i++) mn_await(fibs[i]);
    if (kai_nthreads > 1) {
        /* Hold thread 0 until another worker has stolen the probe and captured. */
        KaiValue *probe = mn_spawn(probe_fiber);
        double deadline = now_ns() + 10e9;
        while (!atomic_load(&probe_captured) && now_ns() < deadline) {}
        mn_await(probe);
    }
    return kai_unit();
}

static void test_migration(void) {
    kai_decref(kai_sched_bootstrap(mn_main));
    check("migrated segments resume in order", atomic_load(&mn_wrong) == 0);
    if (kai_nthreads > 1) {
        check("another worker stole the probe", probe_started_on > 0);
        check("a segment resumed on another worker", atomic_load(&probe_moved) == 1);
    }
}

/* ---------- costs ---------- */

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1e9 + (double) ts.tv_nsec;
}

static KaiValue *unit_body(KaiValue *self, KaiValue **args, int n) {
    (void) self; (void) args; (void) n;
    return kai_unit();
}

static void bench(void) {
    int64_t count = 10000000;
    double t0 = now_ns();
    test_generator(count);
    double t1 = now_ns();
    printf("suspend+resume round trip: %.2f ns\n", (t1 - t0) / (double) count);

    int64_t starts = 2000000;
    t0 = now_ns();
    for (int64_t i = 0; i < starts; i++) {
        KaiValue *k = NULL;
        kai_decref(kai_seg_start(mk_body(unit_body, NULL), &k));
    }
    t1 = now_ns();
    printf("segment start+finish from the pool: %.2f ns\n", (t1 - t0) / (double) starts);
}

int main(int argc, char **argv) {
    kai_set_args(argc, argv);
    if (argc > 1 && strcmp(argv[1], "twice") == 0) return run_twice();
    if (argc > 1 && strcmp(argv[1], "bench") == 0) { bench(); return failed; }

    test_generator(1000000);
    test_discontinue(0);
    test_discontinue(1);
    test_reparent();
    test_inherits_dispatch();
    test_abandon_escapes();
    test_handle_keeps_continuation();
    test_double_resume_traps(argv[0]);
    test_migration();

    if (failed) {
        fprintf(stderr, "segment_runtime_test: FAIL\n");
        return 1;
    }
    printf("segment_runtime_test: OK (%d threads)\n", kai_nthreads);
    return 0;
}

#else

int main(void) {
    printf("segment_runtime_test: SKIP (no stack-segment switch on this target)\n");
    return 0;
}

#endif
