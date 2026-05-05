# Lane experience — issue #251 + #252 (Mutable soundness + Koka-style masking)

## Objective metrics

- Branch: `issue-251-252-mutable-soundness-and-masking`
- Combined fix for #251 (Mutable soundness for `array_set` / `array_grow` / `a[i] := v`) and #252 (Koka-style masking when the target Array is locally constructed and never escapes).
- Lane time budget: ~5–6h. Actual: ~1h45m end-to-end (start `2026-05-04T22:57:23-04:00`, end at PR-creation time below).
- Diff: 8 files changed, +818 / −56. Core typer change concentrated in `stage2/compiler.kai` (+637 / −2). Cascade across non-typer files limited to **2 fixture-row updates** plus the **3 `subst_*` typer helpers** that mutate record-field Arrays.
- Tier gates: `tier0`, `tier1`, `tier1-asan` all green locally on Darwin.
- Selfhost byte-identical at the first iteration (no convergence churn). LLVM smoke (`make test-llvm`) clean.
- No CHANGELOG.md / VERSION edits (per CLAUDE.md `cz` workflow).

## Diagnosis

The `a[i] := v` sugar was being desugared (per the stage 2 comment at line 1298) to `Mutable.array_set`-style calls — but in practice the lowering site (`SIndexAssign` arm in `synth_one_stmt`, ~line 25862) constructed a bare `EVar("array_set")` callee, not an `EField(EVar("Mutable"), "array_set")`. Bare-prelude calls flow through `synth_call`'s default arm via `add_prelude_effect_label` (line 25600), which is the existing intercept point for `print` / `read_line` / `read_file` / `write_file` raising the matching effect label. The `array_set` / `array_grow` builtins were absent from `prelude_effect_for`, so writing through a bare callee silently bypassed Mutable demand. `Mutable.array_set` (the explicit dotted form) DID add the label via the op-call pathway, but the sugar didn't use it.

Two changes were needed:

1. Extend `prelude_effect_for` to return `Some("Mutable")` for `array_set` / `array_grow`. Reads (`array_get`, `array_length`) and the constructor (`array_make`) stay bare.
2. Add a post-`synth` masking pass on the typed function body that drops `Mutable` from the inferred row when every demand targets a locally-constructed Array.

