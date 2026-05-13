# Lane retro — issue #443: protocol op called inside string interpolation fails to resolve when called with multiple arguments

## Bug confirmation status

Reproduced cleanly on HEAD (`1520660`) before any change:

```kai
type Punto = { x: Int, y: Int }
impl Eq for Punto { fn eq(a: Punto, b: Punto) : Bool = a.x == b.x and a.y == b.y }
fn main() = {
  let p = Punto { x: 1, y: 2 }
  let q = Punto { x: 1, y: 2 }
  println("eq: #{eq(p, q)}")
}
```

```
error: cannot find `eq` in this scope
  --> /tmp/repro.kai:1:1
```

Anchored at line 1 — the diagnostic position bug the issue body called out is real.

## Root cause

The driver runs `lower_protocols` *before* `desugar_interp_decls` (stage2/compiler.kai
line 57316 vs 57354). `lower_protocols` strips `DProtocol`/`DImpl`, generates one
`__proto_<op>` dispatcher per op, and runs `rename_proto_calls_decls` to rewrite
every `EVar(opname)` → `EVar(__proto_<opname>)` so the resulting decl stream contains
only references the typer can see (the original `eq` symbol no longer exists as a
top-level fn).

But `desugar_interp_decls` (next phase) re-parses the inner expression of every
`#{...}` body from its source span. That sub-AST never went through
`rename_proto_calls_decls`, so a multi-arg call like `eq(p, q)` survived as a bare
`EVar("eq")`. By the time the typer ran, `eq` had been removed from the env, so it
fired "cannot find `eq` in this scope". The single-arg `show(p)` case
*accidentally* worked because `interp_part_to_expr` always wraps the inner expr in
`__proto_show(...)` regardless — that wrap *is* the show dispatcher, so single-arg
Show calls pre-dispatched themselves.

## Option taken

**Option A from the brief**, with a tweak: instead of reordering passes, re-run
`rename_proto_calls_decls` on the post-interp decl stream. That gives us the
arity-aware, scope-aware rewrite the rest of the program already enjoys, applied
to references that have just surfaced from string spans. The pass is idempotent
on already-renamed `EVar(__proto_<opname>)` (the rewriter checks the op-name
list, not the dispatcher prefix), so the rerun is a no-op for everything outside
freshly-surfaced interp parts.

Rejected:

- **Reorder so `desugar_interp_decls` runs first.** Tempting but risky — the
  desugar pass receives `op_names`/`op_arities` from `lowered.reg`, so it needs
  `lower_protocols` to have run first to know what is a proto op. Reordering
  would require routing the registry construction earlier or running it twice.
- **Inline `rename_proto_calls_expr` inside `interp_part_to_expr` with `bound=[]`.**
  Tried this first; the invariant walker (`rep invariant (b)`) caught it because
  shadowing a proto-op name with a `let` (`let cmp = 42; "#{cmp}"`) ended up
  rewriting the local binder reference into the dispatcher name. Threading
  scope through `desugar_interp_*` would have duplicated `rename_proto_calls_*`'s
  scope tracker — exactly what the rerun avoids by reusing it.

## Implementation surface

`stage2/compiler.kai`:

- `desugar_interp_decls` and the 16 helpers it transitively calls now thread
  `op_arities: [OpAr]` alongside the existing `op_names`. The new arg flows
  through `desugar_interp_kind`, the helpers (arms, fields, list elems, stmts,
  hclauses, hreturn), `lift_interp_string`, and `interp_part_to_expr`. Mechanical
  threading; no logic in those helpers consults `op_arities` directly. Pre-existing
  call sites in `desugar_interp_kind` (`EField` rewrite, `Show` gating) still use
  `op_names` and behave identically.
- The driver (`emit_program`'s pipeline at line ~57316) builds `op_arities` the
  same way `lower_protocols` does (`op_arities_from_ops` + `collect_local_fn_arities`
  + `filter_shadowed_ops`) and then re-runs `rename_proto_calls_decls` on the
  post-interp decl stream.

`examples/protocols/eq_in_interpolation.kai` + `.out.expected` — positive
fixture exercising both the `true` and `false` branch of an `Eq` impl on a
record, called inside `#{...}`.

## Why threading `op_arities` if the rerun does the work?

Symmetry. The pre-existing `EField`-method rewrite inside `desugar_interp_kind`
(line 22877) consults `op_names`. Threading `op_arities` keeps the door open for
the same arity-aware filtering to apply there if the EField path ever needs it,
without re-introducing the threading later. Cost is a few hundred extra arg
slots in mechanically-generated code — negligible.

## Sweep findings

`grep -rE '#\{[a-z_]+\([^)]*,' examples/ demos/ stdlib/` returned no
multi-arg-protocol-op-in-interp shapes, confirming the workaround
("let-bind first, then interpolate") was already universal in the codebase.
The bug shape was real but rare in shipped code; chapter 9 of the kaikai-book
(per the issue body) is the natural surface where users would hit it.

Selfhost is byte-identical post-fix: stage2/compiler.kai itself doesn't
multi-arg-call protocol ops inside `#{...}` (predictable — it's a self-host
compiler, not a user program demonstrating Eq).

## Negative fixture?

None added. The fix doesn't introduce a new diagnostic shape; it makes a
previously-broken case work the same as the workaround. The pre-existing
"shadowed protocol op name" diagnostic (`rep invariant (b): protocol-dispatcher
reference __proto_cmp shadows local binder cmp in scope`) still fires correctly
when a `let cmp = 42` is followed by `cmp(...)` (verified during the option-A
rejection check).

## Cost real vs estimate

- Linus estimate: 0.5 day.
- Actual: ~3 hours (one false start with `bound=[]` that the invariant walker
  caught, one missing `Show` declaration in the fixture that surfaced when
  test-protocols ran kaic2 without the bin/kai prelude wiring).

## Follow-ups

None. The fix is complete and the fixture covers the regression shape.
The diagnostic-position bug (error pointing at line 1:1 instead of the actual
call site) was inherited from how the typer surfaces "name not in scope" errors
generally; it goes away because the call now resolves. A separate effort to
improve "name not in scope" diagnostics anywhere they fire (issue-tracker
search did not surface a current open issue for that) could land independently.
