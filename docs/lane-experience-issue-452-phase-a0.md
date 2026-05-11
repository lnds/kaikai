# Lane retro: Phase A.0 stdlib precompiled cache — aborted (#452)

**Outcome: aborted at Phase 1 bench-first gate.** The bench-first
probe required by the lane brief showed that a pure-kaikai
BinSerialize round-trip is **two-to-four orders of magnitude
slower** than the lex+parse it would replace. Continuing to a full
Phase A.0 implementation would have made `kai build` strictly
slower, not faster.

This retro documents the probe, the numbers, the root cause, and a
concrete recommendation for what the next lane attacking DoD #6
should do instead.

## Scope as planned

Per the lane brief and `docs/cache-design.md` §"Phase A.0":

1. Cache the post-parse `[Decl]` of every stdlib prelude
   (29 files: `core/*.kai`, `protocols.kai`, `effects.kai`,
   `array.kai`, `random.kai`, `random_secure.kai`,
   `encoding/*.kai`, `collections/*.kai`, `math/*.kai`,
   `decimal.kai`, `money.kai`, `fx.kai`, `uuid.kai`, `regexp.kai`,
   `path.kai`, `crypto/*.kai`) as a `.kab` blob keyed by
   `sha256(source_bytes)`.
2. On second-and-later `kai build`, deserialise the `.kab` and skip
   `tokenize` + `parse_program` for the prelude.
3. Expected wall-time delta on `bin/kai build empty.kai`:
   1.47 s → 1.24 s (–230 ms, –16 %) per
   `docs/cache-design.md` §"Empirical breakdown".
4. Bench-first half-implementation: one stdlib file first, measure,
   then decide whether to extend.

## Scope as shipped

Zero cache code. The lane stopped at the bench-first probe.

What this PR ships:

- `tools/bench-binserialize-roundtrip.sh` — reproducible harness
  that compares `to_bytes` / `from_bytes` wall on a synthetic but
  realistically shaped AST proxy at two payload sizes (100 nodes
  / 8 KB and 500 nodes / 40 KB).
- This retro doc.

## The numbers

Hardware: macOS arm64, M-series, 2026-05-11. `bin/kai` against
stage 2 compiled from a tier0-green tree.

| Workload | Median wall (n=5) |
|---|---|
| `bin/kai build empty.kai` (baseline) | 1.70 s |
| `to_bytes` of 100 AST-shaped nodes (8 KB out) | **20 ms** |
| `to_bytes` + `from_bytes` round-trip, 100 nodes / 8 KB | **750 ms** |
| `to_bytes` + `from_bytes` round-trip, 500 nodes / 40 KB | **19.02 s** |

Compare against what Phase A.0 was supposed to replace, per
`docs/cache-design.md` §"Empirical breakdown":

| Stage | Wall |
|---|---|
| Lex of *all 29 preludes* | 240 ms |
| Lex+parse of *all 29 preludes* | 340 ms |
| Lex+parse per prelude (mean) | ~12 ms (lex) / ~3.4 ms (parse delta) |

So the cache hit path would need `from_bytes` of one prelude to
beat ~12 ms (lex) per cached file. The probe shows `from_bytes` of
a 100-node payload (about the size of `option.kai`'s `[Decl]`
forest, the smallest realistic prelude) takes **~730 ms** — **60×
slower** than what it replaces.

