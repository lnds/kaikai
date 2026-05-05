# Lane experience — issues #244 + #248

## Objective metrics

- Branch: `issues-244-248-axiom-export-trailing-lambda` from `main` (post #250).
- Wall clock: ~36 minutes from `lane-start` to all gates green.
- Files modified: `stage2/compiler.kai` (+62 / -6), `stage2/Makefile` (+65 / -2).
- Files added: 4 fixtures (1 cross-module FFI, 3 loop sugar) + this report.
- Tier gates locally: tier0, tier1, tier1-asan, selfhost (C), selfhost-llvm — all OK.

Build TSV is appended at the end of this file.

## Diagnosis

The two bugs are **orthogonal**: different files, different passes, different
mechanisms. The lane brief grouped them because both surfaced while building
the raylib demo. They share no code path and the fixes do not touch each
other's territory.

### #244 — cross-module `pub axiom` export

`tag_decl_module_origin` (the per-module pass that stamps every freshly-parsed
`DFn` with `module_origin = Some(<mod>)` so the codegen mints
`kai_<mod>__<name>`) had no arm for `DAxiom`. The match fell through to the
default `_ -> d` and the axiom decl kept its parser-set `mo = None`.

`lower_axioms` runs later in the pipeline and rewrites every `DAxiom` to a
`DFn`, but at the call site:

```kaikai
DFn(is_pub, name, tparams, params, Some(ret_ty), row, body, l, c, None)
```

it always passed `None` for `module_origin`. So the lowered FFI shim emitted
under the bare `kai_<name>` C symbol while the importer's call site emitted
`kai_<mod>__<name>` (because the importer side resolves the module and
qualifies the call). Linker fail.

The fix lowers `DAxiom` *during* tagging (so the produced `DFn` immediately
carries the module-origin stamp) and adds the same arm to `tag_target_decl`
with the standard `pub`-only carve-out (a non-`pub` axiom is module-internal
and stays bare). `lower_axioms` later runs over decls that are already `DFn`,
so its `DAxiom` arm becomes a no-op for the tagged ones — no duplication.

### #248 — `loop` sugar fails to typecheck under multi-candidate names

`bin/kai` auto-loads every stdlib core prelude (`list.kai`, `string.kai`,
`option.kai`, …). `stdlib/list.kai` and `stdlib/core/string.kai` both export a
function named `repeat`, and `stdlib/loop.kai` adds a third. Inside
`loop.kai`, the recursive call

```kaikai
pub fn repeat(n: Int, body: () -> Unit / e) : Unit / e =
  if n > 0 { body(); repeat(n - 1, body) } else { () }
```

was being inferred as `Int` instead of `Unit`, breaking the `if`/`else` branch
unification, the body's return-type unification, and the original call site.
Importing `loop` therefore failed in the typer before any of the §5 sugars
even reached parsing.

The narrowing helper `pick_by_first_arg_loop` walks every `<mod>::repeat`
candidate, instantiates each scheme with `st_instantiate_report`, and probes
its first parameter with `unify_env`. The probes' returned substitutions are
discarded (the function only commits the picked one). But the substitution's
backing store is an opaque-mutable kaikai `Array[Slot]`. `unify_env` extends
that array via `subst_extend(s, v, ty)`, and the underlying array mutates in
place — even when the returned `Subst` record is thrown away.

Net effect: when a later candidate's probe writes a binding for tyvar slot
`N`, that binding persists for any future `apply_ty` lookup of slot `N`. The
function then returned a `pick.st` captured **before** the later candidates'
probes ran. The caller used that `pick.st` to pick the next fresh tyvar id,
which collided with a slot id whose discarded probe had bound it to a concrete
type (e.g. `list.repeat(x: a, n: Int)`'s `a` probe-bound to `Int`). The fresh
tyvar therefore came back already-bound to `Int`, the call-site's expected
return type became `Int`, and `Unit` failed to unify.

The fix advances each pending pick's `st` to the latest iteration's state
after every step. The substitution slots that the discarded probes wrote to
remain bound (we don't snapshot/restore the array — that would be a much
larger change), but `env.fresh` skips past every slot any probe touched, so
subsequent allocations land in untouched territory.

The bug pre-dated PR #250 — selfhost ran fine because `kaic2` self-compiles
without `import loop`. `make test-loop` ran fine because it invokes `kaic2
--path ../stdlib` directly without auto-loading the core preludes. Only
`bin/kai run` triggered the multi-candidate `repeat` lookup, which is why the
demo agent surfaced it.

## Fix shape

| Site | Lines | What |
| --- | --- | --- |
| `tag_decl_module_origin` (#244) | +10 | New `DAxiom(_,…)` arm — lowers in place via `lower_axiom_one` then re-tags. |
| `tag_target_decl` (#244) | +7 | New `DAxiom(p,…)` arm with `pub`-only carve-out. |
| `pick_by_first_arg_loop` (#248) | +27 / -6 | Refresh each pending pick's `st` after every iteration. |
| `pick_advance_st` (new helper, #248) | +8 | Trivial state replacer; `None` branch is unreachable but kept exhaustive. |

`stage2/Makefile`: +1 new test target per issue (`test-ffi-pub-axiom-cross-module`,
`test-issue-248-loop-sugars`), wired into `test`, `test-fast`, and `.PHONY`.

The fixture for #248 lives in `examples/sugars/loop/` (a new subdirectory) so
the existing `test-sugars` glob (`examples/sugars/*.kai`) doesn't pick it up
without `--path ../stdlib` and trip over the unresolved `import loop`.

## Empirical verification

```
$ /Users/ediaz/.../bin/kai run /tmp/until-test.kai     # the issue #244 repro
5
4
3
2
1
$ /Users/ediaz/.../bin/kai run /tmp/ffi244/main.kai    # the issue #248 repro
11
```

Both produce the bytes the issues claim.

`make tier0`, `make tier1`, `make tier1-asan`, `make selfhost`, `make
selfhost-llvm` all green. Selfhost byte-identical on both backends.

## Friction points

1. The lane brief steered toward `parse_trailing_lambda` for #248. The
   trailing-lambda parser was actually fine — the bug was in
   `pick_by_first_arg`, which only the typer touches. Reproducing showed the
   error fires even for paren-form calls and for fixtures that never use
   trailing-lambda syntax. The diagnosis took the longest single block of the
   lane (~30 min) because the failure mode looked like a sugar bug.
2. `kaikai`'s opaque-mutable `Array` substitution backing — a deliberate
   `Mutable`-effect escape per `docs/effects-stdlib.md` — makes "discard the
   sub" a footgun. Every probe-style helper that calls `unify_env` and throws
   the result away must defensively manage the slot id space. Worth a TODO
   audit for other narrow-and-discard sites; not in this lane's scope.
3. Initial fixture placement under `examples/sugars/` collided with
   `test-sugars`'s glob. Subdirectoring fixed it but the choice of glob is
   fragile — adding any future stdlib-importing fixture requires the same
   workaround.

## Subjective summary

Smaller in code than budgeted (fix is ~50 effective lines, not 50–150).
Larger in diagnosis time than budgeted because the bug masquerade was
convincing — the issue title named the sugar, and even the issue body's
hypothesised root cause matched the wrong subsystem. The mutable-array
substitution interaction is the kind of bug that's easier to find with print
debugging than reasoning, and the kaic2 build cycle (~5 s with cached stage1)
made the print-debug loop bearable.

## Limitations

- The fix retains the dirty bindings in already-allocated slots; only
  `env.fresh` advances. Any code path that intentionally reuses slot ids
  before probing would still pick up the leftover bindings. None such exists
  today, but the invariant ("never reuse a slot id allocated during a
  pick_by_first_arg pass") is implicit, not enforced.
- The #248 fix narrows by `env.fresh` only. If `subst_fresh_row`'s row-slot
  array exhibited the same mutate-and-discard pattern through `subst_row_extend`,
  we'd need the same advancement on `row_fresh`. The current probes only call
  `unify_env` over types, not rows, so this hasn't bitten yet — but it's a
  latent companion bug if the probe ever grows row checks.
- The new `examples/sugars/loop/` subdir is a one-fixture solution to a
  test-glob collision; future loop-related sugar fixtures should land here
  too. A more durable fix would be to re-template `test-sugars` with a
  per-fixture `.flags` file, but that's a tooling cleanup outside the lane.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-04T21:20:38-04:00	tier0	OK	47
2026-05-04T21:25:56-04:00	tier1	OK	298
2026-05-04T21:27:01-04:00	tier1-asan	OK	55
2026-05-04T21:27:40-04:00	selfhost	OK	31
2026-05-04T21:28:28-04:00	selfhost-llvm	OK	42
2026-05-04T21:36:00-04:00	tier1-with-fixtures	OK	299
```
