# Lane experience — issue-293-phase2-rc-discards

Phase 2 of #293: emit-side RC discipline for **discard** sites where
values are produced but never consumed by the body. Targets the four
shapes the Phase 1 retro identified (`docs/lane-experience-issue-293-phase1-rc-record-variant.md`
§*Limitations / next-lane scope*):

1. `SExprStmt` (semicolon-terminated expr stmt)
2. `let _ = expr` (PWild discard)
3. cons unused-binder (`[_h, ...t]` / `[_, ...t]`)
4. variant unused-payload (`Some(_x) -> ...`, `Some(_) -> ...`)

## Objective metrics

| Metric                        | Value                                |
|-------------------------------|--------------------------------------|
| Branch                        | `issue-293-phase2-rc-discards`       |
| Base commit                   | `67ed1e4` (#294 Phase 1 merge)       |
| Files touched                 | 4 (`stage1/compiler.kai`, `stage2/compiler.kai`, fixture + golden) |
| Net additions                 | ~70 LoC across both stages (`name_is_unused` helper + emit-site edits) |
| Tier 0                        | OK (~70 s)                           |
| Tier 1                        | Same ambient `signal-trap` rc=143 as Phase 1 / `main` |
| Tier 1-ASAN                   | Same ambient `signal-trap-asan` rc=143 as Phase 1 / `main` |
| Selfhost byte-identical (C)   | Yes, first iteration                 |
| Selfhost byte-identical (LLVM)| Yes, first iteration                 |
| Wall-clock spent              | ~1 h                                 |
| Calibrated estimate           | 3–5 h                                |

## Per-tag baseline (Phase 1 final) vs after Phase 2

(kaic2 self-compile, final FP iter, `-DKAI_TRACE_RC=1`.)

| Tag     | P1 final live | P2 final live | Δ live    | leak% before | leak% after |
|---------|--------------:|--------------:|----------:|-------------:|------------:|
| int     |    557 342    |     557 561   |   +0.04%  |        12.0% |       12.0% |
| str     |  1 962 609    |   1 963 166   |   +0.03%  |         5.4% |        5.4% |
| cons    |  4 617 658    |   4 574 013   |   **−0.94%** |     56.5% |       56.0% |
| record  |  6 787 699    |   6 790 097   |   +0.04%  |        75.3% |       75.3% |
| variant | 64 057 866    |  64 056 218   |   −0.003% |        82.5% |       82.5% |
| closure |    483 717    |     483 925   |   +0.04%  |        27.9% |       27.9% |
| array   |      8 271    |       8 274   |   +0.04%  |         4.3% |        4.3% |

`cons` improves modestly (−44k live, ≈1%); the other tags are within
noise. The fixture itself (`examples/perceus/rc_discipline_discards.kai`)
is RC-balanced at `live=0` per tag under `KAI_TRACE_RC=1`, confirming
the four named shapes are now correct on the small case. The kaic2
self-compile residual lives elsewhere.

**Phase 2 acceptance gate (record ≤30%, variant ≤50%, cons ≤25%) is
NOT met.** See *Limitations* for the diagnosis.

## The four sub-phases

### Phase 2.1 — SExprStmt + `let _ = expr`

Pre-fix:

```c
{ KaiValue *_ = <expr>; (void) _; }              /* SExprStmt */
{ KaiValue *_w = <expr>; (void) _w; }            /* let _ */
```

After:

```c
{ KaiValue *_ = <expr>; kai_decref(_); }
{ KaiValue *_w = <expr>; kai_decref(_w); }
```

Applied unconditionally — `emit_expr` always returns an owned ref
because `perceus_pass` dups every non-last EVar read; the discard
site is therefore always safe to decref. The stage 2 emitter
previously gated on `is_fresh_alloc` (only decref for ECall /
literals); the gate was overly conservative. Same fix in the LLVM
backend's `SExprStmt` and `PWild`-let arms so both backends agree
on the discard convention.

**Per-tag delta vs Phase 1 baseline:** small (−44k cons, −44k
variant). SExprStmt sites are sparse in kaic2 source.

### Phase 2.2 — EBlock final-expression discarded

**Subsumed by Phase 2.1.** When an EBlock appears as the rhs of an
SExprStmt or `let _ =`, the wrapping `kai_decref` from Phase 2.1
captures the block's final expression (returned through the
`({ ... })` GNU statement-expression). No separate fix required.

