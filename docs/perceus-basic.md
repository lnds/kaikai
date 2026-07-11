# perceus-basic — m5 status, baseline, deferred work

Stage 2 milestone m5 is "basic Perceus": a typed-IR pass that walks
the post-monomorphisation AST and inserts `dup` / `drop` operations
based on use analysis. This document pins the **current state** of
that work — what the codebase actually does today, what landed under
m5 #0, and what stays deferred.

## TL;DR

- m5 as originally framed (a typed-IR pass that reuses last-use
  references and inserts drops) presupposes infrastructure that is
  not yet in place: there is no post-monomorph typed IR distinct
  from the source AST (m4c was deferred), and the emitter does not
  currently follow a reference-counting discipline that a use-
  analysis pass could optimise.
- m5 #0 lands **measurement infrastructure** (`KAI_TRACE_RC` env
  var) and pins the baseline numbers below.
- m5 round 2 (2026-04-26) lands a typed-AST walker for fn-param
  last-use analysis (`scan_uses_expr` + `last_use_for`,
  diagnosable via `--dump-last-use`) and a conservative drop pass
  for unused `let` bindings whose RHS is freshly allocated. Bench
  fixture `stage2/bench/m5_unused_lets.kai` shows ~24× more frees
  and ~7× lower leak rate on a focused workload.
- m5 #7 (constant pool for `unit` / `bool` / `nil`) closes out
  round 2 and **satisfies the brief's bench-gating clause**: kaic2
  self-compile alloc count drops from 130,735,107 (pre-m5) to
  29,533,152 — a **77.4% reduction** with no emitter changes, only
  a runtime swap to shared static singletons.
- m5 #9 (round 3, 2026-04-26) lands the dup/drop infrastructure +
  scope-aware AST rewrite (steps 1+2 of 4). Step 3 (uniformising
  runtime primitives to consume their args linearly) is **blocked
  on stage 1 also needing a perceus pass**: stage 1's compiler.kai
  has no dups, so flipping the runtime to consume mid-flight
  produces use-after-free in stage 1 selfhost. The dup/drop
  infrastructure is staged in tree but inert against the loose
  runtime; it ships as scaffolding for a future stage-1 + runtime
  flip. See "What landed under m5 #9 (round 3)".
- Everything beyond — capture-aware drops, dup at non-last uses,
  drops at fn exit points, drop specialisation, reuse-in-place —
  stays deferred to a future m5 lane, scoped honestly as multi-
  week.
- The single fact every future m5 work item must reckon with: the
  remaining ~29M allocations per self-compile **still leak almost
  in full**. Memory beyond the singletons is reclaimed only when
  the process exits and the runtime's recursive `kai_decref` chain
  runs over the final result. This is not a Perceus inefficiency
  that needs tightening; it is the absence of a Perceus discipline.

## What the emitter does today

The emitter (`stage2/compiler.kai`, ~14k lines) walks the source
AST after type inference and emits portable C against
`stage0/runtime.h`. The runtime is uniform-boxed: every value is a
heap-allocated `KaiValue *` with an `rc` field, an `incref`
that bumps it, and a `decref` that decrements and chain-frees on
zero.

The runtime has the machinery; the emitter barely uses it:

- **One** call to `kai_incref` is emitted, only for the spread tail
  in list literals (`[..., ...xs]`). See `compiler.kai:4989` —
  needed because the new cons takes ownership of `xs`'s tail and
  the source binding `xs` may still be referenced afterwards.
- **Two** call sites emit `kai_decref`: the `_body` value of a
  `test "..." { ... }` block (line 6356) and the final `_result`
  returned by `kai_main` (line 6444). Both are at "obvious"
  program-end points.
- Function parameters, `let` bindings, match scrutinees, captured
  values inside closures — none of these are decref'd anywhere in
  the emitted code.

Constructors in the runtime — `kai_cons`, `kai_record`,
`kai_variant`, `kai_closure` — take ownership of their arguments
without bumping the refcount. When the container is freed, its
children are decref'd and chain-freed. The de facto model is
"linear consumption": a value should be used exactly once, and the
single user transfers ownership downstream. Violations of linearity
do not crash because almost nothing is ever freed — the violating
re-uses always observe the still-alive value.

