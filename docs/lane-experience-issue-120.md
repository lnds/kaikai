# Lane experience — issue #120: opt-in Perceus regions (`region { }`)

**Scope shipped (this PR):** opt-in Perceus regions on **both backends**, end to
end — runtime bump-arena (P0), surface + side-table (P2a), and arena lowering with
deep-copy-out at the border plus per-constructor arena routing (P1) on the C
emitter and, in the closing commits of this lane, the matching LLVM lowering.
Both backends are byte-for-byte at parity on the five `region_*` fixtures
(`75 / 3 / 6 / 7 / 42`) with identical `KAI_TRACE_RC` arena accounting
(`arena_alloc=8 arena_free=8 arena_live=0`).

## Scope as planned vs as shipped

The brief asked for `region { }` as a block form that bump-allocates its
locally-constructed values into a per-region arena and frees the whole arena in
one shot at the closing brace, with values that escape the block deep-copied out
to the ordinary RC heap. All four internal build phases were sequenced by
soundness (not as ship gates):

- **P0 — runtime.** `KaiArena` (grow-only ~64 KiB chunks), `kai_arena_alloc`
  (stamps `rc = INT32_MAX` so decref/incref/`check_unique` short-circuit — a
  region value's decref is a free no-op and reuse-in-place auto-disables, both
  for free), `kai_arena_free` (bulk, subtracts `n_live` from the trace counter),
  a push/pop arena stack, and the C constructors `kai_arena_cons` /
  `kai_arena_record` / `kai_arena_variant` (identical field writes to their RC
  cousins, only the allocation source differs) plus `kai_deep_copy_out` (a
  **tag-gated** structural clone — NOT rc-gated, because arena values and shared
  singletons both carry `rc = INT32_MAX`). Mirrored as `kaix_*` shims in
  `runtime_llvm.c`. **Shipped as planned.**

- **P2a — surface + side-table.** `region` is not a keyword, not a function, not
  an effect. The parser lowers `region { ... }` to the marker
  `ECall(EVar("region"), [block])` (the block as a *direct* argument, not a
  thunk). A pre-typer pass (`compiler/region.kai`, a sibling of the nursery
  rewrite) recognises the marker by shape, records each region's
  `(file, line, col, nth)` in a side-table, and unwraps the marker back to a bare
  `EBlock` so every downstream matcher sees an ordinary block. **No new `ERegion`
  AST variant** — that would have forced a new arm at ~98 exhaustive
  `ExprKind`-matcher sites across 18 modules; the marker reuses
  `ECall`/`EVar`/`EBlock`, which every matcher already handles, for zero
  byte-identity and zero non-exhaustive-match risk. **Shipped as planned.**

- **P1 — arena lowering + deep-copy-out (C backend).** `EmitCtx` gained two
  fields: `regions: [RegionMark]` (the read-only side-table) and
  `in_region: Bool` (toggled per block). The `EBlock` arm checks
  `emit_block_is_region(cx, x.line, x.col)` and, for a region, emits a
  statement-expression:

  ```c
  ({ kai_arena_push();
     <stmts emitted with in_region = true>
     KaiValue *__rout = kai_deep_copy_out(<final expr>);
     kai_arena_pop();
     __rout; })
  ```

  Copy-before-pop is load-bearing. Constructors lexically inside the region route
  through `emit_cons_ctor` / `emit_record_ctor` / `emit_variant_ctor`, which emit
  `kai_arena_*` when `cx.in_region` and the RC ctor otherwise. **Shipped as
  planned for the C backend.**

- **P1 (LLVM) — same lowering on the LLVM emitter.** `LlvmEmit` gained the same
  two fields (`regions: [RegionMark]`, `in_region: Bool`), threaded from the
  driver's `region_marks` through `emit_program_llvm` and down every body-emit
  path. The `EBlock` arm of `llvm_emit_block` checks `llvm_block_is_region` and,
  for a region, emits the sequential IR analogue of the C statement-expression:
  `call void @kaix_arena_push()`, body with `in_region = true`,
  `%rout = call @kaix_deep_copy_out(<final>)` **before** `call void @kaix_arena_pop()`,
  then `set_last(%rout)`. The constructor target helpers (`llvm_cons_target` /
  `llvm_record_target` / `llvm_variant_target`) pick `@kaix_arena_*` when
  `in_region`. Nullary variants, the reuse-in-place path, and the internal
  nullary-spine `llvm_emit_variants_of` stay on the RC heap, matching the C
  carve-outs. **Shipped this lane.**

