# Lane experience — issue #1053 (native variant alloc gap)

## Scope as planned vs as shipped

The brief ordered P1 from the issue: route variant construction/projection
through the typed-slot path (`kaix_variant_masked` / `kaix_variant_arg_i64`),
on the diagnosis that native boxes every primitive variant field. **That
diagnosis was wrong**, and a previous session of this same lane had already
measured why (per-tag `KAI_TRACE_RC`, N=1M rb-tree):

| metric | C | native (baseline) |
|---|---|---|
| variant allocs | 6.3M | 26.2M |
| **int allocs** | **0** | **0** |
| reuse_in_place | 6.3M (100%) | 1M (4%) |

Int fields already ride tagged immediates in both backends; the 147x
`kaix_int` symbols the issue counted are cold static call-sites, not executed
allocs. The whole 4.17x is **reuse-in-place**: native reused 4% of nodes
where C reuses 100%. The typed-slot WIP built on the wrong diagnosis had
already been measured to make allocs worse (26M -> 36M) and was discarded.

Shipped instead: the dominant half of the reuse gap. The native TRMC variant
spine step (`ntrmc_variant_step`) captured the arm-top reuse token, then
**freed it** (`kaix_reuse_free`) and fresh-alloc'd every spine cell through
`kaix_variant` — free+malloc per descent level where the C oracle rebuilds
into the donated shell via `kai_variant_at`. The fix consumes the token in
the step (`kaix_variant_at_argv`).

Measured on rb-tree 1M inserts (native backend):

- `alloc_total` 26.26M -> **12.26M** (4.17x -> **1.95x** vs C backend).
- wall 1.51s -> ~1.03s (harness median-of-5); RSS unchanged.
- `leaked` 17 -> 19-ish constant residual, same as C's 20. RC balanced.

## The stage1 miscompile that ate half the lane

The obvious implementation — a new 5-arg emitter call
`nemit_variant_at_call(ctx, token, tag, cname, argbuf)` — produced LLVM
CallInsts with a corrupt opcode (`<Invalid operator>` /
"User-defined operators should not live outside of a pass!"), with all six
operands perfectly formed. Root cause chain:

1. `make kaic2` compiles the bundle with **kaic1** (stage1).
2. kaic1's RC pass, perturbed by the extra binder/argument flowing through
   `ntrmc_variant_step`, wrapped the node handle in `kai_internal_dup(...)`
   at both of its uses — confirmed by diffing the generated `build/stage2.c`
   against main's (main passes `kai_node` bare; the new shape wraps it).
3. `kai_internal_dup` is `kai_incref`, and LLVM handles are raw
   `LLVMValueRef`s casted across the boxed calling convention — the incref
   **writes a refcount into the middle of LLVM's CallInst**, corrupting its
   opcode. Deterministic, and invisible until module verify.

Even a byte-identical inline body (no shared forwarder) miscompiled; a
constant-null token argument miscompiled; the original 4-arg call from the
same site compiled fine. The trigger is perturbing binder uses in that
block, not any specific construct. Workaround shipped: keep the step's call
shape byte-compatible with the original 4-arg `kaix_variant` form and carry
the token as `args[0]` of the slot buffer (`kaix_variant_at_argv` unwraps
it). Diff on the emitter stays ~20 lines; zero new call shapes for kaic1 to
miscompile. The kaic1 bug itself is out-of-lane and gets its own issue
(repro: the A/B experiment above, `git show` of the two generated bodies).

Lesson reinforced (already in the kaic1-traps folklore): when touching
bundle code, *mimic the exact shape of an existing working site*; any new
shape must be validated by running a real fixture, not by a clean build.
New lesson: RC-managed dup/drop over raw foreign handles means a stage1 RC
mis-inference silently corrupts *host-process* memory — the failure surfaces
arbitrarily far from the cause (here: inside libLLVM's instruction stream).

## Residual and the follow-up design (why this PR does not close #1053)

The remaining 1.95x is the **nested (non-bijective) rotation/rewrap reuse**:
`balance_left/right` arms rebuild through `lower_reuse_con`'s nested path,
which fresh-allocs all rebuilt cells and relies on the match-exit cascade,
where the C oracle does Koka's 2-of-3 token reuse (outer `_scr` overwrite +
`_ru_inner` donation, emit_c.kai). The dual-branch design was mapped during
this lane and needs, mirroring `lower_reuse_dual` (the cons fork):

- a unique/shared fork with the rebuild args lowered per-branch;
- `kaix_reuse_steal_slot(outer_token, nslot, n_inner)` to capture the inner
  cell's shell once the outer is stolen (runtime one-liner over
  `kai_drop_reuse_token`);
- per-branch multiset accounting: unique = (uses-1) dups per embedded binder
  plus old-slot drops for non-embedded slots (incl. binderless `_` slots via
  borrow projection); shared = today's unconditional leaf dups + fresh;
- pattern info (`nslot`, inner arity, grandchild binders) travelling
  arm -> rebuild site through `LowerSt` (one new field, `mscr`-style
  save/restore).

That subsystem produced the #1054/#1069/#1074 UAF family this same week;
it deserves its own lane with the full ASAN cycle, not a tail-end rush.
The issue's own plan said "assess after P1"; the assessment is: dominant
half shipped, second half designed, gate still 1.95x from ~1.0x.

## Fixtures

- `examples/perceus/trmc_variant_spine_reuse_1053.kai` (+ `.out.expected`):
  1000-node variant spine rebuilt 1000 times; golden checksum on both
  backends and `alloc_total < 100000` under `KAI_TRACE_RC` (measures 1004
  with the reuse; ~1M+ without it). Wired as
  `test-perceus-1053-native-trmc-spine-reuse` into tier1-native shard 2,
  same pattern as the #872/#995 gates.
- Existing reuse gates stayed green locally: the perceus example corpus
  (80/84; the 4 diffs are dump-flag fixtures my ad-hoc runner invoked
  wrong plus the bench's unpinned `elapsed` line), tier0 with selfhost
  byte-identity, `nested_pattern_reuse_balance`, #872/#995/#1025/#1069.

## Cost vs estimate

The brief said "RAPIDO — the diagnosis is done, just implement". Reality:
the diagnosis was wrong (already known to this lane's memory, re-verified
on current main in ~15 minutes), the correct fix was a ~20-line emitter
change, and >half the wall-clock went to the stage1 miscompile forensics.
The measurement discipline paid for itself: one `KAI_TRACE_RC` per-tag run
refuted the issue's machine-level narrative before any code was written.

## Follow-ups

- Nested 2-of-3 rotation reuse in the native lowering (design above) — the
  remaining half of #1053.
- kaic1 RC pass wraps raw handles in dup/drop under binder-use perturbation
  (new issue, out of lane).
- `docs/native-codegen-perf-plan.md` §3.3 still conflates scalar re-box
  with variant-field boxing; the per-tag numbers here supersede it.
