# Lane retro — issue #582: LLVM backend installs Cancel default handler

## Scope as planned vs scope as shipped

**Planned (brief).** Close issue #582 — `Cancel.raise()` from a fiber
spawned under a mailboxed parent segfaults on the LLVM backend while
the C backend correctly detects deadlock. The brief framed the bug as
the third instance of the same structural pattern as #570 / #571 (LLVM
emitter missing piece around closure/continuation lowering) and the
reporter hypothesised "the LLVM emitter materialises the continuation
pointer that the resume sequence dereferences immediately after the
raise".

**Shipped.** The diagnosis the brief inherited was almost right — the
crash is exactly the post-raise resume dereferencing a null evidence
slot — but the **null is not in the continuation pointer**; it is in
the Cancel evidence node itself, which the LLVM backend's
`kai_main_install_defaults` never pushed. The fix is the same shape as
the #570 fix: add Cancel to the three sites in `llvm_emit_main_*`
plus declare the one runtime wrapper. The previous retro
(`lane-experience-issue-570-llvm-spawn-default.md`) explicitly listed
Cancel as one of "the eight other unhandled effects" pending a
follow-up; this lane is that follow-up restricted to the one effect
that unblocks the user-visible #582 repro.

The lane stayed strictly in scope. Six categories of escalation from
the brief did **not** apply:

- Cause is not "continuation pointer materialization in Cancel.raise
  lowering" (the reporter's hypothesis). The LLVM IR for the raise
  call site is byte-identical in shape to a working effect call; the
  null is upstream, in the evidence stack.
- No shared lowering between Cancel and the other unhandled defaults
  (Link, Monitor, Stdin, Env, File, Signal, Process) was needed.
- No AST shape change, no continuation ABI change.
- No new tier1 fixture broke.
- Selfhost was byte-identical (Cancel is not in the compiler's main
  row).
- The fix is 30 lines plus one runtime wrapper, well under the brief's
  500-LOC ceiling.

## Reproduction confirmation

The 14-line repro from the issue body, verbatim. Built via
`bin/kai build --backend=<c|llvm> /tmp/repro_582.kai`.

- **C backend, pre- and post-fix:**
  ```
  main: parking on receive
  child: about to raise
  kai: fiber finished with empty run queue (1 parked) — deadlock
  exit 1
  ```
- **LLVM backend, pre-fix:**
  ```
  main: parking on receive
  child: about to raise
  [1]    segfault
  exit 139
  ```
- **LLVM backend, post-fix:** byte-identical to the C backend.

The crash instruction (`ldr x1, [x0]` with `x0 = NULL` immediately
before `kaix_cont_init_identity`) is `Cancel`'s op-fn slot in the
zero-initialised global the LLVM main was failing to populate, not a
continuation pointer in the resume sequence.

## Diff between C backend and LLVM backend in `kai_main_install_defaults`

The C backend's `default_setups_for` (stage2/compiler.kai:18391)
iterates `builtin_default_install_order()` and emits an AST-derived
install body for every effect in main's row. The order pins Cancel at
slot 14 of 18 (between Mutable and Link); the C runtime's
`kai_default_cancel_raise` (stage0/runtime.h:6678) is registered as
the op fn.

