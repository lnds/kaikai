# Lane experience — compiler idiom audit (pre-u8 / Phase A / LSP chain)

**Branch:** `audit-compiler-kaikai-idioms`. **Base:** `main` @ `29f6884`. **Date:** 2026-05-10.

Read-only audit of `stage2/compiler.kai` (51,644 LOC) to find where the
self-hosted compiler is written in C-style kaikai instead of the language
features kaikai was designed around. Numbers come from grep/awk/python over
the working tree; sites are quoted `file:line` so claims can be verified.

The audit is also the lane retro (per CLAUDE.md's mandatory-retro rule).

## TL;DR

- The compiler defines **zero** of its own protocols, uses **66** pipe operators
  in 51.6K LOC (1.28 per KLOC), and threads **318 consecutive `let a = f(x); let b = g(a, …)`** pairs that pipes would compress.
- It does declare **163** functions with effect rows in their return type
  (mostly `/ Console`, plus `/ Mutable` and `/ File`). Effect annotations are
  not the gap. Capability-style use (handlers, `try`, structured diagnostics)
  is essentially absent.
- **The stdlib is not the better example.** Aggregated across `stdlib/`
  (14,199 LOC), there is **one** `|>` total. Idiom debt is project-wide;
  the compiler is just the biggest concentration. A refactor lane that
  rewrites compiler code in idiomatic kaikai is also setting the precedent
  for the stdlib it has not yet been set for.
- Top concrete win: the **147 `None -> None` + 135 `Some(x) -> Some(...)`**
  arms — manual `Option.map`/`or_else` opens — collapse roughly 4:1 once
  `Option.map`/`and_then`/`or_else` exist as plain stdlib fns or as a
  protocol with `?`-sugar awareness. Estimated 800–1100 LOC saved at very
  low risk.

## 1 — Metrics on `stage2/compiler.kai`

### 1a. Result-rewrap chains

| Pattern | Count |
| --- | --- |
| `Err(e) -> Err(e)` (re-wrap arms) | **0** |
| `Result[…]` type mentions | 24 |

The compiler does **not** thread errors through `Result`. The parser/typer/emitter
report diagnostics via `diag_error_from_src` (a `/ Console` side-effect) and
mark a flag on the threaded state record (`Parser.had_error`, `Env.had_error`,
etc.). So the canonical "rewrap-Err cascade" pattern from the audit brief
simply does not exist in this codebase — the metric is zero by architecture,
not by idiomatic discipline.

What does exist instead is the **Option re-wrap** pattern, which is the same
shape one tier down (manual short-circuit through `Option`):

| Pattern | Count |
| --- | --- |
| `None -> None` arms | **147** |
| `Some(x) -> Some(f(x))` arms | **135** |
| `Some(n) -> Some(n)` (identity rewrap, fallthrough pattern) | dozens within the 135 above |

**Sample sites** (each is a textbook `Option.or_else` / `Option.map` candidate):

- `stage2/compiler.kai:6285-6320` — `find_impure_call` walks an `Expr` AST and
  threads `Option[String]` through every variant arm with the
  `match …or_else(…)` pattern manually expanded. The `EIf` case nests three
  `match find_impure_call(…) { Some(n) -> Some(n); None -> … }` levels.
  `first_some([find_impure_call(a, …), find_impure_call(b, …), …])` would be
  one line per arm.
- `stage2/compiler.kai:2309` — `Some(e) -> Some(subst_placeholder_expr(e, repl))`.
  Pure `Option.map` site.
- `stage2/compiler.kai:6290, 6294, 6299, 6301, 6316` — five back-to-back
  `Some(n) -> Some(n); None -> <recursive call>` arms inside the same function.

### 1b. `let`-chains that could be pipes

Heuristic: a line matching `let X = fn(args)` immediately followed by
`let Y = fn'(X, …)` (X is the **first arg** of the next call) marks a candidate
for `|>` rewriting.

- **318 candidates** across `stage2/compiler.kai`.
- Manual classification of a random sample of 20 (seed 42):
  - 15/20 (75%) are obvious state-threading sites (`Parser`, `Env`,
    `FmtState`, `LlvmEmit` records flowing through monotonically increasing
    suffixes `p1, p2, p3` / `e0, e1, e2` / `s0, s1, s2`).
  - 3/20 (15%) thread two unrelated values that happen to share a name —
    not pipe-able in one step but trivially refactorable.
  - 2/20 (10%) genuine false positives (the second call uses `prev_var`
    inside a `match` rather than as the pipe head).
- So roughly **240–290 sites** would compress to pipe form. Two examples:

  ```
  stage2/compiler.kai:48376
    let s0 = fmt_newline(s)
    let s1 = fmt_indent_in(s0)
    # → s |> fmt_newline |> fmt_indent_in

  stage2/compiler.kai:10301
    let e1 = add_all(e0, prelude_names())
    let e2 = register_decls(e1, decls)
    # → e0 |> add_all(prelude_names()) |> register_decls(decls)
  ```

The numbered-suffix pattern (`p1, p2, p3, …`) is itself the smell: pipes
make the suffixes redundant.

### 1c. `if`-`else if` cascades on tagged unions

A walker counts `if … else if …` chains of depth ≥ 3 (the kind a `match`
expresses exhaustively).

- **54 cascades** of depth ≥ 3 in `stage2/compiler.kai`.
- Worst offender: **`keyword_kind`** at `stage2/compiler.kai:258` — a
  **35-branch** `if text == "and" { TkAnd } else if text == "as" { TkAs } …`
  cascade dispatching on a `String` to a `TokKind` tag. This *cannot* become a
  `match` directly because kaikai doesn't pattern-match on string literals
  (issue surface), but it can become a single `match` after lifting keywords
  into an interior enum, or a stdlib map lookup once `Map[String, TkKind]` is
  realistic. **Most importantly**: it's an outlier, not the median.
- Median cascade is 3–8 branches; e.g. `lex_step_single_char` at
  `stage2/compiler.kai:683` — **21 `else if c == '…'`** arms over `Char`.
  This *can* become `match c { '(' -> … '[' -> … }`. The fact it's an
  if-chain not a match is pure idiom debt.

### 1d. Boolean flags as poor-man's enums

Heuristic: functions whose signature contains two or more `Bool` parameters.

- **10 sites** total (low; not a pervasive pattern).
- The repeat offender is `emit_program / emit_program_llvm /
  emit_main_wrapper`, all taking `test_mode: Bool, bench_mode: Bool,
  check_mode: Bool`:
  - `stage2/compiler.kai:16729` `fn emit_main_wrapper(decls, test_mode: Bool, bench_mode: Bool, check_mode: Bool)`
  - `stage2/compiler.kai:22216` `fn emit_program(file, src, raw_decls, test_mode: Bool, bench_mode: Bool, check_mode: Bool, target_origin)`
  - `stage2/compiler.kai:43561` `fn emit_program_llvm(path, src, raw_decls, test_mode: Bool, bench_mode: Bool, check_mode: Bool)`
- A sum type `enum BuildMode { Build, Test, Bench, Check }` collapses three
  flags into one and rules out illegal states (`test_mode = true AND
  bench_mode = true`). Small lane.

### 1e. Manual error context strings

- **0** `diag_error_from_src(...) + ...` sites (no `+` concatenation building
  diagnostics).
- Diagnostics are built with string interpolation (`"#{var}"`), which is
  already idiomatic. The catch is that **error site & message live next to
  each other in the call site**, so an LSP query "all messages emitted by
  the parser" requires a textual scan, not a structured walk.
- `p_error(p, t, "literal string")` appears at **59** call sites — each is a
  candidate for a tagged-record diagnostic (`DiagKind` + payload) that the
  emitter formats centrally. That's a structural change (Tier 1 #4 calls for
  "structured compiler output"), not a syntactic cleanup.

