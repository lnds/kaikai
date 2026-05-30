# Lane experience — Phase 1.B.0: cons-head raw-safe use-analysis

**Date:** 2026-05-29
**Branch:** `perceus-int-cache` (stacked on Phase 1.A commit `aa92d6a`)
**Scope tier:** new frontend analysis pass, zero runtime change

## Scope as planned vs as shipped

**Planned:** the first of three sub-lanes of Phase 1.B (unboxed scalars
that survive the reuse boundary). 1.B.0 is the *use-analysis* pass only:
mark each cons-construction (`[h, ...]`) and each cons-head pattern bind
(the `h` of `[h, ...t]`) as head raw-safe yes/no. Zero runtime change,
zero emitted-text change, verified by a dump + fixtures. It is the
prerequisite that makes the later 1.B.1 representation flip (cons head
→ raw `i64` slot) SAFE — without it, flipping reproduces the three prior
unbox dead-ends (re-box per boxed use, net +60% wall on rb-tree).

**Shipped:** exactly that. New module `stage2/compiler/rawsafe.kai`, a
`--dump-rawsafe` flag, two fixtures (`rawsafe_bump`, `rawsafe_boxed`)
with a `test-rawsafe` golden target. No representation change, no
runtime change, no flip. 1.B.1 is explicitly NOT in this lane.

**Non-goal honoured:** 1.B.0 does NOT close the wall gap. Phase 1.A
already drove `bump`'s int allocs to ~0; the residual ~0.5s / ~13× C is
RC traffic on the reuse path + non-tail recursion, neither of which a
use-analysis pass touches. 1.B.0 produces marks, nothing else.

## Design decisions and alternatives considered (asu)

1. **Mark representation: side table, not an AST field, not `mode`.**
   - NOT reusing `Expr.mode` (`ValueMode`): verified in code that
     `emit_list_tail` (emit_c.kai:2184) emits `ElPlain(x)` via
     `emit_expr`, which already dispatches on `x.mode` (emit_c:3865 /
     emit_llvm:1758). Setting `MUnboxed` on a cons head would change
     emitted C/LLVM *now* → break byte-identity. `mode` means "this node
     produces raw, boxed at the boundary" — orthogonal to "the cons slot
     stores raw".
   - NOT a new field on `ElPlain` / `PBind` (option C1): would break
     dozens of mechanical matches across resolve/infer/fmt/refinements
     and pollute every bind in the language.
   - **Side table** (`[RSMark]`) keyed by `(fn_name, line, col, nth)`.
     The pass does not mutate the AST: no new constructor, no match
     touched, no emit arm reads anything new. Byte-identity is therefore
     *tautological*, not something to pray for. Lookup-by-position is
     the established project pattern (`Use = U(name,line,col,in_lambda)`,
     `LUAt(name,line,col)`).
   - **`nth`** = ordinal of the site in the deterministic per-fn walk.
     Guards against a span-copying desugar producing two distinct
     `ElPlain` with the same `(line,col)` → silent collision (the
     `collect_variant_to_head` indexed-by-position bug-class). Inert
     (always 0) when there is no collision, but sound from day 0.

2. **The 4th-dead-end antidote: conservative use-analysis.** A head is
   `RSRaw` only when its type is integral AND *every* consumer of the
   binding is raw-capable: raw arithmetic, raw comparison, or store into
   another integral-list head. ANY other consumer — a call argument
   (`int_to_string(h)`), field/index, interpolation, lambda capture,
   record field, list spread, a boxed tail-return — forces `RSBoxed`.
   A false `RSRaw` is the soundness bug the three prior lanes hit; a
   false `RSBoxed` only leaves perf on the table. When in doubt,
   `RSBoxed`. The `cap` flag threads "is the enclosing slot raw-capable"
   top-down through `rs_uses_raw_in`.

3. **Both ends marked, must agree.** Load (the `PBind` head of a
   `PList`) and store (the first `ElPlain` of an integral `EList`). Store
   is canonical for 1.B.1's construction flip; load for its destructure
   flip. `bump` shows both `raw`; `show_heads` shows load `boxed`.

4. **Dump runs post-infer, not post-mono.** `dump_rawsafe` re-runs
   `infer_program` (mirroring `dump_typed`) to stamp `.ty`. Post-infer
   suffices: `bump`'s list-element types are already concrete `Int`;
   monomorphisation does not change them. Keeps the dump self-contained.

