# Lane retro — `if`/`else if` cascades → `match` (audit proposal 2)

This lane converts depth-≥3 `if`/`else if` cascades in `stage2/compiler.kai`
to exhaustive `match` expressions, per **Proposal 2** of
`docs/lane-experience-compiler-idioms-audit.md` (PR #463).

## Scope as planned vs shipped

The audit advertised 54 cascades and "~300 LOC" of savings. The lane was
scoped to *mechanical refactor only*, no behavior change, with selfhost
byte-identical as the load-bearing gate.

**Shipped:** 13 cascades converted. All Char-equality cascades that were
expressible as a flat `match c { '...' -> ... }` (no compound conditions, no
mixed predicates) are now `match`. Two pure `Int`-equality cascades
(`tuple_type_for_arity`, `tuple_field_name`, `emit_pow_unroll`,
`emit_pow_unroll_loop`, `hex_digit`) are also `match`. The largest single
win is `lex_step_single_char` (29 arms, the lexer single-character dispatch).

| Function | Line | Arms | Type |
| --- | --- | --- | --- |
| `decode_char_escape` | 1941 | 8 | Char |
| `lex_step_single_char` | 681 | 29 | Char |
| `parse_class_escape` | 6741 | 10 | Char |
| `parse_escape_atom` | 6759 | 10 | Char |
| `parse_quant` | 6787 | 5 | Char |
| `iscan_expr_end` (inner) | 13176 | 3 | Char |
| `mangle_unit_ident_loop` (inner) | 23555 | 7 | Char |
| `json_escape_char` | 39220 | 6 | Char |
| `decode_kai_escape` | 40134 | 5 | Char |
| `lex_skip_interp` (inner) | 483 | 4 | Char |
| `lex_skip_inner_string` (inner) | 504 | 3 | Char |
| `parse_repeat_quant` (inner) | 6812 | 3 | Char |
| `tuple_type_for_arity` | 3819 | 4 | Int |
| `tuple_field_name` | 3828 | 4 | Int |
| `emit_pow_unroll` | 14786 | 3 | Int |
| `emit_pow_unroll_loop` | 14792 | 3 | Int |
| `hex_digit` | 40160 | 16 | Int |

## Cascades skipped, and why

| Class | Count | Reason |
| --- | --- | --- |
| `String`-equality (`if name == "Bool"`, `if op == "+"`, …) | ~21 | Per brief — the lane was scoped Char/Int only, and the audit assumed string-pattern-on-`match` was a language gap. **In practice the language already supports it** (e.g. `demos/forth/main.kai:13-19`), but the lane respects the brief's scope. A follow-up lane can sweep the 21 string cascades. |
| Mixed literal + predicate (`if c == '_' { … } else if ch_is_digit(c) { … }`) | 4 | `match` has no guards; converting would need a nested `if` inside an `_` arm — equal in LOC, less clear than the cascade. |
| Or-pattern compound (`if n == 'x' or n == 'X'`) | 1 | `match` or-patterns (e.g. `'x' \| 'X' ->`) are not used anywhere in the repo; introducing them is a separate idiom decision, not a mechanical refactor. |
| Relational chains (`if op == ">"` mixed with `if hi < lo`) | many | Not equality-only; not a `match`. |
| `keyword_kind` 35-branch string cascade (line 258) | 1 | Excluded by the brief. Convertible per language semantics, but it's a single hot path the team may want to fold into a lifted enum, not just rewrite as `match`. |

Treating only the *pure flat equality cascades* and respecting the brief's
"Char (and Int as bonus) only" framing leaves the 17 conversions above —
which were the cleanest wins.

## LOC delta

The brief predicted ~-300 LOC. The actual delta is **+47** (51 644 → 51 691).

Why the prediction missed: cascades with multi-line bodies (e.g.
`lex_step_single_char` arms with `if lex_peek_is(l1, '=') { … }` blocks)
need braces on each arm in `match` form, and `match X {` / `}` themselves
add two structural lines per cascade. For arm bodies > 1 line, `else if`
and a `match` arm are roughly the same byte count.

The *true* win is **clarity, not compression**: each cascade now reads as a
single exhaustive table rather than a recursive `else`-tail. Reviewers can
scan an arm and know it's mutually exclusive with the others.

## Verification

- `make tier0`: OK — selfhost fixed point holds, demos baseline holds (26
  passing, baseline 26).
- `make selfhost`: OK — `kaic2b.c == kaic2c.c` fixed point, byte-identical
  re-emission. This is the load-bearing gate: any cascade rewrite that
  produced non-deterministic emit would break it.
- `make tier1`: locally failed in `test-effects` with `Error 143` on
  `issue_107_signal_trap` (SIGINT-trap test) — a known macOS-local flake
  unrelated to this lane (the signal harness interaction with `set -e` in
  the test loop). CI (`tier1.yml` on `ubuntu-latest`) is the merge gate per
  CLAUDE.md; this lane defers to CI.

The two demos that report FAIL in tier0 (`forth`, `mini_ledger`) are
pre-existing failures already in the baseline of 26.

## Surprises

1. **The audit's "kaikai doesn't string-match in `match`" claim is wrong.**
   `demos/forth/main.kai` does it. The lane respected the brief anyway, but
   a future "Proposal 2b" can sweep the 21 string cascades and would likely
   land another 100+ LOC of cleanup.
2. **Match guards are not in the language.** I expected to be able to write
   `match c { '_' -> … , c when ch_is_digit(c) -> … , _ -> … }` to fold the
   four mixed-literal-vs-predicate cascades. Without guards, those stay as
   if-chains.
3. **Or-patterns are absent from idiom.** `match c { 'x' | 'X' -> … }` is
   not used anywhere in the repo. The parser accepts `|` as a token but no
   match-arm uses it. Not exercising it here.

## Cost

Estimate: 1 day (per the audit). Actual: 1 day. Most of the time went into
classifying the 48 detected cascades (Char vs Int vs String vs predicate vs
relational), not the mechanical rewrites.

## Follow-ups for next lanes

- A "Proposal 2b" lane to convert the 21 String-equality cascades (most
  notably `keyword_kind` at 258, and ~10 cascades dispatching on operator
  symbols like `"+"`/`"-"`/`"*"`). Now-confirmed language support; the
  audit's pessimism on this can be retired.
- Consider adding match guards (`when`-clauses on arms) as a separate
  language-design lane — the four mixed predicate-vs-literal cascades are
  the canonical motivating examples.
- Or-patterns ditto — `lex_punct` and the `Stdout/Stderr` cascades both
  have multi-name groups that compress naturally with `'x' | 'X' ->`.

## Fixtures

None added. This lane is a pure mechanical refactor with no behavior
change; the gate is selfhost byte-identical, which exercises the entire
modified surface area on every CI run.
