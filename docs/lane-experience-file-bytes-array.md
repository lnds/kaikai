# Lane retro — file_read_bytes / file_write_bytes use Array[Byte]

## Branch / scope

Branch: `lane-file-bytes-array`. Closes the boundary mismatch left
open after PR #488: BinSerialize moved from `[Byte]` to
`Array[Byte]` so its decoder no longer pays O(N²) cons-walks, but
the file I/O primitives shipped in PR #483 still returned and
accepted `[Byte]`. Anything that round-trips a serialized blob
through disk — Phase A.0 stdlib cache (#452) being the immediate
consumer — had to call `array_from_list` over an N-element cons
list on every read, which incurs N `array_set` calls plus the
`Mutable` effect on the caller. That conversion is wasted work for
the 40 KB AST blobs the cache is meant to load fast.

Prereq for #452; not the cache itself.

## Scope as planned

Brief: refactor the runtime primitive surface from `[Byte]` to
`Array[Byte]` on both sides (read returns Array, write accepts
Array), update the typer entries, refresh the existing fixtures,
add namespaced wrappers in `stdlib/fs/file.kai`. ~0.5 day.

## Scope as shipped

Same four layers as the parent #482 lane, edits localized:

1. **Runtime** (`stage0/runtime.h`):
   - `kai_prelude_file_read_bytes(path)` allocates a `KAI_ARRAY`
     directly (`kai_alloc(KAI_ARRAY)` + sized `malloc` + per-slot
     `kai_byte(buf[i])`) instead of building a cons list
     right-to-left. Avoids the default-init incref/decref pair that
     `kai_array_make` + per-slot `kai_array_set` would have done,
     and skips the cons construction entirely.
   - `kai_prelude_file_write_bytes(path, buf)` packs the
     `Array[Byte]` into a contiguous `unsigned char *` buffer and
     issues a single `fwrite`. The old cons-list path did per-byte
     `fwrite` calls; stdio buffering hid the cost but the new
     pack-then-fwrite path is honest about the contract and fails
     fast on a non-Byte slot before any I/O.

2. **Compiler builtin table** (`stage2/compiler.kai`):
   - The two `TyEntry` rows in `add_prelude_sigs` flip from
     `list_ty(TyByte)` to `TyCon("Array", [TyByte])` on both the
     read return and the write parameter.
   - `prelude_effect_for` already tagged both with `File`; no
     change there.
   - `EP(...)` runtime-mapping entries unchanged — same C symbols,
     just different KaiValue tags inside.

3. **Stdlib** (`stdlib/fs/file.kai`):
   - Two new `pub fn` wrappers: `read_bytes(path)` and
     `write_bytes(path, buf)`. They follow the
     `exists`/`delete`/`rename` convention: short module-qualified
     names, no `file_` prefix, body forwards directly to the
     prelude builtin. No shadowing because `file_read_bytes` /
     `file_write_bytes` already carry the prefix.
   - Header doc updated to remove the v1 gap note about deferred
     binary I/O.

4. **Fixtures** (`examples/stdlib/`):
   - `fs_file_bytes_roundtrip.kai` rewritten to construct the
     buffer via `array_from_list([...], mk_byte(0))`, walk the
     read result via `array_length` + `a[i]`, and compare with a
     pair of index-based helpers. Same four sentinel bytes
     (0x00 / 0xFF / ASCII 'A' / `\n`), same golden output.
   - `fs_file_bytes_missing.kai` unchanged: it only pattern-matches
     `Err` on a missing file, never touches the buffer.

## Design decisions and alternatives considered

### Why direct `kai_alloc(KAI_ARRAY)` over `kai_array_make` + per-slot set

`kai_array_make(n, init)` increfs `init` n times and writes the
same pointer into every slot. The natural follow-up — overwriting
each slot with the actual byte — would then call
`kai_array_set_impl`, which decrefs the old slot (the init Byte)
and stores the new one. That pair of operations is pure overhead
for a fill-from-fread loop: every position writes a unique
`kai_byte(...)` so the initial fill is never observed.

Allocating the array shell manually (`kai_alloc` + `malloc` for
`items`) and dropping `kai_byte(buf[i])` straight into the slot
costs one incref per byte (inside `kai_byte` via `kai_alloc`),
zero decrefs, zero default-init traffic.

The price is that the read path duplicates four lines from
`kai_array_make`. Acceptable: the duplication is tiny, marked, and
the cost saving scales with file size (the 40 KB cache blob would
otherwise burn ~40 K wasted incref/decref pairs per load).

### Why pack-then-fwrite instead of per-slot fwrite

PR #483's write path did `fwrite(&b, 1, 1, fp)` once per cons
cell. Correct but it leaves a Byte/non-Byte type check stranded
mid-loop; if slot N is malformed the file is already partially
written and the caller sees `Err("write failed")` without
knowing where. Pack-then-fwrite separates the two concerns: walk
the array once to materialize the C buffer (failing fast and
cleanly if a slot is wrong), then one `fwrite` for the I/O. Same
total syscalls in the happy case under stdio buffering, simpler
failure semantics.

### Why no `array_from_list` shim inside the runtime

A tempting shortcut would be: keep the C runtime cons-list-shaped
and convert at the boundary inside `kai_prelude_file_read_bytes`.
But the conversion would happen in C with no awareness of the RC
discipline, and the resulting Array still pays the per-slot
default-init traffic. Pushing the conversion all the way down to
the file descriptor is the only path that compounds: every future
consumer (cache, crypto, raw image I/O) inherits the O(1) buffer
shape for free.

### Stdlib wrapper naming

The brief required `pub fn read_bytes` / `pub fn write_bytes`.
That collides with — well, almost. The prelude exports
`file_read_bytes` / `file_write_bytes`, prefixed; the bare names
`read_bytes` / `write_bytes` are free. So the wrappers can call
the prefixed prelude names from inside the bodies without
self-recursion. This matches what `exists` / `delete` / `rename`
do in the same file.

Note: the prelude also has `read_bytes` (no `file_` prefix) — the
issue #453 stdin-byte-oriented primitive. The wrappers in this
lane live behind `file.read_bytes(...)` / `file.write_bytes(...)`
(qualified), which the resolver maps to the namespaced exports
without colliding with the bare `read_bytes` prelude builtin.

## Structural surprises the brief did not anticipate

None worth flagging. The lane is mechanically a straight `[Byte]`
→ `Array[Byte]` substitution at four call sites; the only
non-trivial choice was the runtime allocation strategy (direct
`kai_alloc` vs `kai_array_make` + set loop), already discussed
above.

Selfhost stayed byte-identical the first time. File I/O is not on
the compiler's hot path; that was the expectation, and it held.

## Fixtures added and coverage gaps

No new fixtures. The two existing PR #483 fixtures were updated
to exercise the new Array[Byte] surface:

- `examples/stdlib/fs_file_bytes_roundtrip.kai` (golden unchanged:
  `0,255,65,10` / `equal`).
- `examples/stdlib/fs_file_bytes_missing.kai` (unchanged — it
  doesn't touch the buffer).

Wrapper coverage: `file.read_bytes` / `file.write_bytes` have no
dedicated fixture in this lane. Both forward verbatim to the
prelude builtins; verified manually via a smoke test that
constructs a buffer with `array_from_list`, writes, reads, and
checks the resulting Array slot-by-slot. The next lane that
actually consumes the wrappers (the Phase A.0 cache, #452) will
exercise them under load and add fixtures appropriate to that
surface.

## Real cost vs estimate

Estimate: ~0.5 day. Real: ~0.5 day. The runtime rewrite was the
only segment that needed careful thinking (the `kai_alloc` vs
`kai_array_make` tradeoff); the rest was substitution.

## Follow-ups left for next lanes

- **#452 Phase A.0 stdlib cache** — the immediate consumer.
  Should now be able to call `file.read_bytes(path)` and feed the
  result straight into `BinSerialize.from_bytes(...)` without an
  intermediate list-to-array hop.
- **`File` effect op surface for binary IO** — `file_read_bytes` /
  `file_write_bytes` remain bare prelude functions tagged with
  `File` via `prelude_effect_for`, not first-class ops in
  `builtin_file_decl`. If a downstream lane wants `File.read_bytes(...)`
  qualified syntax it'll need to wire up the new `EOp` entries
  plus a `Result[String, Array[Byte]]` type-builder helper.
- **Cleanup of `bytes/byte_array_*` migration** — none in scope.
  No other prelude or stdlib code consumes `file_read_bytes` /
  `file_write_bytes` today, so the breaking shape change is
  observable only inside the two test fixtures.
