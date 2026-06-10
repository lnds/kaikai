# Lane retro — issue #772 (compound row arguments at type-alias instantiation)

**Scope:** Accept a *compound* effect row as a type argument when
instantiating a row-parameterised transparent alias —
`Stream[Int, Stdout + Fail]`, `Stream[Int, Stdout + f]` — and make a
*named* row alias expand in that position — `type SB = Stdout + Stderr;
Stream[Int, SB]`. The #624 follow-up its retro named verbatim:

> Compound row args at the use site are a parser-extension follow-up.

#772 was promoted out of post-1.0 because it is a hard prerequisite for
the #801 stream carrier: every user-facing stream signature has the
shape `Stream[String, File + Fail]`, which did not parse on `main`.

## Scope as planned vs as shipped

Planned (issue #772 fix sketch): a parser extension to accept a
`+`-joined row in the `[...]` argument list, plus a typer path that
lifts a compound concrete arg into a closed row the same way #624 lifts
a single label.

Shipped: the parser extension **plus three things the sketch did not
anticipate**:

1. A new short-lived AST node `TyRow(RowExpr)` (the representation
   choice — see below).
2. **The #624 "re-run effect-alias expansion" was never actually
   exercised for an aliased label.** #624's fixtures used `Stdout` (a
   real label, never an alias) and never *called* the binding, so a
   substituted *alias* label (`Console`, `SB`) was never run back
   through effect-alias expansion. `Stream[Int, Console]` reported
   `effect not handled: Console`. This is a real #624 gap that #772
   closes; it required a fourth, nested-row alias-expansion step.
3. Exhaustiveness fan-out: adding a `TyKind` variant forced a `TyRow`
   arm in **every** exhaustive `match … .tkind` across the compiler
   (~20 sites), because kaikai enforces match exhaustiveness at compile
   time and the compiler is self-hosted.

## Design decisions

### Representation: `TyRow(RowExpr)`, not a carrier or a parallel list

Three options weighed (asu consult):

- **A — new `TyKind` variant `TyRow(RowExpr)`.** Chosen. A compound
  row in type-argument position earns its own node; it is honest and
  structurally distinct.
- **B — `TyFn([], TyName("__row_carrier__"), row)` sentinel carrier.**
  Rejected. A magic name recognised by string is the "clandestine
  string-IR decoded twice" bug-class the KIR design doc flags; any pass
  touching `TyFn`/`TyName` can misread it.
- **C — parallel `[RowExpr]` slot on `TyName(String, [TypeExpr])`.**
  Rejected. Changing the `TyName` signature touches every constructor
  and destructure of `TyName` in ~20 files, including the hot emit
  paths — maximum blast radius.

`TyRow` follows `TyRefine`'s life-cycle exactly: born in the parser,
dissolved before resolve, never lowered to a `Ty`, never reaching the
emitters. The emitters' `match … .tkind` already carry `_ -> …`
catch-alls, so the native-emission lane's files were not touched.

### `TyRow` is born only on a syntactic `+`

The parser produces `TyRow` only when a `+` follows the first label in
a type-argument list (`parse_type_args_loop` → `parse_row_type_arg`).
The `+` is the only local, resolve-free signal that "this argument is a
row." A single ident (`SB`) stays a `TyName` and is resolved as an
effect alias later — the parser cannot know an uppercase ident is a row
alias without resolve information.

### Where the carrier dissolves, and where the alias label expands

- **Fusion** (`fuse_row_arg`, `rows.kai`): in `apply_tp_subst_row`,
  when the substituted row-tparam rep is a `TyRow`, the argument's
  labels and tail are folded into the alias row in place of the
  substituted variable. A kaikai row carries at most one open tail
  (`ROpen` has a single tail slot); the alias side is always
  `ROpen(rv, labels)` with `rv` the substitution point, and the
  argument has at most one tail, so the one-open-tail invariant holds
  by construction — no double-tail row can be formed here.

- **Aliased-label expansion** (`expand_row_aliases_in_te`, `rows.kai`):
  composed at the alias-instantiation chokepoint in `expand_ta_te`,
  on the stabilised node *after* the transparent-alias fixed point. It
  expands an effect-alias label (`Console`, `SB`) in every nested row
  of the instantiated TypeExpr (the positions `expand_aliases_in_decl`
  never reached: param / return / `let`-annotation rows). `AliasResult`
  is flat (the resolver pre-expands alias-of-alias into base labels), so
  one pass suffices; the helper never re-enters `expand_ta_te`, keeping
  the two expansions sequenced and order-independent (asu consult — this
  is composition `pure_post_process ∘ fixed_point`, not the rejected
  "interleaved C" variant).

