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
- Read the `p2:` line of the banner before publishing a native number. A
  table taken with the P2 runtime bitcode in opt-out measures the opt-out,
  not the backend — `bin/kai --version` and `make KAI_LLVM=1 kaic2` report
  the same state, and `tools/gen-runtime-bc.sh --status-line` says how to
  turn it on.

## Results

Measured 2026-07-20 on `d425a904`. It supersedes a 2026-07-19 take on
`b2a50d4b` whose native table was ~2x slower across the board: that pass ran
with the P2 runtime bitcode in opt-out and before #1336, and the two together
put the native backend in a slow path it is no longer in. The environment
banner now prints the P2 state on every run so a table cannot silently be
taken that way again.

| | |
|---|---|
| host | Apple M3 Max, 16 cores (12 performance + 4 efficiency), Darwin 25.5.0 |
| kaikai | 0.103.0 hanga-roa, stage 2 self-hosted, `--release`, LLVM 21.1.8 |
| P2 bitcode | **active** — `bin/kai --version` reports `native p2: active` |
| Elixir | 1.20.2 (compiled with Erlang/OTP 29) |
| Erlang | OTP 29, ERTS 17.0.3, JIT, `smp:16:16` — 16 schedulers online |
| host state | **not headless**: the usual desktop session, a few percent of background load |
| samples | median of 7 warm runs per cell, first run discarded, strictly serial |

The background load is symmetric — it costs kaikai and the BEAM the same
share of the machine — so the **ratios below hold**; it is the absolute
milliseconds that are inflated a few percent. Deliberately measured against
the newest Elixir: a win there takes no qualifiers.

### Median wall-clock of the parallel region (ms, lower is better)

`native` is the default backend, so the first table is what a user gets today.

| scenario | kaikai@1 | @2 | @4 | @8 | @16 | BEAM@default | BEAM@+S1 |
|---|---|---|---|---|---|---|---|
| `serial` (1 x 2e8) | 105.6 | 107.4 | 108.6 | 109.5 | 111.1 | 395.1 | 609.6 |
| `parallel8` (8 x 2e8) | 850.4 | 425.1 | 213.6 | 108.4 | 108.3 | 460.4 | 4821.2 |
| `parallel16` (16 x 1e8) | 842.8 | 426.1 | 212.8 | 107.7 | **86.2** | **301.4** | 4820.9 |
| `oversub64` (64 x 2.5e7) | 851.3 | 427.7 | 215.8 | 108.6 | 78.6 | 278.6 | 4835.0 |

Same source, same `--release`, via `--backend=c`:

| scenario | kaikai@1 | @2 | @4 | @8 | @16 | BEAM@default | BEAM@+S1 |
|---|---|---|---|---|---|---|---|
| `serial` (1 x 2e8) | 106.6 | 107.8 | 109.6 | 108.5 | 110.5 | 397.0 | 606.4 |
| `parallel8` (8 x 2e8) | 850.4 | 428.5 | 213.9 | 108.0 | 108.7 | 461.8 | 4808.0 |
| `parallel16` (16 x 1e8) | 841.9 | 427.8 | 213.8 | 107.0 | **78.6** | **296.3** | 4833.1 |
| `oversub64` (64 x 2.5e7) | 850.2 | 427.2 | 214.5 | 108.5 | 74.5 | 276.7 | 4792.3 |

Every cell in a row printed the same checksum, on both runtimes and at every
`KAI_THREADS` — the M:N scheduler neither lost nor duplicated work in ~800
measured runs.

**P2 is not what moves this workload.** Toggling the bitcode on the same
compiler, same commit, changes the native column by under a percent
(`serial` 105.6 vs 106.2 ms, `oversub64@16` 78.6 vs 79.2) — the load is a
pure arithmetic loop that never calls a runtime op, so there is nothing for
the runtime inlining to bite on. P2 earns its keep on heap-bound code
instead (`list_fold` 1.66x, `rbtree_corpus` 3.78x on the same host). The
native column moved because #1336 fixed the literal-divisor emission this
loop's `i % 7` depends on; the earlier take confounded the two.

### Verdict

**kaikai wins, by less than the folklore said.** The figures quoted in #1207
and #1269 were ~3.6x serial and ~5x parallel against the BEAM. Reproduced
in-repo against Elixir 1.20.2 / OTP 29:

| claim | measured | holds? |
|---|---|---|
| ~3.6x serial | 3.74x (`native`) / 3.72x (`c`) vs BEAM@default | yes, on both backends |
| ~5x parallel wall | 3.50x (`native`) / 3.77x (`c`) | **no** — best is 3.8x |

Two things the old headline hid.

**kaikai does not scale better than the BEAM — it scales the same, from a
faster start.** Both runtimes reach ~10x on 16 cores for this workload
(kaikai 9.8x native / 10.7x C, the BEAM about the same measured against its
own single-scheduler-mode per-unit cost). The entire kaikai advantage is
single-core speed carried through an equally good scheduler. That is still
the right thing to claim — native code beats a JIT'd VM per core — but "we
parallelise better" is not supported by this data.

**The backend no longer picks the winner.** The earlier take had native
emitting ~2.1x slower code than C on this loop (#1326); with #1336 shipped
the two backends land within a percent of each other in every cell, so the
default backend is no longer costing the headline anything here.

What the M:N scheduler itself does well:

- **98% parallel efficiency at 8 threads** (850.4 → 108.4 ms, a 7.8x speedup
  on 8 threads). Scaling to 16 is 9.8x, limited by the host's 12+4 core
  split, not by the runtime.
- **Oversubscription is free.** 64 actors on 16 threads is no slower than 16
  actors — marginally faster, since finer work units even out the P/E-core
  imbalance.
- **`+S 1` is not a fair single-core BEAM baseline.** BEAM@+S1 measures
  609.6 ms serial against BEAM@default's 395.1 ms for the same one-process
  work — pinning to one scheduler costs the BEAM 1.5x beyond losing cores.
  Quoting a speedup against `+S 1` inflates it; the tables above use
  BEAM@default for every comparison in the verdict.

### Measurement quality

Spread (max-min as a % of the median) is under 3% for most cells. It exceeds
10% in the fully saturated ones — kaikai@16 (11-20%) and BEAM@default (up to
49% on `serial`) — which is where the background desktop load competes
directly for the same 16 cores. Those are the cells to re-take on a quiet box
before quoting a third digit; the harness flags every one of them past
`NOISE_PCT` (default 10%) rather than hiding it.

The margins here (3.5x-3.8x) are far outside that noise, so the ranking is
not in question; a clean-room re-take would tighten the third digit, not
change the verdict.