## 2 — Effect-usage

| Pattern | Compiler | Stdlib (14.2K LOC aggregate) |
| --- | --- | --- |
| `fn ... : T / EffectRow` signatures | **163** | **2** |
| Effects referenced in those signatures | `Console` × 189, `File` × 4, `Mutable` × 3, `Stdin` × 1, `Monitor` × 1, `Ffi` × 4 | rare |
| Real `handle`-handler blocks (`^\s*handle`) | **0** | 0 |
| Real `try` expressions | **0** (the 11 matches are all in comments / token names) | 0 |

The compiler does declare effect rows on diagnostic & state-mutation helpers,
which is honest. What's missing is the *capability shape*: errors flow
through a side-effect (`diag_error_from_src` writes to stderr and sets a
flag), not through a handled effect. The "fail fast with structured payload
that an outer handler renders" pattern the design doc gestures at
(`docs/effects.md` §Diagnostics) is not used in the self-hosted compiler.

**Why this is partly architectural, not aspirational**: `stdlib/effects.kai`
explicitly says (lines 5–22 of that file) that the prelude is *documentation
only* — stage 1's parser cannot ingest effect-row syntax on fn signatures, so
the typer cannot enforce `print : Unit / Stdout` without breaking the
bootstrap. This is a kaikai-internal blocker, not a code-quality issue.
Capturing the gap in the audit is appropriate; "refactor it now" is not.

