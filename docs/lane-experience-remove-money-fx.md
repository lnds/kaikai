# Lane experience — remove `money` and `fx` from stdlib

## Scope as planned vs as shipped

**Planned:** delete `stdlib/money.kai` + `stdlib/fx.kai`, their six
`examples/stdlib/` fixtures + `.out.expected` goldens, the aspirational
`examples/aspirational/event_ledger/` demo, the catalog entries in
`docs/stdlib-layout.md` and `docs/stdlib-roadmap.md`, and the stale
`decimal/money/fx` mention in a `stage2/Makefile` comment. A clean
removal, not a redesign.

**Shipped:** exactly that, plus one site the brief flagged for
verification but did not pre-map: `tools/audit-canonical-aliases.sh`
carried two live rows (`stdlib/money.kai:money:money`,
`stdlib/fx.kai:fx:fx`). The audit script iterates the listed module
files, so leaving those rows would make it fail on missing files after
the deletion. Removed both. This was the only in-tree consumer beyond
the mapped set; everything else that matched `money`/`fx` was a false
positive (see below).

## Why they die

`Money` was modelled as `{ amount: Decimal, currency: String }` — a
runtime `String` currency tag plus a runtime mismatch check on every
cross-currency operation. That is the wrong shape. Currency is a
compile-time distinction: a US dollar and a euro are different *types*,
and mixing them should be a type error caught by the checker at zero
runtime cost, not a `String` comparison that panics at run time. The
right model is currency as an inhabitant of a **taxon** (kaikai's
kind/type-equality mechanism), so `Money[USD]` and `Money[EUR]` are
unequal by construction and the compiler forbids `usd + eur` the same
way it already forbids `Int + String`.

`fx.kai` (currency conversion) sat entirely on top of `money.kai`
(`FxPair`/`FxRate`/`FxTable` + `money_*_via_fx` wrappers), so it goes
with it. There is no partial-keep here: the whole `Money`-as-String-tag
edifice is wrong, and `fx` inherits the flaw.

## What replaces them

Nothing in this lane — this is the demolition, not the rebuild. The
replacement is a `Currency` taxon (compile-time currency distinction,
cost-free) plus a re-derived `Money`/`fx` surface on top of it. That is
future work, gated on the taxon surface (`taxon`/`taxology`) landing.
Removing the old house first keeps the tree honest: no half-correct
`Money` lingers to be mistaken for the real thing.

## Structural surprises the brief did not anticipate

- **No build wiring to unpick.** The catalog doc claimed `fx` was
  "wired into `stage2/Makefile` `EXTRA_PRELUDE_FLAGS` and `bin/kai`'s
  prelude chain". That is stale: `EXTRA_PRELUDE_FLAGS` no longer exists
  in the Makefile (retired with the prelude concept). `money`/`fx` are
  ordinary on-demand stdlib modules with zero build-system coupling, so
  deleting them cannot break the compiler — confirmed by grep.
- **`fx` substring false positives everywhere.** The native backend
  names its effect-op emitters `emit_native_fx.kai` /
  `emit_native_fx2.kai` / `nemit_op_fx` / `nfx_*`, and fixture loops use
  a shell variable literally named `$$fx`. A naive `grep fx` lights up
  dozens of unrelated hits. Word-boundary greps
  (`grep -niE '\bmoney\b|\bfx\b'`) plus reading each hit separated real
  references from noise.
- **`import fx` matches `import fx2`.** A lane-experience retro
  (`native-double-resume`) mentions a fixture module `fx2`; the
  `import fx` grep matched it as a prefix. Bitácora, unrelated to
  `stdlib/fx.kai`.
- **A local `type Money` in a protocols example.**
  `examples/protocols/m12_8_y_interp_show.kai` declares its own
  `type Money = { amount: Int, currency: String }` to demonstrate
  `impl Show`. It does not import `stdlib/money.kai`; it is a
  same-named local type. Left untouched — deleting it would be
  out-of-lane and would break an unrelated `Show` fixture.

## Test-harness discovery

`test-stdlib` (and `test-stdlib-survey`) glob `../examples/stdlib/*.kai`
rather than listing fixtures, so deleting the six pairs simply drops
them from the iteration — no hardcoded list to update. No CI workflow
or Makefile target named any of the removed fixtures.

## Fixtures added / coverage gaps

None added — this is a removal lane. The removed fixtures exercised a
type shape that is being retired; there is nothing to keep testing until
the taxon-based `Money` lands, at which point it brings its own
fixtures. Coverage-probe baseline is unaffected (removed fixtures were
positive `.out.expected` goldens, not gap markers).

## Follow-ups left for next lanes

- Re-introduce `Money`/`fx` on the `Currency` taxon once
  `taxon`/`taxology` ships. The historical `docs/stdlib-roadmap.md`
  m14-follow-up row (superseded 2026-05-17) still lists `money`/`fx`
  among the 21 modules of that snapshot — left as dated bitácora, not
  rewritten, per the primary-vs-bitácora doc discipline.
- `tools/kaikai.vim` and `tools/kaikai-syntax.json` still highlight
  `Money` as a built-in type name. Cosmetic only (a syntax-highlight
  keyword, not a stdlib reference) and out of this lane's mapped scope;
  the highlight becomes correct again when the taxon-based `Money`
  returns.
