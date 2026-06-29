# Lane experience — issue #975 (block-vs-record ambiguity in `if`/`match` headers)

## Scope as planned vs. as shipped

**Planned:** in the condition of an `if` (and the scrutinee of a
`match`), an expression ending in an `UpperIdent` immediately followed
by `{` was misparsed as a positional record literal (`Name { ... }`, the
#266 feature), swallowing the `{` that should open the block body. Fix
the parser so the `{` opens the body, à la Rust's "no struct literal in
header position".

**Shipped:** exactly that, by replicating the parser's existing
`allow_trailing_lambda` discipline for record literals. Added a second
`Parser` flag `allow_record_lit`; `parse_if`/`parse_match` disable it
while parsing the cond/scrutinee and restore it before the body; every
delimited sub-context re-enables it for its interior. No new diagnostic,
no grammar change, no behavioural change outside the cond/scrutinee
position.

## Design decision — mirror `allow_trailing_lambda`, restore inside delimiters

The parser already solves the *twin* ambiguity (a trailing `{` lambda
eating the `if` body) with a `Parser.allow_trailing_lambda` flag toggled
off around the cond and back on before the body. The record-literal
hazard is structurally identical, so the fix is the same shape:

- `allow_record_lit: Bool` on `Parser`, default `true`, preserved in
  every Parser-copy constructor (`parser_new`, `p_mark_err`,
  `p_advance`, `p_with_trailing_lambda`).
- Two new helpers: `p_with_record_lit(p, allow)` (mirrors
  `p_with_trailing_lambda`) and `restore_rl(prev, r)` which reinstates a
  caller's flag on the result of a delimited sub-parse.
- The two record-literal decision points in `parse_ident_primary` (the
  bare `Name {` and the qualified `mod.Type {`) gain `and
  p.allow_record_lit`; when the flag is off they fall through to the
  plain-variable branch, leaving the `{` for the body.

The one subtlety the trailing-lambda code does *not* have to deal with,
but record literals do: a record literal is **legal and unambiguous
inside any delimiter**, and that already works today (`if g(Cfg { v: 1 })
{ ... }` compiles). Suppressing the flag for the whole cond subtree would
regress that. So every delimited context re-enables the flag for its
interior and restores the caller's value on the way out:

- call args (`parse_postfix_rest` `TkLparen`),
- index brackets (`parse_postfix_rest` `TkLbracket`),
- `()` / lambda / tuple, `[]` list/range, and `{}` block primaries
  (the `parse_primary` dispatch, via `restore_rl`).

**Why restore matters (the trap):** the flag rides on the immutable
`Parser` value threaded left-to-right. If a delimiter re-enabled the flag
and let it leak forward, the *next* operand in the same cond chain would
see it true again — e.g. `if f(x) == Forest { body }` would re-misparse
`Forest {` after the call returned. So each delimiter site saves the
incoming flag and reinstates it on the returned parser before the cond
chain continues. The cond operand chain (the binary-operator ladder)
keeps the flag false throughout; only delimiter interiors flip it true.

## Structural surprises

- **Record-literal field values need no special handling.**
  `parse_record_lit` is reachable *only* through the two gated decision
  points, which fire only when the flag is already true. So its field
  values inherit `true` and there is nothing to re-enable inside the
  record-literal loops — the brief's "re-enable in field values" step is
  redundant given the gate, and was deliberately omitted to keep the diff
  tight.
- **Match guards (`pat if g -> body`) and case `when` guards must NOT be
  touched.** They disable `allow_trailing_lambda`, so they looked like
  cond positions, but a guard is followed by `->`, never a `{` block —
  there is no block-vs-record ambiguity, and a record literal in a guard
  is legal and intended. Left them at the default (true).
- **`while`/`until` have no parser bug.** They are not keywords; they
  live in the `loop` stdlib package and are spelled as trailing-lambda
  calls `while { cond } { body }`, so they never reach a bare
  `UpperIdent {` in header position. The issue speculated about them;
  that speculation was wrong.
- **`handle` is unaffected.** Its body starts with `{` (parsed via
  `parse_block` directly) and its `with Eff(init)` initialiser is
  delimited by `()`, so no expr-before-brace ambiguity exists.

## Fixtures added (all in `examples/sugars/`, picked up by `test-sugars`)

- `if_cond_variant_block.kai` (+`.out.expected`) — the issue's repro:
  `if id == Forest { ... }`. POSITIVE, was broken.
- `match_scrut_variant_block.kai` (+`.out.expected`) — `match x == A {
  true -> ... false -> ... }`. POSITIVE, was broken the same way.
- `if_cond_record_lit_delimited.kai` (+`.out.expected`) —
  NO-REGRESSION: record literals inside call args, `()`, a `[]` list,
  and a `{}` block, all within an `if` cond. Guards every re-enable site.
- `if_cond_int_rhs.kai` (+`.out.expected`) — NO-REGRESSION: `if x == 0 {
  ... }` (Int RHS, never ambiguous).

Two fixture authoring traps hit along the way: `check` is a reserved
keyword (`TkCheck`), so a helper named `check` fails to parse; and
indexing a list *literal* (`[x][0]`) routes through `array_get` which
wants `Array[T]`, not `[T]` — the list re-enable site is instead
exercised via list `==` with `#[derive(Eq)]`.

## Decisions deferred / out of scope

- **No targeted diagnostic.** The issue floated an optional "record
  literal not allowed in `if` condition; wrap it in parentheses"
  message. We chose the silent Rust-style fall-through instead: a
  genuine top-level record literal in a header is simply spelled with
  parens (`if (Rec { ... }.f) { ... }`, covered by the paren case in the
  delimited fixture). A targeted diagnostic would need to detect the
  suppressed-record shape and risks false positives; it earns its own
  issue if the generic parse error proves confusing in practice.
- **No qualified-variant multi-module fixture.** The qualified path
  (`if id == mod.Forest { ... }`) shares the same `allow_record_lit`
  gate as the bare path, and `test-sugars` runs single files. The bare
  case plus the gate cover it; a multi-module regression fixture was not
  added.

## Cost vs. estimate

Close to estimate. The fix is mechanical once the `allow_trailing_lambda`
precedent is understood; the only real thinking was the restore-on-return
discipline at delimiters and confirming the set of sites that must
re-enable. Most wall-clock went to the bootstrap rebuild loop
(`make kaic2`) and to two fixture authoring potholes (`check` keyword,
list-literal indexing).

## Follow-ups for next lanes

- If the generic parse error from a suppressed top-level record literal
  in a header turns out to confuse users, add the targeted diagnostic
  (open a fresh issue).
- The same block-vs-record discipline would apply if any future prefix
  construct takes `Expr` then a `{` block; reuse `allow_record_lit`
  rather than inventing a new flag.