## 3 — Protocols-usage

- Protocol definitions in `stage2/compiler.kai`: **0** declared. **63**
  string matches for `protocol`/`impl` — all inside the parser, all
  comments/literals/token names (i.e. the compiler **parses** protocols but
  doesn't **use** them).
- Reference: `stdlib/protocols.kai` defines 12 protocols (`Show`, `Eq`,
  `Ord`, `Hash`, `Serialize`, `Default`, `Add`, `Sub`, `Mul`, `Div`, …) and
  ~11 `impl` blocks for the primitive types — so the language feature is
  alive in the stdlib.
- AST dispatch (the `map_expr_kind`, `walk_expr`, `subst_expr`, `fmt_expr`,
  `emit_expr`, `tcrec_*`, `unbox_*` families) is done through giant
  `match e.kind { EVar … ECall … EIf … EMatch … }` blocks duplicated across
  dozens of functions. A `protocol AstWalk { walk(e: Expr) -> Expr }` with
  one impl per phase would centralise the variant table; the bet on
  protocols (Tier 1 #3: "single-dispatch protocols Go/Clojure/Elixir-style,
  O(1) impl-table lookup, are permitted") is exactly the bet that pays off
  here.

  Caveat: protocol dispatch through O(1) lookup is presumably slower than
  monomorphised match. Benchmarks before committing.

## 4 — Pipes-usage

| File / scope | LOC | `|>` count | per-KLOC |
| --- | --- | --- | --- |
| `stage2/compiler.kai` | 51,644 | 66 | **1.28** |
| `stdlib/regexp.kai` | 1,390 | 0 | 0 |
| `stdlib/decimal.kai` | 279 | 0 | 0 |
| `stdlib/protocols.kai` | 492 | 0 | 0 |
| `stdlib/core/tuple.kai` | 150 | 1 | 6.7 |
| **All `stdlib/`** | 14,199 | **1** | **0.07** |
| `examples/pipes/leading_pipe_apply.kai` | small | 5 | high |
| `demos/9d9l/weather/main.kai` | small | 4 | high |

Of the 66 compiler-side pipes, most are clustered around dump helpers and
test pipelines (e.g. `stage2/compiler.kai:917`,
`tokens |> each((t) => dump_token(src, t))`). Outside of test/dump scaffolding
the compiler core is pipe-free.

**Five concrete pipe-rewrite sites** (already cited in §1b but worth listing
together):

- `stage2/compiler.kai:48376-48377` — `fmt_newline` then `fmt_indent_in` on
  `FmtState`.
- `stage2/compiler.kai:48000-48001` — `fmt_emit(fmt_emit(s3, "#"), text)`.
- `stage2/compiler.kai:10186-10187` — `env_push` then `add_params`.
- `stage2/compiler.kai:10301-10302` — `add_all` then `register_decls`.
- `stage2/compiler.kai:41785-41786` — `llvm_append_body` twice in a row on
  the same emit record.

These five are picked because the right-hand calls take the threaded record
as **first** arg and only one other arg — the cleanest pipe shape.

## 5 — Comparison with stdlib

The premise of the brief was: "stdlib is idiomatic, compiler is C-style".
The numbers do not support that.

| Metric | Compiler (per KLOC) | Stdlib aggregate (per KLOC) | Ratio |
| --- | --- | --- | --- |
| `|>` density | 1.28 | 0.07 | compiler is **18×** higher |
| `let`-chain candidates (regexp/decimal/protocols/tuple — 2,311 LOC sampled) | 6.16 (`318 / 51.6K`) | 13.85 (`32 / 2.31K`) | stdlib is **2.2×** higher |
| `None -> None` arms | 2.85 | 0.21 (`3 / 14.2K`) | compiler is **14×** higher |
| `Some(x) -> Some(…)` arms | 2.61 | mid-low | comparable |
| Effect rows on signatures | 3.16 | 0.14 | compiler is **22×** higher |
| Protocol/impl definitions | 0 | 12 / 11 | stdlib is the *only* home |

Reading the table:

- The compiler uses pipes & effect rows **more** than the stdlib per KLOC.
- The compiler has **more** `None -> None`/`Some(x) -> Some(…)` arms
  per KLOC than the stdlib — this is its weakest spot.
- The stdlib has **no** pipes worth speaking of and **no** effect rows
  worth speaking of — different debt, same root (the language's own
  bootstrap constraints).
- Protocols live in the stdlib; neither codebase uses them as a dispatch
  primitive for runtime code.

**The smoking gun is not "compiler vs stdlib"** — it's
"`Option` boilerplate everywhere because the stdlib doesn't yet ship the
combinators (`Option.map`, `or_else`, `and_then`, `first_some`) and `?` only
short-circuits in restricted positions".

## 6 — Refactor proposals (ranked by LOC reduction ÷ lane size)

Each proposal lists target sites, conservative estimate of lines saved,
implied lane size in days, and ordering relative to the planned u8 →
Phase A (cache) → LSP chain.

### Proposal 1 — Ship `Option.map`/`or_else`/`and_then` + rewrite call sites

- **Pattern targeted:** 147 `None -> None` + 135 `Some(x) -> Some(…)` arms.
- **LOC reduction estimate:** ~280 sites × ~3 lines each = **~840 LOC**.
- **Lane size:** ~1–2 days (stdlib combinators are small; the bulk is the
  call-site rewrite, mechanical).
- **Risk:** Low. Combinators don't touch the typer's hot path. Rewrite is
  semantic-preserving by construction. Selfhost remains byte-identical if
  combinators are inlined by Phase 3 unboxing.
- **Order:** **Before u8 lane.** Pre-existing `Option` boilerplate would
  metastasise into the new `u8` arithmetic paths otherwise.
- **Ratio (LOC / day):** ~500.

### Proposal 2 — Convert if-`==`-char/string cascades to `match`

- **Pattern targeted:** 54 if/else-if chains of depth ≥ 3.
- **LOC reduction estimate:** depth-N cascade → match saves ~0.5N lines on
  punctuation (`else if c == 'X' { … } else if …` collapses to `'X' -> …`).
  ~54 sites × ~6 lines average = **~250–350 LOC**.
- **Lane size:** ~1 day. Mechanical; the lexer cascades are clustered.
- **Risk:** Low for `Char` cascades. The `keyword_kind` 35-arm string-eq
  cascade is its own micro-lane because kaikai doesn't string-match in
  `match` patterns today; it stays as-is or becomes a fns-table.
- **Order:** **Before LSP**, after u8. The LSP wants structured tokens; the
  cleaner the lexer reads, the faster onboarding for LSP work.
- **Ratio:** ~250–350.

### Proposal 3 — Pipe-rewrite the state-threading sites

- **Pattern targeted:** ~250 of the 318 `let-chain` candidates classified
  as state-threading.
- **LOC reduction estimate:** Best-case ~2 lines saved per site (kill the
  intermediate `let p1 = …` binding) = **~500 LOC**. Realistic, accounting
  for sites where the chain is broken by intervening logic: **~300 LOC**.
- **Lane size:** ~3 days. Lots of small edits, each easy, but each needs
  reviewer eyes for "did this pipe really preserve semantics".
- **Risk:** Medium-low. The risk is reviewer-cost, not correctness: 250
  edits is a big PR. Slice into 4–5 PRs by module (lexer / parser / typer /
  emitter / fmt).
- **Order:** **After Proposal 1 & 2, after u8, before LSP.** A pipe lane
  immediately after `Option` combinators ship gives the combinators the
  density they were designed for.
- **Ratio:** ~100.

### Proposal 4 — Protocol-ify AST dispatch (`walk_expr` family)

- **Pattern targeted:** the giant `match e.kind { EVar … ECall … }` blocks
  repeated across `subst_expr`, `walk_expr`, `fv_expr`, `fmt_expr`,
  `emit_expr`, `tcrec_*`, and several phase-specific visitors. There are at
  least 12 such functions, each ~80–200 LOC of variant-by-variant code.
- **LOC reduction estimate:** uncertain. A `protocol ExprVisitor` lets
  each phase ship `~30 LOC of interesting variants + impl Default for ExprVisitor`
  instead of `~100 LOC per visitor`. Plausibly **~600–800 LOC saved**, but
  this is the most speculative estimate in the doc.
- **Lane size:** ~5–7 days. Touches every phase; needs careful benchmarking
  against the monomorphised match (Tier 1 #2: runtime efficiency wins ties).
- **Risk:** **High.** This is the only proposal that can regress compile
  time (Tier 1 #3) or runtime perf (Tier 1 #2). Selfhost benchmarks are the
  acceptance gate.
- **Order:** **After LSP.** Pre-LSP risk budget is spoken for; this is a
  separate "compiler-internals-modernisation" lane that should not block
  the planned chain.
- **Ratio:** ~100, but heavily caveated by risk.

### Proposal 5 — Replace 3-Bool flag triples with `enum BuildMode`

- **Pattern targeted:** 10 sites total, dominated by the
  `test_mode/bench_mode/check_mode` triple in `emit_program*`.
- **LOC reduction estimate:** **~30 LOC** (a handful of lines per call
  site).
- **Lane size:** ~0.5 day.
- **Risk:** Very low. Type-driven refactor; the typer is the enforcer.
- **Order:** **Anywhere.** Trivial; can be folded into Proposal 2's PR if a
  reviewer wants one PR less.
- **Ratio:** ~60.

#### Ranked summary

| # | Proposal | LOC saved | Days | Risk | Order vs. u8/Phase A/LSP |
| --- | --- | --- | --- | --- | --- |
| 1 | `Option` combinators + rewrite | ~840 | 1.5 | low | **before u8** |
| 2 | `if`/`else if` → `match` | ~300 | 1 | low | before LSP |
| 3 | Pipe-rewrite state threading | ~300 | 3 | low/medium | after #1, before LSP |
| 5 | `BuildMode` enum | ~30 | 0.5 | very low | any |
| 4 | Protocol-ify AST dispatch | ~700 (speculative) | 5–7 | **high** | after LSP, separate lane |

## 7 — Honest caveats

- **The "compiler vs stdlib" framing was wrong.** Stdlib code is just as
  C-style as the compiler; in some metrics (pipes per KLOC, effect rows per
  KLOC) the compiler is *more* idiomatic, not less. The real gap is
  "kaikai's `Option`-handling vocabulary doesn't exist yet as stdlib
  combinators" — that's a single lane, with downstream effects everywhere.
- **Effect-row signatures are gated by stage 1.** `stdlib/effects.kai` is
  inert documentation because stage 1 cannot parse rows. Asking the
  compiler to use effect-handler-style diagnostics today means either
  extending stage 1 (its own multi-day lane) or accepting the inline-eager
  fallback. The audit measures the gap; it does not propose closing it
  this round. See `docs/m12.8-followup.md` Bug 6.
- **Imprecision in the heuristic.** The let-chain detector requires the
  *first* argument of the next call to be the previous binding. Lots of
  legitimate pipe sites violate this (a record's *third* field happens to
  be the threaded state). Reported numbers therefore undercount, but
  refactor estimates are correctly conservative.
- **Some "C-style" patterns are intentional.** The `keyword_kind` 35-arm
  string cascade can't be a `match` until pattern-match-on-string-literal
  ships. The cascade is uglier than a `match`, but isn't a code-quality
  failure — it's a language-feature gap.
- **Hot-path concerns are not measured.** The protocol-ify proposal (#4)
  assumes O(1) impl-table dispatch is acceptable vs. a monomorphised match
  on the closed `ExprKind` tag. The honest answer is "benchmark before
  committing"; this audit doesn't have the numbers to claim a perf-neutral
  win. Tier 1 #2 (runtime efficiency) trumps Tier 2 #5 (approachability)
  on tie; if benchmarks regress, proposal 4 stays as-is.
- **Manual-error-string concat (1e) wasn't actually a problem.** The
  brief expected `"..." + "..."` style; the compiler uses interpolation
  already. The structural lift (typed `DiagKind` records) is a different
  PR entirely (Tier 2 #4 structured-compiler-output), unrelated to syntax
  cleanups. Captured for context, not as a refactor proposal.
- **The audit is read-only.** Counts come from grep/awk/python over the
  working tree; no code in `stage2/compiler.kai` or any other source file
  changed. Tier 0 should remain green by construction; verified at lane
  close.

---

### Retro section (per CLAUDE.md mandatory-retro rule)

- **Scope as planned vs. shipped:** Planned: 6-section audit + 3–5 refactor
  proposals. Shipped: exactly that. No scope drift.
- **Design decisions and alternatives considered:**
  - Considered defining "C-style" stricter (e.g. cyclomatic complexity per
    function). Rejected: not measurable without parsing the AST, which is
    out of read-only scope. Pattern-counts are the honest compromise.
  - Considered including `stage1/compiler.kai` for comparison. Rejected:
    stage 1 is intentionally minimal (no rows, no protocols, no rich
    `Option`); measuring it would conflate "kaikai-minimal" with "code
    quality".
  - The proposal ranking prioritises LOC ÷ lane-size, not gut-feel
    cleanliness. Proposal 4 (protocols) might *feel* like the biggest
    architectural win but it has the worst risk profile and lowest
    confidence interval on the estimate.
- **Structural surprises the brief did not anticipate:**
  - The "Err re-wrap" metric was zero because the compiler uses
    side-effect diagnostics, not Result. The Option re-wrap shape replaced
    it as the dominant boilerplate.
  - The compiler is more pipe-dense than the stdlib per KLOC, inverting the
    brief's assumption. The framing in §5 was rewritten as a result.
- **Fixtures added and coverage gaps:** none. Read-only audit; no fixtures
  needed and Tier 0/1 are unaffected.
- **Real cost vs estimate:** ~45 min of audit work (estimate was open-ended).
  No model rework required.
- **Follow-ups left for next lanes:**
  - Proposal 1 (Option combinators) is the recommended pre-u8 lane.
  - Proposal 2/3/5 are post-u8 candidates.
  - Proposal 4 needs its own benchmark-led design lane after LSP.
  - The stdlib idiom gap (zero pipes in 14K LOC) deserves its own audit;
    not in scope here.
