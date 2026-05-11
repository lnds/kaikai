# Lane retro — fs/file binary IO (issue #482)

## Branch / scope

Branch: `lane-issue-482-file-bytes` (worktree path still
`kaikai.lane-phase-a0-stdlib-cache` from an earlier rename).

Closes: #482 (follow-up to #345). Two new prelude builtins:
`file_read_bytes` and `file_write_bytes`. Unblocks the Phase A.0
stdlib cache lane (#452) which needs to round-trip binary AST
blobs through disk.

## Scope as planned

The user briefed Phase A.0 proper (precompiled stdlib cache).
Mid-lane discovery (the same pattern as the BinSerialize collections
pre-blocker that became PR #480): the planned approach needs a
runtime primitive — `file_read_bytes` / `file_write_bytes` — that
`stdlib/fs/file.kai:18-22` documents as **deferred** from PR #402
(which closed #345 minus binary IO). Without it, kaikai-side cannot
construct a byte-exact `String` from `[Byte]` for `write_file` to
consume (the `#{c}` interpolation escapes non-printable bytes
through `Show for Char`).

Pivot: file issue #482 documenting the gap, ship the runtime
primitives in this lane, leave the cache itself for the next lane.

## Scope as shipped

Three changes touching the four standard layers:

1. **Runtime** (`stage0/runtime.h`):
   - `kai_prelude_file_read_bytes(path)` — `fopen("rb")` + `fread`,
     builds a `[Byte]` cons-list right-to-left (so the result is in
     file order without a reverse pass).
   - `kai_prelude_file_write_bytes(path, bytes)` — `fopen("wb")` +
     `fwrite` byte-by-byte over the cons-list.
   - Two thunks for the lifted-fn table.

2. **Compiler builtin table** (`stage2/compiler.kai`):
   - Two `EP(...)` entries in the runtime-mapping table so the
     emitter routes `file_read_bytes(...)` / `file_write_bytes(...)`
     to the `kai_prelude_*` symbols.
   - Two `TyEntry` entries in `add_prelude_sigs`:
     `file_read_bytes : String -> Result[String, [Byte]]`
     `file_write_bytes : String -> [Byte] -> Result[String, Unit]`.
   - Two `prelude_effect_for` entries tagging both with the `File`
     effect, same as `read_file` / `write_file` / `file_exists`.
   - Two new names in `prelude_names`.

3. **Fixtures** (`examples/stdlib/`):
   - `fs_file_bytes_roundtrip.kai` — writes `[0x00, 0xFF, 0x41, 0x0A]`,
     reads it back, asserts cons-list equality byte-by-byte. Covers
     the two extremes plus an ASCII char and a newline (which
     historically caused issues for String-based pipes).
   - `fs_file_bytes_missing.kai` — `Err` on a non-existent path.

## Design decisions

### Why bare prelude builtins, not new `File` effect ops

The `File` effect's `builtin_file_decl` only registers `read_file`
and `write_file` as dispatch ops. `file_exists` / `file_delete` /
`file_rename` (from PR #402) are bare prelude functions that
`prelude_effect_for` tags with `File` for the effect row — they do
not appear in the effect's op table. This lane follows the same
pattern: `file_read_bytes` / `file_write_bytes` are bare functions,
tagged with `File`. Qualified syntax `File.read_bytes(...)` is not
supported and not needed for the immediate consumer (#452).

If qualified syntax becomes desirable later, extending
`builtin_file_decl` with two new `EOp` entries plus matching type
helpers (`ty_result_string_list_byte_zero` etc.) is mechanical.
Not done here to keep the surface change minimal.

### Why right-to-left cons-list construction in read_bytes

Reading bytes into a flat buffer first and then folding right-to-left
avoids the otherwise-needed reverse pass at the end. The C side
already has the file size up front via `fseek` + `ftell`. The buffer
costs `n` bytes of transient memory; for the cache lane's largest
prelude (~50 KB) that is trivial. The alternative — building the
list left-to-right and reversing — costs the same memory but adds
an `n`-element walk.

### Why `[Byte]` not `[Int]`

PR #476 made `Byte` a first-class nominal primitive (0..255). The
runtime tag is `KAI_BYTE` (separate from `KAI_INT`). The `[Int]`
sketch in #345's original body predates the rename and would force
explicit `int_to_byte` casts at every caller boundary. `[Byte]`
matches the post-#476 idiom and matches what BinSerialize already
ships.

### Why not run write through `write_file`

`write_file(path, content : String)` writes whatever bytes the
`KAI_STR` holds in `as.s.bytes`. The kai-side gap is constructing
that `String` byte-exactly from a `[Byte]`. Two alternatives were
considered:

- **Add `bytes_to_string([Byte]) : String` runtime primitive.**
  Smaller change to the runtime surface (one fn, not two). Rejected
  because the resulting `String` would be a misnomer — a String
  that happens to carry arbitrary bytes is exactly the source of
  bugs that motivated `Byte` becoming nominal in #476. The
  symmetric `file_read_bytes` direction would still need its own
  primitive anyway.
- **Iterate `write_file(path, ...)` byte-by-byte with append mode.**
  Quadratic in I/O calls; not worth the savings of avoiding one
  runtime fn.

The chosen path adds two symmetric primitives that mirror
`read_file` / `write_file` exactly. Surface stays understandable.

## Acceptance

- `make tier0` green; selfhost byte-identical.
- `make tier1` green locally.
- Both new fixtures pass; the round-trip asserts byte equality
  including 0x00 and 0xFF.
- `hexdump` of the test file confirms `00 ff 41 0a` written
  byte-exactly (manually verified during development).

## Cost vs estimate

Issue body estimated ~80 LOC. Actual: 87 LOC across runtime +
compiler. ~4 hours including the design pivot from #452.

## Follow-ups (next lane: #452 Phase A.0)

With binary IO in place, the cache lane is unblocked:

- Define `KaiAstBlob` header on disk: 32 bytes (magic `KAB1` +
  format_version u32 + kaikai_version u32 + payload_len u64 +
  checksum u64), payload is `[Decl]` serialised via BinSerialize.
- Apply `#derive(BinSerialize)` to the AST type tree (Decl, Expr,
  Pattern, TypeExpr, Stmt, plus their helpers). PR #480 made every
  required derive a one-line annotation now that List/Option/Char
  are covered.
- Cache key: `sha256(source_bytes) + kaikai_version_hash +
  cache_format_version`. Path: `~/.cache/kaikai/preludes-v<N>/<sha>.kab`.
- Atomic write: `<sha>.kab.tmp` + `fsync` + `rename`. The `rename`
  POSIX call is atomic on same-filesystem; `file_write_bytes` plus
  the existing `file_rename` primitive cover this without any new
  runtime surface.
- Hook into `load_prelude` in `stage2/compiler.kai:50394`. On hit,
  skip `tokenize` + `parse_program`; on miss, run the existing
  parse path and write the cache.

Out of scope here and there alike: `metadata` (#345 follow-up),
`FileMetadata`, streaming IO, append-mode binary writes,
Windows support.

## Things I'd do differently

- The user's pattern of briefing the cache lane directly twice in a
  row suggests the design doc's pre-blocker enumeration is not
  visible enough. Adding a one-line cross-reference at the top of
  `docs/cache-design.md` listing the *remaining* pre-blockers would
  make this kind of discovery cheaper on the third pass.
- The runtime primitive could have been folded into PR #480 if the
  scope analysis had gone two levels deep (the user did one level:
  "extend BinSerialize"; the second level — "and we still need
  file_read_bytes/file_write_bytes" — surfaced only when I started
  thinking about how `kaic2` would actually consume a `.kab`).
  Naming this risk pattern in retros may help.
