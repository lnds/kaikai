# Lane experience — native-parity burn-down 7 (`np-crash`)

**Scope:** the crash/timeout family of the in-process libLLVM native backend's
parity gap vs the C-direct oracle (KIR Lane 1.5). From `main` at 67 gaps
(burn-down 6 merged). The brief named the hardest sub-signatures: `nat=1`
panics (Hash proto-dispatch, math_real `-`/`^`, regex parse, requires), `nat=139`
SIGSEGV (quicksort×2, deep recursion), `nat=124` timeout (spiral, nested-alias
m7b_15/m8_12, multi_var_state), `nat=138` (issue_668 map-in-fiber). Standing
instruction: close the MECHANICAL causes to root; a sub-cause that needs a
design decision is documented and left, not improvised.

**Outcome:** TWO mechanical root causes closed (67 → 53, −14, zero regressions),
FOUR design-bearing causes diagnosed to a clean cause and documented. Every
closed fixture is byte-id vs C-direct; every closed crash is ASAN-clean;
selfhost byte-id held; `make test-kir` GREEN with no golden churn.

## The two mechanical root causes closed

### 1. pipe-lowering (`EPipe` → unit) — the quicksort SIGSEGV's real cause

`EPipe(lhs, rhs)` had no real lowering: `lower_expr` returned `KUnitV`, so a
piped call (`xs |> filter(p)`, `ys |> each(print)`) VANISHED. The typer
(`synth_pipe`) rebuilds `EPipe(lhs, ECall(f, args))` rather than desugaring to a
plain `ECall`, leaving each backend its own pipe desugar — emit_c has
`emit_pipe_expr` (the byte-id oracle), the native KIR had none.

The diagnosis that mattered: **quicksort's `nat=139` SIGSEGV was NOT a memory
bug — it was infinite recursion caused by the lost pipe.** `let smaller = rest
|> filter(x => x < pivot)` lowered to `smaller = ()` (unit), so `sort(smaller)`
recursed on a non-shrinking input until the stack overran. The lldb backtrace
(`quicksort__sort + 144` repeated to the stack ceiling, a `kai_internal_drop(v=
0x0)` at the leaf) pointed straight at it; the KIR dump (`smaller: box = ()`)
confirmed it. So an output-mismatch family (the doc filed pipe-lowering under
`output-mismatch`) and a SIGSEGV family shared one root cause — a reminder that
the failure signature is downstream of the bug, not the bug.

Fix: `lower_pipe` (kir_lower_walk.kai) rewrites the pipe to the equivalent
`ECall` with `lhs` spliced in as the FIRST argument and delegates to
`lower_call` — node-for-node the oracle's desugar:
- `lhs |> f(a, b)` → `f(lhs, a, b)`
- `lhs |> mod.g(a)` → `mod.g(lhs, a)`
- `lhs |> f` / `mod.g` → `f(lhs)` / `mod.g(lhs)`
- `lhs |> <expr>` → indirect `kaix_apply((<expr>), lhs)`

The burn-down-3 retro recorded a partial cut that "TRAPPED the multi-candidate
combinator path (`filter`/`map`/`reduce`) with `non-exhaustive match`." The
lesson it missed: that cut hand-built the `ECall` callee/args and reached an
unhandled shape. **Routing through `lower_call` — the same path a non-piped
call takes — makes the multi-candidate combinators fall out for free**, because
the typer already resolved the callee to the right `EModCall`/`EVar` and
`lower_call`'s dispatch consumes it unchanged. Closed collatz / euler1 /
factorials / forth / wc / demos-vs / capture / minimal-fizzbuzz + quicksort×2.

### 2. match guards + all-binder dispatch — guards silently dropped

The KIR `lower_match` dropped EVERY arm guard: every `Arm(p, _, body)`
destructure discarded the `Option[Expr]` guard slot. So `match o { Some(n) if
n > 10 -> "big"  Some(n) -> "small"  None -> "none" }` returned "big" for
`Some(3)` — the first arm whose PATTERN matched won, regardless of the guard.
Worse, a `match` whose arms ALL bind (`fn fizzbuzz(i) = match i { n if n%15==0
-> …  n -> … }`) had no tag to switch on, so it fell to the variant tag-switch
path, which emits `KTagOf` over the scalar scrutinee — and `kaix_variant_tag_of`
dereferences a tagged Int as a pointer → SIGSEGV. The KIR dump was the tell:
`switch (tagof i) [] default L1` — an EMPTY switch routing everything to the
first body.

