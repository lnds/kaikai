# Lane experience — issue #935: O(n²) string builders → O(n)

## Scope as planned vs. as shipped

Planned: make the `stdlib/core/string.kai` build family linear —
`to_upper`, `to_lower`, `repeat`, `join`, `replace`, `pad_left`,
`pad_right`, `index_of`. `reverse` is owned by the parallel #926 lane
(UTF-8 correctness + O(n²)), so it stayed untouched. #926 merged first
(PR #939); the rebase kept its builtin-backed `reverse` and applied
cleanly — the two lanes touched disjoint regions of the file.

Shipped: exactly that set. Each op now passes through one of three
linear primitives instead of rebuilding an immutable accumulator per
char/piece.

## Shared root cause

Every op left-folded an immutable `String` with `string_concat` (or
`"#{x}#{acc}"`), copying the whole accumulator each step: ∑k = n²/2
byte-copies. At ~120k input each one exhausted a 4 GiB heap
(`heap limit exceeded, used 4294930383 bytes`).

The two linear primitives that already shipped carry the fix:

- `string_concat_all([String])` — two-pass single-alloc join
  (`runtime.h`), the join target for `repeat`, `join`, `to_upper`,
  `to_lower`.
- `string_byte_at_int(s, i) : Int` — byte read returning `-1` on OOB,
  no Option/Char allocation, the in-place comparator for `index_of`.

## Which primitive each op now uses

| Op | New shape |
|---|---|
| `to_upper` / `to_lower` | map `chars` to `[String]` via a **tail** accumulator, `list.reverse`, one `string_concat_all` |
| `repeat` | build `[String]` of `n` copies, one `string_concat_all` |
| `join` | interleave `sep` into a flat `[String]`, one `string_concat_all` |
| `replace` | unchanged surface — `join ∘ split`, inherits the `join` fix |
| `pad_left` / `pad_right` | `repeat("#{fill}", k)` ++ `s`, inherits the `repeat` fix |
| `index_of` | `string_byte_at_int` comparison of `needle` against `s[at..]` in place — O(n·m) time, O(1) space, no per-position slice |

## Structural surprise the brief did not anticipate

`to_upper`/`to_lower` were the trap. The obvious shape —
`["#{f(c)}", ...recur(t)]` (non-tail cons over the recursive result) —
is **byte-correct on the C backend but corrupts on native**: the
second and later pieces come back empty/garbled. Reduced to a minimal
repro the divergence is in the **native** lowering of a non-tail
recursive `[String]` build where each piece is `"#{c}"` re-encoding a
multibyte `Char` (the `Char` value does not survive across the
non-tail recursive call). Literal-`String` pieces (what `join`/`repeat`
pass) are unaffected, which is why only the case-mappers tripped it.

Two ways around it were verified to produce byte-identical output on
both backends:

1. `StringBuilder` (append in a tail loop, `build` once) — the brief's
   first suggestion. Rejected: it needs `import string_builder` from
   inside `core/string.kai`, and a stdlib-`as`-alias does not register
   the qualifier for a **type** position when the file is core
   auto-loaded (`unknown module qualifier 'sb' in qualified type`).
2. **A tail-recursive `[String]` accumulator** (pieces come out
   reversed) + `list.reverse` + `string_concat_all`. No cross-layer
   import, sidesteps the native bug. This is what shipped.

The native non-tail-cons-of-reencoded-Char corruption is a real
compiler bug, out of this lane's scope — flagged for a separate issue,
not fixed here.

## Byte-identity verification

A/B probe over every op (ASCII + multibyte UTF-8 + empty + a 120k
input) captured under the **original** tree and the **patched** tree,
on both backends:

- `new-C == orig-C` — byte-identical.
- `new-native == orig-native` — byte-identical.
- `new-native == new-C` — byte-identical.

Multibyte case-folding keeps its pre-existing behaviour: `char.to_upper`
is ASCII-only, so `á`/`é` pass through, and `"#{c}"` re-encodes a
codepoint ≥ 0x80 to its raw byte (a pre-existing `"#{c}"` quirk, not
touched here). The fix preserves that exactly, byte for byte.

## Before / after (native, n = 120000, `KAI_MAX_HEAP=4g timeout 60`)

| | Result |
|---|---|
| Before (original) | `heap limit exceeded (used 4294930383 bytes)`, exit 1, 0 ops complete |
| After (this lane) | all ops complete, peak RSS 201 MB, exit 0 |

C backend after: all complete, peak RSS 135 MB.

## Fixtures added

`examples/string/string_build_linear.kai` (+ `.out.expected`) — every
fixed op over ASCII, multibyte, empty, and a 50k input that previously
OOM'd. Runs clean and byte-identical on both backends; wired into the
`test-string` harness.

## Follow-ups left for next lanes

- File an issue for the native non-tail-cons multibyte-`Char` re-encode
  corruption (minimal repro: `string_concat_all` of a non-tail
  `["#{c}", ...recur(t)]` build over `chars("áéíóú")` — native yields
  garbage, C yields the codepoints).
- If/when that native bug is fixed, `to_upper`/`to_lower` could drop the
  `list.reverse` and cons directly, or move to `StringBuilder` once a
  core file can import it.
