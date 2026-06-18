# Lane experience — native cons/list RC leak (#860)

## Scope as planned vs. as shipped

**Planned (brief):** close the native backend's cons/list RC leak — a
self-tail-recursive arm over an OWNED scrutinee never cascaded
`decref -> free`, leaving `free_total=0` and dragging `list_fold ~1.84×`,
`rbtree ~2.2×` over C. The brief pointed at one site: the owned-scrutinee
match-exit drop that the self-tail (`KTcrecGoto`) back-edge seals before
(`kir_lower_walk.kai` `match_finish` + the deferred NOTE), with the trap
that a naive drop double-frees the TRMC cons-modulo cell.

**Shipped:** the leak was NOT one site but **three** distinct self-tail RC
gaps, all in the native KIR→LLVM path, all closed by mirroring the C oracle
exactly:

1. **Consuming self-tail** (`suml(t, acc + h)`) — the owned-scrutinee
   match-exit drop sits in the continuation the goto seals before, so it
   never ran. Fix: pre-inject the scrutinee drop INTO the arm before the
   body lowers (`match_selftail_scr_drop` in `kir_lower_walk.kai`),
   mirroring the C oracle's `_r = ({ kai_decref(_scr); ... })`
   (`emit_match_arm` / `emit_match_arm_raw`). Gated on
   `owned ∧ arm_body_all_tail_self_tcrec` — the exact `tcrec_tail_always_
   sentinel` predicate, which tests `es_tcrec_is_sentinel` (the pure
   `__kai_tcrec|` self-tail) and EXCLUDES `__kai_trmc|`, so a cons-modulo
   arm never gets the drop (the #856 double-free trap avoided by
   construction).

