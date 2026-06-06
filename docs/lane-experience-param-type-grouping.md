# Lane experience report — parameter type grouping

Go/Pony-style parameter type grouping plus a one-line bug fix in the
parenthesised-lambda parser. Parser-only; the typer and inference were not
touched. Purely additive — the fully-annotated form stays valid, no edition
bump.

## Scope as planned vs scope as shipped

Planned: `fn foo(a, b: Int, c: Real)` ≡ `fn foo(a: Int, b: Int, c: Real)`
(forward inheritance — an unannotated name takes the type written after the
last name of its group). Three touch points:

1. `parse_fn_params` — the shared parser behind `fn`, `axiom`, `effect` ops
   and `protocol` ops (4 call sites, all fixed by the one rewrite).
2. `try_parse_lambda_params_loop` — parenthesised lambda `(a, b: Int) => …`,
   with the n-tuple-sugar fallback preserved.
3. The bug fix at the old parse.kai:2461 — the lambda parser parsed the
   annotated type and then threw it away (`Param { ptype: None }`).

Shipped: all three, exactly as planned. The error case (a trailing group
with no `:`) is rejected with `parameter group has no type annotation`. Block
lambdas (`{ a -> … }`) were left untouched — they have no `:` syntax, so
grouping does not apply there.

## Design decisions and alternatives considered

**Accumulate-then-back-fill, not lookahead-the-type.** Each parser keeps a
`pending: [PendName]` of names seen since the last group boundary and closes
the group when `:` arrives (parse the type, back-fill `Some(ty)` onto every
pending name) or — in the lambda path only — when `)` arrives with no `:`
(close them `None`). This is single-token lookahead at each step (`,` keeps
the group open, `:` / `)` close it), so it stays LL(1) with no backtracking,
matching Tier 1's parser discipline. The alternative — peeking ahead to the
group's type before emitting any Param — would have needed unbounded
lookahead across the comma run.

**`PendName` carries line/col, not just the name.** Diagnostics and the
typer both want per-parameter source positions. Threading `{ nm, nline, ncol }`
keeps each emitted Param pointing at its own name token rather than at the
shared type.

**Two close helpers, deliberately.** `close_param_group` emits `Some(ty)`;
`close_param_group_untyped` emits `None`. The split is the load-bearing
invariant: the `fn`/effect/protocol/axiom path *never* calls the untyped
closer (an unannotated trailing group is an error there, because public
signatures must be annotated), so every Param it emits is `Some(ty)`. Only
the lambda path uses the untyped closer, and only for names the user left
unannotated — which is exactly the pre-existing, typer-tolerated behaviour
(`param_ty_or_fresh` invents a fresh tyvar on `None`). So the bug fix and the
grouping feature share the same `Some`-vs-`None` discipline.

**Ordering arithmetic.** `acc` is built by cons and reversed once at the end,
so its top must be the last-read parameter. A group's pending list is also
built by cons (reverse reading order); back-filling requires iterating it in
reading order, hence `close_param_group(list_reverse(pending), …)`. The two
reversals are not redundant — dropping either flips a group's parameters.
Verified empirically with a two-group `fn run(x, y: Int, z: Real)` and a
`protocol` op `mix(self: Self, a, b: Int)` (a one-name group followed by a
two-name group).

## Structural surprises the brief did not anticipate

**The lambda fallback hinges on `=>`, not on the absence of `:`.** The brief
framed the tuple fallback as "`(a, b)` with no `:` stays a tuple", which is
true, but the real discriminant in the original parser was the `=>` after
`)`: `(a, b) => …` was always a 2-arg untyped lambda, `(a, b)` without `=>`
fell through to tuple sugar. A first cut that failed the scan on any `)` with
pending-and-no-`:` silently broke `(x) => x` and `(a, b) => …`. The fix:
close the pending group *unannotated* on `)`, then require `=>` — fail (→
tuple) only if it is absent. Caught before commit by a smoke test of single /
multi / mixed unannotated lambda forms.

**Error-message line in the negative golden tracks the fixture, not the
brief.** The brief's `.err.expected` cited `param_group_no_type.kai:1`
assuming the `fn` sat on line 1; the fixture has a leading comment, so the
`fn` is on line 4 and the golden reads `:4`. The sugars harness greps each
`.err.expected` line as a substring of stderr, so the parser's
`--> <file>:<line>:<col>` caret satisfies both the message line and the
`file:line` line.

## Fixtures added and coverage

Under `examples/sugars/` (n-tuple-sugar precedent), wired into the existing
`test-sugars` tier-1 target (which auto-globs `*.kai`):

- `param_group_basic.kai` (+ `.out.expected` → `15`): a grouped `fn add3(a, b, c: Int)`
  plus a grouped parenthesised lambda `(x, y: Int) => x + y`.
- `param_group_no_type.kai` (+ `.err.expected`): trailing group with no
  annotation, rejected with `parameter group has no type annotation` and the
  `file:line` caret.
- `lambda_typed_param.kai` (+ `.out.expected` → `42`): regression for the bug
  fix — an identity lambda `(x: Int) => x` whose annotation is the only
  source of `x`'s type (the body returns `x`, contributing nothing).

Coverage gap: the effect-op / protocol-op / axiom call sites are exercised
manually (smoke tests during the lane) but not pinned by a dedicated fixture —
the four call sites share one code path, and `test-sugars` plus a green
selfhost (the compiler's own fully-annotated signatures re-parse identically)
cover the regression surface. A combined effect/protocol grouping fixture
would be cheap to add if a future lane touches `parse_fn_params`.

## Real cost

Single session. The non-obvious work was the ordering arithmetic (two
reversals) and the `=>`-discriminant fallback in the lambda path; the rest
was mechanical. `kai info`/AST checks confirmed `Param`'s slot order and the
parser diagnostic format before writing.

## Verification

- `make tier0` green: selfhost byte-identical (`kaic2b.c == kaic2c.c`),
  demos baseline 35/35, arena gate passes.
- `make test-sugars` green (143 OK), including all three new fixtures and the
  unchanged n-tuple tuple/lambda regressions.
- Manual end-to-end: grouped `fn`, grouped effect op, grouped protocol op,
  grouped axiom, and single/multi/mixed unannotated lambdas all parse, type,
  compile and run.

## Follow-ups left for next lanes

- None required. If `parse_fn_params` is touched again, consider adding a
  dedicated effect-op/protocol-op grouping fixture to lock the shared path.
