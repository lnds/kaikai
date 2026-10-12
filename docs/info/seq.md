# seq

`Seq[t]` — the lazy sequence: pure, shared, possibly endless. Written
with `lazy [...]`.

## Description

A `Seq[t]` is a chain of cells. A cell holds a suspended computation
until it is read; reading it yields either the end of the sequence or
an element and the next cell. A cell is evaluated at most once: every
holder of the sequence sees the same computed prefix.

Because a cell runs when it is read, and possibly never, everything
that builds a `Seq` is pure. An effectful producer is a generator
(`gen.generate`, see *Lazy carriers* below).

`Seq` lives in the `seq` module, and a file that writes `lazy [...]`
imports it — `import seq`, `import seq as s`, or a selective import
all serve. Without the import the literal is an error naming it.

## The literal

```kaikai
import seq

fn main() : Unit / Stdout = {
  let none : Seq[Int] = lazy []                # no elements
  let abc = lazy [1, 2, 3]                     # one cell per element
  let more = lazy [0, ...abc]                  # a spread is the rest
  let nat = lazy [0..]                         # 0, 1, 2, … without end
  let fives = lazy [0, 5..]                    # 0, 5, 10, …: the step is
                                               #   second - first
  let down = lazy [10, 7..]                    # 10, 7, 4, … descends
  let few = lazy [1..5]                        # 1 to 5
  let odd = lazy [1, 3..9]                     # 1, 3, 5, 7, 9
  Stdout.print(show(to_list(more)))
  Stdout.print(show(to_list(take(nat, 3))))
  Stdout.print(show(to_list(take(fives, 3))))
  Stdout.print(show(to_list(take(down, 3))))
  Stdout.print(show(to_list(few)) ++ show(to_list(odd)) ++ show(to_list(none)))
}
```

`lazy` is a reserved word and is followed by a bracket. The bracket is
a list or a range, with two differences from a list literal:

- **A range may have no end.** `lazy [2..]` and `lazy [0, 5..]` never
  finish. A list has an end, so a bare `[2..]` is an error that spells
  the fix: `write lazy [2..]`.
- **A spread is the rest of the sequence**, so it comes last:
  `lazy [a, b, ...rest]`. `rest` is not evaluated until the cell
  holding it is read — that is what lets a sequence be defined in terms
  of itself.

A range walks `Int` or `Char`, as a list range does, and its start says
which: `lazy ['a'..'e']` is a `Seq[Char]`, by code point, and
`lazy ['a'..]` goes on from `'a'`. A stepped range follows the
list rule: the step is `second - first`, its sign gives the direction,
and a step the two elements fix at zero is a compile-time error.

What the forms stand for:

```text
lazy []            seq.defer(() => SeqEnd)
lazy [a, b]        seq.defer(() => SeqCons(a, seq.defer(() => SeqCons(b, seq.defer(() => SeqEnd)))))
lazy [a, ...e]     seq.defer(() => SeqCons(a, seq.delay(() => e)))
lazy [a..]         seq.from(a)
lazy [a, b..]      seq.from_step(a, b - a)
lazy [a..hi]       seq.range(a, hi, 1)
lazy [a, b..hi]    seq.range(a, hi, b - a)
lazy ['a'..]       seq.map(seq.from(char_to_int('a')), int_to_char)
```

## Purity

Everything inside `lazy [...]` is checked against an empty effect row:
elements, the spread, and range bounds. Divergence, `panic` and `todo!`
are allowed — they are not effects.

```kaikai-neg
import seq

fn main() : Unit / Stdout = {
  let s = lazy [1, { Stdout.print("read"); 2 }]   # error: everything
  Stdout.print(show(to_list(s)))                  #   inside is pure
}
```

A cell cannot read a `Ref` either, even one the enclosing function
created: the read is `Mutable`, and a sequence stored back into the
`Ref` it reads would hold itself. A function a cell calls is pure
wherever it comes from: an unannotated function value is typed pure by
the cell that calls it, and passing an effectful one is then an error
at the call that passes it.

Sinks run in the consumer, so they take effectful functions: `each`
and `foldl` perform whatever their function performs.

## Matching

The list patterns `[]` and `[first, ...rest]` match a `Seq`; matching
reads one cell. Exhaustiveness is checked as for a list: `[]` and
`[first, ...rest]` together, or a catch-all.

```kaikai
import seq

fn sieve(xs: Seq[Int]) : Seq[Int] = match xs {
  []           -> lazy []
  [p, ...rest] -> lazy [p, ...sieve(rest |? (x) => x % p != 0)]
}

fn main() : Unit / Stdout = {
  let primes = sieve(lazy [2..])
  primes |> take_while((p) => p <= 30) |> each((p) => Stdout.print("#{p}"))
  Stdout.print(int_to_string(primes |> nth(99)))
}
```

