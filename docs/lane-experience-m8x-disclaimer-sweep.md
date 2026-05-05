# Lane retro — `m8x-disclaimer-sweep` (residual sweep, 2026-05-05)

## Objective metrics

- Branch: `m8x-disclaimer-sweep` (cut from `4c8e3c4`, rebased onto
  `522d342` mid-lane).
- Wall time: ~16 min (start `2026-05-05T13:22-04:00` → end
  `2026-05-05T13:38-04:00`). Brief calibrated 1.5–2h; well under.
- Files touched: 11 prose-only edits across `stage0/runtime.h`,
  three `demos/`, six `examples/effects/`, two `stdlib/` files.
- Lines: +102 / −74 (no semantic change).
- Tier gates: tier0, tier1, tier1-asan, selfhost (stage1+stage2),
  selfhost-llvm — all green. Selfhost byte-identical (prose-only
  changes to runtime header and stdlib comments do not affect
  compilation output).

## The empirical proof

`bin/kai run demos/ping_pong/main.kai` round-robin output, before
and after the sweep:

```
A:1
B:1
C:1
A:2
B:2
C:2
A:3
B:3
C:3
all done
```

Identical pre- and post-sweep — this lane changes prose, not
behavior. The output itself is the proof that fibers actually
suspend (single-threaded interleaving rotates the run queue at every
`fiber_yield()` and at every empty `Actor.receive()`).

`bin/kai run examples/effects/m8x_4_request_reply.kai`:

```
client: sending ping
server: got ping
client: got reply pong:ping
```

`bin/kai run demos/concurrent/main.kai`:

```
supervisor up
7
13
20
all jobs reported
supervisor done
0
```

## What this lane found vs the brief

The brief assumed three stale paragraphs in `stage0/runtime.h`
(signal, sleep, process-wait) plus one doc-block in
`demos/concurrent/main.kai`, plus a missing two-actor request/reply
fixture. Reality on `4c8e3c4`:

- `examples/effects/m8x_4_request_reply.kai` and its
  `.out.expected` **already exist on `main`** — added by PR #73 (the
  earlier `m8x-disclaimer-sweep` round, commit `51e6e0e`). The
  Makefile wiring at `stage2/Makefile:1753-1765` is already there
  too. No fixture work needed.
- The three runtime-header paragraphs and the `demos/concurrent`
  doc-block were correctly identified as stale. Rewritten.
- The brief's "in-runtime contradicting paragraphs at L474 and
  L486-491" cited from the audit doc no longer exist — the m8.x
  audit lane already cleaned those.
- The brief said `stdlib/spawn.kai` and `stdlib/actor.kai` are
  clean and to skip them unless a stale paragraph is found. They
  are clean — confirmed.
- **Eight additional stale references** the brief's inventory
  missed (caught via repo-wide `grep -rn "inline-eager"`):
  - `stage0/runtime.h:303-307` — fiber-state-enum narrative; the
    brief annotation said "past-tense correct, no change needed",
    but its own acceptance gate said "ZERO matches". Rephrased
    without the literal token while preserving the past-tense
    narrative meaning.
  - `demos/net_tcp_localhost/main.kai:7`
  - `demos/ping_pong/main.kai:10` (past-tense comparison; rephrased
    to not name the dead term)
  - `stdlib/effects.kai:107` (NetTcp catalog limitations)
  - `stdlib/time.kai:46` (Clock `sleep_ns` description)
  - `examples/effects/m8_3_spawn_console.kai:3`
  - `examples/effects/m8x_2_yield_interleave.kai:4`
  - `examples/effects/net_tcp_localhost.kai:7`
  - `examples/effects/m8_3_spawn_await_value.kai:2`
  - `examples/effects/m8_9_supervision_pattern.kai:9` (also fixed
    a related stale claim that "Link and Monitor runtime registry
    is m8.x" — both have landed per Phase 5 + Tier 2; pointed
    readers at `m8_monitor.kai` / `m8_trap_exit.kai`).

After the sweep:

```
$ grep -rn "inline-eager" stage0/runtime.h demos/ stdlib/ examples/
(empty)
$ grep -rn "structural deficit" stdlib/ stage0/ demos/ examples/
(empty)
```

## The three runtime-header rewrites (before / after)

### 1. signal handler (`stage0/runtime.h` ~L60)

Before:
```
* blocking-only in v1 (the inline-eager scheduler can't suspend a
* fiber on a readiness event yet); kqueue / epoll integration lands
* with m8.x alongside the rest of the cooperative scheduler.
```

After:
```
* blocking-only: the m8.x cooperative scheduler (landed v0.4.0)
* suspends fibers on mailbox / await / yield, but we have no
* readiness reactor yet, so socket reads/writes park the OS thread
* rather than the fiber. kqueue / epoll integration is a Tier 2
* follow-up tracked in docs/fibers-honesty-targets.md §Reactor.
```

### 2. sleep handler (`stage0/runtime.h` ~L3128)

Before:
```
* v1 sleep blocks the OS thread; the m8 v1 inline-eager scheduler has
* no cooperative yield to deliver `Cancel` mid-sleep. Once m8.x ships
* the cooperative scheduler, this handler upgrades to register the
* fiber on a timer wheel and yield through `Spawn.yield`. Tracked in
* the m8.x follow-up.
```

After:
```
* sleep_ns blocks the OS thread inside `nanosleep` and is therefore
* not a yield point: `Cancel.raise` cannot be delivered mid-sleep
* because no reactor is registered to wake the fiber on a timer
* event. The m8.x cooperative scheduler (landed v0.4.0) has the
* suspend / resume primitives in place; the missing piece is a
* timer-wheel reactor that parks the fiber and resumes it via the
* scheduler's existing wake path. Tier 2 follow-up tracked in
* docs/fibers-honesty-targets.md §Reactor.
```

