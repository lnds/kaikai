# Lane retro — issue #587: LLVM backend installs Link + Monitor default handlers

## Scope as planned vs scope as shipped

**Planned (brief).** Close issue #587 — the fourth instance of the same
structural bug as #571 / #570 / #582. A fiber spawned via
`fiber_spawn(() => with_mailbox { ... })` that calls `Link.link(_)` or
`Monitor.monitor(_)` segfaults under the LLVM backend; the C backend
works. The reporter (ahu) had already produced an 18-line standalone
repro and a full ablation table pinning the bug to Link/Monitor
specifically (Actor.send from the same shape ran cleanly), and the
hypothesis was that `kai_main_install_defaults` was missing the Link
and Monitor entries in the LLVM path — the same gap that #570 closed
for Spawn and #582 closed for Cancel.

**Shipped.** The diagnosis was correct on first read, and the fix was
mechanical: add the same five-site pattern that #582 used (header
declares, globals, install branch, teardown pop, runtime wrappers) to
the LLVM emit path for two more effects. Link has one op (`link`);
Monitor has two (`monitor`, `demonitor`). Both sit between Cancel and
Spawn in `builtin_default_install_order` (slots 16 and 17 of 18), so
the install order is `... Cancel, Link, Monitor, Spawn` and the
teardown is the reverse. Net: ~70 lines in `stage2/compiler.kai` plus
~10 lines in `stage0/runtime_llvm.c`, plus two regression fixtures
(~30 lines each).

No surprises, no scope creep. The brief's "stop if scope crows >300
LOC" guard was never tested.

## Reproduction confirmation

The 18-line repro from the issue body (`/tmp/repro_587.kai`), built
via `bin/kai build --backend=<c|llvm>`:

- **C backend, pre- and post-fix:**
  ```
  body: ran (linked to self)
  exit 0
  ```
- **LLVM backend, pre-fix:**
  ```
  (no output)
  exit 139 (SIGSEGV)
  ```
- **LLVM backend, post-fix:** byte-identical to the C backend.

The Monitor variant (`/tmp/repro_587_monitor.kai`, identical shape but
calling `Monitor.monitor(me)` instead of `Link.link(me)`) follows the
same pattern: SIGSEGV pre-fix, `body: ran (monitored self)` exit 0
post-fix.

## Diff between C backend and LLVM backend in `kai_main_install_defaults`

Same diff as #570 and #582. The C backend's `default_setups_for`
(stage2/compiler.kai:18391) iterates `builtin_default_install_order()`
— `[Stdout, Stderr, Fail, Stdin, Env, File, Clock, Random,
SecureRandom, NetTcp, Signal, Process, Log, Mutable, Cancel, Link,
Monitor, Spawn]` — and emits an AST-derived install body for every
effect in main's row.

The LLVM backend's `llvm_emit_main_install_defaults`
(stage2/compiler.kai:46041) is hand-written, one branch per effect.
Pre-this-fix it covered Stdout, Stderr, Fail, Random, SecureRandom,
Clock, NetTcp, Log, Mutable, Cancel, and Spawn (11 of 18). Link and
Monitor were silently dropped. Five effects remain unhandled after
this lane: Stdin, Env, File, Signal, Process. None are user-visible
in the current issue tracker.

## Fix shape

Five sites, all mechanical, identical to #582:

1. **`stage2/compiler.kai` :: `llvm_header`** (line ~43842) — added
   three `declare` lines for `@kaix_default_link_link`,
   `@kaix_default_monitor_monitor`, `@kaix_default_monitor_demonitor`.
2. **`stage2/compiler.kai` :: `llvm_default_builtin_decls`** (line
   ~46037) — added two `if list_has(main_row, "Link")` and
   `if list_has(main_row, "Monitor")` globals blocks emitting
   `%EvLink` / `%EvMonitor` zero-initialised globals plus the
   node/name constants.
3. **`stage2/compiler.kai` :: `llvm_emit_main_install_defaults`**
   (line ~46229) — added `link_` and `monitor_` install blocks
   (1 op at slot 3 for Link; 2 ops at slots 3 and 4 for Monitor),
   wired into the install order between `cancel_` and `spawn_`,
   matching `builtin_default_install_order`'s positions 16 and 17.
4. **`stage2/compiler.kai` :: `llvm_emit_main_teardown_defaults`**
   (line ~46352) — added `Monitor` and `Link` pops after the Spawn
   pop and before the Cancel pop (LIFO of the install order).
5. **`stage0/runtime_llvm.c`** (line ~582) — added the three
   `kaix_default_link_link`, `kaix_default_monitor_monitor`,
   `kaix_default_monitor_demonitor` one-line forwarders to the static
   `kai_default_*` handlers in `runtime.h`. Pattern identical to every
   other `kaix_default_*` wrapper in the file.

Slot indices verified against `llvm_emit_effect_struct`
(stage2/compiler.kai:16398): the struct is
`%Ev<Name> = type { i64, i8*, %KaiValue*, op_fn_ptrs... }` — three
header fields (`hid`, `env`, `state`) then ops in declaration order.
`builtin_link_ops()` and `builtin_monitor_ops()` pin the op order at
stage2/compiler.kai:56888 and :56912 respectively.

## Verification against ahu fixtures

Cloned `git@github.com:kaikailang-org/ahu` at HEAD and ran the full
`make tier1` suite under LLVM (`KAI_BACKEND=llvm`, the workaround pin
removed):