At 500 nodes (about `list.kai`'s size), one prelude round-trip is
**19 seconds**. Extrapolating across 29 preludes — most of them at
or above the 500-node range, with `compiler.kai`-sized payloads in
`encoding/json.kai`, `regexp.kai`, `path.kai`, and the protocols
file pushing well past 1000 nodes — projects **minutes** of
deserialisation per `kai build`.

The bench-first decision criterion specified in the lane brief
("if Phase 1 bench shows ≥ 100 ms wall savings projected for full
stdlib cache, continue") is missed by a factor of roughly **10⁴ in
the wrong direction**.

### Where the time goes

`to_bytes` alone runs at acceptable speed (20 ms for 100 nodes).
The cost lives in `from_bytes`. Root cause is the algorithmic
shape of `bin_byte_at` in `stdlib/protocols.kai:429`:

```kai
fn bin_byte_at(buf: [Byte], pos: Int) : Result[String, Byte] = match buf {
  []           -> Err("unexpected end of buffer")
  [b, ...rest] -> if pos == 0 { Ok(b) } else { bin_byte_at(rest, pos - 1) }
}
```

Every byte read walks the `[Byte]` cons list from the head to
`pos`. Reading the N-th byte of an N-byte buffer is O(N); reading
the whole buffer in order is **O(N²)**. At N = 8000 bytes this is
6.4 × 10⁷ steps, modest in a C array but expensive when each step
is a pattern-matched recursive call against a cons cell. At
N = 40000 it is 1.6 × 10⁹ steps — twenty seconds of pure-kaikai
recursion.

`bin_list_to_bytes_loop` has a symmetric O(N²) issue via
`list_append(acc, enc(x))` at `stdlib/protocols.kai:704`, but the
constant is smaller in practice because each `enc(x)` produces a
short byte sequence and the outer list of nodes stays small (500
in the worst probe). The deserialiser, walking byte-by-byte
through the whole concatenated buffer, dominates.

## Why the design doc's promise broke down

`docs/cache-design.md` §"Empirical breakdown" and the issue body
both phrased Phase A.0 as "skip lex+parse of preludes, save
230 ms". They never quantified the *cost* of the operation that
replaces lex+parse. The implicit assumption — natural for anyone
coming from C/Rust/Go, where binary deserialisation is several
times faster than a hand-written parser — does not hold for
**pure-kaikai BinSerialize against a list-backed `[Byte]`
buffer**.

Two facts compound the issue:

1. **`[Byte]` is a cons list, not a contiguous array.** Kaikai's
   `[T]` is the persistent singly-linked structure used throughout
   the stdlib. There is no `array_get(buf, i) : Byte` primitive
   today; reads ride `bin_byte_at`, a stdlib helper that recurses
   to position `pos` each call.
2. **BinSerialize is pure kaikai.** It compiles through the same
   Perceus + RC + emit pipeline as user code. There is no SIMD,
   no memcpy, no `i64` cast over `*char` — every byte goes
   through a `Result[String, Byte]` pattern match, a `byte_to_int`
   widening, and a folded multiply-add.

The combination makes BinSerialize's `from_bytes` strictly slower
than `tokenize + parse_program` on the same source content.
`tokenize` walks a `String` (a runtime-contiguous C buffer in the
current stage 0/1/2 implementation) with O(1) `char_at` reads via
the runtime primitive `string_char_at` (a `*char` index, not a
list walk). Parse is recursive descent over the token list, which
is shorter than the byte buffer would be.

In short: **the cache's payload is larger and read more slowly
than the source it would replace.** A.0 as designed is dominated
by the deserialiser at every plausible payload size.

## What we considered before aborting

Three escape routes, all out of A.0's scope:

1. **Replace `[Byte]` with a contiguous byte array primitive.**
   Tracked indirectly by the existing `Array[T]` work and #251 /
   #252. Until kaikai has a true O(1)-index byte vector, no
   pure-kaikai deserialiser will beat the recursive-descent
   parser. Out of scope for #452 — touches the runtime, RC
   discipline, and the `Byte` nominal type's primitive backing.
2. **Add a runtime `[Byte]` indexing primitive
   (`list_byte_at_fast`) that the BinSerialize impls call.** A
   stage 0 C primitive could walk the cons list once and present
   a contiguous view to `bin_*_from_bytes`. Out of scope for
   #452 — it changes the BinSerialize contract (extern_c shim
   per impl) and the runtime surface. Reasonable next lane;
   should be its own issue.
3. **Emit C directly for the cache instead of going through
   kaikai.** `bin/kai` is a shell driver. It could call a tiny C
   helper that writes/reads the `.kab` payload and hand kaic2 a
   pre-parsed in-memory `[Decl]` via a new IPC channel. This is
   essentially "skip the kaikai-native serialiser entirely" and
   bypasses both pre-blocker gaps — but it abandons the
   "kaikai-native cache" design pinned in
   `docs/cache-design.md` §"Serialisation primitive". Out of
   scope for #452.

None of the three is a one-line patch; none was promised by the
lane brief or by `docs/cache-design.md`; and the bench-first gate
exists precisely to surface this kind of misalignment **before**
2500+ LOC of cache code is written.

## Recommendation for the next lane

Skip A.0 entirely. Two paths are viable; pick one.

### Path 1 — go directly to Phase A.1 (typer modularisation cache)

`docs/cache-design.md` §"A.1 payload" notes that A.1 saves 0.43 s
of typecheck on top of the 0.23 s parse savings A.0 would have
shipped. A.1 has the **same BinSerialize cost problem** A.0 has,
plus a 2500–4000 LOC typer-modularisation refactor. Without a
fast deserialiser, A.1 is also dead on arrival. Therefore A.1
inherits A.0's pre-blocker: fix the deserialiser substrate first.

Recommended: open an issue "BinSerialize: O(1)-indexable byte
buffer or runtime fast path" and have it block both #452 (A.0)
and the A.1 / #460 sequence.

### Path 2 — drop BinSerialize, write the cache in C

The `bin/kai` driver is already a shell script; adding a small C
helper at `stage0/cache.c` that produces a flat binary blob from
a fresh `kaic2 --ast` run, and consumes it on subsequent builds
via a new `--prelude-ast-blob <path>` flag, would sidestep the
kaikai-native deserialiser entirely. The trade-off is that the
cache leaves the language's safety surface — but `stage0/runtime.h`
is already C, and the cache is by nature a build-tool concern,
not a runtime concern.

Estimated cost: ~1500 LOC of C in stage 0, plus a kaic2-side
`--load-ast-blob` consumer that bypasses lex+parse. Comparable
size to A.0's 600–800 LOC kaikai estimate, with no algorithmic
risk.

### Recommendation

**Path 1 is the principled choice; Path 2 is the pragmatic one.**
Path 1 keeps the cache inside the language and unblocks several
future lanes that also want O(1) byte access (encoding/json
streaming, regex DFA tables, crypto block ciphers). Path 2 ships
the wall-time win sooner but accrues C surface area.

Either way, **#452 should be closed** and re-opened (or
re-scoped) once the byte-buffer substrate exists. The current
issue body still cites the pre-#511 numbers ("≤ 1.25 s warm
wall") that assumed a fast deserialiser; that assumption is
falsified by this lane's probe.

