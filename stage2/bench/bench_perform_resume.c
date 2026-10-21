/* m7a #9 régime-cost benchmark.
 *
 * Workload: trivial-clause Counter.tick() in a tight loop. The
 * clause body is `resume(())` — the smallest possible
 * perform/resume round-trip. This isolates dispatch + cont-check
 * + identity-resume cost from state / allocation / branch noise.
 *
 * Baseline: a plain C function with the same side-effect
 * (incrementing a global), called directly N times. The same
 * compiler at the same optimisation level builds both halves.
 *
 * Metric: ns/op via clock_gettime(CLOCK_MONOTONIC). Reported
 * alongside ops/sec for cross-language sanity comparison. The
 * gate keys off the ratio: kaikai_ns / c_ns ≤ 5× — see
 * docs/effects-impl.md §*Open questions* #6 for the rationale.
 *
 * Run by hand: cc -O2 -I ../../stage0 bench_perform_resume.c -o /tmp/bench && /tmp/bench
 * Or via: make -C stage2 bench-effects (which also enforces the gate).
 */

#include "runtime.h"
#include <time.h>
#include <stdio.h>

/* Mirrors the EvE struct stage 2 emits for `effect Counter { tick();
 * get(): Int }`. We only exercise tick here; get is in the layout to
 * keep the struct shape identical to a compiler-emitted one. */
typedef struct EvCounter EvCounter;
struct EvCounter {
    KaiHandlerId handler_id;
    void *env;
    KaiValue *(*tick)(EvCounter *self, KaiCont *k);
    KaiValue *(*get)(EvCounter *self, KaiCont *k);
};

/* Global counter — both the C-direct baseline and the effect
 * clause increment it, so neither path can be dead-code-elimed by
 * the compiler. The bytes touched are the same in both halves. */
static volatile long g_count = 0;

/* C-direct baseline: a regular function call with the same side
 * effect as the effect clause (one increment of g_count). */
static void c_direct_tick(void) {
    g_count++;
}

/* Effect clause body for `tick(resume) -> resume(())`. The clause
 * shape exactly matches what stage 2's emit_clause_body produces
 * in m7a #6c (signature, kai_cont_resume tail call). */
static KaiValue *bench_clause_tick(EvCounter *self, KaiCont *k) {
    (void) self;
    g_count++;
    return kai_cont_resume(k, kai_unit());
}

/* Diff two timespecs as ns. */
static double ns_between(struct timespec t0, struct timespec t1) {
    double s  = (double)(t1.tv_sec  - t0.tv_sec);
    double ns = (double)(t1.tv_nsec - t0.tv_nsec);
    return s * 1e9 + ns;
}

int main(int argc, char **argv) {
    (void) argv;
    long n = (argc > 1) ? 1000000 : 10000000; /* default 10⁷ */

    /* C-direct baseline */
    g_count = 0;
    struct timespec c0, c1;
    clock_gettime(CLOCK_MONOTONIC, &c0);
    for (long i = 0; i < n; ++i) {
        c_direct_tick();
    }
    clock_gettime(CLOCK_MONOTONIC, &c1);
    double c_total_ns = ns_between(c0, c1);
    long c_count_check = g_count;

    /* Kaikai effect path: build the EvCounter, push evidence,
     * loop N times invoking the op fn ptr through the struct. */
    EvCounter ev = {0};
    ev.handler_id = kai_fresh_handler_id();
    ev.tick = &bench_clause_tick;
    KaiEvidence node;
    kai_evidence_push_l(&node, kai_eff_intern("Counter"), &ev);

    g_count = 0;
    struct timespec e0, e1;
    clock_gettime(CLOCK_MONOTONIC, &e0);
    for (long i = 0; i < n; ++i) {
        EvCounter *evp = (EvCounter *) kai_evidence_lookup_l(kai_eff_intern("Counter"));
        KaiCont k;
        kai_cont_init_identity(&k, evp->handler_id);
        evp->tick(evp, &k);
    }
    clock_gettime(CLOCK_MONOTONIC, &e1);
    double e_total_ns = ns_between(e0, e1);
    long e_count_check = g_count;

    kai_evidence_pop();

    if (c_count_check != n || e_count_check != n) {
        fprintf(stderr, "bench: count mismatch (c=%ld effect=%ld expected=%ld)\n",
                c_count_check, e_count_check, n);
        return 1;
    }

    double c_ns_per_op = c_total_ns / (double) n;
    double e_ns_per_op = e_total_ns / (double) n;
    double ratio       = e_ns_per_op / c_ns_per_op;

    printf("N             = %ld\n",        n);
    printf("C-direct      = %.2f ns/op  (%.0f Mops/s)\n",
           c_ns_per_op, 1000.0 / c_ns_per_op);
    printf("Effect path   = %.2f ns/op  (%.0f Mops/s)\n",
           e_ns_per_op, 1000.0 / e_ns_per_op);
    printf("Ratio         = %.2fx\n",       ratio);
    printf("Threshold     = 5.00x  (Doc C OQ #6)\n");
    if (ratio > 5.0) {
        printf("VERDICT       = REGRESSION (régime fallback per Doc C §*Fallback*)\n");
        return 2;
    }
    printf("VERDICT       = PASS\n");
    return 0;
}
