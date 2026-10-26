# pipes

Apply (`|>`), map (`|`), flat-map (`||`), filter (`|?`), each (`|!`) —
five pipe operators, each with one intent.

## Description

`|>` is plain application — no dispatch, no type lookup. It is
syntactic sugar: `x |> f(a, b)` desugars to `f(x, a, b)`. kaikai has no
methods, so this is how a call reads left to right.

`|`, `||`, `|?` and `|!` DISPATCH BY CONVENTION on the LHS's head
type. Any type `T` that exports `pub fn map / flat_map / filter / each`
with the canonical signatures participates automatically — no
annotation, no protocol, no compiler change required. A head whose
owner exports no such function is an error; there is no fallback.

## Canonical signatures

```text
pub fn map      (xs: T[A], f: (A) -> B / e)        : T[B] / e
pub fn flat_map (xs: T[A], f: (A) -> T[B] / e)     : T[B] / e
pub fn filter   (xs: T[A], p: (A) -> Bool / e)     : T[A] / e
pub fn each     (xs: T[A], f: (A) -> Unit / e)     : Unit / e
```

`each` runs `f` for its effect on every element, in the order `map`
visits them, and is what `|!` calls. Every type below exports it (`[T]`
through the core `each`, `Array[T]` through `array.each`, which is the only pipe function `Array` has: `|`, `||` and `|?` over an Array are errors, so pipe over `array.to_list(a)`); the mutable `HashMap` / `HashSet` carry
`/ Mutable`.

## `|!` — for each element, for its effect

`xs |! f` calls the head owner's `each(xs, f)`. The stage must return
`Unit`; this is checked at the pipe, so a stage returning a value is an
error that points at the pipe, not a mismatch inside `each`. A stage
returning `Nothing` is fine. The pipe's type is `Unit`, and the stage's
effects flow out to the caller. Because it returns `Unit`, `|!` ends a
pipeline: any pipe after it is an error.

```kaikai
import stream

fn is_leap(y: Int) : Bool = y % 4 == 0 and (y % 100 != 0 or y % 400 == 0)

fn main() : Unit / Stdout = {
  stream.from_list([1, 2, 3]) |! println("#{.}")     # a lazy stream runs
  [1900, 1994, 2000, 2024] |! println("#{.}: #{is_leap(.)}")
}
```

Use `|!`, not `|`, to iterate for effect: `|` builds a new collection,
and over a lazy `Stream` it runs nothing until something consumes the
result. `xs |> each(f)` resolves `each` by name, so with `import
stream` in scope it reaches `stream.each` even for a list; `|!` picks
`each` by the head type.

```kaikai-neg
fn double(x: Int) : Int = x * 2

fn main() : Unit = [1, 2, 3] |! double         # error: stage returns `Int`
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
  Stdout.print("a=#{a} f=#{f}")
  Stdout.print("b len=#{b |> length()} c len=#{c |> length()} d len=#{d |> length()} e=#{e}")

  # |> into an effect op needs an explicit lambda — capability ops
  # (`Stdout.print`, etc.) are NOT first-class values you can bare-
  # reference:
  "via pipe" |> (s => Stdout.print(s))

  # |! runs a stage for its effect on every element:
  [1, 2] |! (n => Stdout.print("n=#{n}"))

  # Composing pipes (left-assoc):
  let chain = [1..10] |? gt1 | (n => n * 2) |> sum
  Stdout.print("chain=#{chain}")
}
```

## `.` in a stage

The right side of `|`, `||`, `|?` or `|!` is a stage, and the stage owns
every `.` in it: a bare `.`, a `.` with a postfix chain (`.name`,
`.a.b`, `.f(v)`), a `.` in a call argument, and a `.` inside a `#{...}`
body. All of them are the same element.

```kaikai
type Person = { name: String, age: Int }

fn add(a: Int, b: Int) : Int = a + b

fn main() : Unit / Stdout = {
  let people = [Person { name: "ana", age: 30 }, Person { name: "bo", age: 41 }]
  people |! println("#{.name} is #{.age}")
  let next = people | add(.age, 1)             # [31, 42]
  let pairs = [1, 2] || [., . * 10]            # [1, 10, 2, 20]
  let grown = people |? .age > 35 | .name      # ["bo"]
  println("#{next} #{pairs} #{grown}")
}
```

