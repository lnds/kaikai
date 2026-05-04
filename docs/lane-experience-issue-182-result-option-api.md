# Lane experience report — issue-182-result-option-api

This report captures the empirical experience of an LLM agent
implementing issue #182 (expand `Result` and `Option` stdlib
APIs). It is part of the rolling baseline data on LLM
authorability of kaikai (Tier 3 — see `CLAUDE.md`).

## Objective metrics

- **Lane name:** `issue-182-result-option-api`
- **Start:** `2026-05-03T19:43:13-04:00`
- **End:** `2026-05-03T19:56:16-04:00`
- **Wall clock:** ~13 minutes (briefing → all gates green).
- **Files touched:**
  - `stdlib/core/result.kai` (+155 lines net: 7 fns + 16 tests)
  - `stdlib/core/option.kai` (+187 lines net: 8 fns + 18 tests)
  - 30 new files in `examples/stdlib/` (15 fixtures + 15 goldens)
  - 0 changes to `stage2/Makefile` (target auto-discovers via
    `wildcard ../stdlib/core/*.kai`)
- **Build / test invocations during the lane** (TSV at end of report):
  - `make test-stdlib-core-intrinsic` — 1 (10 s, after stdlib edits)
  - `make test-stdlib`                — 1 (64 s, after fixtures)
  - `make tier0`                      — 1 (35 s)
  - `make tier1`                      — 1 (276 s)
  - `make selfhost`                   — 1 (19 s)
  - `make selfhost-llvm`              — 1 (27 s)
- **Compiler errors encountered:** 0.
- **Selfhost outcome:** byte-identical fixed point on both C and
  LLVM backends.

## Compiler errors I encountered

None. Every helper compiled and every fixture matched its golden
on the first run. This was the smoothest stdlib lane so far on
this branch.

The only fix-up was an instrumentation slip — running
`make selfhost-llvm` from the repo root (no rule there) instead
of from `stage2/`. Re-ran from `stage2/` and it passed.

## Friction points

### `Pair` accessibility for `opt_zip`

Pinned in the brief as a worry. The empirical answer:
**no friction.** `Pair` is defined in `stdlib/core/tuple.kai`,
which is alphabetically *after* `option.kai` in the
`$(sort $(wildcard ...))` prelude order. Same situation as
`stdlib/core/list.kai` (which already references `Pair` in its
public `zip`/`unzip` signatures). The kaic2 prelude pipeline
concatenates all `--prelude` decls and resolves them as a single
batch before infer, so forward references across prelude files
are fine. `Option[Pair[a, b]]` typed cleanly with no
declaration-order workaround.

### `result_transpose` typecheck

Pinned in the brief as a worry. Empirical answer:
**no friction.** A nested `match` over `Result[e, Option[a]]`
returning `Option[Result[e, a]]` infers cleanly. HM with the
existing `Some`/`None`/`Ok`/`Err` schemes had no trouble
threading the two type variables through. No annotation other
than the public signature was needed.

### `Option`/`Result` cross-references

`option.kai` declares `opt_ok_or` and `opt_ok_or_else`, both
returning `Result[e, a]`. Even though `option.kai` sorts before
`result.kai`, the typer treats `Option`/`Result`/`Some`/`None`/
`Ok`/`Err` as built-in (registered in
`stage2/compiler.kai:19234-19238`), so the order is irrelevant.
This was confirmed by the green test-stdlib-core-intrinsic.

### `kai test` block harness

The `test-stdlib-core-intrinsic` target loops over
`option result string char tuple list`. New `test "..."` blocks
in `stdlib/core/option.kai` and `stdlib/core/result.kai` are
auto-discovered. **No Makefile edit required.** The existing
header comment in each module already documents the
`KAI_NO_STDLIB=1` pattern.

## Spec ambiguities or interpretive choices

### Did all 15 helpers fit conventions cleanly?

Yes. Every helper is a one- or two-level `match` over an
already-existing constructor. None required new tooling, new
syntax, or any compiler change.

### Helpers that needed special handling

- **`result_collect` and `opt_collect`** are the only recursive
  helpers. Both are written as straightforward
  `match xs { [] -> ...; [h, ...t] -> ... }` and short-circuit
  via a nested `match` on the recursive result. This is *not*
  tail-recursive — for stdlib correctness fixtures (≤ 4
  elements) it's fine, but a stage 2 follow-up could rewrite
  with an accumulator + `list_reverse`. Out of scope for #182.
- **`opt_zip`** uses the `Pair { fst, snd }` record literal
  directly (consistent with `stdlib/core/list.kai`'s `zip`),
  not the 2-tuple sugar `(x, y)`. Either would work; the record
  form is preferred for readability when the field names appear
  again in tests (`p.fst`, `p.snd`).
- **`opt_filter`** uses `if pred(x) { Some(x) } else { None }`
  inside the `Some(x)` branch. A `match pred(x) { true ->
  Some(x); false -> None }` would be equivalent; the `if` form
  reads better and matches the rest of the stdlib idiom.

### Method-call sugar (m12.8)

Per the issue, `r.map_err(F)` lowers to `result_map_err(r, F)`
via the `.field` placeholder lambda machinery. **Not exercised
in this lane** — fixtures and tests use the prefix form for
clarity. Validating the sugar with these helpers is a separate
follow-up.

## Subjective summary

This was a **boring lane in the best sense.** The brief was
crisp (signatures and semantics fully pinned), the conventions
were obvious from the existing 5 helpers in each file, and
nothing in the language pushed back. The most time-consuming
step was writing the 15 fixture+golden pairs (mechanical). The
intrinsic test discipline from PR #178/#183 made it natural to
add 16 + 18 = 34 in-source `test` blocks alongside the fixtures
without feeling like duplication: the intrinsic tests probe
the helper against many shapes, the fixtures probe end-to-end
through the full kaic2 pipeline.

The only "hidden cliff" I anticipated was the prelude-order
question for `Pair`. It turned out to be a non-issue, because
kaic2 batch-resolves preludes. Worth recording explicitly so
the next stdlib lane doesn't worry about it either.

## Limitations of this report

- Single-run timings. tier1 (~276 s) is the dominant cost; it
  varies by ~10 s across runs depending on macOS thermals.
- One agent, one model, one session. Other agents on different
  models may stumble on the parts I found trivial — e.g. the
  `Pair` constructor syntax (`Pair { fst: a, snd: b }`), which
  isn't Rust-style and isn't OCaml-style.
- I did not exercise the `r.map_err(F)` method-call sugar end
  to end. The brief said it works "automatically" via m12.8;
  I trusted that and the intrinsic + fixture tests use the
  prefix form. A follow-up lane could add a fixture mixing
  both call styles.
- I did not measure stage 2 compile-time impact of the new
  helpers. Likely negligible (<1% of compile time), but
  unmeasured.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-03T19:50:20-04:00	tier0	OK	35
2026-05-03T19:55:02-04:00	tier1	OK	276
2026-05-03T19:55:26-04:00	selfhost	OK	19
2026-05-03T19:56:06-04:00	selfhost-llvm	OK	27
```
