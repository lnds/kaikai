# Lane experience — issue #1015 L2: `%{…}` / `%[…]` collection literals + spread

## Scope as planned vs as shipped

**Planned (brief):** parser support for `%{ k: v }` map literals, `%[ e ]`
set literals, empties `%{}` / `%[]`, and `...` spread, via **new AST
nodes `EMapLit` / `ESetLit`** carrying `base: Option[Expr]`, desugared
**post-infer** to the pure L1 calls. Plus fixtures, minimal `kai info`
docs, retro. `refs #1015` (not `closes` — L2 of 3; L3 docs closes).

**Shipped:** identical surface and semantics, but **no new AST node and
no post-infer desugar**. The parser lowers each literal **at parse
time**, directly into the L1 calls, exactly as the existing `<int>n ->
bigint.from_int` and `<digits>i -> complex.mk` numeric-literal sugars do.
Everything else (empties, spread, override, mix errors) matches the
brief. Fixtures, docs, retro shipped.

## The one design decision that changed: parse-time, no AST node

The brief said "new `EMapLit` / `ESetLit`, desugar post-infer". On
investigation that path forces an arm in **~30 files** that match
`ExprKind` exhaustively (`infer`, `emit_c`, every `emit_native_*`,
`kir_lower*`, `fmt_expr`, every `lint_*`, `desugar`): adding a variant
to the `ExprKind` sum breaks every exhaustive match until each gets a
(mostly trivial) arm. Pure plumbing surface and a standing
non-exhaustive-match risk for any future `ExprKind` consumer.

asu (language-architect) confirmed the desugar is **purely syntactic**:
unlike the record-spread (which desugars post-resolve **because** it
needs the declared field order from the type table), a map/set literal
needs no type information — `%{k:v}` is mechanically `map.from([Pair…])`.
The "POST-infer" in the brief was over-specification inherited from the
record-spread mental model; the load-bearing property the brief actually
argues — "the literal is pure because `map.from`/`set.from` are pure" —
holds **identically** at parse time, and in fact *better*, because the
typer infers the effect row of an ordinary pure call with zero new code.

kaikai already does exactly this for numeric literals. The precedent was
decisive: the integrator chose parse-time, no AST node. Result: **zero
typer / KIR / emit surface**, the literal inherits L1's types and purity
for free, and "single spread, first" is a parse-site invariant (the
second `...` is rejected where it is seen, mirroring
`parse_record_spread_loop`), so no node is needed to encode it.

The callee is emitted as `EField(EVar(mod), fn)` (not `EModCall`
directly) so the qualified-call resolver rewrites it — the parser runs
**before** `rqc_decls`, so this is the same trick the numeric sugars
use. This also makes the literal hygienic-by-the-same-rule as the rest
of the language: a user `let map = 42` shadows it the same way it
shadows `42n` (verified identical to the bigint sugar), so the lane
introduces no new capture inconsistency.

## Structural surprises the brief did not anticipate

1. **kaic1 does not resolve a call to a definition placed in a *later
   bundle file*.** The first attempt put the parser in a clean new file
   `parse_coll.kai` (A-grade, <400 LOC) inserted after `parse.kai` in
   `BUNDLE_SRCS`. The call site in `parse_primary` (in `parse.kai`)
   failed with `undefined name parse_collection_lit` even though the
   function was concatenated into the same bundle. A forward reference
   to a function *later in the same file* (`parse_list_or_range`) works;
   one to a function *in a later file* does not — kaic1's symbol scan is
   per-segment in a way that bit this case. The fix: keep the code in
   `parse.kai` itself, next to `parse_list_rest`. This loses the "new
   A-grade file" preference, but `parse.kai` is a pre-existing
   F-monolith and the differential bar forbids *lowering* its grade, not
   adding to it; the new region scores B+ standalone (`km score`) with
   cogcom avg 4.8 / max 13 — under the 5/25 gate. The constraint is
   kaic1's, not a quality choice.

2. **Non-ASCII in the new code silently corrupts the kaic1 bundle.** The
   first draft used `…` (U+2026) and `—` (U+2014) in the doc-attr,
   comments, and — worst — inside the error-message string literals.
   This is the known stage1 multibyte-UTF-8 trap. Symptom is misleading:
   the same `undefined name` error, because the corruption derails the
   bundle parse downstream of the offending char. Fix: pure ASCII in
   everything under `stage2/compiler/` (`…` -> `...`, `—` -> `-`). The
   `.md` docs may keep Unicode (other pages already do).

## Fixtures

`examples/sugars/` (harness `test-sugars`, C backend). Positives:
`coll_literal_map` (get + empty), `coll_literal_set` (contains + empty),
`coll_literal_spread` (extend + override + set spread + modulo still
works). Negatives with `.err.expected`: `coll_literal_kv_in_set`,
`coll_literal_bare_in_map`, `coll_literal_double_spread_map`,
`coll_literal_double_spread_set`. The `coll_literal_*` case in
`test-sugars` passes `--path ../stdlib` for the imports. Behaviour
verified on **both** backends (native default + C oracle) before commit;
the `kai info syntax` fence is compiled by `test-info-blocks` (98/98).

Coverage gap: no fixture for `%{}` fixed-by-later-use without
annotation, or `%{}` isolated — both verified ad hoc to infer/accept as
the existing `Array []` does (the typer accepts a free-variable empty
collection; it is not an ambiguity error in kaikai, contrary to asu's
speculation). Worth a fixture if the empty-inference path ever regresses.

## Cost vs estimate

Most of the wall-clock went to two things the brief could not foresee:
diagnosing the kaic1 cross-file forward-ref failure (false-led twice —
first as a non-ASCII issue, then as bundle ordering) and the
parse-time-vs-AST design validation. The actual parser is ~250 LOC of
straight recursive descent cloned from the list/record-spread loops.

## Follow-ups for next lanes

- **L3** (docs): the full `kai info` Collections section (this lane added
  only the minimal `syntax` entry) + closes #1015.
- The kaic1 "later-file forward ref fails" behaviour is a real bootstrap
  sharp edge. Not worth a fix (stage1 is frozen-ish), but worth a note
  in the build-system docs so the next lane that wants a new compiler
  module places it correctly or expects the failure.
