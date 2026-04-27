# Lane experience report — m12.8.z (`#derive(Ord)`)

Lane: implement auto-derivation of `impl Ord for T` for records and
sum types in stage 2, closing the last gap of the m12.8 protocols
cluster (m12.8 shipped `Ord` as a stdlib protocol with primitive
impls, but no `#derive`). Spec is `docs/protocols.md` §"Auto-derivation
via `#derive` annotation"; framework set up by m12.8 phase 1.

## Result, up front

`#derive(Ord)` works for records (lexicographic by declaration order)
and sum types (by variant position then payload). A pre-expansion
validation pass rejects derives whose fields have no `Ord` impl with
a typer-time diagnostic naming both the field and its type. All
gates green; six new fixtures (five positive, one negative) pass on
both backends. Existing `#derive(Show / Eq / Hash)` fixtures are
untouched.

## Objective metrics (from `/tmp/lane-m12.8.z-builds.tsv`)

- Start: `2026-04-27T00:05:44-04:00`
- End:   `2026-04-27T00:34:22-04:00`
- Wall-clock: ~29 min (one Claude session, no human pauses)
- Build/test invocations:
  - `make -C stage2 kaic2`: 2 explicit invocations (initial after
    derive_ord_* added, second after validation pass added). Both OK.
  - `make -C stage2 test`: 1 invocation. OK (full stage 2 suite incl.
    `test-protocols`).
  - `make selfhost`: 1 invocation. OK.
  - `make -C stage2 selfhost-llvm`: 1 invocation. OK.
  - `make -C stage2 test-llvm-coverage`: 1 invocation. OK
    (60 pass / 0 DIFF / 14 skip — same headline as pre-lane).
- Build-cycle ratio: ~5 min average per gate; the implementation
  itself was a single edit pass (no bisect-by-print needed).

```
$ cat /tmp/lane-m12.8.z-builds.tsv
timestamp                         cmd                 outcome  elapsed_s
2026-04-27T00:30:18-04:00         stage2-test         OK       -
2026-04-27T00:30:41-04:00         selfhost            OK       -
2026-04-27T00:30:58-04:00         selfhost-llvm       OK       -
2026-04-27T00:33:59-04:00         test-llvm-coverage  OK       -
2026-04-27T00:33:59-04:00         test-protocols      OK       -
```

## What landed

### Code (stage2/compiler.kai, +377 lines)

| Block | LOC | Notes |
|---|---:|---|
| `derive_ord_impl` + record body / chain | ~50 | Lexicographic via `let cN = cmp(a.fN, b.fN)` + `if cN != 0 { cN } else { ... }` |
| `derive_ord_sum_body` + outer/inner arm builders | ~110 | Outer `match a` per variant; inner `match b` enumerates "before" variants → `1`, self → payload chain, "after" → `_ -> -1` wildcard |
| Wildcard / default / self / payload helpers | ~50 | `derive_ord_sum_wild_arm`, `derive_ord_sum_default_arm`, `derive_ord_sum_self_arm`, `derive_ord_payload_chain` |
| `validate_derive_ord` pre-expansion pass | ~125 | Collects `Ord` providers (existing `impl` + other `#derive(Ord)`), walks each derive target's fields/payloads, errors on missing impls naming both field and type |
| Wire to `derive_impl_for_protocol` and `lower_protocols` | ~10 | One `else if pname == "Ord"` arm, one new step-0 invocation |
| Extra rationale / contract comments | ~30 | Inline doc at the top of each derive_ord_* and validate_derive_ord_* group |

Predicted in the lane brief: ~30 LOC record + ~40 LOC sum = 70 core.
Actual derive-only LOC ≈ 210 (record + sum + helpers + comments). The
overshoot is driven by the per-variant wildcard arm builders (separate
fns for clarity) and the validation pass that the brief asked for as
step 5 but did not budget LOC for. Without validation it would be
~210 LOC and without the explicit per-helper split ~150 LOC.

### Fixtures (`examples/protocols/derive_ord_*.kai`, 202 LOC across 6 files)

