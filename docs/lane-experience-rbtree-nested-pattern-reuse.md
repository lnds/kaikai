# Lane retro — nested-pattern reuse-in-place (rb-tree balance, Koka 2-of-3)

Commits (on `main`): `f92110f` (outer cell), `f7e5bd6` (RC trace counters),
`5ef831a` (inner cell). Base: `83f5705` (Koka-packed 8B header, node 48B).

## Scope as planned vs as shipped

**Planned (from `/tmp/kaikai-parity-next-levers.md` handoff):** attack the
~28% of rb-tree variant nodes the reuse-token leaves un-reused (the
`kai_variant_u` fresh nodes the profiler flagged), described there as the
"#2 reuse-ratio lever, perceus-pure, no ABI". The handoff framed the next big
lever as "#1 borrow inference complete (interprocedural)".

**Shipped:** the rb-tree balance rotation arms now reuse-in-place exactly like
Koka — both the outer matched cell AND the inner deconstructed cell (Koka's
2-of-3 rotation reuse). Local reuse only; **no ABI change, no interprocedural
token passing**. Variant allocs 7.61M → 6.30M (−17%), `reuse_in_place`
19.6M → 21.0M, wall best-of-5 0.70s → 0.63s (−10%). Gates: tier0 green
(selfhost deterministic, demos 34/34, arena+ASAN), full 1M bench clean under
ASAN+UBSan, tree size/height exact, garbage-free preserved (leaked=14, all
str, pre-existing).

## The load-bearing finding — two prior retros were wrong on the same point

This lane was launched to do interprocedural token-passing (`#1`). Reading
Koka's actual implementation (`koka/src/Backend/C/ParcReuse.hs`) and its
**generated C** for the identical rb-tree (`koka/test/bench/koka/rbtree.kk`,
compiled locally) overturned the premise:

1. **Koka's reuse is 100% LOCAL.** `ruBranch` isolates the available-token set
   per branch (`isolateGetAvailable`) and drops unused tokens at the end of
   the branch. There is **no reuse parameter in Koka's ABI** and **no
   interprocedural token passing**. The handoff's `#1` (and its three cited
   memories — `project_kaikai_borrow_inference_design`,
   `project_kaikai_variant_reuse_token_not_cons_overwrite`,
   `project_kaikai_rbtree_wall_levers_post742`) do not exist in the memory
   store / were wrong on this point.

