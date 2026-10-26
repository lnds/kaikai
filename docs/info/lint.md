# lint

A Clippy-style linter for suspect-but-valid code, beside the compiler.

## Description

`kai lint` flags code that type-checks but reads like a mistake — the
way Rust's Clippy sits beside `rustc`. The compiler core stays strict
about correctness and silent about style; the linter owns the opinions.

The linter is **opt-in and non-blocking**: warnings only, always exits
0, and never changes what compiles. It reuses the typed AST and effect
rows the compiler already produces, so its checks are type- and
effect-aware — not just a text scan.

```text
kai lint [<spec>]                  # human-readable warnings
kai lint --json [<spec>]           # findings as JSON, one line per root
```

`<spec>` is a `.kai` file or a package (`.` / `./<sub>`), the same as
`kai build` / `kai test`. A package with an entry point is linted as one
unit, from that entry, and the findings cover every module the package
owns — the same scope `kai build` compiles, so a name another file's
import made visible resolves here too. A package without an entry — a
library — is linted module by module, each module its own root and
reported on once.

Findings name the file that declares the code, not the root the lint
started from. They cover the package under lint and no other: a module
imported from the stdlib or from a dependency, directly or through
another, is never reported on. Its findings belong to its own lint.
A lone file is linted with the modules of its package it loads; with no
`kai.toml`, those are the modules it imports from its own directory.

`--json` prints one JSON array of `{file, line, col, rule, severity,
message}` per compilation root, each on its own line (NDJSON): one
line for a file or a package with an entry, one per module for a
library.

## Rules

Each lint names itself, so it can be referred to individually (and, in
a later phase, allowed or denied).

### discard_pure_value

A block silently drops the value of any statement that is not its tail.
When that dropped value has a type other than `Unit` or `Nothing`, it
is a forgotten use — whether or not the call that produced it performs
an effect:

```kaikai
fn area(w: Int, h: Int) : Int = w * h

fn parse(s: String) : Result[Int, String] / Stdout = {
  println(s)
  Ok(s |> length())
}

fn show(n: Int) : Unit / Stdout = println("#{n}")

fn run() : Unit / Stdout = {
  area(3, 4)        # discard_pure_value: the Int is dropped
  parse("42")       # discard_pure_value: the Result is dropped
  [1, 2] | show     # discard_pure_value: the [Unit] is dropped
  println("done")
}

fn main() : Unit / Stdout = run()
```

To drop a value on purpose, say so: close it with `|> ignore` (a core
function) or bind it with `let _ =`. Neither is flagged:

```kaikai
fn parse(s: String) : Result[Int, String] / Stdout = {
  println(s)
  Ok(s |> length())
}

fn show(n: Int) : Unit / Stdout = println("#{n}")

fn main() : Unit / Stdout = {
  let _ = parse("42")
  parse("7") |> ignore
  [1, 2] | show |> ignore
}
```

The lint also stays silent on an indexed write `a[i] := v` (the write is
the point, not the array it returns) and on a call whose result type is
`Nothing`, which never returns.

A value in **tail position** is the block's own result, never a discard,
so it is never flagged.

Precedent: OCaml Warning 10, Rust `#[must_use]`.

### point_free_nudge

A unary lambda whose body is nothing but a field chain over its
parameter reads cleaner as a leading-dot section. kaikai has
point-free sections for fields and field chains (`kai info syntax`
§Point-free sections), so the lambda has an equivalent shorter form:

```kaikai
type Person = { name: String }

fn names(ps: [Person]) : [String] = ps | (c) => c.name
#                                            ^^^^^^^^^^^^^ point_free_nudge: use .name

fn main() : Int = 0
```

The nudge fires only when the section is **equivalent**: the chain is
rooted exactly at the parameter. A lambda whose body computes anything
more than the access — `(c) => c.age * 2`, or `(x) => f(x, x)` — is left
alone, because no point-free section expresses it. A lambda the parser
already synthesised from a `.field`
section is never re-nudged.

### and_then_to_map_nudge

`o || (x) => Some(expr)` is `o | (x) => expr` when the function
**always** wraps its result in `Some` — the bind is doing nothing a map
would not:

```kaikai
fn inc(o: Option[Int]) : Option[Int] = o || (x) => Some(x + 1)
#                                       ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ and_then_to_map_nudge: use map

fn main() : Int = 0
```

Every tail position of the function must construct `Some` — a direct
`Some(...)`, the tail of a block, both branches of an `if`, every arm of
a `match`. If any path can yield `None` (or anything other than `Some`),
the rewrite would drop a short-circuit `.map` cannot express, so the
call is left alone:

