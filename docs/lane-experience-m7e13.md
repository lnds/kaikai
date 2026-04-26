# Lane experience report — m7e13 (`!` postfix)

Best-effort retrospective by the implementing agent. See limitations
at the bottom.

## Build/test cycles I ran (from transcript count)

- `make all`: 1 (the initial post-edit build after every visitor was
  threaded; subsequent rebuilds happened implicitly through
  `make -C stage2 kaic2` and the test targets).
- `make test`: 3 (one full run after the m7e_13 fixtures landed, plus
  two pipe-into-`grep` re-invocations of the same target to scan for
  failures; counted as separate make invocations).
- `make selfhost`: 1.

Other targets I ran ad-hoc:
- `make -C stage2 test-sugars`: ~3 times (iterating on the chained
  fixture and the negative-fixture diagnostic regex).
- `make -C stage2 kaic2`: 1 explicit rebuild after touching the
  Perceus pass.
- Direct one-off compile-and-run of the C output for each m7e_13
  fixture (`./stage2/kaic2 …kai > /tmp/x.c && cc … && /tmp/x`):
  5 times during early validation.

Context-truncation note: my conversation history fits in a single
window for this lane, so I do not believe earlier attempts were
hidden from this count. The numbers may still be off by ±1 because
some Bash invocations chained multiple commands with `&&`.

## Compiler errors I encountered

1. **`m7e_13_bang_chained` printed `4` instead of `10`** — at
   *runtime*, surfaced by the test-sugars diff. Root cause was in
   the *Perceus pass*: `pcs_collect_uses_kind` had a `_ -> acc`
   catch-all that silently swallowed `EBang`, so `seed` was treated
   as an unused parameter and `pcs_prepend_unused_drops` emitted
   `kai_internal_drop(kai_seed)` at the top of `kai_pipeline` —
   which decref'd the value before `kai_lift_a(kai_seed)` ever
   ran, and the chained `!` then read stale memory. Fixed by adding
   `EBang(inner) -> pcs_collect_uses_expr(inner, …)` to
   `pcs_collect_uses_kind` (and a parallel rewrite case in
   `pcs_rewrite_kind` for symmetry). 1 attempt to diagnose, 1 to
   fix.
2. **Negative-fixture grep mismatch on `.err.expected`** — at
   *test harness*. The test-sugars Makefile recipe uses
   `grep -q "$$line" build/s2-$$name.err`, which is BRE: backticks
   are literal but `[T]` is a character class. So my expected line
   `` `!` requires an `Option[T]` or `Result[E, T]` operand `` was
   read as the regex `Option` followed by the char-class `[T]`
   (matches just the letter `T`), and the actual `Option[T]` text
   in stderr did not match. Fixed by replacing `[`, `]`, and the
   surrounding backticks with `.` (BRE any-char) in both
   `.err.expected` files. 1 attempt to diagnose once I read the
   recipe; the symptom was a clean rejection with the right
   diagnostic visible right next to the FAIL line.

No other distinct error classes are visible in my current context.

## Friction points

- **Locating "the typer".** The lane brief pointed me at `chk_expr`
  (line 4435) for type checking. That function is actually only the
  *resolver* (it walks names and validates scope); the real
  inferencer is `synth` (line 11274) with its `InferState` machinery
  starting at 10685. The Inferred-tree shape (`{ st, ty, expr }`)
  with `st_*` builder helpers wasn't obvious from the brief — I
  burned a few minutes reading `synth_unop`, `synth_binop`, and
  `synth_handle` to understand the threading discipline before
  writing `synth_bang`. Helped: each existing `synth_*` is small
  and self-contained, so the pattern was easy to mimic once
  I picked one.

- **`InferState` field churn.** Adding `ret_ty: Option[Ty]` to
  `InferState` required updating ~12 builder helpers
  (`st_set_env`, `st_set_sub`, `st_bump_err`, `st_set_row`,
  `st_push_alias`, `st_pop_alias`, `st_set_clause_resume`,
  `st_disable_alias_tag`, `st_push_inst`, `st_push_hole`,
  `st_add_label`, `st_restore_entries`) to thread the new field
  through. All mechanical, but tedious — and the type-checker
  cannot tell me when I forget one (kaikai's record literal
  requires every field, so the build catches it; in this case
  `make all` failed loudly the first time, which was the right
  outcome).

- **Visitor sprawl.** `ExprKind` is matched in ~15 places across
  the file (parser, resolver, `fv_expr`, `collect_expr`,
  `body_escapes`, `contains_var`, `specialise_body`, three
  desugar passes, two dump passes, `tag_of_kind`,
  `expr_kind_name`, two Perceus passes, and the two emitters).
  Many of them have a `_ -> ...` catch-all, which means a
  *missing case is silent* — exactly what bit me in the
  `pcs_collect_uses_kind` chained-fixture incident. The lane
  brief listed the obvious ones but not the Perceus pass; I
  found it by grep. Helped: `grep -n "ETodo\b"` after each
  pass to triple-check I'd added EBang to every site that
  named ETodo.

