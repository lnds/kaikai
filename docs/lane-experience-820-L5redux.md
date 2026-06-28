# Lane experience — #820 L5-redux (every effect rides the frame)

## Scope as planned vs as shipped

**Planned.** Make the evidence frame total: remove the 21-name builtin
whitelist so every declared effect gets a frame slot, and convert a
`default {}`-bearing effect's supplier from a stack push (the by-name walk's
source) to the startup-minted default node, supplied through the frame. Keep
the walk live (delete is L5-delete). Native mirrors the C oracle.

**Shipped.** All of the above, plus one structural piece the brief did not
anticipate: **the native perform site did not read the frame at all.** It
always called `kaix_evidence_lookup_node` (the by-name walk). The native frame
was ABI-shaped — built at the call site, passed as `__evf` param 0 — but the
perform ignored it; native parity held only because the walk happened to
resolve the same node the slot would have. Removing the default's stack push
breaks that: with no default node on the stack, the native walk finds nothing.
So L5-redux had to **wire the native perform to read `__evf[slot]`** (the
C-direct mirror) before the conversion could hold on both backends. This was
confirmed by disassembly: `_emit()` called `kaix_evidence_lookup_node` directly,
took no frame param. The user chose Option A (wire it, the real §3 single path)
over leaving the split.

## Design decisions and alternatives

- **Supplier classification (design §3, three cases).** At a call site, a slot
  is filled by: the caller's forwarded `__evf[i]` (the effect is in the caller's
  frame); the effect's startup-minted `_kai_default_node_<eff>` when it has a
  `default {}` block and no lexical handler; else the live by-name walk for a
  lexically-handled effect the frame does not forward. The default-bearing
  property no longer decides *whether* an effect rides the frame, only *which*
  supplier fills its slot.

- **Default node lifetime — file-scope globals, no stack push.** The C-direct
  emitter minted `_kai_default_ev_<eff>` / `_kai_default_node_<eff>` as `static`
  locals of `int main()` and ended with a `kai_evidence_push`. Promoted them to
  file-scope globals so any call site can address `&_kai_default_node_<eff>`,
  and dropped the push (and its matching teardown pops). The walk still resolves
  a *lexical* handler pushed above; the default itself rides the frame. Native
  already minted these as module globals (`llvm_add_global_zeroed`); it kept the
  globals, dropped the push, and recovers them by name (`llvm_get_named_global`).

- **Superset minting (the dead-chain link bug).** The default globals must be
  minted for *every* default-block effect, not just main's row. A dead-code
  call chain A→B where both perform a default-bearing effect absent from main's
  row would otherwise reference `&_kai_default_node_<eff>` with no definition —
  a link error in C, an absent global in native. Reused the same DECLARED-effect
  superset that `kir_synth_handler_effects` walks for op field indices. The
  minted-but-not-installed globals are zero-init and inert (never pushed, never
  dispatched on a dead path), so the soundness cost is nil — only N extra
  zeroed globals (N = default-block effects in the program).

- **Native perform reads the slot — threading.** `nemit_perform` needs the
  enclosing fn's symbol to resolve its slot index
  (`native_ctx_frame_slot_index`). Threaded `cur_fn: String` through
  `nemit_fill_blocks → nemit_stmts → nemit_stmt → nemit_op_fx[_slot] →
  nemit_perform → nemit_perform_node`. The slot read is a GEP into `__evf`
  (param 0) at the index, mirroring C's `__evf[slot]`. Alternative considered:
  store `cur_fn` in the C-side ctx — rejected, it touches runtime.h (two copies)
  for what the existing `handlers`/`fns` threading already models.

- **Two new native prims.** `llvm_get_named_global` (recover a default global by
  name at the call site) and `llvm_handle_is_null` (discriminate the
  get-named-global miss → fall to the walk). Registered in stage1's prim table
  and (the handle-returning ones) in `native_handle_fns()` so kaic1's type-blind
  Perceus does not decref a raw LLVM pointer.

- **C path is single-TU by default, not modular.** The `kai` wrapper's default
  C path is `emit_program` (single translation unit), not `emit_program_modular`
  (opt-in via `KAI_MODULAR=1`). The first fix landed only in the modular path
  and the default C build still broke on an undeclared `_kai_default_node_<eff>`
  (the `static` definition emitted *after* its uses, at `int main()`). The real
  fix emits the global definitions ahead of the fn bodies in `emit_program`.

## The closure oracle — honest framing

