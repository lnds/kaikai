# Compiler performance — front-end (inference) plan (2026-05-27)

Status: planning. Measured-twice diagnosis; to be reviewed by asu + linus
before any lane. Audience: the perf-of-the-compiler campaign.

## Why this plan exists

kaikai is self-hosted (stage2 written in kaikai), so the compiler's speed
is bounded by the language's codegen — improving the hot path is a
self-host multiplier. The user asked for a global perf diagnosis ("measure
twice, cut once"). We measured.

## The measurements (not guesses)

**Self-compile baseline:** kaic2 processes its own ~77.6k-LOC bundle in
~9.0s wall, RSS ~1.44 GB.

**Measurement 1 — RC traffic share** (`-DKAI_PROFILE_RC`, self-compile):
```
wall (instrumented) = 98s   rc_traffic = 43.1%   alloc/free = 1.2%   other = 55.6%
incref calls = 1,409,979,634   decref = 783,823,948   alloc = 29M   free = 12M
```
→ **43% of the compiler's time is incref/decref. 1.4 BILLION increfs** to
compile 77k LOC (~18k increfs/line). Alloc itself is only 1.2%.

**Measurement 2 — wall by phase** (stop-flags, best of 2):
```
lex+parse+resolve+infer (--infer)   8.93s
+ monomorph (--dump-mono)           9.07s   (+0.14s)
FULL (to emit C)                    8.96s   (emit ≈ free)
```
→ **~99% of the wall is in the front-end (infer-dominant).** Monomorph adds
0.14s; emit is ~free.

**Cross-product:** ~9s in inference, of which ~43% (≈3.9s) is RC churn,
and the rest (`other`, ~55%) is TyEnv lookup + unification.

## What the data FALSIFIES (don't chase these)

- **#593 extract-raw** — attacks alloc, which is 1.2%. A user-codegen win
  (RB-tree, numeric), NOT a compiler-speed lever. Confirmed asu over linus.
- **monomorph O(n²)** — adds 0.14s. linus's mono suspicion was theater here.
- **emit / codegen quality** — ~free in self-compile (clang does the heavy
  lifting separately). Confirmed eric.
- **cache #461** — real DX lever (incremental rebuilds) but ORTHOGONAL:
  it avoids re-compiling, it does not reduce the RC the compilation does.

## Root cause (located in code)

`InferState` (infer.kai:6831) is an **~18-field monolithic record**
(file, env, prelude_len, sub, recs, errs, insts, holes, row, eff_uses,
aliases, resume_bind, ret_ty, op_eff_arities, op_to_eff, proto_impls,
alias_tag, …). There are **~16 `st_*` reconstructor functions** with **129
call sites** that rebuild the WHOLE record to change ONE field:
```kai
fn st_bump_err(st) : InferState =        # 51 call sites!
  InferState { file: st.file, env: st.env, ..., errs: st.errs + 1, ... }  # incref the other 17
```
Each `st_*` increfs the ~17 surviving pointer fields. `synth`/`unify` call
these millions of times → the 1.4B increfs. `st_bump_err` (51 sites) is the
worst: bumps an `Int` counter, copies 17 pointers.

**Subst already solved this** (infer.kai:5222): hot mutation rides on an
`Array` field (`array_set` in place, `/ Mutable`) — the record wrapper is
rebuilt but the Array (the big payload) is a stable pointer, not copied.
InferState should adopt the same discipline for its hot accumulators.

## The plan — two sub-fronts, ordered by data-backed impact

### Front A — kill the InferState RC churn (the 43%)
Move the **hot accumulator fields** of InferState to mutable cells (1-element
`Array` or `Ref`, the Subst pattern), so `st_*` mutate in place instead of
reconstructing:
- `errs: Int` → mutable cell (st_bump_err: 51 sites, the biggest win).
- `insts`, `holes`, `diags`/`eff_uses`, `row` → append-mostly lists; back
  with a growable `Array` cell so push mutates instead of cons+rebuild.
- `sub: Subst` is already mutable internally — but `st_set_sub` still
  rebuilds the wrapper; if InferState stops being reconstructed, that
  vanishes too.
Keep the cold fields (file, prelude_len, op_eff_arities…) as-is — they don't
churn. The goal: `synth`/`unify` thread ONE InferState whose mutations are
in-place. Expected: collapse most of the 1.4B increfs → the 43% shrinks
toward the alloc floor (1.2%).
RISK: InferState is threaded functionally everywhere; making it mutable
changes the contract (a stale copy now aliases). Must audit that no code
relies on InferState value-semantics (keeping an old snapshot). selfhost
byte-identity is the hard gate.

### Front B — TyEnv O(n) → O(1) (much of the `other` 55%)
`TyEnv` is a linked list; `ty_env_lookup` is O(n), called per `synth`. As the
program grows, lookup cost grows — the asymptotic bomb linus flagged. The
compiler uses a list **because kaikai had no HashMap**. #373 (Hashable) just
CLOSED; #374 (HashMap) is now unblocked. Once HashMap lands, back TyEnv with
it → O(1) lookup. This is the language limiting itself, exposed by self-host.
ORDER: B depends on #374 (HashMap) shipping first.

## Spike (phase 0 — validate before committing Front A)

Reviewers (asu + linus) converged on the load-bearing risk: `st_restore_entries`
(infer.kai:7811) splits InferState fields into **inner-side** (accumulators that
PROPAGATE: `errs, insts, holes, row, eff_uses, sub, recs, diags` — taken from
`inner`) and **outer-side** (scope that REVERTS: `aliases, clause_resume, ret_ty,
op_to_eff, proto_impls` — taken from `outer`). Confirmed in code. The 5 fields
Front A wants to mutate are ALL inner-side → mutating ≡ propagating inner.field,
which is safe (Subst already proves it). Mutating an outer-side field would break
the restore SILENTLY (a diagnostic leaking between isolated branches; selfhost
might not catch it if both branches coincide).

**The spike** (smallest cut, max signal): make ONLY `errs` a mutable cell
(`Array[Int]` of 1, the Subst pattern), rewrite `st_bump_err` (51 call sites) to
mutate, leave the other 17 fields functional. Then:
- Re-run `-DKAI_PROFILE_RC` self-compile: does the incref count drop measurably
  for a single inner-side field? (errs is bumped 51 sites → a real fraction.)
- `make selfhost` byte-identical? If YES → the inner-side mutation contract holds,
  green-light Front A for the other 4 accumulators. If NO → we learned the
  aliasing risk with the cheapest possible change, before touching 5 fields.
- Bonus: it answers linus's "is self-compile a degenerate proxy?" — measure the
  spike's delta on a mid-size program too (demos/).

The spike is throwaway (one field) but de-risks the whole front. Do it FIRST.

### Spike result (2026-05-27, analysis-only — no field mutated)

The spike paid off WITHOUT writing mutation code — the analysis alone
corrected the plan twice:

1. **"inner-side" is necessary but NOT sufficient.** `errs` is inner-side
   yet has a SNAPSHOT reader: `pre_errs = st.errs; let st1 = ...; if
   st1.errs > pre_errs` (infer.kai:7566) — a "did this sub-walk add errors?"
   check. Mutating `errs` to a cell breaks this silently (pre_errs aliases
   the cell). `holes` has the same (3 snapshot reads). The CLEAN candidates
   (append-only, zero snapshot/length reads) are `insts, diags, eff_uses,
   row`. Real criterion: **inner-side AND append-only-without-snapshot**.

2. **THE KEY FINDING — mutating any single field does NOT reduce the churn.**
   The 1.4B increfs are not from any field's payload; they are from
   `st_*` REBUILDING the 18-field wrapper. `st_unify` (infer.kai:7309) calls
   `st_set_sub` on EVERY successful unification (the hot path: synth→unify,
   millions of times). Each `st_set_sub` rebuilds InferState → increfs the
   other 17 fields. ~82M rebuilds × 17 ≈ the 1.4B. **Mutating `diags`
   (pushed only ~9 sites, cold) is irrelevant; `st_set_sub`/`st_set_env`
   (hot) keep rebuilding regardless.**

**Plan correction:** the target is NOT "make field X a cell." It is
**stop rebuilding InferState in the hot `st_*` (st_set_sub, st_set_env)**.
`sub` is ALREADY mutable internally (Subst's Arrays) — so `st_set_sub`
could mutate nothing and just `return st` IF `sub` were a fixed cell on a
non-reconstructed InferState. The correct shape: make the HOT fields
(`sub`, `env`, and the append-only accumulators) live in mutable cells on
a SINGLE long-lived InferState that the hot `st_*` mutate-and-return-same,
eliminating the rebuild. The snapshot-reading fields (`errs`, `holes`) and
the outer-side scope fields (`aliases`, `ret_ty`, …) STAY functional/
value — they are read by `st_restore_entries` and the pre/post checks.

So Front A is a COORDINATED refactor of the hot accumulators + sub + env
into cells, not a per-field change — but bounded to the hot `st_*` and
gated hard on selfhost byte-identity + the snapshot-reader audit above.

### Micro-experiment (2026-05-27, throwaway bench — validated the lever)

To quantify "stop rebuilding the wrapper" before committing the refactor, a
standalone bench: an 18-field record (InferState shape), 1M iterations of a
`bump` that changes 1 field. Two versions, measured with KAI_TRACE_RC + wall:

| Version | allocs (1M iter) | wall |
|---|---|---|
| **Rebuild** (bump reconstructs all 18 fields) | 2,999,751 | **0.39s** |
| **Mutate cell** (counter in `Array[Int]`, returns SAME record) | 1,999,752 | **0.11s** |

**Stop-rebuilding = 3.5× faster (0.39→0.11s), −33% allocs**, even in the
favorable case where all 18 fields alias the same `[Int]` (Perceus already
elides some). In real InferState the 18 fields point at DISTINCT live
structures, so each rebuild increfs 17 distinct pointers — the incref saving
is larger than this bench shows. This empirically validates the plan's target:
the cost is the wrapper rebuild, and eliminating it (mutate-and-return-same)
is a ~3.5× lever on the churn hot path. Caveat: the mutable version still does
~2M allocs (the Int box per array_get/set) — the floor is not zero; #593-class
scalar work would address that residue, but it is the smaller half.

Conclusion: Front A is GO. The lever is real and large. Proceed to the
coordinated refactor (hot st_* mutate-and-return-same), with the
snapshot-reader audit (errs/holes stay functional) as the safety contract.

### Cell representation: `Array[T]`, NOT `Ref[T]` (decided)

Use `Array[T]` (1-element, the Subst pattern), not `Mutable.ref_*`:
- **Performance (the whole point):** `array_set`/`array_get` are direct
  builtins (`EP("array_set", "kai_prelude_array_set", 3)`, emit_c:847) — no
  effect dispatch. `Ref` goes through the qualified op-call path with runtime
  evidence lookup + handler dispatch (#531). Using Ref to cut RC churn would
  ADD an effect-dispatch cost on the hot path — self-defeating.
- **Consistency:** Subst already does exactly this (`slots: Array[Option[Ty]]`,
  infer.kai:5223), so selfhost already digests the shape.
- **Contract already written:** Subst's 3 rules (infer.kai:5198 — thread
  linearly, NEVER fork-and-compare, do not publish outside the inferencer)
  ARE the safety contract this refactor needs, and they map directly onto the
  snapshot-reader audit: `errs`/`holes` stay functional precisely because a
  mutable cell would violate "never fork and compare" (the pre_errs check).

Ergonomic cost (`array_get(cell, 0)` is ugly) is accepted — the compiler
prioritizes speed over elegance and already made this call for Subst.

## ⚠️ DEEPER ROOT CAUSE FOUND (2026-05-27) — Front A is the WRONG target

A user question ("Array allocs too — and Ref exists for Mutable; your
investigation doesn't add up") forced a deeper look. It was right. The
investigation was attacking a surface symptom.

**Measured: Ref and Array are IDENTICAL for perf** (both 2M allocs / 0.11s
on the 1M-iter bench; both kill the wrapper rebuild = the 3.5× lever). My
"Array is faster, Ref dispatches" claim was a prejudice — never measured.
For the cell, use whichever is idiomatic (Ref, since the language built it
for Mutable). But the cell is NOT the real lever.

**The real residual (1M allocs the cell does NOT remove):** isolated to the
bone:
- `loop(k:Int)` pure recursion → **1 alloc** (unboxed: `int64_t kai_loop(int64_t)`).
- `churn(x:Record, k:Int)` → **1M allocs** — the SAME `k-1` that was free
  in `loop` now allocs, because the signature became
  `KaiValue* kai_churn(KaiValue* x, KaiValue* k)` — `k` got BOXED.

**Root cause — unbox pass keeps function params BOXED in MIXED signatures**
(unbox.kai:51-56: "Function parameters and return values stay MBoxed";
unboxing only widens at unbox-aware call sites, not in signatures). It is
ALL-OR-NOTHING per function: if any param is non-unboxable (a record like
InferState/TyEnv/Subst), ALL params stay boxed, including the scalar `Int`s
that travel with them. Then every arithmetic op on a boxed scalar allocs.

**Cost model (measured, exact):**
- Passing a boxed Int (not operated) → 0 extra alloc (just incref).
- EACH arithmetic op on a boxed scalar → 1 alloc (1 op→1M, 2 ops→2M, linear).
- In the self-compile, ~43% of allocs are tag=int — arithmetic on scalars
  boxed because they ride in mixed signatures.

**Why this matters for the compiler specifically:** the compiler is FULL of
mixed signatures — 94+ fns with record+Int, 36 with `InferState + line:Int +
col:Int` (every AST node carries a position). All those Ints are boxed; every
`errs+1`, every index calc, every `line` compare allocs.

**Front A (cells) does NOT fix this.** Moving `errs` to a cell still boxes the
Int on read/write. The cell only removes the wrapper rebuild (the 3.5×, real
but separate). The deeper allocs stay.

**The actual lever: per-parameter (mixed-signature) unboxing** — emit
`kai_f(KaiValue* st, int64_t line, int64_t col)` instead of all-boxed. The
infra ALREADY EXISTS: `UFnSig = US([Ty], Ty)` holds per-param types;
`ty_is_unboxable_t` classifies each. What's missing is CONSUMING it
per-param in the signature emit (today it's all-or-nothing). This is the
direct generalization of #718 (which did single-param scalar sigs for LLVM)
to MIXED params, BOTH backends. Sibling of #718/#593.

**Revised plan:**
1. **Mixed-signature param unboxing** (NEW, the real lever) — consume the
   per-param UFnSig in signature emit; unbox the scalar params even when
   record params stay boxed. Both backends. Biggest self-host multiplier;
   kills the tag=int allocs (43% of allocs) on the hot path.
2. **#374 HashMap → TyEnv O(1)** (Front B, unchanged) — attacks the `other`
   55% (TyEnv lookup). Unblocked by #373.
3. Front A (cells) — DEMOTED. The wrapper-rebuild 3.5× is real but smaller
   and orthogonal; revisit only after (1), and use Ref not Array.

NOT a Perceus bug, NOT a cell-representation issue. A codegen gap in
per-param unboxing. The user's instinct — "is it deeper? a Perceus bug?" —
located it: deeper, and it's the unbox pass, not Perceus.

## Order of attack

0. **Spike: mutate only `errs`** — validate the inner-side contract + measure
   single-field impact. Throwaway, ~1 lane.
1. **Front A (InferState mutation)** — biggest measured lever (43%), no
   dependency, the Subst pattern is proven in-repo. Only after spike is green.
2. **#374 HashMap** — unblocked by #373; needed for Front B and is a 1.0
   stdlib gap anyway. Parallel-track (different area, stdlib not infer).
3. **Front B (TyEnv → HashMap)** — after #374. Attacks the `other` 55%.

NOT in this plan: #593 (user-codegen), cache #461 (DX, separate track),
monomorph, emit.

## Validation discipline

- Re-run `-DKAI_PROFILE_RC` self-compile after Front A: rc_traffic share MUST
  drop materially (target: from 43% toward <15%). incref count MUST fall
  (target: 1.4B → hundreds of M).
- Wall self-compile MUST drop (target: 9s → ~6s after A, less after B).
- selfhost byte-identical at every step — InferState is the typer's core;
  a semantics slip changes the compiler's output.
- tier1 + ASAN green (mutation = potential aliasing/UAF; ASAN is the gate).
