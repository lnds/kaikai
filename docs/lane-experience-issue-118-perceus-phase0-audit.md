# Lane experience report — issue-118-perceus-phase0-audit

Best-effort retrospective by the implementing agent. Doc-only
lane (Phase 0 audit for the Anga Roa wave). See limitations at
the bottom.

## Objective metrics

- Start: 2026-05-03T19:43:12-04:00
- End:   2026-05-03T19:48:52-04:00
- Wall-clock: ~5m 40s (single-shot agent run; reading + audit
  drafting only — no code, no test invocations).
- Build/test invocations: zero. Doc-only lane covered by
  `paths-ignore` for Tier 0 / Tier 1 / Tier 1-ASAN.

## Compiler errors I encountered

None. Doc-only lane — no kaikai source touched, no `make`
invocations.

## Friction points

1. **The `runtime-debt-2026-04-28.md` doc referenced in user
   memory does not exist on disk.** The memory entry says
   "Linus+Eric found Full lang feature-complete but RC
   fictional…" with a "roadmap reformulated to Phase R" but
   the doc itself is not in `docs/`. The auditable substitute
   was `docs/perceus-honesty-targets.md`, which has more recent
   numbers (post-Tier-2-perceus 2026-04-29) and the actually
   merged audit findings. Lane proceeded against that doc; no
   blocker, but the memory should be reconciled — either the
   doc was retired silently (and the memory should drop the
   reference) or the memory's "RC is fictional" framing is
   stale (post-flip + Tier-2 partial close brought leaked from
   127 M → 13.4 M, well past "fictional").

2. **Counting "constructor-after-deconsume" patterns
   programmatically is fuzzy.** The grep heuristic
   `\[[a-z_]+,\s*\.\.\.[a-z_]+\]\s*->\s*\[` catches the
   canonical cons-rebuild shape but misses sites where the
   rebuild lives behind a helper call (`tree_node(nk, nv, l,
   r)` inside `tree_balance` is a wrapper hiding the `TNode`
   constructor; the audit grouped these by hand). For variants
   the grep is even noisier — same-tag rebuild requires
   reading the body, not just the pattern. The §1.B variant
   inventory was assembled by reading `stdlib/collections/map.kai`
   end-to-end rather than by grep. Future Phase-0-style audits
   on this domain will need a small Bash + AST helper, not a
   single grep, if the candidate count exceeds ~50 sites per
   file.

3. **`Parser` / `Lexer` / `FmtSt` rebuild pattern was the most
   surprising finding.** The issue text frames reuse-in-place
   as "map / filter / fold over recursive structures" — code
   that walks lists. The bigger candidate set is actually the
   **compiler's own internal record state**, where every
   combinator returns `R { ... unchanged ..., one_field:
   new_value }`. The 472 record-rebuild sites in stage2 alone
   dwarf the 30 cons-spine sites. This was not in the issue
   text; the audit lifted it from grep counts and the §1.C
   table. If Phase 1 lands #118 covering only cons + variant,
   the audit's headline "−36 % alloc count" estimate would
   miss most of the win — the integrator should weigh whether
   record reuse-in-place is part of the v1 scope or a fast
   follow-up.

## Spec ambiguities or interpretive choices

1. **Definition of "known-unique"**: the issue text says
   "the type system can prove" but kaikai's typer does not
   carry uniqueness info — what proves uniqueness today is the
   perceus pass's last-use analysis (a flow analysis, not a
   type-system property). The audit reads "known-unique" as
   "rc == 1 at the consumed-cell read site, as proven by
   `pcs_collect_uses_*` + `last_use_for`". If the issue
   intended a stronger property (Koka's `&` borrow / `^`
   uniqueness annotations), the candidate inventory would be
   much smaller (essentially zero, since kaikai has no such
   annotations today). The audit took the more permissive
   reading because the existing analysis already gives sound
   uniqueness *at the use site* and the runtime guard
   (`if v->rc == 1`) handles the imperfect-static-info case
   gracefully.