The brief asked for a fixture that "is corrupted by by-name today and correct
under total-frame" — a *fail-before / pass-after*. **That case does not exist in
L5 without `as`-aliases or capability-as-parameter** (both L6). The by-name walk
with the default pushed on the stack is semantically complete for
lexical-by-innermost resolution: every default-vs-handler shape resolves
correctly today. Frame and walk diverge only when the correct resolution is NOT
the dynamic stack top — which needs `as`/capability-param (L6), or a default
DECOUPLED from the stack (this lane's own change).

So the oracle here is a **no-regression guard of the push→frame-supplier
conversion**, not a fail-before. Its sharp form (per the architect): a
default-bearing effect (`Log`) performed THROUGH a call with a handler of a
*different* effect (`Stdout`) on top of the stack at the perform. The walk
cannot rescue a broken frame supplier here — the stack top is not `Log` — so a
wrong supplier yields garbage or a crash, not the right answer by accident. The
pure result `42` must hold on both backends.

Verified pre-lane: this form already returns `42` on both backends (the walk
finds the pushed default, skipping the interposed `Stdout` node by tag
mismatch). Post-lane it must still return `42` with the default no longer
pushed — now resolved through the frame. Fixture:
`examples/effects/default_through_call_under_other_handler.kai`.

## Fiber-local effects must NOT ride the frame (the trap-exit regression)

Removing the *whole* whitelist was wrong: it conflated two classes. An effect's
evidence is transportable by value only when its `default {}` is a static,
stateless handler (`Stdout`/`File`/`Clock`/`Mutable`/`Process` …). An effect
the scheduler installs **per-fiber** — `Cancel` (cancel_pad), `Link` (link-set),
`Monitor` (observer registry), `Spawn` (nursery), `Actor` (mailbox + `self`) —
is NOT transportable: routing it through the frame carries a parent's handler
across a `spawn`, the escape vector §3 forbids.

It bit `issue_103_trap_exit_cancel_handler` under tier1-asan (not the normal
serial parity — that fixture is ASAN-gated, so the first parity run missed it).
`kai_check_trap_exit_cancel_bypass` lives *inside* `kai_evidence_lookup_node`:
it fires only when `Cancel.raise()` resolves via the walk. Routing Cancel
through the frame skipped `lookup_node`, so the bypass never ran and the child
fiber's Cancel bound the outer lexical handler (`OUTER HANDLER caught Cancel`)
instead of unwinding at its cancel_pad (`supervisor got: Crashed`).

Fix: `frame_is_fiber_local` keeps `{Cancel, Link, Monitor, Spawn, Actor}` off
the frame (still on the walk, where the bypass and per-fiber resolution live).
The criterion — not the enumeration — is documented in `evidence_frame.kai`:
"evidence the scheduler installs per-fiber." A new concurrency effect must join
that set or it reintroduces the spawn-crossing bug. Boundary-detection was
rejected (it would also kill group-A transport into fibers, the lane's value).

## Harness emission-shape assertions (the m7a_7 regression)

The `m7a_7_default_*` and `m8_*` effect targets in `stage2/Makefile` don't just
run the binary — they `grep` the emitted C for a specific wiring shape
(`kai_evidence_push(&_kai_default_node_<eff>`). The push→`kai_evidence_init_default`
conversion makes those greps stale even though the binary output is unchanged
(hello/world/warn still correct). Updated all six greps (console stdout/stderr,
fail, spawn, cancel, process) to match `kai_evidence_init_default`. The
`push_with_jmp` / `pop` greps for *user* handlers (m7b counter) are untouched —
the conversion only changed the default install, not `emit_handle`.

Gate lesson: serial dual parity is not the full CI surface. `m7a_7` (tier1
shard-3) and `issue_103` (tier1-asan) both escaped the first local gate, which
ran only the oracle + parity. The local gate for a default-install change must
be the full `make tier0 tier1` (all shards) + `tier1-asan` + serial parity, not
parity alone — CI runs emission-shape assertions and ASAN-gated fixtures the
parity harness does not.

## Coverage check

After the lane, no emit site routes a DECLARED effect to
`kai_evidence_lookup_node`: a declared effect always takes a frame slot, and the
perform reads the slot. The walk runtime function stays live (L5-delete removes
it once parity proves it dead); it is still emitted for a lexically-handled
effect that the frame does not forward (the rama-1 fallback) and as the call-site
fallback for a non-default effect with no forwarded slot.

## Fixtures + coverage gaps

- `default_through_call_under_other_handler.kai` — the closure oracle (above).
- Existing #820 fixtures (`evidence_two_level_forward`,
  `evidence_handler_below_lexical`, `default_block_full_user_handle`) regress-test
  forward + below-lexical + user-handle-over-default; all still pass.
- Gap left for L5-delete: a dead-chain A→B fixture exercising the superset-mint
  link path (architect-suggested). Not added here — the superset mint is
  covered structurally by the self-compile (the compiler corpus has dead
  default-bearing performs), and a focused fixture belongs with the delete lane
  that proves the walk dead.

## Follow-ups for next lanes (L5-delete)

- Delete `kai_evidence_lookup_node`, the alias special-case, the spawn-clone
  (design §4) once full-corpus parity proves no declared effect reaches the walk.
- The call-site rama-1 fallback (lexically-handled effect, walk-filled slot) is
  the last walk consumer for declared effects; L5-delete must convert it to a
  lexical-node supplier (the §6 named-instance surface) or prove it unreachable.
