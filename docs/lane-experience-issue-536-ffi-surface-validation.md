# Lane experience — issue #536 (FFI surface validation)

## Scope as planned

Two shapes pinned by the #520 audit phase 2:

1. **Missing `/ Ffi` capability** — `extern "C" fn puts(s: String) : Int`
   (no `/ Ffi`) parsed and compiled silently, bypassing the capability
   discipline pinned in CLAUDE.md baseline architecture.
2. **kaikai-only types in extern signatures** — `extern "C" fn list_int() : [Int] / Ffi`
   accepted; `[T]` is the kaikai linked-list type whose runtime layout
   C cannot consume.

Fixtures quarantined under `examples/negative/silent_contract/`
(`extern_missing_ffi_capability.kai`, `extern_array_kaikai_type.kai`)
to be promoted on close.

## Scope as shipped

Both shapes shipped in the same lane:

- `parse_extern_decl` now post-validates the `DAxiom` returned by
  `parse_axiom_decl`. Three independent checks anchor parse-time
  diagnostics to the `extern` keyword token:
  1. row contains `Ffi` (label match by name; args ignored — `Ffi` is
     nullary);
  2. every parameter type passes `is_c_abi_safe_type`;
  3. return type passes `is_c_abi_safe_type(is_return_slot=true)`.
- C-ABI allowlist (hardcoded, not config-driven): `Int`, `Real`,
  `Bool`, `String` for params + return; `Unit` for return only.
  Anything with type arguments (`Option[T]`, `Reader[Int]`,
  user-declared `Box[T]`), any `TyList`, `TyFn`, `TyDim`, `TyRefine`
  is rejected by default — closed allowlist, not blocklist.
- Three negative fixtures live in `examples/negative/ffi/`:
  - `extern_missing_ffi_capability.kai` (promoted from quarantine);
  - `extern_array_kaikai_type.kai` (promoted, return-slot rejection);
  - `extern_param_kaikai_type.kai` (new; exercises the param-slot path
    that the promoted return-slot fixture does not touch).
- `examples/negative/silent_contract/README.md` strikes the #536 row.

## Design decisions

### Validation lives in the parser, not the typer

The briefing left this open. Chose the parser site (specifically the
post-processing step in `parse_extern_decl` after `parse_axiom_decl`
returns) for three reasons:

- The `extern "C" fn` token sequence is already structurally distinct
  from a bare `axiom`; rejecting the malformed surface form before any
  semantic work matches the existing discipline (the ABI-string check
  and the `pub`-order check both live here).
- `lower_axiom_one` rewrites `DAxiom` into a `DFn` with a magic `ETodo`
  body. The typer runs over the rewritten shape and has no DAxiom-
  shaped hook to attach an FFI-specific row check to. Adding one would
  drag DAxiom back into the typer surface, which the pipeline
  deliberately removed in m12.7.
- Diagnostic anchoring is cleaner: the `extern` keyword token is
  available in `parse_extern_decl`, and `p_error(parser, token, msg)`
  produces the same line/col format the rest of the negative-fixture
  suite already matches.

### Closed allowlist, not open blocklist

The Param + return-type validator answers `is_c_abi_safe_type` as
true *only* for the explicit set, returning false for every other
`TyKind` variant. The alternative (rejecting a known list of bad
types: `TyList`, `Option[T]`, records) would silently approve new
kaikai-only types added in future lanes. Closed allowlist forces a
deliberate decision when a new type wants to cross the FFI surface.

### No `Unit` parameters

C's `void` parameter shape is `f(void)`, which has no positional
slot — it is not the same shape as kaikai's nullary `Unit`. Allowing
`Unit` in a parameter list would produce a misleading C ABI mapping
(an empty marshalled value where C expects no argument). `Unit` is
allowed only in the return slot, where it maps cleanly to `void`.

### Diagnostic shape

Single-line `error:` followed by the standard underline. Each check
produces a distinct message keyed by the fixture's `.err.expected`
substring:

- `must declare \`/ Ffi\`` for the row-missing case;
- `parameter \`<name>\` has type \`<surface>\` which is not C-ABI compatible`
  for parameter-type rejection;
- `return type \`<surface>\` is not C-ABI compatible` for return-slot
  rejection.

`type_surface` walks `TyKind` and produces a kaikai-side rendering
(`[Int]`, `Option[Int]`, `Box[T]`). It's reused only here; if any
future diagnostic needs the same rendering, factor up — until then
it lives next to the validator.

## Selfhost risk audit (pre-lane)

The brief flagged a pre-audit ruling out selfhost risk:

- `stage2/compiler.kai` declares zero `extern "C" fn`.
- `stdlib/` declares zero `extern "C" fn` — the C bridges live in
  `stage0/runtime.h`, not on the kaikai side.
- The only `extern "C" fn` declarations in the tree are in
  `examples/ffi/`, `examples/effects/ffi_*`, and the
  `ffi_pub_axiom_cross_module/` fixture. Every one already declares
  `/ Ffi` and uses only `Int` / `String` / `Bool` parameters and
  returns.

Confirmed empirically: `make tier0` after the implementation reported
`self-hosting fixed point: OK` and `demos no-regression OK (28
passing, baseline 27)`. The 3 promoted/new negative fixtures lifted
`test-negative.sh` from 81 PASS → 84 PASS.

## Surprises

- `parse_axiom_decl` is shared between bare `axiom` decls and
  `extern "C" fn` decls. The validator deliberately runs only in the
  `extern "C" fn` path so an old-style bare `axiom name(...) : T`
  declaration (still permitted by the parser even though
  `lower_axioms` rewrites it identically) is not retroactively
  required to surface `Ffi` — bare axioms are not FFI declarations and
  their row may legitimately be `REmpty` or carry domain effects.
- The `extern "C" pub fn` ordering is canonical (`pub` parses *after*
  the optional name override and the ABI literal). Not a parser bug;
  the briefing's note about `myffi.kai:19` flagged this as worth
  checking and the audit confirms it.

## Fixtures + coverage gap

Three fixtures cover three shapes:

| Fixture | Shape | Anchor |
| --- | --- | --- |
| `extern_missing_ffi_capability.kai` | row missing `Ffi` | row-check branch |
| `extern_param_kaikai_type.kai` | `[Int]` in param slot | `first_non_c_abi_param` |
| `extern_array_kaikai_type.kai` | `[Int]` in return slot | `is_c_abi_safe_type` return path |

No fixture for `Option[Int]` in a parameter slot, `fn(Int) -> Int` in
a return slot, or a user-declared sum type in either position. Those
are isomorphic to `[Int]` from the validator's perspective (each
fails the `targs == 0` and known-name combination), so adding them
would test the same code path. Left for a future lane if a user
files a regression.

## Real cost

~3 hours (well under the 4-6h estimate for shape 1 alone; the
shape 2 validator turned out to be ~30 lines once the type surface
helper was factored out, not the day-long retrofit the brief
hedged against). The audit's "zero selfhost risk" call was correct
and let the implementation skip the staged-rollout dance.

## Follow-ups

None scoped from this lane. The C-ABI allowlist is intentionally
narrow; widening it (e.g., `Int32`, `UInt64`, opaque pointer types)
belongs to whichever lane introduces those surface types — they edit
`is_c_abi_safe_type` directly, no new infrastructure needed.
