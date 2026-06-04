# Lane experience — issue #744: String/Char codepoint model

## Scope as planned vs. as shipped

**Planned (owner-pinned before the lane):** rename the byte-wise
`chars()` to `bytes()`; add a real-codepoint `chars()`; make the
compiler accept UTF-8 source (string + multi-byte char literals);
adopt a Rust-shaped `Char` invariant (scalar value, enforced at the
edges). No edition bump — a pre-adoption Hanga Roa surface correction
shipped as `feat(stdlib)!` with a `BREAKING CHANGE` footer. Then do
#745 (`display_width`) as a separate follow-up lane (path 2).

**Shipped:** all of the above, working on `main` directly (no
worktree, per owner instruction to keep `main` green at every step).
Surface delta:
- `string.bytes(s) : [Char]` — the renamed byte-wise function.
- `string.chars(s) : [Char]` — codepoints (UTF-8 decode).
- `string.char_count(s) : Int` — codepoint count, non-materializing.
- `string.byte_length(s) : Int` — explicit byte-count synonym of
  `length` (which stays bytes).
- `string.char_indices(s) : [CharIndex]` — codepoint + byte offset.
- `pub type CharIndex = { off: Int, cp: Char }` — see surprise #2.
- char literals `'á'`, `'▸'`, `'\u{1F389}'` lex to one scalar value
  (stage1 + stage2 lexers).
- `int_to_char` panics on a non-scalar-value argument.

Design study lives in
`docs/decisions/string-char-codepoint-model-2026-06-04.md` (the
Rust-vs-Go axis analysis the owner decided from).

## Design decisions and alternatives

- **`Char` invariant: Rust-shaped, enforced at the edges** (owner's
  pick over the Go `rune`-alias alternative). `int_to_char` and FFI
  returns are the only entry points where a non-scalar-value can
  appear; everything else decodes UTF-8 and is valid by construction.
  So validation is concentrated at `int_to_char` (panic) — zero cost
  on the decode hot path.
- **`int_to_char` validates only the absolute envelope** (`< 0`,
  `> 0x10FFFF`, surrogate `0xD800..0xDFFF`). It deliberately does NOT
  reject byte values `0..255`: the codebase pervasively uses
  `int_to_char(byte)` as a "build a byte" idiom (`protocols.kai`
  BinSerialize, `cache.kai` KAB2 decode, `http.kai`, `emit_llvm.kai`).
  All byte values are valid scalar values, so the idiom is unaffected;
  only a genuinely disastrous Int (an emoji-times-ten, a negative)
  panics. This was a course-correction made *after* auditing the 31
  `int_to_char` call sites — the original plan implied a tighter check
  that would have risked the byte idiom.
- **`string_cp_at` clamps malformed decodes to U+FFFD.** Because
  `chars()` is `int_to_char(string_cp_at(...))` and `int_to_char` now
  panics, a corrupt UTF-8 buffer could otherwise panic mid-`chars()`.
  The decoder maps any non-scalar-value result (only reachable from
  malformed input) to U+FFFD, so `chars()` is panic-free on any byte
  buffer while `int_to_char` stays strict for the user-facing case.
  Well-formed UTF-8 never hits the clamp.
- **Codepoint API written in kaikai over two C primitives**
  (`string_cp_at`, `string_cp_len`) rather than implementing
  `chars`/`char_count`/`char_indices` each in C. One decode classifier
  in C; the walks (advance `off += cp_len`) are kaikai. Smaller C
  surface across the three runtimes, testable in kaikai.
- **No lazy iterator.** `chars()` materializes (`[Char]`, Python-3
  shape). A lazy `Iterator` is a language-wide feature touching the
  effect system and Perceus; deferred to its own lane rather than
  dragged in through a String API.

## Structural surprises the brief did not anticipate

