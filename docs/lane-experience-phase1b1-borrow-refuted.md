# Lane experience — Phase 1.B.1 borrow flip: implemented, measured, REFUTED

**Date:** 2026-05-29
**Branch:** `perceus-int-cache` (stacked on 1.B.0 `f89d736` + 1.B.1-infra `3731fbb`)
**Outcome:** the borrow flip is NOT sound on its own; reverted. Ships the
RC-traffic counter (instrumentation) + this finding.

## What 1.B.1-flip tried

Consume the borrow-safe classifier (1.B.1-infra) to bind a cons head by
BORROW (no `kai_incref`) when every use reads its scalar. Architecture
(asu): the borrow is a Perceus RC decision, not an emit one — perceus
excludes the head from its dup-tracking scope and its arm-exit drop set,
and wraps the rewritten arm body in a `__perceus_borrow_head(EVar(h),
body)` sentinel (the #209 reuse-cons precedent); emit recognises the
sentinel and binds the head without incref.

This was implemented end to end and it WORKED mechanically — sum_sq emit
went from:

    KaiValue *kai_h = kai_incref(_scr->as.cons.head);
    ... kai_op_mul(kai_internal_dup(kai_h), kai_internal_dup(kai_h)) ...
    kai_internal_drop(kai_h);

to:

    KaiValue *kai_h = _scr->as.cons.head;     // borrow, no incref
    ... kai_op_mul(kai_h, kai_h) ...          // no dups
    // no drop

The incref, both dups, and the exit drop all vanished, exactly as
designed.

## Why it is REFUTED — the measurement

A RC-traffic counter (incref_total / decref_total, added to runtime.h
under KAI_TRACE_RC, counting only rc-touching calls — pinned/INT32_MAX
short-circuits excluded) on a sum_sq bench (5000-element list, 1000
consume passes):

| case | incref_total | leaked | verdict |
|---|---:|---:|---|
| small ints (in the 1.A cache) | 5M -> 5M | 3 -> 3 | no change (misleading) |
| big ints (out of cache) | 10M -> 15k | **3 -> 5001** | **LEAK — unsound** |

The big-int case exposed a balance bug: `leaked` jumped from 3 to 5001.

**Root cause:** `kai_op_mul` / `kai_op_add` (and the other arithmetic
ops) CONSUME their arguments — `kai_decref(a); kai_decref(b)` at the end.
In the baseline, the two `kai_internal_dup(kai_h)` supplied the two
extra refs that `kai_op_mul` consumes (plus the bind's incref for the
base ref). The borrow removed the incref AND the dups, but `kai_op_mul`
still decrefs its args — so it over-decrefs the borrowed head, which has
no ref of its own. On a shared list (`xs` consumed 1000×) the head's
refcount goes negative-ish and cells leak / risk UAF.

**Why the small-int case hid it:** the 1.A cache pins ints in
`[-65536,65535]` with `rc=INT32_MAX`, so `kai_decref` short-circuits and
the over-decref is a no-op. sum_sq with small ints was sound BY LUCK
(the cache absorbing the imbalance), not by correctness. This is exactly
the kind of latent bug the project's RC-discipline rule (issues
#697/#703 were silent corruption here) exists to prevent — and the
measurement caught it before it shipped.

## The real lesson: borrow REQUIRES unbox-on-read

The borrow of the head pointer is sound only if its consumers do NOT
consume (decref) it. But every arithmetic op consumes. So a pure borrow
("bind without incref") is inseparable from unbox-on-read ("the head's
uses read `kai_h->as.i` as a raw scalar and never pass `kai_h` to a
consuming op"). The two were separated during design as "borrow now,
unbox-on-read later (optional)". The measurement proved they are ONE
change: borrow without unbox-on-read = guaranteed leak.

To ship 1.B.1 soundly, the emitter must lower `h*h` to
`kai_h->as.i * kai_h->as.i` (raw int64 arithmetic, no `kai_op_mul`, no
decref) when the head is borrowed. That is a coordinated emit change on
the arithmetic-lowering path, gated by the big-int leak test — a larger
lane than the borrow bind alone. The classifier (1.B.0 + 1.B.1-infra)
remains the correct, sound prerequisite for it.

## What ships from this lane

- The RC-traffic counter (`kai_rc_incref_total` / `kai_rc_decref_total`)
  in runtime.h, reported under KAI_TRACE_RC. Runtime-only, emit
  byte-identical. It is the instrument that made this finding measurable
  (alloc_total is unchanged by a borrow; only the RC-traffic counter
  shows the effect) and it will gate the eventual unbox-on-read lane.
- This retro.

The borrow consumer (perceus + emit_c changes) was reverted: the tree is
sound (leaked=3 on every bench, small and big int).

## Follow-up (the sound 1.B.1)

`borrow + unbox-on-read` as one coordinated change:
1. perceus marks the borrow-safe head (already done, the classifier).
2. emit lowers the head's arithmetic uses to raw `->as.i` ops (no
   consuming `kai_op_*`), AND binds without incref, AND no exit drop.
3. gate: big-int sum_sq leaked=3 (not 5001) + ASAN clean +
   incref_total drops ~N + selfhost fixed-point + rb-tree ±3%
   (unaffected — rb-tree has no [Int] arithmetic-consumed heads).

The win is real when it lands: big-int sum_sq incref 10M -> ~15k AND
alloc 5M -> ~20k (the re-box of each `h*h` result also collapses when
the operands are raw). For small ints the 1.A cache already makes the
incref free, so the borrow's marginal win there is just the eliminated
call+branch, not an RC operation.
