# vec-layout

A flat vector built by repeated `vec.push` from empty and then summed by
a tail-recursive index loop, once over `Vec[Point]` (an inline two-`Int`
record, 16 bytes per element) and once over `Vec[Int]`:

```
sum_x(v, i + 1, n, acc + vec.get(v, i).x)
```

```
./run.sh [N]
```

prints the median CPU time (user+sys) of 15 alternated runs per backend
at `N` elements (default 10M), then each backend's `KAI_TRACE_RC=1`
ledger. It runs with `KAI_THREADS=1`.

What to read from it:

- **Storage is flat.** Peak memory is about `N * 16` bytes for the
  points plus `N * 8` for the ints: no per-element header, no box.
- **The build is in place.** `vec_inplace` equals the number of pushes
  (`2 * N`) and `vec_cow=0`: a linearly threaded vector is never copied,
  only regrown.
- **The loops do no reference counting.** `incref_total=0`: the vector
  is borrowed by every read.

On the native backend an `Int` / `Real` element or record field crosses
the buffer as its bare 8-byte word, so the sum is a guarded load and an
add per element, and a push is a store when the buffer is unique and has
room.
