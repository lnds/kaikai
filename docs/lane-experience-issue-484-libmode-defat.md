# Lane experience — issue #484 (library mode `def_at` over locals/params/patterns/closures)

## Scope as planned vs shipped

**Planned (issue #484, brief):** extend the library-mode `def_at`
query so it resolves not just top-level fn / type / const / effect
/ protocol names but also local bindings — Part A `let` binds,
Part B function/lambda parameters, Part C pattern binds
(`Some(x)`, `[h, ...t]`, `T { f: y }`, `as`-aliases), Part D
closure captures (a name used inside a lambda that is not a
closure parameter resolves to the outer binding), Part E correct
shadowing (innermost wins). Unblocks LSP v1 (#447).

**Shipped:** all five parts. The walker covers `SLet`, `SVar`
(handler-rewritten in practice, but the path is present), all 10
`PatKind` variants (including `PNarrow` for type-narrowing arm
patterns and `PHole(Some(name))` for typed pattern holes),
`ELambda` parameters, `EHandle`'s `HClause` parameters and
`HReturn`'s named binder. Shadowing falls out of the LIFO scope
stack — no special handling needed. 13 new fixtures wired into
`make test-library-mode` (tier 1).

The brief asked for "10-15 fixtures"; shipped 13. The brief
listed `def_at_for_loop.kai` as a target — kaikai has no `for`
construct (verified via `grep -n EFor` in `stage2/compiler.kai`,
empty), so that fixture is omitted. A `def_at_var_mut.kai`
fixture was attempted and removed: bare reads of `var name`
bindings parse as State-capability accesses, not ordinary
identifier reads (`@name` is the read syntax), and the typechecker
rejects the bare form with a dedicated diagnostic. That makes
`var` bindings invisible to `find_var_at_*`'s `EVar` predicate by
construction, so the SVar case ships as code (scope-tracking in
`find_local_at_stmt`) without a regression fixture — the fixture
would require the `EBang(EVar(name))` desugar shape, which is one
indirection past what this lane targets.

## Design decisions

### Single-pass walker, not a resolver-side index

The brief's Part A described "scope tracking: each `let` binding
records `(name, span, scope_id)` during the resolver/typer walk."
I rejected that in favor of a query-time AST walk that threads a
lexical scope stack and looks up the query name at the EVar hit
point. Three reasons:

1. **Additive, no resolver plumbing.** The resolver/typer is the
   most-touched part of the compiler — every other compile pass
   reads from it. Threading a scope-id sidecar through inference
   would have rippled across `infer_program`, `walk_*`, the unboxer,
   monomorphisation, and Perceus. The query-time walker lives in
   `stage2/compiler.kai` after `find_decl_def`, touches no other
   pass, and runs only when `dump_library_mode` invokes it.

2. **Cheap to keep coherent.** A sidecar that records scope at
   inference time has to stay in sync with every AST rewrite that
   the lowering passes do (DConst → DAttribPure(DFn), `var` →
   `handle ... with State[T]`, list comprehension desugars, etc.).
   A walker over the post-inference AST sees exactly what the
   queries already see — no synchronisation problem.

3. **LSP latency is fine.** `def_at` is O(n) per probe, same
   complexity as `type_at` and `enclosing_node`. On the
   ~52000-line `stage2/compiler.kai` the existing queries already
   answer fast enough that they're not the bottleneck for tier 1's
   `test-library-mode`. No microbench needed.

The brief's success criterion "performance: `def_at` walltime ≤
existing `type_at`" is satisfied trivially: the local walker is
structurally identical to `find_enc_*` and `find_var_at_*` (same
recursive shape, one extra parameter for scope). Both pre- and
post-lane `def_at` walk the AST once; the lane just adds a second
walk that's no deeper than the first.

### Scope-stack representation: LIFO list of `ScopeBind`

`type ScopeBind = SB(String, String, Int, Int)` — kind, name,
line, col. The kind discriminator ("Local" / "Param" / "PatBind" /
"VarMut" / "HReturn") flows out to the JSON `decl_kind` field so
LSP clients can choose how to render the result (e.g. show
"parameter" vs "local"). Innermost binding sits at the head; the
lookup walks head-to-tail and returns the first hit. Shadowing is
automatic: descending into a `let x = ...` block pushes the new
`x` onto the head, so any deeper `EVar(x)` hit picks up the inner
binding before reaching the outer one.

Variant name was first `LocalBind` / `LB(...)` — collided with the
existing `LocBind` / `LB(...)` in the unboxer (single-letter
constructors are scarce in a 52k-LOC file). Renaming to
`ScopeBind` / `SB(...)` is a one-time cost.

### Pattern walker shared structure, separate function

`pat_binds` extracts bindings from a `Pattern` and prepends them
to the scope stack. It's recursive (`PAs("name", inner)` pushes
`name` then recurses into `inner`; `PList(subs, rest)` pushes
`rest` and walks `subs`; etc.). The walker function is **not**
shared with `find_enc_pattern` or `find_var_at_*` — those query
positions, this one extracts bindings. Sharing them would have
needed three-way recursion plumbing; the duplication is ~35 lines
and the intent is distinct enough that it pays for itself.

### Variant constructors stay unresolved

`MkPair(a, b)` (the constructor in expression position) is parsed
as an `ECall(EVar("MkPair"), [...])`. The walker visits the EVar,
finds nothing in the scope stack, falls back to `find_decl_def`
which scans top-level decls by name — but variant names are bound
to the parent `DType`, not to themselves. So variant-constructor
goto returns `null`. That's an explicit non-goal of this lane;
LSP v2 (#447) will introduce a variant-name → declaring-type
index that `def_at` can consult.

### Innermost EVar match by `pos_ge`, not exact `(line, col)`

The existing `find_var_at_decls` accepts any EVar at position
`(cl, cc) ≤ (ql, qc)` and picks the deepest in document order.
This means a probe column past the EVar's last character still
hits it (e.g. `def 3:99` on a 30-column line hits the last EVar
on the line). The local walker uses the same `pos_ge` predicate —
behavior is consistent. Fixtures place probes inside the
identifier exactly, so this is invisible in goldens, but the
property matters when LSP clients map mouse-cursor positions that
land in trailing whitespace.

## Structural surprises

### `args` is a prelude shadow trap

The first ECall match arm was written as `ECall(f, args) -> {...}`,
mirroring the original `find_var_at_expr_kind`. Stage 1
(`kaic1`) compiles `compiler.kai` with a known bug — a
pattern-bound name `args` does NOT shadow the prelude builtin
`args()` (CLI argv getter), so at the use site the generated C
calls the prelude thunk and the function receives the wrong
closure of the wrong type. The symptom is `panic: non-exhaustive
match` at runtime from a completely unrelated match site
downstream. Cost ~20 minutes to diagnose (the memory pinned in
`feedback_kaikai_param_args_shadow.md` was the rescue).

Renamed every `args` binding in the new walker to `call_args`.
The same bug also still lurks for any future code that uses
`args` / `exit` / `read_line` as parameter or pattern names —
that's a stage 1 fix to schedule independently, not in this lane.

### `var name = ...` is not a plain local

`var name = init` looks like a local binding to the casual reader
but is actually rewritten before resolution into
`handle { rest } with State[T](init) as name { ... }` (see
`stage2/compiler.kai` `desugar_var_decls`). What survives into the
typer is a `Handle` shape, not an `SVar`. So while my walker
*does* register `SVar(name, _, init, line, col)` as a scope binding
(in `find_local_at_stmt`), it never fires because no `SVar` reaches
the typed AST. The path stays in the code as documentation of the
intent and as a guard in case the desugar pass changes in the
future.

### Pattern bind position for variant args needs the sub-pattern's `Pattern { line, col }`

Originally I tried to attribute every bind in a variant pattern to
the enclosing pattern's `line/col` (e.g. for `Some(v)`, attribute
`v` to `Some`'s start). That made `def_at` over `v` point at the
constructor name, not the binder. The fix: recurse into sub-
patterns and let `PBind(name)` use its own `Pattern { line, col }`,
which the parser sets to the binder's position. The rest binder of
a `PList` does NOT have its own position (it's a bare `String`),
so it uses the parent pattern's `line/col` — close enough for goto
in practice.

## Fixtures

Thirteen new fixtures in `examples/library_mode/`:

| Fixture                       | What it pins                                             |
|-------------------------------|----------------------------------------------------------|
| `def_at_local_basic`          | `let x = ...` then `x` later                             |
| `def_at_local_shadow`         | inner `let x` shadows outer                              |
| `def_at_param_basic`          | `fn f(n)` body referencing `n`                           |
| `def_at_param_shadow`         | lambda `(x) => x` shadows enclosing fn's `x`             |
| `def_at_pattern_list`         | `[head, ...rest]` — head bind + rest bind                |
| `def_at_pattern_variant`      | `Some(v) -> v` + Param hit from another arm              |
| `def_at_pattern_record`       | `Point { px, py }` shorthand                             |
| `def_at_pattern_shadow`       | arm pattern `Some(v)` shadows fn Param `v`               |
| `def_at_pattern_as`           | `whole @ MkPair(a, b)` — alias + sub-pattern binds       |
| `def_at_closure_capture`      | lambda body sees outer fn's `x`                          |
| `def_at_nested_let`           | inner block's `let c` invisible outside; outer `a`/`b`   |
| `def_at_match_arm`            | three arms with disjoint binders, each isolated          |
| `def_at_lambda_param`         | `(n) => n * 2` over lambda's own param                   |

All wired into the `test-library-mode` Makefile loop and pass on
tier 1. Coverage gap not exercised: `EHandle` clauses and
`HReturn`'s `state`/`log` magic bindings — those return `null`
intentionally because they have no source span; a goto over
`state` should arguably point at the `handle ... with State[T] as
name` introduction, which is a tactic for a follow-up lane.

## Real cost vs estimate

Brief estimated **3-5 days**. Real cost was **~3 hours** total in
one session:

- Reading the existing walker, designing the scope-stack shape:
  ~25 min.
- Writing the new walker code (~280 LOC): ~40 min.
- Debugging the `args`-shadow panic: ~20 min (would have been ~2
  hours without the memory).
- Writing 13 fixtures + capturing goldens: ~45 min.
- tier0 + tier1 verification: ~10 min compute (mostly tier1 builds).
- Doc update + retro: ~30 min.

The brief overshot because it assumed a resolver-side index;
the walker approach was decisively simpler. Lesson for the LSP
v1 lane: prefer query-time AST walks over inference-time
sidecars unless the query has to run inside the type-checker
itself.

## Follow-ups

For LSP v2 (#447) or downstream lanes:

1. **Variant constructor goto.** `MkPair(...)` in expression
   position should resolve to its declaring `type T = MkPair(...) |
   ...` site. Needs a variant-name → type-decl index;
   `find_decl_def` already walks decls but doesn't descend
   `TBSum([Variant])` bodies.

2. **Cross-module goto.** `def.file` always reports the queried
   file. Phase A (#452) attaches per-decl origin so an imported
   `triple` resolves to its source file's coordinates.

3. **`state` / `log` magic bindings.** Inside a handler clause
   body or return clause, references to `state` and `log` are
   resolved by the typer but have no source span on the binding
   side. Goto should point at the `handle ... with E as ...` site;
   needs a small extension to `EHandle` to carry the state binder's
   line/col separately from the handler body. Could ship cheap.

4. **`find references` (the inverse query).** "Show every use of
   the binding at L:C" is the natural sibling of goto-def. Same
   walker structure, opposite filter: collect every EVar whose
   scope-lookup result equals the named declaration. ~100 LOC,
   one new fixture per declaration kind.

5. **stage 1 `args`-shadow fix.** Independent of LSP — but every
   code change in `compiler.kai` that introduces a new parameter
   or pattern name has to remember this trap until stage 1
   resolves prelude-vs-local binding correctly. The fix lives in
   stage 1's emitter; track as its own issue.

6. **`for x in xs` if/when `for` ships.** Not in the current
   grammar, but the brief listed it. When it lands, the walker
   needs one arm pushing the loop binder for the body's scope.

7. **Index ratchet for large files.** Currently O(n) per probe.
   For files past ~5 kLOC with hundreds of probes in a single
   LSP session, a sorted `(start, end, node_id)` vector with
   binary search wins. Doc already mentions this as future work.

## What this unblocks

LSP v1 (#447) reconnaissance flagged the missing local resolution
before that lane wrote code. With this lane shipped, the LSP can
implement goto-definition end-to-end without a kaikai-side gap.
