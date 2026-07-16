# Lane experience — issue #1238 (C backend under CC=clang is unsound with KAI_THREADS>1)

**Outcome: closed the C-backend residual of #1234 by giving the C path the same
owner-object seam the native path already had — but reached it via a different
mechanism than native, because the C `.c` must stay self-contained.** The bug is
identical to #1234 (clang -O1+ caches the thread pointer across `swapcontext`, so
a work-stolen fiber reads the creator thread's `_Thread_local` scheduler state),
and the fix shape is identical (compile the scheduler in a separate `-O0` owner
object). What differed — and what this retro is for — is *how the program TU
stops defining the scheduler* on the C path, where there is no bitcode/DCE seam
handed to us for free.

## Scope as planned vs as shipped

- **Planned (brief):** route the C-backend runtime owner through the existing
  `-O0` owner machinery (#748 separate-compilation + #1234 `KAI_RUNTIME_OWNER_OPT`),
  reusing #1234's precedent. Do NOT redesign the scheduler off `_Thread_local`
  (option 2); stop and report if the split is not viable without breaking
  something else.
- **Shipped:** the split, plus the discovery that the split's *hard part* on the
  C path is not "add an owner object" (mechanical) but "make the program TU not
  DEFINE the scheduler." Native gets that for free (the program is bitcode that
  references `kaix_*` shims; DCE drops the scheduler). The C program `#include`s
  `runtime.h` and calls the runtime's `static` functions BY NAME, inline at -O2 —
  so an owner object alone does nothing; the scheduler still compiles into the
  program. The lane's real work was the seam: which functions to move to the
  owner, and how, without breaking the self-contained `.c`.

## Why the C path differs from native (the load-bearing distinction)

Native #1234 is sound because it has **two** things the C path lacks:

1. **Shim indirection.** The native program calls `kaix_*` mirrors (external
   linkage). The `kai_*` scheduler bodies are `static` in `runtime.h`; nothing in
   the bitcode references them, so DCE deletes them and `llvm-nm` confirms no
   `swapcontext` in the hot bitcode.
2. **A separate object already in the link.** The owner object is where `kaix_*`
   (and the scheduler) live, compiled by cc at `-O0`.

The default C backend is a **single TU**: the emitted `.c` does
`#include "runtime.h"` and calls `kai_core_mailbox_recv`, `kai_sched_bootstrap`,
and the effect default handlers (`kai_default_spawn_await`, `kai_default_nettcp_*`,
`kai_default_stdin_*`, …) directly — 0 `kaix_*` shims in the emitted C. Those
`static` functions compile inline at -O2 → the hoist → the bug. Same root cause,
no free seam.

## The mechanism explored, shipped, and rejected

Three options were on the table; the middle one shipped a working fix that was
then **reverted** because it broke a contract, which is the main lesson.

- **Option 2 (emit → `kaix_*`), IMPLEMENTED AND REVERTED.** Make the C emitter
  call the `kaix_*` mirrors (the ABI native already uses via `kir_kaix_symbol`),
  compile the program under `KAI_HOT_ONLY`, link the -O0 owner. It **worked** —
  100/100 under clang, single-TU and modular, byte-id held by the selfhost fixed
  point. But it broke the **self-contained `.c` contract**: routing the effect
  handlers through `kaix_*` means every program with effects (even a trivial
  `Stdout.print` — 34 `kaix_` references) stops linking as a single TU. That
  breaks **215 recipes** in `stage2/Makefile` that compile `build/$f.c` with a
  bare `cc` (no owner object), the Makefile `selfhost` (kaic2b.c), and any user
  who compiles the emitted `.c` by hand. This is exactly the "breaks something
  else" the brief said to stop and report on. Reported; owner chose Option A.