```kaikai
# not flagged: the else branch is None, so this is a real bind
fn keep_positive(o: Option[Int]) : Option[Int] =
  o || (x) => if x > 0 { Some(x) } else { None }

fn main() : Int = 0
```

Both nudges recognise the combinator call whether it is still written as
a pipe or has been rewritten by the typer.

### match_option_to_combinator

A manual two-arm `match` over an `Option` that a single combinator
expresses exactly is ceremony around `map` / `and_then` / `unwrap_or`.
Each rewrite is value-for-value identical, including evaluation order:

```kaikai
fn bumped(o: Option[Int]) : Option[Int] = match o {
  Some(x) -> Some(x + 1)   # match_option_to_combinator: use map
  None    -> None
}

fn main() : Int = 0
```

The arm shapes pick the combinator:

- `Some(x) -> Some(expr)` + `None -> None` is `o | (x) => expr`.
- `Some(x) -> opt` + `None -> None` is `o || (x) => opt`.
- `Some(x) -> x` + `None -> dflt` is `o ?? dflt`, and only when
  `dflt` is **trivial** (a literal, a variable, a nullary constructor).
  A computed default is `unwrap_or_else` territory — `unwrap_or` would
  evaluate it eagerly, which the lazy `None` arm does not — so it is
  left alone.

A guard, a destructuring `Some` pattern, a non-`Option` scrutinee, or a
`None` arm that is not literally `None` all stop the nudge.

### redundant_if_bool

An `if`/`else` whose both branches are nothing but the boolean literals
`true` / `false` is the condition itself, or its negation:

```kaikai
fn is_pos(n: Int) : Bool = if n > 0 { true } else { false }
#                          ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ redundant_if_bool: use the condition itself

fn main() : Int = 0
```

`if c { true } else { false }` is `c`; the inverse is `not c`. The lint
fires only when both branches are bare boolean literals (directly, or as
the sole tail of an otherwise-empty block) and **differ**. An `if` with
no else, a branch that computes a boolean (`n > 5`), or two matching
literals are left alone.

### redundant_match_catchall

A `match` with a single unguarded arm whose pattern is `_` or a bare
binding never destructures — it is a direct expression or a `let`:

```kaikai
fn answer(n: Int) : Int = match n {
  _ -> 42                  # redundant_match_catchall: use the expression directly
}

fn main() : Int = 0
```

`match e { _ -> body }` is `body`; `match e { x -> body }` is
`let x = e ... body`. A destructuring pattern, a guard, or more than one
arm means the match does real work and is left alone. Only a `match`
written in the source is judged: a clause-block function (`case n, Up ->
...`) and a multi-scrutinee `match a, b { ... }` need a pattern per
column in every arm, so a column that only binds is not reported.

### list_concat_literal_to_spread

A list literal on the left of `++` builds an intermediate list and then
concatenates it; the spread form emits the cons directly:

```kaikai
fn cons_front(t: [Int]) : [Int] = [1] ++ t
#                                 ^^^^^^^^ list_concat_literal_to_spread: write [1, ...t]

fn main() : Int = 0
```

`[h] ++ t` is `[h, ...t]` and `[a, b] ++ t` is `[a, b, ...t]` — the same
list, minus the intermediate allocation, and the gap widens with the
literal's length (`kai info idiomatic` §Spread over `++` has the
numbers). The lint fires on any **non-empty** list literal on the left,
whatever the right operand.

It does not fire on genuine concatenation (`xs ++ ys`) or a computed
left operand (`f(x) ++ t`) — concatenating two list values is what `++`
is for. Two literal-adjacent shapes are also deliberately out:

- `[] ++ xs` is just `xs` — the runtime shares the right operand, so
  there is no allocation to save; the fix is dropping the concat, not
  a spread.
- `xs ++ [y]` (an append) — no spelling avoids copying `xs`, so the
  lint has no better form to point at.

### non_tail_list_recursion

A function that calls itself on the tail of a list parameter it
destructured, with the call outside tail position, keeps one stack
frame per element. A fiber's stack is small, so a few thousand elements
overflow it:

```kaikai
pub fn join_loop(xs: [String], sep: String) : [String] = match xs {
  []           -> []
  [x, ...rest] -> [sep, x, ...join_loop(rest, sep)]
#                               ^^^^^^^^^ non_tail_list_recursion
}

fn main() : Int = 0
```