asu's verdict (consulted on the architecture): **any `match` with ≥1 guard, or
with no discriminating pattern, degrades to a linear fail-chain — do NOT try to
graft guards into the O(1) switch.** The switch is sound only when the tag alone
decides the winner; a guard can reject an arm AFTER its pattern matched, and the
fall-through must reach the NEXT arm in source order. Grafting that into the
tag-switch reconstructs the linear chain INSIDE the switch (a second source of
truth for arm order) and diverges from the oracle the moment a guard has effects
(it would skip an intermediate other-tag arm). The KIR never needs to be
cleverer than its oracle; exhaustivity is the typer's job, so degrading to
linear is always sound.

Fix: `lower_guard_chain` (kir_lower_walk.kai) mirrors emit_c's `emit_match_arm`
exactly — per arm, in source order: pattern-test (`guard_emit_pat_test`, routing
a mismatch to a per-arm `next_fail`) → `bind_default_pattern` → if guarded,
`condbr guard ? body : next_fail` → lower the body into the shared join. No
`KTagOf`. The dispatcher diverts to it before the specialised paths when
`match_has_any_guard(arms) OR not match_has_discriminating_pattern(arms)`.
Closed fizzbuzz / imc / collatz + attr_unstable_refine_narrow (a `p : Pos`
refinement-narrow over a scalar — also all-binder) + int_field_inline.

## Structural surprises the brief did not anticipate

- **The "exit/SIGSEGV/timeout" family was not one family.** The brief grouped
  by exit signature, but the causes cut across the signature axis: quicksort's
  SIGSEGV and collatz's output-mismatch were ONE cause (pipe), fizzbuzz's
  SIGSEGV and imc's output-mismatch were ANOTHER (guards). Diagnosing by the C
  oracle as ground truth (KIR dump + lldb + the `--emit=c` firma) collapsed the
  signature noise to two mechanical causes — and surfaced that closing the pipe
  cause turned fizzbuzz from output-mismatch INTO a SIGSEGV (the guard bug it
  had been masking), which then closed too.
- **`math_real`'s panic was NOT a math bug — it was an RC use-after-free.** The
  `type mismatch in -` panic on `abs(0.0 - 3.5)` reduced (via lldb watchpoint)
  to: `my_abs`'s return slab has rc=0 the instant it returns, and the next
  alloc (a `kaix_str` of the next label) recycles it. Root: `pcs_ty_is_unboxed`
  treats Real as unboxed so Perceus inserts no dup for a Real used twice
  (`if x < 0.0 { 0.0 - x }`), but the native backend is all-boxed and the shared
  `kaix_lt`/`kaix_sub` CONSUME both operands → double-decref. emit_c is correct
  because it genuinely unboxes Real to a C `double` (no RC). I almost mis-blamed
  the unbox PASS (a no-op experiment did NOT fix it — emit_c re-derives the
  unboxing itself); the real fix is the KIR unboxing milestone, design-bearing.
