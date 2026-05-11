# Lane retro: BinSerialize over `Array[Byte]` (#487 follow-up to closed PR #485)

## Why

Issue #485 reported `BinSerialize.from_bytes` being O(N²) for 40 KB payloads
(19 seconds for a 500-node `#derive(BinSerialize)` round-trip). The
v1 buffer carrier was `[Byte]` — a cons list. Each `bin_byte_at(buf, pos)`
read walked the list to position `pos`, so reading N bytes scans
`0 + 1 + 2 + ... + N - 1 = O(N²)` cells.

PR #485 attempted to fix this inside the runtime (add a "list index
cache" or similar) and was closed: too invasive, conflated with
Perceus RC concerns, and ultimately the wrong layer. The kaikai
runtime already ships `Array[T]`, a contiguous mutable carrier with
O(1) `array_get` / `array_set` / `array_length` / `array_grow`
primitives. Switching BinSerialize's buffer type from `[Byte]` to
`Array[Byte]` reduces every cursor read to O(1) without touching the
runtime at all.

This lane ships that pure-substrate refactor: same protocol shape,
same encoding format, contiguous carrier.

## Scope as planned

- `protocol BinSerialize` surface change: `[Byte]` → `Array[Byte]`
  on every method.
- Atomic impls (`Int`, `Bool`, `String`, `Real`) rewritten over the
  new carrier.
- Collection combinators (`bin_list_to_bytes`, `bin_option_to_bytes`,
  `bin_char_to_bytes`, the matching `_from_bytes` symmetric pair)
  rewritten.
- `derive_binserialize_impl` in `stage2/compiler.kai` emits
  `Array[Byte]` in the buffer position; record / sum encoders emit
  `array_concat_bytes` / `array_one_byte` instead of `list_append` /
  `EList`.
- Existing fixtures (`examples/stdlib/binserialize_*.kai`) updated
  to call `array_length` and `array_concat_bytes` on the new buffer.
- Inline bench harness at `tools/bench-binserialize-roundtrip.kai`.

## Scope as shipped

Identical to planned, plus two judgement calls:

1. **`Mutable` masking compatibility.** The masking pass in
   `stage2/compiler.kai` (issues #251 + #252) drops `Mutable` from a
   function's inferred row when every `array_set` / `array_grow`
   demand targets a locally-constructed Array. Naively, writing
   "fill a buffer in a helper" passes the local Array as a parameter
   to the helper, which counts as escape under the v1 masking rules —
   `Mutable` would leak into BinSerialize's public row, breaking the
   protocol surface.

   Two complementary tactics keep the row pure:
   - Helpers that build a fresh buffer (e.g. `bin_int_to_bytes`,
     `bin_string_to_bytes`) construct the `Array[Byte]` locally and
     mutate it inside a `list.foldl` lambda that *captures* the
     local — never receives it as a parameter. The masking pass
     classifies captured-name demands as local.
   - Helpers that take an existing buffer and either read-only-scan
     it or strictly fill a region (`array_concat_bytes`,
     `bin_write_len4`) are added to the `is_array_safe_callee`
     whitelist in `stage2/compiler.kai`. Whitelisting means a
     caller's local Array passed in as the first argument is NOT
     flagged as escaping — the helper's body still masks `Mutable`
     on its own local `dst`.

   This is a deliberate, narrow whitelist extension; the contract
   that `array_concat_bytes` / `bin_write_len4` are read-only on
   non-`dst` inputs is documented at their definitions.

2. **List-length prefix size.** `bin_list_to_bytes` originally used
   `bin_int_to_bytes_loop(len, 4, …)` — a hand-rolled 4-byte LE
   length prefix. The first refactor pass routed this through
   `bin_int_to_bytes(len)` which writes 8 bytes (full Int); the
   decoder used `bin_read_len4` (4 bytes) and the cursor desynced.
   Fixed by allocating the 4-byte prefix locally and reusing the new
   `bin_write_len4` helper. Documented inline; trivial in hindsight,
   non-trivial during debugging because all three list fixtures
   produced surprising-looking integer values (multiples of 2^32) at
   the consumer.

## Design decisions

### Why not extend the masking pass to track per-name "fresh array"
provenance through arbitrary calls?

Considered and rejected. The v1 masking pass is intentionally
syntactic — a per-name `local_set` and an `escape_set` that errs on
the side of "external". Adding inter-procedural tracking (e.g.
"if a helper's signature says the first arg is consumed-not-mutated,
allow it as safe") expands the masking pass into a small effect
analysis and would force every stdlib write-helper to declare
provenance. The whitelist of two names (`array_concat_bytes`,
`bin_write_len4`) is the minimum surface that closes the BinSerialize
gap; future lanes can revisit if more callers want the same shape.

### Why a `bin_zero` sentinel and not an empty-array primitive?

The runtime's `array_make(n, init)` requires `init: T` even when
`n == 0`. `bin_zero() = bin_byte(0)` is a one-liner that documents
the intent; the constant folds at codegen.

### Why `int_range_to_list` private to protocols.kai?

stdlib/core/list.kai already exports `iota` / `range_step` etc., but
they vary across the m14 phase 1 / phase 2 namespacing flux and the
public list namespace already has a half-dozen "make a list of
integers" entry points. Keeping the BinSerialize-specific helper
private to protocols.kai avoids depending on whichever variant ends
up canonical post-#227.

### Why `(acc, elem)` parameter order in lambdas?

`list.foldl[a, r, e](xs: [a], init: r, f: (r, a) -> r / e) : r / e`.
First refactor pass wrote `(i, cur) =>` (treating `i` as the iteration
index, like `Enumerable#each_with_index` in Ruby). That swapped the
accumulator and element — `cur` became the iteration counter and
`i` became the running value, which is why `bin_int_to_bytes(-3)`
crashed with `index -3 out of range`. Two lambdas needed the swap
(`bin_int_to_bytes`, `bin_write_len4`); the rest were already correct.

## Bench results

Harness: `tools/bench-binserialize-roundtrip.kai`. 500-node payload
(`#derive(BinSerialize)` record with `Int`, `String`, `[Int]` fields),
~40 KB encoded. macOS 25.4 / Apple Silicon; built with `-O2`.

| Variant                  | Wall time (5-run median) | Per-decode |
| ------------------------ | ------------------------ | ---------- |
| build + encode only      | 0.62 s                   | —          |
| build + encode + 1 decode| 0.57 s                   | (noise)    |
| build + encode + 20 decodes | 0.71 s                | ~4.5 ms    |
| build + encode + 100 decodes | 1.34 s               | ~7.2 ms    |

Median: **~5–7 ms per 40 KB decode**, comfortably below the < 100 ms
target. Speedup vs. issue #485's pre-refactor baseline (19 s for one
decode): ~**2600–3800×**.

The build+encode prologue dominates the single-decode timing, which
is why the table shows `1 decode` as noise rather than a meaningful
delta. Multi-decode runs are the load-bearing numbers.

## Selfhost impact

`make tier0` reports `selfhost byte-identical` after the change.
This is the expected outcome: `stage2/compiler.kai` itself does not
use `#derive(BinSerialize)` nor call any `BinSerialize` method, so
the emitted C from kaic1-compiles-compiler.kai is byte-for-byte
unchanged. The `derive_binserialize_impl` change only affects
*downstream* code that uses the derive.

## Coverage / fixtures

All 14 existing `examples/stdlib/binserialize_*.kai` fixtures pass:

```
binserialize_derive_char         OK
binserialize_derive_list         OK
binserialize_derive_list_of_unknown   OK (negative)
binserialize_derive_nested       OK
binserialize_derive_option       OK
binserialize_derive_sum_collections   OK
binserialize_list                OK
binserialize_nested              OK
binserialize_no_impl             OK (negative)
binserialize_real                OK
binserialize_record              OK
binserialize_recursive           OK
binserialize_string_escapes      OK
binserialize_sum                 OK
```

No new fixtures added — the existing matrix already covers the four
atomic impls, both kinds of derive (record + sum), recursive types,
nested collections, and the two error-shape negative tests. The
bench harness at `tools/bench-binserialize-roundtrip.kai` is the
performance fixture; it's not wired into a CI gate yet (see
follow-ups).

