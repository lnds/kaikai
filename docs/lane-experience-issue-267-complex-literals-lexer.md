# Lane experience: issue-267-complex-literals-lexer

Phase 1 of #267 — lexer + parser piece for complex literals
(`3i`, `2.5i`, `1e10i`). Phase 2 (heterogeneous `Real + Complex`
arithmetic) stays open in #267, blocked by #180.

## Objective metrics

- Start: 2026-05-05T03:51:57-04:00
- End: 2026-05-05T04:08:59-04:00
- Wall: ~17 minutes (well under the 3 h calibrated budget).
- LOC delta in `stage2/compiler.kai`: ~30 added, 2 changed.
- LOC delta in `stage2/Makefile`: 5 added.
- Fixtures: 3 new (`complex_literal_basic`,
  `complex_literal_with_complex`, `complex_literal_ident_i`).
- Docs: §8 added to `docs/syntax-sugars.md`.

## Diagnosis

- Numeric literal lex path is `lex_number` →
  `lex_number_decimal` (stage2/compiler.kai). Two emit points: the
  exponent branch and the int/real fallthrough. Hex and binary
  branches sit in `lex_hex_or_bin_int` and intentionally short-
  circuit before reaching the decimal path — Phase 1 chose to
  *not* attach the `i` suffix to hex/bin literals (the spec
  doesn't ask for it and the use case is implausible).
- Parser primary handler is `parse_primary` at line 2578.
  Existing arms for `TkInt`/`TkReal` decode the span and build
  `EInt`/`EReal`; the new `TkComplex` arm reuses `decode_real`
  on the span minus the trailing `i`.
- `try_lit_unit_annotation` (m12.5 unit literals) is *not*
  reached for complex literals — they are conceptually
  dimensionless and the `<` annotation only makes sense on a
  bare scalar.

## Algorithm

Lexer:

1. Run the existing decimal scan to its natural end (digits,
   optional `.frac`, optional `e+exp`).
2. After the scan, peek one char. If it is `i`, consume it and
   emit `TkComplex` whose span covers `digits + i`. Otherwise
   emit the original `TkInt`/`TkReal`.

Helper `lex_maybe_complex_suffix` factored the post-scan check
so both emit points (exponent branch and int/real fallthrough)
share the same logic.

Parser:

1. On `TkComplex`, slice the span to drop the trailing `i`.
2. Decode the prefix as `Real` via the existing `decode_real`.
3. Build
   `ECall(EField(EVar("complex"), "mk"), [EReal(0.0), EReal(im)])`.
4. The resolver later rewrites the qualified `complex.mk` call
   to an `EModCall` once the `complex` module is bound.

## The `i`-as-identifier compat check

The contiguity rule preserves `i` as a free identifier:

- `for i in ...` — whitespace before/after `i`, lex as TkIdent.
- `let i = 5` — whitespace, lex as TkIdent.
- `3 + i` — whitespace breaks the suffix, lex `3 + i` as
  `TkInt Plus TkIdent("i")`.
- `3+i` — `+` terminates the digit scan before the `i`, then
  lex `i` as a fresh TkIdent. Output 8 (verified).

Only `3i` (no separator) attaches the suffix. Verified with the
`complex_literal_ident_i` fixture and an ad-hoc REPL run.

## Empirical verification

```kai
fn main() : Unit / Console = {
  let z = 3i
  print(real_to_string(z.re))   # 0
  print(real_to_string(z.im))   # 3
  let w = complex.from_real(2.0) + 3.0i
  print(real_to_string(w.re))   # 2
  print(real_to_string(w.im))   # 3
}
```

Output: `0`, `3`, `2`, `3`. Exact match.

Additional spot-checks:

- `1e10i` → `re=0`, `im=1e+10` (exponent path).
- `2.5i` → `re=0`, `im=2.5` (fractional path).
- `3+i` (no space) → `8` (i is identifier).

## Friction points

- **Sugars test harness has no preludes.** `make test-sugars`
  invokes `kaic2 <file>` directly with no `--prelude` chain, so
  the desugared `complex.mk` call cannot resolve under the test
  gate even though it works through `bin/kai`. Fixed with a
  per-fixture case that adds the math + protocols preludes for
  `complex_literal_*.kai`. Mirrors how the stdlib test target
  already gates per-fixture.
- **Selfhost target** lives in `stage2/Makefile` but
  `selfhost-llvm` does not have a top-level alias. Ran via
  `make -C stage2 selfhost-llvm`.
- Selfhost convergence: byte-identical on iteration 1 for both
  C and LLVM backends. Lexer changes did not perturb the
  bootstrap.

## Subjective summary

A textbook lexer extension. The peek-and-consume pattern fits
the existing scanner shape; the parser arm is six lines. Most
of the time went into the harness adjustment (sugars Makefile
gate) and the docs note. Phase 2 is independent and waits on
#180 — no entanglement.

## Limitations

- **Phase 2 of #267 still blocked by #180.** Heterogeneous
  arithmetic (`2.0 + 3.0i`, `3.0i + 2.0`) requires
  `protocol P[A]` for `Real + Complex` dispatch. Documented in
  `docs/syntax-sugars.md` §8 and in the issue.
- **No `i` suffix on hex/binary literals.** Implausible use
  case; not worth the extra branch.
- **No underscore separator interplay tested.** `3_000i` should
  work because `lex_scan_digits` already consumes `_`, but no
  fixture exercises it. Trivial follow-up if anyone asks.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-05T03:55:55-04:00	tier0	OK	50
2026-05-05T04:00:00-04:00	tier1	OK	92          (initial fail before Makefile gate fix; rerun below)
2026-05-05T04:06:19-04:00	tier1	OK	301
2026-05-05T04:07:17-04:00	tier1-asan	OK	50
2026-05-05T04:07:58-04:00	selfhost	OK	29
2026-05-05T04:08:46-04:00	selfhost-llvm	OK	38
```
