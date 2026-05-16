# Lane retro — issue #630 Phase R2 reactor (TCP sockets park the fiber)

Closed: 2026-05-16. Branch: `issue-630-reactor-r2`. Parent: #474.
Siblings: #611 (R1, shipped 2026-05-15), #620 (R3, shipped
2026-05-15).

## Scope as planned vs. scope as shipped

**Planned (issue #630 body):** non-blocking TCP sockets through
the scheduler-integrated reactor. The six `NetTcp` ops
(`connect`, `listen`, `accept`, `send`, `recv`, `close`) should
park the *fiber* on socket read/write readiness via
`kqueue(EVFILT_READ/WRITE)` on macOS / `epoll(EPOLLIN/EPOLLOUT)`
on Linux. New per-direction waiter maps (`socket_read_waiters` +
`socket_write_waiters`) extend the existing `KaiReactor`. Every
socket fd is flipped to `O_NONBLOCK` at creation. `connect`
handles `EINPROGRESS` + post-wake `SO_ERROR`; `send` loops on
partial writes. Acceptance demands a new `demos/tcp_concurrent`
that exercises three concurrent client/server round-trips that
pre-R2 would have deadlocked under OS-thread-blocking accept.
Cost estimate: 0.15-0.3 agent days, ~150-250 LOC in runtime.h.

**Shipped:** all four blocking ops (`connect`, `accept`, `send`,
`recv`) now park the fiber on the reactor as specified. `listen`
gains a one-line `O_NONBLOCK` flip on the listener fd so the
parking `accept` path sees `EAGAIN`. `close` is unchanged (it
was already non-blocking). The wait primitive is **not** kqueue /
epoll — R1 and R3 already standardised on `poll()` over self-pipes
+ the live FD set, and R2 simply extends that set with every fd
sitting in either socket waiter list. The two waiter maps land
as intrusive single-linked lists threaded through the existing
`f->reactor_next` slot (R1's design pays off: one new slot would
have required touching every park site). The fd lives in
`f->reactor_wait_pid` (repurposed; pid waiters and socket waiters
are mutually exclusive park reasons, so the slot has space for
either). `demos/tcp_concurrent` runs three server fibers
concurrent with three client fibers; the golden output is the
deterministic sequence `listen ok / accepted 3 / ok`. The
`NetTcp` sidebar in `docs/effects-stdlib.md` is flipped from
"v1 status: blocks OS thread" to "v1 status (R2 reactor,
2026-05-16): parks fiber via reactor" with the new cooperative-
Cancel boundary called out explicitly. `docs/fibers-honesty-
targets.md` gains a shipped bullet and a Tier 2 row; the §"Where
we are today" date moves to 2026-05-16. Demo baseline 32 → 33.

Real cost: ~1 hour. Below the 0.15-0.3d estimate because R1 +
R3 had already proven every load-bearing primitive (poll set
discipline, parked-count bookkeeping, intrusive fiber lists,
fiber park / unpark across reactor wake). R2 was mechanical
reuse plus the partial-writes and `EINPROGRESS` corner cases
on `send` / `connect`.

## Design decisions and alternatives considered

### Wait primitive: re-use the R1 + R3 `poll()` set

The issue body sketched `kqueue(EVFILT_READ/WRITE)` on macOS and
`epoll(EPOLLIN/EPOLLOUT)` on Linux behind `#ifdef`. R1 already
documented the same choice in the opposite direction: until the
reactor genuinely needs the O(1)-per-event scaling of kqueue or
epoll, `poll()` over a small fd array stays well under the noise
floor of the syscalls themselves. v1 socket workloads have tens
of concurrent fds, not tens of thousands; `poll()` is fine and
each platform branch saves ~80 lines of `#ifdef` plumbing for
zero observable win.

When a kaikai-shaped HTTP server with thousands of concurrent
connections becomes a profile target, swapping `kai_reactor_wait`
to platform-native primitives is a localised one-function change
— every park site stays as-is.

