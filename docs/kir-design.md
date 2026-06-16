# KIR — kaikai Intermediate Representation (design)

**Status:** Lane 0 (KIR types + AST→KIR lowering + `--emit=kir` dumper) shipped on
main; lowering-completion (interp/closures/effects) in progress. Lane 1
(in-process libLLVM backend) is the destination, not yet started.
**Scope:** stage 2 only. Does not touch the bootstrap contract (stages 0–1 keep
emitting portable C from any `cc`). The in-process libLLVM backend is the
destination DEFAULT for users (native binary, no external toolchain); it enters
opt-in for validation and a later lane flips the default. libLLVM never enters the
bootstrap chain — bootstrap stays on the C path, which also remains the
differential oracle.
**Motivation:** (1) strategic — KIR is the launch platform for a native, in-process
libLLVM backend (§7.2): native code via the LLVM C API, no `.ll` text, no `clang`
subprocess, serving the fast-compiler and native-codegen goals. (2) structural —
one AST→target lowering instead of the duplicated `emit_c.kai` / `emit_llvm.kai`
pair (§1), with C-direct staying the mature oracle and bootstrap path.

**The near-term framing is a maintainability/correctness refactor; the strategic
goal is a native backend (§7.2).** As a refactor, KIR unifies the AST→target
lowering so it is written once. As a strategy, KIR is the launch platform for an
**in-process libLLVM backend** (Lane 1) — native code via the LLVM C API, no `.ll`
text, no `clang` subprocess — which serves both the "fast compiler" and "native
codegen without an external toolchain" goals. IF the C-text backend is later moved
under KIR (Lane 2, deferrable cleanup), its output is byte-identical to today's C
as a migration proof. The path to lower the Koka gap lives in the runtime and the
data-structure/zipper algorithm (per the rb-tree perf memories), not in how code is
emitted; what KIR does for performance is to let a codegen optimisation
(reuse-in-place, i64-inline, TRMC) be written **once** in the AST→KIR lowering and
reach every consumer.

---

## 1. Why this exists

Stage 2 has two backends that walk the **same** post-perceus `[Decl]`:

- `emit_program(...)` → C (default backend, `--emit=c` / `c-modular`).
- the in-process libLLVM **native** backend (`--emit=native` /
  `--backend=native`): lowers to KIR, builds the LLVM module via the C
  API, and emits a native object — no `.ll` text. Opt-in until its
  native-vs-C parity ratchet reaches zero.

(The earlier `emit_program_llvm` `.ll`-text frontend — `--emit=llvm` — was
removed once the in-process backend's C-API binding was written; see the
retirement note below.)

`emit_shared.kai` (~160 fns) shares **analysis**, not **lowering**: symbol
resolution (`c_sym`, `efn_resolve`, `fns_filter_*`), variant tags
(`evar_find_tag`, `register_variants`, `user_variant_tag_base()=11`), closure
free-variables (`fv_expr`/`fv_arms`/`fv_stmts_scoped`/`fv_interp`), pattern
bindings (`pat_bindings`), lambda collection (`find_lam`, `lc_*`). The frontier
where the duplication lives — the actual AST→target lowering — is **not** shared.

Measured consequences (from the repo-history defect analysis and the LLVM-parity
lane retros/memories):

- The LLVM backend carries the **highest defect density in the project**
  (~2.78 fix/KLOC, above the typer at 2.53). The mature C backend sits at 1.78.
- Roughly 16 LLVM fixes re-paid bugs the C backend had already closed: installing
  default handlers for Spawn/Cancel/Link/Monitor/File/Stdin/Process, continuation
  marshalling, invalid IR, i64-inline slot repr, TRMC/TCO sentinel decoding.
- Project duplication is 8.6%, concentrated in `emit_c ↔ emit_llvm`.
- This is the **only** place in the repo where Conway's residual reappears inside
  a process (ELP) designed to neutralise it: two lanes built the same thing in
  parallel with only *static* shared context (`emit_shared`), and produced
  divergence plus a second harvest of the same defects.

## 2. The reframing finding (lowers the risk)

perceus does **not** add any new `ExprKind`. It reuses the AST as its own IR:

- RC marks are magic-name calls: `ECall(EVar("__perceus_dup"), ...)`,
  `__perceus_drop`, `__perceus_borrow`, `__perceus_reuse_variant/cons/record`
  (injection sites e.g. `perceus.kai:1737` dup, `:3635` drop, `:486` borrow;
  line numbers drift — grep `EVar("__perceus_` for the current set).
- TCO/TRMC marks are **pipe-encoded strings planted as the callee**:
  `__kai_tcrec|<c_sym>|<dropmask>|<p0>|...` and
  `__kai_trmc|<sym>|<holeslot>|<cname>|<dropmask>|<p0>|...`.

And each backend **decodes them separately**: `tcrec_split_pipe` in C
(`emit_c.kai:1982`), `es_tcrec_split_pipe` in LLVM (`emit_llvm.kai:3036,3053`).
That duplicated decoding of a semantic string is, literally, the lowering
duplication frontier.

So KIR is **not** "add an IR where there was none." It is **promoting to typed
structure what is already a clandestine IR encoded as strings over the AST**.
The risk is therefore dominated by byte-id discipline, not by semantic
uncertainty — we are not inventing semantics, we are materialising them.

---

## 3. Abstraction level

**KIR is ANF (A-Normal Form) over named virtual registers, with explicit basic
blocks, but NO SSA and NO materialised CFG edges.** A mid-low point — not full
Cmm/MIR.

- **Flat (ANF / three-address):** every non-trivial subexpression is named.
  `f(g(x))` becomes `let t0 = g(x); f(t0)`. The justification is internal to
  kaikai: drops, resumes, and suspension points all attach to sequencing points,
  and ANF makes every sequencing point an explicit binder. perceus already names
  the hard cases; ANF generalises that. (Other effectful languages converge on
  flat IRs for the same reason — see §3.1 — but the choice stands on kaikai's own
  needs, not on theirs.)
- **Named virtual registers, NOT SSA.** A KIR binder may be reassigned (TCO/TRMC
  slots need it: the goto-loop rewrites `p0..pn`). SSA would force phi-nodes on
  the tcrec back-edge — pure work, no payoff: the only consumers are two target
  translators, and both C (mutable locals) and LLVM (alloca+store, which the
  tcrec.loop already emits) prefer mutable registers to phi. SSA is the right form
  for an optimiser; KIR does not optimise, it lowers.
- **Explicit basic blocks, but light.** A function body is a list of blocks; each
  block is `(label, [KStmt], KTerminator)`. This is what kills the
  TCO/TRMC/match duplication: today both emitters reconstruct control-flow from
  `EMatch`/`EIf`/sentinel-strings; with explicit blocks, AST→KIR does it **once**
  and the translators only print `goto`/`br`.
- **CFG implicit via terminators, NOT a materialised graph.** No edge lists, no
  dominators. Terminators name their successors; that suffices to emit. A CFG
  object is only justified for dataflow analysis over KIR — and we do none,
  because perceus already ran before.
- **Values are atoms; ops take only atoms.** Strict ANF.

