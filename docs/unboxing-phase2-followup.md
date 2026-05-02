# Unboxing Phase 2 — follow-up items

Status: created 2026-04-30 alongside the M6 commit. Tracks the
items the v1 lane explicitly deferred so the principle list at
the top of the lane matches what landed.

## LLVM backend mirror (M6 deferred work)

The Tier 2.5 lane (`docs/unboxing-phase2-design.md`) shipped
the unbox analysis + emitter switches in the **C backend
only**. The LLVM backend (`stage2/compiler.kai ::
llvm_emit_*`) currently ignores `Expr.mode` and emits every
node as boxed via the existing `kaix_int` / `kaix_add` /
`kaix_eq` runtime calls.

Consequence:
- `make -C stage2 selfhost-llvm` stays byte-identical at the
  fixed-point level — the LLVM backend is internally
  consistent; the unbox pass simply produces no observable
  effect on the emitted IR.
- A binary compiled via the LLVM backend does **not** see the
  raw `int64_t` collapse that the C backend gets. Hot numeric
  loops via `--emit-llvm` stay at the pre-Phase-2 ratio.

The real mirror needs:

1. A `llvm_emit_expr_raw` helper symmetric to the C backend's
   `emit_expr_raw`, returning an LLVM IR `i64` / `i1` / `i32`
   register instead of `%KaiValue*`.
2. `llvm_emit_let` updated to allocate an `i64` local
   (`%kair_<name>` SSA register, no heap) when `rhs.mode =
   MUnboxed`.
3. `llvm_emit_binop_typed` extended to emit native LLVM `add` /
   `mul` / `sub` / `srem` / `icmp` / `xor` / `and` / `or`
   when the binop is `MUnboxed`.
4. `llvm_emit_match` fast path mirroring `emit_match_switch`
   in the C backend — an LLVM `switch` instruction over the
   raw scalar instead of the per-arm `kaix_eq` chain.
5. The same Perceus coordination contract: skip
   `__perceus_dup` / `__perceus_drop` for raw locals (the
   `pcs_rewrite_*` machinery already handles this; the LLVM
   backend reads the post-rewrite tree, so no LLVM-side change
   is needed).

Estimate: ~1 day after the C-side patterns are settled.
Suggested driver: a `--emit-llvm-bench` smoke fixture
analogous to `examples/perceus/unbox_bench.kai` that asserts
the inner loop lowers to `i64` IR with no
`call %KaiValue* @kaix_int` inside.

## Stage 1 mirror (M7 deferred work)

Stage 1's `Expr` carries no `ty` field — stage 1 is the
bootstrap intermediate compiler and does not run type
inference. The unbox pass as written in stage 2 keys on
`Expr.ty` (`TyInt` / `TyBool` / `TyChar`) to decide which
nodes are unboxable. Without that information, the only
syntactic signals available are:

- `EInt` / `EBool` / `EChar` literals (trivially unboxable).
- `EBinop` arithmetic (could be `Int` *or* `Real` — stage 1
  does parse `Real` literals, so a blind "treat all numeric
  binops as Int" rule would corrupt floating-point code).

The faithful port would either:

1. Add a tiny syntactic-only type oracle to stage 1 (look at
   the binop's operands' literal shape) and only unbox when
   one side is an `EInt` literal. Maybe ~120 lines, brittle.
2. Add a real type pass to stage 1. Definitely past 80 lines
   and conflicts with stage 1's "minimal kaikai → C bootstrap"
   role.

Per `docs/unboxing-phase2-design.md` §Risks:

> If the mirror balloons past ~80 lines, that is a signal the
> stage 2 design is too coupled; bail out and re-think the
> pass shape rather than forcing the mirror.

The mirror balloons past 80 lines on inspection. Skipping is
the right call: stage 1 only matters for bootstrap (compiling
`stage2/compiler.kai`); user code always runs through stage 2
and gets the full Phase 2 speedup. Bootstrap perf is
irrelevant.

If stage 1 ever grows a real type pass for an unrelated
reason, the unbox port is a one-day follow-up. Otherwise this
item stays parked.

## Loop driver TCO regression (out of scope, observed during M3)

The `bench_loop` recursion in
`examples/perceus/unbox_bench.kai` stack-overflows past
~50 k iterations because tail calls inside a perceus-wrapped
block (`({ ... drop ...; ret; })`) are not currently
TCO'd by the C backend. The bench keeps `N = 20_000` for now;
the per-iteration ratio is the meaningful signal.

The TCO regression is independent of the unboxing pass —
pre-Phase-2 the same loop overflows at the same point. Track
in the m7e §28 / m11 area when revisited.

## `EBinop ^` / `//` raw lowering

`^` (integer power) and `//` (integer division) stay boxed
even when both operands are raw, because they don't have a
native single-C-operator form. Phase 2 routes them through
`kai_pow_int` / `kai_idiv` as before. If profiling shows
either is hot, lift the helper inline (similar to the
`__builtin_popcountll` pattern from m13 bits).

## Match fast path PBind catch-all (M5 deferred)

The M5 switch fast path supports `PWild` catch-alls but not
`PBind(name)` catch-alls (where the body uses `name` as a raw
local). Handling `PBind` requires threading a raw-local
extension through the body's lcs/env so EVar reads of `name`
resolve to `kair_<name>` instead of the boxed `kai_<name>`.
Out of scope for v1; routes through the boxed match path
when present.

## Real / String / Array / `var` / cross-fiber messages

**Real LANDED 2026-05-01** (`a6f4295` — Tongariki M1+M2
extension). `decide_mode` classifies `TyReal` as `MUnboxed`;
`raw_c_type` / `box_wrap` / `unbox_field_for` grew a `double`
row each; `emit_kind_raw` lowers `EReal(r)` directly via
`real_to_string(r)`. Two type-aware gates protect the v1
boundaries: `%` (modulo) stays boxed when the result type is
non-integral (no native C `double` modulo), and the M5 match
switch fast path only fires for integral scrutinees (a raw
`double` cannot drive a C `switch`). Selfhost stays
byte-identical when no Real value reaches the unbox pass.

**String / Array / `var` / cross-fiber messages** remain Phase 3
per `docs/unboxing-phase2-design.md` §Non-goals. No follow-up
needed inside this doc; the design pin already covers them.
