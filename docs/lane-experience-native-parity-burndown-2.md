# Lane experience — native-parity burn-down 2 (runtime-type-mismatch + output + exit-code)

## Scope as planned vs as shipped

**Planned (brief):** the second native-parity burn-down lane. Close the
three value-divergent families — `runtime-type-mismatch` (27),
`output-mismatch` (20), `exit-code-mismatch` (11) — verifying each closed
fixture against the C-direct oracle (stdout + exit identical), and lower
`tools/native-parity-baseline.txt` in the same PR. The brief's hypothesis:
the three families share a box/unbox discipline root cause (a value
reaches an op boxed where raw is expected, or vice-versa).

**Shipped:** the box/unbox hypothesis was HALF right — but the actual root
causes were sharper and more transverse than "box discipline". Two
KIR-lowering bugs, neither one a box/unbox slot mismatch, closed **55**
gaps (137 → 82) — far beyond the three target families' ~58 fixtures,
because both bugs are triggered by a *surface pattern* that appears all
over the corpus, not by a family-specific shape. The full-corpus ratchet
run measured pass=339 (was ~249), fail=82, 0 regressions.

## Root causes found and fixed

The diagnosis method (burn-down 1's legacy): capture the EXACT divergence
signature per fixture, group by it, then minimise to a repro. Instrumenting
the runtime's `kai_op_*` to print the offending operand's tag (a 3-line
temporary edit to `stage2/runtime.h`, reverted before commit) was the
single highest-leverage step — it turned "type mismatch in +" into "the
left operand is a CLOSURE (tag=10), not an Int", which pointed straight at
the lowering.

**1. unary minus aliased the binary `-` (the larger slice).** The KIR
lowering's `EUnop(op, a)` arm lowered `-` as `KPrim("-", [a])` — the RAW op
string. The native backend's `nprim_op_sym("-")` maps to `kaix_sub`, the
TWO-arg subtract helper, so the lowering emitted `kaix_sub(ptr %0)` — a
one-arg call to a two-arg function. The missing second operand read an
uninitialised register (a garbage pointer), so EVERY negative literal
(`-5`), `0 - x`, `abs(-5)`, etc. diverged with a value that changed run to
run. The C-direct oracle never had this: emit_c's `unop_cname` maps unary
`-` → `kai_op_neg` (a distinct symbol from the binary `-`). Fix: a `unop_prim`
helper in `kir_lower.kai` maps `-` → `neg` (so it does NOT collide with the
binary `-` string), and `nprim_op_sym` in `emit_native_ops.kai` maps `neg`
→ `kaix_neg` (the one-arg runtime helper, already forwarded in
`runtime_llvm.c`). `not` was already correct (its `kaix_not` is genuinely
one-arg).

**2. locals-shadow-imports (the transverse slice).** `lower_var` resolved
an `EVar` by checking, in order: nullary-ctor → top-level `const` →
`ls_fn_value` (EFn table / prelude builtin) → local binder. The fn-value
check came BEFORE the local-binder fall-through, so a param / `let` / match
binder named like a stdlib `pub fn` — `init` (vs `list.init`), `sum`,
`head`, `last`, `acc`, … — resolved to a `KClosure` over the stdlib thunk
instead of a local register read. The closure (tag=10) then reached an
arithmetic op (`+`/`%`/`>`) as a non-Int operand → runtime type-mismatch.
This is the EXACT #748/#749 locals-shadow-imports rule, which the C path
already honours via emit_c's `EmitCtx.lcs` (lexical-scope names): the KIR
lowering had no equivalent. Fix: thread a `locals: [String]` field on
`LowerSt`, seed it with params/captures at fn entry (`ls_enter_fn`),
extend it at the single binder chokepoint (`ls_emit` over `KLet`/`KStore`),
and check it FIRST in `lower_var` AND `lower_callee_dispatch` (a callee that
shadows a top-level fn is an indirect `kaix_apply`, not a direct call).

## Structural surprises the brief did not anticipate

1. **The bug was NOT box/unbox slot discipline.** The brief framed the
   three families as a shared box/unbox cause (the kind-1-raw / match-raw /
   EBlock-raw lineage). The actual causes were a wrong-arity prim symbol
   and a name-resolution-order bug — both upstream of any slot decision.
   The "value reaches an op boxed wrong" SYMPTOM was real (a closure where
   an Int was wanted); the CAUSE was the lowering picking the wrong symbol
   / the wrong binding, not a slot-repr desync. Chasing the slot layer
   would have found nothing.

2. **One name-resolution fix is transverse across every family.** Because
   the trigger is "a local name collides with a stdlib fn" — and `init` /
   `sum` / `head` / `acc` are the most natural names for accumulators,
   counters, and list heads — the locals-shadow bug hit fixtures in
   runtime-type-mismatch (26/27), output-mismatch, exit-code-mismatch,
   SIGSEGV, missing-symbols, AND no-effect-handler. Fixing it closed 55
   gaps from a ~58-fixture brief. The family boundaries the brief drew are
   real for *symptoms* but not for *causes*: one cause spans them all.

3. **`LowerSt` had no lexical-scope set.** The KIR lowering tracked the EFn
   table, the variant table, the lambda table, the const table — but never
   the set of names BOUND in the current fn body. emit_c carries exactly
   this (`EmitCtx.lcs`) and threads it through every scope descent. The KIR
   lowering's omission is why it could not honour locals-shadow-imports.
   Adding `locals` to the record meant touching all 7 `LowerSt {...}`
   constructors (kaikai has no record-update spread) — mechanical churn,
   but the single `ls_emit`-chokepoint extension (every binder funnels
   through `KLet`/`KStore`) kept it from leaking into every binding site.

4. **pipe-lowering is a real gap the brief's families HID.** While fixing
   the above I found `EPipe` lowers to a no-op `KUnitV` in the native walk
   — the typer (`synth_pipe`) rebuilds `EPipe(lhs, ECall(f, args))` rather
   than desugaring to `ECall`, leaving each emitter its own pipe
   desugaring, and the KIR walk never grew one. So `ys |> each(print)` /
   `xs |> filter(p)` silently emit no call (the output-mismatch family's
   euler1 / fizzbuzz / capture / imc). A first cut (rewrite to
   `ECall(f, [lhs, ...args])` + delegate to `lower_call`) closed the
   single-candidate + `each(print)` cases but TRAPPED the multi-candidate
   combinator path (`filter`/`map`): the typer resolves those to an
   `EModCall` callee whose `ECall` arg list reaches `lower_exprs` in a
   shape it rejects (`non-exhaustive match` at build — a corrupt `args`
   sourced upstream of the KIR). Shipping the partial cut would trade an
   output-mismatch for a louder build panic with no parity gain, so it was
   REVERTED and documented as a burn-down-3 item with the exact failing
   shape. Discipline call: a partial pipe fix is a false-green hazard.

## Fixtures added / coverage gaps

- `examples/perceus/native_unop_locals_shadow.kai` — one positive fixture
  exercising BOTH root causes (unary `-` / `0 - x`; params named `init` /
  `sum` / `last` used in arithmetic + comparison; a foldl-shaped recursion
  whose accumulator collides with `list.init`). It is its own parity check
  (the C-direct oracle is always correct), gated by the native-parity
  ratchet — a regression in either cause re-diverges native vs C here. No
  `.out.expected` golden: no harness consumes a stdout golden for
  `examples/perceus/`, and the parity harness diffs native-vs-C directly,
  so a golden would be orphaned.
- The 55 closed fixtures ARE the bulk of the regression coverage, locked by
  `tools/native-parity-baseline.txt` 137 → 82 (the set-equality ratchet
  rejects any re-diverging).
- `docs/native-parity-gaps.md` regrouped to the 82 remaining, with the two
  closed causes documented and the burn-down-3 items (pipe-lowering, Real
  box/unbox, `^` on Real, nested-variant-test) diagnosed.

## Quality + gates

- Edited files are all PRE-EXISTING (the KIR lowering); the differential
  A− bar applies to new files. The added helpers (`unop_prim`,
  `ls_bind_local`, `ls_is_local`, `param_local_names`, `ls_bind_local_of`,
  `ls_emit_raw`) are flat tail-recursions / single-branch — cogcom ~1 each,
  no new monolith.
- selfhost byte-id: **OK** (kaic2b.c == kaic2c.c) — the lowering change is
  deterministic and self-consistent.
- tier0: **OK**. test-fast (full C path, sequential): **0 fail**. The same
  light targets run sequentially keep-going: **0 fail**. test-effects /
  test-kir / test-runtime-shadow / rawsafe / borrowsafe / m4c isolated:
  **all OK**.
- RC discipline (CAVEAT — issue #812): `KAI_TRACE_RC`'s incref/decref
  balance is BROKEN on main (it reports `incref_total=0 decref_total=0` and
  `free_total=0` regardless of actual RC traffic), so the naive "balanced"
  gate passes vacuously (0==0). The usable signals from the same tracer are
  the per-tag ALLOC counters; the leak oracle is RSS via `/usr/bin/time -l`.
  Validated against those: across the new fixture and the RC-heavy closed
  fixtures (math_int_basic, list_basic, binserialize_recursive), native's
  `alloc_total` is ≤ the C-direct oracle's (119≤125, 50≤55, 647≤664 — the
  locals-shadow fix REMOVES the spurious stdlib-thunk closures the bug used
  to materialise, so it allocates *fewer*), and native RSS is bounded
  (~1.7–1.9 MB, ~10% over C's base — a constant runtime-link overhead that
  does NOT scale with alloc count, the signature of no leak). A leak would
  show RSS scaling with work; it does not.
- native-parity ratchet: **pass=343 fail=89, 0 new gaps**, baseline lowered
  137 → 82 by this lane (then 89 after rebasing onto #801/#810 corpus
  growth, preserving their entries).

## Known issue surfaced (not this lane's fix)

`make -C stage2 test-light-parallel` (the `-j` path `make test` /
tier1 uses) FAILS deterministically on macOS with a SIGBUS in a shared
`build/s2-<name>` binary — a build-artifact race (two concurrent targets
writing the same `build/$name` output). It is NOT caused by this lane: the
identical targets pass sequentially (`test-fast`, light keep-going) and
each target passes in isolation, and the change touches no Makefile target.
This is the `make -j` artifact-namespacing class (a target must prefix its
`build/` files). CI (Linux) scheduling may not reproduce it — main merged
green with the same Makefile. Left for a separate Makefile lane; opening an
issue needs authorisation.

## Real cost vs estimate

The diagnosis dominated, as in burn-down 1, but the leverage was higher:
runtime-operand instrumentation collapsed 25 "type mismatch" fixtures to
TWO causes in one pass. Most wall-clock went to kaic2 rebuilds (libLLVM
link, ~60-90s each) — the lowering change forced ~6 rebuilds across the
diagnose→fix→verify→revert-pipe loop. The pipe-lowering detour cost the
most: ~4 rebuilds chasing a multi-candidate `synth_pipe` interaction before
deciding (correctly) to revert rather than ship a partial cut.

## Follow-ups for burn-down 3

- **pipe-lowering** (euler1, fizzbuzz×2, capture, imc): `EPipe` → a real
  call. The single-candidate + `each(print)` rewrite is known-good; the
  multi-candidate (`filter`/`map`/`reduce`) `EModCall` arg shape needs its
  own upstream diagnosis (the corrupt `args` source is in the typer/mono
  path, not the KIR lowering).
- **Real box/unbox** (unbox_bench_real, complex_*): native prints
  `9.88131e-324` (a raw i64 read as a double) — the `SReal` slot box
  discipline (the box/unbox cause the BRIEF predicted, on the Real slot).
- **`^` on Real** (free_fall, math_real_basic): `Real ^ Int` reaches
  `kaix_pow_int` boxed wrong.
- **nested-variant-test unbound-register** (json `r`/`n`, binserialize
  nested, writer-state) — carried from burn-down 1; needs a nested decision
  tree, not a bind.
- no-effect-handler (Spawn/Clock/NetTcp), clause-param-origin,
  missing-symbols (bits/crypto), build-failed-other (regex/json/map/http) —
  each its own subset.
