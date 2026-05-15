# Lane retro — issue #620 Phase R3 reactor (stdin parks the fiber)

Closed: 2026-05-15. Branch: `issue-620-reactor-r3`. Parent: #474.
Sibling: #611 (R1, shipped same day).

## Scope as planned vs. scope as shipped

**Planned (issue #620 body):** non-blocking stdin through the
scheduler-integrated reactor. Two surfaces — `Stdin.read_line` and
`Stdin.read_bytes` — should park the *fiber* via the readiness
primitive (kqueue / epoll) rather than blocking the OS thread on
`fgetc(stdin)` / `fread`. Singleton `kai_reactor_stdin_waiter` slot
(rejecting concurrent readers with a clear panic), `O_NONBLOCK` set
on fd 0 once per process, `atexit` restoration of the original
flags. ~80–120 LOC; 0.05–0.1 agent days estimated.

**Shipped:** the two handlers now park the fiber on the reactor as
specified. The wait primitive is **not** kqueue / epoll — R1 shipped
with `poll()` over self-pipes (see the R1 retro), and R3 simply
extends that `poll()` set with a third entry for `STDIN_FILENO`
whenever the singleton slot is held. `O_NONBLOCK` + `atexit`
restoration land exactly as designed. New demo
`demos/stdin_concurrent` proves the wake path runs and exits
cleanly under the regression harness. The `demos` baseline moves
27 → 31 (from R1) → 32 (this lane). Three doc surfaces updated:
`docs/effects-stdlib.md` §`Stdin` gets a v1-status sidebar mirroring
the File / Clock ones; `docs/fibers-honesty-targets.md` gets a
shipped bullet and a Tier 2 row; the `NetTcp` sidebar is left
untouched (R2 is its lane).

Real cost: ~1 hour. Below the 0.05–0.1d estimate because R1 already
proved every load-bearing primitive (poll set, parked-count
discipline, fiber park/unpark across the reactor wake) — R3 was
mechanical reuse plus the singleton-slot bookkeeping.

## Design decisions and alternatives considered

### Wait primitive: re-use the R1 `poll()` set

The issue body sketched `kqueue(EVFILT_READ)` / `epoll(EPOLLIN)`
with a per-platform branch. R1's retro had already justified
`poll()` for the same reason it applies here: the third FD slot in
`kai_reactor_wait` adds 4 lines, while a kqueue/epoll branch would
add ~100 lines of `#ifdef __APPLE__` / `__linux__` plumbing for
zero new capability. The platform-native primitives become
load-bearing when R2 (sockets) needs efficient many-FD readiness;
R3 watches at most one FD at a time (the singleton slot) so
`poll()` over a 3-element array stays well under the noise floor.

R2 will also swap the wait primitive when many sockets land —
`kai_reactor_wait` is the only function that needs to change.

### Singleton slot vs. per-fiber waiter list

Stdin is a process-shared FD. Two fibers calling `read_line()`
concurrently would shred each other's bytes (the kernel has no
notion of "this byte belongs to fiber X"). A waiter *list* would
implement the wrong semantics. A singleton slot makes the bug
visible — the second fiber's parking attempt panics with a clear
diagnostic suggesting the actor-serialisation pattern, mirroring
the discipline already enforced for stdout / stderr.

### `O_NONBLOCK` + `atexit` restoration

The footgun the issue body called out: a kaikai program exits and
leaves the user's shell with `O_NONBLOCK` set on fd 0; the next
shell command sees `EAGAIN` on every read and behaves
incomprehensibly. The runtime saves the original flags into
`kai_reactor_stdin_orig_flags` before flipping the bit and
registers `kai_reactor_stdin_restore` via `atexit`. Manual
verification: ran `demos/stdin_concurrent`, exited, then ran
`echo "test" | cat` in the same shell — behaved normally.

The restore is registered exactly once (guarded by the same
sentinel that guards the flag flip), so a program that opens stdin
multiple times still installs only one atexit slot.

