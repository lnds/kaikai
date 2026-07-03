# Lane experience — issue #1047: `kai migrate`

Automated edition source migration: `kai migrate --from <old> --to
<new>` rewrites a package's source from one edition's surface to the
next. This makes the edition stability promise (CLAUDE.md Tier 1 #4,
"users never dread upgrading kaikai") **executable** rather than
documentation-only. It is a 1.0/Orongo gate.

## Scope as planned vs as shipped

**Planned** (from the issue): the machinery (parse → rewrite → emit) is
independent of the rule set and lands first; the full Hanga Roa →
Orongo rule set is partly gated on pinning the Orongo surface, but at
least one real breaking change must migrate end to end as a proving
fixture.

**Shipped**: exactly that. Four small A-grade modules
(`migrate_rules`, `migrate_walk`, `migrate_scan`, `migrate`), a new
`--migrate` compiler mode, a `kai migrate` subcommand with a safe
dry-run default, one proving rule (the #1015 collection-construction
rename), the `array.*` sentinel drop reported as `manual:`, and two
regression fixtures wired into `test-migrate` (tier1).

## Architecture — AST rewrite, source-preserving

migrate reuses the `kai fmt` path: **parse → apply rewrite rules over
the AST → emit through the fmt-writer**. This is the correctness
route the issue mandated — a textual/regex pass would corrupt code.
The compiler runs migrate post-parse, pre-resolve (like fmt), so it
sees the surface the user typed (`EField(EVar(mod), method)`), not the
resolved `EModCall`.

The one load-bearing invariant, stated in one line at the top of
`migrate.kai`: **migrate is an idempotent, source-preserving AST
rewrite that never emits source that does not parse.** The driver
guards it by reparsing the output before anything is written; on a
reparse failure it prints nothing and fails.

Two stateless passes, deliberately separate:

- **`migrate_walk` (rewrite)**: `Expr -> Expr`, renames each call the
  rule set recognises. Returns only the rewritten node.
- **`migrate_scan` (report)**: `Expr -> [MigRep]`, collects one event
  per recognised call (a counted rename or a `manual:` pointer). The
  driver derives the change count and the manual lines from the event
  list.

They are separate because threading a paired `(node, state)` wrapper
out of a list-match miscompiles under kaic1 (see traps). Keeping each
walk a single-value return sidesteps that entirely. The two passes run
over independent parses of the same source: the rewrite consumes its
owned `decls` (Perceus moves the tree), so scanning the same binding
afterwards would read freed sub-lists.

## Proving rule — why the #1015 rename

The issue requires ≥1 **real** Hanga Roa → Orongo breaking change,
migrated end to end. There is no frozen Orongo surface yet, so I
looked for a surface change that has already landed and is
AST-rewritable. Issue #1015 ("Uniform collection construction")
freezes the collection-construction surface for 1.0 (Orongo); its L1
lane (commit 828bb50b, merged) renamed, in stdlib:

- `map.from_pairs` / `hashmap.from_pairs` → `map.from` / `hashmap.from`
- `set.from_list` / `hashset.from_list` → `set.from` / `hashset.from`

Behaviour unchanged; it is a pure rename of the `pub` stdlib surface —
which `docs/editions.md` explicitly lists as part of the edition
contract. A Hanga Roa package calling `map.from_pairs(...)` does not
compile today, so this is a genuine surface break. asu confirmed it is
a legitimate edition break and the exact shape the migration policy
names ("renames … `kai migrate` … rewrites").

**Match by module, not by method suffix.** The critical scoping
decision: `from_list` survives on `stream` / `stack` / `queue`, so a
suffix match would corrupt still-valid `stack.from_list`. The rule is
a closed whitelist of four `(module, method)` pairs. The proving
fixture (`collections_from`) includes a `stack.from_list` that must be
left untouched — that is the discriminant proving the matcher is not
suffix-based.

**The `array.*` sentinel is reported, not rewritten.** `array.from(xs,
default)` → `array.from(xs)` drops an argument that may carry meaning;
removing it automatically is a silent behaviour change, which the
policy forbids ("we never silently change behaviour"). So it is a
`manual:` diagnostic, not a rule. The `array_sentinel` fixture pins
that the source is left byte-identical and the report is emitted.

## Safe default — dry-run

