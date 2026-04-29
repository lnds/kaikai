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

---

## Demos failing inventory (as of v0.7.1, 2026-04-29)

`make demos-no-regression` reports **20 passing (baseline 20)** plus
**6 failing**. **None of the 6 are regressions** — they fall into two
categories:

1. **Aspirational** — demos that intentionally use syntax for features
   not yet shipped (m7b #5b sugar, tuples).
2. **Pre-existing pre-m14 v1** — demos that depend on language pieces
   or stdlib pieces that were never finished (regex anchors, demo
   not updated to use `!` postfix on `Option`, module path resolution
   plus legacy `println`).

Documenting each failure with root cause and fix path so the next
agent does not need to re-discover. Updating this section is part of
any milestone close that changes the failure inventory.

### Aspirational (intentionally future-looking)

These demos declare in their own comments that they exercise sugar
forms not yet landed. Their FAIL is the expected status until the
feature ships.

#### `demos/state/main.kai` — m7b #5b sugar

```kai
fn make_counter() : () -> Int / Mutable = {
  var n = 0
  () => {
    n := n + 1
    n
  }
}
```

**Error**:

```
type mismatch in return type of make_counter
expected: () -> Int / Mutable
found:    () -> ? / State[?t1] + ?e0
```

**Cause**: `var n = 0` desugars to `with State[Int](0) as n` — the
typer infers `State` effect, not `Mutable`. The `var → Mutable`
collapse is m7b #5b sugar (see `docs/syntax-sugars.md`), not yet
landed. Demo intentionally uses the sugar form; for the explicit
parametric handler that runs today, see `demos/state_explicit/`.

**Fix path**: m7b #5b sugar lane (post-MVP polish).

#### `demos/stack/main.kai` — m7b #5b sugar (handler-clause scope)

```kai
fn run() : Unit / Stack + Console = {
  with Stack {
    var xs = []
    push(x) -> { ... }
    pop()   -> ...
  }
}
```

**Error**: `expected operation name in handler` at `var xs = []`.

**Cause**: `var` inside a handler clause body would desugar to a
sibling `with State[T](init) as xs`, but that handler-clause-scope
extension to m7b #5b is also not landed. Demo intentionally
aspirational. For the explicit parametric handler that runs today,
see `demos/stack_explicit/`.

**Fix path**: m7b #5b sugar lane (post-MVP polish), same as `state`.

#### `demos/toquefama/main.kai` — tuples (REJECTED in m8.5)

```kai
fn count_toques(guess: [Int], target: [Int]) : Int = match (guess, target) {
  ([], _)                  -> 0
  (_, [])                  -> 0
  ([g, ...gs], [t, ...ts]) -> { ... }
}
```

**Error**: `expected ')' after expression` at the parenthesised
match scrutinee `(guess, target)`.

**Cause**: tuple match expressions. m8.5 (2026-04-27) measured a
parser-combinator suite and **rejected tuples** as a second product
form (n=7, generic-record baseline beat tuples on both LOC and
signature length). The tuple syntax is not coming back.

**Fix path**: rewrite the demo to either `match a, b { PatA, PatB ->
... }` (m7d §27 multi-arg match sugar, post-m14 v1.A) or wrap inputs
in a `Pair[a, b]` record. Demo migration, not language change.

### Pre-existing — feature deferred

These demos exercise stdlib or language pieces that are deferred to
known follow-up lanes.

#### `demos/forth/main.kai` — `!` postfix on `Option` (demo not updated)

```kai
fn tokenise(s: String) : [Token] / Fail = {
  s |> string_split(" ") |> map((w) => match w {
    n -> TNum(string_to_int(n))   # string_to_int returns Option[Int]
    ...
  })
}
```

**Error**:

```
type mismatch in function call
expected: (Int) -> Token
found:    (Option[Int]) -> ?t7 / ?e3
```

**Cause**: `string_to_int` returns `Option[Int]`. The demo passes the
`Option` directly to `TNum(_)` which takes `Int`. The intent is
`TNum(string_to_int(n)!)` — propagate the `None` to the enclosing
`Fail` handler via `!` postfix.

`!` postfix on `Option` IS landed (m7e §13, v0.1.0). Demo just
hasn't been updated to use it.

**Fix path**: 5-min demo edit. Replace `TNum(string_to_int(n))` with
`TNum(string_to_int(n)!)`. No language change needed.

#### `demos/mini_ledger/main.kai` — regex anchors

```kai
type AccountId = String where matches /^acc_[a-zA-Z0-9]{8}$/
```

**Error**: `unexpected character '$'` at the closing anchor.

**Cause**: the regex stdlib (lane B, 2026-04-28) shipped a Thompson
NFA matcher but does not yet parse the `^` / `$` start/end anchors.
Tracked as m12.6.x #7 in `docs/m12-6x-followup.md`.

**Fix path**: extend `stdlib/regexp.kai`'s pattern parser to accept
`^` (start-of-input) and `$` (end-of-input), threading them through
the NFA construction as zero-width transitions. ~0.5d lane.

#### `demos/spiral/main.kai` — module path + legacy `println`

```kai
import loop

fn fill(grid: Array[Int], dim: Int) : Unit / Mutable = {
  ...
  if c >= dim { println(acc) }
}
```

**Errors** (in order, with full prelude chain):

1. `cannot open module 'loop' (tried demos/spiral/loop.kai)` —
   module path resolution.
2. `undefined name 'println'` — bare `println` does not resolve to
   the `Stdout` effect post-m12.8 Phase 4b atomic-effects split.

**Cause**:

1. The `import loop` resolver looks under the demo's directory, not
   `stdlib/`. m6.2 v1 ships the `--path stdlib` flag for this; the
   `demos/Makefile` passes it but the bin/kai wrapper does not by
   default.
2. `println` is a legacy bare builtin (pre-m7a). After m12.8 Phase 4b
   the canonical surface is `Stdout.println(s)` (or `println(s)`
   with `use Stdout` in scope). The demo predates that.

**Fix path**: dual demo edit — add `use Stdout` at file top and
either keep `import loop` (with `--path` setup) or inline the loop
combinator. ~10 min.

### Categorization summary

| Demo | Category | Effort | Owner |
|---|---|---|---|
| `state` | aspirational m7b #5b | post-MVP | wait for sugar lane |
| `stack` | aspirational m7b #5b | post-MVP | wait for sugar lane |
| `toquefama` | aspirational tuples (REJECTED) | demo migration | rewrite to `Pair` or multi-arg match |
| `forth` | demo not updated to use `!` postfix | 5 min | demo edit |
| `mini_ledger` | regex anchors not parsed | 0.5d | m12.6.x #7 lane |
| `spiral` | dual: module path + legacy `println` | 10 min | demo edit |

**Cheapest wins**: `forth` (5 min) and `spiral` (10 min) are demo
edits that flip 2 more demos to OK. `mini_ledger` waits for the
regex anchor lane.

`state`, `stack`, `toquefama` stay failing as honest reminders of
deferred features — fixing them either changes the demo's intent
(state / stack) or rewrites it for a feature that will not ship
(toquefama).

### Demos baseline policy

`demos/baseline.txt` records the minimum count of OK+PASS demos
that must keep passing on every commit. Editing the baseline is
allowed only when:

- A demo legitimately becomes aspirational (its feature was retired
  or deferred), at which point the baseline drops by 1.
- A demo that was previously failing flips to OK, at which point
  the baseline rises by 1.

Demos in this section with category "aspirational" or "feature
deferred" are NOT counted in the baseline; they fail by design
until their upstream lane lands.
