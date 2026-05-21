LOOP(7)                         kaikai                         LOOP(7)

NAME
  loop — control flow: while, until, for, if_then_else

SYNOPSIS
  if cond { then_block }
  if cond { then_block } else { else_block }
  while { cond } { body }
  until { cond } { body }

  # iteration over a list/range:
  xs | (x => body_expr)                       # map-pipe + lambda
  xs |> foreach(f)                            # for-effect iteration
  xs |> each(f)                               # prelude alias of foreach

DESCRIPTION
  `if` and `match` are language built-ins. `while` and `until` are
  STDLIB FUNCTIONS that take trailing lambdas (the double-trailing-
  lambda sugar). They are not keywords.

  Because they are functions, they obey the effect row of their body:
  a `while { ... } { body that uses Stdout }` itself has `Stdout` in
  the row.

  kaikai has NO `for x in xs` form. Iteration is via a map-pipe with
  a lambda (`xs | (x => body)`) or, for side effects only, via
  `foreach` / `each` (the prelude alias). The `for` keyword is
  reserved exclusively for the `impl Proto for T` declaration form.

  There is no `break` or `continue`. Loops terminate when their
  predicate becomes false (`while`/`until`) or the input is exhausted
  (pipes / `foreach`). To early-exit, restructure with `find`,
  `take_while`, or tail recursion.

EXAMPLES

  # while + var (sugar for State[T] cell).
  # Reading a `var` requires the `@` prefix; bare reads are rejected.
  fn count_up(n: Int) : Int / Stdout = {
    var i = 0
    while { @i < n } {
      Stdout.print("#{@i}")
      i := @i + 1
    }
    @i
  }

  # iterating a range for side effects:
  [1..10] |> each((i) => Stdout.print("#{i}"))

  # `if` is an expression — both branches must type:
  let label = if n > 0 { "pos" } else { "non-pos" }

  # tail recursion is the idiomatic loop for non-trivial state:
  fn sum_loop(xs: [Int], acc: Int) : Int = match xs {
    []           -> acc
    [x, ...rest] -> sum_loop(rest, acc + x)
  }

PIPE-FOR-EACH
  Iteration over a list to a side effect is conventionally written
  with `|` to an each-like fn:

    [1..10] | (i => Stdout.print("#{i}"))

  or with the stdlib `each` helper:

    [1..10] |> each(println)

TAIL CALLS
  Self-tail calls are MANDATORY-OPTIMISED (Tier 1 #2). A function
  whose final expression is a call to itself uses constant stack.
  Non-self tail calls do not generally get TCO.

NOT IN KAIKAI
  - `break` / `continue` statements.
  - C-style `for (init; cond; step)`. Use `[start..end..step] |> each(...)`.
  - `for x in xs { ... }`. There is no for-in loop. Use a pipe.
  - `do { ... } while (cond)`. Use `var x = ...; while { cond } { ... }`.
  - List comprehensions `[x*2 for x in xs]`. Use `xs | (x => x * 2)`.
  - `goto`.
  - `return`. The last expression of a block is the value.

SEE ALSO
  kai info syntax, kai info pipes, kai info match
