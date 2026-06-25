# Lane experience — issue #892

A monomorphic spec of a protocol-bounded generic (`fn larger[T:Ord]`)
ran ~4× slower than its hand-written monomorphic twin in a hot loop.
The issue framed it as "lost CSE — the spec re-evaluates a multi-use
parameter's argument expression". The step-0 re-measurement showed the
real cause is different: lost UNBOXING, not lost CSE.

## Step 0 — re-measure (the issue's diagnosis was wrong)

The issue cited an old main (`ae4241ba`) and an assembly `msub` count
of 2-vs-1 in `main`. On current main (`#905`), the repro still
reproduces — generic 0.13 s vs direct 0.03 s, ~4.3× — but the assembly
metric was a red herring (kaikai compiles the loop into a separate
self-tail goto-loop function, never inlined into `main`, so the
`msub`-in-`main` count is contaminated by unrelated runtime helpers).

Emitting and reading the C settled it. The `% 1000003` reduction
appears EXACTLY ONCE in both the generic and direct C — there is no
re-evaluation, no lost CSE. The actual difference is the spec's
calling convention:

- Direct twin: `int64_t kai_larger_int(int64_t a, int64_t b)` — raw
  scalar params, the whole loop is `int64_t`.
- Generic spec: `KaiValue *kai_larger__mono__Int(KaiValue *a, KaiValue
  *b)` — BOXED params. The loop boxes each arg (`kai_int(...)`),
  unboxes the result (`kai_intf(...)`), and runs `dup`/`drop` RC per
  iteration. That boxing + RC traffic is the entire ~4×.

So the bug is: a monomorphic spec of a multi-use-parameter `fn[T:P]`
keeps a BOXED signature where the direct twin is unboxed. Multi-use is
incidental (single-use is DCE'd in both); the load-bearing fact is that
the spec never adopts the int64 ABI.

## Root cause — the spec signature stays polymorphic

The pipeline order is `monomorphise → unbox_pass → perceus_pass`
(`driver.kai`; the comment at `unbox.kai:11` was stale and claimed the
inverse). `unbox_pass` classifies each fn via `classify_unbox_sig`
(`fnreg.kai`), which reads the SIGNATURE: a fn qualifies for the
unboxed int64 ABI only if its params/return are concrete C scalars and
`tparams == []`.

`specialise_decl` (`monomorph.kai`) already clears `tparams=[]` on the
spec. But `subst_decl` substitutes ONLY the body (`subst_expr`), never
`Param.ptype` or `opt_ret`. So `larger__mono__Int` arrived with params
whose `ptype` was still `TyName("T")`. `param_ty` resolved that to
`TyCon(None,"T",[])`, `any_param_unboxable` returned false, and
`classify_unbox_sig` returned `None` — the spec stayed boxed.

The body already had concrete types (subst_expr ran), which is why the
program was always CORRECT; only the SIGNATURE was left polymorphic,
and the signature is what unbox inspects.

## Fix

Concrete the spec's syntactic signature in `specialise_decl`: build a
`[SigSubst]` mapping each type-kind tparam name to its concrete
`TypeExpr` (derived from the `MonoTuple`'s `tys`, dropping unit-kind
tparams to stay aligned), then rewrite `Param.ptype` and `opt_ret` with
`apply_sig_subst_te` (mirrors emit_c's `apply_tp_subst_te`). A new
`ty_to_typeexpr` reverses `resolve_ty` for the slots that matter to
unboxing (primitives → bare `TyName`); compound types reconstruct
structurally. The substitution is by NAME (`TyName("T")`), kept
separate from `subst_decl`'s id-keyed `SubstMap` per the design review
(asu): the two substitutions operate on different domains.

The body is untouched — `subst_expr` already concreted it. Only the
signature changes, and only for specs whose tparams were type-kind.

## After-numbers

| | before | after |
|---|---|---|
| generic spec signature | `KaiValue *(KaiValue *, KaiValue *)` | `int64_t(int64_t, int64_t)` |
| boxing in hot loop | 2× `kai_int` + 1× `kai_intf` / iter | none |
| runtime (C backend, best-of-5) | gen 0.13 s / dir 0.03 s | gen 0.04 s / dir 0.03 s |
| runtime (native backend) | — | gen 0.12 s == dir 0.12 s |

The spec body now emits identically to the direct twin:
`((kai_protocols____pimpl_Ord_Int_cmp(kair_a, kair_b) > 0LL) ? kair_a :
kair_b)`.

## Correctness + ABI verification

- `larger[T:Ord]` over Int / Real / String all correct (Int and Real
  unbox; String stays boxed, as it should).
- Function-as-value (`apply2(larger, 10, 4)`) still works: the boxed
  polymorphic original survives for by-name uses, so the unboxed spec
  signature does not break the higher-order ABI (the v0.85/0.86
  `i64()*`→`KaiValue*()*` bitcast hazard asu flagged — not triggered,
  since by-value uses route through the boxed original, by-name direct
  calls route through the spec).
- Selfhost byte-id GREEN both stages (`kaic1b.c==kaic1c.c`,
  `kaic2b.c==kaic2c.c`): the compiler's own self-compile is a fixed
  point even though emitted USER code changed.

## Fixtures

`examples/perceus/mono_arg_unbox_892.kai` + `.out.expected`, wired as
`test-issue-892-mono-arg-unbox` in the light-parallel tier. It runs the
generic and direct twins and asserts they agree, and the Makefile target
greps the emitted C for the unboxed spec signature + asserts no boxing
survives in the hot loop body — an op-shape check, not just a runtime
one (a perf regression that re-boxed the spec would still print
"agree", so the grep is the real gate).

## Structural surprises

- The issue's measurement methodology (`msub` in `main`) does not apply
  to kaikai's codegen — loops live in their own goto-loop functions.
  Reading the emitted C is the reliable oracle; assembly counts mislead.
- The `unbox.kai:11` pipeline-order comment was inverted relative to the
  driver. Corrected the `fnreg.kai` exclusion comment that called this
  a "post-1.0" limitation; it is now closed.

## Follow-ups

- None required. The fix is general: any scalar `fn[T:P]` spec now gets
  the int64 ABI. Compound-typed specs (lists, records) stay boxed,
  unchanged.
