# Lane experience — `Ref[T]` surface sugar (`&` / `:=` / `@`), #1114 (subsumes #1113)

## Scope as planned vs. as shipped

Planned (per `docs/ref-sugar-design.md` + brief): three pieces — (1) `record.field := v`
for a `Ref` field, (2) `&x` make + bare-`Ref` `:=` set, (3) `@r` deref + migrate
`@`-as-binding to `as`.

Shipped: pieces 1, 2, and the *new* half of piece 3 (`@r.field` path deref) plus the
as-binding migration. The **big surprise** reshaped the plan: most of the brief's "piece
3" was **already on main**. Issue #275 had already shipped `@r` deref of a bare binding,
`r := v` set of a bare binding, and the `try_ref_sugar_op` typer bridge that maps `.get`
/ `.set` on a `Ref` receiver to `Mutable.ref_get` / `Mutable.ref_set`. The lane's real
gap was three narrower cuts:

- `record.field := v` (piece 1, #1113) — LHS grammar rejected `EField`.
- `&x` make (piece 2) — `&` was not a lexer token.
- `@r.field` — `@` deref accepted only a bare ident, not a dotted path.

## The brief was stale on the load-bearing claim — verified before acting

The design doc called migrating `@`-as-binding to `as` "the load-bearing dependency of
the elegant trio": deref supposedly *couldn't* use `@` until the as-binding moved off it.
That premise was already falsified on main — `@r` deref shipped in #275 **without**
touching the as-binding, because the two live in disjoint grammatical positions (deref in
`parse_unary`, expression context; as-binding in `parse_pattern`, only reachable from a
`match`/`let` LHS). The parser is never in both positions at once, so there is no parse
ambiguity and no dependency. The `asu` consult confirmed this is the standard LL(1)
positional-reuse pattern (Rust: `@` is *its* as-binding and `*`/`&` are refs; C: `*`
deref vs. multiply; the parser discriminates by position). Verifying the tree instead of
trusting the doc turned a claimed prerequisite into an optional readability move. The
design doc's stale claim was corrected in the same commit as the migration.

The migration was done anyway — user's call — as a soft-deprecation: `as` is now the
canonical spelling, `@`-as-binding stays accepted. The four repo fixtures moved to `as`.

## Design decisions and alternatives

**Piece 1 desugar target.** `record.field := v` desugars to `record.field.set(v)`
(`EField(EField(record,field),"set")`), mirroring the `cap := v` arm exactly. Alternative
considered: emit a direct `Mutable.ref_set(record.field, v)` op-call in the parser. Chose
`.set` because the typer's `try_ref_sugar_op` already maps `.set` on any `Ref`-typed
receiver — including an `EField` base, not just `EVar` — so the parser stays dumb and the
typer owns the effect-row injection. A non-`Ref` field falls through to the ordinary UFCS
lookup and its "method `set` not found for type `Int`" diagnostic, which is honest
(though it doesn't say "expected a Ref" — acceptable, matches the precedent).

**Piece 2 sigil.** `&` is a fresh `TkAmp` token; `&x` desugars in `parse_unary` to
`Mutable.ref_make(x)` — same AST shape as writing `Mutable.ref_make(x)` by hand
(`ECall(EField(EVar("Mutable"),"ref_make"),[x])`). Binds at prefix precedence via a
recursive `parse_unary` call, so `&x + 1` is `(&x) + 1`. `&` is free in kaikai (bitwise-
and lives behind UFCS in `stdlib/math/bits.kai`), so a bare `&` unambiguously opens a ref.

**Piece 3 path deref.** `@r.field` needed the `@` parser to consume a `.field` chain
(`parse_deref_path`). Key subtlety: a bare `@ident` keeps lowering to the `__deref`
sentinel (so the cell-read lowering can reject `@cell` — a `var` cell reads naked), but a
**dotted** `@r.field` can never be a cell (cells are simple bindings), so it skips the
sentinel and lowers straight to `r.field.get()`. The branch is one `match target.kind {
EVar(_) -> "__deref" _ -> "get" }`.

## Structural surprises the brief did not anticipate

- **`@r` / `r:=v` on a binding were already done** (#275) — the brief listed them as
  work. Only the field and path variants remained.
- **`try_ref_sugar_op` already generalises to `EField` bases** — the piece-1 and
  path-deref field cases required *zero* typer changes; the whole feature is parser sugar.
- **Sigil token needs no stage1 rebuild here** — the brief warned a new `TkAmp` might
  force a `kaic1` rebuild (the stage1 bundle lexes the compiler). It did not, because the
  compiler's own source uses no `&`; the stage2 selfhost passed byte-identical.

## Fixtures added and coverage

New (`examples/sugars/`): `ref_amp_make` (piece 2 trio), `ref_field_assign` (piece 1,
#1113), `ref_deref_field` (piece 3 path deref), `ref_field_assign_non_ref` (negative —
`Int` field has no `set`). Migrated: the four `@`-as-binding fixtures to `as`
(`m7d_14_at_pattern_{basic,no_inner,variant}`, `library_mode/def_at_pattern_as` — the
last needed +1 column shifts in its LSP `@probe` goldens, `@`→`as` being one char wider).
Rewrote `kaikai-vs-rust` case 1 to idiomatic `&None` / `@n.next` / `a.next := Some(b)` —
runs byte-identical (`trace=1 2 1 2 1 2`) on both backends.

## Coverage gap / follow-up left for next lanes

**Native backend as-binding-over-list gap (pre-existing, filed as an issue).** An
as-binding whose inner pattern destructures a list — `whole as [h, ...t]` **or** the
legacy `whole @ [h, ...t]` — fails the native backend with `unsupported KIR node (subset
gap): unbound register h/t`. It works on the C backend and works on native for
*variant* as-bindings (`found as Some(n)`), so it is specific to list-spread destructure
under a `PAs`, and it is **orthogonal to this lane** — identical for `@` and `as`, present
on main before the migration. `test-sugars` runs the C backend (kaic2 → C → cc), so the
fixture passes; the gap is invisible there. Not fixed inline (lane discipline); filed as
#1115 with the minimal repro.