### Phase 2.3 — cons unused-binder decref

Helper `name_is_unused(name)`: true iff `name` starts with `_` and
the second char is **not** `_` (the `__` prefix is reserved for
compiler-internal binders like `__cv_*` / `__pcs_ret` /
`__perceus_*`).

`emit_pat_binds` updated:

- `PWild` with `is_alias=false` (record field via `kai_op_field`,
  destructuring-let composite): emit `kai_decref(scr);` so the
  owned ref is released.
- `PWild` with `is_alias=true` (cons head/tail, variant arg, match
  scrutinee): keep returning `""` — the parent's decref reclaims
  the alias via the runtime's transitive cons/variant free.
- `PBind(name)` with `name_is_unused(name)`: same treatment as
  `PWild` — skip the incref on alias paths, decref on owned paths.

This keeps the established RC convention intact: aliases ride the
parent; owned refs are released at the discard site.

**Per-tag delta:** cons live 4 617 658 → 4 574 013 (≈1% improvement).
Smaller than expected because most kaic2 cons spines are walked by
length-style functions that read `t` — the head is often referenced,
so `_h` patterns are rare.

### Phase 2.4 — variant unused-payload decref

**Subsumed by Phase 2.3.** Variant pattern arms route through the
same unified `emit_pat_binds`, so `Some(_x) -> ...` and
`Some(_) -> ...` already get the new behaviour.

**Per-tag delta:** variant live ≈unchanged. Variant payloads are
overwhelmingly *consumed* in match arms that name them; the
`_`-discard pattern is rare in kaic2 source.

### Phase 2.5 — selfhost convergence

Both backends reach byte-identical fixed point on the first iteration
of `make selfhost` and `make selfhost-llvm`. No emit-shape divergence.

### Phase 2.6 — fixture

`examples/perceus/rc_discipline_discards.kai` exercises:

- `make_box(1);` SExprStmt
- `let _ = make_box(2)` PWild let
- `let _ = make_pair(3, 4)` PWild let with variant payload
- `[_, ...t]` cons unused-binder
- `Pair(_a, _b) -> 2` and `Single(_x) -> 1` variant unused-payloads

Output `6` matches `rc_discipline_discards.out.expected`. Built with
`CFLAGS=-DKAI_TRACE_RC=1`:

```
[KAI_TRACE_RC] STRICT alloc_total=9 free_total=9 leaked=0 live_peak=4
[KAI_TRACE_RC] tag=str     allocs=1 frees=1 live=0
[KAI_TRACE_RC] tag=cons    allocs=4 frees=4 live=0
[KAI_TRACE_RC] tag=record  allocs=2 frees=2 live=0
[KAI_TRACE_RC] tag=variant allocs=2 frees=2 live=0
```

Every tag is `live=0` — the four named shapes are RC-balanced
end-to-end on the small fixture.

## Friction points

- **`name_is_unused` first cut included `__`-prefixed names.** That
  broke `m12_6_const_pattern` because the const-pattern desugar plants
  `__cv_<NAME>` binders that the body still references. Refined the
  predicate to require char[0]=='_' AND char[1]!='_'. The fix lands
  fast because tier1 surfaces the C-compile error directly.

