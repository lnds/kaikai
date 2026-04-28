/* Phase 5: smoke test for the Link runtime registry.
 *
 * Exercises kai_link_add_bidirectional + kai_link_propagate_terminate
 * directly on KaiFiber structs, without going through the scheduler
 * or kaikai compilation. Phase 5 ships the runtime primitives; an
 * end-to-end kaikai-level demo for link-driven cancel cascade
 * defers to Phase 5.5+ alongside spawn_actor (m8.x #6) — the
 * current actor surface (with_mailbox) makes pid handoff awkward
 * for a clean two-fiber link demo without it.
 */

#include "runtime.h"
#include <stdio.h>

static int failed = 0;

static void check(const char *what, int cond) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failed = 1;
    }
}

static int chain_len(KaiFiber *f) {
    int n = 0;
    for (KaiLinkNode *ln = f->linked_head; ln; ln = ln->next) n++;
    return n;
}

static int chain_has(KaiFiber *f, KaiFiber *target) {
    for (KaiLinkNode *ln = f->linked_head; ln; ln = ln->next) {
        if (ln->peer == target) return 1;
    }
    return 0;
}

int main(void) {
    /* Three fibers; we'll link A↔B and A↔C, then terminate A. */
    KaiFiber a = {0}, b = {0}, c = {0};
    a.state = KAI_FIBER_RUNNING;
    b.state = KAI_FIBER_RUNNING;
    c.state = KAI_FIBER_RUNNING;

    /* Empty chains. */
    check("a starts unlinked", a.linked_head == NULL);
    check("b starts unlinked", b.linked_head == NULL);

    /* Bidirectional add: a↔b. */
    kai_link_add_bidirectional(&a, &b);
    check("a→b after add",       chain_has(&a, &b));
    check("b→a after add",       chain_has(&b, &a));
    check("a chain len = 1",     chain_len(&a) == 1);
    check("b chain len = 1",     chain_len(&b) == 1);

    /* Self-link is dropped. */
    kai_link_add_bidirectional(&a, &a);
    check("a chain unchanged on self-link", chain_len(&a) == 1);

    /* Add second peer: a↔c. */
    kai_link_add_bidirectional(&a, &c);
    check("a→c after second add", chain_has(&a, &c));
    check("c→a after second add", chain_has(&c, &a));
    check("a chain len = 2",      chain_len(&a) == 2);
    check("c chain len = 1",      chain_len(&c) == 1);

    /* Cancel flags clear pre-propagation. */
    check("b not pre-flagged", b.cancel_requested == 0);
    check("c not pre-flagged", c.cancel_requested == 0);

    /* Propagate from a (simulates trampoline termination of a). */
    kai_link_propagate_terminate(&a);
    check("a chain emptied",      a.linked_head == NULL);
    check("b cancel_requested",   b.cancel_requested == 1);
    check("c cancel_requested",   c.cancel_requested == 1);
    /* The propagation walk also unlinks a's back-pointer from
     * b's and c's chains, so the peers can terminate cleanly
     * later without re-entering a's freed nodes. */
    check("b chain emptied",      b.linked_head == NULL);
    check("c chain emptied",      c.linked_head == NULL);

    /* Re-propagating from b/c is a no-op (no chain). */
    kai_link_propagate_terminate(&b);
    kai_link_propagate_terminate(&c);
    check("re-propagate harmless", failed == 0);

    /* Mailbox owner_fiber roundtrip: alloc, set, read back. */
    {
        KaiFiber owner = {0};
        owner.state = KAI_FIBER_RUNNING;
        kai_active_fiber = &owner;
        KaiMailbox *mb = kai_mailbox_alloc();
        check("mailbox owner is current fiber", mb->owner_fiber == &owner);
        kai_mailbox_free(mb);
        kai_active_fiber = &kai_main_fiber;  /* restore */
    }

    if (failed) {
        fprintf(stderr, "link-runtime FAIL\n");
        return 1;
    }
    return 0;
}
