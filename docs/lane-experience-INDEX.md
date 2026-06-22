# Lane-experience retro index

One line per retro: the single most reusable finding (a trap, a follow-up, a
design decision a future lane in that area needs). Read the relevant line(s)
before launching a lane in that area; open the full retro only when the hook
is directly on your path. This index is the cheap entry point — the 300+ full
retros are bitácora, not source of truth (verify any claim against current code).

---

## Native (in-process libLLVM) backend — RC / correctness

- [native cons/list RC leak #860](lane-experience-native-cons-leak.md) — self-tail cons leak was THREE gaps (consuming-scrutinee drop, accumulator dropmask ignored, TRMC step scrutinee drop); gate the scrutinee drop on the pure `__kai_tcrec|` sentinel, never `__kai_trmc|` (#856 double-free).
- [native cons leak #868 — over-incref not missing drop](lane-experience-issue-868.md) — the leak was OVER-increfing: a blanket per-match scrutinee `dup` doubled it; gate the dup on reuse-arm presence (`match_owns_reuse_arm`), bind bare otherwise. The bisect-window story didn't hold.
- [native `==` dispatch UAF #858](lane-experience-native-eq-dispatch-uaf.md) — list-match bound cons head/tail without the structural incref the C oracle emits → shared tail freed while live. Serial ratchet count is load-sensitive — compare SETS (`comm -23`), never a single count.
- [native nested-owned-match scrutinee aliasing](lane-experience-native-nested-match-scr-drop.md) — two nested owned matches shared the fixed `__pcs_scr_drop` binder, collapsed into one alloca → double-free. ANY RC-touching native change MUST gate `BACKEND_PARITY_JOBS=1` (parallel ratchet hides deterministic crashes as flaky).
- [native multi-alias closure capture](lane-experience-native-multi-alias-capture.md) — install regs keyed by effect NAME not site → two same-effect installs reuse one alloca → `node->parent = node` self-cycle hang; thread (line,col) into reg names.
- [native reuse-in-place / variant-TRMC](lane-experience-native-reuse-token.md) — `KConReuse` routing every reuse through the variant helper breaks records; add `KReuseKind`/`KRecordReuse`; use MOVE (`kai_variant_reuse_at`, no donor child decref) for non-bijective rotations.
- [native clause-capture/evidence ABI](lane-experience-native-clause-capture-abi.md) — install never filled `EvE.env` (field 1) → handler-clause captures had no env channel; a clause `self` is the dispatched Ev blob, NOT a closure env.
- [native sha512 hang](lane-experience-native-sha512-hang.md) — a `let`-bound out-of-cache big Int reused >1× was double-freed in the KIR, corrupting a loop guard into non-termination (not the math). A shadowing `let x` clobbers the outer `x`'s register (native keys registers by name).
- [native double-resume one-shot](lane-experience-native-double-resume.md) — `resume` was a block TERMINATOR in the KIR so a 2nd `resume()` was dropped before the runtime one-shot check; fix is `lower_resume` (resume: terminator→expression), not the cont ABI.
- [native variant_match super-linear](lane-experience-native-variant-match-superlinear.md) — the immortal-variant cache made rebuilt tagged-Int cells immortal (rc=INT32_MAX) → drops short-circuit, never free → >70s; one-line runtime cache fix + a latent match-exit drop.
- [native hash proto-dispatch](lane-experience-native-hash-proto-dispatch.md) — lower the proto-dispatch marker to a runtime shim, NOT a KIR node (one ABI source of truth). 4/5 "proto" fixtures were 3 unrelated native bugs.
- [native extern "C" fn shim](lane-experience-native-ffi-extern-c.md) — an `extern "C" fn`'s `ETodo("__kai_ffi__")` body is lowered as a panic stub unless you synthesize the unbox→call→rebox shim in `lower_fn` via `KFnFfi` (FFI is an ABI frontier in the fn driver, never a KIR expression).
- [native list-head-in-list](lane-experience-native-list-head-in-list.md) — a `PList` head re-enters `lm_emit_cells` against the projected slot-0 cell with a fresh `cont` (fall through, don't exit the arm).

## Native backend — perf (P1–P4 + diagnosis)

- [native codegen-perf diagnosis + plan](lane-experience-native-codegen-perf.md) — the KIR walk is all-boxed so O2 can't penetrate external `declare`s (~86× scalar); re-derive the repro on HEAD before trusting a brief. Honest benches use `N*(seed/seed)` the optimiser can't fold.
- [native Int unbox P1](lane-experience-native-int-unbox.md) — extend the Real raw path to Int arith/cmp; wrapping add/sub/mul (no nsw/nuw). Remember the second `rprelude_table()` in stage1/compiler.kai (double-source like the two runtime.h).
- [native bitcode-link P2](lane-experience-native-bitcode-link.md) — `LLVMLinkModules2` the runtime bitcode before `default<O2>` so it inlines `kaix_*`; generate the `.bc` per-platform at build time, never vendor a mac `.bc`.
- [native raw scalar params P3](lane-experience-native-raw-scalar-params.md) — read the KSlot the lowering populated from UFnSig (mode-slave), never re-derive the sig; Char stays SBoxed; the return crossing belongs in `KRet`.
- [native raw div/rem P4](lane-experience-native-div-rem-raw.md) — emit raw `sdiv`/`srem` with NO guard — the C oracle does, with `/0`+`INT_MIN/-1` UB accepted; a guard would diverge from the byte-exact oracle.
- [native opt passes L4 #498](lane-experience-native-opt-passes.md) — run New-PM `LLVMRunPasses(M,"default<O2>",TM,...)` in-process; reuse the emit TargetMachine; the C-API string format is stable across LLVM 18.x.
- [native non-tail raw-scalar call #861](lane-experience-native-nontail-rawcall.md) — closing the call result alone left 3 box/unbox sites; the dominant missed one was the raw `if`-join (unconditionally SBoxed).
- [native TCO lambda-param overflow](lane-experience-native-tco-lambda-param.md) — TCO DID fire; the overflow was the `[n x ptr]` arg buffer alloca'd in the loop body → hoist to entry block. Dump the KIR before trusting a "TCO doesn't fire" premise.
- [native Real box/unbox (np-real)](lane-experience-np-real.md) — KIR ignored the unbox pass `.mode`, re-boxing a raw Real → double-free; KIR must be mode-slave like the C emitter.

## Native — parity burn-down (168→0) + the default flip

- [native-default flip EXECUTE](lane-experience-native-default-flip-execute.md) — native is the runtime default; build-capability (Makefile `llvm-config` auto-detect) is separate from runtime-default (`bin/kai resolve_backend`); implicit default degrades to C with a note on a C-only kaic2, explicit `--backend=native` errors.
- [burn-down 1](lane-experience-native-parity-burndown-1.md) — closing a mechanical bug surfaces the NEXT bug on the same fixture (count closes < gaps).
- [burn-down 2](lane-experience-native-parity-burndown-2.md) — unary `-` aliased binary `-` (garbage 2nd operand); a 3-line runtime tag-print on `kai_op_*` was the highest-leverage diagnostic. Some closes are macOS-only.
- [burn-down 3](lane-experience-native-parity-burndown-3.md) — shared-tag sub-discrimination (`Exited(0)`/`Exited(_)`) lowered two switch cases → duplicate i32 case + dead arm; new `kir_lower_variant.kai`.
- [burn-down 5](lane-experience-native-parity-burndown-5.md) — KIR temp-register vs surface-binder namespace collision; rebasing brings corpus-growth gaps that are NOT regressions. Burn-down 4 never merged (stale-golden KIR regression).
- [burn-down 6](lane-experience-native-parity-burndown-6.md) — `EBang`/`EPipe`-class nodes with no `lower_expr` arm fall to unit (silent wrong value); kaic1 is lax (builds), selfhost is the real gate.
- [np-crash / burn-down 7](lane-experience-np-crash.md) — quicksort `nat=139` SIGSEGV was NOT memory — `EPipe`→unit lost `filter` so `sort` recursed on a non-shrinking input. Failure signature is downstream of the bug.
- [np-decode](lane-experience-np-decode.md) — the native backend must own the C99 escape table the C backend delegates to cc (`\xHH`/`\ooo`/`\a\b\f\v`).
- [np-handlers](lane-experience-np-handlers.md) — a `pub effect` with NO `default{}` block aborts; the gaps-doc LIST drifted from the prose (verify both).

## KIR design + lowering

- [KIR lane 0 (define + dumper)](lane-experience-kir-lane0.md) — full §4 node set in one cut; shipped with a silent catch-all that mis-lowered whole node families.
- [KIR lowering completion](lane-experience-kir-lowering-completion.md) — Lane 0's `_ -> KUnitV` catch-all silently mis-lowered ERecordLit/EModCall-value/ETodo/ELambda/EHandle/interp; gate = zero unhandled-node traps over corpus. The KIR→.ll text translator was abandoned (libLLVM in-process won).
- [KIR inproc-libLLVM bring-up](lane-experience-kir-inproc-llvm.md) — `TyHandle` is a non-RC opaque scalar Ty for LLVM C-API objects, riding `MUnboxed` so perceus never dup/drops it; emitter is a mode-slave.
- [KIR native walk subset 1](lane-experience-kir-native-walk.md) — no "arithmetic-only" island exists: the auto-loaded core drags ~344 fns into every module; unsupported nodes must abort LOUDLY (`nemit_unsupported`).
- [KIR native-fix (spine)](lane-experience-kir-native-fix.md) — production kaic2 is compiled by kaic1 (stage1), NOT stage2; a broken opt-in backend shipped green (PR #780 CI hole).
- [KIR native builtin defaults](lane-experience-kir-native-builtin-defaults.md) — synthetic builtins and source `default{}` effects are ONE class (all-`$extern_handler` block resolved by `find_effect_default_block`); install at main or every output aborts.
- [KIR effects 2b (stateful handler)](lane-experience-kir-native-walk-effects-2b.md) — stateful handler state lives at Ev field 2 / byte 16; the `["state","log"]` clause-state alias pair is hardcoded in all emitters (m7b #11 debt).
- [KIR native TRMC](lane-experience-kir-native-walk-trmc.md) — a node reshape + its sole consumer MUST land together: PR #800 frontend-alone broke all 13 tier1-native fixtures (stdlib gained `KTrmcStep`, native aborted).
- [KIR trmc lowering](lane-experience-kir-trmc-lowering.md) — `cons_ok = not use_llvm` wrongly excluded kir/native → cons-TRMC lowered to O(n)-stack recursion (mandatory-TCO violation, overflows 200K lists).
- [KIR list/literal/record match lowering](lane-experience-kir-list-match.md) — `lower_match` assumed variant-tag-only discrimination; `PList`/literal/`PRecord` `arm_tag`=-1 → empty switch + binders never emitted → SEGFAULT on native. Fix = cons/nil (and literal, and record-field) fail-chain decision trees. (See also `kir-literal-match`, `kir-record-match`.)
- [KIR clause-stateful flag](lane-experience-kir-clause-stateful-flag.md) — `ClauseInfo`'s 7th field (`stateful`) dropped `_` → KIR consumer saw a clause referencing a free `state` register the KFn never declared.

## Native RC / Koka reuse (rb-tree)

- [#747 native match RC](lane-experience-issue-747.md) — the native backend NEVER implemented match RC (leaked every scrutinee, 3.7 GB); ported the C match-RC layer: kind-1 raw binders, `kaix_variant_arg` BORROW not incref.
- [#747 i64-inline + variant fast-path](lane-experience-issue-747-i64-inline-llvm.md) — the cold `kai_variant_u` preamble dominated the residual gap, not the box round-trip; ported i64-inline + fast ctor paths.
- [Koka arm-binder move](lane-experience-koka-arm-binder-move-leak.md) — Koka's rbtree INSERT uses NO borrow; the leak dies via `is_unique`-gated dup/drop at the match-arm top (Parc), not interprocedural borrow. RSS 38.6×→2.29×C.
- [Koka bind-borrow + arm-top reuse](lane-experience-koka-arm-top-reuse.md) — the Black arm (majority of the tree) CANNOT reuse without B3 interprocedural borrow inference; a resolved ctor `EModCall(_,Ctor)` is linear.
- [rb-tree nested-pattern reuse](lane-experience-rbtree-nested-pattern-reuse.md) — the reuse lever is the nested pattern, not interprocedural; `offsetof(KaiValue, as.var_slots)` not `(KaiValue*)1` member access (UBSan misaligned on every variant alloc).
- [rb-tree allocator free-list](lane-experience-rbtree-reuse-allocator-pool.md) — killing Int boxing moves rb-tree only 15.8×→13.5%; the dominant cost is 22.6M variant malloc/free → fixed-size free-lists. Store-new-then-decref-old-only-if-different (free-list recycle exposes latent double-free libc masked).
- [rb-tree 1×C attack order](lane-experience-rbtree-1xc-attack-order.md) — the `kai_check_unique` rc==1 guard makes TRMC on-by-default safe (OCaml's was opt-in for lack of it).

## Perceus / RC / TRMC (C backend)

- [#118 reuse-in-place v1](lane-experience-issue-118-perceus-reuse-in-place.md) — runtime `kai_check_unique` + `kai_reuse_or_alloc_cons` (incref's the reused cell so it survives match-exit decref — sidesteps pass-ordering). cons-only; variant/record disabled (lexical `[A-Z]⇒ctor` heuristic misfires).
- [#118 phase0 audit](lane-experience-issue-118-perceus-phase0-audit.md) — "known-unique" = perceus last-use, NOT type-system uniqueness; record-rebuild sites (472) dwarf cons-spine sites (30) — cons+variant only misses most of the win.
- [#209 perceus dup](lane-experience-issue-209-perceus-dup.md) — the #118 blocker was `emit_pat_binds_list` increfing cons CHILDREN, not the scrutinee dup; bind head/tail by borrow only after `kai_check_unique`. `args` param shadows prelude `args()` → silent no-op.
- [#210 reuse typer-shape](lane-experience-issue-210-reuse-in-place-typer-shape.md) — re-enable variant/record reuse via a typer predicate `mangle_ty(scr.ty)==mangle_ty(body.ty)` replacing the lexical heuristic.
- [#212 cons-accumulator double-incref](lane-experience-issue-212-tco-double-incref.md) — NOT in TCO/Perceus but in `emit_list_tail`'s `[ElSpread(x)]` O(1) trick stacking `kai_incref` on Perceus's `__perceus_dup`; detect `is_perceus_dup_call(x)`.
- [#293 record/variant RC](lane-experience-issue-293-phase1-rc-record-variant.md) — `kai_record`/`kai_variant` CONSUME their children (don't copy); the real fix is EField base-decref. Leak lives in pass-through, not construction. (See also #293-phase2 discards.)
- [#291 runtime RC tracing](lane-experience-issue-291-runtime-rc-tracing.md) — `-DKAI_TRACE_RC` strict mode; headline: mainline leaks ~82M live KaiValues (emitter RC discipline structurally absent); record/variant dominate.
- [#296 leak-site attribution](lane-experience-issue-296-rc-call-site-attribution.md) — per-chunk `alloc_site` via `__builtin_return_address(0)` needs every alloc wrapper `noinline`; two adjacent sites in `kai_synth_dim_pow` = 62.5% of leak, hidden by tag-level reporting.
- [#297 prelude-table sizing](lane-experience-issue-297-ep-prelude-leak.md) — the 642K "leak" was a SIZING bug: `KAI_IMMORTAL_VAR_BUCKETS` overflowed under self-compile (linear probe never sees empty sentinel → silent install failures → fresh allocs). Re-validate sizing constants as the compiler grows.
- [#812 RC tracer reports 0](lane-experience-issue-812.md) — `incref_total`/`decref_total` sat inside `#ifdef KAI_TRACE_RC` while `alloc_total` did not → default binary run with `KAI_TRACE_RC=1` reported 0 (every "RC balanced" gate vacuous).
- [#817 filter/map arm-binder leak](lane-experience-issue-817.md) — arm-binder + scrutinee leaks are emit_c/perceus-pure and ship alone; only the lambda-param exit-drop wrap is native-coupled (native lowers `EBlock` wrap for `fn` but NOT closure bodies).
- [tcrec coordination](lane-experience-perceus-tcrec-coordination.md) — countdown leak closed by `emit_match_default` injecting `kai_decref(_scr)` before the goto when the arm tail is always a tcrec sentinel.
- [pipeline reorder perceus→tcrec](lane-experience-pipeline-reorder-perceus-before-tcrec.md) — reorder to `typer → perceus → tcrec → emit` so drops are explicit data tcrec treats as ordinary nodes (kills the byte-for-byte dropmask coupling; #92 R6 hit it 4×).
- [perceus int-cache widen](lane-experience-perceus-int-cache.md) — widen `[-128,127]`→`[-65536,65535]` but per-entry LAZY warm (eager = 6MB RSS on first in-range int); cold `.bss` slot has rc=0≠INT32_MAX.
- [immortal variant/nullary singletons](lane-experience-perceus-next-tier-rc-fix.md) — `Some` over an immortal payload routes to a shared cache keyed `(tag,name,args[0..n])`, n≤4. (See also nullary-variant-singletons.)
- [TRMC port from Koka](lane-experience-trmc-port.md) — spine TRMC correct but wall-time didn't move: 35%+ of allocs are tag=int boxes (mixed boxed-`t` signature re-boxes keys every level); needs Int-unbox + specialized struct.
- [TRMC for builtin cons](lane-experience-trmc-elist.md) — recognize EList + reuse-cons shapes; C-text only; blocked from a clean 2M exit by a separate recursive cons-spine free overflow (needs iterative tail-spine free).
- [TRMC sentinel into LLVM IR](lane-experience-trmc-llvm-fallback.md) — the `__kai_trmc|` sentinel has illegal `|` for an LLVM symbol; gate the rewrite to C (`cons_ok`), LLVM falls back to plain recursion.
- [Real unboxing](lane-experience-real-unboxing.md) — extend the unbox pass to Real → raw `double`, ~14×→~2.2× wall vs C.
- [tco-stage1-mirror #42](lane-experience-tco-stage1-mirror.md) — stage-1 selfhost is a FALSE signal for a missed-rewrite TCO bug (compiler.kai too small); stage-2 selfhost is the real gate.

## Typer / resolver / dispatch

- [#235 bare-name narrowing](lane-experience-issue-235-typer-load-order.md) — bare-name lookup has FOUR call paths; all must narrow by first-arg type + rewrite `EVar`→`EModCall`. Concrete-headed first param must beat tyvar-headed on tie or polymorphic `list.repeat` hijacks `string.repeat`. DTest bodies aren't type-inferred — qualify their calls.
- [#219 qualified modcall](lane-experience-issue-219-typer-modcall.md) — `TyEnv` keys schemes by bare name → two modules' `map` collide; fix registers `<mod>::<fname>` qualified key, `EModCall` consults first.
- [#216/#217 resolver gaps](lane-experience-issue-216-217-resolver-gaps.md) — qualified-call resolver is `rqc_decls` (NOT `rename_proto_calls`); prelude bodies were never walked + interp `#{}` re-parses after rqc; keep rqc EARLY (late breaks `list.max`).
- [#205 UFCS](lane-experience-issue-205-ufcs.md) — UFCS = direct `ty_env_lookup` + first-param unify; field always wins over UFCS when the record field exists. `let r=…; r.method()!` hits a Perceus pre-decref UAF — use an inline receiver.
- [#594 pipe convention](lane-experience-issue-594-pipe-convention.md) — per-compile head-owner cache lets any `pub type` with `pub fn map/flat_map/filter` join `|`/`||`/`|?` zero-compiler-change; `DType` carries no `module_origin` (recurring debt) → derive home from `[ModuleEntry]` exports.
- [#773 nominal effect-row carriers](lane-experience-issue-773.md) — the effect row in a nominal `TyCon` arg slot was DROPPED to a phantom var (soundness hole: a `Stream[t, File+Fault]` effect leaked in a pure main).
- [#802 distinct effect cells](lane-experience-issue-802.md) — `find_remove_label` matched labels by NAME ALONE ignoring `ty_args`, fusing `State[Bool]`≡`State[Int]`; gate partition on `labels_args_unify`.
- [#772 compound row args at alias instantiation](lane-experience-issue-772.md) — a substituted alias label was never re-run through effect-alias expansion; needs a short-lived `TyRow(RowExpr)` AST node.
- [#624 alias+row-var panic](lane-experience-issue-624-typer-alias-row-panic.md) — `apply_tp_subst_te`'s `TyFn` case ignored the row field → dropped row tparam survived as ROpen, `Some(-1)` sentinel tripped unchecked `array_get`. The row-inside-TyFn field is the trap.
- [#743 effect-op payload field access](lane-experience-issue-743.md) — `synth_field` decided field-vs-UFCS while the receiver tyvar was still free (a TIMING bug); deferred-field worklist is the fix shape.
- [#842 multi-param effect-op payload](lane-experience-issue-842.md) — two ops of one parametric effect raising the same row-label hit a shallow row-dedup that DROPS the free binding; order-dependent. `decl_row` positional-unify was reverted (broke m7b_11).
- [#758/#341 type anchoring beyond let](lane-experience-issue-758.md) — test/bench/check bodies were never run through HM inference; protocol-call rewrite reads `Expr.ty` so it never fired in a `test` block. Read `Self` from the full return TypeExpr.
- [#645 row subsumption pure callbacks](lane-experience-issue-645-row-subsumption-pure-callbacks.md) — pure fn into effectful slot via GENERALISATION (phantom row var on REmpty sigs), NOT unifier subsumption; effectful→pure stays rejected; monomorph key excludes row vars.
- [#546 dispatcher dedup](lane-experience-issue-546.md) — the bug is the static rewrite fixing the first-declared protocol via single-result `proto_op_lookup` → iterate every protocol declaring the op, pick by impl-for-receiver.

## Protocols / operator overloading / derive

- [#180 protocol P[A] heterogeneous](lane-experience-issue-180-protocol-pa.md) — impl-table key stays `(P, Self)`; heterogeneous impls mangle `__pimpl_P_T_op__a_<arg>` (legacy homogeneous keeps the old symbol → byte-id). Bidirectional inference reads the SLet return annotation.
- [#174 poly-impl constraint](lane-experience-issue-174-poly-impl-constraint.md) — `impl[T: Show] Show for [T]`: the runtime panic is a monomorphisation gap; re-run `resolve_protocol_calls_decl` after `subst_decl` + a post-mono validator. Bounds in the tparam String slot (`"T#b:Show"`). Function-level bounds stay prohibited.
- [#246/#245 operator overloading](lane-experience-issue-246-245-operator-overloading-complex.md) — operator→method rewrite lives INSIDE `synth_binop` (after operands typed) so the primitive fast path stays default; protocol dispatch is opt-in by resolved type. A user `fn show` gets rewritten to `__proto_show`.
- [heterogeneous binop result type](lane-experience-heterogeneous-binop-result-type.md) — `Real + Complex → Complex` re-stamps node `.ty` to the impl's declared return head BEFORE unbox (else a typer bug becomes a memory crash). `--check` shows the old mismatch (its impl table is empty during binop inference).
- [eq/ord nested dispatch](lane-experience-eq-ord-nested-dispatch.md) — `collect_variant_to_head` must be indexed by TAG not list position (`register_variants` head-conses user variants). NESTED RECORD fields still don't dispatch (records never learn their type tag).
- [proto eq ABI shim](lane-experience-fix-proto-eq-abi.md) — unboxed scalar protocol impls stored + called as boxed → SIGSEGV; emit a `kai_<csym>__boxed` shim, keep the raw pimpl for the mono fast path. Local clang opaque pointers HIDE the mismatch — gate on IR-grep, not clang acceptance.
- [default-shim arity0](lane-experience-default-shim-arity0-unboxed.md) — the arity-0 Default ABI bug's root cause was upstream in `classify_unbox_sig` (a gate excluded every arity-0 fn from unboxed classification). Widen to `any_param_unboxable or (arity0 and scalar ret)`.
- [#258 Default protocol](lane-experience-issue-258-default-protocol.md) — `Default` arity-0 return-only Self resolves via use-site annotation; `make[T: Default]()` blocked (free-fn tparam bounds reject non-Type/Measure kinds — same gap as `min[T: Ord]`).
- [protocols runtime dispatch (Option C)](lane-experience-protocols-runtime-dispatch.md) — when post-mono can't pick the impl statically, dispatch via O(1) `(proto_id, head_type_tag)` hash; reuse the FFI `ETodo`-marker pattern for zero typer change; pin tags in `docs/variant-tags.md`.
- [#derive Eq/Hash/Ord for sums](lane-experience-m12.8.x.md) — derive validation in a SEPARATE pre-pass before `expand_derives` (the `lp.errs>0` short-circuit fires before the typer → clean diagnostic). Inner-match binders must be disjoint from the outer. (See also m12.8.z for Ord.)
- [hashable audit](lane-experience-hashable-audit.md) — `Real` hash must bit-cast (IEEE-754) not truncate; variant tag mixing must be multiplicative (`tag*FNV_prime`) not additive (else `Ok(1)==Err(0)`). Real-hash exposed signed-overflow UB → `uint64_t` round-trip gated on TyInt.

## Effects / handlers / runtime

- [#251/#252 Mutable soundness + masking](lane-experience-issue-251-252-mutable-soundness-and-masking.md) — `a[i]:=v` lowered to a BARE `array_set` bypassing the Mutable label; add `array_set`/`array_grow` to `prelude_effect_for` + a masking pass dropping Mutable when every demand targets a locally-constructed Array (captured-in-lambda local, passed-to-helper escapes).
- [#531 Mutable mask general](lane-experience-mutable-mask-general.md) — install the Mutable default UNCONDITIONALLY at the evidence-stack base; base-installable = non-parametric static handler AND effect dies with the local value.
- [#308 effect shadowing](lane-experience-issue-308-effect-shadowing.md) — a user `effect Log` shadows the builtin in the resolver, but default-handler emit gated on `list_has(main_row,"Log")` still wrote nonexistent `EvLog` fields; filter `main_row` by op-name shape match. Don't gate on line/col≠0,0 (source effects have real positions).
- [#556 $extern_handler](lane-experience-issue-556-extern-handler.md) — `$<ident>(args)` reserved sigil for compiler intrinsics; `Cont[Ret]` single-param (never `Cont[A,B]`); a bare-ident `c_sym` local shadowed the top-level FFI `c_sym` fn → emitted empty symbol.
- [#558 default handler Stage C](lane-experience-issue-558-stage-c.md) — generic `builtin_default_block_for(eff, ops)` made 17 migrations 2-line edits; ~500 lines of parallel tables deleted. (See also #533 which only scaffolded.)
- [exp2 handler clause capture](lane-experience-exp2-arm-a.md) — handler clause bodies have NO closure-capture path (emitted as top-level C fns seeing only self/k/op-args/state); thread enclosing data through a private parametric effect's `state`. A single-read `state` ref emits a raw pointer copy that string-concat frees → UAF (workaround: force `__perceus_dup`). (Companion arm-b: handler-composition needs ≥3 invocations to surface the R10 bug.)
- [#866 migrate builtin effects to source](lane-experience-issue-866.md) — moving 25 compiler-injected effects to stdlib source (`Clock` template, `$extern_handler` bridge) shrank driver.kai 5685→3063; a parser collision forced two renames + a latent shadowing bug.
- [#678/#679 stdout buffering + cancel-parked](lane-experience-issue-678-679-stdout-cancel.md) — libc block-buffers non-TTY stdout; `setvbuf(_IOLBF)` once at entry. Cancel must wake a reactor-parked fiber, not only check at op-call boundaries.
- [#682 cancel-from-sibling](lane-experience-issue-682-cancel-sibling-handler.md) — sync `Cancel.raise()` and sibling `Spawn.cancel()` had DIVERGED delivery paths; the sibling path skipped the user `with Cancel` handler.
- [#680 polymorphic fold double-free](lane-experience-issue-680-poly-fold-rc-bug.md) — post-perceus linearity must strip `__perceus_dup(x)` wraps; a mono spec's self-call kept pointing at the poly original (`retarget_self_calls_decl`).
- [#697 check-param String SIGTRAP](lane-experience-issue-697.md) — a single-use check-param moved raw to a decref-aware consumer, then the check loop's iter-drop double-freed it. Fix in perceus; ASAN pins it.
- [#703 double `match v` on let-bound variant](lane-experience-issue-703.md) — Perceus SKIPS the body of a UFn-sig function; each `match` ends with `kai_decref(_scr)` → two reads double-decref. C was wrong, LLVM right (inverted the C-oracle premise).
- [Ref first-class KAI_REF](lane-experience-ref-first-class-kai-ref.md) — `Ref` = a `KAI_REF` single-cell tag (was a length-1 KAI_ARRAY); a LOCAL Ref cannot mask Mutable (unlike Array): `ref_*` route through handler dispatch with no bare-builtin bypass.

## Reactor / concurrency runtime

- [reactor R1 (file+sleep+process)](lane-experience-issue-611-reactor-r1.md) — `poll()` over self-pipes, not kqueue/epoll; epoll does NOT notify on regular files → 4-worker thread-pool offload; set fiber state=PARKED BEFORE arming the wake; block SIGCHLD on worker threads.
- [reactor R2/R3/R4](lane-experience-issue-630-reactor-r2.md) — per-direction waiter lists (sockets full-duplex); R3 stdin singleton waiter + lazy `O_NONBLOCK`+`atexit` restore; R4 single self-pipe with `SA_RESTART` (signal IS the payload). (See R3/R4 retros.)
- [#763 spawn_actor_policy](lane-experience-issue-763.md) — `kai_mailbox_alloc_bounded` stamps owner + overwrites the PARENT's mailbox slot → needed a `_unowned` bounded variant.
- [#668 map fiber-stack overflow](lane-experience-issue-668-map-fiber-stack.md) — `list.map = [f(h), ...map(t,f)]` is non-tail → 40K frames overflow the 64KiB fiber stack (8MiB main stack hid it); rewrote map/filter/flat_map tail-recursive + reverse.

## Cache / serialization / modularization

- [#825 cache the core post-parse](lane-experience-issue-825.md) — the #455 "deserialize≈parse" premise is FALSE for the core: the real lever is LEX of the 4401-LOC core (~70ms); cache post-PARSE per-module, byte-identical C.
- [#455 phase B user-file cache](lane-experience-issue-455.md) — deserialize≈parse so A.0 gives ~0 wall win; the deliverable is correct transitive invalidation (content-addressed key as filename, no stale-serve window). stage1 mis-compiles `if`-without-`else` as unconditional.
- [#592 KAB2 binary cache](lane-experience-issue-592-kab2-binary.md) — `"#{int_to_char(b)}"` corrupts binary (Show for Char escapes NUL/≥128) → use `int_to_byte_string`; batch shasum once (32 forks ate the savings).
- [#597 boundary tagging](lane-experience-issue-597-boundary-tagging.md) — synth sites stamp `module_origin` via streaming `decl_home_hint_reset`; `partition_decls_by_origin` is exhaustive only because every synth site tags (fragile to merge-order reshape); root-file synth stays `mo=None` for byte-id.
- [#460 typer modularisation](lane-experience-issue-460-typer-modularisation.md) — `prelude_len` is the built-in seed boundary NOT a user-prelude quantity; `unions` must be collected globally before any per-module typecheck.
- [#574 typer per-module fold](lane-experience-issue-574-typer-per-module-fold.md) — multi-segment fold renumbers TyVar IDs → `--dump-typed`/`--dump-mono` goldens drift when the cache lane drives it.
- [#585 cache ECall RC panic](lane-experience-issue-585-cache-ecall-rc.md) — pattern binder `args` resolves to prelude `args()` closure in emitted C → non-exhaustive-match panic; convention `args`→`as_`, `body`→`bod_`. Round-trip test must cover EVERY ctor.
- [#677 stage2 modularization (20-phase series)](lane-experience-issue-677-phase1s.md) — the callee graph decides the cut, NOT the brief's line range or source section headers; strip comments BEFORE the graph (a doc-comment naming a fn over-pulls 111/192 decls); a module can't import from main.kai (modules import downward); the driver is the pipeline APEX — extract it LAST; `compiler.protocols` resolves to the stdlib prelude `protocols` (name it `protos`); the two backends share a 133-fn lowering layer (sink to `emit_shared.kai`). The flat bundle hides import + pub-leak + name-clash errors that package-mode `kai test` catches.
- [#748 separate compilation](lane-experience-issue-748.md) — per-module `.o` links cleanly only because mangling makes symbols distinct; a module's bare self-call mangled to the user's root shadow `kai_sum` (invalid C / silent 1-vs-2-arg LLVM ABI mismatch).

## Module system / privacy / shadowing

- [#510 pub enforcement](lane-experience-issue-510-pub-enforcement.md) — `pub` was parse-only until here; `validate_pub_access` recovers a decl's home by streaming (DType/DEffect/DConst carry no `module_origin`); `lower_consts`/`lower_protocols`/DImpl-methods erase origin and need re-tagging. Any pre-#510 retro claiming "privatised X" was a no-op.
- [hanga-roa pub-leak validator](lane-experience-hanga-roa-pub-leak-validator.md) — filter-at-import does NOT work in kaikai (one flat C TU, so a `pub fn`'s private helper must travel into the consumer); enforce privacy at name resolution via two post-typer walkers.
- [#643 private-type leak (arity)](lane-experience-issue-643-private-type-leak.md) — prelude private `Tree[k,v]` leaked into user `Tree` via the flat name-keyed variant table → arity-aware walker + `T::__type_decl_arity_N__` sentinel; same-arity collisions stay skip-listed.
- [#647/#648 same-arity private leak](lane-experience-issue-647-same-arity-private-type-leak.md) — mangle non-pub prelude `TBSum` types to `<module>::T` at AST level pre-`expand_ta_decls`; records NOT mangled (rec_find bare-name-keyed) → use a field-set tie-breaker at each access site instead (#648).
- [#644 ctor overloading (Plan B)](lane-experience-issue-644-ctor-overloading-cap17.md) — register user variants under `<tname>::<cname>` always, bare only when the slot isn't a prelude builtin; `try_bare_call_narrow` must skip ctor callees or it re-routes prelude `Ok` to a user variant.
- [#518 import/duplicate collision](lane-experience-issue-518-import-collision.md) — prelude-vs-import collision is ONE `module_table`; strict-reject chosen over shadowing (runtime cost of scope-aware variant lookup too high).
- [#538 import cycles + missing-import](lane-experience-issue-538-shape1-import-cycles.md) — `in_progress` stack distinct from `visited`; check in_progress BEFORE visited; missing-import needs an `errs: Int` counter on `ResolveState` (was eprint-only, exit 0).
- [#565 privacy transitive leak](lane-experience-issue-565-privacy-transitive.md) — `propagate_home_back` overran a module block because `prev==new` couldn't distinguish anchored from derived → `HA(home, anchored)` flag, stop at first anchored entry.
- [#285 closure capture / array-lift](lane-experience-issue-285-ref-loop-closure-arraylift.md) — `@count` in a loop closure misdispatched to `list.count` because the lambda-lifter drops names shadowing a global fn; thread `enclosing_scope`. Match-arm pattern bindings NOT yet threaded.
- [#672 @cap in interpolation](lane-experience-issue-672.md) — pre-resolve passes counting name USES under-count reads hidden in `#{...}` (unparsed until `desugar_interp_decls`); hand-rolled cap-read span scanner forces the canonical State handler.

## Algebraic types / unions / patterns

- [#187 union milestone](lane-experience-issue-187-milestone.md) — `|`-always-union; option A (dual representation: keep TyCon + variant table, add TyUnion alongside) is purely additive → byte-id. `PatKind` variant touches ~16 walker sites; gate per-component exhaustiveness on `any_arm_is_narrow`.
- [unions cleanup post-#187](lane-experience-unions-cleanup-post-187.md) — `variants_of_type` is the single source of "is X a component of Y?" for both upcast + exhaustiveness; thread `UnionInfo` into `TyEnv` separate from the variant table.
- [#266 positional record sugar](lane-experience-issue-266-positional-record-sugar.md) — `T{v1,v2}` → named via a pre-typer pass keyed off the record registry's field order; positional values tagged `__pos_<i>__` to reuse `ERecordLit`. `git stash` mid-implementation silently dropped edits.
- [#129 multi-arg-match duplicate default](lane-experience-multi-arg-match-duplicate-default.md) — `build_marm_columns` emitted a dead `_ -> fall` after an irrefutable column → two `default:` in one switch; drop it when the column is irrefutable AND guard-free.

## Lexer / parser / surface syntax

- [#608 #[...] attribute syntax](lane-experience-issue-608-attribute-syntax.md) — `#[derive(...)]`/`#[unstable]` canonical; bare `#derive` lexes to TkError (drop at Orongo); unknown attributes parse and DROP from AST. `//`/`/*` reserved as TkError.
- [#311 triple-quote strings](lane-experience-issue-311-triple-quote-strings.md) — bodies carry literal newlines/`"` the C emitter wrapped raw → broken C string; `escape_str_body_for_c` at the non-interp fast path + both interp lit sites. `pcs_rewrite_estr_span` must NOT escape.
- [#312 real literal division](lane-experience-issue-312-real-literal-division.md) — `9.0/2.0`=4 was C-codegen: `real_c_lit` appends `.0` only when neither `.` nor `e/E` present (naive `.`-only breaks `1e+10`). For wrong-arithmetic bugs, read the C output first.
- [#267 complex literals](lane-experience-issue-267-complex-literals-lexer.md) — `3i` peek-consume `i` only when contiguous (`for i`, `3 + i` stay identifiers); sugars test harness has NO preludes (gate per-file).
- [#315 remove `//` operator](lane-experience-issue-315-remove-double-slash-operator.md) — raw-Int `/` already folds to native C `/` via `op_is_raw_arith`; lexer emits `TkError` with a migration message.
- [param type grouping](lane-experience-param-type-grouping.md) — `fn foo(a, b: Int)` via accumulate-then-back-fill (LL(1)); also fixes the parenthesised-lambda parser throwing away the annotated type.
- [#456 qualified record literal](lane-experience-issue-456-qualified-record.md) — `mod.Type {` commits to record-lit only on the third token `{`; qualifier folded into the dotted name string.
- [#232/#234 qualified types + ctors](lane-experience-parser-qualified-types-and-constructors.md) — `mod.Type` in sigs + `mod.Ctor` in expr/pattern; byte-id is the trivial-rewrite case (compiler doesn't yet use the surface).
- [const as value](lane-experience-const-as-value.md) — a `const` is a value not a thunk: both backends inline the literal body (boxing broke unbox); `find_lam` must key on `(enc_fn,line,col)` not `(line,col)` (cross-module collisions).
- [#201 flat-map pipe](lane-experience-issue-201-flat-map-pipe.md) — `EMapPipe`/`EFlatMapPipe` rewritten by the typer into `ECall` before either backend sees them; the compiler does NOT warn on a non-exhaustive `ExprKind` match — selfhost + tier1 is the only safety net for a missed walker.
- [#785 trailing-comment round-trip](lane-experience-issue-785.md) — `fmt_drain_trailing_after` off-by-one in `decls_trailing_limit` demoted a trailing comment to leading on pass 2 (non-idempotent).

## Stdlib / collections

- [#374 HashMap is MUTABLE](lane-experience-issue-374.md) — array-of-buckets + `Ref` cells + `/Mutable`, NOT a pure HAMT (the HAMT was 1.5–1.7× SLOWER — `list.nth` per trie level is O(i)); `m[key]` sugar is one additive `synth_index` arm. Record keys don't dispatch through generic `hash` (head-tag-0 gap).
- [#375 HashSet](lane-experience-issue-375.md) — the issue body is STALE (`Hashable`→`Hash`, persistent→mutable); mirror as-shipped HashMap; constraint discharged by use at leaf call (no `[t: Hash]` bound — no constraint propagation).
- [stdlib pipe-coherence](lane-experience-stdlib-pipe-coherence.md) — every collection rides the pipes; `Set.map`/`HashSet.map` collapse duplicates, `HashMap.map` is pair-shaped (matches `Map`); HashSet/HashMap combinators carry `/ Mutable`.
- [#182 result/option API](lane-experience-issue-182-result-option-api.md) — kaic2 batch-resolves all preludes before infer → forward cross-file refs type cleanly regardless of file order; `result_collect`/`opt_collect` aren't tail-recursive (fine for tiny fixtures).
- [#744 String/Char codepoints](lane-experience-issue-744-string-char-codepoints.md) — `chars()`→`bytes()` (byte-wise), new `chars()` decodes UTF-8, `length` stays bytes; Rust-shaped scalar-value `Char` (`int_to_char` panics on non-scalar). A `feat!` BREAKING pre-adoption, no edition bump.
- [#745 text.display_width](lane-experience-issue-745-display-width.md) — wcwidth-style width in a new opt-in `stdlib/text.kai` NOT `core/string.kai` (the ~80-interval EAW table would bloat the bootstrap prelude every build pays for).
- [#473 u8 nominal primitive](lane-experience-issue-473-u8-primitive.md) — adding a prelude fn needs THREE coupled tables (`prelude_names` + `add_prelude_sigs` + `prelude_table`); u8 stays nominal not refinement (refinements erase at codegen, killing the `Array[u8]` mangle).
- [#482 fs/file binary IO](lane-experience-issue-482-file-bytes.md) — `write_file` can't byte-exactly serialize `[Byte]` (Show-for-Char escapes non-printables) → symmetric `file_read_bytes`/`file_write_bytes` runtime prims tagged `File` via `prelude_effect_for`.
- [#771 chunked file IO](lane-experience-issue-771.md) — ops on the `File` effect (not prelude builtins) for mockability; the `default {}` auto-generates from the ops list, File pinned in both install orders.
- [#801 lazy Stream[t,e]](lane-experience-issue-801.md) — the carrier is a single-constructor VARIANT, not a function-type alias (the typer didn't support a function-shaped carrier with effect rows in higher-order positions).
- [binserialize array buf](lane-experience-binserialize-array-buf.md) — `[Byte]`→`Array[Byte]` kills the O(N²) cons-walk (19s→~5ms); fill via `list.foldl`-captured-local (passing the Array as a param escapes Mutable masking). `list.foldl` is `(acc, elem)` order.
- [const/string drop_prefix](lane-experience-issue-632-string-drop-prefix.md) — a fixture helper named `show` collides with the `Show` protocol → C-level arity-mismatch with no kaikai pointer; pick `report`/`emit` not `show`.

## FFI

- [FFI v1 axiom-extern](lane-experience-ffi-axiom-extern.md) — encodes the signal as `Bool extern_c` on `DAxiom` + a magic `ETodo("__kai_ffi__")` body sentinel (vs a new ExprKind needing ~15 match arms); Perceus must short-circuit on `body_is_ffi_extern`. String lifetime: load bytes → call extern → decref input AFTER.
- [#260/#261 extern "C"("sym")](lane-experience-issue-260-261-extern-c-fn.md) — the override is encoded into the SAME magic ETodo tag (`__kai_ffi__:<override>`); legacy `[<extern_c>]` is a hard migration error (no alias).
- [#352 NetDns / companion types](lane-experience-issue-352.md) — effect companion types must be compiler-injected builtin DTypes; default-handler install order is hardcoded in BOTH emit backends — both required identical or the effect segfaults.
- [#354 NetUdp / two runtime.h Int reprs](lane-experience-issue-354.md) — the load-bearing trap: two runtime.h copies with different Int reprs — stage2 boxes Ints tagged, so a stage0 `->as.i`/`->tag==KAI_INT` copy SEGVs on `0x1`; stage2 handlers MUST use `kai_is_int`/`kai_intf`.

## Tooling / LSP / CI / build infra

- [#447 LSP v1/v2](lane-experience-issue-447-lsp-v1.md) — subprocess (`kaic2 --library-mode`) over linking stage2 (crash isolation); `cons` is a builtin C symbol (don't name a helper `cons`); goto-def for stdlib-colliding names needs a two-pass scan preferring `module_origin=None`.
- [#484 library-mode def_at](lane-experience-issue-484-libmode-defat.md) — query-time AST walk with a LIFO scope stack beats an inference-time sidecar (additive, no resolver plumbing); `var name` desugars to a Handle so SVar never reaches the typed AST.
- [#575 backend-parity CI](lane-experience-issue-575-backend-parity.md) — the sweep found 23 divergences the issue knew nothing about (5 known bugs were all found out-of-band by ahu); auto-skip any fixture with a sibling `.err.expected`/`.diag.expected`.
- [#511/#520 negative-space suite](lane-experience-issue-511-negative-tests.md) — `test-negative.sh` 3 modes (compile-reject/stage1-reject/runtime-panic); `silent_contract/` quarantine keeps tier1 green while preserving the audit; cluster silent contracts by underlying gap.
- [#626 fixture categorisation](lane-experience-issue-626.md) — the "~40 negatives with no golden" claim was FALSE (all had decorative `.err.expected` exercised by nobody); ~20 skip-listed "aspirational" fixtures were live positives. Verify before deleting.
- [#569 package-mode harness](lane-experience-issue-569-package-mode-harness.md) — package name MUST be a literal directory name; `kai run <abs-dir>` fails (only relative or `.`); strip `kai:` lines before diffing goldens.
- [#658 package-aware build](lane-experience-issue-658-package-aware-build.md) — module-qualified imports needed ZERO typer change (`module_to_path` already maps dotted paths, just add `--path`); `kai build ./sub` collides with dir `sub` → place the binary in the sub-dir (Go semantics).
- [#602 #unstable](lane-experience-issue-602-unstable-annotation.md) — `DUnstable(Decl,...)` wrapper over a bool field on every ctor (~150 vs ~30 sites); kaic2 reads no `kai.toml` (driver parses `[unstable]` via awk); warning fires on `EModCall` only.
- [#603 multi-edition dispatch](lane-experience-issue-603-multi-edition-dispatch.md) — `edition_rank` string-compare; empty edition == tongariki (rank 0); cache partitioned by path `preludes-v1/<edition>/`, not folded into the SHA.
- [#681 #[doc] attribute](lane-experience-issue-681.md) — `DDoc`/`DModuleDoc` WRAPPER variants stripped post-parse into a side table (vs a `doc` slot on 6 positional ADTs, ~450 sites); invented `kai info builtins --json` via a file-less kaic2 mode.
- [kai info](lane-experience-kai-info-capa1.md) — `kai info <topic>` over `docs/info/*.md` is the version-correct reference an agent consults BEFORE claiming a form exists (the LLM-authorability bet).
- [#786 kai fmt selfhosting ratchet](lane-experience-issue-786.md) — a Tier 1 gate (`fmt(fmt(x))==fmt(x)` over the whole corpus, empty skip-list) locks in coverage.
- [remove llvm-text backend](lane-experience-remove-llvm-text-backend.md) — `emit_llvm.kai` (8575 LOC), `--emit=llvm`, the #575 parity gate, `selfhost-llvm` are GONE; parity is native-vs-C only; `cons_ok` is now always true. `kaic2 --emit=llvm` silently falls through to C.
- [release mirror pipeline fix](lane-experience-release-mirror-pipeline-fix.md) — the public-mirror push 403'd silently for 8 releases: `actions/checkout persist-credentials:true` installs an extraheader token that beats the embedded PAT when pushing to a DIFFERENT repo. When a local repro falsifies all hypotheses, read the CI log.
- [tier1 wall-clock perf](lane-experience-tier1-perf.md) — stage2 `test` is only ~half of tier1 (~247s of ~443s); `build/$name.*` artifact-name collisions are a latent `-j` race (needs per-target build subdirs).
- [stage0 quality (km)](lane-experience-stage0-quality-km.md) — invariant = kaic0's EMITTED C for stage1 is byte-identical (`cmp -s`), not kaic0 internals.

## Refactors / quality

- [Linus-A EmitCtx data-clump](lane-experience-linus-a-emitctx.md) — bundled the 5-arg emit quintet into one `EmitCtx` + an `emit_ctx_with_lcs` rebind helper; any 6th piece of emit context is now a one-field add.
- [Linus-B list_append O(n²)](lane-experience-linus-b-listappend.md) — replaced `list_append`-chaining in the `inf_scan_uses_*` walkers with a reverse accumulator + one `list_reverse`, preserving the observable order.
- [Linus-E walk-coverage O(N×decls)](lane-experience-linus-e-walkcoverage.md) — precompute one `eff_name→[ops]` map once at the coverage entry, drop the per-`EHandle` rescan.
- [variant-slot FAM C99 portability](lane-experience-variant-fam-union-c99-portability.md) — a flexible array member in a union is a silent GNU extension on Apple clang, rejected by Linux clang → CI-only red; access via a `kai_var_slots(v)` cast macro; use `clang -std=c99 -pedantic-errors` as the local proxy.
- [process-wait Int slot convention](lane-experience-process-wait-int-slot-convention.md) — `-I stage2 -I stage0` resolves `runtime.h` to stage2's copy — instrument that one. A stale variant-slot convention read slot 0 as `.ptr` → segfault.
- [units of measure m12.5](lane-experience-m12.5.md) — Abelian-group unit unification is exact only for |exponent|=1 vars; |k|>1 is a `None` stop-guard (unexercised). Reserved word `unit` collides with a record field name.

## Refuted / negative results (kept for the lesson)

- [#593 extract-raw (not shipped)](lane-experience-issue-593-extract-raw.md) — raw match-arm Int extract needs coordinated changes across 5+ passes; the emitter is the ground truth for "linear". The reason #440's unboxing gate stays open.
- [#599 branch-aware dup](lane-experience-issue-599-branch-aware-dup.md) — `ElSpread(EVar(nm))` lowers to `kai_incref` (borrow), so the linearity predicate must treat it as NON-linear; multi-iteration selfhost failures look like memory corruption (UAF), not rule violations — debug under lldb.
- [front-a RC churn (refuted)](lane-experience-front-a-rc-churn.md) — the `InferState` rebuild is 0.013% of increfs (not "millions"); the 45% RC share is distributed across the whole by-value Perceus discipline. Profile `main.kai` never `build/bundle.kai`.
- [phase1b1 borrow flip (refuted)](lane-experience-phase1b1-borrow-refuted.md) — borrowing a cons head WITHOUT unboxing-on-read is NOT a net win (the int-cache already makes the incref free); the sound version is `borrow + unbox-on-read` as ONE change.
- [drop-specialisation (reverted)](lane-experience-drop-specialisation.md) — per-tag `kai_decref_<tag>`: −1.7% at -O2, +5.4% at -O0 → below threshold, reverted. `pcs_make_drop_stmt` synthesises `ty:None` EVar nodes; type-driven dispatch needs the Ty stamped at insertion.
- [tco-dropmask regression (option B)](lane-experience-tco-dropmask-regression.md) — re-landing precise per-call-site rule-3 tripped the same glibc abort on Linux across three attempts; closed with a doc fixture, no compiler change.
- [m12.8-cleanup (negative audit)](lane-experience-m12.8-cleanup.md) — the compiler has NO hand-written structural Show/Eq/Hash to convert to `#derive` (every `dump_*` is a CLI-contract pretty-printer) — don't propose this conversion lane again.
- [option combinators (audit)](lane-experience-option-combinators.md) — 5 of 6 combinators ALREADY existed; the compiler just doesn't use them. The fix is downstream rewrite (per-module PRs), not API addition.
- [compiler-idioms audit](lane-experience-compiler-idioms-audit.md) — the "compiler vs stdlib idiom gap" framing is wrong (the compiler is MORE pipe-dense); the real debt is `Option` boilerplate. `keyword_kind` 35-arm string cascade can't be `match` until pattern-match-on-string ships.

## Docs audits (point-in-time, method reusable)

- [#521 docs audit](lane-experience-issue-521-docs-audit.md) — 107 HOLDS / 8 STALE across 12 specs; §Mutable promised universal `array_set` interception but bare prelude `array_set` BYPASSES the user handler. Method = verify every `#N` via `gh issue view`.
- [#604 docs honesty](lane-experience-issue-604-docs-honesty.md) — the drift gap is lane-close → SECONDARY docs (cache-design, honesty-targets) getting updated; doc-discipline covers catalogs but not design-doc prose claims.

## Traps from the REMOVED LLVM-text backend (apply to native — backend deleted #850, lesson survives)

- **LLVM default-handler family (#570/#582/#587)** — `llvm_emit_main_install_defaults` was hand-written one-branch-per-effect → a user `default{}` effect SIGSEGV'd under LLVM; the recurring "C table grows, the other backend forgotten" gap is exactly why native now AST-derives defaults (`kir-native-builtin-defaults`) and why backend-parity CI (#575) is load-bearing. The native backend re-learned this same lesson.
- **Slot-mask accessor drift** — `kaix_variant_arg` ignoring the variant `slot_mask` (reading a typed-Int slot as a pointer) was an LLVM-text bug that RECURS on native; route runtime-minted typed slots through `kai_variant_slot_box` like the C oracle (was `llvm-process-wait`, now a native concern).
- **Mixed raw/boxed param mask** — a UFn raw calling convention that lowers EVERY param raw (ignoring the per-param `raw_mask`) reads a boxed String param as `i64` → "field access on non-record"; thread the mask through ALL lowering sites (was `llvm-tco-mixed-param`, identical shape on native raw-scalar params).
- **Backend codegen forks duplicate, they don't share** — the LLVM-text backend duplicated 138 fns/14 types from emit_c; the standing rule (from #677-1r) is to sink the shared lowering into `emit_shared.kai` and DELETE mirrors, never crystallize divergence — applies to any future second codegen path.
