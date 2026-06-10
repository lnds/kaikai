# Native-backend parity gaps — Lane 1.5 burn-down list

> **Status (2026-06-09):** the in-process libLLVM native backend
> (`--backend=native`, docs/kir-design.md §7.2) is at **~250 pass / 168
> fail / 96 skip** over 513 fixtures vs the C-direct oracle — ~60% corpus
> parity. The flip to native-default (Lane 1.5) is **BLOCKED** on closing
> these gaps. This file is the burn-down input: every failing fixture,
> grouped by root-cause family. The anti-regression ratchet that locks the
> count is `tools/native-parity-baseline.txt` (gated in `tier1-native`).
>
> **Flakiness:** a few native gaps are non-deterministic (intermittent
> SIGSEGV / pointer-bearing output) — e.g. `examples/stdlib/hex_basic.kai`
> diverges ~50% of runs. Intermittent parity is not parity, so they are
> listed as gaps. The table below counts the 167 deterministic gaps from a
> single run; `hex_basic` brings the baseline to 168. The ratchet
> re-verifies a candidate new gap 3× and only flags a regression when it
> diverges every time, so flaky fixtures never fail CI at random.
>
> Reproduce: `TARGET_BACKEND=native ORACLE_BACKEND=c tools/test-backend-parity.sh`
> (needs a kaic2 built with libLLVM — `make -C stage2 KAI_LLVM=1`).

## Root-cause families

| Family | Count | Signature / likely cause |
|---|---:|---|
| unbound-register | 35 | `native: unsupported KIR node (subset gap): unbound register <r>` — the KIR walk emits a use of a register it never bound (lowering gap). |
| runtime-type-mismatch | 27 | `kai: type mismatch in +/-/>` at runtime — a value reaches an op boxed where raw is expected (or vice-versa); native box/unbox discipline gap. |
| missing-symbols | 24 | `Undefined symbols for architecture arm64` at link — the emitted object references a builtin/runtime symbol the native path never defines. |
| build-failed-other | 24 | native build fails with a signature not in the buckets above. |
| output-mismatch | 20 | same exit code, divergent stdout — codegen produces wrong values/ordering. |
| SIGSEGV/SIGABRT | 12 | `native=138/139` — a crash in emitted code (bad pointer, RC double-free, ABI). |
| exit-code-mismatch | 11 | `c=0, native=1` (or similar) — native exits non-zero where C succeeds. |
| no-effect-handler | 10 | `KPerform: no handler for effect <Spawn/Clock/NetTcp>` — effect ops the native walk does not install handlers for (Spawn 7, Clock 2, NetTcp 1). |
| timeout | 2 | `native=124` — runs forever; a TCO/TRMC sentinel the native lowering misfires on. |
| clause-param-origin | 2 | handler clause param shape the native walk rejects. |

## Failing fixtures by family

