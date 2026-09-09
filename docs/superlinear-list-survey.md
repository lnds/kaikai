# Superlinear list algorithms in `stage2/compiler/`

Measured on `82879e06`, macOS arm64, C backend, self-compile of
`stage2/main.kai` (~138k LOC of compiler + stdlib).

The hypothesis under test: `bucket_append` is not special. The compiler
threads everything through linked lists, `append` and membership are
both O(n), so any accumulation inside a walk is quadratic by
construction. Raw counts in `stage2/compiler/`: 424 `list_append`, 320
`list_has`, 381 `list_length`, 576 `list_reverse`.

**The hypothesis holds.** `bucket_append` is one of seven confirmed
instances of the same shape, and it is not the largest. But the second
half of the result matters as much: of 388 functions the detector flags,
**7 are defects and 381 are bounded**. The count of `list_append` calls
predicts nothing on its own.

## 1. The detector

`tools/superlinear-survey.py`. A site is superlinear when an O(n) list
operation is re-evaluated once per element of a walk. Three shapes:

| shape | pattern | cost |
|---|---|---|
| `SPINE_COPY_SELF` | `list_append(acc, …)` in a fn that walks or recurses | O(n²) |
| `SPINE_COPY_CALL` | `list_append(p, …)` in a state-threading helper reached from a walk | O(n²) |
| `NESTED_SCAN` | `list_has`/`list_index` over B inside a walk over A | O(\|A\|·\|B\|) |

Two details earn their complexity. The caller search is **transitive**
(three levels): `rs_append_decls` is a straight-line record rebuild whose
loop lives in `resolve_module` ↔ `resolve_imports` mutual recursion, and
a single-level test misses it. And the left operand of `list_append` is
not filtered by name or by being a declared parameter — an early version
required both and lost `bucket_append`, the one site already known.

The detector does **not** classify bounds. That is the analyst's job
(§3), and it is where the signal is.

### Calibration

Against every independently known instance:

| site | status | flagged |
|---|---|---|
| `bucket_append` (`infer.kai`) | known defect, sibling lane owns the fix | yes |
| `pcs_consume_lookup` | already fixed — replaced by `consume_index.kai` | n/a, gone from the tree |
| `efn_collect_matching` | linear itself; the quadratic is its caller `find_ambig_loop` | caller flagged |
| `sv_climb` | bounded by rung count | correctly not flagged |

Recall on the confirmed set (§2) is **8/8**. Precision is low by design:
562 candidate sites across 388 functions, of which 7 are real — about
**1 in 55**. A detector tuned to precision would have missed
`rs_append_decls`; the bound analysis is cheap per candidate and the
inventory below is what stops the next person re-checking them.

## 2. Method: attributing cons cells to kaikai functions

`KAI_TRACE_RC=1` captures `__builtin_return_address(0)` at every
allocation, and the report prints an ASLR slide for static
symbolization. Straight out of the box this is not enough: the two
spine-copying primitives are ordinary C functions, so 30.4M cons
allocations — the single largest block, and exactly the operations this
survey is about — attribute to `kai_core_list_append` and
`kai_core_list_reverse` with no caller information.

The fix is a measurement-only shim: a thread-local `kai_attr_override`
that the two primitives set to their own return address, so cells they
allocate bill to the kaikai function that called them. It is not
committed; it is 12 lines against a scratch copy of `runtime.h`,
rebuilt with `cc` from the pre-generated `stage2/build/kaic2b.c` (no
`make kaic2`).

The instrumented binary is unbiased in the way that matters:
`alloc_total=661,674,085` and `leaked=99,123,525` on both the stock and
the attributing build, and the emitted C is byte-identical.

Complexity is then read off two scaling axes, because the axes disagree
and a single fixture answers the wrong question:

- **decls axis** — N five-line functions in one module, N = 200…3200.
- **modules axis** — M modules of 10 `pub fn` each, M = 20…160.

## 3. Confirmed defects (measured)

Ranked by cost on the real self-compile. "Bound" is the collection whose
growth makes the site quadratic.

| # | site | file | bound | exponent | self-compile cons | share of all allocs |
|---:|---|---|---|---:|---:|---:|
| 1 | `list_minus` / `list_minus_loop` | `emit_shared.kai:1394` | `st.globals` — every global in the program, walked per lambda | 1.05 / lambda | 7,539,728 | 1.14% |
| 2 | `partition_decls_by_home` (= `bucket_append`) | `infer.kai:19649` | decls per home module | 1.67 (decls) | 2,901,030 | 0.44% |
| 3 | `flatten_module_decls` | `infer.kai` | modules × decls each | 1.27 (mods) | 2,393,716 | 0.36% |
| 4 | `fns_prefer_module` | `emit_shared.kai` | the whole `EFn` table, per module | 1.35 (mods) | 2,254,504 | 0.34% |
| 5 | `rs_append_decls` | `driver.kai:1047` | `rs.decls` — all decls resolved so far | 2.02 (mods) | 1,104,607 | 0.17% |
| 6 | `scopes_bind` | `driver.kai` | bindings per owner module | 2.03 (mods) | 109,573 | 0.02% |
| 7 | `rs_add_module` | `driver.kai` | `rs.modules` | 2.03 (mods) | 23,220 | 0.00% |

Together ~2.5% of the self-compile's allocations.

### Closed-form validation

