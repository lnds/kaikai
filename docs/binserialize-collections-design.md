# BinSerialize for collections — combinator design

> **v1 status (2026-05-11):** the buffer carrier described in this
> document as `[Byte]` has shipped as `Array[Byte]` (PR #487, follow-up
> to closed PR #485). Every signature in this doc that mentions
> `[Byte]` now reads `Array[Byte]` in `stdlib/protocols.kai`. The
> protocol shape, the derive machinery, and the combinator design
> below are unchanged — only the buffer type. `bin_byte_at(buf, pos)`
> is now an O(1) `array_get` rather than a list walk; that closes the
> O(N²) gap reported in #485. See
> `docs/lane-experience-binserialize-array-buf.md` for the substrate
> retro.

A pre-blocker lane for Phase A.0 stdlib cache (#452). PR #471 shipped
`BinSerialize` with cursor semantics and the `#derive(BinSerialize)`
machinery for records and sums. The protocol covers `Int`, `Bool`,
`String`, `Real` and the derive validator accepts user-defined types
that recursively rely on those primitives.

The catch: the AST types we want to serialise for the cache layer
(`Decl`, `Expr`, `Pattern`, `TypeExpr`, …) carry `[T]` lists,
`Option[T]` fields, and `Char` payloads everywhere. None of those
three have a `BinSerialize` impl today, and one of them (`[T]`)
cannot have a useful single-dispatch impl at all. This lane closes
the gap so the cache lane can apply `#derive(BinSerialize)` to the
AST.

## What blocks a direct impl

The protocol dispatcher resolves the impl from the head type of the
first argument via `proto_type_name`:

```
proto_type_name(TyList(_))        -> "List"
proto_type_name(TyName("Option", _)) -> "Option"
proto_type_name(TyName("Char", _))   -> "Char"
```

Both `[T]` and the `[Byte]` buffer collapse to `"List"`. A single
global `impl BinSerialize for List` would have to:

- pick the right element encoder/decoder at runtime — impossible
  because `from_bytes(buf, pos) : Result[String, BinCursor[Self]]`
  takes no `T`-aware argument; `Self` is just `List` with no element
  info; and
- co-exist with the buffer's own list-of-Byte head, which the same
  dispatch table is already trafficking for arithmetic on `pos`.

`Option[T]` has the same shape — one global impl cannot carry the
element type. `Char` is simpler (no parameter), but adding it would
expand the protocol's whitelist and force a runtime `kai_op_*` entry
that mostly delegates to `Int`. The current cluster of primitive impls
(`Int`/`Bool`/`String`/`Real`) is the minimum the cache lane can lean
on; any extension that grows the whitelist needs to keep the dispatch
table small.

## What works: derive-time inlining

`derive_binserialize_impl` generates the `to_bytes` / `from_bytes`
bodies *at compile time*, with full access to each field's
`TypeExpr`. When the field is a `[T]` or `Option[T]` or `Char`, the
derive emits a call to a free combinator function instead of a
protocol dispatch. The element type's encoder/decoder is supplied
inline as a lambda — its mangled name is computable from the
element's head type, exactly as the record-field chain does today.

Surface:

```kai
fn bin_list_to_bytes[t](xs: [t], enc: (t) -> [Byte]) : [Byte]
fn bin_list_from_bytes[t](buf: [Byte], pos: Int,
                            dec: ([Byte], Int) -> Result[String, BinCursor[t]])
                          : Result[String, BinCursor[[t]]]

fn bin_option_to_bytes[t](o: Option[t], enc: (t) -> [Byte]) : [Byte]
fn bin_option_from_bytes[t](buf: [Byte], pos: Int,
                              dec: ([Byte], Int) -> Result[String, BinCursor[t]])
                            : Result[String, BinCursor[Option[t]]]

fn bin_char_to_bytes(c: Char) : [Byte]
fn bin_char_from_bytes(buf: [Byte], pos: Int) : Result[String, BinCursor[Char]]
```

These live in `stdlib/protocols.kai` next to the existing `bin_*`
helpers. They are *not* protocol impls — no `impl BinSerialize for
List` is declared, so the dispatcher's two-impl conflict never arises.

### Wire format

- **List**: 4-byte little-endian length prefix + concatenation of
  per-element encodings. Same length encoding as `String`. Empty list
  is four zero bytes.
- **Option**: 1-byte tag (`0` = None, `1` = Some) + payload if Some.
  None encodes to a single byte; Some encodes to tag + recursive
  payload.
- **Char**: 4 bytes little-endian unsigned (the `Int` representation
  via `char_to_int`). Aligns with how `Char` is treated everywhere
  else in the runtime.

### Derive integration

`derive_binser_record_to_bytes_chain` walks the field list and emits
a `to_bytes(self.field)` call per field. The extension inspects
`field.ftype.tkind`:

| `tkind` | encode emits | decode emits |
|---|---|---|
| `TyName(X, _)` X ∈ {Int, Bool, String, Real, Char} | `to_bytes(self.f)` / `bin_char_to_bytes(self.f)` | `__pimpl_BinSerialize_X_from_bytes(buf, pos)` / `bin_char_from_bytes(...)` |
| `TyName(X, _)` X user-defined (derived) | `to_bytes(self.f)` | `__pimpl_BinSerialize_X_from_bytes(buf, pos)` |
| `TyList(t)` | `bin_list_to_bytes(self.f, { x -> <encode_expr(t)> })` | `bin_list_from_bytes(buf, pos, { b, p -> <decode_call(t)>(b, p) })` |
| `TyName("Option", [t])` | `bin_option_to_bytes(self.f, { x -> <encode_expr(t)> })` | `bin_option_from_bytes(buf, pos, { b, p -> <decode_call(t)>(b, p) })` |
| `TyName("Char", _)` | `bin_char_to_bytes(self.f)` | `bin_char_from_bytes(buf, pos)` |

`encode_expr(t)` and `decode_call(t)` are recursive over `t`:
nested `TyList(TyList(Int))` produces

```kai
bin_list_to_bytes(self.f, { x -> bin_list_to_bytes(x, { y -> to_bytes(y) }) })
```

The same shape applies inside sum variant payloads — the
`derive_binser_sum_*` functions get the equivalent treatment.

### Validator

`validate_derive_binserialize` rejects fields whose head type lacks an
impl. The extension:

- adds `"Char"` to the built-in whitelist;
- when the head is `"List"`, recurses into the element `TypeExpr`
  and validates it (so `[[Foo]]` requires `Foo` to provide
  `BinSerialize`);
- when the head is `"Option"`, same recursion.

The recursion keeps the validator's "no impl for X" diagnostic
honest: a field of `[FooNoImpl]` reports `FooNoImpl` as the missing
provider, not `List`.

## Alternatives rejected

### Alt A: `impl BinSerialize for List` with type-tag prefix

Prefix every list encoding with a tag identifying the element type,
then dispatch at decode time on that tag. Rejected because:

- requires every element type to register a globally-unique tag,
  which is new global mutable state;
- protocol single dispatch still can't pick the right decoder from
  a tag value alone — it needs a `T`-aware entry point; and
- two list values of different element types couldn't share the same
  `from_bytes` symbol without giving up static guarantees.

### Alt B: newtype `ByteVec` wrapping `[Byte]`

Disambiguates the buffer from a `[T]` payload by wrapping the buffer
in `ByteVec`. Rejected because it forces every existing call site
(`bin_byte_at`, `bin_have`, the protocol surface itself) to switch
from `[Byte]` to `ByteVec`. That is the kind of cascade #471 already
absorbed once during the `u8 → Byte` rename (#476); doing it again
for `ByteVec` adds churn without solving the parametric-element
problem for `Option[T]`.

### Alt C: redesign the protocol with explicit element decoder

Add a third parameter to `from_bytes` carrying the element decoder.
Rejected because it breaks the protocol's `Self`-only signature
contract, breaks every existing impl, and contradicts the design
doc's "single-dispatch protocols (`O(1)` impl-table lookup)"
principle. The combinator approach above preserves the protocol's
existing shape entirely.

### Alt D: hand-write serialisers per AST node

Sketched at ~1500-3000 LOC in `stage2/compiler.kai`. Rejected: the
derive infrastructure already exists and ships byte-identical output
across re-derivation, which a hand-written set of 30 functions
would not maintain across AST evolutions without diligent review.
The combinator extension is ~80 LOC of stdlib + ~200 LOC of derive
extension, total cheaper than the manual path and far more
maintainable.

## Recursion limits

`TyRefine(base, _)` and `TyDim(base, _)` collapse to their base in
`proto_type_name` and the derive treats them transparently — same as
existing primitive dispatch. `TyFn(...)` is not serialisable
(closures don't fit a content-addressable cache, and AST never
embeds function values), so the derive rejects with the existing
"no impl for `Fn`" message. That stays correct.

`TyDim(TyName("Char", _), _)` — a unit-annotated `Char` — would
collapse to head `"Char"` and try `bin_char_to_bytes`. There is no
such type in practice (units only annotate `Int`/`Real`), but the
fall-through is harmless.

## Out of scope

- Caching of any kind. This lane is the pre-blocker for Phase A.0,
  not the cache itself. The cache (`.kab` files, atomic writes,
  driver wiring, header validation, wall-time bench) ships in the
  next lane (#452).
- Other parametric protocols (`Show[T]`, `Hash[T]`). They have the
  same dispatch shape but no immediate caller — re-revisit when the
  next consumer surfaces.
- Adding `Char` as a first-class `BinSerialize` impl in the
  protocol's dispatch table. The free-function `bin_char_*` is
  enough for the cache use case and keeps the dispatch table small.
- 256+-variant sum types with multi-byte tags. The existing derive
  caps at 256; that limit is independent of this lane.

## Acceptance

- Existing `Int`/`Bool`/`String`/`Real` impls and the existing
  `#derive(BinSerialize)` for primitive-only records and sums must
  remain byte-identical (`make selfhost` is the gate).
- New positive fixtures cover: `[Int]`, `[String]`, `[[Int]]`,
  `Option[Int]`, `Option[String]`, `Char`, sum variants with
  `[T]` and `Option[T]` payloads.
- New negative fixture: `[FooNoImpl]` reports the inner type as
  missing, not the outer `List`.
- `make tier0` + `make tier1` green.
- Phase A.0 (next lane) can apply `#derive(BinSerialize)` to the
  full AST type tree (`Decl` and friends) without further protocol
  changes.