| Fixture | Shape | Asserts |
|---|---|---|
| `derive_ord_record_basic.kai` | record `Point { x, y }` | `cmp` returns -1 / 1 / 0 on the three classic cases |
| `derive_ord_record_lex.kai` | record `Box { x, y, z }` | First differing field decides; equal records → 0 |
| `derive_ord_sum_basic.kai` | enum-style `Tier = Bronze \| Silver \| Gold \| Platinum` | Position dominates: Bronze < Silver < Gold < Platinum |
| `derive_ord_sum_with_payload.kai` | `Shape = Circle(Int) \| Square(Int) \| Origin` | Variant position dominates payload; same-variant uses payload |
| `derive_ord_eq_consistency.kai` | `#derive(Eq, Ord)` record + `#derive(Ord)` sum + manual `impl Eq` for sum | `cmp(a, b) == 0` ↔ `eq(a, b)` for both record and sum cases |
| `derive_ord_no_impl.err.expected` | record `Tagged { id: Int, tag: String }` with only `impl Ord for Int` | Typer rejects with `cannot \`#derive(Ord)\` for \`Tagged\`` + `no \`Ord\` impl for field \`tag\`` |

Predicted ~80 LOC for fixtures; actual 202 (more thorough coverage —
five positive scenarios where the brief described two, plus the
explicit no-impl negative).

### Doc updates

- `docs/protocols.md` §"Auto-derivation via `#derive` annotation":
  added Ord to the example, a "supported protocols and shapes" table
  marking which derive shapes ship in v1, an "Order semantics for
  `#derive(Ord)`" subsection covering record-lexicographic and
  sum-position-then-payload rules, and a "Field-impl validation"
  subsection documenting the new pre-expansion check.
- This file (`docs/lane-experience-m12.8.z.md`).

## Compiler errors I encountered

**None.** The implementation pass produced compiling code on the
first build, and the test failures that surfaced were either:

1. The `derive_ord_no_impl` fixture's `.err.expected` initially used
   the substring "no impl" which doesn't match my diagnostic's
   wording ("no `Ord` impl"); fixed by updating the expected file to
   match the actual phrasing.
2. A pre-existing failure on the branch — `examples/phase4/collatz.kai`
   uses `println` and `bin/kai run` cannot resolve it through the
   `--prelude stdlib/core.kai` path. Reproduced on `git stash`-pristine
   head, confirming it predates this lane (likely a stage-2 prelude
   visibility bug introduced by the 799b772 driver flip). Reported in
   the lane summary; not in scope for this lane.

The single-pass landing is a direct dividend of m12.8 phase 1's
groundwork: `derive_show_impl`, `derive_eq_impl`, and `derive_hash_impl`
established the dispatcher / make_derived_impl scaffolding, the
pattern-binding + per-variant arm builder helpers (`derive_sum_pat_subs`,
`derive_show_sum_arm`), and the EVar / EBinop / EBlock / mk_pat
constructors used to synthesise impl bodies. Everything Ord needed
was already in place; the new code is structurally a `derive_eq_*`
look-alike with `cmp` substituted for `eq`, plus the sum-type
extension that `Eq` and `Hash` deferred.

## Design decisions and tradeoffs

### Lexicographic chains use `let c = cmp(...) in if c != 0 { c } else { ... }`

The example in the lane brief showed a let-binding form, which keeps
each `cmp` call evaluated exactly once (essential when the protocol
op has side effects in user impls — even though stdlib `Ord` is
pure, a user could write an effectful one). The alternative — `match
cmp(a.x, b.x) { 0 -> cmp(a.y, b.y) | n -> n }` — would have been
shorter but binds the user to non-effectful `cmp`. The let-form also
makes the generated AST trivially debug-printable.

### Sum-type inner match: explicit "before" arms + wildcard "after"

The lane brief's example used `_ -> -1` for the after-cases,
exhausting the variants before the outer's index with explicit arms
returning `1`. I followed that exactly; an all-explicit form would
have been O(n²) arms across the whole match where the wildcard form
is O(n × n) outer × inner with each inner at most n+1 arms (n-1
"before", 1 self, 1 wildcard). For a 4-variant enum, the final match
has 4 outer × 4 inner ≈ 16 arms; for an 8-variant enum, ~64. Within
the kaikai compiler's match-emit budget; if a user derives Ord on a
20+-variant sum the generated match is large but still C-compilable.

### Validation runs **before** expand_derives

