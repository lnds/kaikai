# The `Layout` kind — declarative binary representation (design proposal)

**Status:** proposal, not accepted. Design doc for a candidate third kind, written
per `docs/kinds.md` ("adding a new kind requires a separate design doc … that
proposal must motivate why the existing two are insufficient and what concrete
program patterns the new kind enables"). This doc is that motivation.

**Scope of this spec.** The kind, its surface syntax, `decode`/`encode`, and —
added after design review — **data-dependent size (TLV)** via intra-record backward
dependency, which is the one axis where Elixir's bit-syntax currently wins. Two
things remain **out of scope**: (1) *bit-level sub-byte fields* (3-bit / 5-bit
packing) — deliberately conceded, byte-aligned only (see "Byte-aligned boundary");
(2) *bitwise infix operators* — orthogonal, its own follow-up. Noted in
"Boundaries" and the tracking issue.

**Why this beats Elixir (the frame that shaped the scope).** Elixir's `<<>>`
bit-syntax is powerful but **flat, untyped, non-composable, no encode/decode
guarantee, no effects**. kaikai wins not by copying `<<>>` better but by putting
binary formats *inside the type-and-effect system* — which Elixir structurally
cannot do. The four wins, in order of leverage:

1. **TLV via intra-record backward dependency** — closes the one axis Elixir wins
   (see "Data-dependent size"). Same power, but the result is a *typed* record.
2. **Composable / nested layouts** — a field can be another Layout-bearing type;
   `decode` composes. Elixir flattens sub-structures into one giant `<<>>` by hand.
3. **Derived encode from the same Layout** — one type declares the representation
   once; `decode`/`encode` both derive from it, *impossible to desync*. Elixir
   writes parser and serializer separately.
4. **Effects in the decode signature** — `decode[T] : T / Read + Fail` puts
   streaming and failure in the signature, composable with the rest of the effect
   system. Elixir has no typed effects.

Bit-level sub-byte is conceded to Elixir: rare, and it would degrade Layout from
"a typed view over bytes" (phantom, clean) into "a bit-packing engine". The rare
sub-byte case is handled by reading the whole byte-aligned field with Layout, then
extracting bits with the bitwise ops — the same division of labour throughout:
Layout for the byte layout of fields, bitwise ops for the interior of one field.

## Motivation — why a kind, not a library

Binary formats (network protocols, file headers, wire encodings) need per-field
physical layout: width, endianness, signedness. Today kaikai expresses this as
imperative byte-shuffling — `bin_int_to_bytes`, hand-written `bit_shl`/`bit_or`
chains in `stdlib/crypto/hash.kai`, `bin.bytes_to_int` cursors. Every format is
re-implemented by hand, unsafely (an off-by-one in the cursor reads adjacent
memory) and illegibly.

The alternative that was considered and **rejected** is a raw-memory
`reinterpret[T]` (`docs/unsafe-systems-design.md`). It cannot express two things
every real format needs — declared endianness (it assumes host layout) and
data-dependent size — and it is `/Unsafe`. The declarative alternative is to make
*layout a property of the type*, verified at compile time, with a safe
`decode`/`encode` that reads the layout. That property is a **kind**.

## Why `Layout` is a kind (the three tests)

`docs/kinds.md` sets the bar: classify types + own unification algebra +
polymorphism `Type` cannot express. `Layout` passes all three, the same way
`Measure` does.

1. **Classifies types.** `U32<be>` and `U32<le>` are the same base type `U32` but
   distinct *representations* — incompatible without an explicit conversion, exactly
   as `Real<m>` and `Real<s>` are the same `Real` but distinct dimensions. Layout is
   orthogonal to what the value *is*; it is how the value is *laid out in bytes*.

2. **Own algebra.** A record's layout is the composition of its fields' layouts;
   its size is the sum of field sizes; alignment/padding compose by rule. This
   composition is not the structural unification of `Type` — it is a size-and-order
   algebra of its own, the layout analogue of `Measure`'s abelian group.

3. **Polymorphism `Type` cannot express.** `fn decode[l: Layout](bytes: Array[U8])
   : Option[l]` is generic over layouts — one decoder that works for any layout-
   bearing type. `[t: Type]` cannot express "any type that has a byte layout"; a
   protocol cannot either (a protocol dispatches on a value, layout is a property of
   the type with no value to dispatch on). This is the concrete program the kind
   enables and nothing else does.

## Relationship to `Measure` — both phantom, one parametrizes codegen

The load-bearing clarification (this corrected an earlier draft that called
`Layout` inhabitants "executing"):

**At the value level, `Layout` is phantom exactly like `Measure`.** The inhabitant
`be` is erased at runtime just as `m/s` is. `U32<be>` is, at runtime, a bare
`U32` — the `<be>` is a compile-time type-level value, not a runtime tag. There is
no boxed layout, no per-value cost, nothing carried. Both kinds vanish from the
runtime representation of a value.

The one real difference is **how the compiler consumes the kind**:

| | `Measure` | `Layout` |
|---|---|---|
| Value-level presence | phantom (erased) | phantom (erased) |
| Type-check role | verifies dimensions match | verifies field is decodable |
| Codegen role | **none** — never affects emitted code | **guides codegen** — `<be>` on a little-endian host emits a byte-swap; `<le>` does not |
| Runtime value cost | zero | zero |

So it is not "phantom vs executing". Both are phantom. `Measure` only *verifies*;
`Layout` verifies *and parametrizes codegen*. That second role is ordinary — a
type's size already parametrizes codegen (`Array[U8]` vs `Array[U64]` emit
different strides) without making `[T]` non-generic. Layout is type information
that informs representation, which is what compilers do; it does not strain the
kind model. **What executes is the `decode`/`encode` operation that *reads* the
layout, never the layout inhabitant itself** — precisely as a `convert` fn executes
while the `<m/s>` it consults does not.