2. **`lane-experience-koka-arm-top-reuse.md` "fundamental wall" was about the
   wrong function.** That retro concluded the Black arm of `insert_loop`
   cannot reuse without B3 inter-procedural borrow, because
   `balance_left(insert_loop(l,...), ..., r)` passes `l`/`r` to a function and
   the compiler can't know consume-vs-borrow. **True — but irrelevant to the
   lever.** The cell `insert_loop` steals there (`reuse_freed`=5.3M/1M inserts)
   is freed by Koka too (its generated C confirms: `balance-left` does not
   receive `ins`'s cell). The reuse happens INSIDE `balance_left`, on ITS OWN
   matched cells — purely local, no B3 needed.

3. **`lane-experience-rbtree-reuse-allocator-pool.md` rejected the wrong
   thing.** It said "overwrite-in-place reuse of the rotation cases — REJECTED,
   double-frees, the rotation is non-1:1 (match three cells, reconstruct three
   reordered)". The non-1:1 claim is right; the rejection was premature. Koka
   reuses non-1:1 rotations fine via `kai_variant_at` (full-overwrite move
   semantics, NOT 1:1 slot-diagonal). The double-frees that retro hit were the
   old eager-1:1-decref embryo (`kai_reuse_or_alloc_variant`), already replaced
   by `kai_variant_at`.

Rule reinforced: **read the target's source/output, not its paper or our
prior retros.** Three documents (handoff + two retros) pointed at
interprocedural / impossible / rejected; the truth was one local gate in
perceus plus the nested-pattern bind machinery in emit.

## Design decisions

- **The gate was one predicate.** `pcs_all_flat_subs` (perceus) required every
  arm sub-pattern be a flat `PBind`/`PWild`. The balance rotation
  `RBNode(_, RBNode(Red, lx,...), ..)` carries a one-level nested sub-pattern,
  so it fell through to the fresh allocator. Relaxing `pcs_sub_is_flat` to
  accept a one-level nested `PVariant` whose own slots are flat is the whole
  perceus-side change.

- **Outer cell first (f92110f), inner cell second (5ef831a).** Incremental:
  the outer-cell reuse (−8.6%) is the simpler half — `rv_slot_binds`
  deconstructs the nested slot via `emit_pat_binds` (alias → grand-children
  incref'd), decrefs the inner cell, `rv_old_slot_drops` skips the already-
  decref'd nested slot (else double-free). Committed + ASAN-green before
  starting the inner cell.

- **Inner cell required Koka's nested-uniqueness decision (rv_inner_decision).**
  The first inner-cell attempt incref'd grand-children unconditionally then
  `kai_variant_at`'d the inner cell → rc inflation + leak → `non-exhaustive
  match` at 1M. Fix mirrors Koka's generated C exactly: borrow-bind
  grand-children (no incref), then `if (unique(inner)) { ru=reuse(inner); }
  else { dup kept ptr children; decref(inner); ru=null; }`. The borrow binds
  are declared OUTSIDE the if/else (C scope) so the whole rebuilt body sees
  them — Koka does the same.

- **Atomic token consume (the corruption that took two tries).** The donated
  `kai_variant_at(_ru_inner, ...)` did NOT null `_ru_inner`, so the post-rebuild
  `inner_drop` freed a cell that was reused → `size: 4` for 5 distinct inserts
  (a lost subtree). Fix: consume the token in a comma-expression that NULLs it
  (`kai_variant_at((_ru_tmp=_ru_inner, _ru_inner=null, _ru_tmp), ...)`). This
  is the linear-token discipline the TRMC path already had (`_arm_ru = null`
  after consume); the new non-TRMC path needed it too.

## Structural surprises

- **`variant_slot_kind("Int") == 0` (Int is a tagged pointer, not raw .i64).**
  Commit `c221cee` stores Int tagged. So a nested `RBNode`'s `kx`/`vx` Int
  slots are read as `.ptr`, same as boxed children. Briefly looked like a bind
  bug; it is correct — only `Real` (k==2) is raw.

- **String-surgery for token donation.** Donating the inner token to "the
  first same-arity sub-ctor of the body" is done by `replace_first` on the
  emitted C of `rv_new_temps` (`kai_variant_u(tag,"C",n,` →
  `kai_variant_at(...,`). Any same-arity cell is a valid Koka reuse target, so
  first-textual-match is sound. Less elegant than a context flag, but avoids
  threading a `donor` field through all 9 `EmitCtx` constructors. Documented at
  the call site.

## Fixtures added / coverage gaps

- **Gap:** no new golden fixture wired into a tier yet. `examples/perceus/
  rb_tree_bench.kai` exercises the path but is not tiered (too slow). A small
  balance-rotation fixture asserting `rb_size` after rotations (the `size: 5`
  vs `size: 4` shape that caught the corruption) belongs in
  `examples/perceus/`. **TODO before this is considered closed.**

## Real cost vs estimate

The handoff estimated `#1` (interprocedural) as "several sessions, ABI risk".
The actual lever was one perceus predicate + ~4 emit helpers, one session, no
ABI. Most of the time went to (a) reading Koka source to kill the wrong
premise, (b) the two RC-corruption debugging rounds (rc inflation, then
double-use of the token).

## Follow-ups for next lanes

1. **Borrow-pure on the OUTER cell (the remaining lever).** `incref_total`
   ~23.4M: the unique branch still incref's the outer node's flat children
   (`ky,vy,ry`) then decref's them in `old_drops`. Koka MOVES them (no RC) in
   the unique branch. Applying the borrow model to the outer cell (as
   rv_inner_decision already does for the inner) elides that incref/decref pair
   — likely ~half the remaining inner-loop gap.
2. **Tiered regression fixture** (see coverage gap).
3. **`reuse_freed`=5.3M is NOT a lever** — Koka frees the same cell. Cache-miss
   of the random descent is irreducible (C and Koka pay it). Do not pursue.

See memory `project_kaikai_rbtree_reuse_lever_is_nested_pattern` for the
measurement reproduction (KAI_TRACE_RC counters).