- **Option A (gate in `runtime.h`), SHIPPED.** Add one linkage macro
  `KAI_SCHED_FN` (twin of the existing global-linkage pattern): `static` in the
  single-TU build (self-contained, sound under gcc, byte-identical), `extern` in
  a non-owner separate-compilation TU, defined in the owner. Apply it — with a
  `#if KAI_SCHED_DECL_ONLY` body gate — to exactly the suspend-point ops the
  emitted C names AND that reach `swapcontext`. The emitter does NOT change: the
  `.c` still calls `kai_*`, still self-contained. `bin/kai` alone does the
  two-object split on the real build path (separate-compilation → owner -O0).
- **Option 2-source (drop `_Thread_local`), NOT PURSUED.** Out of scope per the
  brief; the split makes the existing design sound without touching scheduler
  logic.

## The gate set — 23 functions, and why DCE closes the rest

The set gated with `KAI_SCHED_FN` is exactly: `kai_sched_bootstrap`, the mailbox
recv/send surface + `kai_core_spawn_actor_fiber`, the 8 `kai_default_spawn_*`,
and the parking effect handlers (`kai_default_nettcp_*` ×6, `kai_default_stdin_*`
×2, `clock_sleep_ns`, `process_wait`, `signal_await`).

The **interior** of the suspend closure — `kai_sched_park`, the reactor, the
trampoline, `kai_worker_loop`, the nursery helpers — is NOT gated. Once the
program TU references those entry points as `extern` (owner-resolved), nothing in
the program TU references the interior, and -O2 DCE deletes it. This is the same
whitelist-by-DCE the native bitcode uses; here the "whitelist" is the frontier
of `extern` entry points, and the interior falls out for free. The invariant is
mechanical: `nm` on the program object asserts no `swapcontext` — a parking op
that slips its gate is caught at build time, not shipped.

The critical realization: the frontier is NOT "the mailbox" (my first, too-narrow
cut). Every program installs ALL default effect handlers via emitted shims, and
the ones that park (NetTcp, Stdin, Clock, Process, Signal, Spawn) reach
`swapcontext` — so IO is NOT swapcontext-free, and gating only spawn+mailbox
would leave a `NetTcp.accept` program unsound under clang. The `nm` gate is what
made finding the exact set safe: gate a candidate set, build, read `nm`, widen
until clean.

## Structural surprises the brief did not anticipate

- **The emitted `.c` calls the runtime BY NAME, not via shims** — the whole
  reason native's fix does not transplant. This is the pivot of the lane.
- **The owner-object cache key hashed the WRONG `runtime.h`.** #1234's
  `resolve_native_modular_runtime_obj` folded `$RUNTIME_INC/runtime.h`
  (stage0/runtime.h) into its cache key, but the owner is compiled with
  `-I $RUNTIME_INC_C` (stage2) leading, so `<runtime.h>` binds to
  stage2/runtime.h. A stage2/runtime.h edit did NOT invalidate the cached owner
  → the newly-gated handlers stayed undefined → link error against a stale owner.
  Cost ~30 min of "why is the owner missing symbols." Fixed the key to hash the
  stage2 header (the one actually compiled). This is a latent #1234 bug the C
  path surfaced.