## Surface syntax — falls out of UoM, no new delimiter

Because `Layout` is a kind, the syntax is the UoM syntax: angle brackets on a
numeric head. **No `bytes< … >` delimiter** (rejected: `bytes` is already a stdlib
function — `random_secure.bytes`, all of `stdlib/bin.kai` — so the token would mean
two things by position, the `Unit`/`Unit` collision `docs/kinds.md` §"three things
called Unit" documents).

```kaikai
# UoM precedent
let g : Real<m/s^2> = 9.81<m/s^2>

# Layout, identical shape
type Header = {
  magic:   U32<be>,     # big-endian, network order
  version: U8,          # single byte — no layout annotation needed
  id:      I64<le>,     # little-endian, signed
}
```

A field with no `<…>` is host-native single-byte or the default width layout (an
open decision — see Boundaries). Layout annotations apply only to fixed-width
integer heads (`U8..U128`, `I8..I128`), mirroring UoM's "numeric heads only" rule
(`U32<be>` yes; `String<be>` a parse error).

### Layout inhabitants (the fixed set for this spec)

| Inhabitant | Meaning |
|---|---|
| `be` / `le` | byte order: big- / little-endian |
| (width is the head) | `U16<be>` is 2 bytes; the integer type fixes the width |

That is the whole set for v1. `packed`, explicit alignment, bit-level fields
(sub-byte widths), and float layouts are deliberately deferred — add them only when
a real format needs them, the same discipline the raw layer follows.

## Operations — `decode` / `encode`, safe, no `Unsafe`

Two polymorphic operations over layout-bearing types, both total (no UB, no
`/Unsafe`):

```kaikai
decode[l: Layout](bytes: Array[U8]) : Option[l]         # None if bytes too short
encode[l: Layout](value: l)         : Array[U8]         # always succeeds
```

Then binary parsing is **ordinary record destructuring** of a decoded value — the
`{ magic, version, id }` pattern that already exists, no special "binary pattern"
form:

```kaikai
fn parse_header(buf: Array[U8]) : Result[Header, ParseError] =
  match decode[Header](buf) {
    Some(h) if h.magic == 0xCAFEBABE -> Ok(h)
    Some(h)                          -> Err(BadMagic(h.magic))
    None                             -> Err(Truncated)
  }
```

`decode` returning `Option` is the safety story: too-short input yields `None`,
never a garbage read. Truncation and bad-magic are ordinary `Result` values. This
is safer than Rust's `nom` (which needs `unsafe` for zero-copy) and needs no new
match machinery — the layout is in the type, the match is `match`.

### Codegen sketch

`decode[Header]` monomorphises per concrete layout (like every kaikai generic).
For each field the compiler emits a width-sized load from the running offset, plus
a byte-swap iff the field's endianness differs from the host's, then a bounds check
against `bytes.length()` up front (one check for the fixed total size). No cursor
object, no per-field bounds check, no allocation beyond the result record. `encode`
is the mirror. Both are straight-line code the optimizer handles well.

## Data-dependent size (TLV) — IN scope, via intra-record backward dependency

This is the axis Elixir wins with `payload::binary-size(count)`, and Layout closes
it. A field's byte length may refer to a **prior field of the same record**, read
first in the sequential decode:

```kaikai
type Packet = {
  len:  U16<be>,           # read first
  body: Bytes<len>,        # its length is the already-read `len`
}
```

`Bytes<len>` is not a *pure* static Layout — its size depends on a value. But the
dependency is narrow and decidable: **backward, intra-record, in declaration
order.** During the sequential `decode`, `len` is bound before `body`'s layout is
resolved, exactly as Elixir binds `count` before `payload`. This is **not general
dependent types** (indecidable, ruled out) — it is a value already in scope from an
earlier field, referenced by name. The typer checks that `<len>` names a prior
field of an integer type; the codegen reads `len`, then reads `len` bytes into
`body`, bounds-checked.

Why this beats Elixir on the same case: the result is a **typed `Packet`** — `body`
is a checked `Array[U8]` field, `len` a checked `U16`, and a typo `p.ln` is a
compile error. Elixir's `<<len::16, body::binary-size(len)>>` gives untyped
bindings and a runtime `MatchError` on a bad length; `decode[Packet]` returns
`Option[Packet]` (or `/ Fail`), failing safely, statically typed.

Restriction (keeps it decidable): the reference is a **plain prior field name** (or
a simple arithmetic expression over prior fields, e.g. `Bytes<len - 4>`), never a
forward reference, never a function call, never a value outside the record. Anything
richer is a named smart-decoder, not a Layout.

## Byte-aligned boundary (bit-level sub-byte — conceded)

Layout is **byte-aligned**: annotations attach to fixed-width integer heads
(`U8..U128`), each a whole number of bytes. Bit-level sub-byte fields (a 3-bit flag,
a 5-bit code, as in `<<x::3, y::5>>`) are **out of scope, conceded to Elixir**:
rare outside a few niches (TCP/IP headers, some codecs, hardware formats), and
supporting them would degrade Layout from a phantom typed-view-over-bytes into a
bit-packing engine — losing the elegance that distinguishes it. The rare sub-byte
case reads the whole byte-aligned field with Layout, then extracts bits with the
bitwise ops.

## Boundaries (explicit non-goals of this spec)

1. **Bit-level sub-byte fields** — conceded, byte-aligned only (above).

2. **Bitwise infix operators / scope.** Independent of `Layout`, but binary code is
   bit-twiddling and kaikai's bit ops are intrinsic *functions* (`bit_and`,
   `bit_shl`, …) not operators. Its own follow-up
   (`docs/bitwise-scope-design.md` / `stdlib/math/bits.kai` UFCS), tracked
   alongside so both gate legible binary code together.

