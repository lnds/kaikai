# Lane experience report — issue #201 (`||` flat-map-pipe + naming-convention dispatch)

This document captures empirical observations from the lane that
implemented the `||` flat-map-pipe operator and migrated `|` /
`||` onto a head-type-to-module dispatch mechanism, per
`docs/proposed-extensions.md` §11 (post-revision form,
2026-05-03 — HKT-based form rejected).

## Objective metrics

- **Start**: 2026-05-03T19:43:22-04:00
- **End**: 2026-05-03T20:26:51-04:00
- **Wall-clock**: ≈ 43 minutes 29 seconds.
- **Build / test invocations** (see TSV at end):
  - tier0 — 1 successful run (41 s).
  - tier1 — 2 runs (1 broken by stale unify-message label, fixed in
    1 line; second run OK in 240 s).
  - tier1-asan — 1 successful run (47 s).
  - selfhost-llvm — 1 successful run (27 s; reached LLVM byte-identical
    fixed point).

## Compiler errors I encountered

### 1. Mismatched briefing assumption (largest single source of friction)

The briefing and `docs/proposed-extensions.md` §11 both spoke as if
an `EMapPipe` AST node already existed and was being *migrated*
("EMapPipe AST node + its codegen (today hardcoded)"). It does not
— a `grep -n EMapPipe stage*/` is empty pre-lane. The actual
pre-lane shape is parser-level desugar of `xs | f` into
`ECall(EVar("map"), [xs, f])`, with the polymorphic `map` resolved
through a hardcoded prelude `TyEntry` and a runtime
`kai_prelude_map`.

Effect on the lane: I raised the misalignment to the integrator
*before* writing code, then committed to the larger scope (option A
in the upstream chat) — introduce both `EMapPipe` and
`EFlatMapPipe` AST constructors, propagate them through every
walker that pattern-matches on `ExprKind`, and then wire dispatch
in the typer.

Where: `docs/proposed-extensions.md` §11 (text claim) vs.
`stage2/compiler.kai:1182-1267` (actual `ExprKind`).

Fix: introduced the two new constructors at
`stage2/compiler.kai:1212-1224` and added arms at every walker
site that pattern-matches `EPipe` (≈35 sites, mostly
recurse-on-children); the typer is the single site where the new
constructors are *consumed* into a `Call`-shaped synthesised AST.

Attempts: 1 — once the new design was clear, the propagation was
mechanical. The `grep -n EPipe` baseline gave the exact site list.

### 2. Unify-error label drift (1 fixture broke)

My initial dispatch passed `"map-pipe call"` / `"flat_map-pipe
call"` to `st_unify`. The existing fixture
`examples/sugars/map_pipe_type_mismatch.err.expected` pins
`type mismatch in function call`. Diagnostic strings are part of
the surface; the lane should not change unrelated wording.

Where: `stage2/compiler.kai:24244` (in `synth_pipe_dispatch`).

Fix: replaced both labels with `"function call"` so the existing
golden still matches identically. Comment at the call site
explains why.

Attempts: 1 — caught on the first tier1 run and reverted in the
same lane, no follow-up needed.

### 3. Codegen path for `EModCall("list", op)` (initial misroute)

First cut had the dispatch produce
`ECall(EModCall("list", op), [lhs, f])`. The C-emit then minted
`kai_list__map` / `kai_list__flat_map` symbols, which only exist
when the file imports `core.list`. Smoke test failed to link.

Where: `stage2/compiler.kai:24229-24245` (the dispatch lowering).

Fix: in v1, route the dispatch through `EVar(op)` rather than
`EModCall("list", op)`. The bare-name `op` resolves through the
prelude env entries (`map` was already there pre-lane; I added
`flat_map` to mirror it) and ends up at `kai_prelude_map` /
`kai_prelude_flat_map` — same runtime as pre-lane for `|`,
new runtime function for `||`. The comment in the dispatch site
records that this is a v1 collapse: m6.2 v2 will lift to module-
prefixed env keys and the same site will switch to the
`EModCall` form so user-defined modules participate via the
qualified codegen path.

Attempts: 1 — first end-to-end smoke test caught the link error
immediately.

## Friction points

### Was the head-type-to-module map easy to integrate?

**Yes — for v1.** With only one entry (`List → list`) the map is a
pure function `head_module_for(name: String) : Option[String]`
with a single `if/else if` branch. No data-structure work, no
registration phase, no resolver pass.

The complexity defers to the day a second eligible head type
shows up. The design doc is explicit that the map is one-to-one
and ambiguity is rejected at registration time, but neither
constraint exercises until a real second entry exists.
**Constraint #4 ("one module per head type") is therefore an
architectural commitment in v1, not a tested invariant.** I noted
this at the top of `head_module_for`.

### Did `Option`/`Result` rejection require special handling?

**Yes — by design, but cleanly.** Constraint #1 was the easiest
constraint to implement: a separate `pipe_rejection_reason`
function maps the head name to a typed diagnostic (or `None` for
heads that are not specifically rejected). It runs *before*
`head_module_for`, so a user typing `Some(5) | f` gets the
"use `!` or `opt_and_then`" error rather than the generic "no
module registered" error. Both errors stamp `TyAny` and bump
`st.errs` for graceful cascade.

The two rejection paths are tested by
`examples/sequence/pipe_option_rejected.kai` and
`examples/sequence/pipe_result_rejected.kai`.

### What did NOT need special handling

- The 35 walker sites that pattern-match `EPipe` (free-vars,
  perceus, unbox, brand-check, nursery rewrite, etc.). All recur
  on children; the new constructors are children-only at all
  sites except parse and the typer.
