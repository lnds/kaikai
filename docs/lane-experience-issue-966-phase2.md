# Lane experience — issue #966 Phase 2 (idiom nudges)

## Scope as planned vs. as shipped

**Planned:** add two idiom-nudge lints to `kai lint` (Phase 1 shipped the
subcommand + `discard_pure_value`): `point_free_nudge` (a lambda that is
just a field/method access → suggest a point-free section) and
`and_then_to_map_nudge` (`and_then` whose function always wraps in `Some`
→ suggest `map`). Both warnings only, never block a build.

**Shipped:** exactly that. Both rules live in a new
`stage2/compiler/lint_idiom.kai`; `lint.kai` calls `idiom_findings` at
each node in its existing walk. Four fixtures (positive + negative per
rule) wired into `test-lint`, plus docs in `docs/info/lint.md`. Verified
on the native backend; selfhost byte-id and tier0 green.

## Design decisions

- **New module, not growing lint.kai.** lint.kai was already 335 LOC; the
  two rules add ~180 LOC of code, which would have pushed it toward the
  800 hard cap. Split before growing per the quality bar: `lint_idiom.kai`
  owns the rules + `LintFinding`, `lint.kai` imports it. Both score A+
  (`km score` 96.7 each, cogcom avg ≤2.2).

- **Precision over recall — the linter must not be noisy.** Each nudge
  fires only when the suggested form is provably equivalent:
  - `point_free_nudge`: the lambda body must be a postfix chain rooted
    *exactly* at the parameter, with no method argument depending on it.
    A body that computes more (`c.age * 2`) or uses the param as an
    argument is left alone.
  - `and_then_to_map_nudge`: *every* tail position (block tail, both `if`
    branches, all match arms) must construct `Some`. Any path to `None`
    keeps the bind, because `.map` cannot short-circuit.

- **The `__pl` discriminant.** A point-free section `.name` desugars to a
  lambda `(__pl) => __pl.name` — structurally identical to a user lambda.
  Re-nudging it would be an infinite false positive. The parser names the
  synthetic param `__pl` (parse.kai), and that name (verified, not
  guessed) is the guard: a lambda binding `__pl` is already point-free
  and skipped.

- **Backend/typer-rewrite independence.** A combinator call reads as
  `EModCall(mod, name)` after inference and `EVar`/`EField(_, name)`
  before it; UFCS moves the method receiver into the first argument
  (`c.name.length()` → `ECall(EModCall("string","length"), [c.name])`).
  Both rules match all forms so the lint does not hinge on how far the
  typer has rewritten the body. Confirmed each shape with `--dump-typed`.

## Structural surprises the brief did not anticipate

- **A compiler param named `args` was silently rebound to the prelude
  builtin in the kaic1 bundle.** `fn call_nudges(..., args: [Expr])` —
  kaic1 (stage1, kaikai-minimal) resolved `args` to `kai_prelude_args`
  (the CLI argv builtin), so the generated C passed a closure where the
  arg list should be, and a list `match` panicked `non-exhaustive match`
  at *runtime* — typecheck and build were green. Diagnosed by reading the
  generated `stage2.c` call site (it read `kai_closure(&_kai_prelude_args_thunk, …)`
  instead of the binding). Fix: rename `args` → `cargs`. This is the
  stage1 face of the shadowing trap; the lesson is recorded for the next
  lane.

- **`test-info-blocks` compiles every `` ```kaikai `` doc block as a full
  program (build + link), so each needs `fn main`.** The three idiom-nudge
  examples in `docs/info/lint.md` were illustrative `fn` fragments and the
  CI gate (tier1-shard-3) failed them with `undefined reference to 'main'`
  — green locally because `test-info-blocks` is not in tier0. Fixed by
  adding `fn main() : Int = 0` to each. A `` ```kaikai-snippet `` fence
  wraps an expression fragment instead, but these blocks declare
  top-level `fn`s so the full-program form is the right one.

- **`--lint` does not build the head-owner cache, so `|` pipes report
  "no module declaring type List".** Inherited from Phase 1's lint mode,
  not this lane's surface. The rules still detect the pipe nodes
  (`EMapPipe`/`EFilterPipe`/`EFlatMapPipe` carry the lambda), but the
  fixtures use UFCS method form to avoid the unrelated resolution noise.
  Not fixed here — driver.kai is owned by a parallel lane (#962 Lane 4)
  and the cache wiring is a separate concern.

- **kaic1 generates a reachable panic for nested `match` over `Option`
  returned in expression position, and for list patterns rooted at `[x]`
  rather than `[]`.** Flattened all such matches to a single level with
  `[]`-first list patterns; the residual cause turned out to be the `args`
  shadow above, but the flattening kept the codegen well within what
  kaic1 handles and is cleaner anyway.

## Fixtures added / coverage

- `examples/lint/point_free_nudge_{positive,negative}.kai`
- `examples/lint/and_then_to_map_nudge_{positive,negative}.kai`

Each with `.lint.expected` + `.lint-json.expected`, gated by `test-lint`.
Positives exercise field / method / method-with-arg chains and direct /
block / match `Some` wraps; negatives exercise the compute-more,
arg-uses-param, already-`__pl`, `if`-else-`None`, match-arm-`None`, and
plain-`map` cases. Coverage gap: no pipe-form positive fixture (the
`--lint` head-owner-cache limitation above); the pipe path is covered by
manual verification, not a golden.

## Cost vs. estimate

The rules themselves were straightforward once the typed AST shapes were
confirmed with `--dump-typed`. The disproportionate cost was the
`non-exhaustive match` panic — green build, green typecheck, runtime
crash — which took an lldb backtrace + reading the generated C to pin to
a parameter-name/prelude-builtin collision in the stage1 bundle.

## Follow-ups for next lanes

- `--lint` should build the head-owner cache so `|` pipes typecheck under
  lint (currently report a spurious "no module declaring type List").
  Belongs with whoever owns driver.kai next.
- A pipe-form positive fixture once the above lands.
- Next idiom nudges from #966's seed list: manual `match` on `Option`
  that a combinator covers; redundant-pattern checks.