This works because:

1. The kaikai compiler source itself, today, is highly disciplined
   about not re-using bindings post-call (the convention is enforced
   by hand because the compiler is its own selfhost target).
2. `kai_decref(_result)` at process exit recursively reclaims the
   transitive closure of everything reachable from the final result.

It works only because programs run to completion and exit. Long-
running fibers (m8) will break this model; the actor mailbox spec
(`docs/actors.md`) presumes a working RC discipline.

## Baseline measurements (m5 #0)

The runtime now ships always-on RC counters in `runtime.h`:

- `kai_rc_alloc_total` — every `kai_alloc` call bumps it.
- `kai_rc_free_total` — every `kai_free_value` (called from
  `kai_decref` when the rc reaches 0) bumps it.
- `kai_rc_live_now` — running difference; the in-flight set size.
- `kai_rc_live_peak` — high-water mark of `live_now`.
- `kai_rc_alloc_by_tag[]` — per-`KaiTag` allocation breakdown.

The exit-time report is gated on the env var `KAI_TRACE_RC`. The
silent path stays silent; the always-on tax is four counter
increments per `kai_alloc` and two per `kai_free_value`. Negligible
on the kaic2 self-compile (wall ~2 s, dominated by the typer + emit,
not by alloc bookkeeping) but measurable on tight allocation-heavy
inner loops: `bench-effects` (which alloc-pressures the effect path
~5 times per iteration) shifts from 7.85 ns/op to 8.04 ns/op
(+2.4 %) — the régime-fallback REGRESSION verdict pre-dates this
change and remains the active item under Doc C §*Open questions*
#6.

Measured 2026-04-25 on Apple Silicon (`darwin 25.4.0`, `clang
-std=c99 -O0 -g`):

### Self-host runs

| run                 | wall    | peak RSS | allocs       | freed      | leaked       | leak rate |
|---------------------|---------|----------|--------------|------------|--------------|-----------|
| `kaic1` on its src  | 0.35 s  | 1.02 GB  | 20,007,446   | 360,616    | 19,646,830   | 98.2 %    |
| `kaic2` on its src  | 2.15 s  | 6.25 GB  | 126,805,194  | 3,442,650  | 123,362,544  | 97.3 %    |

Reproduce with:

```sh
ulimit -s 32768
KAI_TRACE_RC=1 ./stage1/kaic1 stage1/compiler.kai > /dev/null
KAI_TRACE_RC=1 ./stage2/kaic2 stage2/compiler.kai > /dev/null
/usr/bin/time -l ./stage2/kaic2 stage2/compiler.kai > /dev/null
```

Note: `kai_rc_live_peak` equals `leaked` for every measured run
because nothing is freed mid-compile. `live_peak` becomes the
informative metric only after m5 proper inserts intermediate
`drop`s; today it is a tautology.

### Phase 4 demos

(Each row is the binary's allocation profile, not the kaic1
process that compiled it. `KAI_TRACE_RC=1 ./bin/kai run <demo>`
prints both — the first report is kaic1, the second is the demo.)

| demo        | allocs  | freed  | leaked  | leak rate |
|-------------|---------|--------|---------|-----------|
| `hello`     |       2 |      1 |       1 |    50.0 % |
| `factorials`|     407 |     68 |     339 |    83.3 % |
| `euler1`    |  10,400 |  1,000 |   9,400 |    90.4 % |
| `collatz`   |  34,245 |      1 |  34,244 |   99.99 % |

`hello` is the floor: a single string allocation plus the unit
return value. `collatz` shows the pure-functional ceiling: every
intermediate cons / int / bool computed during the iteration
sequence stays alive through the run, freed only at the final
`kai_decref(_result)` recursive sweep.

### Allocation shape (kaic2 self-compile)

Per-tag breakdown, dominant categories first:

| tag       | allocs       | share  |
|-----------|--------------|--------|
| `unit`    |  55,825,958  | 44.0 % |
| `bool`    |  43,605,184  | 34.4 % |
| `char`    |   8,095,951  |  6.4 % |
| `int`     |   7,615,374  |  6.0 % |
| `variant` |   5,756,008  |  4.5 % |
| `str`     |   2,473,507  |  2.0 % |
| `record`  |   1,633,351  |  1.3 % |
| `cons`    |   1,253,599  |  1.0 % |
| `nil`     |     441,558  |  0.3 % |
| `closure` |      63,277  |  0.05% |
| `array`   |      41,426  |  0.03% |

