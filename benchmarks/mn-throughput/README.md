# M:N throughput — kaikai actors vs BEAM processes

Issue #1207 step 5 asks for an in-repo, re-runnable parallel throughput
benchmark. Until now the 3.6x / 5x-vs-BEAM figures quoted in #1207 and
#1269 came from external suites that do not live here, so nobody who
clones the repo — including us — could reproduce them.

This directory is that benchmark: the same workload shape on both
runtimes, plus a harness that measures wall-clock and prints the table.

## What it measures

`workers` CPU-bound actors each grind a pure tail-recursive sum over
`load` iterations with no yield point inside, then send the partial to a
collector that adds them. One burst therefore pins its scheduler thread
end to end, which is exactly the shape the M:N scheduler has to spread.

The total is a function of `(workers, load)` only. It must be identical
at every `KAI_THREADS` and on the BEAM; the harness aborts on a
mismatch, because a wall-clock next to a wrong checksum measures nothing.

Both sides time their own parallel region with a monotonic clock, so the
BEAM's VM boot is not charged against BEAM throughput. Total process
wall is reported in a second table, where that startup tax stays visible.

## Workload equivalence

| | kaikai | Elixir |
|---|---|---|
| inner step | `grind(i - 1, acc + (i % 7))` | `grind(i - 1, acc + rem(i, 7))` |
| unit of concurrency | actor fiber (`spawn_actor`) | process (`spawn`) |
| hand-off | `Actor.send` / `Actor.receive` | `send` / `receive` |
| collector | tail-recursive accumulate | tail-recursive accumulate |
| timing | `time.monotonic` around the region | `:timer.tc(fun, :nanosecond)` |

`rem(i, 7)` equals kaikai's `i % 7` for positive `i`, and both sides run
the same iteration count, so the printed totals match exactly. kaikai's
`Int` is 64-bit and Elixir's is arbitrary precision; at the loads used
here the sum is ~5e9, far inside 64 bits, so no promotion difference
enters the measurement.

## Running

```sh
make bench-mn-throughput          # from the repo root
./benchmarks/mn-throughput/run.sh # or directly
```

Knobs: `RUNS` (warm runs per cell, default 5), `SCENARIOS`
(`name:workers:load`, space separated), `KAI_THREAD_LIST`,
`BEAM_SCHEDS`, `SKIP_BEAM=1`.

**Reported, never a gate.** Wall-clock on a developer box is noisy;
a regression should be visible without blocking merges on host noise.

## Method

The rules below are not decoration — the measurement lane in
`docs/lane-experience-issue-1258.md` produced three rounds of numbers
that led nowhere for want of them.

- Serial runs only. Two measured processes on one box change the thing
  being measured.
- Discard the first run of every cell. macOS cold start is ~250-350ms
  and lands entirely in run 1.
- Median of >= 5 warm runs, not the mean.
- Checksum verified on every single run, not just the first.