`var x = init` desugars (per `desugar_var_block` at line 16800-ish) to `let x__slot = array_make(1, init)` plus `array_set(x__slot, 0, ...)` per assignment. Step (1) without step (2) would force `/ Mutable` on every function with a local `var` — semantically wrong and ergonomically painful (the user's `var` is local state, not observable mutation). The masking pass naturally classifies `x__slot` as local because its `let` RHS is a direct `array_make` and the slot name never crosses a function-call boundary.

## Algorithm

Two passes, both walking the typed body once:

**Pass 1 — provenance scan (`collect_local_arrays`).** Build the set of names that originate fresh local Arrays. A `let n = rhs` binds `n` as local iff `rhs` is one of:

- `ECall(EVar("array_make"), _)` — direct construction.
- `ECall(EVar("array_set"), [EVar(m), ...])` where `m` is already in the local set — chained in-place mutation that returns the same underlying Array (the stage 2 prelude `array_set` returns Array[T] not Unit).
- `ECall(EVar("array_grow"), [EVar(m), ...])` where `m` is local — same chain.

Recurses through every sub-expression so locals introduced inside if/match/block branches are caught.

**Escape scan (`collect_escaped_names`).** A name escapes (gets tainted) when it appears as an argument position of any call other than the safe array_* whitelist (`array_make`, `array_length`, `array_get`, `array_set`, `array_grow`). Pipes `arr |> helper` count as argument-passing to `helper`. Record literals storing a name into a field count as escape (the carrier may hand the reference to the caller). Returning the name as the function's tail value does NOT escape — the caller receives a settled value, which is the whole point of the masking discipline.

**Pass 2 — demand classification (`all_demands_local`).** Walks every `array_set(first, ...)` / `array_grow(first, ...)` call site. The demand is local iff `first` is `EVar(n)` where `n ∈ locals`, `n ∉ escaped`, and `n` is not a function parameter. Otherwise the demand is external. Returns true iff EVERY demand is local.

If `Mutable` is in `st.row` AND every demand is local, `st_drop_label(st, "Mutable")` removes the label (and the matching `eff_uses` entry to keep diagnostics consistent) before `check_body_row` validates the row against the user's annotation. The pass is a no-op when `Mutable` is not in the row to begin with.

The hook lives in `infer_decl` immediately before `check_body_row` (`mask_local_mutable_demand(st1b, ps, rb.expr)`).

## Migration inventory

Surprisingly small. The typer's `Subst` discipline mutates three record-field Arrays (`s.slots`, `s.row_slots`, `s.unit_slots`) inside `subst_extend` / `subst_row_extend` / `subst_unit_extend` — these are the only stage 2 helpers whose receivers escape from the typer's POV. Because the call chains are local to the inferencer, the `/ Mutable` row only had to be added to those three signatures; every transitive caller already lives inside an inference frame whose row is open (a row variable in the tail), so no further cascade was triggered.

| File | Function | Why |
|---|---|---|
| `stage2/compiler.kai` | `subst_extend` | `array_grow(s.slots, ...)` / `array_set(slots1, ...)` of an external (record-field) Array. |
| `stage2/compiler.kai` | `subst_row_extend` | Same pattern over `s.row_slots`. |
| `stage2/compiler.kai` | `subst_unit_extend` | Same pattern over `s.unit_slots`. |
| `examples/sugars/array_index_field_path.kai` | `main` | Mutates a record-field Array (`cfg.scores[i] := ...`) — receiver path is `EField`, not `EVar`, so the conservative classifier marks the demand as external. The fixture's intent (a `main` showing field-path indexed-write sugar) is preserved by adding `+ Mutable` to its declared row. |
| `examples/sugars/m7d_21_pipe_placeholder_evaluates_once.kai` | `next(state: Array[Int])` | Mutates a parameter — observable to caller. Added `/ Mutable`. |

No stdlib (`stdlib/*.kai`) sites needed migration. No demos (`demos/`) or other examples (`examples/perceus/`, `examples/effects/*` pre-existing) needed migration. Stage 1's compiler.kai is unaffected because stage 1 has no Mutable enforcement.

## Empirical verification (the three repros from #251)

Run inside the worktree against `stage2/kaic2`:

```kai
# Repro 1 — modify (parameter mutation, no row) → REJECTED
fn modify(a: Array[Int]) : Unit { a[0] := 42 }
```
Diagnostic: `error: effect not handled: Mutable` pointing at the `a[0] := 42` site, with `note: modify's declared row does not include Mutable` and `help: declare modify : ... / Mutable`. ✓

```kai
# Repro 2 — sum_array (read-only) → COMPILES
fn sum_array(a: Array[Int]) : Int { sum_loop(a, 0, 0) }
fn sum_loop(a: Array[Int], i: Int, acc: Int) : Int = ...
```
EXIT=0. ✓

```kai
# Repro 3 — build_circle (local construction with mutation) → COMPILES (no /Mutable)
fn build_circle(verts: Int) : Array[Int] {
  let f = array_make(verts, 0)
  f[0] := 10
  f[1] := 20
  f
}
```
EXIT=0. ✓ (The masking pass identifies `f` as local because the `let` RHS is `array_make` and `f` never appears as an arg to a non-array_* callee.)

The four pinned fixtures under `examples/effects/` (`array_mutable_required.kai`, `array_mutable_explicit.kai`, `array_read_only_no_mutable.kai`, `array_local_masks_mutable.kai`) wire the same shapes into `make tier1` so regressions surface in CI.

## Friction points

- **Selfhost convergence**: Reached fixed point in ONE iteration (not the 2–3 the brief budgeted for). The change is purely additive on the typer's row inference; the substitution result is unchanged for code whose effective row was already correct, and the `/ Mutable` annotations on `subst_*` get fully resolved on the first pass.
- **Conservative escape rule (passing local Array to helper)**: Did NOT block any legitimate site in the audited tree. All current stdlib / examples / demos either keep mutation strictly inline inside the constructing function or already declare `/ Mutable` correctly. A future case where a helper takes a local Array and the caller wants to mask Mutable would need either an annotation upgrade or a finer interprocedural analysis (out of scope per #251 v1).
- **Multi-line boolean parser limitation**: The kaikai LL(1) parser does not accept `and` at the start of a continuation line — chained boolean expressions across lines need to be rewritten as nested `if not x { false } else { y }` blocks. Caught early by the first stage1 → stage2 typecheck failure; mechanical to fix.
- **`HReturn` arity surprise**: `HR(String, Expr, Int, Int)` (4 fields including line/col), not the 2-field shape my mental model assumed. The compiler dies with a parser error in the AST walker until every `HR(_, body)` becomes `HR(_, body, _, _)`. Caught by the first kaic1 → stage2 build.
- **`map_expr_kind` was unsuitable**: I deliberately wrote dedicated walkers (`collect_local_arrays_kind`, `collect_escaped_names_kind`, `all_demands_local_kind`) instead of reusing `map_expr_kind` because the masking analysis is structure-dependent (let-binding RHS shape matters) and not a uniform map. Three near-duplicate visitors are easier to read than one parameterised over a callback that ignores most node types.

## Doc updates summary

- **`docs/effects-stdlib.md` §`Mutable`**: Replaced "Idiomatic usage" / "Known gap: read-only O(1) access" / "Migration plan" with a new "Observable-effects discipline (issue #251 + #252)" section covering the two precise rules, three worked examples, the v1 conservative escape rule, the rationale (Tier 2 #5 ergonomics + `var` precedent), and an updated migration plan that documents the prelude-shim + masking-pass shape.
- **`docs/effects.md`** (handler discipline): Added one paragraph after the `State[T]` parameterised-handler example explaining that `Mutable` extends the same masking-at-scope-boundary idea — `var x = init` masks `State[T]` at the block boundary; locally-constructed `Array[T]` masks `Mutable` at the function boundary. Cross-references `docs/effects-stdlib.md` §`Mutable`.
- **`docs/design.md`**: Replaced the "Provisional escape — opaque mutable `Array[T]`" bullet (which described `Array` as technical debt) with the new statement that `Array[T]` writes ride `Mutable` on the observable-effects discipline. Removed the "do not expose `Array[T]` in userland surface code" admonition since the discipline is now sound at compile time.
- **`CLAUDE.md`**: Updated Tier 1 #1 to drop the "opaque mutable `Array[T]`" item from the "audited runtime escapes" list — `Array` writes are no longer an escape, they're tracked. The remaining escapes (`panic`, `?`, `todo!`, `axiom`, FFI) are unaffected.

## Subjective summary

The hardest part of this lane was the up-front design call: do we tag the demand at the `array_set` site and post-process, or thread provenance through every let-binding and call site eagerly? I picked the post-process approach because it kept the change orthogonal to the existing inference pipeline — `synth` doesn't need a new field on `InferState`, the prelude shim already exists for `print` / `read_line`, and the masking is a one-shot walk that runs immediately before `check_body_row`. The downside is that the analysis is local to a function body and slightly conservative (an Array passed to a helper escapes), but that matches the spec's v1 scope exactly.

The cascade migration was MUCH smaller than the 30–80 sites the brief estimated — only 3 typer helpers + 2 fixtures. The reason: the typer's mutable-Array discipline is concentrated in `Subst`, which lives entirely inside the inferencer, and the row variable in every TyFnT's open tail absorbs the new `Mutable` label transparently for callers who don't pin the row.

Selfhost byte-identical on the first attempt was a nice surprise — I had braced for one or two iterations of "the new typer compiles itself but emits a slightly different output". The change touches inferred rows, not emitted code, so the fixed point holds immediately.

## Limitations

- **Conservative escape rule**: A locally-constructed Array passed to a non-`array_*` helper is treated as escaping. This is sound but coarse. The spec's example `fig_kona` calls `raylib.cos(...)` and `raylib.sin(...)` with scalar arguments only — never the Array — so it masks correctly. A workaround for the rare case where the user wants masking through a helper is to declare the helper `/ Mutable` and let the caller stay clean.
- **Field-path indexed write (`cfg.scores[i] := ...`)**: The lowering produces `array_set(EField(EVar("cfg"), "scores"), ...)` — the first arg is `EField`, not `EVar`. My `demand_is_local` returns `false` for non-`EVar` first args (conservative). A future refinement could trace the field path back to a locally-constructed record whose field holds a locally-constructed Array, but that's a deeper provenance shape than v1 needs.
- **Refinement boundary**: When the function body is a single expression (e.g. `fn f(...) = expr`), the walker still visits `expr` correctly. When the body is an `EBlock([stmts...], None)` with no tail expression, the walker correctly returns `acc` for the `None` branch. Edge cases: `EHandle`, `EMatch`, `ELambda` are all explicitly recursed; the only missing arm is `EVariantsOf` (resolved to a fixed list at parse time, no Array can hide there) and `EModCall` / `EHole` / `ETodo` (no sub-Exprs to walk).

## Build TSV (appended)

```
timestamp	cmd	outcome	elapsed_s
2026-05-04T23:07:37-04:00	tier0	FAIL	-       (pre-cascade, expected: subst_extend rejected)
2026-05-04T23:09:37-04:00	tier0	OK	-
2026-05-04T23:25:02-04:00	tier1	OK	-
2026-05-04T23:26:03-04:00	tier1-asan	OK	-
2026-05-04T23:40:05-04:00	tier1	OK	-       (re-validation after fixtures + docs)
2026-05-04T23:40:05-04:00	tier1-asan	OK	-
```
