# String/Char codepoint model (2026-06-04) — issue #744

## Decision

kaikai's `String`/`Char` surface is realigned so the API stops lying
about bytes vs. codepoints. Adopted model: **Go's byte substrate with
Rust's naming honesty and Rust's distinct-`Char` invariant.**

Concretely:

1. The current byte-wise `chars()` is **renamed `bytes()`** — it
   already does what `bytes()` should (one `Char` per UTF-8 byte).
2. A **new `chars(s) : [Char]`** returns real Unicode codepoints,
   decoding the UTF-8 buffer.
3. `Char` stays a **distinct type** (it already is — its own
   `KAI_CHAR` runtime tag, its own `Show`, its own equality) carrying
   a **Unicode scalar value** invariant (`0..=0x10FFFF` excluding the
   surrogate range `0xD800..=0xDFFF`), enforced **only** at the two
   entry points where an invalid value can appear: `int_to_char`
   (panic on out-of-range) and `Char`-typed FFI returns. Zero
   validation cost on the decode/iterate hot path.
4. The compiler **accepts UTF-8 source**: multi-byte string *and*
   char literals (`'á'`, `'▸'`, `'\u{1F389}'`) lex to a single
   codepoint.
5. **No edition bump.** This corrects the Hanga Roa surface
   *before public adoption*, exactly the window the hanga-roa edition
   decision doc reserves for surface changes via its rollback clause.
   Shipped as `feat(stdlib)!` with a `BREAKING CHANGE` footer. Orongo
   (the 1.0 edition) is not consumed.

This is issue #744. Issue #745 (`display_width`, wcwidth-style column
count) depends on the codepoint iterator this lane provides and is a
follow-up, not part of this lane.

## Why this model (Rust vs. Go study)

kaikai is **not** choosing from a blank slate. Two facts settled the
representation before the study began:

- The runtime is **already UTF-8** internally (`as.s.bytes` +
  `as.s.len`; slice/split/length all byte-level).
- `Char` is **already a 4-byte slot** (`uint32_t as.c`,
  `stage0/runtime.h`), large enough for any codepoint.
- `docs/ffi.md` already documents `Char → int32_t (Unicode codepoint)`,
  and `stdlib/core/string.kai` already documents the current `chars()`
  as byte-wise.

So the decision is about **what the surface promises**, not the engine.

### Axis 1 — What `Char` is

| | Rust `char` | Go `rune` | kaikai (chosen) |
|---|---|---|---|
| Type | distinct, 4-byte | alias of `int32` | distinct `KAI_CHAR` |
| Invariant | scalar value, enforced on construction | none | scalar value, enforced at `int_to_char` + FFI only |

Chosen: **distinct type, Rust-shaped invariant, Go-cheap enforcement
cost.** The only constructor that can produce an invalid `Char` is
`int_to_char` (and FFI). Every other path decodes UTF-8 and is valid
by construction, so validation is concentrated at exactly those two
boundaries — never paid on the hot decode path. Collapsing `Char` to
an `Int` alias (Go) would *lose* the distinct type kaikai already has,
for no gain; the typer's ability to reject `Char`/`Int` confusion and
`Show for Char` rendering `'á'` rather than `225` are load-bearing for
Tier 2 approachability and Tier 3 LLM-authorability. Tie-breaker
"safety beats ergonomics" (Tier 1) lands on the invariant.

### Axis 2 — What `chars()` returns and how a String is indexed

Central divergence. Rust forbids `s[i]`-by-codepoint (it would be a
lying O(1)); Go exposes `s[i]`-by-byte cheaply plus a rune iterator.

Chosen: **Go substrate + Rust safety-by-absence.**
- `char_at(s, i)` / `slice` / `byte_length` stay **byte-indexed
  O(1)** — the self-hosted lexer's cursor depends on them, and Tier 1
  #2 (runtime-efficient) rewards keeping them.
- There is **no codepoint-indexing operation** — kaikai has no `s[i]`
  string sugar, so it gets Rust's "no lying O(1)-per-char" guarantee
  *for free, by absence*, with no compile-error machinery.
