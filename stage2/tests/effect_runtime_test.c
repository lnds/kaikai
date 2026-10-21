/* m7a #5: smoke test for the handler-stack runtime primitives.
 *
 * Exercises kai_evidence_push / pop / lookup directly, without
 * any compiled kaikai code. m7a #6 (lowering) will provide the
 * end-to-end coverage; this test just confirms the C primitives
 * behave as Doc C §*Handler-stack runtime* describes.
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

int main(void) {
    /* Empty stack: lookup of any label returns NULL. */
    check("empty lookup is NULL",
          kai_evidence_lookup("Io") == NULL);

    /* Push two evidence nodes; lookup must find the innermost. */
    KaiEvidence outer, inner;
    int outer_handler = 1;
    int inner_handler = 2;

    kai_evidence_push(&outer, "Io", &outer_handler);
    kai_evidence_push(&inner, "Io", &inner_handler);

    check("innermost wins",
          kai_evidence_lookup("Io") == &inner_handler);

    /* Different effects coexist; the matching one is returned. */
    KaiEvidence fail;
    int fail_handler = 99;
    kai_evidence_push(&fail, "Fail", &fail_handler);

    check("Io still resolves with Fail on top",
          kai_evidence_lookup("Io") == &inner_handler);
    check("Fail resolves to its own handler",
          kai_evidence_lookup("Fail") == &fail_handler);

    /* Pop unwinds back through the stack. */
    kai_evidence_pop();      /* drops Fail */
    check("Fail gone after pop",
          kai_evidence_lookup("Fail") == NULL);
    check("Io still found",
          kai_evidence_lookup("Io") == &inner_handler);

    kai_evidence_pop();      /* drops inner */
    check("after popping inner, outer wins",
          kai_evidence_lookup("Io") == &outer_handler);

    kai_evidence_pop();      /* drops outer */
    check("empty after popping outer",
          kai_evidence_lookup("Io") == NULL);

    /* A label copy at another address resolves through interning. */
    char io_copy[3] = { 'I', 'o', 0 };
    KaiEvidence node;
    int handler = 7;
    kai_evidence_push(&node, "Io", &handler);
    check("copy of Io finds the handler",
          kai_evidence_lookup(io_copy) == &handler);
    kai_evidence_pop();

    /* Interning: equal text is one pointer; instances of one effect share a base. */
    char st_copy[11] = "State[Int]";
    check("equal text interns to one pointer",
          kai_eff_intern(st_copy) == kai_eff_intern("State[Int]"));
    const char *si = kai_eff_intern("State[Int]");
    const char *ss = kai_eff_intern("State[String]");
    const char *sb = kai_eff_intern("State");
    check("pinned instance matches itself", kai_eff_match(si, si));
    check("distinct pinned instances do not match", !kai_eff_match(si, ss));
    check("bare effect matches a pinned instance", kai_eff_match(sb, si) && kai_eff_match(si, sb));
    check("another effect never matches", !kai_eff_match(kai_eff_intern("Reader[Int]"), si)
          && !kai_eff_match(kai_eff_intern("Reader"), sb));
    check("a longer name sharing a prefix does not match", !kai_eff_match(kai_eff_intern("StateX"), sb));

    KaiEvidence ni, ns;
    int hi = 1, hs = 2;
    kai_evidence_push(&ni, "State[Int]", &hi);
    kai_evidence_push(&ns, "State[String]", &hs);
    check("lookup picks the matching instance under a sibling",
          kai_evidence_lookup(st_copy) == &hi);
    check("bare lookup takes the innermost instance", kai_evidence_lookup("State") == &hs);
    kai_evidence_pop();
    kai_evidence_pop();

    if (failed) {
        fprintf(stderr, "effect_runtime_test: FAIL\n");
        return 1;
    }
    printf("effect_runtime_test: OK\n");
    return 0;
}
