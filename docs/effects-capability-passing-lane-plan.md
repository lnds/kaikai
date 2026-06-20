# Capability-passing evidence transport — lane plan

> **Status: LANE PLAN (asu round, 2026-06-19). Input: `docs/effects-capability-passing-design.md` (Decision A, owner-taken 2026-06-11); umbrella issue #789 / #820.**
> This document resolves §8 of the design (the four open questions) and tears §7's "suggested spine" into a sequenced, gated lane plan (L0–L6). It is implementable input for the worktree lanes — not a re-litigation of Decision A.
>
> **Load-bearing warning, repeated from the design and verified in code (2026-06-19): selfhost byte-id is FALSE-GREEN for this entire effort.** The compiler corpus contains no op-name collision and no effect instance passed through a call. Every lane gate below states explicitly where byte-id proves nothing and what the real oracle is.
>
> **Sequencing constraint (unchanged):** starts only after the native-parity burn-down reaches 0 gaps and #802 has merged — this rewrites the typer/emit/runtime evidence zones both stand on. The native cons/list RC leak lane and the #858 `kai_op_eq` UAF must also be settled first: a moving native floor cannot serve as the parity oracle in L4.
>
> **Code-site drift corrected at authoring time:** the design cites the spawn evidence clone at `runtime.h:10577`; the actual clone is `kai_clone_evidence_chain` called at **`stage2/runtime.h:10649`** (defn `:11125`, free `:11117`). The by-name walk is **`kai_evidence_lookup_node` at `stage2/runtime.h:11332`**; the by-id validity check is **`kai_evidence_lookup_node_by_id` at `:11362`**. Both runtime.h copies (stage0 + stage2) carry the same functions and both change together.

---

## A. Open questions resueltas

The design's §8 left four questions for this round. All four are resolved below. None blocks L0; #1 and #3 are spike-gated inside L2/L3 and must close before L4.

### A.1 Evidence representation — **raw pointer to the evidence node, as an opaque non-RC scalar.** (resolves §8.1)

**Recommendation.** Inject the evidence as a raw `KaiEvidence*` (the node `handler_id` already identifies), typed in the compiler as an opaque scalar in the `TyHandle` family — Perceus must never dup/drop it.

**Reasoning.** The id+side-table alternative buys exactly one property: survival across a future relocation of the evidence node. But under capability passing the node's lifetime is bounded by the `handle` that created it and dominates every frame that receives the pointer (the escape rule §6.2 guarantees no capability outlives its `handle`) — so there is no relocation window to survive. The id route would re-introduce a *lookup* (`by_id` → side table) at every perform site, which is precisely the indirection we are deleting; it would resurrect a second resolution path in spirit. The pointer is the value the C backend already materialises; passing it is free. The libLLVM in-process horizon does not threaten this — an opaque scalar handle is exactly the no-RC newtype pattern that note already pins for LLVM handles. **Precedent:** this is how Effekt and Frank thread evidence (the capability *is* the runtime value, passed directly), and it mirrors the stage1 `TyHandle` exclusion already in the codebase.

**Hidden cost.** A raw pointer in an injected param is an IR-corruption trap if any pass ever treats it as a `KaiValue*` (RC header read at a foreign address). Mitigation is a hard typer invariant: evidence params carry a distinct `TyEvidence`/`TyHandle` tag, and a KAI_TRACE_RC assertion that **zero** evidence-tagged values ever enter dup/drop (gate in L2). This is the same discipline that the `libllvm-in-process` note demands for LLVM handles.

### A.2 Defaults granularity — **one evidence value per builtin effect, minted once at startup (eager), following the #793 module-global pattern.** (resolves §8.2)

**Recommendation.** Builtin effects with a runtime default (`Console`, `Mutable`, `Clock`, the scheduler-backed `Spawn`, etc.) get their evidence node created once in `main`'s prologue and referenced as a module-global. Lazy per-first-demand is rejected for v1.

