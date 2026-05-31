# Issue #120 — opt-in Perceus regions: phased design (asu + linus, 2026-05-31)

Read-only design pass via asu + linus consult. Implementation deferred:

the P0 lane waits for the #86 lane to merge (overlap in runtime.h + both

emitters); P2a/P1/P3 are gated behind P0's per-alloc-site benchmark.

## Verdict: worth doing now?

P0-ONLY now; the rest is benchmark-gated. This resolves the one real asu/linus disagreement in linus's favor, and here is why. Both consults agree on every mechanic (block not effect, INT32_MAX sentinel reuse, side-table escape analysis, TyBranded-does-not-exist). They split on scope-now: asu would start the surface+escape phases immediately (P1 at #86 merge, P2/P3 in parallel); linus would ship P0 alone, benchmark it, and decide P1+P2 from data. Linus is right, for a reason rooted in this repo's own measurements, not in the post-1.0 label (which Eduardo said to ignore, and we do). The self-compile bottleneck is DISTRIBUTED RC churn (~10.3B increfs spread across the whole compiler per the perf-plan memory), not a concentrated lexer/parser scratch hotspot. region{} only pays where allocation is tight LIFO bulk-free-able. There is currently NO measurement that the lexer/parser/formatter scratch allocs are a material fraction of that 10.3B. The asu plan commits to building TyBranded-from-scratch (the thing #71 explicitly punted) wired through every binding form in a 17k-line infer.kai, for a win that could be sub-percent. That is the worst outcome shape this session keeps hitting: permanent type-system complexity for a gain below measurement noise. P0 is genuinely worth doing regardless: it is a clean, idle-free runtime primitive (the arena costs nothing until kai_arena_alloc is called), it proves or kills the performance case with a C-level microbench BEFORE any language-surface or typer investment, and it is even usable by C-level internal compiler passes without any surface. So: land P0, run a per-alloc-site profile of self-compile to find LIFO-scratch-shaped sites, and make the P1+P2 go/no-go on that data. NOT a defer of the feature — a defer of the EXPENSIVE half until it is justified.

## Decisions (asu + linus, unanimous unless noted)

- **Surface:** BLOCK (`region { ... }`), NOT a `Region` effect. Both consults converge here independently; asu gives the load-bearing argument. (1) Tier-1 #1: an effect would force `/Region` onto every signature that allocates in a region, but a region is LEXICAL and NON-ESCAPABLE — the opposite of an effect that an external handler interprets. Modeling it as an effect lies about its nature. (2) Tier-1 #3 (fast compilation, no HKT): making region escape sound through the effect row requires rank-2 polymorphism / skolems so the row cannot leak out of scope — that is HKT-adjacent and explicitly banned. Koka itself does not express regions as a flat effect row; it uses `run` with rank-2 (like Haskell's ST), a piece kaikai does not have and does not want. (3) The block earns its Tier-2 #5 slot ('few forms, clear intent'): it carries a new, unambiguous intent — 'everything allocated here dies when the brace closes' — same structural mold as `nursery { }`, no overlap with an existing form. v1 ships WITHOUT a binder (no `region r -> ...`): every allocation syntactically inside the block goes to the implicit top-of-stack arena. The named binder is the door to nested/polymorphic regions and is deferred.

- **decref no-op:** Reuse the existing `rc = INT32_MAX` saturation sentinel — confirmed in code, costs ZERO new branches. The runtime already short-circuits this sentinel in exactly the three places that matter: kai_decref (runtime.h:2116 `if (v->rc == INT32_MAX) return;`), kai_incref (runtime.h:1376 same guard), and kai_check_unique (runtime.h:2734 `return v != NULL && v->rc == 1 && v->rc != INT32_MAX;`). Mechanism: a thread-local stack of arenas (grow-only ~64KiB chunks + bump pointer). kai_arena_alloc fills the SAME KaiValue header as kai_alloc but stamps `rc = INT32_MAX`. To the RC machinery an arena value is indistinguishable from a singleton — no new tag, no new branch in kai_free_value, kai_decref, or kai_incref. At block exit, kai_arena_free does a bulk free()/munmap per chunk WITHOUT walking values. Two free bonuses fall out: (a) reuse-in-place auto-disables on region values because kai_check_unique returns 0 for rc==INT32_MAX — Perceus's in-place reuse never fires on a region cell, no pass change needed; (b) the existing singleton path is the live proof the trick works. Reject the alternatives asu and linus both dismissed: arena-membership-check on decref is slow (hot-path branch); a region-id field in KaiValue enlarges the struct and adds a branch. CRITICAL bookkeeping landmine: because the sweep frees in bulk without walking, kai_rc_live_now will NOT decrement for region values and KAI_TRACE_RC gates will report false leaks. Fix: maintain a SEPARATE per-arena alloc counter (distinct from kai_rc_alloc_total), and on kai_arena_free subtract that count from kai_rc_live_now AND surface it in kai_rc_report() so a wrong-codegen leak (a non-region value mistakenly arena-allocated, which leaks silently since decref is a no-op and ASAN won't see it) is VISIBLE.

