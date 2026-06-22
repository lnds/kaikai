# Lane experience — issue #773: nominal effect-row carriers (`Stream[t, e]`)

## Scope as planned vs as shipped

**Planned (the brief):** a focused typer change in `infer.kai` extending the
#594 head-owner pipe-dispatch matcher to accept a head type with an
`(element, row)` parameter list, so `Source[t, e]` rides `|` / `||` / `|?`.

**Shipped:** a substantially larger typer change. The brief's premise — that
the pipe-dispatch matcher rejects a two-parameter head — was **false**.
`synth_pipe_dispatch` never inspected head arity: it extracts the head *name*
(`inf_ty_head_name` discards type args), resolves the owning module by name,
and builds `expected = TyFnT([lhs_ty, f_ty], rv, row)` passing the *whole* LHS
type as the first argument. A `Source[Int, e]` LHS unifies against
`Source[A, e]` by ordinary structural unification. Verified end-to-end:
`s |? p | f` already dispatched to `source.filter`/`source.map`, compiled, ran,
and produced correct output on main; the #594 negative (swapped-arg `map`) still
surfaced "type mismatch in function call".

So #773's *literal* acceptance was already met. The real gap was deeper and
orthogonal: **the effect row in a nominal `TyCon`'s argument slot was dropped**.
A `Stream[String, File + ReadFault]` resolved its row slot to a phantom type var
(or the `__row_in_type_position__` placeholder), so the producer's effect leaked
uncheck — a soundness hole (Tier 1.1, "effects visible in row types"). This
afflicts `stdlib/stream.kai`'s `Stream[t, e]` in production: `read_lines`
(row `File + ReadFault`) consumed in a pure `main` compiled clean.

The user opted to fix the leak in this lane rather than close #773 on the
matcher alone. That decision reframed the lane from "pipe matcher" to
"thread the effect row through a nominal `TyCon` argument slot".

## The design decision: representation

