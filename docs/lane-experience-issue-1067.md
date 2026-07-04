# Lane experience — issue #1067: fiber-level trap isolation

## Scope as planned vs as shipped

**Planned (issue #1067):** a runtime trap — array out-of-bounds, division by
zero, non-exhaustive match — inside a fiber calls `exit(1)` and kills the whole
process. Route traps to a fiber-level abort (longjmp to the pad, as `Cancel`
already does), so a surrounding supervisor contains the fault and the rest of
the process survives. Motivating case: the `rongo` HTTPS server runs each
connection on its own fiber inside a nursery and already catches a handler's
`Cancel.raise()` (turns it into a 500), but a runtime trap in a handler
bypasses that and takes the whole server down.

**Shipped:** the runtime now routes to the fiber pad every recoverable trap
that *already* reaches a runtime helper with `exit(1)` today:

- index out of range — `kai_array_get_impl` / `kai_array_set_impl` (the clear
  case, and the exact shape `rongo` hits: `xs[req_index]`),
- divide/mod by zero through the boxed helpers `kai_op_div` / `kai_op_idiv` /
  `kai_op_mod` / `kai_fixed_div`,
- non-exhaustive match through `kai_prelude_panic` (the C backend's runtime
  fallthrough).

With a pad installed the trap unwinds to the fiber and a trap-exit supervisor
receives a distinguishable `"Trapped: <msg>"` reason. With no pad (top level)
the process still terminates with a message and a non-zero exit — the
pre-existing behaviour, preserved.

**Deliberately out of scope (two preexisting UB gaps — see Follow-ups):** the
two of the three trap kinds that do *not* reach a runtime helper today emit
undefined behaviour instead of trapping, and closing them changes emitted code
(breaks byte-id). They are a separate lane, not #1067.

## Design decision — Trap vs Cancel channel (asu consult)

The issue left open whether to surface the trap as (a) a new `Trap`-flavoured
raise or (b) fold it into the existing Cancel/failure channel with a
distinguishable reason. Consulted `asu`; the ruling, which the implementation
follows:

- **Third distinguishable exit reason, not folded into Cancel.** Added
  `KAI_EXIT_TRAPPED` alongside `KAI_EXIT_NORMAL` / `KAI_EXIT_CRASHED`. The
  decisive precedent is Erlang: the termination *reason* distinguishes a
  crash-from-bug from a clean exit, and that distinction is what makes "let it
  crash" work. `"Crashed"` already means intentional-cancel-via-trap_exit in
  this runtime; folding traps there would collapse wanted-vs-bug. The cost is
  one more value in an existing enum, not a new channel.
- **Supervision layer, not algebraic effects.** A `Trap` effect would be viral
  and useless in the row (every function that can index an array would carry
  `/ Trap`), and a lexical `with Trap` handler is the wrong abstraction because
  a trap has no sane continuation to resume. The right model is BEAM's
  `{'EXIT', Pid, Reason}`: the fiber dies, the supervisor observes the reason
  through the mailbox under `trap_exit=true`.
- **Soundness of longjmp across RC frames.** The longjmp skips the intervening
  frames' decrefs, so it leaks — it never dangles (the direction is safe: RC
  too high, not too low). The fiber's private heap is freed whole on death, so
  the leak is not observable. This is exactly what `Cancel` already does; the
  trap path is no less sound. Verified with ASAN over both fixtures: zero real
  findings (only the known `__asan_handle_no_return` warning for
  makecontext/swapcontext fibers, sanitizers/issues/189).

### My scope decision on the reason payload

asu flagged that the mailbox delivers *strings* today (`"Normal"`/`"Crashed"`)
and suggested a structured variant reason to carry the trap message robustly. I
kept the string channel and added `"Trapped: <msg>"` as a third
prefix-distinguishable reason — a structured-variant reason is a redesign of
the actor mailbox surface (today `Actor[Msg]` delivers the user's message type;
trap-exit reasons already ride in as a `KAI_STR` v1 simplification), which is
over-reach for #1067. Left as a follow-up.

## The mechanism (runtime, both copies)

The runtime already had every load-bearing piece; the fix wires the trap sites
to it. The trampoline installs the fiber pad with `setjmp(self->cancel_pad)`
before running the body and gates the longjmp with `cancel_pad_set`. Both
`Cancel` and now traps land at the same pad on the second return.

Added to `KaiFiber`: `int trapped; const char *trap_msg;`. New helper
`kai_trap_abort(const char *msg)`:

- pad installed → set `trapped` + `trap_msg`, longjmp to the pad;
- no pad → `fprintf(stderr, "kai: trap: %s\n", msg); exit(1)` (the pre-existing
  top-level behaviour).

The trampoline's second return maps `trapped` → `KAI_EXIT_TRAPPED`; the link/
nursery propagation delivers `"Trapped: <msg>"` to a trap-exit peer's mailbox.
A trapped child already counted as a nursery failure (it terminates `CANCELLED
&& !cancel_requested`, same as a self-`Cancel`), so the nursery's all-or-nothing
semantics needed no functional change — only a comment correction.

### The pad-vs-no-pad invariant (critical, do not break)

