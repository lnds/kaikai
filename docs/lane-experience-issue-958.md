# Lane experience — issue #958: point-free sections for method calls and chained fields

## Scope as planned vs as shipped

Planned (from the issue): extend the existing single-field point-free
section (`xs | .price`, `o.map(.name)`) to three more shapes —
`.method()`, `.method(args)`, and `.a.b` chains — in pipe and UFCS
combinator positions, and document the feature in `kai info`.

Shipped: exactly that. Three failing repros (`.length()`,
`.starts_with("a")`, `.addr.city`) now compile and run on the native
default backend. Five fixtures (method, method-args, chained-in-pipe,
method-in-UFCS, method-not-found negative) wired into `test-sugars`.
Sections documented in both `kai info syntax` and `kai info idiomatic`.
No scope crept in; no half-feature deferred.

## Design decisions

**Parser: root `parse_postfix_rest` at the placeholder, don't reparse.**
The single-field section built `EField(__pl, name)` directly and stopped.
The clean generalisation was to build that head and then hand it to the
existing `parse_postfix_rest`, which already knows how to grow a postfix
chain — field paths, calls, calls-with-args, index, `!`. The leading-dot
section thus inherits the whole postfix grammar for free, and the
`(...)`-trailing discriminant between "method section" and "field access"
falls out of the same parser that handles `obj.method()` everywhere else
— LL(1)-clean with no new lookahead.

The earlier behaviour for `.a.b` was a latent bug: the head `(__pl) =>
__pl.a` was built, then `.b` applied to the *lambda value*, producing
`EField(lambda, b)`. The fix (chain inside the lambda body) is why
`.addr.city` now type-checks at all.

**Typer: thread the element type before walking the body.** The
"receiver not resolved" errors came from a timing asymmetry. A bare
field access on a free-tyvar receiver *defers* (`DeferredField`,
resolved post-`check_body_row`). A UFCS method call cannot defer — it
must resolve dispatch eagerly — so `.length()` on a free `__pl` errored
where `.length` deferred. Rather than build a deferred-UFCS mechanism
(large, and it would duplicate the dispatch logic), the fix pins the
section's `__pl` to the combinator's element type *before* the body is
walked. By then the receiver is concrete and ordinary UFCS dispatch
runs. This is the same channel the pipes already use; the issue
predicted the fix would "ride that existing channel", and it did.

Threading is gated on `is_point_free_lambda` (a single `__pl`-named
param — the parser's synthetic marker), so user-written lambdas keep
the unchanged synth path and no existing diagnostic shifts. `synth_many`
got a threaded sibling (`synth_many_threaded`) that pairs each argument
with its declared parameter type and pins only point-free args — a
no-op for everything else.

**`_` placeholder: pick one form, don't ship two.** The issue flagged
the overlap with the `|>` positional placeholder `_`. Decision: a
point-free section is unary, receiver-implicit; it does not take a
positional `_`. A section that tries (`.sub(7, _)`) is rejected (the
inner desugar already turns it into an arity mismatch). Documented in
both info pages. No second way to spell the same thing.

## Alternatives considered

- **Deferred UFCS dispatch** (mirror `DeferredField` for method calls):
  rejected — it would re-implement the dispatch resolver in the drain
  path and thread method args through the deferral, far more surface
  than threading the receiver up front.
- **A dedicated parser-level guard rejecting `_`/`.` inside a section**:
  written first, then removed. The inner `desugar_placeholder_args` runs
  before the guard could see an `EPlaceholder`, making the guard
  unreachable. The rejection still happens (arity mismatch), so the dead
  guard was deleted rather than kept as cosmetic.

## Structural surprises

- The single-field `.a.b` case was already subtly wrong before this lane
  (chain applied to the lambda, not the receiver). The issue framed it
  as "only one field level resolves"; the real cause was the postfix
  chain escaping the lambda body. Worth knowing the prior form was
  never fully correct for depth > 1.
- **Worktree edit leak.** Early edits to `parse.kai`/`infer.kai` landed
  in the main checkout (`kaikai/`) instead of the worktree
  (`kaikai.point-free-958/`) because the file paths from the initial
  grep resolved against the wrong tree. Caught when the rebuilt bundle
  showed zero occurrences of the new functions. Recovered by diffing
  main, applying to the worktree, reverting main. Lesson reinforced:
  edit only with absolute worktree-prefixed paths.

## Fixtures and coverage

`examples/sugars/point_free_{method_pipe,method_args_filter,chained_field_pipe,method_ufcs}.kai`
(positive, `.out.expected`) and `point_free_method_not_found_neg.kai`
(negative, `.err.expected`). The negative pins the method-not-found
diagnostic — proving the receiver *is* resolved (it names `String`),
which is the inverse of the bug. Gap: no fixture for `||` flat-map with
a point-free section, since the canonical flat-map body must return a
collection and a bare projection rarely does; covered by inspection,
not a fixture.

## Cost

Two compiler files (`parse.kai`, `infer.kai`), ~90 LOC net, plus five
fixtures and two doc sections. selfhost byte-id held (same compiler,
same self-compile output), so no monomorph/codegen surprise. The bulk
of the time went to the wrong-tree recovery and to locating the
defer-vs-eager asymmetry, not to the change itself.

## Follow-ups

- Consider whether `|>` apply should accept point-free sections too
  (out of this issue's scope; `|>` rarely pairs with a projection).
- The `synth_many_threaded` / `synth_pipe_rhs` element-type extraction
  (`pipe_elem_ty`) handles `[A]` and `T[A, ...]` heads; a head whose
  element is not its first type argument would not thread. No such head
  ships today; revisit if one appears.
