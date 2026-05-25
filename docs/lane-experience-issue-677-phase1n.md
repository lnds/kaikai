# Lane experience — issue #677 Phase 1n: analyze the infer/monomorph/perceus/unbox/tco block

**Lane branch:** `infer-analysis`
**Closes:** issue #677 Phase 1n — cross-section analysis of the
`# bidirectional inference (M2d)` → `# LLVM backend` block, recommending
the extraction order for the remaining vertical-pipeline lanes.
**Predecessors:** Phase 1h parse-analysis (PR #684), 1k driver-analysis
(PR #687), 1m desugar-extract (PR #691).

## Scope as planned vs as shipped

**Planned:** map the sub-layers of lines 16259–34037 (~17 800 LOC,
post-#691), produce a cross-section doc recommending extraction order,
**no code moved.** Default case = analysis-only.

**Shipped:** exactly that — `docs/lane-infer-cross-section-analysis.md`
(~300 lines) + this retro. **No code moved**, no `.kai` source touched.
The analysis identified **4 cleanly-extractable modules**
(`infer.kai`, `monomorph.kai`, `unbox.kai`, `perceus.kai`) plus one
prerequisite type-sink lane (N0) and two deferred layers (TCO → emit_c,
protocols → its own lane). No "easy sub-cut extracted in the same PR" —
unlike parse-analysis, every layer here is large enough to deserve its
own lane, and the one trivial cut (N0, the `Proto*Reg` sink) is a
*prerequisite* whose value is sequencing, so it belongs to its own
focused PR with its own selfhost gate.

## The load-bearing findings (callee graph beat the headers)

The brief warned the block has no `# =====` dividers between sub-layers
and that the callee graph, not header comments, is the source of truth.
Confirmed and then some — four findings reshaped the cut:

### 1. The infer module starts at 12333, not 16259

The brief's range starts at M2d (`# bidirectional inference`, 16259). But
`synth`/`infer` are built on the HM type-scheme/env core (M2b, 12333) and
the substitution/unification core (M2c, 14844) **just above** the range.
`synth`'s 76 head-bound calls are dominated by `apply_ty`/`apply_row`/
`unify`/`instantiate`/`fresh_var` — all in M2b/M2c. Extracting M2d alone
would `pub`-export the HM engine upward into `main.kai`: the
parse-analysis mistake (extract-the-symptom-not-the-layer) in reverse.
**`compiler/infer.kai` = M2b+M2c+M2d+L2 as one module, 12333–27430.**

### 2. monomorph / unbox / perceus consume ZERO typer types

The brief's sharp-edge said "perceus depends on inference type info —
confirm." It does not. Measured: `InferState`, `Subst`, `TyScheme`,
`TypedProgram`, `ProtocolReg` each have **0 uses** in the whole
27431–34037 region. `unbox_pass`/`perceus_pass`/`tcrec_rewrite_decls` are
all `[Decl] → [Decl]`. Perceus's last-use analysis is **structural** over
`Expr` + the `.ty` already embedded in nodes (and `Expr`/`Ty` are already
in `compiler.ast`). So `perceus.kai` imports `ast`, **not** `infer`. This
refutation is the single most useful result for the next lanes — it
unblocks N3/N4 as standalone small cuts instead of infer-coupled ones.

### 3. TCO is emit-coupled — the phase-1m sub-pass-F pattern, again

`tcrec_rewrite_decls(decls) : [Decl]` *looks* like a peer of perceus, but
its body mints C strings (`__kai_tcrec|<c_sym>|…`) and calls `emit_expr` /
`emit_expr_raw` / `c_sym` / `raw_c_type` (emit_c, head 943/5451/5637). TCO
is a C-emitter pre-pass. It stays in main and extracts **with emit_c**,
not with perceus — exactly the reasoning that kept sub-pass F (interp
lift) in main in phase 1m. The lesson generalises: **a `[Decl]→[Decl]`
signature does not prove a pure structural pass; grep the body for emit
calls before classifying.**

### 4. The typer↔protocols arrow is by-value and weak

`infer_program_with_protos_cached` takes `[ProtoImplReg]` as a
*parameter*; `synth_binop_proto` calls only `proto_dispatcher_name` (a
naming helper). The typer never calls the protocol registry's body
(`proto_op_lookup`/`validate_impls`/`lower_impls` have 0 infer-range
callers). So infer and protocols couple **only through the `Proto*Reg`
types** — which makes them order-independent *if* those types sink first.

## AST-sink rule — phase 1n's `RecInfo` is `ProtocolReg`

The phase-1m discipline (a type in a public signature consumed by two
layers sinks to `ast.kai`) found its phase-1n instance: `ProtoImplReg` /
`ProtocolReg` are defined in the protocols block (39202+) but appear in an
**infer public signature**. They are this lane's `RecInfo`/`OpAr`. The
recommendation pre-sinks them (lane N0) **before** either infer or
protocols extracts, so neither needs a lateral import — applying the
phase-1m lesson *ahead* of the cut instead of mid-cut, which is the
refinement this analysis contributes to the pattern.

Good news on the AST level: `Ty`, `Row`, `Label`, `Expr`, `ExprKind`,
`TypeExpr`, `UnitExprT`, `Decl`, `RecInfo` are **already** in `ast.kai`
(589 + the phase-1m `RecInfo` move). The typer-specific types
(`TyScheme`, `Subst`, `TyEnv`, `InferState`, `TypedProgram`, `ResolvedCS`)
are the infer module's own currency and move *with* it, re-pub'd for the
emitter and the L7 dumps that share the HM engine.

## Methodological notes carried forward

- **Strip full-line comments before the callee graph** (modules/desugar
  lesson) held — the `awk 'sub(/^[ \t]*#.*/,"",line)'` pre-pass kept the
  grep counts honest. The zoxide shell-init noise on every Bash call was
  cosmetic; redirecting greps to `/tmp/*.txt` sidestepped it.
- **The `[Decl]→[Decl]` signature is a trap.** Three passes share it; one
  (TCO) is secretly emit-coupled. Always grep the body for cross-layer
  calls, never trust the type alone. This is the generalisation of the
  phase-1m sub-pass-F finding and the single most transferable lesson.
- **The no-back-edge measurement is the whole ballgame.** One `grep |
  awk '$1 in range'` per later entry point, expecting empty output,
  proved the block is a DAG and every layer independently extractable.
  This single check did more than any amount of header reading.
- **Apparent back-edges are usually trivial glue.** The 4 infer→pass and
  7 M2bc→M2d "back-edges" were all generic helpers (`ty_eq`, `list_has_*`,
  `option_str_eq`) physically misplaced — mirror-able, not cycles.

## Fixtures / coverage

None — this lane moved zero code, so there is nothing to fixture. The
extraction lanes N1–N4 each owe a `test_<module>.kai` + a byte-identical
selfhost gate (the typer and perceus run on every prelude, so selfhost is
the exhaustive coverage; targeted unit tests catch the public-surface
shape). Flagged in the analysis doc's per-lane acceptance lines.

## Real cost vs estimate

The brief estimated "200–400 line doc, analysis-only." Shipped ~300 lines.
The bulk of the work was the callee-graph measurement (≈12 Bash scans),
not writing — exactly the right ratio for an analysis lane. The one
surprise that cost an extra scan: the brief's range *understated* the
infer module by ~3 900 LOC (M2b+M2c live above 16259), found by tracing
`synth`'s head-bound calls back to their definitions. Refuting the
"perceus needs type info" hypothesis cost one targeted scan and paid for
itself by unblocking N3/N4 as standalone cuts.

## Follow-ups for the extraction lanes

- **Lane N0 (prerequisite):** sink `ProtoOpReg`/`ProtoTPReg`/`ProtoImplReg`/
  `ProtoOriginReg`/`ProtocolReg` to `ast.kai`. ~40 LOC, byte-identical
  selfhost.
- **Lane N1:** `compiler/infer.kai` = 12333–27430 (M2b+M2c+M2d+driver),
  optionally + L7 dumps (33266–34037). The big one; ~18–30 pub; budget a
  pub-enforcement round (the result-type surprise from phase 1m recurs).
- **Lanes N2/N3/N4:** `monomorph.kai` / `unbox.kai` / `perceus.kai` —
  independent of each other, mechanical once N1 lands. unbox is smallest.
- **Deferred:** TCO → emit_c lane (emit-coupled); protocols (39167–42630)
  → its own `compiler/protocols.kai`, sequenced after N1 so `TypedProgram`
  is already in `infer.kai`.
- **The driver stays apex and last** (PR #687), importing infer/monomorph/
  unbox/perceus as bodies, not back-imports.