One read decides one element, so an arm names at most one before the
rest. To read further, match `rest` in the arm's body:

```kaikai
import seq

fn first_two(s: Seq[Int]) : Int = match s {
  []           -> 0
  [a, ...rest] -> match rest {
    []        -> a
    [b, ..._] -> a + b
  }
}

fn main() : Unit / Stdout = Stdout.print(int_to_string(first_two(lazy [3, 4, 5])))
```

## Pipes and functions

`map`, `flat_map` and `filter` carry the canonical signatures, so `|`,
`||` and `|?` dispatch on a `Seq` (`kai info pipes`). Their functions
are pure: a stage is a cell, and a cell performs nothing.

```text
seq.map(xs, f)            Seq[b]     lazy     f : (a) -> b
seq.flat_map(xs, f)       Seq[b]     lazy     f : (a) -> Seq[b]
seq.filter(xs, p)         Seq[t]     lazy     p : (t) -> Bool
seq.take(xs, n)           Seq[t]     lazy
seq.take_while(xs, p)     Seq[t]     lazy
seq.drop(xs, n)           Seq[t]     reads n cells
seq.nth(xs, n)            t          reads n + 1 cells
seq.to_list(xs)           [t]        reads every cell
seq.each(xs, f)           Unit / e   reads every cell; f may perform e
seq.foldl(xs, init, f)    acc / e    reads every cell; f may perform e
seq.from(a)               Seq[Int]   a, a + 1, …
seq.from_step(a, step)    Seq[Int]   a, a + step, …
seq.range(a, hi, step)    Seq[Int]   a, a + step, … up to hi
```

`to_list`, `each` and `foldl` on an endless sequence do not return: cut
it first with `take` or `take_while`.

## Constants

A `const` whose type holds a `Seq` is evaluated once by each fiber that
reads it and kept until that fiber ends. Every read shares its cells,
so the constant may appear in its own initialiser: a sequence defined
in terms of itself.

```kaikai
import seq

fn sum2(a: Seq[Int], b: Seq[Int]) : Seq[Int] = match a {
  []         -> lazy []
  [x, ...xs] -> match b {
    []         -> lazy []
    [y, ...ys] -> lazy [x + y, ...sum2(xs, ys)]
  }
}

const FIBS : Seq[Int] = lazy [0, 1, ...sum2(FIBS, drop(FIBS, 1))]

fn main() : Unit / Stdout = {
  Stdout.print(show(to_list(take(FIBS, 10))))   # [0, 1, 1, 2, 3, 5, 8, 13, 21, 34]
  Stdout.print(show(nth(FIBS, 80)))             # 23416728348467685
}
```

The rule follows the type: a `const` of a record or a sum with a `Seq`
inside is kept the same way, and any other `const` is still evaluated
at each read. The constant holds the cells it has computed for the life
of its fiber, and each fiber builds its own. A constant read while its
own initialiser runs has no value yet: `const LOOP : Seq[Int] =
drop(LOOP, 1)` panics with `constant depends on itself`. Reading it
inside a `...spread` or a stage is fine, since that runs later.

## Lazy carriers

Three types defer work, one intent each:

```text
Stream[t, e]   push-based; owns its resource; reruns from the start
               when consumed again                       (import stream)
Gen[t, e]      effectful; pulled once                    (import gen)
Seq[t]         pure; shared; evaluated at most once      (import seq)
```

## Notes

- **Sharing goes through a name.** A sequence bound with `let`, passed
  as an argument, returned from a function or named by a `const` is one
  chain of cells, and every reader sees what the others computed. A
  function that defines a sequence through a call to itself builds new
  cells per call.
- Holding the first cell keeps every cell computed after it. Walk a
  long sequence through a name that is not used afterwards, and the
  cells behind the walk are freed as it goes.
- Reading through stacked stages uses stack in proportion to their
  number: the sieve's thousandth prime sits behind a thousand filters.
  The main fiber's stack holds that; a spawned fiber's is small and
  holds a few hundred.
- A `Seq` stays on the fiber that made it. Send `to_list` of the part
  another fiber needs.
- `import seq` opens `map`, `filter`, `take`, `drop`, `each`, `foldl`
  and the rest bare, ahead of the core's list functions of the same
  names: `xs |> take(2)` on a list then names the sequence's `take`. A
  file that works with both binds the module (`import seq as sq`) or
  picks from it (`import seq.{Seq, to_list}`); the literal and the
  patterns need no name.
- `lazy` is a reserved word: it cannot name a binding, a function or a
  field.

## See also

- `kai info syntax` — the one-page quickref
- `kai info pipes` — `|`, `||`, `|?` and the types they dispatch on
- `kai info match` — patterns
- `kai info effects` — effect rows and what "pure" means