## Structural surprises

- **The emitter already reads `ElPlain(x).mode` transitively.** The
  initial instinct (reuse `mode`, "the emitter doesn't read it for
  lists yet") was *false* — `emit_list_tail → emit_expr` has dispatched
  on element `mode` all along. This single fact killed option A and
  drove the whole side-table decision. Verified by reading the emit path
  before committing, not after.
- **The stdlib is analysed too** (it is bundled). The dump over a tiny
  user file emits ~80 marks from `list`/`string`/`protocols`. This is a
  feature for confidence — the stdlib marks line up exactly with the
  prediction: `max`/`min`/`map`/`filter`/`foldl`/`zip` → `boxed` (heads
  feed dispatch/calls), `sum`/`product`/`contains`/`tail` → `raw` (heads
  feed arithmetic/compare/raw-tail). But it makes a whole-output golden
  fragile, so the fixtures grep by the user fns' names.

## Fixtures added and coverage gaps

- `examples/perceus/rawsafe_bump.kai` (+ `.out.expected`) — the raw-safe
  shape: `bump` marks load+store `raw`; the `main` tail-return `h` marks
  `boxed` (a boxed boundary). Proves load==store==raw for the target.
- `examples/perceus/rawsafe_boxed.kai` (+ `.out.expected`) — the
  antidote: `show_heads` (`h` → `int_to_string(h)`) marks load `boxed`,
  while `keep_pos` (`n` → `n > 0` and `[n, ...]`) marks load+store `raw`
  in the SAME file, demonstrating the discrimination.
- `stage2/Makefile` `test-rawsafe` (in the `test:` and `test-fast:`
  aggregators) greps the dump by user fn names so a stdlib edit cannot
  perturb the golden.
- **Gap:** no fixture yet for the `nth` collision path (no known
  span-duplicating sugar over an integral list head was found; `nth` is
  currently always 0 in practice). The key is collision-safe by
  construction; a fixture would need a synthetic span clash. Left for
  1.B.1, which will exercise the lookup under real desugar load.
- **Gap:** the analysis covers `Int`/`Bool`/`Char` list heads
  (`ty_is_integral_raw`); `Real` (double) heads are out of scope for the
  mark (the store check uses `ty_is_integral_raw`, not `ty_is_unboxable`).
  Deliberate: 1.B.1's first flip targets `i64` slots. Real-slot heads are
  a 1.B.2 concern.

## Gates (all green)

- Marks correct: `test-rawsafe` passes both fixtures; `bump`
  load+store=`raw`, `show_heads` load=`boxed`. ✓
- selfhost determinism: kaic1b==kaic1c, kaic2b==kaic2c. ✓
- selfhost-llvm determinism: s1.ll==s2.ll. ✓
- **User-program emitted C byte-identical vs Phase 1.A baseline**:
  `diff` of emitted C for `rawsafe_bump` and the bump bench is
  IDENTICAL between the `aa92d6a` baseline kaic2 and the 1.B.0 kaic2.
  (The bundled `stage2.c` grows by the rawsafe module's own code — that
  is the compiler gaining a pass, not the compiler emitting differently.)
- Static gate: `RawSafe`/`RSMark` referenced in ZERO emit/perceus arms. ✓
- tier0: selfhost + demos 34/34. ✓

## Follow-ups left for next lanes

- **1.B.1 — the representation flip.** Add a `slot_mask` to the cons
  representation (reuse the #440 `KAI_VAR_SLOT_INT` machinery), and at
  sites the side table marks `RSRaw` on BOTH load and store, emit a raw
  `i64` slot store/load instead of `kai_int(...)` + boxed head. Gate:
  fixed-point selfhost (repr changes → byte-identity vs baseline no
  longer the gate; kaic2'==kaic2'' is), ASAN clean, `KAI_TRACE_RC` head
  incref→0 on `bump`, and **rb-tree no-regression (±3%) — the canary**.
- **1.B.2** — record fields with `Int` type; `Real` (double) slots; then
  evaluate whether tagged pointers are worth it for the general case
  (rb-tree keys up to 2.1e9 that the cache never catches).
- The side table is currently recomputed by `dump_rawsafe` only; 1.B.1
  will call `rawsafe_analyze` on the compile path (post-unbox,
  pre-perceus) and consume the marks by `(fn,line,col,nth)` lookup at
  the cons construction/destructure emit sites.
