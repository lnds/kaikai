# pipes

Apply (`|>`), map (`|`), flat-map (`||`), filter (`|?`) — four pipe
operators, each with one intent.

## Description

`|>` is plain application — no dispatch, no type lookup. It is
syntactic sugar so `x |> f(a, b)` reads like `x.f(a, b)` would in
an OO language but desugars to `f(x, a, b)`.

`|`, `||`, and `|?` DISPATCH BY CONVENTION on the LHS's head type.
Any type `T` that exports `pub fn map / flat_map / filter` with the
canonical signatures participates automatically — no annotation, no
protocol, no compiler change required.

## Canonical signatures

```text
pub fn map      (xs: T[A], f: (A) -> B / e)        : T[B] / e
pub fn flat_map (xs: T[A], f: (A) -> T[B] / e)     : T[B] / e
pub fn filter   (xs: T[A], p: (A) -> Bool / e)     : T[A] / e
```

## Examples

```kaikai
fn double(x: Int) : Int = x * 2
fn gt1(x: Int) : Bool = x > 1
fn single(x: Int) : [Int] = [x, x]
fn sub(a: Int, b: Int) : Int = a - b

fn main() : Unit / Stdout = {
  let a = 5 |> double                          # |> apply
  let b = [1, 2, 3] | double                   # | map
  let c = [1, 2] || single                     # || flat-map
  let d = [1, 2, 3] |? gt1                     # |? filter
  let e = 5 |> sub(7)                          # |> with extra args
                                               # → sub(5, 7) = -2
  let f = 5 |> sub(7, _)                       # placeholder `_`
                                               # → sub(7, 5) = 2
  Stdout.print("a=#{int_to_string(a)} f=#{int_to_string(f)}")
  Stdout.print("b len=#{int_to_string(b.length())} c len=#{int_to_string(c.length())} d len=#{int_to_string(d.length())} e=#{int_to_string(e)}")

  # |> into an effect op needs an explicit lambda — capability ops
  # (`Stdout.print`, etc.) are NOT first-class values you can bare-
  # reference:
  "via pipe" |> (s => Stdout.print(s))

  # Composing pipes (left-assoc):
  let chain = [1..10] |? gt1 | (n => n * 2) |> sum
  Stdout.print("chain=#{int_to_string(chain)}")
}
```

## stdlib types that ride the pipes

These stdlib types export the canonical `map` / `flat_map` / `filter`,
so they dispatch through `|` / `||` / `|?`:

```text
[T]                  element a            (core/list)
Stream[t, e]         element t            (stream)
Map[k, v]            element Pair[k, v]   (collections/map)
HashMap[k, v]        element Pair[k, v]   (collections/hashmap, / Mutable)
Set[a]               element a            (collections/set)
HashSet[t]           element t            (collections/hashset, / Mutable)
Stack[a]             element a            (collections/stack)
Queue[a]             element a            (collections/queue)
```

`Set` / `HashSet` collapse output collisions; `Map` / `HashMap`
collapse duplicate output keys. The mutable `HashMap` / `HashSet`
combinators carry `/ Mutable`.

`Option` and `Result` deliberately do NOT ride the pipes — use `!`
for propagation (`x!`) or `and_then` for explicit chaining.

Non-List head dispatch is gated on edition `hanga-roa` or later.

## Precedence

Pipes are at the bottom of the precedence table (level 10, looser
than `and` / `or`, which are looser than comparisons, which are
looser than arithmetic). All pipes associate LEFT. So `a + b |> f`
parses as `f(a + b)`, and `xs | g | h` parses as `(xs | g) | h`.

Comparisons are NON-ASSOCIATIVE: `a < b < c` is a syntax error
(use `a < b and b < c`).

## NOT IN KAIKAI

- `|>` from F# meaning the same thing — close but in F# it is
  `x |> f` ≡ `f x` (no extra args). kaikai's `|>` lets you write
  additional args after `f`.
- Reverse pipe `<|`. Not in kaikai.
- Function composition operator. Use a lambda.
- Operator sections `(+ 1)` to feed into pipes. Use `(x => x + 1)`.
- `||` as boolean OR (C/JS/Java). In kaikai `||` is the flat-map
  pipe. Boolean OR is the keyword `or`. AND is `and`. NOT is `not`.

## See also

`kai info syntax`, `kai info protocols`, `docs/protocols.md`
