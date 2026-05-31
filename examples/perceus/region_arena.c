/* issue #120 — opt-in Perceus regions: P0 runtime arena gate.
 *
 * A C-level fixture (there is no `region { }` surface yet at P0) that
 * exercises the bump-arena primitive directly against stage0/runtime.h
 * and asserts the load-bearing invariants from
 * docs/issue-120-regions-design.md:
 *
 *   1. An arena value carries rc=INT32_MAX, so kai_decref / kai_incref
 *      are no-ops on it and kai_check_unique rejects it (reuse-in-place
 *      auto-disables for free).
 *   2. kai_arena_free reclaims chunks in BULK without a per-value free
 *      walk: kai_rc_free_total NEVER moves across the whole region.
 *   3. The dedicated arena counters balance and kai_rc_live_now returns
 *      to baseline after the sweep (the KAI_TRACE_RC bookkeeping
 *      landmine — a bulk free that did not adjust live_now would report
 *      a false leak).
 *   4. The arena's backing malloc chunks ARE released (the ASAN run in
 *      tools/run-arena-c-fixture.sh confirms no chunk leak / UAF).
 *
 * Wired into `make tier0` (plain) and `make tier1-asan` (ASAN) via
 * tools/run-arena-c-fixture.sh.
 */
#include "../../stage0/runtime.h"
#include <assert.h>

/* Build a small aggregate entirely inside the arena (header + interior
 * fields array) to prove kai_arena_raw keeps interior storage in the
 * arena too — otherwise the fields array would dangle on bulk free. */
static KaiValue *arena_pair(KaiArena *a, int64_t x, int64_t y) {
    KaiValue *rec = kai_arena_alloc(a, KAI_RECORD);
    KaiValue **fields = (KaiValue **) kai_arena_raw(a, 2 * sizeof(KaiValue *));
    KaiValue *fx = kai_arena_alloc(a, KAI_INT); fx->as.i = x;
    KaiValue *fy = kai_arena_alloc(a, KAI_INT); fy->as.i = y;
    fields[0] = fx;
    fields[1] = fy;
    rec->as.rec.n_fields = 2;
    rec->as.rec.fields = fields;
    rec->as.rec.names = NULL;
    rec->as.rec.head_type_tag = KAI_HEAD_ANON;
    return rec;
}

int main(void) {
    int64_t free0   = kai_rc_free_total;
    int64_t live0   = kai_rc_live_now;
    int64_t aalloc0 = kai_arena_alloc_total;
    int64_t afree0  = kai_arena_free_total;

    const int N = 10000;
    KaiArena *a = kai_arena_push();
    assert(kai_arena_current() == a);

    for (int i = 0; i < N; i++) {
        KaiValue *v = kai_arena_alloc(a, KAI_INT);
        v->as.i = i;
        kai_decref(v);                       /* no-op on immortal sentinel */
        kai_incref(v);                       /* no-op on immortal sentinel */
        assert(kai_check_unique(v) == 0);    /* reuse-in-place rejects it */
    }
    /* A handful of aggregates with interior arena storage. */
    for (int i = 0; i < 100; i++) {
        KaiValue *p = arena_pair(a, i, i * 2);
        assert(p->as.rec.fields[0]->as.i == i);
        assert(p->as.rec.fields[1]->as.i == i * 2);
        kai_decref(p);                       /* no-op */
    }

    /* During the region: no per-value free, arena alloc counter moved. */
    assert(kai_rc_free_total == free0);
    assert(kai_arena_alloc_total > aalloc0);
    assert(kai_rc_live_now > live0);

    int64_t live_in_region = kai_arena_alloc_total - aalloc0;

    kai_arena_pop();                         /* bulk free, no value walk */

    /* After sweep: free_total STILL never moved; live_now back to
     * baseline; arena counters balanced. */
    assert(kai_rc_free_total == free0);
    assert(kai_rc_live_now == live0);
    assert(kai_arena_free_total - afree0 == live_in_region);
    assert(kai_arena_alloc_total - kai_arena_free_total == aalloc0 - afree0);
    assert(kai_arena_sp == 0);

    fprintf(stderr,
        "REGION_ARENA_OK values=%lld free_total_delta=%lld live_now_delta=%lld\n",
        (long long) live_in_region,
        (long long) (kai_rc_free_total - free0),
        (long long) (kai_rc_live_now - live0));
    return 0;
}
