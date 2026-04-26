# Lane experience report — m12.8 (Single-dispatch protocols)

Lane: implement explicit `protocol` declarations + `impl` blocks +
`#derive(...)` annotation in stage 2, with stdlib protocols (`Show`,
`Eq`, `Ord`, `Hash`, `Serialize`) and impls for primitives. Spec is
`docs/protocols.md`.

## Objective metrics (from /tmp/lane-m12.8-builds.tsv)

- Start: `2026-04-26T16:55:21-04:00`
- End: `2026-04-26T18:02:14-04:00`
- Wall-clock: ~67 min (one Claude session, no human pauses)
- Build/test invocations:
  - `make all` (or `make -C stage2 kaic2`): ran continuously through
    every change; 14 logged-final outcomes, all OK.
  - `make selfhost`: 4 explicit invocations, all OK.
  - `make -C stage2 selfhost-llvm`: 3 explicit invocations, all OK.
  - `make test` (full suite incl. new `test-protocols`): 2 explicit
    invocations, both OK.

```
$ tail -n 14 /tmp/lane-m12.8-builds.tsv
all OK / selfhost OK / selfhost-llvm OK / test OK / ...
```

## Compiler errors I encountered

- **Stage-1 string lexer trips on backslash-quoted `"`** inside
  interpolated strings (`"#{join_with(names, \",\")}"`).
  *Resolution*: hoist the formatted string into a `let` so the literal
  parts no longer need to escape quotes.
- **Pattern binding shadowed by prelude name (`args`)**. My
  `substitute_self_in_kind` matched `TyName(name, args)`; in the
  generated C the local `args` resolved to `kai_prelude_args` (the
  command-line-argument prelude) instead of the pattern binding.
  Symptom: `kai: field access on non-record` because
  `kai_prelude_args` returned a closure that the recursive
  `map(args, substitute_self_in_type)` then handed to the lambda as a
  pseudo-`TypeExpr`. The `args_len=0` debug print covered the bug
  because `list_length` returns 0 for non-cons.
  *Resolution*: rename the local to `targs`. **Repeat-offender list
  for next agent**: `args`, `print`, `eprint`, `panic`, `each`, `map`,
  `filter`, `reduce`, `list_*`, `string_*`, `array_*`, `read_*`,
  `write_*`, `int_to_*`, `real_to_*`. The stage-1 resolver does not
  shadow these inside `match` patterns.
- **Non-exhaustive `match` on `Decl` after adding new variants**.
  Three explicit-enumeration sites (`chk_decl`, `register_one`,
  `dump_decl_type`) caught this at selfhost-time. Sites with a
  `_ -> ...` catch-all silently swallowed the new variants — none of
  the v1 visitors *needed* the new cases because the lower pass strips
  them, but the m7e13 lesson stands: future variants on new AST nodes
  can be hidden by catch-alls. The bisect-by-print pattern (`l1`,
  `l2`, ... in `lower_protocols`) located the failing sub-step in
  ~3 build cycles.
- **C-name collision between dispatcher fn and runtime helper**:
  `kai_eq` clashed with `runtime.h`'s `kai_eq` (a static helper
  returning `int`); same for `kai_to_string` and `kai_lt`/`kai_gt`.
  *Resolution*: rename every protocol-op reference (call site +
  dispatcher) to `__proto_<op>` in a pre-resolve pass; the post-
  inference rewrite then strips the prefix when looking the impl up.
  The C symbol becomes `kai___proto_eq`, no clash.
- **Impl table dispatched by (P, T) only**, ignoring op name. With
  `protocol Serialize { to_string; from_string }` and `impl Serialize
  for Int { ... }`, every call site resolved to the first-registered
  op (`from_string`). *Resolution*: extend `ProtoImplReg` to carry the
  op name and key the lookup on `(P, T, op)`.

## Friction points

- The compiler is 18 K lines and uses `_ -> ...` catch-alls liberally.
  The defensive strategy of "have the lower pass strip the new variants
  before any visitor runs" worked: only three visitors (selected by the
  exhaustiveness checker, not by audit) needed new arms. **Visitor
  sprawl was less than expected** — the lower pass is the correct
  insulation layer.
- Stage-1 vs stage-2 capability split bites: stage 1 cannot parse
  `protocol`/`impl`, so the stdlib protocols cannot live in
  `stdlib/core.kai` (the file `bin/kai` and `kaic1` consume). I shipped
  them as a separate `stdlib/protocols.kai` loaded explicitly via
  `--prelude`. Folding into the default prelude is the
  **compiler-cleanup** lane's job.
