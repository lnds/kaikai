# Lane experience — issue #911: self-compile throughput regression

Measurement-first lane. Goal: find what regressed stage-2 self-compile
throughput from the 2026-06-05 baseline (≈2,700 LOC/s) to 0.70× (1,881
LOC/s on `dd0714b3`), and fix it only if the cause is a clean complexity
issue. Outcome: **clean fix + diagnosis** — one accidental O(n²) memoised
(+5%), the rest attributed to inherent monomorphisation cost plus a
second, structurally-larger quadratic left as a named follow-up.

## Attribution (measured, not assumed)

All numbers: same Mac, C-direct backend, best-of-3, self-compile of each
commit's OWN stage-2 bundle (so throughput LOC/s normalises out corpus
growth). `--library-mode` runs lex→parse→resolve→infer and stops, isolating
the front-end; the back-end figure is `full − library-mode`.

| Commit | Pos | Corpus | full s | lib s | back-end s | full LOC/s | FE LOC/s | BE LOC/s |
|---|---|---|---|---|---|---|---|---|
| 9a2aba5c (base) | 0 | 85,963 | 27.5 | 17.2 | 10.3 | 3,127 | 4,987 | 8,387 |
| e9c545ff | 170 | 95,098 | 37.2 | 21.9 | 15.4 | 2,554 | 4,350 | 6,183 |
| 2457ace0 | 271 | 90,235 | 42.2 | 20.3 | 22.0 | 2,137 | 4,454 | 4,107 |
| bdddaf7c | 320 | 91,061 | 43.2 | 21.2 | 22.1 | 2,106 | 4,299 | 4,128 |
| e714b092 | ~378 | 92,957 | 49.4 | 25.7 | 23.7 | 1,881 | 3,617 | 3,917 |
| HEAD ae1fbbeb | 384 | 93,032 | 50.0 | 26.1 | 24.0 | 1,859 | 3,564 | 3,884 |

Two facts the bisect nails down:

1. **The issue's hypothesis is refuted.** #903's `/Mutable` sweep
   (`49513c77`) lands between `e714b092` and HEAD. Those two commits time
   identically (49.4s vs 50.0s, both phases). The sweep contributes ≈0 to
   the regression. It is a soundness fix and stays — it is simply not the
   cause.

