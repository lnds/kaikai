# Lane experience — KIR native walk, effects subset 2b (stateful handlers + alias dispatch)

KIR Lane 1.2 Parte B. Builds on the effects 2a subset (one-shot user
handlers: KInstall/KSetjmp/KPop/KPerform/KResume/KFnClause) and the
builtin-defaults subset (2c). This lane covers the two effect features 2a
explicitly deferred: **stateful handlers** (`with Eff(init)`, the `state`
slot + the 2-arg `resume(v, ns)` = KResume2) and **per-instance alias
dispatch** (`with Eff as a`, the `op@a` perform resolved by handler-id).

## Scope as planned vs as shipped

Planned: emit, in the in-process libLLVM native walk, what the C-direct
oracle already emits for stateful + aliased handlers. Shipped exactly
that — both fixtures reach native↔C-direct parity, plus a prerequisite
frontend change and one debug hook.

Two halves, both shipped in this lane (the task grouped them; not phased):

1. **Stateful handlers.** A `handle { ... } with State[Int](40) { get(resume)
   -> resume(state); set(v, resume) -> resume((), v) }` now runs native and
   matches C-direct. Three native pieces:
   - **KInstall `state=` slot** — `nemit_install_state` writes the
     `with Eff(init)` value into the Ev `state` slot (field 2, byte 16) via
     `kaix_clause_state_set(ev, init)`. Stateless handlers carry no init →
     no write (2a path unchanged).
   - **Clause state prologue** — `nemit_seed_clause_state` reads
     `self->state` (new shim `kaix_clause_state_get`, byte 16) once at the
     clause's entry and binds it under `state` AND the legacy alias `log`,
     identical to emit_c's `clause_state_prologue` (the `["state","log"]`
     pair is hardcoded in all three emitters — m7b #11 debt, not generic).
   - **KResume2** — `nemit_resume2` emits `kaix_clause_state_set(self, ns)`
     then `kaix_cont_resume(k, v)` + ret, mirroring emit_c's `self->state =
     ns; kai_cont_resume(k, v)`.

2. **Alias dispatch.** A `handle { log.print(...) } with Io as log { ... }`
   lowers the perform to `Io.print@log` and the install to `install Io _h as
   log`. Native pieces:
   - **Install alias capture** — `nemit_install_alias` boxes the fresh
     handler_id (`kaix_int`) and stores it under the `__alias_id__<a>`
     register (alloca reserved by `nfx_reserve_alias`), mirroring emit_llvm's
     sentinel local + the C `kai_alias_<a>_id`.
   - **Perform by-id** — `nemit_perform` splits the `<op>@<alias>` tag
     (`nfx_op_before_at`/`nfx_op_alias`, a local `@`-scan to avoid an
     import cycle on infer's `string_index_of_at`); `nemit_perform_node`
     resolves an aliased op via `kaix_to_int(__alias_id__<a>)` →
     `kaix_evidence_lookup_node_by_id`, instead of the by-name
     `kaix_evidence_lookup_node` that would pick the innermost same-effect
     handler.

## Prerequisite: the KFn.stateful frontend change (PR #795)

The native backend consumes the structured `KProgram`, whose clause `KFn`
did NOT carry the `stateful` flag the typer computes in `ClauseInfo`. The
`get` clause body referenced a FREE register `state` the KFn never
declared. PR #795 (delegated, merged before this lane) added `KFn.stateful`
+ the `: box stateful {` dump marker. With the flag in hand, this lane's
`nemit_seed_clause_state` keys the prologue on `f.stateful`. (Same shape as
the three earlier frontend KIR gaps — TRMC, list-match, literal-match — but
this one the typer already knew; the KIR just dropped it.)

## Structural surprises the brief did not anticipate

- **The "allocas in entry only" rule bites every new register.** `__self`,
  `state`, `log`, and `__alias_id__<a>` are introduced by the pass-2 seed /
  install, not named as KIR registers, so a naive `nemit_store_reg` aborted
  `store to unbound register`. Each had to be reserved as an entry-block
  alloca in pass 1 (`nfx_reserve_clause_state` / `nfx_reserve_alias`),
  exactly like `nfx_reserve_k` did for the continuation in 2a.
- **load-vs-address bug (cost the most time).** `nemit_resume2` first passed
  `native_ctx_find_reg("__self")` — the alloca ADDRESS — to
  `kaix_clause_state_set`, which writes `(char*)self+16`. That clobbered the
  clause's own stack frame instead of the Ev, so the second `get` read the
  init (40) not the updated value (42). Fix: `nemit_load_reg` to pass the
  LOADED Ev value. The native IR dump (`KAI_NATIVE_DUMP_IR`, added this
  lane) showed `call ... kaix_clause_state_set(ptr %__self.addr, ...)` — the
  `.addr` was the tell.
- **SOUNDNESS RISK #1, again.** `nemit_perform_node` returns a `Handle` (the
  looked-up node Value*). Unlisted in stage1's `native_handle_fns()`, the
  type-blind kaic1 Perceus `kai_decref`'d it → the call instruction
  corrupted to `<Invalid operator>  i64 %10, ptr @kaix_evidence_lookup_node_by_id`
  and the module failed verify. Every new Handle-returning walk fn MUST be
  listed; this is the third subset to hit it exactly as predicted.
- **Module-privacy trap (selfhost-only).** `nemit_load_reg` was private to
  `emit_native_fn`; 2b's KResume2 calls it cross-module. `make kaic2`
  (bundle-concat) hid it; `make selfhost` (real imports) caught it. Marked
  `pub`.

## Fixtures added

- `examples/native/effect_state.kai` — stateful: seeds 40, `set`s 42 via the
  2-arg resume, reads it back, exits 42. Proves the slot is BOTH seeded and
  updated through KResume2 (a get-only fixture would false-green the write).
- `examples/native/effect_alias.kai` — alias dispatch: `with Io as log`,
  `log.emit(7)` resumes with the arg, exits 7. Proves by-id resolution.

Both wired into `tools/test-native-parity.sh` (auto-globbed); suite is now
12/12 (was 11/11 after literal-match). Both verified against the C-direct
oracle (state=42, alias=7) and under ASAN+UBSan (0 errors — stateful +
longjmp is UAF-prone, so this gate matters).

## Debug hook added

`KAI_NATIVE_DUMP_IR=<path>` dumps the in-memory native module IR before
verify (`kai_llvm_emit_object`, runtime.h). Off by default, never affects
the emitted object; it cracked both the load-vs-address and the
SOUNDNESS-RISK-#1 bugs this lane. Kept as a general native-walk debugging
aid.

## Gates (all run locally, all green)

- Native parity: **12 passed, 0 failed** (incl. effect_state rc=42,
  effect_alias rc=7; native == C-direct).
- Selfhost byte-id: **OK (kaic2b.c == kaic2c.c)**.
- ASAN+UBSan on both new fixtures: **0 errors**.
- `km`: emit_native_fx A++ (97.3), emit_native_fx2 A+ (94.6),
  emit_native_term A+ (95.3), emit_native_fn A+ (96.7); cogcom avg ≤ 1.5 /
  max ≤ 3 on all; no new dup groups.

## Follow-ups for next lanes

- **`emit_native_term.kai` is at 393 LOC** (target < 400, hard cap 800).
  This lane added only the KInstall/KResume2 arm wiring (a few lines), but
  the file is now one subset away from the soft target. The NEXT subset that
  touches it (closures/lambdas, or RC) should split the terminators or the
  seed helpers into their own module BEFORE growing it past 400.
- **Multi-shot / non-tail resume** is still out of scope: this subset
  handles tail `resume`/`resume2` (the one-shot continuation). A clause that
  resumes more than once, or in non-tail position, is a later subset.
- **Reader/Writer/Mutable** stateful effects ride the SAME `state`-slot
  machinery as State; they were not exercised by a dedicated fixture but the
  mechanism is identical (`state` read + `resume(v, ns)` write). A fixture
  per canonical stateful effect would harden coverage.
