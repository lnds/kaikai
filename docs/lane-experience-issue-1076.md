# Lane experience — issue #1076: hex/binary integer literal overflow

## Scope

Follow-up to #1059, which added the compile-time overflow diagnostic for
**decimal** integer literals but explicitly left hex/bin out — a `0x`/`0b`
literal past the width still wrapped silently (`0xFFFFFFFFFFFFFFFF` → -1,
`0x1FFFFFFFFFFFFFFFF` → -1, no diagnostic). This lane closes that hole:
hex/bin literals whose written pattern exceeds 64 bits now diagnose at
parse time instead of two's-complement wrapping.

Scope as planned == scope as shipped. A few lines in `parse.kai` plus a
positive/negative fixture pair, exactly as the brief scoped it.

## Design decision — hex/bin limit is u64, not i64

The one open decision was whether a hex/bin literal's ceiling is i64
(2^63-1, same as decimal) or u64 (2^64-1). Consulted asu (language
surface). Chose **u64**: a hex/bin literal is a *bit-pattern* of the
destination width, not a signed magnitude. `0xFFFFFFFFFFFFFFFF` is the
all-ones 64-bit pattern (= two's-complement -1) and `0x8000000000000000`
is the sign bit — both are legitimate, idiomatic mask/flag writes. The
`Layout` kind + Elixir-style bitwise-intrinsic direction reinforces that
hex/bin are patterns. An i64 ceiling would reject exactly the 64-bit
patterns that motivate writing in hex. The mainstream precedent (Rust
`…u64`, Zig, C promotion-to-unsigned) accepts the full destination width.

So: **>16 significant hex digits or >64 significant bin digits** overflow
(64 bits); the full 64-bit pattern is legal. The repro `0x1FFF…` (17 hex
digits > 2^64) is caught; `0xFFFFFFFFFFFFFFFF` (u64 max exactly) compiles.

## Distinct predicate + distinct message

asu flagged (correctly) that the hex/bin check is a *width* test
("more than 64 significant bits?"), not the decimal *signed-range* test
("exceeds 2^63-1?") — two predicates, not one. `int_lit_overflows` now
branches: decimal keeps the `mag_gt(digits, 2^63-1)` string compare;
hex/bin count significant digits and compare against 16/64.

The diagnostic message also differs. Decimal says "out of Int range"
because it reasons about signed magnitude. But under the u64 rule a hex
that overflows *width* may still decode to an in-Int-range value by
two's complement — "out of Int range" would lie. Hex/bin emit
"hex/binary literal exceeds 64 bits; Int is 64-bit — use
bigint.from_string" instead (and it does **not** suggest the `…n`
suffix, which is decimal-only — suggesting `0x…n` would be a false
hint). Decimal message unchanged.

## What was reused

- The digit-counting mirrors `strip_int_lit_digits` (skip `_`, skip
  leading zeros) but counts hex-or-bin digits after the `0x`/`0b`
  prefix; a new `count_base_digits` keeps that local. The u64 rule makes
  it a pure length check — no lexicographic `mag_gt` needed for hex/bin,
  because 16 hex digits max out at exactly u64::MAX (all F) and 64 bin
  digits at exactly u64::MAX (all 1).
- The call site is the same `TkInt` arm in `parse_primary` that #1059's
  decimal check lives in (1598); only the message branches by prefix.

## Comment corrected

The old `int_lit_overflows` comment claimed "Only decimal spans are
checked (hex/bin carry a `0x`/`0b` prefix and a bit-pattern intent)" —
that became a lie the moment the check landed. Rewrote it to state the
actual invariant: decimal compares magnitude against 2^63-1; hex/bin are
bit-patterns whose overflow is a >64-bit width check. `decode_int`'s
`#[doc]` ("decimal/hex/bin … that fit 64 bits") stayed true and was left.

## Structural surprise — lexer rejects `_` in hex/bin

While testing boundary cases I found the **lexer** does not accept `_`
separators inside hex/bin literals at all (`lex_scan_hex_digits` only
continues on `ch_is_hex_digit`), so `0xFF_FF` is a lex error today —
independent of this lane, and not promised by `kai info syntax` (which
only shows `0xFF`/`0b1010`). `count_base_digits` still drops `_`
defensively (consistent with the decimal path and robust if the lexer
gap is ever closed), but the positive fixture avoids `_` in hex. The
lexer gap is out of scope here (this lane is the overflow diagnostic);
worth its own issue if it bites.

## Fixtures

- `examples/numeric/hex_literal.kai` (+ `.out.expected`) — positive:
  `0xFF`, `0xDEADBEEF`, `0b1010`, `0x7FFF…FFF` (i64 max), the full
  64-bit patterns `0xFFFFFFFFFFFFFFFF` and 64-bit-all-ones binary (both
  → -1). Wired into `test-numeric-bigint` (C) and
  `test-numeric-bigint-native` (native parity).
- `examples/numeric/hex_overflow.err.kai` (+ `.err.expected`) —
  negative: the issue repro `0x1FFFFFFFFFFFFFFFF` (17 hex digits) must
  diagnose. Wired next to the #1059 decimal negative in
  `test-numeric-bigint`, same PASS/FAIL/grep shape.

## Verification

Both backends: negative diagnoses, positive compiles with native == C.
tier0 green (selfhost `kaic2b.c == kaic2c.c` byte-identical — the fix
only adds a compile-time rejection path for invalid programs, so the
compiler's own emitted C is unchanged).

## Follow-ups

- Lexer `_`-in-hex/bin gap (above) — separate issue if wanted.