1. **Two divergent runtimes.** `stage0/runtime.h` (minimal, plain
   boxed Int, `x->tag == KAI_INT`) and `stage2/runtime.h` (Koka-packed,
   tagged-Int immediates, `kai_is_int(x)` / `kai_intf(x)`) are separate
   906-line-divergent files. The new primitives had to land in BOTH,
   with the Int-access idiom adapted per runtime. First cut copied the
   stage0 `off->tag == KAI_INT` / `off->as.i` form into stage2 and
   segfaulted at 0x5 (a tagged-Int immediate dereferenced as a
   pointer); fixed by using `kai_is_int`/`kai_intf` in stage2. The
   stage2 LLVM path reuses `stage0/runtime_llvm.c`, so the `kaix_`
   wrappers + `declare`s went there + in `emit_llvm.kai`.

2. **Typer bug: sugared tuples lose their type args in a list spread.**
   `[(off, int_to_char(cp)), ...rest] : [(Int, Char)]` fails inference
   — the `[]` base anchors to `[Pair]` (Pair with unresolved args) and
   never unifies with `[Pair[Int, Char]]`. Minimal repro:
   `let p: (Int, Char) = (n, c)` alone reports `expected Pair[Int,Char],
   found Pair`. A *nominal* record in the same shape works. Worked
   around by making `char_indices` return a nominal `CharIndex { off,
   cp }` instead of `(Int, Char)` — which is also a better API
   (`.off`/`.cp` over `.fst`/`.snd`). The typer bug is real and
   orthogonal to #744; **worth a separate issue** (label `typer`):
   tuple literal `(a, b)` does not propagate type arguments to the
   `Pair` constructor.

3. **Registration surface is wide.** A new prelude primitive touches,
   per stage: stage0 `check.c` + `emit.c` + `runtime.h` +
   `runtime_llvm.c`; stage1 `compiler.kai` (name list + EP table);
   stage2 `emit_c.kai` + `emit_llvm.kai` (declare + LP table) +
   `infer.kai` (TyEntry) + `resolve.kai`. Two primitives × ~10 sites.

4. **Blast radius smaller than projected.** Only 8 `.chars()` sites in
   the tree, all ASCII, so no existing golden changed (ASCII: byte ==
   codepoint). The pre-survey flagged goldens as "HIGH risk"; in fact
   `string_lines_chars`, `string_compose`, `huffman` all stayed green
   under the new `chars()`. Migrations were intent-driven, not
   forced-by-breakage: huffman → `bytes()` (it round-trips a byte
   stream), compose → `byte_length` (it measures `pad_left`'s byte
   width).

## Fixtures added and coverage gaps

- `examples/stdlib/string_codepoints.kai` (+ `.out.expected`) — the
  issue's `"áé▸"` case: bytes=7, chars=3, first byte=195, first
  codepoint=225, `char_indices("á!")` offsets. Passes on both C and
  LLVM backends (parity verified).
- `stdlib/core/string.kai` intrinsic test blocks: `bytes`, `chars`
  (ASCII + multibyte), `char_count`/`byte_length`, `char_indices`.
- Migrated: `demos/9d9l/huffman/main.kai` (→ `bytes()`),
  `examples/stdlib/string_compose.kai` (→ `byte_length`).

**Gap:** no automated fixture for the `int_to_char` out-of-range
**panic**. `test-stdlib`'s `.err.expected` runner checks
*compile*-time errors, not runtime panics; the `.run.err.expected`
runner is `examples/refinements/`-specific. Wiring a new runtime-panic
runner was out of proportion to the value; the panic is verified
manually (`int_to_char(1114112)` → `panic: int_to_char: 1114112 is not
a Unicode scalar value …`, exit 1) and the valid path (bytes 0..255,
emoji) is covered by `string_codepoints`. A follow-up could add a
runtime-panic fixture lane-wide.

## Follow-ups

- **#745 `display_width`** — the next lane (path 2). Consumes
  `char_indices` / `chars`; the East Asian Width table is a codepoint
  lookup over the iterator this lane provides.
- **New issue: typer tuple-literal type-arg propagation** (surprise
  #2). Until fixed, any `[(A, B)]`-returning function with an `[]` base
  case needs a nominal record workaround.
- **Runtime-panic fixture runner** (coverage gap above), if the
  pattern recurs.
- The `Char` ASCII helpers in `stdlib/core/char.kai` (`is_upper`,
  `to_lower`, …) are now ASCII-correct-*only* on what are real
  codepoints; Unicode case folding / categories are a separate,
  deferred concern.
