# Lane experience — issue #868: native cons/list RC leak (`free_total=0`)

## Scope as planned vs as shipped

**Planned (brief).** Fix the native (in-process libLLVM) backend's cons/list RC
leak — `free_total=0` on the `native_cons_selftail_leak_860` fixture, turning
tier1-native green. The brief framed it as a regression of #860's fix and
pointed at the self-tail / TRMC back-edge *missing a drop*, with a bisect of the
`bea17616..c1786673` window (suspect #1: `7261b93f`'s per-match scrutinee drop
binder refactor).

**Shipped.** A one-file change in `kir_lower_walk.kai`: gate the per-match
scrutinee `dup` in `match_preamble` so it fires ONLY for a match that owns a
reuse arm; every other owned match binds the exit-drop binder BARE, matching the
C oracle's linear scrutinee consumption. This restores the cons cascade for all
list/cons shapes (the fixture's `build`/`suml`/`double`, list-fold loops,
map/filter) and leaves the rb-tree variant-reuse leak — a distinct, pre-existing
native-reuse under-incref — as a flagged follow-up.

## The diagnosis: the brief's frame was wrong (over-incref, not missing drop)

The brief's mental model was "native SKIPS the owned-scrutinee drop on the
self-tail back-edge." The trace said otherwise:

```
suml([1,2,3], 0)   C: alloc=10 free=3 incref=2 decref=5
                native: alloc=4  free=0 incref=5 decref=3
```

