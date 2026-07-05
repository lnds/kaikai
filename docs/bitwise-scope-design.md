# Bitwise scope — `` `expr` `` for legible bit manipulation (design proposal)

**Status:** proposal, not accepted. Companion to the `Layout` kind
(`docs/layout-kind-design.md`): `Layout` reads/writes struct *fields* declaratively;
this reads/writes *bits within a value* imperatively. Together they are the two
halves of the binary story.

## The problem

kaikai's bitwise ops are intrinsic *functions*, not operators: `bit_and`, `bit_or`,
`bit_xor`, `bit_not`, `bit_shl`, `bit_shr`, `bit_ushr`. Dense bit expressions become
unreadable — a SHA-256 line in `stdlib/crypto/hash.kai`:

```kaikai
bit_or(bit_or(bit_shl(b0, 24), bit_shl(b1, 16)), bit_or(bit_shl(b2, 8), b3))
```

is `(b0 << 24) | (b1 << 16) | (b2 << 8) | b3` everywhere else. `crypto/hash.kai` is
among the hardest stdlib files to read *for this reason alone*.

The obvious fix — add infix bitwise operators globally — is rejected: the natural
symbols collide. `|` is **already** both the sum-type separator (`Red | Green`) and
the pipe (`xs | double`); `&` is unused but `and` is the logical connective. Adding
global `&`/`|`/`<<` operators either clashes or forces ugly triples (`&&&`, `<<<`),
paying permanent global surface for a domain that is rare outside systems code.

## The idea — a delimited bitwise sub-expression

Inside the scope, the natural symbols recover their bitwise meaning; outside, they
do not exist as operators and collide with nothing (spelling ``b`…` `` is the
current lean, not final — see below):

```kaikai
let bneg = b`x & 0xFF << 8 | 0x80`
let w    = b`b0 << 24 | b1 << 16 | b2 << 8 | b3`
let hi   = b`flags & 0x80`
```

The backtick is a **lexical mode**, a sigil for "bitwise expression" — the same
device as Elixir's sigils (`~w`) or a quasi-quote. The delimiter is what lets `|`
be the pipe outside and bitwise-OR inside with zero ambiguity: the grammar you are
in is named by the delimiter. Backticks are free in kaikai (they appear only in doc
Markdown, never in the lexer), so the sigil is available.

### Sigil spelling — leaning ``b`…` `` (not final)

The exact prefix is **left open** for a dedicated syntax decision, but the current
lean is a **prefixed** form ``b`…` `` (`b` for *bits*/*binary*) over the bare
``` `…` ```, for three reasons:

- **Unambiguous open.** Bare ``` `…` ``` forces the lexer to decide "am I in a
  scope?" only after seeing content that might be a pipe. ``b`…` `` is
  inconfusable from the first character — the mode is known before the content.
- **Established pattern.** Prefix-letter-before-delimiter is the universal sigil
  convention: Rust `b"…"`/`r"…"`, Elixir `~w(…)`/`~r/…/`. `b` is mnemonic and short
  (bit code is dense; the prefix recurs).
- **Keeps the bare backtick free** for a future generic quasi-quote or nothing —
  does not burn the most neutral delimiter on the first use.

Candidates to weigh at decision time: ``b`…` `` (shortest), ``bit`…` ``,
``bits`…` `` (most explicit). One collision to check together: if kaikai ever wants
a byte literal `b'A'` (Rust-style), confirm `b\`` and `b'` do not clash in the
lexer (different delimiters, but decide jointly).

Contained complexity is the whole point: the bitwise grammar lives *inside* the
backtick, and the rest of the language does not know it exists. This suits kaikai —
bit-twiddling is rare in application code, common only in systems/binary work (the
`Layout` kind's domain). A scope that appears only when needed beats global
operators serving the 5% of code that twiddles bits.

## Operators inside the scope

| Symbol | Meaning | Lowers to |
|---|---|---|
| `&` | bitwise AND | `bit_and` |
| `\|` | bitwise OR | `bit_or` |
| `^` | bitwise XOR | `bit_xor` |
| `~` | bitwise NOT (unary) | `bit_not` |
| `<<` | shift left | `bit_shl` |
| `>>` | arithmetic shift right | `bit_shr` |
| `>>>` | logical (unsigned) shift right | `bit_ushr` |

Operands are `Int` and the fixed-width integer types (`U8..U128`, `I8..I128`).
Literals (`0xFF`, `0x80`, decimals) and bound names are allowed; the scope is a pure
expression — no statements, no calls beyond the operators (a `bit_rotate` need is a
plain fn outside). The backtick desugars to the same `bit_*` intrinsics the compiler
already lowers, so there is **no new codegen** — only new parsing.

## Precedence — FLAT, left to right (decided)

The scope has **no precedence levels**. Operators associate strictly left to right:

```kaikai
`x & 0xFF << 8 | 0x80`   ==   `((x & 0xFF) << 8) | 0x80`
```

This deliberately **differs from C**, where `<<` binds tighter than `&` binds
tighter than `|`. C's bitwise precedence is a notorious bug source (`&` lower than
`==`); a language that rejects `1 + "hola"` must not silently accept C's surprising
grouping. Flat left-to-right is predictable with one rule and no memorised table.

The one honest cost: **copying a constant expression from a C header can regroup.**
A C `a & MASK << n` means `a & (MASK << n)`; here it means `(a & MASK) << n`. Mitigation
is the same discipline C programmers already use — parenthesise. The scope makes the
grouping visible and rule-based rather than table-based; it does not hide it.

Parentheses group inside the scope as expected: `` `(a | b) & c` ``.

## Relationship to the `Layout` kind

`Layout` (declarative, `docs/layout-kind-design.md`) extracts struct *fields* from
bytes: `decode[Header](buf)` → `h.magic`, `h.flags`. The bitwise scope (imperative)
inspects *bits within a field*: `` `h.flags & 0x80` ``. A real codec uses both —
`decode` for the field layout, the scope for the bit masks. They are complements,
not alternatives, and together replace the imperative `bin_*` + raw `bit_*` surface.

## Supersedes the #1064 bitwise follow-up

Issue #1064 (`Layout` kind) noted a follow-up: "Elixir-style infix triples `<<<`
`>>>` `&&&` `|||` `^^^`". **This scope replaces that** — it gives legible bit code
without global operators, without colliding with pipe/sum-types, contained to the
domain. The triples proposal is dropped.

## Open questions

- Does the scope nest or compose with normal expression position — is `` `x & m` `` a
  first-class `Int`-typed expression usable anywhere (`f(`x & m`)`, `` let y = `a|b` +
  1 ``)? Intended: yes, the backtick is an expression, its result is an ordinary
  `Int`/fixed-width value. Confirm no lexer ambiguity with a trailing backtick before
  an operator.
- Is a single-token form worth it for the common one-op case (`` `x & m` ``), or does
  the backtick pull its weight only for multi-op chains? (Leaning: always allow it;
  consistency beats a length threshold.)
- Assignment/compound forms (`` `x |= m` ``) — out of scope; the scope is a pure
  expression. A mutating `x := `x | m`` uses the normal `:=`.

## References

- `docs/layout-kind-design.md` — the declarative half (struct field layout).
- `docs/unsafe-systems-design.md` — the binary/systems frame both serve.
- `stdlib/crypto/hash.kai` — the readability evidence (`bit_*` nesting).
- Issue #1064 — the `Layout` kind; this supersedes its bitwise follow-up.