- Codegen. Both `EMapPipe` and `EFlatMapPipe` are rewritten by
  the typer into ordinary `ECall`s before either backend (C or
  LLVM) sees them. The defensive panic stubs at the C and LLVM
  emit sites are unreachable in practice, kept only so the
  exhaustive matches stay valid.

## Spec ambiguities or interpretive choices

### Did I reuse the variant table from #187 phase 2?

**No** — `#187`'s variant table tracks sum-type constructors; the
head-type-to-module map tracks *which module owns the
`map`/`flat_map` for a given head type*. They do not overlap.
The variant table also has no notion of "module of declaration"
in the form the dispatch needs, and the map is one-to-one while
the variant table is many-to-one. I noted this in the comment
above `head_module_for` so a future maintainer doesn't re-litigate
the choice.

### How did I handle the canonical signature check?

**By env construction, not by an explicit walker.** The
`flat_map` and `map` entries in the baseline `TyEntry` block
encode the canonical signature directly:

- `map: ([a], (a) -> b / e) -> [b] / e`
- `flat_map: ([a], (a) -> [b] / e) -> [b] / e`

The dispatch builds an `expected = TyFnT([rl.ty, rf.ty], rv.ty,
expected_row)` and unifies against the env entry's type. Any
deviation (off-shape lambda type, wrong arity, wrong return
shape) surfaces as the standard `type mismatch in function call`
diagnostic with the canonical signature on the "expected" line.

Constraint #3 ("canonical signature enforced") is therefore
**enforced by unification**, not by a hand-written shape check.
The trade-off is that a user-written off-shape `pub fn map` in a
hypothetical `Stream` module would also fail at the call site
(generic unification error) rather than a dedicated "off-shape
for pipe dispatch" message. I think that's the right call for v1
— the unify diagnostic is already specific enough — but a future
lane could add the dedicated message if it pays off in real LLM
authoring data.

### Constraint #2 (half-impl rejection)

A type is dispatch-eligible iff `head_module_for(name)` returns
`Some(...)`. In v1 only `List` is registered, and the prelude
provides BOTH `map` and `flat_map`, so a half-impl can't be
constructed. The rejection PATH (the diagnostic + `TyAny` stamp)
is implemented and triggered for any non-`List` non-`Option`
non-`Result` head, but the specific shape of "module exports
`map` only, missing `flat_map`" can't be tested until a second
head type exists. I left the diagnostic generic enough to cover
both scenarios.

### Fixture coverage

The briefing called out 6 named fixtures plus two more
(`pipe_half_impl_rejected`, `pipe_signature_off_shape`). I
shipped 6 — the half-impl and off-shape fixtures are not testable
in v1 without a stub second head type, which would itself be a
non-trivial design choice. I noted this in the lane experience
above so the integrator can decide whether to add a stub-head
mechanism in a follow-up issue.

## Subjective summary

- **Confidence**: high on the surface of the operator (lex / parse
  / fixture round-trips), high on selfhost byte-identical, medium
  on the architectural depth of the dispatch (Constraint #2/#4
  not exercised by any v1 fixture, only verified by code
  reading).
- **Hardest**: discovering and reporting the briefing-vs-code
  misalignment. It's tempting to assume the doc and briefing are
  authoritative when really the code is.
- **Easiest**: the lex / parse / formatter changes were boilerplate
  ports of the `|>` shape into a new token + AST shape.
  35 walker arms took longer to find than to write.
- **Did the compiler help or hinder?**
  - **Helped**: kaic2 produces precise unify-error diagnostics
    (with file:line:col, expected/found types). The fixture-driven
    tier1 gate caught the unify-message-label drift at the first
    rerun.
  - **Hindered**: the compiler does not warn when an `ExprKind`
    constructor is missing from a pattern-match — kaikai accepts
    non-exhaustive matches silently in many spots. I had to grep
    `EPipe` and add arms manually, with no compile-time check
    that I had covered every site. I worry I may have missed a
    walker that uses a `_ -> default` arm; selfhost byte-identical
    + tier1 green is the only safety net.

## Limitations of this report

- **Single-trial timing.** The TSV reports one run per gate, not
  a distribution. Re-runs would tighten error bars but the lane
  only had one full successful trial.
- **No LLM-authoring trial.** The "Tier 3 strategic bet" prompt
  in CLAUDE.md (LLMs author kaikai) is not exercised by this
  lane — it was written by Claude, but with a human-written
  briefing in the loop. A clean "LLM-only" trial would need a
  different scaffold.
- **Constraints #2 / #3 / #4 are architectural, not exercised by
  v1 fixtures.** When the second head type lands (likely
  `Stream` in ahu), a follow-up lane should add the half-impl
  and off-shape fixtures and verify the rejection diagnostics
  fire.
- **No benchmarking.** I asserted "runtime cost unchanged for
  `[T]`" by code-reading (same prelude entry, same C symbol)
  rather than measuring. A microbench against the pre-lane shape
  would be a small follow-up if the runtime delta matters.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-03T20:12:55-04:00	tier0	OK	41
2026-05-03T20:14:49-04:00	tier1	FAIL	106
2026-05-03T20:25:06-04:00	tier1	OK	240
2026-05-03T20:26:00-04:00	tier1-asan	OK	47
2026-05-03T20:26:42-04:00	selfhost-llvm	OK	27
```

The first tier1 FAIL was the unify-message-label drift (1 line
fix); second run is the post-fix gate that the lane lands on.
