MATCH(7)                        kaikai                        MATCH(7)

NAME
  match — pattern matching with exhaustiveness checking

SYNOPSIS
  match expr {
    Pattern1            -> arm
    Pattern2 if guard   -> arm
    _                   -> arm
  }

DESCRIPTION
  `match` is an expression. Every arm must have the same type. The
  checker requires exhaustiveness over the scrutinee's type — a non-
  exhaustive match is a compile error, not a runtime panic.

PATTERNS

  Wildcards and bindings
    _                                          # ignore
    x                                          # bind
    x: Int                                     # bind with type assert

  Literals
    42        3.14        "hello"        'c'        true        false

  Variants
    Some(x)       None                              # Option
    Ok(v)         Err(e)                            # Result
    Red           Green           Blue              # nullary

  Lists
    []                                              # empty
    [x]                                             # one element
    [x, y, z]                                       # exactly three
    [head, ...tail]                                 # head + rest
    [a, b, ...rest]                                 # two + rest

  Records (patterns only — no type prefix needed in match)
    { x: 1, y: y }                                  # match x == 1, bind y
    { x, y }                                        # punning: bind both
    # NB: a record LITERAL in expression position needs the type:
    #   `Point { x: 3, y: 4 }`

  Guards
    n if n > 0          -> "pos"
    s if s == "hello"   -> "hi"

EXAMPLES

  fn classify(n: Int) : String = match n {
    0                -> "zero"
    n if n < 0       -> "negative"
    n if n % 2 == 0  -> "even positive"
    _                -> "odd positive"
  }

  fn first(xs: [a]) : Option[a] = match xs {
    []             -> None
    [x, ..._]      -> Some(x)
  }

  fn area(p: Point) : Int = match p {
    {x: 0, y: 0}   -> 0
    {x, y}         -> x * y
  }

EXHAUSTIVENESS
  Missing arms surface as `non-exhaustive match: missing <pattern>`.
  The checker reports a specific witness pattern, not a generic error.
  Use `_` only when you genuinely want a catch-all; do not use it to
  silence the checker.

NOT IN KAIKAI
  - `case x of` (Haskell/Erlang). Use `match expr { ... }`.
  - `switch / case` (C/JS). Use `match`.
  - Or-patterns (`A | B -> ...`). Not in v1; write two arms.
  - View patterns. Use a guard.
  - Fall-through between arms. Arms are independent.

SEE ALSO
  kai info syntax, kai info effects (for Fail in handlers),
  docs/grammar.md §match
