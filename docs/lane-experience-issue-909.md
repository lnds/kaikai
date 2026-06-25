# Lane experience — issue #909: memoise `fns_prefer_module` in the native/KIR lowering

## Scope as planned vs as shipped

Planned: mirror PR #907 (which memoised the per-fn-body `fns_prefer_module`
rotation on the C-direct emitter, cutting self-compile RSS 9.34→5.25 GB) on
the native/KIR lowering path, where the same un-memoised call lived in
`ls_enter_fn_in` (`kir_lower.kai`) and was paid once per fn body (~5400 bodies),
each rebuilding the full ~5400-entry EFn table.

Shipped: exactly that, with no scope drift. The `FnsByMod` cache and
`fns_prefer_module_cached` from #907 already lived in `emit_shared.kai`; this
lane only threaded them through the native lowering loop.

## How the cache was threaded into the lowering

The C oracle keeps the cache OUT of `EmitCtx` — `fn_bodies_loop` carries it as
an extra parameter, seeding `fns_by_mod_new(cx.fns)` once in `fn_bodies` and
threading the grown `r.cache` to the next decl. The native path's equivalent
loop is `lower_fns`. Mirroring the oracle exactly:

- `lower_to_kir` seeds `fns_by_mod_new(fns)` and passes it to `lower_fns`.
- `lower_fns` gained an `fbm: FnsByMod` parameter. For each `DFn` it does
  `let r = fns_prefer_module_cached(fbm, mo)`, passes `r.table` (the rotated
  table) to `lower_fn`, and recurses with `r.cache`.
- `lower_fn` now takes the already-rotated `body_fns: [EFn]` and enters via a
  new `ls_enter_fn_with(st0, sym, locals, body_fns)` that swaps `fns: body_fns`
  in instead of recomputing `fns_prefer_module(st.fns, mo)`. `mo` is still
  threaded (it keys `c_sym` / `lookup_ufn_sig` / `ffi_wrapper_sym`).

`ls_enter_fn_in` had a single caller, so it was replaced outright by
`ls_enter_fn_with` rather than kept alongside.

## Design decision: cache outside `LowerSt`, not inside it

The obvious-looking move — add an `fbm` field to `LowerSt` — was rejected.
`LowerSt` is a 16-field record reconstructed at ~14 sites in `kir_lower.kai`;
adding a field touches every one, and the cache is not per-function state (it
is invariant across the whole run, like the C path's loop-local cache). The C
oracle deliberately keeps it out of `EmitCtx` for the same reason. Threading it
as a `lower_fns` parameter is the minimal, faithful mirror: two files touched,
no constructor churn.

## Structural surprise the brief did not anticipate

The brief said to measure RSS on "the native `kaic2` self-compile compiling
main.kai". The trap: `./kaic2 main.kai` with NO backend flag emits **C-direct**
to stdout (`emit_c.kai`, which already has #907's cache) — it does NOT exercise
the KIR lowering at all. Measuring that path showed ~5.9 GB before AND after,
i.e. no change, because the change is dead code on that path. The native path
must be invoked with `--backend=native` / `KAI_BACKEND=native`, which routes
through `lower_to_kir` (where the change lives).

A second surprise: the native backend cannot yet fully self-compile the
compiler bundle — it aborts with "unsupported KIR node (subset gap)" after the
lowering+codegen phase (a pre-existing subset gap, not introduced here). The
RSS comparison is still honest: both baseline and changed runs reach the
identical failure point (25 identical subset-gap nodes), so the measured peak
is the lowering+codegen phase the change actually optimises.

## RSS measurement

`KAI_MAX_HEAP=14g KAI_BACKEND=native /usr/bin/time -l ./bin/kai build
stage2/main.kai -o /tmp/out`, max RSS:

| | Max RSS |
|---|---|
| Baseline (no memoisation) | 9,245,999,104 B ≈ 8.61 GB |
| With memoisation | 6,157,680,640 B ≈ 5.73 GB |
| Reduction | −3.09 GB (−33%) |

Comparable to the C path's #907 win (9.34→5.25 GB). Both runs failed at the
same pre-existing subset gap.

## Correctness gate

`make selfhost` byte-id stayed green (`kaic2b.c == kaic2c.c`): the C-direct
output is unchanged and deterministic — the rotated table the cache returns is
byte-identical to the per-body rebuild (the cache base `fns_by_mod_new(fns)` is
the same `fns` `ls_new` stored, never mutated). Serial native-vs-C parity
(`BACKEND_PARITY_JOBS=1`) was run as the native-path correctness gate.

## Fixtures / coverage

No new fixture: this is a pure memory/perf fix with no behaviour change. The
selfhost byte-id gate (output unchanged) plus serial native-vs-C parity are the
correctness oracles, per the brief.

## Cost vs estimate

Mechanical, as the brief said — the cache infrastructure was prebuilt. The only
non-trivial time went to discovering that `./kaic2 main.kai` measures the wrong
(C-direct) path; once `--backend=native` was used the win was immediate.

## Follow-ups left for next lanes

- The native backend's compiler-bundle subset gaps (`unbound register …`) are
  unrelated and out of scope.
- Issue #910 (the ~38M emit retention) is a separate lane, untouched here.
