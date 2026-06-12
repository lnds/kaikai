# Lane experience — issue #825 (cache the core post-typecheck)

## The headline: the issue asked for post-typecheck; the measurement said post-parse

The brief (inherited from the #455 retro) was explicit: cache the
auto-loaded **core** module set **post-typecheck**, on the premise that
post-parse "won't move the needle" because "deserialize ≈ parse for
kaikai modules". That premise is **false for the core**, and the lane's
first deliverable was measuring it.

### The measurement (M-series mac, 3-LOC user file, kaic2 → C)

A spike wired the existing KAB2 post-parse codec to the dead `caches=[]`
slot of `load_core` and dumped the phase breakdown:

| phase | no cache | post-parse core cache |
|-------|----------|----------------------|
| tokens (lex) | 0.07 s | **0.00 s** |
| ast (+parse) | 0.09 s | 0.03 s |
| check (+typecheck) | 0.14 s | 0.07 s |
| **full → C** | **0.20 s** | **0.13 s ≤ 150 ms ✓** |

The C emitted with and without the cache is **byte-identical** (verified
on two programs, one exercising `list.map` / `list.sum`).

**The real lever is the LEX of the core, not its typecheck.** Lexing the
core's 4401 LOC costs ~70 ms; deserialising the per-module KAB2 blobs
costs ~0 ms. The #455 retro measured post-parse of *user* files (0 LOC)
and mis-extrapolated to a 4401-LOC core. "Deserialize ≈ parse" is true
for a tiny file and false for the core.

So the design flipped: cache the core **post-parse**, per-module,
reusing #455's codec and key machinery wholesale, NOT post-typecheck.
The ≤150 ms target is met by skipping the core's lex+parse.

### Why post-parse is also the *sound* choice

Before flipping, an agent verified empirically that a post-typecheck
core cache is **unsound today** without rewriting the typer. Two
user→core contamination paths, both demonstrated by compiling, diffing
C, and running the binary:

1. **`lower_protocols`** computes the rename set + the impl registry
   over the **combined** prelude++user stream and rewrites core bodies.
   A user `protocol Printable { print(...) }` + `impl Printable for
   String` rewrites the core's `println` (which calls `print` as a free
   var) to dispatch to the user's impl — the tainted binary prints
   nothing.
