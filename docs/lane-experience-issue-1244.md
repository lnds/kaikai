# Lane retro — the `over <domain>` surface (#1244)

## Scope as planned vs. as shipped

Planned as the first third of a three-issue lane: `over` surface (#1244), then real
Module/Composition/Region engines (#1256), then Currency's generic carrier (#1243).

Shipped: **#1244 only.** The lane was split after the engine investigation for #1256
concluded that none of the three theories adds algebra the abelian engine does not
already give, which is a stop-and-report condition rather than an implementation task
(evidence below). #1243 depends on #1256's resolution — "the theory validates the
carrier" presupposes what `Module` means as an engine — so it is held with it.

The `over` surface was landed first precisely because it is verifiable standing alone,
and that ordering paid off: it is green and shippable while the engines wait on a
design decision.

## Design decisions

### `over` is a contextual keyword, not a token

The brief specified a new `over` token in `lex.kai`. That would have been wrong, and
the repo proves it:

```
examples/numeric/bigint_border.kai:30:  let over = bigint.add(i64_max, bigint.from_int(1))
```

A hard `TkOver` breaks that file on the next compile. Beyond the in-tree breakage it
violates Tier 1 #4 — a shipped surface stays valid within an edition, and `over` is a
perfectly ordinary variable name that user code is entitled to use. Introducing a hard
keyword is an edition-level commitment, not a side effect of adding a clause.

So `over` is recognised **contextually**: claimed only in the one slot between a
`kind` head's theory name and its `with`. Everywhere else it lexes as `TkIdent` and
behaves as any other identifier. This is not a new mechanism — `kind` itself is
contextual for the same reason (the ~1055 internal `.kind` field uses), and `region`
before it. LL(1) is preserved: after `kind Name : Theory` the next token is `with`,
`over`, or the end of the declaration — no ambiguity, one token of lookahead.

The regression fixture pins this deliberately: `kinds_over_domain.kai` declares
`kind Cur2 : Module over Decimal` **and** binds `let over = 41 + 1` in the same file,
printing `42`. A future lane that promotes `over` to a hard keyword fails that fixture
rather than silently breaking user code.

### The domain is a name, not a `TypeExpr`

`DKind` gains a `String` slot (declaration order: `is_pub, name, thy, over, intro,
line, col`), holding the domain's type name and empty when absent. The issue says
"recognize `over <TypeExpr>`"; a full type expression was rejected as premature. Every
domain the design names is a plain type name — `over Int` (Layout), `over Rational`
(Waterfall), `over T` (Currency) — and a `TypeExpr` would have to be threaded through
`fmt_decl`, the module qualifier, and the cache path for expressiveness nothing uses.
A flat name matches how the sibling `intro` slot is already stored (verbatim), and can
widen later without moving the field.

Deliberately **not** done: encoding the domain into the existing `thy` string. That is
the `#Unit` string-hack the kind design explicitly criticises, and repeating it to
avoid touching 30 pattern sites would have been the cheap wrong answer.

### `over` is validated, not merely parsed

`over` on a single-domain theory is a formation error:

```
theory `AbelianGroup` has one domain, so it takes no `over` clause
(only Module and Composition cross two domains)
```

This is the half that makes the clause mean something rather than be decoration. The
converse — *requiring* `over` on the two-domain theories — is deliberately **not**
enforced: existing fixtures declare `kind Cur : Module with cur` without it, and
making it mandatory is a surface break needing a migration path. Left as a follow-up
decision, not smuggled in.

## Structural surprises

- **`DKind` has 30 sites across 14 files**, but 8 of them are wildcard patterns, so
  widening the variant was mostly mechanical. The real cost was not the arity change;
  it was confirming that no site *reconstructs* a `DKind` from parts while dropping a
  field (`modules.kai:814` does rebuild one, and needed the new slot threaded).
- **The engine investigation was the expensive part of the lane**, not the surface.
  Reading `synth_dim_mul_div` / `st_check_module_unit` to establish what Module and
  Composition actually do consumed more of the lane than lexer, parser, AST, formatter
  and fixtures combined. That is the right ratio for a lane whose job is to avoid
  shipping a facade, but it is worth planning for.

## The discovered dependency: Composition needs measure-carrying habitants

The finding that stopped #1256, recorded here because it reshapes the plan:

`Composition = { assoc, measure }` in the catalog. Neither property is what runs.

- **`measure`** — the summed per-element measure, the thing that makes Composition
  distinct from every other theory, is not a unification concern at all today. Layout
  sums field byte sizes in codegen (`layout_derive`), reading widths from base types.
  Nothing in `unify_dim` ever sees a measure.
- **`assoc` without `commut`** — Composition declares order load-bearing. The
  implementation routes it to `unify_abelian`, which canonicalises by sorting. Writing
  `Real<hdr*body>` produces the diagnostic ``unit `body hdr` does not exist`` — the
  habitants come back **reordered**. Non-commutativity has no operational meaning.
  `a / a` on a Composition habitant likewise yields `1`, a group inverse the theory
  never declared.

So a real Composition engine cannot be written first: the measure it is supposed to
sum has nowhere to come from. The design's answer is habitant form (c),
`pct pct70 = 70` — a habitant that *declares* its measure, the user's side of the
contract with an intrinsic theory. That surface does not exist. The order is therefore
**measure-carrying habitant surface → Composition engine → Waterfall**, not the
reverse, and #1256 as written skips the first step.

Module's blocker is different and worth separating: its algebra is a *restriction* of
the abelian group, not an extension. `USD/USD → 1` is load-bearing (it is what makes
`kinds_module_scalar`'s `b / a` typecheck), so the abelian inverse cannot simply be
withdrawn. A first-order-equality engine only becomes coherent once `*` and `/` carry
Module-aware signatures — division of two `Money<c>` returning a bare carrier. That is
a typing-rule change, not a new unifier, and it is much larger than the issue implies.

## Fixtures

- `kinds_over_domain` (positive) — `Module over Decimal` and `Composition over Int`
  declared and used, plus the `let over` binding that pins the contextual keyword.
  Output `42`.
- `kinds_over_single_domain_err` (negative) — `AbelianGroup over Int` rejected.

`kinds_layout_fields` and the rest of the Layout suite pass unedited after the catalog
gained `Layout : Composition over Int`, which is the evidence the clause is inert
where it should be.

## Cost vs. estimate

Surface work landed in roughly the time expected for one of three issues. The lane
still consumed its full budget, because establishing *that #1256 must stop* required
the same depth of reading as implementing it would have. A "no" backed by evidence is
not cheaper than a "yes" — worth budgeting for on the remaining honesty-map lanes.

## Follow-ups

- **#1256 / #1243 are held** pending a design decision on what `Module` means as an
  engine, and on the measure-carrying habitant surface `Composition` needs first.
- **The catalog currently misdescribes `Composition`.** `{ assoc, measure }` claims a
  non-commutativity the engine does not honour and omits the `inverse` it does apply.
  Correcting the catalog is independent of building the engine and could land sooner.
- **Requiring `over` on two-domain theories** — currently optional to preserve shipped
  declarations; needs a migration path if it is to become mandatory.