- **The brief's prediction did not match the empirical truth (again).**
  The brief assumed `_`-prefixed binders dominated the leaked variant
  and cons allocations. Empirically, kaic2 source uses `_` (PWild) far
  more often than `_x`-named PBind, and the cons spines are rarely
  walked with `_h` heads. The Phase-2.3/2.4 fixes are *correct*
  (proven by the fixture's `live=0` per-tag report) but address shapes
  that hardly appear at scale in the self-compile.

- **Selfhost remains byte-identical despite the discard changes.**
  Confirms that the `(void) _` → `kai_decref(_)` substitution does
  not alter the typed-AST output — both kaic2 binaries emit the same
  C/LLVM. This is reassuring (no aliasing surprise) but also reflects
  that the change touches only the C output text shape, not the
  emitter's structural decisions.

## Empirical verification

- Fixture: `examples/perceus/rc_discipline_discards.kai` passes
  (output `6`); `KAI_TRACE_RC=1` build shows `live=0` per tag.
- Selfhost C: byte-identical, first iteration.
- Selfhost LLVM: byte-identical, first iteration.
- Tier 0: 23 OK, 3 no-golden, 2 pre-existing failures (forth /
  mini_ledger), demos baseline 26 holds.
- Tier 1: same `test-effects` rc=143 boundary as Phase 1 / `main`
  (ambient signal-trap; not a lane regression).
- Tier 1-ASAN: same `test-signal-trap-asan` rc=143 boundary
  (ambient).

## Limitations / next-lane scope

Phase 2 acceptance gate (record ≤30%, variant ≤50%, cons ≤25%) is
**not** met. Per-tag movement is small because the leak does not sit
where the brief framed it.

The four named shapes are now provably correct (fixture `live=0`),
but the kaic2 self-compile residual is dominated by other paths:

1. **Most variant constructions feed match arms that consume them
   correctly**, so Phase 2.4 finds little to fix.
2. **Most cons spines are walked with `t` referenced**, so the
   `_h` discard fix in Phase 2.3 only catches a thin slice.
3. **The 64 M-allocation variant tag** is dominated by ctor calls
   (especially `None`/`Nil`-shaped nullary variants) whose product
   does eventually feed a match. The leak is somewhere along the
   *chain* — the brief grouped this as Phase 3 (primitives + str
   intermediates), but the same diagnosis applies: the leak is
   likely in EVar / EField pass-through paths under the perceus
   convention rather than at the discard.

The *honest* Phase 3 scope, given Phase 1 and Phase 2 evidence,
should be:

- Audit perceus_pass's last-use convention for **transitive
  pass-through** (function-arg owned ref, lambda capture, EBlock
  final → consumer). The 80 M-leak figure cannot all be discard
  sites; it must include passes where the producer's owned ref is
  not transferred.
- Audit closure capture lifecycle (`as.clo.captures[i]` allocated
  but not always freed when the closure dies).
- Primitive boxing / str intermediate buffers (the brief's original
  Phase 3 scope) — keep that, but it is not the dominant leak.

Phase 2 leaves the four named shapes correctly balanced and adds the
`name_is_unused` helper that future passes can reuse.

## Subjective summary

Same arc as Phase 1: the named scope is implemented cleanly and
proven correct on a fixture, but the umbrella acceptance gate does
not move because the leak lives elsewhere. Two lanes' worth of
empirical work now point to the same diagnosis: the residual is in
the pass-through plumbing of perceus_pass + emit_expr, not at the
discard site.

The lane is short (~1 h) and the changes are minimal. The cleanest
path forward is to instrument perceus_pass (or the runtime) to
attribute leaks to a *site* rather than a tag — once the call site
of a leaked variant is known, Phase 3 can target it.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-06T10:05:10-04:00	lane-start	OK	-
2026-05-06T10:08:30-04:00	tier0-baseline	OK	70
2026-05-06T10:11:00-04:00	selfhost-trace-baseline	OK	60
2026-05-06T10:14:00-04:00	tier0-2.1	OK	70
2026-05-06T10:17:00-04:00	selfhost-trace-2.1	OK	60
2026-05-06T10:21:00-04:00	tier0-2.3	OK	70
2026-05-06T10:25:00-04:00	selfhost-trace-2.3	OK	60
2026-05-06T10:29:00-04:00	tier1-2.3	FAIL_compile	60
2026-05-06T10:31:00-04:00	tier0-2.3-fixed	OK	70
2026-05-06T10:35:00-04:00	tier1-2.3-fixed	FAIL_143	160
2026-05-06T10:39:00-04:00	tier1-asan-2.3-fixed	FAIL_143	250
2026-05-06T10:44:00-04:00	selfhost-llvm	OK	50
2026-05-06T10:47:00-04:00	selfhost-c	OK	50
```

Lane span: 2026-05-06T10:05:10−04:00 → end-of-lane.
