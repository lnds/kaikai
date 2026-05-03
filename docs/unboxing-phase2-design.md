# Unboxing Phase 2 — locals + return values unboxed

Status: **DESIGN**, drafted 2026-04-30. Implementation lane opens
after `fibers-tier-2` closes (still has region-brand full
machinery in flight as of this writing — see
`docs/fibers-honesty-targets.md` Tier 2 Item 2). Target: ~5–7
days end-to-end including stage 1 mirror.

Related docs: `docs/perceus-honesty-targets.md` §Tier 2.5 (the
decision pin), `docs/perceus-basic.md` (Phase 1 small-int + char
cache), `docs/stage2-design.md` (general stage 2 plan).

## Goal

Bring the user-facing performance gap on a tight numeric loop
from ~50–100× C native down to ~5–10× by keeping `Int` / `Bool` /
`Char` values in raw C scalars (`int64_t` / `int` / `uint32_t`)
inside function bodies. Heap boxing as `KaiValue *` happens only
at function-call boundaries and at storage edges — record / sum
/ list / closure capture / message send — where the rest of the
runtime expects boxed values.

The benchmark target is `examples/quickstart/02_fizzbuzz.kai` (or
a successor) compiled with `-O2`: the inner integer loop must
run within ~5–10× of an equivalent C loop. Today the gap there
is ~50–100× because every loop iteration heap-allocates an `Int`
and decrefs the previous one.

## Non-goals

- **Unboxed function signatures** — function parameters and
  return values stay `KaiValue *` for now. The call boundary is
  the cheapest place to keep boxed: it forces only one box per
  call, and inlining + LTO recover most of the cost on hot
  inlined arithmetic helpers. Unboxed signatures move to Phase 3.
- **Cross-fiber unboxed messages** — `send` / mailbox copy stay
  fully boxed; the m8.x message-copy path is correct as-is and
  changing it requires the multi-threaded scheduler design.
  Phase 3.
- **Unboxed `Real` / `String`** — Phase 2 covers only the three
  scalar primitives that fit a single C register: `Int`, `Bool`,
  `Char`. `Real` is in scope architecturally but skipped in v1
  because it doubles the test surface; tracked as a Phase 2
  follow-up item (~1d) once the v1 numbers are in. `String` is
  always heap-resident by nature (length-prefixed bytes); no
  unboxed form exists.
- **Reuse-in-place / drop specialisation / regions** — full
  Koka-style Perceus extensions. Tier 3.
- **Removing the small-int + char cache** — the cache from
  Phase 1 (`stage0/runtime.h` lines ~796–867) keeps paying off
  at every boundary where Phase 2 boxes back. It stays.

## Why now (vs deferring to post-MVP)

Pinned in `docs/perceus-honesty-targets.md` §Tier 2.5 on
2026-04-30. Without Phase 2, the claim *Tier 1 #2 —
runtime-efficient* in `CLAUDE.md` cannot be defended without a
footnote: a hand-written loop in kaikai is two orders of
magnitude slower than the same loop in C, OCaml, Haskell, or Go.
Phase 2 is the smallest contained chunk that closes that gap to
the same band those languages occupy.

Phase 2 is **contained**: it does not need the multi-threaded
scheduler, cross-thread atomics, region inference, or alias
analysis that the rest of full Perceus (Phase 3) requires.

## What changes — three pieces

### 1. Value mode tag on every typed `Expr`

Add a fourth field to `Expr` (in `compiler.kai` ~line 1026):

```
type Expr = { kind: ExprKind, line: Int, col: Int,
              ty: Option[Ty], mode: ValueMode }

type ValueMode
  = MUnknown   # parser default; resolver/typer fill in
  | MBoxed     # KaiValue * (current behaviour everywhere)
  | MUnboxed   # raw int64_t / int / uint32_t, depending on ty
```

`MUnknown` is the parser output; an "unboxing analysis" pass
(new, between `infer` and `perceus_pass` in the pipeline) walks
the typed AST and rewrites each node to `MBoxed` or `MUnboxed`.
Phase 1 of the lane lands the field with everything tagged
`MBoxed` — codegen unchanged, selfhost byte-identical. Phase 2
turns on the analysis. Phase 3 lights up the emitter on a
per-`ExprKind` basis (see §3).

