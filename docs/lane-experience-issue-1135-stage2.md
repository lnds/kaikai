# Lane experience — issue #1135 stage 2: Vec[T] raw element paths

Advances #1135 (does NOT close it — stage 3 remains: seamless slices,
literal minting, fusion-collect). Scope of this lane: the wall-clock
side of the flat vector — raw get/set/push on the unboxed carriers, so
an element read compiles to a direct byte load and a write to direct
stores, without a boxed record crossing the prelude boundary per
access.

## Scope as planned vs as shipped

Planned: borrow reads for `vec_get`/`vec_length` (the #1120 move,
verbatim); fused raw paths for the stage-1 unboxed carriers on both
backends behind ONE classifier (#1110 discipline); the uniqueness
decision never skipped on any raw write. All shipped:

- `vec_get`/`vec_length` borrow variants seeded in Perceus
  (`pcs_prim_borrow_variant` + positions + seed list), runtime
  `kai_prelude_vec_get_borrow`/`_length_borrow`, EP entries, prelude
  names, `kaix_` shims.
- Fused REC-carrier paths: `vec_get(v, i).f` →
  `kai_vec_get_field(_borrow)(v, i, fidx)` (column load, no record);
  `vec_push(v, Rec{..})` → `kai_vec_push_rec_raw` and
  `vec_set(v, i, Rec{..})` → `kai_vec_set_rec_raw` (unpacked scalar
  stores, record never allocated). C emitter fuses at `EField`/
  `emit_call_expr`; the native backend fuses in the KIR lowering
  (`kir_lower_walk`) and emits mixed-signature calls in `nemit_prim` —
  runtime bitcode (P2) inlines them into the loop.
- The ONE classifier: `compiler/vec_raw.kai` (km A++), an exact static
  mirror of `kai_vec_classify` — REC carrier iff a known record of
  1..8 concrete raw-scalar fields. Conservative by exclusion; the raw
  helpers trap on ekind/arity/tag mismatch as the last line of defence.
- RAW-scalar (`Vec[Int]`) needed no fused path: the borrow read plus
  the P2-inlined helper already is the direct load (tagged Ints box
  for free), and scalar writes never allocated.

Unplanned but forced (see below): a tcrec fix — a module-qualified
self-call (`EModCall(own_mod, nm)`) is now recognised and rewritten to
the goto in the gate, the rewrite, and the rule-3 dropmask collector.

## Measured — fill + sum, N = 10M, macOS arm64 M-series, -O2

Process wall (`/usr/bin/time`), first run (cold) and steady (warm):

| bench                    | backend | before (this machine) | after cold | after warm | RSS    |
|--------------------------|---------|-----------------------|------------|------------|--------|
| `Vec[Point]` fill+sum    | C       | 1.00 s                | 0.34 s     | **0.04 s** | 155 MB |
| `Vec[Point]` fill+sum    | native  | 0.80 s                | 0.61 s     | **0.04 s** | 155 MB |
| `Vec[Int]` fill+sum      | C       | 0.39 s                | 0.34 s     | **0.05 s** | 79 MB  |
| `Vec[Int]` fill+sum      | native  | 0.32 s                | 0.60 s     | **0.05 s** | 79 MB  |
| `Array[Point]` boxed     | C       | —                     | 0.92 s     | 0.61 s     | 853 MB |
| `Array[Point]` boxed     | native  | —                     | 0.91 s     | 0.66 s     | 853 MB |

Reference points from the issue (2026-07-08, same shape): Rust ~0.01 s
/ 154 MB; Koka value-struct ~0.09 s / 308 MB; the issue's Array-boxed
baseline 0.38 s / 885 MB. Where we land, stated exactly: warm workload
0.04–0.05 s sits between Rust (0.01) and Koka (0.09) with the best RSS
of the three managed rows; the cold first-run numbers (0.34 C /
0.61 native) are dominated by a fixed process-start cost (identical
for the Int and Point benches), not by the vector paths. Against this
machine's own Array baseline the warm win is ~15× wall and 5.5×
memory. The wall gate ("better than the 0.38 s baseline at minimum")
is met on both backends on workload; the native cold number is the
one honest caveat — 0.61 s > 0.38 s cold-vs-cold against the issue's
figure, and its start-up constant is a pre-existing native-binary
property visible on every native bench, not a Vec regression.

Benches live in `tools/native-perf/benches/vec_fill_sum_point.kai` /
`vec_fill_sum_int.kai`.

## The soundness centre — raw moves ride the same uniqueness decision

Only the element MOVE is raw. Every write path (boxed or fused) now
passes through one extracted runtime step, `kai_vec_ensure_unique`
(unique → in place, shared → clone; the same counters), before
touching bytes. The `vec_rec_raw_fusion_1135` fixture pins the shared
case: a raw `vec_set` through a second binding copies exactly and the
original reads back intact, with `vec_inplace=101 vec_cow=2` exact
under `KAI_TRACE_RC`. Stage-1 fixtures pass unmodified with their
exact counters (`vec_inplace=150 vec_cow=0` / `vec_cow=1`).

Field-index trust: the fused paths read/write columns by declaration
order — the same bet `kai_op_field_at` (the constant-index record
read) already makes system-wide, post-desugar canonicalisation. The
helpers check ekind/arity/per-field scalar tags and trap loudly on any
mismatch; the PENDING (empty) vector classifies REC directly from the
unpacked fields on its first raw push, stamping head tag 0 exactly as
`kai_record` does for literals.

## Structural surprises

1. **The borrow seed exposed a live miscompile in stdlib `vec.map`.**
   Seeding `vec_get` as a borrowing prim made `map_loop`'s container a
   #1126 borrow-move param (no dup on the re-thread + exit drop on the
   base arm) — a plan that is only sound when the self-tail-call is a
   GOTO. In `collections.vec` it was NOT: `map_loop` is a poly name
   shared with `collections.list`, so the monomorphiser retargets its
   bare self-call to `EModCall(vec, map_loop)` — and tcrec only
   matched `EVar` self-calls. Result: the exit drops ran after the
   real recursive call returned, over-releasing the mapped vector
   (use-after-free), plus per-element C-stack growth — a mandatory-TCO
   violation that predates this lane for every module-shared poly loop
   name. Fixed in tcrec: the gate (`tcrec_walk_tail`), the rewrite
   (`tcrec_rewrite_kind`, sentinel plant shared via
   `tcrec_make_step_site`), and the rule-3 dropmask collector
   (`tcrec_collect_dm_kind`) all recognise the same-module qualified
   shape; `own_mod` is threaded through the rewrite. Regression
   fixture: `vec_stdlib_map_qualified_tco_1135` (over-release read +
   200k-deep map).
