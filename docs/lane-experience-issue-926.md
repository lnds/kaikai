# Lane experience — issue #926: `string.reverse` corrupted multibyte UTF-8 (and was O(n²))

## Scope as planned vs as shipped

**Planned:** `string.reverse("áb")` lost a byte and `reverse(reverse(s)) != s`
because a multibyte `Char` was not re-encoded to UTF-8 when materialised back to
a `String`. Make the round-trip correct over the full codepoint range and (per
the performance addendum) replace the O(n²) immutable-concat reverse with a
single-pass byte-level one.

**Shipped:** a runtime builtin `string_reverse(s)` that reverses by Unicode
codepoint in one pass and one allocation, fixing corruption AND the O(n²) at
once. `string.reverse` now lowers to that primitive; the old
`string_reverse_loop` (decode to `[Char]`, rebuild with
`string_concat("#{c}", acc)`) is gone.

The builtin allocates a buffer of the input byte length, walks the source
forward by UTF-8 sequence width (`kai_utf8_seq_len`, already used by the decode
side), and copies each FULL sequence into the destination filled from the tail.
Byte order within a codepoint is preserved while codepoint order reverses. It
never re-encodes a Char — it copies the real source bytes — so it is correct by
construction across the whole width range and is O(n) with the result size known
up front.

## Why the obvious fix (encode at the Char→String boundary) was wrong

The brief's located layer was the Char→String materialisation. The first attempt
made `kai_to_string(KAI_CHAR)` and `Show for Char::show` UTF-8-encode the
codepoint. Both broke things, and the reason is the crux of this lane:

**A kaikai `Char` is overloaded — codepoint in one model, raw byte in another.**

- `string.chars` decodes to real codepoints (`U+2014` → one `Char` valued 8212).
- `string.bytes` yields raw bytes as Chars (`U+00E1` → two Chars: 195, 161).
- The compiler re-emits multibyte string LITERALS byte-by-byte through this same
  `Char`→`String` path (each of `0xE2 0x80 0x94` is a Char rendered as one raw
  byte; concatenation rebuilds the sequence).

So `Show for Char` / `kai_to_string(KAI_CHAR)` are deliberately byte-wise:
codepoint 0xE2 must emit ONE byte (0xE2), not the two-byte UTF-8 of U+00E2.
Changing them to codepoint-wise UTF-8:

- broke `bytes()` round-trip (rebuilding `"áb€"` from its 6 bytes via `#{c}`
  produced 11 bytes — every continuation byte 128..255 re-expanded), and
- broke `make selfhost` byte-id: the self-compiled compiler re-emits its own
  multibyte literals (em-dashes in diag/driver messages) through `Show for Char`,
  so the byte-wise contract is load-bearing for the bootstrap fixed point.

Both were measured, not reasoned: the codepoint-wise `Show for Char` built and
ran, then `selfhost` reported `DIFF` on exactly the 6 em-dash literal lines, and
a `bytes()`-rebuild fixture printed `ROUNDTRIP-BROKEN`. That is why the fix lives
in a dedicated byte-level `reverse` primitive that bypasses `Char`→`String`
entirely, leaving `Show for Char` untouched.

## The `#{c}` codepoint gap is real but out of scope

`#{c}` over a decoded codepoint > U+007F (via `Show for Char` →
`int_to_byte_string`, one byte) still truncates — so `pad_left`/`pad_right` with
a multibyte `fill`, and `to_upper`/`to_lower` over multibyte input, are still
wrong. Fixing that cleanly needs the `Char` model disambiguated (codepoint-Char
vs byte-Char, e.g. `bytes()` returning `[Int]`/`[Byte]` like the bin/random
paths already do, so `Show for Char` can be codepoint-wise without breaking the
byte path or the compiler's literal re-emit). That is a language-surface decision,
not a `reverse` fix; it belongs in its own issue. `reverse` was the reported bug
and is fully closed without touching that semantics.

## Structural surprises

- **The native backend needs a third registration.** A prelude builtin reaches
  the native (libLLVM) path as `kaix_prelude_<name>`, a non-static shim in
  `stage0/runtime_llvm.c` gated by `prelude_names()` in `resolve.kai`. The C-path
  `EP` table and the typer scheme alone are not enough — native linking fails with
  an undefined symbol otherwise. The `.bc` must be regenerated (`rm
  runtime_llvm.bc*`) or the stale bitcode lacks the new symbol.

- **Stale bootstrap masquerades as a real selfhost DIFF.** After editing
  `stage0/runtime.h`, `make kaic2` does NOT rebuild `kaic1` (its dep graph keys on
  `stage1/compiler.kai`, not the shared header). `kaic1` then compiles the
  intermediate `kaic2` against the OLD header while `kaic2b` uses the new one, so
  the bootstrap never reaches a fixed point and `selfhost` reports DIFF for a
  change that is actually byte-stable. `touch stage1/compiler.kai` (or a clean
  bootstrap) before trusting selfhost after a runtime-header edit. This cost real
  time here — the first DIFF was a stale-bootstrap artefact, not the encode change.

- **`./bin/kai` auto-rebuilds a missing `kaic2` WITHOUT `KAI_LLVM`.** If the native
  binary is absent (or a `timeout` kills a native build mid-flight and the Makefile
  deletes the half-written target), the wrapper silently rebuilds a C-only `kaic2`,
  and `--backend=native` then reports "native backend not built". Build native to
  completion first; never wrap a native rebuild in a short `timeout`.

## The lying test

`stdlib/core/string.kai`'s `test "reverse — … multibyte by codepoint"` asserted
`reverse("áb") == "bá"` yet the bug was live. The stdlib test-block harness
(`test-stdlib-modules`) only compiles an `import <mod>` trampoline — it never RUNS
the `test "..."` blocks, so the assertion never executed. The real gate is now an
executable fixture under `examples/string/` (`reverse_multibyte.kai` +
`.out.expected`), which `test-string` compiles and RUNS, diffing stdout, wired
into tier1 via `test-fast` / TEST_LIGHT. The embedded block was also strengthened
to 2/3/4-byte widths + round-trip; it passes under `kai test`, but the CI gate is
the fixture.

## Fixtures added and coverage

`examples/string/reverse_multibyte.kai` (+ `.out.expected`) covers the full UTF-8
width range — 2-byte `á` (U+00E1), 3-byte `€` (U+20AC) and `中` (U+4E2D), 4-byte
`😀` (U+1F600) — plus `reverse(reverse(s)) == s` across a mixed string,
byte-`length` and `char_count` invariance under reverse, ASCII unchanged
(`reverse("abc") == "cba"`), and empty / single / single-multibyte edges. Verified
identical output on both backends (native and C); selfhost byte-id OK on both.

## Follow-ups left for next lanes

- The `#{c}` / `Show for Char` codepoint gap above — its own issue; needs the
  `Char` byte-vs-codepoint model disambiguated first.
- A SEPARATE, pre-existing bug surfaced while diagnosing: a multibyte char in a
  plain string LITERAL (e.g. `"remove — re-parsing"`) is corrupted on the way out
  (`—` → `�<raw>`), on BOTH backends, in clean `main`. It is independent of #926
  (literals never go through `reverse`) and was not touched here; it deserves its
  own issue (lexer/literal span handling, not Char encode).
- The stdlib `test "..."` blocks are dead weight under CI: `test-stdlib-modules`
  compiles but never runs them. A target that runs `kai test` over each stdlib
  module would turn hundreds of embedded assertions into real gates.