**Against the principles:**

- *Fast compilation (Tier 1.3):* ANF is a single flattening pass over the
  post-perceus AST; basic blocks are generated in the same walk. No fixpoint, no
  iteration-to-convergence. Cheaper than SSA (no dominator computation),
  comparable to the existing `tcrec_rewrite_decls`.
- *Do not degrade runtime (Tier 1.2):* the flat form is exactly what codegen
  wants; no extra indirection. ANF `let t0 = ...` collapse to C locals / LLVM
  registers that clang/LLVM eliminate trivially. self-host byte-id is preserved
  because ANF is deterministic (fixed left-to-right evaluation order, as today).

**Why this level and not lower (no machine layout, no CFG object):** kaikai
delegates optimisation and instruction selection to clang and LLVM. Any KIR detail
below "what the target translator needs to print" (machine registers, stack
layout, dominators) is work clang/LLVM redo — it buys nothing and costs
compile-time (Tier 1.3) and risk. KIR stops exactly at the level where the
remaining step is mechanical target syntax. That ceiling is set by kaikai's
delegation choice, not by matching any external IR.

### 3.1 Comparative evidence (NOT a template)

Other languages are cited here only as evidence that "one shared low IR + thin
per-target translators" scales — they are reference points, not a design to copy.
kaikai's target is LLVM directly; its constraints (post-perceus RC, one native
target, bootstrap-C-as-oracle) are its own. The differences matter as much as the
similarities:

- **Zig (AIR):** typed, flat, structured control-flow, translated to C *or* LLVM by
  thin backends. Closest in *shape* to KIR. Difference: AIR is post-typecheck and
  *pre*-RC (Zig is manually memory-managed); KIR is *post*-perceus and carries
  explicit RC nodes (§4.7). We do not adopt AIR's machine-level details.
- **GHC (Cmm), Rust (MIR):** materialise a CFG object plus low-level types because
  they feed custom optimisers/instruction-selectors. kaikai does **not** — so KIR
  deliberately stays above that. Cited as the level NOT to reach, not the one to
  match.
