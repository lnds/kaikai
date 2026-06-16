# Lane experience — native-double-resume

**Lane:** `native-double-resume`
**Date:** 2026-06-15
**Scope:** Close the native-parity gap `examples/effects/m7a_6d_double_resume.kai`
— a handler clause that calls `resume` twice
(`tick(resume) -> { let _ = resume(()) resume(()) }`) must abort with
"continuation resumed twice (handler #N)" exit ≠ 0 (the one-shot guarantee,
Doc C §resume representation). C-direct aborted correctly (exit 1); the
in-process libLLVM native backend (KIR Lane 1.5) exited 0 SILENTLY — a
soundness gap. Take the fixture off the baseline (2 → 1).

## Scope as planned vs. as shipped

**Planned (brief):** reproduce the symptom; the brief HYPOTHESISED a KaiCont
**lifecycle** bug — that native hands each `resume` a FRESH `KaiCont`
(status=UNRESUMED) or re-inits it per clause-invocation, so the second resume
never sees `status=RESUMED`. Confirm via an IR dump, find where native loses
the cont identity/state between the two resumes, fix the native lowering (NOT
the runtime check), consult asu once if the fix touches the continuation ABI.

**Shipped:** the gap closed, but the brief's hypothesis was WRONG. The KaiCont
lifecycle is correct: both `resume()` calls share the same register `k` (the
clause param `1+d`, seeded once in `nemit_seed_clause_params`), and the perform
site inits the cont exactly once (`kaix_cont_init_identity`). The real root was
one layer up, in the KIR's *structural model of resume*: a `resume` was always
a block TERMINATOR, so the second call was dropped before it ever reached the
runtime check. The fix is in `lower_resume` + the KIR `KOp`/`KTerm` shape, not
the cont ABI — so the cont-ABI escalation the brief anticipated did not arise.

## Diagnosis path (where the brief pointed vs. where the bug was)

1. Reproduced cleanly: C exit 1 + "continuation resumed twice (handler #2)";
   native exit 0, no output.
2. Read the runtime (`stage0/runtime.h`): `kai_cont_init_identity` sets
   `status=UNRESUMED`; `kai_cont_resume` checks `status != UNRESUMED` → abort,
   then flips to RESUMED. The check is correct and present on the native path
   (`kaix_cont_resume` → `kai_cont_resume`, runtime_llvm.c). The runtime is not
   the bug.
3. Traced the clause ABI (`nemit_seed_clause_params`): `k` is one register, the
   perform site inits the cont once. Both resumes load the SAME `k`. The
   lifecycle hypothesis is refuted — if both resumes ran, the second WOULD see
   RESUMED.
4. The real question became: *do both resumes run?* Read `lower_resume`
   (kir_lower_walk.kai:1532): `seal_tail(KResume(..., tail=true))` — EVERY
   `resume` seals the current block with a `KResume` TERMINATOR and opens a
   "dead" successor that `seal_tail`'s own comment says "the translator drops".
5. So `let _ = resume(()) resume(())` lowers to: block entry sealed by the
   FIRST resume (`kaix_cont_resume(k,v); ret`); the SECOND resume lands in the
   dropped dead successor. Native emits only the first → `ret` → the second
   never runs → exit 0. The C-direct oracle (emit_c.kai:2034) lowers `resume(v)`
   as a plain EXPRESSION `kai_cont_resume(k, <v>)`, never a terminator, so the C
   function's normal statement sequence runs both and the second aborts.
6. Confirmed `check_resume_one_shot` (kir_lower_fns.kai:392) — the structural
   compile-time net — only fires on `resume_count>1 AND blocks_have_backedge`;
   the LINEAR two-resume case has no back-edge, so neither the net nor the
   runtime fired. The gap was real and unguarded.

## Design decision (asu consult, one round)

Asked asu: Camino A (a tail/non-tail discriminant — non-tail resume =
expression-call, tail resume = terminator-ret) or simply mirror the oracle?
asu recommended **Camino A': resume is ALWAYS an expression-call, NEVER a
terminator** — eliminate the tail classification entirely.

- The oracle treats every `resume` uniformly as `kai_cont_resume(k, v)`; native
  must mirror it (the same mode-slave-to-oracle rule already pinned for match-raw
  and Real unbox).
- A tail/non-tail discriminant is a bug-class: a resume that is tail of an
  `if`/`match` branch but not of the clause would be mis-sealed with `ret`,
  eating the rest of the clause and LOSING the one-shot detection. A' has no
  node to mis-classify.
- The clause's normal `KRet` (from `ls_finish`) supplies the function return,
  exactly as the C function's return sequences the resume call.

Risks asu flagged, in order, and how this lane closed each:

1. **resume result must be a usable register** (deep `let x = resume(v)`), not
   hardcoded unit → `bind_op` materialises the `kaix_cont_resume` result into a
   fresh register; new positive fixture `m7a_6d_resume_value_used.kai`
   (`let x = resume(40); x + 2 == 42`) exercises it, native==C, ASAN-clean.
2. **KResume2Op (stateful write-state)** re-runs write+call per call, exactly
   like the oracle → verified parity on m7b_11_state_basic / writer_basic /
   m7b_14_state_helper / m7b_2b_mutable_intercept / m7b_15_nested_alias /
   issue_842, ASAN+UBSan clean on the stateful pair.