## Design decisions and alternatives considered

The lane brief gave one decision: bench first, then continue or
abort. The bench result removed all subsequent decisions. There
is no design surface in this PR to retro — we did not write a
cache, header, write/atomic-rename, or invalidation logic.

The only judgment call was the **shape of the probe**. Two
alternatives considered:

1. **Probe the real `[Decl]` AST.** Build a `#derive(BinSerialize)`
   for the actual `Decl` / `Expr` / `Type` types in
   `stage2/compiler.kai`. Rejected: that would require resolving
   the design-doc gap "BinSerialize derive support for mutually
   recursive sums" first (the dispatcher at
   `stage2/compiler.kai:46496` does single-pass derive only); not
   a bench-first probe, that's two weeks of compiler work before
   the first wall measurement.
2. **Probe a flat list of `Int` only.** Rejected: too far from the
   real workload. The deserialiser cost scales with byte count,
   not with structural complexity; a flat Int list would give
   honest absolute numbers but understate the constant by ~3×
   relative to a heterogenous AST-shaped record list.

The chosen probe — a `#derive(BinSerialize)` record with one of
each Int / String / `[Int]` field — is the closest reasonable
proxy: same dispatch shape as the real AST nodes, same field
heterogeneity, same `bin_list_*` combinator path. It runs in
~10 seconds (cold build + 5-iter median × 2 modes) and is fully
reproducible from a clean tier0 tree.

