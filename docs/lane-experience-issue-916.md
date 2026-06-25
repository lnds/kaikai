# Lane experience — issue #916: index the protocol impl table

## Scope as planned vs as shipped

**Planned.** Index the immutable protocol impl table at the
`lower_protocols` boundary so impl lookups are O(1) instead of linear
scans of `reg.impls`. The brief framed the linear scan as recoverable
overhead "mis-charged to the inherent 23.6% synth bucket", running
"per ECall/spec … in every source decl AND every monomorphised spec →
O(specs × calls × impl_table_len)".

**Shipped.** The index itself, exactly as designed: a hash table over
`(proto, type)` plus an `impl_fn_name` table, built once at the
boundary, threaded through `ProtocolReg`, consulted by every
`protos.kai` scan with a linear fallback. selfhost byte-id is green on
both backends. **But the measured throughput win is zero**, because
the premise does not hold for the workload that matters — see below.

## The measurement that reframed the lane

Before trusting the index would help, I counted how often the indexed
scans actually fire during a self-compile (`kaic2 main.kai`, the
largest real program, 55 modules):

- **`reg.impls` holds ~105 entries.** Confirmed by an `eprint` trace
  at the boundary.
- **`lower_protocols` runs once per compile.** So building the index
  is cheap and one-shot — the placeholder-then-fill design is right.
- **The indexed scans fire ZERO times during the self-compile.** An
  `eprint("SCANHIT")` in `impl_pool` / `prc_reg_has_impl` emitted
  nothing across the entire `kaic2 main.kai` run.

Why zero: the compiler's own comparisons are all `==` / `<` over
`Int` / `String` / `Bool`, which `prc_rewrite_cmp_binop` short-circuits
*before* the nominal-type scan; and the compiler declares no `impl P
for T` over its own record/sum types that would route a protocol-op
call through `find_impl_*`. The scans only fire for user code doing
custom nominal dispatch — `complex_heterogeneous.kai` produced 8
`impl_pool` hits, and with 105 impls that is ~840 comparisons total,
noise against a ~50 s compile.

Throughput, best-of-3, no parallel load:

| build                | `kaic2 main.kai` |
|----------------------|------------------|
| baseline (no index)  | ~50.0 s          |
| with index           | ~49.85 s         |

The difference is within noise. The self-compile's synth-bucket cost
is `mono/synth` + `ty_env_collect_candidates` O(n²) (already diagnosed
in #911), **not** the impl scan.

A trap worth recording: an early "baseline" of ~7.5 s was bogus — it
came from `kaic2 build/bundle.kai`, which fails immediately on
module-doc / import collisions (the bundle is a concat consumed by
kaic1, not by kaic2). The real self-compile entry point is
`kaic2 main.kai` (kaic2 resolves `import compiler.driver`); that has
always been ~50 s.

## Decision: ship anyway

With the measurement in hand the user chose to merge: the index is
correct, isolated, and future-proof. If the impl table grows by an
order of magnitude, or the cmp-binop fast path narrows, the index pays
off then with no further work. It is purely additive — `reg.impls`
stays the source of truth; the index is a side view with a linear
fallback when unbuilt. The honest throughput delta (≈0 today) is
recorded in the PR body, not dressed up.

## How equivalence to the linear scan was preserved

The index must return the *same* impl, same precedence, as the linear
first-match. `reg.impls` is prepend-built (`[entry, ...reg.impls]`), so
the newest impl is at the front and wins. The guarantees:

1. **Bucket order = `impls` order.** `ii_fill` folds `impls`
   front-to-back and *appends* (not prepends) to each bucket. So the
   first bucket entry matching a predicate is the first list entry
   matching it. (Prepending would have silently inverted precedence —
   the one mistake that byte-id would catch but the code would not
   announce.)
2. **Same fine predicate.** Each `find_impl_*` keys on `t == recv`
   (plus op / arg-names). The `(proto, recv)` bucket holds exactly the
   entries the linear scan would not skip, so running the *unchanged*
   `find_impl_*` over the bucket is the same algorithm on a smaller
   domain — not a second code path. `impl_pool(reg, proto, recv)`
   returns the bucket or, when unbuilt, the whole table; the callers
   are otherwise untouched.
3. **Hash collisions are transparent.** The `(proto, type)` key only
   selects a bucket; the fine `p == proto and t == head` filter
   inside disambiguates any collision, so a weak separator (`|`) is
   correct — it affects distribution, never the answer.

The proof obligation reduces to "first-match over a stable partition =
first-match over the whole list", which holds for a stable group-by.
selfhost byte-id on both backends is the end-to-end check: a wrong
dispatch would change emitted C and fail it. It passed.

## Structural surprises the brief did not anticipate

- **The hot path the brief described is cold.** The brief's O(specs ×
  calls × impls) is real *in principle* but the self-compile never
  enters it. Anyone re-opening impl-scan perf should target the typer-
  side `binop_pick_proto_dispatch` in `infer.kai` (its own scan of a
  raw `[ProtoImplReg]`, owned by lane #914) and the #911 O(n²) costs,
  not `protos.kai`.
- **`ProtocolReg` field, not a 44-fn thread.** The registry is
  consumed by `reg: ProtocolReg` in 44 functions but constructed in
  only 6 sites. Adding an `impl_index` field touches the 6; threading
  a parameter would have touched the 44. The field is the right shape.
  Intermediate construction sites carry the empty placeholder
  (`nbuckets = 0`); the real index is filled once at the boundary.
- **kaic1 constrains the new module's surface.** The bundle is still
  compiled by kaic1, which rejects triple-quoted strings inside
  attributes. The module doc had to be a `#`-comment block, not a
  `#[doc("""…""")]`; single-line `#[doc(...)]` only (same constraint
  `util.kai` already records). `bit_*` ops are also absent in kaic1, so
  the hash sign-clamp uses `((h % n) + n) % n`, not a bitmask.
- **`Mutable` did not leak to readers.** The builder rides `Mutable`
  (array writes), but lookups use `array_get` (pure), so the 44
  consumers stay pure. kaic1 typechecked the bundle with no effect
  error; the masking concern from #903 did not bite here because what
  escapes the builder is the finished value, not the write capability.

## Fixtures

No new fixture: the change is a behaviour-preserving internal index,
and the existing protocol/derive fixtures (`examples/stdlib/complex_*`,
the `test-protocols` / `test-proto-scalar-dispatch` suites) already
exercise every indexed scan. selfhost byte-id is the regression guard —
it fails the instant dispatch selection changes. Adding a fixture that
asserts "same impl chosen" would duplicate what byte-id already proves
end-to-end.

## Follow-ups for next lanes

- The synth-bucket cost is `mono/synth` + `ty_env_collect_candidates`
  O(n²) (#911), not the impl scan. That is where self-compile
  throughput is actually recoverable.
- `infer.kai`'s `binop_pick_proto_dispatch` scans a raw
  `[ProtoImplReg]` on the typer side. If a future profile shows *that*
  scan hot, the same `ImplIndex` could be reused there (it is already a
  `pub` module) — but measure first; this lane's lesson is that the
  scan is colder than it looks.