2. **Coverage projection for §6's perf delta**. The "−36 %
   alloc count" headline assumes ~50 % of `variant + record +
   cons` allocs become reused. This is a judgment call; could
   be 30 % or 70 %. The drop-spec lane's measurement
   methodology suggests ±5 % wall variance per side, so
   distinguishing −5 % from −15 % wall is borderline; a
   confident "−25 %" wall claim would need n ≥ 30 isolated
   benches.

3. **Whether to recommend re-opening #119**. The drop-spec
   retro paired re-evaluation with #118. The audit found
   reuse-in-place shrinks rather than grows the alloc slice
   drop-spec targets, so the re-eval criterion is *not* met.
   But the retro mentions "TyCon → body-shape lookup" as a
   separate, larger win (~99 % alloc coverage). The audit
   pinned that as a Tier-3 long-tail item rather than
   re-opening #119, because the wall measurement on the v1
   drop-spec implementation was already marginal even at
   pre-#118 alloc volumes. A reasonable counter-position
   exists: re-open #119 with a TyCon-extension scope. The
   audit took the conservative read.

## Subjective summary

- **Confidence in correctness of audit data**: high for the
  cons-rebuild count (canonical grep, low false-positive
  rate). Medium for variant rebuild (manual read of
  `stdlib/collections/map.kai`). Medium for record rebuild
  (grep counts on `Parser {`, `FmtSt {` are precise but
  classifying which are *reuse-shaped* — same field set, one
  field changed — was eyeballed, not exhaustively verified).
- **Confidence in §6's wall prediction**: low. The −5 % to
  −15 % range is honest about uncertainty; the "−36 % alloc"
  is more confident because it reduces to arithmetic on
  measured per-tag counts.
- **Hardest sub-task**: §3 (regions). The issue text frames
  regions as a parser/lexer scratch optimisation, but the
  parser/lexer state turned out to be a better fit for #118
  (linear-chain rebuild) than for #120 (LIFO bump arena). The
  audit had to re-frame the candidate set toward the
  token-list-only case and explicitly defer #120 rather than
  proceed with a thin recommendation.
- **Easiest sub-task**: §4 (bootstrap chain). The post-flip +
  Tier-2 perceus state has clear documentation
  (`docs/perceus-honesty-targets.md`); the answer
  ("stage 0 needs runtime helpers, stage 1 unchanged, stage 2
  is the lane") fell out cleanly.
- **Did the docs help or hinder?** Helped overall.
  `docs/perceus-basic.md` + `docs/perceus-honesty-targets.md`
  + `docs/lane-experience-drop-specialisation.md` together
  carried 80 % of the audit's framework. Gaps: no current doc
  inventories the constructor-rebuild density across the
  codebase, which is exactly what this audit produces.

## LLM-friendly bet evidence

> At any point during this lane, would `--effects-json` or
> `--effect-holes-json` have helped you recover from a
> typing or effect-row error? Did you actually use them, or
> did the plain-text compiler output carry the work?

Neither. Doc-only lane — zero compiler invocations, zero
typing or effect-row errors.

The kaikai language tooling that *would* have helped this
specific audit shape: a `kai stats --rebuild-sites` or
`kai trace --constructor-density` query that emits a JSON
table per constructor of (decl site, deconsume site, rebuild
site, last-use proven). The audit produced this manually via
grep + read. A first-class query would make Phase 0 audits on
the perceus domain reproducible.

This is a Tier-3-bet observation: structured compiler queries
beyond holes / effects. Worth flagging because the bet's
acceptance criterion ("LLM completes 80 % of typical
functions in one round") covers authoring; Phase-0-style
audits are a different regime where structured output also
pays off.

## Limitations of this report

- Self-report bias acknowledged.
- Single agent (Claude Opus 4.7).
- Doc-only lane: no compiler measurements were taken to
  verify the §6 predictions. The "−36 %" alloc estimate is
  arithmetic on *projected* coverage of *measured* per-tag
  counts; the projection (~50 % of candidates fire under
  reuse-in-place) is not measured.
- Variant-rebuild inventory (§1.B) is incomplete — only
  `stdlib/collections/map.kai` was read end-to-end. Other
  files with same-tag-rebuild patterns (e.g., effect handler
  state machines in `examples/effects/`) likely add to the
  count but were not catalogued. The audit headline numbers
  treat §1.B as a lower bound.
- The §3 lexer-token-list cell estimate (~4 M cells/compile,
  ~10 % of `live_peak`) was extrapolated from the
  `docs/perceus-basic.md` per-tag breakdown, not measured
  directly with a `KAI_TRACE_RC` segmentation. Could be ±50 %.

## Raw build log

```
timestamp	cmd	outcome	elapsed_s
(empty — doc-only lane, paths-ignore covers Tier 0/1/1-ASAN)
```

(No `make tier0` / `make tier1` invocations — `paths-ignore`
covers `docs/**` doc-only diffs both locally and in CI.)
