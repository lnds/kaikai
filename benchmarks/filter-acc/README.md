# filter-acc

A filter written with an accumulator: each of 300 rounds builds 20K
records and keeps the ones a predicate rejects as heavy, through
`keep(rest, if heavy(c) { acc } else { [c, ...acc] })`. The record moves
into the accumulator on one branch of an `if` inside the self tail call's
argument; on the other branch it must be released.

```
./run.sh
```

prints the median CPU time (user+sys) of 15 alternated runs per backend,
then each backend's `KAI_TRACE_RC=1` ledger. It runs with
`KAI_THREADS=1`. A leak in the discarding branch shows as `leaked` in the
millions and a `live_peak` that grows with the rounds; with the branch
paid, `leaked=0` and `live_peak` stays near one round's 60K cells.
