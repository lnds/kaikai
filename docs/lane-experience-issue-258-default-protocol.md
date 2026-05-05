# Lane experience — issue #258 (`Default` protocol + 6 impls)

Wave 11 of plan A. Adds the sixth stdlib protocol (`Default`) plus
six canonical impls (`Int`, `Real`, `Bool`, `String`, `[T]`,
`Option[T]`) on top of #175 (polymorphic Show/Eq/Ord/Hash).

## Objective metrics

| Metric | Value |
|---|---|
| Files modified | 2 (`stdlib/protocols.kai`, `docs/protocols.md`) |
| Files added | 4 (2 fixtures + 2 goldens) |
| stdlib lines added | +49 |
| docs lines added | +44 |
| Tier 0 elapsed | 48 s |
| Tier 1 elapsed | 322 s |
| Tier 1-ASAN elapsed | 52 s |
| selfhost-llvm elapsed | 44 s |
| Iterations to byte-identical selfhost | 1 (selfhost is bundled in tier0) |
| New fixtures | 2 (`default_basic.kai`, `default_polymorphic.kai`) |

## Impl shapes (the six trivial impls)

The four primitives reduce to a literal each — `0`, `0.0`, `false`,
`""`. The two polymorphics return the empty list / `None`. None of
the bodies recurse on the type variable, so per issue #174 case 3
the `T : Default` bound is dropped on the polymorphic impls; this
matches `impl[T] Show for [T] { fn show(xs) = "list-of-something" }`
in `examples/protocols/poly_impl_no_recurse.kai`.

`Result[E, T]` is intentionally absent — neither `Ok(default()) :
Result[_, T]` nor `Err(default()) : Result[E, _]` is a privileged
default, so the issue spec calls it out as out of scope and the
stdlib comment documents the omission for future readers.

## Empirical verification (the two acceptance tests)

`examples/protocols/default_basic.kai` exercises the four primitive
impls. Each call is pinned by an annotation: `let i : Int =
default()`, `let r : Real = default()`, etc. The golden lines
(`0`, `0`, `false`, `[]`) read out the four canonical defaults
through `int_to_string` / `real_to_string` / a bool branch / a
bracketed string echo.

`examples/protocols/default_polymorphic.kai` exercises both
polymorphic impls at two distinct `T` instantiations each: `[Int]`
+ `[String]` for `[T]`, `Option[Int]` + `Option[[Int]]` for
`Option[T]`. The instance ranking covers both monomorphic primitive
inner types and a nested polymorphic inner type, validating the
"body never recurses on T" claim from #174 case 3 against a
realistic generic instantiation.

Both fixtures pass on the C and LLVM backends (`test-protocols`
diffs both backends against the same `.out.expected`).

## Friction points

The original briefing called for a third fixture exercising `fn
make_one[T : Default]() : T = default()` to demonstrate the generic
helper pattern. **That shape does not compile in v1**: free-function
type-parameter bounds reject anything other than `Type` / `Measure`
kinds (`stage2/compiler.kai:8137` raises `type-parameter kind must
be 'Type' or 'Measure'`). Protocol-aware tparam bounds exist only
inside `impl[T : ...]` headers today, mirroring the same v1 gap
that `stdlib/math/numeric.kai:20` documents for `min[T : Ord]`.

The lane therefore replaces the "generic helper" fixture with a
"polymorphic impls at two T instantiations" fixture, which exercises
the full dispatch path (parser → resolver → impl-site bound → mono
→ codegen) under realistic generic input without depending on a
feature kaikai does not yet ship. The stdlib comment block and the
new `docs/protocols.md` subsection both name the gap explicitly so
the next protocol-aware-bounds lane can pick it up directly.

No selfhost iteration was required: tier0 bundles `selfhost`, the
first run was byte-identical, and `selfhost-llvm` matched on the
first attempt.

## Subjective summary

A clean, mechanical lane on top of the #175 substrate. The three
hard calls were all spec-driven (drop the `T : Default` bound,
exclude `Result`, defer the generic-helper fixture to a future
free-fn-bound feature) and the implementation was four `impl Foo
for Bar { fn baz() = lit }` lines plus two polymorphic siblings.
Most of the lane time went to verifying that v1 dispatch could
resolve return-type-only Self via annotation (it can — same path
as `Serialize.from_string` and `From[a].from`) and to writing the
docs subsection that names what is *not* yet possible.

## Limitations

- **No `Default for Result[E, T]`.** Out of scope per issue #258 —
  no canonical default exists.
- **No auto-derive for user records.** `#derive(Default)` belongs
  to a separate feature; users `impl Default for MyRecord` by hand
  in v1.
- **No separate `Zero` protocol.** `Default` covers numeric zero
  and the broader "canonical default" niche; introducing `Zero` as
  a distinct (`Default`-compatible) protocol is a future call once
  we see whether numeric code wants to demand `Zero` specifically.
- **No `make[T : Default]() : T` generic helper.** Blocked on
  protocol-aware free-fn tparam bounds (the same gap that gates
  `min[T : Ord]`, `max[T : Ord]`, generic accumulators). The
  protocol still ships now because the use-site annotation pattern
  (`let x : T = default()`) carries its weight without the helper.
- **No `clear[T : Default](c: Array[T])` cleanup of `uira/boids.kai`.**
  Same blocker; the lane scope explicitly defers that migration.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-05T14:25:45-04:00	tier0	OK	48
2026-05-05T14:31:14-04:00	tier1	OK	322
2026-05-05T14:32:12-04:00	tier1-asan	OK	52
2026-05-05T14:33:06-04:00	selfhost-llvm	OK	44
```
