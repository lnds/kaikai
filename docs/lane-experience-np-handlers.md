# Lane experience — np-handlers (no-effect-handler native-parity family)

**Branch:** `np-handlers` · **Date:** 2026-06-12→13 · **Closes:** none (no
baseline change — see *Scope-as-shipped*) · **Touches:**
`stage2/compiler/kir_lower.kai`, `examples/effects/`,
`docs/native-parity-gaps.md`.

## Scope-as-planned vs scope-as-shipped

**Planned (brief):** close the no-effect-handler family of the native-parity
baseline — 11 gaps (Spawn 7, Clock 2, NetTcp 1, File 1 / `stream_early_stop`)
— by extending the #793 / burn-down-6 default-handler-install pattern so the
native walk installs the missing Spawn/Clock/NetTcp/File handlers. Each
fixture closed byte-id vs C-direct; baseline drops by what was closed.

**Shipped:** the brief's premise was **stale**. Measured against HEAD (merge
#827, burn-down 6) on a freshly-built `KAI_LLVM=1 kaic2`:

- **10 of the 11 fixtures already pass parity.** Burn-down 6's synth-handler
  superset (`kir_synth_handler_effects` + `KProgram.synth_defaults`) had
  already resolved the actor/Spawn/Clock/NetTcp dispatch shape — dual_actor
  ×3, m8_7/m8_8 mailbox ×3, nested_lambda_with_mailbox, date_basic, date_iso,
  http_server_book_ch17 all build + run identical to C. Only **one** of the
  11 was still in `tools/native-parity-baseline.txt`: `stream_early_stop`.
- **The whole corpus had exactly ONE residual no-handler abort:**
  `stream_early_stop` (`KPerform: no handler for effect ReadFault`). A
  scan of every depth-2 `*.kai` + `**/main.kai` on clean HEAD confirmed it:
  `total no-handler aborts on clean main: 1`.

So there were never 7 Spawn / 1 NetTcp handler gaps to close in this lane —
burn-down 6 closed them. The brief's "Spawn / Clock / NetTcp / File" was the
PRE-burn-down-6 family shape (it survived into the brief because the section
of `docs/native-parity-gaps.md` that LISTS the 11 fixtures was not updated
when burn-down 6 reclassified them — the prose table at line 104 said "Clock
slice closed" but the list at line 306 still enumerated all 11).

## What the lane actually fixed

The last residual abort: a `pub effect` with **NO `default { }` block**
(`ReadFault`, declared in `stdlib/stream.kai`), performed only inside a
function `main` never calls (`pump_lines : … / File + ReadFault`, dragged in
because a `from_list`-only program still imports the whole `stream` module).
The native walk emits a body for every fn, so it lowered the dead
`KPerform ReadFault` and aborted the build — burn-down 6's synth-handler
superset covered only default-BLOCK effects (`kir_default_for_effect`
returns `None` for a no-default-block effect).

**Fix (`kir_lower.kai`, `kir_synth_handler_effects`):** two atomic changes,
both inside the `synth_defaults` path (native-only, invisible to the C
selfhost byte-id):

1. Iterate **declared effects** (`DEffect` decls), not `install_order`.
   `install_order` is `builtins ∪ main_row`, so a non-main-row effect like
   `ReadFault` was never even enumerated. The declared-effect walk reaches it.
2. For an effect **with** a default block, synthesise the `KDefault` from the
   block (unchanged — the `$extern_handler` forwarder symbols). For an effect
   **without** one, synthesise the op→field-index map straight from the
   `effect` decl's op list, in declaration order. The recorded per-op symbol
   (`kaix_synth_<eff>_<op>`) is a placeholder the runtime never invokes —
   `nemit_perform` reads only the field INDEX (`nfx_op_base() + position`),
   and the position from declaration order matches the C-direct `Ev<eff>`
   struct layout exactly. A never-installed effect is inert; the field map
   only lets the dead `KPerform` lower to the same dispatch shape the
   C-direct oracle emits (which never aborts and would null-deref only if the
   dead fn ran — it does not).

This is general, not point: it closes the no-default-block-dead-perform abort
for ANY effect, not just `ReadFault`. The whole-corpus no-handler abort count
is now **0**.

## Why the baseline does NOT drop

`stream_early_stop` — the only baseline fixture this family owned — does not
close byte-id, because it has a **second, independent root cause** outside
this lane's zone: **pipe-lowering**. Once the handler abort is gone, the
fixture builds + runs but prints `count=0` (C prints `count=3`). Bisected:

- `count(from_list([10,20,30]))` (no pipe) → native `count=3` ✓
- `from_list([10,20,30]) |> count` (pipe) → native `count=0` ✗

`EPipe` / `EMapPipe` / `EFilterPipe` all lower to no-op `KUnitV`
(`kir_lower_walk.kai:152-156`) — the documented pipe-lowering gap (a prior
first-cut was reverted on a multi-candidate-arg trap; see the gaps doc
§"pipe-lowering"). Porting `emit_c`'s `emit_pipe_expr` (a ~70-line, many-shape
function with UFn mask marshalling) to the KIR is a separate family and a
separate lane — and 4 lanes were concurrently editing `emit_native`/`kir`, so
cutting into pipe-lowering here would both violate *one-worktree-one-thing*
and risk colliding with them. The user chose (AskUserQuestion) to ship the
handler piece alone and leave pipe-lowering to its own lane.

