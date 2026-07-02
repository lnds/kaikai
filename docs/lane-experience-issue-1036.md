# Lane experience — issue #1036 (TRMC dropmask / perceus wrap-skip misalignment)

## Scope

As planned: align the two ownership judges that disagree on the shape
`[h, ...self(rest, shared)]` — `pcs_branch_aware_skip_params` (the wrap-skip
decision perceus acts on) and `tcrec_compute_site_dropmask` (the goto-block
drop the TCO/TRMC rewrite plants). The diagnosis was closed by a prior
exploration lane: perceus judges `shared` branch-aware linear (one read per
mutually-exclusive arm) and transfers it raw; the emitter re-derives the
skip-set over the *post-perceus* body, where `pcs_recognise_reuse_expr` has
already replaced the cons rebuild with `__perceus_reuse_cons(...)` — a callee
`pcs_is_runtime_primitive` classifies non-linear — so the param falls out of
the re-derived skip-set, the flat `count >= 2` criterion plants the drop, and
the raw transfer is over-decref'd once per iteration. Shipped exactly that
scope; no drive-by fixes.

## Design decision

Two candidate fixes:

1. **Patch the classifier** so `__perceus_reuse_cons/record/variant` args are
   judged ctor-like linear (the #680 precedent, which patched the same
   function for dup-wrapped callees). Small, but keeps the fragile scheme:
   emit re-derives what perceus decided, over a body perceus has since
   rewritten — every future rewrite artifact is a new divergence candidate.
2. **Consume the recorded decision.** The rewrite pass records its ownership
   decision in the body itself: a wrapped read (`__perceus_dup(p)`) leaves
   the original ref alive; a bare read transfers it raw. A walker
   (`tcrec_widen_skip_raw_reads` + `tcrec_*_raw_read`, mirroring the existing
   `tcrec_*_have_evar` family) detects bare non-lambda reads and widens the
   derived skip-set with them at the two computation sites.

Shipped (2): it heals this divergence and any future one in the same
direction, including components of perceus's own skip-set union (borrowed
params, arm/goto moves, raw params) that the emit side never re-derived at
all. Widening-only was deliberate: it can only *suppress* drops that lack a
matching dup (the double-free direction); it never adds a drop, so no
formerly-passing shape can start double-freeing. Reads the walker cannot see
(interp-string spans) simply keep today's mask.

## Surprises

- The codebase already names the invariant twice (perceus.kai, on the
  locals variant: "the two must agree or RC double-counts") and #680 had
  already patched one instance of the same family in
  `pcs_call_consumers_linear` — the divergence class was known, the reuse
  recogniser instance was not.
- The dropmask's own header comment justifies `count >= 2 → drop` with
  "every read is `__perceus_dup`-wrapped" — the fix makes that premise
  checked instead of assumed.

## Verification

- Repro (17-line `append2` shape, shared arg, `len(a) >= rc(b)`): ASAN
  heap-use-after-free on the C backend before the fix; clean after, with
  correct output. The masked `kai_internal_drop(*_kai_tcrp1)` disappears
  from the emitted goto block while the raw `_t1 = kai_b` transfer stays.
- Fixture `examples/perceus/trmc_spread_shared_arg.kai` + `.out.expected`,
  wired as `test-perceus-trmc-spread` (TEST_LIGHT_TARGETS) and
  `test-perceus-trmc-spread-asan` (tier1-asan, `-DKAI_NO_CELL_POOL` so the
  stale cons hits free'd memory instead of the recycling pool).
- tier0 + selfhost locally; tier1 / serial native parity / ASAN delegated
  to CI.
- Self-hosted binary A/B probe (ASAN, cell pool disabled, same
  hello-world input): the PRE-fix self-hosted compiler's first report is
  the heap-use-after-free in the `ar_acc` fold feeding
  `append_op_arities` (`add_decls_loop_rk` → `kai_free_value`) and the
  process cannot proceed past it even with `halt_on_error=0`. The
  POST-fix binary no longer has that UAF at all; inspection confirms the
  masked drop is gone from `append_op_arities` in the self-hosted emit
  while the raw transfer stays, and the full tier0 (selfhost byte-id
  fixpoint included) runs green on that binary.
- The post-fix ASAN run then surfaces a DIFFERENT, previously-shadowed
  report further down the same path: an input-independent
  heap-buffer-overflow READ in `typecheck_module` (redzone before a live
  `expand_ta_decl` allocation — a shape/field-arity confusion, not an RC
  lifetime symptom; drop suppression can only over-retain and the
  non-ASAN binary is byte-id stable). Pre-fix runs never reached this
  site. Next chain link, out of this lane's scope.
- Native chain probe: the native-built compiler (COMPILE+LINK+RUN green)
  no longer dies at the `append_op_arities` double-free; it now advances
  into the typer and spins in `effs_declaring_op` walking what looks like
  a corrupted (cyclic) list — the native lowering's own RC handling,
  also out of this lane's scope.

## Cost

Small: the diagnosis arrived closed (file:line for both judges), so the lane
was implement + verify. The only sizing decision was walker breadth — kept
to a mirror of the existing `have_evar` family plus the dup shield.

## Follow-ups

- Two next chain links surfaced by unblocking this one, both needing their
  own lanes/issues: (a) the input-independent heap-buffer-overflow read in
  `typecheck_module` under ASAN+no-pool on the self-hosted C binary;
  (b) the native-built compiler's non-terminating list walk in
  `effs_declaring_op`.
- The emit-side re-derivation (`pcs_branch_aware_skip_params` at the two
  tcrec sites) is now belt-and-braces under the widening; a later lane could
  replace it outright with the recorded-decision walk and retire the #680
  shim, but that changes masks in the leak (non-crash) direction and wants
  its own parity run.
- Other hand-synchronised perceus↔emit pairs (dm_table ↔ collect_exit_drops,
  arm_all_tails_ctor ↔ consume_token) remain re-derivations; same
  registration-based treatment applies if they diverge.
