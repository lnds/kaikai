# Lane experience — issue #1025: native -O2 cons-reuse over a shared donor

## Scope as planned vs as shipped

**Planned (the #1025 contract):** the native-built compiler SIGSEGVs in
`kaix_variant_tag_of` while compiling any input — a `Decl` argument reaches
`modules__rqc_decl` as a tagged Int (`0xb`) instead of a variant pointer. The
brief's starting hypothesis: the evidence-frame `__evf` param prepended to
`rqc_decl` (it carries a `Console + File` row) collides with passing a
list-destructured element as the first real arg. Close it → recursive self-host
unblocks → the #1021 gate escalates from LINK+RUN to full self-compile.

**Shipped:** root cause found and fixed — but it was NOT the evidence-frame
arg-shift. It was a Perceus cons-reuse miscompile in the native backend: a
`match ds { [d, ...rest] -> [f(d), ...g(rest)] }` reuse arm, over a SHARED donor,
dropped its borrowed head/tail children without an incref. The fix closes the
SIGSEGV exactly. A SECOND, independent use-after-free (masked by the first) then
surfaced downstream in `validate_pub_access` (`field access on non-record`), so
full recursive self-host is NOT yet green. This lane closes the documented
SIGSEGV; the residual is tracked separately.

## Root cause

The native cons-reuse path (`kir_lower_walk.kai`) mirrors the C-direct oracle's
two-branch reuse: a UNIQUE donor donates its cons cell in place; a SHARED donor
fresh-allocs and keeps a BORROWED reference to each kept child (`d`, `rest`).
The oracle increfs `_scr->head` / `_scr->tail` in its shared branch. The native
backend emitted the shared-donor incref (`kai_incref_if_shared`, #872) only for
VARIANT reuse — gated on `reuse_kind_is_variant(rk)` in `lower_reuse_con`, and
the reuse-gc binder set was published only for `PVariant` arms
(`set_reuse_gc_for_arm` → `variant_reuse_scrutinee_binders`). For a `PList` cons
arm the set was empty and no incref was emitted, so the match-exit
`decref(donor)` freed `d`/`rest` while the still-live shared list referenced
them. Reading the list afterward saw recycled memory: a tagged Int where a
variant pointer was expected → `kaix_variant_tag_of(0xb)` → SIGSEGV.

Why the starting hypothesis missed: the evidence-frame IS prepended, but it is
passed correctly. The `__evf` was a red herring — its ONLY causal role is that
carrying an effect row routes the list rebuild through the reuse path instead of
TRMC (a tail-`[h, ...recurse]` with no row lowers as tail-recursion-modulo-cons,
which sidesteps the reuse machinery entirely). That is why a small
`match`-over-list repro without a row did not reproduce, and adding a row to a
NON-shared repro still did not: three conditions must coincide — (1) the arm
rebuilds the cons (reuse), (2) an effect row forces the reuse path over TRMC,
(3) the donor is SHARED (kept live past the walk). In the real compiler,
`rqc_decls` satisfies all three: it carries `Console + File`, rebuilds
`[rqc_decl(d), ...rqc_decls(rest)]`, and the decl list is shared across passes.

## The fix

Three coordinated edits in `stage2/compiler/kir_lower_walk.kai`:

1. `lower_reuse_con` — drop the `reuse_kind_is_variant(rk)` guard on the
   shared-donor incref; gate only on `not nested` (nested is variant-only and
   already handled by the fresh-alloc-and-dup path). Cons now emits the
   `kai_incref_if_shared` per kept child.
2. `set_reuse_gc_for_arm` — dispatch the scrutinee binder set by pattern shape
   via the new `reuse_scrutinee_binders`: `PList` keeps head + tail (both cons
   pointer slots), `PVariant` keeps its slot-kind-filtered binders as before.
3. `lower_list_arms` — publish the reuse-gc set for a reuse arm (the list-arm
   path never called `set_reuse_gc_for_arm`; the variant-arm path did via
   `lower_arm_rc`). Without this the `KConReuse` lowered in the arm body had an
   empty gc set and emitted no incref even after edit #1.

The C oracle is byte-identical (its shared branch already increfs), so
`make selfhost` stays green — this is a native-only correctness fix.

## Verification

- Regression fixture `examples/perceus/cons_reuse_shared_donor_1025.kai`
  (+ `.out.expected`): native diverges from the C oracle WITHOUT the fix
  (`in_tags=-6254316544952944719`, freed memory) and matches WITH it
  (`in_tags=10`). Picked up by `tools/test-backend-parity.sh`
  (`examples/perceus` is in the corpus).
- The native-built compiler no longer SIGSEGVs on `rqc_decl`: the exact #1025
  crash (exit 139 in `kaix_variant_tag_of`) is gone.
- `make tier0` green (incl. ASAN); `make selfhost` byte-id green.
- Serial backend parity (`BACKEND_PARITY_JOBS=1` — mandatory for an RC change,
  parallel false-greens double-frees).

## Structural surprises the brief did not anticipate

- The `-O2` sensitivity in #1025 was misleading. The bug reproduces at `-O0`
  too (verified) — it is a codegen (KIR-lowering) miscompile, not a pass-pipeline
  artifact. `-O2` only shifts WHERE the freed cell is recycled, changing the
  observable crash address; the missing incref is present at every opt level.
  The "known -O0-hides-native-segfaults" trap does not apply here.
- Reducing from the real compiler was essential (the brief predicted this). The
  narrow three-condition trigger is not reachable by extrapolating simple
  patterns; the minimal repro came from replaying `rqc_decls`'s exact shape
  (variant list + effect row + shared donor kept live).
- A SECOND independent use-after-free was hiding behind the first. With the
  SIGSEGV gone, the native-built compiler advances ~170 more decls and then
  aborts in `validate_pub_access` → `vpa_sig_te` with `field access on
  non-record`: a `TypeExpr` in a synthetic decl's signature
  (`real_pow_int_loop`, a lowered `impl` method) reaches a field read as a
  non-record. Instrumented checksums show the decl list is byte-identical to the
  C oracle at the top level right up to `validate_pub_access`, so the corruption
  is a deeper shared-substructure UAF surfacing DURING vpa (the `TypeExpr`
  records are physically shared between `prelude_segs` and `qualified_prelude`).
  It is NOT a missing cons/variant/record reuse incref — an IR audit confirms
  every reuse site now carries its `incref_if_shared`. This is a distinct bug of
  a different mechanism and needs its own reduction.

## Follow-ups left for next lanes

- The residual `field access on non-record` UAF in `validate_pub_access` blocks
  full recursive self-host. The native-built compiler LINKs, RUNs (`--version`),
  and no longer SIGSEGVs — but does not yet compile a program end to end. The
  #1021 gate stays at LINK+RUN; escalation to full self-compile waits on this
  second bug. Repro handle: build the native compiler, compile any `.kai`; it
  aborts on the sig of the first lowered `impl` method whose param `TypeExpr` is
  shared across passes.
- Perf: the cons shared-donor path fresh-allocs and increfs (correct but not
  in-place-recycling). The unique-donor path still donates in place; only the
  shared path pays. No action needed — matches the C oracle's cost.