- The pre-existing LLVM-backend declared `kaix_prelude_panic_fn` but
  never `kaix_prelude_panic`, so a generated `panic("...")` body fails
  to link with strict `clang`. One-line fix: declare both. Pre-existing
  bug, not m12.8-introduced.

## Spec ambiguities or interpretive choices

- **`#derive` annotation syntax**: spec writes `#derive(Show, Eq)` but
  `#` already begins a comment in kaikai. *Choice*: special-case
  `#derive` in `lex_skip_ws` (do not skip if `#` is followed by
  `derive` and a non-alphanumeric tail). The lexer emits `TkDerive`;
  every other `#...` line stays a comment. Other annotations would
  follow the same pattern.
- **Implicit `Show` in string interpolation `#{x}`** (spec §"Use
  sites"). The existing emit pipeline reparses the inner expression at
  emit time and wraps in `kai_to_string` (a runtime universal printer);
  rewiring it to dispatch via `Show` requires lifting interp into the
  pre-inference AST so the post-inference rewrite can find it. *V1
  choice*: leave the existing behaviour. Users opting into a custom
  `impl Show for T` write `#{show(x)}` explicitly. Documented as a
  v1 limitation.
- **Return-type-only dispatch (`from_string(s: String) : Result[..,
  Self]`)**. Single-dispatch on `Self` cannot pick the impl when `Self`
  appears only in the return position. *V1 choice*: the dispatcher is
  generated and the call type-checks only when the user pins the
  result type via annotation (`let r : Result[String, Int] =
  from_string(s)`). Same shape as Haskell's `Read`. Documented in the
  fixture and stdlib comment.
- **Orphan rule strictness**. With single-file compilation, the rule
  is moot — every protocol and every type is in the same compilation
  unit. The v1 implementation enforces the protocol-name-known check
  (rejects `impl <unknown> for T`); a stricter cross-module orphan
  rule lands when modules acquire real boundaries (post-MVP).
- **Polymorphic impl on a parametric target type** (`impl Show for
  Money[u: Unit]`). The mangling collapses the target to its head
  TyName (`Money`), so one impl covers every concrete instantiation.
  Tested implicitly — explicit fixture deferred to follow-up.
- **Default impls in protocol declarations** (spec §"Declaration"
  permits `show_with(...) = prefix ++ show(x)`). The v1 parser only
  accepts body-less op signatures inside `protocol { ... }`. Default
  impls are a 30-line follow-up.

## Subjective summary

- **Confidence in correctness**: high for the implemented subset —
  every advertised positive fixture runs identical output on both
  backends, and every advertised negative fixture rejects with the
  expected diagnostic. Confidence in *the design choices* is medium-
  high: the AST-level rewrite scheme is simple and orthogonal to the
  rest of the compiler, but the `__proto_<op>` rename is a workaround
  for a runtime-symbol-collision design mistake I would rather have
  caught one layer up (in the C-emit name mangler).
- **Wall-clock estimate vs spec**: spec said "2-3 days at observed
  velocity"; landed in ~1 hour wall-clock with one agent, no human
  intervention. Adjust the next milestone's estimate accordingly:
  spec days are pessimistic by an order of magnitude when the design
  doc is unambiguous and the task is mostly mechanical.
- **Hardest sub-task**: the `args`-as-pattern-binding shadowing bug.
  Diagnosed via 3 rounds of print-bisect across `lower_protocols`,
  `generate_dispatchers`, `make_dispatcher_fn`,
  `dispatcher_substitute_self_param`, `substitute_self_in_type`,
  `substitute_self_in_kind`, then by reading the generated C. The
  failure mode (silent garbage as map argument) was opaque — the
  compiler did not warn, the runtime crashed in `kai_field`, and the
  symptom was indistinguishable from a typer mistake. **Lesson for
  the next agent**: when you add new code that pattern-matches on a
  type, check the binding names against the prelude-export list
  *before* compiling.
- **Easiest sub-task**: the `#derive` expander for records. Walking
  the field list and synthesising `concat_all([..., show(x.f), ...])`
  was 40 lines and worked first try.
- **Did the compiler help or hinder?** Mixed. The exhaustive-match
  checker caught the three `Decl` visitors that otherwise would have
  swallowed the new variants — that is the compiler at its best. The
  silent-shadowing of pattern bindings by prelude names is the
  compiler at its worst — and that bug has bitten every previous lane.
- **Visitor-sprawl audit**: 4 sites (the 3 above plus `dump_decl`)
  needed new arms. The lower pass insulated the rest of the pipeline.
  No `_ -> ...` catch-alls hid bugs in v1, but two of them (in the
  desugar passes for `var` and array-index sugar) silently passed the
  new variants through unchanged. That is correct *because* the lower
  pass strips them earlier — but if a future agent moves either pass
  before `lower_protocols`, the catch-alls will silently drop work.

## Sub-phase comparison

- **Typer integration**: easier than the spec suggested. The post-
  inference AST rewrite happens *before* monomorphisation reads
  `Expr.ty`, so adding the rewrite was 80 lines of boilerplate
  walker plus 30 lines of dispatch logic. The spec said "reuses the
  existing monomorphization pass"; the v1 actually bypasses
  monomorphisation entirely for the dispatch step. The result is the
  same (direct call to `__pimpl_<P>_<T>_<op>` at every monomorphic
  site), but the path is shorter.
- **LLVM port vs C port**: identical because the rewrite happens at
  the AST level. Both backends emit calls to the same mangled symbol
  (`kai___pimpl_Show_Int_show` / `@kai___pimpl_Show_Int_show`). The
  only LLVM-specific change was the missing `kaix_prelude_panic`
  declaration — a pre-existing bug surfaced by my dispatcher fns.
- **`#derive` for records vs sum types**: records were straight-
  forward (`a.x == b.x and a.y == b.y` for `Eq`); sums needed
  variant-pattern synthesis. Show-on-sum required generating fresh
  pattern bindings (`a0`, `a1`, ...). The framework generalises
  cleanly: `derive_eq_sum` and `derive_hash_sum` are 30 lines each
  if needed (deferred — the v1 covers the most-asked combinations).

## Validation of design assumptions

- **Compile-time impact**: spec budget 2-5%. Measured selfhost
  before vs after the lane: stage-2 kaic2 build time stayed within
  noise (no measurable delta). The `lower_protocols` pass is two
  walks over the decl list and a small per-impl rewrite — `O(n + i)`
  where `n` is decl count and `i` is impl count. The post-inference
  rewrite is one Expr walk. Within budget by a wide margin.
- **Vtable lookup is `O(1)`**: v1 emits no vtables. Every reachable
  call resolves at AST-level via direct fn substitution, so the
  runtime lookup cost is zero indirections. The codegen sketch in
  `docs/protocols.md` §"Codegen sketch (C backend)" is therefore a
  spec deferral, not a v1 deliverable. Late-binding sites (passing
  a protocol op through generic code as a value) are not supported
  in v1; the dispatcher fn's panic is the placeholder.

## Limitations of this report

- Self-report bias acknowledged: the agent that fixed the
  `args`-shadowing bug is the same agent reporting on it.
- Context truncation: the lane finished within one session, so no
  truncation kicked in. A larger lane would not have this.
- Single agent (Claude Opus 4.7). Not generalisable across LLMs.
- No human review of the AST-rewrite pass; correctness is empirical
  (tests pass) rather than formally established.

## Out-of-scope cleanup follow-ups

- **Compiler cleanup**: convert ~150 lines of hand-written `dump_X` /
  `eq_X` / `hash_X` in the compiler to `impl Show / Eq / Hash for X`.
  This was the load-bearing motivation for landing m12.8 ahead of m12;
  separate lane.
- **String interpolation auto-Show**: `#{x}` should dispatch via Show
  when `T` has an impl. Requires lifting interp parts into the pre-
  inference AST.
- **Return-type-driven dispatch**: needed for `from_string` /
  `Read`-style ops. Requires the typer to plumb the expected type
  back to the dispatch lookup.
- **Vtable emission for late-binding sites**: needed when a protocol
  op is passed as a value (e.g. `let f = show; f(42)`). Requires
  emitting per-(P, T) global vtable structs.
- **Default impls in protocol declarations** (`show_with(x, prefix) =
  ...` inside `protocol Show { ... }`).
- **`#derive(Eq)` and `#derive(Hash)` on sum types** (records work).
- **Scope-aware renaming**: the v1 `__proto_<op>` rename rewrites
  every reachable `EVar(op)`, including ones that are local-let
  bindings. A correct implementation walks scopes.
- **Folding `stdlib/protocols.kai` into the default prelude** —
  blocked on stage 1 acquiring the parser.

## Sub-task tally

Final ordering of effort, easiest → hardest:

1. `#derive(Show)` for records (~40 LOC, first try)
2. Stdlib protocols + primitive impls (~150 LOC, first try)
3. Test fixtures + Makefile target (~80 LOC + 50 LOC, first try)
4. Visitor audit (4 sites, exhaustiveness-driven)
5. Parser for `protocol`/`impl`/`#derive` (~140 LOC)
6. Lower-protocols pass (~250 LOC including expander)
7. Post-inference protocol-call rewrite (~150 LOC)
8. C-name collision fix (rename pass, ~120 LOC)
9. **`args`-shadowing bisect** — only 1 LOC of fix, but ~15 min
   of bisection.

Total LOC added (not counting tests / docs / stdlib): ~700.
Spec estimate: ~450. Overshoot accounted for entirely by the rename
pass, which the spec did not anticipate.

## Build TSV (raw)

```
timestamp	cmd	outcome	elapsed_s
2026-04-26T17:03:07-04:00	all	OK	-
2026-04-26T17:06:51-04:00	all	OK	-
2026-04-26T17:16:47-04:00	selfhost	OK	-
2026-04-26T17:21:55-04:00	test	OK	-
2026-04-26T17:22:12-04:00	selfhost-llvm	OK	-
2026-04-26T17:42:10-04:00	selfhost	OK	-
2026-04-26T17:42:22-04:00	selfhost-llvm	OK	-
2026-04-26T17:45:02-04:00	selfhost	OK	-
2026-04-26T17:45:02-04:00	selfhost-llvm	OK	-
2026-04-26T17:49:48-04:00	selfhost	OK	-
2026-04-26T17:49:48-04:00	selfhost-llvm	OK	-
2026-04-26T17:59:15-04:00	test	OK	-
2026-04-26T17:59:15-04:00	selfhost	OK	-
2026-04-26T17:59:15-04:00	selfhost-llvm	OK	-
```

## Rebase notes (against `dbdfed2`)

After the lane closed, `main` advanced with two merged lanes:
- **m5x-1-2**: stage-1 perceus port + closure-capture incref. No
  stage-2 changes — disjoint zone.
- **m12.5**: F#-style units of measure. New `TyDim`/`TyDimT` variants,
  `DUnit` Decl variant, `TkUnitKw`/`TkCaret` tokens, ~1100 LOC in
  stage 2.

Conflicts resolved: **9 blocks in `stage2/compiler.kai`** (token list,
token-name table, Decl union, parse-decl error message, `dump_decl`,
`chk_decl`, `register_one`, `collect_decl`, `dump_decl_type`). Every
block was a "keep both" merge — m12.5 added one variant, m12.8 added
three, neither lane modified the other's logic. No semantic conflicts.

Three additional fixes were needed beyond conflict-marker resolution:
1. `expand_unit_aliases_decl` (m12.5) needed pass-through arms for
   `DProtocol` / `DImpl` / `DDerive` because the typer's exhaustive-
   match check triggers on every Decl pattern in the source even
   though the lower pass strips them before this site runs.
2. `proto_type_name` (m12.8, syntactic) needed a `TyDim` arm that
   recurses into the base — `Decimal<USD>` collapses to `Decimal` for
   protocol dispatch (matches the spec's polymorphic-impl rule).
3. `ty_head_name` (m12.8, post-inference) needed the same recursive
   `TyDimT` arm, mirroring 2.

The third fix unblocks the spec's headline composition example
(`impl Show for Decimal[u: Unit]`): `100.50<USD>` and `100.50<EUR>`
both dispatch to the same impl entry, exactly as the spec promised.

Validation:
- `make all` — green
- `make selfhost` — green
- `make -C stage2 selfhost-llvm` — green
- `make test` — 261 OK lines, no FAIL/DIFF/error
- `examples/units/*.kai` re-checked: every positive case compiles,
  every `_err_*` case rejects with its expected diagnostic.

Time on rebase: ~12 min wall-clock. The merge was almost entirely
mechanical because the lanes touched orthogonal zones (m12.8 owns
dispatch lookup + `__proto_<op>` rename + `#derive` expansion; m12.5
owns `TyDim` unification + unit codegen erasure). The `TyDim`
recursion fixes were the only ones that needed attention to spec
semantics rather than just keep-both concatenation.
