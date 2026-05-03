# Milestone retrospective — issue #187 (unified algebraic types)

Aggregate retrospective across the five lanes that landed the
unified-algebraic-types redesign. Best-effort summary by the
Phase 5 implementing agent, drawing on the four prior phase retros
plus the audit and PR bodies. Limitations at the bottom.

## The phases

| Phase | PR | Subject | Retro |
|---|---|---|---|
| 0 | #189 | Phase 0 audit (sum-type inventory + collision survey) | (audit doc, no retro) |
| 1 | #190 | Typer foundation: `TyUnion` + `normalize_union` | (no retro — predates instrumentation contract) |
| 2 | #191 | Resolver: `TyUnion` construction + D2 collision | `docs/lane-experience-issue-187-phase2-resolver.md` |
| 3 | #192 | Pattern matching: `bind : Type` + exhaustiveness | `docs/lane-experience-issue-187-phase3-patmatch.md` |
| 4 | #193 | D3 implicit upcast in `unify_heads` + narrowing codegen | `docs/lane-experience-issue-187-phase4-codegen.md` |
| 5 | (this PR) | Documentation + DDD demo + milestone closure | `docs/lane-experience-issue-187-phase5-docs-demo.md` |

The Phase 0 audit (`docs/unions-phase0-audit.md`) closed issue #187
on its own — `gh issue view 187` records #189 as the closing PR —
because the audit's *zero-collision* finding was the precondition
that made the whole milestone tractable. Phases 1–5 landed against
the closed issue but referenced it from each PR body so the
milestone history is preserved.

## What the audit predicted vs what actually happened

The Phase 0 audit's key empirical claim was: **zero in-tree D2
collisions across `stdlib/`, `examples/`, `demos/`**. Phase 2
verified this exactly — `make tier1` and `make selfhost` were
byte-identical at landing with no migration step required. Every
existing `type T = A | B | ...` declaration parses and behaves
identically under the unified model.

The audit's secondary claim was that the typer surface area would
be **smaller** than the original #184 design. This held: Phase 1
added one new `Ty` constructor, ~210 lines of resolver in Phase 2,
~150 lines of typer + desugar in Phase 3, and ~30 lines of unifier
plumbing in Phase 4. Total compiler delta is well under the
estimated ~1140 lines from the original design doc, primarily
because the unified model required no new keyword and no parser
changes.

What the audit did **not** predict:

1. **Phase 2's `[UnionInfo]` would be dead code by Phase 3.** The
   resolver collected union info into a side structure but never
   threaded it into the typer. Phase 3 worked around this by
   reading the env's variant table directly (every component
   registered as a zero-arg ctor of the parent gives the same
   information). The dead `[UnionInfo]` is still present and is a
   candidate cleanup item for a future lane.

2. **The dual-representation runtime layout is good enough.** Phase
   4 was scoped to add Option Y wrapper variants (`__From_C`) for a
   single-tag union runtime; the actual landed code (Option X) ships
   no new runtime layout. The fan-out desugar from Phase 3 plus
   the D3 upcast in Phase 4's unifier covers every canonical
   user-visible flow without a new wrapper variant. The audit
   estimated ~150 lines of codegen; the net codegen change in
   Phase 4 was 0.

3. **`!` propagation needed a 1-line call-order fix, not new code.**
   `bang_finish_result` already used `st_unify`; the fix was
   reordering its two arguments so the asymmetric D3 arm fires.
   Phase 4 caught this only by re-reading the new code, not by a
   failing test.

## Cross-cutting observations from the per-phase retros

### Compiler-as-affordance, not obstacle

Across phases 2–4, the implementing agents reported the compiler
*helped* more than it hindered. Concrete instances:

- Phase 2: zero compile errors on the first `kaic2` rebuild after
  ~210 new lines.
- Phase 3: every walker site that needed a new `PatKind` arm
  surfaced as a "non-exhaustive match" panic during selfhost,
  giving a precise punch-list.
- Phase 4: the C-side stage 2 build flagged the `unify_list` arity
  mismatch in row-effect unification immediately.

The hindering cases were narrow: Phase 3's selfhost stack
non-exhaustive panic with no source location (uninformative the
first time the new exhaustiveness checker mis-flagged
`ExprKind::EVar`); Phase 4's stage-1 grammar rejecting multi-line
`if`-conditions (mildly annoying, well-flagged).

### Spec ↔ implementation drift

Each phase had at least one spec ambiguity that needed an in-line
call:

- Phase 2 — *Option A vs B (dual representation vs single)*: chose
  A per the lane brief's hint, validated by the additivity of the
  resulting change.
- Phase 3 — *exhaustiveness gating*: naively recursing per-component
  mis-fired on regular sum types whose ctor names overlap with
  another type. Switched to "recurse only when at least one arm is
  a `PNarrow`".
- Phase 4 — *Option X vs Y for narrowing codegen*: chose X
  (structural, no resolver reshape), shrinking scope from the
  brief's predicted ~150 lines to ~30 lines plus fixtures.

