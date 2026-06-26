# Lane experience — issue #928: point-free pipe into a Self-bound generic

## Scope as planned vs as shipped

**Planned.** Make `xs |> list.sum` resolve `Self` exactly like the
direct call `list.sum(xs)`, on both backends. The bug: a point-free
apply-pipe into a `[T : Numeric]` generic whose body calls an arity-0
bound op (`zero()`) panicked at runtime with `no impl of Numeric.zero
for runtime head (arity 0 op: caller must annotate Self)`, while the
direct call worked.

**Shipped.** Exactly that, with a one-line root cause in
monomorphisation. The fix is a single new `EPipe` arm in
`rewrite_callsites_kind_sm` plus a 12-line helper
(`rewrite_pointfree_callee`). No typer change, no emitter change.

## Where the bound instantiation was lost

The brief's hypothesis ("the apply-pipe desugar drops the propagation
of `T:Numeric`") was the right symptom but the wrong layer. The typer
is **not** at fault — `--dump-mono` showed both the direct and the
piped `sum` call sites resolving identically to `[Int]`, and the
recorded `RCS` for the pipe site (`sum@4:21 [Int]`) was correct.

The divergence is purely in monomorphisation's **call-site rewrite**.
`rewrite_callsites_kind_sm` retargets a callee to its `__mono__` spec
only when it sees an `ECall(callee, args)` whose callee is `EVar` or
`EModCall`. A point-free `xs |> list.sum` keeps the callee as a **bare
`EModCall("list", "sum")` in the `EPipe` rhs** — never wrapped in an
`ECall` — so the retarget never fired. The generic recursion
(`mono_map_expr_kind`) walked into the rhs but its `EModCall(_, _) ->
k` arm leaves a bare modcall untouched.

Result: `kai_main` called `kai_list__sum__mono__Int` for the direct
form (where `zero()` is statically `kai_numeric____pimpl_Numeric_Int_
zero()`) but the **generic** `kai_list__sum` for the pipe — and the
generic body reaches the arity-0 protocol dispatcher stub, which is a
loud panic by design (`kir_lower_fns.kai` `lower_proto_dispatch` /
`emit_c.kai`). The emitter was a red herring named in the brief: it
faithfully emits `kai_<c_sym(fname, mod)>(lhs)` from whatever name the
`EModCall` carries — so retargeting the node in monomorph is enough;
the emitter picks up the mangled name with no emitter edit.

## The fix

Add an `EPipe(lhs, rhs)` arm to `rewrite_callsites_kind_sm` that
recurses on `lhs` and, when `rhs` is a bare point-free callee
(`EVar`/`EModCall`), retargets it through the same
`rewrite_bare_call` / `rewrite_qualified_call` logic the `ECall` arms
use — called with empty args, since the recorded `RCS` sits at the
callee's own position and supplies the type args (no arg-type synthesis
needed). A non-callee rhs recurses normally. The retargeted callee is
extracted from the `ECall(c, _)` those helpers return and becomes the
new `EPipe` rhs.

## Generality (not a `sum` special-case)

The retarget keys off "this point-free callee has a recorded
instantiation and an emitted spec tuple" — identical to the saturated
call path. Verified on both backends with one fixture covering:

- `|> list.sum` over `[Int]` (arity-0 `zero()`),
- `|> list.product` over `[Int]` (arity-0 `one()`),
- `|> list.sum` over `[Real]` (a different `Numeric` impl),
- `empty |> pick_default`, a **user-declared** `[T : Default]` generic
  whose body calls the arity-0 `Default.default()` — proving the path
  is not stdlib- or `Numeric`-specific.

Each line asserts piped == direct. A bonus confirmation: the demos
no-regression baseline ticked from 35 → 36 because `demos/euler1`
(`… |> sum`) and `demos/vs/python` (`… |> list.sum`) were silently
broken by this bug and now run — these are the real-world shape the
book's cap06 pipe examples use.

## Fixtures

`examples/pipes/collections/pointfree_self_bound.kai` (+ golden). It
lives in the `collections/` subdir because that is the only
`examples/pipes` harness wired with `--path ../stdlib` (the flat
`test-pipes` glob has no stdlib path, and `list.sum` needs it). The
fixture is an apply-pipe (`|>`) guard, distinct from the
convention-dispatch (`|`/`||`/`|?`) siblings around it — the header
says so.

## Verification

- Repro prints `direct=10 pipe=10` on `--backend=c` and
  `--backend=native`.
- `make selfhost` byte-id green (C); `make tier0` green.
- Serial native-vs-C parity (`BACKEND_PARITY_JOBS=1`) clean.
- `test-pipes`, `test-pipes-collections`, `test-protocols` green.

## Cost vs estimate

Small. The diagnosis was the lane — the brief pointed at the desugar
and the emitter; the actual fault was one missing match arm in the
mono rewriter. The `--dump-mono` / `--dump-mono-out` / generated-C
triangulation is what relocated it: both call sites resolving to the
same `sum__mono__Int` ruled out the typer and the emitter, leaving the
rewrite pass as the only place the two forms could diverge.

## Follow-ups

None required for this bug. A latent question worth a separate look:
`EMapPipe`/`EFlatMapPipe`/`EFilterPipe` are rewritten by the typer
into `ECall(EModCall(...))` shape, so they already flow through the
existing `ECall` retarget and are unaffected — but if a future change
ever lets one of those keep a point-free callee, it would want the
same treatment. Not in scope here; called out so the next lane knows
the boundary is deliberate.
