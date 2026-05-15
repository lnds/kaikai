# Lane experience — Issue #593: primitive-slot extract raw

**Branch:** `issue-593-extract-raw`
**Span:** single session, 2026-05-14
**Outcome:** approach mapped + prototyped to compile-error; reverted to
clean tree. Lane closes as a **diagnosis draft PR** that re-scopes
#593 into a coordinated multi-pass refactor.

## Scope as planned

Per the lane brief and #593 body:

- Recognise match-arm `PBind(name)` sub-patterns whose variant slot
  kind is primitive (Int / Real / Bool / Char) and emit the
  binding as a raw C scalar (`int64_t kair_<name> = scr->as.var.slots[i].i64;`)
  instead of the allocating wrap (`KaiValue *kai_<name> = kai_int(...)`).
- Propagate the raw binding through the typer + perceus passes so
  body reads resolve to `kair_<name>` and no incref/decref discipline
  is emitted for the name.
- Box only at the boundary where a raw binding flows into a context
  that needs `KaiValue *`.
- Acceptance gate: RB-tree bench drops from 12.6× C (post-#440) to
  ≤ 10× C, KAI_TRACE_RC counts drop ≥ 40% vs v0.59.0, cons-list /
  compute stay within their thresholds, selfhost byte-identical,
  Tier 0 + Tier 1 + Tier 1-ASAN green, new fixture
  `examples/perceus/phase4_extract_raw.kai`.

## Scope as shipped

**Nothing implemented.** Code prototype reverted after the first
selfhost attempt failed to compile cleanly. The lane delivers:

- A complete map of the affected codepath (this retro).
- A re-scoped recommendation for the follow-up work.

## What was prototyped

A two-pass change was attempted before revert:

**Pass 1 — unbox pass extension.** Threaded `variants: [EVar]`
through the entire `unbox_*_aware` family (~25 call sites).
Added `arm_pvariant_env` that walks a `PVariant(vname, subs)`
pattern and, for each `PBind(name)` sub on a primitive slot,
adds `LB(name, MUnboxed)` to the env used to walk the arm body.
Mirrors the R8 / lambda-capture guards the SLet path already
applies via `expr_has_interp_use` and `uses_have_in_lam`.

**Pass 2 — emit pass extension.** Added an `emit_pat_binds_rw`
variant of `emit_pat_binds` that carries a `raw_names: [String]`
list. In `emit_pat_binds_variant`, when the slot kind is non-zero
and the sub-pattern is `PBind(name)` with `name ∈ raw_names`,
the bind is emitted as `int64_t kair_<name> = scr->as.var.slots[i].i64;`
(no allocation, no RC). The legacy boxed shape stays for every
other sub-pattern.

Computed `raw_names` from `emit_match_arm` via a new helper
`arm_raw_extract_names` that mirrors the unbox-pass predicate
(slot kind 1/2 + R8 + lambda-capture + interp guards), so the
two passes agree on which names take the raw shape.

Diff size at the abort point: **~260 LOC added, ~100 removed**.

## Why it didn't compile

After the prototype was wired up, `make tier0` failed at the
stage 2 → stage 2b roundtrip: `kaic2b.c` referenced both
`int64_t kair_after1` (the new raw bind shape this lane writes)
**and** `KaiValue *kai_after1` (which no longer exists).

The undeclared identifiers came from **perceus pass insertions
that the unbox pass cannot influence.** Specifically:

1. **`pcs_arm_drop_arms`** (line 41541) walks every match arm,
   computes `bound = pat_binders(pat, [])`, and emits one
   `__perceus_drop(kai_<name>)` per `name` in `bound` that isn't
   in `outer_scope`. The `pat_binders` helper returns every
   `PBind` name regardless of slot kind — so a raw-extract name
   ends up with a `kai_internal_drop(kai_<name>)` against a
   non-existent boxed alias.
2. **`pcs_rewrite_expr` / `pcs_rewrite_kind`** wraps every
   non-last `EVar(name)` use in `__perceus_dup(EVar(name))`.
   The wrap site emits `kai_internal_dup(kai_<name>)`, again
   against a binding that the new emit path never declared.

In short, the unbox pass marks the body's `EVar(name)` reads as
`MUnboxed` (so they resolve to `kair_<name>` via
`emit_kind_raw`), but the perceus pass inserts **new** `EVar`
nodes for dup/drop that don't carry that mode — they end up
emitted via `emit_ident_value`, which prefers the boxed
`kai_<name>` shape.

The reuse-in-place pass (`pcs_recognise_reuse_expr`) and the
tcrec pass (`tcrec_rewrite_decls`, dropmask computation) have
the same blind spot for the same reason: they were written
when every match-arm bind was uniformly `KaiValue *`.

## The shape of the real fix

Closing #593 cleanly requires a coordinated change across
**at least five passes**:

1. **Unbox pass** — marks `LB(name, MUnboxed)` on raw-eligible
   match-arm binds (this prototype's pass 1).
2. **Emit pat-binds** — writes the raw extract shape
   (this prototype's pass 2).
3. **Perceus arm-drop pass** — filters raw names out of `bound`
   so no `__perceus_drop` is synthesised against `kai_<name>`.
4. **Perceus dup pass** — filters raw names out of the scope it
   threads into `pcs_rewrite_expr`, so non-last uses don't get
   wrapped in `__perceus_dup`. (Raw scalars have no RC, so the
   dup is meaningless even when emitted correctly.)
5. **Tcrec dropmask** — when computing the dropmask for a
   self-tail-call inside a raw-bound arm, the raw names must
   be excluded from the per-param drop list. The mask emit
   already produces `kai_internal_drop(kai_<p>)` and would
   reference the non-existent boxed alias.

Each pass needs the same `arm_raw_extract_names` predicate that
this prototype already factored. Threading the predicate through
all five passes is mechanical, but each pass walks the AST
independently and the threading touches ~80 call sites
(`fns: [EFn]` already follows this shape; adding `vars: [EVar]`
beside it is a parallel mechanical change).

Reuse-in-place (`pcs_recognise_reuse_expr`) is a sixth pass
that intersects with raw extract but only in a narrow shape
(`Arm(PVariant(_, ps), None, ECall(EVar(C), args))` where the
rebuild repeats every binding). For RB-tree's `balance` arms
the rebuild reads every Int slot, so raw extract *must* feed
back into the reuse rebuild as a boxed re-wrap — at the same
allocation cost as today. The wrinkle is that the unbox pass
must NOT mark those names MUnboxed when reuse-in-place
recognises the arm; otherwise the rebuild loses access to a
boxed view. The cleanest split is: run reuse recognition
first, and the unbox-pass raw promotion sees the already-rewritten
shape (with `__perceus_reuse_*` callees, which the predicate
treats as a boundary-boxing site).

## Why this exceeds a single lane

The brief estimated 3–5 days. The honest re-estimate, given
the five-pass coordination, is **8–14 days** plus selfhost
recovery sweeps. Specific risks:

- **Five-pass invariant.** Each pass must agree on the same
  raw-eligibility predicate. Drifting copies of
  `arm_raw_extract_names` between passes will produce silent
  mismatches that fail at C compile time (best case) or at
  RC discipline (worst case — would surface only via ASAN).
- **Tcrec interaction.** The dropmask is computed pre-tcrec
  rewrite and consumed post-tcrec. A raw-eligible name that
  flows through a self-tail-call site needs the boxed shape
  for the recursive call (the callee expects `KaiValue *`),
  so the boundary-box wrap kicks in inside the tcrec
  rewriter — that needs the predicate too.
- **Reuse recogniser ordering.** As described above, the
  raw promotion must happen after reuse recognition so the
  rebuild sites get a boxed view. The current pipeline
  order is `unbox_pass → perceus_pass → pcs_recognise_reuse_expr`;
  adding raw promotion is either a pre-unbox marker (rejected
  here — too invasive) or a post-reuse marker (which forces
  splitting the unbox pass into two stages).

## Brief's stop conditions

The brief listed four:

1. **Selfhost diff doesn't resolve in 2–3 iterations** —
   selfhost never reached selfhost diff; tier 0 failed at
   the C compile step with the prototype's first build.
2. **RB-tree doesn't reach ≤ 10× C** — not measurable without
   a working compile.
3. **ValueMode load-bearing change in typer** — not triggered;
   the prototype reused `MUnboxed` from #383.
4. **Scope >2000 LOC** — not triggered. Diff at abort was
   ~260 LOC, but the projection for the five-pass fix lands
   closer to ~800–1200 LOC.

Condition 1 is the one that hit, in spirit if not in letter:
the failing build surfaced a structural blind spot in three
post-unbox passes that the brief's scope did not include.
Continuing would have required either a far larger diff than
the brief envisioned or a fragile workaround.

## Acceptance gate honesty

**NOT MET.** RB-tree stays at 12.6× C. No new fixture shipped.
KAI_TRACE_RC counts unchanged from v0.59.0.

#593 stays **open** with this diagnosis as the basis for a
re-scoped follow-up. The follow-up brief should:

- Acknowledge the five-pass coordination as load-bearing.
- Plan reuse-in-place ordering before unbox promotion.
- Factor `arm_raw_extract_names` into a shared helper that
  every consuming pass calls.
- Budget 8–14 agent days instead of 3–5.

## Structural surprises

**The unbox pass and the perceus pass disagree on what
`Arm.pat`'s binders mean.** Unbox sees them as candidates for
representation promotion; perceus sees them as `KaiValue *`
slots that need dup/drop discipline. They were designed
independently and the disagreement is invisible until the
binding representation changes — which is exactly what #593
attempts.

This is the deeper architectural finding of the lane: kaikai's
match-arm binding pipeline is not value-mode-aware end to end.
#383 (Phase 3 unboxing of call boundaries) succeeded because
the binding representation at the call site is a function
parameter (`emit_fn_body` already plants `kair_<p>` and routes
through `unbox_field_for` at the boundary), and perceus already
treats UFn params as RC-free via `is_ufn_decl`. Match-arm binds
have no analogous escape hatch — every pass assumes they live
in the boxed-`KaiValue *` world.

The natural follow-up is to introduce a `BindingMode` on
`Pattern.pkind` (or on `Arm`) that every consuming pass
consults, with the unbox pass computing it once and downstream
passes (perceus, tcrec, reuse, emit) treating it as
load-bearing. That is the right design but it is a separate
lane in its own right.

## Real cost vs estimate

| | Estimate | Actual |
|---|---|---|
| Brief estimate (this lane) | 3–5 days | — |
| Issue body estimate | (small lane) | — |
| **This session** | — | one session — mapping + prototype + revert + retro |
| Realistic re-estimate (5-pass fix) | — | 8–14 days |

The prototype work was worth its cost: it surfaced the
five-pass coordination requirement that neither the issue
body nor the brief anticipated. Without the prototype, the
follow-up lane would have walked into the same wall.

## Follow-ups left for next lanes

1. **Re-scope #593 as a 5-pass coordination lane.** The brief
   above is the starting point. Reuse the prototype's
   `arm_raw_extract_names` predicate and the unbox-pass
   `variants` threading.
2. **Optional: introduce `BindingMode` on `Pattern.pkind`.**
   The architecturally clean way to avoid five-pass
   coordination is to encode the mode in the AST itself.
   Larger up-front change; cleaner downstream.
3. **Drop specialisation (#384).** Reconsider priority once
   the extract refactor lands. Until #593 closes the gap,
   #384 alone cannot close DoD #4b.
4. **Reuse-in-place over typed cells (#118 + #209 follow-on).**
   Unchanged: still gated on the layout work landing in #440.
5. **LLVM backend typed construction.** Unchanged.

## Acceptance gate honesty (restatement)

Per the lane brief: *"Si RB-tree NO baja a ≤10× C, reportar el
número real y parar — predicción incorrecta."* The honest
read is that the *direction* of the prediction was right (raw
extract on primitive slots is the lever that closes the gap)
but the *scope* in the brief was wrong: it assumed the change
was contained in unbox + emit, when in fact it spans five
passes. RB-tree stays at 12.6× C until the follow-up lands.