- **The modular C path (#748) had the same latent bug** and needed the owner too.
  It compiled `__root__.c` as `KAI_RUNTIME_OWNER` (globals owner) with the
  scheduler inline at -O2. Fix: `runtime_llvm.c` becomes the *sole* owner of the
  modular link (globals + scheduler at -O0), `__root__.c` stops being the owner —
  mirroring native-modular. A first attempt kept `__root__.c` as globals-owner
  and added runtime_llvm.c without `KAI_RUNTIME_OWNER`, which left runtime_llvm.c's
  OWN globals (`_kaix_v2h_heap`) orphaned. One consistent owner is the fix.
- **`main` clash.** The C program emits its own `int main`; runtime_llvm.c's
  `main` (gated only by `KAI_HOT_ONLY`) collided. Added `KAI_PROGRAM_PROVIDES_MAIN`
  so the C owner suppresses its `main` — a third orthogonal axis (who provides the
  entry point), distinct from linkage (`KAI_SEPARATE_COMPILATION`) and bitcode
  shim elision (`KAI_HOT_ONLY`).

## Validation (macOS arm64, Homebrew clang 18)

The bug reproduces on mac at a LOWER rate than prodigy's x86_64 (~33/40): mac
arm64 needed KAI_THREADS=8 to expose it (T4 was clean). Mechanism confirmed
identical — same `.c` at clang -O0 is 100/100, at clang -O2 fails.

| config | before fix | after fix |
|--------|-----------|-----------|
| single-TU, CC=clang, T4 | 40/40 (mac under-exposes) | 40/40 |
| single-TU, CC=clang, T8 | 98/100 | 100/100 |
| modular, CC=clang, T8 | 96/100 | 100/100 |
| single-TU, CC=gcc-15, T4 | 40/40 | 40/40 |
| native (#1234 path), T4 | 40/40 | 40/40 (untouched) |

- selfhost byte-id: OK (kaic2b.c == kaic2c.c).
- test-effects: OK (the harness that Option 2 broke — proof the `.c` stays
  self-contained).
- rb-tree hot-path perf under clang: unchanged (~0.40s warm, within noise) — the
  leaf RC/arith path stays inlined at -O2; only the cold scheduler moved to -O0.

## Fixtures added

- `tools/run-mn-c-clang-soundness.sh` + `stage2/Makefile` `test-mn-c-clang-soundness`,
  wired into `tier1-native.yml`. Loops the cross-thread stress fixture on the C
  backend (single-TU AND modular) under `CC=clang` at KAI_THREADS=4/8 — the
  config the native gate (#1234, `--backend=native` only) never covers.
  Self-skips on a clang-less host.

## The two CI regressions the split caused (and their fixes)

The functional gates were green locally, but the first CI run surfaced two
failures the mac gates never exercised. Both were the split's doing (confirmed:
main is green on both); neither was the copy-on-send or the #1234 hoist.

- **`rc-detector` (strict build errored).** The split used `stage0/runtime_llvm.c`
  — the NATIVE owner — as the C owner. That file carries native-only baggage:
  the kaix_* ABI, an `int main`, and `_Static_assert(offsetof(KaiValue, as) == 8)`
  (the layout `emit_native_slot.kai` assumes). Under `-DKAI_TRACE_RC=1` the struct
  grows an `alloc_site` field → offset 16 → the assert fails. The C path never
  compiled runtime_llvm.c before, so it never hit the assert. **Fix:** a minimal
  C owner, `stage2/runtime_owner_c.c` — just `#include <runtime.h>` under
  KAI_RUNTIME_OWNER, no native baggage. The scheduler already lives in runtime.h;
  the owner only instantiates it. Each backend now owns via the file aligned to
  it (native → runtime_llvm.c, C → runtime_owner_c.c), both deriving from
  runtime.h. No `int main`, no kaix_ ABI, no native layout pin.
- **`tier1-tsan` (data race on the scheduler TLS).** Fixed by pinning
  `tls_model("initial-exec")` on the scheduler `_Thread_local`s under separate
  compilation. This one is the reusable lesson of the lane — see the section
  below, which the first cut of this retro got wrong by calling it a plain
  "false positive." The scope experiment (§below) shows it is PREEXISTING, and
  the mechanism is a TLS-address hoist that is the exact sibling of #1234.

## The data race that almost broke the merge — the reusable lesson

The first CI run failed `tier1-tsan` with a data race on `kai_active_fiber` (and
a second on `kai_decref` cross-thread, via `kai_mailbox_pop`). The temptation was
to dismiss it as a false positive (`kai_active_fiber` is `_Thread_local`, so
"physically impossible to write cross-thread"). That framing was too glib. The
scope experiment and the mechanism tell a sharper, reusable story.

**(1) The race is PREEXISTING in main — the split did not introduce it, it made
it DETERMINISTIC.** The scope experiment: build the SAME fixture on the C backend
under `clang -O1 -fsanitize=thread` on clean `main` (no split) and loop it.
Result: main is TSAN-clean *almost* always — but flakes to the same race roughly
1-in-many on macOS (the `run-mn-tsan` gate caught it once on main, then 60/60
clean on a re-loop). So the race lives in main's scheduler TLS access already; it
is just so rare under main's TLS model that CI never caught it. The split changed
the TLS *linkage model* (below), which turned a 1-in-many flake into a
deterministic Linux-CI failure. Verdict: **not a bug the split introduced — a
latent one it stopped hiding.** The scope experiment is what separated "I broke
it" from "I exposed it," and it is the step to run first on any CI regression.

**(2) Why `initial-exec` closes it at the root — it is the #1234 hoist, one layer
up.** #1234 was: clang cached the thread-pointer BASE (`%fs`) across
`swapcontext`, so a work-stolen fiber read the creator thread's TLS. That lives in
the codegen of the function BODY, and the -O0 owner closes it. The TSAN race is
the SAME hoist, but in the TLS LINKAGE model, not the body. Under separate
compilation the scheduler TLS becomes `extern`, which defaults (under -fPIC, which
`-fsanitize=thread` forces) to the general-dynamic model: the address is resolved
by a `__tls_get_addr` call, and clang may cache/CSE that resolved address across
calls — including across `swapcontext`. A resolved TLS address cached across a
context switch that resumes on another OS thread points at the WRONG thread's
slot — exactly #1234's failure mode, reached through the address-resolution call
instead of through the `%fs` base. `initial-exec` forbids the general-dynamic
resolution entirely: the address is a direct `%fs`-relative offset
(`@GOTTPOFF` + `movq %fs:(%rax)` instead of `@TLSGD` + `callq __tls_get_addr@PLT`,
verified with `clang --target=x86_64-linux-gnu -fPIC -S`), which is re-evaluated
off the live thread pointer on every access — nothing to hoist across the switch.
So `initial-exec` is not "suppress a TSAN warning"; it removes the cacheable
indirection that could carry a stale cross-thread address, closing the race by
construction, the same way the -O0 owner closes the body-level hoist.

**(3) The pin ALSO hardens the native path of #1234.** The pin lives in the shared
`KAI_TLS` macro under `KAI_SEPARATE_COMPILATION`, and the native owner object has
used separate compilation since #1234 (bin/kai's `nm_owner_defs` / `wp_owner_defs`
pass it). So native inherits `initial-exec` for free. This matters: #1234's owner
`-O0` closed the body-level hoist, but the native path was NEVER exercised under
TSAN (`run-mn-tsan` only builds `--backend=c`), so the extern-TLS general-dynamic
resolution went unguarded there too. The C-under-clang exposure of #1238 is what
surfaced it, and the fix retroactively blinds native against the same latent
cross-thread TLS-address hoist. #1234 fixed the base; #1238 fixes the address
resolution; together they close both halves of the same class of bug.

**Trap for the next lane — TSAN mac vs Linux.** The race is flaky on macOS arm64
(Mach-O TLS differs from ELF; ~1-in-many), so mac cannot confirm an ELF-TLS-model
fix by running — verify it by *codegen inspection* (`clang --target=x86_64-linux-gnu
-fPIC -S`, assert `__tls_get_addr` is gone), let Linux CI be the runtime judge.
Also: the mac rc-detector throws a PREEXISTING UBSAN "insufficient space for
KaiValue" on sized-variant fixtures (`variant_spine_free_1083` etc.) that main
throws too — NOT the split's doing and NOT what CI (Linux gcc) fails on. CI failed
on the strict *build* (the offsetof assert), which the minimal owner fixes; the
mac UBSAN is environment noise.

## Follow-ups left for the next lane

- The `nm`-based soundness gate lives in `bin/kai`, not in a build assert like
  `gen-runtime-bc.sh`'s `llvm-nm` check. It fires on every real build, which is
  good, but a dedicated CI assert (like `assert-runtime-bc.sh`) would document
  the invariant more loudly. Deferred — the per-build gate is sufficient.
- The owner-cache-key fix (stage2 vs stage0 runtime.h) is general hygiene that
  also protects the native paths; no separate lane needed, but worth noting the
  key had been silently wrong since #1234.
