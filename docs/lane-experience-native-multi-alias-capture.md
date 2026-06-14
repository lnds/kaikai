# Lane experience — native multi-alias closure capture (`native-multi-alias-capture`)

**Scope:** one design-bearing correctness bug in the in-process libLLVM native
backend (KIR Lane 1.5): a closure capturing TWO OR MORE handler-id sentinels
(`__alias_id__<a>`) of the SAME effect, then performing through both, HUNG the
native backend (exit 124) before its body ran. The cluster the brief named:
`multi_var_state_index`, `m7b_15_nested_alias`, `m8_12_self_delegating_handler`,
`demos/spiral/main` — all `nat=124` timeouts vs a clean C-direct oracle. Refs
#789 (the open evidence-resolved-by-op-name issue; the brief's #622 is closed).

**Outcome:** ONE root cause, fixed at the KIR/backend seam. All four cluster
fixtures close parity (124 → exit 0, byte-id vs C-direct), plus the minimal
repro. selfhost byte-id held; tier0 GREEN; ASAN clean on the closed timeouts;
zero parity regressions.

## The root cause — evidence-frame storage keyed by effect, not by site

The bug was NOT the KIR (correct) nor closure capture (`kaix_capture` /
`kaix_closure` correct in isolation). The native backend reserved ONE entry-block
alloca PER EFFECT NAME for each install's evidence frame:

```
nfx_node_reg(eff)  = "__node_" + eff      # "__node_State"
nfx_ev_reg(eff)    = "__ev_" + eff
nfx_jmpbuf_reg(eff)= "__jmpbuf_" + eff
nfx_env_reg(eff)   = "__env_" + eff
nfx_discard_reg()  = "__kai_discard"      # not even per-effect — a single name
```

`nfx_reserve_install` deduped by name (`if reg_slot < 0`), so two nested
`KInstall(eff="State", ...)` — e.g. two `var` cells of the same effect — derived
the SAME name and the SECOND reserve REUSED the first alloca. The runtime push
is:

```c
node->parent    = f->evidence_top;   // (kai_evidence_push_with_jmp)
f->evidence_top = node;
```

Push the SAME `node` storage twice and the second push sets `node->parent = node`
(self-cycle) AND overwrites the first install's handler_id / eff_label / jmp_buf.
`kai_evidence_lookup_node_by_id(id)` then walks `node = node->parent` forever:
the chain never reaches NULL and never matches the looked-up id. Exit 124.

### How it was confirmed (not hypothesised)

1. `KAI_NATIVE_DUMP_IR` showed the closure construction and lambda bodies were
   correct — captures stored/loaded in order, every perform a clean
   `kaix_to_int(alias_id) → lookup_node_by_id`.
2. lldb on the hung repro: `frame #0 kai_evidence_lookup_node_by_id(id=4)` at
   `runtime.h:11367` (`node = node->parent`); `expr node == node->parent` →
   `true`. The cycle was the smoking gun.
3. The IR `define ptr @kai_main` carried ONE `%__ev_State.addr`/`%__node_State.addr`
   alloca but TWO `call kaix_evidence_push_with_jmp(%__node_State.addr, ...)` —
   shared storage, exactly the dedup collapse.

### Bisection that scoped it precisely

- 1 var, or 2 vars where the body touches ONLY ONE → runs (one real install).
- 2+ vars where the closure captures AND performs through 2+ same-effect
  alias-ids → hangs. So the trigger is *multiple same-effect installs alive at
  once*, surfaced through closure capture (the `while` body is a lifted lambda).

The single-var case ran because the optimiser left only one install; the
multi-capture case is the only shape that puts two same-effect `KaiEvidence`
nodes on the stack simultaneously.

## The fix — storage per install-SITE (`sym`), not per effect-name

The oracle C has no bug because emit_c emits each `handle` as a stmt-expr
`({ KaiEvidence _node; jmp_buf _jmp; ...; push_with_jmp(&_node, ...); body;
pop(); })` — each install-site owns a distinct LEXICAL `_node`. The `({})` IS
the per-site scope.

