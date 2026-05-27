# Lane experience — issue #672: `@cap` inside string interpolation

## Symptom

```kai
fn main() : Int / Stdout = {
  var tally = 0
  tally := @tally + 1
  Stdout.print("tally=#{int_to_string(@tally)}")
  @tally
}
```

`error: cannot find \`tally\` in this scope --> file.kai:1:16` — the
span points at the `fn main` signature (line 1, col 16), not the
offending `@tally` on line 4, and offers a bogus `did you mean tail?`.
The same `@tally` *outside* the interpolation (lines 3, 5) resolves
fine. Only the read inside `"#{...}"` fails.

## Root cause — scope-loss, not wrong-span

Both halves of the brief turned out to be the *same* bug, and it was a
scope-loss, not a span bug. The misleading `1:16` span is a downstream
artefact, not a second defect.

A `var name = init` is lowered (in `desugar.kai`, `desugar_var_block`)
to one of two forms:

1. The canonical `with State[T](init) as name { ... }` handler, which
   keeps `name` in scope inside the body via the handler alias.
2. A cheaper 1-element **array-slot** specialisation
   (`let name__slot = array_make(1, init)` + rewriting `name.get()` /
   `name.set()` to `array_get` / `array_set`). This form **drops the
   `name` binder entirely** — nothing is in scope under that name
   afterwards.

The specialiser picks form 2 only when `name` is proven not to escape,
via `body_escapes(name, body)`. But `body_escapes` (and `init_is_safe`,
`contains_var`) treated a string literal as opaque: `EStr(_, _) ->
false`. At the point `desugar_var_decls` runs, the `@tally` inside the
interpolation is **still a raw source span** — it has not been parsed
into an AST. So the escape walk never saw the hidden `tally.get()`,
concluded `tally` did not escape, and chose the array-slot form. The
binder vanished.

Much later in the pipeline, `desugar_interp_decls` (in `emit_c.kai`)
parses `#{int_to_string(@tally)}` out of the span via
`parse_interp_expr`, lifting `@tally` into `tally.get()` (i.e.
`ECall(EField(EVar("tally"), "get"), [])`). The resolver
(`compiler/resolve.kai`, `chk_expr`) walks that fresh sub-AST, finds no
`tally` in scope, and reports `cannot find tally`.

The `1:16` span is the *second* consequence of the same lift:
`parse_interp_expr` builds a fresh parser over just the snippet source
(`parser_new("<interp>", src, toks)`), so the synthesised `EVar`'s
line/col are snippet-relative, not source-relative. Fixing the scope
loss removes the error entirely, so the wrong span never surfaces; the
span machinery itself was left untouched (out of scope, and moot once
resolution succeeds).

## The fix

Make the var-specialisation escape check aware of `@name` cap-reads
hidden inside interpolation spans. In `desugar.kai`:

- `body_escapes` and `contains_var` gained an `EStr(span, is_triple)`
  arm that calls a new pure helper `interp_span_reads_cap`.
- `interp_span_reads_cap` does a lexical scan of the literal body,
  finds each `#{...}` region (brace-balanced, mirroring fnreg's
  `iscan_expr_end`), and checks for a delimited `@name` token inside.
- A hit forces the canonical `State[T] as name` handler (form 1),
  which keeps `name` in scope so the later interp lift resolves.

The scan is **conservative**: a false positive only forgoes the
cheaper array-slot lowering (always-correct, just less optimised),
never affecting correctness. Token delimiting (`dsg_matches_at` +
`dsg_ident_cont`) keeps `@tally` from matching a read of `tal`, and
`@tallyx` from matching `tally`.

### Why a hand-rolled scanner instead of reusing `iscan_collect`

`iscan_collect` lives in `fnreg.kai`, which imports `infer`, which
imports `desugar` — pulling it into `desugar` would form an import
cycle. The scanner is ~40 lines of pure char-walking over `char_at` /
`string_slice` (already used throughout `desugar.kai`), so the
duplication is cheap and keeps the pre-resolve passes free of the
`Console` effect that `parse_interp_expr` would have threaded through
the entire `desugar_var_*` chain.

## Fixtures

- `examples/effects/cap_read_in_interp.kai` — the repro shape; reads
  `@tally` both inside `"#{...}"` and bare, asserts the value, prints
  `tally=1`.
- `examples/effects/cap_read_in_interp.out.expected` — golden `tally=1`.
- Wired into `stage2/Makefile` as `test-issue-672` (compile + run +
  diff, mirroring `test-foreign-prelude`), added to the `test` and
  `.PHONY` aggregates so it gates under tier1.

Coverage gap closed: no prior fixture exercised a cap-read inside an
interpolation, so the array-slot specialiser's blind spot was
invisible.

## Cost

Localised, single-file fix (`desugar.kai`) plus one fixture and the
Makefile wiring. No typer-architecture change. The escape-walk-skips-
raw-spans pattern is the structural surprise worth flagging: any
pre-resolve pass that reasons about how a name is *used* will
under-count uses hidden in interpolation spans, because interpolation
bodies stay unparsed until `desugar_interp_decls`. `body_escapes` was
the instance that bit here; future use-counting passes (e.g. a
linearity or last-use pre-pass) should assume interpolation spans may
carry arbitrary reads.
