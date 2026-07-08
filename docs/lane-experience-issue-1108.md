# Lane experience — issue #1108: the kind engine over AbelianGroup

Outcome: user-declared abelian kinds, per-kind habitant isolation, and
use-site resolution ship. `kind Metric : AbelianGroup with metric` +
`metric m` works in user code; `metric m + imperial ft` is a type
error; a symbol shared across kinds (`unit USD` / `cur USD`) resolves by
qualification → `use kind` → unique-symbol, else a disambiguation error.
Every pre-kind `unit`/`Real<m>`/`[u: Measure]` program compiles,
self-hosts, and diagnoses byte-identically.

## Scope as planned vs shipped

Planned four blocks (from the brief); shipped all but the deferred
alias-across-kinds case:

1. **`unit` de-hardcode + introducers** — shipped. `kind K : AbelianGroup
   with intro` in user code; a linear token pre-scan builds the file's
   `intro → kind` map before any body is parsed.
2. **Engine + tparam over user kinds** — shipped, plus the integrator's
   rename and dispatch seam (below). `[u: Metric]` type-checks like
   `[u: Measure]`.
3. **Use-site resolution** — shipped, all four precedence paths with a
   fixture each.
4. **Fixtures + docs** — shipped: 8 `examples/sugars/kinds_*` fixtures
   (5 positive `.out.expected`, 3 negative `.err.expected`), `kai info
   units` and the design doc updated.

## The design inversion that shaped everything

The brief and `docs/kind-system-design.md` both say the isolation
mechanism is **a kind-tag on `TyDimT`**. Verifying against the code
(with the `asu` architect) showed that is the wrong place: the abelian
engine cancels units by **symbol-name equality** in `usym_insert`, so
isolation *is* non-cancellation in the `UTable`, and the discriminant
therefore has to live in the **symbol key**, not on the enclosing
`TyDimT`. A `TyDimT` tag would be a second source of truth for the same
identity — the desync anti-pattern that bit #1110 (mode↔KSlot) and #1083
(RC↔slot). So the shipped mechanism is **symbol rewriting**: the
resolver rewrites a bare `<sym>` to `Metric$sym` before `unit_to_table`,
and the engine isolates for free with zero new code and no `TyDimT`
change. The user confirmed the direction (a `<...>` never mixes kinds,
like types never mix), and the integrator confirmed the seam shape.

This is the load-bearing lesson: **the spike measured only the
homogeneous case and mis-located the mechanism.** The case that
discriminates the two designs — a symbol shared across kinds (`s` in
both Metric and Imperial) — was never exercised by the spike. It is now
a fixture (`kinds_shared_symbol_err`).

## What generalising the `Kind` enum did (and did not) cost

The brief expected generalising `Kind = KType | KUnit` from a 2-variant
enum to per-kind data. It turned out **the enum did not need to change
at all.** `Kind` stays the binary type-vs-unit discriminant; the
*specific* kind a unit-param ranges over rides in the encoded tparam
string (`u#Unit` for Measure, `u#Unit:Metric` for a user kind). Keeping
the enum binary left its ~15 producer sites untouched and kept Measure's
`#Unit` encoding byte-identical. The decoders (`tp_kind_is_unit` /
`tp_strip_kind` / new `tp_kind_name`) moved to `compiler/kinds.kai` and
generalised from a suffix check to a substring scan. So the feared enum
surgery was avoidable — the encoding already had the room.

## What the introducer table revealed

The v1 retro flagged the per-kind introducer keyword as "the delicate
piece needing a stateful LL(1) table". It is **not stateful and not in
the parser state**. The cited precedent (`region`/`opaque`) does not
apply — those recognise a *literal known to the compiler*, whereas an
introducer is a user-chosen word. But a **read-only pre-scan** over the
already-lexed tokens builds the `intro → kind` set *before* parsing any
body, so the dispatch recognises `metric m` by set membership,
order-independently (a habitant may be declared before its `kind`), and
without degrading the "did you mean `fn`?" typo diagnostic for an
unregistered word. The pre-scan's matcher is deliberately dumb (locate
`kind`, skip to `with`, take the introducer) to minimise divergence from
the real parser.

## The integrator directives (rename + dispatch seam)

