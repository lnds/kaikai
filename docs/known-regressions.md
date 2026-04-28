# Known regressions

Active bugs in `main` that are not blocking selfhost / `make test` but
should be fixed when the appropriate lane visits the relevant code.

Each entry records the symptom, a minimal repro, the hypothesis, and
the fix-verification path so the next agent does not have to
re-discover anything.

---

## R1 — `list_contains` / `list_sort_by` self-recursion leaks a row variable when called from an effectful context

**Status**: **FIXED** 2026-04-26. The hypothesis ("row-unify path
problem") was wrong. The actual bug: the stdlib core file (then
`stdlib/core.kai`, now `stdlib/core/list.kai` after the 2026-04-27
split) declared its polymorphic functions as
`pub fn list_contains(xs: [a], x: a) : Bool`
*without* the `[a]` tparam brackets. Post-m7b #13, lowercase
identifiers in type position are nominal `TyCon`s unless declared
in `[…]`, so `a` was a concrete type and the call site failed to
unify with `[Int], Int`. The reason `make test` looked green is
that the recursive self-call inside the stdlib unifies `a` with
itself; only an external caller with concrete element types
exposes the mismatch.

**Fix**: `fn_scheme_of_decl` and `infer_decl` now auto-collect
lowercase identifiers in a decl's signature that were not
declared in `[…]` and treat them as *implicit* tparams. The
explicit-bracket form keeps working; users who wrote it pre-#13
do not need to rewrite.

**Side effect**: the m7b #13 negative fixture
`examples/sugars/m7b_13_lowercase_undeclared.{kai,err.expected}`
is removed because `pub fn id(x: a) : a = x` is now valid.

Original report below for posterity.

---

**Severity**: medium. `make test` is **green** — every official
fixture under `examples/` compiles. The regression manifests only
when a program *imports the new stdlib list ops* and *its `main` (or
caller) carries an effect row*.

### Symptom

```
$ make -C demos verify
...
state_explicit       FAIL kaikai: error: type mismatch in function call
state_var            FAIL kaikai: error: type mismatch in function call
```

Per-error detail (from `demos/build/state_explicit.err`):

```
error: type mismatch in function call
  --> state_explicit/main.kai:201:34
  = note: expected: ([a], a) -> Bool
  = note: found:    ([Int], Int) -> ?t5 / ?e1
error: type mismatch in function call
  --> state_explicit/main.kai:389:51
  = note: expected: ([a], (a, a) -> Int) -> [a]
  = note: found:    ([Int], (Int, Int) -> Int) -> ?t0 / ?e0
```

The reported file/line points into the demo source but the column
reflects the **prelude** prepended by `kaic2 --prelude
stdlib/core.kai` (the pre-split monolith; today the equivalent is
the chain of `--prelude stdlib/core/*.kai`). Lines 201 and 389 of
the concatenated file fell on the recursive call sites inside
`list_contains` and `list_sort_by` respectively (declarations
formerly at `stdlib/core.kai:163` and `:375`, now in
`stdlib/core/list.kai`).

### What is in the demos

`demos/state_explicit/main.kai` uses **only** `State[Int]` plus
`Console.print`; it does not call `list_contains` or `list_sort_by`
itself. The error comes from elaborating the prelude in this caller's
typing context.

`demos/state_var/main.kai` is similar: uses `var n = 0` (m7b #5b
sugar that desugars to `with State[Int](init) as n`) plus
`Console.print`.

Neither demo touches the new list ops directly. The bug is in how
the typer elaborates the prelude when **the surrounding context
carries an effect row**.

### Hypothesis

Probable interaction between m7b parametric effects (m7b #11 / #12)
and self-recursive prelude functions:

1. `list_contains(xs: [a], x: a) : Bool` (and `list_sort_by`)
   has a recursive call inside its body.
2. m7b #11/#12 introduced row inference for callees: at each call
   site of a polymorphic function, a fresh row variable is
   instantiated.
3. The recursive self-call inside `list_contains` instantiates its
   own row var `?e1`. In a pure context (`make test` fixtures), `?e1`
   unifies to the empty row by default.
4. In a caller whose `main` has a non-empty row (`/ Console`,
   `/ State[Int]`, etc.), the prelude is elaborated **after** the
   caller's row variable is bound. Some unification step fails to
   propagate the caller's row into `?e1`, leaving it free. The
   resulting type does not match the canonical signature
   (`([a], a) -> Bool` with no row) — type mismatch.

This is consistent with: `make test` fixtures use pure `main` (no
row), so `?e1` collapses; demos use effectful `main`, so `?e1` is
exposed.

### Repro outside `demos/`

The minimal repro (do **not** check this in — only for diagnosis):

```kai
# regress.kai
fn main() : Unit / Console = {
  let _ = list_contains([1, 2, 3], 2)
  Console.print("ok")
}
```

```sh
# Pre-split form (kept for the original repro shape):
./stage2/kaic2 --prelude stdlib/core.kai regress.kai > /dev/null
# Post-split equivalent:
./stage2/kaic2 $(for f in stdlib/core/*.kai; do echo --prelude $f; done) regress.kai > /dev/null
# both fail with the same `?e1` row-var leak (pre-fix)
```

### What `make test` does that masks the bug

Existing `examples/stdlib/list_basic.kai` (and siblings) declare
`fn main() { ... }` with no row annotation. The fix path for
`make test` should add at least one fixture with effectful main
that uses a stdlib list op — e.g.:

```kai
# examples/stdlib/list_in_effect_context.kai
fn use_in_effect() : Unit / Console = {
  if list_contains([1, 2, 3], 2) {
    Console.print("yes")
  } else {
    Console.print("no")
  }
}

fn main() : Int / Console = {
  use_in_effect()
  0
}
```

If this fixture passes, the regression is fixed.

### Where to look in the typer

`stage2/compiler.kai` — most likely in the unification path for
parametric row labels added by m7b #11 (`row.ty_args` propagation) or
m7b #12 (open row variables). Specifically, the moment a generic
function is instantiated at its call site **inside the elaboration
of a polymorphic function whose own row has not yet collapsed**.

The error shape after the m7b sweep:

```
expected: ([a], a) -> Bool
found:    ([Int], Int) -> ?t5 / ?e1
```

`expected` is the registered scheme (no row — `list_contains`'s
declared signature is `: Bool` with no `/ row`). `found` is the
self-call's instantiation: tyvars are bound (`a := Int`) but a
fresh row var `?e1` survives. The mismatch is row-shape: empty
row vs row-with-free-var. In a pure caller, `?e1` ends up unified
to `{}` by some side-channel (TBD); in an effectful caller, it
stays free.

The owner is the next agent who **specifically audits the row
side of `unify_*` and `synth_app`** in stage2/compiler.kai. The
m7b sweep (#14, #15, #16, #17, #2a-c, #4, #7) closed in 2026-04-26
without touching this path — none of those tasks needed it.

### Verification path

After a candidate fix:

```sh
make                                    # selfhost still green
make test                               # all suite passes
make -C demos verify                    # state_explicit, state_var → OK
make -C demos/9d9l verify               # 4d9l unchanged (still gated on use Effect / etc.)
make -C demos/vs verify                 # vs/ unchanged
```

Specifically `state_explicit` and `state_var` must flip to OK; that
is the smallest visible signal that the row-leak is gone.

### Workaround for callers (temporary, do not commit)

Casting the call site through a wrapper with explicit row breaks the
leak in some experiments — but the fix is in the typer, not the
caller side. No source-level workaround is recommended; let the
typer be fixed.
