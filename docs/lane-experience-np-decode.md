# Lane experience — native-parity char/hex decode (np-decode)

**Scope:** the char/hex-decode slice of the native-parity baseline (KIR Lane
1.5). The brief named three symptom clusters — json `\uXXXX` (`MISMATCH got='1'
want='A'`), regex predicate/subsume wrong output, and `jwt_encoder` decode — and
asked: find the char/hex primitive or literal lowering that the native
(in-process libLLVM) backend emits differently from the C-direct oracle, close
each fixture byte-id, and ratchet the baseline down. From a clean `main`
(burn-down 6 merged at 67 gaps).

**Outcome:** ONE root cause closed, diagnosed to ground truth with the C-direct
oracle. `kai_llvm_build_string_span` (stage2/runtime.h, the native string-literal
constant builder) decoded escapes with a hand-rolled `switch` that only covered
`\n \t \r \0 \" \\`; the C99 escapes `\a \b \f \v`, hex `\xHH`, and octal `\ooo`
fell to a default that **dropped the backslash and kept the next character** —
`"\x41"` became the three bytes `x41` instead of the single byte `A`. That
corrupted every string literal carrying such an escape, most visibly the JSON
byte-table `json_byte_table()` (`"\x01..\xff"`), which the `\u` decoder slices to
turn a codepoint into a UTF-8 byte. Fixing the decoder to match cc's
interpretation byte-for-byte closed `hex_escape_literal` + `json_surrogate_decode`
(67 → 65). The brief's other two symptom clusters turned out to be SEPARATE causes
the escape fix unmasked, not char/hex — documented, not forced (see below).

## The root cause

### `kai_llvm_build_string_span` — incomplete escape decode

The two backends split on WHO decodes the literal:

- **C-direct (oracle):** emits the verbatim source span into `kai_str("\x41")`
  and lets the **C compiler** decode `\x41` → byte 0x41 at C-compile time. It
  never had the bug because cc owns the escape table.
- **Native:** has no C-compile step for the constant — it builds an LLVM global
  string in memory via the libLLVM C API, so it must decode the escapes ITSELF.
  Its `for`-loop over the span handled only the six common escapes and let
  everything else hit `default: buf[w++] = c` — which for `\x41` consumed the
  `\`, kept `x`, then copied `4` and `1` as ordinary bytes. Three bytes, wrong
  ones.

The symptom chain for `json_basic`'s `dec esc 3` (`"A"` → want `"A"`): the
`\u` path computes codepoint 0x41 correctly (it reads the `0041` digits via
`char_at`/`char_to_int`, which are byte-correct), then calls
`json_utf8_encode(65)` → `json_byte_to_str(65)` →
`string_slice(json_byte_table(), 64, 1)`. With the byte-table literal corrupted
(every `\xNN` → three bytes), index 64 lands on a garbage byte — an ASCII digit,
hence `got='1'`. So the visible failure was a hex-NIBBLE read, but the actual bug
was one layer down: the table the slice indexes was never decoded correctly.

**Fix:** replace the `switch` with a C99-faithful decoder (stage2/runtime.h):
add `\a \b \f \v \' \?`; add a `case 'x'` that consumes every following hex digit
and writes the low 8 bits (cc semantics: `\xH...` is greedy, not fixed-width);
add `case '0'..'7'` that reads 1–3 octal digits and writes the low 8 bits (`\0`
alone stays the NUL byte, a degenerate octal). The empirical oracle was a
fixture enumerating every escape under both backends — cc's output IS the spec,
so I matched it value-for-value (`\a`→7, `\b`→8, `\f`→12, `\v`→11, `\x41`→65,
`\101`→65) rather than reasoning about it.

This is additive and native-only: the function lives under `-DKAI_LLVM` and is
called only on the native path. The C path (selfhost, all of tier1) emits the
span verbatim and is untouched — selfhost byte-id held on the first try, as
expected.

## What was NOT char/hex (unmasked, documented, left for other lanes)

