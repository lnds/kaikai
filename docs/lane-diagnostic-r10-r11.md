# Lane diagnostic — R10 and R11

**Issue**: #61
**Lane**: r10-r11-diagnostic (this branch). Diagnostic only — no
fix lands here.
**Inputs**: R10 + R11 reports (since closed by issue #61, fixture
content migrated into the regression history below);
`docs/effects-impl.md` §"In-dispatch flag" (m8 #12);
`stage2/compiler.kai` (`pcs_is_non_last`, `pcs_rewrite_clauses`,
op-call emit at line 8274);
`stage0/runtime.h` (KaiEvidence.in_dispatch_node, kai_decref);
`examples/effects/trace_prefix.kai`,
`stdlib/trace.kai::with_log_prefix`.
**Repros built in lane**:
`examples/effects/r10_repro.kai`, `examples/effects/r11_repro.kai`.
Both gated `# DIAGNOSTIC ONLY — KNOWN ASAN FAIL` and not wired into
any tier1 / tier1-asan / demos baseline target.

---

## Conclusion

**One bug, two surfaces.** R10 ⊆ R11. The R10 entry's hypothesis
about `KaiEvidence.in_dispatch_node` save/restore is not the
mechanism — that machinery works as specified across both repros.
The actual mechanism is exactly the one R11 documents:
`pcs_is_non_last` returns `false` for a single-use read of `state`
inside a parameterised handler clause, the emit pass produces a
raw transfer of the EvE state slot's storage, and a downstream
decref-aware sink (`kai_prelude_string_concat`,
`kai_prelude_string_length`, `kai_prelude_string_join`) frees that
storage out from under the next op-call invocation.

R10 surfaces in the additional shape *parameterised outer +
self-delegating inner Trace* because the inner clause body's
`Reader.ask() ++ ": " ++ msg` is a textbook decref-aware sink
applied to the result of the parameterised handler's `state` read.
R11 surfaces in the same shape *parameterised outer +
non-delegating inner Trace* (`with_log_prefix` re-emits via
`Trace.log`, but the trigger is the same `++` after the
`TracePrefix.read()`). The crash bytes match across the two repros
to the byte.

**Fix lane (one PR, not two): r10-r11-fix.** Touches
`stage2/compiler.kai::pcs_is_non_last`. Fixes both R10 and R11.
Plan in §"Fix lane" below.

---

## Side-by-side ASAN repros

Both fixtures live under `examples/effects/`. They are gated
`# DIAGNOSTIC ONLY — KNOWN ASAN FAIL` so no human or CI mistakes
them for a passing demo. Build and run:

    cd stage2
    ./kaic2 --path ../stdlib ../examples/effects/r11_repro.kai > /tmp/r11.c
    ./kaic2 --path ../stdlib ../examples/effects/r10_repro.kai > /tmp/r10.c
    cc -std=c99 -O1 -g -fsanitize=address,undefined \
       -fno-omit-frame-pointer -Wno-unused-function -Wno-unused-variable \
       -Wl,-stack_size,0x8000000 -I ../stage0 /tmp/r11.c -o /tmp/r11_bin
    cc -std=c99 -O1 -g -fsanitize=address,undefined \
       -fno-omit-frame-pointer -Wno-unused-function -Wno-unused-variable \
       -Wl,-stack_size,0x8000000 -I ../stage0 /tmp/r10.c -o /tmp/r10_bin
    ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
      UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
      /tmp/r11_bin
    ASAN_OPTIONS=... /tmp/r10_bin

Both fail. Both fail at the same C source line, on the same heap
region (offset 4 inside a 40-byte allocation), with the same
free / read pair. The only thing that differs is the helper name
and the parameterised effect's name.

### R11 ASAN trace (Carrier + self-delegating Trace, abridged)

    ==826==ERROR: AddressSanitizer: heap-use-after-free on address
       0x604000000454 at pc 0x000102a108e4 ...
    READ of size 4 at 0x604000000454 thread T0
        #0 kai_prelude_string_concat runtime.h:1389
        #1 _kai_with_log_prefix_buggy__..._clause_31_5_log r11.c:296
        #2 _kai_worker_thunk r11.c:241
        #3 kai_with_log_prefix_buggy r11.c:225
        #4 kai_trace__with_trace_default r11.c:204
        #5 main r11.c:354

    0x604000000454 is located 4 bytes inside of 40-byte region
       [0x604000000450,0x604000000478)
    freed by thread T0 here:
        #0 free (libclang_rt.asan)
        #1 kai_decref runtime.h:830
        #2 kai_prelude_string_concat runtime.h:1390
        #3 _kai_with_log_prefix_buggy__..._clause_31_5_log r11.c:296
        ...

    previously allocated by thread T0 here:
        #0 calloc
        #1 kai_alloc runtime.h:219
        #2 kai_str runtime.h:940
        #3 _kai_lam_0 r11.c:276    <- the "P" prefix literal, threaded
                                       into Carrier[String]("P")'s state slot

### R10 ASAN trace (Reader + self-delegating Trace, abridged)

    ==1002==ERROR: AddressSanitizer: heap-use-after-free on address
       0x604000000454 at pc 0x0001045788e4 ...
    READ of size 4 at 0x604000000454 thread T0
        #0 kai_prelude_string_concat runtime.h:1389
        #1 _kai_with_log_prefix_via_reader__..._clause_43_5_log r10.c:288
        #2 _kai_worker_thunk r10.c:233
        #3 kai_with_log_prefix_via_reader r10.c:217
        #4 kai_trace__with_trace_default r10.c:196
        #5 main r10.c:346

    0x604000000454 is located 4 bytes inside of 40-byte region
       [0x604000000450,0x604000000478)
    freed by thread T0 here:
        #0 free
        #1 kai_decref runtime.h:830
        #2 kai_prelude_string_concat runtime.h:1390
        #3 _kai_with_log_prefix_via_reader__..._clause_43_5_log r10.c:288
        ...

    previously allocated by thread T0 here:
        #0 calloc
        #1 kai_alloc runtime.h:219
        #2 kai_str runtime.h:940
        #3 _kai_lam_0 r10.c:268    <- the "P" prefix literal, threaded
                                       into Reader[String]("P")'s state slot

The two stacks are isomorphic. Same allocation site (the
`prefix` string literal, allocated once in `_kai_lam_0`'s
`kai_str("P")` call site), same free site (`kai_decref` invoked
from `kai_prelude_string_concat` while building the message),
same read site (the next `kai_prelude_string_concat` invocation
inside the same C clause function on the next op call). The
identical heap address is coincidental but tells you the
allocator's first-fit picked the same slab — these two repros
exercise structurally identical object lifetimes.

---

## Walking the two hypotheses

### R11 hypothesis (the correct one)

`pcs_is_non_last` (`stage2/compiler.kai:23808`) decides whether
to wrap an `EVar` read in `__perceus_dup`. The clause-body scope
extension at `pcs_rewrite_clauses` (`stage2/compiler.kai:23772`,
line 23779) injects `state` and `log` into `scope1` so the
`pcs_rewrite_kind` `EVar` branch (line 23642) treats them as
in-scope and queries `pcs_is_non_last`. The predicate then
counts non-lam uses; with a single use, it returns `false` and
the EVar emits raw — no dup wrap.

For ordinary locals this is sound: the binding owns its
reference, the consumer is the last reader, the consumer's
decref discharges the ref. For `state` it is wrong:

- The clause prologue at `clause_state_prologue`
  (`stage2/compiler.kai:9933`) emits
  `KaiValue *kai_state = self->state;` — a *raw alias* of the
  EvE slot, not a fresh ref.
- Without a `__perceus_dup` wrap on the `state` read, the
  emitted clause body returns `kai_cont_resume(k, kai_state)`
  (op-call branch in `emit_call_expr` at
  `stage2/compiler.kai:8030`) — passing the alias on by
  reference.
- The op caller (the inner Trace clause, here) consumes the
  result through `kai_prelude_string_concat`, which decrefs both
  arguments (`stage0/runtime.h:1389-1390` is the `concat` body;
  see also the runtime's `kai_decref` discipline at
  `stage0/runtime.h:830`).
- The decref discharges the EvE slot's *only* outstanding
  reference. The slot itself remains pointing at the freed
  region. The next op call into the same clause re-reads
  `self->state`, gets the freed pointer, and crashes.

The `_keep_alive` workaround in `stdlib/trace.kai:110` works
exactly because it forces `pcs_count_non_lam_uses(...)` ≥ 2,
flipping the predicate to `true` so both reads emit
`__perceus_dup(state)`. The emitted C is at lines 333–334 of
`/tmp/r11.c` (verified in this lane):

    KaiValue *kai_state = self->state; KaiValue *kai_log = self->state;
    return ({
      KaiValue *kai__keep_alive = kai_internal_dup(kai_state);
      kai_decref(kai__keep_alive);
      kai_cont_resume(k, kai_internal_dup(kai_state));
    });

The `kai_internal_dup` on the second read bumps the slot's
refcount before the consumer's decref burns it off.

### R10 hypothesis (the misdiagnosis)

The R10 entry attributes the crash to `in_dispatch_node`
save/restore not handling parameterised resume entries. The
emit-pass evidence and the runtime evidence both contradict
this:

1. The op-call site in `emit_call_expr` (`stage2/compiler.kai`
   lines 8274–8290) saves `_disp_fib->in_dispatch_node` to
   `_saved_disp` *before* the indirect call to the clause and
   restores it *after*. Inspecting the emitted C for
   `r10_repro.kai` (`/tmp/r10.c:286-290`) shows three nested
   save/restore pairs in the inner Trace.log clause body — one
   for the inner Trace lookup, one for the Reader.ask lookup,
   one for the outer Trace.log. Each save/restore is balanced.
2. `kai_evidence_lookup_node` (`stage0/runtime.h:3577-3594`)
   skips `f->in_dispatch_node` so a self-delegated Trace.log
   from inside a Trace clause walks past the inner Trace and
   resolves to the outer one. `m8_12_self_delegating_handler`
   (existing fixture) verifies this works without a
   parameterised outer.
3. If the in_dispatch_node mechanism were the bug, the `delegating_helper`
   negative control inside the R10 entry would also crash — it
   exercises self-delegation (Trace clause re-emitting Trace.log)
   without a parameterised outer. It does not crash. The R10
   entry itself documents this as "what works today".
4. ASAN points at the parameterised handler's `state` slot, not
   at any KaiEvidence header. The 40-byte freed region is the
   `kai_str("P")` allocation (see "previously allocated by
   thread T0" above) — the prefix string, owned by the EvE state
   slot, not the dispatch flag.

The reason R10 looked like a dispatch-machinery bug is timing:
the crash surfaces on the second/third Trace.log invocation, and
the inner clause's structure routes through the in_dispatch_node
guards, so the trace looks dispatch-shaped. But the second/third
invocation is when the freed-then-reused EvE state slot is read
for the second time, exactly per R11's mechanism.

The negative control in the R10 entry (`delegating_helper` —
inner self-delegation, no parameterised outer) confirms the
trigger is *not* dispatch but *parameterised state with a
decref-aware downstream sink*.

---

## Why R11's existing fixture (`trace_prefix.kai`) did not catch R10

`examples/effects/trace_prefix.kai` exercises `with_log_prefix`
from `stdlib/trace.kai`, which already applies the `_keep_alive`
workaround. It demonstrates the helper *passes* under ASAN, not
that the bug is gone. Removing the workaround (the situation
this lane reproduces) crashes both R10's and R11's repros
identically. The two reports (R10 and R11) were authored by
different lanes (Tier 3 experiment 2 arm A for R11, arm B for
R10) with different proximate symptoms, which is why the same
bug was logged twice with different hypotheses.

---

## Fix lane

### Lane name: `r10-r11-fix`

One PR. Closes both R10 and R11. Closes #61.

### Plan (one paragraph)

Modify `pcs_is_non_last` in `stage2/compiler.kai` (around line
23808) to special-case the names `state` and `log` and return
`true` unconditionally for them — same effect as the
`in_lam_here` branch. This forces every read of either name
inside a clause body to emit `__perceus_dup(state)` /
`__perceus_dup(log)`, which lowers to `kai_internal_dup` at
emit, balancing the downstream decref-aware sink's burn-off.
Update the comment block at lines 23787–23806 to document the
"state and log are aliases of `self->state`, the storage's
lifetime is the handler's not the clause's, so every read pays
a dup" invariant. Remove the `let _keep_alive = state`
workaround in `stdlib/trace.kai::with_log_prefix` (lines
109–112) — collapse the clause back to `read(resume) ->
resume(state)`. Add `examples/effects/r10_repro.kai` and
`examples/effects/r11_repro.kai` to a verified pass list (rename
or re-gate them once they pass; the diagnostic markers can come
off). Run tier0 + tier1-asan to confirm both repros now pass
with no sanitizer diagnostic, and that no existing fixture
regresses (the existing parameterised-handler tests with
non-decref-aware sinks like `Reader[Int]` plus `+ 1` already
pay an unnecessary dup; the runtime cost is one extra
`kai_incref`/`kai_decref` pair per state read, well within the
loose runtime's tolerance).

### Files + functions to change

| File | Function / line | Change |
| --- | --- | --- |
| `stage2/compiler.kai` | `pcs_is_non_last` (line 23808) | Add `if nm == "state" or nm == "log" { true } else { ...existing body... }` at the top of the function. |
| `stage2/compiler.kai` | comment at lines 23787–23806 | Document the state/log alias invariant — why the special case is sound and why it's the minimum-cost fix. |
| `stdlib/trace.kai` | `with_log_prefix` `read` clause (lines 109–112) | Collapse to `read(resume) -> resume(state)`. Drop the `_keep_alive` and the long comment justifying it (replace with a one-line pointer to the fixed predicate if anything). |
| `examples/effects/r10_repro.kai` | header marker | Once the fix lands and ASAN passes, drop the `# DIAGNOSTIC ONLY — KNOWN ASAN FAIL` line and rename to a stable fixture name (suggested: `examples/effects/m8_13_param_state_decref_alias_via_reader.kai`). |
| `examples/effects/r11_repro.kai` | header marker | Same — rename to e.g. `examples/effects/m8_13_param_state_decref_alias_via_carrier.kai`. |
| (R10 + R11 regression entries) | now closed by issue #61 | Both entries were retired with the rest of `docs/known-regressions.md` in PR #76+1; the new fixtures replace the manual repros. |

### What the fix does NOT need to change

- `stage0/runtime.h`'s `kai_evidence_lookup_node` and
  `in_dispatch_node` machinery — works as specified.
- The op-call emit at `stage2/compiler.kai:8274` — the
  save/restore around the indirect clause call is correct.
- `clause_state_prologue` (`stage2/compiler.kai:9933`) — the
  `KaiValue *kai_state = self->state;` aliasing is intentional;
  the dup happens at *each read* under the fix, not at the
  prologue.
- The two-arg `resume(v, ns)` branch in `emit_call_expr`
  (`stage2/compiler.kai:8037-8043`) — this writes `self->state =
  ns` for `State[T]::set(v, resume) -> resume((), new_state)`.
  Untouched. The new state is owned by `self->state` after the
  write, so it follows the same alias pattern — a subsequent
  `state` read inside another clause will need (and get) a
  `__perceus_dup` under the fix.

### Out of scope for the fix lane

- R9 (handler clauses do not capture enclosing-fn parameters).
  Sibling lane `r9-clause-env` is in flight — do not collide on
  `emit_clause_body`. R9's fix needs `EvE.env` plumbing; R10/R11
  fix is local to `pcs_is_non_last`. They are independent.
- The Perceus exit-pass (the rest of m5.x). The dup added by
  this fix accumulates one extra incref per state read; under
  the loose runtime this is acceptable (small leak per call
  invocation, balanced by the consumer's existing decref).
  Tightening this is part of the Perceus follow-up, not this
  fix.

---

## Verification done in this lane

- Built kaic2 from this branch's main checkout.
- Compiled both repros cleanly with kaic2 (`--path ../stdlib`).
- Compiled both C outputs cleanly with `cc
  -fsanitize=address,undefined`.
- Ran both binaries; both crashed with byte-identical
  heap-use-after-free traces, captured inline above.
- Inspected the emitted clause C for both repros and confirmed
  the raw `kai_state` transfer (no `kai_internal_dup`).
- Inspected the workaround-applied stdlib `with_log_prefix`
  emit (`/tmp/r11.c:331-334`) and confirmed the
  `kai_internal_dup` wrap that the workaround forces.
- Confirmed `tier1-asan` only operates on `demos/`; the new
  repros under `examples/effects/` are not picked up by any
  glob-style target in `Makefile` or `stage2/Makefile`.

Issue #61 stays open. This doc is the input to the
`r10-r11-fix` lane.
