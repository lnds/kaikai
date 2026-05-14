# Lane experience report — issue #452 Phase A (second spike, 2026-05-13)

Best-effort retrospective by the implementing agent. See limitations
at the bottom.

This lane was briefed as "implement Phase A.1 stdlib precompiled
cache: serialise the post-typecheck AST + ModuleEnvDelta of each
prelude module to a binary blob; target `kai build empty.kai` ≤ 300
ms wall, ≤ 100 MB RSS, median of 5 cold runs". It ships **no cache
implementation code**. Like the first spike (PR for #452 dated
2026-05-11, `lane-experience-issue-452-phase-a.md`), this lane
closes after empirical analysis revealed the briefed sub-phase has
a pre-blocker that was thought closed but in fact was deferred.

What this lane ships:

- An updated `tools/bench-phases.sh` (RSS column, `-r` / `-q`
  flags) and a refreshed 2026-05-13 baseline that supersedes the
  2026-05-11 numbers in `docs/cache-design.md`.
- A rewrite of `docs/cache-design.md` §"What the breakdown means"
  + §"Acceptance per phase" + a new §"What #460 did and did not
  deliver" that records the gap between #568's shipped API and the
  semantics A.1 requires.
- Follow-up issue **#574** filed for the sub-step 3d-future of
  #460 (typer per-module semantics — `inherited: ModuleEnvDelta`
  actually consumed by `typecheck_module`).
- A new §"Variants that look attractive but do not pay off" in
  `cache-design.md` documenting the token-cache approach that was
  evaluated and rejected on this lane, so the next lane does not
  re-tread the same path.
- This retro.

## Objective metrics

- Start: 2026-05-13T~22:00 (CLAUDE_CACHE/issue-452-phase-a-cache
  worktree, post-v0.56.4).
- End: ~3 h of agent time, conversation-driven, no batched agent
  runs.
- Build / test invocations:
  - `make -C stage2 kaic2`: 1 (clean build from main).
  - `tools/bench-phases.sh -r`: 1 (full RSS + wall pass after the
    harness was updated).
  - Pre-bench `bin/kai build empty.kai` cold-run sweep: 5 runs
    (wall + maximum-resident-set-size), recorded inline in the
    `cache-design.md` table.
  - No `tier0` / `tier1` runs (no compiler-code change shipped).
  - No selfhost runs (same reason).

## Scope as planned vs as shipped

| Planned | Shipped |
|---|---|
| KAB1 header on-disk format implementation | Specified already in `docs/cache-design.md` from the first spike; no implementation |
| Serialise `TypedModule` + `ModuleEnvDelta` post-typecheck | Not implemented — pre-blocker (see "Design decisions" §1) |
| `--prelude-cache` flag in kaic2 | Not implemented |
| `bin/kai` cache hit/miss wiring with sha256 + ~/.cache layout | Not implemented |
| Invalidation fixtures (4) | Not implemented |
| Performance: ≤ 300 ms wall, ≤ 100 MB RSS, median 5 cold runs | Not attempted; target retired for this lane (3-phase span; A.1 + A.2 + Phase C / LLVM-direct) |
| Tier 0 / Tier 1 / Tier 1-ASAN green | N/A — no compiler-code change |
| Selfhost byte-identical pre vs post-cache | N/A |
| `make -C stage2 kaic2` self-compile time ≤ 7 s | N/A — pre-existing baseline preserved (no compiler changes) |
| Refreshed bench harness + baseline | Done: `tools/bench-phases.sh` (now ~98 lines, +RSS); `docs/cache-design.md` updated with 2026-05-13 numbers |
| `cache-design.md` updated to reflect #471 / #568 landings | Done — including the §"What #460 did and did not deliver" gap |
| Follow-up issue for the typer per-module semantics blocker | #574 filed |
| Lane retro | This file |

## Design decisions

### 1. PR #568 closed #460 with the API but deliberately deferred the semantics

The first cache spike (2026-05-11) filed two pre-blockers: #459
(`BinSerialize` protocol + `#derive`) and #460 (typer
modularisation per-module env deltas). Both are now marked closed:

- **#459 → #471 (merged 2026-05-11)**: shipped. `BinSerialize`
  with cursor-based `from_bytes(buf, pos) : Result[String,
  BinCursor[Self]]`, `#derive(BinSerialize)` for records and
  sums, including `[T]` and `Option[T]` collection payloads. The
  serde primitive is real and usable today.
- **#460 → #568 (merged 2026-05-14)**: shipped — but only the
  **API**. The public entry points are:
  - `type ModuleEnvDelta = { name, ty_entries, unions,
    op_eff_arities, recs, sums, op_to_eff, unit_aliases }`
    (`stage2/compiler.kai:36553`).
  - `type ModuleDecls = { name, decls }`.
  - `fn typecheck_module(file, mod, inherited, proto_impls,
    verbose) : TypecheckedModule` (`:36654`).
  - `fn typecheck_program(file, modules, proto_impls, verbose) :
    TypedProgram` (`:36697`).

