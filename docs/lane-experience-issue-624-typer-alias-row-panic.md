# Lane retro — issue #624 (typer panic: alias + row variable + concrete row)

**Scope:** Filed by ahu's `stream.kai` Layer 1 work. The compiler paniced
with `array_get: index -1 out of range (len=16)` when a transparent type
alias declared a row variable as a tparam and was instantiated at the
annotation site with a concrete effect-label arg.

**Repro (8 lines):**

```kai
pub type Foo[t, e] = () -> Option[t] / e
fn main() : Int = {
  let f : Foo[Int, Console] = () => None
  let _ = f
  0
}
```

## Panic site

Localised via `lldb` post-mortem (rebuilt `stage2/kaic2_debug` with the
runtime's `array_get` bounds check swapped from `exit(1)` to `abort()`):

```
frame #0: abort
frame #1: kai_array_get_impl (runtime.h)
frame #2: kai_prelude_array_get
frame #3: kai_subst_row_lookup        (compiler.kai:27744)
frame #4: kai_apply_row               (compiler.kai:28374)
frame #5: kai_apply_ty
frame #6: kai_synth_stmt              (let-binding annotation)
```

Reading `subst_row_lookup`:

```kai
fn subst_row_lookup(s: Subst, v: Int) : Option[Row] {
  if v >= array_length(s.row_slots) { None }
  else { array_get(s.row_slots, v) }      # v = -1 → panic
}
```

The guard only checked the upper bound. The `Some(-1)` sentinel arrived
via `row_of_expr_with_binds` at compiler.kai:27230:

```kai
ROpen(rv, labels) -> Row {
  ...
  tail: match rvbind_lookup(rbinds, rv) {
    Some(id) -> Some(id)
    None     -> Some(-1)        # ← sentinel, paniced downstream
  },
  ...
}
```

## Root cause

Three structural facts compose into the bug:

1. **Transparent type aliases substitute TypeExpr-by-TypeExpr.**
   `apply_tp_subst_te` walks the alias body but its `TyFn` case
   ignores the row field — only `ps` and `ret` get rewritten. The
   alias body `() -> Option[t] / e` instantiated at `Foo[Int, Console]`
   thus produced `TyFn([], Option[Int], ROpen("e", []))` instead of
   `TyFn([], Option[Int], RClosed([Console]))`. The row tparam `e`
   survived as a stray ROpen name.

2. **`row_of_expr_with_binds` emits `Some(-1)` for unbound row vars.**
   When `resolve_ty_with_binds` reached the surviving `ROpen("e", [])`
   inside the let-binding annotation, `rbinds` was empty (a let
   binding has no fn-level row-var scope) and the lookup returned
   `None`. The original code mapped that to `Some(-1)` as a sentinel
   for "unknown row variable" — meant to be reported by a downstream
   diagnostic but never actually reported. It just flowed through
   `apply_row` until it tripped `array_get(-1)`.

3. **Effect-alias expansion runs only before transparent alias
   expansion.** `expand_aliases_in_decls` runs first; it would have
   turned `RL("Console", [])` into the expanded `Stdout+Stderr+Stdin`
   labels. But `expand_ta_decls` then injected fresh `RL("Console", [])`
   labels into the decl stream (via my fix below), too late for the
   alias-expansion pass.

The ablation tracked exactly these three:

| Variant | Trigger condition violated |
|---|---|
| Alias with two type params, no row | (1) not exercised (no row in alias body) |
| Inline row, no alias | (1) not exercised (no alias substitution) |
| Alias with hardcoded row | (1) not exercised (no row tparam to drop) |

## Fix

Three small, layered changes in `stage2/compiler.kai`:

1. **Defensive guard in `subst_row_lookup`** (compiler.kai:27742). Treat
   `v < 0` as `None`. Closes the panic on its own; the rest of the
   diagnostic stack already handles "unknown row tail" gracefully.
   2 LOC.

2. **Substitute row position in `apply_tp_subst_te`** (compiler.kai:24564).
   New helper `apply_tp_subst_row` walks the row, looks up the tparam
   name, and either lifts an uppercase `TyName(NN, [])` into a label
   (`RClosed(labels + [RL(NN, [])])`) or treats a lowercase name as
   another row variable (`ROpen(nm, labels)`). Complex TypeExpr args
   (e.g. `TyName("Foo", [Int])`) fall through unchanged; the natural
   "unknown row variable" diagnostic catches them later. ~60 LOC.

3. **Re-run effect-alias expansion after transparent-alias expansion**
   (compiler.kai:60128). The new labels surfaced by (2) need a second
   pass through `expand_aliases_in_decls` so `Console` resolves to
   `Stdout + Stderr + Stdin`. 4 LOC.

Also extended `expand_ta_te`'s `TyFn` case to walk the row (a separate
correctness gap: aliased effect-alias labels inside a row inside a
transparent alias never got expanded). ~25 LOC.

Total: 6 fns added, ~95 LOC net add in compiler.kai (one diagnostic
guard, one row-substitution helper, one alias-expand walk, one
re-ordering at the program-level pipeline). No data-structure
changes, no kind-system refactor.