2. **Shared `root_fns`** (#748 narrowing guard) changes the typed AST of
   the core's `repeat` depending on whether the user declares a root
   `fn repeat` — benign in codegen, but it breaks byte-identity of a
   would-be post-typecheck artifact.

Cutting both is the 2500–4000 LOC typer-modularisation #455 deferred.
**Post-parse sidesteps both entirely:** parse is the one pipeline phase
structurally closed over the user file (it walks a token stream of only
the core module's bytes), so a core blob produced in isolation is
byte-identical to the core parsed alongside any user file. The cascade
(alias resolution, `lower_protocols`, the typer fold) keeps running
fresh over `core-deserialised ++ user` every build, so the contamination
paths never touch the cached artifact. Precedent: ML/OCaml separate
compilation — the `.cmi`/`.hi` caches the import-closed phase; the
importer's typecheck always re-runs.

The reframe was validated with the language architect (asu) before any
implementation code shipped: post-parse meets the target, is sound by
construction, and avoids both the typer rewrite and the soundness hole.

## Scope as planned vs as shipped

**Planned:** cache the core post-typecheck (resolved + typechecked
[Decl] / symbol tables), wire `load_core`, six-style correctness
fixtures, ≤150 ms.

**Shipped:** a per-module **post-parse** core cache. New module
`stage2/compiler/core_cache.kai` (km A++, 32 LOC of code) owns the
core-specific cache key (content hash of the one module source; no
dependency hash — each core module parses in isolation, its cross-module
references re-resolved in the always-fresh cascade) and the blob path.
`load_core` gains a cached path (`load_core_cached` / `load_core_module`
in `driver.kai`) that does per-module lookup with write-on-miss,
self-populating the 12 blobs on the first warm build. Gated by the same
`--user-cache` opt-in (`KAI_CACHE=1`) #455 introduced, sharing its
`.kai-cache` directory. Four fixtures (`examples/cache/corec_*.sh`) gate
correctness, wired into `make test` via `test-core-cache`. Measured:
0.20 s → 0.13 s, target met.

## Design decisions and alternatives considered

- **Per-module (12 blobs), not one monolithic core blob.** Reuses the
  existing per-file codec verbatim, invalidates granularly (editing
  `list.kai` re-parses only `list`, not all 12 — and the compiler's own
  stdlib gets edited one module at a time during development), and fails
  cleanly (a corrupt blob loses one module, not the whole core). The
  cost — 12 hash checks vs 1 — is ~0 ms.

- **Blobs live in the project `.kai-cache`, not next to the stdlib.**
  The installed stdlib (`share/kaikai/stdlib`) may be read-only; the
  per-project `.kai-cache` bin/kai already creates is always writable.
  Each project pays a one-time 12-blob populate (~98 KB) on its first
  warm build. The blob filename embeds the source content hash, so two
  projects with the same stdlib write the same blob names — and editing
  the stdlib changes the hash, changing the name, so the old blob is
  never read (invalidation is a property of the key).

- **`"core-"` filename prefix** keeps the core blob namespace disjoint
  from the user cache's `<content>-<dep>.kab` in the same directory. The
  header sha is `content + 48 zeros` (vs the user cache's `content +
  dep + 32 zeros`), a second line of defense against a blob renamed onto
  a different key.

- **Write-on-miss inside the loader**, not a separate emit step. The
  first warm build parses each core module via the unchanged
  `load_prelude` and writes its decls — exactly `load_prelude`'s
  post-parse output (doc-stripped, test-stripped, origin-tagged) — so a
  later hit reconstructs precisely what a fresh parse would. No new
  `--emit-core-cache` flag, no shell-side populate step.

- **Codec format unchanged → no `format_version` bump.** We reuse the
  KAB2 codec exactly; the payload schema is identical to what
  `--emit-prelude-cache` already produces. (If a future lane changes
  what the core blob stores, `cache_format_version` must bump — flagged
  in `core_cache.kai`.)

## Structural surprises the brief did not anticipate

- **The core is NOT upstream-independent of the user today.** The whole
  reason post-typecheck is hard. Documented above; this was the finding
  that flipped the design.

- **stage0/kaic0 chokes on `#[doc("""...""")]` with non-token content.**
  The bundle's first lexing pass (stage0) tokenizes *inside* a
  triple-quote doc string, so backticks (`` ` ``), apostrophes (`'`),
  and non-ASCII (`≤`, `→`, `—`) inside a `#[doc("""...""")]` produce
  "unexpected character" / "unterminated char literal" errors. `ast.kai`
  and `parse.kai` use triple-quote docs only because theirs happen to be
  token-clean. The cache cluster (`user_cache.kai`, `cache.kai`) sidesteps
  it with `#` header comments — which is what this module's file-top doc
  now uses. Per-symbol single-line `#[doc("...")]` is fine as long as the
  string carries no `'` or backtick.

- **selfhost catches what the bundle hides** (the pinned trap). The
  bundle concat ignores `import`, so `driver.kai` using `cc_lookup`
  built clean via `make kaic2` but `make selfhost` (real imports) failed
  with "cannot find `cc_lookup`" until `import compiler.core_cache` was
  added to `driver.kai`. Always selfhost before "done".

## Fixtures added

`examples/cache/corec_*.sh`, run by `make -C stage2 test-core-cache`
(wired into `make test` after `test-user-cache`):

- `corec_cold_warm_identical` — the **positive differential gate**:
  emitted C is byte-identical across no-cache (oracle) / cold-cache /
  warm-cache, and the cold run wrote 12 core blobs. This is what
  selfhost byte-id cannot give (the compiler is built cache-off, so
  selfhost never exercises a core hit). It proves a hit reconstructs
  exactly the decls a fresh core parse would.
- `corec_edit_core_file` — copies the stdlib to a temp tree, warms the
  cache, edits `core/list.kai`, and asserts the post-edit build matches
  a fresh no-cache build of the *edited* stdlib. This is the
  **catastrophic-failure guard**: serving a stale core would typecheck
  every program against old stdlib symbols.
- `corec_version_bump` — flips the blob's kaikai-version header byte;
  the header rejects it, the module re-parses, the build stays correct.
- `corec_corrupt_blob` — truncates the largest core blob past its
  header; the decoder rejects the torn payload and re-parses.

## Coverage gaps / follow-ups

- **The `--user-cache` flag now also drives the core cache.** Naming is
  slightly imprecise (it's the single caching opt-in now), but renaming
  would churn #455's surface for no gain. Left as-is; documented.
- **`kai build` wall is dominated by `cc`, not the kaikai phase.** Full
  `kai build foo.kai` is ~0.62 s warm — but ~0.11 s of that is `cc`
  compiling the emitted C to a binary, outside the kaikai compiler. The
  ≤150 ms target (and #455's "0.21 s" baseline) is the kaikai phase
  (kaic2 → C), which is what this caches and what now measures 0.13 s.
- **Whitespace/comment-only edits to a core file invalidate** (content
  hash changes), same accepted cost as #455.
- **The post-typecheck core cache remains the real prize for a bigger
  win** if/when the typer is modularised and the two contamination paths
  are cut — but it is not needed for ≤150 ms and is unsound until then.

## Real cost

Smaller than a post-typecheck lane (which would have been thousands of
LOC + a soundness hole). The implementation is ~32 LOC of new code + a
~50-LOC loader path in `driver.kai` + four fixtures. The work was in the
*measurement and reframe* — proving the inherited premise wrong, proving
post-typecheck unsound, and validating post-parse with the architect —
not in the code. The honest framing (a measured 0.13 s, the lever is the
lex, post-parse is the sound choice) is the deliverable.