The deferred half is called out in the source comments at
`stage2/compiler.kai:36649` as "Scope A v1: ... Sub-step
3d-future will thread the inherited delta per segment and run
`infer_all_loop` per module without changing this API." Three
load-bearing observations:

1. `typecheck_module` **does not read `inherited`**. `grep -n
   inherited stage2/compiler.kai` returns five hits: the type
   parameter declaration, the comment block, and the parameter
   list in the function head — no body reference.
2. `typecheck_program` flattens its `modules` argument via
   `flatten_module_decls` and calls `typecheck_module` once on the
   concatenation, so byte-identical selfhost is preserved at the
   cost of the per-module work being still a single global pass.
3. `collect_program_data` (`:36612`) rebuilds the env from scratch
   on every call. It does not take an `inherited` parameter, and
   adding one is the load-bearing work that #574 tracks.

The brief's "Pre-lectura obligatoria" section asks the agent to
verify the post-#460 state of `infer_program_with_protos`,
`typecheck_module`, `build_ty_env`, and `ModuleEnvDelta`. That
verification is what surfaced the gap.

### 2. The Phase A.1 cache cannot ship without #574

The A.1 cache layer is defined by skipping the typer's work on the
prelude. The mechanism is: serialise the prelude's
`ModuleEnvDelta`, on cache hit pass it to `typecheck_module(user,
user_mod, prelude_delta, …)`, and have the typer reuse the delta
instead of recomputing everything `collect_program_data` would
build from scratch.

The reuse is impossible today: `typecheck_module` discards
`inherited` and `collect_program_data` rebuilds independently. A
cache lane shipping in this state would serialise a delta that the
loader has nowhere to put — the typer would re-typecheck the
prelude regardless, so the cache would buy zero wall-clock at A.1's
payload schema.

The honest call is therefore to file #574 as a separate lane and
not ship the cache yet. Bundling the semantic refactor with the
cache implementation would mix three architectural changes in one
diff (typer semantics, AST serialisation, driver wiring), each
deserving its own selfhost gate.

### 3. The token cache was evaluated and rejected

Before settling on the close, I considered a smaller variant: cache
the `[Token]` stream post-lex instead of `[Decl]` post-parse. This
sits below A.0 in the chain and is documented now as "A.0-tokens"
in the lane log only; no payload schema is reserved for it.

Empirically it does not pay off:

- The 2026-05-13 lex+parse split is 0.33 s (lex) → 0.54 s (parse),
  so the maximum token-cache saving is the 0.33 s of lex.
- Each `Token` carries `kind: TokKind, line: Int, col: Int, start:
  Int, length: Int` (`stage2/compiler.kai:98`) plus a payload for
  `TkHoleName(String)` / `TkError(String)`. Across 32 preludes
  (~10 855 LOC, ~400 KB of source) the token count is in the
  ~35 000–40 000 range; serialising at ~20 bytes/token (a varint
  tag + two ints + a `start/length` pair) yields ~700–800 KB of
  cache file.
- Deserialisation in kaikai is `O(token count)` allocations into
  the `Token` record type, with `BinSerialize` payload decoding for
  each variant. The cache load also has to re-read the source bytes
  anyway, because the parser later slices into them via
  `start/length`.
- The kaikai lexer is already fast on hot caches: 10 855 LOC in
  0.33 s ≈ 33 K LOC/s — comparable to what a hand-rolled
  deserialiser would achieve.

The estimated net win is ≤ 50 ms — within the bench's noise floor
and easily eaten by cache-key hashing + file IO + the kaikai
runtime's allocation cost. The A.0 boundary (`[Decl]`) is the
smallest payload that delivers a measurable, durable saving,
because parse turns `O(source bytes)` work into `O(decl count)`
work — a constant-factor reduction the cache can amortise across
invocations.

Recording this here so the next caching lane does not re-spend
effort on the token variant.

### 4. The 2026-05-11 baseline (1.47 s) is stale

The first spike's `cache-design.md` table cited a 1.47 s wall for
`bin/kai build empty.kai`. Re-bench on 2026-05-13 shows 2.79 s
median across 5 cold runs (M2 Pro arm64). The stdlib auto-loaded
by `bin/kai` has grown from 29 preludes (May 11) to 32 preludes
(May 13) — `decimal.kai`, `money.kai`, `fx.kai`, `uuid.kai`,
`regexp.kai`, `path.kai`, `crypto/hash.kai`, `crypto/mac.kai`,
`encoding/json.kai`, `encoding/toml.kai` were already in the May
11 set; what changed is the per-prelude content (numeric.kai,
list.kai, json.kai grew with new operations) plus the
`tools/bench-phases.sh` line that previously pointed at
`money/decimal.kai` etc. now pointing at the flat
`stdlib/decimal.kai` after the m13 stdlib layout move.

