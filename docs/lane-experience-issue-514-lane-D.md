# Lane experience — issue #514 lane D: DecimalBig + Rational on BigInt

The final lane of the #514 numeric chain (A: fixed-width primitives,
B: BigInt, C: Decimal-Int128, D: this). Closes #514.

## Scope as planned vs as shipped

**Planned** (brief): two concrete stdlib types over lane B's BigInt.
`DecimalBig` = `{ raw: BigInt, scale: Int }` (a `Decimal` with no scale
ceiling — the Int128 `Decimal` from lane C tops out near 38 digits);
`Rational` = `{ num: BigInt, den: BigInt }` auto-normalised by gcd
(positive denominator, sign in numerator, reduced to lowest terms at
every construction). Show/Eq/Ord for both. Pure stdlib; reuse the
BigInt API, do not reimplement it. No runtime or compiler change
expected.

**Shipped**: exactly that, no scope drift. Four stdlib files
(`decimal_big.kai`, `decimal_big_proto.kai`, `rational.kai`,
`rational_proto.kai`), two fixtures, `kai info` + catalog docs. No
runtime touch, no compiler touch — the brief's prediction held. The
only deviations from a naive transcription were two name-collision
traps (below), both fixed by renaming, not by widening scope.

## Design decisions and alternatives considered

**DecimalBig ops are total, no `checked_*`.** Lane C's Int128 `Decimal`
needed `checked_add`/`checked_mul` and a panic path because the carrier
overflows. BigInt never overflows, so `add`/`sub`/`mul` are
unconditionally `DecimalBig → DecimalBig → DecimalBig` with no Option,
no panic, no overflow detection. `div` still returns `Option` (zero
divisor is a domain event, not a carrier event) and still takes an
explicit truncating target scale — same precision-loss discipline as
lane C, just without the width ceiling. This keeps the surface a strict
simplification of `Decimal`, not a divergent API.

**Rational normalises at construction, through one choke point.** Every
op routes through `make`, which divides both sides by their gcd and
forces a positive denominator. This mirrors BigInt's own
`make`-is-the-only-normalizer discipline (lane B): equal values share
one representation, so `Eq` is a direct field compare and `cmp`
cross-multiplies without first reducing. The alternative — reduce
lazily on demand — was rejected: it would make `Eq` reduce-then-compare
and lose the canonical-form invariant that the whole design leans on.

**`gcd` via Euclid over BigInt, not binary GCD.** Euclidean `gcd(a,b) =
gcd(b, a mod b)` reusing lane B's `bc.rem`. Binary (Stein's) GCD would
avoid division but BigInt division is already correct and not a hot
path for the fintech/exact-math magnitudes this targets; Euclid is
three lines and obviously correct. Same simplicity-beats-cleverness
call lane B made for its shift-subtract divider.

**`make` panics on a zero denominator; `div`/`recip` return Option.**
A zero denominator handed to `make` is a corrupt-input invariant (you
asked for `n/0`), parallel to how lane C's `decimal` panics on a
carrier-invariant break. But a zero *divisor* in `div`/`recip` is a
recoverable domain event, so those return `Option`. Same total-vs-
checked split lane C settled, applied to the rational shape.

## Structural surprises the brief did not anticipate

**Two name collisions, both invisible to `./bin/kai run` but fatal to
the Makefile's C-backend target.** This was the lane's only real time
sink and is worth recording precisely:

1. **Fixture filename collides with the stdlib module.** Naming the
   DecimalBig fixture `decimal_big.kai` made `decimal_big_proto`'s
   `import decimal_big` resolve to the *fixture* (same basename in the
   compile's working dir), producing `import cycle detected` +
   `unknown module qualifier 'db'`. Fix: name fixtures
   `decimal_big_demo.kai` / `rational_demo.kai`. This is the
   "module name must not collide with stdlib" trap, hit from the
   fixture side rather than the module side.

2. **A private helper named `reduce` collides with a prelude builtin.**
   `rational.kai` had `fn reduce(x, g)` for the gcd-division step. It
   emitted as `kai_prelude_reduce`, the same symbol as the prelude's
   3-arg `reduce` (fold) builtin — so the generated C called a 3-arg
   function with 2 args: `too few arguments to function call, expected
   3, have 2`. Renamed to `exact_div`. This is the
   "stage1 bundle shadows prelude builtins" trap at the codegen layer.

Both collisions compiled *clean* through `./bin/kai run` (the oracle
flow) and only failed under the Makefile's `$(CPPFLAGS_CORE)` C-backend
compile. Lesson reinforced: run the actual `make` target before
committing fixtures — the wrapper's run path is not the same compile as
the gated target. A name-collision scan of every new function symbol
against `kai_prelude_*` in `runtime.h` is cheap insurance for any
stdlib lane.

## Fixtures added and coverage gaps

- `examples/numeric/decimal_big_demo.kai` (+`.out.expected`): a 42-digit
  value (past the Int128 ceiling) shown exactly; scale-aligning add;
  `mul` scales adding; `div` at target scale 30; scale-independent
  `cmp`; zero-divisor → `None`.
- `examples/numeric/rational_demo.kai` (+`.out.expected`): `1/2 + 1/3 =
  5/6` (normalises); `4/8 → 1/2` (reduces at construction); negative-
  denominator sign migration; whole-number rendering; mul/sub/div/recip
  reduced; cross-multiplication ordering; a BigInt-sized numerator
  beyond i64; parse of fraction + integer; zero-denominator rejection.

Wired into `stage2/Makefile` as `test-numeric-decimal-rational`
(C backend, in `TEST_LIGHT_TARGETS` + `test-fast`) and
`test-numeric-decimal-rational-native` (native parity, in
`tier1-native.yml`). Both backends diff against the same golden.

Coverage gap: no `Hash` impl (BigInt has one, but neither DecimalBig
nor Rational needs hashing for any current consumer; a HashMap-keyed-by-
rational use case would add it). No `Numeric` ring impl for either —
`decimal.kai` has `impl Numeric for Decimal`, but `DecimalBig`'s
unbounded scale makes a single `one()`/ring identity less obviously
useful, and no aggregate consumer needs it yet. Left for demand.

## Real cost vs estimate

Low, as the brief predicted — pure stdlib over a mature BigInt API. The
type+ops+show for each type was a near-mechanical adaptation of lane C's
`decimal.kai` (carrier swap) and a standard reduced-fraction
implementation. The only non-trivial spend was diagnosing the two name
collisions, both of which presented as opaque generated-C errors rather
than kaikai diagnostics.

## Follow-ups left for next lanes

- A `round`-to-target-scale for `DecimalBig` (half-away-from-zero, like
  `decimal.round`) if a consumer needs rounding rather than truncation.
- `Numeric`/`Hash` impls when a concrete consumer appears (see gaps).
- A `Rational ↔ DecimalBig` bridge (`to_decimal_big(r, scale)`) is the
  obvious next convenience if exact-then-render workflows show up.

These are demand-driven, not omissions: the lane shipped the complete
surface #514 lane D specified.
