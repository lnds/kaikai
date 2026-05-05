# Lane experience — issue #285 (Ref[T] loop closure misdispatch)

## Objective metrics

- Branch: `issue-285-ref-loop-closure-arraylift`
- Base: `main` at ed80e7a (post #284 + 0.42.0 bump).
- Lines changed in `stage2/compiler.kai`: ~80 (additive, scope tracking).
- New regression fixtures: 2 (`issue_285_ref_in_until.kai`,
  `issue_285_ref_in_while.kai`) under `examples/sugars/loop/`.
- Tier gates: tier0 / tier1 / tier1-asan / selfhost / selfhost-llvm
  all green.
- Selfhost byte-identical on both C and LLVM backends.

## Diagnosis

Turned out to be **none of the prelabeled hypotheses A/B/C** — the
typer was correct end-to-end. `--dump-typed` confirmed that
`@count` resolves to `Mutable.ref_get(count)` with `count: Ref[Int]`
both inside and outside the trailing-lambda body.

The bug lived in **lambda-lifter free-variable analysis**, much
later in the pipeline:

1. The parser's `@count` desugar produces `count.get()` (an
   `EField(EVar("count"), "get")` callee).
2. The typer's `try_ref_sugar_op` correctly rewrites that to
   `Mutable.ref_get(count)` because `count`'s type is `Ref[Int]`.
3. **But** the closure-capture pass (`fv_expr` /
   `collect_expr` for `ELambda`) computes the captured-name set
   against a flat `globals: [String]` list seeded from every
   visible function. Because the auto-loaded `stdlib/core/list.kai`
   exports `pub fn count(xs, p)`, the bare name `count` is in
   `globals`. `fv_expr` therefore drops `count` instead of marking
   it as a free variable, and the lambda's `LamInfo.caps` array
   does not include it.
4. At emit time the closure body's `EVar("count")` falls through
   `lcs` (empty for that name), through `efn_resolve` finds
   `list.count`, and emits `kai_closure(&_kai_list__count_thunk,
   2, 0, NULL)` as the receiver of `Mutable.ref_get`.
5. The runtime hands that closure value to the default `ref_get`
   shim, which `array_get`-walks it and crashes with
   `array_get: not an array`.

Confirmed empirically by renaming `count` → `counter` (no global
collision): the bug disappears. The same shape with a `let count
= Mutable.ref_make(0)` outside any loop also reproduces — so the
fix is not specific to `loop.until` / `loop.while`, it covers
every closure that captures a name shadowing a global function.

## The fix

Add an `enclosing_scope: [String]` field to `LamCollect`. Push
the current fn's parameter names when entering a `DFn` body;
extend it with `SLet` / `SVar` bindings as the walker descends a
block; restore the outer scope on exit. When `collect_expr` hits
an `ELambda`, compute `effective_globals = globals \ local_scope`
and feed *that* to `fv_expr`. Names shadowed by an enclosing
binding are no longer treated as globals, so they are marked as
free and lifted into the lambda's capture array.

The same shadowing rule is applied inside `lc_record_clauses` so
handle clauses (which also walk via `fv_expr`) capture
shadowed names through their env channel.

The change is additive — every other field of `LamCollect` and
the `globals` list is unchanged — so the only behavioural delta
is that previously-dropped captures are now captured. The
selfhost fixed point still converges byte-identically.

## Regression fixture shape

`examples/sugars/loop/issue_285_ref_in_until.kai` and
`issue_285_ref_in_while.kai` mirror the issue's repro: a fn
parameter `count: Ref[Int]` (colliding with `list.count`), read
via `@count` inside the trailing-lambda body. Each fixture
`print`s `3`. The two fixtures cover both `loop.until` (do-while
shape) and `loop.while` (while shape) so a future regression
that breaks one path independently still gets caught.

## Empirical verification

```
$ bin/kai run /tmp/repro285.kai
3
$ bin/kai run examples/sugars/loop/issue_285_ref_in_until.kai
3
$ bin/kai run examples/sugars/loop/issue_285_ref_in_while.kai
3
```

## Friction points

- The "pick a hypothesis A/B/C" framing in the brief was a red
  herring — the typer was clean. Empirical comparison of the
  generated C between the prelude-light and full-prelude builds
  exposed the real failure (prelude-light: `kai_internal_dup(kai_count)`;
  full-prelude: `kai_internal_dup(kai_closure(&_kai_list__count_thunk,
  2, 0, NULL))`). Without that comparison the chase would have
  stayed inside the typer indefinitely.
- The "no such field `local_scope`" runtime panic surfaced after
  the first `make tier0` — caused by a `LamCollect { ... }`
  literal in `lc_record_clauses` that I missed when adding the
  new field. kaikai's record literals require all fields, so the
  selfhosted compiler crashed on its own AST. Easy fix
  (one-line) once seen, but reminded me to grep for **every**
  `LamCollect {` literal after the type change.

## Subjective summary

A reasonably clean two-step lane: 30 minutes to reproduce + dump
the typed AST + spot the C-output divergence; 1 hour to design
the `local_scope` threading and write the fix; 30 minutes for
fixtures and tier gates. The fix lives in one structural place
(the lambda lifter's scope model), so I did not need to touch
typer, emitter, or runtime — they already do the right thing
once the captures arrive correctly. Total wall time ≈ 2.5h,
under the 3-4h budget.

## Limitations

- The fix tracks scope only through `DFn` parameters, `EBlock`
  `SLet` / `SVar`, and `ELambda` parameters. Pattern bindings
  in `EMatch` arms are *not* threaded — if a match arm binds a
  name that shadows a global, and a closure inside that arm
  references the bound name, the same bug pattern would
  re-emerge. No fixture exists today, so this is documented as
  a known gap rather than fixed eagerly.
- `DCheck` parameters are now seeded too, but `DTest` / `DBench`
  bodies start with an empty `local_scope` (they have no
  parameters by construction). If a future test syntax allows
  parameters, this would need extension.

## Build log

```
timestamp	cmd	outcome	elapsed_s
2026-05-05T15:03:30-04:00	tier0	OK	-
2026-05-05T15:25:36-04:00	tier1	OK	-
2026-05-05T15:25:36-04:00	tier1-asan	OK	-
2026-05-05T15:25:36-04:00	selfhost	OK	-
2026-05-05T15:26:55-04:00	selfhost-llvm	OK	-
```
