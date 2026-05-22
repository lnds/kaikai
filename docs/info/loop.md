# loop

Control flow — `if`, `while`, `until`, and iteration via pipes.

## Description

`if` and `match` are language built-ins. `while` and `until` are
STDLIB FUNCTIONS that take trailing lambdas (the double-trailing-
lambda sugar). They are not keywords.

Because they are functions, they obey the effect row of their body:
a `while { ... } { body that uses Stdout }` itself has `Stdout` in
the row.

kaikai has NO `for x in xs` form. Iterate via a map-pipe with a
lambda (`xs | (x => body)`) or, for side effects only, via `foreach`
/ `each` (the prelude alias). The `for` keyword is reserved
exclusively for the `impl Proto for T` declaration form.

There is no `break` or `continue`. Loops terminate when their
predicate becomes false (`while` / `until`) or the input is exhausted
(pipes / `foreach`). To early-exit, restructure with `find`,
`take_while`, or tail recursion.

## Examples

```kaikai
import loop

fn count_up(n: Int) : Int / Stdout = {
  var i = 0
  while { @i < n } {                            # `var` reads need `@`
    Stdout.print("#{int_to_string(@i)}")
    i := @i + 1
  }
  @i
}

fn sum_loop(xs: [Int], acc: Int) : Int = match xs {
  []           -> acc
  [x, ...rest] -> sum_loop(rest, acc + x)
}

fn main() : Int / Stdout = {
  let _ = count_up(3)

  # iterating a range for side effects:
  [1..3] |> each((i) => Stdout.print("range #{int_to_string(i)}"))

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
  # cancellation) or an effect that unwinds (e.g. `Fail.fail`). In a
  # real program the body would `Cancel.cancel()` or call a Fail op.
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

## Pipe-for-each

Iteration over a list to a side effect is conventionally written
with `|` to an each-like fn:

```kaikai
fn main() : Unit / Stdout = {
  # `|` produces a [Unit]; discard it to keep `main : Unit`.
  let _ = [1..5] | (i => Stdout.print("#{int_to_string(i)}"))
}
```

…or with the prelude `each` helper (auto-loaded, returns `Unit`;
`foreach` is the spelled-out stdlib equivalent in `core/list`):

```kaikai
fn main() : Unit / Stdout = {
  [1..5] |> each((i) => Stdout.print(int_to_string(i)))
}
```

## Tail calls

Self-tail calls are MANDATORY-OPTIMISED (Tier 1 #2). A function
whose final expression is a call to itself uses constant stack.
Non-self tail calls do not generally get TCO.

## NOT IN KAIKAI

- `break` / `continue` statements.
- C-style `for (init; cond; step)`. Use `[start..end..step] |> each(...)`.
- `for x in xs { ... }`. No for-in loop. Use a pipe.
- `do { ... } while (cond)`. Use `var x = ...; while { cond } { ... }`.
- List comprehensions `[x*2 for x in xs]`. Use `xs | (x => x * 2)`.
- `goto`.
- `return`. The last expression of a block is the value.

## See also

`kai info syntax`, `kai info pipes`, `kai info match`
