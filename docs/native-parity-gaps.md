# Native-backend parity gaps — Lane 1.5 burn-down list

> **Status (2026-06-10, after burn-down 3):**
> the in-process libLLVM native backend (`--backend=native`,
> docs/kir-design.md §7.2) is at **351 pass / 82 fail / 96 skip** over 529
> fixtures vs the C-direct oracle on Linux/CI (~81% corpus parity, up from
> ~60% at first measure). The flip to native-default (Lane 1.5) is **BLOCKED** on
> closing these gaps. This file is the burn-down input: every failing
> fixture, grouped by root-cause family. The anti-regression ratchet that
> locks the count is `tools/native-parity-baseline.txt` (gated in
> `tier1-native`).
>
> **History:** 168 gaps at first measure (2026-06-09). Burn-down 1 closed
> 31 (missing-symbols `kai_assert_check` symbol + an unbound-register
> slice) → 137. Burn-down 2 closed 55 (two KIR-lowering root causes —
> unary-minus aliasing the binary `-`, and locals-shadow-imports in
> `lower_var`; see the retro) → 82, then #801/#810 corpus growth + the two
> cross-platform Linux-only SIGSEGV gaps (`list_helpers`, `list_zip3_scan`)
> brought it to 91. Burn-down 3 closed 9 (91 → 82), ONE KIR-lowering root
> cause — **shared-tag sub-discrimination**: a `match` arm sharing a
> top-level tag with a sibling but discriminating on a payload sub-pattern
> (`Exited(0)`/`Exited(_)`, `Branch(Red,_)`/`Branch(Black,_)`) lowered to a
> `KSwitch` with a duplicate i32 case (`Duplicate integer as switch case`,
> LLVM-rejected) and a dead second arm. The fix groups arms by tag and
> opens a payload sub-fail-chain for shared tags (kir_lower_variant.kai; see
> the retro). The whole `Duplicate integer as switch case` family (18) is
> GONE: 9 close; the other 9 now BUILD + RUN but diverge on a PRE-EXISTING
> bug the build error masked (regex char-class parse, json `\uXXXX` decode,
> Call-param-mismatch), reclassified below.
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
| unbound-register | 12 | `unsupported KIR node (subset gap): unbound register <r>` — the residual nested-variant-test slice: a nested variant sub-pattern WITH payload (`Some(JReal(r))`) needs a tag TEST + recursive sub-bind. Burn-down 3's `kai_tag_eq` test covers NULLARY nested ctors (enum slots); the payload-bearing nested ctor is the remaining gap (`bind_unsupported_nested`). Plus the writer-state `log` slice. |
| no-effect-handler | 11 | `KPerform: no handler for effect <Spawn/Clock/NetTcp/File>` — effect ops the native walk installs no handler for (Spawn 7, Clock 2, NetTcp 1, File 1: `stream_early_stop`). |
| clause-param-origin | 7 | `unsupported KIR node (subset gap): clause param origin` — the subset-2b alias-dispatch clause shape the native walk rejects. Includes `ctor_field_effect_row` (#801) and the remaining `stream_*` (`stream_abort_leak`/`stream_mock_disk`/`stream_skip_policy`) whose carrier hits the alias-dispatch clause. |
| Call-param-mismatch | 6 | `module verify failed: Call parameter type does not match function signature!` — the `map` modules (map_basic/map_pipes/map_round_out/map_tree_basic) + jwt_encoder + fx_basic: a generated call passes an operand whose LLVM type disagrees with the callee signature (a monomorph/boxing shape the native walk emits wrong). One root cause across all 6. |
| missing-symbols | 3 | `Undefined symbols for architecture arm64` at link — `bits` / `crypto_hash` runtime entries the native object references but never defines. |

> Several fixtures show more than one signature (e.g. `free_fall` shows
> both a `^` type-mismatch and an exit-diff); each is listed once below
> under its primary root cause. The per-family lists below total 82
> (matching `tools/native-parity-baseline.txt`); the count column above
> sums to 82.

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

## Burn-down 4 root cause (CLOSED — do not re-open)

**synthetic-register namespace collision.** `ls_fresh_reg` (kir_lower.kai)
minted virtual-register temps `t0`, `t1`, … in the SAME namespace as
user-source value binders. A program is free to name a `let` binder or a
`match` field binder `t1`/`t2`/`t3`:

    fn tree_rotate_left(t) = match t {
      TNode(_, _, _, t1, rt) -> match rt {
        TNode(_, _, _, t2, t3) -> tree_node(..., t2, t3)   # binders t2/t3
    ...
    let t0 = fx.table_empty()   # fx_basic.kai

In `tree_rotate_left`'s KIR the inner match plants a synthetic
`t2: i32 = tagof rt` (the `KTagOf` switch scrutinee, slot SInt32) under
the SAME name as the user binder `t2: box = proj rt.3` (slot SBoxed). The
native backend's model is one alloca per register name, KSlot the single
type source (`collect_fn_specs` keeps the FIRST slot it sees for a name),
so the two FUSED into one `%t2.addr` alloca typed `i32`. The user binder's
boxed store + the `tree_node(..., t2, ...)` boxed read then disagreed with
the alloca type, and the call passed an `i32` where the callee (every
user fn is declared all-boxed `ptr (ptr × n)`) wanted a `ptr` →
`module verify failed: Call parameter type does not match function
signature`.

Fix (one line, kir_lower.kai): prefix synthetic registers `__t<N>`,
keeping the synthetic namespace DISJOINT from user binders — the same
reserved-synthetic convention the lowering already uses (`__pcs_scr`,
`_kai_acc_res`, `__self`). Native-path-only: the C oracle lowers AST→C
directly (`emit_c`, never `lower_to_kir`), so `ls_fresh_reg` does not run
on the C path and selfhost byte-id is unaffected (verified: kaic2b.c ==
kaic2c.c).

The whole `Call parameter type does not match function signature` family
(6) is gone: 5 close outright (`map_basic`/`map_pipes`/`map_round_out`/
`map_tree_basic` + `fx_basic`); `jwt_encoder` now BUILDS + RUNS but its
base64/JSON decode round-trip DIVERGES (`decode failed`) — a pre-existing
bug the verify error masked, reclassified to output-mismatch. The two
Linux-only SIGSEGV gaps (`list_helpers`/`list_zip3_scan`) PASS native on
macOS post-fix; they stay listed because the ratchet is validated on
Linux/CI and this lane cannot verify there.

Soundness gates: parity byte-id vs C-direct on the 5 closed fixtures +
`examples/minimal/match_binder_name_collision.kai` (a minimal nested
`match` whose binders are named `t1`/`t2`/`t3` alongside `let t0`/`t1`) +
selfhost byte-id + zero parity regressions across the full corpus +
macOS `leaks` no worse than the C oracle (the residual leak is the known
native-RC debt, not introduced — the fixtures did not build natively
before). → 82 → 77 gaps.

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
- **pipe-lowering** (collatz, euler1, fizzbuzz×2, capture, imc). `EPipe` is
  NOT desugared to `ECall` by any pre-codegen pass: the typer (`infer.kai`
  `synth_pipe`) rebuilds `EPipe(lhs, ECall(f, args))`, leaving each emitter
  its own pipe desugaring (emit_c has `emit_pipe_expr`). The native KIR
  lowering has none — `EPipe` lowers to a no-op `KUnitV`, so the call
  vanishes and the output is empty/wrong.

  > **Root cause nailed (burn-down 4, lldb + instrumented trace).** A first
  > cut (rewrite `EPipe(lhs, rhs)` to `ECall(f, [lhs, ...args])` + delegate
  > to `lower_call`) panics `non-exhaustive match` at BUILD for ANY pipe
  > whose RHS call carries non-trivial args — and NOT only the
  > multi-candidate path: `[1,2,3] |> applyf(dbl)` (single-candidate user fn
  > + fn-value arg) panics, while the IDENTICAL non-pipe `applyf([1,2,3],
  > dbl)` builds + runs. The lldb backtrace + a `lower_exprs`/`lower_list`
  > trace pinpoint the panic to lowering the piped **LHS** after the rewrite.
  > The reason (asu): `synth_pipe`'s `ECall(f, args)` holds ONLY the trailing
  > args — the piped LHS is NOT inside it (the emitter prepends it as param 0
  > at codegen). Fusing the LHS into arg-slot-0 and re-dispatching through
  > `lower_call` builds a call whose arg COUNT disagrees with the callee's
  > original-arity signature; a downstream match traps. (The earlier
  > "corrupt args upstream of the KIR / perceus RC marks" framing was wrong —
  > it is the LHS-fusion shape, not displaced RC marks.)
  >
  > **The clean fix** mirrors `emit_pipe_expr` at the KVal-operand level:
  > lower `lhs` + each `arg` to atoms SEPARATELY, then assemble the call op
  > `[lhs_v, ...arg_vs]` directly (per-callee mask-marshalling like emit_c),
  > moving no AST node — perceus's already-placed RC marks stay put.
  > Desugaring `EPipe → ECall` pre-perceus is REJECTED: perceus runs on the
  > AST shared by both backends, so it would change the C-path selfhost
  > byte-id and cannot be gated to the KIR branch alone. Needs a dedicated
  > lane (gate: parity vs C-direct on the 5 fixtures + selfhost byte-id
  > UNCHANGED + ASAN). Reverted in burn-down 4 — the rewrite trades a silent
  > output-mismatch for a noisy build panic with zero parity gain.
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
examples/stdlib/jwt_encoder.kai
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
# unbound-register — nested-variant-test (payload-bearing) + writer-state (12)
examples/effects/m7b_11_writer_basic.kai
examples/effects/m7b_14_writer_helper.kai
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
# Call-param-mismatch — CLOSED by burn-down 4 (synthetic-register
# namespace collision, kir_lower.kai). 5 of 6 close outright; jwt_encoder
# now BUILDS + RUNS but DIVERGES on a pre-existing decode bug (moved to
# output-mismatch above). See "Burn-down 4 root cause" below.
# missing-symbols — bits / crypto (3)
examples/stdlib/bits_basic.kai
examples/stdlib/bits_dotted.kai
examples/stdlib/crypto_hash_basic.kai
# cross-platform Linux-only SIGSEGV (pass on macOS) (2)
examples/stdlib/list_helpers.kai
examples/stdlib/list_zip3_scan.kai
