# Lane experience — issue #1151: recursive vec_push dies "out of memory"

Fix lane for the flagship-shape regression: a tail-recursive push loop
from `vec.empty()` through the stdlib module call died `kai: out of
memory` at ~100 iterations, both backends, Int and records.

## The exact cause

`kai_vec_push_impl` computed the clone capacity **before** the
uniqueness decision, unconditionally:

```c
int64_t ncap = v->as.vec.cap * 2;      /* always, even with spare room */
...
v = kai_vec_ensure_unique(v, ncap);    /* shared -> clone at ncap */
```

On a UNIQUE vec this is harmless (`ensure_unique` ignores the hint and
the real growth check `len == cap` runs after). On a SHARED vec the
clone takes `ncap` as its capacity — so every shared push doubles the
capacity whether or not the vec is full. A loop that keeps the vec
shared at each push (see below) doubles capacity **per push, not per
fill**: cap = 4, 8, …, 2^k. The `cap * stride` size computation blows
through `int64`/`size_t` within ~60–100 pushes, `malloc` returns NULL,
and the runtime reports "out of memory" with kilobytes of live data —
the "corrupted size" of the issue title is arithmetic overflow, not a
corrupt register.

Why the vec is shared every iteration: for `fill(vec.push(v, k), …)`
Perceus dups `v` at the call and the tcrec step site drops the old
param after rebinding — `pcs_call_consumers_linear` classifies every
lowercase `EModCall` (module fn call) as non-linear by design
("capability ops internally incref"), so the loop param never enters
the skip-set and the vec reaches `vec_push` with rc ≥ 2. Every push is
then a CoW clone. The builtin `vec_push` shape (what the case-6 bench
file uses) never hit the bug: the raw fused path moves the vec, it
stays unique, and appends run in place.

The issue's hypothesis (raw/tcrec register read gone garbage) verified
WRONG: the emitted C for the repro is sound — dup/drop pairs balance;
the failure is pure runtime capacity arithmetic. This also decodes the
isolation matrix: 3 chained pushes = only 3 doublings; `make` + `set`
loop = `vec_set` passes `need_cap = cap` (no doubling); the merged
fixtures push below the regime or hold the vec unique.

## The fix

Grow only at capacity — the pre-computed hint stays `cap` unless
`len >= cap`. Three sites with the same shape: `kai_vec_push_impl` and
`kai_vec_push_rec_raw` in `stage2/runtime.h`, `kai_vec_push_impl` in
`stage0/runtime.h`. The unique in-place path is untouched (same
instructions), so the raw bench regime is unaffected. The native
runtime bitcode regenerates from the patched header at build time
(`tools/gen-runtime-bc.sh`, already wired into `make KAI_LLVM=1 kaic2`).

## Bench re-measure (case-6 gate)

`tools/native-perf/benches/vec_fill_sum_point.kai`, N = 10M, macOS
arm64 M-series, -O2, warm (best of 10), post-fix toolchain:

| bench                 | backend | warm wall | RSS    | #1147 claim  |
|-----------------------|---------|-----------|--------|--------------|
| `Vec[Point]` fill+sum | C       | 0.07 s    | 155 MB | 0.04 s / 155 MB |
| `Vec[Point]` fill+sum | native  | 0.07 s    | 155 MB | 0.04 s / 155 MB |

Output exact (50000005000000). Honest correction: the 0.04 s wall does
not reproduce on this machine today — 0.07–0.08 s is what measures,
2× the claimed wall at identical RSS; still ~8× the 0.61 s Array-boxed
baseline wall and 5.5× its memory. Note the bench file itself never
OOM'd on main (builtin push = raw in-place path); the broken surface
was the **module** call `vec.push` — the shape every user writes.

## What remains (follow-up, measured, not filed)

With the fix, module-surface `vec.push` in a self-recursive loop is
correct but CoW-per-push: O(n²) bytes copied — 100k pushes ≈ 0.7 s,
10M intractable. The lever is `pcs_call_consumers_linear`: extend
linearity from bare `EVar` user fns to *known user module fns* (the
resolver/fnreg can tell them from capability ops), so the loop param
is moved instead of dup'd — the same over-conservatism #1147 fixed for
self-calls only. It touches the skip-set ↔ tcrec dropmask ↔ emit
agreement seam (the delicate triangle of #1147's surprises), so it is
a lane of its own, not a rider on a runtime fix.

## Fixtures added (wired)

- `examples/perceus/vec_push_growth_1151.kai` — the issue repro:
  module-call push loop from empty, Int + record, N=100, probe values.
- `examples/perceus/vec_push_growth_stress_1151.kai` — growth stress
  to N=100k on BOTH append regimes (module CoW + builtin raw), length
  + last element + full-range probe sums.
- Targets: `test-perceus-1151-vec-push-growth` (TEST_LIGHT_TARGETS),
  `-native` (tier1-native.yml step), `-asan` (root `tier1-asan` chain;
  stress under ASAN ≈ 4 s).
- Goldens assert values only — deliberately no `vec_cow`/`vec_inplace`
  exact counters, so the follow-up that turns module pushes in-place
  does not break the growth gate.

## Cost vs estimate

Small lane, one honest surprise: the bisect budgeted for a compiler
miscompile (per the issue hypothesis) resolved in one instrumented
`printf` round — the emitted C was clean and the runtime trace showed
`cap` doubling per push with `unique=0`. Reading the #1147 retro first
pointed straight at the dup-on-module-call, which turned the "why
shared?" question from a hunt into a lookup.