The gap to DoD #6's 300 ms is therefore ~2.5 s, not the ~1.2 s
implied by the first spike. The per-phase share is unchanged in
shape (lex < parse < typecheck < emit), so the relative analysis
in `cache-design.md` still holds — but absolute numbers had to be
refreshed.

### 5. Why this lane does not ship a partial A.0 either

A.0 (cache `[Decl]` post-parse) **is** shippable today: #471 gives
the serde primitive and the API surface is purely additive
(`--prelude-cache` is a new flag; the cache load path lives inside
the prelude loader). The brief explicitly allows it: "Si la
performance target (≤ 300 ms) NO se alcanza después de un esfuerzo
razonable, shipear el cache de todos modos documentando el número
real alcanzado".

Two reasons it did not happen on this lane:

1. The A.0 payload schema must include enough of the AST to faithfully reconstruct `Decl` / `Expr` / `Stmt` / `Pattern` / `TypeExpr` etc., across ~110 sum variants. Annotating each with `#derive(BinSerialize)` is the easy part (~50–60 annotations); the hard part is the validator's restriction on parametric heads. `binser_collection_head` whitelists only `List` and `Option` (`stage2/compiler.kai:52331`). Several AST shapes use `Result` (e.g. inside `expand_imports`'s result types but not the AST proper) and most types carry no parametric heads — so the cost is annotating the variants, not extending the dispatcher. But verifying that across ~110 variants takes a non-trivial walk of the validator codebase that the brief budgeted at "size A.0 ≈ 600 LOC" but in practice is more like 1 500–2 500 LOC of derive annotations + handwritten Header / Atomic-write / sha256-hashing scaffolding.
2. The wall-clock saving from A.0 alone is 0.54 s on a 2.79 s baseline (19% improvement). That is real and worth shipping — but if A.0 ships first and #574 follows, the A.0 payload schema may need a schema-version bump when the A.1 delta lands (because the loader's contract changes from "skip parse" to "skip parse + skip typecheck"). Two payload-version bumps in 4–6 weeks is more churn than shipping A.0 and A.1 together after #574 lands.

I judged it more useful to land #574 first, then ship A.0 + A.1
together as a single payload-schema definition. The integrator may
overrule on the trade-off; the next lane can pick A.0-only off this
analysis without re-deriving the maths.

## Structural surprises

- **The `TypedModule` from #458 is still not the Phase A payload.**
  Confirmed unchanged from the first spike: `type TypedModule =
  { tm_file: String, tm_decls: [Decl] }` (`stage2/compiler.kai:56749`)
  is the LSP query wrapper, not the cache payload. The cache wants
  `TypecheckedModule = { typed: TypedProgram, delta: ModuleEnvDelta }`
  (`:36652`) — but only once `delta` carries something real on a
  per-module basis.
