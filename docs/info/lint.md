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

## Output

The JSON form mirrors `--diags-json` / `--holes-json`: a single array,
one object per finding, stable schema.

```text
[{"file": "src/main.kai", "line": 4, "col": 3,
  "rule": "discard_pure_value", "severity": "warning",
  "message": "pure value of type Int discarded; bind it or drop the statement"}]
```

## Not yet

The linter ships three rules today (`discard_pure_value`,
`point_free_nudge`, `and_then_to_map_nudge`). Redundant-pattern checks,
dead-code and effect-row smells are planned as incremental additions,
each its own rule with its own name.
