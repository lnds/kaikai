# Repair round 1 — base baf56156 (5,446 errors / 257 files)

agent ids (batch → id): front-parse abcb2c1bf7ec455de · front-resolve a84a1ecb3b3d13f9a · front-load-B a5a3f88fa7b144715 ·
surface ac4a91cae097ade7e · tools-A a1ac654a7665207f5 · tools-B aa8dcce86679c92d9 · protos-A a1142ab6350dbf2b9 ·
protos-B aba56db4eafddaf1a · protos-sat a5b3ddd5d9995a89c · infer-A af40ed86210898b33 · infer-B af8ab93a53c39691c ·
infer-C a87ee23c0f6d73704 · mono a68b4b4c18eeca52a · perceus a9c3bd76ce293818d · ownership a4b8db51d770b8947 ·
emit_c-A ad6fdaf91e7648240 · emit_c-B a9410000d560cf8d1 · emit-sat ae3e246ef53606509 · kir aeb2331b17d556418 ·
driver a18e82a57f3a327b1 · (entry-point agent a975788876179ceb3, base a185298e)

QUEUED (prompts in repair/prompts/<name>.md, launch with COMMON.md + <name>.md): front-load-A, desugar, infer-D,
typer-sat, checks, emit_c-C

## Done (branch = agent-<id>, worktree /Users/ediaz/work/src/github/lnds/kaikai.agent-<id>)
- surface: ed3f508a — 247 → 8 (cross-file). Needs-ruling: deep_outline do_pins `nursery` by id (user fn named
  nursery no longer pins); vec_match_build alias test by id; lit_mint Decimal always core id.
  Not-touched finds: emit_c.kai:5430 surface_lower_expr on re-parsed interpolation after resolver (EName reaches
  emitter); const_pattern cpat_has_homed compares written qualifier with module-origin (alias?); resume_scan
  interp_calls detects resume by substring `k(`; fvm_scalar_name duplicates scalar_ty_name.
- tools-A: 9e02c39e — 152 → 2 (type_text(tab, ty) pending typer-sat). Needs front-parse's decl_names change
  (import cycle fmt→symtab→decl_names→modules→parse). fmt_decl panics on a decl id with no table row.
  Decisions: fmt_expr_text prints EOp as `Eff.op` / `<local>.op` (old: internal key); ESym spec prints generic
  name; refinement elision keys on EName("self") (parser output only).
  Finds: EOp clash (lead renames EffectOp ctor to EffOp at merge — see API.md lead note); doc_attr impl member
  wrapped in DDoc renders empty; fmt_decl TBEffectAlias drops label type args; lint_idiom.kai:15 pf_param.