The dominance of `unit` and `bool` is striking. Every conditional
arm and every void-returning expression boxes a fresh `kai_unit()`
or `kai_bool()`. Constants that ought to be statically shared
(`unit`, `true`, `false`, `nil`) are reallocated millions of
times per compile. This is a candidate for an early m5 win — a
constant-pool optimisation in `runtime.h` that returns shared
references for the four nullary constructors would shrink the
allocation count by ~80% before any IR-level work happens. It
preserves the leak-everything semantics; it just allocates less.

## Why m5 cannot ship as a quick patch

The design-doc framing of m5 ("walk the typed AST after
monomorphisation; insert dup / drop based on use analysis")
assumes two things, both currently absent:

1. **A post-monomorphisation typed IR.** What landed under m4 is
   inference (m4a) and call-site instantiation collection (m4b).
   The actual specialiser that produces a typed IR with one
   concrete copy per generic instantiation — m4c — was deferred.
   `infer_program` returns the source `[Decl]` with `Expr.ty`
   populated; `emit_program` re-walks the same `[Decl]` and
   uniformly boxes everything. There is no IR for a Perceus pass
   to walk, separate from the source AST.

2. **An RC discipline already running, that an analysis pass could
   tighten.** As Section "What the emitter does today" laid out,
   the emitter does not follow an RC discipline at all — it leaks.
   Adding `drop`s at last-use positions is not a tightening of an
   existing pattern; it is the introduction of one. Every change to
   the emitter must reason about ownership transfers across all
   ~14k lines of generated calls. Without m4c there is also no IR
   layer to localise that reasoning.

## What landed under m5 #0

- `KAI_TRACE_RC` instrumentation in `stage0/runtime.h`. Counter
  storage, exit-time reporter, lazy registration via `atexit`
  hooked from `kai_set_args`. Both backends (C, LLVM) inherit it
  via the common header.
- The baseline numbers above, pinned in this doc.
- This document, as the honest scope marker for whoever picks up
  m5 next.

The runtime change is purely additive and does not affect emitted
text — the kaic2 self-host fixed point (`make -C stage2 selfhost`)
remains byte-identical.

## What landed under m5 (round 2, 2026-04-26)

A second m5 lane shipped nine commits over m5 #0. The brief's
sub-numbering (m5 #1-#6 = walker + last-use + drop + dup + bench +
docs) was retired during the work; the actual landings line up
with the doc's m5 sub-milestones below as follows:

| commit    | brief grain   | doc grain        | scope                                        |
|-----------|---------------|------------------|----------------------------------------------|
| `f9c84a5` | m4c #1        | precondition     | identity `monomorphise` pipeline slot        |
| `412938b` | m4c #2        | precondition     | mangling helpers + `--dump-mono-out`         |
| `1d85043` | m5 #1         | precondition     | identity `perceus_pass` slot, m4c rescoped   |
| `2474729` | m5 #2         | shared           | last-use walker for fn params + dump         |
| `592c71c` | m5 #2b        | bug fix          | walker covers `EStr` interpolations          |
| `834af22` | m5 #3         | doc m5 **#3**    | drop unused let-bindings, fresh-RHS subset   |
| `448e18d` | m5 #5         | doc m5 (bench)   | bench fixture + measured deltas              |
| `156f597` | m5 #6         | doc m5 (docs)    | this section's first cut                    |
| `6f3255e` | m5 #7         | doc m5 **#1**    | constant pool for `unit` / `bool` / `nil`   |
| `0c5c19a` | m5 #8         | doc              | fold m5 #7 outcomes into this doc           |
| `2a7a7b5` | m5 #9 step 1  | precondition     | runtime helpers + emit special cases        |
| `3a148e7` | m5 #9 step 2  | doc m5 **#4**+   | scope-aware perceus_pass + dups + drops     |

m4c proper (the specialiser) is **not** in tree. m4c #1/#2 wire the
slot and ship a deterministic name-mangling helper, but
`monomorphise(tp) = tp.decls` — actual specialisation hit a
`clause_fn_name` collision under duplicated bodies and was deferred.
The doc's m5 #1 (constant pool), m5 #4 (decref params at exits),
m5 #5 (last-use reuse), and m5 #6 (closure capture incref) did not
land.

**The shipped work is the safe core**: a typed-AST walker for
last-use analysis (fn params only), a dump diagnostic, and a
conservative "drop unused let-bindings whose RHS is freshly
allocated" pass in `emit_expr`'s `EBlock` case. The conservative
filter is `is_fresh_alloc`: literals, calls, lists, record literals,
ranges, lambdas, binops, unops. Excluded: `EVar` (alias),
`EField` / `EIndex` (extracted refs), and the composite control
forms. `PWild` lets get the same treatment when the RHS is fresh.

### Measured deltas

Bench fixture: `stage2/bench/m5_unused_lets.kai` — 4 unused let-
bindings per iteration, self-contained literal RHS, 1000 iterations.

| version    | alloc  | free   | leaked | leak rate | live_peak |
|------------|--------|--------|--------|-----------|-----------|
| pre-m5     | 29,007 |  1,002 | 28,005 |   96.5 %  |   28,006  |
| m5 #3      | 29,007 | 25,002 |  4,005 |   13.8 %  |    4,010  |
| m5 #9 s2   | 25,004 | 22,000 |  3,004 |   12.0 %  |    3,009  |

m5 #3 alone: ~24× more frees mid-program, ~7× drop in leak rate,
same alloc total. m5 #7 (constant pool) trims another 4,003 allocs
by aliasing every `nil` to a singleton. The m5 #9 dup machinery is
inert on this fixture (no multi-use bindings) but does no harm.

Self-compile of `stage2/compiler.kai`:

| version       | alloc       | free      | leaked      |
|---------------|-------------|-----------|-------------|
| pre-m5        | 130,735,107 | 3,545,979 | 127,189,128 |
| m5 #3 round 2 | 138,684,259 | 3,784,645 | 134,899,614 |
| m5 #7         |  29,533,152 |        37 |  29,533,115 |

m5 #3 alone moved alloc upward (+6.1%) — the new walker / emit
helpers in `compiler.kai` itself add code that allocates faster
than the emitted drops can free, and compiler.kai's hand
discipline means the emitted drops mostly fire inside
diagnostic-error branches that do not run on a clean compile.

m5 #7 swings it the other way decisively: shifting `unit`,
`bool`, and `nil` to shared static singletons takes the alloc
count from 138.7M → 29.5M (**-77.4 % vs pre-m5**), satisfying
the brief's gate. The remaining ~29.5M allocations are split
across `int` (8.4M), `char` (8.9M), `variant` (6.3M), `str`
(2.6M), `record` (1.8M), `cons` (1.5M), `closure` (67K), and
`array` (43K). `free_total` collapses to 37 because most prior
"frees" were the unit/bool/nil chain-decrefs at exit; now those
three tags are inert in the RC path, so almost nothing fires.

`live_peak` ≡ `leaked` is back to the m5 #0 tautology — basic
Perceus' eventual contribution lives in shrinking `live_peak`
while leaving alloc roughly fixed, which requires the
capture-aware drop / dup-at-non-last work below.

### Capture-aware drops are deferred

`is_fresh_alloc` checks the outer constructor only. A let whose RHS
captures an existing binding (e.g. `let unused = [n, n+1]` with `n`
a fn parameter) transfers ownership of `n` into the cons cell;
dropping the cons chain-decref's `n` too. Subsequent uses of `n`
are use-after-free. compiler.kai avoids this pattern by hand
discipline, so selfhost stays green; user code that does not
will surface the latent bug.

Capture-aware drops require pairing with `kai_incref` at non-last
uses (the brief's m5 #4) so the original binding survives the
chain-decref. That is its own milestone and lands separately.

## What landed under m5 #9 (round 3, 2026-04-26)

Three commits ship the scope-aware AST rewrite + dup/drop magic
infrastructure but stop short of flipping the runtime:

- **`2a7a7b5` (step 1/4 — infrastructure):** runtime helpers
  `kai_internal_dup` / `kai_internal_drop` + parallel `kaix_*`
  exports for the LLVM backend. Special cases in `emit_call_expr`
  and `llvm_emit_call` lower `ECall(EVar("__perceus_dup"), [a])`
  → `kai_internal_dup(a)` and `__perceus_drop` analogously. The
  magic names are added to `core_names()` so the closure
  free-var collector treats them as globals, not captures.

- **`3a148e7` (step 2/4 — scope-aware rewrite):** replaces
  `perceus_pass`'s identity body with a real walker:
  - `pcs_collect_uses_*` — scope-aware EVar collection (mirrors
    `fv_expr` modulo polarity). Tracks scope through `SLet` /
    `SVar`, `EMatch` arm patterns, `ELambda` params, and
    handler-clause params. The trailing `resume` clause param is
    filtered out (it's a magic keyword the emitter intercepts at
    call sites; wrapping it in dup would route through
    `emit_ident_value`'s undeclared `kai_resume` fallback).
  - `pcs_rewrite_*` — wraps every non-last in-scope EVar in
    `ECall(EVar("__perceus_dup"), [...])`. "Non-last" =
    position != `last_use_for(name).LUAt(line, col)` OR enclosed
    in a lambda body (closure capture conservatively blocks
    last).
  - `pcs_prepend_unused_drops` — wraps the body in `EBlock` to
    insert `SExprStmt(__perceus_drop(p))` for every `LUUnused`
    parameter.

- **Step 3/4 (runtime uniformisation) — attempted, reverted:**
  modifying `kai_lt` / `gt` / `le` / `ge` / `eq_v` / `ne_v` /
  `add` / `sub` / `mul` / `div` / `idiv` / `mod` / `neg` /
  `boolnot` / `truthy` / `field` to consume their args breaks
  stage 1's selfhost. Stage 1's compiler.kai has no
  perceus_pass; its emitted C uses primitive ops in the loose
  pre-m5 discipline, and a linear runtime turns those reads into
  use-after-free.

- **Step 4/4 (effect measurement) — n/a:** with step 3
  reverted, the dup/drop machinery is *staged but inert*. Every
  primitive op in the runtime still leaves its inputs alive, so
  the dups bump rc and never get matched by a consumer.
  `live_peak` and `leaked` do not move on kaic2 self-compile.

### Numbers (kaic2 self-compile)

| version            | alloc        | free      | leaked       | live_peak    |
|--------------------|--------------|-----------|--------------|--------------|
| pre-m5             | 130,735,107  | 3.5 M     | 127,189,128  | 127,189,128  |
| m5 #3              | 138,684,259  | 3.8 M     | 134,899,614  | 134,899,614  |
| m5 #7              |  29,533,152  |     37    |  29,533,115  |  29,533,115  |
| m5 #9 s2           |  33,020,295  |     39    |  33,020,256  |  33,020,256  |
| m5.x flip Phase 1  |  40,635,512  |     614   |  40,634,898  |  40,634,898  |
| m5.x flip Phase 3  |  69,714,160  | 22.8 M    |  46,876,299  |  46,876,299  |

The +3.5 M alloc delta from m5 #7 to m5 #9-step-2 is the perceus
walker's own source code (~550 new lines in compiler.kai itself);
the dup wrapping and drop emission do not allocate.

The m5.x flip Phase 3 (atomic Steps A+B+C+D, 2026-04-28) lands
the runtime flip with producer-side incref-on-extract, exit
drops on multi-use and LUBlocked params, and a stage-0
eager-dup retrofit. Wall RSS drops from 6.25 GB to 3.02 GB
(**52% reduction**); `live_peak` from 127.2 M to 46.9 M (**63%
reduction**); `free_total` jumps from 39 to 22.8 M, evidence
that the dup/drop discipline is finally consuming. The
+36.7 M alloc delta vs Phase 1 is the dup machinery firing
(every multi-use read invokes `kai_internal_dup` →
`kai_incref`, which counts as an alloc-side increment in
the trace counters). Wall time grew from 2.15 s to 5.74 s
(2.7×) due to per-allocation RC bookkeeping; Full Perceus
(drop specialisation + unboxed primitives) is the path to
shrink that. Remaining leak inventory: issue #82.

### What unblocks step 3+

Stage 1 needs its own perceus pass. The simplest path is to port
the m5 #9 step-2 walker to stage 1's `compiler.kai` (~550 LOC
mirror), publish the same `__perceus_dup` / `__perceus_drop`
magic-name handling in stage 1's emitter, then flip the runtime
in one commit covering both stages. The full chain costs another
~1-2 days and finally lets `live_peak` collapse on real-world
workloads.

Alternative: stage-1-skip. Make stage 1 link a *loose* runtime
(static-inlined fork of runtime.h with non-consuming primitives)
while stage 2 + downstream link the linear runtime. Cheaper to
implement (no stage-1 walker port) but introduces a runtime split
that makes diagnostics confusing — both stages emit the same C
text, but the C means different things at link time. Reject
unless the stage-1 walker port turns out unexpectedly hard.

### Step 3+ outcome (m5.x-1-2 lane, 2026-04-26)

The lane landed step 1 (stage 1 perceus port, `f327d34`) and the
companion `kai_closure` capture incref (`80b0015`). Step 2 — the
atomic flip of `kai_lt / gt / le / ge / eq_v / ne_v / add / sub /
mul / div / idiv / mod / neg / boolnot / truthy / field` to
linear-consumption — was attempted and reverted. The revert keeps
the runtime in its current loose state; the dup/drop machinery
(both stages now have it) stays inert.

Why the flip did not land — three compounding issues surfaced
during attempted self-host:

1. **C argument-order independence.** `pcs_is_non_last` marks the
   lexically-last read as transferred-raw, but C does not specify
   the evaluation order of function-call arguments. clang on
   AArch64 evaluates argument lists right-to-left, so the
   transferred read in `f(opts.mode, opts.path, opts.search_paths)`
   fires *before* the dup-wrapped earlier reads. Under linear
   consumption that frees the local first and the dups read from
   freed memory. A conservative variant (any binding with ≥ 2 uses
   has every read dup'd) was prototyped and is sound, but does not
   close the leak gap on its own.
2. **Stage 0's record-pattern emission.** `emit_pat_test` /
   `emit_pat_binds` for `N_PAT_RECORD` chains `kai_field(_scr, ...)`
   for each named field, sharing the same scrutinee. Under linear
   consumption the first field-test consumes `_scr`; subsequent
   tests UAF on it. A targeted dup wrap fixes record patterns but
   the same shape recurs in nested variant patterns where bindings
   alias `_scr->as.var.args[i]` storage.
3. **Stage 0 has no perceus pass.** The kaic1 binary's machine code
   is whatever stage 0 emits from `stage1/compiler.kai`; stage 0
   emits primitive calls without dup wraps. An eager-dup retrofit
   (every local read of `kai_<name>` becomes
   `kai_internal_dup(kai_<name>)`) was prototyped and does protect
   simple programs, but interacts with closure construction and
   match patterns in ways that would need a substantial stage 0
   audit.

Diagnosis ran past the lane budget. Per the lane's revert clause,
step 2 was rolled back. Step 1 + the closure-capture incref are
landed and inert, giving the next lane a clean starting point: a
stage 1 with `perceus_pass` already wired, a runtime with the
closure-capture leak closed, and a recorded list of the three
issues above to address before retrying the flip.

Empirical numbers (from `KAI_TRACE_RC=1 ./stage2/kaic2
stage2/compiler.kai`, post-revert, with stage 1 perceus active):

| stage      | alloc       | free   | leaked      | live_peak   |
|------------|-------------|--------|-------------|-------------|
| pre-flip   | 40,635,512  |   614  | 40,634,898  | 40,634,898  |
| post-flip  | (see above) | (n/a)  | (n/a)       | (n/a)       |

The post-flip row is "not applicable" because the flip did not
self-host. The 40.6 M alloc baseline is ~7 M higher than the
m5 #9 step-2 row in the table above (33.0 M); the delta is the
post-m7e13 feature load (`!` postfix lowering, plus stage 2's
`pcs_*` walker source itself).

## Concrete sub-milestones for m5 proper

In rough dependency order. Each is its own commit-grain piece.
Status as of 2026-04-26: m5 #3 shipped. The rest are deferred to
a future m5 lane.

- **m5 #1 — Constant pool for nullary primitives.** [SHIPPED in
  `6f3255e` as m5 #7] Static `KaiValue` singletons for `unit`,
  `bool(true)`, `bool(false)`, `nil` carrying `rc = INT32_MAX` as
  a saturation sentinel; `kai_incref` / `kai_decref` short-circuit
  on the sentinel. Pure runtime change, selfhost byte-identical.
  Measured 77.4% alloc reduction on kaic2 self-compile — matches
  the predicted ~80%.

- **m5 #2 — m4c specialiser.** [PARTIAL] The pipeline slot
  (`monomorphise(tp) : [Decl]`) ships as identity in `f9c84a5`,
  with a deterministic mangling helper (`mangle_name`,
  `mangle_ty`) and a `--dump-mono-out` diagnostic in `412938b`.
  Specialised body emission was attempted under m4c #3 and hit a
  hard blocker: `clause_fn_name(line, col, op)` mints C symbols
  by source position alone, so duplicating a body that contains
  an `EHandle` produces colliding clause symbols and the linker
  rejects the binary. Fixing it is a real retrofit (~1-2 days,
  plumbing fn name through `collect_decls` / `lc_new` /
  clause-info) and only pays off once unboxing / drop
  specialisation / reuse-in-place arrive. The basic Perceus pass
  below operates on uniform-boxed values regardless. 3-5 days
  remaining if pursued.

- **m5 #3 — Decref of unused locals.** [SHIPPED in `834af22`]
  Identifies `let name = rhs; rest` where `name` is never
  referenced in `rest` AND `rhs` matches the `is_fresh_alloc`
  predicate (literals, calls, lists, record literals, ranges,
  lambdas, binops, unops; excludes `EVar`, `EField`, `EIndex`,
  and composite control forms). Emits `kai_decref(kai_<name>)`
  immediately after the let. `PWild` lets get the same treatment
  when RHS is fresh. The freshness check looks at the outer
  constructor only; capture-aware drops require pairing with
  m5 #4's dups and stay deferred. Bench fixture and measured
  deltas in the round-2 section above.

- **m5 #4 — Decref of function parameters at exit points.**
  [SHIPPED in `3a148e7` as m5 #9 step 2/4, **inert** until the
  runtime uniformisation lands] `pcs_prepend_unused_drops` wraps
  every fn body in an `EBlock` that inserts `__perceus_drop(p)`
  for every `LUUnused` parameter. Combined with the m5 #2
  walker, this covers the brief's m5 #3 ("drops at end-of-scope
  for last-use bindings"). Currently inert because the loose
  runtime does not consume primitive-op args, so an unused param
  passed only to a primitive remains alive at "drop" time.

- **m5 #5 — Last-use reuse / drop suppression.** [SHIPPED in
  `3a148e7` as m5 #9 step 2/4, **inert**] `pcs_rewrite_*` wraps
  every non-last in-scope EVar read in `__perceus_dup`. The last
  read sees no wrap, transferring its existing reference per
  Koka's Perceus model. Inert pending runtime uniformisation
  (the dups bump rc but no consumer matches the increment until
  primitives consume their args).

- **m5 #6 — Closure capture incref.** [DEFERRED] `kai_closure`
  must incref captures at creation; `kai_free_value` already
  decrefs them on chain-free. Independent change once the dup
  machinery is live. Without it, capture-by-share remains unsafe
  once primitives consume — a captured local may be decref'd
  before the closure runs. ~1 day.

- **m5 #9 step 3/4 — runtime primitive consumption.** [BLOCKED
  on stage 1] Modify `kai_lt` / `gt` / `le` / `ge` / `eq_v` /
  `ne_v` / `add` / `sub` / `mul` / `div` / `idiv` / `mod` /
  `neg` / `boolnot` / `truthy` / `field` to decref their args
  before returning. Tried during m5 round 3 and reverted because
  stage 1's selfhost broke: stage 1 emits non-perceus C (no
  dups), and a linear runtime turns its primitive-op uses into
  use-after-free. Unblocks once stage 1 gets its own perceus
  pass — see "What unblocks step 3+".

Round 2 closed out at m5 #3 + diagnostic infrastructure (walker
dump, mono-out dump). Continuing through #4-#6 is the natural
follow-up; the work fits within ~1-2 weeks of focused effort, with
the m4c retrofit as a separate optional milestone.

