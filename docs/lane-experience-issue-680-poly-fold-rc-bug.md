# Lane experience — issue #680: polymorphic fold RC bug (2026-05-23)

## Scope as planned

Fix `henua.aggregate.fold_events(state, events, step)` segfaulting at
runtime when `events.length >= 4`. Pre-fix symptoms: cross-module call
crashed with bus error (exit 138); inline-defined call crashed with
"kai: field access on non-record" (exit 1). Bisect pinned the
regression to commit `788a236 perf(perceus): activate branch-aware dup
elimination (#599)`. The user explicitly rejected reverting #599 —
required architectural fix.

## Scope as shipped

Two changes to `stage2/compiler.kai`, both surgical:

1. **`pcs_strip_dup_wrap`** + use in `pcs_call_consumers_linear`
   (~15 LOC). When the linearity analysis runs on a *post-perceus*
   body inside `tcrec_rewrite_decl`, the callee of a function call
   may have been wrapped in `__perceus_dup(x)` (e.g.
   `step(initial, ev1)` becomes `(__perceus_dup(step))(initial,
   ev1)`). The pre-fix predicate refused to recognise this as a
   linear call (its `match f.kind` only accepted bare `EVar`),
   causing the param to drop out of `skip_set` post-perceus.
   `pcs_pass` had already used the **pre-perceus** skip_set
   (which DID include the param) and elided the defensive
   `dup(initial)`. The misalignment plants a goto-block
   `drop(initial)` without the matching `dup`, double-decrefing
   the original ref on the third iteration of the TCO loop.

   The fix strips the outer `__perceus_dup(x)` wrap so the
   linearity analysis matches the pre-perceus shape.

2. **`retarget_self_calls_decl`** + call from `emit_spec`
   (~40 LOC). The monomorphizer-pre-fix bug: a spec body's
   self-recursive call kept pointing at the polymorphic original
   name. The fn mono `fold_events__mono__S__Ev__Any` body called
   `fold_events` (the poly) instead of itself. That meant the
   mono had no TCO; the poly did. The cross-boundary call chain
   `main → mono(1 iter) → poly(TCO loop)` was the only path that
   exposed the #599 RC misalignment, because the poly and mono
   ran `pcs_branch_aware_skip_params` over different bodies (the
   spec's pre-mono body vs the poly's pre-perceus body).

   The retargeter walks the spec body after `subst_decl` /
   `rewrite_callsites_decl_sm` and rewrites every
   `ECall(EVar(orig_name), args)` to
   `ECall(EVar(mangled_name), args)`. Bare `EVar(orig_name)`
   (function-as-value) is left alone so the polymorphic
   dispatch surface stays available for partial application
   and higher-order use. After the rewrite,
   `tcrec_rewrite_decls` (which already runs post-mono on the C
   path) picks the spec up as a self-recursive function and
   plants its own TCO goto loop.

Both fixes are independently load-bearing. The retargeter closes the
structural gap (mono should TCO itself); `pcs_strip_dup_wrap` closes
the predicate-mismatch gap that #599 exposed. Without (2) the mono
keeps delegating to the poly and the boundary mismatch can still bite
in other recursive shapes; without (1) the mono now has its own TCO
loop but `pcs_branch_aware_skip_params` still disagrees with
`tcrec_compute_site_dropmask` whenever a closure callee is wrapped
post-perceus.

## Design decisions

### Why not revert #599

#599's branch-aware dup elimination saves ~7.9% of dups in
compiler.kai self-compile (per its retro). The user pinned: "no
revertir. hay que hacer lo correcto, y eso es C." The fix had to
preserve the optimisation and close the bug structurally.

### Why retarget at the AST level, not the spec emission level

The retargeter could have lived inside `rewrite_callsites_kind_sm`
as a self-call fast path before the general `resolve_call_inst`
gate. Both linus and asu reviewed; asu argued for a dedicated
walker run at the end of `emit_spec` rather than inlining the
check into `rewrite_callsites_kind_sm`. asu's reasoning:
`rewrite_callsites_kind_sm` is also used from
`rewrite_callsites_decls_full` (pre-mono) where the self-spec
context doesn't apply; mixing paths in one fn complicates the
reasoning. Took the dedicated-walker route.

### Why strip only `__perceus_dup`, not other wraps

The post-perceus body contains exactly one wrap shape that
predates the linearity analysis: `__perceus_dup(x)`. Other
non-EVar callees (computed field reads, method invocations, etc.)
are genuine non-linear callees that the predicate should reject.
Stripping selectively keeps the conservative posture intact.

