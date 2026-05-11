# Lane 4 (issue #473) — u8 nominal primitive type

**Branch**: `lane-4-u8-primitive`
**Issue**: [#473](https://github.com/lnds/kaikai/issues/473)
**Status**: implementation complete, PR pending integrator review.

## Scope as planned vs. shipped

The original brief asked for a "full scope" landing covering both u8
scalar **and** `Array[u8]` contiguous storage (a new `KAI_ARRAY_U8`
runtime tag with `uint8_t *data`, plus 7 helper functions, plus
Perceus fast-path coordination). On the ground, this turned out to be
2-3 weeks of work — the monomorph + emit + Perceus coordination
needed to introduce a second array storage form is the same shape as
issue #440 (Phase 4 payload unbox), which is still at design stage.

After confirming the cost on the actual code (touched 12 sites in the
typer's `Ty` walkers before realising the array storage half would
balloon the lane), the user redirected the lane to **scope 4a**: u8
scalar nominal type only. `[Byte]` continues to use the existing
boxed `kai_array` machinery — slower on memory (8 bytes per element
via `KaiValue *`) but correct, and the typer-level nominality is the
piece that unblocks PR #471. Lane 4b will revisit contiguous storage
once #440 lands or is scoped down.

## Design decisions

### `u8` boxed, not unboxed at the scalar level

Phase 3 unboxing (#383) added a path where integral scalars survive
inside fibers as raw `int64_t` rather than `KaiValue *`. Extending
that path to `uint8_t` was tempting, but Lane 4a stays away from it:

- Phase 3's unboxing was gated by `ty_is_unboxable_t`. Adding `TyU8
  -> true` there would require tracking the 1-byte vs 8-byte width
  through every code path that today assumes "integral unboxed = i64".
- The user-visible value of a 1-byte raw scalar is small (a single
  fiber-local integer is already cheap); the user-visible value of a
  contiguous `Array[u8]` is large (memory honesty). Honest about the
  budget: scalar unboxing pays off only when arrays follow, and
  arrays are Lane 4b.

`ty_is_unboxable_t` and `ty_is_integral_raw_t` deliberately keep
`TyU8 -> false` (via the default arm). Selfhost byte-identical
confirms no Phase 3 path picks up TyU8 by accident.

### Nominal, not refinement-based

`u8` could in principle be encoded as `Int where 0 <= n <= 255` using
the m12.6.x refinement machinery. That would have given automatic
widening (`u8` ⊑ `Int` in the unifier) and free pattern compatibility.
Lane 4 deliberately picks nominal:

- Refinements erase at codegen time (`mangle_ty(TyRefineT(b, _)) =
  mangle_ty(b)`). Two monomorphisations of `Array[T]` with `T = Int`
  and `T = Int where 0..255` collapse to the same impl. That kills
  the storage-honest payoff of Lane 4b: we want `Array[u8]` to mangle
  distinctly so future work can give it a distinct C type.
- The integrator note on PR #471 makes the same call: "u8 nominal
  matters specifically because we need a distinguishable mangle for
  the eventual Array[u8] contiguous representation."

### Wrapping vs saturating arithmetic

`u8_add` / `u8_sub` wrap mod 256 (C `uint8_t` semantics). The
alternative is saturating (clamp to 0/255), which is more useful in
some graphics / DSP contexts but is rarely the byte-buffer case. The
binary-buffer use cases that motivate `Byte = u8` (network frames,
hex/base64, hash digests, BinSerialize cache) all want wrap, which
matches every other language's `uint8_t` semantics. Saturating ops
can be added later as `u8_sat_add` if a real consumer asks.

### Where the alias-in-generics bug went

The brief named "fix the alias-in-generics bug from PR #471" as Part
A's sub-task. The recon turned up that `expand_ta_decls` (line ~23070
of `stage2/compiler.kai`) already rewrites `Array[Byte]` →
`Array[Int]` (and now `Array[u8]`) pre-typing, recursively walking
every type constructor including TyList, TyCon, TyFn, TyDim, TyRefine.
The bug described in PR #471's notes — "Synth'd impl signatures emit
Int rather than Byte because `expand_ta_decls` runs before
`lower_protocols`" — is exactly the expected behaviour of the pass:
aliases are gone by the time the synthesiser builds signatures. PR
#471 carries a single-line workaround acknowledging this is
intentional, not a bug.

Lane 4a's contribution to that picture: once `Byte = u8` (rather
than `Byte = Int`), the synth path still resolves `Byte` to `u8`
pre-typing, but now the underlying type carries the right semantic
identity. Future work that wants to specialise `Byte` impls (e.g.,
generate a `BinSerialize[u8]` instance keyed on the byte slot rather
than the int slot) has the right hook because `mangle_ty(TyU8) =
"U8"` is distinct from `mangle_ty(TyInt) = "Int"`.

## Structural surprises

### `prelude_names` was the gate, not `add_prelude_sigs`

The first smoke test failed with "cannot find `int_to_u8` in this
scope" even after the typer entries were in place. The compiler
exposes prelude functions to user code via **three** coupled tables,
not the one I expected:

1. `prelude_names()` (line ~9819) — the bare-name list. Without this,
   the typer never even tries to look up the function.
2. `add_prelude_sigs` (line ~23927) — the type signatures.
3. `prelude_table` (line ~11571) — the C entry-point names that the
   emitter wires up.

Adding the third without the first produced a silent failure: the
function existed as a `TyEntry` in the environment but the bare-name
resolver never consulted that environment for the unknown identifier.
A future lane that adds prelude functions should consult the trio at
once. Documenting the contract in a comment at the head of one of the
tables would save the next person an hour.

### `_Wswitch` warnings show up but the build doesn't fail

Clang LSP flagged `KAI_U8` as missing from two `switch ((KaiTag)
v->tag)` statements as warnings. The actual gcc/clang build passes
because the switches have `default:` arms — but the warning is real
and worth addressing. I added explicit `case KAI_U8:` arms to all
three switches (`kai_op_eq`, `kai_to_string`, `kai_free_value`) for
documentation rather than correctness. A reader looking at the
runtime should not have to grep through `default:` arms to learn
what u8 does.

### `mk` collides with `complex.mk`

The first arithmetic fixture used `fn mk(n: Int) : u8 = ...` and
broke compilation with "expected: (Int) -> ?t1, found: (Real, Real)
-> ?t2". The stdlib's `complex.mk(re: Real, im: Real) : Complex`
shares the bare name. UFCS resolution looked at every `mk` in scope
and picked the wrong one because no return-type hint disambiguated
the call site. Renamed to `b_of` in the fixture. Future stdlib
fixtures should avoid 2-letter bare names that collide with the
math/complex namespace.

## Fixtures added

| Fixture | Purpose |
|---------|---------|
| `examples/stdlib/u8_basic.kai` | conversion round-trip + boundary values 0/128/255 + out-of-range Err |
| `examples/stdlib/u8_arithmetic.kai` | wrap-mod-256 add/sub + eq/lt |
| `examples/stdlib/byte_alias_check.kai` | `Byte = u8` transparent through fn signatures and `Result[_, _]` |
| `examples/stdlib/u8_int_mismatch.err.expected` | negative: passing `Int` to a `u8` slot rejected at typer |

The negative fixture pairs with the `test-stdlib` Makefile harness
that loops `grep -qF` over each line of `.err.expected` against the
captured stderr — robust against unrelated diagnostic changes.

## Coverage gaps for Lane 4b

Out of scope for 4a, listed in priority order for the follow-up
lane:

1. **`Array[u8]` contiguous storage** (`KAI_ARRAY_U8` tag, `uint8_t
   *data`). Requires monomorph fork on element type, new helper
   family, and a polymorphic `array_length` dispatch.
2. **u8 interning cache** (0..255 fit naturally; one allocation per
   range slot). Mirrors `kai_int_cache_lo..hi`.
3. **u8 in LLVM raw codegen path**. The four LLVM helpers (lines
   ~40341/40379/40391/40421) currently cover Int/Real/Bool/Char with
   wildcards; adding `TyU8 -> "i8"` would let the optional LLVM
   backend unbox u8.
4. **`Show`/`Eq`/`Hash`/`Ord` derive for `u8`**. The arms in
   `derive_builtin_impl` (line ~45477) list specific tnames per
   protocol; adding "U8" lights up the canonical derive paths.
   Today users get equality and to-string explicitly through `u8_eq`
   / `u8_to_string`, but `derive` blocks won't synthesise.
5. **Real consumers**: migrate `random_secure`, `uuid`,
   `encoding/hex`, `encoding/base64` from `[Int]` representations to
   `[Byte]`. Not done in 4a because each migration is a small lane
   on its own and `[Byte]` is still backed by `kai_array` of boxed
   `KaiValue *` (i.e. no memory win until 4b).
6. **`docs/effects-stdlib.md`** NetTcp / UdpSocket signatures cite
   `[Byte]`; the alias resolves them honestly now. No content edit
   required, but a one-line "v1 status" sidebar saying the storage
   is still boxed pending Lane 4b would set expectations correctly.

## Real cost vs. estimate

- Estimate in brief: 3-5 days for full scope (which the brief
  underestimated; full scope is 2-3 weeks).
- Estimate for 4a (scoped down): 3-5 days.
- Real cost: ~3 hours of focused work after the scope correction.
  The 12 typer match sites were mechanical once the recon mapped
  them out. The single non-trivial bug (`prelude_names` missing)
  cost ~10 minutes of debugging.

The gap between "scope as briefed" and "scope as shipped" is the
single most important data point of this lane. The brief assumed
that adding a primitive type is comparable to other typer-only
features (Option combinators #470, if-elseif #467) because the
typer surface area is similar. The real difficulty multiplier
came from the array storage half: introducing `kai_array_u8`
forces a second axis of representation through monomorph + emit
+ Perceus that none of those earlier lanes touch. **Scoping rule
for future "add a primitive" lanes: estimate the scalar lane
separately from any storage-representation lane that depends on
it.**

## Verification gates

- `make tier0` — green. Selfhost byte-identical maintained across
  all three commits.
- `make tier1` — green locally (typer + runtime touches; per memory
  `feedback_tier1_local_optional.md`, this is one of the cases
  where tier1 local is mandatory rather than optional).
- Demos baseline — 26 / 26.
- Smoke test (manual): `int_to_u8(42) -> Ok(u8(42))`, `int_to_u8(300)
  -> Err`, `u8_add(200, 100) = 44`, `Byte` and `u8` interchangeable
  through fn signatures.

## Follow-ups (not opened by this PR)

- Lane 4b — `Array[u8]` contiguous storage (issue to be opened
  by the integrator or next contributor).
- u8 interning cache for the 0..255 range (small but real perf win).
- LLVM backend support for u8 raw scalars (gates Phase 3 unboxing
  for u8 inside fibers; not on the critical path).
- `derive` support for u8 (Show/Eq/Hash/Ord arms in
  `derive_builtin_impl`).
- Migration of `random_secure`, `uuid`, `encoding/hex`,
  `encoding/base64` consumers — deferred until 4b lands so they
  win on memory at the same time.

## Closes

Issue #473. The "fix the alias-in-generics bug" sub-task is
**not** part of this lane — see "Where the alias-in-generics bug
went" above. PR #471 (BinSerialize) was unblocked structurally by
introducing `u8` nominally; the integrator will rebase #471 atop
this lane.