# unbound-register (35)
demos/forth/main.kai
examples/attributes/attr_doc_basic.kai
examples/effects/m7b_11_writer_basic.kai
examples/effects/m7b_14_writer_helper.kai
examples/effects/net_dns_resolve.kai
examples/perceus/int_field_inline.kai
examples/perceus/llvm_arm_top_reuse_shared.kai
examples/perceus/llvm_rc_nested_match.kai
examples/perceus/nested_pattern_reuse_balance.kai
examples/perceus/nested_record_pattern.kai
examples/perceus/rc_discipline_record_variant.kai
examples/perceus/reuse_nested_subpattern.kai
examples/stdlib/binserialize_derive_nested.kai
examples/stdlib/hashmap_basic.kai
examples/stdlib/hashset_basic.kai
examples/stdlib/hashset_ops.kai
examples/stdlib/json_real_decimal.kai
examples/stdlib/json_real_int_regression.kai
examples/stdlib/json_real_negative.kai
examples/stdlib/json_real_scientific.kai
examples/stdlib/list_chunk_windows.kai
examples/stdlib/list_flat_zip.kai
examples/stdlib/list_group_find_last_init.kai
examples/stdlib/list_helpers.kai
examples/stdlib/list_intersperse_enumerate.kai
examples/stdlib/list_partition_split_span.kai
examples/stdlib/list_sort.kai
examples/stdlib/list_while_uniq.kai
examples/stdlib/list_zip3_scan.kai
examples/stdlib/math_real_basic.kai
examples/stdlib/real_libm_basic.kai
examples/stdlib/real_rem_basic.kai
examples/stdlib/regex_subsume_alpha.kai
examples/stdlib/regex_subsume_basic.kai
examples/stdlib/regex_subsume_unsupported.kai
# runtime-type-mismatch (27)
demos/free_fall/main.kai
examples/effects/random_shuffle.kai
examples/llvm/driver_smoke.kai
examples/perceus/gap3_tail_rebuild_variant.kai
examples/stdlib/array_basic.kai
examples/stdlib/binserialize_derive_char.kai
examples/stdlib/binserialize_derive_list.kai
examples/stdlib/binserialize_derive_option.kai
examples/stdlib/binserialize_list.kai
examples/stdlib/binserialize_nested.kai
examples/stdlib/binserialize_real.kai
examples/stdlib/binserialize_record.kai
examples/stdlib/binserialize_recursive.kai
examples/stdlib/binserialize_string_escapes.kai
examples/stdlib/binserialize_sum.kai
examples/stdlib/bits_dotted.kai
examples/stdlib/byte_basic.kai
examples/stdlib/fs_file_bytes_llvm.kai
examples/stdlib/fs_file_bytes_roundtrip.kai
examples/stdlib/fx_inverse.kai
examples/stdlib/fx_missing.kai
examples/stdlib/fx_with_money.kai
examples/stdlib/math_int_basic.kai
examples/stdlib/money_basic.kai
examples/stdlib/random_secure_basic.kai
examples/stdlib/random_shuffle_basic.kai
examples/stdlib/uuid_basic.kai
# missing-symbols (24)
demos/collatz/main.kai
demos/factorials/main.kai
examples/effects/cap_read_in_interp.kai
examples/effects/m4c_flow_through.kai
examples/effects/m4c_handler_in_body.kai
examples/effects/m4c_run_with.kai
examples/effects/m7a_6b_handle_runs.kai
examples/effects/m7a_6c_op_dispatch.kai
examples/effects/m7a_6d_resume_check.kai
examples/effects/m7a_6e_discard_resume.kai
examples/effects/m7a_6f_clause_typed.kai
examples/effects/m7b_11_followup_distinct_types.kai
examples/effects/m7b_11_reader_basic.kai
examples/effects/m7b_11_state_basic.kai
examples/effects/m7b_14_reader_helper.kai
examples/effects/m7b_14_state_helper.kai
examples/effects/m7b_15_nested_alias.kai
examples/effects/m7b_2a_op_distinct_types.kai
examples/effects/m7b_2a_op_id_basic.kai
examples/effects/m7b_2a_op_in_parametric.kai
examples/effects/m7b_2b_mutable_default.kai
examples/effects/m7b_2b_mutable_intercept.kai
examples/effects/m8_12_self_delegating_handler.kai
examples/refinements/contracts_passing.kai
# build-failed-other (24)
demos/mini_ledger/main.kai
examples/effects/process_basic.kai
examples/perceus/enum_slot_repr.kai
examples/perceus/next_tier_rc_fix.kai
examples/stdlib/bits_basic.kai
examples/stdlib/crypto_hash_basic.kai
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
# output-mismatch (20)
demos/euler1/main.kai
demos/fizzbuzz/main.kai
demos/imc/main.kai
examples/minimal/capture.kai
examples/minimal/fizzbuzz.kai
examples/packages/stdlib_across_deps/consumer/main.kai
examples/perceus/int_cache_reuse_bump.kai
examples/perceus/region_in_match.kai
examples/perceus/unbox_bench_real.kai
examples/perceus/unbox_phase2_cond_real.kai
examples/stdlib/complex_basic.kai
examples/stdlib/complex_heterogeneous.kai
examples/stdlib/date_epoch.kai
examples/stdlib/decimal_div_basic.kai
examples/stdlib/hex_escape_literal.kai
examples/stdlib/option_ok_or_basic.kai
examples/stdlib/path_basic.kai
examples/stdlib/queue_basic.kai
examples/stdlib/stack_basic.kai
examples/stdlib/string_lines_chars.kai
# SIGSEGV/SIGABRT (12)
demos/net_udp_localhost/main.kai
demos/quicksort/main.kai
examples/effects/issue_668_map_large_in_fiber.kai
examples/effects/net_udp_localhost.kai
examples/minimal/quicksort.kai
examples/perceus/scalar_fn_sig_deep_recursion.kai
examples/stdlib/decimal_basic.kai
examples/stdlib/result_err_basic.kai
examples/stdlib/result_map_err_basic.kai
examples/stdlib/result_or_else_basic.kai
examples/stdlib/result_unwrap_or_else_basic.kai
examples/stdlib/set_basic.kai
# exit-code-mismatch (11)
examples/effects/m7a_6d_double_resume.kai
examples/llvm/m3b.kai
examples/llvm/spawn_actor_in_mailbox.kai
examples/perceus/reuse_record_basic.kai
examples/stdlib/binserialize_derive_sum_collections.kai
examples/stdlib/hashmap_collision.kai
examples/stdlib/int_helpers_basic.kai
examples/stdlib/list_basic.kai
examples/stdlib/list_pipeline.kai
examples/stdlib/poly_ord_containers.kai
examples/stdlib/string_deferred_396.kai
# no-effect-handler(Spawn) (7)
examples/actors/dual_actor_receive_only.kai
examples/actors/dual_actor_request_reply.kai
examples/actors/dual_actor_send_only.kai
examples/effects/m8_7_actor_self_send.kai
examples/effects/m8_8_mailbox_drop_newest.kai
examples/effects/m8_8_mailbox_drop_oldest.kai
examples/llvm/nested_lambda_with_mailbox.kai
# timeout (2)
demos/spiral/main.kai
examples/effects/multi_var_state_index.kai
# clause-param-origin (2)
examples/effects/r9_clause_capture.kai
examples/packages/cross_package_effects/consumer/main.kai
# no-effect-handler(Clock) (2)
examples/stdlib/date_basic.kai
examples/stdlib/date_iso.kai
# no-effect-handler(NetTcp) (1)
examples/stdlib/http_server_book_ch17.kai
