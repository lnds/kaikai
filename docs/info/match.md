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

Some(x)   None                            # Option
Ok(v)     Err(e)                          # Result
Red       Green       Blue                # nullary variants

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
  whole @ [_, ..._] -> "non-empty len=#{int_to_string(whole.length())}"
  []                -> "empty"
}

fn main() : Unit / Stdout = {
  Stdout.print(classify(0))
  Stdout.print(classify(0 - 7))
  Stdout.print(classify(8))
  Stdout.print(classify(9))
  match first([1, 2, 3]) {
    Some(n) -> Stdout.print("first=#{int_to_string(n)}")
    None    -> Stdout.print("empty")
  }
  Stdout.print(int_to_string(area_at_origin(Point { x: 3, y: 4 })))
  Stdout.print(list_info([1, 2, 3]))
}
```

## Exhaustiveness

Missing arms surface as `non-exhaustive match: missing <pattern>`.
The checker reports a specific witness pattern, not a generic error.
Use `_` only when you genuinely want a catch-all; do not use it to
silence the checker.

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

`kai info syntax`, `kai info effects` (for `Fail` in handlers),
`docs/grammar.md` §match