Decision: a separate field rather than overloading `ty`. The
type is what the value *means*; the mode is *how it is
represented*. `Int` can be both `MBoxed` and `MUnboxed`. Mixing
them into one `Ty` variant pollutes every existing typer
predicate and is the kind of thing that bit OCaml's
representation-polymorphism work for years.

### 2. Escape analysis (the new pass)

Implemented in a new function `unbox_pass(expr: Expr) : Expr`
that runs once per top-level fn body, after type inference and
before `perceus_pass`. The pass is local — it does not
propagate across fn boundaries.

Bottom-up walk over the AST. Each node is `MUnboxed` if and
only if both:

1. **Its `ty` is unboxable** — `TyInt`, `TyBool`, `TyChar`, and
   *only* those. Anything else (`TyString`, `TyReal` v1,
   `TyList`, `TyRecord`, `TyCon`, `TyFn`, `TyVar`, `TyHole`,
   `TyBranded`, refinements) stays `MBoxed`. `TyDim(inner, _)`
   and `TyAlias(_, inner)` strip to their inner before checking.
2. **Every consumer of this value can accept an unboxed
   `int64_t` / `int`**. The consumers are determined by the
   parent node:

| Parent context | Consumes operand as | Operand stays unboxed? |
|---|---|---|
| `EBinop` (numeric / comparison / logical / bit) | unboxed int / bool | ✅ |
| `EUnop` (`-`, `not`) | unboxed | ✅ |
| `EIf(cond, _, _)` — `cond` slot | unboxed bool | ✅ |
| `EMatch(scrut, _)` — `scrut` slot | depends on arm patterns | see below |
| `ELet(p, rhs, body)` — rhs into local | local takes whatever mode rhs has | ✅ if `p` is a single ident; otherwise box |
| `ECall(fn, args)` — every arg slot | boxed `KaiValue *` | ❌ box at call site |
| `ERecordLit` field | boxed | ❌ |
| `EList` element | boxed | ❌ |
| `EVariant` payload (sum constructor) | boxed | ❌ |
| Closure capture (`ELambda` free var) | boxed | ❌ |
| `EBlock` non-final stmt | discard, no consumer | irrelevant; mode follows expr's own analysis |
| `EBlock` final expr → fn return | boxed | ❌ box at return |
| `EHandle` op argument | boxed | ❌ |

`EMatch` scrutinee: if every arm pattern is a literal-or-wild
of an unboxable type (so the dispatch is a C `switch` on the
raw int), the scrutinee can stay unboxed. Otherwise box. v1
ships with the conservative *always box* rule; the literal-arm
optimization (the case that actually matters for `match n { 0
-> … | 1 -> … }`) is a follow-up after v1 numbers are in.

### 3. Emitter changes

Where `e.mode = MUnboxed`, `emit_expr` emits a raw C scalar
expression instead of a `KaiValue *`. The matrix:

| `ty` | unboxed C type | how to box | how to unbox |
|---|---|---|---|
| `TyInt`  | `int64_t`  | `kai_int(x)`  | `x->as.i` |
| `TyBool` | `int`      | `kai_bool(x)` | `(x == &kai_singleton_true)` or `x->as.b` |
| `TyChar` | `uint32_t` | `kai_char(x)` | `x->as.c` |

