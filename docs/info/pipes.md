PIPES(7)                        kaikai                        PIPES(7)

NAME
  pipes — apply (|>), map (|), flat-map (||), filter (|?)

SYNOPSIS
  x |> f(args)                    # apply: same as f(x, args)
  xs | g                          # map: same as <head-type>.map(xs, g)
  xs || h                         # flat-map: <head-type>.flat_map(xs, h)
  xs |? p                         # filter: <head-type>.filter(xs, p)

DESCRIPTION
  kaikai ships four pipes, each with a clear intent. They differ in
  what they pass and how they dispatch.

  `|>` is plain application — no dispatch, no type lookup. It is
  syntactic sugar so `x |> f(a, b)` reads like `x.f(a, b)` would in
  an OO language but desugars to `f(x, a, b)`.

  `|`, `||`, and `|?` DISPATCH BY CONVENTION on the LHS's head type.
  Any type `T` that exports `pub fn map / flat_map / filter` with the
  canonical signatures participates automatically — no annotation, no
  protocol, no compiler change required.

CANONICAL SIGNATURES

  pub fn map      (xs: T[A], f: (A) -> B / e)        : T[B] / e
  pub fn flat_map (xs: T[A], f: (A) -> T[B] / e)     : T[B] / e
  pub fn filter   (xs: T[A], p: (A) -> Bool / e)     : T[A] / e

EXAMPLES

  [1, 2, 3] | (x => x * 2)                 # → [2, 4, 6]
  [1, 2, 3] |? (x => x > 1)                # → [2, 3]
  [[1,2],[3]] || (xs => xs)                # → [1, 2, 3]

  # `|>` with a stdlib fn (single-arg, no parens needed):
  "hello" |> string_length                 # → 5
  42 |> int_to_string                      # → "42"

  # `|>` with extra args (LHS lands first by default):
  3 |> max(7)                              # → max(3, 7) → 7

  # `|>` with explicit placeholder `_` to land elsewhere:
  3 |> max(7, _)                           # → max(7, 3) → 7

  # `|>` into an effectful fn:
  "boom" |> Stdout.print                    # row picks up `Stdout`

  # Composing pipes (left-assoc):
  [1..10] |? is_prime | (n => n * 2) |> sum

PRECEDENCE
  Pipes are at the bottom of the precedence table (level 10, looser
  than `and` / `or`, which are looser than comparisons, which are
  looser than arithmetic). All pipes associate LEFT. So
  `a + b |> f` parses as `f(a + b)`, and `xs | g | h` parses as
  `(xs | g) | h`. When in doubt, parenthesise.

  Comparisons are NON-ASSOCIATIVE: `a < b < c` is a syntax error
  (use `a < b and b < c`).

NOT IN KAIKAI
  - `|>` from F# meaning the same thing — close but in F# it is
    `x |> f` ≡ `f x` (no extra args). kaikai's `|>` lets you write
    additional args after `f`.
  - Reverse pipe `<|`. Not in kaikai.
  - Function composition operator. Use a lambda.
  - Operator sections `(+ 1)` to feed into pipes. Use `(x => x + 1)`.
  - `||` as boolean OR (C/JS/Java). In kaikai `||` is the flat-map
    pipe. Boolean OR is the keyword `or`. Boolean AND is `and`
    (not `&&`). NOT is the keyword `not` (not `!`).
  - `|>` chained without parens around a non-call RHS:
    `x |> y |> z` works because each step is a separate pipe;
    but `x |> (y |> z)` is parenthesised application, evaluated
    right-first.

SEE ALSO
  kai info syntax, kai info protocols, docs/protocols.md
