# Capability-passing evidence transport — design

> **Status: DESIGN ROUND (2026-06-11). Decision A taken by the language
> owner: no shortcuts — the by-name dynamic dispatch is retired and
> evidence travels with calls.** Supersedes the implementation premise of
> `docs/named-handler-instances-design.md` (kept as lineage; its surface
> design survives as §6 here). Implementation is gated on an asu lane
> plan and on the in-flight lanes vacating the affected zones
> (native-parity burn-down, #802). Issue: #789.

## 1. Why this exists, and why we only saw it now

`docs/effects.md` and CLAUDE.md describe kaikai's effects as
**capability-passing, Effekt-style**. The implementation is not that:
an op call walks `fiber.evidence_top` at runtime and picks the innermost
handler **by effect/op name** (`docs/effects-impl.md:364,668`;
`stage2/runtime.h:11260`). That is **dynamic scoping**. The by-id path
added for `with Eff as a` aliases is real but **lexical only** — the
minted `handler_id` is a C local that dies at the first function or
lambda boundary, and the emitter knows it
(`alias_map_disable_tag`, `stage2/compiler/emit_shared.kai:2349`).

Nobody noticed for a year because **the two models are observationally
equivalent on every program in which each effect has at most one live
handler** — which is every program in our corpus, the stdlib, and the
compiler itself. No fixture *could* fail. The gap surfaced only when:

- #789 imported foreign programs (the Effekt examples) where op names
  collide — by-name resolution binds the wrong evidence: silent value
  corruption, runtime type mismatch, or segfault (three repros);
- the named-handler-instances design round tried, for the first time, to
  make a capability *leave* its `handle` block — and the review (asu,
  2026-06-11) had to read the runtime instead of the docs.

Operational lesson, encoded in this doc: **the dispatch model gets an
honesty target** (like fibers and Perceus), and the foundational
fixtures of this redesign are precisely programs that *distinguish* the
two models, so this class of drift can never be invisible again.

## 2. The two models

**Today — dynamic scoping.** `handle` pushes an evidence node (with a
unique `handler_id`) onto the fiber's evidence stack and pops it at the
closing brace. An op call performs a runtime walk: innermost node whose
effect/op *name* matches wins. Callees receive nothing; resolution
happens wherever the perform executes. Spawn clones the parent's
evidence stack into the child fiber (`stage2/runtime.h:10577`).

**Target — capability passing.** Resolution happens **once, in the
typer**, when a row obligation is discharged. The proof of discharge —
the evidence (handler_id / evidence-node reference) — is **injected as a
hidden parameter** into every function whose row demands the effect, and
flows through calls like any argument. At the perform site the evidence
is *at hand*; no name lookup, no stack walk. `kai_evidence_lookup_node`
(by-name) is deleted; `kai_evidence_lookup_node_by_id` survives only as
a validity check, if at all.

Why A and not the alternatives (recorded from the review):

- **B — re-key dynamic scoping by instance:** looks cheap (reuses the
  stack walk) but only disambiguates instances simultaneously alive on
  one stack; a capability that outlives its frame (storage, spawn) still
  dangles. It is the same two-resolution-paths anti-pattern that
  produced #789. **Rejected, including as a bridge.**
- **C — detection only:** make the op-name collision a compile error and
  ship nothing else. Correct as far as it goes, but leaves the by-name
  mechanism — the root cause — alive. **Rejected as an endpoint** (the
  collision diagnostic still ships, see §5.1, but as part of A).

## 3. What is injected, and where

- **Unit of evidence:** one hidden parameter per *distinct effect
  instance* demanded by the function's row. Its runtime value is the
  evidence-node reference the C backend already builds (the thing
  `handler_id` identifies). It is a non-RC scalar in the spirit of
  `TyHandle` — Perceus must not dup/drop it (precedent: the stage1
  handle exclusion, `docs/lane-experience-kir-native-fix.md`).
- **Injection point:** after row inference, before monomorphisation.
  Monomorphisation then specialises functions *per call-shape* exactly
  as it does for types — a row-polymorphic `fn f() : T / e` receives the
  caller's evidence tuple for `e`. Effects already monomorphise (rows
  are resolved statically per instantiation); this rides that machinery.
