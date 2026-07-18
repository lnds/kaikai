# Lane experience — issue #1249

Suggest `|>` when `|` (map) fails because the stage function takes the
container rather than the element.

## Scope as planned vs as shipped

Planned and shipped match: a pure diagnostic change in the pipe
type-check failure path. No lowering, no semantics, no new surface
syntax. `EMapPipe` still desugars to `<module>.map(lhs, f)` exactly as
before; the only difference is which diagnostic the failure path emits.

Shipped slightly wider than the brief in one respect: the detection is
shared by all three dispatching pipes (`|`, `||`, `|?`) rather than
`|` alone, because `synth_pipe_dispatch` is the common lowering for
map / flat_map / filter and the container-vs-element confusion reads
identically in each. `pipe_surface_op` maps the internal op name back
to the operator the user actually typed, so the message never leaks
`map` / `flat_map` / `filter`.

## Design decisions

**Where the check lives.** `synth_pipe_dispatch` (infer.kai), between
building the expected fn type and calling `st_unify_call`. At that
point the LHS type and the stage's parameter type are both resolved,
which is precisely what the detection needs, and short-circuiting
before `st_unify_call` means the generic mismatch is *replaced* rather
than accompanied — the user gets one diagnostic, not two.

**How the shape is detected.** `pipe_stage_wants_container` returns
`Some(param)` iff the stage's single parameter unifies with the
container type but *not* with its element type. Both probes use
`unify_env`, which is non-destructive (it returns a candidate `Subst`
rather than mutating), so the check cannot pollute the real
substitution on the way to a diagnostic.

**Why the element probe is load-bearing.** Requiring `not takes_elem`
is what keeps the suggestion honest on nested containers. For
`[[Int]] | sum_all` where `sum_all : ([Int]) -> Int`, the parameter
unifies with the container's *element*, so this is a correct map and
no suggestion fires. This is the same element-vs-container ambiguity
the issue's design review flagged as the reason not to fuse the pipes;
here it only costs a suggestion, not soundness.

## Structural surprise the brief did not anticipate

The first implementation regressed `map_pipe_lambda_block`, an
existing green fixture:

```kaikai
let squared = [1..5] | { x -> x * x }
```

An un-annotated lambda's parameter is still a metavariable at this
point, and a metavariable unifies with the container for free — so the
detection fired on a perfectly good map and turned a compiling program
into an error. The fix is the `param_open` guard: an unresolved
`TyVarT` / `TyAny` parameter is not evidence of anything, so the
detection declines and the normal path runs.

Worth recording because the failure mode was *not* a bad message on a
broken program — it was a false positive that rejected working code.
A diagnostic lane can still break compilation if it short-circuits the
success path, so the "diagnostics are safe" intuition does not hold
when the detection runs before unification rather than after failure.

## Fixtures

Both under `examples/sugars/`, auto-discovered by `test-sugars` (an
`.err.expected` sibling switches the harness to negative mode; no
Makefile wiring needed).

- `pipe_map_wants_apply` — `[1,2,3] | total` with
  `total : ([Int]) -> Int`. Golden pins the message, the `|>` help
  line, and the span.
- `pipe_map_genuine_mismatch` — `[1,2,3] | takes_str` with
  `takes_str : (String) -> Int`. The stage matches neither container
  nor element, so the golden pins the *generic* `list.map` mismatch,
  proving the suggestion does not over-fire.

The harness greps each golden line as a regex, so `|` must be written
`[|]` — an unescaped `|` is an alternation with an empty branch and
matches everything, which silently turns a golden into a no-op. Cost
about three iterations to notice.

Coverage gap: the guard against false positives on nested containers
(`[[Int]] | sum_all`) was verified by hand but has no fixture, because
the sugars harness has no natural home for "this compiles and must
keep compiling *and* must not emit a suggestion". `map_pipe_*`
positives cover the compiles-clean half.

## Gates

`make tier0` green (selfhost byte-identity `kaic2b.c == kaic2c.c`
intact, as expected for a diagnostic-only change). `test-sugars` green
at 205 fixtures. `test-diagnostics`, `test-infer`, `test-infer-check`,
`test-pipes`, `test-holes`, `test-lint` all green.

`test-pipes-collections` fails on `hashmap_pipes` with a runtime
`field access on non-record`. Verified pre-existing: reproduced on a
baseline compiler built from `main` with this lane's diff stashed. Not
this lane's, and left alone per lane discipline.

## Follow-ups

- The issue asks for a fix-it replacing `|` with `|>`. This lane ships
  the message and the `help:` line but no structured fix-it; the
  diagnostic path here emits via `diag_error` / `diag_help` and has no
  span-rewrite channel. Wiring a real fix-it means extending the
  collected `Diagnostic` with an edit, which is a separate lane.
- `pipe_stage_wants_container` only inspects single-parameter stages.
  A curried / multi-param stage that takes the container is not
  detected. No fixture demanded it, and the common shape is unary.
