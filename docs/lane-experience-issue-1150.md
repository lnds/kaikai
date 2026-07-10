# Lane experience — issue #1150: Vec surface stage 3 (slices, minting, collect)

Closes #1150 and with it the #1135 staged plan: `[h, ...t]` binds an
O(1) slice view, list literals mint to `Vec` in typed context, and a
pure Vec-typed range pipe chain collects into one pre-sized buffer.

## Scope as planned vs as shipped

Planned (the issue's three pieces): seamless slices with documented
pin/sharing semantics and `xs[a..b]` producing the same form; literal
minting in annotation and argument position with the bare literal
staying a cons list; fused collect with capacity from a known range
head. All shipped, plus the surface the pieces implied: `xs[i]` on a
Vec routes to `vec_get` (it was a type error before), the parser
accepts a range inside index brackets, and `vec.slice`/`vec.reserve`
join the stdlib module (forwarders, so #1153's inliner collapses them).

Restrictions shipped as *diagnosed* boundaries, not silent gaps: Vec
match arms take simple binder heads only (a literal/varial head needs
arm fallthrough an if-chain cannot express), no guards on Vec arms,
no spread in a Vec literal. Each rejects with a typed error naming the
workaround.

## The load-bearing decision — typer rewrite, not emitter machinery

The brief pointed at `kir_lower_match` (the list-arm precedent). Both
backends' match machinery models arms as cons-cell walks with
borrow-projected binds and #858's structural alias increfs; a vec arm
has different tests (length, once), different binds (owned values out
of `vec_get`/`vec_tail_from`, not borrows), and would have had to be
mirrored in `emit_c` AND the KIR lowering while keeping the reuse
recogniser and tcrec from misreading `PList` arms over a non-cons
scrutinee.

Instead the typer rewrites the whole match into an if-chain of
`vec_length` tests + `vec_get`/`vec_tail_from` binds (`vec_surface.kai`,
hooked from `synth_match`/`check_match`). Every downstream pass sees
only standard lets, ifs and prims — zero changes in perceus, tcrec,
reuse, or either emitter. The precedent is strong: pipe dispatch,
fusion, numeric minting and index dispatch are all typer rewrites.

The slice runtime is a fourth field on the vec node (`view_of`): a
view holds one strong ref on the owner, `data` points at its first
element, and the two accessors (`kai_vec_meta`/`kai_vec_elems`) are
the only sanctioned block access, so every existing read path works on
views untouched. `kai_vec_ensure_unique` treats any view as shared —
the write-side soundness is one added disjunct. The descent lever:
consuming a UNIQUE view re-slices the node in place (no allocation),
so a `[h, ...t]` walk over 1000 elements allocates ~2 nodes total.

## Structural surprises

1. **Perceus keys RC decisions on node (line, col) identity.** The
   first desugar gave every generated scrutinee use the same position;
   last-use resolution picked an arbitrary one, the tail bind got a
   conservative dup with no balancing drop, and the descent leaked one
   view per level (~1000 nodes). The comment in perceus says it
   plainly: "the (line,col) identity of AST nodes makes a false move
   impossible" — an invariant any typer-side desugar MUST uphold. The
   builder now stamps a distinct per-role column offset on each use.
2. **The zero-alloc descent needed the scrutinee used DIRECTLY.** An
   alias let (`let __vecm = v`) is a multi-use binding, and perceus
   dups every read of those; only fn params ride the skip/borrow-move
   sets. With a bare `EVar` scrutinee the chain uses the param itself:
   the borrowed `vec_length`/`vec_get` reads discount to zero, the
   consuming `vec_tail_from` is the single real consumer, and the
   #1126 borrow-move rule moves it raw — unique view, in-place
   re-slice. Complex scrutinees (or an arm binder shadowing the name)
   keep the alias and the conservative dups: correct, just not
   zero-alloc.
3. **The typed-chain discipline bit once more (#1143's lesson).** The
   Vec literal chain was first built raw and synthesised once; the
   synth re-derived `65 : Int` and broke `Vec[Byte]`. The chain is now
   built fully typed from the checked elements (the composed-lambda
   discipline) and never re-synthesised.
4. **One-line match arms are index-ambiguous.** `[] -> 0  [h, ...t]`
   on one line parses `0[h, ...` as a postfix index — pre-existing
   (the list path errors the same way), surfaced here because the new
   range-index parse reports a different message. Multi-line arms are
   the norm; not touched.

## Measured

Fixtures (exact, gated): descent over 1000 elements `vec_cow=0`,
alloc_total=17 (a per-level regression lands >1000); pin/sharing
`vec_inplace=2 vec_cow=2`; collect at 100k `alloc_total=45`. Native
matches C on every counter.

Collect wall (N=10M, macOS arm64 M-series, -O2, warm best): the
`vec_collect_point` bench (idiomatic `let v: Vec[Point] = [1..n] |
(k => Point{..})`) runs 0.37 s / 154 MB (C) and 0.40 s / 154 MB
(native) vs the explicit `vec_push` loop's 0.11 s / 155 MB. Memory
parity holds (one buffer, zero intermediates — the issue's claim);
the wall gap is NOT list materialisation: the stage closure boxes one
record per element and calls indirectly, where the explicit loop's
literal-record push rides the #1147 caller-side raw fuse. `Vec[Int]`
shows the honest floor: collect 0.13 s vs loop 0.11 s at identical
77 MB. Closing the Point gap needs stage-body inlining into the push
site (hygienic beta-reduction, rejected in #1143) or emitter-level
closure devirtualisation — follow-up, not this lane.

## Fixtures added (wired: TEST_LIGHT_TARGETS, tier1-native.yml, tier1-asan)

- `examples/perceus/vec_slice_descent_1150.kai` — `[h, ...t]` descent
  O(1)/level (alloc<50, cow=0), exact/one/rest arm shapes, empty vec.
- `examples/perceus/vec_slice_pin_1150.kai` — pin + sharing-disables-
  in-place + write-through-slice + unshared write, exact counters.
- `examples/perceus/vec_mint_1150.kai` — annotation, argument
  position, bare-stays-cons, Byte element retag, `xs[i]`, `xs[a..b]`.
- `examples/perceus/vec_collect_1150.kai` — fused range collect
  (alloc<100 at 100k), multi-lambda chain, stepped range, list-source
  and impure-stage fallbacks through `vec_from_list`.
- Targets: `test-perceus-1150-vec-surface` (+`-native`, `-asan`);
  bench twin `tools/native-perf/benches/vec_collect_point.kai`.

## Cost vs estimate

The runtime slice mechanism and the collect landed close to plan. The
unbudgeted half was the perceus interaction of the match desugar: two
full diagnose-fix cycles (position identity, alias-vs-direct) driven
by counter goldens and emitted-C reads. The stage-1/2 retros' counter
discipline is what made both regressions visible in minutes.

## Follow-ups left

- Collect for record stages boxes per element (0.37 s vs 0.11 s at
  10M); needs stage inlining into the push site or closure
  devirtualisation (the #1143 native-lambda finding, same root).
- Discriminating heads on Vec arms (`[0, ...t]`, `[Some(x), ...]`)
  would need a decision-tree lowering with fallthrough; diagnosed as
  unsupported today.
- `list.map_collect_vec` (pure list-source chains still materialise
  the mapped list before `vec_from_list`; a fused list walk would
  collect directly).
- stage0/runtime.h carries the full slice mirror; nothing in the
  bootstrap chain calls it yet (the compiler's own source uses no Vec
  surface).