### Lazy nonblock: at first stdin op, not at runtime init

Two options were considered:

1. Set `O_NONBLOCK` from `kai_reactor_init()` on first
   sleep / file / process op.
2. Set `O_NONBLOCK` on first stdin op only.

The lane chose (2). A program that never reads stdin (every
existing demo *except* `stdin_concurrent`) never modifies fd 0's
flags, so there is no observable difference for compute-only or
file-only workloads. Choosing (1) would have flipped the flag for
the entire `demos/` set on every run, increasing the surface area
for `atexit` slot exhaustion or shell-mode regressions if this lane
shipped a subtle bug. Lazy-on-stdin keeps the blast radius local.

### Wake-path: POLLHUP / POLLERR also count

`poll()` reports POLLHUP when the writing end of a pipe closes
(`echo foo | program` after `echo` exits) and POLLERR on rare
device errors. The drain treats all three as wake events; the
handler's `read()` retry loop will observe the EOF / errno on the
following call. Treating only POLLIN as a wake would have stranded
fibers waiting on a closed pipe — the kernel never sets POLLIN once
the writer is gone, so the demo's "reader eof" line would never
print.

### Read one byte at a time (read_line) vs. larger buffer

`read_line` reads a single byte per `read()` call. A larger buffer
would amortise syscalls but introduces a buffer-management problem:
a partial read of `"abcdef\nXYZ"` would consume `XYZ` bytes that
belong to the *next* `read_line()` call, requiring a per-fd unread
buffer. Stdin is line-oriented for the typical caller (REPL / CLI
prompt), so the syscall amortisation matters less than keeping the
runtime stateless. `read_bytes(n)` does the larger-buffer thing
because the caller specified the exact byte count.

If LSP framing or another high-throughput stdin reader becomes a
profile target, the right fix is a per-process line buffer in the
runtime, not a per-call buffer; deferred.

### Real I/O errors collapse to None

