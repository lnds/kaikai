# Lane experience — Process.wait Exited(Int) slot-convention mismatch

## Scope as planned vs. as shipped

**Planned (from the 2026-06-03 bugfix handoff):** the CI red
`effects/process_basic` + `stdlib/os_process_basic` reported
`wait FAIL: nonzero exit`. The handoff flagged it as "likely
environment-dependent (what binary it spawns) or a Process-runtime bug.
Unknown; diagnose before committing."

**Shipped:** a runtime bug, not environment-dependent. The Process
default handler's `Exited(Int)` / `Signaled(Int)` constructors used a
stale variant-slot convention that #741 had migrated away from. Fixed
the two constructors in `stage2/runtime.h` (and `stage0/runtime.h` for
parity), plus refreshed an obsolete comment in `stage0/runtime_llvm.c`.

## Diagnosis — not environment noise

- `/bin/echo hello` runs (prints "hello") and exits 0, yet the handler
  reported nonzero exit, deterministically across 3 runs.
- A `runtime.h` shadowing gotcha cost a detour: the fixture is compiled
  with `-I stage2 -I stage0`, so `#include "runtime.h"` resolves to
  **`stage2/runtime.h`**, not `stage0/runtime.h`. Instrumenting stage0's
  copy produced no output. Re-instrumented stage2's copy.
- The instrumentation proved the decode was correct:
  `raw_status=0x0 WIFEXITED=1 WEXITSTATUS=0` → the runtime built
  `Exited(0)`. So the runtime's POSIX handling was fine.
- Replacing the literal `Exited(0)` arm with a binding `Exited(c)` and
  reading `c` **segfaulted** (exit 139). That localized the bug to slot
  *reading*, not waitpid.

## Root cause

The emitted match for `Exited` reads slot 0 as a **pointer**:

```c
KaiValue *kai_c = kai_incref(_scr->as.var.var_slots[0].ptr);
```

But the runtime constructed the slot as a **raw scalar**:

```c
KaiVarSlot s; s.i64 = (int64_t) code;
return kai_variant_u(9, "Exited", 1, KAI_VAR_SLOT_INT, &s);
```

Issue #741 (the tagged-Int / reuse-token rb-tree perf lane) moved Int
variant slots to a **boxed** representation: `variant_slot_kind` now
reports `Int` as a pointer kind (returns 0), so both construction and
match in the emitter use `{.ptr = kai_int(n)}` with `slot_mask = 0`. A
user `type Box = Wrap(Int)` confirms the live convention — it builds
`kai_variant_u(11,"Wrap",1,0,{.ptr=kai_int(42)})` and matches via
`.ptr`, and works.

The Process runtime constructors were never updated by #741 — they kept
the #440 Phase 2 raw-scalar convention. The emitter reads `.ptr`,
reinterprets the raw `0` as a NULL pointer, and segfaults on the first
`Exited(0)` — i.e. **every** successful child. (A nonzero exit code read
as a pointer is garbage that happens not to crash, which is why the
literal-pattern path mis-reported "nonzero" instead of crashing.)

## Fix

Mirror the emitter in both runtime constructors:

```c
KaiVarSlot s; s.ptr = kai_int((int64_t) code);
return kai_variant_u(9, "Exited", 1, 0, &s);
```

Boxed slot, `slot_mask = 0` — identical to how the emitter builds any
`Foo(Int)`. The generic RC walkers read `kai_slot_mask_of(tag)`; with
mask 0 they treat the slot as a pointer and `kai_decref` the boxed
`kai_int`, which is correct (matches every user Int-carrying variant).

Applied to:
- `stage2/runtime.h` — the path the failing fixture compiles against.
- `stage0/runtime.h` — same latent bug; stage 1's emitter binds every
  variant slot via `.ptr` (it has no typed-slot path at all), so the
  boxed convention is the only one stage0/1 ever read. Fixed for parity
  so a Process program built through the bootstrap chain works too.
- `stage0/runtime_llvm.c` — no code change needed (it already routes
  runtime-minted typed slots through `kai_variant_slot_box` defensively,
  and with mask 0 now takes the plain `.ptr` path). Refreshed the stale
  comment that cited `Exited(Int)` as a *typed-slot* example — it is now
  boxed like everything else; the mask-honoring branch stays as defense
  for any future typed slot.

## Design decisions / alternatives considered

- **Fix the runtime, not the emitter.** The emitter's `.ptr` read is
  the post-#741 convention and is correct for every other Int-carrying
  variant; the runtime was the lone holdout. Changing the emitter back
  to a typed read would re-break the #741 perf win and every user type.
- **Why not register tag 9/10 with an Int mask instead?** That would
  re-introduce a typed slot for `Exited` only — a one-off divergence the
  generic walkers and both backends would each need to special-case. The
  boxed-slot fix makes `Exited` identical to `Wrap(Int)`; no special
  case anywhere.
- **stage0 parity.** Considered leaving stage0 untouched (it does not
  run the failing fixture). Fixed it anyway: stage1's emitter only reads
  `.ptr`, so stage0's raw-scalar constructor is the same latent
  segfault, and the fix is byte-identical and zero-risk under the
  boxed-only convention.

## Fixtures

No new fixtures — `effects/process_basic` and `stdlib/os_process_basic`
already encode the bug shape (spawn `/bin/echo`, wait, assert
`Exited(0)`). Both now pass: `effect-runtime OK`,
`stdlib OK os_process_basic`. The `effects/process_basic` fixture
specifically reads the exit code through a literal `Exited(0)` arm; a
binding read (`Exited(c)`) is the segfault path and is exercised by the
runtime test harness via the same handler.

## Coverage gaps

`Signaled(Int)` got the identical fix but no fixture spawns-then-kills a
child to assert `Signaled(n)`. The mechanism is the same slot
convention, so the `Exited` fixtures cover the representation; a
kill-path fixture would be additive. Low priority.

## Follow-ups

Scanned for other runtime-side `kai_variant_u(..., KAI_VAR_SLOT_INT,
...)` constructors that #741 might have missed — **none survive**. After
this fix, the only `KAI_VAR_SLOT_INT` / `KAI_VAR_SLOT_REAL` references in
`stage2/runtime.h` are the macro definitions and the *reader* side
(`kai_variant_slot_box` + the generic drop/copy walker), which decode
typed slots defensively. Process was the lone stale constructor. No
follow-up needed.