Routing to the pad applies ONLY when a pad is installed (inside a fiber under a
handler). At the top level (no pad, e.g. a simple program with no nursery) a
trap MUST still terminate the process with a clear message and a non-zero exit
— never a longjmp into nowhere (UB), never silent. `kai_trap_abort` keys on
`cancel_pad_set` for exactly this. Both fixtures assert both halves.

## The three trap sites — what routes and what does not

| Trap kind | Backend C | Backend native | Routed by #1067? |
|---|---|---|---|
| index out of range | `kai_array_get_impl`/`set_impl` (shared helper) | same shared helper (via `kaix_prelude_array_get`) | **YES, both backends** |
| divide/mod by zero (boxed) | `kai_op_div`/`mod`/`idiv`/`fixed_div` | same, via `kaix_div`/`mod` shims | **YES, both backends** |
| divide/mod by zero (**unboxed** `Int`) | inline C `a / b`, no guard | inline `sdiv`/`srem`, no guard | **NO — preexisting UB** |
| non-exhaustive match | `kai_prelude_panic` | `KUnreachable` → LLVM `unreachable` | C: **YES**; native: **NO — preexisting UB** |

The native backend links `stage2/runtime.h` through `runtime_llvm.c`'s `kaix_*`
shims, so rerouting the runtime helpers gives the native backend the fix for
free (index OOB, boxed div) — no codegen change, byte-id preserved. Verified
native↔C parity on the contained and top-level index-OOB fixtures.

## Follow-ups (preexisting UB, NOT fixed here — a separate lane, breaks byte-id)

Two of the three trap kinds do not trap today; they emit UB. Fixing either
changes *emitted* code (not the linked runtime), which breaks selfhost byte-id
— out of this lane's scope. Documented here for the integrator to decide
whether to open an issue:

1. **Divide/mod by zero on unboxed `Int`.** `10 / 0` with opaque operands
   returns garbage and continues (exit 0) on BOTH backends — the unboxed path
   emits an inline division with no `divisor == 0` guard and never reaches
   `kai_op_div`. Closing it needs a guard emitted in `emit_c` and `emit_native`
   → changes emitted C → breaks byte-id.
2. **Non-exhaustive match on the native backend.** The native default arm emits
   `KUnreachable` (LLVM `unreachable`) rather than a panic call; a guard-only
   match that misses at runtime produces a silently wrong result (exit 0)
   instead of a trap. Closing it needs `lower_default_arm`
   (`kir_lower_walk.kai`) to emit a panic call instead of `KUnreachable` →
   changes emitted `.ll` → breaks byte-id + `--emit=kir` goldens.

Both are independent of #1067's motivating case (`rongo`'s handlers validate
divisors and keep matches exhaustive; the crash that took the server down was
an index OOB, which this lane contains).

## Fixtures

- `examples/effects/issue_1067_trap_in_fiber_contained.kai` (+ `.out.expected`)
  — a trap-exit supervisor spawns a child that indexes an `Array` out of range;
  the child unwinds to the pad, the supervisor receives `"Trapped: index 10 out
  of range (len=3)"`, the process runs to completion.
- `examples/effects/issue_1067_trap_top_level_exits.kai` (+ `.out.expected`) —
  the same trap with no pad terminates the process with the message on stderr
  and a non-zero exit.

Wired as `test-issue-1067-fiber-trap-isolation` in `stage2/Makefile`, added to
`.PHONY`, `TEST_LIGHT_TARGETS`, and `test-fast`. The target asserts both the
stdout golden and (top-level) the non-zero exit + stderr trap message. Native↔C
parity for the contained/top-level index-OOB path is covered by the serial
backend-parity sweep.

## Structural surprises

- **The index operator does not compile to a dedicated helper.** `xs[i]` is
  rewritten by the typer (issue #128) to a call to the `array_get` builtin for
  everything that is not `Map`/`HashMap`; a `[T]` list and an `Array[T]` both
  back onto `KAI_ARRAY` and both route through `kai_array_get_impl`. The naive
  "touch the bounds check in the tree-walker" would have missed that these
  helpers are shared by compiled user code, not exclusive to stage 0's
  interpreter.
- **`runtime.h` is not a Make prerequisite of the binaries.** It is a link-time
  include; `make kaic2` reports "up to date" after a `runtime.h`-only edit
  because the emitted `stage2.c` is unchanged. The binaries must be force-rebuilt
  (remove them + emitted C) so they relink against the new runtime — otherwise
  the change silently does not take.
- **The div-by-zero and non-exhaustive UB gaps** (above) were not anticipated by
  the issue, which lists all three trap kinds as if they uniformly `exit(1)`
  today. Only index OOB does uniformly; the other two have preexisting UB paths.

## Verification

- Index OOB contained + top-level, both backends, parity: pass.
- Cancel path intact: `issue_103` (trap_exit + Cancel) and `issue_682` (Cancel
  sibling handler) match their goldens on both backends — the trap channel does
  not disturb Cancel.
- ASAN + UBSan over both fixtures: zero real findings.
- selfhost determinism: OK (emitted C unchanged — the fix is in the linked
  runtime, not codegen).
- `test-issue-1067-fiber-trap-isolation`: pass.
