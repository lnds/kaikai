# Lane experience report — issue #459 BinSerialize

Implementing agent's retrospective. Best-effort; see limitations at
the bottom.

This lane shipped:

- A `Byte` transparent alias (`stdlib/core/byte.kai`).
- A new `BinSerialize` protocol with cursor-based `to_bytes` /
  `from_bytes` plus four atomic impls (Int / Real / Bool / String)
  in `stdlib/protocols.kai`.
- Sibling top-level wrappers (`int_from_bytes`, `bool_from_bytes`,
  `string_from_bytes`, `real_from_bytes`) for the v1 dispatcher
  limitation around return-type-only ops.
- `derive_binserialize_impl` in `stage2/compiler.kai` (records and
  sums), wired through the `derive_impl_for_protocol` dispatcher
  alongside Show/Eq/Hash/Ord and a pre-expansion validator that
  emits typer-time diagnostics for missing field/payload impls.
- A per-derived-type `<lower(tname)>_from_bytes` shim synthesised
  alongside each derived impl so users get a non-overloaded entry
  point.
- 7 positive + 1 negative fixtures under `examples/stdlib/`.

## Objective metrics

- Branch: `lane-6-binserialize`, started from `main` at `ffc7b10`.
- Files touched: 4 (`stdlib/core/byte.kai` new, `stdlib/protocols.kai`
  +~200 lines, `stage2/compiler.kai` +~360 lines, 8 fixtures + 7
  goldens + 1 err).
- Tier 0: green (selfhost byte-identical, demos baseline 26 holds).
- Tier 1: deferred to CI on the PR.

## Scope as planned vs. as shipped

| Planned (brief) | Shipped |
|---|---|
| BinSerialize protocol + 4 atomic impls | Yes |
| `derive_binserialize_impl` for records | Yes |
| `derive_binserialize_impl` for sums | Yes |
| Validator pre-pass | Yes (mirrors `validate_derive_ord`) |
| Round-trip a record fixture | `binserialize_record.kai` |
| Round-trip a sum fixture | `binserialize_sum.kai` |
| Round-trip a list of records | `binserialize_list.kai` (hand-rolled wrapper, not a `[T]` impl — see "Design decisions" §3) |
| Round-trip nested Options | `binserialize_nested.kai` (nominal `MaybeMaybeInt`, not parametric `Option[Option[T]]` — see §3) |
| Strings with escapes | `binserialize_string_escapes.kai` (ASCII only — see §4) |
| Recursive types | `binserialize_recursive.kai` (`BTree = Leaf(Int) | Branch(BTree, BTree)`) |
| Negative: missing field impl | `binserialize_no_impl.kai` |
| Tier 0 green | Yes |
| Selfhost byte-identical | Yes |
| Lane retro | This file |

## Design decisions

### 1. Route (b): sibling protocol, not Serialize refit

The brief specified route (b) of #459 (add a sibling `BinSerialize`
rather than refit `Serialize` to a cursor shape). Confirmed during
implementation — `Serialize.to_string` / `from_string` is still
the right surface for one-shot config-style round-trips, and
keeping it stable avoided touching the four `examples/protocols/serialize_*`
fixtures and any user code that depends on the existing shape.

### 2. `Byte` is a transparent alias of `Int`, not a primitive

Mid-lane course correction: PR #378 (issue #376) restored
transparent aliases for `type X = T`, so the previously-failed
`type Byte = Int` attempt referenced in `stdlib/effects.kai`
§`NetTcp` now unifies. `stdlib/core/byte.kai` is a 1-line decl
that lets the protocol surface state `[Byte]` honestly today
(instead of `[Int]`, which lies — each `Int` is 8 bytes physically,
not one octet of payload). When the post-#459 u8 lane (Lane 4)
ships a real `u8` primitive, this file flips to `pub type Byte = u8`
and every consumer migrates without touching its surface.

One quirk worth flagging: the synth'd derive impls emit `Int`
in their TypeExpr signatures, not `Byte`. The reason is pipeline
ordering — `expand_ta_decls` runs BEFORE `lower_protocols`, so by
the time the derive runs the `__pimpl_BinSerialize_Int_from_bytes`
mangled functions already have `[Int]` parameter types. Synthesising
`[Byte]` in the new impl would unify-fail against those existing
sigs. The user surface (the `Byte` alias) stays correct; only the
synth's interior types are post-expansion. When `Byte` becomes
`u8`, the synth can drop this special-case by switching to `u8`.

### 3. Polymorphic list / option impls deferred

