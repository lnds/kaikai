# Lane experience — issue #956: naked cell read + `var x := init`, and the `@`/Ref reckoning

## Scope as planned vs as shipped

**Planned** (from the issue): drop the `@cap` read sigil for mutable cells / `State[T]` capabilities, move cell declaration from `var x = init` to `var x := init`, leave `@` exclusively as the as-pattern, codemod the tree, update docs, add fixtures.

**Shipped**: the cell surface above, plus a surface decision the issue's wording did **not** anticipate — `@` does **not** become as-pattern-only. The tree has *two* mutable constructs, not one:

- **cell** `var x := init` — `State`/`Reader`, block-local, not first-class. Reads **naked** (`x`), writes `x := v`.
- **`Ref[T]`** (issue #275, live, underpins HashMap) — first-class, crosses functions. Reads with the **deref `@r`** (OCaml `!r` / C `*p`), writes `r := v`.

The issue's "`@` only as-pattern" line was mis-calibrated: it assumed `@` only ever meant cell-read. It did not. `@` is the **deref operator of a first-class reference**, which a cell is not. The cell-read-via-`@` was the intruder #956 removed; `@` itself stays, now unambiguously the Ref deref. This is the Koka model exactly (`@` is Koka's `!`): `var` reads naked, `ref` reads with deref, both write `:=`.

## Design decisions (and alternatives rejected)

1. **`@` survives as the Ref deref.** Alternative — strip `@` entirely, force `Ref` back to `Mutable.ref_get/ref_set` — was rejected: reverts #275 (closed, with fixtures), regresses HashMap to 3–4× more verbose code, breaks Tier-1 stability for zero gain. A first-class reference *needs* a visible deref (ML theorem, not preference).

2. **Write is always `:=`, never `@r := v`.** The user's first instinct was the C model (`*c = v`, deref on both sides). Rejected after pinning down Koka/ML: `:=` is a *dedicated* assign-through-reference operator, so the deref is implicit in it; `@r := v` would say deref twice. Read is asymmetric (naked vs `@`) because first-class-ness changes what the *name* means; write is symmetric because "assign" is one operation regardless of storage class. This is Koka exactly.

3. **`@cell` is rejected, `cell.get()` is preserved.** The subtle one. `@n` and a literal `n.get()` both reach the AST as `n.get()` after the parser, so naively rejecting `@cell` also kills the explicit State op `n.get()` — which is the *real* base op of `State[T]` (the `var` lowers to an `EHandle` with `get`/`set` clauses), not sugar. Prohibiting it would make `State` the only effect whose ops can't be called by name. Fix: the parser lowers `@n` to a **sentinel method `n.__deref()`** (a reserved name the user cannot spell), so the cell-read lowering distinguishes cleanly — `@cell` → error, `@ref` → `ref.get()`, literal `n.get()` → untouched. No AST change.

4. **Escape veto via tail-reachability, not a coarse "inside a lambda" flag.** A closure that reads/writes a cell is illegal only when its *value* reaches the block's exit (tail / returned / stored) — once the block exits the `State` handler is torn down. A closure applied inside the block (`run_with(s, (x) => ...seen...)`, or `let f = () => counter; let r = f()`) is sound and allowed: applying consumes the closure, the value is the call result. A forward taint-pass over the block's statements + one check on the exit value, no dataflow. The first cut was a coarse `under_lam` flag that wrongly rejected `m7b_16_var_escape_falls_back` (which compiles today) — caught because that fixture exercises exactly the applied-inline case.

## Structural surprises the brief did not anticipate

- **Two-pass cell-read lowering.** Naked reads inside `#{...}` interpolation are invisible at the first pass (the interp body is still a raw source span). The lowering runs once before the `var` rewrite (structural reads), then again after `desugar_interp_decls` lifts the interp (the freshly-parsed `EVar` reads). The second pass must NOT re-reject `@cell` (reads are already lowered and indistinguishable) — gated by a `reject_at` phase flag. Idempotence trap: a generated `n.get()` re-entering the lowering would become `n.get().get()` unless the get/set arm recurses into args only, never the callee base.

- **`stage2/compiler` uses neither `var x =` nor `@cap`.** The self-hosted compiler is functional-pure (recursion + accumulators), so the codemod touches zero compiler source. The only `@` in stage2 is inside error-message string literals — which the codemod *would* have corrupted, so stage2 is excluded from the sweep entirely.

- **The codemod is textual, not AST-based.** A `kai fmt --upgrade` would have a fatal circularity: the new parser rejects the old syntax, so it can't parse the old files to migrate them. A one-shot Perl script over raw source (comment/string-aware) sidesteps it. It cannot distinguish a cell `@x` from a Ref `@c` (both are `@ident` textually), so the ~5 Ref-bearing fixtures were restored and hand-migrated.

## Fixtures added

Positive: `examples/sugars/naked_cell_read.kai` (naked read + `:=` + interp). Negatives with `.err.expected` goldens: `var_eq_rejected` (`var x =`), `at_cell_rejected` (`@cell`), `cell_escapes_closure` (escape veto). `var_no_init.err.expected` updated (`=` → `:=` message). `ref_sugar_mixed_with_var` is the canonical dual fixture: cell `x` reads naked, Ref `@c` derefs, in one function.

Coverage gap left: no fixture pins the `@ref` rewrite path *through the second (post-interp) pass* — i.e. a Ref deref inside `#{...}`. Low risk (the sentinel is phase-independent) but unverified.

## Cost vs estimate

Most of the wall-clock went not to the cell surface (mechanical) but to the `@`/Ref reckoning — three rounds with the `asu` agent to pin down that `@` is a deref operator, that the write stays `:=`, and that `n.get()` is a real op needing the sentinel. The issue framed this as a one-construct change; it was a two-construct surface-coherence decision that touched a closed issue (#275). The soundness work (escape veto granularity) was a fourth round. None of this was in the brief.

## Follow-ups for next lanes

- The `reject_at` phase flag is a thread-through parameter on ~20 `lcr_*` functions. It earns its keep but is a smell; a cleaner design would carry pass-phase in a small context record.
- `@ref` inside `#{...}` interpolation is unverified (see coverage gap).
- The escape veto is intentionally conservative on two v1-out-of-scope shapes (a combinator that *stores* a lambda argument; a closure stashed via side-effecting `SExprStmt`). Both are undetectable without signatures in a pre-typer pass. If they ever bite, degrade that sub-case and report, don't relax the whole pass.
