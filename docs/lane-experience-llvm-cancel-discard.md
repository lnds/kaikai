# Lane experience — LLVM Cancel continuation-discard (parity Cluster F, refs #622)

**Scope:** fix the LLVM backend so a handler clause that discards `resume`
(does not call it) unwinds out of the `handle` block instead of letting the
body's tail run. Closes the `m8_4_cancel_caught` parity divergence.

> **Note:** this retro was completed by the integrator loop after the lane's
> Claude session was killed mid-verification by a socket error (`API Error:
> The socket connection was closed unexpectedly`). The fix itself was complete
> in the worktree (3 implementation tasks done); the integrator re-ran the
> full gate from the worktree, confirmed it green, and finished commit + PR.

## The bug
`examples/effects/m8_4_cancel_caught.kai`: a `with Cancel { raise(resume) -> { ...; 42 } }`
clause discards `resume`. Correct (C) semantics: `Cancel.raise()` unwinds out
of the wrapping `handle`; the body tail (`print("never reached"); 99`) never
runs; the handle returns the clause's value `42`. Under LLVM the body tail ran
("never reached" printed, handle returned 99). r9 (#715) was a *related but
distinct* continuation bug (capture corruption + double-resume); re-testing
m8_4_cancel after #715 confirmed it did NOT fall out — separate cause.

## Root cause
The C runtime sets up a `cancel_pad` with `setjmp` around the handle body
(runtime.h ~1424); discarding `resume` longjmps back to the pad and the clause
value becomes the handle value. The LLVM `handle` lowering never established
that pad, so the op-dispatch returned normally and the body continued.

## The fix (localized — emit + bounded runtime shims, NOT a runtime rewrite)
Three pieces (the lane's plan checkboxes):
1. **runtime_llvm.c (+58):** a `jmp_buf`-size global + `push_with_jmp` +
   longjmp shims, so the LLVM side can install/land a setjmp pad through the
   same evidence machinery the C runtime uses. Bounded shims, not a rewrite of
   the unwind discipline.
2. **emit_llvm.kai `llvm_emit_handle` (+ part of the 153):** emit the setjmp
   landing pad around the handle body.
3. **emit_llvm.kai `llvm_emit_op_dispatch`:** emit the discard-detection
   longjmp — when a clause returns without resuming, longjmp to the pad.

This was flagged *possibly structural* in the brief (setjmp/longjmp = stack
unwind). It turned out localized enough not to require an asu/linus consult:
the shims reuse the existing evidence/pad model rather than redesigning it.

## Verification (the critical part for a longjmp change)
- `m8_4_cancel_caught`: C == LLVM, returns 42, no "never reached".
- **`make selfhost` byte-identical** (`kaic2b.c == kaic2c.c`). A `make selfhost`
  on the dirty worktree first reported DIFF — that was a STALE-BUILD ARTIFACT
  (incremental objects from the killed session). A clean `make -C stage2 clean
  && make all && make selfhost` was byte-identical. **Lesson: never trust a
  selfhost DIFF on a worktree that had an interrupted build — clean first.**
- `make tier0` OK (demos baseline 34 holds).
- `make tier1-asan` OK — the decisive gate for setjmp/longjmp: a bad pad target
  would be stack corruption, ASAN saw none.
- `make tier1` green.
- Two regression fixtures added: `cancel_discard_minimal.kai`,
  `cancel_discard_nested.kai`. `m8_4_cancel_caught` skip line removed.

## Cost / surprises
The implementation was clean; the cost overrun was the session dying to a
socket error mid-verification, leaving the fix uncommitted. Recovery: the
integrator loop detected the dead session, verified the fix was complete in the
worktree, re-ran the full gate (catching + dismissing the stale-build selfhost
false-alarm), and finished the lane. Argues for the gate being run by whoever
finalizes, not trusted from a half-finished session.
