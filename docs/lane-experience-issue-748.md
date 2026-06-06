# Lane experience report — issue #748 (separate compilation per module)

Best-effort retrospective by the implementing agent.

## Scope as planned vs scope as shipped

**Planned (brief):** make the C emitter generate a `.h` + `.c` PER
MODULE, compile each separately to its own `.o`, and link them — the
GHC/Koka model — so a user `fn sum(x, y)` that shadows the stdlib
`list.sum(xs)` no longer collides on the C symbol `kai_sum`. Two
non-negotiables: (1) the user program compiles and runs, the user
shadow winning; (2) compilation is genuinely modular (per-module
`.h`/`.c` → `.o` → link).

**Shipped:** both, plus a third layer the brief did not anticipate.
The work split into a *correctness* half (name mangling, which closes
the bug) and a *packaging* half (per-module `.h`/`.c`/`.o`, the
architecture the brief asked for). The owner's framing — "for me
they're the same thing" (complete per-module mangling) — turned out
to be exactly right: the packaging only links cleanly *because* the
mangling makes the names genuinely distinct.

1. **Codegen prune (correctness, both backends).** A module body's
   bare self-call (`list.sum`'s internal `sum(t)`) was mangling to the
   user's root shadow `kai_sum` (2-arg) instead of `kai_list__sum`,
   emitting invalid C ("too few arguments") on the C backend and a
   silent 1-arg-vs-2-arg ABI mismatch on LLVM (UB, the #466 class).
   Fixed in `fns_prefer_module` (`emit_shared.kai`): when lowering a
   module body, prune the root-file shadow of any name the module
   redefines, so `efn_pick_root` cannot grab the user's `kai_sum`. One
   function, both backends (both call `fns_prefer_module` at the same
   body-emit site).

2. **Typer root-shadow precedence (correctness).** A user
   `fn length(s: String)` with the SAME signature as `string.length`
   was being routed to `string.length` by receiver-type narrowing
   (#235), so the shadow lost. Added `TyEnv.root_fns` (seeded once from
   the root file's `mo = None` decls) and a guard in
   `try_bare_call_narrow`: skip narrowing when `nm` is a root shadow.
   The user's decl wins even on exact-signature collision.

3. **Separate compilation (packaging, the brief's ask).** New opt-in
   emit path `emit_program_modular` (`--emit=c-modular`, driver mode
   `MEmitCModular`) partitions the post-mono decl stream by module
   origin `mo` and emits, per module, a `.c` translation unit, plus two
   shared headers (`kai_types.h` = runtime + all structs;
   `kai_decls.h` = all prototypes). The `bin/kai` wrapper (gated on
   `KAI_MODULAR=1`) splits the marker-delimited stream, compiles each
   `.c` to its own `.o`, and links. A multi-module program
   (`examples/multi-module/main.kai`) emits 14 `.c` files — root +
   `math` + `shapes` (user modules) + 11 stdlib core modules — each its
   own object, linked into a binary that runs `49 / 25`.

## Design decisions and alternatives considered

- **Prune vs. threading `cur_mo` (decision 1).** First attempt threaded
  a `cur_mo: Option[String]` through `EmitCtx` + a new `efn_resolve_in`
  and ~7 call sites. It worked on the C backend but was infeasible on
  LLVM (`LlvmEmit` has 44 copy-all constructors; threading a field
  there is a non-starter). Pivoted to pruning the shadow inside
  `fns_prefer_module` — one function, already called by BOTH backends
  at the body-emit site. Reverted the entire `cur_mo` threading. The
  prune is simpler, symmetric across backends, and covers every call
  site at once (they all read the rotated table). asu's review was
  decisive: "the prune is the load-bearing half; partition alone does
  not fix #748 because C has no namespaces — two `kai_sum` in two `.o`
  either clash or the linker picks arbitrarily."

- **Linkage: post-process strip vs. threading `Linkage` (decision 3).**
  The emitter prefixes every top-level sig with `static` — correct in
  single-TU, fatal across `.o` (internal linkage hides the symbol).
  Chose to strip `static` from the modular stream (anchored to column 0
  + a closed whitelist of emitted return types: `KaiValue *`,
  `int64_t`, `double`, `int`, `uint32_t`) rather than thread a
  `Linkage` param through ~17 sig/thunk/lambda/clause emitters. The
  strip is uniform (in modular mode EVERYTHING goes external; names are
  uniquely mangled so external linkage cannot clash), lives entirely in
  the wrapper's split step, and never touches the single-TU path. asu's
  argument against threading: it risks selfhost byte-identity across 17
  defaults in a language without default args, for zero gain, and the
  bite-case (thunks/lambdas emitted outside `EmitCtx`) needs reaching
  anyway. Verified gate: no top-level sig is emitted indented (so the
  column-0 anchor is complete) and runtime.h's intentional `static`
  helpers live in runtime.h, not the emitted stream.

- **Grain: per-module, including stdlib (owner's call).** The minimal
  cut (2 TUs, root vs rest) would close #748, but the owner chose
  max grain — one `.c`/`.h` per module, stdlib core included — because
  it is the real base for #677 (incremental recompilation). The
  partition is by `mo`; each distinct origin is its own TU.

- **Output mechanism: marker-delimited stdout, not `--emit-dir`.**
  kaic2 stays a pure stdout filter (no File-writing output path, no
  bootstrap-chain divergence). The wrapper splits on
  `//KAIFILE:<nonce>::<name>` markers; the nonce (derived from program
  text length) makes the marker uncollidable with emitted C. Selfhost
  byte-identity is preserved trivially: the single-TU `emit_program` is
  untouched and is the only path the bootstrap exercises.

- **Opt-in, not default (owner's call).** Modular mode is behind
  `--emit=c-modular` / `KAI_MODULAR=1`. The default C backend stays
  single-TU, so tier0/tier1/parity validate the unchanged path and the
  separation is ready to promote to default in a later lane. #748's
  corruption is already closed by the mangling in the default path.

## Structural surprises the brief did not anticipate

- **The mangling, not the packaging, is what fixes #748.** The brief
  framed separate compilation as THE fix. In fact the symbol-mangling
  correctness is load-bearing and the packaging is downstream of it —
  without distinct names, two `.o` files produce a duplicate `kai_sum`
  at link. The owner's intuition ("they're the same thing") was the
  correct read: complete per-module mangling IS the fix, and the `.o`
  split is its packaging.

- **A second shadow bug in a different layer.** Writing the multi-shadow
  fixture surfaced `length` losing to `string.length` — a *typer*
  resolution bug (type-directed narrowing), distinct from the *codegen*
  symbol collision. Same principle ("user shadow wins"), different
  layer. The owner's instruction — "fix length too, don't delete it
  from the fixture; don't alter fixtures because things don't work
  out" — kept the fixture honest and forced the typer fix.

- **#466 closed as a side effect.** The `fn show(stack: [Int])` shadow
  of `impl Show[List[T]]` panicked non-exhaustive at runtime. Both
  halves of this lane's fix (codegen prune for the recursive
  `show(rest)`, typer precedence for the `show([-3])` call) close it.

- **LLVM "passed" while broken.** Pre-fix, the #748 repro printed `30`
  on LLVM — but the IR called `@kai_sum` (2-arg def) with 1 arg, working
  only by System V ABI accident (reads one register, ignores the other).
  Latent UB, not correctness. Confirmed the IR now emits
  `@kai_list__sum`.

## Fixtures added and coverage

- `examples/shadowing/user_shadows_stdlib_list_fns.kai` — sum / reverse
  / length / product / concat user shadows; each wins. `length` is the
  load-bearing case (exact-signature collision with `string.length`).
- `examples/shadowing/user_shadows_and_qualified_coexist.kai` — bare
  `sum` (user) and `list.sum` (stdlib) coexist in one program.
- `examples/shadowing/user_show_shadows_protocol_op.kai` — the #466
  shape (user `show` shadows the Show[List] protocol op).
- `examples/multi-module/modular_stdlib_lambdas.kai` — the linkage edge
  case a review flagged: stdlib higher-order fns (`map`/`filter`) create
  lambda closures in a module body that REFERENCE root-emitted
  `_kai_lam_N` symbols cross-TU. Links only because the `static`-strip
  externalises both the lambda definitions AND their `kai_decls.h`
  forwards (the strip runs over the whole stream, header included), so
  the module TU's `&_kai_lam_N` resolves to root's external symbol.
- `stage2/Makefile` `test-modular-build` — exercises `--emit=c-modular`
  end-to-end (split + per-module `.o` + link + run) over 8 multi-module
  / shadowing fixtures; wired into the `test` chain (tier1).

All three `.kai` fixtures run identically on C and LLVM (single-TU);
all 7 modular-build fixtures compile to separate `.o` and link.

Coverage gap: lambdas / clause bodies are routed to the root TU
(LamInfo / ClauseInfo carry no `mo`). Link-safe and complete, but
coarse — a follow-up could seal the real parent-mo into those records
so a module's lambdas live in its own `.c`. Has its own fixture when
taken up.

## How this advances #677

#677 wants the stage2 compiler's OWN sources separately compiled (3
phases). This lane delivers the *mechanism* #677's separate-compilation
goal needs — per-module `.h`/`.c`/`.o` + link, driven by `mo`
partitioning — for USER programs. kaic2 still self-compiles single-TU
(selfhost byte-identity untouched), so #677's phases are not gated on
this; but the emit path, the linkage discipline, and the marker stream
are now in place and proven on real multi-module programs. Promoting
modular to the C default, then pointing it at the compiler's own
modules, is the natural next step.

## A preexisting tier1 break that `main` fixed in parallel

tier1's `test-rc-trace-callsite` was RED before this lane — a preexisting
`runtime.h` bug: under `-DKAI_TRACE_RC=1` the `kai_slab_alloc` bump
allocator was defined only inside the cell-pool `#if !defined(KAI_TRACE_RC)`
block, but `kai_alloc_var` / `kai_alloc_var_nz` call it on their non-pool
`#else` path (taken under exactly that mode) → a call to an undeclared
function (hard C99 error on Linux clang; Apple clang only warned, which is
why it surfaced as a Linux-only tier1 red).

I first fixed it in-lane (authorized, since it gated the lane's tier1) by
lifting the slab allocator above the `#ifdef KAI_TRACE_RC` split. But a
rebase check found `main` had ALREADY fixed it independently, better:
`4ded98b` makes the no-cell-pool branches use plain `calloc`/`malloc` and
`kai_var_block_free` `free()` the block — the slab simply does not exist
without the cell pool, so avoiding it is cleaner than making it always
available (mine left a latent question about slab teardown in trace mode;
`main`'s pairs alloc/free correctly). I reverted my `runtime.h` change and
took `main`'s. Lesson reaffirmed: `git log HEAD..origin/main` before the
first push — the same bug was being fixed in two lanes at once.

## Follow-ups left for next lanes

- Seal real parent-mo into LamInfo / ClauseInfo so lambdas/clauses live
  in their owning module's `.c` instead of root (with a fixture).
- Promote `--emit=c-modular` to the C default once the full tier1 +
  parity suite is validated under modular (not just a sample).
- Bring the LLVM backend to the same per-object-linked model (today it
  is single-TU; runtime parity holds, architecture parity is a
  follow-up the brief explicitly allowed deferring).