asu's call (Option B over a compile-time emission stack): the KIR `KInstall(eff,
sym, ...)` already carries `sym` (`_kai_handler_<line>_<col>`, unique per site).
Make the two nodes that named the effect but not the site carry `sym` too, then
key the frame allocas off `sym`:

- `KSetjmp(String)` → `KSetjmp(String, String)` (eff, sym)
- `KPop(String, KPos)` → `KPop(String, String, KPos)` (eff, sym, pos)
- `KBindEvState(String, KPos)` → `KBindEvState(String, String, KPos)` (the
  stateful return clause's Ev read — same per-site need)
- `lower_handle` threads `sym` through `lower_handle_fork` / `lower_handle_body`
  / `lower_handle_discard` / `lower_handle_ret`; `__kai_discard` becomes
  `__kai_discard_<sym>` (a per-site landing slot — nested handles each own one)
- `nfx_*_reg` derive from `sym`; `nemit_install` / `nemit_setjmp_op` resolve
  `__node_<sym>` / `__jmpbuf_<sym>` etc.

`alias` storage (`__alias_id__<a>`) was ALREADY per-alias — left intact.
`nemit_perform_node`'s by-name lookup needs no site (resolves the innermost at
runtime); the by-id lookup just needed the chain to stop cycling. So the fix
reduces to "each PUSH gets its own node storage"; the setjmp/pop/bind-state read
their own site only to pick the right jmp_buf / Ev.

### Why not the emission stack (the alternative considered)

A push-on-install / top-on-setjmp/pop / pop-on-pop stack in the emitter would
also work, but it re-derives the nesting the lowering already guarantees, and the
discard-longjmp-skips-the-pop case makes its balance fragile to reason about.
The `sym` IS the site identity; making it explicit in two constructors is
visible, byte-id-checkable, and has no implicit balance invariant. Same lesson
as KFn-thunk modeling: structural metadata in the node, not a backend
re-derivation.

## Files touched

KIR + lowering (3): `kir.kai` (constructor arities), `kir_lower_walk.kai`
(thread sym, per-site discard), `kir_dump.kai` (printers). Native backend (5):
`emit_native_fx.kai` (reg helpers + reserve), `emit_native_fx2.kai` (install),
`emit_native_ops.kai` (setjmp), `emit_native_term.kai` (pop/bind dispatch),
`emit_native_fn.kai` (rspec match arity). No new files; all edits are
contained, no monolith growth. The oracle (emit_c) was NOT touched — it reads
the AST, not the KIR, so the KIR shape change is invisible to it.

## Fixtures

- `examples/effects/multi_var_closure_capture.kai` (+ `.out.expected`) — the
  minimal regression: two same-effect `var` cells, a `while` body that
  reads/writes BOTH (forcing a 2-alias closure capture), values read outside.
  The body MUST touch both cells — a single-capture variant false-greens.
- The four pre-existing cluster fixtures (`multi_var_state_index`,
  `m7b_15_nested_alias`, `m8_12_self_delegating_handler`, `demos/spiral/main`)
  flip from baseline gap to passing; removed from
  `tools/native-parity-baseline.txt`.

Coverage gap left: a STATEFUL nested same-effect handler where the return clause
reads `state`/`log` from the inner site while an outer site is live —
`KBindEvState` now carries `sym`, so it is correct by construction, and
`m7b_15`/`m8_12` exercise nested aliased state, but no fixture isolates the
return-clause-state read across two live same-effect frames specifically.

## Cost vs estimate

Diagnosis dominated (IR dump + lldb + bisection) and was decisive — once the
self-cycle was seen, the fix was mechanical and the design call (B vs stack) was
a single asu round. The KIR shape change rippled to every `KSetjmp`/`KPop`/
`KBindEvState` consumer (8 sites across 6 files), all caught at compile time.

## Build note (this worktree)

The vendored static LLVM (`stage0/third_party/llvm/build`) the brief assumed did
NOT exist here. Built kaic2 against Homebrew `/opt/homebrew/opt/llvm` with
`export LIBRARY_PATH=/opt/homebrew/lib:/opt/homebrew/opt/zstd/lib:/opt/homebrew/opt/libxml2/lib`
(the `--link-static` system-libs zstd/xml2 are not on the default linker search
path). IR dump:
`KAI_NATIVE_DUMP_IR=/tmp/x.ll kaic2 --emit=native --path $PWD/stdlib -o x.o f.kai`.

## Follow-ups for next lanes

- `m7a_6d_double_resume` stays a baseline gap — multi-shot resume, NOT this
  bug's shape; out of scope.
- The remaining design-bearing native-parity residue (Spawn fiber runtime, Hash
  proto-dispatch, Real-arith RC unboxing edges) is unaffected; see
  `docs/native-parity-gaps.md`.
- selfhost byte-id is FALSE-GREEN for this specific bug (compiler.kai has no
  nested same-effect handlers in its own source). The runtime repro + cluster
  fixtures vs the C-direct oracle are the real gate — keep them wired into
  `test-backend-parity`.