## What did NOT change

- **No kind separation between type tparams and row tparams.** The
  brief flagged this as a possible "departure" requiring escalation;
  the fix did not need it. Row tparams remain stored in the same
  tparam list as type tparams, distinguished by where they appear in
  the body (`ROpen("e", ...)` vs `TyName("t", [])`). The lifting
  happens at substitution time, not at alias-declaration time.

- **No parser change.** `Foo[Int, Mutable + Console]` (row-expression
  argument) is *not* supported by this lane. The Stream shape works
  when each row arg is a single uppercase label (or another row var
  name). Compound row args at the use site are a parser-extension
  follow-up.

- **No `array_get` change.** The panic was a symptom of `Some(-1)`
  flowing where it should not. Removing the bounds check would have
  masked the bug; the fix addresses the source of the sentinel.

## Acceptance check

| Variant | Pre-fix | Post-fix |
|---|---|---|
| 1. Trigger (`Foo[Int, Console]`) | panic | OK |
| 2. Two type params, no row | OK | OK |
| 3. Inline row, no alias | OK | OK |
| 4. Alias with hardcoded row | OK | OK |
| 5. Stream shape (`Stream[Int, Stdout]`) | panic | OK |

Selfhost byte-identical (kaic1 → kaic2 → kaic3 all match). Tier 0
green; Tier 1 green; tier1-backend-parity green (after fixtures
added).

## Fixtures shipped

- `examples/effects/alias_row_var_basic.kai` — variant 1 (the trigger),
  closes the panic regression.
- `examples/effects/alias_row_var_compound.kai` — the ahu Stream shape
  with `Mutable + e` tail.
- `examples/effects/alias_two_type_params.kai` — variant 2 regression
  guard.
- `examples/effects/alias_inline_row.kai` — variant 3 regression guard.
- `examples/effects/alias_hardcoded_row.kai` — variant 4 regression
  guard.

All five carry `.out.expected` goldens and ride the `examples/effects`
backend-parity gate (C + LLVM).

## Follow-ups

1. **Compound row argument at use site.** `Foo[Int, Mutable + Console]`
   still requires single-label or single-name row args; a richer arg
   shape needs parser support and a separate substitution path
   (RowExpr-as-TypeExpr-arg). Open follow-up if/when ahu needs it
   beyond the single-name case.

2. **`use_it(f: Foo[Int, Console])` consumer pattern.** When the alias
   is used as a function-parameter type and the body invokes `f()`,
   the typer currently reports a Console / Stdout+Stderr+Stdin
   mismatch — the row in the inner annotation stays as `Console`
   single label while the enclosing fn's row has already been
   expanded. This is a pre-existing limitation (reproduces with
   inline `f: () -> Option[Int] / Console` too, no alias involved);
   not in scope for #624 but worth filing separately. The ahu work
   should be aware that consumer functions need to declare their
   row in the expanded form.

3. **Sentinel cleanup.** The `Some(-1)` route in `row_of_expr_with_binds`
   is now load-bearing only for the "unknown row variable name"
   diagnostic path. A cleaner shape would have `tail: Option[Int]`
   replaced by a proper sum (`Resolved(Int) | Unresolved(String)`)
   so the diagnostic carries the original name. Defensive guard
   in place; data-shape change deferred.

## Real cost

Brief estimated 0.2–0.4 day. Actual:

- ~30 min to reproduce + lldb-localise the panic site.
- ~20 min to trace the row-of-expr fallback to the sentinel.
- ~45 min to design the three-layer fix (substitution path +
  re-run alias expansion + defensive guard) and confirm selfhost.
- ~30 min for fixtures + retro.

≈ 2 h end-to-end. Inside the estimate. The bug was localised faster
than expected because the runtime panic site (`subst_row_lookup`) was
two stack frames away from the source-level cause, and `lldb` against
a rebuilt `kaic2_debug` (one swap of `exit(1)` → `abort()` in
`stage0/runtime.h`) gave a complete symbolic backtrace.

## Lessons

- **Negative sentinels paired with unchecked array_get always lose.**
  Two unchecked sites (`subst_row_lookup`, but also any future
  consumer of `Row.tail`) would have shown the same panic shape.
  Defensive bounds checks at the public-fn boundary are cheap.

- **Type-alias substitution is asymmetric across positions.**
  `apply_tp_subst_te` traversed `TyName`, `TyList`, `TyFn(ps, ret, _)`,
  `TyDim`, `TyRefine` — five constructors out of six. The sixth, the
  row inside `TyFn`, was the only one without a recursive walk, and
  that asymmetry was the bug. When adding a new TypeExpr-walking pass
  in the future, audit every constructor field; the row field is the
  trap.

- **The brief's hypothesis was right.** "Off-by-one in a tparam walk"
  was the working theory; the actual cause was different (no walk at
  all in row position, sentinel emitted elsewhere) but in the same
  family. The 16 in `len=16` was just the row_slots initial capacity,
  unrelated to the off-by-one suspicion — a useful reminder that
  panic numerics are sometimes meaningful and sometimes ambient.