3. **`check_resume_one_shot` counted terminators** → rewritten to count resume
   OPS in statements; the back-edge net stays for the loop shape.
4. **TRMC/tcrec back-edges** never carry a resume (clause bodies are post-longjmp
   thunks) → no path inspects `KResume` as a terminator any more (the arm is
   gone); the variant/cons TRMC terminators are untouched.

## Implementation (what changed, 6 files)

- `kir.kai`: `KResume`/`KResume2` removed from `KTerm`; added
  `KResumeOp(KVal, KVal)` / `KResume2Op(KVal, KVal, KVal)` to `KOp` (the
  expression form, yielding the resume result; no tail flag, no `KPos` — the
  enclosing `KLet` carries the position).
- `kir_lower_walk.kai`: `lower_resume` uses `bind_op(KResumeOp(...))` instead of
  `seal_tail(KResume(...))` — the block stays open, the result is returned as an
  `LRes(KVar, …)`.
- `emit_native_fx2.kai`: `nemit_resume`/`nemit_resume2` now RETURN the
  `kaix_cont_resume` result (`: Handle`) and drop the `llvm_build_ret` — the
  call is an expression. RC unchanged (`ncall_sig2` does not free its args, so
  the borrowed `k`/`v` keep the oracle's RC).
- `emit_native_term.kai`: removed the `KResume`/`KResume2` terminator arms;
  `nemit_op_fx` routes `KResumeOp`/`KResume2Op` to the resume emitters (the seam
  must live here — `emit_native_fx2` cannot be imported by `emit_native_ops`
  without a cycle, fx2 already imports ops).
- `emit_native_ops.kai`: defensive loud arms in `nemit_op` for the two new ops
  (they only legitimately appear via `nemit_op_fx`, like `KPerform`).
- `kir_dump.kai`: moved the resume dumps from term to op; dropped the now-unused
  `kir_tail_mark`.
- `kir_lower_fns.kai`: `resume_count` counts resume ops in statements
  (`stmts_resume_count` / `stmt_is_resume` / `op_is_resume`).

## Structural surprises the brief did not anticipate

- The brief framed this as a cont-ABI/lifecycle bug; it was a KIR control-flow
  modelling bug. The fix never touched the continuation representation, the
  perform site, or the runtime. The IR-dump step the brief mandated was not
  needed — reading `lower_resume` + `seal_tail` made the dead-successor drop
  obvious.
- The import-cycle constraint (fx2 imports ops, so ops can't import fx2) forced
  the resume-op seam into `nemit_op_fx` (emit_native_term, the module importing
  both), mirroring the variant↔match seam already in the walk.
- `check_resume_one_shot`'s heuristic silently assumed every resume sealed a
  block — A' invalidated that assumption, so the net had to be retargeted at
  statements or it would have gone permanently dark (always counting 0).

## Fixtures added & coverage

- `examples/effects/m7a_6d_double_resume.kai` (the gap itself, NEGATIVE) — now
  native==C: aborts "continuation resumed twice (handler #2)", exit 1. Already
  wired into the C-direct effects target (stage2/Makefile) and the parity
  ratchet.
- `examples/effects/m7a_6d_resume_value_used.kai` (NEW, POSITIVE) — the
  value-bearing single-resume path (`let x = resume(40); x + 2`), native==C,
  guards the resume-as-expression result materialisation (asu risk 1).
- Coverage gap: m7a #6d ships only the identity continuation, so the resume
  RESULT equals its argument; a deep-handler test where resume returns a
  TRANSFORMED value awaits the full CPS reification (a later milestone) — noted,
  not in scope here.

## Gates (all green local)

1. native double-resume == C-direct (abort "resumed twice", exit 1). ✓
2. single-resume happy path unbroken: m7a_6d_resume_check / 6c / 6e / 6f all
   exit 0 native==C; new m7a_6d_resume_value_used exit 0 native==C. ✓
3. native-parity ratchet: m7a_6d_double_resume off the baseline (2 → 1), zero
   new gaps. ✓
4. selfhost byte-id (FALSE-GREEN — compiler.kai has no non-tail-resume clause;
   the m7a/m7b parity fixtures are the real gate); tier0; ASAN+UBSan clean on
   the resume + stateful fixtures. ✓

## Cost vs. estimate

Smaller than the brief implied. The brief budgeted an IR-dump bisection of the
cont lifecycle and a possible cont-ABI escalation; the actual fix was a control-
flow remodel (resume: terminator → expression op) localised to 6 files, found by
reading the lowering rather than dumping IR. The one asu round confirmed A' over
the tail-discriminant and surfaced the 4 risks the fixtures then closed.

## Follow-ups left for next lanes

- The full CPS reification (resume reifies the rest of the caller's body as a
  separate fn, replacing `kai_cont_identity`) is still a later milestone — when
  it lands, a deep-handler resume-result-transform fixture should be added.
- `issue_668_map_large_in_fiber.kai` (TCO-in-fiber) is the sole remaining native
  ratchet gap — out of this lane's scope.