- **Rename** `unify_unit`/`unify_unit_diff` → `unify_abelian`/
  `unify_abelian_diff`. The `unit` name was historical (Measure was the
  sole client); after generalisation the engine unifies any abelian
  kind, so the name would be false — the same reasoning as `Unit` →
  `Measure` (#253). `TyDimT` stays (`dim` is generic and exact). It is a
  private, in-file rename; selfhost byte-id is unaffected.
- **Dispatch seam** `unify_dim`: establishes the `unify_<theory>` family
  (structural / module / composition to follow). Today it dispatches
  unconditionally to the one engine, with a one-line invariant that the
  kind's theory selects the engine, so a future engine plugs in without
  touching callers. No `TyDimT` tag needed — isolation rides the symbol
  key, so the seam does not branch on a tag today.

## Structural surprises

- **Display vs mangling are two axes, and both are byte-id-critical.**
  A rewritten `Metric$m` must render as `m` in human diagnostics
  (`kind_sym_bare`, applied at 3 display sites + `fmt`) but keep its
  identity for C mangling (`kind_sym_mangle`, applied at 3 mangle
  sites). The default kind `Measure` is elided in *both* — that is what
  keeps `unit` code byte-identical. Missing a display site leaks `$`
  into an error; missing a mangle site either emits `$` (invalid on
  Linux C) or collides two habitants. The gate is a fixture that
  exercises a *unit error with display*, not just a type-check.
- **The alias/rewrite ordering bit.** Rewriting runs before alias
  expansion (so it can read the `DKind` map still present in the merged
  stream). An alias body (`unit Newton = kg·m/s²`) expands *after*
  rewriting, so its factors stay bare — which is fine *only because
  Measure is elided*: bare `kg` from the expansion unifies with a bare
  `kg` literal. Had we qualified Measure symbols, the alias body and a
  qualified literal would have mismatched (`m12_5_alias` regressed until
  Measure elision was added). Alias-across-*user*-kinds
  (`metric.Newton`) is the one case this ordering cannot handle; left as
  a documented follow-up (fix: rewrite after expansion).
- **`DKind` had to survive the doc strip.** It was stripped in
  `dsc_loop` right after parse; the rewrite needs its `intro → kind`
  map over the *merged* (cross-module) stream, so the strip moved to a
  new `strip_kind_decls` after rewriting. Every post-strip site already
  had a defensive `DKind` arm (v1 added them), so the blast radius was
  the strip point only.

## Fixtures added

`examples/sugars/kinds_*` (wired to `test-sugars`, tier1):
- positive: `user_kind`, `order_independent`, `qualified`, `use_kind`,
  `usd_per_kwh`.
- negative (`.err.expected`): `isolation_err` (Mars-Orbiter), `ambiguous_err`,
  `shared_symbol_err`.

## Coverage gaps / follow-ups

- **Alias-across-kinds** (`metric.Newton / imperial.Newton`): not
  isolated, because rewriting precedes alias expansion. Fix is to move
  rewriting after expansion; needs the kind map available post-strip.
  Documented in the design doc's open questions.
- **`[u: Metric]` argument-kind checking**: a `[u: Metric]` fn called
  with an Imperial symbol is not rejected on the tparam bound (the
  concrete-symbol mismatch still catches most cases). This is bound-of-
  kind checking on a unit-var, a separate axis; deferred. It does *not*
  accept-and-lie — a bare user-kind tparam annotation that cannot be
  parsed is a hard error, not silent.
- **`km` grade of `unit_walk.kai` is B+**, below the A− target. Its
  cognitive score is dragged by the 37-arm `ExprKind` walker — inherent
  AST arity copied verbatim from the alias descent, not tangled logic.
  `kinds.kai` (A+) and `kind_resolve.kai` (A) are on target. Splitting
  the walker would fragment a cohesive traversal; kept whole above the B
  floor.

## Cost vs estimate

Larger than a "generalise an enum" framing suggested, because the real
work was neither the enum (untouched) nor a new engine (reused) — it was
the two-axis display/mangling discipline and the pipeline ordering
around alias expansion and the `DKind` strip. The `map_units` extraction
(factoring the alias descent into a generic walker) paid for itself:
without it the rewrite would have duplicated ~250 LOC of AST descent.
