# Lane experience — KIR Lane 1.2 Parte A (native spine reproducible)

Branch `kir-native-fix`. This retro covers **Parte A** of Lane 1.2
(docs/kir-design.md §"Lane 1.2"): make the in-process libLLVM native
backend's `main -> 42` spine actually run, reproducibly, and close the
CI hole that let a broken opt-in backend ship green in PR #780. Parte B
(the generic KIR walk + #779 EBlock-raw) is explicitly NOT in this PR.

## Scope as planned vs as shipped

Planned (brief): reproduce the `--emit=native` panic; diagnose the exact
prim that falls to the generic dispatch; fix so `main -> 42` emits, links,
runs; add a native fixture + a path-gated `tier1-native.yml`.

Shipped: exactly that, plus the discovery that the break had **two**
coupled causes (dispatch AND Perceus RC on handles), and a correction to
the brief's expected observable (see "Surprises").

## The diagnosis (confirmed, not assumed)

The brief's hypothesis was directionally right but incomplete. Verified
chain of facts:

1. `kaic2 --emit=native` on `fn main() : Int = 42` panicked with
   "attempted to call a non-callable value" and produced no object.
   `--emit=kir` on the same file worked → the KIR lowering is sound; the
   fault is in the native prim CALLS.
2. **The kaic2 of PRODUCTION is compiled by kaic1 (stage1), not stage2.**
   `build/stage2.c` is emitted by `../stage1/kaic1 build/bundle.kai`.
   stage2 (the oracle) has `TyHandle` + `rprelude_find` dispatch +
   `MUnboxed` perceus-skip and is correct — but that code lives in
   `emit_native.kai` and is *compiled by kaic1*, which is type-blind.
3. **Bug 1 (dispatch).** The `llvm_*` prim names were registered only in
   stage1's `prelude_names()` (resolver env, so they are not "undefined")
   but never in `prelude_table()` (the EPrelude cname+arity table). So
   `prelude_find` → None, `efn_find` → None, and they fell to the tail
   fallback `kai_apply(kai_llvm_xxx, …)`. At runtime `kai_apply` looks for
   a callable closure; the forwarder is a raw C fn, not a `KaiValue` →
   panic. The stage1 comment "Stage 1 emits each as a plain `kai_<name>`
   call (its non-EP fallback)" was simply false: the non-EP fallback is
   `kai_apply`, not `kai_name(args)`.
4. **Bug 2 (RC on handles).** stage1's Perceus is type-blind and wraps
   every non-last in-scope read in `__perceus_dup`, so it emitted
   `kai_internal_dup(kai_m)` on a binding holding a raw `LLVMModuleRef`.
   `kai_incref` reads `v->rc` (deref of the LLVM pointer as if it were a
   `KaiValue`) and does `v->rc++` → memory corruption. stage2 avoids this
   via `rhs.mode == MUnboxed` + `ty_is_unboxable_t(TyHandle) = true`;
   stage1 has neither modes nor types.

Both had to be fixed together. Bug 1 alone would still corrupt via Bug 2;
Bug 2 is latent on the happy path but live on any error path.

## The fix

A stage1 mirror of stage2's `TyHandle` mechanism, recovered
**syntactically** because stage1 has no types:

- A `rprelude_table()` in stage1 hard-coding each prim's C forwarder +
  per-arg marshalling kind (`RArg = AHandle | AInt | ABoxed`) + result
  shape (`RRet = RHandle | RInt`). The signatures are known statically
  (they mirror the `#ifdef KAI_LLVM` forwarders in stage2/runtime.h), so
  the table encodes what stage2 derives from `ty_is_unboxable_t`.
- `emit_rprelude_call` marshals each arg: an `AInt` is unboxed with
  `kai_intf` (stage1 renders an Int boxed), a handle/String passes
  through. An `RHandle` result is consumed raw; an `RInt` result (only
  `emit_object`) is boxed with `kai_int`.
- Perceus exclusion: a `: Handle` param and a `let h = <handle prim>(…)`
  never enter the Perceus scope, so they are never dup'd/dropped. The
  param exclusion is in `perceus_decl`; the let exclusion is at the
  `SLet` site in BOTH `pcs_block_loop` (collect) and `pcs_rewrite_block`
  (rewrite) — they must agree or the dup decision and the scope diverge.

Generated C is now exactly the intended shape:
`kai_llvm_module_new(kai_str("kai_native"))`,
`kai_llvm_const_int(kai_i64_t, kai_intf(kai_int(42LL)))`,
`kai_int(kai_llvm_emit_object(kai_m, kai_out_path))`.

## Design decisions and alternatives considered

- **Why a stage1 table, not "selfhost-compile the native kaic2".** The
  alternative — build the `--emit=native` kaic2 via `make selfhost`
  (kaic2 compiling itself with correct `TyHandle`) instead of the
  bootstrap kaic1 — would break the kaic0→kaic1→kaic2 model and mix two
  binaries. The native prims must reach kaic1 because kaic1 is the
  compiler that builds the production kaic2. The table is the same shape
  as the existing `prelude_table` next to it; minimal and contained.
- **Why box `emit_object`'s result at the call site.** stage2 boxes the
  raw `int64_t` at `build_native_object`'s return boundary
  (`ufn_call_result`). stage1 has no types, so it cannot box at the
  boundary — and the result crosses into a USER fn whose `let rc = …` IS
  in Perceus scope. A raw `1` parked in a `KaiValue *` would crash
  `kai_decref` (deref of 0x1). Boxing at the prim call site
  (`kai_int(…)`) makes it a real RC-free singleton Int. Stage1 equivalent
  of stage2's boundary box, pushed to the call site.

## Structural surprises the brief did not anticipate

- **The expected observable was wrong.** The brief said `main -> 42`
  should "give exit 42". It does NOT — and neither does the C-direct
  oracle: kaikai's `int main` runs `kai_main()` for its effects and
  returns 0; the `42` return value is not observable as an exit code
  (only an explicit `exit(n)` is). So the right parity is "native exit
  code + stdout == C-direct", both 0 here, which is what the harness
  checks. A fixture that "returns 42" is a weak observable (both
  backends 0); as the walk grows, fixtures move to `print`/`exit` for a
  real behavioural diff. Flagged because "exit 42" would have sent a
  reviewer hunting for a non-existent bug.
- **kaic0 rest-pattern grammar.** `[k, ..._]` (rest bound to `_`) is
  rejected by stage0's parser; it wants an identifier after `...`. Use
  `[k, ...__]` (double underscore) or a named tail. Also: inline
  multi-arm matches on a list pattern (`match xs { [h,...t] -> a [] -> b}`
  on one line) trip kaic0 — split to multiline. Both are stage0 quirks,
  not language rules.

## Fixtures added + coverage

- `examples/native/main_returns_int.kai` — the spine program.
- `tools/test-native-parity.sh` — per-fixture emit→link→run, diffs
  exit code + stdout vs C-direct (the oracle). Requires libLLVM
  (SKIPs with success when no `llvm-config`, so the C path is unaffected).
- `.github/workflows/tier1-native.yml` — path-gated (native backend,
  prims, runtime forwarders, KIR lowering, Makefile), mirrors
  tier1-asan / tier1-backend-parity. This is the hole PR #780 left open:
  an opt-in backend is invisible to tier1 unless a workflow exercises it.

Coverage gap (intentional, for Parte B): only one fixture, because the
native backend only emits the `main -> 42` spine. The generic walk lands
in Parte B and brings real behavioural fixtures with it.

## Verification

- `make -C stage2 selfhost` green (kaic2b.c == kaic2c.c) — the C/default
  path is byte-identical, untouched.
- `make tier0` green — selfhost + 35 demos + arena gate (incl. ASAN).
- Native binary under `-fsanitize=address,undefined` exits 0 with no
  report — proves handles never enter the RC regime (no deref of an LLVM
  pointer as a KaiValue, no UAF).
- `tools/test-native-parity.sh` green from a freshly rebuilt
  kaic0→kaic1→kaic2(KAI_LLVM=1) chain — reproducible, not hand-run.

## Real cost

The diagnosis was the bulk of the work (confirming kaic1-not-kaic2 is the
compiler, then finding both bugs). The fix is ~90 LOC in stage1, all in
small low-complexity functions (km cogcom: +6 fns, avg 5.5→5.6, max
unchanged at 107 — the max is a pre-existing parser fn, not new code).
stage1/compiler.kai is a pre-existing F-- monolith; the differential bar
applies to new files, and the edit does not raise its grade obligation
nor lower it.

## Follow-ups left for Parte B (next, same lane)

- The Perceus exclusion recovers handle-ness from SYNTAX (param `:Handle`
  or direct prim `let`), NOT types. A handle flowing through an
  unannotated intermediate `let h = some_user_fn()` (user fn returning
  `Handle`) would be MISSED and corrupt. The current spine has no such
  case (every handle is a `:Handle` param or a direct prim let), so this
  is sound by construction HERE. Parte B's generic walk MUST extend the
  exclusion to track user fns returning `Handle` before introducing any
  such flow. Marked in the stage1 `perceus_decl` comment as the dangerous
  edge.
- #779 (EBlock-multi-stmt-returns-raw) — the dangerous Perceus-boxing
  change at the EBlock frontier. Parte B, with its own fixture
  exercising EBlock→scalar + KAI_TRACE_RC balanced + ASAN as the gate
  (RC is the failure mode, not compilation).
- `bin/kai` does not yet know a `native` backend (only `c|llvm`). Wiring
  `--backend=native` / `KAI_BACKEND=native` into the driver wrapper is
  Lane 1.5 (the flip), not this lane.