- The correctness path is the named iterator: new `chars()`
  (codepoints) + `char_indices()`-style helper returning
  `[(Int, Char)]` (byte-offset + codepoint) so a consumer that wants
  both doesn't pay two passes.

`bytes()` and `chars()` have genuinely different intent, satisfying
Tier 2 "few forms, each with clear intent" (not redundant forms).

### Axis 3 — Internal encoding

**UTF-8, unchanged.** No re-encoding at FFI/IO; ASCII stays 1 byte;
the runtime is already built on it. UTF-16 internal (JS) is the
cautionary model — `"𝕏".length === 2` counting UTF-16 units — and is
explicitly not copied. String equality and slicing stay byte-level
(canonically-equivalent-but-differently-normalized strings compare
`!=`; same as Rust and Go; normalization is a library concern).

### Axis 4 — `chars()` is O(n); no lazy iterator in this lane

kaikai has **no lazy sequence type** today (only strict lists), so
`chars() : [Char]` is necessarily materialized, O(n) — exactly what
Python 3 does, and Python is the Tier 2 reference. A lazy
`Iterator`/`Seq` is a **language-wide feature** (it touches the effect
system — a lazy eff=ful producer is a generator/`yield` op — and
Perceus ownership of partially-consumed iterators); designing it
*through* a String API would be the tail wagging the dog. **Deferred
to its own lane.**

Mitigation for the hot cases: ship a **non-materializing codepoint
fold/count** primitive in the runtime (decode-and-step, like Go's
`range`) so `char_count(s)` and "any codepoint is …" don't allocate an
intermediate list. `chars() : [Char]` stays for the "I want the list"
case.

### Axis 5 — Char literals

Rust semantics exactly: a char literal is a **single scalar value**.
- `'á'`, `'▸'` decode to their codepoint.
- `'\u{1F389}'` (above the BMP) valid; range `0..=0x10FFFF` minus
  surrogates. Out-of-range / surrogate `\u{...}` is a **lex error**.