## Design decisions / alternatives considered

- **Synthesise for ALL declared effects vs only-performed effects.** Chose
  all-declared: simpler, and provably inert (a declared-but-never-performed
  effect emits no `KPerform`, so its synth field-map is never consulted —
  verified: a program that only declares an effect builds clean unchanged).
  Filtering to performed-only would need a perform-collection pass over the
  KIR for zero correctness gain.
- **Real forwarder symbol vs placeholder for no-default-block ops.** Chose a
  placeholder (`kaix_synth_<eff>_<op>`): the symbol is structurally never
  called (the existing burn-down-6 comment already establishes this for the
  default-block synth handlers). A real symbol would imply an installable
  default the language semantics do NOT define for `ReadFault`/Spawn/File.

## Structural surprises the brief did not anticipate

- The brief described a runtime-handler-install task ("install the missing
  Spawn/Clock/NetTcp/File handlers"). The actual residual was a **build-time
  dispatch-shape** gap (field-index map for a dead perform), not a runtime
  scheduler/syscall task. The gaps-doc prose even said these "need a real
  scheduler/syscall runtime" — true for a LIVE perform, but the only residual
  was a DEAD perform, which needs only the C-direct's never-aborts shape.
- The brief's fixture count (11) was a doc-drift artefact: the family LIST in
  the gaps doc lagged the family TABLE by one burn-down. Fixed both here.

## Fixtures added / coverage

- `examples/effects/dead_perform_no_default_block.kai` (+ `.out.expected`):
  the reduced regression — a no-default-block `effect Fault` performed in a
  dead fn; build-OK + byte-id parity vs C. Gated by `test-backend-parity`
  (the harness globs `examples/effects/`), pass=396 (was 395).
- Coverage gap left: `stream_early_stop` stays a baseline gap (pipe-lowering).

## Gates (all green, macOS dev box, `KAI_LLVM=1` clean rebuild)

- `make -C stage2 selfhost` → `kaic2b.c == kaic2c.c` (byte-id; the change is
  native-only `synth_defaults`, invisible to the C path).
- `make -C stage2 test-kir` → all 7 KIR goldens unchanged (synth_defaults is
  not dumped per-fn; no golden regeneration needed).
- `TARGET_BACKEND=native NATIVE_PARITY_RATCHET=1 test-backend-parity.sh` →
  pass=396 fail=65, **ratchet OK (zero regressions)**. The only "now passes"
  are `list_helpers`/`list_zip3_scan` (the documented macOS-pass/Linux-SIGSEGV
  pair — NOT removed, per the burn-down-1/2/3 lesson).
- Whole-corpus no-handler abort scan → **0** (was 1: `stream_early_stop`).

## Follow-ups for next lanes

- **pipe-lowering** (its own lane): port `emit_pipe_expr` to the KIR walk;
  closes `stream_early_stop` (+ euler1/fizzbuzz/capture/imc). The
  multi-candidate-arg trap (`EModCall` callee, corrupt `args` upstream of the
  KIR) is the known blocker — diagnose that arg shape first.
- `tier1-native` (Linux/CI) re-validation: this lane's fix is a
  platform-agnostic lowering change, but the baseline ratchet is gated on
  Linux; CI is the real oracle.