- **Who supplies it, per call site:**
  - caller handles the effect lexically → the `handle`'s own evidence;
  - caller's row also demands it → forward the caller's hidden param;
  - effect is a builtin with a runtime default → the default-install
    evidence minted in `main` (the #793 module-global pattern is the
    template — defaults become *evidence values created at startup*,
    passed like any other, not a stack fallback).
- **Clauses and `resume`:** a one-shot continuation already captures its
  frame; the evidence params are ordinary locals in those frames, so
  resume restores them for free. Handler clauses receive their own
  evidence (`__self`) the way the stateful-clause ABI (#795/#797)
  already threads state.
- **Masking (`Mutable`/`State` at scope boundaries):** masking today
  means "drop from the row." Under capability passing that is literal:
  the masked callee takes no evidence param for it, and *cannot* perform
  it — the soundness condition the masking discipline (#251/#252)
  already demands. Masking gets *stronger*, not re-designed.
- **Spawn/fibers:** the evidence-stack clone is retired. A spawned
  body's row must be satisfied *inside* the fiber (its own handles or
  builtin defaults). Capabilities do **not** cross `spawn` — that is
  escape vector 4 (§6.2) and the clone was its UAF. This is a
  behavioural change for programs that performed a *user* effect inside
  a fiber against a handler outside it; the corpus must be audited and
  the pattern, if present, migrated to explicit in-fiber handles or
  actor messages.

## 4. What is deleted

- The runtime by-name walk (`kai_evidence_lookup_node`) and its callers
  in both backends.
- The lexical-alias special case (`alias_map_disable_tag` and the
  C-local `kai_alias_*_id` minting): aliases become ordinary evidence
  values, valid wherever a value is valid (§6).
- The spawn-time evidence-stack clone (`runtime.h:10577`).

One resolution mechanism remains. Two live paths is the anti-pattern
that produced #789; this is the load-bearing simplification of the
whole redesign.

## 5. Soundness companions (ship with A, not before or instead)

### 5.1 The op-name collision diagnostic (the #789 typer hole)

Repro 3 of #789 shows a row variable absorbing an effect that has no
real handler when an op name collides with an installed effect — the
type error is *lost* and codegen segfaults. Under capability passing
this becomes structurally impossible (evidence must exist to be
injected), but the **diagnostic** still ships: a clear compile error at
the collision site, with the three #789 repros as `.err.expected` /
behavioural fixtures. These fixtures are foundational: they are the
programs that distinguish the models (§1).

### 5.2 Gates — fixtures, not byte-id

selfhost byte-id is **false-green for this entire effort**: the
compiler corpus contains no op-name collision and no instance passed
through a call. The real gates:

- the three #789 repros (defined behaviour or compile error — never
  corruption);
- a positive fixture passing **two instances of one effect through a
  function call** (the flagship below);
- full-corpus behavioural parity C-direct vs native, both backends
  migrated (the native walk re-implements its evidence model in step);
