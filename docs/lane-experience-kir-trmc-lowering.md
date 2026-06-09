# Lane experience — `kir-trmc-lowering`

Fix the AST→KIR lowering of tail-recursion-modulo-cons (TRMC) for the
builtin-cons idiom `[h, ...self(t)]` so the KIR/native path gets a
well-formed `KTrmcStep` instead of (a) no rewrite at all and (b) a malformed
step that still emits the recursion. The native backend lowering of the step
is explicitly OUT of this lane (integrator follow-up).

## Scope as planned vs as shipped

Planned: the two-part fix (driver gate + `lower_trmc_step` reshape), the
`KTrmcStep` node reshape, its dump printer, and the back-edge recognizer
arity. Shipped exactly that, plus the matching arity bump in
`emit_native_term.kai`'s `nemit_term` match (the reshape changed the node's
arity, so the existing `nemit_unsupported` arm had to count seven slots — its
abort behaviour is unchanged). No native backend logic, no C-direct emitter,
no LLVM-text emitter touched.

## The two-part bug

**Part 1 — gate.** `driver.kai` set `cons_ok = not (use_llvm or use_kir or
use_native)`, so the builtin-cons TRMC rewrite (`tcrec_rewrite_decls`'s
cons arm) ran ONLY for the C-text backend. On the KIR/native path
`map_inc`/`build` lowered to ordinary recursion — correct values, but
O(n) stack (a Tier-1 mandatory-TCO violation; a 200K-deep list overflows
the fiber stack on native). Fix: `cons_ok = not use_llvm`. Only the
LLVM-text emitter (a validation oracle) still lacks the cons-cell step
builder, so it stays gated off there; C and KIR/native both get the rewrite.

**Part 2 — malformed `lower_trmc_step` (the real frontend bug).** The
TRMC rewrite plants a sentinel call `__kai_trmc|sym|holeslot|cname|dm|...`
whose ctor-args are `[head, self_call]` for the builtin cons (holeslot=1,
the cdr). The old lowering did `lower_exprs(cargs, ...)` over ALL the
ctor-args — including the hole-slot arg, which is the self-call
`map_inc(t)`. That emitted `t6 = map_inc(t)` into the KIR (the actual
recursion) plus `{p0 <- t5, p1 <- t6}`, defeating TRMC: the recursion was
still made, so the stack still grew. The self-call must be the loop
back-edge, NEVER a value.

## The `KTrmcStep` reshape (Option A — mirror the C oracle)

Old: `KTrmcStep(loop_label, hole_slot, cname, dropmask, [KAssign] assigns, KPos)`
New: `KTrmcStep(loop_label, hole_slot, cname, dropmask, [KVal] head_vals, [KAssign] assigns, KPos)`

The split mirrors the C oracle `emit_trmc_cons_step` (emit_c.kai) one-to-one:

- `head_c` (the oracle's real head value, `args_e`'s non-hole slot) →
  `head_vals`: the non-hole ctor args, lowered as VALUES. The backend builds
  `cons(head_vals.., NULL_at_hole)` from them.
- `self_args = match t.kind { ECall(_, sa) -> sa }` (the oracle's recursion
  args, read from the hole slot, never evaluated) → `assigns`: the loop-param
  rebinds, exactly as `lower_tcrec` builds `KTcrecGoto`'s assigns from an
  ordinary self-tail-call.

