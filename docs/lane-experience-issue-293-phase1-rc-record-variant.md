# Lane experience — issue-293-phase1-rc-record-variant

Phase 1 of #293: emit-side RC discipline for `record` and `variant`,
the two dominant offenders in the kaic2 self-compile leak audit
(98.5% / 82.7% leak rate at baseline per #291 Track #2).

## Objective metrics

| Metric                       | Value                                |
|------------------------------|--------------------------------------|
| Branch                       | `issue-293-phase1-rc-record-variant` |
| Base commit                  | `7a88b7f` (#292 KAI_TRACE_RC merge)  |
| Files touched                | 4 (`stage1/compiler.kai`, `stage2/compiler.kai`, fixture + golden) |
| Net additions                | ~50 LoC across emit + 1 helper fn (`pat_is_destructuring`) per stage |
| Tier 0                       | OK (~70 s)                           |
| Tier 1-ASAN                  | OK on every fixture **except** `signal-trap-asan` which also fails on `main` (ambient) |
| Tier 1                       | Same — `test-effects` exits with rc=143 on the `signal-trap`/`m12_8_y_phase4_*` boundary on `main` and on the lane (ambient) |
| Selfhost byte-identical (C)  | Yes, first iteration                 |
| Selfhost byte-identical (LLVM)| Yes, first iteration                 |
| Wall-clock spent             | ~1 h 30 m                            |
| Calibrated estimate          | 8–15 h                               |

## Per-tag baseline vs after (kaic2 self-compile, final FP iter)

| Tag     | baseline live | after live  | Δ live   | leak% before | leak% after |
|---------|--------------:|------------:|---------:|-------------:|------------:|
| int     |  1 924 253    |    557 342  |   −71.0% |        41.5% |       12.0% |
| str     |  1 961 585    |  1 962 609  |   +0.05% |         5.4% |        5.4% |
| cons    |  4 634 767    |  4 617 658  |    −0.4% |        56.9% |       56.5% |
| **record** |  **8 871 959** |  **6 787 699** | **−23.5%** | **98.5%** | **75.3%** |
| **variant**|  64 093 266  | 64 057 866  |  −0.06%  |        82.7% |       82.5% |
| closure |    483 436    |    483 717  |   +0.06% |        27.9% |       27.9% |
| array   |     20 787    |      8 271  |   −60.2% |        10.9% |        4.3% |

`record` improves materially (24%); `int` and `array` improve as
side effects of the EField fix (the field-typed children no longer
ride the leaked record). `variant` barely moves: the residual is
not on the construction / destructure shapes Phase 1 names.

**Phase 1 acceptance gate (record ≤5%, variant ≤10%) is NOT met.**
The structural-leak audit underestimated where the residual sits.
See *Limitations* for the diagnosis and the next-lane scope.

## The five sub-phases

### Phase 1.1 — record literal construction (no change)

The runtime convention (`stage0/runtime.h:1163`) is that
`kai_record(n, fields, names)` *consumes* its children — it stores
them into the new record without incref'ing. Each field arrives
already owned because `perceus_pass` wraps non-last reads of
in-scope EVar in `__perceus_dup` and the last read transfers raw,
so the construction site is already balanced.

Adding incref's at the literal would *break* the balance (extra
ref over the consumed one). No code change needed.

### Phase 1.2 — record field access (the EField fix)

Pre-fix:

```c
EField(base, fname) → kai_op_field(base_c, "fname")
```

`kai_op_field` increfs the field but never drops the base. Every
record temporary that fed a field access leaked. With the fix:

```c
EField(base, fname) →
  ({ KaiValue *_b = base_c;
     KaiValue *_f = kai_op_field(_b, "fname");
     kai_decref(_b); _f; })
```

Applied to both `stage1/compiler.kai` and `stage2/compiler.kai`.

**Empirical delta**: `record` 8.87M → 6.78M live (−23.5%);
`int` 1.92M → 0.56M live (−71%) as the field-typed primitives
no longer ride the leaked records.

### Phase 1.3 — destructuring let + nested record destructuring

Two related fixes in one phase.

(a) Destructuring `let { x, y } = r` previously bound a `_letv`
temp and never released it. With the fix the binders use
`is_alias=true` (so each `PBind` increfs through `kai_op_field`
that already increfs internally), and a trailing
`kai_decref(_letv)` releases the composite.

(b) `emit_pat_binds_record`'s textual `kai_op_field(scr, "name")`
expansion for the sub-pattern leaked the intermediate composite
when the sub-pattern was itself destructuring (nested PRecord /
PVariant / PVariantRecord / PList). Fix: lift the field read into
a uniquely-named C temp and drop it after the inner binders own
their slices. PBind / PWild / PLit sub-patterns keep the inline
form so the emitted C does not gain a temp it does not need.

**Empirical delta**: marginal (+0.05M record frees) — destructuring
let with nested patterns is rare in kaic2 source.

### Phase 1.4 — variant ctor + destructure (no change)

Same convention as record literal: `kai_variant(tag, name, n, args)`
consumes its `args[]`. The destructure path in match arms already
increfs via `is_alias=true` in `emit_pat_binds_variant` and the
trailing `kai_decref(_scr)` at match exit balances. Nullary
variants (`Nil`, `None`, user-defined) allocate fresh per use —
they are not pooled — but the consumer still sees them owned.

No change needed for the construction shape.

### Phase 1.5 — selfhost convergence

Both backends reach byte-identical fixed point on the first
iteration. No emit-shape divergence between successive kaic2
binaries.

### Phase 1.6 — fixture

`examples/perceus/rc_discipline_record_variant.kai` exercises
record literal + field access + nested record destructuring +
variant ctor + variant destructure. Under `KAI_TRACE_RC=1`:

```
[KAI_TRACE_RC] STRICT alloc_total=7 free_total=7 leaked=0 live_peak=3
[KAI_TRACE_RC] tag=str     allocs=2 frees=2 live=0
[KAI_TRACE_RC] tag=record  allocs=3 frees=3 live=0
[KAI_TRACE_RC] tag=variant allocs=2 frees=2 live=0
```

Every tag is `live=0` at exit — the four shapes are RC-balanced
end-to-end on the small fixture even though the kaic2 self-compile
still leaks via *other* shapes.

## Friction points

- **The brief's prediction did not match the empirical truth.**
  The brief assumed Phase 1.1 (record literal) and Phase 1.4
  (variant ctor) would need explicit incref's at construction. The
  runtime convention is the inverse — the ctors *consume* their
  children — and `perceus_pass` already arranges owned passing
  via `__perceus_dup`. Adding incref's there would have broken
  balance, not fixed it. Half of the named scope produced no code
  change; the other half (EField, destructuring let, nested
  destructure) carried the entire 24% record reduction.

- **`tier1` and `tier1-asan` exit with rc=143 on `main` too.** The
  failing point is `test-signal-trap-asan` (and a similar boundary
  inside `test-effects`); both are sensitive to the host's signal
  delivery and abort with SIGTERM during `make` orchestration. The
  fixtures themselves print `OK` before `make` reports the error.
  Verified by stash + re-run on `main`: same exit. Not a lane
  regression.

- **The 0.6× calibration was way too generous.** Estimate was
  8–15 h; actual was ~1 h 30 m. The diagnosis pivoted fast once
  the first sub-phase landed (EField + verify table); the
  remaining sub-phases were either no-ops (1.1, 1.4) or one-line
  edits (1.3 (a), (b)) that took minutes. The
  acceptance-gate gap (variant still 82%) is a *scope* problem,
  not an effort problem.

## Empirical verification

- Fixture: `examples/perceus/rc_discipline_record_variant.kai`
  passes (output `38`); `KAI_TRACE_RC=1` build shows `live=0`
  per tag.
- Selfhost C: byte-identical, first iteration.
- Selfhost LLVM: byte-identical, first iteration.
- Tier 0: 23 OK, 3 no-golden, 2 pre-existing failures (forth /
  mini_ledger), demos baseline 26 holds.
- Tier 1-ASAN: clean on every fixture except the pre-existing
  signal-trap one.

## Limitations / next-lane scope

The acceptance gate (record ≤5%, variant ≤10%) is **not** met.

After Phase 1, the residual record leak (75% of allocs) and the
~unchanged variant leak (82.5%) come from shapes outside the
brief's named sub-phases:

1. **`SExprStmt` and `let _ = expr` discards** — `{ KaiValue *_ =
   <expr>; (void) _; }` does not decref. I prototyped the fix
   (`kai_decref(_)`); it built and selfhosted byte-identical, but
   the per-tag deltas were within noise. The shape is uncommon in
   kaic2 source (`grep` confirms a few dozen sites). Reverted.

2. **Function-call results discarded as the final expression of
   an EBlock chain.** `({ s1; s2; finalexpr; })` — when the block
   is in a context that consumes the value the final expr is
   transferred; when it is dropped (e.g. in a `let _ = (...)` or
   an SExprStmt block), the residual leaks.

3. **List literals desugared to `Cons(a, Cons(b, ..., Nil))`
   chains that are passed through transitive consumers.** `cons`
   leak rate (56.5%) is the second biggest tag share by raw count
   after variant (and the "Cons" variants ARE in the variant tag
   for non-list disjoint-union usage). The chains build correctly
   but the consumers' RC discipline (e.g., `length(xs)` walking
   without keeping the spine alive) is not always tail-balanced.