## Cost vs estimate

Estimated: half a day plus retro + PR.
Actual: ~4 hours. The `Mutable` masking interaction was the surprise
that ate the time — the first naive recursive-with-accumulator
rewrite would have been correct functionally but propagated
`Mutable` through every public BinSerialize signature. Re-routing
through `list.foldl` + safe-callee whitelist was the second pass
that landed.

## Follow-ups for next lanes

- **#452 Phase A.0 is now unblocked.** The precompiled-prelude cache
  was waiting on BinSerialize's substrate to land. Reopen #452 and
  let the cache lane consume `Array[Byte]` directly.
- **Tier 2 perf gate for BinSerialize round-trip.** The bench
  harness produces stable numbers but no CI gate; a silent regression
  (e.g. a future lane that re-introduces an O(N) lookup somewhere on
  the decode path) would not be caught until a downstream lane
  complains. Open follow-up issue: wire
  `tools/bench-binserialize-roundtrip.kai` into Tier 2 with a
  conservative ceiling (say, 50 ms for 40 KB single decode).
- **`array_concat_bytes` as a generic `array_concat`.** This lane
  declares `array_concat_bytes(a: Array[Byte], b: Array[Byte])`
  because protocols.kai loads before stdlib/array.kai and the
  generic `[T]`-Byte sentinel ergonomics would need `Default` to
  land cleanly. A future lane can lift this into a polymorphic
  `array_concat[T]` once stdlib namespacing settles.
- **`#derive(BinSerialize)` for parametric types.** Out of v1 scope
  (per the issue #459 retro); the Array[Byte] refactor neither helps
  nor hurts that path.
- **8-byte IEEE-754 `Real` encoding.** Still routes through
  `real_to_string` per #459's v1 fallback. Independent of this
  lane's buffer-substrate work.

## Files touched

- `stdlib/protocols.kai` — BinSerialize section rewritten over
  `Array[Byte]`; added `int_range_to_list`, `array_concat_bytes`,
  `array_one_byte`, `bin_write_len4`, `bin_zero` helpers.
- `stage2/compiler.kai` — `derive_binserialize_impl` and friends
  emit `Array[Byte]`; `is_array_safe_callee` extended with
  `array_concat_bytes` + `bin_write_len4`.
- `examples/stdlib/binserialize_*.kai` — `list_length` →
  `array_length` on byte buffers; `binserialize_list.kai` rewritten
  to use `array_concat_bytes` + `array_make(0, …)`.
- `tools/bench-binserialize-roundtrip.kai` — new bench fixture.
- `docs/lane-experience-binserialize-array-buf.md` — this retro.