### 3. process-wait handler (`stage0/runtime.h` ~L3630)

Before:
```
*   - All ops blocking; the inline-eager scheduler (m8 v1) parks the
*     OS thread inside waitpid. Reactor-driven cancellation-aware
*     wait (the `wait_or_kill` shape) lands with m8.x.
```

After:
```
*   - All ops are blocking: `Process.wait` parks the OS thread
*     inside `waitpid` rather than the calling fiber. The m8.x
*     cooperative scheduler (landed v0.4.0) provides the suspend /
*     resume primitives, but reactor-driven cancellation-aware
*     waiting (`wait_or_kill`) still needs a SIGCHLD-aware reactor
*     plug that registers the pid and wakes the fiber when the
*     child terminates. Tier 2 follow-up tracked in
*     docs/fibers-honesty-targets.md §Reactor.
```

## The new request_reply fixture shape

Already on `main` from PR #73, repeated here for completeness:

```kaikai
fn echo_server(parent: Pid[String]) : Unit / Actor[String] + Console = {
  let req = Actor.receive()
  Stdout.print("server: got " ++ req)
  Actor.send(parent, "pong:" ++ req)
}

fn run() : Unit / Actor[String] + Spawn + Console = {
  let me     = Actor.self()
  let server = spawn_actor(() => echo_server(me))
  Stdout.print("client: sending ping")
  Actor.send(server, "ping")
  let reply  = Actor.receive()
  Stdout.print("client: got reply " ++ reply)
}

fn main() : Int / Console + Spawn = {
  with_mailbox { run() }
  0
}
```

The trace covers main spawning the server (server is enqueued READY,
no body run yet), main sending into a not-yet-parked server (queue,
no waiter wakeup), main parking on its own empty mailbox (kernel-of-
the-test: this is the only way the server's body reaches the
dispatcher), server running its body and replying, main resuming
inside `Actor.receive()`. Three lines of output — all that the
deterministic schedule can produce, by construction.

## Friction points

- **Brief inventory was incomplete.** Eight stale references beyond
  the three the brief named. The brief's gate (`grep -rn
  "inline-eager"` returns ZERO across `stage0/runtime.h` and
  `demos/`) was tighter than the fix list, which forced rewriting
  even one paragraph the brief explicitly said to leave alone
  (`stage0/runtime.h:303`, the past-tense fiber-state narrative).
  Resolving that meant rewording rather than deleting — preserves
  the past-tense comparison without naming the dead term.
- **Phantom file:** the brief said add a fixture; the fixture
  already existed on `main`. Branch was cut from `4c8e3c4` (the
  bump to 0.41.0); the earlier `m8x-disclaimer-sweep` PR #73 had
  already added the fixture and most of the original audit's
  stdlib-side rewrites. The brief read against an older snapshot.
- **Branch was 2 commits behind `origin/main`** at start (PR #282
  for `issue-174-poly-impl-constraint` had merged after the cut).
  Per memory entry on lane rebase discipline, ran `git stash` →
  `git rebase origin/main` → `git stash pop` → re-grep before
  committing. Rebase clean (no overlap with poly-impl-constraint —
  that lane touches `stage2/compiler.kai` + `stdlib/protocols.kai`,
  this lane touches comments).
- **No CHANGELOG / VERSION touch** per CLAUDE.md cz discipline.
  Conventional Commit type is `docs(...)`; it's excluded from the
  changelog and from `cz bump`.

## Subjective summary

The lane is small and mechanical once the inventory is settled.
The interesting bit is operational, not technical: the brief's
inventory was based on the audit doc rather than `grep` against the
current tree, and PR #73 had already done a partial sweep, so the
real residue was a different (larger) set than the brief named.
Lesson: when sweeping for stale terms, run the `grep` first and
treat the brief's named lines as a starting point, not a closed
inventory.

The Link / Monitor disclaimer in `m8_9_supervision_pattern.kai`
deserves a callout — it was claiming a Tier 2 feature was still
m8.x-future when in fact Phase 5 + Tier 2 had landed (per
`docs/lane-audit-m8x-state.md` §5). External readers who reached
that file would have concluded supervision was unfinished. Now
points at the real proofs (`m8_monitor.kai`, `m8_trap_exit.kai`).

## Limitations

The disclaimers about three things remain genuinely true and were
preserved (with framing updated to put the limitation on the
**reactor**, not on the scheduler):

- **Tier 2 — no readiness reactor for I/O.** Socket reads/writes,
  timer-driven sleep, and `waitpid` block the OS thread, not the
  fiber. The m8.x cooperative scheduler has suspend/resume; the
  missing piece is a kqueue/epoll/timer/SIGCHLD-aware reactor that
  registers the fiber and wakes it on the OS event. Tracked in
  `docs/fibers-honesty-targets.md §Reactor`.
- **Tier 3 — asm-level context-switch cost.** `ucontext` swap is
  ~1–2µs on macOS x86_64 vs the Tier 3 target of ~50–100ns. Per
  brief instruction, not surfaced in the runtime header — lives
  only in the honesty doc.
- **Structured-concurrency cancel-on-fail in `nursery`.** The
  surface accepts the children, but the body is still a typed
  pass-through — cancel-on-sibling-crash needs the nursery handler
  to wrap `Spawn` and observe child terminations through Link.
  Called out in the rewritten `demos/concurrent` doc-block.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-05T13:29:40-04:00	tier0	OK	53
2026-05-05T13:34:52-04:00	tier1	OK	304
2026-05-05T13:35:53-04:00	tier1-asan	OK	54
2026-05-05T13:36:34-04:00	selfhost	OK	34
2026-05-05T13:37:43-04:00	selfhost-llvm	OK	47
```