- **P3 — bench + retro + docs + PR.** This retro, the doc updates, and the PR.

## Design decisions and alternatives considered

- **Escape semantics = deep-copy-out (v1), not compile-time rejection.** A value
  that crosses the brace is cloned out of the arena with `rc = 1` (never the
  `INT32_MAX` sentinel — a sentinel-inheriting copy would never free and leak).
  So `let xs = region { [1, 2, 3] }` is legal: the list builds in the arena and
  copies out as an ordinary RC value. For pure LIFO scratch (build, fold to a
  scalar, discard — the canonical target) nothing crosses, the copy never fires,
  and the whole arena frees in one shot with zero RC traffic. The precise
  zero-copy escape via brand-in-signature + region polymorphism (`TyBranded`) is
  a documented **v1.x follow-up**; it was considered and explicitly deferred
  because it needs type-system surface the rest of the language does not yet have.

- **Mechanism B (lexical), not dynamic-extent.** `in_region` is threaded through
  `EmitCtx`; only constructors *lexically* inside the body bump-allocate.
  `kai_alloc` never consults a global "are we in a region?" flag — a
  dynamic-extent design (A/C) would let a value allocated by a callee invoked
  from inside the region land in the arena and then dangle after the pop. Lexical
  is the only sound choice without escape analysis.

- **Nullary variants and the reuse path stay RC.** Nullary variant constructors
  are shared singletons; arena-allocating them would dangle the shared singleton
  at pop, so `emit_variant_call`'s `n == 0` arm and `emit_ident_value`'s inline
  nullary emit deliberately keep `kai_variant_u`. The Perceus reuse path
  (`emit_reuse_variant_body`) is likewise left on the RC ctor — the `INT32_MAX`
  sentinel already disables reuse-in-place on arena values, so routing it would
  be dead at best.

- **Escaping closures stay on the RC heap.** A lambda created inside a region is
  the block value and is applied *after* the arena pops. Two things keep this
  sound: (1) the closure object is built with the ordinary `kai_closure` ctor
  (closures are never arena-routed — there is no `kai_arena_closure`), so it
  lives on the heap from birth; and (2) the lambda body emits with
  `in_region = false` (the carve-out), so any allocation it performs when finally
  called lands on the RC heap, not in an arena that no longer exists. The
  `region_with_lambda` fixture exercises apply-after-pop under ASAN.

## Structural surprises the brief did not anticipate

- **A signature change must update *every* caller, and a missed one fails late.**
  Routing variant constructors required changing `emit_variant_call`'s 4th
  parameter from `variants: [EVar]` to `cx: EmitCtx`. Three callers still passed
  `cx.variants`. The arity matched but the type did not — which does **not** fail
  the stage-1 build. Instead it produced a `kaic2` that self-miscompiled, and the
  symptom was a *runtime* `kai: field access on non-record` on every fixture plus
  a selfhost type error naming the function. The lesson: after a signature
  change, `grep '<fn>('` for **all** callers (not a filtered `grep cx.variants`,
  which missed the multi-line and differently-spelled call sites), read each, and
  confirm the changed argument. This bug cost two full reset-and-redo cycles.

- **Build-clean ≠ feature-works (the fake-green trap).** Twice a routing commit
  was reported green from a `kaic2` binary whose timestamp predated the edit (a
  stale working-tree build, or one left half-written by an interrupted selfhost).
  A from-scratch `rm kaic2 && make kaic2 && run-fixture && make selfhost` exposed
  the real state. Rule adopted for the rest of the lane: nothing is green until a
  fresh rebuild runs the fixtures to expected output **and** selfhost passes.

- **Incremental one-ctor-per-commit isolated the bug.** Cons and record routing
  were committed and fully gated (build + 5 fixtures + selfhost byte-identical +
  ASAN) before variant. That made the variant-only regression obvious instead of
  burying it three commits deep.

