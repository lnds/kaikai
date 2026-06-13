# Native-backend parity gaps — Lane 1.5 burn-down list

> **Status (2026-06-13, after the Real box/unbox lane):**
> the in-process libLLVM native backend (`--backend=native`,
> docs/kir-design.md §7.2) is at **60 listed gaps** on the ratchet after the
> Real box/unbox lane closed 5 (65 → 60): the KIR is now MODE-SLAVE to the
> unbox pass for Real — a `MUnboxed` Real lowers to a raw `f64` register +
> native `fadd`/`fmul`/`fcmp` (`kir_lower_raw.kai`), boxing only at the
> raw↔boxed border, so the multi-use-Real double-free (`9.88131e-324`) is
> structurally impossible. Closed unbox_bench_real / unbox_phase2_cond_real
> / complex_basic / complex_heterogeneous + the `^`-on-Real fixture
> math_real_basic. Prior: the char/hex-decode lane closed 2 (67 → 65)
> (`kai_llvm_build_string_span` now decodes string-literal escapes
> C99-exactly, closing `hex_escape_literal` + `json_surrogate_decode`); the
> np-handlers lane then closed the no-effect-handler family entirely (no net
> baseline change — its residual fixture `stream_early_stop` is blocked by a
> second cause, pipe-lowering) by extending the synth-handler superset to
> no-default-block effects in dead code; and burn-down 6 closed 14 (81 → 67).
> The flip to native-default (Lane 1.5) remains **BLOCKED** on the residue
> (now mostly DESIGN-bearing: json/regex array-decode, rb-tree reuse-token
> model, the remaining File/stream handlers, pipe-lowering,
> clause-param-origin). This file is the burn-down input:
> every failing fixture, grouped by root-cause family. The anti-regression
> ratchet that locks the count is `tools/native-parity-baseline.txt` (gated
> in `tier1-native`). NOTE: the 14 removed are deterministic macOS closes;
> `list_helpers`/`list_zip3_scan` stay listed (macOS-pass/Linux-SIGSEGV), so
> the Linux/CI count may differ by the handful that need Linux re-validation.
> **Status (2026-06-13, after burn-down 7 / `np-crash`):**
> the in-process libLLVM native backend (`--backend=native`,
> docs/kir-design.md §7.2) is at **51 listed gaps** on the ratchet after
> burn-down 7 closed 14 (65 → 51), TWO mechanical root causes in the KIR
> lowering — see `docs/lane-experience-np-crash.md`. (Two preceding lanes
> took the ratchet to 65: `np-decode` closed 2, 67 → 65
> (`kai_llvm_build_string_span` now decodes string-literal escapes
> C99-exactly, closing `hex_escape_literal` + `json_surrogate_decode`); and
> `np-handlers` closed the **no-effect-handler family entirely** with no net
> baseline change — it extended the synth-handler superset to
> no-default-block effects (`ReadFault`/File/Spawn performed in dead code),
> eliminating the last whole-corpus `no handler for effect` build abort; its
> one residual baseline fixture, `stream_early_stop`, is blocked by a SECOND
> cause, pipe-lowering. All three lanes are disjoint from burn-down 7's
> pipe/guard set.)
>   - **pipe-lowering** (`EPipe` → unit). `lower_pipe` (kir_lower_walk.kai)
>     rewrites `lhs |> rhs` to the equivalent `ECall` with `lhs` spliced in
>     as the first arg, then delegates to `lower_call` — the exact desugar
>     the C-direct oracle's `emit_pipe_expr` does at codegen. The earlier
>     burn-down-3 partial cut "trapped" only because it hand-built the callee;
>     routing through `lower_call` makes the multi-candidate combinators
>     (`filter`/`map`/`each`) fall out for free. Closed collatz / euler1 /
>     factorials / forth / wc / demos-vs / capture / minimal-fizzbuzz + the
>     two **quicksort** SIGSEGVs (a lost `filter` left `sort(unit)` recursing
>     on a non-shrinking input → infinite recursion → stack overflow).
>   - **match guards + all-binder dispatch** (`X if cond -> …` ignored;
>     all-binder `match` over a scalar hit `KTagOf` → SIGSEGV). The lowering
>     dropped EVERY arm guard (`Arm(p, _, body)`) AND routed an all-binder
>     match to the variant tag-switch, which `kaix_variant_tag_of`-faults on a
>     tagged Int. `lower_guard_chain` (kir_lower_walk.kai) is a linear
>     fail-chain mirroring the oracle's `emit_match_arm`: per arm,
>     pattern-test → bind → guard-test → body | next-arm, in source order, no
>     `KTagOf`. The detector `match_has_any_guard(arms) OR not
>     match_has_discriminating_pattern(arms)` diverts to it before the
>     specialised paths (asu review: the O(1) switch is sound only when the
>     tag alone decides the winner; a guard or all-binder arms break that, and
>     the typer — not the switch — owns exhaustivity, so degrading to linear is
>     always sound). Closed fizzbuzz / imc / collatz + attr_unstable_refine_narrow
>     (a `p : Pos`-narrow over a scalar) + int_field_inline.
> selfhost byte-id OK; `make test-kir` GREEN (no golden churned — no
> `examples/kir/*` fixture uses a match guard); ASAN clean on every closed
> crash fixture; ratchet OK (improved), zero parity regressions.
>
> Two DESIGN-bearing causes were diagnosed to a clean cause and documented,
> NOT forced (the brief's "a gap that needs a design decision is not
> improvised") — see the *Design-bearing residue* section:
>   - **Real-arith RC use-after-free** (math_real_basic, free_fall, complex_*,
>     unbox_*). `pcs_ty_is_unboxed` treats `TyReal`/`TyInt` as unboxed, so
>     Perceus inserts NO dup/drop for a Real used >1×; emit_c genuinely unboxes
>     Real to a C `double` (no RC), but the native backend is all-boxed and the
>     shared `kaix_lt`/`kaix_sub` CONSUME their operands → a Real param used
>     twice (`if x < 0.0 { 0.0 - x }`) is double-decref'd → its slab is reused
>     as the next alloc → `type mismatch`. asu verdict: this is the KIR
>     unboxing milestone (native mode-slave + raw Real never entering RC), a
>     dedicated lane, NOT a crash-lane patch (a runtime-side dodge swaps a
>     crash for a leak or re-introduces the pinned second-source-of-RC bug).
>   - **multi/nested same-effect alias-dispatch** (spiral, m7b_15_nested_alias,
>     m8_12_self_delegating_handler, multi_var_state_index). Two `var`/`with …
>     as a` handlers of the same effect (State/Inc) where an op through the
>     OUTER alias is performed from inside the INNER body / a while-body
>     closure. Codegen is CORRECT (per-alias `kaix_evidence_lookup_node_by_id`
>     via the captured `__alias_id__<a>`) and the runtime lookup is correct;
>     the bug is the evidence-frame/alias model across closures-in-loops
>     (the subset-2b clause-param-origin family, refs #622). DESIGN-bearing.
>   - **Hash proto-dispatch** (hashmap/hashset ×4). The native `____proto_hash`
>     emits a panic stub; the oracle does runtime dispatch
>     (`kai_head_tag` + `kai_lookup_impl` + indirect call). The dispatch shims
>     (`kaix_proto_dispatchN`) + lowering were spiked and WORK — but
>     `kai_lookup_impl` returns NULL because the native backend NEVER registers
>     the impl table (`_kai_register_proto_tables` is C-only; native emits no
>     impl-table / variant-to-head startup registration). Building that
>     registration subsystem (materialise an fn-ptr table via the C-API +
>     startup hook, mirror emit_c's `collect_pimpl_rows`/`emit_impl_table_array`)
>     is its own infra lane; the spike was reverted.
>   - **issue_668 map-in-fiber** (nat=138). `list.map` over 40K elements ALONE
>     passes native (TRMC/TCO applies); the crash is only INSIDE the spawned
>     fiber → the no-effect-handler Spawn family (native has no real fiber
>     scheduler runtime). DESIGN-bearing.
>
> The flip to native-default (Lane 1.5) remains **BLOCKED** on the residue
> (json/regex array-decode, rb-tree reuse-token, File/stream handlers, Real
> box/unbox, clause-param-origin, Spawn/NetTcp runtime, proto-dispatch table
> registration). This file is the burn-down input: every failing fixture,
> grouped by root-cause family. The anti-regression ratchet that locks the
> count is `tools/native-parity-baseline.txt` (gated in `tier1-native`).
> NOTE: `list_helpers`/`list_zip3_scan` stay listed (macOS-pass/Linux-SIGSEGV)
> even though they now pass on the macOS dev box — the burn-down 1/2/3 lesson:
> the ratchet is validated on Linux/CI, so a macOS pass is not a closed gap.
>
> **Burn-down 6 (2026-06-12)** closed FOUR root causes (see
> `docs/lane-experience-native-parity-burndown-6.md`):
>   - **EBang (postfix `!`)** was unhandled in `lower_expr` → fell to
>     `lower_unhandled` (silent unit). Added `lower_bang` (kir_lower_walk.kai),
>     a control-flow fork mirroring emit_c's `emit_bang`: test the success tag,
>     `KProj 0` on match, `KRet` (early-return) on miss. Transversal (any
>     `expr!`); unblocked date_basic/date_iso.
>   - **Clock dead-code synth-handler.** Native aborted on a `KPerform Clock`
>     in a dead fn (`today : Date / Clock`) main never installs. Added
>     `kir_synth_handler_effects` — a SUPERSET of `defaults` (every
>     default-block effect, no main_row filter) exposed as
>     `KProgram.synth_defaults`, fed ONLY to the dispatch (`ndef_synth_handlers`);
>     install/teardown stay on `defaults`. Byte-id failure-mode parity with C.
>     Closed date_basic + date_iso.
>   - **nested-variant Form B (variant-slot).** `Node(_, Node(Red, a, ..), ..)`
>     trapped `bind_unsupported_nested`. Three atomic changes:
>     `psub_is_discriminating` admits any `PVariant`, `var_seal_variant` tests
>     the tag then recurses into payload sub-slots, `bind_slot_pattern` recurses
>     the bind. Closed reuse_nested_subpattern.
>   - **nested-variant Form A (list-head record-field-variant).**
>     `[Pair { snd: JReal(r) }]` — `lm_emit_head` (kir_lower_match.kai) tests +
>     binds a nested head. Closed the unbound-register for json_real ×4 (they
>     now diverge on a SEPARATE json-array decode bug, reclassified to
>     regex/json).
> selfhost byte-id OK (after a `PHole(_)` arity fix kaic1 tolerated but selfhost
> caught); `make test-kir` GREEN (goldens unchanged — the simple-head path was
> kept direct, no churn); full-corpus parity ZERO regressions.
>
> **History:** 168 gaps at first measure (2026-06-09). Burn-down 1 closed
> 31 → 137. Burn-down 2 closed 55 (unary-minus aliasing the binary `-` +
> locals-shadow-imports) → 82, then #801/#810 corpus growth + the two
> cross-platform Linux-only SIGSEGV gaps → 91. Burn-down 3 closed 9 (91 →
> 82), shared-tag sub-discrimination — the whole `Duplicate integer as
> switch case` family (18) GONE (9 close, 9 reclassified to regex/json).
> Burn-down 4 NEVER MERGED (stale-golden KIR regression). Burn-down 5
> (2026-06-11) closed 9 cross-platform (82 → 81 NET — main added 8
> corpus-growth gaps during the rebase, listed but out of this lane's zone),
> THREE root causes:
>   - **KIR temp-register / surface-binder namespace collision**
>     (Call-param-mismatch, 5 closed: fx_basic + map_basic/pipes/round_out/
>     tree_basic). `ls_fresh_reg`'s `t<rc>` temporaries aliased surface
>     binders named `t0`/`t1`/`t2` (fx_basic's variables; map.kai's
>     `tree_rotate` binders) → the alloca took the first slot seen (i32 from
>     a `tagof`), a boxed store + i32 load disagreed at the call →
>     `Call parameter type does not match function signature`. Fix:
>     `ls_fresh_reg` acuña `.t<rc>` (`.` is lexer-rejected in a surface
>     ident, so no collision is possible) + `rspec_add` widens to box on a
>     slot conflict (soundness floor). jwt_encoder reclassified (char/hex).
>   - **bit-op prelude shims** (missing-symbols, 2 closed: bits_basic,
>     bits_dotted). The native runtime never defined `kaix_prelude_bit_*`;
>     12 additive shims in runtime_llvm.c reproduce the C oracle's inline
>     operator + refcount exactly. crypto_hash reclassified (flaky SIGSEGV).
>   - **stateful return-clause `state`/`log` bind** (unbound-register
>     writer-state, 2 closed: m7b_11/m7b_14). `lower_handle_ret` never bound
>     the handler's final state for a stateful return clause. Fix: a new
>     `KStmt` `KBindEvState(eff)` the native walk reads from the handle
>     frame's Ev (shared `nemit_bind_ev_state` with the clause prologue).
> All 7 `examples/kir/*.kir.expected` goldens were regenerated (the dump
> changed: `.t` prefix + `bind-ev-state` line) and the `.kir.fns` filter
> re-verified (no stdlib spill — the burn-down-4 failure mode). selfhost
> byte-id OK; 3-way parity + ASAN clean on the closed fixtures.
>
> **Flakiness:** a few native gaps were non-deterministic (intermittent
> SIGSEGV / pointer-bearing output) — e.g. `examples/stdlib/hex_basic.kai`
> diverged ~50% of runs (closed by burn-down 2). Any that remain are
> listed as gaps. The ratchet re-verifies a candidate new gap 3× and only
> flags a regression when it diverges every time, so flaky fixtures never
> fail CI at random.
>
> Reproduce: `TARGET_BACKEND=native ORACLE_BACKEND=c tools/test-backend-parity.sh`
> (needs a kaic2 built with libLLVM — `make -C stage2 KAI_LLVM=1`).

## Root-cause families (82 remaining)

The counts below are re-measured post-burn-down-3 by build/run signature
(`Duplicate integer as switch case` is now **0**). The `output-mismatch`
and `exit-code-mismatch` families GREW as the 9 reclassified regex/json
fixtures surfaced their masked runtime bug.

| Family | Count | Signature / likely cause |
|---|---:|---|
| output-mismatch | 19 | same exit code, divergent stdout. Sub-causes: **pipe-lowering** (`EPipe` lowers to unit, the call vanishes — collatz/euler1/fizzbuzz/imc/capture), **Real box/unbox** (`9.88131e-324` ≈ a raw i64 read as a double — complex/unbox_*), and the **regex/json reclassified** (regex predicate/subsume wrong output, json `\uXXXX` decode `MISMATCH got='1' want='A'` — a char/hex decode the build error masked). |
| exit-code-mismatch / SIGSEGV / timeout | 22 | `c=0, native=1/124/138/139`. Sub-causes: `nat=1` panics (hashmap/hashset `Hash:hash` proto-dispatch todo, math_real `-`/`^`, regex parse `unterminated character class`, `requires violated`), `nat=139` SIGSEGV (quicksort×2, deep recursion, attr_unstable), `nat=124` timeout (spiral, m7b_15/m8_12 nested-alias, multi_var_state), `nat=138` (issue_668 map-in-fiber). Includes the Linux-only `list_helpers`/`list_zip3_scan`. |
| unbound-register | ~1 | **Burn-down 6 closed the nested-variant payload-bearing slice** (Form B variant-slot `Node(_, Node(Red, ..), ..)` via `var_seal_variant`+`bind_slot_pattern`; Form A list-head record-field-variant `[Pair { snd: JReal(r) }]` via `lm_emit_head`). json_real ×4 now BUILD+RUN (diverge on the json-array decode bug, reclassified to regex/json). The rb-tree perceus fixtures (nested_pattern_reuse_balance, llvm_rc_nested_match, llvm_arm_top_reuse_shared, int_field_inline) now BUILD but diverge on the **reuse-token model gap** (see below). Residual: `binserialize_derive_nested` (`Some([x, y])` — a list-pattern inside a variant slot, not yet recursed). |
| reuse-token model (rb-tree) | 4 | (reclassified from unbound-register, burn-down 6) — `nested_pattern_reuse_balance`/`llvm_rc_nested_match`/`llvm_arm_top_reuse_shared`/`int_field_inline` BUILD after the nested-variant fix but diverge (`size: 0` / SIGSEGV): the simple `KConReuse` → `kaix_reuse_or_alloc_variant` eager-decrefs slots 1:1, DOUBLE-FREEING on a non-bijective rb-tree rotation. The C oracle uses the Koka drop-reuse-token protocol; the native EMIT traps `KDropReuse`/`KFreeToken` (`nemit_unsupported`, emit_native_term.kai). The runtime forwarders exist; wiring the emit is the next lane's DESIGN-bearing work. |
| no-effect-handler | 0 | **CLOSED (burn-down 6 + np-handlers).** Burn-down 6 closed the Spawn/Clock/NetTcp slice (actors/mailbox/http_server/date) — those effects carry a default block, so the synth-handler superset (`kir_synth_handler_effects`) gave their dead-code perform a dispatch shape. The np-handlers lane closed the last residual: `ReadFault` (NO default block, dragged into a `from_list`-only stream program via `stdlib/stream.kai`'s dead `pump_lines : … / File + ReadFault`) — `kir_synth_handler_effects` now iterates DECLARED effects (not `install_order`) and synthesises an op→field-index map straight from the `effect` decl for a no-default-block effect, so the dead `KPerform` lowers to the C-direct dispatch shape instead of aborting the build. The whole-corpus no-handler abort count is now 0. `stream_early_stop` itself stays a gap, RECLASSIFIED to **pipe-lowering** (its `|?`/`|`/`|>` lower to no-op unit → `count=0`); the handler abort that masked it is gone. |
| clause-param-origin | 7 | `unsupported KIR node (subset gap): clause param origin` — the subset-2b alias-dispatch clause shape the native walk rejects. Includes `ctor_field_effect_row` (#801) and the remaining `stream_*` (`stream_abort_leak`/`stream_mock_disk`/`stream_skip_policy`) whose carrier hits the alias-dispatch clause. |
| jwt char/hex decode | 1 | (reclassified from Call-param-mismatch) `jwt_encoder` now BUILDS + RUNS after burn-down 5 closed the namespace-collision root cause for the 5 map/fx modules; its decode diverges (char/hex, the regex/json family). |
| crypto flaky SIGSEGV | 1 | (reclassified from missing-symbols) `crypto_hash_basic` LINKS after burn-down 5's `kaix_prelude_bit_*` shims closed bits_basic/bits_dotted; it SIGSEGVs intermittently (a flaky native memory bug). |

> Several fixtures show more than one signature (e.g. `free_fall` shows
> both a `^` type-mismatch and an exit-diff); each is listed once below
> under its primary root cause. The per-family lists below total 73 — the
> set this lane diagnosed. The count column sums to 71 (the macOS dev box's
> failing set); the two `list_*` Linux-only SIGSEGV gaps pass on macOS but
> stay listed (→ 73). The 8 corpus-growth gaps main added during the rebase
> (weather/vs demos, FFI, perceus leak audit) are in
> `tools/native-parity-baseline.txt` (→ 81 total) but are NOT broken out by
> family here — they are pre-existing divergences outside this lane's zone
> (Spawn handler, extern-C symbols, pipe-lowering), for a later burn-down.

## Burn-down 2 root causes (CLOSED — do not re-open)

Both lived in the KIR lowering and were transverse — they hit fixtures
across *every* family, not just one, because the trigger is a surface
pattern (a negative literal; a local name colliding with a stdlib fn)
that appears all over the corpus.

1. **unary minus aliased the binary `-`.** `EUnop("-", a)` lowered to
   `KPrim("-", [a])`, which the native backend routed to the two-arg
   `kaix_sub` — a one-arg call to a two-arg helper, so the second operand
   read an uninitialised register. Every `-5` / `0 - x` diverged with a
   garbage value. Fix: `unop_prim` (kir_lower.kai) maps `-` → `neg`, the
   native `nprim_op_sym` maps `neg` → `kaix_neg` (the one-arg helper,
   mirror of emit_c's `kai_op_neg`).

2. **locals-shadow-imports.** `lower_var` consulted the EFn / prelude
   fn-value table BEFORE the local-binder check, so a param / `let` /
   match binder named like a stdlib `pub fn` (`init` vs `list.init`,
   `sum`, `head`, `acc`, …) lowered to a `KClosure` over the stdlib thunk
   instead of a local read. The closure then reached an arithmetic op as
   a non-Int operand. Fix: thread a `locals: [String]` set on `LowerSt`
   (seeded with params/captures at fn entry, extended at every `KLet` /
   `KStore` via `ls_emit`) and check it first in `lower_var` /
   `lower_callee_dispatch` — the #748/#749 locals-shadow-imports rule, the
   KIR mirror of emit_c's `EmitCtx.lcs`.

## Burn-down 3 root cause (CLOSED — do not re-open)

**shared-tag sub-discrimination.** The variant path of `lower_match`
(kir_lower_walk.kai) switches on the scrutinee's top-level tag
(`KTagOf` → `KSwitch`) — the O(1) jump table for the common case (every
arm a distinct constructor). It broke when two arms SHARED a top-level tag
but discriminated on a payload sub-pattern:

    match e { Exited(0) -> A   Exited(_) -> B   Signaled(_) -> C }
    match n { Branch(Red, _) -> A   Branch(Black, _) -> B   Leaf -> C }

`plan_arms` emitted two `KC(Exited_tag, ..)` cases — a duplicate i32 switch
case LLVM rejects (`module verify failed: Duplicate integer as switch
case`) — and even were it accepted the second arm was dead (nothing tested
the payload). This was the largest single native-parity family (18).

Fix (mirror of the oracle's `emit_pat_test_variant_subs`, WITHOUT losing
the switch):

1. `plan_arms` (kir_lower_walk.kai) now emits ONE switch case per DISTINCT
   tag (`kcase_has_tag` dedup), in first-appearance order. A unique tag
   keeps its bare O(1) switch jump.
2. A tag SHARED by N>1 arms routes its case to a GROUP block holding a
   sub-fail-chain (`lower_variant_groups` / `lower_group_chain`): each arm
   tests its payload (`var_emit_subtests`, kir_lower_variant.kai), branching
   to its body on match / the next arm on mismatch; the tag's catch-all arm
   (`Exited(_)`) ends the chain unconditionally (source order guarantees it
   comes last — the typer rejects an unreachable arm). The chain's final
   fail routes to the switch `default` (`ldef`), which carries the global
   `_` body or is `KUnreachable` when the match is exhaustive.
3. The per-slot test: a LITERAL slot → `kai_eq_raw (KProj scr i) lit`; a
   NULLARY ctor / enum slot → `kai_tag_eq (KProj scr i) ctor_tag`, an
   additive runtime shim (`kaix_tag_eq`, runtime_llvm.c) that compares ONLY
   the immediate variant_tag (never recurses into fields nor routes through
   a custom `Eq` impl — that would panic on a proto-dispatch-todo type).
4. `bind_slot_pattern` (kir_lower_bind.kai) now treats a NULLARY nested
   ctor (`Red`) as a bind no-op (it carries no inner name, only a tag the
   sub-chain tested) instead of trapping `bind_unsupported_nested`. A
   payload-bearing nested ctor (`Some(Node(x))`) still traps loud — the
   remaining nested-variant-test gap.

Soundness gates: parity vs C-direct byte-id on the 9 closed fixtures +
3 minimal fixtures (literal-slot, enum-slot, multi-literal pick-correct-arm)
+ selfhost byte-id + tier0 + zero parity regressions across the full corpus.

## Diagnosed for burn-down 4

- **regex/json reclassified (burn-down 3 unmasked).** The 9
  `Duplicate integer as switch case` fixtures that now build+run but diverge
  were hypothesised to share a char/hex-decode root. That hypothesis was
  **half right** and is now resolved into TWO distinct causes:
  - **char/hex escape decode — CLOSED (2026-06-12, np-decode lane).** The
    native backend decodes string-literal escapes in
    `kai_llvm_build_string_span` (stage2/runtime.h), and its hand-rolled
    switch dropped `\a \b \f \v`, hex `\xHH`, and octal `\ooo`, so the JSON
    byte-table (`"\x01..\xff"`) and every literal carrying such an escape was
    corrupted — `json_basic` `dec esc 3` decoded `A` to `'1'` not `'A'`
    (the `string_slice(byte_table, …)` landed on a garbage byte). Now decodes
    C99-exactly. Closed `hex_escape_literal` + `json_surrogate_decode`.
  - **json-array nested-variant decode — STILL OPEN, NOT char/hex.** With the
    escape fix in, `json_basic` / `json_real_*` / `json_surrogate_encode` /
    `jwt_encoder` STILL diverge: a non-empty array/object decode (`[1]`,
    `{"a":1}`) returns `None` under native. The `json_loop` driver
    (json.kai) pushes `JFrame` variants with payload (`ArrFrame([JsonValue])`,
    `ObjFrame([…], Option[String])`), conses them with list spreads
    (`[ArrFrame(acc1), ...rest]`), and matches the top frame with nested
    variant + list sub-patterns — the same nested-variant / list-match KIR
    lowering family burn-down 6 partly closed (residual
    `binserialize_derive_nested`). `[]` / `{}` (empty) decode fine; only
    non-empty containers fail. This is a list-in-variant-slot decision-tree
    gap, not a char read.
  - **regex matcher logic — STILL OPEN, NOT char/hex.** `regex_basic` /
    `regex_anchors_repetition` panic `unterminated character class` and
    `regex_subsume_*` / `regex_predicate_basic` produce wrong output (e.g.
    `slug`/`other` classification inverted) — the regex engine's matching
    logic diverges under native, independent of the (now-fixed) literal
    decode. Diagnose the matcher's decision-tree lowering, not the char read.
- **Call-param-mismatch** (6: map_basic/map_pipes/map_round_out/map_tree_basic
  + jwt_encoder + fx_basic). `module verify failed: Call parameter type does
  not match function signature!` — one root cause: a generated call passes an
  operand whose LLVM type disagrees with the callee. Likely a monomorph /
  boxing shape (Map's comparator closure?) the native walk types wrong.
- **pipe-lowering** (euler1, fizzbuzz×2, capture, imc, `stream_early_stop`).
  `stream_early_stop` joined this family from no-effect-handler: once the
  np-handlers lane stopped the dead-`ReadFault`-perform build abort, the
  fixture builds + runs but prints `count=0` — `from_list(...) |> count`
  pipes through `EPipe`/`|?`/`|`, all of which lower to no-op unit, so the
  carrier is never driven. `count(from_list(...))` (no pipe) is correct.
  `EPipe` is NOT
  desugared to `ECall` by any pre-codegen pass: the typer (`infer.kai`
  `synth_pipe`) rebuilds `EPipe(lhs, ECall(f, args))`, leaving each emitter
  its own pipe desugaring (emit_c has `emit_pipe_expr`). The native KIR
  lowering has none — `EPipe` lowers to a no-op `KUnitV`, so the call
  vanishes and the output is empty/wrong. A first cut (rewrite to
  `ECall(f, [lhs, ...args])` + delegate to `lower_call`) closed the
  single-candidate + `each(print)` cases but TRAPPED the multi-candidate
  combinator path (`filter`/`map`/`reduce`): the typer resolves those to an
  `EModCall` callee whose `ECall` arg list reaches `lower_exprs` in a shape
  it rejects (`non-exhaustive match` at build — a corrupt `args` whose
  source is upstream of the KIR). Shipping the partial cut trades an
  output-mismatch for a louder build panic with no parity gain, so it was
  reverted; the multi-candidate arg shape needs its own diagnosis first.
- **Real box/unbox** (unbox_bench_real, unbox_phase2_cond_real,
  complex_basic, complex_heterogeneous) — **CLOSED 2026-06-13.** The root
  was NOT a slot reaching an op in the wrong shape: the KIR was all-boxed
  (`slot_of_ty`→SBoxed) and IGNORED the unbox pass's `.mode`, re-boxing every
  Real and lowering `a*7.0` to the CONSUMING `kaix_mul`, but it inherited
  perceus's RC-plan, which SKIPS the dup/drop of unbox-promoted-raw vars
  (`prc_pat_bindings_skip_raw`). A multi-use raw Real thus double-freed; the
  reused slab read as a `double` is the `9.88131e-324`. Fix (asu, Camino A):
  the KIR is MODE-SLAVE like the C emitter — a `MUnboxed` Real lowers to a
  raw `f64` register (`SReal`) + native `fadd`/`fmul`/`fcmp`
  (`kir_lower_raw.kai` + `llvm_build_fbinop`/`fcmp`/`logical`), boxing only
  at the raw↔boxed border (`kaix_real` box-on-read / `kaix_real_field`
  unbox-borrow). No RC on a raw value → no double-free. See
  `docs/lane-experience-np-real.md`.
- **`^` on Real** (free_fall) — math_real_basic CLOSED 2026-06-13 by the same
  raw-Real lane (its raw base reached `kaix_pow_int` correctly once the
  operand was raw). `free_fall` (a package fixture) still listed.
- **stream effect handlers** (#801 — 4 `stream_*` + `demos/wc.kai`). The
  lazy `Stream` line-IO carrier performs `File` / `ReadFault` ops the
  native walk installs no handler for. The np-handlers lane closed the
  no-default-block ABORT half of this (the build no longer aborts on a dead
  `ReadFault` perform); what remains is the per-fixture divergence: the
  `stream_*` carriers reach the **clause-param-origin** subset, and
  `stream_early_stop` reaches **pipe-lowering** (`count=0`).
- **nested-variant-test unbound-register** (json `r`/`n`, binserialize
  nested, writer-state) — carried over from burn-down 1's follow-ups; a
  nested variant/list sub-pattern in a match arm needs a tag/length TEST
  in the decision tree (oracle = emit_c's `emit_pat_test_list`).
- **clause-param-origin**, **missing-symbols** (bits / crypto),
  **build-failed-other** (regex / json / map / http modules) — each its own
  subset, as before. (**no-effect-handler is CLOSED** — burn-down 6 +
  np-handlers; see the family table above.)

## Failing fixtures by family

> Re-measured post-burn-down-3 (82 total, matching the ratchet). The
> `Duplicate integer as switch case` family is gone; the 9 reclassified
> regex/json fixtures now sit under `output-mismatch` / `exit-sigsegv-timeout`.

# output-mismatch — pipe-lowering + reclassified regex/json (15)
# [Real box/unbox CLOSED 2026-06-13: unbox_bench_real / unbox_phase2_cond_real
#  / complex_basic / complex_heterogeneous — see docs/lane-experience-np-real.md]
demos/collatz/main.kai
demos/euler1/main.kai
demos/factorials/main.kai
demos/fizzbuzz/main.kai
demos/imc/main.kai
examples/minimal/capture.kai
examples/minimal/fizzbuzz.kai
examples/stdlib/hex_escape_literal.kai
examples/stdlib/json_basic.kai
examples/stdlib/json_surrogate_decode.kai
examples/stdlib/json_surrogate_encode.kai
examples/stdlib/regex_predicate_basic.kai
examples/stdlib/regex_subsume_alpha.kai
examples/stdlib/regex_subsume_basic.kai
examples/stdlib/string_lines_chars.kai
# exit-code-mismatch / SIGSEGV / timeout (22)
demos/forth/main.kai
demos/free_fall/main.kai
demos/quicksort/main.kai
demos/spiral/main.kai
demos/wc.kai
examples/attributes/attr_unstable_refine_narrow.kai
examples/effects/issue_668_map_large_in_fiber.kai
examples/effects/m7a_6d_double_resume.kai
examples/effects/m7b_15_nested_alias.kai
examples/effects/m8_12_self_delegating_handler.kai
examples/effects/multi_var_state_index.kai
examples/minimal/quicksort.kai
examples/perceus/reuse_record_basic.kai
examples/perceus/scalar_fn_sig_deep_recursion.kai
examples/stdlib/hashmap_basic.kai
examples/stdlib/hashmap_collision.kai
examples/stdlib/hashset_basic.kai
examples/stdlib/hashset_ops.kai
# math_real_basic CLOSED 2026-06-13 (raw-Real lane, ^-on-Real)
examples/stdlib/regex_anchors_repetition.kai
examples/stdlib/regex_basic.kai
examples/stdlib/regex_subsume_unsupported.kai
# unbound-register — nested-variant-test (payload-bearing) (10)
# [burn-down 5 closed the writer-state slice: m7b_11_writer_basic +
#  m7b_14_writer_helper via the KBindEvState stateful return-clause bind]
examples/perceus/int_field_inline.kai
examples/perceus/llvm_arm_top_reuse_shared.kai
examples/perceus/llvm_rc_nested_match.kai
examples/perceus/nested_pattern_reuse_balance.kai
examples/perceus/reuse_nested_subpattern.kai
examples/stdlib/binserialize_derive_nested.kai
examples/stdlib/json_real_decimal.kai
examples/stdlib/json_real_int_regression.kai
examples/stdlib/json_real_negative.kai
examples/stdlib/json_real_scientific.kai
# no-effect-handler — CLOSED (burn-down 6 + np-handlers). The Spawn/Clock/
# NetTcp slice (dual_actor ×3, m8_7_actor_self_send, m8_8_mailbox ×2,
# nested_lambda_with_mailbox, date_basic, date_iso, http_server_book_ch17)
# all pass parity after burn-down 6's synth-handler superset; np-handlers
# extended that superset to no-default-block effects, closing the last
# whole-corpus no-handler abort (ReadFault). `stream_early_stop` is the only
# residual fixture and is now listed under pipe-lowering (its `count=0` is
# the EPipe→unit gap, not a missing handler).
# clause-param-origin — subset-2b alias dispatch (7)
examples/effects/ctor_field_effect_row.kai
examples/effects/net_dns_resolve.kai
examples/effects/r9_clause_capture.kai
examples/effects/stream_abort_leak.kai
examples/effects/stream_mock_disk.kai
examples/effects/stream_skip_policy.kai
examples/packages/cross_package_effects/consumer/main.kai
# jwt char/hex decode (reclassified from Call-param-mismatch) (1)
# [burn-down 5 closed the Call-param-mismatch root cause — namespace
#  collision — for fx_basic + map_basic/map_pipes/map_round_out/
#  map_tree_basic; jwt_encoder now builds+runs but its decode diverges
#  (char/hex, the regex/json family)]
examples/stdlib/jwt_encoder.kai
# crypto flaky SIGSEGV (reclassified from missing-symbols) (1)
# [burn-down 5 added the kaix_prelude_bit_* shims, closing bits_basic +
#  bits_dotted; crypto_hash_basic now links but SIGSEGVs intermittently]
examples/stdlib/crypto_hash_basic.kai
# cross-platform Linux-only SIGSEGV (pass on macOS) (2)
examples/stdlib/list_helpers.kai
examples/stdlib/list_zip3_scan.kai
