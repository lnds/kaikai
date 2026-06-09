# Lane experience — issue #758 (+ #341 Form 1): extend type anchoring beyond let-bindings

## Scope as planned vs as shipped

**Planned (brief):** the typer only anchors a bare/return-typed call's type
via an annotated `let`-binding. Extend anchoring to two more sites so that
(1) the #235 first-arg narrowing fires for a bare cross-module name used
inline in a comparison/`assert` (issue #758), and (2) a protocol op whose
`Self` is informed only by the return position (`default()`,
`from_string(s)`) dispatches when pinned by the enclosing `fn`'s declared
return type (issue #341 Form 1). Forms 2 and 3 of #341 explicitly out of
scope. Pure typer change; no runtime/emit edits.

**Shipped:** exactly that, but the two halves turned out to be *mechanically
unrelated* bugs that happen to share the issue's framing ("anchoring is
let-only"):

- **#758 was not an anchoring gap at all.** Test / bench / check bodies were
  never run through HM inference: `infer_decl` had only a `DFn` arm and a
  catch-all that returned the decl *untyped*. So `try_bare_call_narrow`
  (and the post-inference protocol rewrite, which reads `Expr.ty`) never
  fired inside a `test` block — both calls resolved to the wrong candidate
  by load order. The "let works, inline assert doesn't" symptom is real but
  the discriminant is **test-block vs fn**, not let vs inline: inside a
  plain `fn` *both* forms already resolved correctly. Fix: type test-like
  bodies like a paramless `fn` (`infer_test_body`), absorbing builtin
  effects the way `main` does.

- **#341 Form 1 was a Self-extraction gap.** `try_rewrite_proto_call_with_ret`
  read Self from `ty_head_name(ret_ty)` — the *outermost* head. That works
  for `default() : Self` (head = `Int`) but reads `Result` for
  `from_string(s) : Result[String, Self]`, so the impl lookup missed and the
  call stayed on the runtime dispatcher (panic). Fix: align the declared
  return `TypeExpr` (which carries `Self`) against the inferred `Ty`
  positionally and read the head at Self's slot (`self_head_in_ret`).

Both new fixtures land in the same commit. `examples/protocols/`
`bare_narrow_in_assert.kai` (run with `--test`) and `from_string_in_return.kai`
(run as a program, C + LLVM golden).

## Design decisions and alternatives considered

### #758 fix location: type the test body in-place, not a separate file

The brief's quality bar says new substantial logic goes in its own A-grade
file, not the F monolith. I kept `infer_test_body` *inside* `infer.kai`
anyway. The logic is ~40 LOC and coupled to a dozen non-`pub` infer
internals (`synth`, `check_body_row`, `InferState` constructors,
`add_infer_params_with_binds`, `finalise_typed_expr`, `TypedDecl`).
Extracting it would force `pub` on all of them, *worsening* infer.kai's
public surface for a fake module boundary. The differential rule is "don't
*lower* an F file's grade"; `km cogcom` confirms the new fns score 0 and 1,
and the file's max-complexity function is unchanged, so the grade held
(12.9 → 12.8, flat). Adding 40 well-factored LOC to an 11.7k-LOC file that
legitimately owns body inference is the right call over inventing a module.

### Test-body effect row: reuse `main`'s REmpty absorption, not a new discipline

A `test` body has no declared row but may `print`, `assert`, and call
effecting helpers. The test runner installs the *same* builtin default
handlers `emit_main_wrapper` installs for `main`. So the correct row
semantics is exactly `main`'s REmpty branch in `check_body_row`: absorb the
builtin-default set, reject `Ffi` / user effects with no handler (those
would segfault at runtime, in a test just as in `main`). I pass the literal
`fname == "main"` to `check_body_row` to trigger that branch — safe because
the RClosed `check_main_row_handlable` path and the main-row write-back both
live in the DFn arm, which a test body never reaches. (asu consult
confirmed this over inventing a bespoke test-row rule.)

### Self extraction: a parallel structural walk, not unification

`self_head_in_ret` gates on `te_self_count(declared)`:

- 0 → `ty_head_name(inferred)` (preserves the pre-#341 behaviour for ops
  whose declared return is a concrete type equal to the impl head, e.g.
  `protocol P { op() : Int }` for `impl P for Int` — the
  `boundary_tagging_597` fixture depends on this).
- 1 → align the declared shape against the inferred `Ty` and read the head
  at Self's position (handles both `Self` and `Result[String, Self]`).
- >1 → decline (no single dispatch head).

Considered reusing the typer's unifier to bind `Self`, but the inferred
return is already concrete (the typer unified it) — a ~25-line positional
walk on `ty_head_name` is simpler, has no failure modes the unifier would
surface, and stays inside protos.kai with no new infer surface.

### Receiver-only impl lookup for return-typed dispatch

`find_impl_with_args` could not serve the nested-Self case: its
`arg_names_match` treats the lone non-Self argument as the receiver
(`["String"]` would have to equal recv `"Int"`). For a return-typed op the
params are genuine non-Self arguments the typer already unified;
single-dispatch v1 keys each impl uniquely on `(protocol, op, Self-type)`,
so a new `find_impl_by_recv` matching those three is both sufficient and
sound.

## Structural surprises the brief did not anticipate

1. **The brief's mental model was wrong about #758.** "Anchoring is
   let-binding-only" implies a missing anchor *site*. The real cause was a
   missing *pass* (no inference on test bodies). The let-anchored form
   worked in a test block only by accident in the issue's framing — actually
   it *also* mis-resolved (verified in the emitted C). The fix is broader
   and cleaner than "add two anchor sites": one site (test bodies) gains the
   entire existing inference machinery for free.

2. **The `te_self_count == 0` fallback is load-bearing and dangerous.**
   First attempt restricted `self_head_in_ret` to exactly-one-Self and
   regressed `boundary_tagging_597` (an `op() : Int` op dispatched via the
   return head with zero Self). Re-adding the 0-count → `ty_head_name`
   fallback fixed it but then over-fired: `show(x: Self) : String` (Self in
   the *param*, concrete return) got hijacked onto `impl Show for String`,
   breaking the `m12_8_y_postfix_no_impl` negative fixture. The real gate is
   `proto_op_self_only_in_return(params, ret)` — only ops with NO Self in any
   parameter are return-dispatched; the rest belong to arg dispatch. Two
   fixtures caught two distinct over-reaches; without them the change would
   have shipped a silent misdispatch.

3. **The `args` param-shadow trap bit again, in stage2.** A `match t {
   TyCon(_, _, args) -> args }` arm silently emitted the prelude `args()`
   CLI-argv closure instead of the bound list, and a *downstream* `match`
   panicked `non-exhaustive match` on a value that was neither NIL nor CONS.
   ~an hour of lldb bisection (the `-O2` backtrace inlined the cold path and
   pointed at unrelated `dump_builtins_doc`; frames 4-9 were the truth). The
   tell is in the generated C: the arm reads `_kai_prelude_args_thunk` where
   you expected your binding. Renamed to `tas`. This is a known trap
   (memory `feedback_kaikai_param_args_shadow`) — re-confirmed live in
   stage2, not just stage1-compiled bootstrap code.

4. **Worktree/main edit leak.** The first round of protos.kai edits landed
   in the *main* checkout (absolute path to the non-worktree dir) while the
   build ran against the worktree's unchanged file — so the fix "didn't
   work" until I noticed the bundle had zero of my changes. Patch-extract +
   restore-main + re-Edit-with-worktree-path recovered it. The
   `git apply` of the stale-main patch onto the rebased worktree fuzzed and
   left duplicate functions; a clean `git checkout` + manual re-Edit was
   faster than untangling the fuzz.

## Fixtures added and coverage gaps

- `examples/protocols/bare_narrow_in_assert.kai` (+`.out.expected`) — the
  #758 repro verbatim, run with `--test` via the new `test-bare-narrow-assert`
  target (skipped by the program-glob `test-protocols` loop, mirroring
  `poly_scalar_dispatch`). C-only: the `--test` runner is the C path.
- `examples/protocols/from_string_in_return.kai` (+`.out.expected`) — #341
  Form 1, both nested-Self (`from_string`) and direct-Self (`default`) in fn
  return position, run as a program on C + LLVM by the existing glob.

**Why a behavioural fixture is mandatory here, not just selfhost byte-id:**
the compiler's own source has *zero* `test`/`bench`/`check` blocks (grep
confirms — the one `"test "` in `fmt_decl.kai` is a string literal). So the
Gap B change never exercises during selfhost, and byte-id stays green while
proving nothing about it. `bare_narrow_in_assert` is the real gate. (asu
flagged exactly this false-green risk.)

Coverage gap: no fixture exercises a `check` block with `with`-params
through the new path (only `test`). `infer_test_body` threads DCheck params
via `add_infer_params_with_binds`, but the property-check runner's random
input generation is a separate concern not regression-pinned here.

## Real cost vs estimate

Larger than "extend a hook to two sites." The two halves were independent
bugs in different files (protos.kai Self-extraction; infer.kai missing
pass). The `args`-shadow trap and the worktree-leak each cost real
debugging time that the brief could not have predicted. The two over-reach
regressions (boundary_tagging, postfix_no_impl) were caught only because the
existing protocol fixtures are thorough — a reminder that the negative
fixtures earn their keep.

## Follow-ups left for next lanes

- **#341 Form 2** (arity collision: free `min(xs)` vs `Ord.min(a, b)`) and
  **Form 3** (protocol-aware tparam bounds on free fns) remain open — both
  need a resolver/constraint-solver design decision, out of scope here. #341
  stays open; this lane references it.
- The `te_self_count > 1` case (a `(Self, Self)` return) declines silently.
  If a future protocol genuinely needs multi-Self return dispatch, it needs
  a design (which Self keys the impl?) — not a typer patch.
- Whether `default()` / `from_string()` can now retire their
  `let`-with-annotation usages in `stdlib/protocols.kai` and the
  `serialize_basic` example (the brief's stretch acceptance) is a stdlib
  cleanup lane, not this typer lane.
