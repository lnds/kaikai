# Lane experience — issue #514 lane B: pure-kaikai BigInt

Numeric lane B for the Orongo (1.0) numeric set: arbitrary-precision
signed integers, pure kaikai, no GMP. Builds on lane A's `Int` domain
(the bit operations and wrapping arithmetic) but adds no compiler
primitive of its own beyond a single literal suffix. `#514` stays open;
lane D closes it.

## Scope as planned vs as shipped

Planned (brief): BigInt as a type with an `Array[UInt64]` sign-magnitude
carrier; an inline i64 fast path that promotes on overflow and never
demotes; add/sub/mul/divmod/compare with a pre-sized mutable-local
carry-in-place buffer; Show/Eq/Ord/Hash; a lexer `n` suffix; fixtures
and docs.

Shipped: all of the above functionally, with **two carrier/representation
decisions escalated to and decided by the owner** because they diverged
from the brief's letter on soundness grounds (see below). The fast path,
sign-magnitude form, promotion discipline, the four protocols, the `n`
suffix, and C↔native parity all shipped as specified.

### Two owner decisions that diverged from the brief

Both were escalated (not taken unilaterally) because they change the
type's representation, which the brief and issue #514 had pinned.

1. **Carrier is `Array[Int]` radix-2^32, not `Array[UInt64]`.** kaikai's
   bit operations (`bit_and`/`bit_shl`/`bit_ushr`) are defined only on
   `Int`, not on `UInt64`. The multiply needs the high word of a 64×64
   product, obtained by splitting each operand into halves — which needs
   those bit ops. `Array[UInt64]` would force either u64↔int conversions
   in the multiply hot loop (with the bit-63→negative trap) or new
   UInt64 bit-ops in the runtime + both backends (violating "minimal
   compiler touch"). A 32-bit `Int` limb keeps all arithmetic in the one
   domain where the bit ops already live. The owner chose this for
   soundness + zero compiler touch; the ~2× word count vs u64 is noise
   for the fintech target (Int128 range, not thousand-digit crypto).
   UInt64 does not appear anywhere in the carrier.

2. **Multiply splits each 32-bit limb into 16-bit halves.** A raw 32×32
   limb product is `(2^32-1)² ≈ 1.8e19 > i64::MAX ≈ 9.2e18` — signed
   overflow UB (the same UBSan trap lane A hit). The 16×16 partial
   products are each `< 2^32`, and the column accumulator stays well
   within `Int` range. This is the literal reading of the owner's "the
   product of two limbs decomposes into 32-bit with carry, never forms
   `(2^32-1)²` raw".

The fast path (`Small(Int) | Big(sign, Array[Int])`) was *kept* in v1 as
the brief specified, against the architect's initial recommendation to
defer it — the owner prioritised the inline-value profile.

## Design decisions (and alternatives)

Validated with the language architect (`asu`) up front, then against the
code and `kai info`, not the brief alone.

1. **`n` desugars to a constructor call, no new compiler node.** `99n`
   lexes to `TkBigInt` and the parser emits `bigint.from_int(99)` —
   exactly the existing `TkComplex` → `complex.mk` pattern. BigInt is a
   pure-stdlib type, so the front end stays ignorant of it (Tier 1: the
   typer learns no special case). The suffix only covers i64-range
   literals; larger values go through `from_string`. Alternative
   considered and rejected: a `TkBigInt`-carrying AST node à la `EFixed`
   — unnecessary, because the value always fits the EInt decode.

2. **Promote eager, demote lazily — only at the canonical `make`.** The
   brief said "never demotes". Taken literally with "canonicalize zero
   for direct Eq", that contradicts: a `Big` that shrank into i64 range
   but stayed `Big` would mean `Small(5)` and `Big([5])` are the same
   number with two shapes, forcing range logic into Eq AND Hash. The
   resolution (decided by the owner): `make` — the single normalization
   point every op routes through — demotes a `Big` to `Small` whenever
   it fits `Int`. So no value ever has two shapes; Eq/Hash are biyective
   by construction. "Never demotes" reads operationally as "no eager
   demote thrash in a loop", which still holds — demotion happens once,
   at the result boundary.

3. **`i64::MIN` is the one Int-range value held as `Big`.** Its magnitude
   is 2^63, not a signed Int, so excluding it from `Small` removes the
   `negate(i64::MIN)` overflow trap from every arithmetic path. The one
   branch lives in `from_int`, never in the hot path.

4. **`Mutable` masking drove the file structure.** The observable-effects
   discipline (issue #251/#252) masks `Mutable` only when a locally-built
   Array never crosses a call boundary. Wrapping a freshly-mutated buffer
   in a variant constructor (`Big(s, mag)`) or a `Pair` counts as a
   crossing — so the public ops would have leaked `Mutable` into their
   rows. The fix: producers that build and return a *raw* Array inline
   (while-loop, no helper takes the buffer), and separate wrappers that
   receive the settled Array. This is why `divmod` runs two sweeps (one
   per output Array) instead of returning a `Pair` of two locals.

## Structural surprises the brief did not anticipate

- **`to_string` / `check` / `rem` collide with protocol op names.** A
  free `fn to_string` shadows `Serialize.to_string`; `check` is a test
  keyword; a `var rem` collides with `__proto_rem`. Worked around by
  implementing `Serialize for BigInt` (the idiomatic home for
  `to_string`/`from_string`) and renaming internal binders. This is the
  same builtin-shadowing class noted in prior lane memory.

- **Variant/Pair wrapping breaks `Mutable` masking** (above). Not
  documented as a masking boundary in `docs/effects-stdlib.md` — the
  worked examples only cover "return the raw Array". Follow-up candidate:
  add a wrapped-return example to that doc.

- **`km` size pressure forced a 4-file split.** A single file with
  carrier + arithmetic + conversion dropped to B+ / C- cognitive at
  ~400 LOC (Halstead density on bit arithmetic). Splitting into
  `bigint` (carrier + add/sub/mul/compare), `bigint_limbs` (unsigned
  magnitude layer), `bigint_convert` (decimal + division), and
  `bigint_proto` (protocols) put every file at A or better. The limb
  layer's `pub` surface is internal-by-convention (operates on
  `Array[Int]`, exposes nothing about `BigInt`).

## Fixtures added

In `examples/numeric/`, wired into `test-numeric-bigint` (C backend,
tier1) and `test-numeric-bigint-native` (native parity, tier1-native):

- `bigint_arith` — products/sums/division/ordering beyond 2^64 and 2^96.
- `bigint_factorial` — factorial(20/30/50), byte-identical to reference;
  factorial(30) = 265252859812191058636308480000000.
- `bigint_border` — i64::MAX/MIN render, promotion on overflow, lazy
  demotion round-trip, and Small-vs-demoted-Big Eq/Hash agreement.
- `bigint_literal` — the `99n` suffix, including `max^2` past i64 and
  interop with `from_string`.
- `bigint_errors` — bad parse and divide-by-zero surface as `None`.

Coverage gap: no microbenchmark gating the fast path's value (the
architect's suggested gate for keeping `Small` vs deferring it). The
fast path is correct and tested; its *performance* payoff is unmeasured.

## Real cost

Most of the effort was the up-front design escalation (two rounds with
the architect, one owner decision) and the `Mutable`-masking discovery —
not the arithmetic, which is textbook schoolbook. The 16-bit multiply
split and the lazy-demote canonicalization were the two correctness
pivots; both were caught in design, not debugging.

## Follow-ups for next lanes

- **`pow` / modular arithmetic.** Not in scope; a `bigint.pow(base, exp)`
  and `mod_pow` would serve the crypto use case the issue names.
- **Faster division.** `divmod` is binary shift-subtract (O(bits·n)) run
  twice (quotient + remainder) to keep signatures pure. Fine for the
  fintech range; a Knuth-D limb divider with a single sweep is the
  upgrade if a hot divide path appears — but it needs a `Mutable`-masking
  story for returning two buffers.
- **`Rational`** (issue #514 category D) can now build on `BigInt` as
  pure stdlib.
- **Doc the wrapped-return masking boundary** in `docs/effects-stdlib.md`.