- **`+` inside a closure trips a pre-existing Add-protocol codegen gap, unrelated
  to regions.** The first `region_with_lambda` fixture used `(x) => x + base`,
  which failed to compile (`kai_protocols____pimpl_Add_Int_add` called with one
  argument instead of two). The same lambda fails identically with **no** region,
  confirming it is orthogonal. The fixture was rewritten to return its captured
  value directly so the region carve-out is the only thing under test. The
  `+`-in-closure gap is noted as a separate pre-existing issue.

## Fixtures added

`examples/perceus/` — all with `.out.expected` goldens, wired into the region
fixture set:

- `region_scratch.kai` → `75` — LIFO scratch, scalar result; zero deep-copy-out;
  KAI_TRACE_RC shows arena allocs = frees = 8, live = 0.
- `region_return_value.kai` → `3` — a list escapes the brace (deep-copy-out).
- `region_nested.kai` → `6` — nested regions.
- `region_in_match.kai` → `7` — region inside a `match` arm.
- `region_with_lambda.kai` → `42` — a closure escapes the region and is applied
  after the arena pops (carve-out; ASAN-guarded).

## LLVM backend parity — shipped this lane (lessons)

The LLVM half closed in the final two commits of this lane (scaffold-first, then
arena logic). Three lessons transferred from the C-emitter cost and held:

- **Scaffold the ctx threading first, with zero region logic, and gate it.** The
  `LlvmEmit` struct gained `regions` + `in_region` and every construction site
  (passthrough helpers, the three `e_scoped` match/switch restores, the four
  seed-fresh body emitters, `llvm_emit_new`) plus the seven-fn signature chain
  (`emit_program_llvm` → `decls_loop` / `lambda_bodies` / `clause_bodies` →
  `llvm_emit_fn` / `llvm_emit_lambda_body` / `llvm_emit_clause_body`) was threaded
  in a **first commit that compiles, self-hosts byte-identical, and still routes
  every ctor to the RC heap.** That isolates "is the threading sound?" from "is
  the lowering correct?" — a stale caller after a signature change auto-miscompiles
  `kaic2` (symptom: a runtime "field access on non-record" far from the edit), so
  the scaffold's selfhost gate is the cheap detector.
- **The six `declare` lines are invisible to selfhost.** `kaix_arena_push` /
  `kaix_arena_pop` / `kaix_deep_copy_out` / `kaix_arena_cons` / `kaix_arena_record`
  / `kaix_arena_variant` must be declared near the other `@kaix_*` ctor declares.
  Selfhost never emits a `region { }`, so a missing declare passes every internal
  gate and only a real `--backend=llvm` program with a region trips it (clang
  rejects IR that calls an undeclared symbol). The gate that catches it is running
  the fixtures through `--backend=llvm`, not selfhost.
- **The carve-outs are by call-site, not by helper.** Routing is per ctor
  call-site: the real list/record/variant builders go to `@kaix_arena_*` when
  `in_region`, but the nullary `@kaix_variant(...,0,null)` branch, the
  reuse-in-place path, and the internal nullary-spine `llvm_emit_variants_of`
  (`variants[T]()`) stay on `@kaix_cons` / `@kaix_variant`. Mis-routing the
  nullary or internal-spine sites would diverge from the C carve-outs.

## Coverage gaps left for next lanes

- **Region nominal records in protocol dispatch.** `kai_arena_record` stamps
  `head_type_tag = 0` (like `kai_record`, unlike `kai_record_h`). A nominal
  record built inside a region and used as a protocol-dispatch receiver would
  miss its head tag. Not exercised by any fixture; documented gap.
- **Nested-region arena stacking** beyond the simple `region_nested` case, and
  **escape-via-global** (storing a region value into a top-level mutable) are not
  fixture-covered.

## Cost vs estimate

Larger than a clean implementation would suggest, almost entirely due to the
signature-change-caller-miss bug (two reset cycles) and tooling instability
during the C-emitter session. The runtime (P0) and surface (P2a) were
straightforward; the C emit routing (P1) was where the cost landed, and the
discipline that finally worked — incremental per-ctor commits, each fully gated on
a fresh rebuild — is the transferable takeaway. The LLVM half, by contrast,
landed cleanly in two commits (scaffold + logic) with **no reset cycles**, because
the scaffold-first split made the signature-change-caller-miss class detectable by
the selfhost gate before any region logic existed — exactly the lesson the C side
paid for.
