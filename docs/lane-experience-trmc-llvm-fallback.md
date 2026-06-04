# Lane experience — TRMC sentinel leaks into LLVM IR (parity block)

## Scope as planned vs. as shipped

**Planned:** after the FAM portability fix turned tier1-asan green, the
only remaining red was `tier1-backend-parity` — 1 fixture of 483
(`examples/perceus/nested_pattern_reuse_balance.kai`) failing on the
LLVM backend. The task: fix it so CI goes fully green (and the version
bump can proceed).

**Shipped:** the LLVM backend was emitting a TRMC cctx sentinel as a
raw function name with illegal `|` characters. Fixed by gating the TRMC
rewrite to the C backend only; LLVM-bound TRMC-eligible fns fall back to
ordinary recursion. One predicate threaded through 3 functions + the
driver call site.

## Root cause

`tcrec_rewrite_decls` (emit_c.kai) is an AST→AST pre-pass run for BOTH
backends since #706. It plants two kinds of sentinel `EVar` callees:

- `__kai_tcrec|<sym>|<dropmask>|<p0>|...` — plain self-tail-call (TCO).
- `__kai_trmc|<sym>|<holeslot>|<cname>|...` — tail-recursion-modulo-cons
  (Okasaki rb-tree rotations build `Node(.., recurse, ..)` in tail
  position; TRMC rewrites it via a constructor-context (cctx) so the
  spine is reused in place).

The LLVM emitter (`llvm_emit_call`) recognises the `__kai_tcrec|`
sentinel (added in #706) and lowers it to a `br`-loop. It has **no**
handler for `__kai_trmc|`. So a TRMC-rewritten body reached the LLVM
emitter as `ECall(EVar("__kai_trmc|insert_loop|1|Node|2|t|k|v"), ...)`,
and the emitter fell through to its default "call a named function"
path, printing:

```
%t6477 = call %KaiValue* @kai___kai_trmc|insert_loop|1|Node|2|t|k|v(...)
                                          ^ error: expected '(' in call
```

`|` is illegal in an LLVM identifier → invalid IR → clang rejects it.
The C backend was unaffected: it lowers both sentinels.

This is a #706 follow-up. #706 made `tcrec_rewrite_decls` run for both
backends (previously C-only) to single-source the TCO analysis; that
correctly added `tcrec` lowering to LLVM but left the `trmc` sentinel —
a strictly more complex rewrite — unhandled on the LLVM side.

## Fix

Thread an `allow_trmc: Bool` flag from the driver
(`allow_trmc = not use_llvm`) through `tcrec_rewrite_decls` →
`tcrec_rewrite_decl` / `tcrec_rewrite_pcs_ret_wrap` to the two TRMC
decision sites. When false, a TRMC-eligible fn skips
`trmc_rewrite_body` and takes the ordinary path:

- if it ALSO has a direct tail self-call, it gets the plain `tcrec`
  rewrite (which LLVM does lower);
- otherwise it is left as ordinary recursion (no rewrite). Correct —
  just without the modulo-cons reuse-in-place the C backend gets.

Plain `tcrec` TCO still fires on both backends (it always did since
#706); only the `trmc` cctx rewrite is now C-only.

### Why fall back rather than implement TRMC in LLVM

Implementing the cctx lowering in the LLVM backend is a real feature
(constructor-context allocation, hole tracking, linear apply) — far
beyond a CI-unblock. The fallback is sound and cheap: the fn still
computes the right answer, it just allocates the spine the ordinary way
instead of reusing it. The LLVM backend already lacked TRMC (it emitted
broken IR, i.e. it never worked), so this removes a latent
miscompile, not a working optimization. The honesty note: LLVM rb-tree
does not get modulo-cons reuse; the C backend (the perf-critical one)
keeps it.

## Verification — asymmetric, both backends correct

- **LLVM:** the failing fixture now emits zero `|`-named functions,
  clang compiles it clean, runs, and matches the golden.
- **C unchanged:** the same fixture still emits the cctx/TRMC machinery
  (4 sites) and matches the golden — the C path is untouched
  (`allow_trmc = true`).
- **rb-tree perf intact (the load-bearing concern):** C-backend
  1M-insert bench still emits TRMC (4 cctx sites), median 0.460s — same
  as the pre-session baseline (0.454s) within noise, mins identical. The
  modulo-cons reuse the whole representation exists for is preserved on
  the C backend.
- **Bootstrap + selfhost:** clean rebuild, `kaic2b.c == kaic2c.c`
  deterministic.

## Fixtures

No new fixture — `examples/perceus/nested_pattern_reuse_balance.kai`
already exercises the TRMC-through-arm-wrap shape and is wired into
`tier1-backend-parity` (the gate that caught it). It was the 1 failing
fixture of 483; it now passes on both backends.

## Coverage gaps

The fallback path (TRMC-eligible fn → ordinary recursion on LLVM) is
exercised by this fixture via the parity gate. Not separately measured:
whether any LLVM program now overflows the stack because a TRMC fn that
*would* have been bounded by modulo-cons reuse is now plain recursion —
but TRMC fns are not tail-recursive in the `tcrec` sense (their tail is
a constructor, not a self-call), so they were never TCO'd on LLVM
anyway; the fallback matches the pre-#706 LLVM behaviour. Low risk.

## Follow-ups

Implementing the `__kai_trmc|` cctx lowering in the LLVM backend (to
match the C backend's modulo-cons reuse) is a genuine feature, worth an
issue if LLVM-backend rb-tree perf ever becomes load-bearing. Out of
scope here: this lane's job was to unblock CI, and the fallback is the
correct minimal change.