3. **Default-width layout of an un-annotated field.** Whether `id: U32` (no
   `<…>`) means host-endian, big-endian-by-convention, or a parse-requires-
   annotation is an open call, resolved at implementation.

## Component status

| Piece | Status |
|---|---|
| `Type`, `Measure` kinds | shipped (`docs/kinds.md`) |
| kind machinery (parser `parse_optional_kind_annotation`, separate ID namespace) | exists for `Measure`; `Layout` extends the same slot |
| `Layout` kind + `<be>`/`<le>` inhabitants | new |
| `decode`/`encode` codegen | new |
| imperative predecessor to migrate | `stdlib/bin.kai` + `bin_*` in `stdlib/protocols.kai` |

## Open questions for evaluation

- Is `Layout` worth a third kind now, or does the imperative `bin.*` surface
  suffice until a real format-heavy program (a protocol impl, a file parser) exists
  to justify it? (The same "wait for a measured case" discipline the raw layer got.)
- Should `decode`/`encode` be free functions, or methods behind a `Serialize`-style
  protocol keyed on the `Layout`? (Protocol dispatch on a *type property* with no
  value is exactly what a kind, not a protocol, is for — leaning free functions.)
- Default-width layout of an un-annotated integer field (Boundary 3).

## References

- `docs/kinds.md` — the two-kind system and the bar for a third.
- `docs/units-of-measure.md` — the `Measure` kind, the syntax `Layout` mirrors.
- `docs/unsafe-systems-design.md` — the raw `reinterpret` alternative this
  supersedes for structured formats, and the systems frame.
- `stdlib/bin.kai`, `stdlib/protocols.kai` (`bin_*`) — the imperative surface a
  `Layout`-based `decode`/`encode` would replace.