The brief asked for "list of records" and "nested options" fixtures.
Both naturally want `impl[T : BinSerialize] BinSerialize for [T]`
and `Option[T]` — but v1 single-dispatch picks the impl from the
first argument's type, and `from_bytes(buf: [Byte], pos)` would
always pick the `[T]` impl when one exists, conflating "the buffer
is a list of bytes" with "the value being decoded is a list".
Adding the polymorphic impls would silently break dispatch for
every other type.

Worked around by: hand-rolling per-element wrappers (`pts_to_bytes`
/ `pts_from_bytes` in `binserialize_list.kai`) and by nesting
nominal sums (`MaybeMaybeInt` in `binserialize_nested.kai`).
Both fixtures demonstrate the cursor pattern explicitly so users
can crib it. A real polymorphic impl needs annotation-driven
dispatch to disambiguate buf-vs-value — same machinery the
return-type-only `from_bytes` (and `Serialize.from_string`,
`Default.default`, `From[a].from`) already needs. Tracked in §
"Follow-ups".

### 4. Real and String impls are ASCII / decimal v1

Two structural surprises during implementation:

- **No `real_to_bits` runtime primitive.** `stage0/emit.c`'s
  prelude table has `real_sqrt`, `real_pow`, `real_sin`, … but
  nothing that exposes the IEEE-754 bit pattern. Real's `to_bytes`
  goes through `real_to_string` + a 4-byte LE length prefix. Finite
  values round-trip exactly via `string_to_real`. NaN / ±Inf parsing
  is the runtime's responsibility; not exercised in the fixtures.
- **No `int_to_byte_string` runtime primitive.** The String impl's
  decoder rebuilds the result via `int_to_char` + `"#{c}"`
  interpolation. The interpolation routes through `Show for Char`,
  which emits `\xNN` for bytes outside `[32, 126]`. ASCII-only
  callers (the Phase A cache's primary use case — file paths,
  identifier names, source spans) are fine. Strings with null bytes
  also truncate at `\0` because the runtime treats `String` as a
  null-terminated C buffer (separate, unrelated bug).

Both gaps are tracked as follow-ups; both are runtime-primitive
additions, not dispatcher work, so they cleanly defer.

### 5. Sibling wrappers, not annotation-driven dispatch

`from_bytes(buf, pos)` has Self only in the return position. v1
dispatch picks the impl from the first argument's type — `buf` is
`[Byte]` (= `List`), so without an additional mechanism every
`from_bytes` call routes to a non-existent `BinSerialize for List`
impl and panics. Two ways out:

1. Add annotation-driven impl lookup (the typer reads the
   surrounding `let x : Result[String, (T, Int)] = ...` annotation
   and uses `T` to pick the impl). This is real typer work, several
   lanes worth.
2. Per-type top-level shim: `int_from_bytes`, `point_from_bytes`,
   etc. The user / generated code calls the shim directly,
   bypassing the dispatcher.

Lane shipped (2). Atomic types get hand-written shims in
`stdlib/protocols.kai`; derived types get a synthesised shim
named `<lower(tname)>_from_bytes` alongside the impl. The shim
forwards to the mangled `__pimpl_BinSerialize_<T>_from_bytes`
direct call. (1) lands when the rest of the return-type-dispatch
family — Serialize.from_string, Default.default, From[a].from —
share the same itch.

### 6. Dispatcher API: `Option[Decl]` → `[Decl]`

`derive_impl_for_protocol` previously returned `Option[Decl]`
(one impl per protocol). BinSerialize needs to emit BOTH the impl
AND the user-visible shim, so the dispatcher API now returns
`[Decl]`. Show/Eq/Hash/Ord pass through `option_to_list` to
preserve their behaviour byte-identically; no fixture needed
to change.

## Structural surprises

- **Pipeline ordering:** the discovery that `expand_ta_decls`
  runs before `lower_protocols` cost ~30 minutes — the synth'd
  impl referenced `Byte`, the existing `__pimpl_*` had `Int`, and
  the typer's "expected `[Int]`, found `[Byte]`" diagnostic
  pointed at the synth site (right) but didn't mention that
  alias expansion had already run on stdlib (the load-bearing
  fact). Captured in §2.

- **Test harness:** `examples/protocols/` runs without stdlib
  preludes, which means every fixture there has to inline its
  protocol + atomic impls. `examples/stdlib/` runs WITH preludes,
  which is the right home for BinSerialize fixtures because the
  protocol lives in `stdlib/protocols.kai`. Switched location
  mid-lane after the first fixture failed under the no-prelude
  harness.

