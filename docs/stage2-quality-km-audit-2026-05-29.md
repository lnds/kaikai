# stage2 quality audit (km-guided) — 2026-05-29

Deep critical audit of `stage2/compiler` (the self-hosted kaikai compiler)
by the linus reviewer agent, using the `km` (Kimün) code-metrics tool
(v0.23.0). Read-only audit + an abordage plan. Eduardo asked for a
sharply critical eye: inefficient functions (sub-optimal algorithms),
bad practices, smells, language under-use, unmaintainable functions, bad
code in general.

> **Scoping rule (Eduardo, load-bearing):** the plan below is prioritised
> ONLY by (1) impact, (2) soundness risk, (3) gate verifiability. NO time
> estimates — this project's history refutes every "days/weeks" guess;
> time is noise, not data. A lane with a binary gate is attemptable at any
> size: if it breaks, the gate shows it and we revert.

## km baseline (`km score stage2/compiler`)

- **Project Score: F-- (39.3)**, 23 files, 52.6K LOC.
- Cognitive Complexity 7.4/100 (F--), Halstead Effort 18.4 (F--),
  File Size 14.1 (F--), Indentation 75.3 (C+), **Duplication 100 (A++)**.
- Worst file: `infer.kai` (score 12.6, cognitive 103, 11.7K LOC code).
- File sizes (wc -l): infer 18121, emit_c 10239, emit_llvm 6465,
  parse 6315, driver 5203, desugar 4499, protos 4493, cache 3375,
  emit_shared 2981, perceus 2794, modules 2683.

Duplication A++ matters: the "duplication" that exists is the intentional
bootstrap mirrors, not random copy-paste. The disease is concentrated in
individual functions that grew unbounded + sub-optimal data structures.

## Verdict (one line)

The compiler is technically correct but structurally sick — complexity is
concentrated in individual functions that grew without limit, `TyEnv` is a
linear list with O(n) everything, and there are at least three redundant
AST-walkers running over the same tree in separate passes.

## Findings by axis

### 1. Sub-optimal algorithms (O(n²) and re-compute)

- **`ty_env_lookup` — infer.kai:2733 — O(n) per call, called on O(n) nodes.**
  `TyEnv.entries` is `[TyEntry]`, a linked list; `ty_env_lookup_loop` linear-scans
  it. `ty_env_add` allocates a fresh `TyEnv` per call. Caveat: Front A
  (2026-05-28) measured `st_unify` at 93K calls, not millions — the real perf
  cost may be distributed (Perceus dup/drop), not a single env-lookup hotspot.
  **Measure before optimising.**
- **`inf_dedup_strs` — infer.kai:198-206 — O(n²).** `list_has` over a growing acc.
- **`variants_of_type_loop` / `_arity` — infer.kai:13455 — O(env_size) per call**
  during exhaustiveness check; O(N_arms × M_entries) overall.
- **`is_effect_in_env_loop` — infer.kai:9537 — O(env_size) per op-call**, from
  `try_op_call` for each `EField(EVar, op)`.
- **`inf_scan_uses_kind` — infer.kai:1160 — `list_append(p1, list_append(p2, p3))`
  = O(n²)** in AST node count (list_append copies the left list). Fix:
  reversed accumulator + `list_reverse`. Duplicated across `inf_scan_uses_*`,
  `walk_coverage_*`, `collect_call_labels*`, `all_demands_local*`.
- **`synth_candidates` — infer.kai:17573 — O(|scope|²)** (diagnostic-only / holes,
  bounded impact).

### 2. Bad practices / smells

