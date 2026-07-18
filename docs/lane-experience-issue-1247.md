# Lane experience — issue #1247 (flip `Result` to Ok-first)

## Scope as planned vs. as shipped

**Planned:** flip `Result` from Err-first (`Result[e, a]`, error in
slot 0) to Ok-first (`Result[a, e]`, success first) to match Rust,
staged compiler-first, with `map` transforming the success (Rust
semantics, abandoning the mechanical "map = last tyarg" rule). Four
stages: compiler internals, stdlib, stage0/stage1 bootstrap, and
examples/fixtures/docs plus a `kai migrate` rule.

**Shipped:** exactly that, across 79 files. The flip is entirely a
**type-level** change — the surface `Result[X, Y]` argument order and
the typer's builtin schemas — because `Ok`/`Err` are named
constructors with fixed runtime tags (Ok=2, Err=3). No emit, KIR,
Perceus, or runtime lowering changed: they project slot 0 of the
constructor's own payload, which is independent of the type-argument
order. That single structural fact is what kept the blast radius to
annotations and schemas rather than the whole backend.

## The two-phase bootstrap (the reusable lesson)

The compiler is written in kaikai and self-hosts. Flipping the
`Result` convention means the compiler that compiles the flip is
itself compiled against the flip. The safe sequence:

1. **Edit the typer + the compiler's own internal `Result` uses**
   (infer.kai builtin schemas, the `!` operator's positional
   destructure, the `BinSerialize` derive in protos.kai). At this
   point the *old* `kaic1` (unchanged, Err-first) still compiles the
   *new* stage2 source, because the only things that moved are type
   annotations and schema orderings — the `Ok`/`Err` constructor tags
   the emitted code depends on did not move. `make kaic2` succeeds.

2. **Run `make selfhost`.** The freshly-built Ok-first `kaic2` must be
   a fixed point: `kaic2` compiling itself emits byte-identical C. On
   the first try it did *not* close — the byte-id diff surfaced a
   `Result[String, BinCursor[Real]]` vs `Result[BinCursor[Real],
   String]` mismatch. That was the signal that **stage 1 (compiler)
   and stage 2 (stdlib) are coupled at the `BinSerialize` protocol**:
   the compiler's derive emits the Ok-first shape, so the stdlib's
   `BinSerialize` protocol declaration had to flip in the same step,
   not a later one. The plan's stage boundary was an illusion here —
   the derive and the protocol it targets land together.

3. **Flip the stdlib** (`core/result.kai` type + helpers, then the
   152 sites across 16 files), rebuild, and `make selfhost` closes.

The lesson: **selfhost byte-id is the exact instrument that catches a
half-flipped compiler.** It does not merely confirm determinism — a
byte-id diff points at the specific type whose order is inconsistent
between the compiler's output and the stdlib the compiler embeds. When
it fails, read the diff *as a bug report*, do not force the next stage.

## Semantic (not textual) flip — the `write_file` trap

The flip is per-site semantic, never a blind `[a, b] -> [b, a]`.
`write_file` shipped Err-first as `Result[TyUnit, TyString]` — which,
read Err-first, meant Err=Unit/Ok=String, the inverted #771 bug. Under
Ok-first the *same text* `Result[Unit, String]` now correctly means
Ok=Unit/Err=String, so `write_file` was **left unchanged textually and
the bug fixed by the convention flip alone**. Every other `Result[Err,
Ok]` had to be read for which arg was the error before flipping:
`file_delete: Result[String, Unit]` → `Result[Unit, String]`,
`int_to_byte: Result[String, Byte]` → `Result[Byte, String]`, etc.
The `ty_result_*_zero` helpers the #771 retro warned about no longer
exist in the tree (refactored out earlier); the workaround they
embodied is gone for good with the flip.

## The `kai migrate` rule + the kaic1 bundle trap

The migrator only rewrote *expression bodies* before this lane; type
annotations passed through untouched. Added `migrate_types.kai`: a
type walk that rebuilds every two-argument `Result[a, b]` as
`Result[b, a]` across return types, params, `let`/`var` annotations,
record/variant field types, effect/protocol op signatures, and impl
targets. Wired into `mig_decl` (return + params + const + type body +
effect/protocol ops + impl target) and `mig_stmt`/`ELambda` (body
annotations).

Two kaic1 bundle-miscompile traps bit here (both in
`project_kaikai_kaic1_variant_slot_read_traps`):

- **Passing a variant's `[TypeExpr]` slot positionally to a helper
  that re-matches it corrupts it** — the first draft split the flip
  into `mig_tyname(name, args)` doing `match args { [a,b] -> ... }`,
  and the output silently *dropped all type arguments* (`Option[Int]`
  → `Option`). Fix: keep the `[a, b]` match inside the fn that already
  `map`ped the list (`mig_result_args` on the fresh post-`map` list),
  never hand the raw slot to a re-matching helper.
- **A binder literally named `args` reads empty** — renamed every
  `args` binder to `targs`/`vargs`.

The migrator is **one-shot**, not idempotent: a type-argument reorder
has no post-flip marker, so re-running it flips back. Added a
`.oneshot` marker convention to `tests/migrate_fixtures.sh` that skips
the idempotency assertion for such fixtures, and documented it.

## Goldens updated, not masked

The `!`-operator diagnostic format strings render the operand/return
`Result[…]` in argument order, so two `.err.expected` goldens changed
(`Result.String, Int.` → `Result.Int, String.`,
`.Result.E, T..` → `.Result.T, E..`). `poly_show`/`Show for Result`
goldens did **not** change — `show` prints `Ok(x)`/`Err(e)` by
constructor name, order-independent. That asymmetry (diagnostics move,
constructor output does not) is the tell that the flip is type-level
only.

## Stage 3 (stage0/stage1) was near-empty

stage0/check.c registers `Ok`/`Err` as *scope names* only — no
type-argument order. stage1 uses its own `StrResult` (`StrOk`/
`StrErr`), never the builtin `Result`. So the whole bootstrap chain
below stage2 carries no observable Err-first order: the only stage-3
work was correcting Err-first *comments* in both `runtime.h` copies
(the runtime constructs `Ok`/`Err` by fixed tag, so the code was
already correct; the comments lied).

## Fixtures added

- `examples/stdlib/result_ok_first_flip.kai` (+`.out.expected`) —
  exercises `!` propagation, `result.map` on success, and
  `result.map_err` on the error together under Ok-first. Wired into
  `test-stdlib` (tier1).
- `examples/migrate/result_ok_first.input.kai` /
  `.expected.kai` / `.oneshot` — proves the `kai migrate` type flip
  across fn signatures, `let` annotations, variant fields, and params.
  Wired into `test-migrate` (tier1) via `migrate_fixtures.sh`.

## Gates

selfhost byte-id closed at every stage boundary (the load-bearing
gate); tier0 green (34 OK, deterministic, demos baseline 37); tier1
targets exercised locally: test-stdlib, test-sugars, test-effects,
test-protocols, test-unions, test-ufcs, test-migrate, test-fmt,
test-fmt-selfhost, test-negative all pass. `km score`
`migrate_types.kai` A++ (98.1).

## Follow-ups

- The migrator flips *any* two-arg `Result`, so a codebase already on
  Ok-first would be corrupted by a second run. This is inherent to a
  markerless type-order migration; the `.oneshot` convention documents
  it but does not enforce single-application. An edition-version guard
  (only run when the source declares the pre-flip edition) would make
  it safe to re-run; deferred as out of scope for the flip itself.
