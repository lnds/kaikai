# Lane experience report — issue #452 Phase A.0 step 3 (driver wire) — STOPPED

The lane was scoped to wire kaic2's `--prelude-cache` + bin/kai's
hit/miss shell + 4 invalidation fixtures on top of the PR #584 cache
serdes layer (Step 2). It stopped early after the diagnostic phase
revealed that the cache layer cannot serialise any prelude with an
`ECall` body — i.e. essentially all real preludes. The bug is filed
as #585; this lane parks until that ships.

## Objective metrics

- Start: 2026-05-14 (post-v0.58.0, post-#584 merge).
- End:   2026-05-14, same day, ~1.5 hours.
- LOC added: 0 (all local edits reverted before push).
- Issues filed: 1 (#585).
- Selfhost: not affected (no committed changes).

## Scope as planned vs as shipped

| Planned (brief) | Shipped |
|---|---|
| kaic2 `--prelude-cache <source> <cache>` flag | implemented locally, validated against synthetic test, NOT committed |
| kaic2 `--emit-cache` mode | implemented locally, NOT committed |
| `--prelude-cache-sha <sha>` flag | implemented locally, NOT committed |
| `CliOptions.preludes : [PreludeSpec]` (vs `[String]`) | implemented locally, NOT committed |
| `load_prelude_cached` reading `.kab` + deserialising | implemented locally, NOT committed |
| bin/kai sha256 + cache hit/miss + atomic write | not started |
| 4 invalidation fixtures | not started |
| Closing PR with `closes #452` | not opened (blocked by #585) |

## What blocked the lane

The cache serdes layer that PR #584 shipped (`cache_serialize_module`
+ `cache_deserialize_module` over `[Decl]`) only round-trips the
synthetic 4-decl fixture used in `cache_roundtrip_self_test()`. That
fixture's two `Expr` bodies are `EInt(42)` and `EReal(3.14159)`, both
of which encode without ever invoking `cache_expr_list_to_hex`. The
first time a real `[Decl]` flows through the encoder — i.e. one
where any `DFn.body` is or contains an `ECall` — `cache_expr_list_loop`
panics on a non-exhaustive `match xs { [] -> ... [h, ...t] -> ... }`.

Diagnostic trace: patched `emit_match_expr` in `stage1/compiler.kai`
to print `_scr->tag` and the `scrut.line/col` before the panic. The
loop receives `TAG=10` (`KAI_CLOSURE` per `stage0/runtime.h:101`)
where it expects `KAI_CONS` / `KAI_NIL`. The bug reproduces with the
following addition to the existing self-test:

```kai
let probe_arg = Expr { kind: EInt(7), line: 5, col: 20, ty: None, mode: MUnknown }
let probe_args : [Expr] = [probe_arg]
let g_var = Expr { kind: EVar("g"), line: 5, col: 19, ty: None, mode: MUnknown }
let call_body = Expr { kind: ECall(g_var, probe_args), line: 5, col: 18, ty: None, mode: MUnknown }
# add `DFn(true, "callg", [], [], Some(int_ty), row, call_body, 5, 1, None)` to the decls list
```

Renaming `args` to `as_` in the `ECall` / `EIntrinsic` match arms
of `cache_exprkind_to_hex` (memory `feedback_kaikai_param_args_shadow.md`
warns about this exact shadow against `kai_prelude_args`) did NOT
fix the panic. The root cause is somewhere deeper — either in
stage1 codegen for the variant destructure of `ECall(f, as_)` (the
extracted `_scr->as.var.args[1]` is reaching the loop via a closure
chain that Perceus' RC is losing) or in how the `cons` cell tail
extracted by `kai_incref(_scr->as.cons.tail)` interacts with the
surrounding `kai_concat_all([...])` chain. Full hypothesis list in
#585.

## What was implemented locally (reverted)

The wire path itself is straightforward — once #585 ships, the
following can be reapplied with no design surprises:

1. **`Mode` enum** gains `MEmitCache` (parses `--emit-cache`). The
   mode's `run` arm loads the single `--prelude` source, calls
   `cache_serialize_module(decls, sha)`, and prints the blob to
   stdout. bin/kai redirects stdout into a tmpfile and atomically
   renames it into the cache dir.

2. **`CliOptions.preludes` type change** from `[String]` to
   `[PreludeSpec]` where `PreludeSpec = PSrc(path) | PCached(source_path, cache_path)`.
   `PCached` carries the source path alongside the cache path so the
   resulting `ModuleEntry` origin matches what plain `--prelude` would
   produce (path threading is load-bearing for diagnostics + qualified
   imports — `prelude_spec_source` is the helper).

3. **`--prelude-cache <source> <cache>` flag** (two-arg) and
   **`--prelude-cache-sha <sha>` flag** (single arg) in
   `parse_cli_loop`. Three new setters: `cli_with_prelude_cache`,
   `cli_with_emit_cache_sha`, plus existing `cli_with_prelude` left
   untouched. The new field `emit_cache_sha: Option[String]` threads
   through every setter.

4. **`load_preludes`** dispatches on each spec: `PSrc -> load_prelude`,
   `PCached -> load_prelude_cached`. The latter reads the `.kab`
   blob via `read_file`, runs it through `cache_deserialize_module`,
   reuses `prelude_module_name` / `strip_prelude_tests` /
   `collect_pub_exports` / `tag_decls_module_origin` to build the
   same-shape `PreludeLoaded` the parser-path produces.

5. **Fail-loud policy in `load_prelude_cached`**: any error path
   (read failure, deserialize returns `None`, etc.) emits a clear
   message to stderr (`kaic2: cache invalid at <path> — re-run with
   --prelude <source> to regenerate`) and returns `None`. The
   caller already treats `None` as fatal, so `kaic2` exits 1.
   bin/kai's hit/miss shell would catch this by deleting the stale
   cache before retrying with `--prelude`.

The whole change set was ~120 LOC of `stage2/compiler.kai` plus
~30 LOC of `usage()` / docstring updates. No bin/kai work landed,
no invalidation fixtures landed. All edits reverted via `git
checkout` before pushing.

## Design decisions that did land in the diagnostic phase

### 1. `--emit-cache` as a new mode, not `--cache-write <path>`

The brief proposed two designs:
- `kaic2 --prelude <source> --cache-write <path>` (kaic2 writes the file)
- `kaic2 --emit-prelude-cache <source> <path>` (single call precompute)

Local choice: a new mode (`--emit-cache`) that *emits the blob to
stdout* and lets bin/kai handle the file write. Reasons:

- bin/kai already handles atomic tmpfile + rename in the kai-pkg
  module install path; reusing that pattern keeps the file-write
  invariant in one place.
- kaic2 stays pure to its current contract (one source in, one
  artifact stream out — same shape as `--emit=llvm`, `--fmt`,
  `--diags-json`).
- The error path is simpler: kaic2 exits 1 with stderr, bin/kai sees
  the empty stdout + nonzero exit, drops the tmpfile, falls back to
  `--prelude`. No "partial write left behind" failure mode.

This choice did not survive merging because the underlying
`cache_serialize_module` call panics; #585 needs to ship before the
mode is usable.

### 2. `PreludeSpec` sum type vs parallel `[Option[String]]`

Considered `preludes: [String], prelude_caches: [Option[String]]`
(parallel lists, indexed in lockstep). Rejected: more error-prone
than a sum type, and stage 1 has no problem with two-variant sums
in this shape (the cache-design memo notes single-variant quirks,
not multi-variant). `PreludeSpec` reads better at every site.

### 3. Cache-path argument shape: two positional vs separator-delimited

Two positional args (`--prelude-cache <src> <cache>`) vs a single
colon-delimited form (`--prelude-cache <src>:<cache>`). Two positional
is simpler to shell-escape (no need to worry about colons in paths
on weird filesystems) and matches the existing `--path` style. The
parser's "consume next two tokens" pattern is already established
by `match more { [a, b, ...rest2] -> ... }` shapes elsewhere in
`parse_cli_loop`.

### 4. SHA computation in bin/kai, not kaic2

kaic2 takes the SHA as a flag (`--prelude-cache-sha`) rather than
computing it itself. This keeps kaic2's dependency on hashing primitives
nil and lets bin/kai use `shasum -a 256` (already in coreutils on
both macOS and Linux). bin/kai is also the layer that already knows
how to time-stamp + cache-key, so SHA computation belongs there.

## Structural surprises

- **The cache layer's round-trip test is a fixture that does not
  exercise its own code path.** PR #584's `cache_roundtrip_self_test`
  builds a 4-decl `[Decl]` whose Expr bodies are `EInt(42)` and
  `EReal(3.14159)`. The encoder for those two never calls
  `cache_expr_list_to_hex` (no recursive sub-expressions). The
  fixture passes; the layer ships dormant; the first real prelude
  blows up. PR #584's retro acknowledged this limitation explicitly
  ("Round-trip test is synthetic — it does not exercise a real
  prelude. Until a real prelude flows through the encoder (Step 3),
  there is a real risk that some ExprKind / PatKind / TyKind variant
  has a subtle bug") — that warning landed correctly, and Step 3
  is where the bug surfaces.

- **Stage 1's panic location only points at the scrutinee, not the
  expected variant.** "non-exhaustive match" reports the
  `match SCRUT` site but not which arm SCRUT didn't satisfy. Without
  the runtime tag I patched in, the bug would have been very hard
  to bisect. Worth filing as a stage1 tooling improvement: every
  non-exhaustive panic should at least print the scrutinee's tag
  for variant scrutinees (no extra cost; the tag is right there).

- **The `feedback_kaikai_param_args_shadow.md` rule is necessary
  but not sufficient.** Memory says "never name a binding `args`"
  because it shadows `kai_prelude_args`. PR #584 already obeyed
  this in `cache_exprkind_to_hex` (the binding is `args` literally
  in source; the param's resolution to `kai_prelude_args` does
  surface in the generated C in some compile passes but not here).
  Renaming to `as_` cleared the literal shadow but did not fix the
  symptom — the root cause is something else.

## Fixtures added

None. All would have referenced the broken `cache_serialize_module`.
Two negative fixtures will land alongside the #585 fix:

1. The `ECall` body extension to `cache_roundtrip_self_test`
   (above) — must pass before the cache layer is considered live.
2. A full-prelude round-trip: parse `stdlib/core/string.kai`,
   serialise, deserialise, compare blobs. This is the real
   acceptance gate.

## Real cost vs estimate

| Activity | Brief estimate | Actual |
|---|---|---|
| Read brief + #584 retro + bin/kai + cache entrypoints | not budgeted | ~10 min |
| Local edit: `--prelude-cache` + `--emit-cache` + `PreludeSpec` | included in 450 LOC envelope | ~15 min |
| First selfhost build + `--cache-roundtrip-test` (passed) | not budgeted | ~5 min |
| First `--emit-cache` test (panic) | not budgeted | ~5 min |
| Bisecting the panic (location, scrutinee, tag) | not budgeted | ~30 min |
| Filing #585 with repro + diagnostic | not budgeted | ~15 min |
| Reverting + writing this retro | not budgeted | ~15 min |
| **Total** | "1-1.5 day for steps 3+4+5+6" | ~1.5 hours, 0 LOC, lane stopped |

## Follow-ups

- **#585 (filed)** — `cache_expr_list_loop` panics with TAG=10
  closure when serialising any `ECall` body. **Blocks Step 3 entirely.**
- **Step 3 restart** — once #585 ships, re-apply the local
  implementation described above. The wire path is already worked
  out; expect ~120 LOC and ~30 min if the cache layer is healthy.
- **Step 4 (bin/kai)** — start cleanly once #585 ships. The plan
  remains: `shasum -a 256` lookup, `~/.cache/kaikai/preludes-v1/<sha>.kab`,
  `--prelude-cache` on hit, `--emit-cache` redirected to tmpfile +
  `mv` atomic on miss.
- **Step 5 (invalidation fixtures)** — magic / format-version /
  kaikai-version / source-sha mismatches. Each fixture creates a
  corrupted cache file, invokes bin/kai (or kaic2 directly with
  `--prelude-cache`), asserts the expected fail-loud behaviour.
- **Suggested addition to #585 acceptance** — extend
  `cache_roundtrip_self_test` to include at least one ECall body,
  one EMatch arm, one EIf, and one EBlock with statements. This
  costs ~30 LOC of fixture but would have caught the bug in #584.
- **Tooling improvement** — stage1 `emit_match_expr` should print
  `_scr->tag` in the non-exhaustive panic message for variant
  scrutinees. The runtime tag is already on the value; the codegen
  cost is one `fprintf` per match site. Worth maybe 20 LOC across
  stage1, would have saved 30 minutes of bisection on this lane.

## What this lane did NOT do

- Touch `CHANGELOG.md` or `VERSION` (per CLAUDE.md).
- Open a PR. The branch is being pushed for review of the retro
  and #585 acknowledgement, not for merge.
- Run `make tier1` (no code changes to gate).
- Modify anything in `stdlib/` or `bin/kai`.
- Update `docs/stdlib-roadmap.md` or `docs/roadmap.md` — no entries
  in those files reflect Step 3 status either way; #452 stays at
  the same "in progress" state it had post-#584.

## Limitations of this report

- The hypotheses in #585 about Perceus RC ordering are unverified.
  A real fix needs someone to either bisect stage1 codegen with a
  smaller program (not compiler.kai) or instrument
  `cache_expr_list_loop`'s recursive frame to capture the closure's
  payload at the point of substitution.
- No estimate for #585 fix size. The repro is one line of patch;
  the fix could be anywhere from "rename a variable" to "rewrite
  the recursive cons-walker as an explicit accumulator passed by
  value". Until someone digs in, scope is unbounded.