The emitter already knows how to do both directions — every
intrinsic that lands in `emit_call_value` (the bit ops just
merged on PR #31, `unit_name`, `__strip_unit`) currently does a
local box + unbox dance with `KaiValue *_a = …; ; KaiValue *_r =
kai_int(_a->as.i & _b->as.i); _r;`. Phase 2 lifts that pattern
out: when the call argument is `MUnboxed`, the emitter writes
the raw expression directly, and the boxed result wrapping
disappears whenever the parent is also `MUnboxed`.

**Boundary tactic**: at every `MUnboxed → MBoxed` edge, the
emitter wraps in the constructor. At every `MBoxed → MUnboxed`
edge, it unwraps with `->as.i` / `->as.b` / `->as.c`. Both
directions are O(1); the cache from Phase 1 means the
`MUnboxed → MBoxed` edge for small ints is allocation-free.

The mode of an `EVar` reference is the mode of its binding.
Local `let x = …` rebinds with the rhs's mode; function
parameters are always `MBoxed` (per non-goal #1). Existing
`PerceusPass` is mode-aware in one direction only: it only
incref/decref's `MBoxed` values. Unboxed scalars don't need RC.

### Where the integration touches `compiler.kai`

In rough order of how the diff lands:

1. `Expr` record gets a `mode` field; every `mk_expr`-style
   constructor takes `MUnknown` by default. ~30-line diff that
   ripples through pattern-matching on `Expr`. Selfhost stays
   byte-identical (no behavioural change yet).
2. New `unbox_pass` module — ~150 lines. Pure function `Expr ->
   Expr`. Called once per fn body in the pipeline driver right
   before `perceus_pass`.
3. `emit_expr`, `emit_call_args`, `emit_let_stmt`,
   `emit_match_expr`, `emit_binop`, `emit_unop`, `emit_if` —
   each grows a `match e.mode` switch. The `MBoxed` arm is the
   current code unchanged; the `MUnboxed` arm is new. Estimate
   ~400 lines added, ~50 modified.
4. Bit-ops emission (lines ~7920–8060) collapses: the manual
   box/unbox dance disappears when both operands and the result
   are `MUnboxed`. Net negative diff there.
5. Stage 1 mirror — same field on its `Expr`, same pass. Stage 1
   is a kaikai-minimal subset; the analysis fits in ~80 lines
   without the optimisations stage 2 has.

The LLVM backend (`stage2/llvm_emit.kai`) needs the mirror
treatment in the same pass set; pre-flight check on whether the
LLVM IR mode matches its symmetric C codegen lives at the end
of the milestones below.

## Milestones within the lane

Each one ends in a green `make tier1` and a single commit; the
commit message names the milestone number.

1. **Field on `Expr`, default `MUnknown`** — no behavioural
   change. Selfhost byte-identical. ~half day.
2. **Pipeline hook + no-op `unbox_pass`** — pass exists, runs
   per fn body, leaves every node `MBoxed`. Selfhost still
   byte-identical. ~half day.
3. **Local `let` bindings + arithmetic** — analysis fires on
   `let x = a + b` style; emitter handles `MUnboxed` `EBinop`
   for numeric ops only. The bit-ops manual dance collapses.
   `examples/quickstart/02_fizzbuzz.kai` benchmark drops by
   ~3–5×. ~1–1.5 days.
4. **Comparisons + boolean ops + `EIf` cond** — closes the rest
   of the inner loop body. Benchmark hits target ~5–10×.
   ~1 day.
5. **Match scrutinee fast path (literal + wildcard + PBind catch-all)**
   — the only v1 pattern-match optimisation. Skips if any arm is
   non-trivial. As of issue #91 (closed 2026-05-02), a
   `PBind(name)` catch-all also stays in the fast path: the body
   binds the raw scrutinee as `kair_<name>` and EVar reads of
   `name` resolve to the raw scalar via an env extension threaded
   by the unbox pass. Hazard guard: if the body references `name`
   inside a `#{...}` interp slot or inside a captured ELambda body,
   the whole match falls back to the boxed path (those read sites
   bypass the alias map and would resolve to a non-existent
   `kai_<name>`). ~half day.
6. **LLVM backend mirror** — same analysis output, symmetric
   IR. `make -C stage2 selfhost-llvm` byte-identical. ~1 day.
7. **Stage 1 mirror** — slimmer port. ~half day.

Estimate envelope: **5.5–7 days**, matching the Tier 2.5 pin.

## Testing

- **Selfhost byte-identical** at every milestone. The pipeline
  must produce the same C as before for every file already in
  the tree, *unless* the milestone explicitly opts in (#3, #4,
  #5). When opted in, the diff is a strict simplification: an
  `kai_int(_a->as.i + _b->as.i)` becomes `(_a + _b)` style.
- **`make demos-no-regression`** baseline 24 holds at every
  milestone.
- **New fixture** `examples/perceus/unbox_phase2_*.kai` per
  milestone with structural greps on the emitted C: assert that
  `kai_int(` does *not* appear inside specific labelled inner
  loops, and that no `->as.i` is dangling (orphan unbox without
  consumer).
- **Benchmark fixture** `examples/perceus/unbox_bench.kai` —
  the fizzbuzz-style numeric loop. The fixture's
  `.out.expected` records the wall-clock band as a sanity ratio
  vs a hand-written C reference (committed alongside as
  `unbox_bench.c.ref`). Not a hard tier1 gate; reports go to
  `tools/coverage-baseline.txt` style ratchet.
- **Stress fixtures** under `make daily` — the existing
  selfhost stress + RC budget probe must not regress. v1's
  acceptance is "no new leaks, no slower than pre-Phase-2" plus
  the benchmark improvement.

## Acceptance criteria

1. `examples/perceus/unbox_bench.kai` runs within ~5–10× of the
   C reference under `-O2` on the developer's machine. v0
   pre-Phase-2 number is ~50–100×.
2. `make tier1` green; `make selfhost` byte-identical except
   inside files the milestones explicitly touched, where the
   diff is allocations-down (boxing constructors removed).
3. `make -C stage2 selfhost-llvm` byte-identical (after
   milestone 6).
4. RC leak count under `KAI_TRACE_RC` does not regress vs
   post-Tier-2 baseline (currently 13.4 M leaked on kaic2
   self-compile per `f9a8822` merge commit). Phase 2 should
   bring it down — every alloc that disappears is an alloc that
   cannot leak.
5. Every existing example in `examples/` continues to run with
   identical stdout.

## Risks + open questions

- **Mode propagation through closures** — captured locals are
  always `MBoxed` per the table above, but the closure record
  stores a `KaiValue *` that on unbox needs a one-shot constructor
  call. v1 just always boxes the captured local at the closure
  site; if benchmarks show this is hot, follow-up.
- **`var` (mutable) bindings** — m7b §`Mutable` `var x = 0`
  desugars to a `Ref[T]` capability. The cell stores a
  `KaiValue *`. v1 keeps `var` boxed throughout. Unboxing
  mutable cells is a Phase 3 item that touches the runtime mem
  model.
- **Refinement types on `Int`** — `Int{x : x > 0}` in
  m12.6 lands as a `TyRefineT`; the unbox analysis must
  strip the refinement to find the underlying `TyInt`.
  Confirmed mechanical (m12.6.x refinements treat refinement
  as a wrapper; remaining items in issues #83–#86).
- **Effect ops** — every effect op currently takes `KaiValue *`
  args. Boxing at the op call site is correct and stays. The
  question is whether the op's *result* propagates as
  `MUnboxed` when the type is right. Conservative v1: result
  of an op is always `MBoxed`. Followup once handler RC story
  is solid.
- **Stage 1 cost** — the stage 1 mirror is the cheapest port
  on the list because stage 1 lacks closures, refinements, and
  most of the surface that complicates stage 2's analysis. If
  the mirror balloons past ~80 lines, that is a signal the
  stage 2 design is too coupled; bail out and re-think the
  pass shape rather than forcing the mirror.
- **Coordination with Perceus pass** — `perceus_pass` rewrites
  reads/drops on `MBoxed` values. After unbox_pass, an
  `MUnboxed` read needs no rewrite. Easy contract: `perceus_pass`
  takes the post-unbox tree and skips any node where
  `e.mode = MUnboxed`. Documented invariant in the perceus pass
  comment.

## Sequencing relative to other lanes

- **`fibers-tier-2`** is the precondition. Both lanes touch
  `emit_call_value` heavily — running them in parallel produces
  conflicts that have to be merged by hand. The fibers lane has
  three of its four items closed already (per-op generics on
  Spawn ✅, LLVM in_dispatch ✅, Monitor + spawn_actor ✅) and
  region-brand full machinery in flight. Once region-brand
  lands, the lane is clear.
- **m13 stdlib** continues in parallel — it touches the typer's
  intrinsic table, not `emit_call`. PR #31 (math/bits) is the
  template; further chunks (the `bit.*` dotted surface, the
  `check` / `bench` blocks) are independent of unboxing.
- **m12.6.x refinements followup** — orthogonal; refinements
  are a typer wrapper, the unbox analysis strips them.

## What this document is NOT

- Not a sign-off on Phase 3. Reuse-in-place, drop specialisation,
  regions, unboxed messages, and unboxed function signatures
  stay explicitly post-MVP. This doc is the contained chunk that
  lets the kaikai CLAUDE.md drop "with a footnote" from its Tier
  1 #2 claim.
- Not a calendar — the 5–7 day estimate assumes the lane runs
  serial without competing for `compiler.kai`. If a fibers
  follow-up reopens, add 1–2 days buffer per the rule pinned in
  `docs/lane-experience-m5x-1-2.md`.
- Not a commitment to remove the small-int / char cache from
  Phase 1. Cache + unbox are complementary: cache wipes the
  cost of boxing back at boundaries; unbox wipes the boxing
  inside the loop.
