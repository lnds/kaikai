# Lane experience — #820 L3+L4 (emit_c flip + spawn audit + native flip)

The cross-together integration block of the capability-passing redesign:
both backends flip from the by-name evidence walk to consuming injected
evidence at perform sites. C flips first as the oracle, native against it,
neither reaches `main` until full dual-backend SERIAL parity is 0 gaps.

## Architecture — the packed evidence frame (Koka evidence vector)

The unit of transport is a **packed evidence frame**: one hidden
`KaiEvidence **__evf` parameter per function whose row demands a transported
USER effect, a slot per distinct effect label in canonical row order. The
call site fills each slot per ITS OWN supplier classification; the perform
site reads its slot by index. This is the Koka evidence-vector ABI, not N
individual scalar params.

Two designs were rejected before this one:

- **Individual hidden params** (one scalar per effect). Refuted by the L2
  arity spike (p99 = 5, 503 functions carry a 5-effect builtin row) AND by
  soundness: a flat param list can't express per-call-site slot fill cleanly.
- **Collapse `SupForward`-of-a-builtin to a direct startup-global read**
  (no param). Proven UNSOUND: the forward chain's terminal is non-local — it
  can be a lexical `handle with Mutable` several frames up (`handle runner()
  with Mutable` where the perform is two frames down). The emitter, seeing
  only the callee, cannot know locally whether its terminal is a handle or
  the global; two call sites of the same fn demand different answers for the
  same `SupForward`. The param IS the discrimination mechanism. This failure
  is invisible in the ≤1-handler corpus — the §C false-green in its purest
  form.

## A.3 — the arity ABI decision

The binary criterion (p99 ≤ 3 AND self-compile < 5% AND no bench > 5% →
individual params; else packed tuple) is satisfied by construction toward
the **packed frame**: p99 = 5 > 3 alone forces the tuple, and the frame IS
the tuple. One hidden frame pointer per fn — no N-scalar spilling, no
emitted-C arg explosion. The "individual params" branch never ships.

## The verifiable cut (the load-bearing scope decision)