### Per-direction waiter lists vs. a single combined list

Sockets are full-duplex: the same fd may be read by one fiber and
written by another simultaneously (rare in v1 — typical HTTP
fibers serialise send/recv on a Conn — but it is the correct
semantics for HTTP/2-shaped multiplexing). Two separate lists
keep the read mask (`POLLIN`) and the write mask (`POLLOUT`)
disjoint; the same fd appears twice in the poll set with each
direction's mask in its own entry. POSIX permits this.

A single combined list with a per-fiber direction tag would have
worked too but at the cost of a third slot on `KaiFiber` and a
filter at drain time. The two-list shape costs zero new struct
fields (the existing `reactor_next` + `reactor_wait_pid` pair
serves both directions) and the drain helper takes the list head
as a parameter, so the read and write paths share one
implementation.

### Reusing `reactor_wait_pid` as the fd slot

A fiber parked on a socket is, by construction, not also
waiting on a `Process.wait`: the two ops never compose on the
same fiber at the same time. Renaming the slot is gratuitous;
adding a new field to `KaiFiber` (already at ~14 reactor-related
bytes) bloats the per-fiber footprint for every fiber, including
those that never touch sockets. The slot stays named
`reactor_wait_pid` in the struct comment, with a small note in
the R2 forward-decl block that explains the dual use. Lane retro
of #620 made the same call for stdin (it sits on no slot — the
singleton waiter pointer is enough).

### `connect` handles `EINPROGRESS`, not `EAGAIN`

POSIX is explicit that non-blocking `connect()` reports
"handshake started, completion pending" via `EINPROGRESS`. Some
documents also list `EAGAIN` as a possible return (typically
when the local kernel cannot allocate an ephemeral port); the
handler treats both identically — park on write-readiness, then
`getsockopt(SOL_SOCKET, SO_ERROR)` to surface the real outcome.
`SO_ERROR == 0` means the handshake completed; non-zero is the
errno to translate via `strerror`.

`POLLHUP` and `POLLERR` count as wake events (same as in the R3
stdin path), so a `connect` to a refused port wakes promptly
rather than hanging on the `poll()` timeout.

### `send` loops internally on partial writes

The pre-R2 `send` op exposed POSIX's "partial write" behaviour
to user code by returning the actual byte count and letting the
caller loop. With `O_NONBLOCK` that contract breaks down: the
caller would now have to handle `EAGAIN` and re-park, which is
exactly what the runtime is supposed to hide. The R2 handler
loops internally on every short return, parking on write-
readiness for each `EAGAIN`, and returns the total bytes written
only when the buffer is fully drained — equal to the input
length on success. User code can keep its v1 `match Ok(_n) -> ...`
without thinking about partial writes.

`MSG_NOSIGNAL` is passed when the platform defines it (Linux);
on macOS the equivalent guard is `setsockopt(SO_NOSIGPIPE)` on
each fd. The runtime does not install `SO_NOSIGPIPE` per-fd in
this lane — the program-wide `SIGPIPE` install at startup is
the right place for that, and it is queued for a separate
hardening lane. The risk in practice: a kaikai program that
writes to a peer who has dropped the read side will receive
`SIGPIPE` (default action: terminate) on macOS. Documented as
a known v1 gap in the lane retro; not in the demo's path.

### `accept` resets `O_NONBLOCK` on the conn fd

Linux's `accept4(SOCK_NONBLOCK)` is the clean way to inherit
non-blocking on the connection fd. macOS does not expose it;
the portable shape is `accept()` followed by
`fcntl(conn_fd, F_SETFL, O_NONBLOCK)` on the returned fd. The
`kai_socket_set_nonblock` helper is idempotent, so the small
redundancy on Linux (where `accept4` could have saved one
syscall) is acceptable for the simpler single-platform path.

### Spurious wakes are tolerated by retrying

