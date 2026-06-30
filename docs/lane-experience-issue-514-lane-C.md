# Lane experience — issue #514 lane C: Decimal carrier Int128

## Scope as planned vs as shipped

**Planned** (brief): swap the `Decimal` carrier from `Int` (i64) to
`Int128`, lifting the silent fintech overflow at the ~9.2e18 i64
ceiling; make `add`/`mul` detect Int128 overflow (Option or panic,
never wrap); update `pow10`/`from_parts`/docs; add fixtures (scale-GDP,
overflow, money). stdlib-side only; depends solely on lane A's Int128.

**Shipped**: all of the above, plus one unplanned prerequisite —
**wiring Int128 division in the runtime** (a gap lane A left). Lane A
cabled `+`/`-`/`*`/`<`/`==` for the fixed-width tags but NOT `/`;
`kai_op_div` fell through to "type mismatch in /". `decimal.div`
intrinsically divides the i128 carrier, and the `mul`-overflow check
uses a division inverse, so the carrier swap could not land without it.
Added `kai_fixed_div` (signed-aware, divide-by-zero abort) in
`stage2/runtime.h`, parallel to the existing `kai_fixed_arith`.

## Design decisions and alternatives considered

**Total+panic vs Option for `add`/`sub`/`mul`.** Chose: the three stay
total (`Decimal → Decimal → Decimal`) and PANIC on carrier overflow;
three new `checked_*` return `Option[Decimal]` (`None` on overflow).
Rationale (validated against the language-architect lens): the total
ops have real consumers that need a ring — `impl Numeric for Decimal`,
`money.kai`, `fx.kai`, the existing fixture chain `add(add(...))`.
Moving `add` to `Option` would poison every generic Numeric consumer
for an event none of them can handle, and a ring whose `add` returns
`Option` is not a ring. Overflow of a 38-digit carrier is a
corrupt-input invariant break, not a domain outcome — `panic` (an
audited Tier-1 escape) is the honest response; the silent mod-2^128
wrap is the incidental one #514 exists to kill. This mirrors Rust's
`+` (panics) vs `checked_add` (`Option`). `div` keeps its `Option`
(zero divisor = domain failure) and gains a panic on internal scaling
overflow — the two failure classes stay distinct, not collapsed.

**Rejected: generic `Decimal[T]`.** Tier-1 forbids Haskell-style
constraint propagation; one concrete type with an `Int128` carrier.

**Rejected: a new `string_to_int128` runtime prim for `parse`.** The
architect's steer was "no new prim for this". `parse` accumulates the
i128 digit by digit in pure kaikai (`acc*10 + d` via the carrier ops),
so magnitudes up to ~38 digits decode without touching the runtime
surface beyond the division fix that was independently required.

**pow10 ceiling: panic, not clamp.** `pow10` panics for `n > 38` (the
carrier ceiling) rather than silently clamping to 38 as the i64 version
clamped to 18. A silent clamp would reintroduce exactly the wrong-value
class this lane removes.

## Structural surprises the brief did not anticipate

1. **The `/` gap (above).** The brief assumed Int128 was complete from
   lane A; it was complete for the four ops `decimal` happened not to
   stress in lane A's own tests, but division was untested and unwired.

2. **The native bitcode cache.** Editing `stage2/runtime.h` does NOT
   reach the native backend until `stage0/runtime_llvm.bc` is
   regenerated (`tools/gen-runtime-bc.sh`). The `.bc` is a build
   artifact (gitignored, rebuilt by `make kaic2` / CI); a stale `.bc`
   silently kept the old division behaviour. First smoke run after the
   fix still failed for this reason — diagnosis was "is it the typer or
   the runtime", answer was "neither, it's the cached bitcode".

3. **`parse` overflow was its own wrap site.** `acc*10 + d` with raw
   i128 arithmetic wraps: `parse("…105728")` (i128 MAX+1) returned a
   silently-negated value. Fixed by routing the digit fold through the
   `checked_raw_*` helpers — out-of-range literal → `None` (malformed),
   never a wrapped value. Side effect: i128 MIN is unparseable via the
   positive-magnitude path (its magnitude `2^127` does not fit a
   positive i128); rejecting it with `None` is the honest outcome.

4. **`check` is a reserved test keyword.** The first fixture helper
   named `check(...)` parse-errored ("expected function name") — `check`
   is the property-test form alongside `test`/`bench`. Renamed to
   `check_eq`.

5. **`money.parse` is `"CUR AMOUNT"`, not `"AMOUNT CUR"`.** Cost one
   fixture round-trip.

## Fixtures added and coverage

- `decimal_int128_scale` — i64-ceiling magnitudes (i64max+1, 9e18+9e18,
  30-digit GDP-scale parse, large mul) now exact. `.out.expected`.
- `decimal_overflow_checked` — `checked_add`/`checked_mul`/`checked_sub`
  return `None` at the carrier boundary; non-overflowing calls stay
  `Some`. `.out.expected`.
- `decimal_money_large_portfolio` — eight 2.5e18 positions summing past
  the i64 ceiling to 2.0e19; interest-on-large-balance. `.out.expected`.

All three byte-match across C and native. The existing
`decimal_basic`/`money`/`fx`/`div` fixtures stay green (no regression).

**Coverage gap left:** the total-op PANIC path is verified manually
(panic message + exit 1 on both backends) but has no harness fixture —
the stdlib fixture harness models compile-time `.err.expected` and
runtime `.out.expected`, neither of which captures a runtime panic
golden cleanly. The `checked_*` `None` fixtures cover the same overflow
boundary observably; the panic is the same detection wired to `panic()`
instead of `None`.

## Real cost vs estimate

The carrier swap itself was mechanical (kind-distinct Int128 forces
every `raw` literal through `int_to_int128`/`i128z()`, but that is
find-and-replace shaped). The unplanned third of the work was the
runtime division gap + bitcode-cache diagnosis + the `parse` wrap site
— none in the brief, all load-bearing for "no silent wrap" to be true.

## Follow-ups for next lanes

- `kai_op_idiv` / `kai_op_mod` are still unwired for fixed-width (only
  `/` was needed here). A lane that wants `//`/`%` on Int128 should
  mirror `kai_fixed_div`.
- i128 MIN is unparseable (`None`). If a caller needs the exact carrier
  minimum, a dedicated constructor would be the place, not `parse`.