2. **The bundle-shadow trap struck the new classifier.** A pattern
   binder named `args` in `vec_raw.kai` resolved to the prelude `args`
   builtin (CLI args) inside the bundle — the classifier silently
   returned false and the fusion never fired, with zero diagnostics.
   Renamed to `targs`. (Same class as the known stage-1
   bundle-shadows-prelude trap; the lesson: never name a binder after
   a prelude builtin in compiler sources.)
3. **`vec.map` copies per element today** (`vec_cow` scales with N in
   the map fixture): the `out` re-thread through `vec_set`'s owned
   slot still dups under the conservative plan, so every write clones.
   Pre-existing (stage-1 shape, not introduced by the seed) and
   invisible to wall gates here; the real fix is stage 3's
   fusion-collect into a unique pre-sized buffer, which replaces the
   map_loop shape entirely.
4. `kai_record_h` (the head-stamping record ctor) turned out to be
   emitted by nobody — record literals always stamp head tag 0, so the
   raw push takes no head-tag parameter and stamps 0, staying
   bit-identical with the boxed classify path.

## Fixtures added (wired; native variants in tier1-native.yml)

- `examples/perceus/vec_rec_raw_fusion_1135.kai` — fused push/set/
  get-field round-trip, PENDING classify through the raw push, boxed
  full-record read of raw-written slots, shared raw set copies with
  original intact; exact counters gate (`vec_inplace=101 vec_cow=2`).
- `examples/perceus/vec_stdlib_map_qualified_tco_1135.kai` — the
  tcrec/borrow-move regression: original vector survives `vec.map`,
  and a 200k-deep map stays on O(1) stack.
- Makefile: `test-perceus-1135-vec-raw` (TEST_LIGHT_TARGETS) +
  `test-perceus-1135-vec-raw-native` (tier1-native.yml step).

## Cost vs estimate

The mechanism itself (runtime helpers + one classifier + two emitter
fuse sites + native KPrim cases) was the planned size. The unplanned
majority cost was the stdlib `vec.map` corruption chase: it presented
as a broken stage-1 fixture, reduced only under module import, and
took a C-emit diff between an inline and a module copy of the same
function to see the missing goto. The bundle-shadow miscompile of the
classifier cost one more instrument-rebuild-diff cycle.

## Follow-ups (stage 3 of #1135, noted not filed)

1. Fusion-collect (#1134 integration): fused pipe chains collecting
   into a unique pre-sized Vec — also deletes the per-element CoW in
   the `vec.map` shape (surprise 3).
2. Seamless slices `[h, ...t]`; literal minting (the staged plan).
3. `Vec[Real]` fused reads box per access (`kai_real` allocates);
   a raw f64 KIR slot for the fused read is mechanical if a workload
   needs it.
4. The #1126 borrow-move plan remains goto-coupled: it is only sound
   when tcrec rewrites the re-thread. Today the two decisions agree by
   construction (tcrec now sees every direct self-call shape Perceus
   models), but a shared predicate would make the coupling structural
   rather than coincidental — a candidate refactor when the next
   borrow-plan lane opens the file.
