# Lane retro — issue #592 KAB2 binary cache format

## Scope as planned

Flip the on-disk cache payload format from KAB1 hex ASCII to KAB2
packed binary. Deliver the originally projected ~0.41 s warm wall
saving on `bin/kai build empty.kai`, target warm ≤ 1.97 s on M2 Pro
(≥ 17 % faster than cache-OFF baseline 2.31–2.37 s). Flip the
`KAI_PRELUDE_CACHE` default from OFF to ON when warm wins. Update
the four invalidation fixtures to seed KAB2 header bytes.

## Scope as shipped

KAB2 codec shipped end-to-end. Default flipped from OFF to ON.
Selfhost byte-identical confirmed. All four invalidation fixtures
pass. **The 1.97 s gate did not land** — see "What KAB2 actually
delivers" below. Warm wall is ~2.28 s (1 % net positive vs cache-OFF
2.31 s) instead of the projected 1.97 s.

The reason: the design doc's `0.41 s saving` projection was based
on the cumulative lex+parse phase wall, but the cache decoder still
pays ~0.20 s of its own wall, so the realistic save is ~0.20 s, not
0.41 s. With the driver-side shasum overhead also fixed (see
"Surprise #2" below), the net wall improvement is the cache-load
saving minus the residual decoder overhead. The cache wins, but
not by the margin the design doc anticipated.

The decision was: ship KAB2 anyway because (a) it closes the KAB1
hex regression (16 % slower → 1 % faster), (b) the infrastructure
unblocks the A.1/A.2 lanes that close the remaining gap, and
(c) `KAI_PRELUDE_CACHE=1` is no longer a footgun. Issue #592 closes
with KAB2 + default-on; the wall-target sub-issue is the lane that
ships A.1.

## Design decisions

### Byte path: Option A revised (added two runtime primitives)

The brief recommended **Option A** — express the binary codec via
`"#{int_to_char(b)}"` interpolation, no stage 0 changes. Empirical
verification showed this path is broken:

- `"#{int_to_char(0)}"` produces the 3-byte literal `\x0`, not
  1 byte NUL.
- `"#{int_to_char(255)}"` produces the 4-byte literal `\x255`, not
  1 byte 0xFF.

The culprit is `impl Show for Char` in `stdlib/protocols.kai:55` —
chars outside `[32, 126]` get rendered through a `\xNN` escape
chain. The limitation is **already documented** in
`stdlib/protocols.kai:555-568` and `:636-645` as a known gap that
blocks BinSerialize from supporting non-ASCII / NUL-containing
Strings.

Per the brief's stop clause ("Si Option A requiere infrastructura
nueva en stage 0/1, parar y considerar Option B"), I paused and got
explicit authorisation to proceed with **Option A revised**: add a
small runtime primitive `int_to_byte_string(n: Int) : String` that
constructs a 1-byte String via `kai_str_from_bytes`, bypassing
`kai_to_string(KAI_CHAR)`. This is one wrapper around an existing
runtime function (~6 lines of C), one entry in each prelude table
(stage 0 / 1 / 2 EP + LP + TyEntry + LLVM declare). Far smaller than
Option B (full `Byte` type to kaikai-minimal).

The same lane added a second primitive `string_byte_at_int(s, i) :
Int` (returns -1 on OOB) so the decoder's per-byte read does not
pay an `Option[Char]` + `Char` allocation. The KAB2 decoder calls
this ~10^6 times per warm load; the bypass cut cache deserialise
wall by an additional ~0.1 s on M2 Pro.

Both primitives are general-purpose — `int_to_byte_string` also
closes the documented BinSerialize String/Real ASCII-only gap in
`stdlib/protocols.kai`. A follow-up issue will route the existing
`bin_collect_chars` through this primitive.

### Codec naming kept as `cache_*_to_hex` / `cache_hex_to_*`