Native already emits decrefs (3) — it is not missing the drop. It is
**over-increfing**: `incref=5 > decref=3`, so the cons cell's refcount never
reaches zero and never frees. The bisect-window story also did not hold up: the
#860 fix (`08f0c5d7`) and the suspect (`7261b93f`) both merged AFTER the first
tier1-native failure (`c1786673`/#862). What actually happened: #862 broke
tier1-native with 15 backend-parity SEGFAULTS (a different bug, since fixed by
#864 et al.); the cons-leak fixture did not exist until #863. At HEAD the parity
ratchet is already green (`pass=484 fail=0`) — the ONLY remaining tier1-native
failure is the cons-leak gate. So this lane is purely the cons cascade, not the
#862 parity segfaults.

The KIR (shared by both backends, since both run after perceus) showed the
smoking gun. `match_preamble` emitted, for every owned match:

```
dup xs            # blanket scrutinee dup  (added by 7261b93f)
.t0$sd = xs       # exit-drop binder
...
drop .t0$sd       # exit drop
```

For a live-binder scrutinee (rb-tree `match ll`), the KIR then had TWO `dup ll`
— the perceus producer-side dup AND this blanket one. The C oracle
(`emit_match_default_owned`) binds `_scr = sc` with NO dup — exactly one. So the
blanket dup is a pure over-incref: +1 per self-tail iteration that nothing
balances, blocking the cascade. `7261b93f`'s commit message claimed it "mirrors
the oracle's `_scr = kai_internal_dup(<scrv>)`"; that is factually wrong — the
oracle never dups the scrutinee.

## Which shape broke, and the false bottom under it

Two changes rode in `7261b93f`:
1. a FRESH per-match exit-drop binder `<join>$sd` (fixes the `max([T])`
   alloca-collision double-free) — CORRECT, kept untouched.
2. a blanket scrutinee `dup` — the over-incref. This is the regressor for the
   cons cascade.

First attempt: remove the blanket dup entirely (the asu architect's first call,
"the oracle never dups, perceus already inserts the producer dup"). This fixed
suml/build/double to free EXACTLY like C (incref/decref counts matched) — but
**crashed gap3** (rb-tree rotation) with EXC_BAD_ACCESS at driver `n≥3`.

Why: the native `KConReuse` reuse lowering emits a SINGLE donate-shape arm body
and leans on `kaix_variant_reuse_at`'s runtime uniqueness fallback. When the
donor is SHARED (non-unique — the rotation case, `ll` referenced by `l`), that
fallback `kai_variant_u`'s a fresh node and MOVES the kept children WITHOUT the
per-child incref the C oracle emits in its non-unique reuse arm
(`emit_match_arm_reuse_variant`). So the fresh node and the still-live original
subtree both own `lll`/`llr` with no extra ref → the later `drop l` cascade
frees them while the result holds them → UAF. The blanket dup was MASKING this:
it inflated the parent RC enough that the premature frees never reached zero,
turning the UAF into a (universal) leak.

So neither "always dup" (leaks every cons) nor "never dup" (UAF on shared-donor
reuse) is correct. The two cases are genuinely distinct.

## The fix: gate the dup on reuse-arm presence

`match_owns_reuse_arm(x)` = any arm body holds a `__perceus_reuse_*` call
(reusing the existing `arm_body_uses_reuse` predicate). Keep the scrutinee dup
ONLY for such a match (its scrutinee is the reuse donor; the dup keeps the
under-increfed children alive); bind bare otherwise. This is sound because:
- a NON-reuse match consumes the scrutinee linearly — one exit decref frees it,
  identical to the oracle (cons cascade restored);
- a reuse match keeps the dup, so it behaves EXACTLY as before this lane (correct
  output, leaks) — no new crash, no regression.

The rb-tree variant-reuse leak is therefore neither introduced nor fixed here:
it leaked before (everything did, `free_total=0`) and still leaks, but is now
the ONLY residual rather than the universal symptom. Its real fix is to port the
C oracle's two-branch (`kai_check_unique`) reuse emission — or the runtime
token protocol (`kaix_drop_reuse_token`/`kaix_variant_at`, already exposed) —
to the native `KConReuse` lowering so the non-unique path increfs kept children.
That is a separate high-risk lowering change, out of scope for the cons/list
cascade #868 closes.

## Fixtures / shapes verified beyond the gate

- Gate `test-perceus-860-native-cons-leak`: OK (`free_total=2000 == alloc-1`).
- Per-shape RC vs C (native now matches C's free/incref/decref exactly):
  `suml`, `build`, `double`, 3-cons `tiny`, list-fold loop at 1000×100
  (`free_total=100000`, `leaked=1`), map/filter cons-modulo.
- gap3 driver at n=3 and n=8: out parity, exit 0 (was 139 with the bare-bind
  spike).
- ASAN (instrumented native: bitcode hidden, `runtime_llvm.c` compiled
  `-fsanitize=address,undefined`): CLEAN on max/min over lists (the `max([T])`
  double-free `7261b93f` fixed — still survives), gap3, double, suml, map/filter.
- SERIAL backend-parity ratchet (`BACKEND_PARITY_JOBS=1`): `pass=482 fail=2
  skip=58`, both "fails" flaky-passed on recheck, `ratchet OK — 0 gaps`. The 4
  known mac-only pre-existing fails did not surface.
- selfhost byte-id: OK (`kaic2b.c == kaic2c.c`).
- tier0: OK (selfhost deterministic, demos baseline 35/35, arena gate).

## Perf delta (`RUNS=5 tools/native-perf/run.sh`, mac-arm64, best-of-5)

| bench | C (s) | native (s) | gap before | gap now |
|---|---|---|---|---|
| scalar (`arith_*`, `deep_rec`, `variant_match`) | — | — | parity | parity |
| `list_fold` | 4.95 | 6.40 | 1.95× | **1.29×** |
| `rbtree_corpus` | 1.14 | 2.72 | 2.22× | 2.39× |

`list_fold` closed most of its heap-bound gap (the cons cascade no longer
accumulates garbage). `rbtree_corpus` is essentially flat — it is dominated by
the variant-reuse leak this lane deliberately leaves (the dup-gate exception),
confirming the fix targets the cons/list mechanism precisely and the rb-tree
residual is the next lever. Native binaries stay ~30% SMALLER (92 KB vs
132–149 KB).

## Coverage gap

The existing fixture only checks the cons cascade (`free == alloc-1`) on
list/cons shapes and output parity. It does NOT check RC balance on a
variant-reuse (rb-tree) program — which is exactly why the rb-tree leak sat
unnoticed and why the blanket-dup leak read as "fixed" (output was always
correct). A follow-up RC-balance fixture over an rb-tree insert should be added
WITH the variant-reuse fix.

## Follow-ups for next lanes

- **Native variant-reuse under-incref (rb-tree).** `KConReuse` →
  `kaix_variant_reuse_at` donates kept children without the per-child incref the
  C oracle's non-unique reuse arm emits; shared-donor rebuilds leak (rb_tree_bench
  native `free_total=23` vs C `6.3M`; ~2.9× slower). Fix by porting the oracle's
  two-branch (`kai_check_unique`) emission, or wiring the already-exposed runtime
  reuse-token protocol, into the native reuse lowering — then the
  `match_owns_reuse_arm` dup-gate exception can be removed entirely and native
  matches the oracle for ALL shapes. Gate: ASAN + KAI_TRACE_RC balanced on
  rb_tree_bench + serial ratchet + selfhost.
- **tier1-native is NOT a required status check** (required = tier0, tier1). That
  is why this regression sat red on `main` across #863/#864/#869/#870 merges —
  the merges landed because tier1-native is advisory. Making it required would
  have caught it at the #862 boundary (where it first went red, for the parity
  segfaults). Worth promoting now that native is the default backend.

## Cost vs estimate

The brief estimated a bisect + a back-edge drop port. Real cost: the bisect
window was a red herring (the regressor was a blanket dup that landed with the
#860 fix's neighbour, not a missing drop), and the "obvious" fix (remove the dup)
hit the #856 double-free trap exactly as warned. The genuine work was
distinguishing the over-incref (cons leak) from the reuse under-incref (rotation
UAF) and finding the gate that fixes the former without unmasking the latter —
two asu rounds and a spike that crashed gap3 before the reuse-arm gate landed.