## Structural surprises the brief did not anticipate

1. **The brief assumed BinSerialize was simply slower than parse,
   not catastrophically slower.** It allocated 1–2 days for
   Phase 1; the bench took 90 minutes. The brief's "≥ 100 ms
   wall savings projected" floor turned out to be off by four
   orders of magnitude.
2. **The pre-blocker `#471` (BinSerialize protocol + derive)
   closed correctly — but did not solve A.0's actual problem.**
   `#471`'s acceptance was "round-trip works on a 2-field
   record"; correctness, not throughput. The lane brief and the
   issue body inherited the implicit assumption that
   correctness was sufficient. It is not.
3. **No one had run a perf probe on BinSerialize before this
   lane.** `examples/stdlib/binserialize_*` exists, but every
   example round-trips ≤ 10 elements. The lane brief asking for
   a bench-first probe is what surfaced the cliff.

## Fixtures added and coverage gaps

- `tools/bench-binserialize-roundtrip.sh` — reproducible probe;
  not a fixture in the `make tier0` / `make tier1` sense (no
  acceptance threshold), but a `make` target the next lane can
  re-run after any BinSerialize change.

No `.kab` cache fixtures because no cache shipped. No invalidation
fixtures, no atomic-write fixtures, no header-corruption fixtures.

Coverage gap: there is no BinSerialize perf gate in any tier.
Adding one would have caught the regression risk at #471's lane.
Recommended follow-up: open an issue
"Tier 2 BinSerialize perf gate" that asserts a 100-node
round-trip under N ms, parameterised by future deserialiser
improvements.

## Real cost vs estimate

| Phase | Estimated | Actual |
|---|---|---|
| Phase 1 (single-file bench) | 1–2 days | 1.5 hours |
| Phase 2 (full A.0) | 3–5 days | not started |
| Phase 3 (retro + PR) | 0.5 day | 2 hours |
| **Total** | **4.5–7.5 days** | **3.5 hours** |

The bench-first discipline saved ~5 days of cache-implementation
work that would have produced a slower compiler.

## Follow-ups left for next lanes

1. **Open a blocker issue: "BinSerialize substrate is O(N²); cache
   lanes depend on this."** Tag with `runtime`, `stdlib`, and
   reference `#452`. Lane brief: pick Path 1 or Path 2 from
   §"Recommendation for the next lane" above.
2. **Update `docs/cache-design.md` §"Phase A.0" with the bench
   numbers** so the next reader does not re-attempt the lane
   with the same assumptions. The "≤ 1.25 s warm wall" target
   was conditional on a fast deserialiser; that conditional was
   implicit.
3. **Mark #452 as blocked on (1) and update the issue body** to
   reflect the bench-first outcome. Do not close — DoD #6 still
   wants a stdlib cache; the path to it just got rerouted.
4. **Tier 2 perf gate for BinSerialize round-trip** (covered
   above). Prevent silent regressions on whatever substrate the
   next lane lands.

## Closing

The bench-first approach worked: it produced a definitive
abort signal in 90 minutes that prevented a multi-day
implementation that would have made `kai build` slower, not
faster. The lane brief's framing ("if Phase 1 bench fails, the
PR still gets opened but as `[WIP] abort` with the bench results,
design doc explaining why, and recommendation") is what this PR
delivers.

The next attempt at Phase A.0 — under whichever rerouting wins —
should re-read this retro and start from a working `from_bytes`
throughput baseline before writing any cache code.