The original step-2 codec (PR #584) named every encoder
`cache_<T>_to_hex` and every decoder `cache_hex_to_<T>`. KAB2 makes
those names slightly misleading — they emit raw bytes, not hex.
**I left the names unchanged** to keep the diff focused on the
encoding flip. A future cleanup lane can rename them
`cache_<T>_to_bin` / `cache_bin_to_<T>`; doing it now would have
churned 800+ call sites (every AST node serdes pair) for cosmetic
gain.

### Header layout: 76 bytes fixed, no newlines

KAB1's header was 4 newline-terminated ASCII lines. KAB2 is fixed
76 bytes with no newline framing — `string_slice` reads each field
directly by known offset. The line-reader helper (`cache_read_line`
+ `CacheReadLine`) is gone. Saves ~4 bytes per header and one
function definition.

Layout:

```
magic           4 bytes  "KAB2"
format_version  4 bytes  u32 LE
kaikai_version  4 bytes  u32 LE
source_sha     64 bytes  ASCII lowercase hex
```

### Int decoder inlined / Option-free

`cache_hex_to_int` originally walked 8 bytes via a recursive helper
that allocated one `Option[CacheIntPos]` per call. Profiling showed
this dominated the warm-load wall (~30 % of cache deserialise time).
The new shape reads all 8 bytes inline via the
`string_byte_at_int` primitive, never wraps in Option, and only
allocates the final CIP. Same for `cache_hex_to_u32` (4 bytes).
This is the single biggest performance win in the lane.

### Driver: batch shasum once, not per-prelude

`bin/kai` originally called `shasum -a 256 $f` once per stdlib
prelude — 32 forks at ~9 ms each = ~280 ms of unaccounted overhead
on the warm path. The KAB1 retro flagged shasum but assumed it was
unavoidable; the KAB2 codec was supposed to compensate.

In fact the codec-only savings (kaic2 cache: ~0.19 s) are smaller
than the shasum overhead (~0.28 s). Without batching, the warm
wall would have been ~0.10 s **worse** than cache-OFF even with
KAB2.

The fix is `prelude_sha_batch` — one `shasum -a 256 file1 file2 …`
invocation feeds a tmp file `<sha> <path>` lookup table. Per-prelude
lookups use `awk` against the table. shasum batch cost is ~10 ms
total — a 28× speedup.

### Default flip from OFF to ON

With KAB2 + batch shasum, the warm wall is consistently within
noise of cache-OFF (~1 % faster on M2 Pro, n=5 median). The brief's
condition for flipping the default — "cuando warm gana vs cache-OFF"
— is met. The strict 17 % gate is not. The flip is therefore
defensible: it does no harm, it unblocks future improvements, and
it lets users feel the eventual A.1 wall reduction without having
to opt in.

`KAI_PRELUDE_CACHE=0` and `KAI_NO_PRELUDE_CACHE=1` both restore the
pre-cache path for benchmarks or CI runs that need to compare.

## Surprises

### Surprise #1 — Show for Char escapes broke Option A

Already covered above. The empirical test ran in the first 30 min
of the lane and forced the stop-and-report path. Without the
empirical check, the codec rewrite would have produced silently
corrupt blobs (every NUL byte vanishing, every byte ≥ 128
expanding 3–4×) and the round-trip test would have failed days
later under stranger circumstances.

The lesson: when stage 0 / stage 1 documentation says a primitive
is "not first-class", verify the workaround empirically before
committing 800+ LOC of codec to it. The `BinSerialize` impls in
`stdlib/protocols.kai` already documented the limitation; the brief
did not, so the test was the only feedback path.

### Surprise #2 — bin/kai shasum was eating the cache savings

The KAB1 retro attributed the regression to "hex decoder is slower
than parser". True, but not the whole story — the driver also adds
0.27 s of per-prelude shasum overhead. KAB2 alone would have left
the warm wall at ~2.45 s; the batch-shasum fix is what brings it to
2.28 s. The two are independent: even with the original 0.27 s
shasum cost in place, KAB2 alone would have been faster than KAB1,
but still 4 % slower than cache-OFF.

The fix is purely shell — a single `prelude_sha_batch` helper plus
a wrapper `file_sha256_cached` that consults the tmp file when it
exists. Future driver changes that compute per-prelude data should
batch by default.

### Surprise #3 — the 0.41 s lex+parse projection is a cumulative phase number, not a deletable cost

The design doc projects A.0 saving 0.41 s based on the
`tools/bench-phases.sh` cumulative wall for `--tokens` + `--ast`.
What the cache replaces is roughly that work, **minus** the cost of
the cache loader. The loader is not free — it walks ~30 KB of binary
per prelude × 32 preludes = ~1 MB of decode work, ~10^6 byte reads,
~10^5 small allocations (every AST node materialises). Empirically
this costs ~0.20 s, leaving ~0.20 s of net save.

The 1.97 s wall target depended on the loader being nearly free.
With current kaikai runtime / RC discipline, that is not achievable
without a `[Decl]` deserialiser that avoids per-node allocations
(arena allocation, RC-free during cache load, etc). That is
infrastructure beyond the codec.

The honest path: keep KAB2 as a stepping stone, flip the default
on, and document that the wall target moves to the A.1 lane.

## What KAB2 actually delivers (M2 Pro, n=5 median, 2026-05-14)

Wall numbers for `bin/kai build /tmp/empty.kai -o /tmp/empty`:

| Path                              | Wall    | Δ vs OFF | Δ vs KAB1 |
|-----------------------------------|---------|---------:|----------:|
| cache OFF (KAI_PRELUDE_CACHE=0)   | 2.31 s  | baseline | —         |
| KAB1 hex, cache ON warm (v0.60.0) | 2.70 s  | +16.9 %  | baseline  |
| KAB2 binary, cache ON warm        | 2.28 s  | **-1.3 %** | -15.6 %  |
| KAB2 binary, cache ON cold        | 2.93 s  | +26.8 %  | -21.0 %   |

Isolated kaic2 (no shell overhead, n=3 median):

| Path                  | Wall    |
|-----------------------|---------|
| no cache, 32 preludes | 1.59 s  |
| with cache (warm)     | 1.40 s  |

Cache layer net contribution at the kaic2 level: **-12 % (0.19 s
faster)**. Bin/kai driver contribution (batch shasum + paths) closes
the rest of the gap. Cold compile pays ~0.6 s of cache emit on top
of a fresh build; subsequent warm runs amortise that immediately.

Blob size: KAB1 produced 60 kB hex per prelude (for ~30 kB of raw
data). KAB2 produces ~30 kB binary — close to the raw AST size,
since the format is essentially a serialised projection of `[Decl]`
with no compression.

## Selfhost

`make -C stage2 selfhost` confirms byte-identical fixed point. The
codec change is observable only at the cache boundary; selfhost
runs without `KAI_PRELUDE_CACHE` and is unaffected.

## Tier coverage

- **Tier 0**: `make tier0` — green. 25 demos OK + 3 PASS-no-golden,
  baseline 27 held. Selfhost byte-identical.
- **Tier 1**: green locally (see `make tier1` output in the PR).
- **examples/cache/*.sh**: 4 / 4 fixtures pass with KAB2 headers.

The cache fixtures gained one byte-level subtlety vs KAB1: the
`format_version_mismatch` and `kaikai_version_hash_mismatch`
fixtures must `printf '\NNN\000\000\000'` to seed a 4-byte LE u32
field, where KAB1 used `printf '63000000\n'` (8-char hex line).
Octal `\NNN` is portable in `printf(1)` across BSD + GNU userlands.

## Real cost vs estimate

Brief budgeted ~500–800 LOC compiler + ~40 LOC bin/kai + fixture
refresh. Actuals:

- `stage2/compiler.kai`: ~150 LOC net change (codec rewrite +
  unrolled Int/u32 decoders + 2 new EP/LP/TyEntry/LLVM-declare
  rows + 105 tag literal rewrites via sed).
- `stage1/compiler.kai`: 2 new EP rows + allowlist entries (~4 LOC).
- `stage0/runtime.h`: 2 new prelude functions (~30 LOC) + 2 thunks.
- `stage0/runtime_llvm.c`: 2 forwarders (~2 LOC).
- `stage0/emit.c`, `stage0/check.c`: 2 new prelude table entries
  each.
- `bin/kai`: ~50 LOC for `prelude_sha_batch` + `file_sha256_cached`
  + the stdlib_prelude_flags batching block + default flip.
- `examples/cache/*.sh`: 3 of 4 fixtures rewritten for KAB2 binary
  header. Fourth (source content change) unchanged.

Total ~280 LOC. Under the 800 budget.

Wall-clock: ~4 hours, split:

- 40 min — reading docs, codec, runtime, identifying the Show
  for Char trap and the runtime primitive shape.
- 30 min — adding the runtime primitive across stage 0/1/2 + smoke
  test.
- 45 min — codec rewrite (header, byte/u32/int/bool/string/option
  encoders + decoders) + tag-literal sed sweep.
- 45 min — `pos + 2` → `pos + 1` bulk fix (via python script
  excluding the u32 decoder window) and roundtrip validation.
- 20 min — Option-free unrolled Int/u32 decoders + roundtrip re-
  validate.
- 30 min — bin/kai batch shasum + benchmark loop to verify net
  win.
- 20 min — fixtures rewrite + verify all 4 pass.
- 30 min — this retro + acceptance gate runs.

## Follow-ups

Filed as separate issues:

- **A.1 (#461 / sub-issue)** — typed `[Decl]` cache + module env
  delta. The lane that gets us under 1.97 s wall. Pre-blockers
  documented in `docs/cache-design.md` (`lower_protocols` boundary,
  consumed `inherited` in `typecheck_module`).
- **A.2 (#461)** — post-perceus cache + emit-only-user. Blocked on
  A.1.
- **BinSerialize String/Real non-ASCII fix** — `bin_collect_chars`
  (`stdlib/protocols.kai:559`) and the String impl (`:646`) can now
  call `int_to_byte_string` to close the documented `\xNN` /
  NUL-truncation gap. Tiny stdlib edit; benefits anyone using
  `#derive(BinSerialize)` on a String-bearing record.
- **Codec naming sweep** — rename `cache_*_to_hex` /
  `cache_hex_to_*` → `cache_*_to_bin` / `cache_bin_to_*` for
  clarity. Mechanical 800-call-site rename; deferred from this lane
  to keep the diff focused.

## Bitácora notes for the next lane (A.1)

- The cache decoder's hot path now goes through `string_byte_at_int`
  (Int-return, no Option), not `cache_hex_byte_at` (Option-wrapped).
  Future per-byte readers should follow the same pattern.
- `int_to_byte_string` is the canonical 1-byte raw-string primitive
  going forward. Do not use `"#{int_to_char(b)}"` for binary byte
  emission — `Show for Char` escapes will silently corrupt the
  output.
- The driver's batch-shasum trick generalises: every per-prelude
  shell-out should consolidate via a single batched call before the
  per-prelude loop. A.1 will add a per-prelude `ModuleEnvDelta`
  emit; that should also batch.
- The lane-experience-issue-452 retro lists the `cache_sha`
  sentinel as a 64-char placeholder. KAB2 keeps that convention
  (ASCII hex in the header, opaque to kaic2). Don't move it to raw
  bytes — `bin/kai`'s atomic-write path inspects the header for
  debugging via `xxd`.
- The 1.97 s gate target dates to a 2026-05-14 baseline that
  measured 2.37 s. The current cache-OFF baseline is 2.31 s — the
  intervening lanes (post-#578 typer fold) shaved 0.06 s without
  touching the cache. Future gate targets should re-measure cache-
  OFF before quoting a wall delta.
