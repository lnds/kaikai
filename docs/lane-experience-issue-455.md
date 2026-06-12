# Lane experience — issue #455 (Phase B: user-file incremental cache)

## Scope as planned vs as shipped

**Planned (brief):** extend Phase A's precompiled cache to user files;
skip parse + typecheck when `(mtime, content hash, dep hashes)` are
unchanged; target ≤150 ms for a no-change tiny recompile. Wire it in
`bin/kai` via `--prelude-cache` / `--prelude <source>`.

**Shipped:** a per-project, content-addressable user-file cache with
correct transitive invalidation. New module
`stage2/compiler/user_cache.kai` (km A+, 124 LOC) owns the composite
cache key, transitive dependency discovery, hit/miss, and the on-disk
write; `driver.kai` consults it before lexing each imported module;
`bin/kai` opts in under `KAI_CACHE=1`. Six fixtures
(`examples/cache/userb_*.sh`) gate invalidation correctness, wired into
`make test` via `test-user-cache`.

Three brief premises were false against the code and were corrected
after reading it (and confirming the reframing with the user):

1. **`--prelude-cache` / `--prelude` are retired in Hanga Roa**
   (`driver.kai` rejects them). The core module set is hard-coded and
   loaded inside kaic2; there is no user-facing prelude flag to reuse.
   So the cache lives in kaic2's import resolver, not in a `bin/kai`
   flag dance.

2. **Phase A is A.0 (post-parse `[Decl]`), not post-typecheck.** The
   cache skips lex+parse, never the typer. "Skip typecheck" is A.1, a
   separate 2500–4000 LOC typer-modularisation lane, still open.

3. **Phase A's cache was never wired into production.** `load_core`
   calls `load_preludes(..., caches=[], ...)`; the only things that
   exercise the KAB2 codec are `--emit-prelude-cache` and
   `--cache-roundtrip-test`. The prelude cache is dead code on the
   `kai build` path. (Recorded as a follow-up; NOT touched here — its
   invalidation axis is version-hash/bootstrap, not user mtime, so
   coupling it to this lane would be the wrong reshape.)

## The honest performance result

**The ≤150 ms cold-trivial target is NOT met, and A.0 does not move it.**
Measured on an M-series mac:

- 8 trivial user modules: no-cache 0.21 s, warm-cache 0.21 s — no
  change.
- 1 user module of 2000 LOC: no-cache 1.22 s, warm-cache 1.28 s — if
  anything marginally *slower*.

Two compounding reasons, both predicted by `docs/cache-design.md`:

1. **The prelude dominates.** On a tiny user file ~100 % of the wall is
   the prelude's lex+parse+typecheck. A.0 applied to *user* files skips
   the user parse, which is ~0 ms. The prelude is re-processed every
   build (it is loaded with `caches=[]`). Only A.1 (cache the prelude
   *post-typecheck*) touches that, and A.1 is out of scope.

2. **Deserialize ≈ parse for kaikai modules.** Replacing a 2000-LOC
   parse with a 2000-decl KAB2 deserialize is close to a wash — the
   same observation that killed the token-cache variant in Phase A's
   design doc ("within ~50 ms of the cost it replaced"). A.0's net
   wall-time win on user files is essentially zero today.

So the deliverable is **not** a speed win. It is the **correct
transitive-invalidation infrastructure**: the composite key, the
dependency-graph hashing, and the six-fixture correctness gate. That is
the durable, load-bearing piece — it is what #447 (LSP didSave
re-typecheck) builds on, and it is the prerequisite that A.1 will reuse
when it caches the prelude. The wall-time payoff lands when A.1 ships;
this lane lays the user-file half of the foundation and proves the
invalidation is sound. **No green was reported against ≤150 ms.**

## Design decisions and alternatives considered

