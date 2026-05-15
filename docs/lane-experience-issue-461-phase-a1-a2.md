# Lane retro — issue #461 Phase A.1 + A.2 cache (NO CACHE CODE SHIPPED)

Best-effort retrospective by the implementing agent. **No cache
implementation code shipped in this lane.** The lane stopped at the
verification gate after empirically confirming the pre-blocker
documented in `docs/cache-design.md`. Outputs: this retro, a new
GitHub issue (#597) for the pre-blocker, and a docs-only PR that
refers #461 without closing it.

## Scope as briefed vs. as shipped

| Briefed | Shipped |
|---|---|
| Implement A.1 (typed `[Decl]` + ModuleEnvDelta cache) | **NOT shipped** — see "Why the lane stopped". |
| Implement A.2 (post-perceus + emit-walked cache) | **NOT shipped** — A.1 is the pre-blocker. |
| Verify `lower_protocols` boundary preservation empirically | **Shipped** — confirmed the boundary is destroyed. |
| Open issue for pre-blocker if needed | **Shipped** — issue #597. |
| Lane retro | This file. |
| PR closing #461 with auto-merge if gates pass | **Draft PR** that refs #461 (does not close); no auto-merge. |

## Why the lane stopped

The brief contained one critical factual error that drove the lane
toward a fail-fast halt:

> The brief says: "el lane spike de #452 (PR #579 abierto y cerrado)
> verificó que `module_origin` SÍ se propaga via
> `tag_decl_module_origin` (línea 55979-55981) y
> `rename_proto_calls_decls` lo preserva (52946-52949). Los
> dispatchers nacen con `mo == None` pero conviven con root code sin
> problema."
>
> Your job: verify this finding empirically.

Verification result: **the brief misreads PR #579's conclusion.**
PR #579 was a docs-only spike that explicitly **flagged the
boundary blocker as open** and deferred the decision to the
integrator. Quoting #579's body:

> After this point the decl stream has no recoverable "where did
> this decl come from" information. A cache loader has no way to
> partition it back into the two `ModuleDecls` segments the fold
> consumes.

The brief's reading ("the dispatchers live with root code without
problem") is true for **emit** (no name collision at codegen) but
false for **the cache loader partition step**, which is the
load-bearing step A.1 needs to skip the prelude typer pass.

The brief also instructed: "If you find that the module_origin
boundary IS destroyed by lower_protocols (contradicting the #579
spike), stop — design decision, draft PR." The empirical evidence
matches that branch, not the proceed branch. So the lane stopped.

## Empirical verification (what I read in stage2/compiler.kai)

Three signals together confirm the brief is wrong and `cache-design.md`
is right:

1. **`make_dispatcher_fn` (line 51354)** always synthesises
   `DFn(..., None)` — every protocol op's dispatcher is born without
   `module_origin`, regardless of which module the protocol came from.

2. **`lower_protocols` final concat (line 53243)**:
   ```
   let final_decls = list_append(user_renamed,
                                  list_append(impl_renamed, dispatchers))
   ```
   `dispatchers` are appended at the tail, all with `mo = None`,
   alongside `user_renamed` (root file's own DFns with `mo = None`).
   The post-cascade decl stream's positional boundary between
   prelude- and user-origin entries is destroyed here.

3. **`efn_resolve` (line 11207)** treats `mo == None` as evidence of
   "the root-file entry":
   ```
   #   - the root-file entry (module_origin = None) when present, even
   #     if other modules also export the same name (locals-shadow rule
   #     applied at top-level scope);
   ```
   A cache loader handing the typer two segments (cached prelude +
   fresh user) cannot recover the boundary because every dispatcher
   in the cached prelude looks like a "root file entry" to the
   typer's lookup.

There are additional synthesis sites with the same shape:

- `lower_const_one` (`stage2/compiler.kai:19984`) — `DConst → DFn(..., None)`.
- `lower_axiom_one` (`stage2/compiler.kai:20014`) — `DAxiom → DFn(..., None)`.
- `build_refine_pred_fn` (`stage2/compiler.kai:20434+`) — synthesised
  `__ref_pred_X` for every `type X = T where pred`.

(`tag_decls_module_origin` partly compensates — `tag_decl_module_origin`
re-walks DConst/DAxiom via `lower_axiom_one`/`lower_const_one` and
re-tags after lowering, so a prelude-loaded DConst becomes a DFn
with `mo = Some("module")`. But this only fires before the cascade
runs, on the load_prelude path; the cascade itself's lowerings —
`lower_consts(prelude_decls)` at line 58809 and `lower_axioms` at
line 58921 — run on the merged stream and don't re-tag. Verify in
the follow-up lane.)

## Why "just tag the dispatchers" is a separate lane

The brief frames this as a "verify, then proceed" step. But the
boundary-tagging refactor is not free:

- **4 synthesis sites** need tag inheritance (`make_dispatcher_fn`,
  `build_refine_pred_fn`, `lower_const_one`, `lower_axiom_one`).
- **`ProtoOpReg.POR` needs a new field** (or the protocol's
  module_origin lookup table) so the dispatcher knows which module
  the protocol came from.
- **`efn_resolve`'s "root-file wins" rule** is affected. Today
  every dispatcher is `mo = None` and "wins" priority over any
  bare-name match. After the refactor, prelude dispatchers
  (`mo = Some("protocols")`) would lose that priority. In practice
  no user code shadows `__proto_<op>` (it's an internal name), but
  selfhost-byte-identical needs verification at every sub-step.
- **`validate_pub_access` (#510)** special-cases `mo == None` as
  "root-file decl". The refactor may shift its behaviour. Audit
  required.

`cache-design.md` estimates 300-500 LOC for this refactor, separate
from any cache wiring. That matches my read of the code surface.
This is **not** the right scope to bundle with cache serialisation
— it's a distinct refactor with its own selfhost-byte-identical
risk.

## What the brief got right

The brief acknowledged two stop-clauses that I exercised:

> "Si encuentras que el module_origin boundary SÍ está destruido por
> lower_protocols (contradiciendo el spike de #579), parar — design
> decision, draft PR. Probable lane separado para refactor de
> lower_protocols antes."

> "Si el agente descubre downstream perceus/emit passes que asumen
> 'decls ya procesados desde scratch' (el bug que mató #593), escribe
> retro honesto, abre issue del blocker, NO fuerces."

Both apply. The cache lane that the brief asked for was 2-3 lanes
deep in disguise:

1. **Boundary tagging refactor** (this issue, #597) — 300-500 LOC.
2. **A.1 cache payload + driver wire-up** — 1500-2500 LOC.
3. **A.2 cache payload + emit reachability seed** — 1500-2500 LOC.

The brief's "scope crece >3000 LOC sin cerrar" clause would bite
mid-way through step 2 if I'd ignored the blocker.

## What this lane did NOT prove

- **Whether the A.1 target wall (1.31 s) is actually reachable**
  with current runtime / RC allocation costs. The KAB2 retro
  (#592, "Surprise #3") showed the design's projected savings
  overestimated by ~2× because the cache loader is not free.
  A.1's 0.59 s typer-savings projection assumes the loader
  costs near zero per-decl; without measuring on a real prototype,
  the 1.31 s gate is paper, not metal. **The integrator should
  consider whether A.1 is worth a 300-500 LOC refactor + 2000 LOC
  of cache wiring without a prior bench-only prototype that
  confirms the saving exists.**

- **Whether A.0 + A.2 without A.1** is viable. PR #579's
  retro suggested this might be cheaper to ship and deliver
  ~42% of the baseline saving. It assumes the user file's
  reachability closure can be computed without typed prelude
  info, which my read of `monomorphise` + `perceus` suggests is
  false (both depend on typed AST). The integrator may want to
  spike this independently.

- **Anything about the codegen-side cost.** A.2's "emit only
  reached prelude DFns" is the bigger saving (~0.55 s). The
  brief assumes A.2 trivially follows A.1; in practice A.2
  needs `monomorphise + perceus + lower_protocols + emit` to
  consume cached typed input on a per-DFn basis. The current
  pipeline's structure (`infer_program_with_protos` returns a
  whole `TypedProgram`; `monomorphise` walks it whole; `perceus`
  walks it whole; emit walks the merged stream once) means A.2
  is yet another structural refactor of the back end.

## What I read

Pre-read list per brief, in order:

1. `gh issue view 461` — confirmed Phase A.2 body and acceptance.
2. `docs/cache-design.md` — primary source of the lower_protocols
   blocker description (lines 85-149).
3. `docs/lane-experience-issue-452-step3-4-5-6-driver.md` — A.0
   driver retro, no contradiction.
4. `docs/lane-experience-issue-592-kab2-binary.md` — KAB2 retro,
   confirms the design's projections are optimistic by ~2× because
   loader cost is non-trivial. Pre-blockers for A.1 listed
   explicitly there.
5. `docs/lane-experience-issue-574-typer-per-module-fold.md` —
   typer fold landed; the driver-side multi-segment wiring is
   listed in §"Out of scope" as deferred to the cache lane.
6. `stage2/compiler.kai`:
   - `cache_serialize_module` / `cache_deserialize_module` — KAB2 codec.
   - `typecheck_module` (line 37020), `typecheck_program` (line 37097),
     `typecheck_program_loop` (line 37115).
   - `lower_protocols` (line 53177-53245).
   - `make_dispatcher_fn`, `lower_impl_methods`, `lower_const_one`,
     `lower_axiom_one`, `build_refine_pred_fn`.
   - `compile_source` (line 58696-59300+) — the full driver cascade.

I did not read `monomorphise` / `perceus` / `emit_program` in
depth because the lane stopped at the typer boundary; A.2's
prerequisite (A.1) was already a blocker.

## Sub-step record

No code sub-steps. Three deliverables:

1. **Issue #597 opened** — pre-blocker for A.1 cache: tag
   synthesised decls with `module_origin`.
2. **This retro** — `docs/lane-experience-issue-461-phase-a1-a2.md`.
3. **Draft PR refs #461** — docs-only, no cache implementation, no
   auto-merge. Recommends sequencing per #597 → A.1 → A.2.

## Selfhost / tier coverage

No code changes; selfhost is untouched. Tier 0 / Tier 1 / Tier 1-ASAN
not run because there's nothing for them to verify. The PR is
docs-only and gated by `paths-ignore` in `.github/workflows/tier1.yml`.

## Real cost vs. estimate

| Activity | Brief estimate | Actual |
|---|---|---|
| Pre-read | ~30 min | ~25 min |
| `module_origin` verification | mid-lane checkpoint | ~30 min |
| Reading cascade + typer | ongoing | ~25 min |
| Decision + issue draft | ~15 min if blocker | ~25 min |
| Retro + PR | ~60 min | ~45 min |
| **Total** | full implementation: 4-8 h | ~2.5 h (verification-only) |
| LOC | 1500-3000 | 0 compiler + ~300 docs |

The lane was sized for full implementation. The verification path
took ~2.5 h and consumed ~0 LOC of compiler change. That is the
correct outcome when the brief is wrong — the early-stop saved 4-6
hours of false-start work that would have hit the blocker mid-PR.

## Follow-ups

- **#597 (this lane opened)** — boundary tagging refactor for
  `lower_protocols` + synth desugars. Pre-blocker for A.1.
- **#461 stays open** — Phase A.1 + A.2 await #597 closing.
- **`cache-design.md` may need a "Realistic projections after KAB2"
  section.** The KAB2 retro showed the design's 0.41 s lex+parse
  saving for A.0 was 0.20 s in reality. Similar correction for
  A.1's 0.59 s typer saving is likely needed before quoting a
  1.31 s gate. Defer to the A.1 lane that actually benches.
- **Strategic question (deferred to integrator)**: is the 0.55 s of
  A.2 a more attractive lane than A.1 if it can be wired without
  cached typed AST? The #579 spike suggested it might be, with a
  caveat about reachability seed needing types. Worth a
  bench-only spike before committing to the full A.1+A.2 chain.

## What I'd do differently

The brief should have been a verification-first sub-issue ("read
the code, confirm/refute the #579 finding, file a report") that
spawned an implementation lane only after the design question was
settled. As-written, the brief asked me to implement on a premise
the code disagrees with; the answer was always going to be either
"the premise holds" or "stop". Splitting that into two PRs would
have made the autonomy-overnight authorisation match the actual
risk: open question first, code second.

## Bitácora notes for the next lane (A.1 implementer)

- **Read `efn_resolve` and `validate_pub_access` before touching
  `make_dispatcher_fn`.** The `mo == None` semantics are load-bearing
  in two places besides the cache loader's mental model.
- **The "Path B" alternative in `cache-design.md`** (cache post-
  cascade, accept format-version churn) is uglier than it sounds.
  Every walker addition or removal in the ~30-pass cascade bumps
  the format. Path A (the #597 refactor) is the cleaner long-term
  story.
- **Don't trust phase-wall projections without a bench-only spike.**
  KAB2 was supposed to save 0.41 s; it saved 0.03 s after loader
  cost. A.1's projected 0.59 s saving needs an actual prototype
  before quoting a 1.31 s wall gate.
- **`infer_program_with_protos` (line 37174)** is still the single
  call site for the typer; the multi-segment fold is unexercised
  by any caller as of v0.61.0. The A.1 cache loader is the first
  caller; selfhost byte-identical risk on TyVar ID renumbering
  (per #574 retro §4) is real if any of `--dump-typed` /
  `--dump-mono` goldens depend on exact ID values.

## Limitations of this retro

- I did not run `monomorphise` / `perceus` / `emit_program` reading
  to confirm A.2's structural feasibility. The lane stopped at
  A.1's pre-blocker so the deeper read was out of scope.
- The "300-500 LOC" estimate for #597 is paper, not measured. The
  actual refactor lane will produce a real figure.
- No bench numbers shipped because no code shipped. The 2.28 s
  warm wall from v0.61.0 stands as the current baseline.
