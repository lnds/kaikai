# alloc-trmc

An allocation-heavy workload: each of 80 rounds rebuilds a 50K-cell user
variant (`mapv`, a same-arity TRMC step), converts it to a list (`tolist`,
cons TRMC), filters it (`keep_even`) and sums it. That is about 10M
allocations and as many frees, so the run time is dominated by the
allocator's per-cell path: the pool, the RC bookkeeping and the ledger
counters.

```
./run.sh
```

prints the median CPU time (user+sys) of 15 alternated runs for the C
backend and, when `kaic2` is built with libLLVM, the native one. It runs
with `KAI_THREADS=1`.

`KAI_TRACE_RC=1` on either binary prints the ledger: `alloc_total=10050001
free_total=10050000` on both backends.