2. **Accumulator self-tail** (`build(n-1, [n, ...acc])`) — the
   `KTcrecGoto` back-edge re-stored `acc` from the new cons but never
   dropped the OLD `acc`. The native backend was **ignoring the dropmask**
   (`_ignored_dropmask` in `nemit_tcrec` / `nemit_trmc_step`), the very
   bitfield the C oracle replays as `tcrec_emit_drops_masked`. Fix: emit
   the masked drop on the native back-edge between eval and store
   (`nemit_drop_assigns_masked` in `emit_native_fn.kai`), reading the
   raw/boxed distinction the C oracle gets from `pmask` directly from the
   param's KIR slot tag (`native_ctx_reg_slot_at == 0 ⇒ SBoxed`). A raw
   scalar param is never dropped (the #709 corruption).

3. **TRMC cons-modulo self-tail** (`[h*2, ...double(t)]`) — the
   `__kai_cons_s` step consumes the scrutinee, but the back-edge skips the
   match-exit drop. The native step fresh-allocated the cell (no
   reuse-in-place) and never dropped the consumed scrutinee, leaking the
   input spine. Fix: the step drops `__pcs_scr_drop` for the `_s` cname
   (`ntrmc_extend_and_loop_scr` in `emit_native_trmc.kai`), mirroring the C
   oracle's `scr_drop` in `emit_trmc_cons_step`. The plain `__kai_cons`
   (raw EList `build`) has no scrutinee to free, so it is not dropped.

## Design decisions and alternatives considered

- **Mirror the C oracle's discriminant, do not invent one.** The brief
  warned the fix "needs the C oracle's pmask raw/boxed param distinction to
  drop precisely". The oracle already had every piece: `tcrec_tail_always_
  sentinel` (pure-tcrec ≠ trmc), `tcrec_emit_drops_masked` (dropmask ∧
  boxed-slot), `emit_trmc_cons_step`'s `scr_drop` (`__kai_cons_s` only).
  Every native helper is a 1:1 port reading the same facts (the KIR slot
  tag IS the pmask the C reads from `US(_, _, pmask)`). No new analysis was
  needed — the deferral was a porting gap, not a missing capability.

- **Where the predicate lives.** `arm_body_all_tail_self_tcrec` was written
  in `kir_lower_walk.kai` (not reused from emit_c) so the lowering depends
  only on `pub` symbols (`es_tcrec_is_sentinel` from `emit_shared`), keeping
  `emit_c.kai` untouched (selfhost byte-id intact) and the predicate local
  to its single use.

- **Shared drop helper placement.** `nemit_drop_assigns_masked` lives in
  `emit_native_fn.kai`, imported by both `emit_native_term` (KTcrecGoto) and
  `emit_native_trmc` (KTrmcStep) without a term↔trmc import cycle (fn is
  below both in the DAG). The bundle hides the cycle; selfhost would catch
  it — so it was designed acyclic from the start.

- **No reuse-in-place.** The TRMC cons step still fresh-allocs (the native
  cons path never captured a reuse token — same as the C oracle, see
  `emit_trmc_cons_step`'s "no `_arm_ru` donation"). Donating the scrutinee
  cell to the step is a perf follow-up, NOT a correctness gap; the fix
  drops the consumed scrutinee instead, exactly as C does.

## Structural surprises the brief did not anticipate

- **The leak was three sites, not one.** The brief's repro (`build`+`suml`)
  conflated a CONSUMING leak (suml, the brief's named site) with an
  ACCUMULATOR leak (build, the ignored dropmask) and — once both were
  closed — a TRMC cons-modulo leak (map/double). Isolating with a literal
  list (`[1,2,3]` + suml cascaded cleanly after fix #1) vs. `build` (still
  leaked) vs. `build`+`double` (the `_s` step) was what split them apart.

- **The `runtime_llvm.bc` red herring.** Early on, the same program
  cascaded perfectly when the native runtime was the cc-compiled
  `runtime_llvm.c` (`free_total=3`) but leaked under the vendored
  `runtime_llvm.bc` (`free_total=1`). This looked like a stale-bitcode bug;
  it was not — the bitcode is a pure function of `runtime_llvm.c`+
  `runtime.h` (neither touched). The real cause was the codegen sites
  above; the `.bc` path simply made the leak visible at the SAME counts the
  cc path did once the codegen was fixed. The runtime never needed a change.

- **`free_total=0` even for hello-world** (the issue's headline) is the
  consuming-leak's most degenerate case: every owned scrutinee a match
  consumes leaked, so even a one-line program with a single match left
  `free_total=0`. Fix #1 alone takes hello-world to balanced; the build/map
  fixes close the loop-scaling residual.

## Fixtures added and coverage

- `examples/perceus/native_cons_selftail_leak_860.kai` (+`.out.expected`)
  — exercises all three shapes (build accumulator, double TRMC cons-modulo,
  suml consuming) over 50 reps of a 20-element list (1000 cons). Picked up
  automatically by the native-vs-C parity ratchet (output parity).
- `make test-perceus-860-native-cons-leak` (stage2/Makefile) — native-gated
  RC gate asserting `free_total == alloc_total - 1` (every cons cascades;
  the lone residual is the result string). Wired into the tier1-native
  workflow as a dedicated step (the parity ratchet gates output; this gates
  the RC ledger directly).

Coverage gap left: reuse-in-place for the TRMC cons step (the perf
follow-up) is not added — the fix is correctness (drop the consumed cell),
not zero-alloc. `map`/`filter` now match the C oracle's RC ledger
byte-for-byte (`incref_total`/`decref_total` identical), so the residual
`leaked` they share with C is program behaviour, not a backend gap.

## Verification

- Repro `build(N)`+`suml`: native `free_total=N` (was 0), `leaked=1`
  constant for N ∈ {10, 1000, 100000} — was `leaked=N`. `decref_total`
  identical to the C oracle (29 for N=10).
- `map`/`filter` (the #856 double-free trap): output correct, no crash,
  `incref_total`/`decref_total`/`free_total` identical to the C oracle.
- selfhost byte-id: OK (the KIR drops do not touch the AST→C emitter).
- SERIAL native-parity ratchet (`BACKEND_PARITY_JOBS=1`): 0 new gaps.
- ASAN: clean over the map/filter + build/suml shapes.

## Real cost vs. estimate

Diagnosis dominated: the brief's "one site" framing meant the first fix
(consuming) looked complete (hello-world balanced) but the loop repro still
leaked. The split into three sites came from disciplined isolation
(literal-list vs. build vs. map), each pointing at a distinct oracle helper
the native path had stubbed. Once located, each fix was a small, mechanical
1:1 port of an existing C-oracle helper.

## Follow-ups for next lanes

- **Reuse-in-place for TRMC cons** (perf): donate the consumed scrutinee
  cell to the step (`kai_reuse_or_alloc_cons`-style) instead of fresh-alloc
  + drop — zero alloc per level. The C oracle defers this too; it is a perf
  lane, not correctness.
- The `_ignored_dropmask` comment in `emit_native_trmc.kai` /
  `emit_native_term.kai` is now obsolete; both back-edges honour the mask.