- **Effect rows leak through helper functions.** `print` raises
  `Stdout`, so the `round_trip` helpers in
  `binserialize_nested.kai` / `binserialize_string_escapes.kai`
  / `binserialize_real.kai` need an explicit `: Unit / Stdout`
  annotation. Forgetting the annotation produces a clear
  diagnostic — not a surprise per se, just a reminder that
  fixture-helpers escape the inferred-row latitude main gets.

- **Name collision with `stdlib/collections/map.kai`.** The
  recursive-tree fixture initially named the type `Tree`, which
  shadowed `Tree[k, v]` in the AVL-map module and produced a
  cascade of "non-exhaustive match: missing TNode, TEmpty"
  diagnostics far from the source line. Renamed to `BTree`.
  Worth a memory entry — a hidden cost of the flat-prelude
  loader.

## Fixtures added

Under `examples/stdlib/`:

1. `binserialize_record.kai` — 2-field Int record. 16-byte payload,
   round-trips field-for-field.
2. `binserialize_sum.kai` — `Maybe = NothingM | JustM(Int)`.
   1-byte tag + 8-byte payload (when present).
3. `binserialize_list.kai` — list of records via hand-rolled
   wrapper helpers; demonstrates the cursor-composition pattern.
4. `binserialize_nested.kai` — `MaybeMaybeInt` (nested derived
   sums); exercises the "decode a sum whose payload is another
   derived sum" path.
5. `binserialize_string_escapes.kai` — ASCII strings with quotes,
   newlines, tabs, backslashes. Documents the non-ASCII / null-byte
   limitation inline.
6. `binserialize_recursive.kai` — `BTree = Leaf(Int) | Branch(BTree,
   BTree)`. Self-reference works because the validator whitelists
   the type's own name.
7. `binserialize_real.kai` — `Sample { id: Int, value: Real }`.
   Documents the `real_to_string` v1 fallback inline.
8. `binserialize_no_impl.kai` (`.err.expected`) — record whose
   field type lacks a BinSerialize impl. Confirms the validator
   names the offending field at typer time.

Coverage gaps: parametric types (Option[T], List[T]); Real
NaN/Inf round-trip; String with null bytes / non-ASCII; sums with
more than 256 variants (would silently truncate the tag byte).

## Real cost vs. estimate

Estimate (from brief): 1–2 days.

Actual: ~3.5 hours of agent time, conversation-driven.

The sole significant rework was the `Byte → Int` swap inside the
synth'd impls (~30 minutes), forced by pipeline ordering. Every
other surprise was a documentation-and-move-on cost (annotated in
the impl with a comment, captured here in the retro). The
existing derive_ord precedent paid for itself: most of the synth
helpers map line-for-line to the Ord chain shape (record-field
chain, sum-tag chain, validator pre-pass).

## Follow-ups

These belong on the issue tracker; the lane does not open them
itself but the integrator should consider them.

- **`real_to_bits` / `bits_to_real` runtime primitive.** Closes the
  Real impl's decimal-string fallback and gives a fixed 8-byte
  layout. Surface unchanged; the impl body shrinks.
- **`int_to_byte_string : Int -> String` runtime primitive.** Closes
  the String impl's ASCII-only limitation and the null-byte
  truncation. Trivial in stage0 (`malloc(2); s[0]=byte; s[1]=0;`
  is wrong — needs a length-tracking String type, which is a
  different lane).
- **Annotation-driven impl lookup.** Closes return-type-only
  dispatch for BinSerialize.from_bytes, Serialize.from_string,
  Default.default, From[a].from. Lets `impl[T : BinSerialize]
  BinSerialize for [T]` and `Option[T]` ship without dispatch
  ambiguity.
- **Parametric `#derive(BinSerialize)` for `T[a]`.** Same precedent
  gap as Show/Eq/Ord/Hash on parametric types. Out of scope for
  this lane.
- **u8 alias migration.** When Lane 4 ships `u8`, flip
  `stdlib/core/byte.kai` to `pub type Byte = u8` and remove the
  "synth emits Int instead of Byte" workaround in
  `make_binser_impl` / `derive_binser_shim`.
- **Phase A cache consumes the primitive.** Issue #452 sub-issue
  A.0 — the deserialise-AST step — is now unblocked.

## Limitations of this report

- Wall-clock numbers are the agent's own estimate; no shell
  history was kept.
- The "estimate vs. actual" comparison is skewed by agent-speed;
  a human authoring the same change would spend more time on the
  initial design read-through and less on each iteration.
- Tier 1 was not run locally (per the integrator's call to defer
  to CI). If CI surfaces a Tier 1-only failure, this retro should
  get an addendum.
