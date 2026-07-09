# Lane experience — issue #1134: map-pipe fusion (deforestation)

## Scope as planned vs as shipped

**Planned** (issue + integrator addendum): fuse adjacent `map`-pipe stages
into one loop, gated on pure effect rows; fuse `map`-into-terminal-fold to
zero lists; treat a range literal at the head of a fused chain as a
generator (counting loop, never materialised).

**Shipped**: all three.

1. **Map-map fusion** — `xs | f | g` collapses to `map(xs, g∘f)`: one
   traversal, one result list. Lives in the typer's pipe-dispatch lowering
   (`try_fuse_map_pipe` in infer.kai), which is where the chain is still
   visible whole and each stage's row is resolved.
2. **Terminal-fold fusion** — `xs | stage |> foldl(init, combine)` rewrites
   to the stdlib `map_foldl`, folding `combine` over `stage(x)` in one pass:
   the mapped list disappears. A multi-map chain flattens all its stages
   first, so `xs | f | g |> foldl` fuses end to end.
3. **Range-head generator** — `[a..b] | stage |> foldl(...)` rewrites to
   `range_map_foldl` (or `range_step_map_foldl`), a counting loop that never
   builds the range spine.

Not shipped (out of v1 by design): `flat_map`/`filter`-pipe fusion, and
`sum`/`product`/`length` as recognised terminals. Only explicit `foldl` is
recognised as the terminal shape — `sum` desugars to its own `sum_loop`,
not `foldl`, so it does not match. A chain ending in `sum` still gets the
map-map fusion; only the last-list elimination is missed. Documented here
as the known gap, not silently dropped.

## Measured numbers (KAI_TRACE_RC, C backend)

| Shape (N=1M) | alloc_total before | after | note |
|---|---|---|---|
| `xs \| dbl \| inc \| dbl` | 7,000,011 | 4,000,009 | two intermediate lists gone |
| `xs \| dbl \| inc \|> foldl` | ~3,000,000 | 1,000,009 | only the source list remains |
| `[1..1M] \| dbl \|> foldl` | 3,000,008 | 9 | range never materialised |

Wall (warm, 5 runs, `xs | dbl | inc | dbl` + foldl over N=1M): fused
~0.03–0.04s vs ~0.05–0.06s unfused — the ~40% the issue predicted, since
the shape is memory-bound on exactly the intermediates fusion removes.

## Design decisions and alternatives considered

The load-bearing decision was **where the act of fusion lives** and **how
to build the fused node without corrupting the typer's substitution**.

- **Where.** `|`/`||`/`|?` are not builtins — they dispatch by convention
  to `map`/`flat_map`/`filter`. The parser keeps `EMapPipe` AST nodes that
  survive to the typer, which rewrites them to `ECall(map, …)` once the head
  type is known. So the chain is visible whole exactly at the pipe-dispatch
  lowering, with each stage's row resolved. A post-infer pass over already-
  rewritten `ECall(map, [ECall(map, …)])` was rejected: it loses the pipe
  shape and re-derives purity from a less obvious AST.

- **KIR loop-fusion rejected on evidence.** The initial architect steer was
  "gate in infer, fuse in KIR". Checking the stdlib killed it: `map` is
  `reverse(map_loop(…))`, a recursive function — in KIR it is a `KCall`, not
  an inline loop. Fusing two `KCall(map)` in KIR would need to inline `map`'s
  body first, a far larger transform. AST map-compose collapses two `KCall`
  into one directly and is the only proportionate v1 path.