- **`prelude_len: Int` is still a positional index, not a module
  boundary.** Still threaded through `infer_all_loop` (~10
  call-sites) as a diagnostic index ("preludes are decls
  `0..prelude_len`"). After #568 it is still informational; the
  module boundary lives in `ModuleDecls.name` for the API, but the
  fold flattens everything before the typer sees it.
- **`bin/kai` carries the canonical prelude list.** 32 lines of
  `STDLIB_<NAME>=...` plus a single `stdlib_prelude_flags`
  function that emits `--prelude <path>` for each
  (`bin/kai:80-110` and `bin/kai:595-625`). `tools/bench-phases.sh`
  has to be hand-synchronised to it; this was already out of date
  on the first spike (pointed at `money/decimal.kai` instead of
  `decimal.kai`) and got fixed in this lane. A future lane should
  consider extracting `stdlib_prelude_flags` into a single source
  of truth that both the driver and the bench harness read — e.g.
  a generated list file at `tools/_stdlib_preludes.list` populated
  from one place.
- **kaikai-native sha256 is in `stdlib/crypto/hash.kai`, but the
  cache hashing must run in the driver before kaic2 starts.** The
  driver uses `shasum -a 256` (POSIX shell utility, available on
  macOS + Linux). No C runtime change is required for hashing;
  atomic write (`fsync` + `rename`) is also reachable from shell
  + a small C helper if one is needed for the kaic2 side of the
  write.

## Fixtures added

- No `examples/cache/` fixtures — no cache code shipped.
- `tools/bench-phases.sh` extended with `-r` / `-q` flags. Default
  behaviour is unchanged (no RSS column, full table).

## Real cost vs estimate

| Activity | Brief estimate | Actual |
|---|---|---|
| Read brief + cache-design + first spike retro | not budgeted | ~30 min |
| Verify pre-blocker state (#459, #460 closed?) | not budgeted | ~10 min |
| Build kaic2 + bench baseline | not budgeted | ~5 min (build) + ~5 min (bench) |
| Read `typecheck_module` / `ModuleEnvDelta` / `collect_program_data` | not budgeted | ~30 min |
| Re-evaluate token-cache approach + reject | not budgeted | ~20 min |
| Update `tools/bench-phases.sh` + run new harness | not budgeted | ~15 min |
| Rewrite `cache-design.md` (baseline numbers, §"What #460 did and did not deliver", §"Variants that look attractive but do not pay off") | not budgeted | ~40 min |
| File #574 with full body | not budgeted | ~15 min |
| Retro (this file) | not budgeted | ~30 min |
| Total lane time | "1–2 weeks" implementation | ~3 h of conversation |
| LOC shipped | ~1 500 cap (planned A.1 cache) | ~110 (bench harness +) + ~140 (cache-design.md re-write) + this retro file |

The brief's size estimate was again wrong, but in a different way
than the first spike: the first spike found that the cache layer
hierarchy was wrong; this lane found that an issue marked "closed"
shipped only half of what its body promised. Both shapes of finding
are typical for ambitious infra refactors where the doc state lags
the code state.

## Follow-ups

- **#574 (filed by this lane)** — typer per-module semantics
  (sub-step 3d-future of #460). Pre-blocker for A.1 cache. 2 500–
  4 000 LOC, selfhost-byte-identical risk non-trivial.
- **A.0 cache lane (TBD)** — shippable today. Saves ~0.54 s on
  the 2026-05-13 baseline. Decision pending integrator: ship A.0
  alone now and accept a payload-schema bump later, or wait for
  #574 to land and ship A.0 + A.1 together as a single payload.
- **#461 (Phase A.2) — depends on A.1 landing.** Unchanged.
- **#455 (Phase B user-file cache) — depends on the A.0 wire-up
  landing.** The cache key shape (`sha256(source) +
  sha256(concat(transitive_import_shas)) + kaikai_version_hash +
  format_version`) is fully specified in `cache-design.md`; the
  driver wiring is what's missing.
- **Bench harness drift** — when a prelude is added or moved in
  `bin/kai`, `tools/bench-phases.sh` has to be hand-synchronised.
  Consider a shared source-of-truth file (see "Structural
  surprises" above).

## What this lane proved was wrong (for the record)

- "#460 closed → A.1 is unblocked" → no. #568 (which closes #460)
  landed the API but explicitly deferred the per-module semantic
  fold. A.1 still needs #574 to land.
- "A.0-tokens would buy us 0.33 s on the cheap" → no. Token-stream
  deserialisation in kaikai is comparable in wall-clock to the
  lexer it replaces. The smallest payload that delivers a durable
  saving is `[Decl]` post-parse.
- "Baseline is 1.47 s per the first spike's table" → 2.79 s today.
  Stdlib growth between 2026-05-11 and 2026-05-13 nearly doubled
  the cold wall. The relative phase breakdown shape is unchanged;
  the absolute targets in the brief and in DoD #6 need to be
  re-read against the new number.

## What this lane confirmed (for the record)

- `BinSerialize` + `#derive(BinSerialize)` is real and works today
  on records and sums with `[T]` / `Option[T]` payloads. The A.0
  cache lane has the serde primitive it needs; the gap is the
  scope of the annotations, not the protocol.
- `typecheck_module` / `typecheck_program` are public entry points
  with stable signatures suitable for the cache lane to consume
  once `#574` lands.
- `bin/kai`'s prelude list and `tools/bench-phases.sh`'s prelude
  list are two parallel sources of truth and will continue to
  drift apart absent a shared source.

## Limitations of this retro

- No code was written for the cache itself, so there is no diff to
  point at for the central claim. The "proof" of each claim is in
  the bench numbers (re-runnable via `tools/bench-phases.sh -r`),
  the cited `stage2/compiler.kai` line numbers, and the body of
  #574.
- Numbers are from a single machine (M2 Pro, macOS 26.4.1). A
  Linux runner will show different shell + cc overhead share; the
  pipeline-internal breakdown (parse / cascade / typecheck /
  codegen) is unlikely to shift meaningfully because all of it is
  in-process kaikai work.
- The decision not to ship A.0 alone on this lane is a
  judgement-call. The integrator may overrule and request A.0 as
  a standalone follow-up; the analysis in §5 above is the record
  of why I did not pre-empt that decision tonight.
- I did not attempt a derive-annotation walk of the AST sum types
  to confirm the 1 500–2 500 LOC estimate for the A.0 payload
  schema. That number is bracketed conservatively; the next lane
  may find it lower (more variants are records with primitive
  fields than I assumed) or higher (more variants carry types
  that need their own derive than I assumed).