### Threading `aliases` through `expand_ta_*`

The chokepoint composition needs the effect-alias table inside
`expand_ta_te`. Threading `aliases: [AliasResult]` through the 23-fn
`expand_ta_*` cluster was chosen over (a) a separate Decl→Decl walk
(which would duplicate the navigation skeleton — two walks to keep in
sync, itself a bug surface) and (b) threading the table into
`apply_tp_subst_row` (which would expand only labels lifted by
substitution, missing hand-written nested rows — covers the case, not
the class). The threading is purely mechanical (one forwarded param)
and confined to the front-end alias cluster, untouched by the native
lane.

## Structural surprises

- **The #624 re-run is `DFn.rexpr`-only.** `expand_aliases_in_decl`
  rewrites a function's *own* effect row but no nested row inside an
  annotation TypeExpr. That is why `Stream[Int, Console]` failed in
  param / return / let-binding positions identically. The fix had to
  reach all TypeExpr positions, which the `expand_ta_te` chokepoint
  does for free (every nested TypeExpr passes through it).

- **Exhaustiveness is a compile-time gate, caught only by selfhost.**
  The kaic1-built bundle compiles a non-exhaustive `match` to a runtime
  `default: panic`, so a missing `TyRow` arm slipped through the bundle
  build and paniced only when a fixture exercised that path. The
  *modular* selfhost build (kaic2 compiling its own source) enforces
  exhaustiveness statically and surfaced every remaining gap at once.
  `make selfhost` is the real gate for an AST-variant addition; the
  bundle build is not.

- **A leading lowercase row-var in a compound arg paniced.** `a +
  Stdout` seeded a row *variable* as the first *label*, flowing an
  unbound `Some(-1)` tail into substitution (the same `array_set` index
  -1 shape #624 fixed for a different cause). `te_as_row_label` now
  requires the leading label be uppercase; a lowercase leading ident is
  rejected at parse time ("expected an effect label before `+`").

## Fixtures shipped

Positive (`examples/effects/`, ride the backend-parity C+LLVM gate):

- `alias_row_arg_compound.kai` — `Stream[Int, Stdout + Fail]`, the
  #801 spike shape; producer + consumer + `Fail` handler, prints 1 2 3.
- `alias_row_arg_named.kai` — `type SB = Stdout + Stderr;
  Stream[Int, SB]` expands (was `effect not handled: SB`).
- `alias_row_arg_builtin_alias.kai` — `Stream[Int, Console]` in
  return-type position (the builtin alias case).
- `alias_row_arg_open_tail.kai` — `Stream[t, e] = … / Stdout + e`
  instantiated at `Fail + r`, fusing to `Stdout + Fail + r` with the
  argument's tail `r` surviving as the single open tail.

Negative (`examples/negative/parser_syntax/`, ride test-negative):

- `row_arg_leading_var.kai` — `Stream[Int, a + Stdout]` rejected with a
  clean diagnostic instead of the prior panic.

## #802 — diagnosed, not in this lane

#802 (lambda + `var` + function-typed param mis-inference) was confirmed
**independent** of #772 — a `while`-desugar / thunk-row-unification bug
in `infer`, not the alias/parser path #772 touches. Root: `while`'s
condition thunk and body thunk get *separate* fresh open row tails; when
the condition reads one `var` (`State[Bool]`) and the body touches a
different effect set (`State[Int] + Stdout`), the tails fail to unify.
The `(Bool) -> ?t8` param collapse in the issue's "Form A" is a
downstream symptom of the same root. A diagnosis comment was posted on
#802; the fix is its own lane.

## Cost

Larger than the "parser extension" the sketch implied — not because the
parser was hard (one re-read of the row after a `+`), but because the
representation choice rippled exhaustiveness arms across ~20 match sites
and because closing the #624 alias-label gap required the nested-row
expansion the sketch did not anticipate. The new logic is isolated in
`rows.kai` (km A+, 88 LOC, cogcom avg 2.0 / max 6); every other change
is a single arm or a mechanical parameter thread.

## Follow-ups

- **#802** — the `while` thunk-row unification bug. Independent lane.
- The `expand_ta_te` chokepoint now carries two responsibilities
  (transparent-alias expansion + effect-alias-label normalisation of
  the instantiated node). Documented inline; a future reader expecting
  one responsibility should see the comment.
