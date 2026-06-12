# Native-backend parity gaps — Lane 1.5 burn-down list

> **Status (2026-06-11, after burn-down 5):**
> the in-process libLLVM native backend (`--backend=native`,
> docs/kir-design.md §7.2) is at **81 fail** on the Linux/CI ratchet (73 this
> lane diagnosed + 8 corpus-growth gaps main added during the rebase — see
> below; macOS dev measures 79). The flip to native-default (Lane 1.5) is
> **BLOCKED** on closing these gaps. This file is the burn-down input: every
> failing fixture, grouped by root-cause family. The anti-regression ratchet
> that locks the count is `tools/native-parity-baseline.txt` (gated in
> `tier1-native`).
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
| unbound-register | 10 | `unsupported KIR node (subset gap): unbound register <r>` — the residual nested-variant-test slice: a nested variant sub-pattern WITH payload (`Some(JReal(r))`) needs a tag TEST + recursive sub-bind. Burn-down 3's `kai_tag_eq` test covers NULLARY nested ctors (enum slots); the payload-bearing nested ctor is the remaining gap (`bind_unsupported_nested`). [Burn-down 5 closed the writer-state `log` slice via `KBindEvState`.] |
| no-effect-handler | 11 | `KPerform: no handler for effect <Spawn/Clock/NetTcp/File>` — effect ops the native walk installs no handler for (Spawn 7, Clock 2, NetTcp 1, File 1: `stream_early_stop`). |
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
  share a likely common root: a char/hex decode in the native path. `regex_basic`
  / `regex_anchors_repetition` panic `unterminated character class` (the regex
  parser's `Some(']')` char-match never fires — though a minimal `Some(']')`
  match passes parity, so the bug is in how the real parser reads the char,
  not the match lowering); `json_basic` decodes `A` to `'1'` not `'A'`
  (a hex-nibble read). `regex_subsume_*` / `regex_predicate_basic` produce
  wrong output. Diagnose the char/string-index read on the native path first.
- **Call-param-mismatch** (6: map_basic/map_pipes/map_round_out/map_tree_basic
  + jwt_encoder + fx_basic). `module verify failed: Call parameter type does
  not match function signature!` — one root cause: a generated call passes an
  operand whose LLVM type disagrees with the callee. Likely a monomorph /
  boxing shape (Map's comparator closure?) the native walk types wrong.
- **pipe-lowering** (euler1, fizzbuzz×2, capture, imc). `EPipe` is NOT
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
  complex_basic, complex_heterogeneous). Native prints `9.88131e-324` (a
  raw i64 bit-pattern read as a `double`) where C prints the real value —
  a Real (`SReal`) slot reaches an op raw where boxed is expected, or
  vice-versa. Same class as the burn-down-2 box discipline, on the Real slot.
- **`^` on Real** (free_fall, math_real_basic). `Real ^ Int` runtime
  type-mismatch; the base reaches `kaix_pow_int` boxed wrong.
- **stream effect handlers** (#801 — 4 `stream_*` + `demos/wc.kai`). The
  lazy `Stream` line-IO carrier performs `File` / `ReadFault` ops the
  native walk installs no handler for. Same no-effect-handler subset as
  Spawn/Clock/NetTcp.
- **nested-variant-test unbound-register** (json `r`/`n`, binserialize
  nested, writer-state) — carried over from burn-down 1's follow-ups; a
  nested variant/list sub-pattern in a match arm needs a tag/length TEST
  in the decision tree (oracle = emit_c's `emit_pat_test_list`).
- **no-effect-handler** (Spawn / Clock / NetTcp), **clause-param-origin**,
  **missing-symbols** (bits / crypto), **build-failed-other** (regex /
  json / map / http modules) — each its own subset, as before.

## Failing fixtures by family

> Re-measured post-burn-down-3 (82 total, matching the ratchet). The
> `Duplicate integer as switch case` family is gone; the 9 reclassified
> regex/json fixtures now sit under `output-mismatch` / `exit-sigsegv-timeout`.

# output-mismatch — pipe-lowering + Real box/unbox + reclassified regex/json (19)
demos/collatz/main.kai
demos/euler1/main.kai
demos/factorials/main.kai
demos/fizzbuzz/main.kai
demos/imc/main.kai
examples/minimal/capture.kai
examples/minimal/fizzbuzz.kai
examples/perceus/unbox_bench_real.kai
examples/perceus/unbox_phase2_cond_real.kai
examples/stdlib/complex_basic.kai
examples/stdlib/complex_heterogeneous.kai
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
examples/stdlib/math_real_basic.kai
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
# no-effect-handler — Spawn 7 / Clock 2 / NetTcp 1 / File 1 (11)
examples/actors/dual_actor_receive_only.kai
examples/actors/dual_actor_request_reply.kai
examples/actors/dual_actor_send_only.kai
examples/effects/m8_7_actor_self_send.kai
examples/effects/m8_8_mailbox_drop_newest.kai
examples/effects/m8_8_mailbox_drop_oldest.kai
examples/effects/stream_early_stop.kai
examples/llvm/nested_lambda_with_mailbox.kai
examples/stdlib/date_basic.kai
examples/stdlib/date_iso.kai
examples/stdlib/http_server_book_ch17.kai
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