The call is flagged when its argument is a rest binder (`...rest`) of a
`match` on one of the function's own parameters and the call is not the
last thing the arm does: an operand (`x + f(rest)`), a pipe stage
(`f(rest) |> g`), a spread after two or more heads, or a record field. Make the call the arm's result and carry the partial result in a
parameter, reversing once at the end if order matters:

```kaikai
pub fn join_acc(xs: [String], sep: String, acc: [String]) : [String] = match xs {
  []           -> list_reverse(acc)
  [x, ...rest] -> join_acc(rest, sep, [x, sep, ...acc])
}

fn main() : Int = 0
```

A cons with one head (`[h, ...f(t)]`) or a constructor whose only
self-call is one slot (`Link(x, f(rest))`) is compiled to a loop when the
function's effect row is empty, so it is not flagged. A row left open
(`/ e`) is flagged: it loops only for the instantiations that perform no
effect, and overflows when the callback does. Recursion on a
tree is depth-bounded and not flagged, nor is a call to a
different function, nor a call whose arguments are not a rest binder.
Only direct self-recursion is tracked; mutual recursion is not.

### dead_code_unused_priv

A private root-file function whose name appears nowhere in the file is
dead code. The compiler warns about neither it, so the linter owns the
check:

```kaikai
fn unused_helper(n: Int) : Int = n * 2   # dead_code_unused_priv

pub fn api(n: Int) : Int = n + 1

fn main() : Int = api(5)
```

The scan is intentionally **non-transitive**: a name mentioned anywhere
— even inside another unused function — counts as used, so mutually
recursive or example-only chains never produce a false positive. `pub`
functions are reachable from other modules and `main` is the entry
point, so both are exempt; only the sound private case fires.

Only a function the **source declares at top level** is a candidate. An
`impl` method and a `#[derive(...)]` impl both become ordinary top-level
functions by the time the lint runs, but neither remedy the message
proposes applies to them — you cannot remove a function the compiler
synthesised, nor export it — so they are exempt:

```kaikai
#[derive(Eq, Show)]
pub type Color = Red | Green | Blue    # not flagged: derive writes the impls

fn main() : Int = if Red == Red { 0 } else { 1 }
```

The exemption is about **provenance, not spelling**: a private function
you write yourself is flagged whatever you name it, including a name
that mimics the compiler's own mangling.

### effect_over_declared

A `pub` signature whose declared effect row carries a label its own body
never demands over-declares it:

```kaikai
pub effect Logger { log(s: String) : Unit }

pub fn pure_one() : Int / Logger = 1   # effect_over_declared: body never uses Logger

fn main() : Int = 0
```

The check is per-function and reuses what the typer already resolved: an
effect referenced as an op (`Logger.log`), carried on any call's stamped
row (so an effect reached through a helper counts as used), or installed
as a handler counts as demanded. Only **closed, monomorphic** rows fire
— an open row (a row variable) is a polymorphic promise the body may
satisfy elsewhere, so it is left alone.

### effect_ffi_without_extern

A specialised face of `effect_over_declared`: a `pub` signature that
declares `/ Ffi` but whose body calls no extern function. It carries its
own lint id and message so it can be allowed / denied on its own:

```kaikai
pub fn fake_ffi(n: Int) : Int / Ffi = n + 1   # effect_ffi_without_extern

fn main() : Int = 0
```

A body that calls an extern function (the call's row carries `Ffi`), and
an `extern "C" fn` declaration itself (its body IS the foreign call),
are both left alone.

## Output

The JSON form mirrors `--diags-json` / `--holes-json`: a single array,
one object per finding, stable schema.

```text
[{"file": "src/main.kai", "line": 4, "col": 3,
  "rule": "discard_pure_value", "severity": "warning",
  "message": "value of type Int discarded; close it with `|> ignore` or bind it with `let _ =`"}]
```

## A growing catalog

The linter is a living tool: rules are added incrementally, each its own
name, each firing only when the suggested form is provably equivalent and
exists in the language. A noisy linter is worse than none, so every rule
biases toward false-negatives — when a rewrite is not certainly
equivalent, the rule stays silent.

Today's rules: `discard_pure_value`, `point_free_nudge`,
`and_then_to_map_nudge`, `match_option_to_combinator`, `redundant_if_bool`,
`redundant_match_catchall`, `list_concat_literal_to_spread`,
`non_tail_list_recursion`, `dead_code_unused_priv`, `effect_over_declared`,
`effect_ffi_without_extern`. More land over time as new equivalences
earn their place.
