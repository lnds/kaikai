# Lane experience — issue #1143: fusion recognizer gaps (inline lambdas + ring terminals)

## Scope as planned vs as shipped

**Planned** (issue): (1) the range/terminal fusion path fuses inline-lambda
stages/combines as fully as named-function ones; (2) `sum`/`product`/`length`
recognized as fold terminals so `xs | stage |> sum` materialises nothing.

**Shipped**: both, plus a latent typer bug fixed en route (below).

## The bisection's hypothesis was wrong — the numbers were right

The issue hypothesised the recognizer "matches `EVar`-headed stages and
misses `ELam` nodes". Verifying before touching showed the recognizer and
gate are spelling-agnostic and terminal fusion **did fire** for lambdas —
`KAI_TRACE_RC` tags showed the 1M allocs were **closures**, not list cells.
The composed stage `x => (k => k*2+1)(x)` kept the raw `ELambda` node
inside the composed closure's body, so every per-element call re-evaluated
it: one fresh closure per element.

Fix, in `pipe_fusion_fold_rewrite`:

- a **single stage** is passed directly as the fold's stage argument
  (evaluated once; no composing wrapper at all — this also drops the
  eta-wrapper the named path used to build);
- a **multi-stage chain** hoists each `ELambda` stage into a `let` bind
  (`EBlock` + `SLet`, fresh `__fusev_*` names) and composes variable
  references, so the per-element path only calls through variables.

Hygiene note: binding-and-referencing is capture-safe by construction
(binders stay with their bodies). Beta-reducing stage bodies into the
wrapper was considered and rejected: substituting an expression with free
variables under a stage's binders can capture (classic hygiene), and the
per-element call count is the same as the bind-and-reference shape for the
dominant single-stage case.

## Measured (C backend, `KAI_TRACE_RC`, N=1M; wall warm @10M)

| shape `[1..n] \| stage \|> terminal` | alloc before | alloc after | wall before | wall after |
|---|---|---|---|---|
| named fns `\|> foldl(0, comb)` | 6 | 3 | 0.08 s | 0.08 s |
| lambdas `(k => k*2+1) \|> foldl(0, (a,b) => a+b)` | 1,000,003 | 3 | 0.19 s | 0.13 s |
| lambdas, two stages `\|> foldl` | ~2,000,000 | 5 | — | — |
| `(k => k*2+1) \|> list.sum` | ~1,000,000 (list kept) | 2 | — | 0.06 s |

The remaining 0.13 vs 0.08 wall gap is NOT fusion residue: a hand-written
`range_map_foldl(1, n, (k => ...), 0, (a,b) => a+b)` times identically
(0.13 s). It is the general cost of calling a locally-constructed closure
vs a top-level fn on the native backend (no static-closure emission /
devirtualization for capture-free lambdas) — it affects every lambda call
site in the language; repro and numbers recorded in the closing PR, out
of this lane.

## Ring terminals: why new stdlib fns instead of a foldl rewrite

`sum` cannot rewrite to `foldl(0, +)`: the seed literal `0` would pin the
element type and break (or silently mis-type) non-Int `Numeric` rings.
The correct seed is the ring's `zero()` — so the rewrite targets new
stdlib combinators under a `[b : Numeric]` bound (`list.map_sum` /
`map_product`; `protocols.range_map_sum` / `_product` + `range_step_*`;
`range_length` / `range_step_length`), mirroring how `list.sum` itself is
built (`sum_loop` + `zero()` seed). Genericity is preserved: a `Real`
(or any `Numeric` impl) chain fuses and keeps exact `sum` semantics.

**Trap found**: the obvious one-liner `range_map_sum = range_map_foldl(...,
zero(), add)` traps at runtime — an effect-row variable on a closure
parameter (`stage: (Int) -> b / e`) defeats monomorph's grounding of `b`,
so the arity-0 `zero()` falls to runtime dispatch and panics ("caller must
annotate Self"). Bisected: named-vs-lambda irrelevant, single-vs-multi
instantiation irrelevant; the row tvar alone breaks it. The 6-line repro
is recorded in the closing PR. The shipped combinators therefore take
**pure** stage params (no row) — which costs nothing because the fusion
gate for implicit terminals requires strictly pure stages anyway:

- explicit `foldl` keeps the Mutable-masked gate (stage still runs, only
  reordered);
- `sum`/`product`/`length` demand strictly empty labels — their targets
  take row-less stages, and `length` **drops** the stages entirely
  (mapped length = source length; dropping an unmasked-`Mutable` stage
  would skip observable writes, hence strict).

Recognized spellings: bare `sum`/`product`/`length` (matching the
existing bare-`foldl` precedent and its shadowing caveat) and
`list.`-qualified. Other-module qualified names do not match.

## The latent bug: isolation synths must thread the state

Multi-chain programs mixing element types (`... |> sum` on Int then on
Real in one fn) mis-typed: the second chain's scheme instantiation came
out pinned to the first chain's types. Root cause, pre-existing on main
(#1134's `fuse_raw_pure`): the purity gate synthesises stages in
isolation and **discarded the returned state**. The fresh-tvar counter
lives functionally in `InferState` (`st.env.fresh`) while the
substitution is mutated in place — rewinding the counter re-mints ids the
isolation pass already bound, and types silently cross between chains.
The #1134 retro's "fresh tvars are inert" claim holds only if the counter
advances. Named-stage fixtures never tripped it because non-generic
`EVar` stages mint no tvars.

Fix: the gate now threads `st` (`FuseGate`/`FuseAttempt` records) into
both the fused synth **and the decline path** (`synth_pipe_plain`
continues from the threaded state, since the isolation mints already
happened). This is load-bearing for any future pass that synthesises
speculatively: *discarding an `InferState` after a synth against the
shared substitution is never safe.*

## Fixtures added

- `examples/perceus/pipe_fusion_lambda_1143` — single- and multi-lambda
  chains into `foldl`; output golden + `alloc_total < 100` (regression
  would be ~1M). C target in `TEST_LIGHT_TARGETS`; native in
  `tier1-native.yml`, both with the alloc gate.
- `examples/perceus/pipe_fusion_ring_1143` — `sum`/`product`/`length`
  over range / step-range / list sources, bare + qualified, a `Real`
  ring row (generic `zero()`/`add` preserved), and an effectful stage
  that declines (golden pins its per-element `e` lines). Same gates.
- `pipe_fusion_ordering_1134` (the effectful-decline soundness net)
  passes byte-identical, untouched.

## Cost vs estimate

Most of the lane was verification, in both directions: disproving the
issue's `EVar`/`ELam` hypothesis (tag counters), disproving the zero()
one-liner (runtime trap), and bisecting the multi-chain mis-typing to the
discarded counter. The code diff itself is small; the two probes that
paid for themselves were `KAI_TRACE_RC` tag breakdown (closures, not
cells) and the hand-written-equivalent timing (proved the residual wall
gap is not fusion's).

## Follow-ups left for next lanes

- Monomorph: row tvar on a closure param defeats grounding of a
  protocol-bounded tparam (found-issue with repro; blocks expressing the
  ring combinators as `range_map_foldl` one-liners).
- Native backend: capture-free lambdas cost ~60% over top-level fns per
  call (no static-closure emission); closing it would make the lambda
  row's wall equal the named row's 0.08 s.
- Multi-stage lambda chains make one indirect call per stage per element;
  a hygienic beta-reducer (or emitter-level closure inlining) could
  collapse the composed wrapper to one call. Single-stage (the idiomatic
  case) is already optimal.
- `filter`-pipe integration into the fused loop (inherited from #1134's
  list).