## Out of scope (full Perceus, m6+)

Per `docs/stage2-design.md` §m5 ("Out of scope for the basic
pass"): reuse-in-place, drop specialisation per type, unboxing of
`Int` / `Real` / `Bool` / `Char` into native registers, opt-in
regions. None of these are addressed by m5 #0-#6 above. They are
the "full Perceus" milestone scheduled after m5 in the recommended
ordering of `docs/stage2-design.md` §*Recommended ordering*.

## Follow-ups (out-of-scope for this branch, worth pinning)

Items that surfaced while landing m5 #0 + m14-pre but did not get
their own commit. Each is small enough to stay in this list rather
than its own design doc, but big enough that the next contributor
should not rediscover them from scratch.

1. **Runtime symbol shadowing.** User-defined `fn add(...)` /
   `fn neg(...)` / `fn sub(...)` collide at link time with
   `kai_add` / `kai_neg` / `kai_sub` from the runtime, because
   the emitter mints `kai_<userfn>` without any namespace prefix
   and the runtime helpers share the same `kai_` family. The
   stdlib fixtures under `examples/stdlib/` worked around this
   ad-hoc by renaming (`int_add` instead of `add`). Long-term fix
   options: prefix user functions with `kai_user_` (breaks the
   selfhost byte-equivalence; large change), rename runtime
   helpers to `kairt_*` (smaller surface), or emit a static-asserted
   reserved-name error at name resolution. None landed.
2. **m14 nominal migration covers 16 new functions.** When m14
   proper migrates `list_take` → `list.take` etc., the m14-pre
   set must migrate alongside: `list_head`, `list_tail`,
   `list_repeat`, `list_contains`, `list_count`, `list_take_while`,
   `list_drop_while`, `list_uniq`, `list_max`, `list_min`,
   `list_max_by`, `list_min_by`, `list_flat_map`, `list_zip_with`,
   `list_sort`, `list_sort_by` (+ the `int_cmp` helper, possibly
   to `core.ordering` or `math.int`). The corresponding test
   fixtures (`examples/stdlib/list_*.kai`) need their call sites
   updated and re-validated against `kaic2 --path stdlib` once
   the kaic2 core-polymorphism issue (item 3 below) is fixed.
3. **kaic2 typer rejects polymorphic core calls from foreign
   files.** Historical: `kaic2 --core stdlib/core.kai` plus a
   fresh file that calls `list_take([1,2,3], 2)` failed with
   `expected: ([a], Int) -> [a], found: ([Int], Int) -> ?t1 / ?e0`.
   Empirically resolved by 2026-04-27 — `test-stdlib` now routes
   through kaic2 over the post-split `stdlib/core/*.kai` chain
   without typer rejection. The original kaic1-only routing and
   its inline Makefile comment have been retired. m14 proper still
   needs to re-validate selfhost on the new `list.*` names.
4. **Per-process double KAI_TRACE_RC report when run via bin/kai.**
   `KAI_TRACE_RC=1 ./bin/kai run foo.kai` emits two reports
   (kaic1 process + the user binary) because the env var is
   inherited. Useful as a 2-for-1 for casual measurement, but
   confusing if the reader expects one. A future polish would
   tag each report with its pid or with a process-role string,
   or have `bin/kai` strip the env var when forking the user
   binary.

## References

- Reuse Analysis for the Perceus Reference Counting Optimization
  (Reinking et al., 2021) — the Koka paper. Linked from
  `docs/stage2-design.md` §m5 references; the algorithm m5 #5 will
  approximate.
- `docs/stage2-design.md` §m5 — the milestone definition.
- `docs/stage2-design.md` §2 *Full Perceus memory management* — the
  full set of optimisations targeted post-m5.
- `stage0/runtime.h` lines 102-200 — the runtime RC primitives the
  emitter currently under-uses.
