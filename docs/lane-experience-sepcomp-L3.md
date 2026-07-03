# Lane experience — sep-comp L3: native per-module separate compilation

Closes #989 (LLVM per-module separate compilation) and #963 (separate
compilation umbrella, Phase 3). Third and final lane of the Model A modular
chain, on top of L1 (#1045, one runtime owner) and L2 (#1046, shared `home_tu`
partitioner + `KFn.mo`).

## Scope as planned vs shipped

Planned: partition the whole-program `KProgram` by `KFn.mo` → N LLVM modules →
N content-addressed objects (keyed on the per-module KIR dump) → link with the
runtime owner. Shipped exactly that, plus two mechanism additions the plan did
not enumerate but that the design implied:

- **A third TU role.** The plan spoke of "root vs leaf". Implementation needed
  a *third* role, `NModWhole`, so the single-TU `--emit=native` build keeps
  INTERNAL linkage on its default-evidence globals (byte-identical to the
  pre-modular emitter) while the modular *root* gives them EXTERNAL linkage.
  Collapsing whole-program into "root" would have silently changed the
  single-TU object. The role enum is `NModWhole | NModRoot | NModLeaf`.
- **A raw object emitter.** `--emit=native` (whole-program) merges the runtime
  bitcode into the module and internalises everything-but-`main` so O2 inlines
  the `kaix_*` bodies. That is fatal for modular: it would duplicate the runtime
  in every object AND strip the external linkage a cross-TU call depends on. So
  `llvm_emit_object_raw` skips both steps; modular links the runtime as one
  owner TU (`runtime_llvm.c`) at `cc` time — the L1 runtime-owner model carried
  to native. `kai_llvm_emit_object_impl(m, path, link_runtime)` is the shared
  body; the two public entries differ only by the flag.

## Design decisions

- **The per-module object is a PROJECTION of the whole-program computation.**
  The frontend (parse/infer/perceus/monomorph) runs once. Each partition
  DECLARES every program fn (external forward references, so a cross-TU call
  resolves its callee) and DEFINES only its own. `LLVMAddFunction` defaults to
  ExternalLinkage, so cross-TU calls link with no extra work.
- **Singletons live in exactly one TU (the owner).** proto-init, default
  install/teardown, the default-evidence global definitions, the fn thunks, and
  the synthetic `kai_main` runner driver. A leaf declares the default globals
  external and emits none of the rest. asu confirmed root ownership with no
  leaf exceptions; the only subtlety is that the owner emits fn thunks over
  EVERY program fn, not just its own — a `KClosure` over `char.to_lower` in the
  `string` module references `_kai_char__to_lower_thunk`, so all thunks must sit
  in one TU for every module's closure to resolve one definition.
- **Cache key = hash of the per-module KIR dump INCLUDING external callee
  signatures** (asu option B). `nmod_cache_key` folds the fns the TU defines
  plus the signature (name + param modes + slots) of every program fn. A
  producer's signature or ownership-mode flip rewrites the consumer's key →
  cache miss. There is no serialised ABI that can go stale, because the caller's
  own KIR text carries the callee's shape. This is the anti-ABI-skew guarantee
  of Model A, made honest rather than patched with a side hash.
- **Cache lives inside kaic2**, because the expensive step (LLVM codegen) is
  there. Objects are content-addressed `<dir>/<hash>.o`; a hit skips codegen.
  Existence is probed with `read_file` (its `Ok` = present) rather than a
  `file_exists` prim, and there is no `tmp+rename` — both `file_exists` and
  `file_rename` are ABSENT from stage1, so calling them breaks the kaic1
  bootstrap ("undefined name"). Content-addressing makes the direct write
  idempotent, so the rename was unnecessary anyway.

## Measured results (the gate)

Fixture `examples/multi-module/issue-989-native-modular/` (mono cross-module +
effects + cross-module fiber), driven by `test-native-modular`:

- **Byte-identical**: the modular binary's stdout/exit equals the whole-program
  native build's, byte for byte.
- **Incremental**: a body-only edit to a named fn re-emitted exactly 1 object;
  the other 14 were cache hits (15-object program: root + `mathx` + `worker` +
  12 stdlib modules). Verified by diffing the content-addressed object-name sets
  across two builds.
- **Anti-ABI-skew**: changing `pick`'s signature (arity) re-emitted its consumer
  (root) AND its producer (`mathx`) — the key fold caught the callee-shape
  change. NOTE the current fold is over EVERY program fn's signature, so a
  signature change conservatively invalidates ALL objects, not just the actual
  callers. That is sound (never a stale hit) but coarse; refining the fold to
  only the fns a module references is a follow-up (see below).
- **ASAN+UBSan serial**, -O2, on the cross-module-fiber fixture (the axis most
  prone to RC UAF, since the fiber scheduler is shared across objects): clean,
  correct output. The runtime owner (`runtime_llvm.c`) is sanitised; the kaic2
  objects are LLVM-O2 without instrumentation, same coverage level as the
  whole-program native path.

## Structural surprises

- **kaic1 runs `if cond { body }` WITHOUT an else UNCONDITIONALLY** (a known
  stage1 codegen quirk). The singleton-block guard
  `if nmod_owns_singletons(role) { hooks }` ran in EVERY partition — every
  object defined `install_defaults`/`teardown`/`proto_init` → duplicate symbols
  at link. It type-checks and selfhosts clean; only the runtime link failed.
  The tell was decisive: `nm` showed the default globals were 1-def/N-decl
  (proving role threading worked) but the singletons were N-def — isolating the
  fault to the `if`, not the role. Fix: `let _s = if cond { body } else { () }`.
  Every new bundle `if`-statement must have an else.
- **`runtime.h`'s `kai_llvm_ctx` is a global** reassigned per
  `llvm_module_new`; the native backend was written assuming ONE module per
  process. Emitting N modules SEQUENTIALLY is safe (each `module_new` recreates
  the context, each `native_ctx_new(m)` is a fresh struct deriving from its
  module), but holding two live modules at once would corrupt. The modular
  driver emits and finalises one object before creating the next.

## Fixtures added

- `examples/multi-module/issue-989-native-modular/` — the 4-axis fixture
  (`main.kai` root + `mathx.kai` generic + `worker.kai` fiber).
- `test-native-modular` (stage2/Makefile, KAI_LLVM-gated, in TEST_LIGHT_TARGETS
  + test-fast): byte-id vs whole-program, the measured incremental, and the
  signature-flip re-emit — three assertions from one fixture. Skips cleanly on
  a C-only build.

## Follow-ups for next lanes

- **Per-module reference set in the cache key.** Fold only the signatures of the
  fns a module actually references, not every program fn, so a signature change
  invalidates only true callers. Requires scanning each partition's call sites.
- **Full-instrumented ASAN on the native objects.** Today only the runtime owner
  is sanitised (LLVM-O2 objects carry no instrumentation). The C-modular path's
  Linux CI ASAN covers the frontend/RC discipline both backends share.
- **Wire `KAI_NATIVE_MODULAR` into `kai build` defaults** once the path has
  soaked; today it is opt-in via the env var, mirroring `KAI_MODULAR` for
  c-modular.
