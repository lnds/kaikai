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
kai lint --json [<spec>]           # findings as a JSON array
```

`<spec>` is a `.kai` file or a package (`.` / `./<sub>`), the same as
`kai build` / `kai test`.

## Rules

Each lint names itself, so it can be referred to individually (and, in
a later phase, allowed or denied).

### discard_pure_value

A block silently drops the value of any statement that is not its tail.
When that dropped value is **pure and non-Unit**, it is dead code or a
forgotten use:

```kaikai
fn area(w: Int, h: Int) : Int = w * h

fn run() : Int {
  area(3, 4)        # discard_pure_value: the Int result is dropped
  0
}

fn main() : Int = run()
```

The lint fires only on a **provably pure** discard. An effectful discard
is legitimate — the effect row already tracks the effect — so a call
whose type carries a non-empty row is left alone, even when its result
is non-Unit:

```kaikai
effect Logger {
  log(s: String) : Unit
}

fn audit() : Int / Logger {
  Logger.log("checked")
  0
}

fn run() : Int / Logger {
  audit()           # not flagged: audit performs Logger
  0
}

fn main() : Int = 0
```

A value in **tail position** is the block's own result, never a discard,
so it is never flagged.

Precedent: OCaml Warning 10, Rust `#[must_use]`.

### point_free_nudge

A unary lambda whose body is nothing but a field or method chain over
its parameter reads cleaner as a leading-dot section. kaikai has
point-free sections for fields, method calls, and chains (`kai info
syntax` §Point-free sections), so the lambda has an equivalent
shorter form:

```kaikai
type Person = { name: String }

fn names(ps: [Person]) : [String] = ps.map((c) => c.name)
#                                            ^^^^^^^^^^^^^ point_free_nudge: use .name

fn main() : Int = 0
```

The nudge fires only when the section is **equivalent**: the chain is
rooted exactly at the parameter and no method argument depends on it
(the section supplies the receiver, the written arguments follow). A
lambda whose body computes anything more than the access — `(c) => c.age
* 2`, or `(x) => f(x, x)` — is left alone, because no point-free section
expresses it. A lambda the parser already synthesised from a `.field`
section is never re-nudged.

### and_then_to_map_nudge

`o.and_then((x) => Some(expr))` is `o.map((x) => expr)` when the function
**always** wraps its result in `Some` — the bind is doing nothing a map
would not:

```kaikai
fn inc(o: Option[Int]) : Option[Int] = o.and_then((x) => Some(x + 1))
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
  o.and_then((x) => if x > 0 { Some(x) } else { None })

fn main() : Int = 0
```

Both nudges recognise the combinator call whether it is still written as
a pipe / UFCS method or has been rewritten by the typer.

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

- `Some(x) -> Some(expr)` + `None -> None` is `o.map((x) => expr)`.
- `Some(x) -> opt` + `None -> None` is `o.and_then((x) => opt)`.
- `Some(x) -> x` + `None -> dflt` is `o.unwrap_or(dflt)`, and only when
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
arm means the match does real work and is left alone.

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
  "message": "pure value of type Int discarded; bind it or drop the statement"}]
```

## A growing catalog

The linter is a living tool: rules are added incrementally, each its own
name, each firing only when the suggested form is provably equivalent and
exists in the language. A noisy linter is worse than none, so every rule
biases toward false-negatives — when a rewrite is not certainly
equivalent, the rule stays silent.

Today's rules: `discard_pure_value`, `point_free_nudge`,
`and_then_to_map_nudge`, `match_option_to_combinator`, `redundant_if_bool`,
`redundant_match_catchall`, `dead_code_unused_priv`,
`effect_over_declared`, `effect_ffi_without_extern`. More land over time
as new equivalences earn their place.