- **Five independent AST-walkers for the same job:** `inf_map_expr_kind`
  (infer.kai:592), `dsg_map_expr_kind` (desugar), `prc_map_expr_kind`
  (perceus), `proto_map_expr_kind` (protos) — copies of the same structural
  functor. Adding a new `ExprKind` = 4 synchronised edits. Known debt
  (issue #677 ast-sink follow-up).
- **7-arg context clump repeated 61× — emit_c.kai:3864.** The quintet
  `(lcs, fns, variants, lams, cls)` appears in 61 signatures. Should be one
  `EmitCtx` record. Adding a 6th context field today = 61-signature surgery.
- **`add_prelude_sigs` — infer.kai:2919 — 220 LOC that is a table.** Nested
  `ty_env_add(ty_env_add(...))` allocating N intermediate `TyEnv` records.
- **Six near-identical name-collision validators — infer.kai:3822, 3998, …**
  (`validate_{union,type_name,fn_name,effect_name,const_name,axiom_name}_collisions_decls`),
  ~100 LOC each.
- **`inf_indent_of` — infer.kai:238 — O(depth²) string concat** (debug path).
- **Duplicated section comment — emit_c.kai:4400-4416** (copy-paste residue).
- Mixed `string_concat` chaining vs `concat_all` — some silent O(n²).

### 3. Language under-use (calibrated against bootstrap constraints)

linus correctly discounted several false positives: the compiler avoids
advanced features on purpose because stage1 (kaikai-minimal) must compile
the stage2 bundle. Real findings:

- The `list_append(a, list_append(b, c))` pattern is an algorithm bug, not
  pipe under-use (fix is accumulators, not `|>`).
- `list_has_int` (infer.kai:17495) duplicates stdlib `list_has` — signals
  the protocol-Eq path isn't leveraged.
- Pattern-matching-as-dispatch over AST types is a **design constraint**,
  not under-use (protocols over AST types would be architecturally heavy).

### 4. Unmaintainable functions

- **`synth_handle` — infer.kai:8199 — ~400+ LOC, 6 sub-problems mixed**
  (effect known-check, clause names, clause arity, effect tyargs, row-var
  freshening, op clauses, return clause, label merge, state handlers,
  capability propagation). Top factoring candidate.
- **`walk_coverage_expr` — infer.kai:2221 — 115 LOC, 30 match arms, 6 params.**
  `decls: [Decl]` threaded through every level just to call
  `effect_default_op_names` per `EHandle` — pre-computable.
- **`all_demands_local_kind` — infer.kai:11506 — 95 LOC**, 3 context params
  through 15 recursive forms (should be a record).
- **`variants_of_type_loop_arity` — infer.kai:13455 — 5 concerns interleaved**,
  match nesting depth 4-5.
- **`try_op_call` — infer.kai:9056 — 90 LOC, 7 cascading resolution paths**,
  match depth 6.
- `synth` (infer.kai:8002) itself is a clean dispatcher — NOT the problem; its
  callees are.

### 5. Documented known debt

- AST-functor sink (4 `map_expr_kind` copies): issue #677 follow-up.
- `env.entries` linear list: Front A suggests the real perf cost is
  distributed, not this. Matiza el findng #1.
- Emitter has no RC discipline (97% allocs leaked): documented separately;
  emission problem, not compiler-logic problem.

## Abordage plan — 6 lanes (priority by impact / risk / gate, NO time)

| Lane | Fixes | Risk | Gate |
|------|-------|------|------|
| **A — EmitCtx record** | data clump in 61 emit_c signatures → one `EmitCtx` record | Low | `make selfhost` byte-identical + `make tier0` |
| **B — list_append O(n²) → accumulator** | `inf_scan_uses_*` + `collect_call_labels*` reversed-acc + `list_reverse` | Low | selfhost byte-identical + tier1 |
| **C — AST-functor sink** | promote one `map_expr_kind` to `compiler.ast`, drop 4 mirrors (infer/desugar/perceus/protos) | Moderate | selfhost + selfhost-llvm + tier1 |
| **D — unify 6 collision validators** | one parametric validator + 6 calls (~400 LOC → ~100 + table) | Low | tier1 (collision fixtures) + selfhost |
| **E — walk_coverage params** | pre-compute `eff_name → [op]` map, drop `decls` param + per-EHandle rescan | Low | tier1 (effect-coverage) + selfhost |
| **F — add_prelude_sigs as table** | replace nested `ty_env_add` with a `[TyEntry]` literal + one fold | Very low | selfhost byte-identical |

All gated by selfhost (byte-identical or fixed-point) → verifiable: if a
refactor changes behaviour, selfhost catches it. That makes them attemptable
regardless of size.

### Lane detail (scope each one safely)

- **A (EmitCtx):** emit_c.kai only. `type EmitCtx = { lcs, fns, variants, lams, cls }`,
  pass as one. Mechanical, no semantic change. Compiler-as-judge: byte-identical
  output ⇒ correct.
- **B (O(n²)):** `inf_scan_uses_*` (infer.kai:1024-1227) + `collect_call_labels*`
  (1610-1750). Internal-only; public entry points keep their signature. Same set
  of `Use`s built in a different order (then reversed) ⇒ byte-identical.
- **C (AST-functor):** compiler.ast + 4 references. Touches ast public interface;
  the concatenated bundle changes (4 mirrors disappear). Verified no import cycle
  (infer imports ast, not vice-versa). Most rigorous gate of the six.
- **D (validators):** infer.kai ~3773-4470 only. No public interface touched.
- **E (walk_coverage):** infer.kai:2221-2455 + callees. Drops O(n_decls) work per
  EHandle in the program being compiled.
- **F (add_prelude_sigs):** infer.kai:2919-3140. One function. Removes ~200 TyEnv
  allocations per compilation.

## What is HEALTHY (do not break)

- **`Subst` as Array with doubling grow** (`subst_extend` etc.) — correct, efficient.
- **`unify_env` / `unify_row` / `unify_unit`** — correct, well-commented (issues
  #532, #187), solid test suite. Changes here need full tier1-ASAN.
- **`synth` as a pure dispatcher** — correct structure; don't collapse.
- **`TyEnv.entries` linear list** — Front A showed st_unify is 93K calls, so the
  linear-lookup impact may be smaller than theory predicts; **don't optimise
  until re-measured under real load**.
- **Perceus pass (perceus.kai)** — clean module extraction; #703 fix is fresh.
- **Duplication A++** — real; the only "duplication" is intentional bootstrap
  mirrors.

## Files involved

- `stage2/compiler/infer.kai` (18121 LOC — primary)
- `stage2/compiler/emit_c.kai` (10239 LOC — Lane A)
- `stage2/compiler/ast.kai` (Lane C)
- `stage2/compiler/{perceus,desugar,protos}.kai` (Lane C mirrors)