- The closest stage owns a `.`. An explicit lambda or a handler is a
  barrier.
- `|>` is not a stage: it keeps `_`, and a `.` in its call is the
  innermost-argument lambda, so `xs |> each(println(.))` hands
  `println` the identity function. Write `xs |! println(.)`.
- Two nested stages that both use `.` are an error. Name one element
  with `(x) => ...`.
- Outside any stage, `.` inside `#{...}` is an error.

```kaikai-neg
fn main() : Unit / Stdout = {
  let n = 3
  println("n=#{n} #{.}")                       # error: no stage
}
```

## stdlib types that ride the pipes

These stdlib types export the canonical `map` / `flat_map` / `filter`
/ `each`, so they dispatch through `|` / `||` / `|?` / `|!`:

```text
[T]                  element a            (core/list)
Option[a]            the Some payload     (core/option)
Result[a, e]         the Ok payload       (core/result)
Stream[t, e]         element t            (stream)
Gen[t, e]            element t            (gen)
Seq[t]               element t            (seq, pure-only functions)
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

`Option` rides all four pipes: `|` maps `Some`, `||` is `and_then`,
`|?` keeps a `Some` that passes the predicate, and `|!` runs the stage
only on `Some`. `None` passes through untouched. `Result` rides `|`,
`||` and `|!` over `Ok`; `Err` passes through untouched. `|?` on a
`Result` is an error: a rejected `Ok` has no error to carry.

```kaikai
fn inc(x: Int) : Int = x + 1
fn half(x: Int) : Option[Int] = if x % 2 == 0 { Some(x / 2) } else { None }
fn big(x: Int) : Bool = x > 5
fn show_it(x: Int) : Unit / Stdout = Stdout.print("got #{x}")

fn main() : Unit / Stdout = {
  Stdout.print(show(Some(4) | inc))            # Some(5)
  Stdout.print(show(Some(3) || half))          # None
  Stdout.print(show(Some(9) |? big))           # Some(9)
  Stdout.print(show(Ok(4) | inc))              # Ok(5)
  Some(4) |! show_it                           # got 4; None prints nothing
  Stdout.print(show((Some(4) | inc) ?? 0))     # 5
}
```

`??` binds tighter than the pipes, so `opt | f ?? 0` reads
`opt | (f ?? 0)`: parenthesise the pipe, `(opt | f) ?? 0`.

Non-List head dispatch is gated on edition `hanga-roa` or later.

## Precedence

Pipes are at the bottom of the precedence table (looser
than `and` / `or`, which are looser than comparisons, which are
looser than arithmetic). All pipes associate LEFT. So `a + b |> f`
parses as `f(a + b)`, and `xs | g | h` parses as `(xs | g) | h`.

Comparisons are NON-ASSOCIATIVE: `a < b < c` is a syntax error
(use `a < b and b < c`).

With named stages that grouping is invisible. With LAMBDA stages it
decides scope: a lambda stage does NOT open a scope for the stages
that follow it, so the next lambda is its SIBLING, not its body.

```kaikai-neg
fn grid() : [[Int]] = [0..2] | (r) => [0..2] | (c) => r * 10 + c
#                                                     ^ cannot find `r`
```

`(c) => …` is a stage of the OUTER pipe, so `r` was never in scope.
The error's help says so: it names the lambda the pipe ended and shows
the parenthesised body. Parenthesise to nest the inner pipe inside the
outer lambda:

```kaikai-snippet
[0..2] | (r) => ([0..2] | (c) => r * 10 + c)
# [[0, 1, 2], [10, 11, 12], [20, 21, 22]]
```

### Lambda bodies inside parentheses and arguments

The rule above is for a BARE lambda stage. A lambda inside parentheses,
or passed as an argument, has a body that extends to the closing `)` or
`,`, across `|>` and the other pipes:

```text
xs | (o => o |> unwrap_or(0))                    # body: o |> unwrap_or(0)
foldl(xs, Ok([]), (done, x) => done |> and_then(ys => inserted(x, ys)))
[1..10] |? gt1 | (n => n * 2) |> sum             # `)` closes the lambda first
```

So `(h => h |> twice)` is the lambda `h => (h |> twice)`, never
`twice(h => h)`. A lambda that does not start an argument or a group
still ends at the next pipe.

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