- front-parse: 48769f60 — 141 → 0. New class NCMethod (impl methods: method_sym(op, impl_scope(home, line, col)));
  qualified union member row "m.T" (modules.kai qualtype_member must re-point); nt_collect moved to
  symtab_build.kai (decls, tab, target); nt_type_spellings/TypeSpelling removed; st_build(rows, target),
  st_program(rows); PParser.names. At merge unresolved names: nt_collect importers (name_uses, name_report,
  value_scope_table), nt_type_spellings (protos.kai:522, proto_orphan), parse_program/st_program/st_build callers.
  Needs design: parse.kai extern-C checks by spelling (pending in ledger); parse.kai reads written_text (add to
  written-readers.txt); parse_interp_expr → EName after resolver; resolver must keep minted binders/ELocal.
  Finds: parse wrappers (#[doc] etc.) drop fn protocol bounds (pre-existing); te_as_row_label rejects qualified
  label; symtab_test "12 gates" text stale.
- launched after: emit_c-A ad6fdaf91e7648240, infer-D abcd74bb558bdd521, front-load-A (see task list)
  STILL QUEUED: desugar, typer-sat, checks, emit_c-C
- protos-A: 037bdd8f — >150 → 17 (all cross-file). proto_type_name(tab, te); generate_dispatchers(reg, tab);
  LawReg by (home, impl line, col). DESIGN TO DECIDE AT MERGE: (1) emitted spelling of homonymous types in
  generated symbols (`__pimpl_<P>_<T>_<op>` for two modules' `T`): one function `ref_text(tab, DrSym(s))` in
  protos.kai — make it home-spelled when contested; (2) resolver/typer must leave pre-resolve DrSym alone;
  (3) text comparisons confined: op_label_is/impl_method_op, ref_bare_text (Self, tparams), refs_same fallback,
  duplicate-impl key string; (4) derive reach (HeadTree/ImplReach/writers) keyed by head text.
  Finds: collect_proto_decls unused `ps`; proto_op_lookup dead; two derives sharing method names need scope keyed
  by protocol in tools-B.
- launched after: typer-sat. STILL QUEUED: desugar, checks, emit_c-C
- front-load-B (scope tables): 3573b601 — 181 → 9 (scope_walk hdf: (DeclRef) -> DeclRef, sw_head_same).
  TRAP: deleting a pub name another file calls blanks the census (resolve phase). es_note/es_lookup/ESp are
  TEMPORARY shims → resolve.kai drops eff_spellings, then delete.
  DESIGN FOR MERGE/ROUND 2: (1) run-time spelling of a contested effect must be derived from the effect id
  (by-id entry point for emitters missing; es_spell needs ESCtx) — else homonymous effects collide at run time;
  (2) DUse/SUse(String) need a DeclRef (use E on contested effect matches first by name); (3) Var(SymId) for a
  qualified union member `m.T` must be settled by whoever resolves union components; (4) effect aliases found
  by written name (AC/AR/RA, find_alias); (5) eff_declares_op by id only → resolver fallback chk_op_head dead.
  Finds: es_qualified_perform strips qualifier of any `m.x.f` with no effect guard (m.cfg.port → cfg.port);
  dual-slots.txt still lists EO, ESp.
- launched after: desugar. STILL QUEUED: checks, emit_c-C
- infer-A: 9ceac9e6 — 185 → 11 (cross-part). Env: EnvKey = KSym | KCtor | KOp(SymId, Written) | KLocal(Binder);
  TyEnv.tab; no name index. Stubs for others: <S>/infer-A/env_api_stubs.kai. Committed tree shows 57
  `cannot find` in B/C/D (expected until merge).
  DESIGN: (1) inf_resolve_row_arg: TyName in row-kind slot has no effect id; (2) unresolved RL(DrWritten) →
  sym_none with no fallback (resolver must settle all row labels incl. alias components); (3) pat_binds_var_ann
  `__ann` suffix on written name; (4) nullary variant-is-a-declared-type by name (top_names, tsh_tycon);
  (5) st_visible_candidates / field_hidden take text; (6) "bare Ok/Err/Some/None belongs to core" and "newest
  bare registration shadows" moved out of env → resolver / C / D own them; (7) visible diffs: candidate order,
  `mod.T` display, --dump-env one line per binding, TyRow-in-type placeholder prints empty.
- launched after: checks, emit_c-C. QUEUE EMPTY.
- protos-B: b4e2b132 — 111 → 2 (cross-file: ii_key_bucket DispHead, arg_names_match). Standalone stops at
  resolve (needs part A's imports + ref_text). Needs from protos-sat: DispHead = DhSym | DhScalar, dh_ref_is,
  dh_text, rows_with_renames, pir_sym, impl_symbol_renames(impls, tab, root), impl_renamed, ii_key_bucket.
  Callers: driver st_extend lowered.rows + validate_resolved_protocols(typed, tab, reg, df); rec_stamp
  stamp_targets derived: [SymId]; mono_front.kai:53 table; monomorph.kai:1024/1042 ty_disp_head.
  DESIGN: (1) EModCall arm of try_rewrite_proto_call = core show for `#{x}` (LEAD-TODO 5); (2) op labels text;
  (3) validate_inv_kind (b) spelling test; (4) Self / tparams by written text; (5) collect_binser_types names.
  Finds: proto_rewrite_corrected_ty with tparam return head; validate_resolved_kind lacks EIntrinsic/ESurface
  arms; derived_impl_syms lowers derived impls twice.
- launched after: emit_c-C. QUEUE EMPTY — all 26 batches launched.
- MERGED into lane (71957465 + …): front-parse, surface, tools-A, front-load-B, protos-A, protos-B, infer-A, infer-B.
- infer-B: 022cc58a — 131 → 2 (`Label.cell`, LEAD-TODO 2). DESIGN: union component order now by sym_raw (may
  print `B | A`; fix = sort at display in printer); typer reads written text where resolver left a name
  (synth_name_read, modcall_sym, sum_ctor_named, callee_type_sym, effs_declaring_op); mint_bigint_home String.
  Unconfirmed: homonym.kai HCand(SymId, Ty)/hom_choose/HPick (front-resolve), part D VariantsCheck [SymId],
  strip_label_alias removed but part D ~19118/19133 call it.
  Finds: dead rerank_* fns; try_bare_call_narrow lost capital guard; value read of EOp/local records no CallSite.
- emit_c-B: 892db93f — 173 → 0 (against stubs). MERGED. Assumed of part A (sent to emit_c-A): emit_ident_value(fid),
  const_body_of(consts, s), eff_mo(effs, s), emit_ob_names [String], emit_unw_let_owned [LocalId].
  Part C must follow: emit_lam_helpers(src, lams, tab, specs, fns, …), emit_clause_forwards(cs, effs, tab),
  effect_struct_decls(decls, tab), fn_forward_decls(decls, tab, specs, fx, capable, const_syms, entry),
  thunk_forward_decls(decls, tab, specs), rd_donatable_filter(capable, decls, tab, specs, fx),
  emit_proto_runtime_tables(decls, tab, specs, variants, fns, ids, ctors, entry).
  INTENDED C CHANGE: qualified nullary ctor in value position → kai_nullary_fast (was kai_variant_u).
  RISKS: `let` shadow test scans scope by spelling via ln_text (measure emit time); two types named `Capture`
  (emit-sat pub, spawn_capture private) — check the flat C bundle; emit_ctx_const switches cx.locals for inlined
  const bodies (binders inside interpolation fragments are not numbered).
- driver: 4a913c82 — 240 → 12 unresolved names / 158 cross-file once those exist. MERGED. Also edited cache_ast,
  cache, core_cache, user_cache (rows in blobs; cache_format_version 20; uc_lookup folds home) → EXPECT CONFLICT
  with emit-sat. Table chain: tab0 → tab1 → sym_tab → typed_tab → emit_tab. Long list of ASSUMED callee
  signatures in API.md `## driver API` (round 2: reconcile driver calls with what owners landed).
  DESIGN: ec_decl_is_cspec still `__cspec__` substring on fn_emit_name; wec_te_holds ignores DrWritten fields;
  symid-pipeline gate needs kir_eff_name to reproduce contested effect labels (Log__ra / Log__main);
  lib-mode `def` on a ctor answers the DType (goldens may move).
  CONFLICT WARNINGS: fn_emit_name defined by BOTH mono (fnreg, Written) and emit-sat (emit_shared, String);
  EFn 5 slots (mono) vs 4 (kir); register_fn_decls (perceus at base sig); collect_effect_ids [SymId] vs
  effs: [String] consumers.
- protos-sat: 07c11527 — 172 → 0 (against posted shapes). MERGED. closure_spec substitutes by LocalId; pasted
  binders renumbered local_minted(<spec>_<line>_<col>#<old>); #2435 guard + subst_scoped shadow tracking DELETED.
  Driver fixes for round 2: bounds_by_sym(ds, recs, target_mod) has NO tab (driver 5809/5816);
  gen_law_checks(decls, ps: PScope, reg, file) (driver 5278); closure_spec_pass(ds, target, tab, ids) : CSpecOut
  {decls, rows} (mono_front 46); pid_* take Written (infer.kai 10151); op_protocols_fold callback gets SymId
  (core_reach); cs_pat_binders [LocalId] (spawn_capture 231, 250).
  DESIGN: cs_decl_is_derived by spelling (`__pimpl_`, `__binser_put_`); FBR(name, mo, bounds) name-keyed in
  ast.kai → carry SymId; pipe_fusion recognises map/foldl/sum/… by label for EModCall/EName; law_checks witness
  type by name when target written; EOp via a closure param not substitutable.
  Finds: pir_ret_ty scalar head now scalar type (unboxing of those call nodes may differ); proto_orphan `[T]`
  owned by whoever declares `List`; dead ps_key/ps_visible/pk_resolve/pk_visible_from, mo_is.
- ownership: d57e01b8 (walkers) + 698323c6 — 291 → 1 (against stubs). MERGED.
  LEAD ast.kai: `CtorIntSlots = CIS(SymId, [Bool])`, `RawArmBinder = RAB(LocalId, Int, Int)` (emit_c
  build_ctor_int_slots keys by ctor id). TEMPORARY shims: ReuseCtorName / RCN / reuse_ctor_home_name (emit_c).
  BEHAVIOUR CHANGES (list in PR / check in CI): clause resumes when callee is its last-param binder (was: only
  when spelled `resume`); purity callees by exact FnId (heuristics gone); purity_traps qualified non-ctor value
  ref now clean; uo_owned qualified ctor/const operand judged by type (more operands bound); carriers by id.
  DESIGN: ub_discards / unw_blank_binder read `_` prefix from written name; interpolation spans by binder text.
  Finds: unw_call_unwinds for captured closure; nab_mentions skips EHandle; ufn_promote_calls unwired.
- ENTRY-POINT agent: 79eb9c89 on base a185298e (NOT merged; port after round 1: cherry-pick, conflicts in
  driver/infer/entry/lint_deadcode; pre-resolver `entry_spelled(name)` sites can go by id now). 4 new fixtures:
  effects/entry_only_in_root, modules/lib_main_with_user_effect, negative/effects/non_root_main_implicit_row,
  negative/modules/import_entry_module; grouped_test_builds/check.sh case 9. Finding (grouped test calling its
  own main) sent to integrador. migrate: not done (not cheap). Docs to touch at PR: docs/info/syntax.md,
  effects.md §Default handlers, testing.md.
- Integrador informed (12/26 merged).
- tools-B: 8cf0f0a2 — 192 → 0 (against posted shapes). MERGED.
  ROUND 2 fixes: protos.kai part A's two `derive_*_impl` calls → `derive_json_impl(tab, t, body, line, col) :
  Minted` / `derive_layout_impl(…)`; cell_ref_name → cell_ref_local (4 callers emit_c.kai, 1 kir_lower_walk);
  CSLoose(LooseWhy) in front-resolve; protos-B validators pass ps_tab(ps); CtrId(LocalId, …) in kir/emit_c.
  DESIGN: refinement predicates inside types never resolved (self, matches by text); habitant slots by type
  name (Matrix); derive_reach by "P/Head" text; json/layout derive call `__pimpl_Json_<T>_to_json` by text
  (homonymous types collide → LEAD-TODO 6); rv_head_sym.
  BEHAVIOUR: cell_promote now promotes a `var` shadowed inside its scope; json_shape accepts nested DrSym record.
  Finds: cmp_lt no order ESym vs EInt (pred_canon not idempotent); classify_requires_clause id coincidence if
  LocalIds per declaration; intervals match_is_total stub; region rmark_name unread.
  written_text readers for ledger: refinements, refine_help, refine_coerce, refine_enforce, refine_variance,
  kind_resolve, cell_scan, derive_mint.
- front-resolve: cbf0b94d — 239 → 0 (against stubs). MERGED. Bound = BDecl | BLocal; binders numbered from 1;
  handler-state aliases (`state`, `log`, `__var_cell`) = local_minted(key). sym_erase only renumbers site ids;
  core_unstamp identity (dead); core_stamp only stamps leftover EName builtins (dead once nobody leaves one).
  TEMPORARY shims to delete: collect_effect_names (alias), SwHead type in scope_walk.kai.
  Driver fixes round 2: check_site_ids(decls, tab) at driver ~5926; collect_effect_ids.
  DESIGN: (1) state alias vs enclosing user binder named `state`/`log` — emitters with spelled scope must use
  clause_state_aliases((key) => list_has(spelled, key)); clean fix = slot on EHandle/HC; (2) nested stateful
  handlers: inner clause's `state` = same minted id as outer's (perceus dropped "bound outside" test → would
  rebind) — CHECK; (3) `__` prefix marks compiler-synth decls (is_compiler_synth_name, rs_type_decides,
  rs_minted); (4) DEffect default blocks not walked; (7) check_row_labels written label arity by first name.
  BEHAVIOUR: EModCall(m, n) naming a const or ctor now ESym; homonym compares type head by id.
- perceus: f192ca16 — 436 → 7 (cross-file). MERGED. Tracks `Binder`; callee key FnId; ctor test
  sym_is_ctor(tab, s) (tab threaded through ~65 fns); new perceus_span.kai; changed name_ix, ret_slots,
  borrow_map, borrow_view, consume_index.
  Round 2: driver.kai:5887 perceus_pass_in(decls, tab, specs, views, root); emit_c 13190/13339
  pcs_use_index(body, scope: [Binder]), pcs_borrow_lookup(bmap, FnId); reuse_uses.kai (kir rewrote over
  [LocalId]) CONFLICTS with perceus index ([Binder], ni_bucket_id) — adjustment posted in API.md to kir.
  DECISIONS: pcs_callee_is_ctor keeps `EOp(_) -> true` for C parity (one line; probably should be false —
  decide after first C diff); every ctor needs an NCCtor row incl. Some/None/Ok/Err (CHECK first C diff);
  span matching relies on unique_binders keeping spellings unique; binders inside `#{…}` = local_minted(name).
  BEHAVIOUR: `__` prefix exemption gone; homonym matches gone; branchy_single_use by FnId; handle-alias scope
  no longer subtracts aliases bound outside (see front-resolve design 2 — nested stateful handlers CHECK).
- front-load-A: 53cd5f90 — 221 → 0 (against stubs). MERGED. pat_binders : [Binder]; pat_binder_names (old
  behaviour). Qualified union member re-pointed to sym_derive(NCCtor, "T", home); QualTyped { decls, rows }.
  Round 2: pub_access_decl.kai:254, 329 → pat_binder_names; driver 5089–5092 take `.decls` and st_extend
  `.rows` of qualtype_decls / qualtype_segmented; driver ~5307 validate_extern_fn_homes(decls, tab, df);
  emit_c.kai:9789 stdlib_head_tag_c passes cx.tab.
  DESIGN: impl_target_head_tag / ctor_owner_head_tag find decl by name parsed out of an impl symbol →
  exact answer decl_head_tag(type id, decls) via pid_impl → PIR target (change in emit_shared
  collect_pimpl_rows / emit_c); htd_respelled_root cuts `T__<root>`.
  Finds: dotted `m.T` ctor row stays in table unread; comments still say EVar.
- infer-C: 40622a6b — 156 → 0 (on merge 326e0d1d + stubs). MERGED.
  Lead at merge: infer.kai imports `compiler.local_names`, `compiler.call_site_target` in part A's import block.
  EOp built once in `op_callee`. ViaRecv = capability param AND taggable `with UserEff as x` alias.
  Assumed of checks (sent to it): CarrierShape = CS(SymId, [TypeExpr]) etc. Part D: check_pattern_stamped /
  PatChecked { st, pat }, report_ctor_ambiguity_if_any(st, Written, line, col). evidence_inject.kai:66
  spelled_head_owners(decls, tab, []).
  REGRESSION TO FIX IN ROUND 2 (owner rule: no regressions): UFCS on a LOCAL fn value (`x.f()` with local `f`)
  unreachable — typer needs "local in scope spelled <label>" (scan env locals by Binder.name: the label is an
  unresolved Written, so this is resolution, legit); same for "`f` is a function in scope, not a field".
  DESIGN: variant-record literal ctor id stamp (synth_record_lit only finds records); names handed on as text
  (pipe fallback EName(op), dispatcher fallback EName("__proto_<op>")); written_text outside diagnostics:
  pir_answers / pipe_op_impl_of op label, st_visible_candidates, modules_declaring_type, name_is_ctor.
  BEHAVIOUR: head owners by sym_contested; ctor detection by table class; rhs_is_inplace_local /
  is_array_safe_callee now live in build mode (by id); contested effect prints decl_name in unify messages.
- emit-sat: 1f13b752 — 161 → 9 (cross-file: mono 4, checks 1, infer-D 4). MERGED.
  Lead/round 2: TEMPORARY stubs at end of emit_shared.kai (build_globals, collect_op_qualified_names,
  const_names → rewrite callers to lc_new(tab, specs): emit_c part C ×2, kir_lower_fns.kai); tcl_push_binders
  takes 3 args (kir posted 2); cache_delta_test.kai pinned digests e1144957adb22459 / b52921f38738b4f5 will
  drift → re-pin from first `--cache-typed-selftest` run; Label literals in cache_delta_test (2),
  cache_module_test (2) need `cell: local_none()`; examples/editions/edition_cache_invalidation/check.sh:94
  label "c-modular KCT1" (ec_format_tag now KCT2) — check.
  Versions: cache_format_version 20, uc_typed_key_version 14, uc_key_version 2, cc_key_version 2 (both unread!),
  ec_format_tag KCT2.
  DESIGN: collect_pimpl_rows / parse_pimpl_name parse `__pimpl_…` (identity lives in ProtoIds.impls — thread
  to emitters); proto_dispatch_parts reads marker text; stdlib_head_tag_int / collect_variant_to_head by
  (home, name) → decl_head_tag(s, decls); binders inside interpolation fragments id 0 → CapLocal(local_none());
  evar_find* by (home, name) for literal "Some"/"Ok".
  BEHAVIOUR: ViaRecv(x) captured as a read of x in lambdas (old fv never captured `op#recv` receiver);
  effect default block is the identified declaration's.
  Finds: core_cache keys blob by content only while ids derive from home; main_row_labels drops written labels.
- emit_c-A: 8c426c5b — 164 → 1 (merged view). MERGED. EmitCtx gains tab, specs, locals (last); calls dispatch
  in emit_callee_call; only by-name path left: emit_spelled_call (interpolation fragments).
  Round 2: part A spells the effect at a perform from decl_name(cx.tab, r.eff) and `_kai_default_node_<name>`
  from the bare name → must use the `## effect run-time spelling` helper (emit_c-C) so contested effects
  stay distinct (they were `E__home` before); evf_thunk_slot / evf_require_walk decide by frame key string
  (FEF(String, [String]) in evidence_frame.kai — checks agent) and call part C's
  effect_has_default_block(eff_name: String, decls); mono_sum_decls(decls, tab, ctors) (part C 2 calls);
  ReuseDonor(SymId, [LocalId]); reuse_gc_uses / reuse_taken_uses 3 args (ns first) per kir's file;
  drop `import compiler.sym_read`, unused name_scope / proto_ids imports.
  DECISION: bare nullary ctor inside an interpolation fragment now spells kaiv_<Name> (only builds without core
  Show reach it) vs `#{Some(x)}` call still by name — make both by name or neither (round 2).
- desugar: 9cdc1f57 — 239 → 2 (cross-file). MERGED. dsg_written_sym(scope, cls, name) settles record heads,
  `p : T` narrow, `use E` (homonymous effects resolved via root view). DsgMinted { decls, rows }.
  Round 2 driver: refine_pred_fn_syms(decls, tab); collect_refine_alias_ids(decls) : [SymId];
  lower_pattern_narrow_decls(decls, tab, sym_views, target_mod_name, ids); lower_extern_opaques(ds,
  target_mod_name) : DsgMinted; desugar_pos_records_decls(ds, tab, sym_views, target_mod_name, df);
  desugar_use_decls(decls, tab, sym_views, target_mod_name, file); check_nursery_brand_mismatch_decls(ds, tab,
  df, c, b); driver 2449 lower_axiom_one(d, mod); st_extend with synth.rows + opaques.rows.
  TEMPORARY shims: collect_refine_alias_names, fn_decl_sym in desugar.kai.
  DESIGN: rewrite_nursery_caps_kind by written callee name "nursery" (pass should run after resolver);
  brand check by written fn name (BTR); DUse/SUse(String); single-declaration shortcut ignores visibility;
  ledger: desugar.kai reads written_text (~60 pre-resolve sites), compares text with == in 8 fns.
  Finds: use_decl_is_root only mo == None (root pub fns tagged Some(target) → file-level `use` doesn't rewrite
  them?); desugar_pos_records_decl skips DConst/DImpl/wrapped decls; nursery rewrite EHandle alias shadowing.
- kir: cfb10bee — 193 → 0 (against scaffold) + 2 real (TrmcStep.ctor, LEAD-TODO 3). MERGED (39 files).
  KPerform(SymId, [Ty], String, KVia, [KVal]); KVia = KvAmbient | KvLexical | KvAlias(String) | KvRecv(String).
  Round 2 driver: lower_to_kir(raw_decls, tab, specs, ids: ProtoIds, regions, fns, variants, target_origin,
  raw_binders, traps, stamp_ids, entry) (3 calls add `ids`); emit_kir(decls, tab, specs, regions, inline_on,
  stamp_ids, entry). reuse_gc_uses / reuse_taken_uses(ns, cargs, gc: [LocalId]) — emit_c 3800, 3801, 4324 pass
  cx.locals (perceus wanted [Binder]; reconcile). kir_eff_name / kir_eff_key / kir_op_text in kir.kai.
  DESIGN: install order = list of effect NAMES (Clock, Random, … have no CoreEffect id); `_` discard test on
  written name (rn_slot_add); alias evidence keyed by alias written name (relies on unique_binders);
  runtime globals `_kai_default_ev_<name>` / `_kai_default_node_<name>` spelled from bare effect name →
  homonym effects collide (same item as emit_c: one `effect run-time spelling` helper for both backends).
  BEHAVIOUR (homonym case only): cons reuse uses fixed cons layout (was by-name "Cons"); `!` success tag = core
  ctor id. Finds: indirect call through a local ignores an active shadow rename; pattern binders KLet without
  shadow-rename check. core_effect_id(CeState) = (NCEffect, "State", "concurrent") — agrees with the table.
- mono: fcdcf7c7 — 309 → 1 (stand-in artefact). MERGED? (see merge output). MT(SymId, tys, units);
  MonoOut(decls, SpecNames, leaks, viols); MonoFront gains specs, rows; EFn(Written, …) 5 slots;
  fn_emit_name(tab, specs, f) : Written (fnreg — emit_shared must NOT define another);
  register_fn_decls(decls, tab, specs, entry); unique_binders_decls(ds, tab).
  Round 2: driver tailfuse_decls(decls, tab, target) : TailFused {decls, rows} (no `pids`), st_extend rows;
  emit_c part C txd_plant(ns, e, drops) / txd_plant_exit(ns, e, after) with ln_of_decl(d).
  TEMPORARY entry points: fnx_qualified, fnx_kai_qualified (fn_index.kai), self_ref_name (self_call.kai);
  callers emit_c 2754, 7207 + six tcrec sites; kir_lower 462, 523; perceus 758, 3661.
  CONTESTED SPELLING RULE (adopt everywhere emitted names use a type name): `hs_spell(name, home)` when the
  type is non-root, non-core and sym_contested — mono's mangler does it; emit_c part C `mangle_ty_in(tab, t)`,
  protos `ref_text`, json/layout derive, effect run-time label must agree.
  BEHAVIOUR (homonymous generics only): every RCS naming a generic seeds a tuple (discovery order / instance
  index may differ; root's spurious spec gone); inst_spec_tys erases by id; diagnostics for contested types
  print `Item` (was `Item__da`).
  DESIGN: `#{…}` spans read binders by text (scan_uses_*, unique_binders, txd_pick); ctor_ix row keys from
  (home, name) of EV → VariantEntry should carry ctor id; live_fns reaches impl method via op label;
  FnTPBound = FTB(Int, [String]) by protocol text; ctor_stamp leaves unanswered sites written, unreported.
  Finds: same-position clones conflated by unique_binders (same id) vs closure_spec's distinct ids — CHECK.
- checks: 76cb1bd9 — 273 → 0 (scratch tree with posted shapes). MERGED.
  Round 2 callers: cont_crossings(e, tab, res, holds, needs) (infer D); core_reach(decls, tab, core_mods, reg),
  CoreCut([Decl], [SymId]), core_cut_misses(decls, tab, dropped) (driver; table must hold lowered.rows);
  build_evidence_frames(decls, tab, specs, entry) (emit_c ~9029/~11019/~11191, emit_native.kai:88);
  check_spawn_rows(df, tab, recs, mod_decls, all_decls) (infer D ~19561). spawn_capture private Capture → Carried.
  REGRESSION RISK (round 2): mutation_carriers te_names_mutating — field types inside DType are DrWritten, so
  the closure through USER records (HashSet { inner: HashMap }) is lost → Mutable masking + spawn capture
  check. Fix: settle DType payload refs to DrSym before (type_stamp / type_scope), or give it the resolver view.
  RULING NEEDED: obl_eff_instance_count keys instances by alias binder id → two handlers in different fns with
  the same alias spelling count as two (cross-instance op collision error can newly fire).
  DESIGN: writes_only_first_arg by `__binser_put_` prefix; spawn_row reads l.cell; OPR alias as LocalId →
  Option[Binder] would remove a search.
  BEHAVIOUR: core_reach ESym reaches only its id; lint_idiom no longer nudges `(x) => m.f(x)`; lint_effects /
  evidence dumps print declared effect name (no written qualifier); cont_crossing binders by id.
  Finds: obl_walk doesn't descend into lambdas; free_carriers skips EHandle; lint_core_ctor duplicates
  core_ctor_sym (export one from core_syms).
- typer-sat: e86c7111 — 313 → 0 (scaffold). MERGED (36 files). type_text(tab, t) / type_key / row_text /
  row_label_text table-first; ref_display(tab, r), ref_decl_name(tab, r); ref_spelled(r) (scalar_ty).
  Round 2 driver: dump_holes, dump_holes_json, dump_effects_json, dump_effect_holes_json, strict_holes_check,
  validate_constructor_uniqueness_decls take tab; dump_tycon_ids(decls, tab); validate_unit_refs_decls(decls,
  tab, scope, df) (driver 5259, 5359); validate_habitant_slots tab first; type_home_table(ds, tab)
  (emit_shared). Removed: ut_component_named (infer 17425), tsh_slot (infer 3452, 3629), type_text_with
  (protos 860), tsh_home_of, type_home_of.
  DESIGN: row_label_keys must use emit_c-C's `eff_rt_name` (effect run-time spelling) or label key and
  instance key disagree for contested effects; ctor uniqueness claim by (module, type name text); tycon_pat
  leaves pattern written when expected type unknown → XOdd; ty_text keys nominal as `#<id>` (unresolved = #-1).
  --dump-typed format shifts (sym#<id> name; ident <name> for locals) → goldens.
  tycon_home now only stamps arm / let patterns.
- infer-D: 071558bc — 126 → 1 (scaffold; fmt_expr_text 3 args). MERGED.
  LEAD DECISION: infer_program / infer_program_with_protos_cached / typecheck_module now `/ Console + File +
  Mutable` because TyEnv holds the SymTab (arrays) and masking no longer sees it as local → callers holding the
  table as a param/field need `+ Mutable`: typed_cut (tcut_module, tcut_infer_publish, tcut_loop,
  tcut_typecheck_program, tcut_infer_program), lint.kai:316, infer_dump (5 dump_*), infer_holes_json
  strict_holes_check, evidence.kai:449, evidence_inject.kai:220, rawsafe 793/806, emit_c 12906/12933, driver
  compile_to_module. (Alternative: table out of TyEnv.) → accept `+ Mutable` in round 2 unless it reaches main.
  infer.kai needs `import compiler.core_fns` (+ local_names, call_site_target from infer-C) in part A's block.
  check_pattern_stamped → part C's SLet should store pc.pat (still calls check_pattern).
  BEHAVIOUR: duplicate-variant-arm diagnostics reported after arms typed (order in goldens); union-variant
  coverage slightly more permissive; Fiber/Pid return allow-list by id (user fn named `spawn` no longer
  exempt; user type named Fiber no longer trips); ambiguous-ctor note lists homonymous types as two parents;
  qualified variant pattern `m.Ctor(..)` tries the qualifier's module first; verbose `row` dump loses `@cell`.
  DESIGN: pattern_first_duplicate by spelling (belongs in resolver); type-directed ctor resolution by
  st_candidates(tab, NCCtor, name) in the typer.
  Finds: part A te_ref_sym drops qualifier; part B first_bound_sym takes first table candidate (base: newest).
- emit_c-C: 7edd11c4 — 181 → 1 (EOp clash). MERGED. ALL 26 BATCHES MERGED. Lead commit 2c238272 (Label.cell,
  CIS(SymId, [Bool]), RAB(LocalId, …), TrmcStep.ctor, EffOp) + effop on emit_c.
  `## effect run-time spelling`: eff_rt_name(tab, root: Option[String], s) : String, eff_label(cx, s) in
  emit_c part C → MOVE to a neutral file (kir cannot import emit_c; it reads symtab, decl_text, core_effects,
  home_spell) and rewire: part B ev_name_of, part A perform site (inst_key_of_callee), evidence-frame keys,
  kir_eff_name, typer-sat row_label_keys, mono mangler / protos ref_text / derive for TYPES (same rule).
  Round 2 in emit_c: import home_spell; emit_trmc_step(src, s: TrmcStep, args_e, cx) (part A calls 8-arg);
  effect_has_default_block(rt: String, decls, lab: (SymId) -> String) → thread lab in evf_thunk_slot /
  emit_evf_thunk_frame; TrmcStep literals 13391/13452 fill ctor; build_ctor_int_slots CIS(sym, …) (599-600).
  Driver: emit_program*(…, specs, ids: ProtoIds, …); tcrec_rewrite_decls(…, tab, specs);
  collect_transparent_aliases(decls, tab, eff_names, target_mod_name, core_mod_names(core_mods, []));
  expand_ta_decls(decls, tab, table, aliases); expand_aliases_in_decls(decls, tab, aliases);
  mangle_core_segments(decls, tab, segs, n_core).
  DESIGN: core-module test in eff_rt_name = "module owns CeMutable/CeSpawn/CeProcess" (needs cores list in
  EmitCtx/kir); mangle_ty_in lacks mono's contested rule (--dump-mono-out); evidence frames carry no effect id;
  TcrecStep/TrmcStep params text must agree with part A's c_local_of.
  BEHAVIOUR: mangle_core_segments no longer renames DType to `mod::T` (settles refs to DrSym); ctor callee only
  ESym; desugar_op_calls probably a no-op after the typer.
  Ledger: eff_rt_name acc/hs_spell row; emit_c.kai as written_text reader.

# Round 2 — base 1902ecf7 (114 errors / 21 files)
- r2-rest: 66bce96b — 62 → 0; whole compiler 52 in driver.kai. MERGED. build_ctor_int_slots moved to
  emit_shared.kai (CIS(ctor id, flags)). `+ Mutable` on 16 public fns (tcut_*, run_lint*, dump_*…): driver must
  mask (local let table) — must not reach main.
  OPEN: contested effect loses its default-node thunk slot until frame keys (row_label_keys, FEF) are spelled
  with eff_rt_name → the "effect run-time spelling" item.
- r2-driver: agent aa9cc360c87a1fe98 (running).
- HOST BUG (kaic2-y2, maybe main): a reuse-donating fn (qh_ref) called inside a LAMBDA in a match arm is emitted without its donor arg (C: too few arguments). Worked around in qual_head.kai (named helper qh_alias_label). Report to integrador with repro once the compiler builds.

# Build phase (compiler type-checks at c0517835)
- g1 (host y2 → new source) builds and passes the kaic2-fast smoke. Self-compile bugs found so far:
  1. Typer picked the first homonymous ctor (own private `CS`/`RP` vs imported): fixed with own_first in
     name_read_sym (infer.kai). NEEDS FIXTURE (own private ctor homonym of an imported pub ctor, different arity,
     in synth position and in a lambda `match`).
  2. HOST BUG, present in origin/main (kaic2-main2) and y2, FIXED by the new compiler: a `match` nested in an
     arm whose OUTER pattern is nested (`Some(Donor(a, b)) -> if … { … } else { match e { ELocal(l) -> … } }`)
     reads the outer match's hoisted slot `_hs0` as the inner binder → wrong value → "non-exhaustive match".
     Repro: <S>/t/hoist.kai (main2/y2 panic; g1 prints yes/no/no). Scanner <S>/t/hs_scan.py found exactly one
     instance in the compiler (trmc_slot_is_donor_identity) → worked around with a named helper because CI's
     boot compiler is the release. REPORT TO INTEGRADOR (finding: miscompile on main) + add fixture.
  3. HOST BUG (y2; main unknown): reuse-donating fn called inside a lambda in a match arm emitted without its
     donor arg (qual_head.kai) → worked around with a named helper.
- kaic2-z: host built from 3a7d2e66 (has the `__` perceus fix) — available at <S>/kaic2-z.
- PR body: "fixes a C-backend miscompile present in 0.139.6 (nested match under a nested outer pattern read the outer hoisted slot)"; fixture examples/match/nested_match_in_nested_arm on both backends; keep named-helper workaround (gen1 built by release). Recorded in #2641 by integrador. Release 0.139.3 tarball local: stage2/build/boot/kaikai-v0.139.3-darwin-arm64 (C panics, native ok).

# Round 3 — base 5a7f7fa2 (compiler self-hosts: g2 = <S>/kaic2-g2; fixtures committed)
Agents (brief <S>/repair/prompts/ROUND3.md; build via <S>/bslot.sh): gate a9edf6d74fa8500c0 (names gate final +
tier0 wiring) · entry a256209d35c547b20 (root-module/entry split) · rtname a613002a5a1a7137a (one by-id run-time
spelling) · typer2 a2066dc33b963b622 (UFCS on local fn; DType field refs before mutation carriers).
INTEGRADOR's plan: as soon as tier0 passes → names gate --final + quick cost check → push + open PR (no
auto-merge) with the 4 items as pending in the checklist; push round-3 results together as ONE commit/push
after the first CI round reports.

# STOPPED HERE (usage limit) — lane HEAD = the commit above ("two modules nothing reaches are gone")
State: compiler type-checks (0 errors), builds from the release boot compiler (tier0 path) and self-hosts.
NOT pushed, PR NOT opened. PR body draft: <S>/PR-BODY.md.
tier0 (make -k tier0, log in <lane>/tier0.log): 3 gates fail, everything else OK:
 1. symid-pipeline: no line `^kperform ra__run Log__ra\.tally#[0-9]+ Log@ra$` → contested-effect run-time
    spelling (rtname agent's item).
 2. respelling-confined: emit_c 1, monomorph 1 mint a home spelling → consolidate in the one rt-name module and
    name it in the gate's allowlist (rtname agent).
 3. walker-catchall-audit: NEW: anf_ops anf_is_atom, driver decl_def_of, fnreg body_kind_is_effectful (renamed
    from _s), raw_border raw_border_owning_named, symtab_build ntc_one (moved from decl_names nt_one), tailfuse
    tf_is_atom, unique_binders ub_kind (a WALKER — check it is total, do not just baseline it). STALE to remove
    from tools/walker-catchall-baseline.txt: decl_names nt_home / nt_one / nt_type_spelling, driver
    find_decl_def_any / find_decl_def_root / find_local_at_decl, fnreg body_kind_is_effectful_s.
Round-3 agents were STOPPED mid-work (worktrees kept; resume with SendMessage to the id, or relaunch from
<S>/repair/prompts/ROUND3.md + the per-item text in the transcript): gate a9edf6d74fa8500c0, entry
a256209d35c547b20, rtname a613002a5a1a7137a, typer2 a2066dc33b963b622. Their worktrees:
/Users/ediaz/work/src/github/lnds/kaikai.agent-<id> (check `git status` there for uncommitted work).
Next: fix the 3 gates → tier0 green → names gate --final → cost check (<S>/cost.sh) → push + open PR (no
auto-merge) → merge round-3 results as one more push after the first CI round.

# RESUMED (limit lifted). Lane HEAD after walker baseline commit. Critical path to the first push:
 a. mainmerge agent ae615bcb8de5ea7d2 — merge origin/main (4 commits: #2681 perceus flat arm + slot_take.kai,
    #2679 closure_spec, #2678 parse if-without-else, 58a7cfd4 perceus holder) into the lane, port to new shapes.
 b. rtname agent (resumed) COMMIT 1 → makes symid-pipeline + respelling-confined gates pass.
 c. walker-catchall-audit: DONE (baseline updated, gate OK).
 d. cost check: building <S>/kaic2-mainpure (pure origin/main, host kaic2-z) as base for <S>/cost.sh.
 Then: merge a + b, tier0, push, open PR (no auto-merge), report push time + first CI result to integrador.
Round-3 agents resumed: gate, entry, rtname, typer2 (same ids).
- typer2 DONE + MERGED: 947b5d16 (UFCS reaches a local fn value; order field → module fn taking the receiver →
  innermost binder → root fn → builtin; fixtures examples/ufcs/local_*, field_wins_over_local, …) and b59c76cc
  (mutation carriers through user types: CarrierShape = CS(SymId, [SymId], Bool), sets built once in tc_shared,
  TyEnv.carriers; fixtures examples/negative/mutable/*, negative/spawn_capture/capture_nested_record,
  effects/nested_record_local_masks_mutable, 5 namespace-collisions carrier_* + 3 matrix rows). Measured C
  backend by the agent; native/tier0 not run.
  Divergence from release (deliberate): a local named like a fn exported by several modules is now called via
  UFCS (release: "bare name exported by multiple modules" error). → PR body.
  OPEN: spawn capture uses narrower set (records only) than masking — as before; same DrWritten-core-only
  pattern in refine_variance rv_head_sym, effect_instance_canon:144, tailsig:92, tycon_pat:200.
- COST (measured, alternated x3, base = pure origin/main 2490193b self-built <S>/kaic2-mainpure2, new = g2):
  instructions 422.9–423.2 G vs 718.6–719.4 G (−41 %); cells 4,657,009 vs 5,403,361 (−13.8 %); wall 23.8 s vs
  36.7 s; RSS 1.9 GB vs 3.7 GB. No cost to recover.
- gate agent DONE + MERGED (60bf75f6, 85f9b6ed): gate.py rewritten (5 checks), wired into tier0 NON-final;
  `--final` blocked by 148 rows in tools/symid-census/pending-written.txt (span 34, marker 27, by-name 25,
  csym 16, effect-key 12, refine 7, tparam 6, regs 6, mint 6, c-local 4, discard 4). evar-above-erase gate
  retired. Fixes: written_eq_check was blind; law_checks `List` homonym (fixture
  examples/sugars/shape_kind_laws_list_homonym.kai). After EVERY merge: run `sh tools/symid-names-gate.sh` and
  add/adjust rows in written-readers.txt (keyed by file+fn) — needs a kaic2 built from the current source.
  Agent's decision items: reason L; R after the typer (stamps); csym cluster → E?; `"#{w}"` interpolation is a
  second exit from Written (Show impl).
  Finds: examples/perceus/trap_mid_derived_binserialize.kai does not compile on the C backend (nullary ctor as
  record field value emitted as a local `kaiv_Boom`) — with g2 too → CHECK (regression? release?);
  kir_lower_walk ~2979 user binder `state`/`log` disables alias; kir_lower ~858 user effect spelled like a
  builtin install-order name; ffi_v2 extern-type registry name-keyed; unwind ub_discards `_x`.
- Integrador informed (cost, gate, pending list → owner's call on merging with the pending list).
- Building <S>/kaic2-g3 from the lane (typer2 + gate merged), then full names gate.
- entry agent DONE + MERGED (c96d99de; conflict in infer.kai tc_shared resolved: roots + carriers both).
  uc_typed_key_version 15; guard_iface digests a row-less main's body. OWNER DECISION PENDING (report):
  solo runner builds — the test file's `main` still holds the entry id; on native a test block calling its own
  `main()` in a SOLO run crashes (exit 137 / pthread_create failed) on release 0.139.6 too (pre-existing).
- FIX (lead): `type Boom = | Boom` — value read of a nullary variant spelled like its own type emitted
  `kaiv_Boom` (old emitter's by-name ctor fallback is gone) → typer stamps ESym(ctor) (unbound_ctor_read);
  fixture examples/unions/self_named_variant_value. (Matching on it is rejected on main too — not ours.)
- OWNER DECISION (via integrador): PR does NOT merge with pending rows. Each of the 148 must be CONVERTED to id
  or PROVEN legitimate (no id exists or can exist at that point); `--final` is the merge gate. Likely
  legitimate, to argue: spans ONLY if the interpolation body is not yet resolved (else resolve to expr);
  csym tables ONLY if key is emission spelling and identity settled upstream. NOT legitimate by default:
  markers (27) → id or typed marker; by-name with id in hand (25) → id; effect keys (12), KIR regs (6),
  mints (6) → ids. Send proof list to integrador. Still push ASAP once main merged + tier0 green.

# Round 4 — pending list to ZERO (owner: convert, or send a proof to the integrador BEFORE moving a row;
# accepted proof: type-directed constructor resolution with 3 conditions). Brief: <S>/repair/prompts/ROUND4.md.
# Base 1786ee93 (root fix for `type Boom = | Boom`: variant spelled like its own type is a ctor).
Agents: tnames a174285481484482e (typer by-name + mint + tparam + ctor-resolution conditions; owns infer.kai) ·
span a51bf0fb085846470 (interpolation bodies become expressions; 35 rows) · markers a2a1cdb9919715c8c
(marker 27 + refine 7). TO LAUNCH after rtname + mainmerge land: emit-keys (csym 16, effect-key leftovers,
regs 6, c-local 4, discard 4, by-name in ctor_ix / emit_shared / ffi_v2 / head_tag_decl).
Still running from before: rtname a613002a5a1a7137a (commit 1 = two tier0 gates), mainmerge ae615bcb8de5ea7d2.
- rtname DONE + MERGED: 174e26e2 (rt_name.kai: rt_name(tab, s), rt_ref_name; SymTab gains root + cores;
  respelling-confined gate rewritten → OK) and 8c4abd23 (generated symbols spell a contested type by id;
  conflict in protos.kai with the gate's dt_name resolved: DerivedType has NO name fields; dt_name(dt) =
  declared, dt_rt_name(dt) = rt_name). Agent measured (C): namespace-collisions + 3 effects dirs 417→435 pass,
  0 regressions; 26 still fail same before/after (list in <S>/rtname/kai-after.txt).
  Emitted-name corners that differ from pre-change: a type whose only homonym is an effect-row alias is now
  `T__home`; `__ref_pred_<T>` keeps the declared alias name. → PR body.
- mainmerge DONE + MERGED: 9e4907a7 (origin/main incl. 0.139.7; #2676/#2678/#2679/#2681 ported; CbArrayTake;
  closure_spec keeps the id mechanism, main's by-name capture guard NOT taken; evar-above-erase gate deleted).
  Lane is 0 behind origin/main at 2f934b9c. Native not verified (CI).
- Ledgers reconciled (143 pending). Lane tip 9b8679f6.
- ekeys agent ab90aa3f59ed3b944 launched (csym, effect-key, regs, c-local, discard, emitter by-name) on 9b8679f6.
- OPEN before push: symid-pipeline gate's UFCS section (ufcs_callee_identity `scale@main` count; contest_cut
  "cached typed blob" premise is obsolete: a UFCS callee ALWAYS carries its id now) — building g6 to look.

# PUSHED 2026-10-10 14:45 (Chile) — PR #2682 https://github.com/lnds/kaikai/pull/2682 (NO auto-merge).
Lane tip at push: 3e6812cc. tier0 green locally (boot from release). Pending rows 143; `--final` is the merge
gate. Round-4 agents running: tnames, span, markers, ekeys. Next: report first CI result to integrador; burn
CI failures down; merge round-4 results, one push per CI round.
- OWNER (via integrador) after push: (a) solo `kai test` — FIX IN THIS PR: the runner is the entry in every
  test build; file's `main` never holds the entry id; fixture solo on both backends; native verified by CI →
  entry agent a256209d35c547b20 RESUMED on the lane tip for a second commit. (b) spawn-capture narrower set:
  pre-existing, NOT in this PR (recorded in #2641).
- CI watcher: background task `ciwatch.sh 2682` (reports when the first round ends).

# CI ROUND 1 (push 14:45, 3e6812cc): red tier1-shard-1, -3, -7, asan(a) [logs only when run completes].
- shard-7 = diags-json parity unknown_type: FIXED locally, uncommitted → type_scope_check.kai returns
  [Diagnostic] (ts_and), driver `ty_scope_vd = vd_render(...)` + `vd_exit(json,[ty_scope_vd])`. Build g7 +
  `make test-check-parity` in background task bko3oc8rj. Commit after it passes.
- span agent a51bf0fb085846470 DONE (not merged): commits 0c02fd39, b86355c4, d9c6c1dd, 125b8ca5 on 1786ee93;
  35 → 1 rows (ub_text re-caused c-local). Lift total before resolver; text machinery deleted. Hunks in
  emit_c.kai / kir_lower_fns / kir_lower_walk / infer (comment) / protos (1 arm). Decisions sent to integrador:
  (1) `#{}` in string pattern rejected at parse (breaking) vs literal; (2) const block initialiser double read
  in interpolation regresses on C (pre-existing const-not-through-Perceus). WAIT for answer before merging? No:
  merge it; adjust per decision afterwards.
- Told tnames about EName-survives-typer fixtures (match_narrowing_basic, disambig_type_effect_bare).

# CI ROUND 1 RESULT (finished 15:22, RED 14 checks). Logs: /private/tmp/claude-501/-Users-ediaz-work-src-github-lnds-kaikai/8fce2374-7a27-4f29-86e1-754689fede16/scratchpad/ci2682/*.log
Lane tip c899918d (diags-json fix + span merged). Owner: NO REGRESSIONS; one push for the round.
Routing:
- tnames a174285481484482e: ambiguity lost (neg_names_candidates, neg_import_order_cannot_decide,
  neg_ambiguous_fn_bare, neg_ambiguous_ctor_bare/_payload, ctor_same_payload(+_nullary)); wrong rung
  (packages/builtin_import `reduce`); shape_fusion_1200 pipe on root type; neg_ctor_wrong_arity_homonym
  qualified spelling; positives type_union_member_qualified, dot_local_shadows_effect, dot_module_named_bit,
  protocol_op_dispatcher. Baseline-3 on main (not regressions): neg_selective_missing_symbol,
  adv_alias_shadows_module, disambig_type_effect_bare (measured with kaic2-mainpure2).
- derive agent a30dd6325ee796047: issue-1852 cross-module derive json, derive_qualified_module_protocol,
  m12_8_x_derive_eq_sum_no_impl (fmt_property's 2 uncovered = these, not compiling).
- genleak agent a18289403dadee211: segments build_your_own_generator leak grows per run (C asan 29→57,
  native 20→39).
- span agent a51bf0fb085846470 RESUMED: owner decisions (1) interpolated string pattern = compare against
  interpolated value on both backends, no parse rejection; (2) const initialisers through Perceus + KIR names.
- entry agent a256209d35c547b20 resumed earlier: runner is the entry in every test build.
- ME: digest re-pin in cache_delta_test.kai (computed: iface f7d02fcb4be01afa, folded b1befc767b1e3cc8) —
  WAIT for local tier0 to finish before editing compiler sources; rc-leak pin impl_method_borrowed_arg 0:0
  (written, uncommitted); core_effect_under_root_handler(+_scalar) expectation → "effects.Log" (written,
  uncommitted; tell integrador: diagnostic now qualifies the contested effect).
- Native CAN be built locally: PATH="/opt/homebrew/opt/llvm/bin:$PATH" LIBRARY_PATH="/opt/homebrew/lib" make KAI_LLVM=1 kaic2
- 16:0x: local tier0 GREEN on c899918d. Committed: digest re-pin + leak pin + 2 goldens. Entry agent's second
  commit f567dc7a MERGED (b05251ef; runner is the entry in every test build; C entry symbol is `kaiu_main`;
  non-entry root main = `<target>__main`; fixtures examples/test-runner/own_main*.kai). 0 type errors, 109 pending.
- NATIVE BUILD running in the lane tree (background bkqzdbadr; log <S>/native-build.log) → then
  test-runner-own-main-native. DO NOT edit compiler sources in the lane tree until it finishes.
- Integrador accepted: routing; "effects.Log" qualified only when contested.
- 15:57 NATIVE compiler built in lane tree from b05251ef; `test-runner-own-main-native` OK (measured).
  `make -k -C stage2 KAI_LLVM=1 test-native-wrapper` running (bn4m8a9m1; log <S>/native-wrapper.log).
- markers agent a2a1cdb9919715c8c DONE: d9b32138, c138bff1, e060a9fc on 1786ee93. MERGED in scratch worktree
  <S>/mwt (branch symid-merge-r2, on b05251ef): conflicts marks/protos/symtab/symtab_move/pending resolved,
  rt_name_test NEnt rows + NmSource; 0 errors; pending 77. → FAST-FORWARD the lane to symid-merge-r2 once the
  native wrapper run ends (don't change lane sources mid-run: make would rebuild).
  Markers decisions to relay: (1) contested dispatcher in generic code fails at C compile (pre-existing);
  (2) `fn pos(x: Int where x > 0)`; pos(0) now compile error (was runtime panic); (3) 2 expr_struct_eq rows:
  NOT a proof (ids can exist: type predicates once at TyRefine→TyRefineT) → convert after tnames (infer.kai);
  (5) thin baseline 77→75 not lowered, nscol baselines; (6) cache_format_version 20→21.
  Looked wrong: nested handlers `state` reads own state (pre-existing; fv_clause_aliases by spelling);
  resolver spelling tests (`__` prefix: chk_hidden, is_compiler_synth_name, rs_type_decides, rs_minted);
  emit_native_term.kai:83 tests `__pimpl_` on a KIR symbol; x__ann/x__slot binders enter scope by name.
- Integrador 16:1x: ACCEPTED `pos(0)` compile-time rejection (PR body, done in draft); refine proof not
  forwarded (convert after tnames); two pre-existing items recorded in #2641. PR-BODY.md draft updated.
- 16:2x: native wrapper suite local = only FAIL segment-handles-native build_your_own_generator (20→39, same
  as CI; genleak agent). Lane FAST-FORWARDED to 6f167ad2 (markers merged; pending 77). Native rebuild + new
  fixtures on native + namespace-collisions(-neg)-native running: background bu4tq9vbq (logs
  <S>/native-build2.log, <S>/native-nscol.log). Told ekeys + tnames the tip moved (they merge at hand-back;
  tnames also gets the 2 expr_struct_eq rows; ekeys gets ub_text + emit_native_term.kai:83).
- Running agents: tnames a174285481484482e, ekeys ab90aa3f59ed3b944, span a51bf0fb085846470 (owner
  decisions), derive a30dd6325ee796047, genleak a18289403dadee211, "Convert refine group rows"
  a8f4f1a330c94400d (the markers agent's fork — its commit c138bff1 is already merged; ignore unless it reports
  something new).
- 16:17 native rebuilt from 6f167ad2 (measured): new fixtures OK on native (interp_show_own_protocol,
  adv_fn_named_like_impl, protocol_op_dispatcher, impl_table_op_with_underscore, state_alias_capture_scope,
  const_interp_show, self_named_variant_value, nested_match_in_nested_arm). namespace-collisions-native 4
  failing (baseline 2): type_union_member_qualified, dot_local_shadows_effect, dot_module_named_bit [tnames],
  derive_qualified_module_protocol [derive]. neg-native 7 (baseline 3): neg_ambiguous_fn_bare,
  neg_import_order_cannot_decide, neg_names_candidates, neg_ctor_wrong_arity_homonym [tnames] + 3 baseline.
- genleak DONE + MERGED (06d83461): root cause unwind_clauses.kai `unw_abandoning` — a segment handle's
  forwarding clause (`__seg_k($seg_request…)`) counted as resuming once compared by LocalId (main judged it
  non-resuming by a spelling accident "resume" vs "__seg_k") → effect classed as not unwinding → frames
  release nothing on discontinue. Fix: a clause making a `$seg_request` (EIntrinsic InSegRequest) is not
  resuming. Agent measured C + native: build_your_own_generator 29,57 → 1,1 (C), 20,39 → 1,1 (native); also
  gen_finally_on_drop, gen_pipeline, stateful_discontinue. New fixture examples/segments/discontinue_frame_values.
  RESUMED for: stream_shout_roundtrip leaked 1,1 (main) vs 2,2 (lane, constant); classify other diffs; run the
  COMPLETE C rc-leak-gate (+ native if time).
- 16:45: span agent's owner-decision commits MERGED: 587e9abf (interpolated string pattern = binder + equality
  guard via interp_pattern.kai; both backends agree), b306358e (const_init.kai: a const initialiser with
  locals/lambda becomes fn `__kai_const_<NAME>`; NEnt row marked NmHelper by me). Lane tip = commit after
  af91b108 ("the constant's initialiser function is a helper row"). 0 errors, 77 pending.
  Validation running in background bzbt34e99: native rebuild (<S>/native-build3.log), new fixtures both
  backends, test-segment-handles-native + own-main-native (<S>/native-seg.log), then `make -k tier0`.
  Reported to integrador incl. pre-existing findings (const effect row discarded; heap String scrutinee vs
  literal patterns leaks; `let "a3" = v` untested; test bodies simple Perceus).
- STILL RUNNING: tnames (typer ladder + CI regressions 1,3,5 + rows), ekeys (emitter rows), derive (CI 2,4),
  genleak follow-up (stream_shout_roundtrip +1, full rc-leak-gate C/native).
- BEFORE PUSH (round 2): all agents merged; errs 0; tier0 green; native build + namespace suites at/below
  baselines; update PR body (gh pr edit 2682 --body-file <S>/PR-BODY.md); lower baselines that dropped
  (tests/namespace_matrix_thin_baseline.txt 77→75?, tools/baselines/nscol/*); report push time.
- 17:0x: validated 11d822e9 (measured): native build OK; new fixtures OK on both backends
  (string_pattern_interp, const_block_initialiser, const_interp_show, discontinue_frame_values,
  build_your_own_generator, const_block_homonyms); test-segment-handles-native + own-main-native OK; tier0 red
  only at namespace-matrix (native 4 vs 2, neg-native 7 vs 3 — tnames/derive items).
- derive agent a30dd6325ee796047 DONE + MERGED (91c59566; lane ff to fad6dac9 + fixture move commit):
  derive generator by id — core protocol → own generator; other protocol → the structural generator (Show,
  Eq, Ord, Hash) whose core protocol declares exactly the same ops (label+arity); BinSerialize/Layout/Json only
  for their own core protocol. Reach validation by ids. Json: field heads settled before the derive reads them.
  OWNER DECISION NEEDED (sent): the shape rule is new surface (`#[derive(Same)]` for a user protocol with
  eq/2 now works) and narrower in 2 corners (user protocol NAMED BinSerialize/Layout/Json no longer picks that
  generator; user protocol with a core name but other ops → "no derive generator").
- impl_table_op_with_underscore moved to examples/stdlib (protocols recipe has no stdlib path).
- PRE-EXISTING (main too, measured with mainpure): a protocol declared in the ROOT file, dispatcher taken as
  a value → "panic: no impl of Power.pow_int for runtime head N" (builtin or user head). → #2641 via integrador.
- genleak follow-up DONE + MERGED (752fbd80: fixture examples/effects/clause_continuation_dropped_on_one_path).
  stream_shout_roundtrip 1,1→2,2 is NOT a lost reference: main marked every fn named `map` live by bare name
  (stream.map → option.map/result.map "run"), registering reuse tags {Some,Ok,Err}; by id they never run, so
  the runtime interns one immortal Ok cell (2 fewer allocs per run, +1 live at exit, growth 0). Agent proved it
  by patching the tag table in emitted C. COMPLETE leak gates at 06d83461 (agent, boot-built C and native,
  704/704): only FAIL = perceus/shape_fusion_1200 BUILD-FAIL (tnames).

# 17:45 — tnames a174285481484482e DONE + MERGED (lane tip 2ba6bc85, ff). 0 errors; PENDING 52
# (csym 16, effect-key 8, tparam 6, regs 6, c-local 5, by-name 5, discard 4, refine 2).
- `infer.kai settle_name` is THE one resolving place (conditions 1-3). CI items 1,3,5 fixed (agent, C).
  nscol baselines: c 2, neg lowered 3→2. Remaining 2+2 pre-existing on main (modules.kai rqc_kind etc).
- Decisions sent to integrador: D1 two order-based picks remain (later_core for char/char_unicode,
  option/result collect; newest dispatcher with unknown receiver); D2 proofs proposed: tparam 6 rows (my view:
  convertible via TBinder/TyParam ids, ~157 sites), lint_idiom callee_named 1 row; D3 refine 2 rows need a
  state-free settle pass over predicates at typer entry (convert next); derive rule (structural vs generator
  name) still awaiting owner.
- VALIDATION of 2ba6bc85 running: background boe9tscca (native build → native nscol/own-main/segments →
  C nscol/packages/protocols/stdlib/shape-fusion/issue-1852 → tier0 → check-parity + fmt-property). Logs:
  <S>/native-build4.log, r2-native.log, r2-c.log, tier0.log (lane tree), r2-parity.log.
- IF GREEN → PUSH ROUND 2 (don't wait for ekeys ab90aa3f59ed3b944): update PR body
  (`gh pr edit 2682 --body-file <S>/PR-BODY.md`: pending count 52, checklist, behaviour changes from tnames,
  derive rule, const/pattern fixes, leak fix), `git push`, report push time to integrador, start ciwatch.
- Agents still running: ekeys only.

# 18:20 — validation of 2ba6bc85 (measured, native build local): nscol C 2/349 @baseline 2, neg 2/115 @2,
# native 2/341 @2; packages/protocols/shape-fusion/issue-1852 OK; check-parity 296; tier0 red only by stale
# walker baseline (fixed); fmt-property 1 fail (minted local id position-derived → harness normalises; fixed).
- Lane tip a9a24ed8: + fmt harness/walker baseline commit (1afe8df9) + sugars recipe stdlib path commit.
- KEY FINDING: CI round 1 covered little — each shard stops at its first failure; test-sugars never ran. Local
  `test-sugars` on the merged tip: 5 FAIL → 2 harness (import decimal w/o --path; fixed in recipe), 3 diagnostic
  texts in infer.kai display (`@core.real_to_string`, `list.map`→`map`, `n.loud`→`.loud`) → tnames.
- tnames a174285481484482e RESUMED: record-literal head by expected type (stdlib/row_kind_homonym_core_pair)
  + the 3 sugars diagnostics + run whole test-sugars / records / unions / modules-qualified.
- SWEEP agent acda8bf01042a51ad launched: full `make -k tier1-shard-1..8`, tier0, asan a/b in its worktree,
  per-fixture enumeration, classify vs pure main (<S>/mainpure-wt), FAILURES.md at <S>/sweep/, fix
  regressions outside infer.kai / emitter files. PUSH ROUND 2 WAITS for sweep phase 1 + tnames follow-up.
- NOTE for integrador: commit 8f871502 (earlier in the lane, BREAKING footer) made `Decimal` without
  `import decimal` an error and `type Foo = MkFoo` (undeclared) an error — listed in PR body, accepted earlier.
- ekeys ab90aa3f59ed3b944 still running (emitter rows).

# ROUND 2 PUSHED 18:05 (Chile) — tip a9a24ed8 (integrador asked: push now + keep local sweep). PR body updated.
# (Earlier STATUS/time notes around "17:45/18:20" were ~15 min ahead of the real clock.)
- Known red in this tip: stdlib row_kind_homonym_core_pair; sugars m12_6_uom_refinement,
  pipe_map_genuine_mismatch, contract_method_effectful_rejected (all tnames follow-up).
- CI watcher: background bvm0k8fde (`ciwatch.sh 2682`).
- Running agents: tnames follow-up (a174285481484482e), sweep (acda8bf01042a51ad), ekeys (ab90aa3f59ed3b944).
- NEXT PUSH (round 3) = sweep fixes + tnames follow-up (+ ekeys if landed) + CI round-2 failures.
- Owner decisions pending (with integrador): derive rule; later_core + newest-dispatcher picks; tparam proof;
  lint row; Decimal-without-import.

# ~18:30 — ekeys ab90aa3f59ed3b944 DONE + MERGED locally (lane tip 6ffa95a2; NOT pushed). 0 errors. PENDING 9:
# tparam 6 (proof with owner), refine 2 (convert: mine), lint 1 (proof with owner).
- ekeys could NOT run native. VALIDATION running in lane tree: background b46q67aqw (native build5 →
  nscol native + own-main + segments (<S>/r3-native.log) → test-native-wrapper (<S>/r3-wrapper.log) → tier0).
  DO NOT edit lane sources until it ends.
- Proposals sent to integrador for owner ruling: P1 ub_text as emitter read; P2 type_text ref_decl_name (two
  typer-phase callers compare (home, type name): infer_type_collisions.constructor_decl_type,
  infer_unit_habitant.dim_base_head_name → I convert); P3 dual slots VariantEntry, PimplRow; P4 absorption of
  user Ok/Some/None/Err by core rows (spelling fact via homonyms).
- TODO mine after validation: emit_native_ops.kai `eff == "Ffi"` by text → id; the 2 refine rows + P2 callers
  (needs tnames to finish with infer.kai first); native repro of alias-register clash
  (<S>/ekeys/t/alias1.kai, alias2.kai) on main vs lane.
- Tell sweep agent the tip moved when it reports (it merges at hand-back).
- RULE (integrador, owner tracks a bet by these times): EVERY time stated in a message must come from running
  `TZ=America/Santiago date +%H:%M` in that same turn. Never estimate a clock time.

# CI ROUND 2 (push 18:05, a9a24ed8) finished 18:42: GREEN native (shards, selfhost, rc-leak), tsan, asan a/b,
# mn-corpus, tier0, rc-detector, shards 2,3,4,6,8. RED: shard-1 (test-compiler-intervals: stale unit test →
# FIXED by me, committed), shard-5 (independence-oracle lane1: --dump-typed-core path "cannot find tag" →
# tnames, repro sent), shard-7 (test-negative prelude_call_anchors_at_user: callee display → tnames).
# Logs: <S>/ci2/*.log. (written at 18:43)
- Local validation of ekeys merge (6ffa95a2, native built 18:20): nscol native 2/344 @baseline, native wrapper
  suite exit 0, tier0 green.
- Lane tip now: ekeys merge + intervals unit-test commit. NOT pushed.

# ROUND 3 PUSHED 18:44 (Chile, from date) — tip c42311cc (ekeys merged + intervals unit test). PENDING 9.
- CI watcher: background bqf315pbk. Known red in this tip: independence-oracle lane1, test-negative
  prelude_call_anchors_at_user, (if reached) stdlib row_kind_homonym_core_pair + 3 sugars callee-display.
- Running: tnames follow-up (a174285481484482e), sweep (acda8bf01042a51ad). Both merge the lane tip at hand-back.
- Mine next (when tnames releases infer.kai): refine 2 rows (state-free settle pass over stored predicates),
  P2 callers (infer_type_collisions.constructor_decl_type, infer_unit_habitant.dim_base_head_name),
  emit_native_ops.kai `eff == "Ffi"`; native repro of alias-register clash (<S>/ekeys/t/alias*.kai) lane vs main.
- 18:45: measured ekeys' alias probes (lane vs release 0.139.7): alias1 (nested same-name aliases) C 21 / native 11
  on BOTH lane and release (pre-existing native miscompile); alias2 (alias named like a param): release rejects
  at compile time; lane accepts, C 42, NATIVE fails at run time "type mismatch in *" → MUST FIX (lane compiles
  what native miscompiles). ekeys ab90aa3f59ed3b944 RESUMED: native alias registers by LocalId, KInstall
  resolved registers, hdecl_add / captures by id, emit_native_ops Ffi by id, verify its native paths with a
  native build; report size of per-LocalId C identifiers (P1) but do not do it.

# CI ROUND 3 (push 18:44, c42311cc) finished 19:19: 29 green; RED only tier1-shard-5 (independence-oracle
# lane1) and tier1-shard-7 (test-negative: 300 PASS, 1 MISS prelude_call_anchors_at_user) — both known, both
# with tnames follow-up. Native (incl. ekeys merge) all green: shards 1/1b/2, selfhost, rc-leak; asan, tsan,
# mn-corpus, tier0, shards 1,2,3,4,6,8. Logs <S>/ci3/.

# 20:04 — INTEGRADOR DECISIONS (within owner's rules; owner may veto; don't wait):
- derive: A (structural, as merged) — list the two (−) cases in PR body. [me]
- Decimal/undeclared types: B — numeric carriers' module in the CORE SET so Decimal etc. are declared and
  reachable on the core rung by id; revert the 7 fixtures' added `import decimal` and the
  `type Foo = | MkFoo` rewrites if only needed for that. NO regression. [agent "carriers"]
- D1(i) duplicate core exports: no stdlib edit; core-module priority = explicit documented rung order by module
  id (order the core set lists them), only between core modules; bare value read stays a tie error. [tnames]
- D1(ii) op in several protocols, unknown receiver: no "newest"; an explicit unresolved-protocol-op node with
  candidate ids, settled by the dispatch rewrite when the type is known; never known → ambiguity error. Measure
  corpus; if a compiling program breaks, tell integrador BEFORE landing. [tnames]
- D2 tparam: IN THIS PR — type parameters get binders with minted ids; Self one per protocol. [agent "tparam"]
- D2 lint callee_named: ACCEPTED legitimate (diagnostic-only naming convention) → move row to ledger. [me]
- D3 refine: convert. P2: dumps/diagnostics legit; convert the two typer checks. [tnames or me]
- P1: convert — one C identifier per LocalId (<name>_<local id>) after the native alias fix. [ekeys]
- P3: REJECTED — no name slot beside an id: remove VariantEntry/PimplRow name slots; emitted names from a
  table keyed by id at emission; KFn.sym too if same shape. [ekeys]
- P4: keep (core row by id); language question goes to owner separately.
- Push each round as soon as locally green; one push per round.
- 20:06 DISPATCHED: ekeys ab90aa3f59ed3b944 (already on native alias registers) + P1 convert (one C identifier
  per LocalId) + P3 (remove VariantEntry/PimplRow/KFn.sym name slots); carriers agent ad8036765407892ad
  (numeric carriers' module in core set; `type T = X` undeclared → one-member sum; revert fixture edits);
  tparam agent a67abc4bcb101986c (TBinder/TyParam ids, Self per protocol; widest change); tnames
  a174285481484482e told: hand back current follow-up ASAP, then second hand-back D1(i), D1(ii), D3 refine, P2.
  Sweep acda8bf01042a51ad still running. Lint row moved to written-readers (D=1): PENDING 8 (lane 0b46e603).
- PR-BODY.md draft updated (derive (−) cases, pending 8, checklist). When carriers lands: drop the "type name
  nothing declares is rejected / type T = X" behaviour-change bullet (or narrow it to what main also rejects).
- User asked "¿Qué decisión?" at ~20:06 — answered with the list of rulings (integrador decided on his behalf;
  he can veto).

# 20:36 — tnames FIRST follow-up MERGED (lane tip 282c8656): record head settled like any name (expected type
# first, else ladder over records; typer lays fields in declared order — fixed a field-order MISCOMPILE the
# desugar caused when it read the head differently), callee display "qualify only when contested", n.loud,
# TyperIn = one set of typer inputs for every driver mode (fixes independence oracle). Agent measured C on the
# merged tree: oracle OK, test-negative 301/0/0, sugars 0 FAIL, stdlib 0 FAIL, nscol at baseline, records,
# unions, modules-qualified 0 FAIL. Goldens whose text moved (uncontested callee bare / list.map): 7, passing.
- tnames RESUMED for second hand-back: D1(i), D1(ii) (measure corpus, stop+report if anything breaks), D3, P2,
  qualified positional/spread record head, expected type threaded to block tail + other call paths.
- VALIDATION of 282c8656 running: background b03ewne13 (native build6 → native nscol + wrapper → tier0 →
  oracle/negative/parity → sugars/stdlib/intervals). If green → PUSH ROUND 4.

# ROUND 4 PUSHED 21:29 (Chile, from date) — tip 282c8656 (typer follow-up + lint ledger). PENDING 8.
- Measured before push (native built 20:44): tier0 green, oracle OK, test-negative 301/0/0, parity 296,
  nscol native 2/344 @baseline, native wrapper exit 0. Expect round 4 fully green.
- CI watcher: background brs23gcjm. (Stray wait task br3ae59z8 stopped: its pgrep matched other agents' makes —
  never wait on `pgrep -f "make …"` while agents run builds; read the task's own output instead.)
- In progress: tparam a67abc4bcb101986c, carriers ad8036765407892ad, ekeys ab90aa3f59ed3b944 (alias regs, P1,
  P3), tnames a174285481484482e (second batch), sweep acda8bf01042a51ad. (noted at 21:29)

# 21:34 — SWEEP DONE (acda8bf01042a51ad): the complete -k tier1 on a9a24ed8 (C, per-target) = 7 failing fixtures in
# 5 targets, ALL already fixed in round 4 (intervals unit test; oracle lane1; prelude_call_anchors_at_user; the
# three sugars texts; row_kind_homonym_core_pair). Everything else green on C: light pool 291/294, shard fixed
# work, namespace axes at baseline (incl. modular), tier0, 81 ASAN legs, tsan gates. NATIVE sweep: 178 targets
# pass, parity ratchets 357/0, 444/0, 293/1 (the Pair fixture), native selfhost gate, M:N determinism 2180
# pairs 0 findings. Table: <S>/sweep/FAILURES.md.
- Its two commits NOT merged (f8464b76 dup of my intervals fix; 1fc133f9 superseded by tnames' TyperIn). I took
  only its gate: `test-report-modes-imports` (9 modes incl. --dump-mono, --dump-call-elision) in stage2/Makefile
  + light.txt (sorted). Committed on the lane (not pushed).
- Noted by sweep: neg-native baseline could drop 3→2 (macOS only; confirm on CI); `examples/fmt/op_signature.*`
  use HashMap without import (no gate compiles them).

# 21:48 — OWNER AUDIT (via integrador): every existing fixture whose SOURCE was edited only to keep passing must be
# restored to main's exact text and pass unmodified. Keep only edits justified by an accepted behaviour change;
# PR body gets a table: fixture | change | accepted decision. List of 31 modified/deleted existing files:
# <S>/fixtures-modified.txt. → carriers agent ad8036765407892ad owns restore + table (message sent).
- When carriers lands: paste its table into PR-BODY.md (new section "Existing fixtures this PR changes"),
  re-run `git diff origin/main --name-status -M --diff-filter=MDR -- examples demos tests/*.tsv` to confirm.
- 21:51 CORRECTION (integrador): do NOT make `type T = X` a one-member sum — per `kai info syntax` it is an ALIAS;
  0.139.7 silently accepts an alias to an undeclared type when only used as a type = the bug. The 10 `= | X`
  fixture edits + 3 undeclared-type goldens are JUSTIFIED and stay (table row: "alias to an undeclared type
  (bug)"). Only improve the help to name the declared alias. The 7 `import decimal` still get restored via
  carriers in core (decision B). Forwarded to carriers agent ad8036765407892ad.
  PR body: keep the bullet "a type name nothing declares is rejected", reword: alias to undeclared type was
  silently accepted; Decimal needs no import (core).

# CI ROUND 4 (push 21:29, 282c8656) finished 22:04: ALL GREEN — 31 pass, 0 fail (1 skipping = mn-corpus
# build-native by design). PR MERGEABLE / CLEAN. NOT merging: merge gate `--final` not met (8 pending) and the
# owner's audit items (fixture restore, tparam, P1/P3, D1, D3, P2, carriers) are in progress. No --auto.
