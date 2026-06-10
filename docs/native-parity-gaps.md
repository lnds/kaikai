# Native-backend parity gaps — Lane 1.5 burn-down list

> **Status (2026-06-10, after burn-down 1):** the in-process libLLVM native
> backend (`--backend=native`, docs/kir-design.md §7.2) is at **284 pass /
> 137 fail / 96 skip** over 517 fixtures vs the C-direct oracle. Burn-down 1
> closed **31** gaps (168 → 137): the whole `missing-symbols` family (24 —
> one `kai_assert_check` lowering bug) plus a slice of `unbound-register`
> (const / prelude-fn-value inline, nested record sub-pattern binds,
> default-arm binders) plus `mini_ledger` (build-failed-other, closed as a
> side effect of the assert fix). The flip to native-default (Lane 1.5) is
> **BLOCKED** on closing the rest. This file is the burn-down input: every
> failing fixture, grouped by root-cause family. The anti-regression ratchet
> that locks the count is `tools/native-parity-baseline.txt` (gated in
> `tier1-native`).
>
> **Corpus growth (2026-06-10, issue #801 — stream carrier):** the lazy
> `Stream` lane added 6 new fixtures the native walk does not yet cover —
> **143 fail** now, not a regression. They fall into existing families:
> `stream_early_stop` / `stream_mock_disk` / `stream_skip_policy` /
> `stream_abort_leak` are `no-effect-handler` (`File`/`ReadFault` ops the
> native walk has no handler for); `demos/wc.kai` is `exit-code-mismatch`
> (same root cause, surfaced at runtime instead of build); `ctor_field_effect_row`
> is `clause-param-origin` (subset 2b alias dispatch). All six compile and
> run correctly under the C-direct oracle — they are backend gaps, not
> source bugs. Note: `stream_shout_roundtrip` (the read→map→write_lines
> gallery program) already PASSES native and is intentionally not listed.
>
> **Burn-down 1 lesson (see the retro):** closing a mechanical bug UNMASKS
> the next bug on the same fixture. The unbound-register slice that did NOT
> close mostly advanced to *other* families now visible behind the abort —
> `nested-variant-test` (a nested variant/list sub-pattern in a match arm
> needs a tag/length TEST the bind cannot emit) and `switch-duplicate`
> (variant arms sharing a top tag but discriminating on a nested
> sub-variant). Both are control-flow lowering, not bind gaps; the binder
> lowering traps them loud (`bind_unsupported_nested`) rather than binding
> against an untested value (a false-green). They are the next lane's work.
>
> **Flakiness:** a few native gaps are non-deterministic (intermittent
> SIGSEGV / pointer-bearing output) — e.g. `examples/stdlib/hex_basic.kai`
> diverges ~50% of runs. Intermittent parity is not parity, so they stay
> listed as gaps. The ratchet re-verifies a candidate new gap 3× and only
> flags a regression when it diverges every time, so flaky fixtures never
> fail CI at random.
>
> Reproduce: `TARGET_BACKEND=native ORACLE_BACKEND=c tools/test-backend-parity.sh`
> (needs a kaic2 built with libLLVM — `make -C stage2 KAI_LLVM=1`).

## Root-cause families

> **Burn-down 1 (2026-06-10) reshaped the families.** `missing-symbols`
> went to **0** (one `kai_assert_check` lowering fix). The
> `unbound-register` family split: the mechanical binds closed; the residue
> moved to two control-flow families that were *masked* behind the abort —
> `nested-variant-test` and `switch-duplicate`. Several closed-link fixtures
> then surfaced as `output-mismatch`. The counts below are post-burn-down-1;
> the exact 137-fixture set is `tools/native-parity-baseline.txt`.

| Family | Count | Signature / likely cause |
|---|---:|---|
| runtime-type-mismatch | 27 | `kai: type mismatch in +/-/>` at runtime — a value reaches an op boxed where raw is expected (or vice-versa); native box/unbox discipline gap. |
| build-failed-other | ~23 | native build fails with a signature not in the buckets above. |
| output-mismatch | ~24 | same exit code, divergent stdout — codegen produces wrong values/ordering. Grew in burn-down 1: fixtures that link now (assert fix) but compute wrong (collatz, factorials, …). |
| SIGSEGV/SIGABRT | ~12 | `native=138/139` — a crash in emitted code (bad pointer, RC double-free, ABI). |
| exit-code-mismatch | ~12 | `c=0, native=1` (or similar) — native exits non-zero where C succeeds. Includes `demos/wc.kai` (#801: `read_lines` over `File + ReadFault`, no native handler → diverges on exit). |
| no-effect-handler | 14 | `KPerform: no handler for effect <Spawn/Clock/NetTcp/File/ReadFault>` — effect ops the native walk does not install handlers for (Spawn 7, Clock 2, NetTcp 1, + #801's `File`/`ReadFault` line-stream stages: `stream_early_stop`, `stream_mock_disk`, `stream_skip_policy`, `stream_abort_leak`). |
| switch-duplicate | ~7 | `native module verify failed: Duplicate integer as switch case` — variant arms sharing a top tag but discriminating on a NESTED sub-variant (`Node(_, Node(R,..), ..)`, regex `subsume_*`) need a nested decision tree, not one `KSwitch`. Unmasked by burn-down 1's binder fix. |
| nested-variant-test | ~5 | `unbound register` (loud trap) — a nested variant/list sub-pattern in a match arm (`Some(JReal(r))`, `Some([x,y])`) needs a tag/length TEST the bind lowering deliberately does NOT emit (`bind_unsupported_nested`; oracle = emit_c `emit_pat_test_list`). |
| unbound-register (handler-state) | 2 | `unbound register log` — a handler's state free name (`Writer` `tell`/`return`) is not bound on the native walk. Effects family. |
| timeout | 2 | `native=124` — runs forever; a TCO/TRMC sentinel the native lowering misfires on. |
| clause-param-origin | 3 | handler clause param shape the native walk rejects (subset 2b alias dispatch). Includes `ctor_field_effect_row` (#801 typer-regression fixture — a ctor field with an effect row; the native walk hits the alias-dispatch clause-param gap). |
| multi-scrutinee | 1 | `match a, b` desugars to nested EMatch with duplicated fall-through arms; native lowering diverges (`demos/forth`). |

## Failing fixtures

The authoritative failing set is `tools/native-parity-baseline.txt` (137
fixtures, one per line). It is the ratchet input, so it never drifts from
reality: the gate fails if any listed fixture starts passing without being
removed, or any unlisted fixture starts failing.

## Closed by burn-down 1 (2026-06-10)

The 31 fixtures below moved from the baseline to passing. Their root-cause
fixes live in `stage2/compiler/kir_lower_walk.kai` (assert lowering,
`lower_var` const/prelude cascade, `lower_let_bind`, `lower_default_arm`),
`stage2/compiler/kir_lower_bind.kai` (nested record sub-pattern binds), and
`stage2/compiler/emit_native_ops.kai` (`nemit_assert_check`).

### missing-symbols → 0 (the `kai_assert_check` lowering fix)
All 24 `missing-symbols` fixtures now LINK (the `kaix_assert` symbol is
gone). 19 of them close outright (link + parity vs C-direct, listed below);
the other 5 (`demos/collatz`, `demos/factorials`,
`examples/stdlib/{hashmap_basic,hashset_basic,hashset_ops}` and friends)
link but then DIVERGE — a pre-existing output-mismatch the link error hid —
so they stay gaps under a different family. The link-and-parity closures
(plus `mini_ledger`, a build-failed-other fixture the assert fix also
closed):

```
examples/effects/cap_read_in_interp.kai
examples/effects/m4c_flow_through.kai
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
examples/effects/m7b_2a_op_distinct_types.kai
examples/effects/m7b_2a_op_id_basic.kai
examples/effects/m7b_2a_op_in_parametric.kai
examples/effects/m7b_2b_mutable_default.kai
examples/effects/m7b_2b_mutable_intercept.kai
examples/refinements/contracts_passing.kai
demos/mini_ledger/main.kai
```

### unbound-register slice (const / prelude-fn-value / nested record / default-arm)
```
examples/attributes/attr_doc_basic.kai
examples/perceus/nested_record_pattern.kai
examples/perceus/rc_discipline_record_variant.kai
examples/stdlib/list_chunk_windows.kai
examples/stdlib/list_flat_zip.kai
examples/stdlib/list_group_find_last_init.kai
examples/stdlib/list_intersperse_enumerate.kai
examples/stdlib/list_partition_split_span.kai
examples/stdlib/list_sort.kai
examples/stdlib/list_while_uniq.kai
examples/stdlib/real_libm_basic.kai
examples/stdlib/real_rem_basic.kai
```

## Notable residue with identified root cause (next lanes)

- `examples/stdlib/json_real_{decimal,int_regression,negative,scientific}.kai`,
  `examples/stdlib/binserialize_derive_nested.kai` — **nested-variant-test**:
  a nested variant (`Some(JReal(r))`) / list (`Some([x,y])`) sub-pattern in
  a match arm needs a tag/length TEST. The binder lowering traps these loud
  (`bind_unsupported_nested`) so they fail the build honestly rather than
  binding against an untested value (the false-green a naïve recursion would
  produce — `reuse_nested_subpattern` demonstrated it).
- `examples/perceus/{int_field_inline,llvm_arm_top_reuse_shared,llvm_rc_nested_match,nested_pattern_reuse_balance}.kai`,
  `examples/stdlib/regex_subsume_{alpha,basic,unsupported}.kai` —
  **switch-duplicate**: variant arms sharing a top tag, discriminating on a
  nested sub-variant; the `KSwitch` gets duplicate integer cases. Needs a
  nested decision tree under the matched case (same family as above).
- `demos/{collatz,factorials}.kai`, `examples/stdlib/{hashmap_basic,hashset_basic,hashset_ops,list_helpers,list_zip3_scan,math_real_basic}.kai`
  — link now (assert / const / prelude fixes) but **output-mismatch** or
  **SIGSEGV** (`list_helpers` = 138): pre-existing codegen bugs the
  unbound-register abort hid. Separate families.
- `examples/effects/m7b_11_writer_basic.kai`,
  `examples/effects/m7b_14_writer_helper.kai` — `unbound register log`: a
  handler's state free name is not bound on the native walk (effects family).
- `demos/forth/main.kai` — **multi-scrutinee** `match a, b` (parser desugars
  to nested EMatch with duplicated fall-through arms).