- **Koka (Core + Parc):** see §5.1. Evidence that a single IR scales to multiple
  targets (Koka has C/JS/C#), with the explicit caveat that Koka's *form* is not
  KIR's form.

---

## 4. KIR node set

### 4.1 Top-level
```
KProgram = { types: [KTypeDecl], fns: [KFn], handlers: [KHandlerDecl],
             install_order: [String], regions: [KRegion] }
KTypeDecl = KSum(name, [KCtor]) | KRecord(name, [KField])
KCtor     = { name, tag: Int, slots: [KSlot] }
KSlot     = SBoxed | SInt64 | SReal | SBool     # i64-inline → SInt64 first-class
KFn       = { sym: String, params: [KParam], ret: KSlot, blocks: [KBlock],
              is_handler_thunk: Bool }
KParam    = { name, slot: KSlot, mode: KOwn | KBorrow }   # borrow is a param property, not a wrap
```

The TRMC sentinel's `dropmask` / `holeslot` / `cname` stop being a string and
become typed fields of the relevant terminator. That removes both `*_split_pipe`
parsers.

### 4.2 Blocks and terminators
```
KBlock = { label: String, stmts: [KStmt], term: KTerm }
KTerm
  = KRet(KVal)
  | KBr(label)
  | KCondBr(KVal, then_label, else_label)
  | KSwitch(KVal, scrut_tag_source, [(Int, label)], default_label)   # match on variant tag
  | KTailCall(callee_sym, [KVal])                                    # non-recursive tail call
  | KTcrecGoto(loop_label, [KAssign(slot_idx, KVal)], dropmask: Int) # ex __kai_tcrec
  | KTrmcGoto(loop_label, hole_slot: Int, cname: String,
              dropmask: Int, [KAssign(slot_idx, KVal)])              # ex __kai_trmc
  | KResume(cont: KVal, arg: KVal, tail: Bool)                       # ex resume → kai_cont_resume
  | KUnreachable
```

### 4.3 Statements (ordered, inside a block)
```
KStmt
  = KLet(name, slot, KOp)         # bind the result of an op
  | KDo(KOp)                      # effectful op, result discarded
  | KRC(KRCOp)                    # explicit Perceus op — KRCOp defined in §4.7
  | KStore(slot_target, KVal)     # slot mutation (TCO loop assigns, Array writes / Mutable)
```

### 4.4 Ops (RHS, all take atoms)
```
KOp
  = KCall(callee_sym, [KVal])                       # direct call to a known fn
  | KCallIndirect(KVal, [KVal])                     # closure / function value
  | KPrim(prim_name, [KVal])                        # +,-,==, kai_intf, ... (inline builtins)
  | KCon(ctor_name, tag, [KSlotInit])               # constructor: variant/record/cons
  | KConReuse(donor: KVal, ctor, tag, [KSlotInit])  # ex __perceus_reuse_*
  | KProj(KVal, slot_idx)                           # destructure: read slot i (= kai_variant_at)
  | KTagOf(KVal)                                    # read the tag (for KSwitch)
  | KClosure(thunk_sym, [KVal] captures)            # captures = what fv_* computes today, resolved
  | KPerform(eff, op, [KVal])                       # invoke an effect op (raises to installed handler)
  | KInstallHandler(eff, handler_sym, body_thunk)   # install a handler over a body
  | KLit(literal)
```

### 4.5 Values (atoms)
```
KVal = KVar(name) | KInt | KReal | KBool | KChar | KStr | KUnit
```

### 4.6 Slot-init (constructor field repr — boxed vs i64-inline)
```
KSlotInit = SIBoxed(KVal) | SIInt64(KVal) | SIReal(KVal)
```
The AST-level `ValueMode` string dies here: boxed/i64 is the **slot-init variant**,
decided once in AST→KIR from `CtorIntSlots` + `mode`. C emits `{.i64 = v}` for
`SIInt64`; LLVM emits the i64 store. Neither translator re-derives unboxability.
This makes the #741/#747 class (runtime-mint-i64 vs emitter-read-ptr desync)
structurally impossible: one place decides the slot repr.

### 4.7 The transversal decision: Perceus as first-class nodes, NOT annotations

**Recommendation: first-class `KRC` nodes.** Deliberately the opposite of today's
magic-name ECalls.

```
KRCOp
  = KDup(KVal)             # incref — ex __perceus_dup
  | KDrop(KVal)            # decref — ex __perceus_drop / kai_decref
  | KDropReuse(KVal) -> token   # uniqueness-conditional decref producing a reuse token
  | KFreeToken(token)     # release an unused token
```

`borrow` stops being a wrap (`__perceus_borrow`) and becomes the `mode: KBorrow`
of the `KParam` plus the *absence* of a `KDrop` at the match exit. Borrow info is
a structural property, not a node the emitter must recognise.

Arguments (the core of the design):

1. **They are already nodes today, just mistyped.** `__perceus_dup(x)` is an
   `ECall` indistinguishable from a real call until you compare the callee string
   — and each backend does that comparison on its own, in multiple sites. A typed
   `KDup` is matched structurally once.
2. **As annotations on other nodes you lose the ordering again.** The delicate
   part of Perceus is the **relative order** of dup/drop vs calls and resumes. As
   ordered `KStmt`s in the block list, the order *is* the list position; the
   translator prints in order. Zero reconstruction. As a `drops_after: [KVal]`
   field on a `KCall`, the translator would have to reconstruct where to emit each
   decref relative to the resume and the tail-call — exactly the duplicated
   reconstruction we want gone.
3. **It keeps RC checkable in the lowering.** With `KDup`/`KDrop` as typed nodes,
   the invariant "no RC op over a non-boxed slot" (§6 Risk 5) is a structural
   check in AST→KIR — a compiler error, not a runtime segfault. Magic-name ECalls
   cannot be checked that way. (Reference points, not the reason: Koka and Rust MIR
   both carry RC/storage ops as explicit block statements rather than annotations —
   §5.1. Cited as corroboration; the argument above stands on kaikai alone.)
4. **`KConReuse`/`KDropReuse` capture reuse-in-place without strings.** The donor
   is `KConReuse`'s first typed field; the `KDropReuse` that produced the token and
   the `KConReuse` that consumes it sit in the same statement list, linked by token
   name. Dynamic uniqueness (the runtime `kai_check_unique` branch) **stays in the
   runtime** — KIR only says "reuse this donor for this ctor," as today.

**The one thing that does NOT become a first-class node: the region.** Regions
(arena) are a lexical/scope axis, not a sequencing one. `KRegion` stays as
program-level metadata plus `region_id: Option[Int]` on `KFn`/`KBlock` that the
translator consults to pick the allocator. This mirrors that perceus is
region-unaware today (issue #120: arena-switch is an emit concern); KIR preserves
the separation rather than forcing unification.

---

## 5. What is shared, what diverges

### Single AST→KIR lowering (the hard semantics, written ONCE)
- ANF flattening + basic-block construction. All "EMatch → switch on tag +
  per-arm blocks", "EIf → condbr", "EBlock → statement sequence" lives here.
  Today duplicated across `emit_expr`/`emit_kind` (C) and `llvm_emit_expr` (LLVM).
- TCO/TRMC sentinel decode → typed `KTcrecGoto`/`KTrmcGoto`. The current
  `tcrec_rewrite_decls` plants nodes, not strings. Both `*_split_pipe` disappear.
- Default-handler installation + `install_order`. The `builtin_default_install_order`
  + install loop (`emit_c.kai:8416-8637`) produce `KInstallHandler` nodes + the
  `KProgram.install_order` field. Today C-only — the #1 reason effects diverge.
- Continuation marshalling. `resume` → `KResume(cont, arg, tail)`; one-shot
  semantics (the continuation is consumed on resume) is a property of `KResume`,
  not a per-translator decision.
- Symbol/tag resolution. `c_sym`, `evar_find_tag`, etc. (already in emit_shared)
  run in the lowering and leave symbols/tags **already resolved** inside KIR. The
  translator resolves nothing.
- Closure captures. `fv_expr`/`fv_arms` run here; `KClosure` carries the
  precomputed capture list. Today both emitters re-call `fv_*`.
- Perceus drops. Already marked by perceus; the lowering translates them to
  `KDrop`/`KDropReuse` in position. Tail-position care (§6) is resolved here, once.

### Dumb KIR→C / KIR→LLVM translators (target syntax only)
Each KIR node has **one** print rule per target. Examples:
- `KSwitch` → C `switch (tag) { case N: goto L; ... }` / LLVM `switch i32 %t, ...`.
- `KDup(v)` → C `kai_incref(v);` / LLVM `call void @kaix_incref(...)`.
- `KConReuse(donor, ctor, tag, inits)` → C `kai_reuse_or_alloc_variant(...)` /
  LLVM `call @kaix_reuse_or_alloc_variant(...)`. No parsing, no recogniser.
- `KSlotInit::SIInt64(v)` → C `{.i64 = v}` / LLVM store i64.

The translator is `kir_stmt -> String` (or `-> [LLVMInstr]`) with an exhaustive
match over the ~28 KIR nodes (9 terminators + 4 statements + 11 ops + 4 RC ops;
atoms are leaves). No analysis state, no `fv_*`, no `evar_find_tag`, no
`*_split_pipe`. **Success metric:** the translator imports nothing from
emit_shared except possibly the target name mangle (genuine target syntax).

### Mapping against the 16 LLVM fixes that re-paid C bugs

| Fix class | Prevented by KIR? | Why |
|---|---|---|
| i64-inline / kind-1 (#747, #741) | **Yes, eliminated by construction** | Slot repr (`SInt64` vs `SBoxed`) decided once in AST→KIR. |
| TRMC/TCO lowering (#668, #706, scrutinee drop) | **Yes** | holeslot/dropmask/cname no longer a twice-decoded string. |
| Token-model / arm-top reuse / borrow (regexp UAF) | **Yes** | `KDropReuse`→token→`KConReuse` is one structure, written once. |
| Boxed-Int read-side leak (#747 Option B) | **Yes** | Slot binder is `KProj` with typed `SInt64` → not boxed, not duped. |
| Shim-call boundary / -flto / llvm-link | **No** | Linking / LLVM codegen shape, not lowering. KIR neither fixes nor worsens. |
| Runtime-unify / RSS / single runtime.h | **No (orthogonal)** | Runtime convergence, not IR. A desirable precondition, not a dependency. |

~12–13 of the 16 belong to classes KIR makes structurally impossible (the decision
is made once and materialised typed). The remaining 3–4 are linking/runtime
problems orthogonal to the IR. That is the quantitative justification for the lane.

### 5.1 Koka as comparative evidence (NOT a template)

Koka is a reference point that corroborates the "one IR, thin per-target
translators" shape — it is **not** a design to imitate. kaikai's situation and
target differ, and the differences are the point:

- **What Koka confirms:** a single typed Core IR feeds multiple backends (C,
  JavaScript, C#) via thin per-target code generators. Evidence the pattern scales
  to several targets; nothing more.
- **Where Koka's form differs from KIR (do NOT copy):**
  - Koka's reuse/RC analysis (Perceus + reuse specialization) is tied to its C
    backend, because for Koka C *is* the native target while JS/C# rely on host GC
    and insert no dup/drop. kaikai's native target is **LLVM directly**; RC is not
    C-specific here. So KIR carries the RC nodes (§4.7) as a *target-neutral*
    layer, not bolted to one backend. This is a deliberate divergence from Koka,
    justified by kaikai's LLVM-direct goal.
  - Koka's Core is a full typed lambda calculus with many optimisation passes
    (inlining, specialization, monadic translation, ...). KIR is intentionally
    lower and dumber: flat ANF post-perceus, no optimiser. We do not want Koka's IR
    surface; we want the minimum that makes the two translators thin.
  - Koka's multi-backend model includes GC targets (JS/C#). kaikai has one native
    RC target; the second "backend" (C) is bootstrap + differential oracle, not a
    distinct product target. Koka simply does not have kaikai's
    two-paths-to-the-same-native-target anti-pattern — which is the problem KIR
    removes.
- **Net:** Koka shows the destination shape is viable for a serious language;
  KIR's specific form is derived from kaikai's code (perceus-as-string-IR, the
  duplicated `*_split_pipe`), not from Koka's layout.

---

## 6. Soundness risks (concrete)

**Risk 0 — Silent catch-all swallows a live AST family (ALREADY HIT, 2026-06-08).**
The lowering `lower_expr` ends with a `_ -> <unit>` catch-all "for forms desugared
before codegen." A first cut of Lane 0 let that arm silently swallow THREE families
the emitters DO handle — string interpolation (`#{x}` → `string_concat_all(())`,
parts lost), closures (`ELambda` → no `KClosure`), effect bodies (`handle`/`resume`
→ `ret ()`). It passed a "304/304 lower without panic" gate because
unit-instead-of-correct does not crash; it would have produced wrong binaries.
**Mitigation, now mandatory:** the lowering gate measures CORRECTNESS, not absence
of panic — behavioural faithfulness checked against C-direct (the oracle), with a
golden per family. During development the catch-all should `panic`/log the
unhandled `ExprKind` so a missing family is loud, not silent. Any `ExprKind` the
mature emitter handles reaching the catch-all is a bug. (This is why Lane 0's brief
now says "COMPLETE lowering" and lists interp/closures/effects explicitly.)

**Risk 1 — Tail-position. KIR RESOLVES it structurally.** perceus deliberately
does NOT place drops in tail position (it would drop *after* the tail via a
`__pcs_block_ret` wrap, hiding the self-tail-call from `tcrec_rewrite_decls`;
`perceus.kai:1770-1780`). In KIR this becomes structurally sound: the tail-call is
a *terminator* (`KTcrecGoto`/`KTailCall`/`KRet`), drops are *statements* that go
**before** the terminator in the block list. "Drop after the tail" cannot exist
because the tail IS the terminator and nothing follows a terminator by
construction. Invariant the lowering must keep: end-of-scope drops perceus marked
emit as `KStmt` before the `KTerm`; the `KTcrecGoto`/`KTrmcGoto` dropmask carries
the drops that must happen INSIDE the re-loop (old params). That is exactly what
the dropmask string encodes today — we only type it. **Gate:** TCO fixture +
KAI_TRACE_RC balanced pass identically.

**Risk 2 — self-host byte-id (hardest gate).** KIR is deterministic only if:
(a) ANF flattening uses fixed left-to-right evaluation order, (b) virtual-register
names are generated deterministically (monotonic counter **per function**, reset
per function — not global, or function processing order would shift names),
(c) block output order is deterministic (generation order, not hash-set). The C
translator must produce **byte-identical** output to today during migration —
forcing care with temporary names / spacing. Achievable because ANF→C is
mechanical. **Binary gate: selfhost byte-id. One byte differs → lane does not
close.**

**Risk 3 — one-shot effects.** `KResume(cont, arg, tail)` must preserve that the
continuation is consumed on resume (one-shot = zero-cost default, Tier 1.2). Risk:
the lowering duplicating a `KResume` over the same continuation (via inlining or an
arm on two paths) silently violates one-shot → use-after-free. Mitigation: the
lowering does NOT inline; it copies structure 1:1 from the post-perceus AST, where
the resume is already unique. **Gate:** `examples/effects/` fixtures + ASAN. This
is where the C backend as oracle is most valuable: the mature C has correct
`kai_cont_resume` semantics; KIR→C must reproduce it; `test-backend-parity.sh`
verifies against LLVM.

**Risk 4 — reuse-in-place is local to the nested pattern.** kaikai's reuse lever is
local to the nested pattern (per the rb-tree reuse-lever memory: the reuse window
is the match arm, not interprocedural).
`KDropReuse`→`KConReuse` linked by token must stay in the same block (or blocks the
runtime uniqueness check covers). Risk: ANF flattening separating `KDropReuse`
(arm-top) from `KConReuse` (arm body) across a block boundary that breaks the
linkage → token lost or freed wrong. Mitigation: the token is a named binder; its
`KFreeToken` emits if no `KConReuse` consumes it in scope (the "shell-dispose if no
downstream Con" shape). Uniqueness stays dynamic (runtime); KIR only carries
intent. **Gate:** leaked==0 on rbtree + regexp, KAI_TRACE_RC reuse≈1M, selfhost
byte-id.

**Risk 5 — kind-1 i64 in patterns.** `KProj` over an `SInt64` slot yields a raw
i64, not boxed. Risk: a `KDup`/`KDrop` applied to a raw i64 (not an RC pointer).
Structural mitigation: `KDup`/`KDrop` are valid only over `KVal` whose slot is
`SBoxed`. The lowering emits no RC op over raw i64 — checkable: an assert "no KRC
over non-boxed slot" turns it into a compiler error, not a segfault. This would
have caught #747 read-side at compile time.

**Risk 6 — compile-time regression.** An extra IR is one more pass and structure.
Risk: AST→KIR + KIR→target slower than direct AST→target. Mitigation: ANF is a
single linear pass; KIR is transient (discarded after emit). Net cost should be
near zero because we delete the double analysis walk each emitter does today
(each re-calls `fv_*`, `evar_find_tag`, re-parses sentinels). **Gate:** measure
self-compile time before/after; must not rise materially (baseline ~31s).

**Risk 7 — source-position loss across the lowering.** The AST carries
`line`/`col` on every node. Today both emitters read those positions straight off
the AST. KIR sits between AST and target, so any position not threaded through the
lowering is lost to everything downstream. Two consumers depend on it: (a) runtime
panic / contract-violation messages with source locations, which must stay
byte-identical (covered by the byte-id gate — if positions drift, byte-id fails,
so this is self-policing for the C path); (b) future DWARF/debuginfo emission
(#500 builds DWARF "from the source positions already tracked in the AST" via
`LLVMDIBuilder` — if KIR drops them, #500 becomes impossible). Mitigation: KIR
nodes carry an optional `pos: SrcPos` (line, col) populated from the AST node they
lower from; the translator emits it where the target wants it (C `#line` /
comments, LLVM `!dbg` metadata). This is metadata, not a node — it does not affect
the ~28-node match. **Gate:** panic-location fixtures pass byte-identical; the KIR
dump (`--emit=kir`) shows positions so #500's lane can rely on them. *This
requirement is not optional: omitting it silently blocks #500.*

---

## 7. Lane plan (ELP)

**Strategy: the destination is the in-process libLLVM backend (§7.2). The
critical path to it is short — `Lane 0 (KIR + lowering) → Lane 1 (in-process
libLLVM)` — validated against C-direct, the mature backend that already exists
and stays untouched as the oracle. Everything else is off the critical path.**

> **This plan was reweighted on 2026-06-08.** The original plan migrated BOTH
> text emitters (`.c` via `cc`, `.ll` via `clang`) under KIR and treated the
> in-process backend as a final, optional lane. That ordering was wrong for the
> project's actual goal. **Decision (2026-06-08):**
>
> - The **LLVM-text** backend is NOT being polished to parity and NOT becoming a
>   KIR consumer. Pursuing a `.ll`-text translator to 388-fixture parity is work
>   on a throwaway artifact — the destination is in-process libLLVM, which does
>   not emit `.ll` text at all. The existing `--emit=llvm` text path is left AS
>   IS (untouched), useful only as a *read-only reference* for which LLVM
>   instructions each construct produces while writing the C-API binding.
>   **(Retired 2026-06-16.)** That read-only-reference role was over once the
>   in-process backend's C-API binding was written; with native at full
>   native-vs-C parity its frozen `.ll` path was dead weight whose stale
>   failures kept a gate red without protecting anything. `emit_llvm.kai`,
>   `--emit=llvm`, `--backend=llvm`, and the C↔LLVM-text parity gate were
>   removed. `stage0/runtime_llvm.c` stays — it is the shared `kaix_prelude_*`
>   forwarder the native backend links against.
> - **C-direct is the sufficient oracle.** It is mature (1.78 fix/KLOC), already
>   the differential safety net, and validates behaviour ("same program, same
>   output"). A second LLVM-text oracle adds nothing C-direct doesn't already
>   give — the one question it could answer ("does KIR map to SSA/registers, not
>   just to C?") is a design question answered by reading the C API, not a
>   continuous test to maintain.
> - The **in-process native backend is the destination DEFAULT** — native binary
>   compilation is what users get (the goal: fast compiler, native code, no
>   external toolchain). The **C-text backend is demoted to two roles: the
>   pure-bootstrap path** (stage 0-1 + bring-up from any `cc`) **and the
>   differential oracle** (validate the native backend's behaviour). It is NOT
>   the long-term user-facing default.
> - **Transition is staged, not a flag-day.** The native backend is built as an
>   OPT-IN first (Lane 1) so it can be validated against C-direct without touching
>   the default or the selfhost byte-id gate. A LATER lane flips the default to
>   native once it is in full parity. Building-opt-in-then-flipping is the safe
>   route to the same destination; do not make the first native lane the default.
> - **selfhost byte-id stays on the C-text/bootstrap path.** A native backend does
>   not emit text, so textual selfhost byte-id does not apply to it. The bootstrap
>   (stage 0-1 → C) keeps textual byte-id as the deterministic scaffold; the native
>   backend is validated by behavioural parity vs C-direct (+ reproducible object),
>   not by textual byte-id. Two gates, two roles.
> - Migrating the C-text backend itself under KIR (the old Lanes 3–4) is
>   deferrable cleanup, off the critical path — the C-text backend already works as
>   bootstrap+oracle whether or not it is ever moved under KIR.

**Parallel layer, NOT big-bang.** KIR enters as `--emit=kir` (dumpable between
passes, Tier 1) plus a new consumer, leaving the existing direct emitters intact
until a KIR-driven replacement is validated against C-direct.

**Every lane below also carries the §7.1 code-quality gate** (each new file scores
`km` **A− or better**, < 400 LOC target / 800 hard cap, avg cognitive < 5/fn, max
< 25/fn, zero new duplication) in addition to its listed soundness gates. The bar
is the repo's own A/A+ files, not its F monoliths. New files are split before they
grow, not after.

### Lane 0 — Define KIR + dumper + COMPLETE lowering
- **Brief:** Create `stage2/compiler/kir.kai` with the §4 types. Write
  `lower_to_kir(decls, regions) : KProgram` producing KIR from the
  post-perceus/post-tcrec AST, **lowering every AST family the emitters handle —
  including string interpolation, closures, and effect handler bodies.** Add
  `--emit=kir` printing readable KIR. No emitter changes.
- **Status (2026-06-08):** the types + dumper + the core lowering shipped on main
  (commits aa53a74, f4ee072). A first cut left three AST families lowered to unit
  by a silent catch-all — interpolation (`#{x}` → `string_concat_all(())`),
  closures (`ELambda` → no `KClosure`), and effect bodies (`handle`/`resume` →
  `ret ()`). The completion lane (interp/closures/effects + bare-name fn-vs-local
  call dispatch) closes those; that work is what made the gap visible.
- **Gate (corrected per the §6 lesson):** the gate is **lowering CORRECTNESS, not
  "no panic."** `--emit=kir` is deterministic; the dump is behaviourally faithful
  for interpolation, closures, and effects (e.g. `print("x is #{x}")` lowers to a
  concat of the parts, NOT `()`); selfhost byte-id (codegen untouched);
  `.kir.expected` goldens for representative programs incl. one each of interp,
  closure, effect, TRMC. A silent catch-all arm that swallows a live AST family is
  a bug (see §6 Risk 0 below).
- **Deps:** none. Foundation.

### Lane 1 — KIR → in-process libLLVM (THE DESTINATION)
- **Brief:** Build `build_llvm_module(kp: KProgram)` — a KIR consumer that walks
  KIR calling the **LLVM C API** (`LLVMModuleCreateWithName`, `LLVMBuildAdd`,
  `LLVMBuildCall2`, `LLVMBuildBr`, `LLVMBuildCondBr`, the switch/phi builders, …)
  to construct the module IN MEMORY, runs the pass pipeline, and emits the object
  file directly — **no `.ll` text, no `clang` subprocess, one process.** Wired
  through the Path-1 prelude-primitive mechanism (§7.2): ~30–60 forwarders to the
  C API in `stage2/runtime.h`, with stage2 linked against `libLLVM` (discovered
  via `llvm-config`, static-linked into the binary so brew users need no LLVM —
  the Rust/Zig/Julia model). Enters behind an opt-in flag (e.g.
  `KAI_BACKEND=native`) for VALIDATION; the C-text backend remains the default
  *during this lane only*. The native backend is the destination default (the flip
  is Lane 1.5); C-text's permanent role is bootstrap + oracle, not user default.
- **Build incrementally for isolation** (this replaces the "third oracle" the old
  plan leaned on): bring it up one construct at a time — a `main` returning a
  constant, then arithmetic, then calls, then control flow, then ctors/match,
  then RC, then effects — each step validated against C-direct. Isolation comes
  from build granularity, not from a parallel LLVM-text backend (a text path does
  not even exercise the FFI/ABI, so it could not isolate a binding bug anyway).
- **Critical reprs (§7.2):** LLVM handles (`LLVMValueRef`, `LLVMModuleRef`,
  `LLVMBuilderRef`, `LLVMTypeRef`, `LLVMBasicBlockRef`) cross as a **non-RC opaque
  scalar newtype, NOT `Int`** — the tagged-Int runtime would corrupt a pointer.
- **Gate:** behavioural parity vs C-direct over the 388 fixtures (the ONLY oracle
  needed); ASAN; `KAI_TRACE_RC` shows ZERO refs carrying the handle newtype repr
  (proves handles never enter the RC regime); object emits + links + runs.
  **Measure compile-time end-to-end — this is where the "fast compiler" goal is
  won** (no fork/exec, no text serialization, no re-parse). Mandatory retro.
- **Deps:** Lane 0 (complete lowering). Does NOT depend on any text-backend lane.
- **Status (2026-06-08, PR #780):** Lane 1 shipped the bring-up only — `TyHandle`,
  opt-in libLLVM link, `--emit=native`, `main → 42` end-to-end, and `match-raw`
  (the unbox-pass promotion the walk needed, valuable on its own). The generic KIR
  walk is NOT shipped (staged in `git stash kir-step2-wip`); it is blocked on the
  EBlock-raw issue below. The `main → 42` spine was validated AD-HOC, not by a
  reproducible fixture — see Lane 1.2.

### Lane 1.2 — complete the native walk + make it reproducible (PREREQUISITE of 1.5)
- **Brief:** Two coupled pieces that together make the native backend actually
  emit every program, reproducibly:
  1. **Unblock #779 — EBlock-multi-stmt-returns-raw.** Same bug-class as match-raw
     but at the `EBlock` frontier: a block `{ s1; s2; handle }` boxes its raw tail.
     asu deliberately isolated this from match-raw because promoting it threads the
     Perceus dup/drop discipline through the block — an error here is a LEAK or
     use-after-free (silent), not a compile mismatch. **This is the single most
     dangerous change in the whole KIR effort** — the Perceus-boxing-hell in pure
     form. It gets its own lane, maximum discipline.
  2. **Make `main → 42` (and the walk) reproducible.** Lane 1 validated the native
     spine by hand. Wire a real fixture + the `tier1-native.yml` path-gated
     workflow: emit object in-process → link → run → diff vs C-direct, by one
     command. NB (2026-06-08): the post-rebase `--emit=native` did not reproduce
     in a fresh check (`attempted to call a non-callable value`); confirm whether
     the rebase regressed a prim registration or the spine was always fragile.
- **Gate:** selfhost byte-id (the C/default path) green; **a fixture that EXERCISES
  the EBlock-raw and native paths** (byte-id is false-green without it — the
  recurring lesson); `KAI_TRACE_RC` balanced + ASAN (RC is the failure mode here,
  not compilation); native parity vs C-direct over the 388 once the walk lands.
- **Deps:** Lane 1. **Hard prerequisite of Lane 1.5** — the default cannot flip to
  native until the native backend emits every program in full parity, which needs
  this lane. Do NOT treat the ad-hoc `main → 42` as "done".
- **Status (2026-06-08, Parte A shipped):** piece 2 (reproducibility) is done. The
  `--emit=native` panic was diagnosed to TWO coupled stage1 bugs — the production
  kaic2 is compiled by kaic1 (type-blind), which (a) emitted the `llvm_*` prims
  through the generic `kai_apply` closure path (a non-callable forwarder → runtime
  panic) and (b) ran Perceus dup/drop over raw handles (RC corruption). Fixed by a
  stage1 `rprelude_table` (per-arg marshalling + result-box) and a syntactic
  Perceus exclusion of handle params + handle-prim lets. `main → 42` now emits →
  links → runs reproducibly (exit code + stdout == C-direct; both 0 — the return
  value is NOT an exit code, the brief's "exit 42" was a wrong observable). Wired
  `examples/native/`, `tools/test-native-parity.sh`, and the path-gated
  `tier1-native.yml`. Gates green: selfhost byte-id, tier0, ASAN+UBSan on the
  native binary (handles proven out of the RC regime). Piece 1 (#779 EBlock-raw +
  the generic walk) is **Parte B**, next in this lane. Retro:
  `docs/lane-experience-kir-native-fix.md`.

### Lane 1.5 — flip the default to native
- **Brief:** Once Lane 1 is in full parity, make the native backend the DEFAULT
  (`kai build` emits a native binary in-process; no flag). The C-text backend
  stays reachable as bootstrap + `--emit=c` oracle, but is no longer what a user
  gets by default. Release distribution: the release tarball/brew formula now
  ships the native compiler with libLLVM static-linked (no `depends_on llvm`,
  Rust/Zig/Julia model); release CI builds against libLLVM, the USER does not need
  it.
- **CI move:** the native backend's parity-vs-C gate becomes the PRIMARY PR gate
  (it now protects the default), the way `tier1` protects C today. While native is
  still opt-in (Lane 1), its gate is a path-gated workflow (`tier1-native.yml`,
  same pattern as the existing `tier1-asan` / `tier1-backend-parity`); at the flip
  it is promoted to the always-on PR gate.
- **selfhost byte-id:** stays on the bootstrap/C path (textual determinism of the
  scaffold). The native default is gated by behavioural parity vs C + a
  reproducible-object check, NOT textual byte-id (a native backend emits no text).
- **Gate:** native parity 388 stays green as the PR gate; bootstrap-from-`cc` still
  works (C path intact); brew install on a clean machine (no LLVM) produces a
  working native-emitting `kai`. Mandatory retro.
- **Deps:** Lane 1.2 (complete native walk + reproducible + in full parity vs
  C-direct over the 388). The flip cannot happen on the Lane 1 bring-up alone.
- **Status (2026-06-09): BLOCKED on parity — the flip did NOT proceed.** The
  premise that the native backend "emits the whole language" held for the 14
  curated `examples/native/` fixtures but FAILED on the corpus. Measured
  native-vs-C-direct over the full corpus (513 fixtures, grown from 388):
  **~250 pass / 168 fail / 96 skip (~60%)** (167 stable + 1 intermittent —
  some native gaps are flaky, e.g. an intermittent SIGSEGV). The gaps are
  real codegen work, not documentation — families: unbound-register 35,
  runtime-type-mismatch 27, missing-symbols 24, build-failed-other 24,
  output-mismatch 20, SIGSEGV/SIGABRT 12+, exit-code-mismatch 11,
  no-effect-handler 10 (Spawn/Clock/NetTcp), timeout 2, clause-param-origin 2.
  ALL 11 `demos/` core programs fail. Full list: `docs/native-parity-gaps.md`;
  retro: `docs/lane-experience-native-default-flip.md`.

  What shipped is the **infrastructure** the flip needs, with the default
  left at `c`:
  - `stage2/Makefile` capability auto-detection (`llvm-config` present →
    native-capable; absent → C-only) + announce line — `tier0`/`tier1` stay
    green without LLVM.
  - `bin/kai` `native` backend wired and reachable opt-in
    (`--backend=native` / `KAI_BACKEND=native`); a C-only kaic2 rejects it
    with an actionable error (no silent fallback to C).
  - `tools/test-backend-parity.sh` parametrised (`TARGET_BACKEND`/
    `ORACLE_BACKEND`) — now gates native-vs-C (the C↔LLVM-text gate it once
    also served was removed with the llvm-text backend); plus a
    `NATIVE_PARITY_RATCHET=1` mode.
  - `tools/native-parity-baseline.txt` — the 167-gap anti-regression
    ratchet (a new gap fails CI; the baseline only tightens). The flip
    re-runs when this file is empty.

  The deferred flip steps (default flip, `tier1-native` → always-on,
  release static-link) are wired-but-dormant; they land when parity closes.

### Lane 2 (deferrable cleanup, OFF critical path) — migrate C-text under KIR
- **Brief:** Only if/when reducing the emit_c↔emit_llvm duplication (§1) is worth
  it on its own. Write a KIR→C consumer; route `--emit=c` through KIR; delete the
  direct `emit_program`/`emit_program_modular` and the duplicated lowering helpers.
- **Gate (strictest, IF this lane is done):** **selfhost byte-id** — C-from-KIR
  textually identical to C-direct over the compiler itself, as a *migration proof*
  (then a follow-up normalization commit may diverge toward cleaner C, validated
  by behaviour + selfhost-determinism, not against the retired path). ASAN,
  parity, KAI_TRACE_RC, #748 modular intact.
- **Deps:** Lane 0. Independent of Lane 1. **Explicitly optional / later** — the
  in-process backend is the production target; the C-text backend already works
  and stays as bootstrap+oracle whether or not it is ever moved under KIR.

### Lane 3 (optional) — effects-on-KIR audit
- **Brief:** With the in-process backend live, audit that `install_order` and
  one-shot resume semantics are correct and that effects behave identically to
  C-direct. Close any effect issue the prior LLVM-text immaturity left open.
- **Gate:** effects fixtures pass identically vs C-direct; ASAN.
- **Deps:** Lane 1.

> **Note — there is deliberately no KIR → LLVM-text lane.** An earlier draft had
> one (a `.ll`-text translator to 388-fixture parity, used as a third oracle).
> It was cut on 2026-06-08: the destination (Lane 1) emits native code via the
> libLLVM C API in-process and never produces `.ll` text, so a text translator
> would be a polished throwaway, and C-direct already gives the only oracle the
> in-process backend needs. The `--emit=llvm` text path served briefly as a
> read-only reference for which LLVM instructions a construct maps to — it was
> never migrated under KIR, and it was **removed on 2026-06-16** once the
> in-process backend's C-API binding was written (see the retirement note above).

**Dependency summary.** The critical path is short: **`Lane 0 → Lane 1`** — the
complete KIR lowering, then the in-process libLLVM backend, validated against the
untouched C-direct oracle. Lane 0 is the only hard prerequisite for Lane 1. The
two remaining lanes are off the critical path: **Lane 2** (migrate C-text under
KIR) is deferrable duplication-cleanup that does not advance the in-process goal;
**Lane 3** (effects audit) follows Lane 1. C-direct stays the permanent
bootstrap+oracle throughout, whether or not Lane 2 is ever done.

### 7.1 Code-quality gate (every lane) — new code must be genuinely good

The standard is **absolute, not relative to the existing compiler.** The repo's F
files (`infer.kai` 11.7K LOC, `emit_c.kai` 8.4K, `cache.kai` avg cognitive 18.6/fn)
are the disease, not the baseline — calibrating against them would just relax. The
proof that kaikai code can score well is in the repo itself: the entire `stdlib/`
is full of **A++/A+** files (`result.kai` A+, `queue.kai` A++, `set.kai` A+), and
even inside the compiler `chars.kai` and `diag.kai` score **A**, `intervals.kai`
and `region.kai` **A−**. That is the bar. KIR is new code written from scratch with
none of the historical debt — it has no excuse to land at F.

| Metric (`km`, per file the lane adds/grows) | Gate | Bar (from the repo's A/A+ files) |
|---|---|---|
| **`km score`** (per file) | **A− or better; never below B+** | stdlib modules are A+/A++; `diag.kai`/`chars.kai` are A; `intervals.kai` A− at 231 LOC |
| File size (LOC of code) | **target < 400; hard cap < 800** | A/A+ files are 9–300 LOC; A− `intervals.kai` is 231; nothing good is over ~300 |
| Cognitive complexity, **average per function** | **< 5; target ≤ 3** | `queue.kai` 0.8, `result.kai` low; only F files climb past 8 |
| Cognitive complexity, **max single function** | **< 25** | A files keep max well under 25; F monsters hit 103–430 |
| Duplication introduced (`km dups`) | **zero new duplicate groups** the lane authored | the point of KIR is to *remove* duplication, not add any |

Notes on measuring:

- **`km score` is the headline gate here** — and it *does* discriminate when the
  file is well-sized: small, well-decomposed files score A, monolocks score F. The
  earlier worry that "km score lands at F even for healthy modules" was an artefact
  of measuring 2–3K-LOC modules; at < 400 LOC the score reflects real quality.
  Cross-check with `km cogcom` avg/max per function.
- A file approaching the size cap is split **before** it grows, not after. KIR's
  natural seams: `kir.kai` (types only), `kir_lower.kai` (AST→KIR), and per-target
  translators each split by node family (terminators / ops / RC / types) if one
  approaches 400 LOC. Aim for several A-grade files, not one big one.
- The dumb-translator design makes this easy, not hard: a translator that is one
  exhaustive `match` over ~28 nodes with one print rule each is naturally
  low-complexity. If a translator's avg cognitive climbs past 5, logic leaked from
  the lowering into the translator — move it back into AST→KIR (also the correctness
  goal). High km numbers here are a *design smell*, not an accepted cost.

**Binary gate, like byte-id:** a lane that ships a file scoring below B+ (or over
the size cap) does not close until it is split or simplified. The new subsystem
lands as a cluster of A-grade files or it does not land. We do not inherit the
monolith's F; we set the example the monolith failed to.

### 7.2 The horizon — KIR → in-process libLLVM (strategic target)

The project's real codegen goal is NOT "two text emitters under one IR." It is a
**fast self-hosted compiler that emits native code without invoking an external
toolchain**, while reusing LLVM's optimiser / register allocator / instruction
selection / object emission (and, later, WASM) **as a library**. Today both
backends are out-of-process: the C backend writes `.c` and forks `cc`; the LLVM
backend writes `.ll` text and invokes `clang`. Each is `fork+exec` + disk write +
re-parse — the exact overhead that fights the "fast compiler" goal. The horizon
removes it: kaikai **links libLLVM**, builds the module via the C API in memory,
and emits the object in one process.

KIR is the launch platform for this. ANF + explicit basic blocks + typed
terminators + Perceus as first-class `KRC` nodes is *precisely* the shape the LLVM
`IRBuilder` consumes — each KIR node maps to one or two `LLVMBuild*` calls. **This
retroactively validates KIR's core decisions:** a string-encoded sentinel
(`__kai_tcrec|…`) is trivial to print to text but hostile to translate into C-API
calls; promoting TCO/TRMC/Perceus to typed nodes (§2, §4.7) is what makes the
in-process path tractable. KIR was built for this destination without it being the
stated goal.

**Feasibility verdict (asu review, 2026-06-08): viable today, with one repr fix.**

1. **Vehicle: Path-1 prelude primitives — but handles are a non-RC newtype, NOT
   `Int`.** The compiler-internal prelude mechanism (`docs/ffi.md` Path 1: runtime
   forwarder + thunk + typer registration, the 3-site pattern stdlib already uses
   for libm/POSIX) is the right vehicle, and the wiring does not explode — the LLVM
   C API surface KIR needs is ~30–60 functions, not hundreds. **But** passing an
   `LLVMValueRef` as `Int` is a soundness trap: stage2's tagged-Int runtime boxes
   small Ints with a tag bit (see `project_kaikai_two_runtimes_int_tagging_trap`),
   and a 48-bit pointer that loses a bit to tagging is silent IR corruption — the
   worst bug class. The fix (as Zig/Crystal/Julia do in their C-API bindings): a
   **non-RC opaque scalar newtype** for handles, carried unboxed, never through the
   `Int` repr. This is the one new repr piece the horizon needs.

2. **Bootstrap: preserved by the three-stage architecture.** Linking libLLVM in
   stage2 does NOT break "bootstrap from any machine with `cc`" — stages 0–1 and
   the C backend touch no libLLVM, and bootstrap always uses the C path even after
   native becomes the user default. The C-text backend remains the **permanent
   pure-bootstrap path and differential oracle** (it stops being the user-facing
   default but never the bootstrap). Tier 1.3's "no deps in stage 0" is intact
   (only stage2 links libLLVM, and the distributed binary static-links it so users
   need no LLVM). This is the escape Zig and Crystal did not have: they became
   non-bootstrappable on fresh
   machines precisely by putting libLLVM in the arrival path. kaikai's stage split
   is the structural defence.

3. **Perceus: a non-issue once handles are unboxed non-RC.** The translator runs
   under Perceus (it is self-hosted compiler code, and must stay so). The defence is
   representational — the newtype of trap #1 carries no RC — not "run outside
   Perceus." An LLVM handle is a raw pointer whose lifetime the `LLVMContext` /
   `Module` owns, never Perceus. Gate: `KAI_TRACE_RC` over the translator must show
   zero refs carrying the handle newtype's repr.

4. **Ordering: go straight from KIR to in-process; no LLVM-text lane.** An earlier
   draft proposed building a KIR→LLVM-**text** translator to parity first, as a
   third differential oracle, then pivoting. That was cut (2026-06-08): a text
   translator emits `.ll` the destination never produces, so it is a polished
   throwaway, and it would not even isolate the failure modes that worry us — a
   text path exercises no FFI and no link/codegen, so it cannot catch a C-API
   binding or ABI bug. **C-direct is the sufficient oracle** (it validates
   behaviour). Isolation against the four error sources (KIR lowering, C-API
   binding, FFI ABI, link/codegen) comes from **building the in-process backend
   incrementally** — constant `main`, then arithmetic, calls, control flow,
   ctors/match, RC, effects — each step diffed against C-direct. The existing
   `--emit=llvm` text path is kept untouched only as a read-only reference for
   which LLVM instruction each construct maps to while writing the binding.

**LLVM version policy.** Use the latest stable LLVM that ships a linkable
`libLLVM` + `llvm-config` — today Homebrew `llvm` (22.x). Apple's bundled clang
(clang-2100 / "21") does NOT expose `libLLVM.dylib` or `llvm-config`, so it cannot
be linked in-process; it stays fine for the pure-bootstrap C path. Discover the
LLVM location via `llvm-config --version/--libdir/--cflags` at build time rather
than hardcoding a version — the LLVM **C API is stable across major versions**
(far more so than the C++ API), so the binding survives version bumps, and
discovery keeps the build off Apple clang and portable to Linux/CI. The in-process
backend's libLLVM dependency is opt-in; it never enters the bootstrap chain.

---

## 8. Anchors in the current code

- `stage2/compiler/ast.kai` — ExprKind + ValueMode (what KIR replaces/types).
- `stage2/compiler/perceus.kai:1770-1780` — tail-position rationale (Risk 1);
  `__perceus_*` injectors at `:1737` (dup), `:3635` (drop), `:486` (borrow) — grep
  `EVar("__perceus_` for the current set, line numbers drift.
- `stage2/compiler/emit_c.kai:1820+` — reuse lowering; `:8416-8637` — effect
  install; `:1982+` — TRMC sentinel decode (`tcrec_split_pipe`).
- `stage2/compiler/emit_llvm.kai:2527+` — reuse; `:3030-3053,7701+,7745+,7827+` —
  tcrec/trmc sentinel decode (`es_tcrec_split_pipe`) — the duplicated mirror.
- `stage2/compiler/emit_shared.kai` — the shared *analysis* (stays); the shared
  *lowering* is what KIR creates.
- `stage2/compiler/driver.kai:1343-1358` — `--emit=` dispatch (where `--emit=kir`
  and the `-kir` backend flags hook in); `:5232-5268` — pipeline order.
- `tools/test-backend-parity.sh` — the native-vs-C differential harness the
  gates extend.

For the §7.2 in-process backend (Lane 1):
- `docs/ffi.md` Path 1 — the compiler-internal prelude-primitive mechanism (runtime
  forwarder + thunk + typer registration) that the libLLVM C-API binding rides.
- `stage0/runtime_llvm.c` (1243 LOC) — the `kaix_prelude_*` C forwarders the native
  object links against (originally the llvm-text backend's, now shared with and
  owned by the in-process backend).
- the in-process backend links stage2 against an `llvm-config`-driven static
  `libLLVM` (opt-in via `make -C stage2 KAI_LLVM=1`), building the module via the C
  API in memory and emitting a native object — no out-of-process `clang`-on-`.ll`
  round-trip.

---

## 9. Bottom line

KIR is ANF + basic blocks + named registers (no SSA, no materialised CFG), with
Perceus as first-class `KRC` nodes and the TCO/TRMC sentinels promoted from strings
to typed terminators. It does not invent new semantics — it materialises the
clandestine IR perceus already encodes as strings over the AST. The single AST→KIR
lowering carries the hard semantics (effects, continuations, drops, TCO/TRMC,
reuse) once, validated for correctness against C-direct, the mature backend that
stays the untouched oracle and bootstrap path. From that one lowering, the
in-process libLLVM backend (Lane 1) is the destination consumer. Closing the
duplicated AST→target lowering between `emit_c` and `emit_llvm` (by migrating
C-text under KIR, Lane 2) is a deferrable second prize, not the goal.

And it lands as **A-grade code** (§7.1): new, debt-free, written to the standard
the repo's `stdlib` and `chars`/`diag` modules already meet — not to the F of the
monoliths it replaces. We do not inherit the monolith's metrics; KIR is the chance
to set the example they failed to.

**Beyond unification — the horizon (§7.2).** Closing the Conway duplication is the
near-term payoff, but it is not the endgame. The endgame is a fast self-hosted
compiler that emits native code **in-process** via libLLVM-as-a-library — no `.ll`
text, no `clang` subprocess — reusing LLVM's optimiser and codegen (and WASM later)
while owning the fast inner dev loop. KIR is the launch platform for that backend
(Lane 1): its typed, ANF, basic-block shape maps directly onto the LLVM C API. The
critical path is short — complete the KIR lowering (Lane 0), then build the
in-process backend (Lane 1), validated against C-direct, which stays the permanent
pure-bootstrap oracle. There is deliberately no KIR→LLVM-**text** translator: it
would emit `.ll` the destination never produces and C-direct is the only oracle the
in-process backend needs. Migrating the C-text backend under KIR (Lane 2) is
deferrable duplication-cleanup, off the critical path. Read the whole plan with
that ordering of importance.
