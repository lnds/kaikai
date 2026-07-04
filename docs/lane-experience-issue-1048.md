# Lane experience — issue #1048 (native record scope-exit leak)

## Scope as planned vs as shipped

**Planned (brief):** find the one site where the native backend fails to decref
a heap RECORD leaving scope while the C oracle frees it, and emit the missing
drop generally. The brief framed it as records-only (variants "free correctly,
`leaked=3`") and posed the perceus-vs-emit question: does perceus fail to mark
the drop, or does native fail to emit a marked drop?

**Shipped:** it was **neither perceus nor the native emit stage** — it was the
**KIR lowering** (`kir_lower_walk.kai`), and it was **two distinct leak sites**,
not one. Both backends consume the same post-perceus `perceus_decls` AST; the C
backend emits directly from it, the native backend lowers it to KIR first. The
drop discipline that the C emitter carries in its own codegen was simply never
ported into the KIR lowering. Two shapes leaked:

1. **Dead binding** (`let p = P { .. }`, never read). The C emitter computes
   `block_unused_lets` (in shared `emit_shared.kai`) and decrefs each dead
   fresh-alloc `let` at block exit. `lower_block` did no such thing. Fixed by
   calling the SAME `block_unused_lets` and emitting `KDrop` for each name —
   boxed bindings only (a raw `SInt64`/`SReal`/`SBool` register carries no rc;
   dropping it is the #709 over-drop). The raw/boxed gate reuses `raw_let_slot`,
   the exact predicate the binder itself uses, so the two never disagree.

2. **Field read** (`p.x`). The C emitter's `EField` codegen is a self-contained
   borrow-with-drop: `_f = kai_op_field_at(_b, n); kai_decref(_b)`. The base
   arrives OWNED (perceus dups non-last reads, transfers single-use, ctors
   return rc=1); the field read increfs the field, so the base is no longer
   reachable through the result and the C emitter drops it. The native `EField`
   lowering read the field but left the base undropped ("base disposition
   unchanged"), so the owned base leaked. Fixed by binding the field op to a
   fresh register (captures the field BEFORE the base dies), then `KDrop`ing the
   base — a node-for-node mirror of the C `kai_decref(_b)`.

The brief's variant-frees-correctly premise did **not** hold against `main`:
with the current compiler a bare `let p = Wrap(iter)` in the same loop leaked
identically (leaked ~N), for the SAME dead-binding reason. The fix closes the
record AND the variant path — both are `is_fresh_alloc` bindings the shared
`block_unused_lets` covers, so no shape is special-cased.

## How the C-vs-native dump localized it

`--emit=kir` (backend-agnostic KIR dump) over the repro's while-body closure
`_kai_lam_14` showed **no `drop p`** for the dead binding — but that alone did
not say perceus vs lowering, because the C backend does not consume that KIR.
The decisive step was reading the C backend's actual generated `.c`
(`KAI_BACKEND=c kaic2 <src>`, no `-o`, emits C to stdout): the record lambda
there carried `kai_internal_drop(kai_p)` at scope exit AND `kai_decref(_b)`
inside the field read. Both were emitter-local drops with no KIR counterpart —
so the gap was in `lower_to_kir`, and `block_unused_lets` / the `EField` codegen
in `emit_c.kai` were the exact discipline to mirror.

## Design decisions and alternatives considered

- **Mirror the C oracle's shared helper, don't re-derive.** `block_unused_lets`
  already lives in `emit_shared.kai` and is trusted by the C backend; the fix
  imports and calls it, so the drop DECISION is single-sourced across backends
  (any shadowing/aliasing quirk is shared behavior, not a new native bug). This
  is the #860/#872 lesson applied preemptively: the native leaks were all "the
  C emitter carries a drop the KIR lowering forgot."
- **Boxed-only filter via `raw_let_slot`, not `rhs.mode`.** The register's raw/
  boxed kind is decided by `raw_let_slot` at bind time, so the drop gate reads
  the same predicate — using `rhs.mode` could disagree with the actual slot.
- **Drop the field base unconditionally.** Considered gating on "is the base a
  perceus-dup", but the C oracle drops `_b` in every case because the base is
  always owned by the time it reaches `EField`; matching that keeps the two
  emitters aligned and the rb-tree reuse fixtures (872/882/995, heavy nested
  field reads) confirm no over-drop.

## Fixtures and coverage

`examples/perceus/native_record_scope_leak_1048.kai` (+ `.out.expected`) drives
all three shapes — bare dead record, field-read record, list-of-records — under
the native backend with `KAI_TRACE_RC`, gated by
`test-perceus-1048-native-record-leak` (asserts `free_total >= alloc_total-10`,
a small constant; a regression makes `free_total ~2` while `alloc_total ~6000`).
Wired into `tier1-native.yml` beside the #860/#872/#995 native-leak steps.

## Verification

- Repro (bare, used, list) native `leaked` bounded (~1, `live_peak` flat), was
  ~N. 10M records `--release`: RSS 1.7 MB, was 768 MB.
- `free_total` matches the C oracle **exactly** on the used/nested edge cases
  (4004 == 4004) — RC now byte-equivalent to the oracle for records.
- Native leak regression suite (860/872/882/995) green — the field-base drop
  does not over-drop the rb-tree reuse paths.
- selfhost byte-id green (`kaic2b.c == kaic2c.c`) — the C oracle path is
  untouched; the change is native-KIR-only.

## Follow-ups

None. The fix is the general scope-exit / field-read decref discipline; no
record-shape hardcoding, no phasing.