- **Brand reuse:** Almost nothing transfers as code; the PATTERN transfers, the deferred half is the actual prerequisite. CONFIRMED by direct read: `TyBranded(Ty, BrandId)` does NOT exist — all 4 hits (infer.kai:14745, 14820; desugar.kai:3912, 3921) are comment lines (#) describing deferred spec. What #71 actually shipped is check_no_fiber_escape (infer.kai ~14755), a SHALLOW SYNTACTIC walker over the user-written TypeExpr annotation (not the resolved Ty) that rejects Fiber[T]/Pid[Msg] in RETURN-TYPE position of public signatures, with an allow-list (fiber_producer_helpers, ~14831). Why it is the WRONG granularity for regions: (1) it fires at function DECLARATION boundaries (you cannot return a Fiber); region escape is a BINDING-SITE problem — `let x = region {...}`, passing a region value to a storing function, putting it in a list — these cross the block scope, not a return type. (2) it is syntactic over TypeExpr, not semantic over Ty. (3) the allow-list works because there are exactly ~4 legitimate Fiber producers; regions are constructed at EVERY `region {}` block, so an allow-list is the wrong abstraction. TRANSFERS: the code STRUCTURE (walk typed form, detect brand violation, emit diagnostic) and the 'fresh BrandId per syntactic scope + side-table stack of binders' idiom from the nursery rewrite (desugar.kai:3277+). DOES NOT TRANSFER: do not bolt region escape onto check_no_fiber_escape — wrong granularity. The full TyBranded propagation that the comments at 14745-14754 describe (thread a brand through let/match/list-literal/record-field/fn-arg, reject mismatch) is EXACTLY what precise region escape needs — and it has NOT been built. So 'reuse #71' is not a shortcut; it is 'build the thing #71 deferred.' v1 sidesteps this entirely via side-table flow analysis (rawsafe.kai pattern) + conservative deep-copy-out at the border, so v1 does NOT need TyBranded at all — TyBranded is only for the deferred PRECISE (zero-copy) escape and region polymorphism.

## Phases

### P0 — runtime bump-arena + sentinel decref-no-op, NO surface

Add KaiArena (bump pointer + grow-only chunk list, malloc-backed for portability — no mmap dependency in stage0), kai_arena_alloc(arena, tag) that fills the standard KaiValue header with rc=INT32_MAX, and kai_arena_free(arena) that frees all chunks in bulk without walking values. Add a SEPARATE per-arena alloc counter distinct from kai_rc_alloc_total, surfaced in kai_rc_report(); on kai_arena_free subtract it from kai_rc_live_now so trace gates balance. Zero parser, zero typer, zero AST changes. Both emitters, when they eventually emit region alloc, call the SAME C helper via the runtime-helper FFI path — do NOT write a separate LLVM bump-alloc (that is the C/LLVM divergence trap from #618/#558/#714).

- **Gate:** C-level fixture: alloc 10,000 values into an arena, assert kai_rc_free_total does NOT increment on arena_free, assert kai_rc_live_now returns to baseline after the sweep, assert the arena memory is released. Microbench: N arena allocs + 1 arena_free vs N calloc + N kai_decref. tier1-ASAN MANDATORY (new bulk free path). build-release.sh LLVM smoke MANDATORY (helper must be callable/linkable from both backends even before lowering uses it). Explicit arena-alloc counter visible in kai_rc_report().
- **Risk:** Silent-leak class: a non-region value mistakenly arena-allocated leaks permanently (decref no-op, kai_free_value never called) and ASAN does NOT catch it (memory not freed = no use-after-free, just a leak). Mitigation: the dedicated arena-alloc counter makes kai_rc_alloc_total vs free_total divergence visible. FILE-OVERLAP dependency: P0 touches runtime.h + both emitters' helper-declaration surface, which the #86 lane is editing NOW — P0 implementation must wait for #86 to merge.

### BENCH GATE — per-alloc-site self-compile profile

With P0 landed, profile the self-compile to identify which allocation sites are LIFO-scratch-shaped (lexer token scratch, parser recovery state, formatter buffers) and what fraction of the ~10.3B incref traffic they represent. This is the data linus requires before committing to the typer work.

- **Gate:** A profile that attributes alloc traffic per call-site. Decision rule: if scratch-shaped sites are a material fraction (linus's illustrative threshold ~15%), proceed to P2a+. If sub-percent (~3%), STOP — defer the surface+typer phases; P0 stands alone as a primitive usable by internal C-level passes.
- **Risk:** If skipped, the project risks building TyBranded-from-scratch through 17k lines of infer.kai for a sub-measurement-noise win — the exact worst-outcome shape this session keeps hitting. This gate is the whole point of P0-first.

### P2a — parser + AST + escape analysis, lowering to NORMAL RC alloc

Add region{} to the parser (LL(1), scope-marker, same mold as nursery{}) and AST. Lower it to NORMAL reference-counted alloc with a region marker — NOT arena alloc yet. Region values under normal alloc are SAFE even if they escape (just slower). Add the escape-analysis pass as a PURE side-table over the AST (rawsafe.kai pattern, keyed by (fn,line,col,nth)): every kai_alloc-emitting constructor syntactically inside region{} tints its binder region-local; let/match propagate the tint RHS->LHS; simple aliasing followed within the lexical scope. Check the FOUR escape sites (see soundness_landmines).

- **Gate:** selfhost byte-identical (region{} lowering to normal alloc must not perturb anything else). 4 fixtures (one per escape site) in examples/perceus/ — negative fixtures get .err.expected goldens, positive get .out.expected. Escape check fires green on all 4 with lowering still on the SAFE normal-alloc path. tier1 + tier1-ASAN.
- **Risk:** This is the seam linus corrected: building parser+AST+escape-check on top of NORMAL alloc means the type system is proven correct with ZERO memory danger. Only the typer-machinery complexity risk remains, and the bench gate already justified paying it.

### P1 — switch lowering from normal RC alloc to arena alloc

Flip the region{} lowering from normal RC alloc to kai_arena_alloc (push arena on entry, kai_arena_free on exit), in BOTH emitters via the shared C helper. Add conservative deep-copy-out at the border (the value is copied OUT of the arena with rc=1 when it crosses an escape site the analysis cannot guarantee contained).

- **Gate:** By this point the escape check from P2a is already enforced, so the use-after-free window is structurally closed. selfhost byte-identical for non-region code. tier1-ASAN MANDATORY (this is where the real bulk-free path goes live and where escape-without-check would be a use-after-free factory — ASAN catches the true use-after-arena-free). build-release.sh LLVM smoke MANDATORY (both emitters must call the same helper). KAI_TRACE_RC gates balancing post-sweep.
- **Risk:** HIGHEST-risk phase if ever done before P2b — escape -> arena freed -> dangling interior pointers -> use-after-free. Ordering it AFTER the escape check (P2a) is what makes it safe. Also: deep-copy-out must produce rc=1, NOT propagate INT32_MAX (else the copy never frees = leak).

### P3 — benchmark the win

Demonstrate the alloc-traffic reduction on the scratch-shaped sites the bench gate identified, end-to-end through the real compiler.

- **Gate:** Same lexer/parser pass with vs without region{} shows reduced alloc traffic / wall, no regression elsewhere, selfhost byte-identical.
- **Risk:** If the realized win does not match the bench-gate projection, the surface investment is the cost of the lesson — but P0 already de-risked this by being the cheap proof.

## Soundness landmines

- (a) THE central hole: a region value passed to a function that stores it in a longer-lived structure. Without brand-in-signature the typer cannot see it cross the call boundary. v1 policy: deep-copy-out at the border (copy the value out of the arena with rc=1), NOT an error. For scratch buffers this essentially never fires. The PRECISE (zero-copy) fix is brand-in-signature + region polymorphism = the deferred TyBranded work.
- (d) sibling of (a): a closure captures a region value and outlives the scope -> use-after-arena-free. Same policy: deep-copy-out or error. ASAN catches it if the analysis has a hole (true use-after-freed-memory once the arena is released).
- P0b interior-pointer dangle: 'region values keep KaiValue shape' means a region KAI_STR still holds a char* bytes pointer, a record holds a fields array, a cons holds head/tail. When the arena is bulk-freed those INTERIOR pointers dangle. Any non-region value holding a reference to a region value and outliving the arena gets a dangling pointer. There is NO runtime guard — the escape check (P2a) is the ONLY safety mechanism, which makes it load-bearing for CORRECTNESS, not just ergonomics.
- (b) region value sent in a message to a fiber: NOT a hole — send already deep-copies BEAM-style. BUT verify the copier produces rc=1 and does NOT propagate INT32_MAX (else the copy is immortal = leak). Gate with a send fixture under KAI_TRACE_RC.
- (c) reuse-in-place over a region cell: covered for FREE by the sentinel — kai_check_unique returns 0 for rc==INT32_MAX so Perceus in-place reuse auto-disables on region values with no pass change. Nothing to do in v1.
- deep-copy-out must stamp rc=1, never INT32_MAX — a copy that inherits the sentinel never frees (leak).
- Wrong-codegen silent leak: a non-region value mistakenly arena-allocated leaks permanently and is INVISIBLE to ASAN (no free = no use-after-free). Only the dedicated arena-alloc counter in kai_rc_report() surfaces it.

## Gates (all phases)

- tier1-ASAN is MANDATORY on every phase that adds or exercises a free path (P0, P2a, P1). This session learned the bug-class the hard way: #697/#703 use-after-drop were caught ONLY by ASAN, not plain tier1. Regions add a NEW bulk free path (kai_arena_free); ASAN coverage of it is non-negotiable.
- build-release.sh LLVM smoke is MANDATORY on P0 (helper must link from both backends) and P1 (lowering switch). This session's unboxed-impl-under-boxed-sig bug was SILENT on tier1 and only caught by build-release.sh — exactly the C/LLVM emitter-parity gap class (#618/#558/#714).
- C/LLVM parity: arena alloc must be in BOTH emitters via the SAME C helper called over the runtime-helper FFI path. Do NOT implement a separate LLVM bump-alloc — the divergence risk outweighs any throughput gain.
- Explicit arena-alloc counter (distinct from kai_rc_alloc_total) surfaced in kai_rc_report(), so a wrong-codegen silent leak (which ASAN cannot see) is visible as alloc/free divergence.
- KAI_TRACE_RC bookkeeping must balance: kai_rc_live_now adjusted by the per-arena count on sweep so leaked-vs-iterations gates do not report false leaks.
- selfhost byte-identical on P2a and P1 for all non-region code.
- Regression fixtures: 4 escape-site fixtures (negative .err.expected + positive .out.expected) in examples/perceus/, named in the closing PR per lane-discipline.

## asu/linus disagreement + resolution

The only real disagreement is scope-now. asu's plan starts the surface (P2) and escape-analysis (P3) phases immediately in parallel with #86, landing the runtime phase (P1) at #86 merge — i.e. commit to the whole feature now, just ordering by file-overlap. linus CONDITIONALLY APPROVES P0 as a standalone PR and REJECTS P1+P2 until P0's benchmark is in hand, on the grounds that the self-compile bottleneck is distributed RC churn (not a measured lexer/parser hotspot) and the typer investment (building TyBranded from scratch through 17k lines of infer.kai) could be paid for a sub-percent win. RESOLUTION: linus wins, on data grounds. Everything else they AGREE on: block not effect, INT32_MAX sentinel reuse (decref no-op for free + reuse-in-place auto-off for free), side-table escape analysis NOT a Ty extension, TyBranded does not exist and is the deferred prerequisite for precise escape, deep-copy-out as the conservative v1 border policy, and the #86 file-overlap dependency. One additional correction linus makes to asu's PHASE ORDER that I adopt: asu's phase 3 (escape analysis) would land AFTER surface+lowering; linus sequences the escape check (on SAFE normal-alloc lowering, P2a) BEFORE the arena-alloc switch (P1), which closes the use-after-free window structurally. So the synthesized order is asu's mechanics + linus's seam (escape-check-before-arena-switch) + linus's bench gate before committing to the surface half.

## First lane brief (P0 only — launch AFTER #86 merges)

LANE BRIEF — issue #120 Phase P0: runtime bump-arena primitive + sentinel decref-no-op (runtime only, NO surface, NO typer).

PRECONDITION: This lane launches ONLY AFTER the #86 (contract panic value) lane MERGES. #86 is currently editing stage0/runtime.h + stage2/compiler/emit_c.kai + emit_llvm.kai + llvm_shim.c; P0 touches runtime.h and the emitters' helper-declaration surface, so starting before #86 merges will collide. Before first push: git log HEAD..origin/main to confirm #86 is in, then rebase.

GOAL: a standalone runtime bump-arena allocator proving the performance primitive at the C level BEFORE any language surface or typer investment. No parser, no AST, no infer.kai, no region{} syntax in this lane.

WHAT TO BUILD (all in stage0/runtime.h):
1. KaiArena struct: a bump pointer + a grow-only linked list of chunks (~64KiB each). malloc-backed, NOT mmap — stage0 must build on any ANSI cc with zero deps; do not add an mmap dependency.
2. kai_arena_alloc(KaiArena* a, tag): bump-allocate inside the current chunk (grow a new chunk if full), fill the SAME KaiValue header that kai_alloc fills, and stamp rc = INT32_MAX. This is the existing saturation sentinel — confirmed live at kai_decref (runtime.h:2116 `if (v->rc == INT32_MAX) return;`), kai_incref (1376), and kai_check_unique (2734 returns 0 for it). So decref is already a no-op on arena values, reuse-in-place already auto-disables on them, with ZERO new branches. Do NOT add a region-id field to KaiValue and do NOT add an arena-membership branch to decref.
3. kai_arena_free(KaiArena* a): free every chunk in bulk WITHOUT walking values.
4. A SEPARATE counter for arena allocations, distinct from kai_rc_alloc_total. On kai_arena_free, subtract that count from kai_rc_live_now so KAI_TRACE_RC leaked-vs-iterations gates balance after the sweep. Surface the arena-alloc count in kai_rc_report() so a wrong-codegen silent leak (a non-region value mistakenly arena-allocated leaks permanently and is INVISIBLE to ASAN, since nothing is freed) shows up as alloc/free divergence.

DELIBERATELY OUT OF SCOPE THIS LANE: region{} surface, AST nodes, escape analysis, TyBranded (does not exist — comments only at infer.kai:14745/14820, desugar.kai:3912/3921 — and is NOT needed for P0), deep-copy-out, both-emitter lowering. Those are later phases gated behind a benchmark.

ACCEPTANCE GATE (all mandatory):
- C-level fixture: allocate 10,000 values into an arena; assert kai_rc_free_total does NOT increment when the arena is freed; assert kai_rc_live_now returns to baseline after the sweep; assert the arena memory is released.
- Microbench in the fixture: N arena allocs + 1 arena_free vs N calloc + N kai_decref.
- tier1-ASAN MANDATORY (new bulk free path — this session's #697/#703 use-after-drop bugs were caught ONLY by ASAN).
- build-release.sh LLVM smoke MANDATORY (the helper must be linkable/callable from both backends even though no lowering uses it yet — this session's unboxed-impl-under-boxed-sig bug was silent on tier1, caught only by build-release.sh).
- selfhost byte-identical (P0 adds dead-but-linked runtime code; the compiler output must not change).
- Regression fixture committed (the C-level arena fixture) and named in the closing PR per lane discipline.
- Lane retro: docs/lane-experience-issue-120.md (P0) BEFORE gh pr create.

PROCESS: open a PR via gh pr create once the gate is green. NO auto-merge — do NOT run gh pr merge (including --auto). The integrator merges only after CI is FULLY green. If CI fails, fix on the same branch and push again (force-push fine for the unreviewed lane branch).

KEY FILE REFS: stage0/runtime.h — kai_alloc ~1314, INT32_MAX singleton pattern to copy 1358-1373, kai_incref short-circuit 1376, kai_decref short-circuit 2116, kai_check_unique 2734, RC counters/kai_rc_report (grep kai_rc_alloc_total / kai_rc_live_now / kai_rc_report).