`Stdin.read_line` is typed `: Option[String]` (m7a's
simplification of Doc B's `: Option[String] / Fail`). When the
read syscall fails with anything other than EAGAIN / EINTR, the
handler resumes with `None` rather than panicking. This keeps
parity with the pre-R3 `fgetc` path which silently mapped read
errors to EOF. The `Fail`-propagating shape is the m7a follow-up
the catalog calls out.

## Fixtures added

`demos/stdin_concurrent/` — a nursery containing a stdin reader and
a 3-step compute loop. The harness invokes the binary with no piped
input, so `read_line()` resolves to `None` immediately and the
demo's golden is deterministic:

```
reader eof
compute step
compute step
compute step
ok
```

Manual sanity (not part of the regression harness — wall-time
behaviour, not exit-status): with slow piped input
`(echo a; sleep 0.5; echo b) | demo`, the compute fiber's three
"compute step" lines print **before** "reader eof", proving the
reader is parked while compute runs. Pre-R3 the same input would
print "compute step" three times only after the reader returned —
the OS thread would be stuck in `fgetc`.

`demos/baseline.txt` bumped 31 → 32.

## Coverage gaps

- `examples/effects/reactor_stdin_*` test cases for cancel-during-
  park semantics are NOT in this PR. R3 inherits R1's cooperative-
  cancellation contract: cancel arrives at the next yield-point
  hook after the wake, not mid-syscall. The cancel-aware redesign
  queues for R2 (where it is load-bearing for socket reads).
- Multi-fiber stdin panic is not covered by an automated fixture —
  the panic exits the process, which is awkward to assert against
  in the demo harness. A `should_panic`-style fixture in
  `examples/effects/` is queued for the diagnostic-coverage lane.
- Terminal modes (raw / echo off) are out of stdlib v1; programs
  that need them can FFI into `tcsetattr`. The reactor does not
  interfere — it only flips `O_NONBLOCK`, which is orthogonal to
  termios state.

## Real cost vs estimate

Estimated 0.05–0.1 agent days (~1–3 hours). Actual: ~1 hour. Time
sinks:

- ~10 min: reactor inventory + reading the R1 retro to confirm the
  `poll()` shape.
- ~20 min: writing the slot, helpers, handler rewrites, and the
  poll-set extension.
- ~5 min: forward-decl placement (the stdin handlers live ~1700
  lines above the reactor implementation; an explicit
  `kai_reactor_park_stdin` helper threads cleaner than exposing
  the global slot pointer).
- ~10 min: demo + golden + manual piping verification.
- ~15 min: docs + this retro.

## Follow-ups for next lanes

- **R2 (TCP sockets) — Orongo.** The reactor's `kai_reactor_wait`
  needs to grow real readiness FDs in its `poll()` set (or swap to
  `kqueue` / `epoll` for many-socket workloads). The R3 wire is
  the proof-of-concept: a single FD added to the poll array, a
  drain that wakes the parked fiber on POLLIN, the same `O_NONBLOCK`
  + EAGAIN-park pattern at the syscall site. The structural
  difference is per-fiber waiter lists (each socket has its own
  fiber) instead of a singleton slot.
- **Cancel mid-syscall.** R3 explicitly does not deliver Cancel
  inside the parked read. The cancel pad fires at the next
  yield-point hook after the wake. R2's lane drives the redesign
  because socket reads are the surface where mid-syscall cancel
  matters most.
- **Stdin `Fail` propagation.** The m7a simplification swallows
  read errors as `None`. When `Fail` is in the row routinely (as
  Doc B specifies), surface real errors via `Fail.fail("read_line:
  …")` instead of `None`.
- **`read_line` line buffer.** A per-process unread buffer would
  let `read_line` use a larger `read()` and still preserve byte
  ordering for the next caller. Deferred until LSP-shaped readers
  show up on a profile.
- **`should_panic` fixture for multi-fiber stdin.** The demo
  harness has no way to assert "this program panics with this
  message". A small extension to `examples/effects/` machinery
  (or a one-off shell test) would cover the singleton-slot
  diagnostic.
- **Terminal modes.** Raw mode / echo off / non-canonical line
  discipline live behind FFI today. A future `Tty` effect could
  bundle them; not on the v1 path.

## Lessons load-bearing for future runtime lanes

1. **Lazy nonblock keeps the blast radius local.** Setting
   `O_NONBLOCK` only at the first stdin op (rather than at reactor
   init) means compute-only and file-only programs are byte-
   identical to pre-R3 — even their fd-0 flags. A bug in this lane
   could only damage programs that actually read stdin.

2. **POLLHUP / POLLERR are wake events too.** Treating only POLLIN
   as a wake stranded fibers waiting on a closed pipe in the first
   draft. The fix was a one-line bitmask change in the drain.

3. **A singleton slot is the right shape for shared FDs.** Stdout /
   stderr / stdin are process-wide; multiple fibers writing /
   reading concurrently is a logic bug, not a feature to support.
   The runtime panics with a fix-suggesting message ("serialize via
   an actor") instead of silently shredding bytes.

4. **`atexit` restoration matters even for "small" flag flips.**
   `O_NONBLOCK` on the user's shell is invisible inside the kaikai
   program but extremely confusing immediately after exit. Every
   kernel-state change made by the runtime should have a paired
   `atexit` cleanup; the cost is ~5 lines and a one-line manual
   verification.

5. **Forward-decl a helper, not the global state.** The stdin
   handlers live ~1700 lines above the reactor implementation. An
   earlier draft tried to forward-declare the global slot pointer
   and the parked-count, but `static` redefinition rules are
   awkward in C99. Exposing a single `kai_reactor_park_stdin(f)`
   helper and putting its body next to the other `kai_reactor_park_*`
   functions is cleaner and matches the R1 idiom.
