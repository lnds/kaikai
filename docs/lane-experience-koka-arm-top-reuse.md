# Lane retro — Koka bind-borrow + arm-top reuse (rb-tree toward 1×C)

Commit: `349298b` (on `main`). Base: `c44a445` (#741, ~5×C).

## Scope as planned vs as shipped

**Planned:** port Koka's coupled reuse model (Parc bind-borrow + drop_reuse +
alloc_at + reuse-token-by-size) so the rb-tree's reuse-in-place fires and RSS
drops from 1.8 GB toward Koka's 48 MiB (1.02×C).

**Shipped:** the *consumer half* of the model, gated to the one arm shape it
is provably sound for without inter-procedural borrow:
- `kai_drop_reuse_token` (runtime) = Koka `kk_block_drop_reuse`, borrow model.
- `emit_arm_top_reuse` (emit) = Koka insert_loop shape: borrow binds +
  arm-top `is_unique` decision + TRMC step reusing `_arm_ru` via
  `kai_variant_at`. Gated by `arm_all_tails_consume_token` (every tail leaf is
  a TRMC step or ctor rebuild → the captured token is always consumed).
- `pcs_all_consumers_linear` fix: a resolved ctor `EModCall(_, Ctor)` is linear
  (was a #599 hole blocking every reuse-arm binder).

**Measured:** rb-tree N=1M incref 96.2M → 86.4M (−10%). `reuse_in_place` STILL
flat (5.65M) — the win is fewer increfs, not yet reuse firing. Gates: ASAN
(rb-tree + regexp), selfhost byte-identical, demos 34/34, tier0 + tier1 green.

## The fundamental wall (the load-bearing finding)

The Black arm of insert_loop — the **majority of the tree** — cannot reuse
without **B3 (inter-procedural borrow inference)**. Confirmed by ~6 distinct
measured attempts. The chain, each link verified:

1. reuse fires only when `kai_check_unique(_scr)` is true → `_scr` must be
   UNIQUE down the descent.
2. `_scr` is unique only if the descent does NOT incref it → binds must be
   BORROW (no incref).
3. but the Black arm has a `balance_left(insert_loop(l,...), ..., r)` branch:
   `l`/`r` are passed to FNS. Moving a borrow-bound binder into a fn is sound
   ONLY if the fn CONSUMES it. The compiler does not KNOW consume-vs-borrow
   without B3 — `pcs_all_consumers_linear` assumes every user-fn call consumes,
   which is FALSE in general (the stdlib/regexp UAF: `nb_add_transition`
   field-reads its arg).
4. tried the OWNED model for the Black arm (incref binds +
   `kai_drop_reuse_token_owned` drops children): SOUND but INERT — incref
   shares the tree, `_scr` never unique, reuse stays flat. Measured.

So: the Red arm (pure modulo-cons, no external fn call) → borrow safe (the TRMC
step is the guaranteed consumer) → SHIPPED. The Black arm → needs B3.

**The path to Koka's RSS is B3**: a per-fn analysis recording, per param,
consume-vs-borrow (Koka's `Core.Borrowed` + Parc owned/borrowed environment —
NOTE Koka does NOT infer this, it reads the programmer's `borrow`/`fip`
annotation; params are owned-by-default). kaikai has no such annotation, so B3
must either infer it (worklist over the call graph; B1 skeleton exists in
`pcs_borrow_params`) or adopt an annotation. The arm-top + drop_reuse +
alloc_at infra shipped here is the consumer side, READY for B3 to feed it
unique cells.

## Design decisions & alternatives considered

- **perceus SIGNS, emit TRANSCRIBES.** The reuse decision (which binders move)
  is made in perceus (pre-dup AST) and READ off the AST by emit (bare-use vs
  `__perceus_dup`), never recomputed. Recomputing the branch-aware analysis on
  the post-dup AST diverges → infinite-alloc OOM (hit 4×). This is exactly why
  Koka separates ParcReuse (reads Parc's drops) from Parc.
- **Koka dups children CONDITIONALLY in the shared branch**, not
  unconditionally (verified against Koka's generated C — asu initially claimed
  unconditional; the C disproved it). The arm-top emits
  `if (_arm_ru == null) { dup children }`, matching Koka.
- **arm-LOCAL branch-aware skip-set** (move the recolor arm's binders for a
  further −10M incref): written, SOUND on rb-tree, but USE-AFTER-FREES
  stdlib/regexp — the record/field linearity check has a hole the
  `arm_all_tails_ctor` gate doesn't cover. Reverted; needs robust
  record-consumer linearity (≈ B3) before re-enabling.

## Structural surprises the brief did not anticipate

- The handoff listed "strcmp 55%" as a lever; it does not exist — match
  dispatch is already `variant_tag == N` (0 strcmp), flipped in a prior lane.
- The real gap is RSS (39×C), not wall — and wall is a consequence of RSS.
- The reuse-in-place and borrow-inference levers the handoff listed separately
  are the SAME problem seen from two sides — reuse cannot fire without borrow.

## Bugs found + fixed

- slot_mask is 2-bit-per-slot base-4 (`kai_var_slot_kind`), NOT a 1-bit bitmap
  — decoding it as 1-bit decref'd a tagged Int as a pointer → SEGV.
- `EModCall(_, Ctor)` not treated as linear (#599 hole).
- `SAssign`/`SExpr` Stmt variants do not exist — `make kaic2` (via stage1)
  tolerated the typo, **selfhost caught it**. The hard gate earned its keep.

## Cost vs estimate

Far over. The "first measurable hit" turned into a multi-attempt excavation of
why every reuse strategy is inert without borrow. The value is the validated
method (sign/transcribe, the consume-token gate) + the −10% + the precise B3
diagnosis, not the −10% alone.

## Follow-ups for next lanes

1. **B3 inter-procedural borrow** — the remaining ~80% of the gap. Without it
   no reuse strategy reaches Koka's RSS. See
   `project_kaikai_reuse_needs_borrow_coupled` (memory) and
   `project_kaikai_borrow_inference_design`.
2. Re-enable the arm-local skip-set once a robust record/field consumer
   linearity (≈ B3) lands.
3. nested-cell reuse in balance_left/right (asu's 2-donor design) — a separate,
   lower-frequency lever (~1M rotations vs ~14M descents).