Four sites are confirmed the way `bucket_append` was — predict the cell
count from the model, run an input of known size, compare:

`rs_append_decls`, model `D·M(M−1)/2` with D = 11 decls per module:

| M | predicted | measured | delta |
|---:|---:|---:|---:|
| 20 | 2,090 | 2,090 | 0.0% |
| 40 | 8,580 | 8,580 | 0.0% |
| 80 | 34,760 | 34,760 | 0.0% |
| 160 | 139,920 | 140,080 | +0.1% |

`rs_add_module` and `scopes_bind`, model `M(M−1)/2` (one entry per
module): exact at M = 20/40/80 (190 / 780 / 3,160, zero error); at
M = 160 measured 12,880 and 13,038 against 12,720 predicted, the residual
being the core modules entering the table.

`flatten_module_decls` decomposes as `D·M(M−1)/2` plus a per-module
linear term that stays near-constant (1,383 / 1,308 / 1,406 / 1,725 per
module across M = 20…160).

### Why `list_minus` ranks first

`emit_shared.kai:1446` computes
`list_minus(st.globals, st.local_scope)` once per lambda, to shadow
globals the local scope redefines. The existing comment reasons that
"O(|globals| × |locals|) is acceptable: |locals| stays tiny" — true, and
it addresses the wrong factor. `st.local_scope` is indeed tiny; the
cost is `|lambdas| × |globals|`, and `st.globals` holds every global
name in the program. Measured at 1.05 exponent per lambda with globals
held fixed, and 7.5M cons on the self-compile: the largest single
superlinear site in the compiler, 2.6× `bucket_append`.

## 4. Dismissed — and the bound that clears them

These carry the detector's shape and are safe. Recorded so the next
survey does not re-open them.

| site | scanned list | bound |
|---|---|---|
| `tcrec_walk_tail`, `trmc_walk_modcons`, `trmc_rewrite_kind` | `scope` | lexical shadowing depth |
| `fv_name` | `own`, `acc` | free variables of one lambda |
| `fv_expr` | — | **717 cons on the whole self-compile**; see below |
| `lower_reuse_args_donate`, `lower_ctor_args` | `acc` | constructor arity |
| `kin_dce_stmts` | `dead` | statements in one function body |
| `pcs_is_non_last`, `pcs_collect_exit_drops` | `force_set`, `skip_set` | binders in one scope |
| `ts_imported` | `imports`, `acc` | imports of one module (tens) |
| `validate_impls_loop` | `proto_names`, `seen` | protocol/impl count; measured flat at 633 across both axes |
| `ty_env_prepend_batch_loop` | `entries` | uses `[e, ...entries]` — O(1) prepend, not a spine copy |
| `xp_all`, `xp_opt`, `xp_elems`, `inode_prepend` | — | one cons per element; linear, high volume |
| `field_decl_list_has`, `rec_find_with_field_loop` | `fs` | fields of one record |
| `dedup_reqs` | `seen` | distinct specialisations per call site |

`fv_expr` deserves its own line because it is the trap this survey
nearly fell into. A first fixture that placed 800 lambdas in **one**
function body measured `fv_expr` at exponent **3.01** — 161K → 85M cons,
apparently the largest defect in the compiler. Separating the axes
refutes it: with globals varying and lambda count fixed the exponent is
**0.00**, and with lambdas varying and globals fixed it is **0.95**. The
cubic was the single giant body, not program scale. On the real
self-compile `fv_expr` allocates **717** cons cells. `list_minus`,
called from the same line of the same pass, allocates 7,539,728. One
operand is bounded and the other is not, and only measurement separates
them.

## 5. What this says about the hypothesis

Confirmed, with a qualification worth keeping.

The shape is systemic — seven instances, none found by reading the code
for suspicious names, and the largest (`list_minus`) sits under a
comment asserting it is fine. `#1897`'s summary ("every win came from
the same shape — a table scanned linearly because no index existed")
describes the compiler accurately.

The qualification: **the shape is common and the defect is rare.** 388
functions carry it; 7 pay for it. The difference is never in the call
and always in the bound, and the bound is not visible at the call site —
`list_append(acc, [x])` looks identical whether `acc` holds three type
parameters or every declaration in the program. This is why the raw
counts at the top of this note are not a work list, and why a finding
without its bound is noise.

The per-site cost is also modest: the largest is 1.14% of allocations
and the seven together are ~2.5%. They are worth fixing as a class
because they are O(n²) against a language whose programs will get
bigger, not because any one of them dominates today's profile. The
retention measurement in `docs/retention-cause-measurement.md` reached
the same conclusion from the other direction: the bulk of the
compiler's memory is the AST it is supposed to be holding.

## 6. Reproducing

```sh
# candidates
QSURVEY_JSON=findings.json tools/superlinear-survey.py stage2/compiler/*.kai

# attribution (needs the runtime shim of §2)
cc -std=c99 -O2 -I <shim-dir> -I stage0 -DKAI_TRACE_RC=1 \
   -DKAI_STDLIB_PATH=\"$PWD/stdlib\" stage2/build/kaic2b.c -o kaic2-attr -lm
KAI_TRACE_RC=1 KAI_TRACE_RC_TOP=4000 KAI_THREADS=1 ./kaic2-attr stage2/main.kai >/dev/null
# symbolize: atos -o kaic2-attr $((site_addr - aslr_slide))
```