The retros consistently note that the lane brief's STOP-and-merge
clause was a useful pressure-relief valve — Phase 3 invoked it
explicitly, deferring the construction-side D3 upcast to Phase 4,
and Phase 4 validated that the deferral was the right call (the
canonical fixture works end-to-end with the subset of changes that
fit cleanly).

### Instrumentation gap (Phase 1)

Phase 1 has no retro because it landed before the lane-experience
instrumentation contract was reintroduced. This is a small but
real loss for the LLM-baseline dataset — Phase 1 was the
foundation lane (typer changes, ~250 lines of walker arms) and
its friction profile would have been informative.

**Recommendation for future multi-phase milestones**: enforce the
instrumentation contract on **every** phase, including the first.
The marginal cost is one short retro doc; the benefit is a
complete dataset for that milestone.

## Surprises during the milestone

1. **Phase 3's STOP-and-merge clause activated mid-lane.** Phase 3
   discovered that the canonical narrowing form (`ie : IdentityError
   -> handle_id(ie)`) requires Decision-3's call-site upcast, which
   needs env threading inside `unify_heads` — a non-trivial change
   that the brief had earmarked for Phase 4. Phase 3 invoked the
   STOP clause, landed the parser/typer/exhaustiveness work with
   fixtures that exercise narrowing through dual-rep ctors only,
   and explicitly handed off the construction-side work to Phase
   4. The handoff was clean enough that Phase 4 closed the gap in
   ~30 lines.

2. **Issue #187 closed at Phase 0.** GitHub records #189 as the
   closing PR. This is a quirk of the issue tracker (the audit
   referenced #187 with `Closes` in its body), not a change in
   scope. Phases 1–5 all landed against the closed issue with
   `References` instead of `Closes`. The milestone is logically
   complete with this PR.

3. **The dual-representation runtime layout subsumed Option Y.**
   The original plan was to introduce single-tag wrapper variants
   (`__From_C`) at Phase 4 to make narrowing codegen trivial.
   Instead, Phase 3's `expand_narrow_arms` desugar fans `PNarrow`
   into one `PAs + PVariant` arm per inner variant **plus** one
   for the parent's dual-rep ctor. This covers both runtime
   construction paths without a new wrapper, and the canonical
   end-to-end fixture (`narrow_canonical.kai`) compiles and runs
   under Option X alone.

4. **Phase 5 (this lane) is small.** The original design doc
   estimated ~400 lines of doc + stdlib + examples. The shipped
   Phase 5 is one new user-facing doc (~250 lines), four
   in-place doc edits, one demo fixture (~110 lines), this rollup
   retro, and one Phase-5 instrumentation retro. Stdlib was
   intentionally not migrated — existing declarations are valid
   unions under the unified model, so there is no migration to do.

## Recommendations for future multi-phase milestones

1. **Run instrumentation from Phase 1.** No exceptions. The Phase 1
   gap in this milestone left a hole in the LLM-baseline dataset
   that cannot be retroactively closed.

2. **Treat STOP-and-merge as a first-class option for layered
   phases.** Phase 3 demonstrated that mid-lane scope reduction is
   not failure; it is the integrator's relief valve when a phase
   discovers that its canonical case requires the *next* phase's
   work. The phase-3 retro's note "the lane brief's STOP clause was
   consulted" is the right pattern.

3. **Predict where the unifier will need env access.** Phase 4's
   primary change was threading `env` through `unify` /
   `unify_list` / `unify_heads`. This was not in any earlier
   phase's brief, even though Decision 3 in the design doc made it
   inevitable. Adding "env-threaded unifier" as an explicit Phase 1
   item would have shifted the work to where it belonged
   architecturally.

4. **Honour the audit's predictions, including the negative ones.**
   The Phase 0 audit said "zero in-tree collisions" and
   "stdlib migration is syntactically zero". Both held. Trusting
   the audit empirically saved roughly a phase of migration work
   that the original design budgeted.

5. **The `[UnionInfo]` ghost is a warning shape.** Phase 2 added a
   data structure that Phase 3 found unnecessary. This pattern —
   a collector landing without a consumer — will keep happening if
   phases are written in isolation. **Test the consumer** by
   sketching the next phase's call sites before declaring a phase
   complete.

## Selfhost contract

Every phase landed with selfhost byte-identical on both backends:

- `make selfhost` (C backend) — byte-identical fixed point.
- `make selfhost-llvm` (LLVM backend) — byte-identical fixed point.
- `make tier1` and `make tier1-asan` — green.

This is the strongest correctness signal the project has. The fact
that no phase regressed it — including phases that touched the
typer (1, 3, 4), the resolver (2), and the unifier (4) — is the
empirical evidence that the unified model genuinely is
backwards-compatible at the source level.

## Limitations of this report

- Self-report bias acknowledged. The Phase 5 agent compiled this
  rollup from the four prior retros plus PR bodies; each
  individual retro is itself a self-report.
- Context truncation: per-phase counts and error lists exclude
  anything that fell out of the implementing agent's visible
  context window.
- Phase 1 has no retro to draw from. Observations about Phase 1
  here are inferred from PR #190's body, the audit, and Phase 2's
  retro (which references the Phase 1 representation).
- Single agent (Claude) across all phases. Not generalisable
  across LLMs.