- **The substitution is mutated in place** (documented "never fork a Subst
  between two trials"). This forbade the clean-looking "synthesise
  speculatively, revert if impure" approach. Two resolutions, both used:
  - Map-map: synthesise the LHS linearly (as the dispatch already does),
    then build the composed lambda **already-typed** — its `.ty` is a direct
    projection of resolved stage types — so no node is re-synthesised.
  - Terminal-fold: synthesise **only** each stage/combine closure in
    isolation to read its row for the gate (fresh tvars each time are inert
    in the shared sub — they are never re-referenced), then, if pure, rewrite
    to the raw stdlib call and synthesise it **once**. `synth_call`
    instantiates the generic scheme cleanly, so no type is built by hand.

- **The purity gate reads concrete row labels, not the tail.** Kaikai
  lambdas always carry an open row tail (Rémy-style polymorphism), so an
  open tail is not impurity — only realised labels are. The gate is:
  labels ⊆ {Mutable} → pure. A cell captured by reference escapes, so its
  mutation is not masked and surfaces as `State`, which the gate rejects —
  the effect system carries the soundness, exactly as intended.

## Structural surprises the brief did not anticipate

- **The tail is always open.** The architect's steer assumed a closed row
  for a pure stage; empirically (`--effects-json`) every lambda's row is
  `[labels] + ?e`. Reading the tail as the purity signal would have declined
  every fusion. The concrete labels are the truth.

- **kaic1 rejects a file-top `#[doc("""…""")]` in the bundle.** The compiler
  bundle is one concatenated file; a triple-string module doc at file top
  broke the lex. Downgraded to plain `#` comments for the compiler module
  (stdlib modules, compiled as user programs, keep their `#[doc]`).

- **Module dependency direction.** `pipe_fusion.kai` needs `Subst`/`apply_ty`
  from infer, but infer must import `pipe_fusion` to call it — a cycle in the
  modular (main.kai-with-imports) build, which the bundle's global namespace
  hid. Resolved by splitting: `pipe_fusion.kai` holds the AST-only half
  (pattern recognition, typed-node construction) and imports only `ast`; the
  typer keeps the gate and the projections that need the live sub. This is
  also why the bundle build passed while `make selfhost` (which uses the
  real modular path) first failed — the modular selfhost is the honest
  oracle, exactly the lesson #1127 recorded.

- **Multi-map + terminal ordering.** First cut composed only the last map
  stage into the fold, leaving inner maps materialised (3M allocs on the
  1M chain). Fixed by flattening the whole map-pipe chain into a stage list
  before composing, so every adjacent stage fuses (1M — only the source).

## Fixtures added

- `examples/perceus/pipe_fusion_purity_1134` — three pure maps fuse; cons
  budget gate (< 400, vs ~600 unfused). C + native.
- `examples/perceus/pipe_fusion_ordering_1134` — two effectful stages decline
  fusion; golden pins `d d d i i i` (non-interleaved). Soundness net.
- `examples/perceus/pipe_fusion_terminal_1134` — range-headed terminal fold;
  alloc_total < 100 (nothing materialised), 1M-wide in O(1) stack. C +
  native.

C targets in `TEST_LIGHT_TARGETS`; native targets in `tier1-native.yml`
(never in `TEST_LIGHT`, per the tier1-shard-is-C-only trap).

Coverage gap: no fixture for `sum`/`product`/`length` terminals (not
recognised in v1). A negative-shape fixture confirming they still compile
(via map-map fusion only) would document the boundary; deferred.

## New stdlib

- `list.map_foldl` — map-then-left-fold in one pass, no intermediate list.
- `protocols.range_map_foldl` / `range_step_map_foldl` — fold over a range
  as a counting loop, no materialisation.

All carry `#[doc]`. `map_foldl` is a list combinator (lives in `core/list`);
the range folds live beside `int_range_to_list` in `protocols`.

## TRMC / reuse / RC guards

Unchanged. The fused map is the same stdlib `map` over the same List spine,
so TRMC and reuse fire identically. `map_foldl` / `range_map_foldl` are
tail-recursive, so a 1M-wide fold runs in O(1) stack (verified on both
backends). The #817/#1117/#1120/#1126/#1127/#1130 fixtures pass unchanged.

## Follow-ups for next lanes

- Recognise `sum`/`product`/`length` as terminals (map to `foldl` with the
  ring's `combine`/`zero`/`one`, or a builtin count). Closes the last-list
  gap for the most common terminal shapes.
- `filter`-pipe integration into the fused loop (skip-instead-of-emit), so
  `xs |? p | f |> foldl` fuses. The stdlib generator would grow a predicate
  arm; the gate already covers the predicate's row.
- These numbers feed the cons-vs-flat list representation decision (the Roc
  question): intermediates now disappear regardless of representation, so
  the representation choice can be measured against the fused baseline.