Two consults with the language architect agent. The first recommended
**rejecting** nominal effect-row carriers (route users to a transparent alias,
which already threads the row via #772). That was killed by a hard fact: the
nominal carrier is **not optional**. The pipes dispatch by *head type*; a
function-type alias has no head, so the pipes decline. `stdlib/stream.kai`
already ships `pub type Stream[t, e] = Stream(...)` as a nominal wrapper *for
exactly this reason* (its doc says so), and the issue names it as the target.
Rejecting it would break the stdlib and selfhost.

So the row must thread through the nominal slot. The question was how to
represent "an effect row occupying a `TyCon` argument slot" given `Ty`'s args
are `[Ty]` and a row is not a `Ty`.

- **Rejected: a new `Ty` variant (`TyRowArg(Row)`).** ~25 exhaustive `Ty`
  match sites across `infer`, `emit_c`, `emit_shared`, `monomorph`, `perceus`,
  `protos` would each need a new arm — every one a chance to silently lower the
  type if an arm was missed. High blast radius.
- **Chosen: a degenerate `TyFnT([], TyUnit, row)` carrier** (`row_arg_carrier`).
  `TyFnT` is an existing `Ty` constructor every walker already handles. Zero new
  arms. Unifying two carriers routes the row to `unify_row` via the `TyFnT`
  arm — sound without mixing the type-var / row-var substitution pools (the
  carrier's *constructor* carries the kind, not a reused id). Mangling of
  `TyFnT([], TyUnit, _)` ignores the row, so two `Stream[t, e]` differing only
  in `e` share one monomorphisation — the row is correctly erased at codegen.

`grep TyRowArg` returns zero: no new `Ty` variant shipped.

## Narrow vs general

Narrow. The carrier is keyed on a tparam the type declares in *row position*
(after `/`) inside a ctor field — detected syntactically by
`collect_rvars_in_typeexprs`, the same mechanism #801 already used. No kind
inference, no HKT, no constraint propagation (all banned). A per-type
`RowKindEntry` table (`compute_row_kind_slots`) records which slots are
row-kind; use-sites consult it. This is first-order, syntactically determined
kinds — not a kind system.

## Where the matcher tolerates the row (the actual edits)

1. **`scheme_of_variant`** — the head `TyCon`'s slot for a row-kind tparam is
   `row_arg_carrier(row_with_tail(id))` keyed on the same row-var id the ctor
   field's `/ e` uses, so the head slot and the function row are one row var.
2. **`resolve_ty_with_binds_rk`** (+ `resolve_tycon_arg`) — a `TyCon` argument
   slot the parent declares row-kind resolves to a carrier, covering a row var,
   a single concrete effect (`Tick`), or a compound row (`File + ReadFault`)
   equally. Threaded via a backward-compatible wrapper (`[]` table = legacy).
3. **`fn_scheme_of_decl_rk`** + **`collect_rowkind_args_decl`** — a row var
   sourced only in a `T[..., e]` *return* slot (the `from_list(...) : Stream[t, e]`
   case, where `e` never appears after a `/`) is collected, bound, and
   generalised as a row var; its dead type-var id is dropped from the scheme.
4. **`infer_decl`** + **`param_ty_or_fresh_with_binds`** — the body-walk's
   param/return resolution consults `env.row_kind` so the declared "expected"
   type matches the registered scheme (this is what made annotation naming work).
5. **`inf_resolve_ty_rk`** + **`inf_resolve_row_arg`** — `let`-annotation
   resolution honours the table, so `let s : Stream[String, File + ReadFault]`
   unifies with the inferred carrier.
6. **`TyEnv.row_kind`** field carries the table; populated over the
   *whole-program* decl stream in `collect_program_data_inherited` (same scoping
   as alias resolution) so a root `fn : Stream[t, e]` resolves against stdlib's
   `Stream`.
7. **`validate_nominal_row_vars_decls`** (driver) — a nominal type whose ctor
   field uses an *undeclared* row var (`type Q[t] = Qq((t) -> Unit / e)`) was a
   compiler **panic** (`tpbind_id_of`); now a clean unbound-row-var diagnostic.
8. **`inf_tycon_arg_to_string`** — diagnostics render the carrier as the row
   (`Stream[String, File + ReadFault]`) not the carrier fn.

## The #624 / compound-row-arg prereq finding

#772 (CLOSED) already handles compound row args at **transparent alias**
instantiation: `type Source[t, e] = () -> Option[t] / e` (an alias, not a sum)
names and unifies cleanly today. The gap this lane closes is the **nominal**
case. After the fix, naming a nominal carrier works for both the function-
signature form (`read_lines : Stream[String, File + ReadFault]`, which the
stdlib relies on) and the `let`-annotation form, for single concrete effects,
compound rows, and row vars, same-module and cross-module.

## Fixtures

- `examples/effects/row_carrier_pipe.kai` (`ticks=3`) — nominal carrier rides
  the pipes AND threads its row; the `Tick` handler counts producer effects.
- `examples/effects/row_carrier_naming.kai` (`named ticks=2`) — concrete-row
  annotation (`Stream[Int, Tick]`, function sig + `let`) resolves the row slot.
- `examples/negative/effects/row_carrier_leak.kai` — **the soundness gate**: an
  effectful producer in a pure `main` is rejected. Selfhost byte-id is
  FALSE-GREEN for this fix (the compiler declares no nominal effect-row
  carrier), so this negative is what actually distinguishes "row threads" from
  "row leaks".
- `examples/negative/effects/row_carrier_unbound.kai` — undeclared row var →
  diagnostic (was a panic).

The #801 precedent `ctor_field_effect_row.kai` (already in the suite) still
passes — this lane completes the head-slot half of what #801 started on the
ctor-field half.

## Structural surprises

- The brief's premise was wrong; the matcher never needed changing. Verifying
  this empirically before writing code (a reproducer + the generated C showing
  `source.map(source.filter(...))`) saved building the wrong thing.
- The leak is in `stdlib/stream.kai` in production — not a hypothetical.
- `--dump-typed` does NOT build the head-owner map, so it prints a spurious "no
  module declaring type `Stream`" error and pure-row callee types; it is
  unreliable for diagnosing pipe threading. Direct qualified calls dump
  faithfully — use those.
- Effect aliases (`Console = Stdout + Stderr + Stdin`) and builtin effects
  (`Stdout`) are absorbed by `main`'s REmpty branch, so a `Console`-carrying
  carrier in a pure `main` compiles — that is correct, not a leak. The
  soundness fixtures use a *non-builtin* effect (`Tick`) to exercise the gate.

## Coverage gaps / follow-ups

- The carrier's diagnostic display shows the row labels when concrete but `_`
  when the row is an unresolved tail var at print time (`Stream[Int, _]`).
  Cosmetic; the type is correct underneath.
- The whole-program row-kind table is computed per typer entry; it relies on
  `all_program_decls` being populated. The path where it is empty falls back to
  the current module's decls — adequate for the corpus but a latent edge if a
  future caller drives the typer with a partial decl stream.

## Gates

selfhost byte-id (held at every step), tier0, tier1 all green. Native backend
built (`KAI_LLVM=1`) and serial backend-parity run (typer-only change, no
codegen impact — the carrier is erased at codegen). New fixtures compile, run,
and match goldens; the #594 negatives (`pipe_wrong_signature`, `pipe_no_map`,
`pipe_ambiguous`) stay red; the #594 positives stay green.