- **Choosing where the `return` goes.** The C lowering relies on
  the host C function's `return` statement, which is correct only
  when `e!` sits directly inside the enclosing kaikai fn. Inside a
  kaikai *lambda*, the lambda becomes its own top-level C function
  (`_kai_lam_N`), and a `return _b;` would short-circuit the
  closure body, not the user-intended fn. I chose to reject
  `e!` inside lambdas at the typer (clear `ret_ty = None`
  in `synth_lambda`), which gives a proper diagnostic instead
  of silently-wrong codegen. Spec is silent on this; matched
  the conservative end of Rust's `?` semantics for closures.

## Spec ambiguities or interpretive choices

`docs/proposed-extensions.md` §13 is fairly tight; the cases I
had to decide:

1. **`e!` inside a lambda.** Spec says "the enclosing function's
   return type must be `Option[U]` / `Result[E, U]`". A lambda
   *is* a function, so an aggressive reading would let `e!`
   propagate from the closure. I implemented the conservative
   reading: `!` is rejected inside any lambda body with the
   diagnostic "`!` is only valid inside a function body". My
   reasoning: the lambda lowers to its own top-level C function,
   so a closure-scoped `!` would short-circuit the closure's C
   return, not the surrounding fn — silently wrong. A more
   permissive future implementation would need to teach the
   emitter to wrap closures in a sum-typed return slot, which is
   out of scope for a §13 mini-lane.

2. **LLVM backend.** Spec doesn't address backends. The C path
   is fully wired; the LLVM emitter has a stub that aborts with
   an `eprint` and emits `undef`. Stage 2's own source does not
   use `!`, so `make selfhost-llvm` is unaffected; programs that
   want `!` go through `--emit=c`. Marked as a follow-up in the
   updated §13 status block.

3. **`bang_finish_result` unification on the error type.** The
   spec says "same `E` (no `From`-style coercion)". I implemented
   this via `st_unify(e_ty, ret_e_ty, …)` rather than a strict
   structural equality, so two `Result[String, _]`s — one with
   `String` literal and one with a tyvar `?t` that resolves
   to `String` — succeed. This is what HM unification gives for
   free; it is *not* a `From` coercion (no implicit conversion,
   only unification of identical types via the substitution).

4. **Diagnostic phrasing.** Spec: "name both types and point at
   both the `!` and the function signature." I name both types
   in the diag note + help text but only point at the `!` site
   (not the fn signature). Pointing at the fn signature would
   need the typer to remember the signature's source span, which
   the lane brief did not authorise me to extend. The note
   ("enclosing return type: …") still gives the user the type;
   they can find the signature themselves.

## Subjective summary

- **Confidence in correctness**: medium-high.
  - High for the four positive paths (Some/Ok unwrap; None/Err
    propagate). Tested end-to-end via test-sugars and verified by
    eyeball on the generated C.
  - High for the negative paths (wrong return type, wrong
    operand). The diagnostics are explicit and the typer rejects
    correctly.
  - Medium for the typer's interaction with polymorphism. I did
    not write a fixture that exercises `fn f[a]() : Option[a]
    { let x = g()! …}` where `g : Option[?t]` and `?t` only gets
    pinned later. The unification-based design *should* handle
    it (HM gives it for free), but I haven't proven it under load.
  - Medium for the lambda-rejection diagnostic. No fixture.
- **Estimated wall-clock for the lane**: ~4 hours, ±50%. Lots of
  reading (the InferState shape, visitor sites) before any
  productive editing.
- **Hardest sub-task**: the *typer* — specifically deciding that
  `ret_ty: Option[Ty]` belongs on `InferState` (vs. plumbed as a
  parameter through every `synth_*`), then doing the field-churn
  surgery without missing one.
- **Easiest sub-task**: the *fixtures*. Once the C path produced
  correct stdout, writing six positive/negative `.kai` files was
  ~15 minutes.
- **Did the compiler help or hinder you?** Mostly help.
  - **Help #1**: the kaikai record-literal exhaustiveness check
    means `make all` fails fast if I forget a field on
    `InferState`. That caught the first-pass omission without
    me needing to mentally enumerate the helpers.
  - **Help #2**: `make selfhost`'s fixed-point check. After every
    structural change, kaic2 has to compile itself byte-identically.
    No regression slipped past it.
  - **Hindrance #1**: the `_ ->` catch-alls in 15+ visitor
    sites. The compiler did not warn about the missing
    `pcs_collect_uses_kind` arm because the `_ -> acc` pattern
    swallowed it. The bug only surfaced as a runtime
    miscompilation in a single fixture (`m7e_13_bang_chained`).
    A linter that rejects `_ ->` for `match k {}` over an
    enum named `ExprKind` would have saved me one debug cycle.
  - **Hindrance #2**: the test-sugars grep regex behaviour
    (`[T]` as char class) is non-obvious; I had to read the
    Makefile recipe to understand why my literal-looking
    expected line failed. A `--fixed-strings` flavour of the
    fixture format would have avoided this.

## Limitations of this report

- **Self-report bias acknowledged.** Agent reporting on own
  performance is suspect.
- **Context truncation.** Counts and error lists exclude anything
  that fell out of my visible context window. For this lane I do
  not believe truncation occurred, but I cannot fully prove it.
- **No external instrumentation.** No precise timings;
  "wall-clock" is a guess based on transcript density and the
  complexity of each phase.
- **Single agent (Claude).** Not generalisable across LLMs.