- ASAN + the restored RC tracer (#816) with **non-zero** activity;
- the spawn audit of §3 (every fixture that performs user effects
  inside fibers re-validated).

## 6. The surface layer: named handler instances

With transport solved, the original feature lands as designed, with the
review's corrections.

### 6.1 Surface (corrected)

`with Eff as a` binds `a` as a first-class **capability value** whose
type is the capability itself (`Cell`, `State[Int]`) — no `Handler[E]`
wrapper (HKT stays banned). Uniform rule:

- capability **in a row** (`fn f() : T / Cell`) — *demanded*; satisfied
  by a `handle` or a default;
- capability **as a parameter** (`fn f(c: Cell) : T`) — *provided* by
  the caller; not in the row. (**Not** `/ {}` — an empty row literal
  does not parse; an empty row is spelled by omitting `/`.)

```kaikai
effect Cell { get() : Int  set(n: Int) : Unit }

fn add(c1: Cell, c2: Cell, dst: Cell) : Unit =
  dst.set(c1.get() + c2.get())
```

Under capability passing this is not even a special case: `c1`, `c2`,
`dst` are the evidence parameters of §3, written *explicitly* instead of
injected. Named instances are the manual gear of the same transmission.

### 6.2 Escape rule (positional, extended)

A capability value — bound by `as` **or received as a parameter** — may
appear only as a call argument or as the receiver of an op call. It may
not be the RHS of a `let`, a record field, a collection element, a
return value, a free variable of a closure that survives the `handle`,
or an argument to `spawn`/actor sends. Checked locally in
`resolve`/`infer`, no flow analysis.

The four escape vectors from the review, and their disposition:

| # | Vector | Covered by |
|---|---|---|
| 1 | direct call-arg in the `handle` body | allowed (the feature) |
| 2 | ephemeral local within the frame | rejected by the `let` rule (cheap over-restriction; revisit if it hurts) |
| 3 | storage in a value that outlives the `handle` | rejected by the positional rule |
| 4 | crossing `spawn` into a fiber | rejected explicitly; the evidence clone no longer exists to make it accidentally "work" |

What must escape was never a handler's job: a value that outlives its
scope is a `Ref[T]` under `Mutable`; a dynamic population of stateful
entities is actors/`Spawn`. (The lineage doc's §5 argument survives
verbatim and is incorporated by reference.)

### 6.3 Out of scope, unchanged

No `Handler[E]`/HKT; no multi-shot; no escape analysis or regions; no
first-class capability storage; instances monomorphic in v1
(`f(c: State[Int])`, not `f(c: State[T])`); `mask` deferred until
named reach proves insufficient.

## 7. Migration shape (input to the asu lane plan — not the plan)

The riskiest property: **both backends must cross together** per
program. A bridge where backend A passes evidence and backend B walks
the stack cannot share fixtures or parity. Suggested spine, to be
torn apart by the lane-plan review:

1. **Foundational fixtures first** (the model-distinguishing programs
   of §5.2) — written against today's compiler, documenting today's
   broken/accidental behaviour as `.err.expected` or quarantined
   negatives. The honesty target for the dispatch model lands here.
2. **Typer: evidence obligations + injection**, behind the existing
   row machinery; no codegen change yet (evidence computed, unused).
3. **emit_c flips** to consuming injected evidence; C-direct corpus +
   fixtures green; spawn audit executed here.
4. **Native walk flips** against the C oracle; parity corpus green.
5. **Delete** the by-name walk, the alias special case, the spawn
   clone (§4) — the PR where two paths become one.
6. **Named instances surface** (§6) — after the model is single-path.
7. `docs/effects.md` / `effects-impl.md` / CLAUDE.md rewritten to
   describe the now-true model; the honesty target updated.

Sequencing constraint: starts only after the native-parity burn-down
and #802 merge (this rewrites the code both stand on).

## 8. Open questions (for the lane-plan round)

1. **Evidence representation:** raw pointer to the evidence node vs
   stable id + side table. Pointer is faster; id survives any future
   evidence relocation. Spike-able.
2. **Defaults granularity:** one evidence value per builtin effect at
   startup, or lazy per first demand? Interacts with #793's
   module-global pattern in the native walk.
3. **Arity cost:** rows with many effects inject many hidden params;
   measure call-ABI pressure on the hot corpus (rb-tree, self-compile)
   before choosing between individual params and an evidence tuple.
4. **Actor ops:** `send/receive/self` ride `Actor[Msg]` — confirm the
   actor runtime's dispatch sites tolerate the same injection or need
   their own step in the spine.
