# Lane experience — issue #910 (emit-phase retention profile)

Measurement-first lane. Goal: find what the emit phase keeps live to
end-of-run (the #907/#911 follow-up reported ~38M objects added by emit,
live_peak peaking at run end) and reduce it, pushing self-compile RSS
toward ~4 GB. Diagnosis was the primary deliverable; a clean fix was
allowed but not forced.

## Re-measured mono/emit split (HEAD `abb1cf07`, after #914/#915/#916)

Method: `KAI_TRACE_RC=1` self-compile (`./kaic2 main.kai`), plus an A/B
gate that returns before `emit_program` (env-free driver edit, reverted
after measuring) to isolate emit's contribution to `live_peak`.

| | live_peak | RSS | alloc_total |
|---|---|---|---|
| Full (with emit) | 99.72M | 6.42 GB | 271.0M |
| No-emit (stop after perceus) | 69.73M | 4.76 GB | 227.1M |
| **Δ EMIT** | **+30.0M** | **+1.66 GB** | **+43.9M** |

So the original 38M had shifted to **~30M** after the velocity lands.
Mono+perceus retains 69.7M; emit adds 30M live that survives to the end.

Intra-`fn_bodies` markers showed the emit growth is **linear in the
number of `DFn` emitted** (~2.6M net per 2000 fns), not stepped — which
ruled out the `FnsByMod` cache (that would step once per module) and
pointed at a per-decl/per-call-site retainer.

## The localised structure

Leaksite tracer (`-DKAI_TRACE_RC -DKAI_TRACE_RC_LEAKSITE`, attributing
each leaked chunk to its `(scope_fn, tag)`), diffed full vs no-emit,
named the emit retainer precisely:

- `rprelude_table()` (`native_prims.kai`): variant+cons leak 1.83M
  (no-emit) → 18.74M (full). **Δ emit ≈ +16.9M**, frees ≈ 14K against
  millions of allocs.
- `prelude_table()` (`emit_c.kai`): **Δ emit ≈ +5.8M**.
- `list_has` on the emit path: **+4.3M**.

Root cause: `prelude_find(target) = prelude_find_loop(prelude_table(),
target)` and `rprelude_find(target) = rprelude_find_loop(rprelude_table(),
target)` each **rebuild a whole list literal** (93 `EP` / 91 `RP`
entries — the `RP` carries a `[Ty]` per entry, so it is heavier) **on
every call**. They are consulted per emitted callee/ident (`emit_ident_value`,
`emit_named_call_lookup`, `emit_pipe_named_lookup`, `emit_kind_raw`).
Self-compile has ~100K+ call sites → the table is materialised that many
times and the spine is not freed after the lookup, so it stays live to
end of run. Retention is quadratic-ish in fn count.

A defining check: the #1 leaker overall, `ty_env_strip_qualified_prefix`
(21.75M str), is **identical between full and no-emit** — it is a
typer/TyEnv cost, not emit. Easy to misattribute without the A/B split.

## The fix (shipped)

Pattern the repo already blessed for `fns_prefer_module` → `FnsByMod`:
materialise the table once and thread it, read borrowed.

- Two `EmitCtx` fields `prelude: [EPrelude]`, `rprelude: [RPrelude]`,
  built once at the two emit entry points (`emit_program`,
  `emit_program_modular`) and passed to the lambda-helper emitter too.
- `prelude_find` / `rprelude_find` now take the table and walk it
  borrowed (the `evar_find` idiom): the match reads each field and
  copies the hit (`Some(EP(name, cname, ar))`) instead of returning the
  shared spine node, so the threaded table stays live and unmutated
  across every call site — no rebuild, no RC churn on the shared table.
- `emit_ident_callee` was dead (no callers) — removed.
- `unbox.kai`'s `call_is_raw_prelude` kept its inline `rprelude_table()`
  rebuild (it is pre-emit, never showed as a dominant leaker, and
  threading the table through the unbox mode-decider chain is out of the
  measured scope — neutral, not a regression).

### Result (clean, no instrumentation)

| | Before | After | Δ |
|---|---|---|---|
| live_peak | 99.72M | 76.99M | **−22.7M** |
| RSS | 6.42 GB | 5.29 GB | **−1.13 GB** |
| alloc_total | 271.0M | 248.1M | −22.9M |

`make selfhost` byte-id OK (`kaic2b.c == kaic2c.c`); `make tier0` green
(selfhost deterministic, 33 fixtures, demos baseline, arena + heap
gates). Output is functionally identical; the only `.c` diff is the
compiler's own changed function signatures (expected — it compiles
itself).

## Surprises / scope decisions

- The emit growth being *linear* (not stepped) was the key
  discriminator: it killed the obvious `FnsByMod`-style suspicion and
  forced the leaksite pass, which is what actually named the table.
- The dominant overall leaker (`ty_env_strip_qualified_prefix`) is a
  red herring for *this* lane — it is pre-emit. The A/B gate is what
  separated it cleanly; the aggregate leaksite table alone would have
  pointed the wrong way.
- RSS landed at ~5.29 GB, not ~4 GB. The remaining 76.99M live is
  mono+perceus territory (the 69.7M floor plus a residual emit copy),
  a different axis than this lane.

## Follow-up left for a next lane

The leaksite frees≈0 on `prelude_find_loop` / `list_has` is a real
RC bug-class, not just rebuild churn: a list loop of the shape
`match xs { [h, ...t] -> if cond { Some(h) } else { recurse(t) } }`
does not free the un-selected tail in the non-recursive arm when `xs`
is a temporary. This lane's fix removes the *amplification* (the table
is no longer rebuilt per call site, so the bug retains a handful of
nodes once instead of millions), but the underlying shape will bite any
future `find`-over-temporary-list. Same family as the move-in-last-use
gap. Worth a dedicated regression fixture in `examples/perceus/` and a
typer/perceus fix; verifiable with the leaksite tracer on an isolated
loop.
