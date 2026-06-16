# native-perf — native-vs-C codegen benchmark harness

Measures the wall-time gap between the two `kaic2` backends (C-direct and
in-process libLLVM native) on the *same* binary, so a regression or improvement
in native codegen quality is visible. Backs the diagnosis in
[`docs/native-codegen-perf-plan.md`](../../docs/native-codegen-perf-plan.md).

## Run

```sh
RUNS=3 tools/native-perf/run.sh            # all benches, both backends
tools/native-perf/run.sh benches/arith_runtime.kai   # one bench
ROUTES="native" RUNS=5 tools/native-perf/run.sh      # native only
```

`run.sh` builds each `benches/*.kai` (those defining `fn main`) with
`KAI_BACKEND=c` and `KAI_BACKEND=native`, then times the binary best-of-`RUNS`.
It prints binary size and wall time per route. Needs a `kaic2` built with the
native backend (`make -C stage2 KAI_LLVM=1 LLVM_CONFIG=… kaic2`).

## Benches

Each bench fixes its iteration count `N` *opaquely*: `N * (seed / seed)` where
`seed = string_length(program_name())`. At runtime `seed / seed == 1` so `N` is
exact, but the optimiser cannot prove `seed != 0` and so cannot const-fold the
loop to a closed form. This defeats the const-fold that otherwise makes a pure
arithmetic loop a misleading benchmark.

- `arith_const` — the brief's original repro (literal bound, const-foldable; kept
  only to reproduce the historical 340 KB/6 s figure).
- `arith_runtime` — honest pure arithmetic (~86× native-vs-C).
- `deep_rec` — non-tail recursion, fib trees with a varying argument (~30×).
- `variant_match` — variant build + `match` interpreter (tag dispatch + field
  projection + nested match).
- `list_fold` — cons build + fold, alloc/RC heavy (~3.4×).
- `rbtree_corpus` — the canonical Perceus rb-tree (`rb_tree.kai`, copied from
  `examples/perceus/`), 2M inserts (~2×).

Not wired into a CI tier — perf benches are diagnostic, not merge gates.