2. **The back-end, not the front-end, dominates the regression.** Front-end
   throughput fell 4,987→3,564 LOC/s (0.71×); the back-end fell
   8,387→3,884 LOC/s (0.46×). The largest back-end step is between pos 170
   (`e9c545ff`, 6,183 LOC/s) and pos 271 (`2457ace0`, 4,107) — the
   native-codegen-perf batch (#852) and the protocol-bounded-generics work
   (#877/#891/#893/#897), not one single commit. Note the corpus is NOT
   monotonic: `e9c545ff` carried 95,098 LOC, larger than HEAD's 93,032 —
   later refactors shrank it — which is exactly why throughput (LOC/s), not
   wall-time, is the honest metric.

The "back-end" label is slightly loose: the full path calls
`infer_program_with_protos_cached`, which library-mode's `infer_program`
does NOT — protocol-spec synthesis (`synth`) runs inside the cached
inferencer. So part of the "back-end" delta is protocol-generic
monomorphisation that the `full − library` split files under back-end.

## Profile (macOS `sample`, HEAD, self-time)

| Bucket | self % |
|---|---|
| `synth*` — protocol-bounded generic spec synthesis | 23.6% |
| runtime prims — `op_eq` / `strcmp` / `op_field` (linear name lookups) | 22.7% |
| infer / typer | 17.5% |
| proto-rename O(n²) (`local_arities_for_mo_canon` etc.) | 6.3% |

The proto-rename frames are deeply recursive, so they over-show in
appearance-count but under-show in self-time — wall-clock impact landed at
~5%, see below.

## The fix (clean, applied)

`rename_proto_calls_decls_mo` (protos.kai) recomputed the per-module shadow
set — `filter_shadowed_ops(raw_ops, local_arities_for_mo_canon(local_fns,
dmo, ...))` plus `local_names_for_mo_canon(...)` — once PER DECL. Those
depend only on the decl's canonical module bucket (`dmo`). A self-compile
carries thousands of decls but tens of distinct modules (≈84 stdlib
modules + the root bucket), so the per-decl recompute is O(decls × fns) —
the same O(n²) retention #901/#912 fixed on the emit and KIR paths.

Memoised by module origin: a `ShadowByMod` cache (root bucket + a
per-module entry list) threaded through a `rename_proto_calls_decls_mo_loop`,
mirroring the existing `FnsByMod` / `fns_prefer_module_cached` pattern in
emit_shared.kai. Per-decl cost becomes an O(1) lookup after the first decl
of each module.

- Profile: `local_arities_for_mo_canon` frames 3478 → 919.
- Wall: 52.2s → 49.6s, ~5%, consistent across 4 interleaved A/B rounds.
- Selfhost byte-id: `kaic2b.c == kaic2c.c` (deterministic; output unchanged
  by construction beyond the source edit itself).
- `km cogcom`: new fns 1/2/4 — all `simple`, under the A-grade bar.

## What is NOT fixed here, and why (diagnosis)

- **`ty_env_collect_candidates` (infer.kai) is a genuine O(env_size ×
  call_sites) quadratic.** It walks the entire `env.entries` (thousands,
  growing with every decl/import) doing string-suffix matching, called once
  per `EVar`/`EModCall` during `synth`. It is the source of much of the
  op_eq/strcmp 23%. A clean fix is a per-target `name→candidates` index on
  `TyEnv`, built once and maintained on every `add`. That is a structural
  change to the typer's core env type in F-grade `infer.kai`, with real
  soundness risk (an index that desyncs from the authoritative
  `env.entries` is the "second source of truth" bug-class). Per
  one-thing-per-worktree discipline it belongs in its own lane with its own
  gate — byte-id + serial parity + a proof the index yields the same
  candidate set as the linear walk across the whole corpus. Filed as a
  named follow-up, not a vague "later".

- **`synth` spec generation itself (~24%) is inherent cost**, not a
  regression: monomorphising more protocol-bounded generics is a new
  feature batch since baseline (#877/#891/#893/#897). It does more work
  because it does more.

## Scope-as-planned vs scope-as-shipped

Planned: bisect, attribute, fix if clean. Shipped exactly that — with the
twist that the headline suspect (#903) was the wrong one, and the clean fix
that did exist (proto-rename) is a smaller win (~5%) than the bulk of the
regression, which is inherent + a second quadratic deferred for risk. Both
the issue's outcomes — "clean fix" and "definitive diagnosis" — landed
together.

## Structural surprises

- `--library-mode` and the full path use DIFFERENT inferencers
  (`infer_program` vs `infer_program_with_protos_cached`), so `full − lib`
  is not a clean infer/emit split — protocol synthesis hides inside the
  cached front-end. Worth knowing for any future phase-timing work; a real
  per-pass timer would beat the subtraction trick.
- The corpus is non-monotonic across the range. Anyone re-deriving these
  numbers from wall-time alone (not LOC/s) will mis-attribute.

## Fixtures / coverage

No behavioural change → no new fixture. Correctness gate is selfhost
byte-id (output identical) + tier0. The proto-rename memo is a pure
refactor of an existing pass already covered by the protocol test suite
(`test-protocols`, `test-proto-scalar-dispatch`).

## Follow-ups for next lanes

- TyEnv per-target candidate index (the remaining quadratic). Profiling
  attribution and the index shape are written above; it is the single
  largest accidental cost left in the front-end.
- A real per-pass wall-timer in the driver would make the next throughput
  lane a measurement instead of a subtraction.
