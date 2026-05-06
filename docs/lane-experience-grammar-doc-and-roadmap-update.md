# Lane retro — `grammar-doc-and-roadmap-update`

**Lane**: doc-only — `docs/grammar.md` (closes #264) + reconcile
`docs/roadmap.md` with reality (Tongariki closed 2026-05-02; Anga
Roa de facto since ~2026-05-03).

**Branch**: `grammar-doc-and-roadmap-update`
**Base**: `main` at `7111de4` (`bump: version 0.42.0 → 0.43.0`).
**Started**: 2026-05-05T18:21:53-04:00.

## Objective metrics

- New file: `docs/grammar.md` (~600 lines, 6 sections, EBNF +
  prose).
- Edited file: `docs/roadmap.md` (~50 lines diff — Status snapshot
  rewrite + Tongariki marked closed + Anga Roa precondition list).
- Edited file: `docs/design.md` (one bullet — points at
  `grammar.md` as canonical syntax reference).
- Edited file: `docs/kaikai-minimal.md` (one note at the top —
  cross-references `grammar.md`, names this doc as the stage-0
  subset).
- Code changes: zero (`.kai` source untouched per lane scope).
- Tier 0: green (selfhost byte-identical, demos baseline 26/26).
- Tier 1: failed locally on macOS at `test-effects` with
  `Error 143` after `m12_8_y_phase4_subtyping (LLVM)`. **Verified
  pre-existing**: reproduced on a clean `main` checkout (via
  `git stash` revert), and `gh run list --branch main` shows the
  Linux CI tier1 / tier1-asan / tier1-stdlib-core gates all
  green. Failure is environmental on macOS, not lane-introduced.
  Doc-only diffs are `paths-ignore`'d in CI per CLAUDE.md, so the
  PR will not run tier gates anyway.

## Grammar.md scope coverage

| Coverage check                                              | Result |
|-------------------------------------------------------------|--------|
| Every keyword in `Tk*` enum appears in §1 / §2              | yes    |
| Every sugar in `docs/syntax-sugars.md` §1-§9 appears in §4   | yes    |
| Every effect-row form in `docs/effects.md` appears in §2.4   | yes    |
| Every UoM construct in `docs/units-of-measure.md` appears in §1.6 / §2 | yes |
| Every operator in `parse_*_rest` chain appears in §3        | yes    |

Notes:

- §3 precedence table maps 1:1 to the `parse_pow / parse_mul /
  parse_add / parse_concat / parse_cmp / parse_and / parse_or /
  parse_pipe` chain in `stage2/compiler.kai`.
- §1.4 keyword list cross-checked against the alphabetical
  `TkAnd … TkWith` block at `compiler.kai:41-44`.
- §1.5 punctuation table cross-checked against `tk_name`
  (`compiler.kai:103-131`).
- Refinements (§2.8) are referenced but only the surface shapes
  appear; the predicate language is in `docs/refinements.md`.

## Roundtrip-check trace

Sample: `demos/ping_pong/main.kai` (chosen because it exercises
imports, function decls with effect rows, lambdas, calls, pipes,
trailing lambdas, qualified calls, the `if`/`else` shape, and
`fiber_spawn` discard via `let _`).

Hand-parse using only `docs/grammar.md`:

1. `import actor` — matches `import ::= 'import' module_path` →
   `module_path = "actor"`. ✓
2. `import spawn` — same. ✓
3. `fn worker(target: Pid[String], tag: String, i: Int, n: Int) :
   Unit / Actor[String] + Spawn + Console = { ... }` matches:
   - `fn_decl ::= 'fn' IDENT '(' params ')' return_spec fn_body`.
   - `params = "target: Pid[String], tag: String, i: Int, n: Int"`
     each matches `param ::= IDENT ':' type`. `Pid[String]` is
     `IDENT type_args`. ✓
   - `return_spec = ': Unit / Actor[String] + Spawn + Console'`
     decomposes as `':' type effect_suffix` where
     `effect_row = Actor[String] + Spawn + Console` matches
     `effect_atom ('+' effect_atom)*`. The `Actor[String]` atom
     matches the parametrised `IDENT type_args` form. ✓
   - `fn_body = '=' block` (the `{ ... }` is parsed as a block
     because the `=` is present; this is the one *block-as-value-of-
     `=`-body* form). ✓
4. Inside the body: `Actor.send(target, tag ++ ":" ++
   int_to_string(i))` decomposes as:
   - `qualified_call = Actor.send` (production
     `qualified_call ::= module_path '.' IDENT`).
   - `(target, tag ++ ":" ++ int_to_string(i))` is the call
     postfix. The middle argument is a `concat_expr` chain
     (`tag ++ ":" ++ int_to_string(i)`) — three operands separated
     by `++`, which §3 places at level 6, right-associative. ✓
5. `fiber_yield()` — `IDENT '(' ')'` postfix. ✓
6. `worker(target, tag, i + 1, n)` — same shape, with `i + 1` an
   `add_expr`. ✓
7. `if i <= n { ... } else { ... }` matches `if_expr ::= 'if' expr
   block ('else' block)?`. ✓
8. `let me = Actor.self()` matches `let_stmt`. ✓
9. `let f_a = fiber_spawn(() => worker(me, "A", 1, 3))` — the
   inner `() => worker(...)` is the multi-arg arrow lambda
   `'(' params? ')' '=>' expr` from §2.6. ✓
10. `let _ = fiber_spawn(...)` — `_` as an `IDENT`-like binding
    matches `let_stmt`'s pattern (a `pattern` can be `_`). ✓
11. `with_mailbox { run() }` — call with paren-less trailing
    lambda. The grammar production is `postfix ::= trailing_lambda`
    after the bare callable `with_mailbox`. ✓
12. `fn main() : Int / Console + Spawn = { ... }` — top-level entry
    point with the `Int / Console + Spawn` row. ✓

All 12 constructs in the sample reduce cleanly under the productions
in §2 plus the precedence rules in §3. **No production was missing,
no ambiguity was hit that wasn't already documented in §5.**

## Roadmap reconciliation summary

**What was stale (pre-edit)**:

- `Status snapshot` named `kaikai-Tongariki Wave 3` as the current
  target — Wave 3 closed 2026-05-02 via PR #73 (issue #59).
- `HEAD` was pinned at `0.31.0` — actual is `0.43.0` (twelve
  feature releases since).
- The Tongariki section described Wave 3 as ongoing.
- The Anga Roa section listed all six items as "remaining" with no
  awareness that the protocols + ergonomics chain landed
  preconditionally between 2026-05-03 and 2026-05-05.

**What is current now**:

- `Status snapshot` names `kaikai-Anga Roa` as the current target,
  de-facto since ~2026-05-03, with HEAD at `0.43.0`. Lists the 12
  precondition PRs explicitly.
- Tongariki marked **CLOSED 2026-05-02** with PR #73 cited; today's
  PR #283 (residual disclaimer cleanup) noted as post-closure.
- Wave 3 sub-section header marked closed; DoD item 6 stamped with
  the closure date.
- Anga Roa section reframed: "precondition work (landed early)"
  enumerates the 12 PRs; "remaining" preserves the original 6
  scope items (m11, lsp, repl, bench v1.x, check shrinking,
  reuse-in-place); estimate revised from "~4-6 weeks" to "~3-5
  weeks remaining" per pace observed.

## Friction points

- §4.4 (pipe forms): `||` is the flat-map pipe and shares
  precedence level 10 with `|>` and `|`. `compiler.kai`'s
  `parse_pipe_rest` accepts all three at the same level; the older
  `kaikai-minimal.md` table only listed `|>`. Documented the full
  triple in §3.
- §5.3 (`a.b(args)` resolution) is **not** a parser ambiguity — the
  parser produces one shape and the resolver picks among module
  call / UFCS / projection. Documented as a "post-parse rewrite"
  rather than as parser bookkeeping, to be honest about where the
  decision happens.
- The **refinement language** (`requires` / `ensures` / per-binding
  `where`) has surface shapes in the grammar but its expression
  sub-language is more restricted than ordinary `expr`. §2.8 only
  documents the shape; the predicate restrictions stay in
  `docs/refinements.md` per scope.
- The `protocol P[A]` form (issue #180) and the bounded
  `impl[T : Show + Eq] P for [T]` form (issue #174) needed two
  separate type-parameter productions: protocols use `type_params`,
  impls use `impl_type_params` because only impls accept bounds.
  This split is unusual but mirrors `compiler.kai`.

## Subjective summary

The grammar.md write went smoothly because the prior `kaikai-minimal.md`
already had ~70% of the EBNF in usable form; the lane was mostly
*generalization* (adding effect rows, protocols, UoM, sugars) and
*pinning ambiguity rules*. The §3 precedence table required reading
the parse-chain order in `compiler.kai` carefully — `parse_pow` sits
above `parse_unary`, which surprised me until I read the unary
production and saw it dispatches into `parse_pow`. Documented as
"unary at level 3, pow at level 2" with the parser fn names so
external implementers can verify against the source.

Roadmap reconciliation was simpler — it was a rewriting exercise
based on `git log --oneline --since="2026-05-02"`, the pinned
memories about Tongariki closure, and the issue-tracking on Anga
Roa.

## Limitations

- **Semantic rules out of scope**: typing, effect inference,
  exhaustiveness, unification, drop semantics — none of these are
  in this doc by design (issue #264 §*Scope (out)*).
- **Tree-sitter generation follow-up**: the grammar is suitable for
  manual transcription to a tree-sitter `grammar.js`, but this lane
  does not produce one. That's a separate issue.
- **Refinement predicate language**: §2.8 documents only the *shapes*
  of `where` / `requires` / `ensures`; the predicate sub-language
  (which atoms / functions are allowed inside) stays in
  `docs/refinements.md`.
- **Stage 0 vs full divergence**: cross-referenced in
  `docs/kaikai-minimal.md` but not enumerated production-by-production
  here. The "stage 0 is a strict subset" claim relies on the reader
  to walk it.

## Build TSV (Tier gates)

Appended at end of lane (see `/tmp/lane-grammar-doc-and-roadmap-update-builds.tsv`).
