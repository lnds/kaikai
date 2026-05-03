# Lane experience report — issue-187-phase2-resolver

Best-effort retrospective by the implementing agent. See limitations
at the bottom.

## Objective metrics (from /tmp/lane-issue-187-phase2-resolver-builds.tsv)

- Start: 2026-05-03T16:14:06-04:00
- End:   2026-05-03T16:28:52-04:00
- Wall-clock: ~14m46s
- Build/test invocations:
  - `kaic2` build:        1 invocation, 1 pass, 0 fails
  - `make selfhost`:      1 invocation, 1 pass, 0 fails
  - `make selfhost-llvm`: 1 invocation, 1 pass, 0 fails
  - `make tier0`:         1 invocation, 1 pass, 0 fails
  - `make tier1`:         1 invocation, 1 pass, 0 fails
  - `make tier1-asan`:    1 invocation, 1 pass, 0 fails

Plus three smoke-test runs of `make -C stage2 test-unions` while
authoring fixtures (not separately logged — each took <2s and all
three passed first time).

## Compiler errors I encountered

No compiler errors visible in current context. The lane compiled
cleanly on the first kaic2 build after introducing
~210 lines of new resolver code.

## Friction points

The biggest unknown going in was **where in the resolver pipeline to
hang the new logic**. kaikai's stage 2 has no separate "type-name
environment" — uppercase identifiers in type position fall through
`resolve_ty_with_tparams` into `TyCon(name, ...)` without lookup, so
there was no obvious slot to "register a type". The lane brief
nudged toward "option A" (dual representation: keep TyCon + variant
table, add TyUnion alongside) and that turned out to be exactly
right — once I committed to it, every component fell out cleanly:

- `collect_unions` is a pure walk over `[Decl]`, no environment
  threading.
- `validate_union_collisions_decls` is a pre-typer validator with
  the same shape as `validate_var_uses_decls` /
  `validate_contract_predicates_decls` (Int / Console return,
  threaded into the `compile_source` exit-code aggregator).
- `union_info_to_ty` is the bridge into phase 1's
  `normalize_union`. Phase 1 already handled the algebra (D6:
  associative / commutative / idempotent), so phase 2 only needed
  to feed it components.

The diagnostic-format work was the smallest part — `diag_error` +
`diag_help` already existed (and are identical to what the
`validate_var_uses_decls` family uses), so matching the brief's
expected wording was just a `concat_all([...])`.

## Spec ambiguities or interpretive choices

- **Option A vs B.** The brief asked the agent to pick. I picked A
  (dual representation) for two reasons: (1) the lane brief itself
  recommends A unless B is mechanically simpler, (2) Phase 1 already
  added `TyUnion` as an inert representation with no source-level
  populator, so phase 2's job is by construction *additive* — the
  existing variant-table machinery is what keeps every existing test
  green. Switching to B would have required rewriting
  `scheme_of_variant`, `add_variants_loop`, `variants_of_type`,
  `check_exhaustiveness`, and the `#derive(*)` walkers in the same
  PR — a much larger blast radius for no immediate user-visible win.
  Phase 3 / 4 will take steps toward B as match-by-type and codegen
  rework land, by which point the typer's union view will be
  consumed in earnest.

- **D2 fired on existing code unexpectedly?** No. The phase-0 audit
  predicted zero in-tree collisions and the audit was correct: full
  `make tier1` and `make selfhost` are both byte-identical and the
  D2 validator returns 0 errors across the entire stdlib + examples +
  demos tree. Selfhost-llvm also byte-identical.

- **D2 wording.** The brief gave verbatim wording. I matched it
  exactly modulo file paths (the brief used `foo.kai` as the
  placeholder). One small judgement call: the brief shows `(foo.kai:5:18)`
  appearing twice — once for the offending second decl in the main
  message, once attached to the `type B = ...` clause. I rendered it
  the same way (both locations included in the message). The fixture
  golden checks substrings, so the exact whitespace/punctuation isn't
  brittle, but the message reads naturally to a human.

- **`TyCon(component_name, [])` for every variant component.** The
  resolution-rule from `docs/unions-design.md` proposal § says
  payload variants `Foo(T1, T2)` should be auto-declared as
  *records with positional fields*. Phase 2 does NOT do that — it
  still types every component as `TyCon(cname, [])`, the same opaque
  handle that `resolve_ty_with_tparams` would produce for a
  hand-written `: Foo` annotation today. The reasoning: the existing
  variant-constructor entry registered by `add_variants_loop` is
  what makes payload-bearing variants type-check today, and rewriting
  that side would have crossed into Phase 4 (codegen) territory. The
  union view exposed by `union_info_to_ty` is what phase 3 will
  consume; the structural / record side waits for phase 4.

## Subjective summary

- Confidence in correctness: **high**, because (a) the lane is purely
  additive in option A — no existing code path's behaviour changed,
  proved by selfhost byte-identical on both backends; (b) the D2
  validator only fires on previously-untested patterns and the
  fixture coverage shows it firing exactly when expected; (c) the
  full tier1-asan suite passes, so no memory bug crept into the new
  code.
- Hardest sub-task: **navigating the resolver pipeline to find the
  right insertion slot**. Once located, the code was mechanical.
- Easiest sub-task: **wiring the Makefile fixtures**. `test-sugars`
  was an exact template; `test-unions` is a 35-line near-clone.
- Did the compiler help or hinder you? **Helped consistently** —
  zero compile errors on the first kaic2 build after writing 210
  lines of new code, and the existing diagnostic infrastructure
  (`diag_error` / `diag_help`) was a drop-in.
- Was the design doc (`docs/unions-design.md`) sufficient? **Mostly.**
  It is still mostly the #184 (`union` keyword) era doc; the actual
  spec for this lane comes from issue #187 which supersedes it. The
  lane brief and `docs/unions-phase0-audit.md` filled the gap. The
  audit's blast-radius estimate (~250 lines for phase 1, with phase 2
  smaller) held: I added ~210 lines including the validator and helpers.
- Was the Phase 0 audit (`docs/unions-phase0-audit.md`) useful?
  **Very.** Knowing in advance that "zero in-tree collisions" was
  empirically verified meant I could land D2 without a migration
  step — every existing fixture passes unmodified.

## Limitations of this report

- Self-report bias acknowledged.
- Context truncation: counts and error lists exclude anything that
  fell out of my visible context window.
- Single agent (Claude). Not generalisable across LLMs.

## Raw build log

```
timestamp	cmd	outcome	elapsed_s
2026-05-03T16:20:22-04:00	kaic2	OK	-
2026-05-03T16:22:38-04:00	selfhost	OK	-
2026-05-03T16:23:06-04:00	selfhost-llvm	OK	-
2026-05-03T16:23:43-04:00	tier0	OK	-
2026-05-03T16:27:45-04:00	tier1	OK	-
2026-05-03T16:28:38-04:00	tier1-asan	OK	-
```