The brief grouped three clusters under "char/hex decode"; the escape fix proved
two of them are distinct causes:

- **json-array nested-variant decode.** With escapes fixed, `json_basic` /
  `json_real_*` / `json_surrogate_encode` / `jwt_encoder` STILL diverge: a
  non-empty array/object (`[1]`, `{"a":1}`) decodes to `None` under native, while
  `[]` / `{}` (empty) decode fine. `json_decode` runs the iterative `json_loop`
  driver, which pushes `JFrame` variants with payload (`ArrFrame([JsonValue])`,
  `ObjFrame([…], Option[String])`), conses them with list spreads
  (`[ArrFrame(acc1), ...rest]`), and matches the top frame with nested variant +
  list sub-patterns. That is the nested-variant / list-in-variant-slot KIR
  lowering family burn-down 6 partly closed (residual `binserialize_derive_nested`)
  — NOT a char read. `jwt_encoder` fails through the same `json_decode`.
- **regex matcher logic.** `regex_basic` / `regex_anchors_repetition` panic
  `unterminated character class`; `regex_subsume_*` / `regex_predicate_basic`
  emit wrong output (`slug`/`other` classification inverted). The literal decode
  is now correct (the char-class brackets read fine); the divergence is in the
  matcher's decision-tree lowering under native. A separate diagnosis.

Per the lane rule "NEVER cut a cause in half — finish it or don't touch it," I
closed the char/hex cause completely and left these two untouched, with a precise
hand-off in `docs/native-parity-gaps.md` so the next lane starts from the
diagnosis, not the symptom.

## Fixtures

- **`hex_escape_literal.kai`** (pre-existing, issue #618 / llvm-parity-A) — was a
  regression fixture for the SAME bug class on the retired LLVM-text backend;
  exercises `\x` + NUL. Now passes native. Removed from the baseline.
- **`json_surrogate_decode.kai`** — exercises the `\u` + byte-table path end to
  end. Now passes native. Removed from the baseline.
- **`string_escape_decode.kai`** (NEW) — a dedicated regression fixture covering
  the FULL scope of the fix: every simple escape (`\a \b \f \t \n \v \f \r`), hex
  (`\x41 \x7e \xff`), octal (`\101 \176`), the byte-length invariant
  (`\x41\x42` == "AB", len 2), and the byte-table slice path the JSON decoder
  depends on. Golden (`.out.expected`) generated from the C-direct oracle; passes
  byte-id on both backends. The parity harness diffs both backends on it, so a
  future native-decoder regression is caught against cc.

## Coverage gaps

- The fix covers every C99 string-literal escape cc emits FROM A kaikai SPAN. It
  does not add `\uNNNN` / `\UNNNNNNNN` universal-character-name decoding to the
  native string-span builder, because the kaikai lexer does not pass those forms
  through to a string-literal span as cc-decodable escapes (char literals use
  `'\u{...}'`, decoded elsewhere; JSON `\u` is DATA decoded by json.kai at
  runtime, not a literal escape). If a future surface form routes `\uNNNN` into a
  string span, the decoder needs a `case 'u'`/`case 'U'` — flagged here.

## Cost vs estimate

Single root cause, single runtime.h function, ~50 lines. The bulk of the lane was
DIAGNOSIS (proving the byte-table corruption was the cause, then proving the
array-decode and regex residue were NOT char/hex) and the no-regression sweep —
the full native parity harness over ~250 fixtures confirmed zero new gaps and
exactly the two intended closes (the two Linux-only `list_*` flaky gaps that
passed on macOS were correctly NOT removed, per the burn-down 1/2/3 lesson).

## Follow-ups for the next lane

- json-array nested-variant decode (json_basic / json_real_* / json_surrogate_encode
  / jwt_encoder) — list-in-variant-slot decision tree, oracle = emit_c's list +
  variant test lowering.
- regex matcher logic (regex_basic / regex_anchors_repetition / regex_subsume_* /
  regex_predicate_basic) — matcher decision-tree lowering under native.
