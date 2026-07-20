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

## Results

Measured 2026-07-19 on `b2a50d4b` — the first commit where `KAI_THREADS>1`
is sound (the double-dispatch fix, #1301/#1284). Before it the runtime lost
fibers roughly one run in eight, so a throughput number measured noise.

| | |
|---|---|
| host | Apple M3 Max, 16 cores (12 performance + 4 efficiency), Darwin 25.5.0 |
| kaikai | 0.103.0 hanga-roa, stage 2 self-hosted, `--release`, LLVM 21.1.8 |
| Elixir | 1.20.2 (compiled with Erlang/OTP 29) |
| Erlang | OTP 29, ERTS 17.0.3, JIT, `smp:16:16` — 16 schedulers online |
| host state | **not headless**: WindowServer ~50%, sketchybar ~22%, loginwindow ~12%, WallpaperAerials ~5% — about 0.9 of 16 cores, ~5% background load |
| samples | median of 7 warm runs per cell, first run discarded, strictly serial |

The background load is symmetric — it costs kaikai and the BEAM the same
share of the machine — so the **ratios below hold**; it is the absolute
milliseconds that are inflated a few percent. Deliberately measured against
the newest Elixir: a win there takes no qualifiers.

### Median wall-clock of the parallel region (ms, lower is better)

`native` is the default backend, so the first table is what a user gets today.

| scenario | kaikai@1 | @2 | @4 | @8 | @16 | BEAM@default | BEAM@+S1 |
|---|---|---|---|---|---|---|---|
| `serial` (1 x 2e8) | 214.2 | 218.1 | 226.8 | 226.3 | 224.1 | 381.2 | 570.8 |
| `parallel8` (8 x 2e8) | 1708.9 | 873.6 | 443.6 | 224.4 | 224.7 | 451.6 | 4672.4 |
| `parallel16` (16 x 1e8) | 1708.7 | 886.0 | 443.5 | 224.4 | **156.1** | **272.2** | 4695.7 |
| `oversub64` (64 x 2.5e7) | 1711.4 | 873.4 | 445.4 | 224.2 | 154.7 | 262.7 | 4767.7 |

Same source, same `--release`, via `--backend=c`:

| scenario | kaikai@1 | @2 | @4 | @8 | @16 | BEAM@default | BEAM@+S1 |
|---|---|---|---|---|---|---|---|
| `serial` (1 x 2e8) | 101.4 | 103.3 | 105.9 | 106.0 | 108.1 | 392.5 | 588.3 |
| `parallel8` (8 x 2e8) | 821.4 | 421.6 | 211.8 | 106.6 | 108.4 | 414.4 | 4796.8 |
| `parallel16` (16 x 1e8) | 811.5 | 422.5 | 210.5 | 106.5 | **77.4** | **273.2** | 4746.8 |
| `oversub64` (64 x 2.5e7) | 822.1 | 423.3 | 213.1 | 107.1 | 71.1 | 263.4 | 4709.3 |

Every cell in a row printed the same checksum, on both runtimes and at every
`KAI_THREADS` — the M:N scheduler neither lost nor duplicated work in ~800
measured runs.

### Verdict

**kaikai wins, by less than the folklore said.** The figures quoted in #1207
and #1269 were ~3.6x serial and ~5x parallel against the BEAM. Reproduced
in-repo against Elixir 1.20.2 / OTP 29:

| claim | measured | holds? |
|---|---|---|
| ~3.6x serial | 3.87x (`c`) / 1.78x (`native`) vs BEAM@default | only on the C backend |
| ~5x parallel wall | 3.53x (`c`) / 1.74x (`native`) | **no** — best is 3.5x |

Two things the old headline hid.

**kaikai does not scale better than the BEAM — it scales the same, from a
faster start.** Both runtimes reach ~11x on 16 cores for this workload
(kaikai 10.5-10.9x, BEAM 11.2x measured against its own single-scheduler-mode
per-unit cost). The entire kaikai advantage is single-core speed carried
through an equally good scheduler. That is still the right thing to claim —
native code beats a JIT'd VM per core — but "we parallelise better" is not
supported by this data.

**Which backend you build with matters more than the scheduler.** The native
backend emits ~2.1x slower code than the C backend on this loop (#1326), and
that gap swallows more than half the advantage over the BEAM. Closing #1326
is worth more to the headline than any further scheduler work.

What the M:N scheduler itself does well:

- **95% parallel efficiency at 8 threads** (1708.7 → 224.4 ms, a 7.6x speedup
  on 8 threads). Scaling to 16 is 10.9x, limited by the host's 12+4 core
  split, not by the runtime.
- **Oversubscription is free.** 64 actors on 16 threads is no slower than 16
  actors — marginally faster, since finer work units even out the P/E-core
  imbalance.
- **`+S 1` is not a fair single-core BEAM baseline.** BEAM@+S1 measures
  570.8 ms serial against BEAM@default's 381.2 ms for the same one-process
  work — pinning to one scheduler costs the BEAM 1.5x beyond losing cores.
  Quoting a speedup against `+S 1` inflates it; the tables above use
  BEAM@default for every comparison in the verdict.

### Measurement quality

Spread (max-min as a % of the median) is under 3% for most cells. It exceeds
10% in the fully saturated ones — kaikai@16 and BEAM@default — which is
where the ~5% background UI load competes directly for the same 16 cores. An
independent re-take at 15 warm runs moved those medians by under 5%
(`parallel16` kaikai@16: 156.1 → 149.8 ms native, 77.4 → 75.5 ms C), so the
spread is outlier-driven and the medians are reproducible. The harness flags
any cell past `NOISE_PCT` (default 10%) rather than hiding it.

The margins here (1.7x-3.9x) are far outside that noise, so the ranking is
not in question; a clean-room re-take would tighten the third digit, not
change the verdict.