- **Hash proto-dispatch is half-mechanical, half-infrastructure.** The dispatch
  shims (`kaix_proto_dispatchN`) + the `lower_fn` rewrite are mechanical and
  WORK (the dispatch runs); but `kai_lookup_impl` returns NULL because the
  native backend NEVER registers the impl table — `_kai_register_proto_tables`
  (variant-heads + impls + reusable-tags + payload-ctors) is C-only, emitted
  into the C startup. Building that registration subsystem for the native path
  (materialise an fn-ptr table via the C-API + a startup hook, mirror
  `collect_pimpl_rows`/`emit_impl_table_array`) is its own infra lane. I spiked
  the dispatch half, confirmed the table is the blocker, and REVERTED the spike
  rather than land a half-cause (the brief's "finish or don't touch").

## Design-bearing residue (documented, not forced)

Per the brief — a gap needing a design decision is documented, not improvised.
All four were diagnosed to a clean cause (not "it crashes, unknown"):

- **Real-arith RC use-after-free** (math_real_basic, free_fall, complex_basic,
  complex_heterogeneous, unbox_bench_real, unbox_phase2_cond_real). The KIR
  unboxing milestone: native must be mode-slave (raw Real never enters RC; arith
  routes to f64 prims, not `kaix_sub`; box/unbox only at boundaries). A
  crash-lane patch (native-side ops that don't consume Real operands) swaps the
  crash for a leak or re-introduces the pinned second-source-of-RC bug-class.
- **multi/nested same-effect alias-dispatch** (spiral, m7b_15_nested_alias,
  m8_12_self_delegating_handler, multi_var_state_index). Two `var` / `with … as
  a` handlers of the SAME effect where an op through the OUTER alias runs from
  inside the INNER body or a while-body closure. Codegen is correct (per-alias
  `kaix_evidence_lookup_node_by_id` via the captured `__alias_id__<a>`) and the
  runtime lookup is correct; the bug is the evidence-frame/alias model across
  closures-in-loops (the subset-2b clause-param-origin family, refs #622).
- **Hash proto-dispatch table registration** (hashmap_basic/collision,
  hashset_basic/ops). The native backend emits no impl-table / variant-to-head
  startup registration; the dispatch shims are ready, the table is not.
- **issue_668 map-in-fiber** (nat=138). `map` over 40K alone passes native
  (TRMC/TCO applies); the crash is only inside the spawned fiber — the
  no-effect-handler Spawn family (no real native fiber scheduler runtime).

## Fixtures added and coverage

No new positive fixtures: the closed baseline fixtures ARE the regression
coverage, locked by `tools/native-parity-baseline.txt` (67 → 53). The minimal
repros driving each diagnosis (`my_abs`/`chk` for the Real-RC cause, `classify`
for guards, the two-var while for alias-dispatch) live as the lane's diagnostic
record here, not as fixtures — they reproduce known-design-bearing causes the
baseline already tracks via the real fixtures.

## RC + soundness validation

- Parity byte-id vs C-direct on every closed fixture (a leak / double-free /
  lost-call diverges or crashes).
- ASAN clean on every closed CRASH fixture (quicksort, fizzbuzz, imc, collatz,
  int_field_inline) — the slab allocator's pool defeats ASAN on the heap, so the
  ASAN signal is on the link-level memory ops; clean means no buffer/UAF the
  sanitizer can see.
- `make test-kir` GREEN, goldens UNCHANGED — asu verified no `examples/kir/*`
  fixture uses a match guard, and the pipe rewrite delegates to existing
  `lower_call` (no new dump shape), so zero golden churn (the burn-down-4
  stale-golden failure mode avoided by construction).
- selfhost byte-id OK — the KIR-lowering changes do not touch the C path that
  builds kaic2.
- `tools/test-backend-parity.sh` with `NATIVE_PARITY_RATCHET=1`: ratchet OK
  (improved), exit 0, zero regressions.
- No changes to emit_c.kai or stdlib/ (the oracle does not adjust to itself).
  Runtime changes were additive only (and the proto-dispatch spike was
  reverted, so the runtime is unchanged on the landed diff).

## Real cost vs estimate

Diagnosis dominated. The lldb watchpoint on the Real-RC slab and the KIR dump on
the empty guard-switch each collapsed a whole family to one cause in minutes; the
C `--emit=c` firma (`double kai_my_abs(double)`) was the single clue that the
Real divergence is unboxing, not arithmetic. Two asu consults paid for
themselves: the Real-RC verdict (it's the unboxing milestone, not a patch —
saved a wrong fix that swaps crash for leak) and the guard-chain architecture
(linear over switch-graft — saved an order-of-evaluation bug the byte-id gate
would NOT have caught, since zero goldens use guards). The proto-dispatch spike
cost one build cycle to confirm the table-registration blocker, then a clean
revert — the cheap way to learn a "mechanical" cause is actually infra.