- Multi-codepoint grapheme (`'👨‍👩‍👧'`) is a **lex error** — use a string
  literal. Graphemes (UAX #29) are a library concern, never a
  core-type one.
- Malformed-UTF-8 source is rejected (stricter than Go's `0xFFFD`
  tolerance; correct for *source code* per Tier 1 "safe at compile
  time").

The stage0 C lexer already decodes multi-byte char literals correctly
(`decode_utf8_char` / `decode_unicode_escape`). The real gap is the
**stage2 self-hosted lexer** (`stage2/compiler/lex.kai`), which walks
source with byte-wise `char_at` and today consumes only the first
byte of a multi-byte char literal. stage1 (`stage1/compiler.kai`) has
the same single-byte limitation and compiles stage2, so it is fixed
too.

### Axis 6 — FFI / `int_to_char` boundary

`int_to_char` stays total in its type — `(Int) -> Char` — and
**panics** on an out-of-range / surrogate argument. Tier 1 #1
explicitly sanctions `panic` as an audited runtime escape; an
out-of-range `int_to_char` is a genuine programming error (the same
class as array OOB), so panicking is honest and avoids forcing
`Option[Char]` ceremony on the overwhelmingly-common valid call (which
would hurt Tier 2/Tier 3). FFI returns typed as `Char` validate at the
boundary. This makes the Axis-1 invariant *actually true* — every
`Char` in a running kaikai program is a valid scalar value — without
paying validation on the decode hot path.

### Axis 7 — `length` honesty

| | bytes | codepoints |
|---|---|---|
| Rust | `.len()` | `.chars().count()` |
| Go | `len(s)` | `utf8.RuneCountInString` |
| Python | — | `len(s)` |
| kaikai (chosen) | `byte_length(s)`, and `length(s)` stays bytes | `char_count(s)` |

`length(s)` **stays bytes**. Rationale: it is O(1), the compiler's
slice arithmetic depends on it, and Go/Rust both make the cheap O(1)
operation the short name. `byte_length(s)` is the explicit synonym.
`char_count(s)` (the non-materializing fold from Axis 4) is the
codepoint count. The firm rule: **no name is ambiguous about its
unit.** `length`-means-bytes is defensible only because it sits in a
coherent byte-level vocabulary (`char_at`, `slice`, `byte_length`);
documented in `kai info` so it's learned once. Elixir's
`String.length` = graphemes is deliberately **not** copied.

### Axis 8 — Newcomer surprise

The chosen model lands at "Go's surface with Rust's honesty," which
serves every newcomer pool: Python users get `chars()` = codepoints
(matches iterating a `str`) and `char_count` matching their `len`
intuition; Elixir users find `bytes()`/`chars()` ≈ binary vs.
`String.codepoints`; Go/Rust users find exactly their model. The
single biggest surprise-reduction is the **rename itself** (today's
`chars()`-returns-bytes is the actual footgun); the Rust-vs-Go choice
is secondary to that for newcomer surprise.

## Surface delta

Removed/renamed:
- `string.chars(s) : [Char]` byte-wise → **`string.bytes(s) : [Char]`**.

Added:
- `string.chars(s) : [Char]` — codepoints (UTF-8 decode).
- `string.char_count(s) : Int` — codepoint count, non-materializing.
- `string.byte_length(s) : Int` — explicit byte count (synonym of
  current `length`).
- `string.char_indices(s) : [(Int, Char)]` — byte-offset + codepoint.

Unchanged (byte-level, documented as such):
- `string.length` (bytes), `char_at` (byte index), `slice`,
  `byte_at`, `starts_with`/`ends_with`/`drop_prefix`/`drop_suffix`,
  `split`/`join`/`replace`/`lines`, `pad_left`/`pad_right`, `trim`,
  `is_blank`.

Compiler:
- stage2 + stage1 char-literal lexing decodes a full codepoint
  (validate range, reject surrogate / multi-codepoint).
- `int_to_char` panics on out-of-range / surrogate.

## Migration / blast radius

Edition: **none** (pre-adoption Hanga Roa correction; `feat!` +
`BREAKING CHANGE` footer).

`.chars()` callers in the tree (8 sites, almost all fixtures) split
into two cases — bytes-intent stays byte-intent (rewrite to `bytes()`),
codepoint-intent moves to the new `chars()`:
- `examples/stdlib/string_lines_chars.kai` (+ golden) — fixture,
  update to exercise both `bytes()` and `chars()`.
- `examples/stdlib/string_compose.kai` — `length(chars(id))` counts
  ASCII, equivalent under either; pin to `byte_length`/`char_count`
  explicitly.
- `demos/9d9l/huffman/main.kai` — Huffman over `chars(text)` counts
  byte frequency; this is **byte**-intent (it reconstructs the byte
  stream), rewrite to `bytes()`.

New fixtures (regression discipline): a positive fixture with
non-ASCII input exercising `bytes()` vs `chars()` vs `char_count`
divergence (`"áé▸"`: bytes=7, chars=3, first codepoint=225), and a
negative fixture for an out-of-range `int_to_char` / surrogate char
literal (`.err.expected` golden).

Docs to update at close: `docs/ffi.md` (confirm `Char`=u32 codepoint +
invariant policy), `stdlib/core/string.kai` doc comments, `kai info`
strings page, `docs/stdlib-layout.md` / `docs/stdlib-roadmap.md`
catalog rows.

## Alternative considered (rejected)

**`Char` = transparent alias of `Int` (Go `rune`)**: no invariant,
`int_to_char`/FFI no-op, invalids render U+FFFD. Lighter (no invariant
machinery, less typer work) but loses the distinct type kaikai already
has, lets the typer accept `Char`/`Int` confusion, and pushes
validation to every consumer. Rejected under "safety beats
ergonomics": spending the breaking change to *remove* type safety is
the wrong direction.

## Related

- Issue #744 (this), #745 (`display_width`, dependent follow-up).
- `docs/decisions/edition-hanga-roa-2026-05-16.md` — the pre-adoption
  surface-change window this lands in.
- `docs/ffi.md` — `Char ↔ int32_t` contract.
