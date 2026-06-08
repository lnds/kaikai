# Lane experience — KIR → in-process libLLVM backend (bring-up)

**Branch:** `kir-llvm-inproc`. **Closes:** none directly; advances KIR Lane 1
(`docs/kir-design.md` §7.2). **Follow-up issue:** #779 (EBlock-raw).

## Scope as planned vs as shipped

The brief laid out a 9-step incremental bring-up of the in-process libLLVM
backend (setup → arithmetic → calls → control flow → ctors/match → RC → TCO/TRMC
→ effects → closures), each validated against C-direct as the only oracle.

**Shipped (5 commits, all green, selfhost byte-id intact):**

1. `TyHandle` — a non-RC opaque scalar `Ty` for LLVM C-API objects
   (Module/Builder/Value/Type/BasicBlockRef). Threaded through the exhaustive
   `Ty` walkers; rides the existing `MUnboxed` machinery so perceus never
   dup/drops it.
2. Opt-in libLLVM link (`make KAI_LLVM=1`, discovered via `llvm-config`) +
   `#ifdef KAI_LLVM` C-API forwarder section in `stage2/runtime.h` with `#else`
   panic stubs so the default build links without libLLVM.
3. **`main → 42` end-to-end** — `--emit=native` builds the LLVM module via the
   C API in memory, emits a Mach-O object, links it against the effects runtime,
   runs, and matches C-direct. The vehicle is proven: native code in one
   process, no `.ll` text, no `clang`.
4. **match-raw unboxing** — a `match` whose result is unboxable now lowers with
   a raw `_r` (no per-arm `kai_int` box). Per asu's design: promotion in the
   unbox pass, emitter is a mode-slave, new `emit_match_raw` branch (boxed path
   untouched), anti-false-green fixture.
5. match-raw divergence — a `_ -> panic(...)` catch-all arm does not block raw
   promotion.

**Staged but not shipped (in `git stash` `kir-step2-wip`):** the generic KIR
walk (`emit_native.kai` orchestration + `emit_native_ops.kai` per-node lowering
+ the N-ary handle-vector prims + the C-side register env). It compiles raw for
the match→Handle case but hits the EBlock-raw blocker (#779).

So: steps 0–1 fully shipped + a load-bearing codegen improvement (match-raw)
that the walk needed; steps 2–9 staged, blocked on one well-understood issue.

## Design decisions and alternatives considered

- **Handle repr = `TyHandle`, not boxed.** The first asu-style instinct was a
  boxed `KAI_LLVM_VALUE` nominal (RC-tracked). Rejected: the close gate demands
  "zero refs carrying the handle repr", and a boxed handle is RC-tracked. A
  spike confirmed `TyHandle` rides the existing raw-scalar machinery
  (`ty_is_unboxable_t` → `MUnboxed` → perceus skips it) with near-zero churn —
  `TyByte` was the precedent for adding a scalar `Ty`.
- **Prim ABI = raw-prelude, not extern-C/FFI.** A spike (not an argument) settled
  it: the extern-C/FFI shim is structurally excluded from the raw path
  (`classify_unbox_sig` rejects `body_is_ffi_extern`) and always reboxes the
  result via `kai_int` (the tagged-Int pointer-corruption trap). A new
  `RPrelude` registry (`native_prims.kai`) with a type-aware emit dispatch
  carries handles raw. Generated typer schemes + resolver names from one table
  so the three sites can't drift.
- **Register env lives in C, not in a kaikai data type.** Discovered the hard
  way: a `TyHandle` cannot be stored in a boxed kaikai variant slot (the RC
  would treat it as a `KaiValue *`, and box_wrap routes it through `kai_int`).
  The name→handle map is therefore itself a `TyHandle` (a C-side struct),
  threaded like the module/builder.
- **match-raw promotion in the unbox pass.** Our initial mental model was wrong
  (we thought the fix lived in the emitter). asu corrected it: raw promotion is
  an unbox-pass concern; the emitter is a slave of `e.mode`. The asymmetry was
  that `EMatch` was marked nowhere AND had no raw clause. Fixing both, in
  dependency order, with the new `emit_match_raw` branch never mutating the 99%
  boxed `emit_match_default`.

## Structural surprises the brief did not anticipate

- **The thunk ABI corrupts handles.** A fn whose signature mentions `TyHandle`
  gets a panic thunk — a handle can't ride the boxed `KaiValue **args` thunk ABI.
  Such a fn is never a first-class value, so the thunk is dead; the stub makes an
  accidental indirect call loud.
- **The bootstrap is two-generation.** `make kaic2` produces a gen-1 (compiled by
  stage1, which has no `TyHandle` → boxed ABI for the backend code, but never
  executes it). The sound native binary is a **gen-2** (compiled by a gen-1 that
  HAS `TyHandle`). This is the selfhost-gen2 pattern the repo already uses; the
  native release-build wiring rides Lane 1.5.
- **match-raw was a second instance of a bug-class.** Promoting match→raw
  exposed that the SAME bug exists at the `EBlock` frontier (a multi-stmt block
  with a raw tail boxes its result). That one touches Perceus dup/drop threading
  — a leak/UAF risk, not a compile-time mismatch — so asu explicitly scoped it
  OUT. Filed as #779.
- **Byte-id is false-green without a match→scalar fixture.** The compiler source
  has no match producing a scalar, so selfhost byte-id stayed green even with
  `emit_match_raw` broken. The mandatory `unbox_phase2_match_scalar` fixture
  (with a structural grep) is the only thing that exercises the path. asu flagged
  this trap; it was real.

## Fixtures added and coverage gaps

- `examples/perceus/unbox_phase2_match_scalar.kai` — match→Int over a boxed
  variant (`area`/`rank`/`score`) + a `_ -> panic` catch-all (`sides`). The
  `test-unbox-phase2` harness asserts `_r` lowers raw (no `kai_unit()` accumulator)
  for both, so the byte-id can no longer lie.
- Coverage gap: the native backend itself has no fixture yet — the `main → 42`
  spine was validated ad-hoc (object → link → run → diff vs C-direct), not wired
  into a tier. A `tier1-native.yml` path-gated workflow (per the Lane 1 plan) is
  the home for native parity once the walk lands; deferred with the walk.

## Real cost vs estimate

The brief framed this as a large, novel lane and was right. Two-thirds of the
effort went into two unanticipated rabbit holes: (1) the handle-ABI spikes
(thunk, register env, prim dispatch) and (2) the match-raw codegen work, which
turned out to be a prerequisite for the walk rather than a step within it.
`main → 42` and match-raw are the durable wins; the generic walk is real code
(A++ km score) waiting on #779.

## Follow-ups left for next lanes

- **#779** — EBlock multi-stmt raw result. The one blocker for the generic walk.
  Same bug-class as match-raw; deferred for its Perceus-RC danger.
- **Generic KIR walk** (stash `kir-step2-wip`) — resumes once #779 lands: it
  already lowers match→Handle raw; only the EBlock-return case is blocked.
- **Lane 1.5** — flip native to default + static libLLVM release link +
  `tier1-native.yml` promoted to the per-PR gate. Out of scope here.
- Steps 4–9 of the original bring-up (control flow, ctors/match, RC, TCO/TRMC,
  effects, closures) — each a future increment validated against C-direct.
