# Lane experience — issue #771 Phase 1 (chunked / streaming file IO)

## Scope as planned vs. as shipped

**Planned (re-scoped 2026-06-09):** ship the low-level chunked primitive
set on the `File` effect — `open_read` / `read_chunk` / `open_write` /
`write_chunk` / `close_file`, an opaque `FileHandle`, and runtime
default handlers that route through the R1 thread pool with fiber
parking, identical to `read_file`. **Not** the originally-planned
Phase 2 line surface (`with_lines` / `fold_lines` / `each_line`) — that
was superseded by a stream-carrier design and re-homed to #801. The
write side (`open_write` / `write_chunk`) was added to Phase 1 by the
re-scope comment because the carrier's `write_lines` sink needs it.

**Shipped:** exactly that. Five new ops on the `File` effect, a
`FileHandle` opaque record `{ fd: Int }`, ten runtime functions (five
prelude primitives + five pool-parking default handlers) in **both**
`runtime.h` copies, five LLVM forwarders, three fixtures (real disk
roundtrip, mock-via-handle, negative open), and the catalog doc
updates. The PR refs #771 but does not close it — the surface tracked
in #801 is what closes the umbrella.

## Design decisions and alternatives

- **Ops on `File`, not prelude builtins.** The decisive payoff is
  mockability: `handle ... with File { open_read(...) -> ... }` lets a
  test intercept the whole chunked stream without touching disk
  (`file_stream_mock.kai` proves it). This also made the compiler work
  almost free — the `default {}` block is auto-generated from the ops
  list by `builtin_default_block_for`, and `File` is already pinned in
  both install orders, so adding ops to `builtin_file_ops()`
  automatically wires the `kai_default_file_<op>` symbols the emit
  expects. No emit_c / emit_llvm edit was needed.

- **`FileHandle = { fd: Int }`, modelled on `Conn`.** The net `Conn`
  handle is the exact precedent: an opaque single-field record, built
  in the runtime by `_kai_file_make_handle` and read back by
  `_kai_file_handle_fd`. Reusing that shape kept the surface and the
  RC discipline boring.

- **`write_chunk : Result[String, Unit]`, NOT `Result[Unit, String]`.**
  The issue prose writes `Result[Unit, String]` Ok-first, but kaikai's
  `Result[e, a]` is Err-first. The legacy `write_file` decl carries the
  inverted `ty_result_unit_string_zero` shape (a known bug, flagged in
  `stdlib/fs/file.kai`); the `ty_result_string_unit_zero` helper exists
  precisely so new ops read as `Ok(())` / `Err(msg)` correctly. Used it.

- **`read_chunk` EOF = `Ok("")`, never `Err`.** Copies the spec from
  the issue and matches `Stdin.read_line`'s "absence is structural, not
  a fault" rule. A short read (0 < n < max) is normal and returned
  as-is; the consumer loops until the empty string.

- **Raw POSIX `open`/`read`/`write`/`close`, not stdio `FILE*`.** The
  bulk ops use `fopen`/`fread`, but the chunked ops want a long-lived
  descriptor with explicit partial-read/partial-write semantics, which
  is cleaner on the raw fd. The fd lives inside the `FileHandle` across
  calls; stdio buffering would fight the chunk boundaries.

## Structural surprises the brief did not anticipate

- **The compiler side is tiny.** Most of the lane's mass is the two
  runtime.h copies (the real work) plus fixtures + docs. The
  effect-catalog machinery (auto-generated default block + pinned
  install order) meant the typer / emit changes were three edits in
  `driver.kai` (ops list, `FileHandle` decl, injection) and zero
  elsewhere. No `infer.kai` prelude-sig entry was needed — the ops are
  called qualified (`File.open_read`), so the effect decl alone makes
  them visible, unlike the bare `read_file` shortcut.

- **The Int-tagging trap is real and load-bearing.** stage2 boxes small
  Ints tagged, so `_kai_file_handle_fd` must read the `fd` slot with
  `kai_is_int` / `kai_intf`, never `->tag` / `->as.i` (which segfault on
  a tagged `0x1`). stage0's copy uses `->tag` / `->as.i` directly. The
  net `Conn` code already diverges exactly this way between the two
  runtimes; I mirrored it. String args (`path`, `data`) stay
  `->tag == KAI_STR` in both — a String is always a heap pointer.

## Fixtures added and coverage

- `examples/effects/file_stream_roundtrip.kai` — real disk: `open_write`
  → two `write_chunk` → `close_file`, then `open_read` → `read_chunk(4)`
  loop to EOF → `close_file`, reassembling "hello streaming world".
- `examples/effects/file_stream_mock.kai` — `handle ... with File`
  intercepting `open_read` / `read_chunk` / `close_file`, never touching
  disk. Proves interceptability.
- `examples/effects/file_stream_open_missing.kai` — negative: `open_read`
  of an absent path → inspectable `Err(msg)`, branched and printed.

All three wired into `stage2/Makefile`'s `test-effects` target, each
grepping the `kai_default_file_*` wiring in the emitted C and asserting
exact stdout. Coverage gap: a mid-stream `read_chunk` *fault* (disk
dies after open) is not fixtured — it is hard to provoke deterministically
and the negative `open_read` case already exercises the `Err` register.

## Cost vs. estimate

Close to the "mechanical given `write_file` is the template" framing in
the re-scope. The only non-mechanical judgement calls were the
`Result` arg-order (Err-first) and the raw-fd-vs-stdio choice, both
settled by precedent (`ty_result_string_unit_zero`, `Conn`).

## Follow-ups for next lanes

- **#801** — the ergonomic line surface (`with_lines` / `fold_lines` /
  `each_line`) and the stream carrier, built on these primitives. The
  re-scope notes it needs a resumable `ReadFault` effect for skip-bad-
  chunk recovery (stdlib `Fail.fail` returns `Nothing`, non-resumable);
  that belongs to #801, not here.
- **Cancel-safety.** An open `FileHandle` leaks its fd if a fiber is
  cancelled between `open_*` and `close_file`. Documented as a known
  limitation (same honesty treatment as the non-atomic `append`); the
  cancel-aware bracket closes it post-MVP.
- **Pool routing for the rest.** `read_bytes` / `write_bytes` and the
  `exists`/`delete`/`rename` builtins still run inline, not on the R1
  pool — unchanged by this lane, noted in the §`File` v1 sidebar.
