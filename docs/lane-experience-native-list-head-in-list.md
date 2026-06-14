# Lane experience — native list-head-in-list

**Scope:** close the native (in-process libLLVM, KIR Lane 1.5) correctness gap
where a list pattern whose HEAD is itself a list (`[[a, b], ..._]`, `[[a], [b]]`,
`[[a, ...rs], ..._]`, and the same under a variant slot `Some([[a, b], ..._]])`)
aborts the native backend with `unbound register`.

## Scope as planned vs as shipped

Planned: one bounded fix in `stage2/compiler/kir_lower_match.kai` — intercept a
`PList` head in `lm_emit_head` and re-enter `lm_emit_cells` against the projected
head cell, so the nested list's own length test + element binds are emitted by
the same machinery (the Maranget column decomposition).

Shipped: exactly that. `lm_emit_head` gained a `PList(pats, opt_rest)` arm that
projects slot 0 (`KProj(cur, 0)` — a borrow, the same projection the variant /
record head arm already does) and hands the projected value to a new helper
`lm_emit_head_list`, which threads a fresh `cont` label into `lm_emit_cells`
(the nested match must FALL THROUGH to the enclosing arm's remaining cells, not
exit the arm) and reopens there. A mismatch routes to the shared `fail_lbl`. No
other file changed; `match_list_head_in_list.kai` added as the regression
fixture.

## Design decisions and alternatives considered

- **Re-enter `lm_emit_cells` vs a bespoke nested-list test+bind.** The cell walk
  already interleaves test (`is_cons`/`is_nil`) and bind (`KProj` slot 0/1)
  correctly for the OUTER list; a list head is the identical problem one level
  down. Reusing it is the oracle's own shape — `emit_pat_test`/`emit_pat_binds`
  recurse into `emit_pat_test_list` when a list head appears. A bespoke path
  would have duplicated the cons/nil discrimination and the rest-binder logic,
  earning a `km dups` group for no benefit. Rejected.

- **No `kir_lower_walk` seam (unlike the variant-slot sibling 242e33eb).** The
  variant-slot fix had to route through `kir_lower_walk`'s
  `emit_arm_subtests_and_binds` because `lm_emit_cells` is in the match module
  and the variant module that owned the slot path could not call it (variant↔match
  import cycle). Here BOTH `lm_emit_head` and `lm_emit_cells` live in
  `kir_lower_match.kai`, so the head list lowers in place — a direct call, no
  cross-module seam, no cycle. This is why the variant-slot case
  (`Some([[a, b], ..._]])`) also closed for free: the walk already diverts the
  whole arm to `lm_emit_cells`, which now recurses into the list head itself.

- **Borrow, not own, the projected head.** `KProj` lowers to `kaix_variant_arg`,
  which BORROWS (does not incref). The nested binders are `KLet` slot aliases
  (`lm_bind_name`), which never incref — refcounting is KRC nodes that perceus
  already placed against the recursively-collected `pat_ubinds`. So the fix adds
  zero RC operations; it only adds the test + the slot-alias binds that were
  MISSING. Verified with ASAN over the matching AND the non-matching path
  (`[[1], [2]]` against `[[a, b], ..._]` — the head's length test fails, the
  inner binders must NOT have been bound): zero double-free / UAF.

  Because the design reuses the same borrow + KLet-alias discipline the variant
  and record heads already use (and which is exercised on Linux/CI today), no
  asu soundness consult was needed — the RC question is settled by reuse, not by
  a new mechanism.

## Structural surprises

- **`psub_is_discriminating(PList)` was already `true`.** Set by the variant-slot
  lane (242e33eb) so a list slot is never treated as its tag's catch-all. That
  meant a `PList` head, BEFORE this fix, entered `lm_emit_head_test`'s
  discriminating branch but then fell to `lm_head_seal_test`'s `_ ->` catch
  (an unconditional pass) AND `lm_bind_head_value`'s `_ -> st` (bind nothing) —
  so the length test was skipped AND the binders left unbound. The fix
  intercepts `PList` in `lm_emit_head` BEFORE that generic path, so neither
  generic catch is reached for a list head.

- **The two "closed" ratchet fixtures are a portability false-positive.**
  `examples/stdlib/list_helpers.kai` and `list_zip3_scan.kai` show as PASS-now
  on macOS but are listed in the baseline as Linux-only SIGSEGV gaps (a separate
  codegen root cause). They passed on macOS before this lane too — this fix did
  NOT close them — so they STAY in `tools/native-parity-baseline.txt`. Removing
  them would (per the burn-down 1/2/3 lesson) break the Linux/CI-validated
  ratchet. The genuine list-head-in-list coverage is the new fixture, which
  passes on both backends locally.

## Fixtures added

`examples/effects/match_list_head_in_list.kai` (auto-scanned by
`tools/test-backend-parity.sh`, native-vs-C diff). Covers: exact-length list
head (`[[a, b], ..._]`), two singleton list cells (`[[a], [b]]`), rest-binder
inside the head list (`[[a, ...rs], ..._]`), and a list-head-list under a variant
slot (`Some([[a, b], ..._]])`). Each probe distinguishes the correct value from
the wrong-branch / unbound outcome, including the non-matching head-length cases
(`[[1], [2]]` -> -1, `[[4, 9], [5]]` -> -1, `[[], [9]]` -> -1).

## Cost vs estimate

Bounded as estimated: ~24 lines of lowering (one new arm + one helper), one
fixture. The diagnosis was supplied; the only investigation was confirming the
oracle's recursion shape (`emit_pat_test_list_loop` / `emit_pat_binds_list`) and
the borrow semantics of `KProj`. One wrinkle cost a rebuild: the first `Edit`
landed in the sibling `main` checkout, not the worktree (the known
edit-leaks-to-main trap) — caught when the regenerated `stage2.c` lacked the new
symbol; reverted main, re-applied in the worktree.

## Follow-ups left for next lanes

- `list_helpers` / `list_zip3_scan` Linux-only SIGSEGV (baseline) — unrelated
  codegen root cause, untouched here.
- The 20 remaining native-parity gaps (hashmap/hashset `Hash:hash` proto
  dispatch, crypto timeout, FFI extern-C, m7a double-resume, weather NetTcp,
  huffman timeout) — separate families, see `docs/native-parity-gaps.md`.
