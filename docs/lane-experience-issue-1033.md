# Lane experience — issue #1033: single runtime owner for modular TUs

## Scope as planned vs shipped

**Planned:** under `--emit=c-modular` each translation unit gets a private copy
of the runtime's `static` state. The RC pools and tag-keyed tables were already
shared behind the `KAI_SEPARATE_COMPILATION` / `KAI_RUNTIME_OWNER` guard
(closed the earlier pool-overflow issue); this lane extends sharing to the rest
of the mutable runtime state that a multi-module *user* program touches:
fiber scheduler, region-arena stack, heap accounting, and the dedup/identity
caches. Add cross-module fixtures for a fiber, an arena, and pointer identity.

**Shipped:** exactly that, plus one thing the plan did not anticipate — the
Makefile modular-build harness was compiling *without* the guard defines, so it
never exercised the owner model at all (see "Structural surprise" below). Fixed
that too, since otherwise the fixtures would have passed vacuously.

## Design decision: guard-extend, not a physical runtime.o

The plan floated "converge C-modular to a real runtime.o (as native already
links `runtime_llvm.c`), stop extending `#ifdef` guards." Measured against the
code, the two are the same thing in the C path: the owner TU **is** the
runtime.o. It is the single TU that defines the state; every other generated TU
references it `extern`. There is no separate `runtime_llvm.c` to link on the C
side — the owner TU already fills that role. A physically separate `.o` would be
a rename, not a fix.

The only soundness risk a separate `.o` avoids is the cross-TU static-init-order
fiasco: a symbol whose initializer depends on another TU's already-initialized
state. That cannot happen here — all state lives in one owner TU. `kai_active_fiber
= &kai_main_fiber` and `kai_unit() = &kai_singleton_unit` take the address of
symbols in the *same* TU; link-time-known addresses, no dynamic order. So the
decision was guard-extend, with an expert (asu) sign-off on the reasoning.

Hot-path RC (`incref`/`decref`/alloc bump) stays `static inline` and is a
non-issue: those functions carry no state of their own; the pools they touch are
already `extern` behind the guard. Duplicating a stateless inline function per
TU is code, not state — the ODR-for-data rule does not apply. The one trap to
check was a `static` *local* inside an inline body (a per-function counter/cache
that would diverge per TU); the reactor's lazy-init flag was exactly that, and it
was promoted to a shared global.

## What was shared, and the inclusion criterion

The criterion (asu): *does more than one module observe or mutate this through an
effect op?* → share it; *is it a process-unique resource born and freed inside a
single call, never compared by identity across modules?* → leave it owner-static.

- **Fiber scheduler** (`kai_main_fiber`, `kai_active_fiber`, ready queue,
  `kai_parked_count`, `kai_pending_free`) — scheduler globals, cross-TU by
  nature.
- **Region-arena stack** (`kai_arena_stack`, `kai_arena_sp`) — a nested region
  across a module boundary pushes/pops one shared stack.
- **Heap accounting** (`kai_heap_committed`, `kai_heap_inited`,
  `kai_heap_limit_cached`) — `KAI_MAX_HEAP` caps the process, not each TU.
- **Dedup / identity** (`kai_singleton_unit/true/false/nil`, `kai_char_cache`,
  `kai_str_intern_table`) — the hard criterion here is *return-by-address*:
  `kai_unit()` hands out `&kai_singleton_unit`, so a per-TU copy breaks any
  pointer-identity fast path. The intern table's whole purpose is identity.
- **Reactor** (self-pipes, timer wheel, waiter lists, parked count, file-pool
  queue/threads/lock, init flag) — shared as a unit: `kai_reactor_wait` is one
  function in one TU that polls the pipes and drains the lists that another TU's
  effect op populated. Splitting "lists yes, pipes no" would let one TU enqueue
  a waiter the polling TU never sees.
- **Cross-cutting effect state** (`kai_next_handler_id`, `kai_g_argc/argv`,
  the PCG RNG, the subscribed-signal set, the SIGSEGV install flag) — each is
  written in one module and read in another; a per-TU copy splits it.

Left owner-static (reachability filter said no cross-TU extern edge): `test` /
`bench` / `check` harness counters — the emitter lands every `test {}` / `bench {}`
block in the root TU (`__root__.c`), which is the owner, so those statics are
already single-copy by construction.

## Structural surprise the brief did not anticipate

`test-modular-build` in `stage2/Makefile` compiled each TU with plain
`$(CC) $(CFLAGS) -c` — **no `-DKAI_SEPARATE_COMPILATION`**. So every TU fell into
the `#else static` branch and materialised its own copy of every symbol. The
programs still ran (structural `==`, and small programs reference few globals),
so the harness was green while testing the *wrong* model — the per-TU world this
lane exists to eliminate. `nm` on the fiber binary showed 3 addresses for
`kai_singleton_unit` and 9 for `kai_str_intern_table` before the fix. The real
driver (`bin/kai` with `KAI_MODULAR=1`) always passed the guard; only the CI
harness diverged. Fixed the harness to pass the same defines (`__root__.c` as
owner), matching the driver, and now `nm` shows one address per shared symbol.

Lesson: a modular harness that does not pass the same compile flags as the
driver proves nothing about the driver's model.

## Fixtures

Three, all under `examples/multi-module/`, wired into `MODULAR_FIXTURES` +
`test-modular-identity`:

- **`issue-1033-cross-module-fiber`** — nested nurseries across a module
  boundary. The outer fiber is spawned in the root TU; its body calls
  `producer.nested_work`, which opens *its own* nursery and runs spawn/await in
  the producer TU. That puts a scheduling op executing in the producer TU while
  the caller's fiber is parked on the same scheduler. A simpler "worker-in-A,
  spawn+await-in-B" shape would have been a false-positive (spawn+await both in
  one TU never cross the scheduler); asu flagged this and the fixture uses the
  nested form. The `Fiber[T]` escape check forbids returning a handle across a
  function boundary, which is why nested nurseries — not a returned fiber — are
  the vehicle.
- **`issue-1033-cross-module-arena`** — nested regions across a module boundary;
  `sp` must nest 0→1→2→1→0 on one shared stack.
- **`test-modular-identity`** — a harness `nm` check, not a kaikai program.
  Pointer identity is not observable from pure kaikai (`==` is structural, there
  is no `===`), and a *successful link does not prove uniqueness* — an
  internal-linkage `static` symbol links green while materialising one copy per
  TU (exactly the bug). So the only reliable gate is `nm` on the final
  executable: exactly one address per shared symbol. Two addresses = regressed to
  per-TU state. Same reasoning C++ ODR-violation tests use.

## Native untouched

The native backend links `runtime_llvm.c` / `.bc` as a real object already; it
does not include this `runtime.h` under the guard and does not use these defines.
The changes are confined to the C-modular path. `make kaic2` (C) rebuilds clean;
the native path is unchanged.

## Follow-ups left for next lanes

- L2 (`home_tu` shared + `KFn.mo`) and L3 (#989 native modular) are the rest of
  the modular chain; this lane is scoped to the runtime owner only.
- The `test-modular-build` guard-flag fix is a harness correctness fix that
  benefits every modular fixture, not just #1033's — worth remembering that the
  harness now actually tests the owner model.
