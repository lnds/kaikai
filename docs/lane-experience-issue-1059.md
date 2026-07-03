# Lane experience — issue #1059: BigInt / Int literal overflow

## Scope

Two front-end correctness bugs, one root cause (the parser decoded every
integer literal to an i64 first, wrapping silently past `Int` range):

- **A — BigInt literal past i64 wraps.** `<int>n` desugared to
  `bigint.from_int(decode_int(<digits>))`. `decode_int` folds to an i64
  with two's-complement wraparound, so `2^128n` reached `from_int` as
  `0`, `2^64+1n` as `1`, and `2^63n` as a *negative* BigInt. The type
  whose whole point is never to overflow was handed an already-wrapped
  value.
- **B — plain `Int` literal past i64 wraps silently.** `let x =
  9223372036854775808` produced `-9223372036854775808` with no
  diagnostic, violating no-silent-coercion.

## What shipped

- **A:** the `<int>n` desugar now routes the *digit string* through a new
  `pub fn bigint.from_literal(digits: String) : BigInt`, which folds the
  digits with exact BigInt arithmetic (`add`/`mul`/`from_int`) — no i64
  intermediate. The AST node changed from `EInt(decode_int(...))` to an
  `EStr` carrying the quoted digit span, so the existing string pipeline
  delivers the digits to `from_literal` untouched.
- **B:** the `TkInt` arm in `parse_primary` calls a new
  `int_lit_overflows(span)` check; on overflow it emits a compile-time
  `p_error` pointing at the BigInt escape hatch (`n` suffix /
  `bigint.from_string`) instead of decoding a wrapped value. In-range
  literals (including i64::MAX exactly, hex, bin, `_`-separated) are
  untouched.

## Design decisions

- **Helper lives in `bigint`, not `bigint_convert`.** `from_string`
  already exists in `bigint_convert`, but the literal desugar produces
  `EField(EVar("bigint"), <fn>)`, and the qualified-call rewrite
  (`modules.kai` `rqc_kind`) only rewrites to `EModCall` when the module
  is imported *and* exports the name. `bigint_convert` is neither in the
  prelude nor imported by the `import math.bigint` the repro uses, so
  targeting it would make every big literal require an extra import —
  and break existing code. `from_literal` therefore lives in `bigint`
  (already imported) and is implemented on `bigint`'s own primitives to
  avoid a `bigint → bigint_convert → bigint` import cycle.
- **B is a hard error, not a warning or a widen-to-BigInt.** The literal
  is `Int`-typed; silently promoting it to BigInt would change its type,
  and a warning still emits a wrong value. A compile-time error with a
  fix-pointing message is the honest no-silent-coercion answer. (Chosen
  by the lane owner; not re-opened.)
- **i64::MIN as a bare literal.** `-9223372036854775808` is `-` (unary)
  applied to the literal `9223372036854775808`, whose magnitude 2^63
  exceeds i64::MAX — so it now diagnoses. This is not a regression: that
  form already failed to compile before (it emitted `--…LL`, invalid C).
  The lane turns an unreadable C error into a clear kaikai diagnostic.

## Structural surprises

- **kaic1 mis-lexes a backtick immediately before a closing quote inside
  a string literal.** The first draft of the B diagnostic used
  ``"… `" ++ span ++ "n` …"``; kaic1 (which compiles the bundle)
  rejected the bundle with a parse error inside the string. Switching to
  interpolation (`#{span}`) and dropping the backtick-quote adjacency
  fixed it. A non-ASCII `…` in the message is also a bundle hazard — kept
  the message ASCII-only.
- **`EStr` carries the *raw* span with quotes**, not the decoded value —
  the desugar wraps the digits in `"` so the downstream string pipeline
  (quote-strip, interp, unescape) treats them as a normal literal. Digits
  never contain `#{`, `\`, or `"`, so no escaping hazard.

## Fixtures

- Positive: `examples/numeric/bigint_literal.kai` extended with the three
  repro literals (2^63, 2^64+1, 2^128) asserting exact output.
- Negative: `examples/numeric/int_overflow.err.kai` +
  `.err.expected` — a bare `Int` literal past i64 must diagnose. Wired
  into `test-numeric-bigint` (C backend); the diagnostic is
  backend-independent (parser-level), so the native gate needs no
  separate negative case.

## Cost

Small, contained: `bigint.kai` (+20 LOC, still A / cogcom 2.5 avg),
`parse.kai` (desugar swap + ~55 LOC of overflow-check helpers, each a
single-purpose tail recursion), `lex.kai`/`parse.kai` comment fixes,
`docs/info/syntax.md` prose + fence, one Makefile step. Selfhost byte-id
holds (the compiler uses no `n` literals, so the desugar swap is inert to
its own build).

## Follow-ups

- Hex/bin literal overflow past i64 is *not* diagnosed (only decimal is
  checked). A `0x`-prefixed literal beyond 2^64 still wraps. Out of scope
  here (the issue is about decimal); worth a separate lane if it bites.
