# vec

`Vec[T]` — the pure value vector: flat contiguous storage with
functional semantics.

## Description

A `Vec[T]` is an immutable *value* over one flat growable buffer.
Every operation is pure — nothing carries an effect, in particular no
`Mutable`. `set` and `push` return the resulting vector; aliasing two
bindings to one `Vec` is never observable.

Under the hood the runtime mutates in place whenever the buffer is
uniquely owned (refcount 1 — the same check behind Perceus
reuse-in-place) and copies on write when shared. Thread a `Vec`
linearly (accumulator style) and a build loop costs one allocation.

Element storage is unboxed when the element shape allows it:

```text
Vec[Int] / Vec[Real] / Vec[Bool] / Vec[Char] / Vec[Byte]
    raw 8-byte payloads, no per-element header

Vec[Point]   (small record, up to 8 all-scalar fields)
    fields inlined at n_fields * 8 bytes per element

anything else (String, variants, nested records, closures)
    boxed fallback — still value semantics via copy-on-write
```

Contrast with `Array[T]`, which stays: Array is the mutable
*reference* — aliased writes are observable and every write demands
`/ Mutable`. Reach for `Vec` when you want a collection that behaves
like a value.

## Literals and pipeline collect

A list literal in a `Vec`-typed position builds the vec directly —
one pre-sized buffer, no cons list. Both the annotation and the
argument position mint; a BARE `[1, 2, 3]` still denotes the cons
list.

```kaikai
fn total(v: Vec[Int]) : Int = vec_get(v, 0) + vec_get(v, 1)

fn main() : Unit / Stdout = {
  let a: Vec[Int] = [10, 20, 30]        # annotation mints
  Stdout.print(int_to_string(total([1, 2])))   # argument position mints
  Stdout.print(int_to_string(vec_length(a)))
}
```

A `Vec`-typed map-pipe chain *collects*: with pure stages and a range
head it writes straight into one buffer sized from the range — no
range spine, no intermediate list. Any other chain (list source,
impure stage) keeps the ordinary pipeline and converts once at the
end.

```kaikai
fn main() : Unit / Stdout = {
  let squares: Vec[Int] = [1..1000] | (k => k * k)   # one allocation
  Stdout.print(int_to_string(vec_get(squares, 999)))
}
```

## Slices — O(1) views

`xs[i]` reads an element; `xs[a..b]` (inclusive, like the range
literal) is an O(1) *view*: offset + length over the shared buffer,
no elements copied. Matching `[h, ...t]` binds `t` as the same view
form, so a head/tail descent walks N elements with a constant number
of allocations.

```kaikai
fn suma(v: Vec[Int], acc: Int) : Int =
  match v {
    []        -> acc
    [h, ...t] -> suma(t, acc + h)      # t: O(1) view, re-sliced in place
  }

fn main() : Unit / Stdout = {
  let v: Vec[Int] = [1, 2, 3, 4]
  Stdout.print(int_to_string(suma(v[1..3], 0)))   # 9
}
```

Slice semantics, by design:

- a live slice **pins** the whole buffer (it holds a reference on it);
- sharing disables in-place: writing the source while a slice is live
  copies, and writing *through* a slice copies its elements out — the
  slice always keeps seeing the bytes it was cut from;
- once the last slice dies, in-place writes on the source resume.

Vec match arms support `[]`, `[a, b, ...rest]` with simple binders,
and a catch-all. Guards and nested element patterns are not supported
on Vec arms — bind the element and match it in the arm body:

```kaikai-neg
fn f(v: Vec[Int]) : Int =
  match v {
    [0, ...t] -> vec_length(t)   # error: literal head on a Vec arm
    _         -> 0
  }
```

## Operations

Construction is by function (`import collections.vec`):

```kaikai
import collections.vec

fn demo() : Int = {
  let v = vec.make(3, 0)              # [0, 0, 0]
  let w = vec.push(vec.set(v, 0, 10), 7)   # [10, 0, 0, 7]
  vec.get(w, 3) + vec.length(w)       # 7 + 4
}

fn main() : Unit / Stdout = {
  Stdout.print(int_to_string(demo()))
}
```

Transforms and conversion:

```kaikai
import collections.vec

fn main() : Unit / Stdout = {
  let v = vec.from_list([1, 2, 3])
  let doubled = vec.map(v, (x) => x * 2)
  let total = vec.foldl(doubled, 0, (acc, x) => acc + x)   # 12
  Stdout.print(int_to_string(total))
  Stdout.print(int_to_string(list_length(vec.to_list(doubled))))
}
```

| op                     | type                                  | cost |
|------------------------|---------------------------------------|------|
| `vec.make(n, init)`    | `(Int, a) : Vec[a]`                   | O(n) |
| `vec.empty()`          | `() : Vec[a]`                         | O(1) |
| `vec.reserve(n)`       | `(Int) : Vec[a]`                      | O(1) |
| `vec.from_list(xs)`    | `([a]) : Vec[a]`                      | O(n) |
| `vec.length(v)`        | `(Vec[a]) : Int`                      | O(1) |
| `vec.is_empty(v)`      | `(Vec[a]) : Bool`                     | O(1) |
| `vec.get(v, i)`        | `(Vec[a], Int) : a`                   | O(1) |
| `vec.slice(v, i, n)`   | `(Vec[a], Int, Int) : Vec[a]`         | O(1) view |
| `vec.set(v, i, x)`     | `(Vec[a], Int, a) : Vec[a]`           | O(1) unique / O(n) shared |
| `vec.push(v, x)`       | `(Vec[a], a) : Vec[a]`                | amortised O(1) unique / O(n) shared |
| `vec.map(v, f)`        | `(Vec[a], (a) -> b / e) : Vec[b] / e` | O(n) |
| `vec.foldl(v, z, f)`   | `(Vec[a], r, (r, a) -> r / e) : r / e`| O(n) |
| `vec.to_list(v)`       | `(Vec[a]) : [a]`                      | O(n) |

`get`, `set` and `slice` trap on an out-of-range index, like `Array`.

## Notes

- The bare literal default is unchanged: `[1, 2, 3]` with no `Vec`
  context is the cons list.
- Writes copy when the vector is shared — including while a slice is
  live. To stay on the in-place fast path, thread the vector linearly
  and let slices die before writing.
- Equality (`==`) is structural, element by element.

## See also

- `kai info syntax` — the one-page quickref
- `kai info pipes` — pipe chains; a `Vec`-typed chain collects
- `kai info effects` — why `Vec` ops carry no row entry while
  `Array` writes ride `Mutable`