- **Pre-fix (kaikai 0.58.0):** 10 of 16 fixtures pass; 6 segfault
  (the `restart_*`, `cross_restartable_cell*`, and
  `examples/resilient_counter` fixtures — all use `Link` or
  `Monitor`).
- **Post-fix:** **16 of 16 fixtures pass.** Including
  `restart_temporary_crash`, `restart_transient_normal`,
  `restart_intensity_escalate`, `cross_restartable_cell`,
  `cross_restartable_cell_restart`, and `examples/resilient_counter`.

This is the milestone the brief framed as "the fourth and (esperemos)
último bug del mismo perfil estructural" — with this lane the entire
ahu test suite runs cleanly under the LLVM backend for the first time
since kaikai 0.56.6 was released.

The ahu Makefile's `KAI_BACKEND=c` pin can now be dropped (a separate
PR in the ahu repo).

## Selfhost / tier behaviour

- `make tier0` — green. Selfhost byte-identical (Link and Monitor are
  not in the compiler's own `main` row, so the install branches
  never fire for `compiler.kai`), 28 demos passing (baseline 27).
- `tools/test-llvm-driver.sh` — 16 of 17 fixtures pass parity,
  including the two new ones
  (`examples/llvm/link_in_spawned_fiber.kai` and
  `examples/llvm/monitor_in_spawned_fiber.kai`). The single failure
  (`examples/quickstart/02_fizzbuzz.kai`) is the pre-existing
  effect-row issue documented in the #582 retro — unrelated to this
  lane and unchanged on `main`.
- `make tier1` — green local.

## Real cost vs estimate

Brief estimated 1–3 hours. Actual: ~45 minutes, of which:

- ~5 min confirming repro (`/tmp/repro_587.kai`).
- ~10 min reading the #582 retro to lock in the exact five-site
  pattern and locate the surrounding code.
- ~10 min implementing the additions (header declares, globals,
  install + teardown blocks for two effects, three runtime wrappers).
- ~10 min verifying repro post-fix + writing the two regression
  fixtures + adding them to the LLVM driver.
- ~10 min cloning ahu, running tier1 under LLVM, confirming 16/16.

The estimate was tight; the 1-hour low end was the realistic figure
because the #582 retro had already mechanised the work.

## Fixtures shipped

Two positive fixtures wired into `tools/test-llvm-driver.sh`:

- `examples/llvm/link_in_spawned_fiber.kai` — the verbatim shape of
  the #587 repro (18 lines, no external deps). Pre-fix on LLVM
  reproduces SIGSEGV; post-fix both backends print
  `body: ran (linked to self)` and exit 0.
- `examples/llvm/monitor_in_spawned_fiber.kai` — Monitor variant of
  the same shape. Pre-fix on LLVM reproduces SIGSEGV; post-fix both
  backends print `body: ran (monitored self)` and exit 0.

No negative fixtures: the install branches are unconditional within
their `if list_has(main_row, "Link")` / `"Monitor"` gates, matching
every other builtin's install pattern.

## Cross-references and recommendations

- Issue #587 — closed by this lane.
- Issue #582 — closed by the previous lane (Cancel install). This
  lane's fix is the same shape applied to two more effects.
- Issue #570 — Spawn install. Same family.
- Issue #571 — lambda info table. Structurally unrelated but cited as
  the first of the four-instance pattern.
- `docs/lane-experience-issue-582-llvm-cancel-raise.md` — the retro
  that already mechanised this lane's work and enumerated Link +
  Monitor as the next two effects to port.

### Recommendation on #575 — backend parity CI

**This is the fourth lane in the same family.** Each lane has shipped
a per-fix fixture in `tools/test-llvm-driver.sh`; the driver now
covers 17 fixtures (was 9 at #570's close). But the gap between "C
works, LLVM segfaults" continues to surface from downstream users
(ahu) rather than from CI, because:

- The driver's fixtures are hand-picked; they catch the bugs we already
  know about but don't catalogue the parity of arbitrary programs.
- The full LLVM coverage gap (5 effects still unhandled: Stdin, Env,
  File, Signal, Process) is invisible to any current CI signal.

#575 — full parity sweep in CI, running every fixture in `examples/`
through both backends and diffing stdout/stderr — is now load-bearing.
The previous retro recommended launching #575 "in parallel with the
Link install lane, not after". Link is now landed; #575 is the next
high-leverage lane. Without it the fifth instance of this bug family
(whichever effect surfaces next from a downstream user) will repeat
the same out-of-band reporting cycle.

### Strategic refactor follow-up

The longer-term cleanup is to migrate `llvm_emit_main_install_defaults`
to AST-derive from `default_setups_from_block` the way the C backend
already does. Eliminates the per-effect maintenance and prevents this
category of gap entirely. ~200-LOC lane; not urgent (the per-effect
ports leave only 5 unhandled builtins, none user-visible), but
sensible to do once #575 lands and the per-fix fixtures can shrink to
a regression smoke test.

## Follow-ups

1. **#575 — LLVM parity CI** — now load-bearing per the analysis above.
2. **ahu repo: drop `KAI_BACKEND=c` pin** — separate PR in the ahu
   repo; remove the Makefile's `export KAI_BACKEND ?= c` and the
   `docs/known-regressions.md` §`kaikai#582` block.
3. **5 remaining unhandled builtins**: Stdin, Env, File, Signal,
   Process. None user-visible per the tracker; audit before they
   surface. Each is the same ~20-line shape.
4. **Strategic AST-derived refactor of `llvm_emit_main_install_defaults`**
   per the previous lane's retro. Land after #575.
