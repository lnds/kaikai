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

    /* Propagate from a (simulates trampoline termination of a).
     * Reason CRASHED — peers without trap_exit get cancel_requested
     * regardless, so the existing assertions hold. */
    kai_link_propagate_terminate(&a, KAI_EXIT_CRASHED);
    check("a chain emptied",      a.linked_head == NULL);
    check("b cancel_requested",   b.cancel_requested == 1);
    check("c cancel_requested",   c.cancel_requested == 1);
    /* The propagation walk also unlinks a's back-pointer from
     * b's and c's chains, so the peers can terminate cleanly
     * later without re-entering a's freed nodes. */
    check("b chain emptied",      b.linked_head == NULL);
    check("c chain emptied",      c.linked_head == NULL);

    /* Re-propagating from b/c is a no-op (no chain). */
    kai_link_propagate_terminate(&b, KAI_EXIT_NORMAL);
    kai_link_propagate_terminate(&c, KAI_EXIT_NORMAL);
    check("re-propagate harmless", failed == 0);

    /* Mailbox owner_fiber roundtrip: alloc, set, read back. */
    {
        KaiFiber owner = {0};
        owner.state = KAI_FIBER_RUNNING;
        kai_active_fiber = &owner;
        KaiMailbox *mb = kai_mailbox_alloc();
        check("mailbox owner is current fiber", mb->owner_fiber == &owner);
        check("alloc stamps owner.mailbox",     owner.mailbox == mb);
        kai_mailbox_free(mb);
        check("free clears owner.mailbox",      owner.mailbox == NULL);
        kai_active_fiber = &kai_main_fiber;  /* restore */
    }

    /* Tier 2 trap-exit propagation. Two scenarios:
     *   1. peer.trap_exit=1 with a mailbox: terminator pushes a
     *      "Normal"/"Crashed" string into the peer's mailbox, leaves
     *      cancel_requested unset.
     *   2. peer.trap_exit=0 (default): terminator sets cancel_requested
     *      (the existing v1 behaviour).
     */
    {
        KaiFiber dying = {0}, watcher = {0};
        dying.state   = KAI_FIBER_RUNNING;
        watcher.state = KAI_FIBER_RUNNING;

        kai_active_fiber = &watcher;
        KaiMailbox *wmb = kai_mailbox_alloc();
        watcher.trap_exit = 1;
        check("watcher mailbox stamped", watcher.mailbox == wmb);

        kai_active_fiber = &dying;
        kai_link_add_bidirectional(&dying, &watcher);

        /* Normal exit. */
        kai_link_propagate_terminate(&dying, KAI_EXIT_NORMAL);
        check("trap-exit: cancel_requested NOT set",
              watcher.cancel_requested == 0);
        check("trap-exit: mailbox got 1 message",
              wmb->len == 1 && wmb->head != NULL);
        if (wmb->head) {
            KaiValue *msg = wmb->head->msg;
            check("trap-exit: message tag KAI_STR",
                  msg && msg->tag == KAI_STR);
            check("trap-exit Normal: payload \"Normal\"",
                  msg && msg->tag == KAI_STR
                      && msg->as.s.len == 6
                      && memcmp(msg->as.s.bytes, "Normal", 6) == 0);
        }

        /* Re-link + crash. */
        kai_link_add_bidirectional(&dying, &watcher);
        kai_link_propagate_terminate(&dying, KAI_EXIT_CRASHED);
        check("trap-exit: still no cancel_requested",
              watcher.cancel_requested == 0);
        check("trap-exit: mailbox now has 2 messages",
              wmb->len == 2);
        /* Drain head + check tail says Crashed. */
        if (wmb->head && wmb->head->next) {
            KaiValue *msg = wmb->head->next->msg;
            check("trap-exit Crashed: payload \"Crashed\"",
                  msg && msg->tag == KAI_STR
                      && msg->as.s.len == 7
                      && memcmp(msg->as.s.bytes, "Crashed", 7) == 0);
        }

        kai_mailbox_free(wmb);
        kai_active_fiber = &kai_main_fiber;
    }

    /* Tier 2: peer with trap_exit=0 still gets cancel_requested. */
    {
        KaiFiber dying = {0}, plain = {0};
        dying.state = KAI_FIBER_RUNNING;
        plain.state = KAI_FIBER_RUNNING;
        plain.trap_exit = 0;
        kai_link_add_bidirectional(&dying, &plain);
        kai_link_propagate_terminate(&dying, KAI_EXIT_CRASHED);
        check("plain peer cancel_requested",     plain.cancel_requested == 1);
        check("plain peer chain emptied",        plain.linked_head == NULL);
    }

    /* Tier 2: trap_exit=1 but no mailbox falls back to cancel. */
    {
        KaiFiber dying = {0}, watcher = {0};
        dying.state   = KAI_FIBER_RUNNING;
        watcher.state = KAI_FIBER_RUNNING;
        watcher.trap_exit = 1;
        watcher.mailbox   = NULL;  /* explicit no-mailbox case */
        kai_link_add_bidirectional(&dying, &watcher);
        kai_link_propagate_terminate(&dying, KAI_EXIT_NORMAL);
        check("trap_exit without mailbox falls back to cancel",
              watcher.cancel_requested == 1);
    }

    if (failed) {
        fprintf(stderr, "link-runtime FAIL\n");
        return 1;
    }
    return 0;
}
