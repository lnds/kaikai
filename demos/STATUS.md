# Demos: how to read the output

Each demo here is a complete kaikai program. `make -C demos verify`
compiles and runs every one; the table it prints is the measurement, and
`make -C demos no-regression` (part of tier 0) fails when fewer demos pass
than `baseline.txt` records.

## How to use

```sh
make -C demos verify
```

Output ends with a table:

```text
  DEMO                 STATUS
  ----                 ------
  beer_song            OK
  blackjack            PASS (no golden)
  collatz              OK
  ...
  34 OK, 3 ran (no golden), 0 diff, 0 failed
```

`OK` = compiles, runs, diff against golden matches.
`PASS (no golden)` = compiles and runs; no `main.out.expected` to diff.
`DIFF` = compiles and runs but stdout diverged from the golden.
`FAIL: <reason>` = compiler or runtime error; first line of stderr shown.

## Conventions

- Ship a `main.out.expected` whenever the output is deterministic; demos
  that draw from `Random` (`blackjack`, `poker_dealer`) or read stdin
  (`toquefama`) have none.
- A demo whose golden records one legal interleaving pins the scheduler in
  a `main.env` beside it (`KAI_THREADS=1`), one `VAR=value` per line.
- Keep each demo small and focused on one or two features.
- Update `baseline.txt` only when a demo legitimately leaves the passing
  column.

## Features some demos exercise

| Demo | Features it relies on |
|---|---|
| `hello` | `println` with the default `Stdout` handler |
| `fizzbuzz` | range, `\|` map pipe, `match` with guards |
| `factorials` | recursion, range, `each`, `requires` / `ensures` contracts |
| `collatz` | recursion, list spread, `requires` |
| `quicksort` | list pattern matching, `filter`, recursion, `++` |
| `euler1` | range, `filter`, `sum` |
| `euler2` | recursion, accumulators |
| `euler3` | recursion (largest prime factor) |
| `euler4` | nested recursion, palindrome via digit reverse |
| `euler5` | LCM via gcd + recursion |
| `euler6` | arithmetic series vs sum of squares |
| `state_var` | `var n := init` cell with explicit `n.get()` / `n.set(v)` |
| `state_explicit` | `handle ... with State[Int](0)` parametric effect |
| `beer_song` | match with literal arms, recursion, string interpolation |
| `imc` | match with guards on `Real` |
| `spiral` | `Array[Int]` indexing under `Mutable` + `var` cells + `while` (`import loop`) + `++` |
| `stack` | user-defined effect + handler + handler-scoped `var` |
| `stack_explicit` | the same stack with a parametric stateful handler (`resume(v, ns)`) |
| `forth` | sum types with positional fields, multi-arg match on `(token, stack)`, `Fail` effect, `++` |
| `9d9l/huffman` | sum types + assoc list + priority queue + recursive encode/decode + bit ops (`bit._shl` / `bit._shr` / `bit._and` / `bit._or`) |
| `toquefama` | `Stdin` + `Console`, recursion, multi-arg match on `(guess, target)`, `todo!` |
| `blackjack` | sum types, records, `Random` effect + `shuffle` from `stdlib/random.kai` |
| `poker` | sum types with positional payloads, records, list pattern matching (`[g1, g2]`), `list.sort_by` + custom comparators, run-length grouping on a sorted list |
| `poker_dealer` | part 2 of poker: `Random` + `shuffle`, deals two 5-card hands from a 52-card deck and picks the winner over 3 rounds |
| `mini_ledger` | record spread (`T { ...src, field: v }`), refinement-typed fields, `requires` / `ensures` contracts, a user-declared `Audit` effect |
| `concurrent`, `*_concurrent`, `parallel_actors`, `ping_pong` | fibers, nursery, actors, and IO that parks a fiber instead of the thread |
