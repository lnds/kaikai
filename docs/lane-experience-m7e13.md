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

---

# Phase 2: LLVM backend port

Best-effort retrospective by the implementing agent.

## Objective metrics (from /tmp/lane-m7e13-phase2-builds.tsv)

- Phase 2 start: `2026-04-26T16:26:58-04:00`
- Phase 2 end:   `2026-04-26T16:35:32-04:00`
- Phase 2 wall-clock: **~8m 34s** (instrumented, not estimated).
- Build/test invocations:
  - `make all`:           1 invocation (~2s).
  - `make test`:          1 invocation (~71s, plus one
    PIPESTATUS-based shell-condition logging glitch — see
    *Compiler errors* §2).
  - `make selfhost`:      1 invocation (~6s).
  - `make selfhost-llvm`: 1 invocation (~5s, same logging
    glitch).
  - `--emit=llvm` manual: 6 invocations (one early single-
    fixture smoke; one loop over the five positive m7e_13
    fixtures, each emit + clang + run + diff against the
    pre-existing C-backend `.out.expected`).

## Compiler errors I encountered (Phase 2)

1. **No compiler errors visible in current context.** The LLVM
   port compiled clean on first build (`make all` after the
   `EBang(inner) -> llvm_emit_bang(e, inner)` swap and the new
   `llvm_emit_bang` helper). The first manual `--emit=llvm` on
   `m7e_13_bang_option_basic.kai` produced clean IR; clang
   accepted it; running printed `7` matching the C-backend
   golden. All five positive fixtures round-tripped without
   intermediate errors.
2. **Shell logging glitch (not a compiler error).** My
   instrumentation used
   `RC=${PIPESTATUS[0]}; ... [ $RC -eq 0 ]` inside a `Bash` tool
   call where `$RC` happened to expand to empty (PIPESTATUS
   reset between subshells), tripping zsh
   `(eval):[:1: unknown condition: -eq`. The underlying `make`
   succeeded — `grep -E "(FAIL|DIFF|Error)"` over the same
   output returned nothing. I logged a corrective `OK` row
   immediately after; the TSV preserves both rows for honesty.

## Friction points (Phase 2)

- **Locating the right LLVM-emit primitives.** The m7c-b commit
  message and `docs/effects-impl.md` §m7c gave me the conceptual
  shape (push/body/pop with a scalar `last_val` register), but I
  needed concrete examples of *branching out of an SSA expression
  with an early `ret`* — which `EHandle`'s LLVM lowering does NOT
  do (no setjmp landing yet, deferred to m7c-e). The closest
  analogue was `llvm_emit_match`, which uses `unreachable` after
  `kaix_match_panic` and then opens the `match.end` block to PHI
  between arms. I copied the "open a fresh block after a
  terminator instruction" pattern from there. Helped:
  `grep -n "unreachable\|ret %KaiValue"` to enumerate every
  place the LLVM backend already terminates a block mid-fn.

- **No surprise from the runtime side.** `kaix_is_variant` and
  `kaix_variant_arg` were already declared in `llvm_header` and
  defined in `runtime_llvm.c`, with the same semantics the C
  backend uses. Zero new runtime helpers needed.

