# Lane experience — issue #995: native backend reuses recursive-ADT nodes in place when the value is shared

## Scope as planned vs as shipped

**Planned.** Fix the native (LLVM) backend so a functional rewrite of a
recursive ADT (`inc(t)` mapping `+1` over a tree/list) does not reuse the
original's nodes in place while the original binding is still live. The
reproducer: `let t = Node(Leaf(1), Leaf(2)); let t2 = inc(t)` must print
`orig=3 new=5`; native printed `orig=5 new=5` (mutated `t`), C was correct.

**Shipped.** Exactly that, plus the linear-list shape (`SCons`/`SNil`), with a
two-fixture regression suite (corruption gate + dead-donor valid-reuse gate)
and two native CI gates. One ~12-line change to the KIR reuse lowering. No
runtime change, no native-emit change, no perceus.kai change.

## Root cause — analysis/lowering, not emit, not runtime

The brief flagged the risk both ways (analysis marking reuse wrongly vs emit
ignoring the reuse token) and asked which. The answer is **lowering**, and the
runtime/emit are both already correct:

- Both backends share ONE runtime `kai_check_unique` (`v->rc == 1`) and ONE
  `kai_variant_reuse_at` that falls back to a fresh alloc on a shared donor.
  The native emit (`nemit_variant_reuse_call`) delegates to that helper. The
  uniqueness gate is intact on both paths.
- The KIR is identical for both backends. So is the gate. The divergence is in
  what RC traffic the lowering emits AROUND the reuse.

The C oracle's AST path (`emit_match_arm_reuse_variant`) emits the reuse arm as
TWO textual branches: a unique branch (bind children by borrow → MOVE) and a
shared `else` branch (`kai_incref(child)` for each kept child → the recursion
sees rc==2 → fresh-allocs). C never inspects the callee; per-branch RC decides
everything. The native KIR path collapses to ONE body and recreates the C
`else` with conditional `KIncrefIfShared(donor, child)` ops.

The defect: for a rebuild that threads a kept scrutinee child through a call
(`Node(inc(l), inc(r))`), `reuse_gc_uses_expr` counted binders embedded in
nested CTOR calls but SKIPPED binders passed to ordinary fn calls — with a
comment claiming "a NON-ctor call CONSUMES its args, so an incref would be
spurious (the `Node(Red, ins(l,..),..)` leak)". That reasoning was wrong for
the map-recursion shape: when the donor is shared the reuse fresh-allocs but
the still-live donor keeps its reference to the child; the recursive call then
reused that aliased child in place and mutated the original.

A second, subtler half: even once `reuse_gc_uses` counted the binders, the
increfs landed AFTER `lower_exprs` (i.e. after `inc(l)`/`inc(r)` had already
run and mutated the leaf). The shared-donor incref must precede the consuming
call. The old "order vs the reuse op is irrelevant" note holds for children
embedded by reference but NOT for children threaded through a call.

## The fix

Two coupled changes in `kir_lower_walk.kai`:

1. `reuse_gc_uses_expr` descends into ALL call args (and the callee), not only
   ctor calls. A kept child consumed by a call still needs the shared-donor
   incref.
2. `lower_reuse_con` emits the shared-donor increfs BEFORE lowering the rebuild
   args, so the conditional incref lands before the call that consumes the
   child.

`KIncrefIfShared` is conditional (`if (!kai_check_unique(donor)) incref(child)`),
so a unique donor pays nothing and the child MOVEs — valid reuse-in-place is
preserved. A shared donor gives the child its own ref so the call fresh-allocs.

## The `ins(l,..)` worry the old comment raised — verified harmless

The old comment feared the mixed rb-tree arm `Node(Red, ins(l,..),..)` would
leak under this change. It does not, for two reasons confirmed empirically on
`llvm_arm_top_reuse_shared.kai` (rb-tree with a deliberately SHARED base):

- `KIncrefIfShared` is conditional. On a shared donor it increfs `l`, `ins`
  sees `l` shared and fresh-allocs (no in-place recycle) — the balance closes
  exactly as the C `else` branch.
- The genuinely-dangerous NESTED rotation (`Node(.., Node(Black, .., ..), ..)`)
  rides `reuse_has_nested_ctor` to a separate fresh-alloc-and-dup path and
  never reaches `emit_reuse_gc_increfs`. The fix does not touch it.

The rb-tree shared RC trace is byte-identical pre/post fix (alloc_total=4581,
reuse_in_place=402, incref=4271, decref=5408; sums unchanged), confirming the
no-regression of the `ins` case.

## Fixtures added

- `examples/perceus/native_shared_reuse_corruption_995.kai` (+ `.out.expected`):
  the reproducer, tree + linear list, asserts `orig=3`. Picked up by the
  native==C parity ratchet (shard 1 walks `examples/perceus`); the explicit
  native gate `test-perceus-995-native-shared-reuse-corruption` anchors the
  absolute correct output (catches a regression that corrupts both backends).
- `examples/perceus/native_dead_donor_reuse_995.kai` (+ `.out.expected`): a
  dead-donor rewrite; gate `test-perceus-995-native-dead-donor-reuse` asserts
  `reuse_in_place > 0` so an over-conservative fix is rejected.

Both gates wired into `.github/workflows/tier1-native.yml`.

## Verification

- Reproducer: `orig=3 new=5` on native AND C; linear list likewise.
- Dead-donor: native `reuse_in_place=7` (preserved); same on C.
- rb-tree shared: RC trace identical pre/post; sums correct on both backends.
- ASAN+UBSan: clean (C build of reproducer + rb-tree, exit 0).
- KAI_TRACE_RC: incref_total/decref_total identical between native and C (8/18
  for the reproducer) — same RC balance.
- Serial parity (`BACKEND_PARITY_JOBS=1`, RC-native discipline): native vs C
  over `examples/perceus` pass=84 fail=0.
- selfhost byte-identical (kaic1 + kaic2 deterministic); tier0 green.

## Structural surprises

- The brief's mental model ("C increfs and evaluates the recursion once") was
  off; an asu consult corrected it to "C duplicates the body into two
  per-branch variants". The native single-body collapse is why the conditional
  `KIncrefIfShared` machinery exists at all (#872 introduced it for the
  embedded-by-reference case; #995 extends it to the consumed-by-call case).
- Ordering matters here in a way the existing `emit_reuse_gc_increfs` comment
  explicitly denied. The denial was true for its original use (embedded leaves)
  and became false for the new use (call-threaded children). Both the position
  and the predicate had to change together.

## Follow-ups left for next lanes

- The native reuse still fresh-allocs the outer node in the shared-donor case
  where C also fresh-allocs — correct, but a shared-donor reuse-in-place is in
  principle impossible anyway (the donor is aliased), so there is no perf gap
  here. The perf gap that remains is the general native-vs-C reuse-rate
  difference already tracked under the native perf cluster, not this lane.
- A separate parser/typer oddity surfaced while building list reproducers: a
  recursive sum whose FIRST constructor is nullary (`type L = LNil | LCons(...)`)
  with certain helper names triggered a spurious "`print` expects 1 arg, got 2"
  error that the `Tree`-shaped reproducer did not. The `t2.kai` variant
  (`type T2 = TE | TN(Int, T2)`) compiled fine, so this is a narrow naming/parse
  interaction, not part of #995. Worth a separate issue if it recurs.