The LLVM backend's `llvm_emit_main_install_defaults`
(stage2/compiler.kai:46022) is hand-written, one branch per effect,
and the pre-fix version handled only ten (Stdout, Stderr, Fail,
Random, SecureRandom, Clock, NetTcp, Log, Mutable, Spawn). Cancel
(plus Link, Monitor, Stdin, Env, File, Signal, Process) was silently
dropped from the install. The previous lane (#570) closed Spawn the
same way and explicitly enumerated the seven that remained.

This lane closes Cancel only. The seven other unhandled effects are
identical in shape (each adds ~20-30 lines: a globals block, an
install block, a teardown pop, an LLVM header `declare`, and a runtime
wrapper). Filing them as a follow-up after the user-blocker (ahu) is
addressed is the right shape; the C-backend's AST-derived path
remains the natural target for a structural cleanup that eliminates
this category of gap entirely.

## Fix shape

Five sites, all mechanical:

1. **`stage2/compiler.kai` :: `llvm_header`** (around line 43838) —
   add `declare %KaiValue* @kaix_default_cancel_raise(i8*, i8*)`. Signature
   matches `Spawn.yield` (no args beyond `self` and `KaiCont*`).
2. **`stage2/compiler.kai` :: `llvm_default_builtin_decls`** (around
   line 46005) — add the three Cancel globals (`@_kai_default_ev_cancel`,
   `@_kai_default_node_cancel`, `@_kai_default_cancel_name`) gated on
   `list_has(main_row, "Cancel")`. `%EvCancel` itself is already emitted
   by `llvm_effect_struct_decls(decls)` because Cancel is part of the
   builtin effect inject.
3. **`stage2/compiler.kai` :: `llvm_emit_main_install_defaults`** (around
   line 46163) — add the `cancel_` branch storing the single op fn-ptr
   at slot 3 of `%EvCancel` and pushing the evidence node. Wired into
   the install order between `mutable_` and `spawn_`, matching
   `builtin_default_install_order`'s position 14.
4. **`stage2/compiler.kai` :: `llvm_emit_main_teardown_defaults`** (around
   line 46228) — add the matching `kaix_evidence_pop()` after the Spawn
   pop and before the Mutable pop (LIFO from the install order).
5. **`stage0/runtime_llvm.c`** (around line 572) — add the
   `kaix_default_cancel_raise` wrapper as a one-line forwarder to
   `kai_default_cancel_raise` in `stage0/runtime.h`. Pattern identical
   to every other `kaix_default_*` already in `runtime_llvm.c`.

Slot index verified against the emitted struct definition: `%EvCancel
= type { i64, i8*, %KaiValue*, %KaiValue* (%EvCancel*, %KaiCont*)* }` —
three header fields (`hid` / `env` / `state`) then `raise` at index 3.
Cancel's op list is a single entry per `builtin_cancel_ops()`
(stage2/compiler.kai:56877).

Net change: ~30 lines in stage2/compiler.kai plus 3 lines in
stage0/runtime_llvm.c, plus the 35-line regression fixture and a
single line in the LLVM driver.

## Why the reporter's "continuation pointer materialisation"
## hypothesis was off by one layer

The backtrace points at `_kai_lam_NN + 144`, with `ldr x1, [x0]` and
`x0 = NULL` immediately before `kaix_cont_init_identity`. That
description fits *both* "the continuation pointer was null" and "the
evidence node's op fn slot was null", because the LLVM lowering
materialises the continuation **inside the op invocation** and the op
fn is loaded from the evidence struct one instruction earlier. The
`x0 = NULL` is the evidence pointer that the loaded op fn would have
been called with; `kaix_cont_init_identity` is the next instruction
that runs once the op fn returns, and would have constructed the
continuation. The reporter saw "next call after the failing load is
kaix_cont_init_identity" and reasoned backwards through the resume
sequence; the actual culprit is one frame earlier in the dispatch.

This is a useful future heuristic: when a backtrace shows
`kaix_cont_init_identity` *after* a null deref, suspect the evidence
op slot before suspecting the continuation construction. The op
table is zero-initialised at file scope and only gets populated by
`kai_main_install_defaults`; a missing default install therefore
crashes at the first op call exactly the way #582 did.

## Verification against ahu fixtures

Cloned `git@github.com:kaikailang-org/ahu` at HEAD and ran every
`examples/*/main.kai` and `tests/*.kai` file through the LLVM backend.

- **Pre-fix:** 9 of 16 fixtures run without segfault (per the brief's
  starting state, and consistent with the previous #570 retro).
- **Post-fix:** **9 of 16** — same count. The 6 fixtures the brief
  flagged as blocked on #582 still segfault, but they all declare
  `Link` (and several declare `Monitor`) in their type rows:
  `tests/restart_temporary_crash.kai`,
  `tests/restart_transient_normal.kai`,
  `tests/restart_intensity_escalate.kai`,
  `tests/cross_restartable_cell.kai`,
  `tests/cross_restartable_cell_restart.kai`,
  `examples/resilient_counter/main.kai`. Plus `examples/echo/main.kai`
  is the seventh, which the brief did not list but is in the same
  bucket.

The #582 reproduction is closed (no longer segfaults; matches the C
backend's stdout+stderr byte-for-byte). The remaining 6 ahu failures
are blocked on the same root cause but for a different effect (Link),
not on the Cancel install gap. The previous lane's retro predicted
this exact situation: "the failing 5 ahu tests all use Link + Cancel
+ Console (per their type signatures), so they need the Link/Cancel
additions too. A follow-up lane can audit all eight missing builtins
together."

So this lane unblocks the **minimal Cancel-only repro** (which is what
#582 actually documents), and the ahu test suite stays at 9/16 pending
the Link install lane. The right framing is: #582's scope is the
Cancel slice; the ahu number was reported alongside it because Cancel
is *one* of the missing pieces, not the only one. Closing #582 closes
the Cancel slice cleanly. The Link follow-up is a separate issue that
should be filed and scoped explicitly rather than blended into the
#582 close criteria.

## Selfhost / tier behaviour

- `make tier0` — green. Selfhost byte-identical (Cancel is not in the
  compiler's own `main` row), 28 demos passing (baseline 27).
- `tools/test-llvm-driver.sh` — 14 fixtures pass parity (was 13),
  including the new
  `examples/llvm/cancel_raise_in_fiber_under_mailbox.kai`. The
  pre-existing `examples/quickstart/02_fizzbuzz.kai` failure
  (effect-row issue, unrelated to this lane) remains the only failure
  and is unchanged on `main`.
- `make tier1` — green local (full run).

## Real cost vs estimate

Brief estimated 2–4 hours. Actual: ~50 minutes, of which:

- ~10 min confirming the repro on the pre-fix binary (C backend exit
  1 with deadlock, LLVM backend exit 139 with SIGSEGV).
- ~10 min reading the previous lane's retros (#570 and #570+#571),
  confirming the diagnosis pattern, locating the install / teardown
  sites.
- ~10 min implementing the five mechanical additions (header declare,
  globals block, install block, teardown pop, runtime wrapper).
- ~20 min validating against the repro, the new fixture, the LLVM
  driver, the ahu tree, and tier0/tier1.

The estimate's generosity reflects "if the reporter's hypothesis had
been right, this would have required actual investigation into the
LLVM lowering of `Cancel.raise()` itself, possibly multiple hours of
LL IR reading". Because the diagnosis was actually one layer off and
the previous lane had already pinned the family of bugs, the work
collapsed to a mechanical port.

## Fixtures shipped

`examples/llvm/cancel_raise_in_fiber_under_mailbox.kai` — the verbatim
shape of the #582 repro (16 lines, no external deps). Wired into
`tools/test-llvm-driver.sh`'s `FIXTURES` list so the parity sweep
builds it with both backends and diffs the combined stdout+stderr.
Pre-fix on the LLVM backend reproduces SIGSEGV; post-fix both
backends print the same two `Stdout.print` lines, deadlock, and exit
1 with the same runtime banner.

No negative fixture: the install branch is unconditional within the
`if list_has(main_row, "Cancel")` gate, matching every other builtin's
install pattern.

## Cross-references

- Issue #582 — closed by this lane.
- Issue #570 — closed by the previous lane (Spawn install). This lane's
  fix is the same shape applied to a different effect.
- Issue #571 — closed by the lane before that (lambda info table).
  Structurally unrelated to #582 but cited in the brief as the
  third-instance pattern.
- `docs/lane-experience-issue-570-llvm-spawn-default.md` — the retro
  that enumerated Cancel as one of the eight pending defaults; this
  lane is the targeted follow-up.
- `default_setups_for` (stage2/compiler.kai:18391) and
  `builtin_default_install_order` (stage2/compiler.kai:18418) — the
  C-backend pattern that the LLVM hand-written branches are mimicking
  effect-by-effect.
- ahu repository: the 6 fixtures blocked on `Link` remain failing
  post-fix because Link is a separate missing default. Recommend
  opening a Link/Monitor install follow-up as the next lane to fully
  unblock ahu under LLVM.

## Follow-ups

1. **Link default handler install** (the next unblock for ahu's 6
   remaining fixtures). Same shape as this lane: declare ops, push
   globals, install, teardown, runtime wrappers. `builtin_link_ops()`
   has 3 ops per `stage2/compiler.kai` and `kai_default_link_*`
   wrappers exist in `runtime.h`.
2. **Monitor default handler install** (some ahu fixtures use this
   too).
3. **Stdin / Env / File / Signal / Process** — the remaining 5
   unhandled builtins. None are user-visible right now per the issue
   tracker, but the gap should be audited before they surface.
4. **Strategic refactor**: migrate `llvm_emit_main_install_defaults` to
   AST-derive from `default_setups_from_block` the way the C backend
   does. Eliminates the per-effect maintenance and prevents this
   category of gap entirely. Larger lane; not a hotfix; should land
   after the per-effect ports leave the install table empty.
5. **Parity CI (#575)**: this lane raised the per-PR LLVM parity
   confidence by one regression fixture. #575 — full parity sweep in
   CI — should land before too many more missing-default ports
   accumulate, so regressions in this area are caught on every PR
   rather than discovered by downstream users.

## Recommendation on #575 timing

#575 is becoming load-bearing. Three consecutive lanes have shipped
LLVM-emitter fixes (#571, #570, #582), each gated by a single per-fix
fixture in `tools/test-llvm-driver.sh`. The parity-sweep approach
catches "C builds and runs, LLVM doesn't" *before* a downstream user
reports it. With ahu pinning `KAI_BACKEND=c` as a workaround and the
Link/Monitor lane still pending, the asymmetry between backends is
not closing fast enough on its own. Recommend launching #575 in
parallel with the Link install lane, not after — the parity sweep
will surface the next missing default (whichever it turns out to be)
before the same out-of-band reporting cycle has to play out again.