`kai migrate` is **dry-run by default**: it prints the migrated source
to stdout and touches no files. `--write` (aka `--in-place`) applies
in place. No interactive `[y/N]` prompt — it breaks CI/scripts and
needs stdin. This is the standard tool pattern (`2to3 -w`, `gofmt -w`,
`cargo fix`): safe-by-default, explicit write, works headless. It also
respects the `kai fmt` in-place-destructive trap — migrate must never
silently clobber. `--write` twice is a byte-stable no-op.

## Structural surprises — kaic1 codegen traps

The bulk of the lane's real cost was three kaic1 (stage1) miscompiles
that produce a runtime `panic: non-exhaustive match` or silently-wrong
results, NOT a build error. The bundle is compiled by kaic1, so any
new compiler module hits them:

1. **2nd-field binder of an `ECall(callee, args)` arm feeding a
   list-match** miscompiles. The fix that actually worked: walk child
   lists with the `map` / `reduce` prelude prims (which run in the
   runtime) instead of a local list-match, mirroring how `desugar.kai`
   walks call arguments. This is why every list walk in the migrate
   modules is `map(xs, f)` / `reduce(...)`, not a hand-written
   `match xs { [] .. [x, ...rest] .. }`.
2. **A binder named `args` shadows the `args` prelude prim** (the CLI
   argv reader). kaic1 miscompiled the shadowed binder so `args` read
   as an empty list — the rename worked (it does not count args) but
   the `array.*` manual report silently failed (its rule needs
   `argc == 2`). Renaming the binder to `cargs` fixed it. This one
   cost the most to find because it presented as "only the manual case
   is broken."
3. **kaic1 does not support type parameters on bundle functions**
   (`fn foo[a](...)`), so `flatten_reps` is monomorphic
   (`[[MigRep]] -> [MigRep]`), with callers mapping their typed child
   list to `[[MigRep]]` via the generic `map` prim first.

Method that worked: `lldb` breakpoint on `kai_prelude_panic` for the
backtrace (it showed the panic was in `fmt_call_args` formatting a
node the walker produced, not in the walker), then bisecting by
stubbing functions to `ME(e, st)` and rebuilding. A clean `make kaic2`
never means the binary is correct — only a real fixture run does.

Also worth recording: two helpers I named generically (`opt_list`,
`field_exprs`, `stmt_exprs`, `stmt_expr_list`) collided with `lint.kai`
homonyms under the bundle's flat C-symbol mangling. Prefixed the scan
helpers `msc_` to disambiguate.

## Fixtures and coverage

- `examples/migrate/collections_from.{input,expected}.kai` — the four
  renames + the `stack.from_list` discriminant left untouched.
- `examples/migrate/array_sentinel.{input,expected}.kai` +
  `.report.expected` — the manual-only case: source unchanged, the
  `manual:` report matched against a golden.
- `tests/migrate_fixtures.sh`, wired into `make test-migrate` and
  tier1: per fixture asserts rewrite == golden, idempotency, output
  re-parses, and (when present) the manual report matches.

Coverage gap: only the C backend is exercised (migrate is a
frontend-only, pre-resolve rewrite — there is no codegen surface to
verify on the native backend). selfhost byte-id is green (a new
frontend mode, well isolated, does not change the compiler's C).

## Cost vs estimate

Most of the wall-clock went to the three kaic1 traps, not the design.
The AST-rewrite design was settled up front (the issue pinned it); the
proving rule was found quickly from the #1015 commit. The debugging
loop was long because each hypothesis needed a ~2-min `make kaic2`
rebuild, and the traps compounded (fixing the list-match trap revealed
the `args`-shadow trap underneath).

## Follow-ups for future lanes

- **The full Hanga Roa → Orongo rule set is gated on the Orongo
  surface-pin** (a separate 1.0 gate). This lane ships the machinery +
  one real rule; when the Orongo cut freezes more breaking changes,
  each becomes a new `(module, method)` whitelist entry or a `manual:`
  rule in `migrate_rules.kai`. The rule set is data — extending it does
  not touch the walk.
- **`--from` / `--to` are validated in `bin/kai`, not the compiler.**
  The compiler applies the single known rule set; the wrapper is the
  gate that rejects unsupported pairs. If a second migration
  (Orongo → next) ever ships, the compiler will need to select a rule
  set by the pair, which today it does not.
- **Package-directory migration** (walk every `.kai` under a package)
  is not implemented — `kai migrate` takes one file. `resolve_package_spec`
  is already wired, so extending to a directory sweep is small.