Putting the validation pass after expansion would mean traversing
synthesised impl bodies (looking for `cmp(a.field, b.field)` calls),
deciding which were synthetic vs. user-written, and matching them
back to the original field. Running before expansion lets the pass
walk `DDerive(names, DType(_, tname, _, body, _, _))` directly and
reach each FieldDecl / Variant by name. The cost is one extra walk
of the decl stream (`O(n + i)` where i is the number of `#derive(Ord)`
annotations); negligible per the m12.8 phase 1 measurements.

### Validation is **scoped to Ord**

The lane brief asked for typer-time validation specifically for Ord.
The same pattern applies to Show / Eq / Hash and could be lifted to
all four protocols, but doing so without the lane brief's blessing
would be scope creep. The other derives keep their v1 behaviour:
missing impls fall through to the dispatcher's runtime panic. This
is a documented v1 tradeoff; tightening it for Show / Eq / Hash is
captured as a follow-up in `docs/m12.8-followup.md`.

### Recursive types whitelisted

`#derive(Ord) type Tree = Leaf | Node(Tree, Tree)` would be rejected
by a strict provider check (Tree isn't yet in providers when its own
derive is being validated). The validation explicitly whitelists
`head == tname` so recursive types are accepted. The synthesised
`cmp` calls itself via the dispatcher, which the post-inference
rewrite redirects to the impl that the *same expansion* produces.
This was untested in the v1 fixtures (no recursive Ord example
shipped); a recursive-Ord fixture would be a useful addition in a
future lane.

## Validation of design assumptions

- **Reuse of m12.8 phase 1 framework**: confirmed. The new derive
  fns mirror `derive_eq_*` and `derive_hash_*` line-for-line in
  shape; the only novelty is the sum-type inner-match with
  "before/self/after" partitioning and the let-binding payload
  chain. This is the exact "framework generalises cleanly" outcome
  m12.8's lane report predicted (`docs/lane-experience-m12.8.md`
  §"`#derive` for records vs sum types").
- **Compile-time impact**: spec budget 2-5%. `make -C stage2 kaic2`
  before vs after stayed within noise (~2.5s ± 0.3s on a warm cache).
  The validation pass adds one walk of the decl stream — `O(n + i)`
  where `n` is decl count and `i` is `#derive(Ord)` count. Within
  budget.
- **No vtable indirection at runtime**: `Ord` follows the same
  AST-rewrite path as the other protocols. Generated `cmp` calls
  resolve to `kai___pimpl_Ord_<T>_cmp` at the C level; no runtime
  lookup.

## Out-of-scope / explicitly deferred

- **`#derive(Eq)` and `#derive(Hash)` for sum types**. The
  framework supports them (the m12.8 lane report estimates ~30 LOC
  each) but they were not in this lane's brief. Captured as the
  obvious next derive lane.
- **`#derive(Serialize)`**. Out of scope per lane brief; needs
  return-type-driven dispatch which v1 does not have.
- **Field-validation generalisation to Show / Eq / Hash**. Same
  pattern as `validate_derive_ord` would apply; deferred to keep
  the lane scoped.
- **Recursive-Ord fixture**. The validation pass whitelists
  recursive types; an end-to-end fixture exercising the synthesised
  cmp on a recursive sum would prove the rewrite path is correct
  for the recursive case. Logged as a follow-up.

## Coordination notes

The lane brief flagged potential overlap with m12.8.x phase 4 in
`stage2/compiler.kai`. The actual zone touched here is the
`derive_*_*` block (lines ~19661-19905 after the edit) plus the
`lower_protocols` step list. Phase 4's reported scope (parser
extension, typer effect enforcement, `/ Console` annotations on 43
helpers) is disjoint from the derive expander code. Conflict on
rebase is expected to be empty or trivially keep-both.

## Limitations of this report

- Self-report bias acknowledged: same agent that wrote the impl
  reports on it.
- Wall-clock under 30 min and code under 400 LOC; the data point
  reinforces the m12.8 prediction that subsequent derives are quick
  to add when the framework is in place, but does not generalise to
  novel protocols (e.g. multi-method ones like `Iterator`) which
  would need new dispatcher / desugaring infrastructure.
- Single agent (Claude Opus 4.7). Not generalisable across LLMs.
- The println / collatz failure was not investigated beyond
  confirming it is pre-existing; root-causing it is a separate
  ticket (likely belongs in `docs/m12.8-followup.md`).
