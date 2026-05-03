# Lane experience report — issue-187-phase5-docs-demo

Best-effort retrospective by the implementing agent. See limitations
at the bottom.

## Objective metrics (from /tmp/lane-issue-187-phase5-docs-demo-builds.tsv)

- Start: 2026-05-03T18:25:33-04:00
- End:   2026-05-03T18:37:33-04:00
- Wall-clock: ~12 minutes
- Build/test invocations:
  - `make tier0`:         1 invocation, 1 pass, 0 fails
  - `make tier1`:         1 invocation, 1 pass, 0 fails
  - `make selfhost`:      1 invocation, 1 pass, 0 fails
  - `make selfhost-llvm`: 1 invocation, 1 pass, 0 fails
  - `make -C stage2 test-unions`: 1 invocation, 1 pass, 0 fails
    (11/11 fixtures including new `ddd_ledger_demo`)

`make tier1-asan` was not run — the lane is doc-only plus one demo
fixture with no codegen changes. Tier1 + selfhost on both backends
covers the relevant correctness signal.

## Compiler errors I encountered

No compiler errors visible in current context. The DDD demo
fixture compiled cleanly on the first `kaic2` invocation; the
golden file matched on the first run. Selfhost was byte-identical
without any iteration.

## Friction points

- **Reconciling the design doc with what shipped.** The original
  `docs/unions-design.md` is a #184-era artifact (additive `union`
  keyword, separate `union` declarations). Issue #187 superseded
  that with the unified `|`-always-means-union model. Writing
  `docs/unions.md` required cross-referencing the issue body, the
  four phase PR descriptions, and the per-phase retros to identify
  the *actually-shipped* surface (e.g., D3 does not chain; the
  wrapper-variant Option Y was deferred). The design doc is now
  marked "implemented" with a historical-note preface, but the
  body still describes the rejected `union` keyword approach. A
  more invasive rewrite was scoped out — preserving the rejected
  design as the document of record is more honest than rewriting
  history.

- **Issue #187 was already CLOSED at PR #189 (audit).** GitHub's
  `closedByPullRequestsReferences` shows the audit PR as the
  closer. This is benign for the milestone — Phases 1–5 all
  reference #187 from each PR body — but means this PR's
  `Closes #187` is idempotent. The milestone is logically
  complete with this PR regardless.

- **DDD demo design.** Settling on three bounded contexts with
  realistic-but-runnable error shapes took one pass — no
  recompiles, no golden diffs. The constraint that drove the
  design was kaikai's stage-1 grammar: nested `if`/`else` rather
  than `else if` ladders. Each context's stub function is a
  three-level nested `if` so the grammar stays uniform.

## Spec ambiguities or interpretive choices

- **Where does "Pattern C" sit relative to Pattern A?** The lane
  brief said Pattern C is canonical for new code and Pattern A
  remains for legacy. I added a comparison table indexed by
  *situation*, not by *code age* — Pattern A still wins when the
  wrapper carries extra structure (request id, retry count) or
  when a single-tag runtime layout is required for FFI /
  serialisation. The "default to Pattern C for new code, migrate
  Pattern A only when the wrapper is pure type identity" advice
  follows from the table; users do not need to read it as a
  blanket Pattern A → Pattern C migration mandate.

- **Status of `docs/unions-design.md`.** Two options were
  considered: (1) edit in place to describe the unified model,
  (2) preserve the rejected design as historical record with a
  status update. Chose (2) because the rejected design's risk
  register and the four design alternatives (A/B/C/D for
  `union`-keyword semantics) are still informative — they show
  the path the project did not take and why. Rewriting them out
  would lose that.

- **Cross-reference in `docs/design.md`.** The lane brief asked
  for a "minimal in-place edit". I added one sentence at line 90
  in the "Few visible concepts" Tier-2 principle, where the
  existing prose already lists "sum types" among the core ten
  concepts. The cross-reference reads naturally there without
  restructuring the surrounding paragraph.

- **Protocol interaction note placement.** Added a new subsection
  (`Interaction with union types (issue #187)`) inside the
  existing `What this is not` section in `docs/protocols.md`.
  This sits next to the Haskell-typeclass comparison table, which
  is the right neighbourhood for "things kaikai protocols
  deliberately do not do".

## Subjective summary

- Confidence in correctness: **high**. The lane is doc-only plus
  one demo fixture. The fixture compiles, runs, and matches its
  golden. Selfhost is byte-identical on both backends with no
  iteration. The doc edits are local and reviewable inline; none
  of them changes compiler behaviour.

- Hardest sub-task: writing the milestone rollup retro
  (`docs/lane-experience-issue-187-milestone.md`). It required
  reading three per-phase retros + the audit + four PR bodies and
  synthesising a coherent narrative without simply concatenating
  them. The framing decision — "what the audit predicted vs what
  happened" + "surprises" + "recommendations" — emerged late.

- Easiest sub-task: the DDD demo fixture. The error/handler
  pattern is mechanical once Pattern C is internalised; the
  golden file is a list of strings the demo prints in order. No
  type-inference surprises, no runtime quirks.

- Did the compiler help or hinder you? **Helped.** kaic2 accepted
  the demo on the first try. The narrowing match arms typecheck
  without any annotation beyond the standard
  `bind : ComponentType` syntax. The single nuance — `else if`
  not being part of stage 1's grammar — is well-known and
  unrelated to this lane.

- Was Phase 4's PR body a clean handoff? **Yes.** The "Out of
  scope (deferred)" list named exactly the four items that
  Phase 5 had to either document (Option Y wrappers, resolver
  cleanup, generic unions, D3-aware diagnostics) or leave to
  future lanes. Each item appears in `docs/unions.md` *Out of
  scope (v1)* with the same phrasing.

## Limitations of this report

- Self-report bias acknowledged.
- Context truncation: counts and error lists exclude anything that
  fell out of my visible context window.
- Single agent (Claude). Not generalisable across LLMs.

## Raw build log

```
timestamp	cmd	outcome	elapsed_s
2026-05-03T18:30:29-04:00	test-unions	OK	-
2026-05-03T18:32:30-04:00	tier0	OK	-
2026-05-03T18:36:32-04:00	tier1	OK	-
2026-05-03T18:36:56-04:00	selfhost	OK	-
2026-05-03T18:37:27-04:00	selfhost-llvm	OK	-
```