The ≤1-handler corpus masks exactly ONE failure axis — selection between
live instances (the walk's axis). The frame has THREE axes the corpus does
NOT mask and a frame-only fixture DOES falsify: **delivery** (frame not
threaded to a callee → crash), **addressing** (wrong slot index → empty/
wrong slot), **classification** (call site picks the wrong supplier). A
fourth axis — two live instances through a call — is genuinely L6
(capability-as-parameter surface) and unfalsifiable here.

So the cut: the **perform-site read** is corpus-wide (prefer the frame slot
when present, else the live by-name walk — additive, retreatable). The
**frame construction + threading** lands only where the distinguishing
fixtures exercise it. The 503 builtin-wide signatures are NOT rewritten —
no L3 oracle falsifies them, so they ride the walk and land with L6 when
programs that need them exist. This is a verifiability limit, not confessed
debt. Builtin-default effects (the 18 canonical + Actor/Console/Ffi/Spawn)
stay on the walk; `frame_effect_is_user` draws the line.

## The flagship is L6, not L3

`two_instances_through_call.kai` uses capability-as-parameter (`add(c1:
Cell, c2: Cell, dst: Cell)` + `dst.set`). The `--dump=evidence-obligations`
arbiter labels its params `PENDING-L6`; the typer rejects `dst.set` ("method
`set` not found for type `Cell`"). No non-L6 / non-typer form yields two
distinct obligations through a call — distinguishing two as-bound instances
through a call inherently needs naming the non-innermost instance (L6
surface or an `infer.kai` alias-survives-call change, both out of L3 lane).
It stays quarantined; L3's positive oracle is two purpose-built fixtures:

- `evidence_two_level_forward.kai` → 16: evidence forwarded two call levels
  (`deep`→`mid`→handle in `main`); each handle threads its own node.
- `evidence_handler_below_lexical.kai` → handled: a perform two frames below
  the lexical `handle with Log`; the user handler must run, not the default.
  The separator a collapse-to-global transport would fail.

## Root causes found during the C threading

The frame param landed on signatures first, then call sites — finding every
emission of `kai_<csym>(...)` for an EFn callee (the direct boxed path, the
raw UFn path, the pipe paths, the thunk entry) and funnelling them through
`emit_user_call`. Two non-obvious breaks surfaced:

1. **`emit_lam_helpers` built its EmitCtx with `evf: []`.** A handle body
   reifies through the lambda-helper path, so every user-fn call site inside
   a handle (the `let a = worker_a()` shape) lost the frame table and emitted
   the bare call against a frame signature. 12 of 16 breaking fixtures.
   Lesson: threading a table through EmitCtx means EVERY EmitCtx constructor,
   including the lambda/clause emitters that look detached from the main body.

2. **`main` must be frame-exempt.** It is the transport root — no caller
   fills its frame, and its row effects are satisfied by the startup defaults
   the main wrapper installs (found via the walk). A `main` with a user-effect
   default (MyConsole / NetDns / NetUdp) gained a frame param while the main
   wrapper called `kai_main()` bare. Exempting `main` in
   `build_evidence_frames` fixed the last 4.

After both fixes: 202 effects+actors fixtures compile on `--backend=c` with
zero frame-arity errors; both distinguishing fixtures green on the frame
path.

## Spawn / actor audit

The §3 behavioural change — capabilities do not cross `spawn`, the evidence
clone was their UAF — required auditing every fixture that performs a user
effect inside a fiber against an outside handler. Finding: **the corpus has
none.** Every spawn/fiber/actor fixture performs only BUILTIN effects
(`Actor`/`Spawn`/`Console`), which stay on the walk + the live clone
(`runtime.h:10649`, deleted in L5). The frame touches only user effects, so
no user-effect crosses a fiber today and there is nothing to migrate. The 8
spawn/actor fixtures that "fail" are negative-by-design (escape errors,
brand mismatches, the intentional stack-overflow Bus error) with
`.err.expected` goldens — unchanged by the flip. The A.4 actor tripwire
(`dual_actor_request_reply`, parent performs an actor op + child actor)
passes: the actor dispatch tolerates the flip.

## C-backend result

Dual SERIAL parity (`BACKEND_PARITY_JOBS=1`) with the C frame flip + native
on the by-name walk: **pass=514, fail=0 over 577 fixtures.** The corpus is
≤1-handler throughout, so C-frame and native-walk are observationally
equivalent (the accidental equivalence the redesign exists to retire) — the
distinguishing fixtures give 16 / handled / trace correct on BOTH backends.

## Native flip — done

The native flip mirrors the C cut: frame read at `nemit_perform_node`, frame
construction where the fixtures exercise it, `main` frame-exempt. Without it,
C-frame/native-walk is the §7 bridge — green only by accident-of-≤1-handler.
With both backends on the frame, dual parity becomes a REAL differential
oracle: it compares two INDEPENDENT frame constructions (C-text ABI vs
native-LLVM calling convention), not the frame vs the walk, so any ABI /
slot-order / construction asymmetry is a parity red. That is the oracle the
bridge state could never give.

The native ABI table lives in `KaiNativeCtx` (`KaiNFrame { sym; slots;
nslots }`) with `add_frame_slot` / `frame_slot_count` / `frame_slot_index` /
`frame_slot_eff` prims. The bootstrap gotcha: a new native prim must be
registered in BOTH `native_prims.kai` AND `stage1/compiler.kai` — kaic1
resolves the bundle against its OWN rprelude list, so a stage2-only prim
fails the bundle with "undefined name". The flip then is: the
`nfn_llvm_type` leading-`ptr` frame param; the param-index shift +
`__evf`-register seed of LLVM param 0; the `__evf` register reserved in the
entry head; and the call-site frame construction (`alloca [N x ptr]` +
per-slot `kaix_evidence_lookup_node` + `array_gep` store, prepended in both
the raw and boxed call funnels).

The one real bug: the frame array was first alloca'd via
`build_alloca_entry`, which repositions the builder into the current fn's
entry block. On a forward-chain call (a frame fn calling another frame fn)
the entry block is mid-construction when the call site builds the callee
frame, so `GetFirstInstruction` on it faulted (`EXC_BAD_ACCESS` at a small
offset — lldb pointed straight at `kai_llvm_build_alloca_entry`). A plain
`build_alloca` plants the array where the builder already sits, which is
valid — the frame array does not need promotion. After that one-line fix:
181 effects+actors fixtures compile native with zero crashes/verify-fails,
the distinguishing fixtures give 16 / handled under native matching C.

## A.3 — measured

The frame IS the packed tuple, so the binary criterion resolves to it by
construction (p99=5>3 mandates the tuple; the frame is one hidden pointer
per fn, no N-scalar spilling). No separate individual-params build was
needed — the soundness need for per-call-site slot fill already rules out a
flat param list. Self-compile + benches unaffected (the compiler bundle
carries no user-effect frame; its effects are builtins on the walk).