4. **Variant payloads built fresh and immediately discarded.**
   `tag=variant` allocations dominate (77.5M / run, 17M consumed,
   60M leaked). The construction site is balanced; the discard
   site is not. This is the same root cause as #2 but on a
   different tag.

The next lane is Phase 2 of #293 (cons + closure RC discipline).
After Phase 2 the `SExprStmt` / discard pattern becomes the
remaining structural lie and is the right Phase 3 target — the
brief grouped that under "primitives" but in practice the discard
pattern is tag-agnostic.

## Subjective summary

The hardest part of the lane was understanding the runtime
convention (`kai_record` *consumes*, doesn't *copy*) and reading
`perceus_pass` carefully enough to trust that field passing is
already owned. Once that landed, the EField fix was four lines
and produced 24% of the eventual record reduction.

The lane delivers on Phase 1's *named scope* — the four shapes
are now RC-balanced on the fixture. It does *not* meet the
umbrella acceptance gate for record/variant leak rate. The honest
read is that the brief's framing of "Phase 1 = record + variant
ctor and destructure" undershoots the work needed to actually
move those tags' leak rates: most of the remaining leak is in
the discard / pass-through plumbing that the brief deferred to
later phases. Phase 2 + a Phase-3 expansion to cover discards is
where the umbrella gate gets met.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-06T09:02:01-04:00	selfhost-trace-baseline	OK	48
2026-05-06T09:06:55-04:00	selfhost-trace-1.2	OK	36
2026-05-06T09:12:16-04:00	selfhost-trace-1.3	OK	38
2026-05-06T09:14:00-04:00	tier1-asan-1.3	OK	56
2026-05-06T09:16:53-04:00	tier1-1.3	FAIL_143	166
2026-05-06T09:33:27-04:00	selfhost-llvm-1.3	OK	50
2026-05-06T09:43:53-04:00	selfhost-trace-1.4	OK	37
```

Lane span: 2026-05-06T08:59:52−04:00 → end-of-lane.
