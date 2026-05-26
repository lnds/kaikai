# Lane experience — LLVM↔C parity Cluster A (`\xNN` hex escapes)

Issue: #618 (closed). Refs #622 (cluster umbrella).
Branch: `llvm-parity-A` (forked from main after PR #704 cluster-D merge).
Plan: `docs/llvm-parity-plan-2026-05-26.md` Cluster A.

## Scope as planned vs as shipped

**Planned.** Add `\xNN` decoding to the LLVM string-literal emitter so it
agrees with the C backend (which emits the literal span verbatim and lets
the C compiler decode `\x41`). Close #618's two JSON-surrogate fixtures plus
`json_basic` and `r9_clause_capture`, remove their four skip lines, expect
the parity pass-count to rise by 4.

**Shipped.** The `\xNN` decode landed exactly as planned and is a genuine
**single** root cause — the plan's own earlier "two sub-bugs (ASCII vs
UTF-8)" guess (attributed to asu) was already retracted in the refined plan,
and live work confirms the retraction: one patch to `llvm_encode_body_loop`
fixes ASCII (`"\x41"`→`AB`) and the JSON byte-table path
(`"\x01..\xff"` in `json_byte_table()`) in one stroke. Closed 3 of the 4
listed fixtures. **`r9_clause_capture` was misclassified by the plan** — see
below — so its skip line stays, and the pass-count delta is **+4** counting
the new regression fixture (`hex_escape_literal`) and the three resolved
ones, not +4 of the originally-listed set.

## Root cause (confirmed single, not two)

`stage2/compiler/emit_llvm.kai`'s `llvm_encode_body_loop` walks a string
literal byte-by-byte and, on `\`, called `decode_kai_escape(c2)` then
advanced the cursor by a fixed **2**. `decode_kai_escape` had cases for
`\n \t \r \0` and a `char_to_int` fallback for `\" \' \\` — but **no `\x`
case**. So `"\x41"` was read as escape `\x` → the byte for `x` (0x78),
leaving `41` as two literal bytes `4`,`1`. Result: `x41` instead of `A`,
length 3 instead of 1.

The C backend never had the bug: `emit_string_lit_body` (`emit_c.kai:941`)
returns the raw span unchanged and the C compiler decodes `\x41` itself.
That asymmetry — "emit inline vs forward the literal to the downstream
toolchain" — is exactly the divergence mechanism eric flagged as the
structural follow-up. The two emitters disagreed because only one of them
decodes escapes; the other delegates.

## The loop-advance subtlety (the real trap)

The brief warned about this and it was the whole difficulty: `decode_kai_escape`
returns a single `Int` and the **caller** advances by a hard-coded 2. A
`\xNN` escape consumes 4 source chars (`\`, `x`, hi, lo), so the fix could
**not** live inside `decode_kai_escape` — it had to branch in the loop
itself. Implemented as: on `\x`, call a new `llvm_decode_hex_byte(src, i+2, n)`
returning `Option[Int]`, emit the byte via the existing `llvm_byte_literal`,
and recurse with `i + 4`. Malformed `\x` (fewer than two hex digits, which
the front-end already rejects) falls back to the single-char path so we
never silently drop bytes.

Module boundary note: `parse.kai`'s `decode_hex_loop` is not `pub` and its
`}`-terminator behaviour is char-literal-specific (`\u{HHHH}`), so reusing
it would have meant widening the public surface for a subtly different
shape. Cheaper and clearer to add a self-contained `llvm_hex_digit_value` +
`llvm_decode_hex_byte` pair local to `emit_llvm.kai`, mirroring the lexer's
`ch_is_hex_digit` acceptance.

## `\u` did NOT need handling

The lexer's `lex_string_body` only *skips* escapes (it doesn't decode), and
the parser stores `EStr` as the raw span (`parse.kai:293`) — decoding is the
emitter's job. Char literals decode `\u{HHHH}` (`decode_char`), but **string
literals in the fixtures use only `\x`**, and `emit_string_lit_body`'s own
comment says it doesn't handle `\u{HHHH}` in strings either. So C and LLVM
agree on the absence of string `\u`, and no `\u` work was needed. The
fixtures need only `\x \n \t \r \0 \" \' \\`, all now covered.

## `r9_clause_capture` — plan misclassification (handoff to Lane B)

The plan listed `r9_clause_capture` under Cluster A ("char `P` lost → prints
`[[` not `[P`"). **This is wrong.** The fixture contains no `\x` escape at
all — its strings are `"P"`, `"svc"`, `"info"`. Verified on the pre-fix main
checkout: it fails identically with `[[]` + `kai: continuation resumed twice
(handler #1)`, exit 1. My `\x` fix does not touch it. The real divergence is
in the **effect-handler clause-capture / continuation path** under LLVM
(`prefix` capture lost, continuation resumed twice) — that is Cluster B
territory (default-handler / `kai_main` install), not literal escapes.

Decision (taken in-lane, no escalation): **I did not remove its skip line.**
Removing a skip for a still-diverging fixture would turn the parity gate red
on a bug this lane doesn't fix. Its skip stays at
`tools/backend-parity-skips.txt` with the existing `:622:` annotation; Lane B
should pick it up. This is the honest move and matches the "don't close
tickets you didn't fix" discipline.

## Fixtures

- **New:** `examples/stdlib/hex_escape_literal.kai` (+ `.out.expected`) —
  exercises `\x41\x42`→`AB`, mixed `\x09`/`\x0A` with text, `\x00\x01\xff`
  (a `\x00`-leading string reports length 0 on both backends — NUL
  terminates; the C oracle agrees), and a no-escape baseline. Auto-discovered
  by `tools/test-backend-parity.sh` (it globs `examples/stdlib/*.kai`) and by
  the golden-diff harness via its `.out.expected`.
- **Unskipped (now byte-identical C↔LLVM):** `json_surrogate_decode`,
  `json_surrogate_encode` (#618), `json_basic` (#622).
- **Left skipped (misclassified):** `r9_clause_capture` — see above.

## Cost vs estimate

Estimate: "~one patch, advance by 4 not 2." Actual: matched the estimate for
the emitter change (one branch + two small helpers, ~40 LOC). The unbudgeted
cost was triage on `r9_clause_capture` to prove it's not a `\x` bug before
declining to unskip it — ~15 min reading the fixture and reproducing on the
main checkout. Net well under the half-day a parity lane usually takes;
cheaper because the refined plan had already nailed the root cause to a
one-line repro.

## Notes for Lanes B / C

- **Lane B** inherits `r9_clause_capture` (clause-capture / continuation,
  not escapes). The `[[]`-not-`[P]` symptom is the `prefix` capture being
  lost, and "continuation resumed twice" suggests the LLVM handler installs
  the wrong continuation or double-resumes — squarely the `kai_main`
  default-handler family.
- The structural follow-up eric proposed (symbol-coverage script:
  `kai_*` in runtime.h must each have a `kaix_*` mirror) would NOT have
  caught Cluster A — this was an *emitter* divergence (inline decode vs
  forward-the-literal), not a runtime-shim drift. The right structural net
  for A-class bugs is eric's third bullet: audit every "emit inline instead
  of calling a `@kaix_` wrapper" decision in `emit_llvm.kai`. This was one
  such site.