### Why the mono still keeps the poly around

Both reviewers (linus, asu) flagged: kaikai's uniform boxing
model needs the poly version available for function-as-value
uses (partial application, closures over polymorphic fns,
higher-order callers). Rust monomorphizes and drops the poly
because it has fat pointers / trait objects; kaikai's uniform
`KaiValue *` representation makes that trade-off different.
Poly + mono coexistence is correct here, not a design smell.

## Structural surprises

1. **`pcs_branch_aware_skip_params` is called on two distinct
   bodies** — once in `perceus_decl` (pre-perceus body) and once
   in `tcrec_rewrite_decl` (post-perceus `inner` extracted from
   the `__pcs_ret` wrap). The author of #599 anticipated this
   could be a problem (see comment at lines 47191-47196: "Issue
   #599 — recompute the branch-aware skip-set so the TCO-site
   dropmask aligns with pcs_pass's wrap-skip decisions") and
   wrote the call to look align-correct on the surface. What
   they missed was that the **shape** of `inner` post-perceus
   differs from `body` pre-perceus by exactly the `__perceus_dup`
   wraps that pcs_pass itself just inserted — and the linearity
   predicate had no awareness of those wraps.

2. **The bug manifested at exactly depth 3 of TCO recursion.**
   For lists of length 1 / 2 / 3, the first arm or second arm
   fires and the third-arm TCO path doesn't execute. From length
   4 onward the path executes >= 1 time, and the second TCO
   iteration is where the freed ref gets reused. Depth 3 is the
   minimum because the first iteration's drop is on a ref that
   `kai_apply` already consumed; the second iteration sees stale
   memory in `kai_initial`; the third iteration tries to read a
   field on that stale memory and trips
   `kai: field access on non-record`.

3. **Two reviewers agreed on direction, disagreed on placement.**
   Linus: self-call fast path inline in
   `rewrite_callsites_kind_sm` (~6 LOC). Asu: dedicated walker
   at the end of `emit_spec` (~40 LOC). Both viable; asu's
   placement keeps the existing rewriter path unchanged. Both
   surface comment at line 47137-47139 (which claims
   `tcrec_rewrite_decls` is "not wired into the pipeline yet")
   is stale and should be cleaned up.

## Fixtures added

- `examples/tco/issue_680_polymorphic_fold.kai` — canonical
  shape: `fold_events[s, ev, e]` with closure-typed `f` param,
  exercised at depths 1, 2, 3, 4, 8, 16, 64.
- `examples/tco/issue_680_polymorphic_fold.out.expected` — the
  N values for each depth.
- New `test-tco-issue-680` target in `stage2/Makefile`, added
  to `.PHONY`, `test`, and `test-fast` lists.

## Cost vs estimate

~1 session of focused work. Investigation took ~3 hours (bisect,
two failed hypothesis paths, finally tracing the post-perceus
predicate divergence with a `dropmask=N` C-comment annotation
emitted from the goto block). Implementation + tests + this
retro: ~1 hour. The bisect was the cheap part; understanding
why `pcs_branch_aware_skip_params` returned different sets pre-
and post-perceus was the expensive part.

## Follow-ups

1. **Stale comment at `stage2/compiler.kai:47137-47139`** says
   "no caller wires tcrec_rewrite_decls into the pipeline yet";
   line 66708 has been doing exactly that for some time. Worth a
   one-line cleanup in the next pass through that area.

2. **`pcs_branch_aware_skip_params` could be made stable across
   perceus**. Right now it relies on the linearity predicate
   looking through `__perceus_dup` to give the same answer pre-
   and post-. A stricter fix would compute the skip_set once
   (pre-perceus) and thread it through to `tcrec_rewrite_decl`,
   eliminating the recompute. That's larger surgery; left as
   follow-up.

3. **Audit other linearity predicates** for similar pre/post-
   perceus assumptions. The walker visits ECall, EField, EIndex,
   ERecordLit, EList — any of these might need the same
   `pcs_strip_dup_wrap` treatment if there's a use-case that
   trips them. Not blocking #680; worth a sweep next time
   perceus changes.

4. **Selfhost byte-identical**: the mono now generates its own
   TCO goto loops where before it delegated. The kaic2b binary
   bytes shift accordingly. Confirmed: `kaic2b.c == kaic2c.c`
   (per-compiler determinism) still holds. The pre-fix vs
   post-fix bytes of the compiler binary differ; that's the
   correction, not a regression.
