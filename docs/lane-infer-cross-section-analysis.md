# Infer / monomorph / perceus / unbox / tco cross-section analysis — issue #677 phase 1n pre-flight

**Date:** 2026-05-25
**Branch:** `infer-analysis` (research-only)
**Base:** `main` HEAD `5750056` (post-#691 desugar-extract)
**Author:** lane research agent
**Goal:** map the real sub-layers inside the `# bidirectional inference (M2d)` →
`# LLVM backend` block of `stage2/main.kai` and propose the extraction
order for the remaining vertical-pipeline lanes. **No code moved.**

Precedent: `docs/lane-parse-cross-section-analysis.md` (PR #684, analysis +
re-scope), `docs/lane-driver-cross-section-analysis.md` (PR #687, apex is
last), `docs/lane-experience-issue-677-phase1m.md` (AST-sink rule,
`dsg_` mirror discipline, emit-coupled sub-pass stays in main).

## TL;DR

The brief's range (16259–34037, ~17 780 LOC, 1 000 defs, 0 `pub`) is **not
one layer and not even one homogeneous block.** It is a clean **downward
DAG of seven sub-layers** with *zero* back-edges between them — every layer
is independently extractable in principle. Three findings reshape the cut:

1. **The infer module's real boundary starts at 12333, not 16259.** The
   HM type-scheme/environment core (M2b, 12333) and the
   substitution/unification + unit-ops core (M2c, 14844) live *just above*
   the brief's range and are the foundation `synth`/`infer` (M2d) is built
   on. They form one cohesive `compiler/infer.kai` with M2d. Extracting
   M2d alone would force M2b+M2c `pub` upward — the parse-analysis mistake
   in reverse.

2. **monomorph / unbox / perceus are pure `[Decl] → [Decl]` structural
   passes** that do **not** consume any typer type (`InferState`,
   `Subst`, `TyScheme`, `TypedProgram`, `ProtocolReg`: **0 uses each** in
   the entire 27431–34037 region). This directly refutes the brief's
   "perceus depends on inference type info" hypothesis: Perceus's last-use
   analysis reads structure + the `.ty` already embedded in `Expr` nodes
   (and `Expr`/`Ty` are already in `compiler.ast`). Perceus does **not**
   import the typer. unbox/perceus extract as standalone modules.

3. **TCO (`tcrec`) is emit-coupled, not a `[Decl]→[Decl]` peer.** Despite
   the `tcrec_rewrite_decls(decls) : [Decl]` signature, the body mints C
   strings (`__kai_tcrec|<c_sym>|…`) and calls `emit_expr` / `emit_expr_raw`
   / `c_sym` / `raw_c_type` (the emit_c layer, defined at head 943/5451/
   5637). TCO is a **pre-pass of the C emitter**, the phase-1m sub-pass-F
   pattern again: it stays in main and extracts *with* emit_c, not with
   perceus.

4. **The dumps/holes/effects-json tail (33266–34037) is a transverse
   diagnostic layer**, not a pipeline pass. It depends on the HM engine
   (`apply_ty`/`instantiate`/`unify`/`ty_to_string`/`scheme_to_string`).
   It travels with infer or with the driver, like `dump_program`.

Recommended order: **infer (M2b+M2c+M2d) → monomorph → unbox → perceus**,
each its own lane. TCO and dumps defer to the emit / driver lanes. The
protocols system (39167–42630, *outside* the range) is the natural
sibling lane after infer.

## Sub-section inventory

Verified entry-point definitions (`grep -nE "^(pub )?(fn|type) "`), not
header comments — main.kai still has every pass inlined.

| # | Logical layer | Lines | Entry-point | defs | Extract as |
|---|---------------|-------|-------------|------|------------|
| L0a | HM type schemes + env (M2b) | 12333–14843 | `TyScheme`, `TyEnv`, `instantiate`, `generalize` | ~120 | `compiler/infer.kai` |
| L0b | substitution + unification + unit-ops (M2c + m12.5) | 14844–16258 | `Subst`, `apply_ty`, `apply_row`, `unify`, `fresh_var` | ~160 | `compiler/infer.kai` |
| L1 | bidirectional inference (M2d) | 16259–24118 | `synth` (17641), `synth_*` family | ~430 | `compiler/infer.kai` |
| L2 | infer driver + effect-check + decl loop | 24119–27430 | `infer_decl` (24119), `infer_program` (26123), `infer_program_with_protos_cached` (26647) | ~210 | `compiler/infer.kai` |
| L3 | monomorphisation (m4c) | 27431–28551 | `monomorphise` (27431) | ~78 | `compiler/monomorph.kai` |
| L4 | unboxing (m6) | 28552–29451 | `unbox_pass` (28552) | ~50 | `compiler/unbox.kai` |
| L5 | Perceus RC (m5) | 29452–32114 | `perceus_pass` (29452) | ~150 | `compiler/perceus.kai` |
| L6 | TCO (tail-call rewrite) | 32115–33265 | `tcrec_rewrite_decls` (32143) | ~70 | **stays in main → emit_c lane** |
| L7 | dumps / holes / effects-json | 33266–34037 | `dump_holes`, `dump_holes_json`, `dump_effects_json`, `dump_intervals`, `dump_last_use`, `strict_holes_check` | ~70 | with infer **or** driver |

`# LLVM backend` begins at 34038 (out of range, the emit_llvm lane).

### Layer boundaries are header-anchored above 16259

The three M2 sub-layers carry real `# =====` dividers — use these as
stable anchors when the line numbers drift:

```
# type schemes + environment (M2b)        → 12333
# substitution + unification (M2c)         → 14844
#   m12.5 — unit-of-measure operations     → 14985  (rides M2c)
# bidirectional inference (M2d)            → 16259  (brief's start)
```

Below 16259 there are **no `# =====` major dividers at all** until
`# LLVM backend` at 34038 — the brief was right. L3–L7 boundaries were
recovered purely from the callee graph + the entry-point signatures, not
from comments.

## Cross-layer callee graph

### No back-edges anywhere in the pipeline

The decisive measurement. For every later layer's entry-point, count
callers in earlier layers (a back-edge = a cycle that blocks separate
extraction):

```
infer (16259–27430) → {monomorphise, unbox_pass, perceus_pass,
                        tcrec_rewrite_decls, unbox_expr, perceus_decl}:  0 hits
monomorph/unbox/perceus → {synth, infer_decl, infer_program, st_unify}:  0 hits
perceus → unbox_* :  0 ;  tco → perceus_*/unbox_* :  0 ;
monomorph → perceus_*/tcrec_* :  0
```

The four lower passes **never call each other** and **never call the
typer.** The driver orchestrates them in sequence
(`infer → monomorph → unbox → perceus → tco → emit`); the data flow is a
straight `[Decl]` pipe. This is the cleanest topology of any block
analysed so far — cleaner than the driver (mutually-recursive A–E) and
cleaner than parse (refinement engine tangled inside).

> The only grep "back-edges" from infer into the L3–L7 range are
> `list_has_int` / `list_has_string` / `ty_eq` / `unit_eq` — four trivial
> generic helpers that happen to be *defined* in the monomorph zone but
> are used everywhere. They are downward glue (the phase-1m `dsg_` mirror
> case), not pipeline coupling. Promote to `compiler.util` or mirror.

### Outbound: what each layer needs from outside

Per-layer unique call targets, classified (comment-stripped scan):

| Layer | unique calls | internal | → head/modules | head symbols (the coupling) |
|-------|-------------|----------|----------------|------------------------------|
| infer L1 (M2d) | 697 | 546 | 76 | HM engine (`apply_ty`, `apply_row`, `unify`, `instantiate`, `fresh_var`) — **all in L0a/L0b**, plus AST-functor glue |
| monomorph L3 | 82 | 62 | 9 | `mangle_name`, `map_expr_kind`, `fn_scheme_of_decl`, `dim_collapse`, `unit_canon`, `normalize_union`, `collect_implicit_tparams_in_decl`, `tparam_id_split`, `unit_expr_display` |
| unbox L4 | 76 | 67 | 3 | `efn_resolve`, `lookup_ufn_sig`, `register_fn_decls` |
| perceus L5 | 160 | 136 | 8 | `map_expr_kind`, `pat_bindings`, `pat_bindings_skip_raw`, `register_fn_decls`, `is_ufn_decl`, `mangle_ty`, `body_is_ffi_extern`, `iscan_collect` |
| tco L6 | 84 | 55 | 7 | **`emit_expr`, `emit_expr_raw`, `c_sym`, `raw_c_type`** (emit_c!), `lookup_ufn_by_csym`, `pat_bindings`, `string_starts_with_q` |
| dumps L7 | 88 | 53 | 7 | **`apply_ty`, `instantiate`, `unify`, `ty_to_string`, `scheme_to_string`, `subst_empty`** (HM engine), `join_with` |

The pattern: L3/L4/L5 reach head only for **AST-functor glue**
(`map_expr_kind`, `pat_bindings` — exactly the phase-1m mirror families)
and **fn-registration helpers** (`register_fn_decls`, `efn_resolve`,
`lookup_ufn_sig`). These are small, mechanical, and already have a
mirroring precedent. L6 reaches the emitter; L7 reaches the HM engine.

### The typer ↔ protocols arrow is weak and by-value

`synth_binop_proto` (19738) calls only `proto_dispatcher_name` (a pure
naming helper, def 39291) and `binop_proto_method`. The infer driver
takes `[ProtoImplReg]` and `ProtocolReg` as **parameters**
(`infer_program_with_protos_cached`, 26647) — injected by the driver, not
fetched. The typer never calls the protocol registry's *body*
(`proto_op_lookup`, `register_proto_ops`, `validate_impls`, `lower_impls`
all live in 39167+ and have **0 callers** inside the infer range). So
infer and protocols couple only through the `Proto*Reg` **types**, which
makes the two layers extractable in **either order** provided those types
sink to a shared module (below).

## Public-surface estimate

Inbound external callers (driver/emit, outside 16259–34037), counted:

| Module | pub fns (entry points) | pub types (in signatures) | est. total |
|--------|------------------------|---------------------------|------------|
| `compiler/infer.kai` | `infer_program` (2), `infer_program_with_protos` (driver), `infer_program_with_protos_cached` (1) — plus `synth`/`apply_ty`/`instantiate`/`unify`/`ty_to_string`/`scheme_to_string` re-pub'd for L7 dumps & the HM engine the emitter shares | `TypedProgram`, `Ty`* (ast), `TyScheme`, `TyEnv`, `TyEntry`, `Subst`, `InferState`, `Inferred`, `ResolvedCS`, `Instantiated`, `HoleKind`, … | **~18–30** |
| `compiler/monomorph.kai` | `monomorphise` (1 caller) | `MonoOutput`, `MonoTuple`, `MLeakRecord`, `GenSpecsResult`, `SpecEmitOut`, `SubstMap`, … (returned in sigs) | **~8–12** |
| `compiler/unbox.kai` | `unbox_pass` (1) | `UbStmts`/`UbStmt` (internal), `LocBind` | **~2–4** |
| `compiler/perceus.kai` | `perceus_pass` (1) | `Use`, `LocBind` (or shared) | **~2–4** |

`infer.kai` is the heavy module — its public surface is large not because
the driver calls many entry points (it calls ~3) but because **the HM
engine (`apply_ty`/`instantiate`/`unify`/`ty_to_string`/`scheme_to_string`)
is shared with the emitter and the dumps layer**, and **the typer's
result types cross into monomorph and emit**. Budget the pub-enforcement
round: kaic2's own modular compile will flag every result type returned by
a pub entry but not itself `pub` (the six-type surprise from phase 1m).

monomorph/unbox/perceus are textbook small-surface cuts: one entry point,
a handful of result types.

## Cycle detection — summary

**No cycles.** The block is a DAG:

```
L0a/L0b (HM core) → L1 (synth) → L2 (infer driver) ─┬─→ L3 monomorph → L4 unbox → L5 perceus → (L6 tco) → emit
                          ↑                          │
                    proto types (by value, injected) │
                                                      └─→ L7 dumps (reads HM engine)
```

The only intra-infer "back-edges" (L0a/L0b → a few fns physically in
L1/L2: `option_str_eq` 16339, `module_slot_compat` 16361, `st_set_sub`
16631, `ty_eq_shape` 16772, `extract_return_tycon` 23001,
`ty_env_add_decl_marker` 23162, `variants_of_type` 23195) are **internal
to the future `infer.kai`** — they vanish the moment L0a+L0b+L1+L2 move as
one module. They are not a reason to split infer; they are a reason **not**
to split it.

## AST-sink rule check (the phase-1m `RecInfo`/`OpAr` discipline)

The load-bearing question: which shared types cross layers and must sink
to `compiler.ast` (the only module every layer imports without a lateral
arrow)?

**Already sunk (good news):** `Ty`, `Row`, `Label`, `Expr`, `ExprKind`,
`TypeExpr`, `UnitExprT`, `Decl`, `Arm`, `Param`, `Stmt` are **already in
`compiler.ast`** (lines 67–589). `RecInfo` was sunk in phase 1m. So the
*AST-level* sink rule is satisfied — the typer and every pass read AST
nodes from `ast.kai` today.

**Still in main head, typer-specific — candidates to evaluate per lane:**

| Type | Def | Crosses to | Recommendation |
|------|-----|------------|----------------|
| `TyScheme` (`Scheme`) | 12351 | infer, monomorph (`fn_scheme_of_decl`), emit, L7 dumps | **moves with `infer.kai`**; re-pub. Not an ast sink — it is the infer module's own currency, consumed downstream by-value. |
| `Subst` | 14887 | infer-internal + emitter (`apply_ty` re-parse paths) | moves with `infer.kai`, re-pub for the emitter. |
| `TyEnv`, `TyEntry` | 12391/12357 | infer-internal + L7 dumps | move with `infer.kai`. |
| `InferState`, `Inferred` | 16490/16587 | infer-internal only | move with `infer.kai`, stay private if no external caller. |
| `TypedProgram` | 24929 | infer → monomorph → driver | moves with `infer.kai`, **pub** (monomorph & driver consume it). |
| `ResolvedCS` | 23887 | infer → monomorph (`insts`) | moves with `infer.kai`, pub. |
| `ProtoImplReg`, `ProtocolReg` | 39212/39226 (protocols zone) | injected into `infer_program_with_protos_cached`; defined in the protocols block | **THIS is the phase-1n `RecInfo`.** A protocols *type* sits in an infer *public signature*. Per the asu rule (type in a public sig consumed by two layers → sinks to the module both import without a lateral arrow), `ProtoImplReg`/`ProtocolReg` should sink to `compiler.ast` (or a new `compiler.proto_types`) **before** either infer or protocols extracts — otherwise `infer.kai` must `import compiler.protocols` sideways, the exact inversion the rule forbids. |
| `MonoTuple`, `MonoOutput`, `MLeakRecord` | 27614/66/… | monomorph-internal; `MLeakRecord` defined at head line 66 already | move with `monomorph.kai`; `MLeakRecord` is already head-global, leave or sink. |

**The one mandatory pre-lane sink: `ProtoImplReg` + `ProtocolReg`** (and
`ProtoOpReg`/`ProtoTPReg`/`ProtoOriginReg` they aggregate). They are the
`RecInfo` of this phase: a protocols-layer type embedded in an infer
public signature. Sink them to `ast.kai` first; then infer and protocols
each import them downward.

**Mirror-able downward glue (the `dsg_`/`mods_` precedent):**
`map_expr_kind` + the AST-functor family, `pat_bindings` /
`pat_bindings_skip_raw`, `register_fn_decls`, `efn_resolve`,
`lookup_ufn_sig`, `ty_eq` / `unit_eq` / `list_has_int` / `list_has_string`,
`string_starts_with_q`. Each extracted module mirrors what it needs with
an `inf_`/`mono_`/`ubx_`/`prc_` prefix (avoiding duplicate C symbols in
the flat bundle), exactly as phase 1m did.

## Recommended extraction order

Five lanes. **Lane N0 is a prerequisite; N1 is the big one; N2–N4 are
mechanical once N1 lands.**

### Lane N0 — sink the protocol-registry types (one PR, ~40 LOC moved)

- Move `ProtoOpReg`, `ProtoTPReg`, `ProtoImplReg`, `ProtoOriginReg`,
  `ProtocolReg` (39202–39230) into `compiler/ast.kai` (or a small
  `compiler/proto_types.kai` if ast.kai is felt to be overloaded).
- Rationale: they appear in `infer_program_with_protos_cached`'s
  signature (the infer public surface) **and** are produced by the
  protocols block. Sinking first means neither the infer lane nor the
  protocols lane needs a lateral import.
- Acceptance: tier1 green, byte-identical selfhost (pure relocation).

### Lane N1 — extract `compiler/infer.kai` (one PR, ~10 500 LOC moved)

- Move **L0a + L0b + L1 + L2** as one module:
  `# type schemes + environment (M2b)` (12333) through the end of the
  infer driver (~27430). This is the single biggest lane in the whole
  modularization effort.
- **Take L7 dumps with it** (33266–34037) **or** leave them for the
  driver lane — they read the HM engine, so co-locating them in
  `infer.kai` keeps `apply_ty`/`ty_to_string`/`scheme_to_string` private.
  Recommendation: **take L7 into infer.kai** (the dumps are typed-AST
  queries, the same family as `--infer`). Note L6 (TCO) sits physically
  *between* L5 and L7; pulling L7 means skipping over L6, which stays.
- Public surface: ~18–30 (the 3 entry points + the HM engine the emitter
  re-uses + the result types). Budget a pub-enforcement round.
- Mirror the AST-functor + fn-registration glue with an `inf_` prefix.
- Acceptance: tier1 green, **byte-identical selfhost** (the typer runs on
  every prelude — a self-compile mismatch localises instantly).

### Lane N2 — extract `compiler/monomorph.kai` (one PR, ~1 200 LOC)

- Move L3 (27431–28551). Imports `compiler.infer` (for `TypedProgram`,
  `ResolvedCS`, `TyScheme`) and `compiler.ast` (for the `Proto*Reg` sunk
  in N0). One entry point `monomorphise`; ~8–12 pub.
- Mirror `map_expr_kind`, `mangle_name`, `unit_canon`, etc.
- Acceptance: tier1 green, byte-identical selfhost.

### Lane N3 — extract `compiler/unbox.kai` (one PR, ~700 LOC)

- Move L4 (28552–29451). Tiny pub surface (`unbox_pass` + maybe
  `LocBind`). Imports `compiler.ast` only; `[Decl]→[Decl]`. The easiest
  of the five.
- Acceptance: tier1 green, byte-identical selfhost.

### Lane N4 — extract `compiler/perceus.kai` (one PR, ~2 100 LOC)

- Move L5 (29452–32114). `[Decl]→[Decl]`, imports `compiler.ast`.
  **Confirmed: does not import the typer** — its last-use analysis is
  structural over `Expr` + the embedded `.ty`. One entry point
  `perceus_pass`; `Use`/`LocBind` either move with it or sink to util.
- Acceptance: tier1 green, byte-identical selfhost (perceus runs on every
  build).

### Deferred to the emit / driver lanes (not phase 1n)

- **L6 TCO** stays in main until the **emit_c lane**. It calls
  `emit_expr`/`emit_expr_raw`/`c_sym`/`raw_c_type` and mints C goto
  strings — it is a C-emitter pre-pass, the phase-1m sub-pass-F pattern.
  Extracting it before emit_c would invert emit→tco into tco→emit.
- **The protocols block (39167–42630)** is the natural sibling lane to
  N1 — `collect_proto_decls`, `validate_impls`, `lower_impls`,
  `resolve_protocol_calls`, the `#[derive(…)]` synthesisers. After N0
  sinks the `Proto*Reg` types, it extracts as `compiler/protocols.kai`
  with infer and protocols importing the shared types downward. Sequence
  it **after N1** (so `TypedProgram` is already in `infer.kai` for
  `resolve_protocol_calls(typed: TypedProgram, …)`).
- **The driver** stays apex / last, per PR #687.

### Why this order

- **N0 first** removes the one lateral type arrow (the `Proto*Reg` in the
  infer signature) before any module is born — the `RecInfo`-sink lesson
  from phase 1m, applied *ahead* of the cut instead of mid-cut.
- **N1 before N2–N4** because monomorph consumes `TypedProgram` /
  `ResolvedCS` / `TyScheme` from the typer; those must already live in
  `infer.kai` so monomorph imports them downward.
- **N2/N3/N4 are independent of each other** (no inter-pass calls). They
  can ship in any order, or in parallel worktrees, once N1 lands. unbox
  (N3) is the smallest and a good warm-up.
- **TCO and protocols are not phase-1n** — TCO is emit-coupled, protocols
  is its own large layer outside the range.

## What NOT to do

- Do **not** extract M2d (16259+) alone. M2b+M2c (12333–16258) are its
  foundation; splitting them forces the HM engine `pub` upward and births
  an `infer.kai` that back-imports its own substitution core. Extract
  12333–27430 as **one** module.
- Do **not** put `perceus.kai` behind `infer.kai` expecting a type
  dependency — there is none. Perceus imports `ast`, not `infer`.
- Do **not** extract TCO with perceus just because they are physically
  adjacent (L5 then L6). TCO belongs to emit_c.
- Do **not** leave `ProtoImplReg`/`ProtocolReg` in the protocols block
  and have `infer.kai` import protocols sideways. Sink the types (N0).
- Do **not** bundle N1 with N2–N4. A ~13 000-LOC multi-module diff with
  new public surfaces makes a selfhost-byte mismatch un-bisectable.

## Verification commands a reviewer can run

```sh
# layer anchors (the three M2 dividers above the brief's range)
grep -nE "^# (type schemes|substitution|bidirectional|LLVM backend)" stage2/main.kai

# entry points
grep -nE "^fn (synth|infer_decl|infer_program|monomorphise|unbox_pass|perceus_pass|tcrec_rewrite_decls)\b" stage2/main.kai

# the decisive no-back-edge check (must print nothing)
for s in monomorphise unbox_pass perceus_pass tcrec_rewrite_decls; do
  grep -nE "\b$s\b *\(" stage2/main.kai | awk -F: '$1>=16259 && $1<=27430'
done

# passes are [Decl]->[Decl] (no typer types) — must all be 0
for t in InferState Subst TyScheme TypedProgram ProtocolReg; do
  echo -n "$t: "; grep -nE "\b$t\b" stage2/main.kai | awk -F: '$1>=27431 && $1<=34037' | wc -l
done

# TCO is emit-coupled (prints emit_expr / c_sym sites)
grep -nE "\b(emit_expr|emit_expr_raw|c_sym|raw_c_type)\b" stage2/main.kai | awk -F: '$1>=32115 && $1<=33265'

# the Proto*Reg types sit in the protocols block, used by an infer signature
grep -nE "^type Proto(Op|TP|Impl|Origin)?Reg" stage2/main.kai
grep -n "proto_impls: \[ProtoImplReg\]" stage2/main.kai
```

## Notes for the integrator

- This PR contains **only this analysis doc** + the lane retro — no code
  moved, no `main.kai` import line, no Makefile change.
- The decision you own: accept the **N0 → N1 → {N2,N3,N4}** sequencing, or
  re-scope. The data supports infer-as-one-module (12333–27430) and the
  three lower passes as independent small cuts.
- The line numbers drift on the next merge; the **`# =====` headers**
  (`type schemes`, `substitution`, `bidirectional inference`,
  `LLVM backend`) and the entry-point fn names are the stable anchors.
- I did **not** run `make selfhost` or `make tier1` — research-only lane,
  zero `.kai` source changed.
