# match

Pattern matching with exhaustiveness checking.

## Description

`match` is an expression. Every arm must have the same type. The
checker requires exhaustiveness over the scrutinee's type — a non-
exhaustive match is a compile error, not a runtime panic.

```text
match expr {
  Pattern1            -> arm
  Pattern2 if guard   -> arm
  _                   -> arm
}
```

## Patterns

```text
_                                         # wildcard
x                                         # bind
x: Int                                    # bind + union-variant narrow

42  3.14  "hello"  'c'  true  false       # literals

1..9      'a'..'z'                        # inclusive range (Int/Char,
                                          # literal bounds — no step,
                                          # no expressions)

Some(x)   None                            # Option
Ok(v)     Err(e)                          # Result
Red       Green       Blue                # nullary variants
MAX       m.MAX       Some(m.MAX)         # constants: equal to the
                                          # value; a qualifier picks
                                          # the module's constant

[]                                        # empty list
[x]                                       # one element
[x, y, z]                                 # exactly three
[head, ...tail]                           # head + rest
[a, b, ...rest]                           # two + rest

{ x: 1, y: y }                            # match x == 1, bind y
{ x, y }                                  # punning: bind both
                                          # (no type prefix in pattern)

name @ pattern                            # as-pattern: bind whole
                                          # to `name`, also destructure
```

A record LITERAL in expression position needs the type prefix
(`Point { x: 3, y: 4 }`). Only patterns and `let`-destructure allow
the prefix-less form.

## Examples

```kaikai
fn classify(n: Int) : String = match n {
  0                -> "zero"
  k if k < 0       -> "negative"
  k if k % 2 == 0  -> "even positive"
  _                -> "odd positive"
}

fn first(xs: [Int]) : Option[Int] = match xs {
  []             -> None
  [x, ..._]      -> Some(x)
}

type Point = { x: Int, y: Int }

fn area_at_origin(p: Point) : Int = match p {
  { x: 0, y: 0 } -> 0
  { x, y }       -> x * y
}

fn list_info(xs: [Int]) : String = match xs {
  whole @ [_, ..._] -> "non-empty len=#{whole.length()}"
  []                -> "empty"
}

fn main() : Unit / Stdout = {
  Stdout.print(classify(0))
  Stdout.print(classify(0 - 7))
  Stdout.print(classify(8))
  Stdout.print(classify(9))
  match first([1, 2, 3]) {
    Some(n) -> Stdout.print("first=#{n}")
    None    -> Stdout.print("empty")
  }
  Stdout.print(int_to_string(area_at_origin(Point { x: 3, y: 4 })))
  Stdout.print(list_info([1, 2, 3]))
}
```

## Range patterns

`lo..hi` matches when the scrutinee falls in the inclusive interval.
Both bounds are Int or Char literals — no expressions, no step. A range
arm is exactly a `when lo <= x <= hi` guard, so a catch-all is still
required (an interval never covers all of `Int`). A range may sit at any
depth of a pattern (`Some(1..9)`, `(0..1, y)`, `{ x: 0..2, y }`); its
check runs before the arm's own guard.

```kaikai
fn classify(n: Int) : String = match n {
  0      -> "zero"
  1..9   -> "small"
  10..99 -> "medium"
  _      -> "large"
}

fn grade(c: Char) : String {
  case 'a'..'z' -> "lower"
  case '0'..'9' -> "digit"
  case _        -> "other"
}

fn main() : Unit / Stdout = {
  Stdout.print(classify(5))
  Stdout.print(grade('k'))
}
```

Both bounds must be literals — a variable bound is rejected:

```kaikai-neg
fn f(n: Int, hi: Int) : String = match n {
  1..hi -> "in"
  _     -> "out"
}

fn main() : Unit / Stdout = Stdout.print(f(5, 9))
```

The scrutinee must be numeric — a range over a String is a type error:

```kaikai-neg
fn f(s: String) : String = match s {
  1..9 -> "in"
  _    -> "out"
}

fn main() : Unit / Stdout = Stdout.print(f("hi"))
```

## Exhaustiveness

Missing arms surface as `non-exhaustive match: missing <variant>`, or
`non-exhaustive match: no arm covers <pattern>` when the gap sits inside
a pattern. The checker reports a specific witness pattern, not a generic
error. Use `_` only when you genuinely want a catch-all; do not use it to
silence the checker.

Coverage looks through nested patterns: `Some(0)` covers only part of
`Some`, and `(Red, _)` only part of a pair. A guarded arm covers nothing
on its own, so `Some(x) if x > 0` still needs a `Some(_)` arm after it.
Only `true` and `false` exhaust a literal type: a match over `Int`,
`Char`, `String` or `Real` literals needs a catch-all.

```kaikai
fn sign(o: Option[Int]) : Int = match o {
  Some(x) if x > 0 -> 1
  Some(_)          -> 0
  None             -> 0 - 1
}

fn main() : Unit / Stdout = Stdout.print("#{sign(Some(4))}")
```

## `case`-led fn body

A function whose body is `{ case <pat> (when <guard>)? -> <body> ...
}` desugars into a `match` against the function's parameter(s). The
guard keyword inside `case` arms is `when`, not `if`. Multi-arg form
matches a tuple of up to 4 parameters per arm.

```kaikai
fn classify(n: Int) : String {
  case 0            -> "zero"
  case n when n < 0 -> "neg"
  case _            -> "pos"
}

fn main() : Unit / Stdout = Stdout.print(classify(-3))
```

Useful when the function exists to dispatch on its parameters and an
explicit `= match { ... }` would just add a wrapper.

## NOT IN KAIKAI

- `case x of pat -> ...` (Haskell/Erlang). The `case` keyword exists
  in kaikai but only in a *clause-block fn body* (`fn f(x) { case 0
  -> ...; case _ -> ... }`); it is not a standalone expression.
- `switch / case` (C/JS). Use `match`.
- Or-patterns `A | B -> ...` as a single arm. Write two arms.
- View patterns. Use a guard.
- Fall-through between arms. Arms are independent.

## See also

`kai info syntax`, `kai info effects` (for patterns in handlers),
`docs/grammar.md` §match
