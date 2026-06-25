# Lane experience — issue #902: stdlib `StringBuilder`

## Scope as planned vs as shipped

**Planned:** an efficient text accumulator backed by a mutable
`Array[Byte]` with amortised (doubling) growth, so building large
strings is O(n) instead of the O(n²) of repeated immutable `String`
concatenation. `append` rides `Mutable`; `build` is pure (masks
`Mutable`). The brief anticipated one technical unknown — the final
`Array[Byte] → String` conversion — and pre-authorised adding a
runtime prim (`kai_string_of_bytes`) in both `runtime.h` copies if no
clean total bytes→string primitive already existed.

**Shipped:** the same public contract (`append` rides `Mutable`,
`build` is pure), but backed by a growable `Array[String]` of
fragments, **not** `Array[Byte]`, and with **zero** runtime or
compiler change. The owner chose this backing after the spike showed
it meets the O(n) invariant and the masking property identically while
avoiding the prim.

7 pub fns: `new`, `with_capacity`, `append`, `append_char`, `build`,
`len`, `is_empty`. Top-level module `stdlib/string_builder.kai`
(94 LOC, `km` A++, cogcom avg 0.8 / max 2).

## The bytes→string prim decision (the planned unknown)

The spike resolved it by **not needing a prim at all**. The real
source of the O(n²) cost in a `++`-fold is re-copying the accumulator
at every step. `string_concat_all([String])` (runtime
`kai_string_concat_all_impl`) already eliminates that: it is a
two-pass measure-then-`memcpy` join, O(total length), one allocation
for the result — exactly what `build` needs. So a builder that
accumulates the original `String` fragments and joins once in `build`
is O(total) with no new primitive.

This was offered to the owner as a design fork (Array[Byte] + new prim
vs fragment-list + `string_concat_all`); the owner picked the
fragment-list backing. Net: no `runtime.h` / `runtime_llvm.c` / typer
/ emit / resolver change, so the C selfhost byte-id is preserved by
construction and the native path needs no new shim.

`int_to_byte_string` / `kai_str_from_bytes` were found along the way
(either could back an `Array[Byte]` version) but are unused — recorded
here so a future lane that wants a byte-level builder knows the prim
surface already exists.

## How masking was implemented + proven

`append` does a direct `array_set` into the builder's fragment array
reached from its `sb` parameter; that write is observable to the
caller, so the typer demands `/ Mutable` (verified: dropping the row
from `append` is rejected with `effect not handled: Mutable`).
`build` / `len` / `is_empty` only read the array (`array_get` /
`array_length`), which never demand `Mutable`, so they are pure. A
caller that creates a builder, appends, and builds entirely locally
has `Mutable` masked at its boundary — the same Koka-style
observable-effects rule `array.from_list` relies on. Proven across
several `array_grow` rounds while staying local
(`string_builder_masks_mutable.kai`, `_build_correct.kai`).

### Structural surprise — the masking pass is unsound for forwarded Array-only records (#903)

The brief's stated negative gate — "`append` used in an exposed
position carries `Mutable`", to be proven by a caller that *receives*
a builder by parameter and forwards it to `append` — does **not**
hold, and the reason is a real soundness hole, not a quirk of this
module.

`mask_local_mutable_demand` (`stage2/compiler/infer.kai`) classifies a
record as mutation-carrying only if it has a transitive `Ref` field;
`te_names_mutating` deliberately excludes `Array` ("an Array-only
record is an opaque buffer, not an observable cell"). That
justification is correct for the handler-install question but wrong
for the escape question: a function that forwards a shared
`Array`-backed record to a `/ Mutable` callee mutates state the caller
observes, yet the pass drops `Mutable` from its row. Minimal repro
(`before: 0` / `after: 7` through a non-`/Mutable` `forward`) filed as
**#903** (`typer`, `regression`). This module does not rely on the
hole — `append`'s write is direct, the correctly-rejected case — so it
ships independently.

The negative fixture was reformulated accordingly: a self-contained
`Array`-backed record with a fn that does the **direct** `array_set`
on a parameter field and omits `Mutable` — rejected, no import / no
privacy noise. That is the genuine rule forcing `append` to carry
`Mutable`.

## Amortised-growth strategy

The fragment array starts at capacity 8 (`with_capacity` overrides,
clamped to ≥ 1). `append` writes at `count`; on overflow it
`array_grow`s to `2 * cap` first. Doubling gives O(1) amortised
append, O(total) over a run. kaikai has no record-update form, so
`append` returns a fresh `StringBuilder` carrying the new `count`; the
fragment array is shared and mutated in place, so this stays O(1) —
the result is used linearly, like a `var` cell.

## Benchmark — the O(n) win, measured

`benchmarks/string-builder/` builds an N-fragment string two ways:
StringBuilder vs a naive left-fold of `++`. Wall time (native backend,
release-equivalent kaic2), single run each:

| N fragments | naive `++`-fold | StringBuilder | speedup |
|-------------|-----------------|---------------|---------|
| 20 000      | 0.34 s          | 0.27 s        | 1.3×    |
| 40 000      | 0.48 s          | 0.28 s        | 1.7×    |
| 80 000      | 0.90 s          | 0.29 s        | 3.1×    |

Doubling N doubles the naive time (super-linear toward quadratic)
while the StringBuilder stays flat; the speedup grows with N as the
O(n²) vs O(n) asymptotics predict. The sharper difference is memory:
at N = 50 000 the naive fold **exhausts a 4 GiB heap and aborts**,
while the StringBuilder builds N = 200 000 in 0.53 s under 4 GiB.

## Fixtures added

- `examples/effects/string_builder_build_correct.kai` (+ `.out.expected`)
  — correctness across 30 appends (multiple grows), mixed
  `append`/`append_char`, empty-builder, `len`/`is_empty`.
- `examples/effects/string_builder_masks_mutable.kai` (+ `.out.expected`)
  — a local builder caller compiles **pure** (Mutable masked), across grows.
- `examples/effects/string_builder_append_requires_mutable.kai`
  (+ `.err.expected`) — direct `array_set` on a parameter field without
  `Mutable` is rejected.

All three wired into `stage2/Makefile` `test-effects` (positives in the
`--path ../stdlib` masking group, negative in the `.err.expected`
group). Native-vs-C parity verified on the positives.

## Cost vs estimate

No estimate given (lane policy). The bulk of the lane was the masking
spike, which both settled the design (fragment-list, no prim) and
surfaced #903.

## Follow-ups for next lanes

- **#903** — fix the masking-pass soundness hole (`te_names_mutating`
  should treat a transitive `Array` field as mutation-carrying for the
  escape check, while keeping `Ref`-only for handler install).
- A byte-level `Array[Byte]`-backed builder (with the
  `kai_str_from_bytes` prim) remains possible if a future need wants
  byte-exact accumulation without `String` fragments; not shipped
  because the fragment-list form already meets #902's O(n) goal.
