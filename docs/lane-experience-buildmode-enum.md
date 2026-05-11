# Lane experience — BuildMode enum (audit proposal 5)

**Branch:** `lane-3-buildmode`. **Base:** `main` @ `ffc7b10`. **Date:** 2026-05-10.

Mechanical refactor closing audit proposal 5 from
`docs/lane-experience-compiler-idioms-audit.md` (§ Proposal 5): collapse the
`test_mode: Bool, bench_mode: Bool, check_mode: Bool` triple in
`emit_program / emit_program_llvm / emit_main_wrapper` to a single
`BuildMode` sum type, ruling out the illegal `test_mode && bench_mode`
state by construction.

## Scope as planned vs. shipped

Planned: replace the triple across the 3 emit functions and the driver
call site; check the other ~7 sites the audit counted and fold if they
clustered around a similar concept.

Shipped: the 3 emit functions plus the driver. The audit's "10 sites
total" appears to have counted call sites (or used a looser regex over
multi-line signatures); `grep -E "^fn ...: Bool...: Bool"` finds only 5
fn signatures with 2+ Bool params in `stage2/compiler.kai`:

| Site | Bools | Verdict |
| --- | --- | --- |
| `emit_main_wrapper:16729` | test/bench/check | folded |
| `emit_program:22216` | test/bench/check | folded |
| `emit_program_llvm:43561` | test/bench/check | folded (params unused; kept for symmetry as `_bmode`) |
| `parse_axiom_decl:5682` | `is_pub`, `extern_c` | left alone — orthogonal concepts (visibility vs FFI source) |
| `scan_uses_in_interp:37678` | `is_triple`, `in_lam` | left alone — orthogonal concepts (string-literal kind vs lexical context) |

The two non-triple sites are genuinely independent flags. Folding them
into an enum would invent a category that doesn't exist; left as-is per
the "don't introduce abstractions beyond what the task requires" rule.

## Design decisions and alternatives

- **`type BuildMode = BMBuild | BMTest | BMBench | BMCheck` rather than
  `enum BuildMode { ... }`.** The brief asked for `enum`, but
  `stage2/compiler.kai` already uses `type Mode = MA | MB | ...` form
  exclusively. Using the existing style keeps the diff minimal and
  avoids drawing a stylistic distinction that doesn't exist in the rest
  of the file.
- **`BM`-prefix to disambiguate from `Mode`.** The driver already has
  `MTest / MBench / MPropCheck` — short bare `Test / Bench / Check`
  variants would shadow or collide. `BM` mirrors the `M`-prefix
  convention used by `Mode`.
- **`emit_program_llvm` keeps its `BuildMode` param even though the
  LLVM emitter never branches on it.** Symmetric signatures with the C
  emitter; renamed to `_bmode` to flag the unused-param status (matches
  the existing `_is_triple`, `_p` style at `tcrec_str_has_evar:38515`).
  The alternative (drop the param from `emit_program_llvm` entirely)
  would slightly improve LOC but split the API asymmetrically — if a
  future LLVM-emitter test/bench wrapper lands (currently absent;
  `kai --emit-llvm --test` is undefined behaviour), it would re-add
  the param. Not worth the churn.
- **Single `match bmode { ... }` in `emit_main_wrapper` instead of
  three `if match bmode { BMTest -> true _ -> false } { ... } else if
  ...`.** The single match is clearer and also collapses the previous
  trailing `else if has_main_decl(decls)` chain into a `BMBuild` arm.

## Surprises

- **`emit_program_llvm` never used the triple.** It accepted
  test_mode/bench_mode/check_mode and silently ignored them — the LLVM
  emitter has no test/bench/check wrapper paths. The C emitter is the
  only consumer of `emit_main_wrapper`. The triple was therefore dead
  weight on the LLVM signature.
- **Driver already had `Mode` with `MTest / MBench / MPropCheck`.** The
  three Bools were synthesised from `mode` at the call site (lines
  51539-51541 pre-refactor). The collapse is therefore not about
  inventing a new concept — `Mode` already encoded it — but about
  threading the narrower view (just the wrapper-shape selection)
  through the emitters instead of three Bools that re-derived from
  `Mode` upstream.
- **LOC delta is `+16`, not the audit's `-30`.** The signatures save
  ~3 lines (4 sites × ~6 column saving folded by the formatter into
  single-line signatures). The single `match` in `emit_main_wrapper`
  adds ~10 lines of arm braces vs. the original `else if` chain
  (kaikai's `match` arms with block bodies require `BMX -> { ... }`,
  3 lines of overhead per arm). Net negative on LOC, net positive on
  type safety (illegal `test && bench` is now unrepresentable). The
  audit's `-30` estimate appears to have measured signatures only and
  missed the body-shape cost; documented here so future audits can
  calibrate.

## Fixtures

None. Pure refactor — no behavioural delta possible (the typer enforces
total coverage of `BuildMode` variants, and the four wrappers are
unchanged byte-for-byte). Verification:

- `make tier0` — green, selfhost byte-identical, demos baseline holds.
- Smoke-tested all four modes:
  - `bin/kai run /tmp/build.kai` → "hello buildmode" (BMBuild).
  - `bin/kai test /tmp/test.kai` → "1/1 tests passed" (BMTest).
  - `bin/kai bench --iters 10 /tmp/bench.kai` → "1 benches" (BMBench).
  - `bin/kai check /tmp/check.kai` → "1/1 checks passed" (BMCheck).
- Local `make tier1` deferred to CI per the mechanical-lane convention
  (memory `feedback_tier1_local_optional.md`); Tier 1 stays the merge
  gate.

## Cost vs. estimate

- Estimated: 0.5 day. Actual: ~30 min of editing + verification.
  Under estimate because the change shape was straightforward and the
  triple's call sites were all in two contiguous regions of the file.
- No coordination cost with the other parallel lanes (1 Option
  combinators, 2 if-elseif → match, 6 BinSerialize). The
  buildmode edits are localised to `emit_main_wrapper`, the two
  `emit_program*` signatures, and the driver block — far from the
  Option-rewrite sites, lexer cascades, and serialisation code that
  the other lanes touch. Trivial-merge by construction.

## Follow-ups left for next lanes

- `parse_axiom_decl(is_pub, extern_c)` and
  `scan_uses_in_interp(is_triple, in_lam)` remain as 2-Bool
  signatures. Not folded — see § Scope above. If a future lane finds
  a third site that shares either concept, revisit.
- `emit_program_llvm`'s `_bmode` param is unused. If the LLVM emitter
  ever grows a test/bench/check wrapper path (currently absent — the
  test/bench/check runners only run against the C backend), drop the
  underscore.
