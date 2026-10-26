# loop

Control flow — `if`, `while`, `until`, and iteration via pipes.

## Description

`if` and `match` are language built-ins. `while` and `until` are
STDLIB FUNCTIONS that take trailing lambdas (the double-trailing-
lambda sugar). They are not keywords.

Because they are functions, they obey the effect row of their body:
a `while { ... } { body that uses Stdout }` itself has `Stdout` in
the row.

kaikai has NO `for x in xs` form. Iterate with the pipes: `xs | f`
to transform, `xs |! f` to run an effect per element. Use
`while`/`until` or tail recursion when the loop carries state. The
`for` keyword is reserved exclusively for the `impl Proto for T`
declaration form.

There is no `break` or `continue`. Loops terminate when their
predicate becomes false (`while` / `until`) or the input is exhausted
(pipes). To early-exit, restructure with `find`,
`take_while`, or tail recursion.

## Examples

```kaikai
import loop

fn count_up(n: Int) : Int / Stdout = {
  var i := 0
  while { i < n } {                             # naked cell read
    Stdout.print("#{i}")
    i := i + 1
  }
  i
}

fn sum_loop(xs: [Int], acc: Int) : Int = match xs {
  []           -> acc
  [x, ...rest] -> sum_loop(rest, acc + x)
}

fn main() : Int / Stdout = {
  let _ = count_up(3)

  # iterating a range for side effects:
  [1..3] |! Stdout.print("range #{.}")

  # `if` is an expression — both branches must type:
  let n = 5
  let label = if n > 0 { "pos" } else { "non-pos" }
  Stdout.print(label)

  Stdout.print(int_to_string(sum_loop([1, 2, 3, 4, 5], 0)))
  0
}
```

`while`, `until`, `repeat`, and `forever` live in the `loop` stdlib
package — `import loop` at the top of the file.

## `repeat` and `forever`

```kaikai
import loop

fn maybe_done() : Bool = false                 # placeholder predicate

fn main() : Unit / Stdout = {
  repeat(3) {                                  # `body` runs n times
    Stdout.print("hi")
  }

  # `forever` returns `Nothing`: the type system knows there is no
  # normal-completion path. The only exits are `Cancel` (cooperative
  # cancellation) or an effect op returning `Nothing`. In a real
  # program the body would call `Cancel.raise()`.
  if maybe_done() {
    forever {
      Stdout.print("tick")
    }
  }
}
```

Signatures (all four are row-polymorphic over a single effect `e`):

```text
while  : (() -> Bool / e, () -> Unit / e) -> Unit    / e
until  : (() -> Bool / e, () -> Unit / e) -> Unit    / e
repeat : (Int,            () -> Unit / e) -> Unit    / e
forever:                  (() -> Unit / e) -> Nothing / e
```

`repeat(n, body)` with `n <= 0` is a no-op. `forever`'s body must
end the program by raising or by being cancelled — there is no
graceful return.

## Iterating for effect

End the pipeline with `|!`: it runs the stage for each element and
returns `Unit`, so it closes a `Unit` body.

```kaikai
fn main() : Unit / Stdout = {
  [1..5] |? (. % 2 == 1) |! Stdout.print("odd #{.}")
}
```

Not `xs | f |> ignore` (it builds a list only to drop it) and not
`xs |> each(f)` (it names the iteration fn instead of letting the
head type pick its own). The functions `each` and `foreach` exist,
but `|!` is the idiom; see `kai info idiomatic`.

## Tail calls

Self-tail calls are MANDATORY-OPTIMISED (Tier 1 #2). A function
whose final expression is a call to itself uses constant stack.

Mutual tail recursion gets the same guarantee: a cycle of functions
calling each other in tail position — the `is_even`/`is_odd` pair, or
any longer cycle — runs in constant stack on both backends. The group
is fused into one self-recursive function, so the cycle becomes the
same loop a self-tail call compiles to.

The cycle's members have to agree on the return type and the effect
row, and declare the same type parameters (same kinds, same bounds);
their parameter lists may differ. A cycle whose members disagree on
any of those still consumes a frame per hop.

A tail call that is not part of a cycle — `f` calling `g` where `g`
never leads back to `f` — is an ordinary call and keeps its frame.

## Performance

`while`/`until`/`repeat` written with literal blocks compile to inline
loops on both backends: the lambdas never become runtime closures, and
a local scalar `var` (Int/Real/Bool) mutated by the loop lowers to a
plain register, not a heap cell or a dispatched `State` handler. A
tight numeric `while`+`var` loop runs at the same speed as the
equivalent self-tail-recursive function, so pick whichever form reads
better.

The inline lowering applies to the literal trailing-lambda form. A
combinator name that is shadowed, or a loop whose pred/body arrive as
closure VALUES, keeps the ordinary closure-calling combinator.

## NOT IN KAIKAI

- `break` / `continue` statements.
- C-style `for (init; cond; step)`. Use `[start, next..end] |! ...`.
- `for x in xs { ... }`. No for-in loop. Use a pipe.
- `do { ... } while (cond)`. Use `var x := ...; while { cond } { ... }`.
- List comprehensions `[x*2 for x in xs]`. Use `xs | (x => x * 2)`.
- `goto`.
- `return`. The last expression of a block is the value.

## See also

`kai info syntax`, `kai info pipes`, `kai info match`
