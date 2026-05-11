# Lane experience report — issue #452 Phase A (spike + re-scope)

Best-effort retrospective by the implementing agent. See limitations
at the bottom.

This lane was briefed as "implement Phase A stdlib precompiled cache:
serialise typed AST to disk, deserialise on hit, target ≤ 300 ms".
It ships **no implementation code** for the cache itself. It ships
the empirical bench harness that retired the issue's central
assumption, a corrected design doc, and three follow-up issues that
re-scope the work. The integrator decided to cut the lane here once
the bench numbers showed the original plan would not have closed
DoD #6 even at 5–10× its briefed size.

## Objective metrics

- Start: 2026-05-11T10:50 (rebase from main; #458 had just merged)
- End:   2026-05-11T14:?? (commit + PR)
- Wall-clock: ~3 h of agent time, conversation-driven, no batched
  agent runs.
- Build/test invocations:
  - `make -C stage2 kaic2`: 1 (clean build from main).
  - `tools/bench-phases.sh`: ~6 runs across the bench iterations.
  - No `tier0` / `tier1` runs (no compiler-code change shipped).
  - No selfhost runs (same reason).

## Scope as planned vs as shipped

| Planned | Shipped |
|---|---|
| KAB1 header on-disk format | Specified in `docs/cache-design.md`; no implementation |
| sha256 + atomic write FFI in runtime | Not implemented |
| Handwritten serializer per AST node type (~600 LOC) | Not implemented; route rejected (see below) |
| `--prelude-cache` flag in kaic2 | Not implemented |
| `bin/kai` cache hit/miss wiring | Not implemented |
| 4 invalidation fixtures | Not implemented |
| Tier 0 / Tier 1 / Tier 1-ASAN green | N/A — no compiler-code change |
| `bin/kai build empty.kai` ≤ 300 ms warm | Not attempted; target retired as unreachable in a single lane |
| Lane-experience retro | This file |
| Empirical bench harness | `tools/bench-phases.sh` (47 lines) |
| Honest re-scope of `cache-design.md` | Done (~140 lines reworked) |
| Follow-up issues for A.0/A.1/A.2 | Opened |

## Design decisions

### 1. The "1.3 s is re-parsing" claim in the #452 issue body is imprecise

Three control measurements (n=5 median, M2 Pro arm64, 2026-05-11)
on `bin/kai build empty.kai` with the post-#458 compiler:

| Stage | Wall | Delta |
|---|---|---|
| `kaic2 --help` (startup) | 0.003 s | — |
| `kaic2` no preludes + empty.kai | 0.003 s | — |
| `kaic2 --tokens` 29 preludes + empty.kai | 0.23 s | 0.23 s lex+parse of preludes |
| `kaic2 --ast` 29 preludes | 0.34 s | +0.10 s pre-typer cascade |
| `kaic2 --infer` 29 preludes | 0.77 s | +0.43 s typecheck |
| `kaic2` default (emit C) 29 preludes | 1.19 s | +0.42 s codegen on prelude |
| `bin/kai build empty.kai` total | 1.47 s | +0.28 s cc + shell |

The 1.19 s of `kaic2` wall is **100% prelude processing across all
pipeline stages**, not "re-parsing". Only 0.23 s is lex+parse; the
remaining 0.96 s is the cascade, typer, and codegen running over the
merged prelude++user `[Decl]` because the typer and codegen have no
per-module boundary today.

The user's empty file processes in ~3 ms. The pipeline is fast; what
is slow is doing it on 10 290 lines of stdlib at every invocation.

### 2. A.0 cache alone cannot reach ≤ 300 ms

This was the breakthrough the issue body was missing. The cache
*layer* matters more than the cache *format*:

- A.0 (post-parse cache) saves 0.23 s. Wall → 1.24 s.
- A.1 (post-typecheck cache, requires typer refactor) saves another
  0.43 s. Wall → 0.81 s.
- A.2 (post-perceus cache + emit-only-user, requires A.1) saves the
  remaining 0.41 s of codegen on prelude. Wall → 0.40 s.
- Closing the final ~0.10 s to 300 ms requires Phase C
  (daemon-amortised cc + shell) or LLVM-direct emission.

DoD #6 (≤ 300 ms cold) is the **endpoint** of a multi-lane Phase A
roadmap, not a single-lane gate. The lane briefed under #452 cannot
close DoD #6 on its own at any reasonable size.

### 3. `Serialize` protocol gaps block kaikai-native A.0

The kaikai-idiomatic path for A.0 is `#derive(Serialize)` on the AST
types. The integrator pushed back correctly when an early estimate
proposed 2 500–4 000 LOC of handwritten serde: that is "not using
the language well". Investigation revealed two real gaps that block
the derive approach:

1. **No `derive_serialize_impl` in the compiler.** The dispatcher
   in `stage2/compiler.kai:45590` covers Show / Eq / Hash / Ord;
   Serialize is absent. Adding it follows the m12.8.z precedent
   (~150–200 LOC) but is its own change.
2. **`Serialize.from_string : String → Result[String, Self]` is
   whole-string.** For nested AST it needs cursor semantics
   (`from_bytes : ([Byte], Int) → Result[E, (Self, Int)]`) or a
   sibling `BinSerialize` protocol. Changing `Serialize` is a
   public-protocol change to `stdlib/protocols.kai`; adding
   `BinSerialize` avoids breakage but doubles the surface area.

Either path is a design pass of its own. The lane that ships A.0
must resolve gap (1) and gap (2) before any cache code lands.

### 4. Handwritten serde was the fallback if Serialize cannot be extended

Estimated cost of handwritten binary serde over ~110 sum variants
across `Decl/Expr/Pattern/Stmt/TypeExpr/RowExpr/TypeBody/ImportKind/
ExprKind/PatKind/TyKind/UnitExprT/...`: ~2 500–2 800 LOC in
`stage2/compiler.kai`, plus ~300 LOC of C runtime (sha256, atomic
write, fsync) and ~150 LOC of bin/kai wiring. Total ~3 000–3 300 LOC.

This route was rejected per the integrator's direction to use the
language's protocol machinery instead. The estimate stays in the
record for the next lane that re-evaluates the trade-off.

### 5. Why this lane closes without shipping A.0

After two pre-blockers materialised (Serialize cursor, derive
support) the honest call is to file them as their own issues and
close this lane on the doc + harness + retro. Pushing through to a
fait-accompli A.0 in one PR would mix (a) extending Serialize,
(b) extending derive, and (c) implementing the cache — three
distinct architectural changes in one diff. Each deserves its own
review and selfhost gate.

## Structural surprises

- **`TypedModule` from #458 is not the Phase-A payload.** The
  comment in `stage2/compiler.kai:50329` correctly says "Phase A
  widens this to a serialisable form" — the form is yet-to-be-built.
  `{ tm_file, tm_decls }` is a query-only wrapper for LSP hover /
  goto-def; it does not carry the env deltas a post-typecheck cache
  needs. #458 is still load-bearing for #447 (LSP v1).
- **`--prelude` passes raw `[Decl]`, not typed.** `compile_source`
  (`stage2/compiler.kai:51101`) takes `prelude_decls: [Decl]` from
  `load_prelude` and merges them with user decls before any
  pre-typer pass runs. The typer therefore has no concept of "this
  decl came from a prelude with a known typed env"; the
  `prelude_len: Int` is just a positional diagnostic index. This is
  the structural fact that makes A.1 a real refactor, not a serde
  exercise.
- **Dead-code elimination at emit already works.** `kaic2` on
  `empty.kai` with `core/*.kai + protocols + effects + array` in
  scope emits 235 lines of `out.c` with 39 `kai_*` symbols — none
  of them prelude map/filter/fold/reduce. The 0.42 s of codegen on
  prelude is *deciding* what to emit, not emitting all of it. A.2's
  win is caching that decision, not the emission.
- **The kaikai-native protocol `Serialize` exists but is
  config-shaped.** Four impls (Int / Real / String / Bool); no
  polymorphic list / Option / Result impls; no derive. The protocol
  is well-named but designed for a different use case.

## Fixtures added

- `tools/bench-phases.sh` — 47 lines. Reproducible phase-by-phase
  bench harness, portable (resolves repo root relative to script).
  Re-run after any caching lane lands to confirm the breakdown
  still holds.
- No `examples/cache/` fixtures (no cache code shipped).

## Real cost vs estimate

| Activity | Brief estimate | Actual |
|---|---|---|
| Spike feasibility | not budgeted | ~30 min |
| Bench harness | not budgeted | ~30 min |
| Re-bench after intuition push | not budgeted | ~30 min |
| Doc rewrite | not budgeted | ~45 min |
| Retro + issue prep | not budgeted | ~30 min |
| Total lane time | "1–2 weeks" implementation | ~3 h of conversation |
| LOC shipped | ~1 500 cap | ~190 (140 doc edits + 47 bench script + this file) |

The brief was wrong about size. The lane was right-sized to what it
actually produced: an honest scope correction.

## Follow-ups

Three issues filed alongside this PR:

1. **#459 — Extend `Serialize` (or add `BinSerialize`) for
   cursor-based stream parsing + add `derive_serialize_impl`.**
   Pre-blocker for the A.0 lane. Design pass needed; not a
   one-line change.
2. **#460 — Typer modularisation refactor.** Pre-blocker for A.1.
   Touches `build_ty_env` and `infer_program` to produce per-module
   env deltas. Est. 2 500–4 000 LOC, selfhost risk non-trivial.
   Without this, A.1's payload cannot be defined.
3. **#461 — Phase A.2 — post-perceus cache + emit-only-user.**
   Depends on A.1. Required for the codegen 0.41 s to disappear.
   The lane that ships this also closes ~80% of DoD #6.

#452 stays open as the Phase A epic and gets re-scoped (body update
in this PR) from a single-lane implementation to a parent issue with
A.0 / A.1 / A.2 as sub-issues.

## What this lane proved was wrong (for the record)

- "1.3 s is re-parsing" → no, 0.23 s is parse; the rest is the
  pipeline running on prelude across stages.
- "Serialise TypedModule" → TypedModule from #458 is not what the
  cache wants; it's an LSP query wrapper.
- "Single lane, ≤ 1 500 LOC, closes DoD #6" → DoD #6 closure
  spans three lanes plus a daemon/LLVM-direct piece.
- "Use derive(Serialize)" was the right intuition but the
  protocol's current shape blocks it; the lane that uses derive
  also extends the protocol.

## Limitations of this retro

- No code was written so there is no diff to point at. The "proof"
  of each claim is in the bench numbers + the cited
  `stage2/compiler.kai` line numbers.
- The numbers are from a single machine (M2 Pro, macOS 26.4.1). A
  Linux runner may show a different shell + cc overhead share; the
  pipeline-internal breakdown (parse / cascade / typecheck /
  codegen) is unlikely to shift meaningfully because it is all
  in-process kaikai work.
- I (the agent) zig-zagged through the scope multiple times before
  landing here. The conversation log in the PR description is the
  honest record; future lanes should expect similar exploration
  when a brief's central assumption turns out empirically wrong.
