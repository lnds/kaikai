# Lane experience — issue #706: TCO in the LLVM backend

**Scope:** implement tail-call optimization in the LLVM backend so
self-tail-recursive functions run in constant stack space, matching the
C backend. TCO is mandatory (Tier 1 load-bearing per CLAUDE.md) — the
LLVM backend was invalid for 1.0 without it. Sequenced after parity
lane A (#707) and before lane B in `docs/llvm-parity-plan-2026-05-26.md`.

**Layer the change is in:** the codegen *consumer* layer
(`stage2/compiler/emit_llvm.kai`) + the pipeline fork
(`stage2/compiler/driver.kai`). The tail-position + dropmask *analysis*
(`tcrec_*` in `emit_c.kai`) was reused untouched — this lane consumes
its output in a second backend, it does not rewrite the analysis.

## Option 1 vs Option 2 — chose Option 1 (sentinel → br-loop)

The issue offered two shapes:

1. **Mirror the C `tcrec_*` pre-pass**: run the analysis for both
   backends and lower its sentinel to a param-rebind + branch-back.
2. **Emit `musttail call`**: idiomatic LLVM, but requires exact
   signature match + immediate `ret`, and covers only what LLVM's own
   tail-call contract allows.

I took **Option 1**, as the brief strongly preferred, and it was the
right call for one decisive reason: **the C `tcrec_rewrite_decls`
pre-pass is ~1000 lines of proven tail-position + per-call-site
dropmask analysis** (rules 1–3, the #92 R6 history, the perceus-wrap
descent). Reusing it verbatim means the two backends agree on *which*
sites are tail-self-calls and *which* params get dropped — a single
source of truth. `musttail` would have re-derived all of that inside
the LLVM emitter, and the dropmask (RC discipline) has no `musttail`
analogue. Option 1 also keeps mutual-recursion as a clean future
extension on the shared analysis rather than two divergent ones.

The pipeline already had the exact fork at `driver.kai:5089`
(`if use_llvm { post_perceus_decls } else { tcrec_rewrite_decls(...) }`).
The fix was to drop the skip and run the pre-pass unconditionally.

## SSA param-rebind mechanism — alloca, not phi

The load-bearing structural problem: an LLVM `%p_<name>` function
argument is an immutable SSA value, so a back-edge cannot reassign it.
Two ways to make params re-assignable across the loop:

- **phi nodes** at a loop header — "optimal", but the emitter would have
  to know *all* back-edge predecessors before emitting the header, and
  the existing emitter is single-pass (it threads `cur_label` forward,
  it does not collect predecessors up front). Structurally hostile.
- **alloca + load/store** — one slot per param, stored on entry, reloaded
  at the loop head, restored on each back-edge. This is exactly how the
  C backend models a mutable C parameter (`kai_<p> = _tN`), and the LLVM
  emitter already uses alloca/load/store/GEP liberally (records,
  evidence, FFI shims). `mem2reg` promotes the slots to phi under `-O`
  anyway, so alloca is "free" at runtime.

I chose **alloca**. The mechanism (`llvm_emit_fn`, gated on
`llvm_body_has_sentinel`):

1. rename the incoming arguments `%p_<name>` → `%parg_<name>`
   (`llvm_param_list_tcrec`);
2. in `entry:`, `alloca %pslot_<name>` per param, `store %parg_<name>`,
   `br label %tcrec.loop`;
3. open `%tcrec.loop`, reload each param **as `%p_<name>`** — the exact
   register the body already reads, so the body IR is emitted unchanged.
   The reload is the single dominating definition of `%p_<name>` for the
   whole loop body.

The sentinel lowering (`llvm_emit_tcrec_goto`, in `llvm_emit_call`):
evaluate the new args into temps, drop the dropmask'd params (on the
*current* `%p_<name>` value — same value C drops via
`kai_internal_drop(kai_<p>)`), store the temps into the slots, `br
%tcrec.loop`, then **open a fresh dead block**. The dead block is the
one real subtlety: the surrounding emitter always appends a
`br`/`ret`/phi after a tail expression, so after our unconditional
back-edge `br` we must leave `cur_label` pointing at a well-formed (if
unreachable) block with a valid `last_val` (`undef`). LLVM's CFG
simplification drops the dead block; clang accepts it.

## Structural surprise the brief did not anticipate

My first `llvm_body_has_sentinel` was a tail-position-only walk
(EIf/EMatch/EBlock-tail/ECall). It compiled and the repro worked — but
selfhost's `list.nth` emitted a `store ... %pslot_i` against a slot that
was never alloca'd, because the sentinel was **not in tail position**:
Perceus wraps multi-use-param bodies in
`EBlock([entry, SLet(__pcs_ret, <body>), exit_drops], Some(__pcs_ret))`
and tcrec rewrites the **SLet RHS** (a statement). The detector must
mirror `tcrec_scan_kind` (emit_c.kai) and descend block *statements*,
not just the block tail. This is the same perceus-wrap coupling that
bit the C TCO at #89 — re-encountered from the consumer side. Lesson:
the sentinel detector and the analysis must agree on reachability, and
"sentinels are always in tail position" is false once Perceus runs.