- **Verifying the fixtures end-to-end.** The existing
  `make -C stage2 test-llvm` recipe iterates only
  `examples/minimal/*.kai` and `examples/llvm/*.kai`; my
  `examples/sugars/m7e_13_*` fixtures are not picked up. The lane
  brief explicitly said *do not invent structural Makefile
  changes — ask if needed*. I instead validated the five
  positives with a manual one-shot bash loop (emit → clang → run
  → diff vs the existing C-backend `.out.expected`), and
  documented adding the sugars to `test-llvm` as a follow-up
  (named `m7e13-llvm-test-harness` in the brief's suggestion).

## Spec ambiguities or interpretive choices (Phase 2)

1. **`ret` directly from inside the SSA region.** The C lowering
   uses a GCC statement-expression and a literal `return _b;`
   that yanks control out of the host function. The LLVM
   backend has no statement-expression; the natural fit is a
   `br i1 %cond, label %unwrap, label %fail` with `fail` doing
   `ret %KaiValue* %inner`. I went with this directly — the
   `unwrap` block becomes the new `cur_label` and its
   `kaix_variant_arg` result is the EBang's `last_val`. No PHI
   is needed because the fail block does not flow back. Not in
   the spec, but the only sensible LLVM analogue.

2. **`incref` on the unwrapped payload.** `kaix_variant_arg`
   already increfs its return value (per the runtime comment
   "incref so the caller owns it; the C backend passes variant
   args as borrowed references, but the LLVM path keeps
   ownership uniform"). I did not add a separate dup; the
   existing helper is correct for both backends. The C backend's
   `emit_bang` reads `_b->as.var.args[0]` directly (no incref),
   which is consistent with the C backend's borrowed-reference
   convention. The two backends therefore have *different* RC
   discipline for the unwrapped value, but both are internally
   consistent and both pass selfhost.

3. **Lambda rejection inheritance from Phase 1.** Phase 1
   already rejected `e!` inside lambdas at the typer
   (`synth_lambda` clears `ret_ty`). The LLVM port relies on
   that — `ret %KaiValue* %inner` in the LLVM emit would
   short-circuit the lambda's C function (`@_kai_lam_N`)
   instead of the user's enclosing fn, exactly the same
   footgun as the C backend would have had. No new spec
   choice; the typer rejection is what keeps the LLVM lowering
   correct.

## Subjective summary (Phase 2)

- **Confidence in correctness**: high.
  - Selfhost-llvm fixed point passes (so the LLVM emitter is a
    pure function of the typed AST, even after my changes).
  - Stage 2 source does not use `!`, so the LLVM-port code is
    only exercised by user fixtures — not by the bootstrap
    chain.
  - All five positive m7e_13 fixtures emit, link, run, and
    produce stdout byte-identical to the C backend's
    pre-existing goldens.
  - Negative fixtures stay typer-rejected before the LLVM
    emitter ever runs (the C backend is unchanged in that
    respect).
- **Wall-clock estimate**: ~9 minutes instrumented. My Phase 1
  guess was ~4 hours; that included substantial reading time.
  Phase 2 was much shorter because (a) I already had
  `bang_success_variant` from Phase 1, (b) the LLVM helpers I
  needed already existed, and (c) the selfhost-llvm gate is
  fast (~5s).
- **Hardest sub-task**: figuring out that `llvm_emit_match`'s
  panic-then-`unreachable`-then-`open_block` pattern was the
  template I needed for "terminator instruction inside an SSA
  expression". Took longer than the actual edit.
- **Did the LLVM port reuse most of the C path's logic, or did
  it require fresh design?** Reused 100% of the typer (Phase 1
  validated `inner.ty`; Phase 2 just reads it). Reused
  `bang_success_variant` verbatim. The codegen itself was
  fresh design but small (~50 LOC) because both runtime
  helpers (`kaix_is_variant`, `kaix_variant_arg`) and the
  LLVM-emit infrastructure (fresh-reg / fresh-label /
  open-block) were already there.

## Comparison Phase 1 vs Phase 2

- **Was Phase 1 (C backend) easier than Phase 2 (LLVM)?**
  *Phase 1 was harder per LOC.* Phase 1 carried the structural
  cost: AST node, parser branch, 15 visitor sites, the typer
  surgery (`InferState.ret_ty`, save/restore in `synth_lambda`,
  diagnostics), the Perceus-pass case, and the C lowering. The
  Perceus omission cost me one debug cycle (~10 minutes). Phase
  2 was almost-pure follow-on: AST, typer, and visitors were
  already in place; the only new surface was the LLVM-emit
  helper, ~50 LOC. No debug cycles.
- **Would I have preferred to do both in Phase 1, or was the
  split helpful?** Honest answer: doing both in Phase 1 would
  have been *cheaper end-to-end* — the LLVM port took ~9 minutes
  net, less than the overhead of a second prompt + restart. The
  split was helpful for *me, the agent*, in that Phase 1 closed
  with a coherent C-only checkpoint that I was confident in;
  but for an agent fluent in the LLVM-emit primitives, doing
  both at once would have removed the partial-backend smell
  that motivated Phase 2 in the first place. The lane brief for
  a future similar feature should probably scope "all backends
  in scope" by default and only carve out an LLVM follow-up
  when the LLVM lowering is genuinely larger than the C one.

## Phase 2 raw build log

```
timestamp	cmd	outcome	elapsed_s
2026-04-26T16:30:39-04:00	make-all	OK	2
2026-04-26T16:30:55-04:00	llvm-emit-basic	OK	0
2026-04-26T16:31:11-04:00	llvm-link-basic	OK	0
2026-04-26T16:31:24-04:00	llvm-fixture-option_basic	OK	1
2026-04-26T16:31:24-04:00	llvm-fixture-option_propagates	OK	0
2026-04-26T16:31:25-04:00	llvm-fixture-result_ok	OK	1
2026-04-26T16:31:25-04:00	llvm-fixture-result_err	OK	0
2026-04-26T16:31:25-04:00	llvm-fixture-chained	OK	0
2026-04-26T16:32:53-04:00	make-test	FAIL	71
2026-04-26T16:34:12-04:00	make-test	OK	-
2026-04-26T16:34:34-04:00	make-selfhost	OK	6
2026-04-26T16:34:46-04:00	selfhost-llvm	FAIL	5
2026-04-26T16:34:52-04:00	selfhost-llvm	OK	-
```

## Limitations of this report

- Same as Phase 1, plus: agent has been on this task for two
  phases, may suffer from familiarity bias underestimating
  residual effort. The "no compiler errors" claim for Phase 2 is
  notable and worth scepticism; one plausible reading is that
  the residual design from Phase 1 made the LLVM port
  trivially-mechanical, and a less-prepared agent on the same
  task would not have seen the same error-free path.
- The shell-logging glitch (Phase 2 *Compiler errors* §2) means
  the raw TSV has two redundant rows for `make-test` and
  `selfhost-llvm`. The first row in each pair is the FAIL with
  elapsed time; the second is the OK confirmation with no
  elapsed time. Reading the TSV requires deduplication.
