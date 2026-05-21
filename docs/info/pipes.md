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

  # `|>` with a stdlib fn:
  "hello" |> string_length                 # → 5
  42 |> int_to_string                      # → "42"

  # Composing pipes:
  [1..10] |? is_prime | int_to_string |> list.intersperse(", ")

PRECEDENCE
  Pipes are LEFT-ASSOCIATIVE. They bind looser than arithmetic and
  comparisons. When in doubt, parenthesise.

NOT IN KAIKAI
  - `|>` from F# meaning the same thing — close but in F# it is
    `x |> f` ≡ `f x` (no extra args). kaikai's `|>` lets you write
    additional args after `f`.
  - Reverse pipe `<|`. Not in kaikai.
  - Function composition operator. Use a lambda.
  - Operator sections `(+ 1)` to feed into pipes. Use `(x => x + 1)`.

SEE ALSO
  kai info syntax, kai info protocols, docs/protocols.md
