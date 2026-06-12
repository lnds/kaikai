# Lane experience — native-parity burn-down 5

**Scope:** fifth burn-down lane on the in-process libLLVM native backend's
parity with the C-direct oracle (KIR Lane 1.5). From a clean `main` —
burn-down 4 never merged (it closed with a stale-golden KIR regression; its
diagnosis lives on in `docs/native-parity-gaps.md`). Target: chip the
`tools/native-parity-baseline.txt` ratchet down toward zero, the antechamber
to the native-default flip (a separate lane).

**Outcome:** closed 9 cross-platform (verified byte-id vs the C-direct oracle).
The ratchet went 82 → 81 NET, not 82 → 73: rebasing onto `origin/main`
(which advanced 7 commits during the lane) brought 8 NEW corpus-growth gaps —
weather/vs demos, FFI extern-C fixtures, a perceus leak audit — that diverge
under native for PRE-EXISTING causes OUTSIDE this lane's zone (`KPerform: no
handler for effect Spawn`, unresolved extern-C symbols, pipe-lowering output
mismatch). None touch the namespace / bit-op / `KBindEvState` fixes; they are
listed as gaps (exactly as #801/#810 corpus growth was absorbed in burn-down 2),
NOT regressions. So: 9 closed − 8 corpus-growth added = 1 net ratchet drop.
THREE root causes, each a clean single cause that the brief's "family"
framing split correctly:

1. **KIR temp-register / surface-binder namespace collision** (Call-param-mismatch,
   6 fixtures → 5 closed, jwt reclassified) — the largest.
2. **bit-op prelude shims missing in the native runtime** (missing-symbols /
   bit-op intrinsics, 3 fixtures → 2 closed, crypto reclassified).
3. **stateful return-clause `state`/`log` never bound** (unbound-register
   writer-state slice, 2 fixtures → 2 closed).

No flip — that stays a separate lane with its own gates. No new files (all
edits are to pre-existing KIR-lowering / runtime / dump modules, additive).

## Scope as planned vs as shipped

**Planned (brief):** work the families in causal order — Call-param-mismatch
(6), missing-symbols (3), bit-op intrinsics, nested-variant payload-bearing,
EBang, pipe-lowering, no-effect-handler (11), clause-param-origin (7), then the
value-divergent residue. Stop at the first change that needs DESIGN, never cut
mid-family.

**Shipped:** the three causes above, all single-cause-clean with a clear
oracle. Stopped exactly where the brief said to: the next families in order
(nested-variant payload-bearing, pipe-lowering, no-effect-handler for
Spawn/Clock/File, clause-param-origin alias-dispatch) are all DESIGN-bearing —
they restructure the match decision tree, the pipe desugaring (reverted twice
in prior lanes), or need a real scheduler/syscall runtime. The writer-state
`log` slice WAS design-adjacent (it needed a new `KStmt`), but the design was
small, bounded, and asu-reviewed in one consult, so it earned its slot. A
mid-size green PR > one eternal PR.

## The three root causes and fixes

### 1. KIR temp-register namespace collision (Call-param-mismatch)

The native verify failed with `Call parameter type does not match function
signature!`. The exact offending IR (via `KAI_NATIVE_DUMP_IR`):

```
store ptr %268, ptr %t2.addr, align 8     ; a boxed value stored as ptr
%271 = load i32, ptr %t2.addr, align 4    ; loaded as i32 — slot mismatch
%275 = call ptr @fx__convert(i32 %271, ...) ; all-boxed callee wants ptr
```

`ls_fresh_reg` (kir_lower.kai) acuña temporaries `t0`, `t1`, `t2`, … in a flat
namespace. `bind_pattern_fields` (kir_lower_bind.kai) emits a SURFACE binder
verbatim as its register (`PBind(name) -> KLet(name, …)`). When a user / stdlib
binder is named `t2` — `stdlib/collections/map.kai:128`'s
`TNode(xk, xv, _, t1, t2)`, or fx_basic literally naming its variables
`t0`/`t1`/`t2` — it ALIASES a `t2` temporary: same string-keyed alloca, the
second store clobbers the first. The register-collection pass
(`collect_fn_specs`, emit_native_fn.kai) recorded `t2` with the FIRST slot seen
(`SInt32` from a `tagof`), so the alloca was `i32`; the boxed `store ptr` + the
`load i32` for the call produced the verify failure. The C-direct oracle never
collides — emit_c emits nested C expressions, never named ANF registers, so its
surface and temporary namespaces can't overlap.

asu's review found this is TWO separable bugs:

- **Bug A (root): namespace aliasing.** Fix: `ls_fresh_reg` now acuña `.t<rc>`.
  The leading `.` is rejected by the lexer in a surface identifier, so a `.tN`
  temporary can NEVER collide with a surface name — an impossibility, not a
  heuristic (`__t` would only lower the odds; a user CAN write `__t2`). LLVM
  accepts `.` in a local value name (the walk already appends `.addr`).
- **Bug B (soundness floor): `rspec_add` took the first slot on a conflict.**
  Even without Bug A, a name legitimately reappearing with two slots (`KLet
  n: i32` then a boxed `KStore n`) under-sized the alloca, truncating an 8-byte
  `ptr` store into an `i32`. Fix: `rspec_add` now WIDENS to `SBoxed` on a slot
  conflict (the universal slot every value fits), so the alloca is `ptr`-wide.

Both shipped. A pure widen-to-box (without the namespace split) was rejected as
the primary fix: it makes the verify pass but the two `t2`s remain the same
alloca, so the stores still clobber — a false-green that swaps an LLVM-caught
crash for a silent wrong value. The namespace split fixes the aliasing by
construction; widen-on-conflict is the defensive net for any other path.

This is a `--emit=kir` dump change — every `tN` temporary became `.tN` — so all
7 `examples/kir/*.kir.expected` goldens were regenerated and the `.kir.fns`
filter re-verified to still trim to user-fns (see the burn-down-4 lesson below).

### 2. bit-op prelude shims (missing-symbols)

`bits_basic` failed to link: `Undefined symbols: _kaix_prelude_bit_and`
(and the other 11 bit ops). The `bit_*` names are in `prelude_names()`
(resolve.kai), so `ncall_sym` (emit_native_ops.kai) routes them to
`kaix_prelude_bit_<op>` — but the native runtime never defined those shims. The
C-direct oracle never needs them: emit_c lowers each `bit_*` call INLINE to the
matching C operator (`kai_int(kai_intf(_a) & kai_intf(_b))` &c.), so there is no
`kai_prelude_bit_*` static body to forward to.

Fix (additive, runtime zone): 12 `kaix_prelude_bit_*` shims in
`stage0/runtime_llvm.c` reproducing the oracle's operator + refcount shape
EXACTLY — read both KaiInts (`kai_intf`), apply the operator, box the result,
decref each input once. Slot/repr identical to the oracle keeps native
byte-identical. Two-runtime trap honored: the shims compile against
`stage2/runtime.h` (tagged Int, `kai_intf`) in the native link path — verified
with `-Werror=implicit-function-declaration` (the Linux-fatal check macOS
hides).

### 3. stateful return-clause `state`/`log` bind (writer-state)

`m7b_11_writer_basic` failed native with `unbound register log`. A stateful
handle's return clause that reads the handler's final state:

```
} with Writer[Int]([]) {
  tell(w, resume) -> resume((), list_append(log, [w]))
  return(x)       -> list_length(log)    # log = handler's final state
}
```

`lower_handle_ret` (kir_lower_walk.kai) lowered `rexpr` inline WITHOUT binding
`state`/`log`. Both oracles bind them in the merge point: emit_c emits
`kai_state = _ev.state; kai_log = _ev.state;`, emit_llvm GEPs the Ev's field 2
(state) → load → push_local `state`/`log`.

Fix (asu-reviewed, the "third option"): a new `KStmt`, `KBindEvState(eff)`,
emitted by `lower_handle_ret` only when the handle is stateful (`init` present).
The native walk translates it to `find_reg(__ev_<eff>)` → `kaix_clause_state_get`
→ store `state` + `log` — extracted into `nemit_bind_ev_state(ctx, ev_addr)`, a
helper SHARED with the clause prologue (`nemit_seed_clause_state`) so the two
paths can never bind differently; the only difference is the address source
(a clause's `self` param vs the handle frame's `__ev_<eff>` alloca).

Why a new `KStmt` and not a `KPrim`/`KVal`: the Ev's useful value is the
ADDRESS of its slot-(-1) alloca, not a load of its content. A `KVal`
(`KVar`/`KProj`) lowers via `nemit_load_reg`, which would load the first slot
as a boxed ptr — the wrong thing. `KBindEvState` is a statement-with-effect
(mutates the register env, produces no value), symmetric to `KInstall`/`KPop`,
keeping the address-of inside the backend where it belongs. The KIR gains an
honest node instead of a `KVal` that says "value" but means "address".

## Structural surprises the brief did not anticipate

- **map_basic fails the SAME verify with NO `tN` surface binder.** It uses
  `m0`..`m6`, yet `load i32, ptr %t2.addr` still appeared — because the collision
  was with a `t2` binder INSIDE `stdlib/collections/map.kai`'s
  `tree_rotate_right`/`balance`, not in the user file. The fix is the same
  namespace split; the surprise was that the trigger lives in stdlib, so it hits
  any program that imports the map.
- **Closing the verify error unmasks a runtime bug (the recurring lesson).**
  `jwt_encoder` and `crypto_hash_basic` now BUILD + RUN but diverge:
  jwt's decode fails (char/hex decode — the burn-down-3 regex/json family),
  crypto SIGSEGVs intermittently (a flaky native memory bug). Reclassified, not
  closed — exactly the burn-down-1/2/3 shape.
- **The Ev address-vs-load distinction is what makes this a `KStmt`, not a
  `KPrim`.** The instinct (a `KPrim("kai_ev_state_get", [KVar(ev)])`) is a
  false-green: `KVar(ev)` loads the Ev's content, not its address. The slot-(-1)
  convention is a backend repr detail the KIR must not leak into a `KVal`.

## The burn-down-4 lesson, honored

Burn-down 4 closed with a stale-golden regression: it touched the KIR dump but
left `examples/kir/*.kir.expected` stale, invisible to tier0 (which does not run
test-kir). This lane touched the dump TWICE (the `.t` prefix and the
`bind-ev-state` line), so BOTH golden-regen passes ran, and after each the
`.kir.fns`-filter trim-to-user-fns was re-verified (no stdlib spill — the exact
burn-down-4 failure mode). `make test-kir` is green; tier1 (which runs it)
gates the merge, not just tier0.

## Fixtures added and coverage

No new positive fixtures: the closed fixtures ARE the regression coverage,
locked by `tools/native-parity-baseline.txt` (9 closed; 82 → 81 NET after the
8 corpus-growth gaps main added). The writer-state slice's
own fixtures (`m7b_11_writer_basic`, `m7b_14_writer_helper`) exercise the
`KBindEvState` path and are gated by the parity ratchet; both verified 3-way
(native == C-direct == llvm-text) and ASAN-clean.

Cross-platform note (burn-down 1/2/3 lesson): `list_helpers` / `list_zip3_scan`
PASS native on macOS now but SIGSEGV on Linux — they STAY listed in the
baseline (the ratchet is a Linux/CI gate; this lane does not touch their root
cause). Closed cross-platform = 9 (5 Call-param + 2 bits + 2 writer-state); the
baseline drops 82 → 81 NET after the 8 corpus-growth gaps main added during the
lane. The macOS-only list_* pair are NOT removed (Linux SIGSEGV).

## RC + soundness validation

- 3-way parity (native == C-direct == llvm-text) byte-id on the 2 writer
  fixtures — a leak/double-free would diverge or crash.
- ASAN clean (`-fsanitize=address`) on the writer + bits + map + fx fixtures —
  the `KBindEvState` state read is a borrow (`kaix_clause_state_get` returns the
  slot without incref), the rexpr consumes it as a borrow, no double-free.
- selfhost byte-id: OK (kaic2b.c == kaic2c.c) — the `.t` prefix + `KBindEvState`
  are deterministic and self-consistent.
- full-corpus parity (native vs C, macOS): pass=362 fail=71, ZERO regressions
  (`comm` against the baseline shows only closes, no new gaps).
- KAI_TRACE_RC remains broken for the incref/decref balance (#812), so RC is
  validated by parity + ASAN, not the tracer.

## Real cost vs estimate

Diagnosis dominated, as in every burn-down. `KAI_NATIVE_DUMP_IR` collapsed the
Call-param-mismatch family to one cause in one dump (the `load i32` /
`store ptr` slot disagreement was visible immediately). Two asu consults paid
for themselves: the first split the namespace bug into A (root) + B (soundness
floor) and rejected the widen-only false-green; the second drew the
`KStmt`-vs-`KVal` line for the Ev state bind before it became debugging. Most
wall-clock went to kaic2 rebuilds (libLLVM link, ~2-3 min each; kir.kai is core,
so the writer fix forced a full bootstrap).

## Follow-ups for burn-down 6

- **nested-variant payload-bearing** (10 of the 12 unbound-register: json_real_*,
  binserialize_derive_nested, perceus reuse_nested_*) — `Some(JReal(r))` needs a
  recursive tag-TEST + sub-bind in the decision tree, not a bind. The
  `var_emit_subtests` machinery (kir_lower_variant.kai) tests nullary/literal
  sub-patterns but cannot BIND under a nested tag test; closing it restructures
  where the test vs the bind runs. DESIGN-bearing.
- **jwt / crypto reclassified** — jwt's decode (char/hex, the regex/json family),
  crypto's intermittent SIGSEGV (flaky native memory bug). Both unmasked by
  this lane's closes.
- **pipe-lowering** (collatz/euler1/fizzbuzz/imc/capture) — `EPipe` → unit; the
  multi-candidate combinator arg shape needs upstream diagnosis (reverted twice).
- **no-effect-handler** (Spawn 7 / Clock 2 / NetTcp 1 / File 1) — Spawn needs a
  real scheduler; Clock/File/NetTcp need the `kaix_default_*` forwarders + a
  syscall runtime. DESIGN-bearing.
- **clause-param-origin** (7, alias-dispatch subset-2b) — `PCapture`/`PEvidence`
  in a clause, the alias-dispatch shape the native walk still rejects loud.
