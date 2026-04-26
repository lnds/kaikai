# Demos as language probes

Each demo here is written in **target kaikai syntax** — the language we are
designing, not necessarily what `bin/kai` accepts today. Some compile now,
some don't. That mismatch is the signal: it tells us which features still
need to land for which class of program.

## How to use

```sh
make -C demos verify
```

Output ends with a table:

```
  DEMO                STATUS
  ----                ------
  hello               OK
  fizzbuzz            OK
  factorials          PASS (no golden)
  state               FAIL: parse error: unexpected `var`
  mini_ledger         FAIL: unknown type `Money`
```

`OK` = compiles, runs, diff against golden matches.
`PASS (no golden)` = compiles and runs; no `main.out.expected` to diff.
`DIFF` = compiles and runs but stdout diverged from the golden.
`FAIL: <reason>` = compiler or runtime error; first line of stderr shown.

A demo failing today is not a bug in the demo — it's a measurement of where
the compiler is.

## Conventions

- Use the language as the design docs say it should read, including
  features still on the roadmap.
- Ship a `main.out.expected` only when the expected output is **already
  decided** (so a future passing run can be golden-diffed).
- Keep each demo small and focused on one or two features. Bigger
  showcases come later, when the basics pass.

## Roadmap features each demo exercises (cheat sheet)

| Demo | Features it relies on |
|---|---|
| `hello` | `Console` effect, default handler |
| `fizzbuzz` | range, `\|` map pipe, `match` with guards |
| `factorials` | recursion, range, `each`, refinements (m12.6) |
| `collatz` | recursion, list cons + spread, refinements (m12.6) |
| `quicksort` | list pattern matching, `filter`, recursion, `++` operator (m7d §23) |
| `euler1` | range, `filter`, `sum` |
| `euler2` | recursion, accumulators |
| `euler3` | recursion (largest prime factor) |
| `euler4` | nested recursion, palindrome via digit reverse |
| `euler5` | LCM via gcd + recursion |
| `euler6` | arithmetic series vs sum of squares |
| `state` | `var` cell + `:=` short form (m7b #4 / target), closures |
| `state_var` | `var x = init` desugar to `with State[T](init) as x` (m7b #5b) — explicit `x.get()` / `x.set(v)` |
| `state_explicit` | `handle ... with State[Int](0)` (m7b #11 parametric effects) |
| `beer_song` | match with literal arms, recursion, string interpolation |
| `imc` | match with guards on `Real` |
| `spiral` | `array_make` + array indexing (m7b #6) + `var` cells + `while` loop (`stdlib/loop.kai`) + `++` (m7d §23) |
| `stack` | **user-defined effect** (m7a) + handler + `var` cell |
| `forth` | sum types with positional fields, multi-arg match on `(token, stack)`, `Fail` effect, `++` (m7d §23) |
| `9d9l/huffman` | sum types + assoc list (Map deferred) + priority queue + recursive encode/decode + **bit ops** (m13 §16: `bit.shl`/`bit.shr`/`bit.and`/`bit.or` as intrinsics) + bit-pack/unpack |
| `toquefama` | `Stdin` + `Console`, recursion, multi-arg match on `(guess, target)`, `todo!` (m7d §1) |
| `blackjack` | sum types, records, hypothetical `Random` effect, `todo!` (m7d §1), `++` (m7d §23) |
| `mini_ledger` | UoM (m12.5) + refinements + contracts (m12.6) + record update (`with`) + protocols (m12.8 — `Show` + `#derive`) |

When a feature lands, demos that depended on it should flip from `FAIL` to
`OK` — that's the validation event.

## Historical sketches — migration log

All 12 pre-redesign Go-frontend sketches that lived as flat `*.kai` here
have been migrated to per-demo subdirectories (in target syntax), or
deleted when their syntax / concepts had no recovery path. Originals
preserved in git history.

### Migrated (12)

| Flat → | Subdirectory | Target shape |
|---|---|---|
| `beer_song.kai` | `beer_song/` | match with literal arms + recursion |
| `imc.kai` | `imc/` | match with guards |
| `euler2.kai` | `euler2/` | even-fib accumulator |
| `euler3.kai` | `euler3/` | largest prime factor |
| `euler4.kai` | `euler4/` | palindrome search |
| `euler5.kai` | `euler5/` | LCM via gcd |
| `euler6.kai` | `euler6/` | arithmetic identity |
| `spiral.kai` | `spiral/` | array indexing + `var` + `while` loop |
| `stack.kai` | `stack/` | user-defined effect with handler |
| `forth.kai` | `forth/` | tiny token-driven evaluator |
| `toquefama.kai` | `toquefama/` | mastermind variant; uses `todo!` for digit parser |
| `blackjack.kai` | `blackjack/` | minimal deal; uses `todo!` for deck builder pending `Random` |

### Earlier cleanup — deleted (16)

- Redundant with `examples/` (6): hello, fizzbuzz, factorials, collatz, euler1, quicksort.
- Elixir-only syntax with no recovery path (5): maps_records, bin_search_tuple, tuples, generators, strain.
- Blocked on roadmap features with cleanly different target shape (5): actors, counter_actor, pmap, clock, state — fresh demos when m8 / `Net+Time` land.
