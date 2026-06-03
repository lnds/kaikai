# Lane experience — mono-struct C name collision (issue #647 backend follow-up)

## Scope as planned vs. as shipped

**Planned (from the 2026-06-03 bugfix handoff):** the handoff listed
`shadowing/user_redeclares_same_arity_private` as
"`--path` harness noise (missing `regexp` module), probably not a real
bug — confirm." The task was to confirm whether it was noise.

**Shipped:** it was **not** noise — `stdlib/regexp.kai` exists and loads
fine. The fixture exposed a real backend codegen bug: the monomorphic
node-representation emitter (`mono_sum_decls`) names every sum-type
struct `kai_user_<bare-name>`, so a user type and a private prelude type
that share a bare name emit two `struct kai_user_NB_FragR { ... }` and
clang rejects the redefinition. Fixed by minting the struct name via the
same module qualifier the function symbols already use.

## Confirming it was a real bug, not harness noise

- `stdlib/regexp.kai` is present; `kaic2 --path stdlib <fixture>` emits C
  cleanly (exit 0). So the "missing module" hypothesis was wrong.
- The *compile* of the emitted C failed:
  `error: redefinition of 'kai_user_NB_FragR'` at two `struct` decls.
- `grep "struct kai_user.*NB_FragR"` on the output showed the type
  emitted twice: once for regexp's private
  `type NB_FragR = NB_Frag(NfaBuilder, NfaFrag)`, once for the user's
  `type NB_FragR = LocalA | LocalB`. Both bare → identical C name.

## Root cause

`mono_sum_decls` (emit_c.kai) walks every `DType` with a `TBSum` body
and emits `struct kai_user_<name>` using the **bare** type name. It
ignored the decl's module-origin field entirely. Two homonymous sum
types from different modules therefore minted the same C struct name.

Issue #647's mangling (`mangle_prelude_segments`, `mod::name`) fixes the
typer's *variant table* (exhaustiveness) but never reaches the struct
emitter — the emitted C had **zero** `::` and both structs came out
bare. #647 and this bug are the same collision in two different layers;
#647 closed the typer layer, this closes the backend layer.

## Fix

Mint the struct name with `c_sym(name, mo)` — the exact module-qualifier
the function symbols already use (`kai_regexp__rx_ok`). The result:

- regexp's private type → `struct kai_user_regexp__NB_FragR`
- the user's type → `struct kai_user_<rootmod>__NB_FragR`

Distinct names, no redefinition. The diff comment still prints the bare
`type NB_FragR` (a new `srcname` param carries the unqualified name for
the comment only), so the Koka-mould diff stays readable.

The per-constructor structs (`mono_ctor_struct` / `mono_ctor_structs`)
needed no change — they already build `kai_user_<tyname>__<ctor>` off
the `tyname` they receive, so passing the qualified name in propagates
the module prefix automatically.

## Design decisions / alternatives considered

Three options were weighed (asu architect consult):

- **(A) dedup-skip** — track emitted names, skip the second. Rejected:
  silently discards divergent layout, lies in the diff that is the
  structs' only purpose today, and breaks at the first real consumer.
- **(B) namespace only the prelude type, keep user bare** — rejected:
  needs a reliable "is this from a module?" test, and the driver warns
  the `DType` module-origin field is unreliable for prelude decls.
- **(C') namespace unconditionally via `c_sym`** — chosen. Aligns the
  struct names with the function names (which are *already* qualified),
  kills the collision at the root for any module pair, and is exactly
  what the mono-construction phase will need when these structs gain
  consumers. No bifurcation, no reliance on a per-decl flag.

A key empirical finding overturned the driver's pessimism: the `DType`'s
`mo` field **is** populated in the emit path (the regexp type came out
`regexp__NB_FragR`, the user type `<rootmod>__NB_FragR`). The driver's
warning is about a *streaming heuristic* (`decl_home_hint_reset`), not
about the `mo` field that `expand_imports` populates — which is the same
field the function symbols ride.

## Why this is correct today and forward-safe

The mono structs are **additive** — "emitted but not yet
constructed/matched/dropped against. Nothing reads them yet" (verified:
`kai_user_` appears only in this emit_c.kai block, no other pass). So
the only requirement today is that the C compile. Namespacing satisfies
that AND pre-stages the naming the construction phase will require, so
it is not throwaway scaffolding.

## Fixtures

No new fixtures — the two pre-existing ones
(`shadowing/user_redeclares_same_arity_private_nbfragr` and `_splitn`)
encode the bug shape (user redeclares a bare name that a private prelude
sum type also uses). Both now pass via the C backend; `nbfragr` also
verified via LLVM (which does not emit these structs, so it was never
broken). Verified end-to-end: emit → cc → run → diff golden
(`user=3 match=1`).

## Coverage gaps

Covered: one user type colliding with one private prelude sum type.
Not exercised: three-way collision (two modules + user all sharing a
bare sum-type name) — the fix handles it (each gets its own
`<mod>__name`) but no fixture proves it. Low priority; the mechanism is
per-decl and order-independent.

## Follow-ups

When the mono node-representation phase grows consumers (construction /
match / drop against these structs), the *references* to these struct
names must mint through the same `c_sym(name, mo)` path so they resolve
to the qualified name. That is in-scope for that phase, not this lane;
flagged here so it is not forgotten.