Also: the LLVM backend has **no UFn (raw-scalar) param path** — every
param is boxed `%KaiValue*`. So only the boxed branch of the C
`tcrec_emit_goto` has an LLVM analogue; the `kair_<name>` raw-rebind
branch is C-only. This simplified the lowering (no raw-type bookkeeping).

## Sentinel wire-format single-sourced

To avoid two copies of the `__kai_tcrec|<sym>|<dropmask>|<p>...` format,
I moved the recognition + parsing helpers (`es_tcrec_is_sentinel`,
`es_tcrec_split_pipe`, `es_tcrec_parse_decimal`) into `emit_shared.kai`
(imported by both backends). `emit_c.kai`'s `tcrec_is_sentinel` /
`tcrec_split_pipe` / `tcrec_parse_decimal` are now thin wrappers — the C
TCO behavior is byte-identical, only the format owner moved.

## Verification

- Repro `loop(5000000, 0)` → `5000000`, exit 0 on **both** backends
  (was exit 139 / stack overflow under LLVM).
- **`make selfhost` byte-identical** (`kaic2b.c == kaic2c.c`). Critical
  gate: the stage-2 compiler is itself full of self-tail-recursion; a
  broken rebind would crash selfhost or change its output. It did the
  first time (the tail-only detector bug) — fixing the detector restored
  byte-identity.
- **ASAN+UBSan clean** on the fixture, both backends (`detect_leaks=0` on
  macOS). A wrong dropmask would be a use-after-free or leak; ASAN saw
  neither.
- All 8 existing C-side TCO tests still green (`test-tco`,
  `test-tco-309`, `test-tco-perceus-wrap`, `test-tco-rule3-*`, etc.) —
  the shared analysis was untouched.
- `make tier0` green locally; tier1 deferred to CI.

## Parity fixtures that fell out together (confirming TCO, not bugs)

Removed from `tools/backend-parity-skips.txt` (each re-tested
individually, both backends, C == LLVM):

- `examples/perceus/unbox_bench.kai` — tight numeric loop, now exit 0.
- `examples/perceus/unbox_bench_real.kai` — same, Real payload.
- `demos/euler4/main.kai` — deep arithmetic recursion, now exit 0.

Reclassified (segv fixed, but still parity-exempt):

- `examples/perceus/rb_tree_bench.kai` — the segv is gone; both backends
  now run the 1M-insert loop to completion. The only remaining diff is
  the `elapsed: N.NNNs` wall-clock line (non-deterministic by design,
  same class as `poker_dealer`). Skip reason updated from "segv" to
  "non-deterministic timestamp".

Still overflow / diverge after TCO — **genuine non-TCO causes**, left
skipped (out of scope, per the issue's "triage separately"):

- `m7a_7_default_env` / `_file` / `_stdin`, `process_basic`,
  `issue_558_user_effect_default_main_install` — LLVM exit 139 with **no
  deep recursion** (single `Env`/`File` op). These are the Cluster B
  default-handler null-deref, not TCO.
- `m8_fiber_stack_overflow` — designed to overflow (SIGBUS 138).
- `rb_tree_bench`'s timestamp (above), `rc_discipline_record_variant`
  (unbox type mismatch), `r9_clause_capture`/`spiral` (Char/Array bugs)
  — Clusters E/F, orthogonal.

The harness also reports 4 pre-existing divergences not in any skip
line and not in the TCO triage (`issue_682_cancel_sibling_handler`,
`auto_install`, `cross_package_effects/consumer`,
`m8x_signal_await_parks`) — scheduler-order / package-resolution /
Array / fiber-stack, all orthogonal to this lane and pre-dating it.
`tier1-backend-parity` is diagnostic (not a required check), so these
do not gate.

## Cost vs estimate

Roughly as estimated. The bulk was reading the C `tcrec_*` machinery to
understand exactly what the sentinel carries (~1 pass), then the
mechanism design (alloca vs phi — settled quickly once I saw the
single-pass emitter). The one real cost overrun was the tail-only
detector bug: it passed the repro and only selfhost caught it, costing
one rebuild cycle. That is the intended role of the selfhost gate — the
fixture alone would have shipped a latent miscompile of any
non-tail-position sentinel.

## Follow-ups left for next lanes

- **Mutual tail recursion** (`f → g → f`) is not covered; only
  self-recursion. The shared `tcrec_*` analysis only plants self-call
  sentinels. `musttail` (Option 2) layered on top would be the natural
  vehicle — a clean future enhancement, not a 1.0 blocker.
- The 4 orthogonal parity divergences above belong to Clusters B/C/E/F
  in the parity plan; lane B onward.
