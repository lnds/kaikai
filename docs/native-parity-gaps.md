# Native-backend parity gaps — Lane 1.5 burn-down list

> **Status (2026-06-10, after burn-down 2 + #801/#810 corpus growth):**
> the in-process libLLVM native backend (`--backend=native`,
> docs/kir-design.md §7.2) is at **342 pass / 91 fail / 96 skip** over 528
> fixtures vs the C-direct oracle on Linux/CI (~80% corpus parity, up from
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
> `lower_var`; see the retro) → 82. Issue #801 (stream carrier, PR #809)
> then ADDED 6 new corpus fixtures as gaps (NOT a regression — new programs
> the native walk does not yet cover): 4 `stream_*` (no-effect-handler on
> `File`/`ReadFault`), `demos/wc.kai` (exit-code-mismatch, same root cause
> at runtime), `ctor_field_effect_row` (clause-param-origin). → 88. Issue
> #810 (`#[unstable]` transparency) then added `attr_unstable_refine_narrow`
> (native SIGSEGV, C clean — a backend gap, not a regression). → 89. Finally,
> `list_helpers` + `list_zip3_scan` PASS native on macOS but SIGSEGV on
> Linux/CI (the merge oracle), so they stay listed despite closing locally
> in burn-down 2 — the cross-platform lesson burn-down 1's retro flagged.
> → 91.
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

## Root-cause families (91 remaining)

| Family | Count | Signature / likely cause |
|---|---:|---|
| build-failed-other | 25 | native build fails with a signature not in the buckets below (regex / json / map / http — large stdlib modules that pull in an unhandled lowering shape). |
| no-effect-handler | 14 | `KPerform: no handler for effect <Spawn/Clock/NetTcp/File/ReadFault>` — effect ops the native walk installs no handler for (Spawn 7, Clock 2, NetTcp 1, + #801's `File`/`ReadFault` line-stream stages: `stream_early_stop`, `stream_mock_disk`, `stream_skip_policy`, `stream_abort_leak`). |
| exit-code-mismatch / SIGSEGV | 18 | `c=0, native=1` (or 138/139 — a crash counts as a non-zero exit): native exits non-zero / crashes where C succeeds (hashmap/hashset, quicksort, deep recursion). Includes `demos/wc.kai` (#801 `read_lines` over `File`+`ReadFault`, no native handler → diverges on exit), `attr_unstable_refine_narrow` (#810 — native SIGSEGV), and `list_helpers` / `list_zip3_scan` (native SIGSEGV on Linux only — pass on macOS). |
| unbound-register | 13 | `native: unsupported KIR node (subset gap): unbound register <r>` — the residual nested-variant-test / writer-state slice. The mechanical binds closed in burn-down 1; these need a nested decision tree. |
| output-mismatch | 13 | same exit code, divergent stdout. Two sub-causes: **pipe-lowering** (`EPipe` lowers to unit, the call vanishes) and **Real-arithmetic** (`9.88131e-324` ≈ a raw i64 read as a double). See below. |
| missing-symbols | 3 | `Undefined symbols for architecture arm64` at link — `bits` / `crypto_hash` runtime entries the native object references but never defines. |
| clause-param-origin | 3 | `unsupported KIR node (subset gap): clause param origin` — the subset-2b alias-dispatch clause shape the native walk rejects. Includes `ctor_field_effect_row` (#801: a ctor field with an effect row hits the alias-dispatch clause-param gap). |
| type-mismatch (`^`) | 2 | `kai: type mismatch in ^` at runtime — `Real ^ Int` (free_fall / math_real_basic): the base reaches `kaix_pow_int` boxed wrong. Distinct from the burn-down-2 unary/locals causes. |

> Several fixtures show more than one signature (e.g. `free_fall` and
> `math_real_basic` show both a `^` type-mismatch and an exit-diff); each
> is listed once below under its primary root cause. The per-family lists
> below total 91 (matching `tools/native-parity-baseline.txt`); the count
> column above sums to 91.

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

## Diagnosed for burn-down 3

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

# build-failed-other (25)
demos/spiral/main.kai
examples/effects/process_basic.kai
examples/perceus/enum_slot_repr.kai
examples/perceus/next_tier_rc_fix.kai
examples/stdlib/fx_basic.kai
examples/stdlib/http_redirect_follow_basic.kai
examples/stdlib/http_redirect_policy_custom.kai
examples/stdlib/http_redirect_too_many.kai
examples/stdlib/http_server_basic.kai
examples/stdlib/json_basic.kai
examples/stdlib/json_real_invalid.kai
examples/stdlib/json_surrogate_decode.kai
examples/stdlib/json_surrogate_encode.kai
examples/stdlib/json_surrogate_invalid.kai
examples/stdlib/jwt_encoder.kai
examples/stdlib/map_basic.kai
examples/stdlib/map_pipes/main.kai
examples/stdlib/map_round_out.kai
examples/stdlib/map_tree_basic.kai
examples/stdlib/regex_anchors_repetition.kai
examples/stdlib/regex_basic.kai
examples/stdlib/regex_predicate_basic.kai
examples/stdlib/regex_subsume_alpha.kai
examples/stdlib/regex_subsume_basic.kai
examples/stdlib/regex_subsume_unsupported.kai
# no-effect-handler — Spawn 7 / Clock 2 / NetTcp 1 / File-ReadFault 4 (14)
examples/actors/dual_actor_receive_only.kai
examples/actors/dual_actor_request_reply.kai
examples/actors/dual_actor_send_only.kai
examples/effects/m8_7_actor_self_send.kai
examples/effects/m8_8_mailbox_drop_newest.kai
examples/effects/m8_8_mailbox_drop_oldest.kai
examples/effects/stream_abort_leak.kai
examples/effects/stream_early_stop.kai
examples/effects/stream_mock_disk.kai
examples/effects/stream_skip_policy.kai
examples/llvm/nested_lambda_with_mailbox.kai
examples/stdlib/date_basic.kai
examples/stdlib/date_iso.kai
examples/stdlib/http_server_book_ch17.kai
# exit-code-mismatch / SIGSEGV (18)
demos/forth/main.kai
demos/quicksort/main.kai
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
examples/stdlib/list_helpers.kai
examples/stdlib/list_zip3_scan.kai
# unbound-register — nested-variant-test slice (13)
examples/effects/m7b_11_writer_basic.kai
examples/effects/m7b_14_writer_helper.kai
examples/effects/net_dns_resolve.kai
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
# output-mismatch — pipe-lowering + Real box/unbox (13)
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
examples/stdlib/string_lines_chars.kai
# missing-symbols (3)
examples/stdlib/bits_basic.kai
examples/stdlib/bits_dotted.kai
examples/stdlib/crypto_hash_basic.kai
# clause-param-origin — subset-2b alias dispatch (3)
examples/effects/ctor_field_effect_row.kai
examples/effects/r9_clause_capture.kai
examples/packages/cross_package_effects/consumer/main.kai
# type-mismatch `^` on Real (2)
demos/free_fall/main.kai
examples/stdlib/math_real_basic.kai
