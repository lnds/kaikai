# The `Layout` kind — declarative binary representation

**Status:** shipped. `Layout` is a kind over the public `Composition`
theory (`stdlib/core/kinds.kai`). The kind, its `<be>`/`<le>` surface, the
byte-order habitants, and the atomic-habitant formation guard are live;
`U32<be>` and `U32<le>` classify as distinct representations that never
unify. This doc is the design and the motivation; where a piece is a
declared follow-up rather than shipped, the section says so.

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

Two operations over Layout-bearing records:

```kaikai
encode(value)  : Array[Byte]           # total — always serialises
decode(bytes)  : Option[T]             # None when the buffer is too short
```

**The signature is `encode[t](value: t)` / `decode[t](bytes) : Option[t]` over an
ordinary `[t: Type]`, NOT `[l: Layout]`.** This is deliberate and load-bearing: the
`Layout` kind classifies *habitants* (`be`/`le`), not record types. `Header` is a
plain `Type` whose *fields* carry Layout habitants. A `[l: Layout]` bound would make
`l` a habitant variable (the thing that goes in `<l>`), not a type usable in
`Option[l]` — and a kind bound over a type is exactly the constraint-propagation the
language rules out (CLAUDE.md Tier 1 #3). Instead the compiler checks a concrete
call's target *is* Layout-bearing (every field carries `<be>`/`<le>`) and rejects it
otherwise; the same phantom-habitant shape `unit_name` uses. Do not "fix" the
signature back to `[l: Layout]`.

`T` comes from the call's type context — an annotation or the surrounding
expression:

```kaikai
fn parse_header(buf: Array[Byte]) : Result[Header, ParseError] = {
  let decoded : Option[Header] = decode(buf)
  match decoded {
    Some(h) if h.magic == 0xCAFEBABE -> Ok(h)
    Some(h)                          -> Err(BadMagic(h.magic))
    None                             -> Err(Truncated)
  }
}
```

`decode` returning `Option` is the safety story: too-short input yields `None`,
never a garbage read. Truncation and bad-magic are ordinary `Result` values, no new
match machinery — the layout is in the type, the match is `match`.

### How it lowers

For each Layout-bearing record the compiler synthesises `__layout_encode_T` /
`__layout_decode_T` (before the typer, same slot as refinement predicates), and a
post-inference pass redirects each concrete `encode`/`decode` call to them. The
generated bodies are ordinary kaikai — a fold of `bin.put_uint_be/le` /
`bin.get_uint_be/le` over the fields in declaration order — so **both backends (C
and native) lower them for free, byte-identical, with no per-backend codegen.** The
width comes from the base type (`U32` = 4 bytes); the `<be>`/`<le>` habitant picks
the byte order. `encode` folds an `Array[Byte]`; `decode` is a match tower that
short-circuits to `None` on any truncated read and rebuilds the record at the
innermost success.

## Data-dependent size (TLV) — declared follow-up, not shipped

> **Status:** the shipped cut is byte-aligned fixed-width fields only. TLV
> (`Bytes<len>`) and nested Layout records are a **follow-up**: a record with any
> field that is not a fixed-width `U8..U64` carrying `<be>`/`<le>` is not
> Layout-bearing, so `encode`/`decode` reject it up front rather than mis-encode it.
> `Bytes<len>` also fails earlier — `len` is not a `Layout` habitant, so it is an
> unknown-unit error. The design below is the intended shape for that follow-up.

This is the axis Elixir wins with `payload::binary-size(count)`. A field's byte
length may refer to a **prior field of the same record**, read first in the
sequential decode:

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
| `Type`, `Measure`, `Currency`, `Region` kinds | shipped (`docs/kinds.md`) |
| `Composition` theory (public, user-declarable) | shipped |
| `Layout` kind + `<be>`/`<le>` habitants + formation guard | shipped |
| `encode`/`decode` over fixed-width fields (C + native, byte-identical) | shipped |
| TLV (`Bytes<len>`), nested Layout records, signed heads, sub-byte | follow-up (rejected, not mis-encoded) |
| imperative predecessor | `stdlib/bin.kai` + `bin_*` in `stdlib/protocols.kai` (`encode`/`decode` compose the new `bin.put_uint_*` / `bin.get_uint_*` bricks) |

## Resolved during implementation

- **`decode`/`encode` are free functions**, not a protocol — protocol dispatch is on
  a value, and a layout is a property of the *type* with no value to dispatch on;
  that is exactly what a kind, not a protocol, is for.
- **The signature is over `[t: Type]`, not `[l: Layout]`** — see *Operations*: a kind
  bound over a type is constraint-propagation the language forbids; `Layout`
  classifies habitants, and the emitter checks the concrete target is Layout-bearing.
- **Un-annotated fixed-width fields are not Layout-bearing** (Boundary 3): a record is
  Layout-bearing only when *every* field carries `<be>`/`<le>`. An un-annotated `U32`
  makes the record plain, so `encode`/`decode` on it are rejected — no implicit
  host-endian default.

## References

- `docs/kinds.md` — the two-kind system and the bar for a third.
- `docs/units-of-measure.md` — the `Measure` kind, the syntax `Layout` mirrors.
- `docs/unsafe-systems-design.md` — the raw `reinterpret` alternative this
  supersedes for structured formats, and the systems frame.
- `stdlib/bin.kai`, `stdlib/protocols.kai` (`bin_*`) — the imperative surface a
  `Layout`-based `decode`/`encode` would replace.