**Reasoning.** Lazy minting reintroduces a *check-and-install* at the first perform of each effect — branchy, and it recreates a dynamic fallback ("is the default installed yet?") that is the dynamic-scoping smell we are killing. Eager startup install makes defaults *ordinary evidence values* (§3's framing: "defaults become evidence values created at startup, passed like any other, not a stack fallback"), which is the whole point — a default is just a `handle` the runtime opened for you in `main`. The #793 module-global pattern is the existing template and the native walk already knows it. **Hidden cost:** startup pays for every builtin default whether used or not; for the ~handful of builtins this is noise (a few node allocations), and it removes a per-effect branch from every hot perform site, which is the trade we want. Revisit only if a future effect has an expensive-to-construct default — none does today.

### A.3 Arity cost — **measure first; decide tuple-vs-individual-params by a binary threshold on the hot corpus.** (resolves §8.3 — the one that must be measured, not argued)

**Recommendation.** Default to **individual hidden params** (one per distinct demanded effect instance, §3); fall back to a **single packed evidence tuple/struct** *only if* the measurement below crosses the threshold. This is a spike inside L3 (it depends on emit_c already injecting), gated before L4.

**QUÉ medir, exacto:**

1. **Static arity distribution.** Over the whole compiler self-compile + stdlib, after L2's injection pass runs (evidence computed, codegen still off — §7 step 2 state), histogram the count of injected evidence params per function. Record: max, p99, mean, and the count of functions with **> 3** injected params. Source of truth: a `--dump=evidence-arity` dump added in L2 (one pass, throwaway-able).
2. **Call-ABI pressure, runtime.** Build the hot corpus **both ways** (individual params vs one packed tuple) at the L3 emit_c stage and run the existing benches: `tools/native-perf/benches/` (scalar `+ - *`, `list_fold`, `rbtree`) **plus the self-compile wall clock** (`make kaic2`, the ~31 s baseline). Measure: (a) wall-clock self-compile delta; (b) per-bench runtime delta; (c) emitted-C LOC delta (mono already explodes 16k→198k — extra params multiply through every specialisation, so this is the real risk axis, not the per-call cost).
3. **Stack/register pressure proxy.** On the rbtree + list_fold benches, compare `RSS` high-water and (if cheap) callee-saved spill count from the C compiler's own stats; many small scalar params past the register-arg count (6–8 on the targets) start spilling.

**Criterio de decisión binario:**

> **If** (p99 injected-param count ≤ 3) **AND** (self-compile wall-clock regression < 5% vs the individual-params build) **AND** (no bench regresses > 5% vs the packed-tuple build): **ship individual params.**
> **Else** (any of: p99 > 3, OR self-compile regression ≥ 5%, OR a bench ≥ 5% worse than the tuple build): **ship the packed evidence tuple** (one struct pointer per call, fields = the demanded instances).

The asymmetry is deliberate: individual params are the simpler ABI (no struct construction at every call site, monomorphisation stays a type-tuple game it already plays), so they win ties; the tuple only earns its place if the data says the corpus actually has wide rows. The prior, stated honestly: kaikai rows are narrow (most functions demand ≤ 2 effects — `Console`, maybe `Mutable`), so individual params are expected to win and the tuple branch to never ship. But this is the one question the design correctly refused to answer from a chair — **measure, then decide.**

### A.4 Actors — **`send/receive/self` ride the same injection; no separate spine step, but L3 carries an actor-specific fixture as a tripwire.** (resolves §8.4)

**Recommendation.** `Actor[Msg]`'s ops resolve through the same evidence mechanism as any other effect; they do **not** get their own lane. But because the actor runtime has its own dispatch glue (`kai_actor_*`, the `Actor[Msg]` handler installed at `runtime.h:5524`), L3's spawn-audit must include at least one actor fixture (a parent that performs an actor op and a child actor), confirming the injected evidence reaches the actor op sites without the now-deleted spawn clone.

**Reasoning.** `send/receive/self` are ops of `Actor[Msg]` (per `docs/actors.md`) — structurally identical to `get/set` on `State`. The transport doesn't care that the handler is runtime-provided. The one real risk is that actors, like spawn, may have *relied* on the evidence clone to carry the mailbox capability into the spawned actor body. That is the same escape-vector-4 hazard as `Spawn` (§3, §6.2) — and the resolution is the same: the actor body's row is satisfied *inside* the actor (its own `Actor[Msg]` handle, installed by the runtime when the actor is created), not inherited from the parent. **Hidden cost:** if the actor runtime turns out to inherit a *user* capability through the clone (not just `Actor[Msg]` itself), that surfaces in L3's audit as a behavioural change to migrate — which is exactly where it belongs. Confirm-by-fixture, don't pre-engineer a step.

---

## B. Lane plan (L0–L6)

Seven lanes. **Three strict barriers** (marked **⛔ BARRIER**): L0 (nothing starts without the distinguishing fixtures), L4 (the native flip — no merge without dual parity), and the L5 delete (point of no return) / L6-docs (only after the model is single-path and true). **L3 (emit_c flip) and L4 (native flip) cannot land independently** because a bridge where C passes evidence and native walks the stack shares no fixtures and no parity oracle. They are sequenced (C first as oracle, native against it) but neither may merge to `main` while the other is on the old model — they land as one integration block, gated by full dual-backend serial parity.

> Mapping to design §7: L0=step1, L1+L2=step2 (split: typer obligations vs injection+monomorph + collision diagnostic), L3=step3, L4=step4, L5=step5, L6-surface=step6, L6-docs=step7.

### L0 — Foundational fixtures ⛔ BARRIER

**Objetivo.** Land the model-distinguishing programs as fixtures against *today's* compiler, documenting today's broken/accidental behaviour, and land the dispatch-model honesty target — so this drift class can never be invisible again.

**Archivos/zonas.** `examples/effects/` (new fixtures + goldens); new `docs/dispatch-honesty-targets.md`; one paragraph in the honesty-targets index so the new target is discoverable.

**Detalle implementable:**

1. **The three #789 repros — `.err.expected` or quarantine.**
   - `examples/effects/collision_value_corruption.kai` — Repro 1 (by-name binds wrong evidence → silent wrong value). Today this *compiles and produces a wrong answer*. It **cannot** get a `.out.expected` with the wrong value (that golden-locks corruption). It goes to **quarantine**: a `.kai` with a top `# QUARANTINE: #789 — by-name dispatch returns corrupt value; no golden until L2 makes it a compile error` and **no** golden, wired out of the parity glob (an `examples/effects/quarantine/` subdir the harness skips, or a `.quarantine` sidecar). It becomes a real `.err.expected` in L2 when the collision diagnostic lands.
   - `examples/effects/collision_type_mismatch.kai` — Repro 2 (runtime type mismatch). If it currently *aborts*, it gets a `.err.expected` matching today's abort **with a `# DOCUMENTS BROKEN BEHAVIOUR` header**, flipped to the real diagnostic in L2. If it silently corrupts, quarantine like Repro 1.
   - `examples/effects/collision_segfault.kai` — Repro 3 (row-variable-absorbs-phantom-effect typer hole → segfault). A segfault has no stable golden → **quarantine** with the hypothesis comment, flipped to `.err.expected` in L2.
2. **The flagship positive — quarantine today, `.out.expected` in L3.** `examples/effects/two_instances_through_call.kai` — two instances of one effect (`add(c1: Cell, c2: Cell, dst: Cell)` from §6.1) passed **through a function call**. Today the surface `with Eff as a` capability dies at the call boundary (`alias_map_disable_tag`). **Quarantined** with the expected-correct output written in the comment as the L3 acceptance target; gets its real `.out.expected` when L3's emit_c flip makes capability-through-call work.
3. **Spawn-audit baseline — `.out.expected`, kept green throughout.** Snapshot the *current* correct behaviour of every existing `examples/effects/` fixture that performs a user effect inside a fiber against an outside handler (grep `spawn` + a non-builtin `handle`). These get/keep goldens so L3's spawn audit has a before-picture to diff against. **No new behaviour — this is the regression net for the L3 audit.**

**Honesty target landing.** New `docs/dispatch-honesty-targets.md`, structured like `fibers-honesty-targets.md`: rows *Shipped* / *Accidental (works only because ≤1 live handler)* / *Broken (#789)* / *Deferred*. Load-bearing row: **"Dispatch model: dynamic-scoping-by-name (NOT the capability-passing the docs claim) — accidental equivalence holds only while each effect has ≤1 live handler."** Flips to "Shipped: capability-passing" in L6-docs.

**Gate exacto.** `make tier0` + the new fixtures parse and produce their declared goldens/quarantine state. **byte-id is irrelevant (no compiler-code change).** Real gate: every golden matches today's actual output, every quarantine is wired out of the parity glob, the honesty doc names the accidental-equivalence condition verbatim.

**Dependencias.** None (writes against today's compiler). **Barrier:** nothing in L1–L6 starts until L0's distinguishing fixtures exist.

**Paralelo.** Sí — the one lane that can start immediately (touches no compiler code).

### L1 — Typer: evidence obligations

**Objetivo.** Compute, per function, the set of distinct effect-instance evidence obligations its row demands and which call sites discharge them — no injection yet, no codegen change.

**Archivos/zonas.** `stage2/compiler/infer.kai` (row discharge — `inf_row_*` around `:919–969`, discharge logic `:328–333`, `:2221–2402`); `stage2/compiler/resolve.kai` (capability binding for §6 prep, read-only this lane).

**Gate exacto.** A `--dump=evidence-obligations` dump showing, per function, the demanded evidence set and per-call-site supplier (lexical handle / forwarded caller param / startup default — the §3 three-way classification). **byte-id GREEN and meaningful here** — adds an analysis pass emitting nothing into the AST that codegen consumes, so the compiler must still self-host byte-identical. Plus: the obligation dump for the L0 collision fixtures must show the collision (an obligation with no supplier).

**Dependencias.** L0. **Highest design risk — see §C.**

**Paralelo.** No (foundation for L2). Internally parallel-safe with L6-surface's resolve.kai work if that doesn't touch `infer.kai`.

### L2 — Typer: evidence injection + monomorphisation + collision diagnostic

**Objetivo.** Inject the L1 obligations as hidden params (A.1 representation) threaded through monomorphisation, and ship the #789 op-name-collision diagnostic — evidence is computed and present in the AST but codegen still ignores it.

**Archivos/zonas.** `stage2/compiler/infer.kai` (injection after row inference, before mono — §3); `stage2/compiler/monomorph.kai` (specialise per evidence-tuple alongside types — `mono_mangle_*` `:200–283`); the collision diagnostic at the row-discharge hole (`infer.kai:2221–2402`).

**Gate exacto.** **byte-id FALSE-GREEN** — injected-but-unused evidence params may not yet change emitted C, so the corpus could self-host byte-identical while injection is wrong. Real gates: (1) the three L0 collision fixtures now produce the **collision diagnostic** as `.err.expected` (out of quarantine — #789's typer-hole fix, §5.1); (2) the A.3 arity spike runs here — `--dump=evidence-arity` histogram recorded for the A.3 decision; (3) **KAI_TRACE_RC: zero evidence-tagged values enter dup/drop** (A.1 corruption tripwire); (4) ASAN clean.

**Dependencias.** L1. **Crosses with L3 as the frontend→emit_c block.** L2 may merge before L3 only if codegen provably ignores the new params (byte-id proves that, and only that). A reshape here whose only consumer (L3) aborts on the new shape must not merge frontend-alone (coupled-reshape discipline).

**Paralelo.** No.

### L3 — emit_c flip + spawn audit ⛔ (block with L4)

**Objetivo.** Flip the C backend to consume injected evidence at perform sites instead of the by-name walk; execute the spawn/actor audit; make the L0 flagship correct.

**Archivos/zonas.** `stage2/compiler/emit_c.kai` — perform-site emission at **`:2341`** (`kai_evidence_lookup_node("eff")` by-name → consume injected param), by-id sites `:2339/:5145/:6377`; `stage2/compiler/emit_shared.kai` (alias prologue `:605–655`, `:1201–1218`). The spawn audit reads `runtime.h:10649` (clone) but **does not delete it yet** (L5).

**Gate exacto.** **byte-id FALSE-GREEN.** Real gates: (1) **full C-direct corpus behavioural parity** vs the L0 spawn-audit baseline goldens; (2) the L0 flagship `two_instances_through_call.kai` now produces its correct `.out.expected` (out of quarantine); (3) **spawn audit executed** — every fixture performing a user effect inside a fiber re-validated; any that relied on the evidence clone fails loudly and is migrated to explicit in-fiber handles / actor messages, committed in this lane (the §3 behavioural change); (4) the A.4 actor fixture passes; (5) ASAN + KAI_TRACE_RC non-zero balanced. **The arity ABI decision (A.3) is finalised here** (build-both, measure, pick per the binary criterion).

**Dependencias.** L2. **Strictly blocked with L4** — C lands first in the integration branch, native validated against it; neither reaches `main` without the other (one integration block, dual parity green).

**Paralelo.** No — half of the cross-together block.

### L4 — Native walk flip ⛔ BARRIER (no merge without dual parity)

**Objetivo.** Flip the native backend's evidence model to consume injected evidence, validated against the L3 C-direct oracle, achieving full dual-backend parity.

**Archivos/zonas.** The native KIR lowering of perform/handle (`kir_lower_walk.kai` + the native emitter's evidence sites — the native mirror of emit_c's `:2341`); the native consumption of `kai_evidence_lookup_node` flips to the injected param exactly as C did.

**Gate exacto.** **byte-id FALSE-GREEN.** Barrier gate: **full-corpus parity C-direct vs native (SERIAL ratchet, `BACKEND_PARITY_JOBS=1` — parallel is false-green per the #858 lesson), 0 gaps**; all L3 fixtures pass under native; ASAN + KAI_TRACE_RC balanced non-zero. **Strict barrier: the L3+L4 block does not merge to `main` until this serial dual parity is 0 gaps.** A bridge state (C new, native old) is forbidden on `main`.

**Dependencias.** L3 (its oracle). **Hard prerequisite:** the native cons/list RC leak and #858 `kai_op_eq` UAF fixed first — a leaking/crashing native floor cannot serve as parity oracle. (Note 2026-06-19: the nested-match double-free `7261b93f` and #863 cons-leak are in; #858 closed in #862. Re-confirm the native floor is clean before L4.)

**Paralelo.** No.

### L5 — Delete: by-name walk + alias special case + spawn clone ⛔ POINT OF NO RETURN

**Objetivo.** Delete the three obsolete mechanisms so one resolution path remains — the load-bearing simplification.

**Archivos/zonas (verified 2026-06-19):**
- `kai_evidence_lookup_node` — **`stage2/runtime.h:11332`** + the stage0 twin, and all callers (emit_c `:2341` dead after L3).
- The lexical-alias special case — `alias_map_disable_tag` at **`stage2/compiler/emit_shared.kai:2497`**, the `kai_alias_<a>_id` C-local minting (`emit_shared.kai:605–655`, `:1201–1218`; emit_c `:2339/:5145/:6377`).
- The spawn-time evidence clone — `kai_clone_evidence_chain` call at **`stage2/runtime.h:10649`** (defn `:11125`, free `:11117`, field `:2272`), stage0 twins, clone-forward-decls `:2142–2143`.

**Open decision (the §C one):** `kai_evidence_lookup_node_by_id` (`:11362`). The design hedges ("if at all"). **Decide in L5 by grep:** after L3+L4, if no emitted code calls it, **delete it too** — a validity-check with no caller is a dormant second mechanism. Recommendation: **delete it** unless L3/L4 leaves a concrete caller; if the typer wants a post-injection assertion, that lives in the compiler, not as a runtime lookup.

**Gate exacto.** **byte-id FALSE-GREEN** for *behaviour* — but **meaningfully RED-then-regenerated** for `--emit=kir` and emitted-C goldens (deleting code changes emitted text; regenerate per the KIR-goldens-drift discipline). Real gate: full corpus + all new fixtures green on **both** backends, serial parity 0 gaps, ASAN clean, KAI_TRACE_RC balanced. **Point of no return:** after this lane the old model is gone; a revert means re-adding three subsystems.

**Dependencias.** L4. **Barrier:** the PR "where two paths become one."

**Paralelo.** No.

### L6-surface — Named handler instances (§6)

**Objetivo.** Land the surface feature: `with Eff as a` binds `a` as a first-class capability value; capability-as-parameter (`fn f(c: Cell)`); the positional no-escape rule extended to parameters.

**Archivos/zonas.** `stage2/compiler/resolve.kai` + `infer.kai` (the positional escape check — "call-arg or op-receiver only", local, no flow analysis, §6.2); parser already accepts `with Eff as a` (no new syntax — empty row is *omit `/`*, not `/ {}`).

**Gate exacto.** `.out.expected` fixtures for: the `add(c1,c2,dst)` flagship; each escape vector (vector 1 allowed → `.out.expected`; vectors 2/3/4 rejected → `.err.expected` with the positional-rule diagnostic); monomorphic-only (`f(c: State[Int])` works, `f(c: State[T])` rejected per §6.3). **byte-id GREEN and real** (new surface, existing code unchanged). Dual-backend parity on the new fixtures.

**Dependencias.** L5 (named instances need the single-path model under them). **Out of scope, do not build:** `Handler[E]`/HKT, multi-shot, escape analysis/regions, capability storage, `mask` (§6.3).

**Paralelo.** No — gated on L5.

### L6-docs — Rewrite the now-true model ⛔ BARRIER

**Objetivo.** Rewrite `docs/effects.md`, `docs/effects-impl.md`, CLAUDE.md to describe capability-passing as the real model; flip the dispatch honesty target to *Shipped*.

**Archivos/zonas.** `docs/effects.md` (the regions describing the deleted walk), `docs/effects-impl.md` (the by-name walk pseudocode), `CLAUDE.md` (the "capability-passing Effekt + inference" line is now *true* — drop any hedge), `docs/dispatch-honesty-targets.md` (flip the accidental-equivalence row to Shipped).

**Gate exacto.** Doc-only (skips tiers). Gate: no surviving sentence describes the by-name walk as current; the honesty target's "Broken/Accidental" rows retired; `gh issue view 789`/`820` cross-checked closed. **Barrier:** docs must not flip to "true" until L5 makes them true.

**Dependencias.** L5 + L6-surface. Closes #789 and #820.

**Paralelo.** Can draft in parallel with L6-surface; merges last.

---

## C. Riesgos transversales

**Mayor riesgo de diseño — L1 (evidence obligations).** L1 decides *what gets injected where* — the §3 three-way classification and the row-instance identity that distinguishes "two `Cell`s" from "one `Cell`." Every downstream lane inherits this analysis; if it mis-classifies an instance, L2 injects the wrong evidence and L3/L4 faithfully propagate a wrong answer that the ≤1-handler corpus *cannot reveal* (the false-green trap). The risk is not a crash — it is silently encoding a subtly-wrong evidence model that passes every gate except the L0 distinguishing fixtures. **Mitigation:** the L0 distinguishing fixtures are L1's only real oracle; the `--dump=evidence-obligations` gate must show the collision on the #789 fixtures, or L1 is blind. Same "analysis looks right, corpus can't falsify it" shape that hid the dispatch drift for a year (§1).

**Mayor riesgo de soundness — L5 (the delete).** L5 removes three subsystems simultaneously. A soundness defect here is a use-after-free or a missing-evidence segfault, not a wrong value. The spawn clone is the sharpest: it currently *masks* escape-vector-4 (a capability accidentally "working" across `spawn`); deleting it is correct, but any fixture that silently relied on it now performs against freed/absent evidence. **Mitigation:** L5 cannot start until L3's spawn audit has *already* migrated every such fixture (the audit is in L3 precisely so the delete in L5 lands on clean ground); ASAN + serial KAI_TRACE_RC on both backends is the gate, and the A.1 "evidence never enters dup/drop" assertion guards the corruption axis.

**Punto de no retorno — L5 (delete).** Before L5, the old model still exists and any lane can retreat to it (L2–L4 are additive: evidence flows *alongside* a still-live walk). L5 is the first irreversible step. Order the integration so L5 merges only after L3+L4 dual parity is **serially** 0-gaps — a parallel-ratchet green here is false (the #858 lesson).

**Dónde el spawn-audit fuerza cambios de comportamiento — L3.** §3 is explicit: capabilities do **not** cross `spawn`; the clone was their UAF. Any program that performed a *user* effect inside a fiber against a handler *outside* it changes behaviour — it must migrate to an explicit in-fiber `handle` or to actor messages. Surfaced AND resolved in L3 (migrations commit in the same lane), so L5's delete of the clone lands on already-migrated code. The actor variant (A.4) is audited in the same lane. This is the one user-visible behavioural break, and it is intentional — the clone was making unsound programs accidentally run.

**Decisión abierta — `kai_evidence_lookup_node_by_id` (`runtime.h:11362`).** Pushed into L5 with a binary rule: after L3+L4, grep for callers; if none, delete it. Recommendation: **delete** and implement any post-injection sanity check at compile time, not as a surviving runtime lookup. Leave it alive *only* if L3/L4 produces a concrete caller (not expected — the alias by-id path is deleted in L5 itself).

---

## Recommended entry point

Launch **L0 now** — it is the only lane with no code dependency and it is the barrier that enables everything else. Do not touch L1 until `--dump=evidence-obligations` has the L0 distinguishing fixtures to validate against. L1 and L2 are the design-risk core (the false-green trap); L3+L4 are the cross-together integration block gated by serial parity; L5 is the point of no return; L6 is surface + the doc rewrite that finally makes the docs true.
