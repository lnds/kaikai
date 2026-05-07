# Lane: issue-311-triple-quote-strings

## Objective metrics

- Start: 2026-05-06T23:22:31-04:00
- End: 2026-05-06T23:40:15-04:00
- Wall clock: ~18 minutes
- Diff: `stage2/compiler.kai` +48/-3, two new fixtures (4 files).

## Diagnosis

The kaikai lexer (`lex_string_body`) accepts both regular `"..."` and
triple-quote `"""..."""` literals, threading an `is_triple` flag into
the token. The parser stores the raw source span (including the
quotes) in `EStr(span, is_triple)`.

The C emitter at `emit_string_expr` (stage2/compiler.kai:11869) had a
single fast path that wrapped the raw span verbatim in `kai_str(...)`:

```kai
if not has_interp(span, body_start, body_end) {
  cat3("kai_str(", span, ")")
}
```

For a regular string this works because the lexer guarantees the body
contains no literal newlines and no unescaped `"`, so the raw span is
already a valid C string literal.

For a triple-quote string the body may carry literal newlines, tabs,
and unescaped `"` characters. Emitting the raw span produced

```c
kai_mensaje = kai_str("""
    First line
    """);
```

which the C preprocessor reads as an open string with embedded
newlines — exactly the failure mode in the bug report.

The `IpLit` raw production used by the interpolation path (lines 11795
and 11833) had the same shape: `cat3("\"", piece, "\"")` directly
wrapped raw body chars.

## Fix

Added one helper, `escape_str_body_for_c`, that walks the body and
emits a C-escape for each problem byte:

- literal `\n` → `\n`
- literal `\r` → `\r`
- literal `\t` → `\t`
- literal `"` → `\"`
- a backslash + next char passes through verbatim (matching the
  lexer's source-level escape pass-through in `lex_string_body`)
- everything else is copied as-is.

For a regular string the body has no escape-target bytes, so the
helper is identity and behaviour is unchanged. For a triple-quote
string every problem byte is normalised to a valid C escape.

Three call sites:

- The non-interp fast path (`emit_string_expr`): when `is_triple`,
  slice out the body and route through `escape_str_body_for_c`.
- The interp split path: a new `raw_lit_for_c(piece)` helper wraps
  literal pieces in `"..."` after escaping. Called from both
  `iscan_collect_rev` (mid-string lit) and `finish_iscan_rev` (trailing
  lit). For regular strings, escape is identity; for triple-quote with
  interpolation, the lit pieces between `#{...}` get escaped.

## Empirical verification

Two new fixtures under `examples/literals/`:

- `triple_quote_basic.kai` — the bug-report repro: a 3-line indented
  triple-quote string. Expected output: leading newline + two indented
  lines + indented trailing line + the final `\n` from `print`. Diff
  vs golden: clean.

- `triple_quote_with_quotes.kai` — embeds `"`, `""`, and a literal tab
  byte in the body. Exercises the `\"` and `\t` escape branches.
  Output round-trips through `print` correctly.

A separate smoke test (not committed) confirms interpolation still
works inside triple-quote bodies via the `#{n}` path.

Both backends self-host byte-identical.

## Friction points

None substantive. The fix shape was one helper plus three call-site
swaps; the codebase already had the right factoring (separate
`escape_str_body_for_c` plus `raw_lit_for_c`) waiting to be
introduced.

One sharp edge worth noting: `pcs_rewrite_estr_span` (stage2/
compiler.kai:33334) reconstructs a kaikai-source span and feeds it
back to the parser. That path must NOT escape — it operates pre-emit
and the result is parsed again. Verified the helper is only called
from the C emit path.

## Subjective summary

A precise codegen bug with an obvious shape and a small fix. Time
budget was 1.5h calibrated; actual was ~18 minutes wall (most of
which was tier validation). The hardest decision was where to put
the escape — folding it into both the fast path and the interp path
was cheaper than carrying `is_triple` through the IPart shape.

## Limitations

- Interpolation behaviour inside triple-quote: unchanged in shape,
  newly correct in C output. The split-at-`#{` logic preserves
  literal pieces verbatim and routes them through the same escape.
- Source-level escape semantics: `\n` (backslash + n) in source
  passes through to the C string as `\n` and prints as a newline.
  Same as a regular string.
- This lane does not introduce dedent / trim semantics. The body is
  emitted verbatim (modulo C escape).

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-06T23:35:42-04:00	tier0	OK	-
2026-05-06T23:35:42-04:00	tier1	OK	293
2026-05-06T23:35:42-04:00	tier1-asan	OK	50
2026-05-06T23:40:08-04:00	selfhost	OK	112
2026-05-06T23:40:08-04:00	selfhost-llvm	OK	124
```
