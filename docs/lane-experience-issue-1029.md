# Lane experience — issue #1029: dev fast rebuild of kaic2 via modular self-compile

## Scope as planned vs. as shipped

Planned: a `kaic2-fast` make target where an existing `kaic2` recompiles the
compiler from `stage2/main.kai` through `bin/kai`'s `KAI_MODULAR=1
--backend=c` path (imports resolved, no kaic1, no bundle, parallel cc +
`.o` content-hash cache), plus an equivalence gate and docs. Estimated as a
build-system-only lane.

Shipped: all of the above — **plus a runtime fix the lane could not avoid**.
The first fast-built staging binary segfaulted on any real compile
(`kai_free_value`, EXC_BAD_ACCESS at 0x4). This was a latent, pre-existing
bug in the c-modular runtime model, not a regression from this lane: the
tier1 gate `test-modular-selfhost` builds the whole compiler modularly but
only smokes `--version`, so the modular-built binary had never actually
*compiled anything* before this lane pointed it at a program.

## The bug: per-TU copies of process-global runtime tables

`runtime.h` is header-only; every symbol is `static`/`static inline`, so
under separate compilation each TU gets private copies of all runtime
state. The `KAI_SEPARATE_COMPILATION` / `KAI_RUNTIME_OWNER` guard (the
shared-pool fix that made the modular link possible) covered exactly three
symbol groups — the multi-MB RC free-list pools — because the visible
symptom then was a link-time `.bss` overflow. Everything else stayed
per-TU, including the tables that are *process-global by design* (their
own comments say so):

- `kai_variant_to_head` / `_len` — variant-tag → head-type map, installed
  once at startup by emitted main **into main's TU copy only**. Every
  other TU kept the 11-entry builtin bootstrap table, so freeing a user
  variant (tag ≥ 11) in another TU indexed out of it.
- `kai_slotmask_table` / `_seen` / `_cap` — tag → slot-kind masks,
  registered at first construction *in the constructing TU*. A reader TU
  saw mask 0 = "all slots are pointers" and dereferenced unboxed Ints:
  the observed crash.
- `kai_impl_table` / `_cap` / `_count` — protocol dispatch table; per-TU
  copy means every dispatch outside main's TU returns "no impl".
- `kai_varname_table` / `_cap` — tag → ctor name for to_string/dispatch.
- `kai_nullary_singletons` + `kai_enum_by_tag` — enum-slot re-boxing
  (`kai_enum_slot_box`) silently returns `unit` on a per-TU miss: a wrong
  *value*, worse than a crash.
- `kai_immortal_vars` — immortal-args cache (also ~15 MB `.bss` per TU
  when replicated).
- `kai_reusable_tag_bits` — reuse-target registry; a per-TU miss
  immortalises reuse-target cells and leaks the structures wholesale.

Fix: extend the existing extern/owner pattern to those seven table groups.
Mechanical, preprocessor-only; the single-TU build (no defines) yields the
original `static` lines verbatim, so the default path is byte-identical —
`selfhost` proves it.

Deliberately left per-TU (inert for the compiler workload, noted for a
follow-up issue): fiber scheduler state (`kai_active_fiber`, ready queue),
arena stack, heap-limit accounting, string-intern/char caches (immortal
dedup caches — per-TU is waste, not incorrectness), RC stats counters,
slab allocator bump state (safe by design: freed blocks go to the shared
block pool, never libc `free`).

## Design decisions

- **Selection logic: explicit and additive, no auto-selection.** `make
  kaic2` stays pure bootstrap; `make kaic2-fast` is opt-in. A stale kaic2
  silently building a wrong kaic2 was the failure mode to avoid; typing
  `-fast` is the developer's acknowledgment that the binary in place is
  trusted. (Settled with the integrator up front.)
- **Staging + sanity gate before swap.** The fast build lands in
  `stage2/build/kaic2-fast.bin`; only after `--version` and a golden demo
  (compiled with NO flags, so the baked `KAI_STDLIB_PATH` is exercised)
  does it `mv` over `stage2/kaic2`. This gate rejected the broken pre-fix
  build on its very first run — it earned its place immediately.
- **Equivalence gate = emitted-C byte-identity, as its own target.**
  `kaic2-fast-verify` builds the staging binary and diffs its emitted C
  against `./kaic2`'s for the compiler itself (single-TU self-compile) and
  a sample program. Byte-identity of the *binaries* is out of reach
  (multi-`.o` parallel link vs one `-O2` TU); identical emitted C from the
  same source is the same functional-parity currency `selfhost` uses.
  Precondition: `kaic2` built from the current source — the target says so
  when the diff fails.
- **Baked stdlib path.** `bin/kai`'s default CFLAGS do not carry
  `-DKAI_STDLIB_PATH`, so a naive `bin/kai build stage2/main.kai` produces
  a kaic2 that cannot find the stdlib when invoked raw (as ~147 stage2
  test targets do). The target passes the same define the bootstrap recipe
  uses, via the `CFLAGS` env override `bin/kai` honors.

## Surprises

- The headline surprise: issue #1029's "evidence it already works" tables
  measured *build* times of the modular self-compile — nobody had ever
  run the resulting binary as a compiler. The gap between "links and
  answers --version" and "works" was the whole lane.
- Cold-cache builds report 2 cache hits: two TUs in the same build have
  identical generated C (same content hash), so the second is a legit
  intra-build hit. Not a bug.
- `bin/kai` builds its `kai-pkg` helper through the same modular path
  first (~20 extra TUs) on first use; subsequent builds reuse it.

## Fixtures / gates

- `kaic2-fast-verify` is the repeatable equivalence gate (full modular
  self-compile + byte-diff of emitted C on two inputs). It subsumes a
  smoke fixture: any per-TU runtime regression that corrupts compilation
  shows up as a crash or a diff in the self-compile.
- `test-modular-selfhost` (tier1) extended in this lane: after the link
  and `--version` smoke, the modular-built binary must now compile a
  program and match `kaic2`'s emitted C byte-for-byte. This is the
  regression fixture for the runtime-sharing fix — the exact check whose
  absence let the bug ship.

## Measured timings (16 cores, C backend, this machine)

| build | wall | note |
|---|---|---|
| bootstrap `make kaic2` (cold, full chain) | 135.6s | kaic0 + kaic1 + bundle + one `cc -O2` TU |
| `make kaic2-fast` warm | 71.4s | 86/86 `.o` cache hits, zero cc |
| `make kaic2-fast` one-module edit | 72.2s | 1 TU recompiled, 85 hits |

The backend is near-free warm/incrementally; the remaining wall is the
single-process frontend emit (~65s), which is the next lever and out of
scope here. A comment-only edit produces byte-identical module C and
recompiles nothing.

## Cost

Estimated: half a day of Makefile + docs work. Real: that, plus root-cause
and fix of the runtime sharing bug (lldb backtrace → `runtime.h` static
audit → 7 mechanical guard extensions → full re-verify). The debugging was
fast because the crash reproduced on a 3-line program and the guard
pattern already existed to copy.

## Follow-ups

- Residual per-TU runtime state under c-modular (fibers, arena stack,
  heap accounting, intern/char caches) — filed as #1033; harmless for
  the compiler workload but must be shared (or asserted absent) before
  c-modular graduates from opt-in.
- Consider running the modular-built binary on a real compile inside
  `test-modular-selfhost` so tier1 catches this class.
- Native fast rebuild blocked on the native subset gap (#1021).