- **Content-addressable, not mtime-based.** The brief said mtime; the
  pinned `docs/cache-design.md` says content-addressable ("two machines
  with the same content must hit the same key"). kaic2 also has no
  `file_mtime` prim. Went with content hashing via the `string_hash`
  runtime prim (FNV-1a 64-bit) — not cryptographic, but it only needs
  to change when content changes, which it does for any byte edit.

- **Cache key materialised as the blob filename**
  (`<content_hex>-<dep_hex>.kab`). Invalidation is then a *property of
  the key*, not an imperative sweep: an edit anywhere in a module's
  transitive closure changes its dep hash, so its filename changes and
  the old blob is never read. No invalidation walk, no stale-serving
  window. The KAB2 header (magic/format/version) is a second line of
  defense against drift and corruption.

- **Dependency discovery by token scan, not parse.** Computing the dep
  hash needs a module's imports. Doing a full `parse_program` to get
  them would defeat the cache (parse is exactly what we amortise). The
  scan reproduces the import grammar's path shape from the token stream
  only (`TkImport` then `TkIdent (TkDot TkIdent)*`), which `tokenize`
  already produces — cheap relative to parse. Aliases and selective
  lists don't change which file loads; `import ?hole` loads nothing
  (matches `process_imports`' `IkHole`).

- **Atomicity + mkdir delegated to `bin/kai`.** The stage1 bootstrap
  compiler that builds the kaic2 bundle knows only
  `read_file`/`write_file`/`abspath` among FS prims — not
  `file_rename`, `file_delete`, `dir_create_dir`, `file_exists`, or
  even `bit_and`/`bit_ushr`. So directory creation (`mkdir -p`) lives
  in the shell wrapper, and kaic2 writes the blob with `write_file` to
  its final content-addressed path. A torn write is self-correcting:
  the next build recomputes the same key, finds a header-invalid blob,
  and overwrites. (Adding the prims to stage1 was the alternative;
  rejected — it widens the bootstrap base for an opt-in feature, and
  the shell already has the tools.)

- **Opt-in, off by default.** Gated by `KAI_CACHE=1` so the default
  build path is byte-for-byte unchanged and pays no lookup cost. The
  hundreds of fixture-harness builds that assume no cache are
  untouched.

## Structural surprises the brief did not anticipate

- **Two bootstrap-compiler quirks cost real debugging time** (both now
  pinned in this lane's notes):
  - **stage1 does not support bit operators or most FS prims.** Forced
    the arithmetic-only hex encoding and the shell-side FS delegation.
  - **stage1 mis-compiles an `if`-without-`else` as a statement** — it
    runs the body unconditionally. The cache wrote a `-.kab` (empty
    key) on *every* build, even with the cache off, because
    `if cache_on and content_hex != "" { uc_write(...) }` ignored its
    guard. Fix: make it an `if/else` expression bound to `let _w`.
    This is a latent stage1 trap any future bundle code can hit; the
    symptom (guard silently ignored) is nasty because it type-checks
    and selfhosts clean.

- **`Ctor { ... }` after `==` parses as a record literal.** `if t.kind
  == TkImport { ... }` made stage1 read `TkImport { ... }` as a record
  construction. Converted the token-kind dispatch to `match t.kind`
  throughout the scanner.

## Fixtures added

`examples/cache/userb_*.sh`, run by `make -C stage2 test-user-cache`
(in `TEST_LIGHT_TARGETS` + `test-fast`):

- `userb_edit_source` — edit own bytes → content-hash invalidation.
- `userb_edit_import` — edit a direct import → consumer dep-hash
  invalidation.
- `userb_edit_transitive` — edit a two-hop import → cascade.
- `userb_version_bump` — flip the blob's kaikai-version byte → header
  rejects.
- `userb_corrupt_blob` — truncate the payload → decoder rejects.
- `userb_cold_warm_identical` — the **positive differential**: emitted
  C is byte-identical across cold-cache / warm-cache / no-cache. This
  is the gate selfhost byte-id cannot give (the compiler is built with
  the cache off, so selfhost never exercises a hit). It is what proves
  a hit reconstructs exactly the decls a fresh parse would.

## Coverage gaps / follow-ups for next lanes

- **A.1 (post-typecheck prelude cache)** is where the ≤150 ms target
  actually lives. Pre-blockers #574/#578/#597 are closed; the typer-
  side semantic step remains. This lane's user-file key + invalidation
  is reusable there.
- **Phase A's prelude cache is dead on the `kai build` path** (loaded
  with `caches=[]`). Worth a follow-up issue to either wire it in or
  retire `--emit-prelude-cache`. Deliberately not coupled to this lane.
- **The root file itself is not cached** — only its imports. The root
  is the file you just edited, so caching it rarely helps; the
  multi-module win is entirely in the imports. If A.1 makes
  deserialize cheaper than typecheck, revisit caching the root.
- **Whitespace/comment-only edits invalidate** (content hash changes).
  An AST-normalising key could avoid it; out of scope, acceptable cost,
  same as the design doc states.

## Real cost

Larger than a "1–2 day" extension of an existing format, because the
format was wired to nothing and three brief premises were false. The
codec reuse held (the KAB2 serializer round-trips user decls
unmodified — the differential fixture confirms it), but the
import-resolver wiring, the bootstrap-prim constraints, and the two
stage1 quirks were the real work. The honest framing — infrastructure +
correctness, not a speed win — was settled with the user before any
code shipped.