So the new `lower_trmc_step` lowers `trmc_drop_nth(cargs, hole)` (the head
value(s)) first, then `trmc_self_args(cargs, hole)` (the self-call's args)
into the rebinds — preserving left-to-right evaluation order. The hole-slot
arg is never materialised; the recursion is gone.

I chose Option A over a "lower everything then split" alternative because
the latter would still emit the self-call as a register before discarding it
(the exact bug), and over folding head+rebinds into one list because the
backend needs the two DISTINCT (one builds the cell, one is the back-edge) —
the C oracle keeps them distinct for the same reason.

## Verification — the self-call is deferred (dump before/after)

`build` (raw EList shape `[n, ...build(n-1)]`):

Before (gate off — no rewrite, plain recursion):
```
fn map_inc(xs: box) : box {
  ...
  L4:
    t5: box = prim +(h, 1)
    t6: box = map_inc(t)                                 <- recursion emitted
    t7: box = con-reuse __pcs_scr Cons #0 {t5, t6}
    t0 <- t7
```

After:
```
  L5:
    t5: box = prim +(h, 1)
    trmc-step _kai_map_inc_entry hole=1 __kai_cons_s head=[t5] {p0 <- t} dropmask=0
```
`head=[t5]` is `h + 1` (the real head value); `{p0 <- t}` is the rebind from
`map_inc(t)`'s arg; NO `map_inc(t)` call is emitted before the step. The
`build` fn shows `head=[n] {p0 <- t3} dropmask=1` (t3 = `n - 1`), confirming
the raw-EList shape and a non-zero dropmask both flow through.

## Dropmask handling

The dropmask is parsed from the sentinel string (`es_tcrec_parse_decimal`)
and carried verbatim onto `KTrmcStep`, identical to how `lower_tcrec` carries
it onto `KTcrecGoto`. The producer (`trmc_make_step_call`) computes it as
`base_mask + rule3_mask` over the same p0..pN ordering as the rebinds; the
KIR node just threads it. `build`'s dropmask=1 vs `map_inc`'s dropmask=0
(observable in the dump) confirms the per-site value survives the reshape.
The hole-slot arg carries no ref to drop (never materialised), so there is
no extra RC to thread — consistent with the oracle, which reads `head`/
`self_args` then drops only `_scr` (the reuse-cons arm) via the dropmask.

## Gates (all run, all green)

1. KIR dump correct — `head=[..]` + rebind assigns distinct, no recursive
   call. Shown above for both `build` and `map_inc`.
2. C-direct fixture — `make test-trmc-list-build` OK (1M-deep build + map,
   O(1) stack). The gate flip did not regress the C path (C kept
   cons_ok=true throughout).
3. Selfhost byte-id — `make selfhost` → "selfhost determinism: OK
   (kaic2b.c == kaic2c.c)". The hard gate held: the normal C self-compile
   uses neither use_kir nor use_native, so its cons_ok stays true as before;
   the reshape changed no C-emitted bytes.
4. tier0 — `make tier0` OK (selfhost + demos baseline 35/35 + arena gate).
5. Backend parity — `tools/test-backend-parity.sh` pass=413 fail=0 (LLVM-text
   stays gated off, so C↔LLVM-text parity is unaffected).
6. `make test-kir`, `test-issue-668`, `test-tco` all OK.

## `km` scores (differential vs branch baseline HEAD 263afe1d)

| File | before | after |
|------|--------|-------|
| kir.kai | A++ (98.3) | A++ (98.3) |
| kir_lower_walk.kai | B+ (83.1) | B+ (83.2) |
| driver.kai | F-- (35.4) | F-- (35.4) |
| kir_dump.kai | A++ (97.9) | A++ (97.9) |
| kir_lower_fns.kai | A+ (94.9) | A+ (94.9) |
| emit_native_term.kai | A+ (95.3) | A+ (95.3) |

`kir_lower_walk.kai` is a pre-existing B+ monolith (the list/record-match
work pushed it there before this lane); my edit held the band and nudged it
+0.1 (indentation A→A+ after I split `lower_trmc_seal` out of
`lower_trmc_step` to keep each lowering at one nesting level). Cogcom stays
avg 2.5 / max 9 — well inside the bar. All other files held their grade.

## Structural surprises

- The first km comparison used `git show main:` as the baseline and showed
  an A−→B "drop". That was a false alarm: the branch's `kir_lower_walk.kai`
  was already ahead of main (list/record-match PRs) at B+. The correct
  differential is vs the BRANCH HEAD, not main — the file the lane edits
  already carried the prior lanes' debt.
- `trmc_expr_nth` exists in emit_c.kai but is not `pub`; under real
  `make selfhost` (imports, not bundle-concat) a cross-module private call
  would not resolve. I wrote a local `trmc_self_args` scanner in
  kir_lower_walk.kai rather than reach across the module boundary.

## Follow-up (integrator's lane, NOT this one)

The NATIVE backend lowering of `KTrmcStep` / `KTrmcApply` — the cctx
goto-loop (`kaix_cctx_extend` / `kaix_field_addr` / `kaix_cctx_apply` shims,
mirroring the C `emit_trmc_cons_step`) — is unbuilt. `nemit_term` still routes
both to `nemit_unsupported`, so a native build aborts on a TRMC fixture by
design. That is the integrator's follow-up; this lane's job was only to make
the KIR `KTrmcStep` well-formed (head value + back-edge rebinds, no
recursion) and visible in the dump. No native fixture was added to
`examples/native/` (it would red the native-parity harness before the
backend lane lands).