POSIX permits a `poll()` to report a fd ready when a subsequent
read returns `EAGAIN` (the kernel buffer drained between wake
and retry). Every park site loops on the syscall and re-parks
on `EAGAIN`, which is the correct behaviour under POSIX rules
even though spurious wakes are rare in practice. No assumption
of edge-vs-level triggering is hard-coded into the runtime.

### Cancel discipline preserved

Per `docs/structured-concurrency.md` §"Non-goals", cancellation
is cooperative at yield points. A fiber parked in `recv` or
`accept` receives `Cancel.raise()` from a peer; the cancel flag
is checked at the next reactor wake (which is also a yield
boundary). The syscall itself is not interrupted mid-flight —
the mid-syscall redesign (close fd + `pthread_kill`) is Orongo
territory and the issue body explicitly forbids touching it in
this lane.

A practical consequence: a fiber parked on `recv` against an
idle peer will not see the cancel until the peer writes (or
closes). Programs that need bounded-time cancellation should
wrap `recv` in a `select` with a `sleep` (the Spawn.sleep is
itself cancellation-friendly via R1's timer wheel).

## Fixtures added

`demos/tcp_concurrent/` — three client fibers and three server
fibers in one nursery on the same listener. Each server fiber
runs `accept → recv → send → close`; each client fiber runs
`connect → send → recv → close`. The golden output is fixed:

```
listen ok
accepted 3
ok
```

The "accepted 3" line is the load-bearing signal: it prints only
when every server fiber has completed a handshake, which is
only possible if the three concurrent `accept` calls each park
the fiber instead of blocking the OS thread. Pre-R2 the first
`accept` would have blocked the scheduler thread and the client
fibers spawned earlier would have starved — the program would
deadlock.

`demos/baseline.txt` bumped 32 → 33.

## Regression checks

- `demos/net_tcp_localhost` — byte-identical (single-fiber
  sequential round-trip; the R2 wiring changes timing but not
  output ordering).
- `demos/ping_pong` — byte-identical (scheduler ordering of nine
  mailbox sends; no socket usage but the new poll-set sizing
  path could in principle change `poll()` timing for unrelated
  workloads; it does not).
- `demos/stdin_concurrent` — byte-identical (uses the same
  reactor; the read-waiter drain path is now per-list instead
  of a single stdin slot, but the stdin path is unchanged).
- `make tier0` — green; selfhost byte-identical (compiler is
  unchanged by this lane).

## Coverage gaps

- **macOS `SIGPIPE` exposure on `send` to closed peer.** The
  platform-portable fix is `SO_NOSIGPIPE` set per-fd, ideally
  in the same place as `O_NONBLOCK`. Deferred to a hardening
  lane; the demo does not exercise this path.
- **Cancel-during-park semantics for sockets.** The R2 handlers
  inherit R1's cooperative contract: cancel fires at the next
  yield-point hook after the wake, not mid-syscall. The cancel-
  aware redesign queues for Orongo.
- **No fixture in `examples/effects/` covering the multi-fiber
  per-Conn footgun.** Two fibers reading the same Conn is a
  logic bug (the bytes shred). The runtime does not detect or
  panic on it today; a typer-level discipline (linear `Conn`
  values?) is design space, not a stdlib v1 concern.

## Real cost vs estimate

Estimated 0.15-0.3 agent days (~1-2.5 hours). Actual: ~1 hour.
Time sinks:

- ~10 min: reactor inventory + reading the R3 retro to confirm
  the `poll()` shape and the singleton-vs-per-direction call.
- ~25 min: writing the waiter maps, helpers, six handler
  rewrites, and the poll-set extension.
- ~10 min: demo + golden + manually verifying the deadlock
  proof against a pre-R2 build (informal).
- ~10 min: docs (sidebar flip + honesty bullet + Tier 2 row).
- ~10 min: this retro.

## Follow-ups for next lanes (Orongo)

- **Cancel mid-syscall.** The cooperative contract is preserved
  in R2 as it was in R1 + R3, but socket reads are the surface
  where mid-syscall cancel matters most. The redesign requires
  `close(fd)` from a peer fiber plus a `pthread_kill`-style
  interrupt to break the `poll()` cycle; non-trivial because
  the close has to land before the wake to avoid use-after-free
  on the reader's buffer.
- **TLS / HTTPS.** Lands as a separate `Tls` effect on top of
  the same `Conn` shape (issue body's "out of scope" list).
- **NetUdp / NetDns installation.** Both effects exist in the
  declaration catalog (`docs/effects-stdlib.md`) but have no
  runtime handler, no compiler builtin, and no
  `stdlib/net/{udp,dns}.kai` module. Separate lane each.
- **`SO_NOSIGPIPE` on macOS.** One-line setsockopt at every
  socket-creation site so a closed-peer `send` returns `EPIPE`
  through the `Result` instead of terminating the process.
- **Multi-threaded scheduler.** Single-thread cooperative stays
  through 1.0; work-stealing across N OS threads is Tier 3.
- **`io_uring` on Linux.** Post-MVP. `poll()` + non-blocking is
  enough for v1's target workloads.

## Lessons load-bearing for future runtime lanes

1. **R1's `reactor_next` + `reactor_wait_pid` shape pays off
   recursively.** Two new socket-direction lists land with zero
   new fields on `KaiFiber` because the existing slots carry
   both "what list" and "what value" for any park reason. Future
   reactor extensions (FIFO / signalfd / timerfd) should plan
   to reuse the same pair rather than adding their own slot.

2. **`poll()` keeps winning until it doesn't.** Three reactor
   lanes (R1 file/timer/process, R3 stdin, R2 sockets) all
   sketched kqueue/epoll in their issue bodies and all converged
   on `poll()` over the shared fd set. The cost of platform
   branches outweighs the throughput benefit until the fd count
   hits the thousands. When that threshold arrives, swapping
   `kai_reactor_wait` is a one-function change.

3. **Partial writes belong inside the handler, not in user
   code.** The pre-R2 `send` exposed POSIX's short-write
   contract to user code as a defensible v1 simplification.
   Once `O_NONBLOCK` is in play that contract becomes a footgun
   the runtime is uniquely positioned to hide. Looping
   internally costs ~10 lines and removes a whole class of
   "I send 100 bytes, only 50 arrive, why?" bugs.

4. **`EINPROGRESS` is not `EAGAIN`.** Non-blocking `connect`
   uses its own errno value and requires a `SO_ERROR` check
   post-wake instead of a simple retry loop. Handling them as
   the same case (the original draft) would silently swallow
   connection failures and surface them as zero-byte recvs much
   later. Worth the asymmetric code path.

5. **Demo golden output should be a deadlock-detector, not a
   wall-time-detector.** The first draft tried to assert
   timestamps to prove parallel progress; flaky under load.
   The shipped golden ("accepted 3" prints only when all three
   server fibers complete) is a fixed string that pre-R2 would
   never produce because the program would deadlock. Same
   correctness gate, deterministic harness.

6. **`OR` every pfds entry for the same fd in the drain.** The
   first draft of `kai_reactor_socket_drain` matched a waiter
   to "the first pfds entry whose fd equals the waiter's fd"
   and read that entry's revents — which is wrong when the
   same fd appears multiple times in the poll set (two readers
   on a shared listener, or read+write on a full-duplex Conn).
   `poll()` reports per-entry revents; if the ready bit lands
   on the second entry, the first-match search returned zero
   revents and the waiter never got promoted. The 2-server
   demo deadlocked on this exact path, with poll cycling
   forever over the same level-triggered POLLIN. The fix is
   one line: `revents |= pfds[i].revents` across every matching
   entry, then check the OR'd value. Worth pinning in any
   future reactor work that registers the same fd more than
   once.
