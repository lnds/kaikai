# Lane experience — issue #1153: module-call consumer linearity

Fix lane for the real case-6 blocker: the idiomatic Vec surface
(`import collections.vec`, `vec.push` / `vec.set` in recursive loops)
was a complexity class more expensive than the builtin spelling —
CoW per operation, O(n²) bytes, 10M elements intractable.

## Scope as planned vs as shipped

Planned: extend `pcs_call_consumers_linear` so a resolved module-fn
callee is a linear consumer (the issue's lever), flip the #1151
stress fixture's module regime to in-place, land the case-6 gate.

Shipped: that fix — **plus a second, unplanned pass**. The linearity
fix alone flipped CoW→in-place (0.63 s → 0.01 s at 100k) but left the
10M idiomatic bench at 0.61 s vs the builtin's 0.08 s: the stdlib
wrapper boxes every element at the call boundary, and the caller-side
raw fuses (`vec_push(v, Rec{..})` → `kai_vec_push_rec_raw`, borrow
reads) never see the primitive's call shape behind `EModCall`. The
gate was defined on wall clock, so the lane also shipped
**trivial-forwarder inlining** (`stage2/compiler/fwd_inline.kai`):
a saturated qualified call to a pub module fn whose body is exactly
`prim(p1,…,pn)` (own params, in order, no `^`, no declared row)
rewrites to the direct primitive call, post-typer / pre-monomorph.
The vec surface (`make/empty/get/set/push/length`) collapses to the
builtin spelling and every existing fuse applies unchanged.

## The linearity verdict — why it is one line, and why that is sound

`EModCall` only exists after the qualified-call rewrite proved the
module is in the table and the name is in its export list
(`modules.kai`, rule B). Export collection admits `pub fn`, types +
ctors, effects, protocols, axioms, units — but effect OPS and
protocol METHODS are not exports, so a lowercase `EModCall` callee
can only be a real kaikai fn (or FFI shim), all callee-consumes by
construction, exactly like the bare `EVar` user-fn arm the classifier
already trusted. The old arm's "capability ops internally incref"
fear was unreachable: `alias.op(x)` stays an `EField` (a local alias
shadows imports) and falls to the conservative `_ -> false` arm.
Borrowed callee slots need no special case here: `pcs_callee_key`
already produces the qualified `"mod.fn"` key, so the borrow/consume
maps cover the qualified shape downstream.

The #1147 one-verdict lesson resolved itself: the classifier is one
pure syntactic predicate used by both sides of the tcrec rewrite
(pcs_pass skip-set and the emit-side re-derivation), the #680 dup-wrap
strip already normalises the post-perceus body, and
`tcrec_widen_skip_raw_reads` covers the reuse-recogniser divergence.
No second classifier existed to disagree with.

## Design decisions

- **Forwarder inlining over emitter-side EModCall fuses.** Matching
  `EModCall("vec","push")` in the emitters would hardcode a stdlib
  module name into codegen and would still box the element (the fuse
  must fire in the CALLER, before the arg crosses the wrapper ABI).
  The structural criterion (body forwards own params in order to a
  name no program DFn declares) is general: any user module gets the
  same collapse, and a module-local helper can never be misread as a
  primitive (it would not resolve in the caller's namespace).
- **Pre-monomorph placement.** One `monomorphise` call site exists,
  so one insertion point covers every mode (whole-program, modular,
  test). Inlining first also stops mono from specialising wrappers
  nobody references anymore.
- **Value uses keep the wrapper.** Only saturated `ECall` sites
  rewrite; `vec.push` passed as a value still materialises the fn.
- **`dsg_map_expr_kind` made pub** rather than a fourth hand-rolled
  Expr walker (the ExprKind-sweep trap).

## Measured — the case-6 gate (N = 10M, macOS arm64 M-series, -O2, warm best of 3)

| bench                        | backend | wall   | RSS    |
|------------------------------|---------|--------|--------|
| builtin `vec_push`/`vec_get` | C       | 0.08 s | 155 MB |
| builtin `vec_push`/`vec_get` | native  | 0.08 s | 155 MB |
| idiomatic `vec.push`/`vec.get` | C     | 0.08 s | 155 MB |
| idiomatic `vec.push`/`vec.get` | native | 0.08 s | 155 MB |

Output exact (50000005000000) on all four. Pre-lane the idiomatic
spelling was intractable at 10M (O(n²) CoW); with linearity alone it
ran 0.61 s (linear, but boxed per element); with inlining it is
indistinguishable from the builtin. The #1151 stress fixture's module
regime flipped to `vec_inplace=200000 vec_cow=0` (was 100000/100000).

## Structural surprises

1. **The issue's lever was necessary but not sufficient for the
   wall-clock gate.** Linearity removes the copies; the wrapper ABI
   still boxes. Two independent silent-routing taxes stacked on the
   same spelling.
2. **The conservative EModCall arm guarded against a shape that
   cannot occur.** Effect ops never become EModCall — the comment
   claiming so predated the export-collection rules and cost this
   surface a complexity class for months.
3. **With inlining live, the vec fixtures stopped exercising the
   linearity arm** (their qualified calls collapse to prims before
   perceus runs). The regression fixture pair therefore includes a
   deliberately non-trivial module fn (block body) that survives
   inlining and pins the EModCall verdict itself.

## Fixtures added (wired)

- `examples/perceus/modcall_linear_1153.kai` + sibling module
  `modcall_step_1153.kai` — non-forwarder qualified call threading a
  vec through a recursive loop; harness asserts `vec_inplace=2000
  vec_cow=0` exactly.
- `examples/perceus/vec_module_surface_1153.kai` — the idiomatic
  surface (make/empty/push/set/get/length, Int + record) with the
  same exact counters, plus a C-side grep that the emitted C reaches
  `kai_vec_push_rec_raw` (proof the inline exposed the caller fuse).
- Targets: `test-perceus-1153-modcall-linear` (TEST_LIGHT_TARGETS),
  `-native` (tier1-native.yml step), `-asan` (root tier1-asan chain).
- Perf twin `tools/native-perf/benches/vec_fill_sum_point_mod.kai`
  for the case-6 table above.

## Cost vs estimate

The linearity fix itself was the one-line change the issue predicted,
and the delicate seam (skip-set ↔ tcrec dropmask ↔ emit) absorbed it
without any alignment work — the widen + dup-strip machinery from
#680/#1147 already made the verdict single-sourced. The unbudgeted
half was the forwarder inliner (new pass, ~150 LOC + wiring), which
consumed most of the lane. Reading the export-collection code before
touching perceus is what made the one-liner defensible.

## Follow-ups left

- Forwarders to *user* fns (callee declared by a DFn) are not
  inlined; only primitive forwarders. Extending to same-program fn
  targets needs a namespace-correctness argument for the rewritten
  callee and showed no measured need.
- `vec.from_list` / `vec.map` / `vec.foldl` are real bodies (loops,
  HOF) — they benefit from the linearity verdict but not from
  inlining; if a bench ever pins them, that is fusion work (#1134's
  lane), not forwarder work.
