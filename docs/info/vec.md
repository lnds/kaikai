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

| op                   | type                                  | cost |
|----------------------|---------------------------------------|------|
| `vec.make(n, init)`  | `(Int, a) : Vec[a]`                   | O(n) |
| `vec.empty()`        | `() : Vec[a]`                         | O(1) |
| `vec.from_list(xs)`  | `([a]) : Vec[a]`                      | O(n) |
| `vec.length(v)`      | `(Vec[a]) : Int`                      | O(1) |
| `vec.is_empty(v)`    | `(Vec[a]) : Bool`                     | O(1) |
| `vec.get(v, i)`      | `(Vec[a], Int) : a`                   | O(1) |
| `vec.set(v, i, x)`   | `(Vec[a], Int, a) : Vec[a]`           | O(1) unique / O(n) shared |
| `vec.push(v, x)`     | `(Vec[a], a) : Vec[a]`                | amortised O(1) unique / O(n) shared |
| `vec.map(v, f)`      | `(Vec[a], (a) -> b / e) : Vec[b] / e` | O(n) |
| `vec.foldl(v, z, f)` | `(Vec[a], r, (r, a) -> r / e) : r / e`| O(n) |
| `vec.to_list(v)`     | `(Vec[a]) : [a]`                      | O(n) |

`get` and `set` trap on an out-of-range index, like `Array`.

## Notes

- There is no `Vec` literal yet: `[1, 2, 3]` is always the cons list.
  Build with `vec.make` / `vec.from_list` / `vec.push`.
- Writes copy when the vector is shared. To stay on the in-place fast
  path, thread the vector linearly — pass it on, don't hold a second
  live binding across a write.
- Structural equality and slices (`[h, ...t]` views) are not wired to
  the surface yet.

## See also

- `kai info syntax` — the one-page quickref
- `kai info effects` — why `Vec` ops carry no row entry while
  `Array` writes ride `Mutable`
