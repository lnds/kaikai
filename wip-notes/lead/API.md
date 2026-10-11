decl_map.kai  decl_map_named_bodies  : (d: Decl, f: (String, Expr) -> Expr / e)  =>  (d: Decl, f: (FnId, Expr) -> Expr / e)  -- the callback gets the owning function's FnId; `fnid_none()` for a test/bench/check/const body (was ""). Caller: deep_hoist.kai dh_body. (agent ownership)
infer.kai  rec_pick  : (rs: [RecInfo], decl: SymId, target: String, arity: Int, probe: String) : Option[RecInfo]  =>  (rs: [RecInfo], decl: SymId) : Option[RecInfo]  -- by id only (= rec_find); the name/arity/field-probe fallbacks are gone. Callers in infer parts C, D. (agent infer-B)
infer.kai  rec_find_arity  : (rs, target: String, arity: Int)  =>  REMOVED  -- call rec_find(rs, sym). (agent infer-B)
infer.kai  SumInfo  : SI(String, [String], [Variant], Option[String])  =>  SI(SymId, [String], [Variant], Option[String])  -- the sum's declaration id in place of its name; print with decl_name(tab, s). Readers: infer parts C, D, cache_delta.kai suminfo_key, cache_module_test.kai. (agent infer-B)
infer.kai  sum_find  : (ss: [SumInfo], target: String) : Option[SumInfo]  =>  (ss: [SumInfo], s: SymId) : Option[SumInfo]  -- by declaration id. Caller in infer part D. (agent infer-B)

## infer env API

Owner: infer-A (part A of `infer.kai`). The environment binds by identity; it has NO name index. Everything below is
defined in part A; write your calls against it.

Types
```
#[derive(BinSerialize)]
pub type EnvKey = KSym(SymId)            -- a declaration: fn, const, builtin (`builtin_sym(name)`)
                | KCtor(SymId)           -- a constructor, by the constructor's own id
                | KOp(SymId, Written)    -- an effect operation: the effect's id + the op label
                | KLocal(Binder)         -- a local binder (never in a cached module's entries)
#[derive(BinSerialize)]
pub type TyEntry = { key: EnvKey, scheme: TyScheme }          -- was { name: String, scheme }
pub type TyEnv = { entries: [TyEntry],           -- newest first, locals included (hole scope / `core_len` / `toplevel_len` unchanged)
                   fresh, unions, constructors, head_owners, row_kind, ty_homes,   -- unchanged
                   tab: SymTab,                  -- NEW: the program's symbol table; the typer reads it as `st.env.tab` / `env.tab`
                   index: NameIndex[TyEntry],    -- KSym / KCtor under their id, KOp under the effect's id
                   vindex: VariantIndex[TyEntry],-- constructors by sum id and by ctor id (unchanged)
                   locals: NameIndex[TyEntry] }  -- KLocal by binder id
                   -- `sindex` and `SymEntry` are GONE.
pub type TyEnvParts = TEP([TyEntry], NameIndex[TyEntry], VariantIndex[TyEntry])   -- unchanged shape: (entries, index, vindex)
```
A `TyEnv { … }` literal outside part A (infer.kai ~6620, 6657, 8325, 12963, 20072, 20104 at the base) must stop listing
every field: write `TyEnv { ...env, fresh: n }` / `TyEnv { ...ty_env_empty(), fresh: n }` / `TyEnv { ...env, head_owners: hos }`.

Lookups (all `fn` in infer.kai; the `pub` ones are marked)
```
pub fn env_sym(env: TyEnv, s: SymId) : Option[TyScheme]
      -- the scheme of the declaration / constructor / builtin `s`. Replaces `ty_env_lookup(env, name)` for a
      -- top-level name, `ty_env_lookup_qualified(env, mod, name)`, `ty_env_lookup_sym`, `ty_env_lookup_fn_id`,
      -- `ty_env_lookup_ctor_sym`, `ty_env_lookup_ctor_in_home`. An `ESym(s, _)` is typed with it.
fn env_local(env: TyEnv, l: LocalId) : Option[TyScheme]
      -- the scheme of the local binder `l` (`ELocal(l)`). Replaces `ty_env_lookup(env, name)` for a local,
      -- `ty_env_binds`. None for an unnumbered id (`local_none()`).
fn env_op(env: TyEnv, eff: SymId, op: Written) : Option[TyScheme]
      -- the scheme of operation `op` of effect `eff`. Replaces `ty_env_lookup(env, "Eff.op")`,
      -- `op_lookup_either`, `ty_op_key_sym`, `ty_op_key_name` (all four are gone). From an
      -- `EField(base, op_name)` pass `written(op_name)`; from an `OpRef r` pass `r.eff, r.op`.
fn env_has_effect(env: TyEnv, eff: SymId) : Bool
      -- some op is registered for `eff`. Replaces `is_effect_in_env(env, eff_name)` (part C: delete it).
fn env_ctors(env: TyEnv, sum: SymId) : [TyEntry]
      -- the constructors of the sum `sum` declares, newest first (= `vix_chain(env.vindex, sum)`); each key is `KCtor(c)`.
pub fn env_key_sym(k: EnvKey) : SymId
      -- the id a key holds: the declaration / constructor / effect; `sym_none()` for a local.
pub fn env_key_text(tab: SymTab, k: EnvKey) : String
      -- how a key prints (dumps, hole scope, completions): `decl_name(tab, s)`, `Eff.op`, the binder's name.
      -- Replaces every read of `e.name` for display.
pub fn env_key_ord(k: EnvKey) : String
      -- a stable text key for sorting / hashing serialised entries (cache_delta `tyentry_key`): no table needed.
```
Binds
```
fn env_bind_local(env: TyEnv, b: Binder, sc: TyScheme) : TyEnv
      -- replaces `ty_env_add(env, name, sc)` for EVERY local: a param (`p.binder`), a pattern binder, `SVar`,
      -- a handler alias, `HR`'s binder, a clause's continuation. A temporary the typer mints:
      -- `env_bind_local(env, binder_minted(key), sc)` and read it as `ELocal(local_minted(key))`.
fn env_bind_sym(env: TyEnv, s: SymId, sc: TyScheme) : TyEnv
      -- replaces `ty_env_add_sym`, `ty_env_add_fn`, `ty_env_add_qualified`, `ty_env_add_sym_only`.
fn env_bind_ctor(env: TyEnv, ctor: SymId, sc: TyScheme) : TyEnv
fn env_bind_op(env: TyEnv, eff: SymId, op: Written, sc: TyScheme) : TyEnv
fn ty_env_prepend_batch(batch: [TyEntry], entries: [TyEntry], ix: NameIndex[TyEntry], vx: VariantIndex[TyEntry]) : TyEnvParts
      -- the `homes` argument is gone (part D `tc_shared_extend`: drop it). The indexes are rebuilt from each entry's key.
```
GONE with no replacement in the env (they searched by spelling): `ty_env_lookup`, `ty_env_add`, `ty_env_lookup_qualified`,
`ty_mod_key`, `ty_ctor_home_key`, `ty_bucket_key`, `ty_env_bare_is_sym`, `ty_bare_count`, `ty_names_builtin`,
`ty_env_modules_exporting`, `ty_env_uppercase_names`, `ty_entry_*`, `ty_decl_sym`, `OpBase` / `op_base_of`.
A name the TYPER itself must settle (a UFCS method `x.f(…)`, an unsettled `EModCall(m, f)`, a pipe convention's
`map`, `Type(args)` through `#[constructor]`, a constructor written bare that only the expected type decides, the
"did you mean" lists) is turned into ids through the symbol table and then looked up by id:
`st_candidates(env.tab, NCFn | NCCtor, name) : [SymId]`, `st_module(env.tab, mod)` + `st_lookup(env.tab, cls, name, owner)`,
then `env_sym(env, s)`. Which module declares `s`: `sym_owner(env.tab, s)`. Two helpers stay in part A for this:
```
type TyCandidate = { scheme: TyScheme, sym: SymId }          -- was { scheme, mod: String }
fn ty_env_collect_candidates(env: TyEnv, target: String) : [TyCandidate]   -- every FUNCTION spelled `target` the env holds, newest first
fn st_visible_candidates(st: InferState, nm: String) : [TyCandidate]       -- the ones the file under inference can reach
fn env_ctor_names(env: TyEnv) : [String]                     -- replaces `ty_env_uppercase_names` (the unknown-variant hint)
```
What part A registers, and under which key:
- a `DFn(_, fid, …)` under `KSym(fnid_sym(fid))`; a `DConst(_, s, …)` under `KSym(s)`; a builtin / native prim under
  `KSym(builtin_sym(name))` (`core_syms.kai`);
- a sum's `Var(c, _)` under `KCtor(c)`; the runtime's `Some` / `None` / `Ok` / `Err` under
  `KCtor(sym_derive(NCCtor, name, core_type_home()))`. ONE entry per constructor: there is no bare / `Type::Ctor` /
  `home::Type::Ctor` triple any more, so "is the bare slot the core's" is a question for the resolver / the table;
- an effect's `EOp(op, …)` under `KOp(effect id, written(op))`.
Part-B types part A constructs (please define them so):
- `OpEffArity = OEA(String, Int)`  =>  `OEA(SymId, Written, Int)`  (effect id, op label, the effect's tparam arity).
Part-A types that changed (other files read them):
- `OpToEff = OEFP(String, String)`  =>  `OEFP(Written, SymId)`  (op label, the effect declaring it)
- `HeadOwnerEntry = HOE(String, [String])`  =>  `HOE(SymId, [String])`  (the type's id, the modules that spell it) -- driver `build_head_owner_map`, evidence_inject `spelled_head_owners`, typed_cut `hoe_chunk`
- `RowKindEntry = RKE(String, [Int], SymId)`  =>  `RKE(SymId, [Int])`
- `FieldRec = FRec(String, [FieldDecl], Option[String], SymId)`  =>  `FRec(SymId, [FieldDecl], Option[String])`
- `HandleFrame = HF(String, SymId, [String], [String], Int, Int)`  =>  `HF(SymId, [String], [String], Int, Int)`
- `Use = U(String, Int, Int, Bool)`  =>  `U(LocalId, Int, Int, Bool)`; `inf_last_use_for(nm: String, uses)` => `inf_last_use_for(l: LocalId, uses)` -- emit_shared, rawsafe, unbox
- `UnionInfo = UI(String, [Ty], Int, Int)`  =>  `UI(SymId, [Ty], Int, Int)`  (the union's declaration)
- `CtorEntry = CtorE(String, String, String)`  =>  `CtorE(SymId, SymId)`  (the constructed type, the `#[constructor]` fn) -- cache_delta `ctorentry_key`

Type printers (part A) take the table first; every caller passes `st.env.tab` / `env.tab` / the pass's `tab`:
```
pub fn inf_ty_to_string(tab: SymTab, t: Ty) : String                                   -- was (t)
pub fn inf_ty_to_string_ow(tab: SymTab, owners: [HeadOwnerEntry], t: Ty) : String      -- was (owners, t)
pub fn inf_tycon_display_ow(tab: SymTab, owners: [HeadOwnerEntry], s: SymId) : String  -- was (owners, cname)
pub fn scheme_to_string(tab: SymTab, sc: TyScheme) : String                            -- was (sc)
pub fn dump_env(env: TyEnv) : Unit                                                     -- unchanged (reads env.tab)
fn inf_ty_string_list(tab: SymTab, ts: [Ty]) : [String]
fn inf_row_to_string_suffix(tab: SymTab, r: Row) : String
fn inf_mangle_ty(tab: SymTab, t: Ty) : String
fn inf_ty_head_sym(t: Ty) : Option[SymId]      -- replaces `inf_ty_head_name(t) : Option[String]`; a scalar answers its core id (`scalar_ty.kai`)
pub fn build_ty_env(decls: [Decl], tab: SymTab) : BuiltEnv                             -- was (decls); driver MEnv passes its table
```
(More part-A signature changes are appended below this section as they land, one line each.)
infer.kai  inf_ty_head_name  : (t: Ty) : Option[String]  =>  (tab: SymTab, t: Ty) : Option[String]  -- CORRECTION to the section above: it STAYS, for display and for a registry still keyed by a written scalar name; `inf_ty_head_sym(t: Ty) : Option[SymId]` is added beside it and answers the nominal head's id (`List` = `core_ty_sym(CtList)`), None for a scalar (a scalar has no id) -- infer-B/C/D

## effect op callee
(agent infer-C; produced by the typer in `infer.kai` part C: `synth_op_call_with_scheme_keys`, `synth_op_call_provided`. Consumers: evidence*.kai, kir_lower*.kai, emit_c.kai, emit_shared.kai fv_*, spawn_capture.kai, unwind_calls.kai, mutation_callees/all_demands_local.)
A settled perform is `ECall(f, args)` with `f.kind = EOp(OpRef { eff, op, via })`. There is no other form: the old `EName("Eff.op…")` leftover for an effect with no id is gone (every effect has an id).
- `eff`  : the effect DECLARATION's id (never an alias, never a type alias: `Console.print` settles to the component effect that declares `print`). `Mutable` ref sugar (`c.get()`/`c.set(v)` on a `Ref`) gives `core_effect_id(CeMutable)`.
- `op`   : `written(<op label>)`, the bare label as the effect declares it — no `@`, `#`, `!` or any other tail ever. For a Reader-bound alias the `get` of `@cap` is already remapped to `ask` (as before).
- `via`  : the old key tail, as the local it named:
    old `"Eff.op"`        -> `ViaAmbient`
    old `"Eff.op@a"`      -> `ViaAlias(a's LocalId)`   — ONLY for a taggable `with State/Reader as a` cell (a `var`): the by-id dispatch (`kai_alias_<a>_id`, closure capture `__alias_id__<a>`). `a` is the `Binder.id` of the `EHandle` alias slot.
    old `"Eff.op#x"`      -> `ViaRecv(x's LocalId)`    — the evidence node is the value the local `x` holds. As before this covers BOTH a capability parameter `x: Eff` AND a taggable `with UserEff as x` alias (non-cell effect): the old key spelled both `#x` and no consumer told them apart. `x` is the param's / alias binder's `Binder.id`; its C/KIR name is `ln_text(locals, x)`.
    an alias whose tag is suppressed (inside a lambda; a capability alias inside a clause body) -> `ViaAmbient`, exactly where the old key was bare.
- `f.ty` : unchanged. `Some(TyCon(eff, row_args))` (the instance, for `inst_key_of_callee`) on the ambient/alias paths; `None` on a capability-PARAMETER perform (`synth_op_call_provided`), as before.
- Nothing the old key carried is lost: it had exactly these three shapes. The multi-shot/handle-shadow `!` was never the typer's (kir_lower `perform_op_tag` adds it to its own op tag).
- The ROW LABEL's `@alias` cell tag (`"State@i"`, `label_eff_with_alias`) is a different thing (parts A/B, `Label`); it is not on the callee.
- A read of the alias/receiver local is NOT a separate `ELocal` node: a pass that needs "this expression uses local x" (free variables, captures, liveness) must read `via`.

## front-parse API (parser, decl_names, symbol table) — owner: agent front-parse

The parser mints every declaration and constructor id and returns the table rows; nobody recovers a name by walking declarations.
```
parse.kai  parse_program  : (file: String, src: String, toks: [Token]) : PParser / Console  =>  (file: String, src: String, toks: [Token], home: String) : PParser / Console
      -- `home` is the string this file's declaration ids are derived with: the name the loader tags the file's
      -- `module_origin` with (core file: `module_name_of_path(path)`; imported module: `module_home(mod_path, cores)`),
      -- the build target (`target_mod_name`) for the root file. A caller that only inspects shapes (migration scans,
      -- `import ?hole` candidates, guard_iface, law_checks) passes the same string it would tag with, or the file's module name.
parse.kai  PParser  : { p, decls, fn_bounds }  =>  { p, decls, fn_bounds, names: [NEnt] }
      -- `names`: one row per declaration and per sum constructor the file declares, in declaration order, all homed at `home`
      -- (exactly what `nt_collect` derived from a tagged file). Empty when `decls` is None. Concatenate the rows of every
      -- loaded file and build the table from them: `st_program(rows)` / `st_build(rows, target)` / `st_extend(tab, rows)`.
      -- The decl cache must store a file's rows beside its decls (a cache hit has no parser): `NEnt` / `NCls` derive BinSerialize.
```
Ids the parser puts in the nodes (all `sym_derive(cls, name, home)`, `symtab_build.kai`; same key => same id as the table row):
- `DFn(pub, fnid_decl(sym_derive(NCFn, name, home)), …)`, `DConst(pub, sym_derive(NCConst, name, home), …)`,
  `DType(pub, sym_derive(NCType, name, home), …)`, `DEffect(pub, sym_derive(NCEffect, name, home), …)`,
  `DProtocol(pub, sym_derive(NCProtocol, name, home), …)`, a sum's `Var(sym_derive(NCCtor, ctor, home), tys)`.
- `DAxiom` / `DKind` / `DUnit` / `DExternType` keep their name string; their rows are `NEnt(NCFn | NCKind | NCUnit | NCExternType, name, home, pub)`.
  Whoever lowers a `DAxiom` to a `DFn` mints `fnid_decl(sym_derive(NCFn, name, home))`: the row is already there.
- AN IMPL METHOD (`fn op(…)` inside `impl P for T { … }`) is not a function of its module's namespace and gets a row of the NEW class
  `NCMethod`: `DFn(false, fnid_decl(method_sym(op, scope)), …)` with `scope = impl_scope(home, impl line, impl col)`, row
  `method_ent(op, scope)`. `sym_name(tab, fnid_sym(fid))` is the op label, `sym_owner` / `sym_home` the impl's module, and two impls'
  `show` are two ids. A pass that BUILDS a `DImpl` (derive) mints its methods the same way (any key unique in the module:
  `impl_scope_keyed(home, key)`) and returns `method_ent` rows; the protocol lowering reads the op label with `sym_name` and mints the
  lowered function `sym_derive(NCFn, mangled, home)` + `NEnt(NCFn, mangled, home, false)` as before. `DImpl`'s trailing `SymId` is left `sym_none()` by the parser.
- Everything written is text: `EName(written(n))`, `EModCall(written(m), written(n))`, `binder_written(n)`, `ref_written(n)` / `ref_qualified(q, n)`
  in `TyName` / `RL` / `ERecordLit` / `EHandle` / `PVariant` / `PVariantRecord` / `DImpl`'s protocol; a tuple is `DrSym(tuple id)` (`tuple_type_ref`).
```
decl_names.kai  NCls  : … | NCModule | NCBuiltin  =>  … | NCModule | NCBuiltin | NCMethod   -- an impl method before the protocol lowering
decl_names.kai  NEW  nent_sym(e: NEnt) : SymId                         -- the id a table gives the entry
decl_names.kai  NEW  impl_scope(home: String, line: Int, col: Int) : String, impl_scope_keyed(home: String, key: String) : String
decl_names.kai  NEW  method_sym(op: String, scope: String) : SymId, method_ent(op: String, scope: String) : NEnt
decl_names.kai  nt_collect  : (decls: [Decl], target: String) : [NEnt]  =>  MOVED to symtab_build.kai: nt_collect(decls: [Decl], tab: SymTab, target: String) : [NEnt]
      -- the rows `tab` holds for what `decls` declares, in stream order (names come from the table; a declaration `tab` does not hold contributes nothing).
      -- Callers (name_uses, tycon_scheme_home, name_report, value_scope_table): add `import compiler.symtab_build`, pass the table.
decl_names.kai  nt_protocols  : (decls, target)  =>  MOVED to symtab_build.kai: nt_protocols(decls: [Decl], tab: SymTab, target: String) : [NEnt]
decl_names.kai  nt_type_spellings / TypeSpelling  =>  REMOVED -- no declaration is respelled any more: a type is its id; its written name and home are
      -- `sym_name(tab, s)` / `sym_home(tab, s)` (proto_orphan.kai OrphanScope, protos.kai:522).
decl_names.kai  no longer imports compiler.modules / compiler.home_spell (the parser imports decl_names).
symtab_build.kai  st_build  : (decls: [Decl], target: String) : SymTab  =>  (rows: [NEnt], target: String) : SymTab
core_syms.kai  st_program  : (decls: [Decl], target: String) : SymTab  =>  (rows: [NEnt]) : SymTab   -- core builtins + rows
      -- callers that built a table from a bare `[Decl]` (resolve.kai resolve_program, perceus.kai perceus_pass, driver early_tab) take the rows or the table from their caller.
symtab.kai  NEW  st_ent(t: SymTab, s: SymId) : Option[NEnt]            -- the row of `s` as an entry (class, name, home, exported)
```
handler_alias.kai  clause_continuation  : (params: [Param]) : Option[String]  =>  (params: [Param]) : Option[Binder]  -- the clause's last param as its binder: a post-resolve caller reads `.id`, a pre-resolve one (resume_decls / resume_segment / resume_segment_state, which run before the resolver) reads `.name`. (agent surface; owner front-resolve)
modules.kai  pat_binders  : (pat: Pattern, acc: [String]) : [String]  =>  (pat: Pattern, acc: [Binder]) : [Binder]  -- the binders a pattern declares, as binders: a post-resolve caller (emit_c rv_*) reads `.id`, a pre-resolve one (resume_split stmt_taints, before the resolver) reads `.name`. (agent surface; owner front-load-A)
infer.kai  QUESTION infer-C -> infer-B (please answer here with the exact shapes; part C constructs / calls all of these):
      (1) the CELL TAG of a row label: old `Label { eff: label_eff_with_alias(eff_n, row_alias), ty_args, sym }` ("State@i") — `Label { sym, ty_args }` has no slot for it. What does part C build for an op call through a `with Eff as a` alias (it holds: effect id, row args, the alias binder's `LocalId`)? Same question for `OPV(eff, esym, ids)` / `st_push_op_payload_vars`, from which part C (`pin_via_op_vars`) rebuilds that label.
      (2) `AliasBinding = AB(String, String, Bool, SymId)` and `lookup_alias` / `lookup_alias_sym` / `lookup_alias_taggable`: part C calls `lookup_alias_taggable(st.aliases, l: LocalId) : Option[Bool]` (None = `l` is no alias) and `lookup_alias_sym(st.aliases, l: LocalId) : SymId`; it no longer calls `lookup_alias`.
      (3) `CallSite = CS(String, …)`: part C writes `CS(<callee's SymId>, line, col, fresh, unit_fresh, None)` (UFCS / pipe narrowing: the picked function's id).
      (4) `record_op_res(r, key: String, alias_opt: Option[String], tagged, line, col)`: part C calls `record_op_res(r, eff: SymId, op: String, alias: Option[LocalId], tagged: Bool, line, col)`.
      (5) `DFd(recv, field, res, line, col, in_pat)`, `rec_instantiate(st, ri)`, `fty_find`, `field_hint`, `check_pfield_visible(st, ri, fnm, line, col)`, `st_unify*`, `st_add_label`, `st_unify_dedup_label`, `clause_cont_in_lambda(in_clause, ps)`: called with their base signatures.

## tools-A API (formatter, doc tool, migrate, mutate) — owner: agent tools-A

These tools run on PARSER OUTPUT; the one value they take is `tab: SymTab`, built from the file's own rows:
`st_of_entries(parsed.names)` (symtab_build.kai). A printer reads a declaration's name with `decl_name(tab, s)`.
fmt.kai  fmt_program  : (decls: [Decl], cmts: [Cmt], toks: [Token], src: String, width: Int) : String  =>  (decls: [Decl], tab: SymTab, cmts: [Cmt], toks: [Token], src: String, width: Int) : String  -- driver.kai MFmt, MFmtCheck, run_migrate_scanned: tab = st_of_entries(parsed.names) (agent tools-A)
fmt.kai  fmt_expr_text  : (e: Expr) : String  =>  (e: Expr, tab: SymTab, locals: LocalNames) : String  -- prints `ESym(s, _)` as decl_name(tab, s), `ELocal(l)` as ln_text(locals, l), `EOp(r)` as `Eff.op` (ViaAmbient) / `<local>.op` (ViaAlias, ViaRecv). `locals` must name every binder `e` reads (the enclosing declaration's `ln_of_decl(d)`, plus `ln_with_expr(ns, e)` for a predicate declared elsewhere). Callers: refinements.kai, refine_coerce.kai, refine_help.kai, infer.kai (rv_text, reject_predicate_effect), call_site_check.kai (agent tools-A)
fmt_writer.kai  fmt_init  : (cmts, toks, src, starts, blanks, closes, width) : FmtSt  =>  (cmts, toks, src, starts, blanks, closes, width, tab: SymTab, locals: LocalNames) : FmtSt  -- FmtSt gains `tab`, `locals`; NEW fmt_sym_text(s, sym), fmt_ref_text(s, r: DeclRef), fmt_local_text(s, l). File in nobody's batch; only fmt.kai calls fmt_init (agent tools-A)
doc_attr.kai  DocItem  : { kind, name: String, line, is_pub, doc, sig: Option[String] }  =>  { kind: String, line: Int, is_pub: Bool, doc: Option[String], decl: Decl }  -- the item keeps its declaration; name and signature are read when the JSON is rendered, so `doc_strip_collect` / `doc_strip_all` / `doc_item_of` / `doc_for_line` keep their signatures and need no table (agent tools-A)
doc_attr.kai  doc_item_json  : (it: DocItem) : String  =>  (it: DocItem, tab: SymTab) : String  -- (agent tools-A)
doc_attr.kai  doc_module_json  : (module: String, module_doc: Option[String], items: [DocItem]) : String  =>  (module: String, module_doc: Option[String], items: [DocItem], tab: SymTab) : String  -- driver.kai:2731 (builtins JSON), 5651 (--doc-json): tab = the table of that file's rows (agent tools-A)
doc_attr.kai  doc_sig  : (params: [Param], ret: Option[TypeExpr], eff: RowExpr) : String  =>  (params: [Param], ret: Option[TypeExpr], eff: RowExpr, tab: SymTab) : String  -- driver.kai lib_decl_to_completion (agent tools-A)
doc_attr.kai  doc_te  : (t: TypeExpr) : String  =>  (t: TypeExpr, tab: SymTab) : String  -- driver.kai lib_decl_to_completion (agent tools-A)
doc_attr.kai  imports compiler.written, compiler.symtab, compiler.decl_text  -- legal only once decl_names.kai stops importing compiler.modules (front-parse's change above): at the base it closes the cycle parse -> doc_attr -> symtab -> decl_names -> modules -> parse (agent tools-A)
mutate.kai  mutate_sites  : (decls: [Decl]) : [MutSite]  =>  (decls: [Decl], tab: SymTab) : [MutSite]  -- the site's scope spells the enclosing fn / const name. driver.kai MMutateList, MMutateListJson, run_mutate_apply: tab = st_of_entries(parsed.names) (agent tools-A)

## infer state API (part B of `infer.kai`: `InferState`, its side tables, labels) — owner: agent infer-B

`InferState` gains no field: the symbol table is `st.env.tab` (part A's `TyEnv`). Side tables defined in part B, by identity:
```
type CallSite          = CS(SymId, Int, Int, [Int], [Int], Option[String])   -- was CS(String name, …): the instantiated DECLARATION's id (an `ESym(s, _)`'s `s`).
                                                                             -- Part D `ResolvedCS` should follow (`RCS(SymId, …)`); its readers are monomorph / emit_c / inst_index.
pub type OpEffArity    = OEA(SymId, Written, Int)                            -- effect, op label, the effect's tparam arity (as part A builds it)
pub type OpPayloadVars = OPV(SymId, LocalId, [Int])                          -- was OPV(String "Eff@cell", SymId, [Int]): effect, cell (`local_none()` = no cell), live row-arg tyvar ids
pub type OpResolution  = OPR(FnId, Written, [SymId], SymId, Option[LocalId], Bool, Int, Int)
                         -- was OPR(fn_name, op, cands: [String], chosen: String, alias: Option[String], tagged, line, col):
                         -- enclosing fn (`fnid_none()` until part D `infer_decl` stamps it), op label, the effects declaring the op, the chosen effect,
                         -- the alias BINDER the call went through, tagged, line, col. Readers: evidence.kai, evidence_inject.kai, cache_module_test.kai (print with decl_name(tab, _)).
pub type AliasBinding  = AB(LocalId, Bool, SymId)                            -- was AB(alias name, eff name, taggable, eff id): alias binder id, taggable, the effect. Also built by emit_shared.kai (AM map).
pub type SumInfo       = SI(SymId, [String], [Variant], Option[String])      -- (above)
```
Functions (part B) whose signature changed; callers in parts C / D:
```
fn st_push_alias(st, alias: LocalId, esym: SymId)                             -- was (st, alias: String, eff: String, esym)
pub fn lookup_alias(aliases: [AliasBinding], l: LocalId) : Option[SymId]      -- was (aliases, name: String) : Option[String]; the effect the alias binder `l` names
pub fn lookup_alias_sym(aliases: [AliasBinding], l: LocalId) : SymId          -- was (aliases, name: String)
fn lookup_alias_taggable(aliases: [AliasBinding], l: LocalId) : Option[Bool]  -- was (aliases, name: String)
fn st_record_op_res(st, op: Written, chosen: SymId, alias_opt: Option[LocalId], tagged: Bool, line, col)   -- was (st, op: String, chosen: String, alias_opt: Option[String], …)
fn record_op_res(r: Inferred, eff: SymId, op: Written, alias_opt: Option[LocalId], tagged: Bool, line, col) : Inferred   -- was (r, key: String "Eff.op", alias_opt: Option[String], …)
fn effs_declaring_op(oe: [OpToEff], op: Written) : [SymId]                    -- was (oe, op: String) : [String]
fn st_push_op_payload_vars(st, esym: SymId, cell: LocalId, ids: [Int])        -- was (st, eff: String "Eff@cell", esym, ids)
fn st_push_inst(st, cs: CallSite)                                             -- unchanged; build `CS(sym, line, col, fresh, unit_fresh, None)`
fn synth_sym_read(st, sy: SymId, e: Expr)                                     -- was (st, sy, nm, e)
fn synth_local_read(st, l: LocalId, e: Expr)                                  -- NEW: an `ELocal(l)` read (replaces synth_name_read(st, nm, e), which is gone)
fn rec_pick / sum_find                                                        -- (above)
```
Row-label CELL (the old `"State@i"` suffix `label_eff_with_alias` put in `Label.eff`): two instances of one effect bound by `with Eff as a` /
two `var`s are two labels. The identity of a cell is its alias BINDER's `LocalId`. `Label { sym, ty_args }` has no slot for it, so
**ast.kai `Label` NEEDS a third field `cell: LocalId`** (`local_none()` = the label of no cell) -- request to the lead; every other
`Label { sym: …, ty_args: … }` literal then adds `cell: local_none()` (infer A 640/657/1834/4059, C 11037/11463, D 20120, core_effects, fnreg,
monomorph, tycon_home, cache_delta*, cache_module_test). Part B is the only reader of the slot, through:
```
fn label_in_cell(esym: SymId, ty_args: [Ty], cell: LocalId) : Label    -- the label of effect `esym` in `cell`; part C `synth_op_call_with_scheme_keys` builds its row label with it
fn alias_cell(alias_opt: Option[LocalId]) : LocalId                    -- replaces label_eff_with_alias(eff, alias_opt): the cell an alias names, `local_none()` for None
fn label_cell(l: Label) : LocalId
fn label_is_aliased_cell(l: Label) : Bool                              -- was (eff: String); part D 18493 passes the label
fn label_same(a: Label, b: Label) : Bool                               -- one effect id (two absent ids are the same) and one cell
fn label_same_id(a: Label, b: Label) : Bool                            -- the same, and the id is settled
strip_label_alias(eff: String)  =>  REMOVED                            -- a label prints as decl_name(tab, l.sym); the cell never printed (part D label_to_string)
```
Needed from other owners (part B is written against these):
```
handler_alias.kai  ClauseCont  : CcNone | CcOwn(String) | CcInLambda(String)  =>  CcNone | CcOwn(Binder) | CcInLambda(Binder)   -- the continuation's binder; the typer compares `.id`, a diagnostic prints `.name`
handler_alias.kai  clause_cont_reaches  : (cc: ClauseCont, nm: String) : Bool  =>  (cc: ClauseCont, l: LocalId) : Bool
infer.kai (part A)  union ordering: part B's `normalize_union(components: [Ty]) : Ty` keeps its signature and no longer calls `inf_mangle_ty`
      (it has no table inside `apply_ty`): components are ordered by a table-free key (`sym_raw` for a nominal head). Display order of a union may change.
```
(agent infer-B)

## LowerSt
(agent kir; owner of `kir.kai`, `kir_lower*.kai`, `kir_dump.kai`, `kir_lin_*`, `kir_inline*`, `kir_varslot.kai`, `kir_pat_alias.kai`, `emit_native*.kai`)
```
kir_lower_fns.kai  lower_to_kir  : (raw_decls, regions, fns, variants, target_origin, raw_binders, traps, stamp_ids, entry)
                              =>  (raw_decls: [Decl], tab: SymTab, specs: SpecNames, regions, fns, variants, target_origin, raw_binders, traps, stamp_ids, entry)
      -- driver.kai (3 calls): pass `sym_tab` and the monomorphiser's SpecNames. The table must hold a row for every DFn / DType / DEffect / DConst in `raw_decls`
      -- (lifted lambdas, dispatchers, derived fns included) and `specs` every `FnId` with `inst != spec_none()`.
kir_dump.kai       emit_kir      : (decls, regions, inline_on, stamp_ids, entry)  =>  (decls: [Decl], tab: SymTab, specs: SpecNames, regions, inline_on, stamp_ids, entry)
kir.kai            KProgram      : gains `tab: SymTab` -- the table the program's ids index. `build_native_object`, `build_native_modular`, `ncore_*`, `kir_lin_hook`,
      -- `kir_inline_program`, `kvs_program` keep their signatures and read `kp.tab`; nothing else has to be threaded to the native backend.
kir_dump.kai       kir_dump_fns_pub  : (fns: [KFn])  =>  (tab: SymTab, fns: [KFn])   -- a perform prints its effect's name. `kir_dump_fn_sig_pub(f)` unchanged.
```
`LowerSt` (kir_lower.kai): `st.tab : SymTab`, `st.specs : SpecNames`, `st.locals : LocalNames` (binder id -> written name, `ln_of_decl(d)` of the declaration being
lowered; a lifted lambda / handler clause is lowered under the map of the declaration that encloses it). The old `locals: [String]` (the register names bound in
the lexical scope) is now `st.regs`. `renames` maps a `LocalId` to the fresh register a shadowing binder got; `ls_reg_of(st, l: LocalId) : String` is the register
of a local (its rename, else `ln_text(st.locals, l)`). `consts` and `ffi` are looked up by `SymId`; `effs` is gone.
KIR nodes that held an effect name beside its id (kir.kai):
```
KPerform(String, SymId, String, [KVal])  =>  KPerform(SymId, [Ty], String, KVia, [KVal])   -- effect, the instance's type arguments, op label (bare), evidence route, args
      KVia = KvAmbient | KvLexical | KvAlias(String) | KvRecv(String)
      -- KvLexical = the old `op!` (a perform under a `handle` of its instance in the same fn); KvAlias(a) = the old `op@a` (a = the alias binder's written name,
      -- the same string `KInstall` carries; the register is `__alias_id__<a>`); KvRecv(r) = the old `op#r` (r = the register holding the evidence node).
KDef(String, SymId, [KDefOp])            =>  KDef(SymId, [KDefOp])
KHandlerDecl { eff: String, eff_sym, … } =>  KHandlerDecl { eff_sym, … }
kir.kai  NEW  kir_eff_name(tab: SymTab, s: SymId) : String, kir_eff_key(tab: SymTab, s: SymId, targs: [Ty]) : String   -- the runtime label / instance key of an effect
```
symid_dump.kai (driver batch) `sd_*` reads `KPerform(eff, s, o, _)`: now `KPerform(s, targs, o, _, _)`; the key it printed is `kir_eff_key(tab, s, targs)`.
What the lowering expects of the typed AST it reads (say so here if your pass produces something else):
- a continuation call is `ECall(f, args)` with `f.kind = ELocal(local_minted(canonical_continuation()))` (infer.kai `resume_root`: the old `mk_ref("resume")`);
- a handler-state alias read is `ELocal(local_minted(key))`, `key` one of `live_state_aliases(…)` (`"state"`, `"log"`, `var_cell_alias()`): the lowering binds
  each alias `binder_minted(key)` to a register of its own in a stateful clause / return clause;
- a match scrutinee alias read is `EMark(MkScrutinee)` (unchanged);
- a `PVariant(DrSym(c), _)` / `PVariantRecord(DrSym(c), _)` / `ERecordLit(DrSym(c), _)` of a variant record names the CONSTRUCTOR `c`, found with `ctor_ix_row`;
- a constructor value or call is `ESym(c, _)` with `c` a constructor id; `EModCall` / `EName` do not reach the lowering (an arm for them panics);
- `EVariantsOf(_, [SymId])`: constructor ids.

## unwind API (agent ownership) — unwind_*.kai; consumers: emit_c.kai, emit_unwind_c.kai, emit_reuse_slots.kai, emit_native_*.kai, kir_lower*.kai, kir_reuse_held.kai
Every binder set the unwind analysis takes or returns is `[LocalId]` (was `[String]` of binder names), compared with `local_eq`. An emitter spells one with `ln_text(cx.locals / st.locals, l)`. Registers, C temps and fiber locals (`fib`, `UnwPadOpen`, `UnwM`) stay `String`: they are emitted text.
```
unwind_sites.kai (nobody's batch; changed by ownership)
  UnwSite : UnwCall(Int, [String]) | UnwMatch(Int) | UnwHandle(Int, [String]) | UnwPad(Int, [String]) | UnwAssert(Int, [String]) | UnwExit(Int, [String])
         => the same with [LocalId]
  unw_exit_at / unw_call_at / unw_pad_at / unw_assert_at / unw_handle_at : (sites, site) : Option[[LocalId]]
  unw_merge(new: [UnwSite], drop: [LocalId], acc: [UnwSite])
unwind_pads.kai (nobody's batch; changed by ownership)   unw_pad_names(sites, x) : Option[[LocalId]]
unwind_pend.kai
  UnwFrame { ucx, sites, bound: [LocalId], arms: [LocalId], own: [LocalId], scr, mstack, fib, traps, pad }
  unw_fn_frame : (u, enc: String, row, seeds: [String], params: [String], body)  =>  (u, locals: LocalNames, enc: String, row: Option[RowExpr], seeds: [LocalId], params: [LocalId], body)
  unw_frame    : (u, seeds: [String], params: [String], body)                    =>  (u, locals: LocalNames, seeds: [LocalId], params: [LocalId], body)
      -- `locals` = the declaration's binder map (`cx.locals` / `st.locals`): two binders SPELLED alike are one emitted variable, so neither is
      -- registered, and an interpolation span reads a binder by its text. `enc` stays the enclosing fn's emitted symbol (it is matched against ClauseInfo's).
  unw_admit(u, names: [LocalId]) : [LocalId];  unw_owning(u, names: [LocalId]) : UnwFrame
  unw_param_seeds(params: [Param], sig, ucx) : [LocalId];  unw_clause_seeds(c: ClauseInfo) : [LocalId]   -- params as ids: map(ps, (p) => p.binder.id)
  unw_call_of(u, x) / unw_pad_of(u, x) / unw_call_names(sites, x) : Option[[LocalId]]
unwind_calls.kai
  unw_call_unwinds(f, xs, locals: [LocalId], ucx);  unw_lent_reads(f, xs, ucx) : [LocalId]
      -- callee shapes: ELocal(l) a closure in a local; ESym(s, inst) a fn by FnId; EOp(r) a perform, judged by r.eff. No by-name path is left.
unwind_binders.kai
  unw_binders(e, acc: [LocalId]) / unw_arm_binders(e, acc) / unw_destructured(pat) / unw_stmt_binders(s) : [LocalId]
  unw_rc_names(xs: [String]) : [String]  =>  unw_rc_binders(bs: [Binder]) : [LocalId]   -- drops the `_`-spelled discards
  unw_dups(xs: [String], seen, acc)      =>  unw_spelled_twice(ns: LocalNames, xs: [LocalId]) : [LocalId]
  unw_minus / unw_inter / unw_union : over [LocalId]
  unw_bare_read(e) : true for ELocal(_) and ESym(_, _) (what the old bare EName / ESym read was)
unwind_reads.kai   unw_lent(ns: LocalNames, f, xs, live: [LocalId]) : [LocalId];  unw_filter(ns, pend: [LocalId], e) : [LocalId];  unw_confined(ns, xs, h, jumps, body) : [LocalId]
unwind_cx.kai      UnwCx : effs: [String] and unw_spelled: [String] are GONE (a capability param's type and a perform both carry the effect id); carriers: [String] => [Int] (type ids, like unw / eff_ids)
                   unw_spelled_unwinds, unw_bare_effect, unw_last_dot : REMOVED;  unw_is_cap_param(ucx, t) asks the TyName's ref_sym
unwind_clauses.kai UnwClause : UnwC(SymId, String, Expr) => UnwC(SymId, LocalId, Expr)   -- the effect, the continuation's binder, the body
unwind_operands.kai  unw_is_named_operand : REMOVED (no caller; it tested a name prefix)
```
What ownership's unwind files NEED from files of other batches (written as below; say so here if you chose otherwise):
```
modules.kai (front-load-A)        pat_binders : (pat: Pattern, acc: [String]) : [String]  =>  (pat: Pattern, acc: [Binder]) : [Binder]
emit_tcrec_live.kai (emit-sat)    tcl_consumes : (e: Expr, nm: String) : Bool  =>  (ns: LocalNames, e: Expr, l: LocalId) : Bool   -- ELocal(x) -> local_eq(x, l); the EStr arm asks tcl_span_mentions(span, ln_text(ns, l))
emit_tcrec_live.kai (emit-sat)    tcl_span_mentions : (span: String, nm: String)  =>  (span: String, w: Written)
                                  (unwind no longer calls tcl_filter / tcl_filter_stmts: the ledger's entry type is emit-sat's to choose)
emit_shared.kai (emit-sat)        ClauseInfo read as CI(_, _, _, _, params: [Param], body, _, enc: String, _, _, esym) — 11 fields as at the base; if a field goes, unwind_clauses.kai ue_clauses, unwind_effects.kai ue_handle_fns and unwind_pend.kai unw_clause_seeds follow
resume_anf.kai (surface)          anf_bind_until(e, pick, prefix, n) is called AFTER the resolver by unwind_operands.kai: the operand binders it mints there must be binder_minted(prefix + n) read as ELocal(local_minted(prefix + n))
```

## emit_c part B (agent emit_c-B: `arms_shared_ctor` .. before `emit_nullary_enum_seed`) — what it calls, what it needs

PRELIMINARY (posted early so owners can converge; the final list replaces nothing, lines are only added below).
Part B follows rule 3 literally: every table of locals the emitter threads holds `LocalId`s; a local's C text is
`written_text(ln_text(cx.locals, l))`; `pat_binders` returns `[Binder]` (line above). If you own one of these and chose
another shape, post it here and part B's call is the one to change.
emit_c.kai (part A)  EmitCtx.kind1_raw, EmitCtx.borrowed  : [String]  =>  [LocalId]  -- with `emit_ctx_kind1_raw_has(cx, l: LocalId)`, `emit_ctx_with_kind1_raw(cx, ks: [LocalId])`, `emit_ctx_with_lcs(cx, l: [LocalId])`, `pat_kind1_binders(pat, cx, acc: [LocalId]) : [LocalId]`, `emit_unw_vars(names: [LocalId], cx) : [String]`, `emit_unw_own(names: [LocalId], cx)`. Part B builds every derived context with a record update (`EmitCtx { ...cx, lcs: l, locals: ns }`), never a full literal, except `emit_lam_helpers` (below).
emit_c.kai (part B)  tcp_rc_binders  : (pat, cx) : [String]  =>  (pat: Pattern, cx: EmitCtx) : [LocalId]  -- called by part A (emit_unw_owning, emit_unw_let_owned)
emit_shared.kai  LamInfo / ClauseInfo captures  : [String]  =>  NEED (owner emit-sat, please post the shape): part B must tell a captured local from a captured handler id (old `"__alias_id__<a>"`), in ONE ordered list (the order is the closure's capture array and the handle env struct layout). Part B is written against `pub type Capture = CapLocal(LocalId) | CapAlias(LocalId)` (`CapAlias(a)`: the handler id of the `with … as a` cell, `a` the alias binder's id) and `caps: [Capture]` in both types.
emit_shared.kai  LamInfo / ClauseInfo  =>  NEED a way to spell the body's locals: a lambda helper / clause body is emitted outside its declaration, and `cx.locals` must be the ENCLOSING declaration's `ln_of_decl(d)` (the captures are declared there). Part B is written against two accessors: `lam_locals(l: LamInfo) : LocalNames`, `clause_locals(c: ClauseInfo) : LocalNames` (the collector stamps the map of the declaration it is walking).
emit_shared.kai  ClauseInfo  : CI(line, col, eff: String, head, params, body, stateful, enc_fn, caps, mo, eff: SymId)  =>  part B assumes `CI(line, col, eff: SymId, head, params, body, stateful, enc_fn, caps, mo, locals: LocalNames)`-like and reads it ONLY through accessors it defines over one `match` each (so one line per accessor to fix if the shape differs).
emit_shared.kai  block_unused_lets  : (stmts, opt_final) : [String]  =>  : [LocalId]  -- the binders the emitter releases inline
emit_tcrec_live.kai  tcl_push_binders  : (pend, names: [String])  =>  (pend, names: [LocalId])  -- part B passes ids; the ledger type itself (`cx.tcpend`) is opaque to part B (it only calls tcl_scr_entry, tcl_scr_count, tcl_push_binders, tcl_exit_drops and compares with `[]`)
unwind_pend.kai  unw_frame / unw_fn_frame (seeds, params), unw_param_seeds, unw_clause_seeds, unw_owning, unw_admit, and the names in unw_call_of / unw_exit_at / unw_handle_at  : [String]  =>  [LocalId]
local_names.kai  NEED  pub fn ln_with_binder(ns: LocalNames, b: Binder) : LocalNames  -- `ns` with the binder `b` (= the private `ln_binder(b, ns)`). The emitter declares C locals no node declares (a stateful clause's `state` / `log` / `__var_cell` aliases, `self`, `k`): it brings them into `cx.locals` as `binder_minted(<name>)` so a read `ELocal(local_minted(<name>))` spells `kaiv_<name>`.
front-parse, parser output details (what a pass on parser output matches):
- A QUALIFIED head is never a dotted string any more: `m.T` in a type, record literal, `with m.E`, `impl m.P`, a row label `m.E` is
  `DrWritten(Some(written("m")), written("T"))` (`ref_qualified("m", "T")`); unqualified is `DrWritten(None, written("T"))`.
  The qualifier passes (modules.kai qualtype_*, qual_head.kai, type_scope / effect_scope) read the qualifier from the ref instead of
  `split_dotted_qualifier(name)`. Still dotted text, because their slot is still a String: `DDerive`'s protocol names, `PsQualified`.
- A QUALIFIED UNION MEMBER (`type U = m.T | Other`) has no written slot (`Var(SymId, …)`): the parser mints
  `Var(sym_derive(NCCtor, "m.T", home), [])` with the row `NEnt(NCCtor, "m.T", home, pub)` — the one row whose name is dotted.
  modules.kai qualtype_member reads it with `sym_name(tab, id)`, validates, and re-points the variant (the member's own id and row).
- Temporaries the PARSER mints are minted binders, already identified before the resolver; the resolver keeps a binder whose id is
  minted (`local_is_minted(b.id)`) and passes an `ELocal` through:
    pipe placeholder  `x |> f(_, y)`      : `Param { binder: binder_minted("__pipe") }`, reads `ELocal(local_minted("__pipe"))`
    point-free `.`    `xs | (. + 1)`      : `Param { binder: point_free_binder() }`, reads `ELocal(point_free_binder().id)`
    destructuring block lambda `{ (a, b) -> … }` : `binder_minted(lam_pat_binder_name(l, c))`, scrutinee `ELocal(local_minted(that key))`
    multi-scrutinee match                 : `PBind(binder_minted("__mm_s_<i>"))`, reads `ELocal(local_minted("__mm_s_<i>"))`
  fmt recognises them by id (`local_eq(pm.binder.id, local_minted("__pipe"))`), never by spelling.
  Names the user can also write stay text: the elided `self` / `matches` of a refinement, `ensure`, a punned field `{ x }` -> `EName(written("x"))`.
- `EVariantsOf(te, [])`: the parser leaves the constructor list empty, as before.
- `FBR(name, mo, bounds)` (PParser.fn_bounds) is unchanged: the parser still knows the name it just read.
underscore_binders.kai  ub_rest_sentinel  : (line: Int, col: Int) : String  =>  (line: Int, col: Int) : Binder  -- the written binder (`binder_written`) an anonymous list rest takes; surface_lower.kai sfl_pat puts it in `PList(_, Some(_))` as is. (agent surface; owner front-resolve)

## protocol registry
(first entry: agent protos-B — the SECOND part of `protos.kai`: the BinSerialize sum derive, the derive validators, `lower_protocols`, `proto_rungs`, the post-inference rewrite `resolve_protocol_calls*`, `validate_resolved_*`, `validate_typer_invariants`, `ty_disp_head`, the `find_impl_*` family, `proto_scope_*`. protos-A and protos-sat: add your own lines below; where we disagree the earlier line stands unless you say why.)

The row shapes in `ast.kai` (POR / PTPR / PIR / PrOR / PKey / DispInfo) fit part B as they are. Model part B is written against (the same as infer's correction above):
- a NOMINAL head is its declaration's id (`TyCon(s, _)`; a list is `core_ty_sym(CtList)`); a SCALAR has no id: a registry reference to one stays written (`DrWritten(None, "Int")`) and is read with `scalar_ty_named`;
- an operation is a LABEL inside a protocol: the protocol's id says whose, the label says which (`Written` in POR / PIR, `String` in `DispInfo` / `disp_op`). Part B compares a label only inside rows already selected by protocol id.

Needed from protos-sat (part B's calls are written for exactly this):
```
impl_identity.kai  DispHead   : DispHead(String, SymId)  =>  DhSym(SymId) | DhScalar(Ty)   -- a dispatch receiver: a nominal head by id, a scalar by itself. Built ONLY by protos.kai `ty_disp_head(t: Ty) : Option[DispHead]` (monomorph.kai 1042: call it instead of `DispHead(head, ty_head_sym(ty))`)
impl_identity.kai  dh_ref_is  : NEW (r: DeclRef, h: DispHead) : Bool   -- a registry reference (PIR target / protocol arg / ret head) names the head `h`: `DrSym(s)` vs `DhSym` by id (an absent id names nothing); a written scalar vs `DhScalar` through `scalar_ty_named`
impl_identity.kai  pir_serves : (p, h: DispHead) : Bool  -- signature kept; = dh_ref_is(target, h)
impl_identity.kai  dh_same    : (a, b) : Bool            -- signature kept
impl_identity.kai  dh_text    : NEW (tab: SymTab, h: DispHead) : String   -- how a diagnostic names a head (`decl_name` / `scalar_ty_name`); replaces `dh_name` in messages
impl_identity.kai  dh_name / dh_names / pir_ret_head  =>  REMOVED (no name beside the id; part B matches the PIR row and asks `dh_ref_is`)
impl_identity.kai  pir_sym    : (p) : String  =>  (p) : SymId   -- the impl function's id (PIR slot 4)
impl_identity.kai  pir_ret_ty : (p) : Option[Ty]         -- signature kept: `Some(TyCon(s, []))` for a `DrSym` ret head, the scalar for a written scalar
impl_identity.kai  reg_with_target_ids / pir_stamp : must settle the target AND every protocol arg AND the ret head to `DrSym` when they name a declared or core type; the rewrite compares all three by id
impl_identity.kai  impl_symbol_renames : (impls, root)  =>  (impls: [ProtoImplReg], tab: SymTab, root: String) : [ImplRename]   -- `tab` holds the impl functions' rows (lower_protocols extends its table with them first), so a symbol's spelling is `sym_name(tab, pir_sym(p))`
impl_identity.kai  impl_renamed : (rs, m: String) : String  =>  (rs: [ImplRename], s: SymId) : SymId   -- the id the impl function `s` has after the renames
impl_identity.kai  rows_with_renames : NEW (rows: [NEnt], rs: [ImplRename]) : [NEnt]   -- the rows with each renamed root impl function under its new name
impl_identity.kai  decls_with_renames(ds, rs) / reg_with_renames(reg, rs) : signatures kept (a renamed `DFn` takes its new `FnId`, PIR slot 4 its new id, an `ESym` of the old id the new one)
impl_index.kai     ii_key_bucket : (idx, proto: SymId, type_: String)  =>  (idx: ImplIndex, proto: SymId, h: DispHead) : [ProtoImplReg]
proto_ids.kai      pid_with_impls(ids, impls, root), pid_impl(ids, s), pid_ref_disp(ids, f: Expr), pid_bare_disp(ids, f: Expr), disp_op(d) : String, disp_home(d) : Option[String], pid_empty() : signatures kept. Part B calls ONLY these: a callee goes in as an `Expr` (never `pid_qualified_disp` / `pid_named_disp` with text)
proto_rank.kai     proto_ranked_matches, proto_matches_ambiguous, PMatch(home, tier, impl) : kept; ambiguous_proto_call_diag(file, line, col, opname: String, pname: String, ms) : kept (part B passes `written_text(decl_name(tab, protocol id))` as `pname`)
```
Part A <-> part B of `protos.kai`. Part B calls these functions of part A with their BASE signatures: `derive_binser_put(acc, v, ftype, writers: [String], line, col)`, `derive_binser_field_guard`, `derive_binser_field_next`, `derive_binser_field_values`, `binser_ok_cursor`, `binser_call(name: String, args, line, col)`, `binser_lib`, `binser_option_elem`, `binser_leaf_ty`, `proto_type_name(te) : String`, `impl_home_of(methods)`, `derive_from(ps, inner)`, `derive_error`, `validate_derives`, `derive_entry_diags`, `collect_proto_decls`, `proto_reg_empty`, `validate_impls`, `validate_impl_methods`, `strip_protocols`, `proto_ops_lookup_all(reg, op: String)`, `proto_dispatcher_name`, `proto_map_expr_kind`, and the literal `DeriveCx { ps: …, ids: …, cores: … }`. The three generators must hand their rows up (rules §4), and part B reads them so:
```
protos.kai (A)  DispGen        : { decls, ids }  =>  { decls: [Decl], ids: ProtoIds, rows: [NEnt] }              -- one NCFn row per dispatcher
protos.kai (A)  LowerImplsRes  : { reg, fns }    =>  { reg: ProtocolReg, fns: [Decl], rows: [NEnt] }             -- one NCFn row per lowered impl method (`NEnt(NCFn, mangled, home, false)`)
protos.kai (A)  Expanded       : Expanded([Decl], [Decl])  =>  Expanded([Decl], [Decl], [NEnt])                  -- third slot: every row a derive minted (its impls' `method_ent` rows, the writer, the `_from_bytes` shim)
```
`lower_protocols` calls `validate_impls` / `validate_impl_methods` / `lower_impls` with a `PScope` whose table already holds the `Expanded` rows, so `sym_name(ps_tab(ps), fnid_sym(fid))` answers a derived method's op label.
Part B's functions, as part A and other files call them:
```
protos.kai (B)  derive_binser_put_sum         : (variants, writers, line, col)  =>  (tab: SymTab, variants: [Variant], writers: [String], line: Int, col: Int) : Expr
protos.kai (B)  derive_binser_sum_from_bytes  : (tname: String, variants, line, col)  =>  (tab: SymTab, tname: String, variants: [Variant], line: Int, col: Int) : Expr   -- `tname` is the text of the runtime "invalid tag" message
protos.kai (B)  collect_binser_types(ps, decls, impls, acc) : [String], proto_dispatcher_name_at, op_arities_from_ops, OpAr(String, Int), proto_scope_ops, proto_scope_tparams, proto_key_of(dkeys, pname: String, psym) : String, proto_visible_from, proto_rungs, prc_reg_has_impl(reg, proto, h: DispHead), lower_protocols, resolve_protocol_calls, resolve_protocol_calls_decl, validate_typer_invariants : signatures KEPT
protos.kai (B)  LoweredProtocols  : { decls, reg, errs, diags, derived: [String] }  =>  { decls, reg, errs, diags, derived: [SymId], rows: [NEnt] }   -- `derived`: the ids of the impl functions `#[derive]` minted (rec_stamp.kai stamp_targets: `derived: [SymId]`, compare with `pir_sym`); `rows`: every function the lowering generated — dispatchers, lowered impl methods, derive shims (and the derives' `method_ent` rows) — for the driver to `st_extend` its tables with before the resolver
protos.kai (B)  validate_resolved_protocols  : (typed, reg, df)  =>  (typed: TypedProgram, tab: SymTab, reg: ProtocolReg, df: DiagFiles)   -- driver.kai 5766: pass `rd.tab`
protos.kai (B)  validate_resolved_decls      : (ds, reg, df, errs)  =>  (ds: [Decl], tab: SymTab, reg: ProtocolReg, df: DiagFiles, errs: [Diagnostic])   -- mono_front.kai 53
protos.kai (B)  ty_head_name  : (t: Ty) : Option[String]  =>  REMOVED -- a head is `ty_disp_head(t) : Option[DispHead]`; print it with `dh_text(tab, h)` (monomorph.kai 1024)
protos.kai (B)  ImplHit  : ImplHit(String, String)  =>  ImplHit(SymId, SymId)   -- the owning protocol's id, the impl function's id
```
(agent protos-B)

## front-load-B (type_scope*, effect_scope*, effect_alias*, pub_access*) — agent front-load-B
What these passes PRODUCE now (nothing is respelled: no node has a name slot to respell):
- effect pass (`es_segmented` / `es_core_decls` / `es_expr_scoped`): a row label and a handler head the ladder (or a `m.E` qualifier) settles is `RL(DrSym(effect id), targs)` / `EHandle(_, DrSym(effect id), …)`; an unsettled one stays `DrWritten` (a `m.E` whose module exports `E` but declares no such effect loses the qualifier: `DrWritten(None, E)`, as before). A perform base: `m.E.op` -> `EField(ESym(effect id, spec_none()), op)`; a bare `E.op` on a CONTESTED effect -> the same `EField(ESym(effect id, spec_none()), op)` (it was `EField(EName("E__home"), op)`); an uncontested bare `E.op` is left for the resolver, as before. `DEffect` is returned untouched (it was respelled `E__home`): the run-time spelling of a contested effect is no longer on any node — an emitter that needs it spells it from the id (`es_spell(cx, name, home)` in effect_scope_table.kai is still there). `DUse(name)` keeps the name as written (was respelled).
- type pass (`ts_segmented` / `ts_expr_scoped`): a type reference the scope settles to a contested declaration (or any transparent alias) of a non-root, non-core module, and every qualified `m.T` naming a scoped declaration, is `TyName(DrSym(type id), args)` (it was the respelled `T__home`); everything else stays `DrWritten`. `DType` is returned untouched. A type-qualified constructor path on such a type: `T.Ctor` / `m.T.Ctor` -> `EField(ESym(type id, spec_none()), "Ctor")` (it was `EField(EName("T__home"), "Ctor")`).
- these passes accept a qualified written reference in either shape: `DrWritten(Some(m), T)` or a dotted `DrWritten(None, "m.T")`.
scope_walk.kai  sw_expr / sw_hclauses / sw_hreturn_opt  : (…, sh: [String], lf: (Expr, [String]) -> Option[Expr], tyf: (TypeExpr) -> TypeExpr, hdf: (String, SymId) -> SwHead)  =>  (…, sh: [String], lf: (Expr, [String]) -> Option[Expr], tyf: (TypeExpr) -> TypeExpr, hdf: (DeclRef) -> DeclRef)  -- EXPECTED by effect_scope.kai / type_scope.kai (owner front-resolve): `sh` stays the TEXT of the binders in scope (the walk runs before the resolver numbers them); a handler head goes through `hdf` as the DeclRef it is; `SwHead` is gone; `sw_head_same(r: DeclRef) : DeclRef`; `sw_params(ps: [Param], acc: [String]) : [String]` unchanged. A record-literal / record-pattern head goes through `tyf` as `TyName(r, [])` and takes the DeclRef that comes back.
modules.kai  pat_binders(pat, acc: [String]) : [String], fn_param_names(ps) : [String]  -- EXPECTED unchanged (binder text, pre-resolve) by pub_access_decl.kai (owner front-load-A)
effect_scope_table.kai  es_collect  : (decls: [Decl], target: String)  =>  (decls: [Decl], tab: SymTab, target: String)  -- driver.kai:5061
type_scope_table.kai  ts_collect  : (decls: [Decl], target: String, effs: [String])  =>  (decls: [Decl], tab: SymTab, target: String, effs: [String])  -- driver.kai:5071
effect_alias.kai  collect_alias_cands  : (decls: [Decl], df: DiagFiles)  =>  (decls: [Decl], tab: SymTab, df: DiagFiles)  -- driver.kai:5324, infer.kai:19404. `AliasCand` / `AliasResult` / `find_alias(aliases, name: String)` keep their shape: an alias is still found by the name a row label WRITES (`DrWritten(None, n)` -> `find_alias(aliases, written_text(n))`; a `DrSym` label is an effect, never an alias).
effect_alias_scope.kai  EffOps  : EO(String, SymId, [String])  =>  EO(SymId, [String])  -- resolve.kai:1066; `eff_declares_op` decides by id only
effect_alias_scope.kai  EffSpelling / ESp / es_note / es_lookup  =>  REMOVED  -- no head is spelled for a contested effect any more (see above): resolve.kai drops `eff_spellings` and the `es_lookup` fallback of `chk_handle_sym`
effect_alias_scope.kai  RowAlias = RA(String, [RowLabel]), alias_components(ras, head: String, depth), alias_op_owner  -- unchanged; the alias name is `written_text(sym_name(tab, type id))` (resolve.kai:1055)
pub_access.kai  validate_pub_access  : (segs, bounds, target_mod, cores, df)  =>  (segs: [CoreSegment], tab: SymTab, bounds, target_mod, cores, df)  -- driver.kai:5185
pub_access.kai  pa_hidden_from_root  : (segs, target_mod)  =>  (segs: [CoreSegment], tab: SymTab, target_mod: String)  -- driver.kai:5813

### protocol registry — part A (agent protos-A: `protos.kai` up to `derive_binser_put_sum`)

Part A agrees with part B's entry above (row shapes fit; `DispGen` / `LowerImplsRes` gain `rows`; `Expanded([Decl], [Decl], [NEnt])`; part B's functions are called
as it posted them, `derive_binser_put_sum(tab, …)` / `derive_binser_sum_from_bytes(tab, tname, …)` included) EXCEPT two signatures, and why:
```
protos.kai (A)  proto_type_name  : (te: TypeExpr) : String  =>  (tab: SymTab, te: TypeExpr) : String
      -- WHY: a `TyName(DrSym(s), _)` has no text, and it reaches the lowering before the resolver: the parser's tuple (`tuple_type_ref`), a head a scope pass
      -- settled, and every derived impl's target (below). Part B's 4 calls (collect_binser_types, validate_derive_binser_one, collect_ord_providers,
      -- validate_derive_ord_one) and law_checks.kai pass the table (`ps_tab(ps)`).
protos.kai (A)  generate_dispatchers  : (reg) : DispGen  =>  (reg: ProtocolReg, tab: SymTab) : DispGen
      -- WHY: the dispatch tag `__kai_proto_dispatch__:<P>:<op>` spells the protocol's NAME, which only the table has. lower_protocols: `generate_dispatchers(reg0, ps_tab(ps))`.
```
The table the lowering reads names from is `ps_tab(ps)`: `lower_protocols` needs no `tab` parameter of its own.
What part A puts in the rows (written until `reg_with_target_ids` stamps them):
```
POR(psym, written(op), params, ret, line, col, mo)   PTPR(psym, tparams, mo)   PrOR(psym, mo, is_pub)      -- psym = the DProtocol's own id
PIR(psym, proto_type_ref(target), written(op), impl fn id, map(proto_args, proto_type_ref), proto_type_ref(ret) | the target's ref, impl home)
      -- target / protocol args / ret head: the DeclRef the impl WROTE (`TyName(r, _)` -> r; `[T]` -> ref_written("List"); a fn type -> "Fn"; a row -> "Row");
      -- a derived impl's target is born `DrSym(type id)`. impl fn id = sym_derive(NCFn, mangled, the method's home) = the lowered DFn's id.
```
New in part A, usable by part B:
```
protos.kai (A)  NEW  pub fn proto_type_ref(te: TypeExpr) : DeclRef              -- the single-dispatch head as a reference; no table
protos.kai (A)  NEW  fn ref_text(tab: SymTab, r: DeclRef) : String              -- `decl_name(tab, s)` of a resolved ref, the written name otherwise (spelling / message)
protos.kai (A)  NEW  fn refs_same(tab: SymTab, a: DeclRef, b: DeclRef) : Bool   -- one declaration when both are resolved, one spelling while either is written
protos.kai (A)  NEW  fn proto_ref_spell(tab: SymTab, r: DeclRef) : String       -- a `DImpl`'s protocol head as `ps_resolve` / `derive_gen` / `picks_gen` take it (`m.P` | `P`)
protos.kai (A)  NEW  fn proto_ref_sym(ps: PScope, r: DeclRef, from: Option[String]) : SymId   -- the protocol that head denotes from `from` (a `DrSym` head is that protocol)
protos.kai (A)  LawReg  : PL(Option[String], String, String) / AI(Option[String], String, String, String)  =>  PL(SymId, String) / AI(Option[String], Int, Int, String)
      -- a `laws` clause is filed under its protocol's id; an `axiom` impl under its home and the impl's (line, col): an impl has neither an id nor a name.
protos.kai (A)  law_theory_of     : (r, home: Option[String], proto: String)  =>  (r: LawReg, proto: SymId) : Option[String]                       -- caller law_checks.kai (protos-sat)
protos.kai (A)  law_impl_axiomed  : (r, home, proto: String, head: String)    =>  (r: LawReg, home: Option[String], line: Int, col: Int) : Bool    -- caller law_checks.kai: the DImpl's own line / col
```
How part A's derived declarations name things (for the resolver / typer agents and tools-B's generators):
- the derived TYPE and its CONSTRUCTORS by id (no declaration is respelled any more, so a written `T` could reach a homonym of another module):
  `TyName(DrSym(t), args)`, `ERecordLit(DrSym(t), fields)`, `PVariant(DrSym(ctor), subs)`. The resolver must leave a reference born `DrSym` alone
  (as it does `derive_mint`'s `ESym`). Everything else stays text as before (`mk_name("buf")`, `binder_written("a0")`, `ref_written("Err")`,
  `TyName(ref_written("String"), [])`, the `__pimpl_…` and writer names a body calls).
- a derived impl's head is `DrSym(core_proto_id(g))`.
- a derived FUNCTION: top-level `fnid_decl(sym_derive(NCFn, name, home))` + `NEnt(NCFn, name, home, pub)`; an impl method `fnid_decl(method_sym(op, scope))`
  + `method_ent(op, scope)`, `scope = impl_scope_keyed(home, "derive <Proto> <tname>")` (front-parse's helpers); `home` = the module of the type's file
  (root file: the target module).
Needed from other owners (part A is written against these):
```
derive_generic.kai  derive_generalize  : (d, pname: String, tname: String, tps)  =>  (d: Decl, pname: String, t: SymId, tps: [String]) : Decl
      -- the head to apply the parameters to is `TyName(DrSym(t), [])`; `pname` is still the entry's spelling, for the `#b:` bound text. (owner tools-B)
layout_derive.kai  derive_layout_impl  : (tname: String, shown: String, body: TypeBody, line, col) : [Decl]
                                    =>  (t: SymId, tname: String, home: String, body: TypeBody, line: Int, col: Int) : <a record with `decls: [Decl]`, `rows: [NEnt]`>
json_derive.kai    derive_json_impl    : the same change
      -- `t` the derived type's id, `tname` its written name (the old `tname` and `shown` are one text now), `home` the module its ids are derived with;
      -- `rows`: one row per function among `decls`, impl methods included. Part A reads only `.decls` and `.rows`: the record's name is the owner's
      -- (proposed: `pub type Minted = { decls: [Decl], rows: [NEnt] }` in derive_mint.kai). (owner tools-B)
proto_orphan.kai   OrphanScope  : OrphanScope(PScope, [TypeSpelling])  =>  OrphanScope(PScope)   -- TypeSpelling is gone (front-parse); `OrphanImpl` as at the base (`pspell: String`). (owner protos-sat)
proto_ids.kai      pid_note_disp(ids, name: String, op: String, home, arity, proto: SymId, root), pid_fn_id(name, home, root), pid_is_disp(ids, s) : called as at the base. (owner protos-sat)
derive_pick.kai (derive_gen, derive_gens, DeriveGen, derive_pick_diags), derive_reach.kai : called as at the base (String spellings, head texts). (owner tools-B)
decl_names.kai     method_sym, method_ent, impl_scope_keyed : as front-parse posted them.
```
`protos.kai` gains `import compiler.written`, `import compiler.symtab`, `import compiler.decl_text` in its import block (part A adds them; part B need not).
(agent protos-A)

## perceus API (agent perceus) — perceus.kai, perceus_*.kai; consumers: emit_c.kai (tcrec, part C), trmc_exit_drops.kai, borrow_infer.kai, underscore_binders.kai, reuse_uses.kai, emit_shared.kai, driver.kai
THE UNIT PERCEUS TRACKS IS A `Binder` (id + written name), not a bare `LocalId`. Reason: an interpolation span (`EStr`) is source text until emit, and
every Perceus question ("does `e` mention this binder", path counts, bare reads, …) must also match the binder against the identifier tokens of the
spans under `e`; that needs the binder's written name, and almost every helper can reach a span. Identity is always decided on `b.id` (`local_eq`);
the name is read in ONE place (`perceus_reads.kai pcs_interp_ident_count` and its two siblings). So:
- every `nm: String` (a binder name) of a perceus function that walks an expression is `b: Binder`; every `[String]` of binder names (scopes, skip / move /
  force / borrowed sets, `pnames`) is `[Binder]`. A caller passes `p.binder` / `map(params, (p) => p.binder)` / the `Binder` of a `PBind`.
  A caller that holds only a `LocalId` (it read it off an `ELocal`) builds the binder from the declaration's names: `Binder { id: l, name: ln_text(ns, l) }`.
- functions that only ask the read table or test membership take the `LocalId`: `pcs_has(bs: [Binder], l: LocalId) : Bool` (perceus_reads.kai) is the membership test.
```
perceus_reads.kai
  Read : Rd(String, Int, Int, Bool, Bool)  =>  Rd(LocalId, Int, Int, Bool, Bool)      -- reuse_uses.kai ru_reads_of compares with local_eq
  UseIx                                    -- still `NameIx[Read]`, bucketed by `local_raw` (name_ix.kai gains ni_add_id / ni_bucket_id, see below)
  NEW  LastUse = LuAt(Int, Int) | LuBlocked | LuUnused        -- perceus no longer builds infer.kai's `LU` (it carried the name)
  last_use_for(nm: String, uses) : LU      =>  last_use_for(l: LocalId, uses: UseIx) : LastUse
  pcs_read_count / pcs_read_once (nm, uses) =>  (l: LocalId, uses);   pcs_uses_have_in_lam(uses, nm) => (uses, l: LocalId);   pcs_read_raw_at(nm, uses, l, c) => (b: LocalId, uses, l, c)
  pcs_mentions(nm: String, e: Expr) : Bool =>  pcs_mentions(b: Binder, e: Expr) : Bool                 -- borrow_infer.kai:57, trmc_exit_drops.kai:153, underscore_binders.kai
  pcs_interp_ident_count(nm: String, span, is_triple) => (b: Binder, span: String, is_triple: Bool)   -- the span boundary: reads written_text(b.name)
  prc_pat_bindings(pat, acc: [String]) : [String]  =>  (pat: Pattern, acc: [Binder]) : [Binder];  prc_pat_bindings_skip_raw likewise     -- trmc_exit_drops.kai:99
  pcs_filter_non_resume(params) : [String] =>  : [Binder]
  NEW  pcs_has(bs: [Binder], l: LocalId) : Bool
perceus_read_walk.kai
  pcs_use_index(body: Expr, scope: [String]) =>  pcs_use_index(body: Expr, scope: [Binder]) : UseIx / Console            -- emit_c.kai:13190/13339, reuse_uses.kai:48
  pcs_use_index_in(body, scope: [String], boxed: [String], slots) => (body, scope: [Binder], boxed: [Binder], slots)
perceus_arg_reads.kai
  pcs_is_bare / pcs_any_arg_is_var / pcs_any_arg_bare (nm: String, …)  =>  (l: LocalId, …)      -- emit_c.kai:13620 passes the param's `binder.id`
  pcs_read_outside_lambda / pcs_any_arg_mentions (nm: String, …)       =>  (b: Binder, …)
perceus_payer.kai
  PcsParamSets { moved: [String], forced: [String] }  =>  { moved: [Binder], forced: [Binder] }                            -- emit_c.kai:13419
  pcs_param_payer(nm: String, uses, sets)  =>  pcs_param_payer(l: LocalId, uses: UseIx, sets: PcsParamSets) : PcsPayer   -- emit_c.kai:13420: `p.binder.id`
  pcs_let_payer / pcs_arm_payer / pcs_fate_of / pcs_unread (nm: String, …)  =>  (l: LocalId, …);   pcs_fate(lu: LU, nm, uses, moved, forced) => (lu: LastUse, l: LocalId, uses, moved, forced)
  pcs_names_paid_by(names: [String], …) : [String]  =>  (names: [Binder], …) : [Binder];   pcs_make_drop_stmt(nm: String, ln, cl) => (l: LocalId, ln, cl)
perceus_plant_drop.kai
  ppd_drop_name(s: Stmt) : Option[String]   =>  : Option[LocalId]                    -- trmc_exit_drops.kai:51/138/184
  ppd_drop_among(d: Stmt, names: [String])  =>  (d: Stmt, names: [Binder])           -- trmc_exit_drops.kai:104
  ppd_let_names(stmts) : [String]           =>  : [Binder]                           -- trmc_exit_drops.kai:73
  ppd_plant(e, self_name, names: [String])  =>  (e, self_name: SelfRef, names: [Binder]);   ppd_drops(names: [String], ln, cl) => (names: [Binder], ln, cl)
  ppd_bind_then, ppd_hoist_then, ppd_has_self_call, ppd_is_hoist_block : unchanged signatures (the slots they mint are binder_minted(ret_slot_name(..)) read as ELocal(local_minted(..)))
perceus.kai
  pcs_callee_key(callee: Expr) : String     =>  pcs_callee_key(callee: Expr) : FnId      -- `FnId(s, inst)` of an `ESym(s, inst)`, `fnid_none()` otherwise. emit_c.kai:13553/13677 compile unchanged
  pcs_borrow_lookup(bmap, key: String)      =>  pcs_borrow_lookup(bmap: BorrowMap, key: FnId) : [Int]
  pcs_explicit_borrow_names(params) : [String] => : [Binder];   pcs_list_union(xs: [String], ys: [String]) => over [Binder] (union by id)      -- emit_c.kai:13203/13204
  pcs_branch_aware_skip_params(pnames: [String], body, bmap) : [String]  =>  (pnames: [Binder], body: Expr, bmap: BorrowMap) : [Binder]        -- emit_c.kai:13206/13349
  pcs_borrow_params_hybrid(is_pub, me, params, pnames: [String], body) : [String] => (…, pnames: [Binder], body) : [Binder];  pcs_borrow_params, pcs_max_paths_in_expr(nm: String, e) => (b: Binder, e)
  perceus_pass(decls)                       =>  REMOVED (no caller; it built a table from the decls).  perceus_pass_in(decls, tab, views, root) : unchanged
  pcs_build_borrow_map / pcs_planned_borrow_map / pcs_has_int : unchanged
```
Files of NOBODY's batch changed by perceus (additive unless said):
```
name_ix.kai       NEW  ni_add_id(buckets: Array[[a]], key: Int, x: a) : Array[[a]] / Mutable;  ni_bucket_id(ix: NameIx[a], key: Int) : [a]     -- an index over ids; the String-keyed pair stays for kir_lin_ops
borrow_map.kai    BorrowEntry : BwE(String, [Int]) => BwE(FnId, [Int]);  bm_lookup(bm, key: String) => bm_lookup(bm: BorrowMap, key: FnId) : [Int]   -- keyed by function identity (fnid_eq); an unresolved id answers []
borrow_view.kai   bv_build unchanged; `bv_fid_key(fid) : String` STAYS (unwind_calls.kai / unwind_effects.kai key their own tables with it)
consume_index.kai ConsumeEntry : CE(String, [Int]) => CE(FnId, [Int]);  cix_lookup(cx, key: String) => cix_lookup(cx: ConsumeIndex, key: FnId) : [Int]
ret_slots.kai     NEW  ret_slot_binder(s: RetSlot) : Binder (= binder_minted(ret_slot_name(s)));  ret_slot_local(s: RetSlot) : LocalId;
                  NEW  ret_slot_id_is_any / ret_slot_id_is_fn / ret_slot_id_is_scope(l: LocalId) : Bool, ret_slot_binds_tail_arg(b: Binder) : Bool   -- the id-taking twins of ret_slot_is_*; the String ones stay for emitters that hold text
```
What perceus NEEDS from files of other batches (written as below; say so here if you chose otherwise):
```
self_call.kai (mono)          SelfRef(FnId, String) => SelfRef(FnId);  is_self_callee(callee, me) unchanged;  self_ref_name REMOVED (perceus no longer calls it)
borrow_infer.kai (checks)     borrow_infer_params(self_name: SelfRef, pnames: [String], body) : [String]  =>  (self_name: SelfRef, pnames: [Binder], body: Expr) : [Binder]
modules.kai (front-load-A)    pat_binders(pat, acc: [Binder]) : [Binder]   -- as ownership / surface already recorded
handler_alias.kai (front-resolve)  clause_op_args(params) : [Param] unchanged;  user_state_aliases() : [String], var_cell_alias() : String unchanged (mint keys).
                              Perceus no longer calls handle_live_aliases / clause_lent_names: a stateful handle's aliases are `binder_minted(key)` for each key
                              (as the kir section says: a state alias read is ELocal(local_minted(key))); a user local spelled `state` has its own id, so no "bound outside" test is left.
core_builtins.kai / fn_index.kai / fnreg.kai (mono)  builtin_ref(e), builtin_ref_at(b, at), builtin_id(b), lending_builtins(), builtin_lending_form(b), builtin_is_unit_coercion(b),
                              fnx_build(register_fn_decls(decls, entry)), fnx_decl_sig(fns, fid), fnx_of_sym(fns, fid), fnx_callee_sig(fns, callee) : called with their base signatures
ctor_ix.kai (mono)            sym_is_ctor(t: SymTab, s: SymId) : Bool : base signature
expr_positions.kai / ast_map.kai (ownership)  xp_positions, xp_positions_kind, xrole_inline, prc_map_expr_kind, prc_map_opt_expr, prc_map_stmt_exprs : base signatures
perceus_cap.kai -> symtab / scope_view (front-parse, nobody)  sv_resolve_any(tab, view, [NCEffect, NCType], name: String) : base signature (a capability param's annotation `TyName(DrSym(s), _)` is asked by id; only a still-written ref climbs the view)
```

## front-resolve API (resolver, scope chain, handler aliases, scope_walk, call sites) — owner: agent front-resolve

What the resolver leaves in the tree (resolve.kai):
- Every binder the walk declares is numbered `LocalId(1..)` across the run and rebuilt with `binder_numbered`: fn / lambda / check params, `PBind`, `PAs`, `PNarrow`, `PList` rest, `SVar`, a handle's `as` alias, `HR`'s binder, clause params. A binder that already carries an id (minted, or numbered by an earlier walk) keeps it. A `DEffect` default block is NOT walked (as before).
- A read `EName(w)` becomes `ELocal(id)` (a binder in scope) or `ESym(sym, spec_none())` (a declaration, a core builtin); a qualified `EModCall(m, n)` that names a function, a constant or a constructor the table holds becomes `ESym(sym, spec_none())`. Left as written (`EName` / `EModCall`), exactly where it was left unstamped before: an unbound name (reported), a bare constructor, a name contested on one rung, a `__proto_*` dispatcher several homes declare, a core name some module also declares — the typer settles those.
- `Eff.op(...)`: the head is `EField(ESym(effect id, spec_none()), op)` (on a row alias: the component effect that declares `op`). `EHandle(body, DrSym(effect id), …)` once the head is settled.
- THE HANDLER-STATE ALIASES have no binder in the tree; their identity is the minted one: inside a stateful handle's clauses a read of `state` / `log` / `__var_cell` that reaches the handler state is `ELocal(local_minted("state" | "log" | "__var_cell"))`. A read of an enclosing user binder of that name is that binder's own numbered id. A pass that binds / spells them takes the binders from handler_alias.kai (below).
```
scope_chain.kai  NEW  Bound = BDecl(SymId) | BLocal(LocalId)                         -- what a scope binds a written name to
scope_chain.kai  ChBind  : ChBind(String, SymId)  =>  ChBind(String, Bound)        -- the String is the resolver's lookup key
scope_chain.kai  ch_bind  : (c, name: String, s: SymId)  =>  (c, name: String, b: Bound)
scope_chain.kai  ch_lookup  : Option[SymId]  =>  Option[Bound];  ch_locals  : [SymId]  =>  [LocalId];  ch_is_local  =>  REMOVED (match on `Bound`)
scope_chain.kai  NEW  ch_next_local(c) : LocalId (the id the next `ch_declare_local` binds), ch_bind_local(c, name, l: LocalId)
resolve.kai  ParamIds  : ParamIds(SymId, [SymId])  =>  ParamIds(SymId, [LocalId])   -- redundant now: the same ids are `p.binder.id` of the DFn's params (tailfuse.kai, driver `rd.params`)
resolve.kai  resolve_program  : (df, decls, target)  =>  REMOVED: call resolve_program_with(df, decls, tab, target)   -- no table can be built from bare decls
resolve.kai  check_program / resolve_decls  : (df, decls, target)  =>  (df, decls, tab: SymTab, target)
resolve.kai  resolve_decls_views  =>  same result as resolve_decls_stamped (a core builtin stays `ESym(builtin id)`; core_unstamp_decls is the identity)
resolve.kai  collect_effect_names  : (decls) : [String]  =>  RENAMED collect_effect_ids(decls: [Decl]) : [SymId]   -- every DEffect's id (driver, emit_shared, emit_c, kir_lower_fns, infer)
resolve_callee.kai  Disp  : Disp(Option[SymId], String)  =>  DispSym(SymId) | DispWritten(String)
resolve_callee.kai  LocalArity  : (SymId, Int)  =>  (LocalId, Int);  Reach.RLocal  : RLocal(SymId)  =>  RLocal(LocalId)
handler_alias.kai  user_state_aliases() : [String], var_cell_alias() : String  -- unchanged: the KEYS (`mk_name(var_cell_alias(), …)` before the resolver, `mk_local(var_cell_local(), …)` after)
handler_alias.kai  NEW  var_cell_local() : LocalId = local_minted(var_cell_alias());  state_alias_binders() : [Binder]  -- `binder_minted` of state, log, __var_cell
handler_alias.kai  clause_state_aliases  : (bound_outside: (String) -> Bool) : [String]  =>  (bound_outside: (LocalId) -> Bool) : [Binder]
      -- the alias binders a stateful clause binds: `state` / `log` unless the enclosing scope already binds THAT minted id (an enclosing stateful clause), plus `__var_cell`. `.id` to bind / compare, `.name` to spell.
      -- An enclosing USER binder named `state` is a different id, so it no longer answers `bound_outside`: the alias is then bound but never read (see the report: the C clause prologue must not declare an alias the clause's captures already spell).
handler_alias.kai  live_state_aliases  : (stateful: Bool, local_scope: [String]) : [String]  =>  (stateful: Bool, local_scope: [LocalId]) : [Binder]
handler_alias.kai  handle_live_aliases  : (init_opt, local_scope: [String]) : [String]  =>  (init_opt, local_scope: [LocalId]) : [Binder]
handler_alias.kai  clause_lent_names  : (params, init_opt) : [String]  =>  (params, init_opt) : [LocalId]
handler_alias.kai  canonical_continuation  : () : String  =>  () : Binder = binder_minted("resume")   -- the typer's canonical continuation callee is `ELocal(canonical_continuation().id)`
handler_alias.kai  clause_bound_names  : (params) : [String]  =>  (params) : [LocalId]   -- the canonical continuation's id, then the params' ids
handler_alias.kai  clause_continuation  : (params) : Option[String]  =>  (params) : Option[Binder]   (as agent surface asked)
handler_alias.kai  ClauseCont  : CcOwn(String) | CcInLambda(String)  =>  CcOwn(LocalId) | CcInLambda(LocalId);  clause_cont_reaches(cc, nm: String)  =>  (cc, l: LocalId)
handler_alias.kai  NEW  expr_reads_local(l: LocalId, e: Expr) : Bool   -- `ELocal(l)` anywhere in `e` (post-resolve)
handler_alias.kai  seg_state_aliases / seg_state_block / aliases_read_by_clauses  -- unchanged, by written name: they run inside the resolver, on the tree before it is resolved
scope_walk.kai  sw_params  : unchanged ([String]: the written names; scope_walk runs before the resolver and reads binders by `written_text(b.name)`)
scope_walk.kai  SwHead / sw_head_same  =>  REMOVED;  the head hook `hdf: (String, SymId) -> SwHead`  =>  `hdf: (DeclRef) -> DeclRef` in sw_expr / sw_hclauses / sw_hreturn_opt
      -- pass `(r) => r` to leave heads alone (type_scope, value_scope, name_uses); effect_scope returns `DrSym(effect)` or the reference as written. `SUse(n, …)` offers `ref_written(n)` and keeps `n` unless a written reference comes back.
      -- `ERecordLit(r, …)` / `PVariantRecord(r, …)` with a still-written `r` reach `tyf` as `TyName(r, [])` (qualifier inside the reference, no dotted spelling) and take the reference `tyf` hands back.
call_site_target.kai  CallTarget  : (SymId, Expr, [Expr], [String], [Expr])  =>  (SymId, Expr, [Expr], [LocalId], [Expr])   -- the lambda binders by id
call_site_target.kai  callee_id / callee_fnid / home_fn_ref(home: String, name: String) : ExprKind / collect_const_vals / fold_const_refs  -- signatures unchanged
call_site_check.kai  dump_call_elision / call_site_diags / check_call_site_refinements  : (file, decls)  =>  (file, decls, tab: SymTab)   -- the callee's name is printed from the table (driver)
sym_erase.kai  sym_erase_decls_in / sym_qualify_decls_in  -- signatures unchanged; both only renumber site ids now (no reference is respelled)
core_stamp.kai  core_stamp_decls  -- unchanged signature; stamps a leftover `EName` that spells a core builtin, nothing else
name_uses.kai  NDisp  : NDisp(String, String, String)  =>  NDisp(SymId, String, SymId)   -- the dispatcher fn's id, the op label, the protocol's id
name_uses.kai  nu_scan_picks / nu_scan_tab  : `t: [NEnt]` unchanged;  nu_scan_consts(decls, target, picks)  =>  (decls, tab: SymTab, target, picks);  nu_const_coll(decls, target)  =>  (decls, tab: SymTab, target)   (driver)
name_report.kai  nr_report_program  =>  REMOVED (no caller; nr_report_program_tab is the entry)
```
What front-resolve calls in files it does not own (written as below; owners please confirm or correct here):
```
effect_alias_scope.kai  RowAlias  : RA(String, [RowLabel])  =>  RA(SymId, [RowLabel])   -- the alias type's id; the resolver hands the labels already settled (`DrSym`) where it can
effect_alias_scope.kai  EffOps  : EO(String, SymId, [String])  =>  EO(SymId, [String])
effect_alias_scope.kai  alias_components  : (ras, head: String, depth)  =>  (ras, head: SymId, depth) : [RowLabel];  alias_op_owner(eos, comps, op: String) : Option[RowLabel]  -- unchanged
effect_alias_scope.kai  EffSpelling / es_note / es_lookup  -- no longer called by resolve.kai (no effect is respelled)
refinements.kai  RefBinding  : RBind(String, Expr)  =>  RBind(LocalId, Expr);  subst_self(e, name: String)  =>  subst_self(e, l: LocalId)  (`self` becomes `ELocal(l)`)
refinements.kai  subst_params(pred, pnames: [String], args)  =>  (pred, ps: [LocalId], args);  arg_for_param(pnames, args, target: String)  =>  (ps: [LocalId], args, target: LocalId)
refinements.kai  classify_arg_vs_pred(req_pred, pname: String, arg, refs)  =>  (req_pred, p: LocalId, arg, refs);  classify_requires_clause(pred, pnames: [String], args, refs)  =>  (pred, ps: [LocalId], args, refs)
modules.kai  pat_binders(pat, acc: [Binder]) : [Binder]  -- as published by agent surface; decl_home_hint_reset(d, cur, target) : String unchanged
kai_builtin.kai  kai_unshadowed(names: [String], ds: [Decl]) : [String]  -- called unchanged
symtab_build.kai  nt_collect(decls, tab, target), nt_protocols(decls, tab, target)  -- as published by front-parse
```

## mono API (agent mono: fnreg, fn_index, fn_csym, self_call, live_fns, ctor_ix, ctor_stamp, unique_binders, tail*, monomorph, mono_front)
PRELIMINARY, posted early; monomorph / mono_front / tailfuse lines are added below as they land.
```
fnreg.kai     EFn  : EFn(String, Int, Option[String], Option[UFnSig], FnId)  =>  EFn(Written, Int, Option[String], Option[UFnSig], FnId)
      -- SAME positions. Slot 1 is the name the function is EMITTED under (a declaration's `decl_name`, a specialisation's `spec_text`), never a key:
      -- the registry is found by `FnId` only. An emitter spells it `c_sym(written_text(n), mo)`.
fnreg.kai     NEW  fn_emit_name(tab: SymTab, specs: SpecNames, f: FnId) : Written   -- `spec_text(specs, f)` when `fnid_inst(f) != spec_none()`, else `decl_name(tab, fnid_sym(f))`
fnreg.kai     register_fn_decls  : (decls, entry)  =>  (decls: [Decl], tab: SymTab, specs: SpecNames, entry: SymId) : [EFn]
      -- callers: driver.kai (5), emit_c.kai (2), kir_dump.kai, perceus.kai, unbox.kai. `specs` = `MonoFront.specs` (below); `spec_names_empty()` before the monomorphiser.
fnreg.kai     fn_of_decl_sym  : (d, s: FnId, entry)  =>  (d: Decl, tab: SymTab, specs: SpecNames, entry: SymId) : Option[EFn]   -- the id is the DFn's own; `fn_of_decl` REMOVED (no caller)
fnreg.kai     classify_unbox_sig  : (entry: Bool, name: String, mo: Option[String], tparams, params, opt_ret, row, body)  =>  (entry: Bool, tparams, params, opt_ret, row, body)   -- purity.kai:155; name/mo only fed a self-call exemption that no node shape reaches any more
fnreg.kai     body_is_effectful_s / body_kind_is_effectful_s / body_opt_is_effectful_s / any_*_effectful_s / stmt_is_effectful_s, fnreg_modcall_is_self  =>  REMOVED; `body_is_effectful(e)`, `body_kind_is_effectful(k)`, `body_opt_is_effectful(o)`, `stmt_is_effectful(s)` keep their signatures
fnreg.kai     efn_collect_matching(fns, target: String, acc)  -- signature kept (it compares the emitted name's text: only the bare-name ambiguity WARNING of emit_c.kai find_ambig_efns reads it)
fnreg.kai     scan_uses_expr  : (e, in_lam)  =>  (ns: LocalNames, e: Expr, in_lam: Bool) : [Use]
fnreg.kai     scan_uses_in_stmts  : (stmts, in_lam)  =>  (ns: LocalNames, stmts: [Stmt], in_lam: Bool) : [Use]
fnreg.kai     scan_uses_in_interp : (span, is_triple, in_lam)  =>  (ns: LocalNames, span: String, is_triple: Bool, in_lam: Bool) : [Use]
      -- `Use = U(LocalId, …)` as infer-A posted. A read is `ELocal(l)`, the local an `EOp`'s `via` names, or a name SPELLED inside a `#{…}` span: a span
      -- has no ids, so a spelled read counts as a read of every binder of `ns` (the declaration's `ln_of_decl(d)`) spelled so. Callers: emit_shared, infer_dump, rawsafe, unbox.
local_names.kai  NEED  pub fn ln_ids_named(ns: LocalNames, w: Written) : [LocalId]   -- every binder of `ns` whose name is `w` (the reverse of ln_text; fnreg's span reads call it)
fnreg.kai     resolve_ty(t: TypeExpr) : Ty  -- signature kept: `TyName(DrSym(s), args)` -> `TyCon(s, args)`; a written name -> its scalar, else its core type's id, else `TyCon(sym_none(), args)`
fn_index.kai  FnIndex  : the name index `ix` is GONE (by `FnId`: `sx`; by emitted C symbol: `cs`)
fn_index.kai  fnx_ufn_sig_first  : (fx, name: String, mo: Option[String])  =>  (fx: FnIndex, f: FnId) : Option[UFnSig]   -- the FIRST registry entry of the instance `f` (emit_c 8273/8308/8862/9054, emit_shared 2052, kir_lower_fns 203: pass the DFn's FnId)
fn_index.kai  fnx_ufn_sig(fx, name, mo), fnx_qualified(fx, home, name), fnx_kai_qualified(fx, home, name)  =>  REMOVED   -- they derived an id from (home, name); a callee is `ESym(s, inst)`: `fnx_of_sym(fx, FnId(s, inst))` / `fnx_callee(fx, f)` / `fnx_kai(fx, f)`
fn_csym.kai   (nobody's batch; changed by mono) csym_index / csym_sig keep their signatures; `c_sym(name: String, mo)` unchanged
self_call.kai SelfRef  : SelfRef(FnId, String)  =>  SelfRef(FnId);  self_ref_name  =>  REMOVED   -- a local never shadows a fn's id: delete the `list_has(scope, self_ref_name(me))` tests (emit_c part C, perceus.kai 3661, trmc_self_call.kai, closure_spec*.kai)
live_fns.kai  live_decls  : (decls, entry)  =>  (decls: [Decl], ids: ProtoIds, entry: SymId) : [Decl]   -- reuse_ctors.kai reusable_tags: thread `reg.ids`. A fn is live by `FnId`; a running dispatcher reaches the impl methods (`pid_impl`) of its operation's label
ctor_ix.kai   ctor_ix_build  : (variants, decls, root)  =>  (variants: [VariantEntry], decls: [Decl], tab: SymTab, root: Option[String]) : CtorIx   -- emit_c (2), kir_lower_fns, reuse_ctors
ctor_ix.kai   reads `VariantEntry` as `EV(home: Option[String], name: String, _, _, tag)` (base shape, emit_shared.kai)
unique_binders.kai  unique_binders_decls  : (ds)  =>  (ds: [Decl], tab: SymTab) : [Decl] / Console   -- driver.kai 5843. After it, inside one function, two binders share an id exactly when they share a spelling: a binder SPELLED like another gets `name__LINE_COL` (as before), and one whose ID occurs twice gets `local_minted(<its new spelling>)`; reads (`ELocal`, an `EOp`'s `via`, span text) follow
unique_binders.kai  calls handler_alias.kai `handle_live_aliases(init_opt, local_scope: [String]) : [String]` and `canonical_continuation() : String` with their BASE signatures (span text only)
core_effects.kai  core_effect_label / core_effect_row_label : signatures kept (`Label { sym, ty_args }` as ast.kai has it today; `RL(DrSym(id), [])`)
```
(agent mono)
type_text.kai  type_text  : (t: TypeExpr) : String  =>  (tab: SymTab, t: TypeExpr) : String  -- NEEDED by mutate.kai (mut_impl_scope, mut_proto_head call it as `type_text(tab, ty)`): a `TyName(DrSym(s), _)` has no text without the table. Owner typer-sat: if you settle on another shape, mutate.kai's two calls are the only ones to adjust (agent tools-A)
fmt_shape.kai  fmt_subst_var  : (e: Expr, name: String, to: ExprKind) : Expr  =>  (e: Expr, b: Binder, to: ExprKind) : Expr  -- NEW fmt_reads_binder(e: Expr, b: Binder) : Bool. Both accept either spelling of a read of a parser-minted binder (`ELocal(b.id)` or `EName(b.name)`), so the formatter recovers `.` / `_` sugar whichever the parser writes for `__pl` / `__pipe` (agent tools-A)

### front-resolve CORRECTION — handler_alias.kai (this block REPLACES the handler_alias.kai lines of the section above; it follows what agents kir and infer-B already wrote against)
The aliases and the canonical continuation are MINTED binders, named by their mint KEY (a `String`, as rule §3 has it): the binder is `binder_minted(key)`, a read is `ELocal(local_minted(key))`.
```
handler_alias.kai  user_state_aliases() : [String], var_cell_alias() : String, canonical_continuation() : String   -- UNCHANGED: mint keys ("state", "log", "__var_cell", "resume")
handler_alias.kai  clause_state_aliases  : (bound_outside: (String) -> Bool) : [String]   -- UNCHANGED signature: alias keys in, alias keys out.
      -- `bound_outside(key)` answers "is the alias of this key already bound around the clause" in the caller's own terms: the resolver asks the written name;
      -- a pass that tracks scope by id asks `local_minted(key)`; an emitter that tracks the names it has SPELLED (C locals, KIR registers) asks its spelled scope.
handler_alias.kai  live_state_aliases  : (stateful: Bool, local_scope: [String]) : [String]  =>  (stateful: Bool, local_scope: [LocalId]) : [String]   -- keys whose minted id is not in `local_scope`
handler_alias.kai  handle_live_aliases  : (init_opt, local_scope: [String]) : [String]  =>  (init_opt: Option[Expr], local_scope: [LocalId]) : [String]
      -- a caller holding a SPELLED scope uses `clause_state_aliases((key) => list_has(spelled, key))` instead (that is what keeps the C clause prologue from redeclaring a captured user `state`).
handler_alias.kai  clause_lent_names  : (params, init_opt) : [String]  =>  (params: [Param], init_opt: Option[Expr]) : [LocalId]   -- op-arg binder ids, then the minted alias ids
handler_alias.kai  clause_bound_names  : (params) : [String]  =>  (params: [Param]) : [LocalId]   -- `local_minted(canonical_continuation())`, then the params' ids
handler_alias.kai  clause_continuation  : (params) : Option[String]  =>  (params: [Param]) : Option[Binder]
handler_alias.kai  ClauseCont  : CcNone | CcOwn(String) | CcInLambda(String)  =>  CcNone | CcOwn(Binder) | CcInLambda(Binder);  clause_cont_reaches(cc, nm: String)  =>  (cc: ClauseCont, l: LocalId)   (as infer-B asked)
handler_alias.kai  clause_cont_of(params) : ClauseCont, clause_cont_in_lambda(cc, ps: [Param]) : ClauseCont, clause_op_args(params) : [Param], canonical_cap_getter(esym, op_name: String) : String   -- signatures unchanged
handler_alias.kai  NEW  expr_reads_local(l: LocalId, e: Expr) : Bool   -- `ELocal(l)` anywhere in `e`
handler_alias.kai  seg_state_aliases / seg_state_block / aliases_read_by_clauses   -- unchanged, by written name: they run inside the resolver, on the tree before it is resolved
```
(`var_cell_local()` and `state_alias_binders()` of the section above do not exist: write `local_minted(var_cell_alias())`, `binder_minted(key)`.)

## tools-B API (refine*, refinements, intervals, kinds / kind_resolve, region*, cell_*, derive_*, json_*, layout_*) — owner: agent tools-B

Where each file runs: BEFORE the resolver (text): refine_help, refine_enforce, kind_resolve, region, derive_*, json_*, layout_*. AFTER it (ids): intervals,
refine_variance, region_yield, cell_promote, cell_scan. BOTH: refinements, refine_coerce (a contract clause is lowered before the resolver; a predicate
inside a TYPE is never resolved, so its `self` and every other name in it stay `EName(written(..))` for good; a `requires` assert in a body is resolved).
```
refinements.kai  NEW  pub type PredVar = PvName(Written) | PvLocal(LocalId);  pub fn pv_self() : PredVar
      -- what a predicate ranges over: a written name (a parameter before the resolver; the `self` of a refinement in a type), or a local binder.
refinements.kai  wrap_with_contracts  : (body, reqs, enss, params, fn_name: String, line, col)  =>  (body: Expr, reqs: [Expr], enss: [Expr], params: [Param], fn_name: Written, line: Int, col: Int) : Expr
      -- before the resolver; `fn_name` only spells the panic message (surface_lower.kai sfl_fn_body: the DFn's `sym_name(tab, fnid_sym(fid))`). `result` is `binder_written("result")` / `mk_name`.
refinements.kai  subst_self  : (e: Expr, name: String) : Expr  =>  (e: Expr, to: ExprKind) : Expr
      -- every `self` becomes a node of kind `to` at the replaced position: `EName(p.binder.name)` before the resolver, `ELocal(p.binder.id)` after
      -- (call_site_check.kai extract_param_refinement_reqs: `CRq(p.binder.id, subst_self(pred, ELocal(p.binder.id)))`).
refinements.kai  subst_var  : (e: Expr, from: String, to: Expr) : Expr  =>  (e: Expr, from: PredVar, to: Expr) : Expr
refinements.kai  RefBinding  : RBind(String, Expr)  =>  RBind(LocalId, Expr)   -- a local binder and its refinement with `self` := `ELocal(that binder)`
refinements.kai  collect_caller_refinements(params, acc) : signature kept (reads `p.binder.id`; the params of a RESOLVED declaration)
refinements.kai  classify_arg_vs_pred  : (req_pred, pname: String, arg, caller_refs)  =>  (req_pred: Expr, v: PredVar, arg: Expr, caller_refs: [RefBinding]) : CallSiteStatus
      -- call_site_check.kai classify_call: `PvLocal(<the callee param's LocalId>)`; refine_coerce passes `pv_self()`.
refinements.kai  classify_requires_clause  : (pred, pnames: [String], call_args, caller_refs)  =>  (pred: Expr, pnames: [LocalId], call_args: [Expr], caller_refs: [RefBinding]) : CallSiteStatus
refinements.kai  subst_params  : (pred, pnames: [String], call_args)  =>  (pred: Expr, pnames: [LocalId], call_args: [Expr]) : Expr   -- call_site_check.kai bound_args: lambda binders as `[LocalId]`
refinements.kai  arg_for_param  : (pnames: [String], call_args, target: String)  =>  (pnames: [LocalId], call_args: [Expr], target: LocalId) : Option[Expr]
refinements.kai  CallSiteStatus  : CSElided | CSRuntime | CSLoose(String)  =>  CSElided | CSRuntime | CSLoose(LooseWhy)
refinements.kai  NEW  pub type LooseWhy = LwLiteral(Expr, Expr) | LwArgs(Expr) | LwCaller(Expr, Expr)   -- (literal, predicate) | (clause) | (caller's refinement, predicate)
refinements.kai  NEW  pub fn loose_text(w: LooseWhy, req: (Expr) -> String, own: (Expr) -> String) : String
      -- the old reason text. WHY a value and not a String: the callee's predicate reads the CALLEE's binders and the caller's refinement the CALLER's,
      -- two declarations' `LocalNames`; whoever reports holds them. `req` prints the predicate / clause, `own` the literal and the caller's refinement:
      -- e.g. `(e) => fmt_expr_text(e, tab, callee_locals)`, `(e) => fmt_expr_text(e, tab, caller_locals)` (call_site_check.kai dump + call_site_diags_loop).
refinements.kai  ContractIdent  : CtrId(String, Expr, Expr)  =>  CtrId(LocalId, Expr, Expr)   -- the reported binding's id, its read (`ELocal`), the literal.
      -- emit_c.kai emit_assert / kir_lower_walk.kai lower_assert: the name is `written_text(ln_text(locals, l))`; `reported_binding_span(ident: String)` keeps taking that text.
      -- `assert_reported_binding`, `contract_single_ident_node` keep their signatures and answer only for a LOCAL (`ELocal`); `contract_single_ident` is private now.
refinements.kai  expr_struct_eq(a, b) : signature kept. `ELocal` by `local_eq`, `ESym` by id + instance, `EName` by its text (two unresolved names).
refinements.kai  synth_refine_pred_fn_name(tyname: String) : String, try_eval_int / try_eval_real / try_eval_pred, callee_at, contract_assert_kind, assert_span_is_requires : unchanged
refine_help.kai  NEW  pub fn pred_text(p: Expr) : String   -- a predicate the resolver never reached, printed with no table (`fmt_expr_text(p, st_empty(), <binders p declares>)`)
refine_help.kai  requires_help_line  : (ident: String, pred, params)  =>  (ident: Written, pred: Expr, params: [Param]) : String
refine_coerce.kai  RcVerdict  : RcProven | RcRefuted(String) | RcCheck([Expr])  =>  RcProven | RcRefuted(LooseWhy) | RcCheck([Expr])
      -- infer.kai refine_coerce_value: `RcRefuted(why) -> … concat_all([what, ": ", loose_text(why, show, show)])` with `show = (e) => fmt_expr_text(e, st.env.tab, <the locals of the body under inference>)`
      -- (both sides read the same body here). `rc_verdict(want, value, have)` keeps its signature; `value` is a resolved expression (`ELocal`).
refine_coerce.kai  rc_assert  : (name: String, pred, ctx: String, strip: Bool)  =>  (b: Binder, pred: Expr, ctx: String, strip: Bool) : Stmt
      -- the check reads `b`: `ELocal(b.id)` when the binder has an id, `EName(b.name)` while it is `binder_written` (before the resolver).
      -- infer.kai refine_checked: `PBind(binder_minted("__rv"))`, `rc_assert(binder_minted("__rv"), p, st.refine_ctx, strip)`, tail `mk_local(local_minted("__rv"), …)`.
refine_coerce.kai  rc_check_var_writes  : (name: String, ann, body)  =>  (name: Written, ann: Option[TypeExpr], body: Expr) : Expr   -- desugar.kai:2895 (before the resolver): the `SVar` binder's `.name`
refine_coerce.kai  rc_preds, rc_is_dim, rc_var_binds, rc_subst, rc_may_owe, rc_mentions : unchanged
refine_enforce.kai  enforce_refinements_decls  : (decls, pred_fns: [SymId], root: String)  =>  (decls: [Decl], tab: SymTab, pred_fns: [SymId]) : [Decl]
      -- driver.kai:5363. `tab` spells the function a violated refinement names; a predicate fn is recognised by the `DFn`'s own id (`root` is gone).
refine_variance.kai  rv_param  : (defs: (String) -> [TyDef], name: String, j: Int)  =>  (defs: (SymId) -> [TyDef], t: SymId, j: Int) : Int
refine_variance.kai  rv_flow  : (defs: (String) -> [TyDef], have, want)  =>  (defs: (SymId) -> [TyDef], have: Ty, want: Ty) : Option[RvMiss]
      -- infer.kai st_ty_defs(st) answers by the type's declaration id. A `TyDef` payload names a type by `TyName(DrSym(s), args)` and a type PARAMETER by
      -- `TyName(DrWritten(None, p), [])` (a parameter has no id).
intervals.kai  IntervalBinding  : IB(String, Interval)  =>  IB(LocalId, Interval);  ie_lookup(env, l: LocalId), ie_extend(env, l: LocalId, iv)   -- was (env, name: String)
      -- `ival_of_decl(d)` / `ival_of_expr(e, env)` read `ELocal`: infer_dump.kai dump_intervals passes the resolved decls as they are (no `sym_locals_read_decls`).
kind_resolve.kai  build_kind_ctx  : (decls, reg, sites, view)  =>  (decls: [Decl], tab: SymTab, reg: KindReg, sites: [UnitSite], view: HabView) : KindCtx   -- driver.kai:5254: `early_tab`
kind_resolve.kai  scope_habitant_slots_of(s: UnitScope, name: String) : unchanged -- `name` is the text of the WRITTEN type head (infer_unit_habitant.kai dim_base_head_name:
      -- `TyName(DrWritten(_, n), _) -> written_text(n)`; the unit validator runs before the resolver). A head already `DrSym(s)`: `written_text(decl_name(tab, s))`.
kinds.kai  habitant_slot_decls(decls)  =>  MOVED to kind_resolve.kai, private (it needs the table; kinds.kai is imported by the parser). NEW pub fn hab_slots_add(acc, name: String, slots)
region.kai  RegionMark  : RM(String, Int, Int, Int, String)  =>  RM(String, Int, Int, Int, Written);  rmark_name(m) : Written   -- the region binder as written; no reader today
region.kai  rewrite_regions_decls, fn_has_region_brand, te_is_region_branded : unchanged (te_is_region_branded calls fnreg.kai `resolve_ty(te) : Ty` as at the base)
cell_promote.kai  cell_ref_name(args: [Expr]) : String  =>  cell_ref_local(args: [Expr]) : LocalId   -- the cell's binder (`local_none()` when the first argument is no local read).
      -- emit_c.kai / kir_lower_walk.kai spell it with `ln_text(locals, l)`.
cell_promote.kai  promote_scalar_cells(ds), is_cell_intrinsic(i) : unchanged
cell_scan.kai  cp_uses / cp_opt_uses / cp_stmts_use  : (nm: String, …)  =>  (b: Binder, …)   -- a read is `ELocal(b.id)`; the binder's written name is read only against an interpolation span
```
tools-B calls these with the signature their owner posted: `fmt_expr_text(e, tab, locals)` (tools-A), `method_sym` / `method_ent` / `impl_scope_keyed` (front-parse),
`pat_binders(pat, acc: [Binder]) : [Binder]` (surface / front-load-A). With their BASE signature: desugar.kai `interp_span_reads_cap(span, trip, cap: String)`,
`dsg_map_expr_kind`; fnreg.kai `resolve_ty`; core_builtins.kai `builtin_ref`, `is_builtin_ref`, `builtin_mk`; ast_map.kai `prc_map_expr_kind`.
refinements.kai / refine_help.kai / refine_coerce.kai import `compiler.symtab` / `compiler.local_names` (for `fmt_expr_text`): legal once decl_names.kai stops importing
compiler.modules (front-parse), as tools-A's fmt.kai already needs.
(agent tools-B; the derive / json / layout generators follow protos-A's entry above — details appended below when they land)
### front-resolve CORRECTION 2 (replaces the matching lines of the front-resolve section; follows front-load-B's and surface's entries)
```
scope_walk.kai  sw_head_same  : (n: String, s: SymId) : SwHead  =>  (r: DeclRef) : DeclRef   -- KEPT (not removed), as front-load-B expects; `SwHead` is gone
scope_walk.kai  no longer calls modules.kai pat_binders (its signature is contested): it reads a pattern's written binder names itself
effect_alias_scope.kai  resolve.kai calls it as front-load-B posted: `RA(String, [RowLabel])` keyed by the alias's name (`written_text(sym_name(tab, type id))`), `alias_components(ras, head: String, depth)`,
      `alias_op_owner(eos, comps, op) : Option[RowLabel]`, `EO(SymId, [String])`; `EffSpelling` / `es_note` / `es_lookup` are no longer called. (My `RA(SymId, …)` line above is withdrawn.)
      The resolver registers an alias with each component label that names an EFFECT already settled to `DrSym` (from the alias's own home), so `eff_declares_op` can decide by id; a component that names another alias stays written.
underscore_binders.kai  ub_rest_sentinel  : (line, col) : String  =>  (line: Int, col: Int) : Binder   -- as surface asked: `binder_written("__list_rest_<l>_<c>__")`
underscore_binders.kai  ub_arm / ub_block / ub_silenced_name(name: String)  -- signatures unchanged; the pass runs on parser output, so "is this binder read" is asked of the written reads (`EName`, interpolation spans), not through perceus_reads.kai pcs_mentions (which is by id now)
```

## emit_shared / emit_tcrec_live / emit_reuse_* / caches — owner: agent emit-sat
PRELIMINARY shapes, posted before the code so emit_c-A/B/C, kir, ownership and mono can write against them (lines are only added below).

Lambda / clause descriptors and captures (emit_shared.kai):
```
pub type Capture = CapLocal(LocalId) | CapAlias(LocalId)      -- as emit_c-B wrote it. CapAlias(a): the handler id of the cell alias `a` (old "__alias_id__<a>"); ONE ordered list
LamInfo     : LamInfo(Int, String, Int, Int, [Param], [String], Expr, Option[String])
          =>  LamInfo(LocalNames, String, Int, Int, [Param], [Capture], Expr, Option[String])
      -- still 8 slots, every `LamInfo(_, ef, ln, cl, params, caps, body, mo)` pattern stands. Slot 1 was a serial nobody read; it is now the
      -- binder names of the declaration the lambda sits in (`ln_of_decl(d)`). NEW `lam_locals(l: LamInfo) : LocalNames`.
ClauseInfo  : CI(line, col, eff: String, head, params, body, stateful, enc_fn, caps: [String], mo, esym)
          =>  CI(Int, Int, LocalNames, ClauseHead, [Param], Expr, Bool, String, [Capture], Option[String], SymId)
      -- still 11 slots, every other slot where it was (unwind's `CI(_, _, _, _, params, body, _, enc, _, _, esym)` stands). Slot 3 (the effect's NAME) is now
      -- the enclosing declaration's binder names; the effect is slot 11 as before. NEW `clause_locals(c) : LocalNames`, `clause_eff(c) : SymId`.
      -- The maps hold what nodes declare: the emitter adds the locals no node declares (`state` / `log` / `__var_cell`, `resume`) itself.
lc_new      : (globals: [String])  =>  (tab: SymTab, specs: SpecNames)      -- the collector spells each fn's enc_fn symbol from the table; callers: emit_c part C (2), kir_lower_fns
build_globals, const_names, collect_op_qualified_names, op_qualified_names_for, efn_name, list_minus, list_minus_loop, list_concat_strings  =>  REMOVED
      -- the capture analysis needs no global-name set: a global is `ESym`, a local is `ELocal`.
lc_record_clauses : (cs, eff: String, esym, stateful, hline, hcol, enc_fn, st)  =>  (cs, esym: SymId, stateful, hline, hcol, enc_fn, st)
LamCollect  : { next_id, lams, clauses, scope: LamScope, cur_enc_fn, cur_mo }  =>  { lams, clauses, scope: FvOuter, cur_enc_fn, cur_mo, locals: LocalNames, tab: SymTab, specs: SpecNames }
NEW fn_emit_name(tab: SymTab, specs: SpecNames, fid: FnId) : String   -- rule 8: `decl_name(tab, sym)`, or `spec_text(specs, fid)` for a specialisation
```
Free variables (emit_shared.kai) — every `fv_*`:
```
fv_expr / fv_exprs / fv_arms / fv_hclauses / fv_hreturn / fv_field_inits / fv_list_elems / fv_stmt / fv_interp / fv_interp_parts
            : (x, own: [String], globals: NameScope, acc: [String]) : [String]  =>  (x, own: [Binder], outer: FvOuter, acc: [Capture]) : [Capture]
fv_stmts_scoped : … : FvScoped { own: [String], acc: [String] }  =>  FvScoped { own: [Binder], acc: [Capture] }
pub type FvOuter = { binders: [Binder], aliases: [LocalId] }     NEW fv_outer_empty() : FvOuter, fv_outer_of(binders: [Binder]) : FvOuter
      -- own   : the binders bound INSIDE the walked body, innermost first.
      -- outer : the binders in scope AROUND it (innermost first) — read ONLY to resolve the `EName` of an interpolation fragment re-parsed at emit time, by
      --         its written name (own first, then outer; no hit = a global, not captured) — and the handler aliases among all binders, never captured as values.
      -- `ELocal(l)` not in own -> CapLocal(l).  `EOp` via `ViaAlias(a)`, a not in own -> CapAlias(a).  `EOp` via `ViaRecv(x)` -> a read of the local x.
      -- `ESym` -> nothing.  The Perceus fn/arm result slots (`local_minted(ret_slot_name(RsFn | RsArm))`) are never captured, as before.
      -- "which of these binders does `e` read" (emit_c `ru_*` / `ubind_scope`): fv_expr(e, [], fv_outer_of(binders), []) and keep the `CapLocal`s.
pat_bindings / pat_bindings_loop / pfield_bindings_loop : (pat, acc: [String]) : [String]  =>  (pat, acc: [Binder]) : [Binder]   -- desugar, emit_c, protos, tailfuse, tailgraph read `.id` / `.name`
params_to_names(ps) : [String]  =>  params_binders(ps: [Param]) : [Binder]
block_unused_lets : (stmts, opt_final) : [String]  =>  : [LocalId]         any_use_of : (uses, nm: String)  =>  (uses: [Use], l: LocalId)
```
Variant table (emit_shared.kai): `VariantEntry = EV(home, name, arity, kinds, tag)` is UNCHANGED (the row keeps the text it emits).
```
register_variants  : (decls)  =>  (decls: [Decl], tab: SymTab)     -- driver, emit_c part A (643), part C (2), kir_dump
variant_ctor_excess: (decls)  =>  (decls: [Decl], tab: SymTab)     -- infer_type_collisions
NEW evar_home(v) : Option[String], evar_arity(v) : Int, evar_kinds(v) : [Int]   (beside evar_name(v); the tag is ctor_ix's ctor_row_tag(v))
      -- a node's constructor is found by id: `ctor_ix_row(ix, s)` / `ctor_ix_site(ix, e)`, then these accessors. The `evar_find*` / `evar_slot_kinds*`
      -- (home, name) family keeps its signatures for a constructor the EMITTER names by literal (`"Some"`, `"Ok"`); nothing in emit-sat's files calls it.
collect_variant_to_head : (variants, decls)  =>  (variants, decls, tab: SymTab)      -- emit_c part B, emit_native_proto
stdlib_head_tag_int     : (home, tname, decls)  =>  (home, tname, decls, tab: SymTab) -- emit_native_proto
variant_head_tag_for / variant_head_tag_for_in : + trailing `tab: SymTab`
collect_pimpl_rows      : (ds, fns, acc)  =>  (ds: [Decl], fns: [EFn], tab: SymTab, specs: SpecNames, acc)   -- emit_c part B, emit_native_core, emit_native_proto.
      -- A native caller that only roots / registers rows may pass `spec_names_empty()`: a specialisation's row never registered (its parsed target names no type).
```
Effects by id (emit_shared.kai) — every `[String]` of effect names is `[SymId]`:
```
main_row_labels(decls, entry) : [SymId]      unconditional_default_effects() : [SymId]      ensure_unconditional_defaults(row: [SymId]) : [SymId]
add_missing_effects(extra: [SymId], row: [SymId]) : [SymId]      default_effects_absorbed_by_handler(decls) : [SymId]      runner_entry_row(decls) : [SymId]
find_effect_default_block(eff: SymId, decls) : Option[DefaultBlock]      effect_default_block_all_extern(eff: SymId, decls) : Bool
default_block_owner_mo(eff: SymId, decls) : Option[String]               default_block_owner_sym(eff: SymId, decls) : SymId
      -- the block is the one the declaration `eff` carries (effect_default.kai `effect_decl_of`): a homonym's block is never picked.
```
Other signatures (emit_shared.kai):
```
ffi_kind_of_ty : (t)  =>  (t: TypeExpr, tab: SymTab)      ffi_ret_kind_of : (opt)  =>  (opt: Option[TypeExpr], tab: SymTab)   -- `FfiRetUnknown` names the type; emit_c part B, extern_fn_types, ffi_v2
ty_to_string : (t)  =>  (tab: SymTab, t: Ty)      ty_string_list(tab, ts)      row_to_string_suffix(tab, r)     -- table first, like infer's printers; driver, emit_c
AliasMap = AM([AliasBinding], Bool) over infer-B's `AB(LocalId, Bool, SymId)`:
  alias_map_push(m, l: LocalId, esym: SymId)      alias_map_lookup(m, l: LocalId) : Option[SymId]
  alias_map_lookup_sym, alias_map_remove, alias_remove_loop, alias_map_remove_all, rewrite_alias_stmts_scoped, StmtsRewrite  =>  REMOVED (a binder that shadows an alias has another id)
  rewrite_alias_kind: `EField(ELocal(a), op)` with `a` an alias in scope  =>  `EOp(OpRef { eff, op: written(canonical_cap_getter(eff, op)), via: ViaAlias(a) | ViaAmbient })` (ambient where the tag was suppressed)
```
emit_tcrec_live.kai — the ledger is typed; `EmitCtx.tcpend` / `LowerSt.tcpend` and every `[String]` that carried it become `[TclEntry]`:
```
pub type TclEntry = TclScr(String) | TclTok | TclUnw(String) | TclBinder(LocalId)     -- scrutinee C alias / register, the arm token, an unwind push under a fiber local, a pattern binder
tcl_scr_entry(cvar) / tcl_tok_entry() / tcl_unw_entry(f) : TclEntry      tcl_is_scr / tcl_is_tok / tcl_is_unw (e: TclEntry) : Bool
tcl_reg(e) : String  -- the register of a scrutinee entry (as before; "" for a binder)      tcl_unw_reg(e) : String      NEW tcl_binder(e) : Option[LocalId]   (kir_lower_walk tcp_drop_entries: `ls_reg_of(st, l)`)
tcl_push_binders : (pend, names: [String])  =>  (ns: LocalNames, pend: [TclEntry], ls: [LocalId])   -- `ns` because an earlier entry SPELLED like a new binder is the same C variable and is dropped
tcl_filter : (pend, e)  =>  (ns, pend, e)      tcl_filter_stmts : (pend, stmts)  =>  (ns, pend, stmts)
tcl_goto_entries : (pend, args)  =>  (ns, pend, args)      tcl_goto_drops : (pend, args)  =>  (ns, pend, args)     -- emit_c part C passes cx.locals
tcl_consumes : (e, nm: String)  =>  (ns: LocalNames, e: Expr, l: LocalId)      tcl_span_mentions : (span, nm: String)  =>  (span: String, w: Written)   -- as ownership asked
tcl_cname : (e)  =>  (ns: LocalNames, e: TclEntry)
tcl_hold, tcl_unhold, tcl_drop_unw, tcl_without_tok, tcl_scr_count, tcl_exit_entries, tcl_exit_drops, tcl_has_goto, tcl_has_exit, tcl_has_bang : same arity, `[TclEntry]` for `[String]`
```
emit_reuse_*.kai / emit_ffi_c.kai — a list of binders is `[LocalId]`; a function that only formats C text keeps taking the text:
```
emit_reuse_donate.kai  rd_scan_capable : (decls)  =>  (decls: [Decl], tab: SymTab, specs: SpecNames) : [String]     -- emit_c part C, kir_lower_fns (the set is keyed by emitted symbol, as before)
emit_reuse_slots.kai   cons_direct_binders : (head_pat, rest_name: String) : [String]  =>  (head_pat: Pattern, rest: LocalId) : [LocalId]
                       rv_untaken_slot_drops / rv_taken_slot_clears (…, taken: [LocalId])      rv_taken_binders(subs, ks, i, taken: [LocalId]) : [LocalId]
                       reuse_slot_decl(nm: String, slot, own), cons_rest_bind(rest_name: String, read, own) : unchanged (C text in)
emit_reuse_nested.kai  rv_slot_settles : (subs, ks, token, taken: [String], vs)  =>  (ns: LocalNames, subs, ks, token, taken: [LocalId], ctors: CtorIx)
                       rv_slot_leaf_dups : (subs, ks, taken, vs)  =>  (ns: LocalNames, subs, ks, taken: [LocalId], ctors: CtorIx)
                       rv_cell_binds : (sp, cell, vs)  =>  (sp: Pattern, cell: String, ctors: CtorIx)        rv_bind(nm: String, k, slot) : unchanged (C text in)
```
What emit-sat's files CALL in other batches (say so here if you chose otherwise; the call is then the one to change):
```
head_tag_decl.kai (front-load-A)  impl_target_head_tag(home, tname, decls, tab: SymTab)      ctor_owner_head_tag(home, ctor: String, decls, tab: SymTab)   -- `tab` right after the declarations
evidence_scan.kai (checks)        obl_eff_handled_anywhere(all, ds, eff: SymId)
unbox.kai (ownership)             expr_has_interp_use(e: Expr, b: Binder) : Bool      uses_have_in_lam(uses: [Use], l: LocalId) : Bool   -- arms_pbind_safe_for_fast_path holds the `PBind`'s binder
reuse_cells.kai (ownership)       CellKid = KidBind(Int, LocalId) | KidDrop(Int) | KidCell(Int, ReuseCell)      reuse_ctor_kinds(ctors: CtorIx, r: DeclRef, isubs: [Pattern]) : [Int]
                                  reuse_slot_cells(subs, ks, ctors: CtorIx) : [(Int, ReuseCell)]      reuse_is_cell(sp, k) : unchanged
ffi_v2.kai (typer-sat)            ffi_kind_of_ty_reg(t: TypeExpr, reg: [ExternTypeReg], tab: SymTab)
infer.kai (D)                     label_to_string(tab: SymTab, l: Label) : String
fnreg.kai (mono)                  scan_uses_expr / scan_uses_in_stmts : [Use] with infer-A's `U(LocalId, Int, Int, Bool)`      fn_index.kai  fnx_decl_sig(fx, fid: FnId) (as at the base)
type_home.kai (typer-sat)         type_home_table(decls) : TyHomeTab, payload_slot_kind(hs, home, te) : base signatures
handler_alias.kai (front-resolve) clause_state_aliases(bound_outside: (String) -> Bool) : [String], canonical_continuation() : String, canonical_cap_getter(esym, op) : String : base signatures
modules.kai (front-load-A)        pat_binders(pat, acc: [Binder]) : [Binder]   (as posted)
```
(agent emit-sat)

## kir needs (agent kir) — what `kir_lower*.kai` / `kir_pat_alias.kai` / `kir_reuse_*.kai` call in files of other batches
Written as below; if you own one of these and chose another shape, post it under this heading and the kir call is the one to change.
```
emit_tcrec_live.kai (emit-sat)  the goto-tail ledger: [String]  =>  [TclEntry]   -- kir NAMES the entry type (`LowerSt.tcpend: [TclEntry]`, `pend0: [TclEntry]` params): `pub type TclEntry`,
      -- whatever its constructors; kir reads an entry only through: tcl_is_scr / tcl_is_tok / tcl_is_unw (e: TclEntry) : Bool, tcl_reg(e) : String (a scrutinee entry's register),
      -- tcl_unw_reg(e) : String, and NEW `tcl_local(e: TclEntry) : LocalId` (a binder entry's binder; kir then asks `ls_reg_of(st, l)`).
      -- kir builds entries with tcl_scr_entry(reg: String), tcl_unw_entry(f: String), tcl_tok_entry(), tcl_push_binders(pend, names: [LocalId]), tcl_hold / tcl_unhold(pend, f: String, t: String).
      -- the scans take the declaration's binder map first (an interpolation span reads a binder by its text, as unwind's tcl_consumes):
      --   tcl_filter(ns: LocalNames, pend, e), tcl_filter_stmts(ns, pend, ss), tcl_goto_entries(ns, pend, args) : [TclEntry]; tcl_exit_entries(pend), tcl_without_tok(pend), tcl_drop_unw(pend, f), tcl_has_exit(body) unchanged.
emit_shared.kai (emit-sat)   LamInfo / ClauseInfo captures: kir follows emit_c part B above: `caps: [Capture]`, `Capture = CapLocal(LocalId) | CapAlias(LocalId)`, and
      -- `lam_locals(l: LamInfo) : LocalNames`, `clause_locals(c: ClauseInfo) : LocalNames` (the ENCLOSING declaration's binder map). kir reads `LamInfo(_, ef, line, col, params, caps, body, _)`
      -- (8 fields as at the base) and `CI(line, col, _, head, params, body, stateful, enc_fn, caps, _, esym)` (11 fields as at the base, the effect's id LAST, like ownership's unwind files).
      -- A capture's register: `CapLocal(l)` is spelled `ln_text(locals, l)`, `CapAlias(a)` is `"__alias_id__" + ln_text(locals, a)`.
emit_shared.kai (emit-sat)   block_unused_lets(stmts, tail) : [LocalId]  (as emit_c part B asks)
emit_shared.kai (emit-sat)   VariantEntry = EV(home, name, arity, kinds, tag) read as at the base (5 fields); kir reaches a row ONLY by `ctor_ix_row(st.ctor_ix, ctor id)`: no `evar_find*` / `evar_slot_kinds*` /
      -- `evar_qual_home` call is left in kir. bang_success_variant(opt_ty) : Option[String] read as at the base (a core constructor's word; kir derives the core id from it).
emit_shared.kai (emit-sat)   main_row_labels(decls, entry), runner_entry_row(decls), default_effects_absorbed_by_handler(decls), ensure_unconditional_defaults(row), add_missing_effects(extra, row)
      -- : [String] of effect names  =>  kir is written against [SymId] (effect ids; `ensure_unconditional_defaults` adds `core_effect_id(CeMutable)`).
      -- find_effect_default_block(eff_name, decls) / default_block_owner_sym(eff_name, decls) / collect_effect_names / collect_op_qualified_names / const_names / build_globals: no longer called by kir
      -- EXCEPT the collector's seed: kir calls `collect_decls(decls, lc_new(build_globals(fns, variants, collect_effect_names(decls), collect_op_qualified_names(decls), const_names(decls))))`
      -- exactly as at the base (kir_lower_fns.kai collect_lams); if the collector's entry changes, that one call follows.
fnreg.kai (mono)             EFn(String, Int, Option[String], Option[UFnSig], FnId)  =>  kir reads `EFn(arity, mo, sig, fid)` (the name slot gone; the name is `decl_name(tab, fnid_sym(fid))` / `spec_text(specs, fid)`).
fn_index.kai (mono)          kir calls fnx_callee(fx, fid), fnx_of_sym(fx, fid), fnx_decl_sig(fx, fid), fnx_kai(fx, fid), fnx_ufn_by_csym(fx, csym: String), fnx_build_decls(decls, fns); no by-name lookup
      -- (`fnx_qualified`, `fnx_kai_qualified`, `fnx_ufn_sig_first(fx, name, mo)`, `kai_builtin_name` are no longer called by kir).
ast.kai                      RawArmBinder = RAB(String, Int, Int): kir matches it by the binder's source position (line, col) only, as before; the name slot is not read.
ast.kai                      TcrecStep.sym / TrmcStep.sym (the enclosing fn's emitted symbol) read as at the base (String). NEED for TrmcStep the spine constructor's ID: `cname: String` is the
      -- builtin-cons marker or a constructor NAME, and a name does not say which constructor. kir is written against a NEW field `TrmcStep.ctor: SymId`
      -- (`sym_none()` for the builtin cons spine; the pass that builds the step has the constructor call's `ESym(c, _)` in hand); `cname` keeps the text the dump prints.
handler_alias.kai (front-resolve)  live_state_aliases(stateful, local_scope: [String]) : [String]  -- called as at the base with the registers in scope; each result is a MINT KEY: kir binds `binder_minted(key)`.
reuse_cells.kai (ownership)  CellKid = KidBind(Int, String) | …  =>  KidBind(Int, LocalId); kir calls `reuse_pattern_cells(p: Pattern, ix: CtorIx) : [(Int, ReuseCell)]` (the constructor's slot kinds come
      -- from its row by id, not from (home, name, vs)).
refinements.kai (tools-B)    assert_reported_binding(cond, opt_msg) : Option[ContractIdent]; kir reads `CtrId(ident, idnode, _)` and prints `reported_binding_span(ident)`: `ident` whatever type the owner gives it.
effect_instance.kai (typer-sat)  inst_key_of_tes(eff: String, args: [TypeExpr]) : String, key_of(eff: String, texts: [String]) : String, ty_text(t: Ty) : String, inst_keys_match(a, b) — called as at the base
      -- (the effect NAME is `decl_name(tab, eff id)` text: a key is a runtime string).
perceus_read_walk.kai / perceus_reads.kai (perceus)  reuse_uses.kai (nobody's batch; changed by kir to `[LocalId]` in and out) calls `pcs_use_index(e, gc: [LocalId]) : UseIx`,
      -- `ni_bucket(ix, g: LocalId)` and reads `Rd(l: LocalId, _, _, _, _)`: say here what the index is keyed by if not so.
```
Changed by kir in files of NOBODY's batch:
```
reuse_uses.kai       reuse_gc_uses / reuse_taken_uses : (cargs, gc: [String]) : [String]  =>  (cargs: [Expr], gc: [LocalId]) : [LocalId];  reuse_taken_binders(gc: [LocalId], uses: [LocalId]) : [LocalId]   -- emit_c.kai 3800, 3801, 4324 call them
kir_reuse_cells.kai  kc_cells_own / kc_uses_outside_cells : uses: [String]  =>  [LocalId]
kir_reuse_held.kai   rh_own / rh_take_slots : names: [String]  =>  [LocalId];  rh_repeats(xs: [LocalId]) : [LocalId]
emit_native_fnix.kai NFnIndex gains `tab: SymTab`; nfnx_build(fns)  =>  nfnx_build(fns: [KFn], tab: SymTab)
```
front-parse CORRECTION (supersedes the `nt_collect` / `nt_protocols` / `nt_type_spellings` lines of the front-parse block above):
  a name that stops resolving ("cannot find X" / "X is not imported") halts the whole check before the type checker, so these three STAY in decl_names.kai — no import to add:
```
decl_names.kai  nt_collect  : (decls: [Decl], target: String) : [NEnt]  =>  (decls: [Decl], ent: (SymId) -> Option[NEnt], target: String) : [NEnt]
      -- `ent` answers the entry of an id; with the program's table: `nt_collect(decls, (s) => st_ent(tab, s), target)`.
      -- (decl_names.kai cannot import symtab.kai, which imports it.) Callers: name_uses, tycon_scheme_home, name_report, value_scope_table, driver.
decl_names.kai  nt_protocols  : (decls, target)  =>  (decls: [Decl], ent: (SymId) -> Option[NEnt], target: String) : [NEnt]
decl_names.kai  TypeSpelling  : TSp(String, String, String)  =>  TSp(SymId)      -- a type the stream declares, by id
decl_names.kai  nt_type_spellings  : (decls: [Decl], target: String) : [TypeSpelling]  =>  (decls: [Decl]) : [TypeSpelling]
      -- kept only so the name resolves; proto_orphan.kai needs neither: the impl target's `DeclRef` is `DrSym(type id)`, its home `sym_owner(tab, id)`.
```
There is NO `nt_collect` in symtab_build.kai. Unchanged from the block above: `parse_program(file, src, toks, home)`, `PParser.names`, `st_build(rows, target)`, `st_program(rows)`, `st_ent`, `NCMethod`, `impl_scope` / `method_sym` / `method_ent`, `nent_sym`.
An impl's scope is recomputable from the node: `impl_scope(home, l, c)` with the `DImpl`'s own line and column.
doc_attr.kai  DocItem  : CORRECTION to the tools-A line above: the item is { kind: String, line: Int, is_pub: Bool, doc: Option[String], head: DocHead } with NEW `pub type DocHead = DocDecl(SymId) | DocFn(SymId, [Param], Option[TypeExpr], RowExpr) | DocAxiom(String) | DocImpl(DeclRef, TypeExpr, [DocHead]) | DocOpaque` -- it holds the declaration's id and signature pieces, never the body; no file outside doc_attr.kai reads `.name` / `.sig` (agent tools-A)
fmt.kai / fmt_writer.kai  import compiler.symtab (+ decl_text, local_names)  -- like doc_attr.kai, legal only once decl_names.kai stops importing compiler.modules: at the base parse -> refinements -> refine_help -> fmt -> symtab -> decl_names -> modules -> parse is a cycle (agent tools-A)

### infer env API — notes added by infer-A while repairing (read these too)
- CHECKER TRAP: the checker prints only `cannot find X` errors while ANY name is unresolved and stops before type checking. A call you write against a part-A function that does not exist in your worktree yet (`env_local`, `env_sym`, `env_bind_local`, …) therefore hides every type error of the whole compiler. Paste `<S>/infer-A/env_api_stubs.kai` (real signatures, placeholder bodies) at the end of your own part while you work and delete it before committing.
- Small helpers part A now defines (use them, do not re-define): `ref_name(r: DeclRef) : String` (the name a reference still spells: a type parameter, a scalar, an unsettled type; "" once settled), `ref_text(tab: SymTab, r: DeclRef) : String` (a reference as a diagnostic names it), `te_ref_sym(hv: TyHomeView, r: DeclRef) : SymId` (the type a `TyName` ref names from `hv`), `core_ctor_sym(name: String) : SymId` (`Some`/`None`/`Ok`/`Err`).
- Imports part A added at the top of infer.kai: `compiler.written`, `compiler.decl_text`, `compiler.core_syms`. If your part needs another import (e.g. `compiler.local_names`), say so in your report: the import block is in part A.
infer.kai  check_field_privacy  : (df, home: TyHomeView, mod_decls, recs)  =>  (df: DiagFiles, tab: SymTab, home: TyHomeView, mod_decls: [Decl], recs: FieldRecs)  -- caller in part D
infer.kai  check_default_coverage  : (df, mod_decls, all_decls, homes)  =>  (df: DiagFiles, tab: SymTab, mod_decls: [Decl], all_decls: [Decl], homes: TyHomes)  -- caller in part D
infer.kai  collect_union_rows  : (ds, hv)  =>  (ds: [Decl], tab: SymTab, hv: TyHomeView)  -- a variant's component type is read from its constructor's name; callers in part D (`tc_shared`, `collect_program_data_inherited`)
infer.kai  collect_unions  : (ds, hv)  =>  (ds: [Decl], tab: SymTab, hv: TyHomeView)  -- returns `UI(union's SymId, comps, line, col)`
infer.kai  collect_top_type_names  : (ds: [Decl]) : [String]  =>  (ds: [Decl], tab: SymTab) : [String]  -- callers: part D, infer_union_collisions.kai
infer.kai  collect_constructors  : (ds, acc)  =>  (ds: [Decl], homes: TyHomes, acc: [CtorEntry])  -- `CtorE(type id, fn id)`; caller in part D (`sh.homes`)
infer.kai  constructor_lookup  : (cs, tname: String)  =>  (cs: [CtorEntry], tsym: SymId) : Option[CtorEntry]  -- caller `try_constructor_call` (part B): the callee names a type by id; the call target is `ESym(fn id, spec_none())`
infer.kai  constructor_type_name  : (te) : Option[String]  =>  REMOVED; `pub fn constructor_type_sym(te: TypeExpr, hv: TyHomeView) : SymId` (`sym_none()` when `te` names no declared type)  -- infer_type_collisions.kai
infer.kai  build_ty_env_inherited  : (seed, inh, inherited, decls, unions, top_names, rk_table, tab, target, homes)  =>  (seed, inh, inherited, decls, unions, top_names, rk_table, tab: SymTab, homes: TyHomes)  -- `target` is gone (a declaration registers under its own id); caller in part D
infer.kai  ty_names  : (ts: [Ty]) : [String]  =>  (tab: SymTab, ts: [Ty]) : [String];  union_component_names : (unions, parent) => (tab: SymTab, unions: UnionTable, parent: SymId)  -- part D
infer.kai  inf_eff_has_op / inf_find_eff_for_alias_op  : the effect is its SymId (`rl_sym(c)`); `inf_find_eff_for_alias_op(components, op_to_eff, op_name: String)` keeps its shape  -- part C
infer.kai  LU  : LUAt(String, Int, Int) | LUBlocked(String) | LUUnused(String)  =>  the same over `LocalId`  -- infer_dump.kai (`inf_last_use_for(p.binder.id, uses)`)
infer.kai  root_written  =>  REMOVED (no declaration is respelled per home any more)  -- one caller left in part C (`bare_module_effects` area): delete the use
infer.kai  label_to_string  : (l: Label)  =>  (tab: SymTab, l: Label)   -- part D owns it; part A's printers call it so
infer.kai  label_display  : (owners: [HeadOwnerEntry], lab: String)  =>  (tab: SymTab, owners: [HeadOwnerEntry], l: Label)   -- part D owns it; part A's printers call it so (the head is `inf_tycon_display_ow(tab, owners, l.sym)`)
infer.kai  spelled_head_owners  : part C owns it; part A calls it unchanged as `spelled_head_owners(decls, [])` and passes the result to `inf_tycon_display_ow(tab, owners, effect id)`
infer_index.kai  NEW  idx_prepend_local(ix: NameIndex[a], l: LocalId, payload: a) : NameIndex[a], idx_chain_local(ix: NameIndex[a], l: LocalId) : [a]  -- the env's local index (file in nobody's batch; edited by infer-A)
tycon_ref.kai  tsh_tycon_at  : (v, r: DeclRef, nm: String, args)  =>  (v: TyHomeView, r: DeclRef, args: [Ty]) : Ty  -- part A calls it so (the name is inside `r`); owner typer-sat
field_privacy.kai  field_hidden / priv_read_diag / priv_construct_diag : part A calls them UNCHANGED (`field_hidden(v, rec_name: String, rec: SymId, fd)`), the record's name read from `decl_name(tab, rec)`; whoever drops the name parameter fixes the two calls in part A's `walk_field_priv_expr` / `walk_priv_field_inits`
tycon_scheme_home.kai  part A still calls with TEXT, unchanged: `tsh_sym(v, nm: String) : SymId`, `tsh_tycon(v, nm: String, args) : Ty`, `tsh_fn_visible(hs, reader: String, home: String, nm: String)`, `tsh_homes(ds, tab, views, target)`, `tsh_view`, `tsh_at`, `tsh_target`; it no longer calls `tsh_slot`, `tsh_decl_sym`, `tsh_decl_home`

## surface API (surface_lower, loop_lower, resume_*, deep_*, const_pattern, brand_transp, vec_surface, lit_mint) — owner: agent surface

Every driver-called pass of this batch runs BEFORE the resolver and reads written text; four of them need a declaration's written
name and take the table. The table passed must hold a row for every `DFn` / `DConst` of `ds` (impl methods included: their `NCMethod`
rows), or the name reads as "" and the pass silently stops recognising it.
surface_lower.kai  surface_lower_decls  : (ds: [Decl]) : [Decl]  =>  (ds: [Decl], tab: SymTab) : [Decl]  -- the name a contract's violation message spells (`wrap_with_contracts(…, fn_name: Written = decl_name(tab, fnid_sym(fid)), …)`). driver.kai 4963, 5046. `surface_lower_expr(e)` unchanged (emit_c.kai 5430, 10822)
loop_lower.kai  lower_loop_intrinsics  : (ds: [Decl]) : [Decl]  =>  (ds: [Decl], tab: SymTab) : [Decl]  -- the blocked-name scan reads each `DFn` / `DConst` name from the table. driver.kai 5418
deep_hoist.kai  deep_hoist_decls  : (ds: [Decl]) : [Decl]  =>  (ds: [Decl], tab: SymTab) : [Decl]  -- a branch calling its own function by written name stays in its frame. driver.kai `c_bounded_decls` (4675)
const_pattern.kai  desugar_const_patterns_decls  : (decls: [Decl]) : [Decl]  =>  (decls: [Decl], tab: SymTab) : [Decl]  -- the constants' written names. driver.kai 5506
brand_transp.kai  brand_transp_decls  : (ds: [Decl]) : [BrandTranspRec]  =>  (ds: [Decl], tab: SymTab) : [BrandTranspRec]  -- `BTR(String, _)` stays keyed by the written fn name, `brand_transp_lookup(ix, name: String)` unchanged: the brand walk looks a callee up before the resolver. desugar.kai 4506
deep_outline.kai  do_branch  : (fname: String, weight: Int, branch: Expr)  =>  (tab: SymTab, owner: FnId, weight: Int, branch: Expr)  -- only deep_hoist.kai calls it
resume_segment.kai  HandleParts  : { body, eff: String, hsym: SymId, tys, init, alias: Option[String], clauses, ret }  =>  { body, eff: DeclRef, tys, init, alias: Option[Binder], clauses, ret }  -- built only by resume_decls.kai
resume_segment_state.kai  NEW  param_of(b: Binder, at: Expr) : Param;  param(name: String, at) = param_of(binder_written(name), at)
resume_anf.kai  anf_bind_until(e, pick, prefix, n)  -- signature kept. It mints `binder_minted(prefix + n)` read as `ELocal(local_minted(prefix + n))` (unwind_operands.kai, after the resolver). `anf_resuming_operand` (resume_split, before the resolver) writes `binder_written` / `EName`.
anf_ops.kai  anf_is_atom  -- signature kept; `ELocal(_)` and `EOp(_)` are atoms beside `EName` / `ESym` / `EModCall`
vec_surface.kai  VecArmKind  : VArmList([Pattern], Option[String]) | VArmAs(String, [Pattern], Option[String]) | VArmCatch(Option[String])  =>  the same with `Binder` for each `String`  -- infer.kai only matches the constructors
vec_surface.kai  vec_lit_build / vec_match_build / fixed_vec_lit_build, fixed_vec_match.kai fixed_vec_match_arm / fixed_vec_arm_build, lit_mint.kai mint_literal  -- signatures kept. What they build, for the typer that re-synthesises it:
      -- a core builtin callee (`vec_empty`, `vec_reserve`, `vec_push`, `vec_length`, `vec_get`, `vec_tail_from`) is `ESym(builtin_sym(name), spec_none())`;
      -- the scrutinee read is `ELocal(l)`; the alias of a non-local scrutinee is `binder_minted("__vecm_<line>_<col>")` read as `ELocal(local_minted(…))`;
      -- an arm's binders are the pattern's own `Binder`s, in `SLet(PBind(b), …)`;
      -- a fixed vec's rest annotation is `TyDim(TyName(DrSym(core_ty_sym(CtVec)), [TyName(ref_written(<scalar>), [])]), USym(n))`;
      -- a minted Decimal / DecimalBig is `ERecordLit(DrSym(core_decl_ty_id(c)), fields)` typed `TyCon(core_decl_ty_id(c), [])`, BigInt's call typed `TyCon(core_decl_ty_id(CdBigInt), [])`.
Needed from other owners (written against; the owners published the same shapes above): `clause_continuation : Option[Binder]`, `pat_binders : [Binder]`, `ub_rest_sentinel : Binder`, `decl_map_named_bodies` with `(FnId, Expr)`, `wrap_with_contracts(…, fn_name: Written, …)`. `tuple_type_ref`, `build_tuple_field_inits`, `build_tuple_pfields`, `home_fn_ref`, `var_cell_alias`, `home_is` are called with their base signatures.
fmt_shape.kai  fmt_subst_var  : CORRECTION to the tools-A line above: (e: Expr, name: String, to: ExprKind) : Expr  =>  (e: Expr, l: LocalId, to: ExprKind) : Expr; there is no `fmt_reads_binder`. The formatter recognises the parser's minted binders by id only, as front-parse specifies: `local_eq(p.binder.id, local_minted("__pipe"))`, `params_point_free`, `local_minted(lam_pat_binder_name(l, c))`, and their reads as `ELocal` (agent tools-A)
fmt_writer.kai  NEW  pub fn fmt_with_locals(s: FmtSt, locals: LocalNames) : FmtSt  -- fmt_program sets it per declaration (`ln_of_decl(d)`), so an `ELocal` the sugar recovery does not consume still prints its binder's name (agent tools-A)
front-parse FINAL (the "CORRECTION" block just above is WITHDRAWN; the first front-parse block stands as published, since front-resolve already coded against it):
```
symtab_build.kai  nt_collect(decls: [Decl], tab: SymTab, target: String) : [NEnt]      -- callers import compiler.symtab_build
symtab_build.kai  nt_protocols(decls: [Decl], tab: SymTab, target: String) : [NEnt]
decl_names.kai    nt_collect / nt_protocols / nt_type_spellings / TypeSpelling          -- GONE from this file (nt_only stays)
```
value_scope_table.kai (typer-sat) calls `nt_collect` and does not import compiler.symtab_build at the base: add the import with the call.
### protocol registry — part A, addendum (agent protos-A)
Two more part-A functions that part B calls take the table first, for the same reason as `proto_type_name` (the head TEXT of a field's type — the writer's name,
the `__pimpl_BinSerialize_<Head>_…` it calls — and a field type can be `TyName(DrSym(s), _)`: a tuple, a head a scope pass settled):
```
protos.kai (A)  derive_binser_put          : (acc, v, ftype, writers, line, col)  =>  (tab: SymTab, acc: Expr, v: Expr, ftype: TypeExpr, writers: [String], line: Int, col: Int) : Expr
protos.kai (A)  derive_binser_field_guard  : (ftype, idx, pos_e, then_e, line, fcol, col)  =>  (tab: SymTab, ftype: TypeExpr, idx: Int, pos_e: Expr, then_e: Expr, line: Int, fcol: Int, col: Int) : Expr
      -- part B's callers (derive_binser_put_payload, derive_binser_sum_decode_chain) sit under derive_binser_put_sum(tab, …) / derive_binser_sum_from_bytes(tab, …): thread that `tab`.
      -- derive_binser_field_next / derive_binser_field_values / binser_ok_cursor / binser_call / binser_lib / binser_option_elem / binser_leaf_ty: unchanged.
type_text.kai   type_text  : (t: TypeExpr) : String  =>  (tab: SymTab, t: TypeExpr) : String   -- part A calls it so (tools-A's request above; owner typer-sat). It replaces part A's
      -- `type_text_with(t, (n) => hs_display(n, …))`: no declaration is respelled, so there is no home spelling to strip from a row label.
```
(agent protos-A)

## LEAD NOTE — `EOp` constructor clash (read this if you construct an `EOp`)
`ast.kai` has two constructors named `EOp`: the new expression node `ExprKind.EOp(OpRef)` and the old
effect-declaration operation `EffectOp = EOp(String, [String], [Param], TypeExpr, Int, Int)`. Patterns pick by
scrutinee type; CONSTRUCTION picks the old one, so `EOp(OpRef { … })` reports "wrong number of arguments to
`EOp`: expects 6, got 1" in your worktree. The lead renames the OLD one to `EffOp(name, tparams, params, ret,
line, col)` at merge. In your files: keep writing `EOp(r)` for the expression node (leave that one error, list
it as cross-file), and write `EffOp(…)` wherever you touch the six-field effect-operation constructor or
pattern (untouched `EOp(six fields)` sites are renamed by the lead).
effect_alias_scope.kai  CORRECTION (front-load-B): `EffSpelling` / `ESp` / `es_note` / `es_lookup` are NOT removed, they stay as TEMPORARY shims so resolve.kai still resolves: `EffSpelling = ESp(SymId)`, `es_note(spellings, name: String, s: SymId)` adds nothing, `es_lookup(spellings, spelled: String)` answers `sym_none()`. resolve.kai should drop `eff_spellings` and both calls; then the block is deleted. (A pub name another file still calls must not vanish: `cannot find X` is a name-resolution error, and the checker stops there and reports NO type error for any file.)
front-parse DONE (commit 48769f60 on its worktree branch). Served: `local_names.kai ln_with_binder(ns, b) : LocalNames`, `ln_ids_named(ns, w: Written) : [LocalId]` — as requested above.
At merge these names stop resolving until their callers are repaired (each halts the check before the type checker):
  `nt_collect` / `nt_protocols` in a file that does not import compiler.symtab_build (name_uses.kai, name_report.kai, value_scope_table.kai at the base);
  `nt_type_spellings` / `TypeSpelling` (protos.kai:522, proto_orphan.kai at the base).

## EmitCtx
(agent emit_c-A; owner of `emit_c.kai` part A = everything before `fn arms_shared_ctor`, the `EmitCtx` type and the import block. PRELIMINARY, posted first; lines are only added below.)
```
emit_c.kai  EmitCtx  : three NEW fields, declared LAST (after `root_mod`):  tab: SymTab,  specs: SpecNames,  locals: LocalNames
      -- cx.tab    : the program's symbol table (declaration names: `decl_name(cx.tab, s)`, homes: `decl_home` / `sym_owner`)
      -- cx.specs  : the monomorphiser's emitted names of specialisations (`spec_text(cx.specs, FnId(s, inst))`)
      -- cx.locals : binder id -> written name for the declaration being emitted. Set it with `emit_ctx_with_locals(cx, ln_of_decl(d))` wherever a
      --             declaration starts; a lambda helper / clause body emitted separately gets `lam_locals(l)` / `clause_locals(c)` (the ENCLOSING declaration's map).
emit_c.kai  EmitCtx.lcs, .borrowed, .kind1_raw  : [String]  =>  [LocalId]   (as part B wrote);   EmitCtx.tcpend : [String]  =>  [TclEntry] (emit-sat's ledger type)
emit_c.kai  ReuseDonor  : ReuseDonor(String, [String])  =>  ReuseDonor(SymId, [LocalId])   -- the donor cell's CONSTRUCTOR id and its positional sub-pattern binder ids
      -- (`local_none()` for a slot with no usable PBind: `local_eq` never matches it). Read by part C (`cx.reuse_donor`, ~14603): compare the rebuilt ctor's id with `sym_same`, a rebuild arg `ELocal(l)` with `local_eq`.
emit_c.kai  import block (part A) gains: compiler.written, compiler.decl_text, compiler.local_names, compiler.spec_names   (compiler.symtab, compiler.decl_names, compiler.ctor_ix, compiler.core_prim_ix were there). Parts B / C: say here if you need another import; do not edit the block.
emit_c.kai  NEW (part A)  emit_ctx_with_locals(cx: EmitCtx, ns: LocalNames) : EmitCtx
emit_c.kai  NEW (part A)  c_local_of(cx: EmitCtx, l: LocalId) : String   -- the C identifier of a local: `c_local(written_text(ln_text(cx.locals, l)))`
emit_c.kai  NEW (part A)  emit_local_text(cx: EmitCtx, l: LocalId) : String   -- the local's written name as text (`written_text(ln_text(cx.locals, l))`), for the mangling helpers that take the name
```
Full-literal `EmitCtx { … }` sites that are NOT in part A and must add `tab: …, specs: …, locals: …` (every `emit_ctx_with_*` helper in part A carries the three through):
  part B: `emit_lam_helpers` (~5697), the fn-body context (~8379), ~9086, ~9180, ~9525;   part C: the two program-entry contexts (~11018, ~11190: `tab` / `specs` come from `emit_program`'s new parameters, `locals: ln_empty()`).

## infer part C API (`try_op_call` … up to `pattern_collect_binds`) — owner: agent infer-C
Part C is written against the `## infer env API` (A), the `## infer state API` (B: `CS(sym, …)`, `OEA(SymId, Written, Int)`, `OPV(SymId, LocalId, [Int])`, `AB(LocalId, Bool, SymId)`,
`lookup_alias_sym` / `lookup_alias_taggable(aliases, l)`, `record_op_res(r, eff, op: Written, alias: Option[LocalId], tagged, line, col)`, `st_push_op_payload_vars(st, esym, cell, ids)`,
`label_in_cell`, `alias_cell`, `rec_pick(rs, sym)`, `SI(SymId, …)`), protos-B's `DispHead = DhSym(SymId) | DhScalar(Ty)` / `dh_ref_is` / `pir_serves` / `dh_same` / `pir_sym : SymId`,
tools-B's `rv_flow(defs: (SymId) -> [TyDef], …)` / `RcRefuted(LooseWhy)` / `loose_text` / `rc_assert(b: Binder, …)`, tools-A's `fmt_expr_text(e, tab, locals)`, front-parse's `ln_with_binder`,
typer-sat's `tsh_tycon_at(v, r: DeclRef, args)`. Part-C functions other parts / files call, with their new signatures:
```
infer.kai  synth_record_lit      : (st, rh: DeclRef, nm: String, fis, line, col)  =>  (st: InferState, rh: DeclRef, fis: [FieldInit], line: Int, col: Int) : Inferred   -- part B `synth`: `ERecordLit(rh, fis) -> synth_record_lit(st, rh, fis, e.line, e.col)`. The typed node is `ERecordLit(DrSym(record id), fis)` when the record is found (the reader's ladder: `tsh_tycon_at`), the reference as it came otherwise.
infer.kai  PickResult            : { st, inst_ty, fresh, unit_fresh, mod: String }  =>  { st, inst_ty, fresh, unit_fresh, sym: SymId }   -- the picked FUNCTION's id (`TyCandidate.sym`). Part B `try_bare_call_narrow` (≈10127): the callee is `mk_sym(pick.sym, …)`, the call site `CS(pick.sym, …)`. `pick_by_first_arg(st, candidates, target_ty)` keeps its signature.
infer.kai  lookup_op_eff_arity   : (table: [OpEffArity], key: String "Eff.op") : Int  =>  (table: [OpEffArity], eff: SymId) : Int   -- every op of an effect records the effect's tparam count, so the effect's id answers. Part B ≈9146.
infer.kai  eff_tparam_arity      : (table, eff: String) : Int  =>  (table: [OpEffArity], eff: SymId) : Int   -- part B ≈8862
infer.kai  is_effect_in_env      =>  REMOVED (as part A asked): `env_has_effect(env, eff: SymId)`. Part B `check_handle_effect_known` ≈9304.
infer.kai  arg_names_match       : (reg_args: [String], recv: String, want: [String]) : Bool  =>  (reg_args: [DeclRef], recv: DispHead, want: [DispHead]) : Bool   -- a PIR's protocol-argument refs against the call's heads (`dh_ref_is`; no args registered = the homogeneous `[recv]`, `dh_same`). protos.kai `find_impl_with_args`: `arg_names_match(ans, recv, args)`.
infer.kai  build_head_owner_map  : (decls, modules, target_mod, edition)  =>  (decls: [Decl], tab: SymTab, modules: [ModuleEntry], target_mod: String, edition: Edition) : [HeadOwnerEntry]   -- driver.kai (4 calls): the table of `decls`. Entries are `HOE(type id, [modules])`.
infer.kai  spelled_head_owners   : (ds, acc)  =>  (ds: [Decl], tab: SymTab, acc: [HeadOwnerEntry]) : [HeadOwnerEntry]   -- evidence_inject.kai:66, infer part A ≈2196. No declaration is home-spelled any more, so the entry is `HOE(s, [home])` for every module type / effect that is CONTESTED (`sym_contested(tab, s)`): the ones `inf_tycon_display_ow` shows as `mod.T`.
infer.kai  head_owner_seed()     -- signature kept; `[HOE(core_ty_sym(CtList), ["list"])]`
infer.kai  string_strip_alias_tag  =>  REMOVED (no caller; the `@alias` key is gone). `string_index_of_at`, `dotted_prefix`, `dotted_suffix`, `string_contains_dot`, `real_c_lit` kept as they were.
infer.kai  KEPT signatures: try_op_call, try_ufcs_call, synth_field, synth_index, synth_binop, synth_unop, synth_if, synth_lambda, synth_lambda_threaded, synth_lambda_pinned, synth_list_expr, synth_range, synth_pipe, synth_map_pipe, synth_flat_map_pipe, synth_filter_pipe, synth_block, synth_stmts, synth_lit_mint_or_err, synth_let_rhs_or_mint, maybe_mint_literal, maybe_lower_decimal_lit, refine_coerce, refine_coerce_tail, refine_call, coerce_ctor_args, coerce_lambda_args, ctor_coerce_expected, rc_mentions_in, mask_local_mutable_demand, drain_deferred_fields, decl_row_concrete_labels, ty_env_with_head_owners, take_first_tys, ty_var_id, eff_is_cell_effect.
```
What part C produces in the typed tree (besides `## effect op callee` above):
- a UFCS call `x.f(args)`, a narrowed pipe callee (`xs |> f(..)` on a contested `f`), a collection pipe (`|`, `||`, `|?`) and a `Map` / `HashMap` index read call `ESym(function id, spec_none())` — never `EModCall`. `Map`/`HashMap` `get` is `home_fn_ref(core_coll_home(c), "get")`.
- a pipe whose head module ships no `map`/`flat_map`/`filter` calls the core builtin `ESym(builtin_sym(op), spec_none())`; only when the env holds no such builtin is the callee left `EName(written(op))` (typed `TyAny`, as an unresolved name was).
- the typer's temporaries are minted binders: `binder_minted("__slice_coll")`, `binder_minted("__slice_lo")` (`xs[a..b]`), `binder_minted("__rv")` (a refinement check), read as `ELocal(local_minted(key))`.
- THE INDEX PLACEHOLDER (`index_coll.kai`): part C binds its type as `env_bind_local(env, binder_minted(ixc_name()), mono(t))` and the node stays `EMark(MkIndexColl)`; part B's `synth` must read an `EMark(m)` as `env_local(env, local_minted(mark_name(m)))` (it was `synth_name_read(st, mark_name(m), e)`).
Needs / questions:
```
infer.kai imports (part A's block; part C adds no import line): compiler.written, compiler.decl_text, compiler.core_syms (builtin_sym), compiler.call_site_target (home_fn_ref), compiler.local_names (ln_empty, ln_with_binder)
infer.kai (part B)  callee_display_name(callee: Expr) : Option[String]  -- part C (emit_pipe_wants_apply) calls it as `callee_display_name(st, stage)` with the InferState, so it can print an `ESym` from `st.env.tab` and an `ELocal` from the env's binder. infer-B: say here if you keep `(callee)` or take something else.
mutation_carriers.kai (owner?)  written against: `CarrierShape = CS(SymId, [TypeExpr])`, `sum_shape(s: SymId, vs: [Variant])`, `record_shape(r: RecInfo)`, `mutating_type_names(shapes) : [SymId]`, `ty_may_carry_mutation(t: Ty, dmut: [SymId])`, `writes_only_first_arg(tab: SymTab, s: SymId) : Bool` (was `(name: String)`: the callee's declaration; a bytes writer is its builtin id, a derived BinSerialize writer its declared name's prefix)
mutation_callees.kai (owner?)   written against: `opaque_binders(params, body) : [LocalId]`, `callee_is_opaque(callee: Expr, opaque: [LocalId])`, `fn_value_is_transparent(e: Expr, opaque: [LocalId])`, `pipe_as_call` unchanged
record_fields.kai  record_lit_field_errors(rec: String, given, declared)  -- called as at the base (`rec` = the record's declared name, message text)
```

### protocol registry — part B, follow-up (agent protos-B; after part A's entry)
Part B follows part A's two corrections and uses its new helpers; what changed against part B's first entry:
```
protos.kai (B)  lower_protocols calls `generate_dispatchers(reg0, ps_tab(ps))`; it no longer calls `proto_type_name` at all: the derive validators read a head with part A's `proto_type_ref(te)`, compare with `refs_same(tab, a, b)`, print with `ref_text(tab, r)`, and resolve an impl head with `proto_ref_sym(ps, r, from)` (all four as part A posted them)
protos.kai (B)  the BinSerialize sum derive names the constructors BY ID, as part A does: `PVariant(DrSym(ctor), subs)` and, for the decoded value, `ESym(ctor, spec_none())` (`mk_sym`) as the callee / the nullary value. The resolver must leave both alone.
protos.kai (B)  derive_binser_put_sum(tab, variants, writers, line, col) / derive_binser_sum_from_bytes(tab, tname, variants, line, col) : signatures KEPT as posted (part A calls them so); `tab` is no longer read by either (constructors by id) — it can be dropped from both and from part A's two calls at merge
protos.kai (B)  collect_binser_types(ps, decls, impls, acc) : [String]  -- KEPT for part A's `writers` (the names, via `ref_text`); part B's own provider sets (BinSerialize, Ord) are `[DeclRef]`, each head settled through the scope of the file that writes it (`sv_resolve` from the type's / the impl's home; a core type by its core id; a scalar and a type parameter stay written)
protos.kai (B)  ty_head_name  : (t: Ty) : Option[String]  =>  (tab: SymTab, t: Ty) : Option[String]   -- CORRECTION: KEPT, for display only (= `dh_text(tab, h)` of `ty_disp_head(t)`); what a type dispatches on is `ty_disp_head(t)` (monomorph.kai 1024 / 1042)
impl_identity.kai / impl_index.kai  -- `ii_key_bucket(idx, proto, h: DispHead)` needs `DispHead` visible in impl_index.kai, which impl_identity.kai imports: declare the type there (or in ast.kai). Part B only needs the name `DispHead`, its two constructors `DhSym` / `DhScalar`, and the functions listed above. (owner protos-sat)
impl_identity.kai  impl_symbol_renames(impls, tab, root)  -- the `tab` part B passes is `st_of_entries(<the lowered impl functions' rows>)`: it holds those rows and nothing else
resolve_callee.kai  ProtoRung(op: String, arity: Int, dispatcher name: String, home: Option[String], exported: Bool)  -- built by `proto_rungs` exactly as at the base (the op label and the dispatcher's emitted name as text; no id beside either). (owner front-resolve)
driver.kai  -- `lowered.rows : [NEnt]` are the functions the protocol lowering generated (dispatchers, lowered impl methods, derive writers / shims, the derives' method rows): extend the tables the resolver and the typer read with them (`st_extend(tab, lowered.rows)`); `validate_resolved_protocols(typed_prog, rd.tab, proto_reg, diag_ctx)`; `stamp_targets(proto_reg.impls, lowered.derived, …)` gets `[SymId]`. (owner driver)
```
OPEN (for the lead; part B cannot settle it): the interpolation sugar's `show` is `EModCall(<core home>, "__proto_show")`, and `try_rewrite_proto_call` reads "only that home's protocols may answer" off that node KIND. Once the dispatchers have table rows, the resolver settles `m.f` to `ESym(dispatcher id)` (front-resolve's entry), and a homed reference can no longer be told from a bare one: a user protocol declaring `show` in the calling module would then capture `#{x}`. Part B keeps the `EModCall` arm (it asks `pid_ref_disp(reg.ids, callee)`), which stays correct only while that node survives to the rewrite.
(agent protos-B)

## ownership API, part 2 (agent ownership) — purity*, reuse_*, vec_raw, raw_border; corrections to the unwind block above
```
unwind (CORRECTION)  unw_spelled_twice(ns, xs) does NOT exist: `unw_dups(xs: [LocalId], seen: [LocalId], acc: [LocalId]) : [LocalId]` stays, by id (after unique_binders two binders of a
                     function share an id exactly when they share a spelling, as mono posted). `locals: LocalNames` in unw_frame / unw_fn_frame only serves the interpolation spans.
unwind_walk.kai      unw_walk(ucx, ns: LocalNames, pend, bound, banned, params: [LocalId], body, traps)
unwind_reads.kai     NEW unw_filter_stmt(ns, pend: [LocalId], s: Stmt) : [LocalId]
unwind_binders.kai   NEW unw_has(xs: [LocalId], l: LocalId) : Bool, unw_blank_binder(b: Binder) : Bool
unwind_clauses.kai   NEW unw_reads_continuation(f: Expr, k: LocalId) : Bool   -- `ELocal(k)` or `ELocal(local_minted(canonical_continuation()))` (handler_alias.kai, base signature: the mint key)
purity.kai   PurityMark : PM(String, Option[String], Purity)  =>  PM(FnId, Option[String], Purity);  pmark_name => pmark_fn(m) : FnId;  purity_of(marks, name, mo) => purity_of(marks, f: FnId)
purity.kai   purity_analyze(decls, entry) : unchanged
purity.kai   purity_const_syms : (marks)  =>  (marks: [PurityMark], tab: SymTab, specs: SpecNames) : [String]   -- emit_c.kai (2), emit_native.kai:92; the symbol is c_sym of fnreg's fn_emit_name(tab, specs, f)
purity.kai   NEW purity_const_fns(marks) : [FnId]   -- the same set by identity, for a backend that holds the FnId
purity.kai   dump_purity_marks : (marks)  =>  (marks: [PurityMark], tab: SymTab, specs: SpecNames)   -- driver.kai:5932
purity_traps.kai  trapfree_syms : (decls)  =>  (decls: [Decl], tab: SymTab, specs: SpecNames) : [String]   -- emit_native.kai:93;  NEW trapfree_fns(decls, tab) : [FnId]
reuse_cells.kai   as emit-sat and kir wrote: CellKid = KidBind(Int, LocalId) | KidDrop(Int) | KidCell(Int, ReuseCell);  reuse_ctor_kinds(ctors: CtorIx, r: DeclRef, isubs: [Pattern]) : [Int];
                  reuse_slot_cells(subs, ks, ctors: CtorIx);  reuse_pattern_cells(p: Pattern, ctors: CtorIx);  reuse_is_cell(sp, k) unchanged
reuse_ctors.kai   reusable_tags : (decls, variants, entry)  =>  (decls: [Decl], ids: ProtoIds, ctors: CtorIx, entry: SymId) : [Int]   -- emit_c.kai:9694, emit_native_proto.kai:80
                  -- the caller hands the index it already built WITH its root (`cx.ctor_ix`): the old one was built without a root and found the root's constructors by spelling.
reuse_ctors.kai   ReuseCtorName / RCN  =>  REMOVED;  reuse_ctor_home_name(ctors, ctor) : Option[ReuseCtorName]  =>  RENAMED reuse_ctor_id(ctors: CtorIx, ctor: Expr) : Option[SymId]
                  reuse_same_ctor : (phome, pname, b: ReuseCtorName)  =>  (p: DeclRef, b: SymId) : Bool   -- the PVariant's reference and the rebuild's constructor id (emit_c.kai 4208-4211, 4632, 4666: the layout is ctor_ix_row(cx.ctor_ix, id))
vec_raw.kai       signatures unchanged (a record is found by its TyCon's id: rec_find)
raw_border.kai    signatures unchanged (an owning callee: ESym, ELocal, EOp; a callee an interpolation fragment still spells, EName / EModCall, owns too)
expr_positions.kai / expr_exists.kai / ast_map.kai  signatures unchanged; ELocal and EOp are leaves
```
What these files NEED (written as below):
```
fnreg.kai (mono)      fn_emit_name(tab: SymTab, specs: SpecNames, f: FnId) : Written (purity.kai, purity_traps.kai);  classify_unbox_sig(entry, tparams, params, opt_ret, row, body) (purity.kai)
live_fns.kai (mono)   live_decls(decls, ids: ProtoIds, entry) (reuse_ctors.kai)
ctor_ix.kai (mono)    sym_is_ctor(tab, s), ctor_ix_row(ix, s), ctor_ix_site(ix, e), ctor_row_tag(row) : base signatures
```
infer.kai (part A)  DONE: commit 9ceac9e6 (branch agent-af40ed86210898b33) holds the real part A (everything before `fn subst_empty`) and infer_index.kai; `git show 9ceac9e6:stage2/compiler/infer.kai` to read the definitions the `## infer env API` section describes. Part A calls, as their owners posted them: `tsh_tycon_at(v, r, args)` (typer-sat), `label_to_string(tab, l)` / `label_display(tab, owners, l)` (part D), `spelled_head_owners(decls, tab, [])` (part C), `OEA(esym, written(op), arity)` (part B). If `Label` gains `cell: LocalId` (infer-B's request), part A builds its labels in ONE place: `fn row_label(eff: SymId, ty_args: [Ty]) : Label` (add `cell: local_none()` there).
infer.kai (part A)  `import compiler.sym_read` is kept ONLY because parts B/C/D still call `sym_local_kind` at the base; drop the import line with the last call. `import compiler.core_prims` is removed (nothing in infer.kai used it once the env stopped asking `core_builtin_names()`): re-add it if your part needs it.

## infer part B — what it now calls in other parts / files (agent infer-B; part B type-checks against exactly these)
```
part A (requests / assumptions beyond the env API section):
  fn env_local_name(env: TyEnv, l: LocalId) : Written     -- NEW, REQUESTED: the name of the bound local `l` (its `KLocal(Binder).name`; empty when unbound).
                                                          -- Part B prints a local callee / the clause continuation with it (callee_display_name, synth_local_read).
  fn constructor_lookup(cs: [CtorEntry], tsym: SymId) : Option[CtorEntry]   -- by the constructed type's id (was by type name); CtorE(type, fn) as announced
  fn ty_env_collect_candidates(env, nm) : [TyCandidate]   -- as announced; part B reads `.sym` (name_read_sym: what an unsettled bare EName reads as)
  imports: part B needs `import compiler.written`, `compiler.decl_text`, `compiler.core_syms` in infer.kai's import list (part A's lines; committed apart, see the report)
part C:
  fn lookup_op_eff_arity(table: [OpEffArity], eff: SymId, op: Written) : Int   -- was (table, key: String "Eff.op")
  fn eff_tparam_arity(table: [OpEffArity], eff: SymId) : Int                   -- was (table, eff: String)
  fn synth_record_lit(st, rh: DeclRef, fis: [FieldInit], line, col)            -- was (st, rh, nm, fis, line, col); synth passes `ERecordLit(rh, fis)`
  type PickResult = { st, inst_ty, fresh, unit_fresh, sym: SymId }             -- `sym` (the picked declaration) replaces `mod: String`; part B builds `mk_sym(pick.sym, …)` and `CS(pick.sym, …)`
  fn coerce_ctor_args(st, callee, …)   -- synth_call now passes the SETTLED callee (`rc.expr`: an EName/EModCall the typer settled is already `ESym`)
part D:
  type VariantsCheck = VCOk([SymId]) | VCNotSum | VCHasPayload([SymId])        -- constructor ids (was names); `EVariantsOf(te, ctors)` takes VCOk's list as is
  fn variants_of_type(env: TyEnv, tsym: SymId) : [String]                      -- part B (emit_union_focus_notes) still joins these as DISPLAY names; keep a names-returning form
front-resolve (handler_alias.kai, homonym.kai, resolver):
  ClauseCont = CcNone | CcOwn(Binder) | CcInLambda(Binder); clause_cont_reaches(cc, l: LocalId)   -- (above)
  HomCand = HCand(SymId, Ty)   -- was HCand(module name, Ty): the candidate DECLARATION
  hom_choose(cs: [HomCand], arg: Ty) : Option[SymId]                           -- was Option[String] (a module name)
  HomPick = HPick(Int, Int, SymId)                                             -- was HPick(line, col, module name): the picked declaration; hom_retarget rewrites the callee to `ESym(s, spec_none())`
  DeferredHom = DHom(String, Ty, Int, Int)                                     -- unchanged: the written name of the unsettled callee
  the stateful-clause aliases `state` / `log` / `__var_cell` (handler_alias user_state_aliases / var_cell_alias) have no binder in the AST: part B binds each as
      `binder_minted(name)`, so the RESOLVER must write a read of one (when no enclosing local of that name takes it over) as `ELocal(local_minted(name))`.
      A read the resolver sends to an enclosing local carries that binder's own id, so the typer no longer asks "is the name bound outside".
```
What part B's walker PRODUCES (typer output, for every pass after it):
- a name the resolver left unsettled — `EName(w)` in value position, `EModCall(m, f)` — is settled through the table and the node becomes `ESym(id, spec_none())`
  (was: the node kept its text and later passes re-read it). An `EName` survives the typer only as the callee of a deferred homonym call (until `hom_retarget`)
  or for a name nothing declares.
- a contested bare call settled by its first argument (`try_bare_call_narrow`): callee `ESym(picked id, spec_none())` (was `EModCall(mod, name)` / a dispatcher `ESym`).
- `Type(args)` through `#[constructor]`: callee `ESym(constructor fn id, spec_none())` (was `EModCall(home, fn)`).
- a clause continuation call, whatever the clause names it: callee `ELocal(local_minted(canonical_continuation()))` (was `EVar("resume")`).
- `EMark(m)` read as a value is typed from `env_local(env, local_minted(mark_name(m)))` when the typer bound a temporary under the mark's spelling
  (so part C must bind `MkIndexColl` as `env_bind_local(env, binder_minted(ixc_name()), sc)`), else from `env_sym(env, builtin_sym(mark_name(m)))`.
- `EVariantsOf(te, [constructor ids])`; `EHandle` keeps its `DeclRef` and alias `Binder` untouched.
(agent infer-B)

### protocol registry — part B, DONE (agent protos-B; commit b4e2b132 on its worktree branch)
```
protos.kai (B)  CORRECTION to the follow-up above: `tab` IS read by derive_binser_put_sum(tab, …) / derive_binser_sum_from_bytes(tab, tname, …): part B threads it to part A's `derive_binser_put(tab, acc, v, ftype, writers, line, col)` and `derive_binser_field_guard(tab, ftype, idx, pos_e, then_e, line, fcol, col)` (part A's addendum). Both signatures stand as first posted.
protos.kai (B)  find_impl_with_args calls infer-C's `arg_names_match(ans: [DeclRef], recv: DispHead, args: [DispHead])` as infer-C posted it.
protos.kai (B)  adds NO import line: it needs `compiler.written` and `compiler.symtab`, which part A adds to the import block.
```
Names part B calls that exist only once their owners land (each is a `cannot find` until then): part A — `proto_type_ref`, `ref_text`, `refs_same`, `proto_ref_sym`, the three imports; protos-sat — `DhSym`, `DhScalar`, `dh_ref_is`, `dh_text`, `rows_with_renames`. With those in place part B type-checks except two recorded mismatches: `ii_key_bucket(idx, proto, h: DispHead)` (impl_index.kai) and `arg_names_match` (infer.kai part C).
(agent protos-B)

## checks API (agent checks) — borrow_*, cont_crossing, core_reach, evidence*, lint*, mutation_*, rawsafe, spawn_*
PRELIMINARY, posted early; lines are only added below.
borrow_infer.kai  borrow_infer_params  : (self_name: SelfRef, pnames: [String], body: Expr) : [String]  =>  (tab: SymTab, self_name: SelfRef, pnames: [Binder], body: Expr) : [Binder]  -- CORRECTION to the perceus line above (one more parameter, FIRST): "an arm's tail rebuilds a constructor" was an uppercase-initial test on the callee's NAME; it is `sym_is_ctor(tab, s)` of the callee `ESym(s, _)` now, and no id says its class without the table. perceus.kai pcs_borrow_params_hybrid passes the table it tests its own constructors with. (agent checks)
borrow_escape.kai  binf_absent / binf_absent_fields / binf_absent_elems  : (nm: String, …)  =>  (l: LocalId, …)   -- a read is `ELocal(l)`; binf_arms_binders_borrowed(self_name, i, arms) unchanged. Only borrow_infer.kai calls them. (agent checks)

### infer part B — CORRECTIONS to its two sections above (agent infer-B; part B is committed against these)
```
infer.kai (part A)  env_local_name  -- REQUEST WITHDRAWN: part B defines `fn local_name_in(env: TyEnv, l: LocalId) : Written` itself, over part A's `idx_chain_local(env.locals, l)` + `KLocal(b)`.
infer.kai (part B)  callee_display_name  : (callee: Expr) : Option[String]  =>  (st: InferState, callee: Expr) : Option[String]   -- as part C calls it. (`EName` / `EModCall`: the written text;
      -- `ESym`: decl_name(tab, s); `ELocal`: the bound binder's name; `EOp`: `Eff.op`; `EField(_, f)`: `f`.)
infer.kai (part C)  lookup_op_eff_arity(table, eff: SymId) : Int   -- part B calls it as part C posted it (two arguments), not with the op label.
infer.kai imports   part B's commit adds `import compiler.written`, `compiler.decl_text`, `compiler.core_syms` right after `import compiler.ast`: the SAME three lines, in the same place,
      -- as part A's commit 9ceac9e6, so the two merge as one change.
```
Checked: part B type-checks against part A's commit 9ceac9e6 (its real part A + infer_index.kai) with 2 errors left, both `no field 'cell' on Label`
(`label_in_cell`, `label_cell`): the `Label.cell: LocalId` slot requested from the lead above. With that field added, every `Label { sym, ty_args }` literal needs `cell: local_none()`.

## emit_c part B — FINAL (agent emit_c-B; `arms_shared_ctor` .. before `emit_nullary_enum_seed`; adds to the PRELIMINARY block above)
Part B type-checks (0 errors) against a scratch copy stubbed with exactly the shapes posted in this file by emit_c-A (`## EmitCtx`), emit-sat, ownership,
mono, tools-B, front-resolve (CORRECTION block), front-parse, and the assumptions on part A listed here.
Signatures of part B that parts A / C call and that CHANGED:
```
emit_c.kai (B)  tcp_rc_binders(pat, cx) : [String]  =>  : [LocalId]                                              -- part A: emit_unw_owning, emit_unw_let_owned (so `emit_unw_let_owned(s, cx) : [LocalId]`)
emit_c.kai (B)  emit_lam_helpers(src, lams, fns, fx, …, ucx)  =>  (src, lams, tab: SymTab, specs: SpecNames, fns, fx, variants, cls, consts, effs, recs, stamps, core_ix, ctor_ix, evf, root_mod, ucx)   -- part C (2 calls)
emit_c.kai (B)  emit_clause_forwards(cs, effs)  =>  (cs: [ClauseInfo], effs: [Decl], tab: SymTab)                -- part C (2)
emit_c.kai (B)  effect_struct_decls(decls)  =>  (decls: [Decl], tab: SymTab)                                    -- part C (2)
emit_c.kai (B)  fn_forward_decls(decls, fx, capable, const_syms, entry)  =>  (decls, tab: SymTab, specs: SpecNames, fx, capable, const_syms, entry)   -- part C (2)
emit_c.kai (B)  thunk_forward_decls(decls)  =>  (decls: [Decl], tab: SymTab, specs: SpecNames)                   -- part C (2)
emit_c.kai (B)  rd_donatable_filter(capable, decls, fx)  =>  (capable: [String], decls, tab: SymTab, specs: SpecNames, fx)   -- part C (2); the set stays keyed by emitted symbol
emit_c.kai (B)  emit_proto_runtime_tables(decls, variants, fns, entry)  =>  (decls, tab: SymTab, specs: SpecNames, variants, fns, ids: ProtoIds, ctors: CtorIx, entry)
      -- part C `emit_main_wrapper`: `ids` / `ctors` feed ownership's `reusable_tags(decls, ids, ctors, entry)` (pass `cx.ctor_ix`, the index built WITH its root).
emit_c.kai (B)  UNCHANGED for callers: emit_expr, emit_expr_raw, emit_fn_body, fn_bodies, emit_clause_bodies, emit_handle_env_typedefs, emit_lam_forward_decls, emit_all_tests / _benches / _checks,
      emit_match_default, tcp_arm_body_cx, emit_bang, box_wrap, raw_c_type, unbox_boxed_scalar, hoist_slot_member, evf_join_params, has_main_decl, test_register_list, collect_fixed_vec_typedefs,
      `ev_name(name: String, mo)` (text in, as before). NEW `ev_name_of(tab: SymTab, effs: [Decl], s: SymId) : String` (= ev_name of the effect's declared name and its `eff_mo`).
emit_c.kai (B)  `type SharedCtor` (the line right above `fn arms_shared_ctor`, formally in part A's range)  =>  `SC(SymId, Int)` -- edited by part B, which is its only reader. Part A: please leave it.
emit_c.kai (B)  `emit_fn_body` sets the declaration's binders itself (`EmitCtx { ...cx, locals: ln_of_decl(d), … }`), the test / bench / check runners too (`ln_with_expr(ln_empty(), body)`): part C need not.
```
What part B CALLS in part A, with the signature it assumes (part A owns them; all their callers are in part B unless noted):
```
emit_ident_value(name, fid, fx, lcs, fns, variants, core_ix, root_mod)  =>  emit_ident_value(fid: FnId, cx: EmitCtx) : String
      -- by id only: the fn's thunk closure (`fnx_callee`), the builtin's (`core_ix_boxed`), else the C local of its emitted name. Part B's `emit_sym_value` asks the
      -- constructor index and the const catalogue first, and reads a local itself (`emit_local_value`): the `lcs` / spelled-ctor / const-by-name paths have no caller left.
const_body_of(consts, name: String)  =>  const_body_of(consts: [Decl], s: SymId) : Option[Expr]   -- the body of the `DConst` whose id is `s`. `const_body_of_home` has no caller left.
eff_mo(effs, name: String)  =>  eff_mo(effs: [Decl], s: SymId) : Option[String]                    -- the module origin of the `DEffect` whose id is `s`
emit_ob_names(args_e, i) : [String]  -- KEPT as the temps' mint KEYS (`"bc0"`, …): part B scopes them with its `emit_ctx_declaring(cx, keys)` (below), so a read `ELocal(local_minted("bcN"))` spells `kaiv_bcN`
emit_unw_let_owned(s, cx) : [LocalId];  emit_unw_vars(names: [LocalId], cx) : [String];  emit_unw_own(names: [LocalId], cx);  emit_ctx_with_lcs / _kind1_raw ([LocalId]);  emit_ctx_with_tcpend(cx, [TclEntry]);
emit_ctx_kind1_raw_has(cx, l: LocalId);  pat_kind1_binders(pat, cx, acc: [LocalId]) : [LocalId];  emit_local_text(cx, l) (as part A posted; part B defines none)
BASE signatures, called unchanged: emit_pat_test, emit_pat_binds, pbind_raw_alias, emit_match_arms, emit_match_expr, emit_match_switch, match_borrow_inner, emit_nullary_read(row),
      emit_variant_tag_test(scr, home, name: String, variants) (emit_bang names the core `Some` / `Ok` by literal), emit_raw_rcore, emit_user_call_seq, emit_arg_vals_masked, emit_ob_hold / _binds / _drops / _rewrite_args,
      emit_args_have_owned_borrow, emit_unw_call, emit_unw_scr*, emit_unw_owning, emit_ctx_route_brand, emit_ctx_tcpend_after*, emit_ctx_with_enc_fn / _evf / _hoisted / _in_region / _unw, emit_block_is_region,
      emit_list_expr, emit_range_expr, emit_record_lit, emit_rec_stamp, emit_vec_get_field_fused, emit_evf_thunk_frame, evf_join_args, ufn_call_result, region_copy_out.
No longer called by part B: ec_callee, ec_callee_sym, ec_local_callee, ctor_qual_home, emit_variant_call, evar_find_qual / evar_find_tag (the `EModCall` value arm and the by-name read are gone).
```
Private helpers part B defines (do not re-define these names in parts A / C; use them if they fit):
```
locals_have(ls: [LocalId], l) : Bool      pat_local_ids(pat) : [LocalId]      param_local_ids(params) : [LocalId]
emit_scope_spelled(cx, name: String) : Option[LocalId]   -- the innermost binder in `cx.lcs` emitted under `name` (C-shadow test of a `let`; the name of an interpolation fragment parsed at emit time)
locals_declaring(ns: LocalNames, keys: [String]) : LocalNames      emit_ctx_declaring(cx, keys: [String]) : EmitCtx   -- C locals no node declares, as `binder_minted(key)`: in `cx.locals` (and `cx.lcs`)
emit_body_locals(ns: LocalNames) : LocalNames   -- `lam_locals(l)` / `clause_locals(c)` plus the handler-state aliases and the continuation (a lambda inside a stateful clause captures them)
clause_live_aliases(stateful, spelled: (String) -> Bool) : [String]      emit_fn_text(tab, specs, f: FnId) : String      type_ref_text(tab, r: DeclRef) : String
emit_sym_value(src, fid, cx)      emit_local_value(l, ty, cx)      emit_spelled_value(w: Written, ty, cx)      emit_ctx_const(cx, body)      ClauseParts / clause_parts(c)
capture_same / capture_locals / capture_text(ns, c) / merge_capture_set / list_drop_local(s)
```
Called in OTHER batches with the BASE signature (no owner posted a change; if yours changed, part B's call follows):
```
evidence_frame.kai (checks)   frame_slots_of(frames, csym: String), build_evidence_frames(decls, entry), frames_index, frame_shadow(evf, enc: String, key: String)   -- keyed by emitted symbol / instance key
effect_instance.kai (typer-sat)  inst_key_of_tes(eff: String, args: [TypeExpr])   -- as kir: the effect's name is `written_text(decl_name(tab, id))`
emit_ffi_c.kai / ffi_v2.kai   emit_ffi_v2_shim_body(sig, csym, params, ret, reg), ffi_sig_uses_v2(params, ret, reg)
emit_reuse_donate.kai  rd_is_capable(csym, capable), rd_splice_param, rd_wrap_body, rd_null_arg      emit_shared.kai  bang_success_variant(opt_ty) : Option[String], proto_dispatch_parts / PDP(String, String) / pdp_core /
      pdp_op_id_int, ffi_extern_symbol_of(body, name: String), PimplRow PR(csym, proto, tname, arity, sig, op, home, lent), c_local / c_alias_id / c_user_sym / user_clause_sym / handler_scoped_sym / runner_enc_fn / find_lam / lam_symbol / clause_sym
ast.kai  TcrecStep { sym, dropmask, params } read as at the base (emitted text), handed to part C's `tcrec_emit_goto(src, sym, dropmask, params, args, cx)`
```
Notes for the lead:
- an `EName` / `EModCall` reaching `emit_expr_boxed` (only an interpolation fragment the lift left unparsed could spell one) reads the binder in scope of that spelling, else spells `kaiv_<name>`: the
  by-name const / constructor / qualified-fn fallbacks are gone (they looked a declaration up by name).
- emit_shared's new `pub type Capture` shares its name with the PRIVATE `type Capture = Cap(…)` of spawn_capture.kai. The type checker accepts both (spawn_capture does not import emit_shared); if the
  flat C bundle names a type's symbol after the bare type name, one of the two needs another name.
(agent emit_c-B)

## driver API (agent driver) — driver.kai, symid_dump.kai, written_eq_check.kai, and the decl caches (nobody's batch)
THE TABLE: the driver builds it right after loading, from parser rows only: `tab0 = st_program(core_rows ++ import_rows ++ root_rows)`. It is extended, in order,
with the rows each generator hands up: `tab1 = st_extend(tab0, <refinement predicates>)`, `sym_tab = st_extend(tab1, lowered.rows)` (what the resolver gets; `rd.tab` is that table),
`typed_tab = st_extend(sym_tab, <tail fusion rows>)` (the typer, mono_front's `PScope`), `emit_tab = st_extend(typed_tab, <closure-spec rows>)` (unbox, perceus, tcrec, both emitters, the dumps).
A per-file table `st_of_entries(parsed.names)` serves the stages that run on one parsed file (fmt, migrate, mutate, doc JSON, the load-time checks, the export lists).
Changed by driver in files of NOBODY's batch (the decl caches carry a file's rows beside its decls: a restored module has no parser):
```
cache_ast.kai   CacheDeserialized  : CDs([Decl], String)  =>  CDs([Decl], [NEnt], String)
cache_ast.kai   cache_serialize_module  : (decls, sha)  =>  (decls: [Decl], rows: [NEnt], source_sha: String) : Array[Byte];  cache_roundtrip_check(decls)  =>  (decls: [Decl], rows: [NEnt])
      -- needs `NEnt` / `NCls` to derive BinSerialize (front-parse: served) — it calls `to_bytes(r)` and `nent_from_bytes`.
cache.kai       cache_format_version 19 => 20 (the payload gained the rows)
core_cache.kai  CcLookup.CcHit([Decl])  =>  CcHit([Decl], [NEnt]);  cc_write(cache_dir, content_hex, decls)  =>  (cache_dir, content_hex, decls: [Decl], rows: [NEnt])
user_cache.kai  UcLookup.UcHit([Decl])  =>  UcHit([Decl], [NEnt]);  uc_write(cache_dir, content_hex, dep_hex, decls)  =>  (…, decls: [Decl], rows: [NEnt])
user_cache.kai  uc_lookup  : (cache_dir, root_path, src, toks, search_paths, memo)  =>  (cache_dir, root_path, home: String, src, toks, search_paths, memo)
      -- the blob holds ids derived from the home the file was parsed under, so the content key folds `home` in (two files of one text under two homes no longer share a blob).
```
driver batch, pub signatures:
```
symid_dump.kai        symid_decl_lines(stage, tab, core, ds)  =>  (stage: String, tab: SymTab, specs: SpecNames, core: [String], ds: [Decl]);  symid_own_lines(tab, core, ds)  =>  (tab, specs, core, ds)
      -- an effect is spelled with kir's `kir_eff_name(tab, s)` (handle heads, performs, `own effect`), a function with fnreg's `fn_emit_name(tab, specs, f)`; reads `KPerform(s, _, op, via: KVia, _)` and `EFn(n, _, mo, _, fid)` as kir / mono posted.
written_eq_check.kai  check_written_eq(decls)  =>  (decls: [Decl], tab: SymTab, specs: SpecNames)
```
What the driver CALLS. As their owner posted: parse_program(file, src, toks, home) / `.names`, st_program(rows), fmt_program(decls, tab, …), mutate_sites(decls, tab), doc_module_json(…, tab), doc_sig(…, tab), doc_te(t, tab),
surface_lower_decls(ds, tab), lower_loop_intrinsics(ds, tab), deep_hoist_decls(ds, tab), desugar_const_patterns_decls(decls, tab), es_collect(decls, tab, target), ts_collect(decls, tab, target, effs: [String]),
collect_alias_cands(decls, tab, df), validate_pub_access(segs, tab, …), pa_hidden_from_root(segs, tab, target), nu_const_coll(decls, tab, target), nu_scan_consts(decls, tab, target, picks), collect_effect_ids(decls),
build_kind_ctx(decls, tab, reg, sites, view), enforce_refinements_decls(decls, tab, pred_fns), lower_protocols(…) + `.rows` / `.derived`, build_ty_env(decls, tab), build_head_owner_map(decls, tab, modules, target, edition),
call_site_diags / dump_call_elision(file, decls, tab), validate_resolved_protocols(typed, tab, reg, df), register_fn_decls(decls, tab, specs, entry), register_variants(decls, tab), unique_binders_decls(ds, tab),
lower_to_kir(decls, tab, specs, …), emit_kir(decls, tab, specs, …), dump_purity_marks(marks, tab, specs), ty_to_string(tab, t), env_key_text(tab, e.key), `MonoFront.specs`.
`core_unstamp_decls` is no longer called (front-resolve: identity). `ts_collect`'s and the alias passes' `effs: [String]` are `written_text(decl_name(tab, s))` of `collect_effect_ids`.
ASSUMED (not posted when the driver was written; `tab` right after the declarations, rows handed up — owners: if you chose otherwise, the driver call is the one to change):
```
desugar.kai            lower_axiom_one(d, home: String), lower_axioms(ds, home: String)   -- the home the lowered DFn's id is derived with (a module's tag; the target for a root axiom)
desugar.kai            synthesize_refine_pred_fns(decls, tab, target) : a record with `.decls: [Decl]`, `.rows: [NEnt]`;  refine_pred_fn_syms(decls, tab, target);  collect_refine_alias_names(decls, tab)
desugar.kai            desugar_pos_records_decls(ds, tab, df);  desugar_use_decls(decls, tab, file)
tailfuse.kai           tailfuse_decls(decls, tab, target, pids) : a record with `.decls: [Decl]`, `.rows: [NEnt]`
mono_front.kai         MonoFront gains `rows: [NEnt]` (the closure specialisations `closure_spec_pass` minted) beside `specs`
unbox.kai              unbox_pass(decls, tab, specs, cis, nat, entry)   -- it calls register_fn_decls
fn_bounds.kai          collect_surface_bounds(ds, tab);  bounds_by_sym(ds, tab, recs, target_mod)
law_checks.kai         gen_law_checks(decls, tab, reg, file)
scope_file.kai         sel_with_ctors(sels, scopes, global, decls, tab, root)
modules.kai            qtchk_decls(ds, tab, mt), qtchk_segmented(segs, decls, tab, scopes, mt), qualtype_decls(decls, tab, mt, df), qualtype_segmented(segs, decls, tab, scopes, mt, df), rqc_collect_fx(decls, tab)
value_scope_table.kai  vs_collect(decls, tab, target)
extern_fn_types.kai    validate_extern_fn_types(decls, tab, df)
infer_name_collisions / infer_type_collisions / infer_union_collisions
                       validate_module_same_module_collisions(decls, tab, file), validate_{type,fn,effect,const,axiom}_name_collisions_decls(decls, tab, file),
                       validate_union_collisions_decls(decls, tab, df), validate_variant_capacity(decls, tab, df)
kai_builtin_check.kai  kai_builtin_manifest(ds, tab), kai_builtin_decl_diags(ds, tab, df, is_core)
type_text.kai          type_text(tab, t) (as tools-A / protos-A asked), row_label_text(tab, l)
tycon_ids_dump / type_ids_check   dump_tycon_ids(decls, tab, target), check_type_ids(decls, tab)
infer.kai (D)          infer_program(file, decls, tab, verbose)
infer_dump / rawsafe / emit_c dumps   dump_infer, dump_typed, dump_typed_core, dump_last_use, dump_intervals, dump_rawsafe, dump_borrowsafe, dump_mono, dump_mono_out : (file, decls, tab);  dump_types(decls, tab)
evidence*.kai          check_op_collisions(file, ors, decls, tab), dump_evidence_obligations(file, decls, tab, entry), dump_evidence_arity(file, decls, tab, entry)
emit_c.kai             build_ctor_int_slots(decls, tab), collect_transparent_aliases(decls, tab, effs),
                       emit_program(file, src, decls, tab, specs, bmode, target_origin, regions, stamp_ids), emit_program_modular(file, src, decls, tab, specs, bmode, …), emit_program_modular_cached(file, src, decls, tab, specs, bmode, …)
```
Called with their BASE signatures (say here if yours changed): strip_law_wrappers, report_import_holes, tag-time `mod_names` / `ledger_*`, es_segmented / es_core_decls / eschk_segmented / tschk_segmented / ts_segmented (they already took the table),
qh_* / pqchk_* / qhchk_*, vs_segmented, rqc_decls / rqc_segmented, nu_scope_const_decls, mexp_*, unstable_check_decls, mangle_core_segments, pa_reseg / pa_import_segs, lower_pattern_narrow_decls, lower_extern_opaques, the kind passes,
validate_nominal_row_vars_decls, validate_constructor_uniqueness_decls, validate_extern_type_homes, validate_extern_fn_homes, resolve_alias_cands, expand_aliases_in_decls, expand_ta_decls, rewrite_regions_decls,
split_resume_decls, desugar_index_decls, lower_cell_reads_decls / relower_cell_reads_decls, desugar_var_decls, check_interp_parses, desugar_interp_decls (the table it took), bang_hoist_decls, desugar_range_patterns_decls,
rewrite_nursery_caps_decls, check_nursery_brand_mismatch_decls, resolve_decls_views / resolve_decls_stamped, reg_with_target_ids, tsh_homes, stamp_targets, core_reach / core_cut_misses (`CoreCut(_, dropped)` printed as `[String]`),
tcut_infer_program, resolve_protocol_calls, validate_typer_invariants, region_yield_findings, nr_report_program_tab, report_unused_in_decls, strict_holes_check, mono_front, the report dumps taking a `TypedProgram`
(dump_holes, dump_holes_json, dump_effects_json, dump_effect_holes_json, dump_explain), run_lint / run_lint_json, inject_evidence_params / reindex_evidence_params, promote_scalar_cells, sym_qualify_decls_in / sym_erase_decls_in,
unw_name_operands, native_raw_arm_binders, perceus_pass_in, tcrec_rewrite_decls, check_site_ids, purity_analyze, ctor_site_lines, kir_inline_program, ncore_*, build_native_*.
(agent driver)
emit_c.kai (B)  CORRECTION: the helper is named decl_ref_text(tab: SymTab, r: DeclRef) : String (not type_ref_text): the name a reference names, its declaration's once resolved. (agent emit_c-B)

### tools-B: the derive generators (derive_mint, derive_generic, json_derive, json_shape, layout_derive, layout_synth, layout_validate) — agent tools-B
All run BEFORE the resolver. Follows protos-A's entry EXCEPT the two generator signatures, and why:
```
layout_derive.kai  derive_layout_impl  : (tname: String, shown: String, body, line, col) : [Decl]  =>  (tab: SymTab, t: SymId, body: TypeBody, line: Int, col: Int) : Minted
json_derive.kai    derive_json_impl    : (tname: String, shown: String, body, line, col) : [Decl]  =>  (tab: SymTab, t: SymId, body: TypeBody, line: Int, col: Int) : Minted
      -- NOT `(t, tname, home, …)` as protos-A proposed. WHY: (1) the JSON derive needs the table anyway — a sum's wire tag and its "unknown tag" list are
      -- the CONSTRUCTORS' names (`Var(SymId, _)` has none), and a nested record already settled to `DrSym` is spelled from it; (2) the type's name and
      -- home ARE the table's (`sym_name(tab, t)`, `sym_home(tab, t)` = the string its id was derived with), so passing them beside the id is the dual slot
      -- rule 5 removes. protos.kai:1660 `derive_one`: `derive_layout_impl(ps_tab(ps), t, body, line, col)` / `derive_json_impl(ps_tab(ps), t, body, line, col)`.
      -- `tab` must hold the type's row (the parser's): the generator panics `identity: the derived type has no row…` otherwise.
derive_mint.kai  NEW  pub type Minted = { decls: [Decl], rows: [NEnt] }   -- as protos-A proposed: `.decls` = [the DImpl, the shim DFn], `.rows` = the two methods' `method_ent` rows + the shim's `NEnt(NCFn, name, home, true)`
derive_mint.kai  NEW  minted_none() : Minted, minted_append(a, b) : Minted
derive_mint.kai  NEW  pub type DeriveFor = { tab: SymTab, t: SymId };  derive_type_name(f) : String;  derive_self_ty(f, line, col) : TypeExpr  (= `TyName(DrSym(t), [])`)
derive_mint.kai  NEW  pub type DeriveMethod = DeriveMethod(String op, [Param], TypeExpr ret, Expr body)
derive_mint.kai  NEW  derive_impl(f: DeriveFor, g: CoreProto, methods: [DeriveMethod], line, col) : Minted
      -- `DImpl(DrSym(core_proto_id(g)), [], TyName(DrSym(t), []), methods, line, col, [], sym_none())`, each method `DFn(false, fnid_decl(method_sym(op, scope)), …, None)`
      -- with `scope = impl_scope_keyed(home, "derive <core_proto_name(g)> <type name>")`, rows `method_ent(op, scope)`. protos-A's own generators may call it.
derive_mint.kai  NEW  derive_fn(f: DeriveFor, name: String, params, ret: TypeExpr, body, line, col) : Minted   -- `DFn(true, fnid_decl(sym_derive(NCFn, name, home)), …)` + its row
derive_mint.kai  NEW  derive_ref_text(tab: SymTab, r: DeclRef) : String   -- a type head as text: `decl_name` of a settled ref, the written (dotted when qualified) name otherwise
derive_mint.kai  NEW  derive_bare_text(r: DeclRef) : String              -- the text of a head written bare, "" for a qualified or settled one
derive_mint.kai  derive_op_ref, derive_lib_ref, derive_builtin_ref : signatures kept (`ESym(s, spec_none())`; the fallback of derive_op_ref is `EName(written(op))`)
derive_generic.kai  derive_generalize  : (d, pname: String, tname: String, tps)  =>  (d: Decl, pname: String, t: SymId, tps: [String]) : Decl   -- as protos-A posted; a method signature names the type as `TyName(DrSym(t), [])`
derive_generic.kai  derive_bound_params : unchanged
layout_validate.kai  layout_reject_of  : (fd: FieldDecl)  =>  (tab: SymTab, fd: FieldDecl) : LayoutReject    -- protos.kai:3079 (part B validate_derive_layout_*): pass `ps_tab(ps)`
layout_validate.kai  layout_head_name  : (te: TypeExpr)  =>  (tab: SymTab, te: TypeExpr) : String             -- protos.kai:3084
json_shape.kai       json_field_reject : (ftype: TypeExpr)  =>  (tab: SymTab, ftype: TypeExpr) : Option[String]  -- protos.kai:3182 and the record-field twin
json_shape.kai       json_head_name    : (ftype: TypeExpr)  =>  (tab: SymTab, ftype: TypeExpr) : String          -- protos.kai:3186
      -- WHY `tab`: the message names the field's type, and a head a scope pass settled (`m.T`, a contested type) is `DrSym` with no text.
json_shape.kai       JsonShape  : … | JSRecord(String)  =>  … | JSRecord(DeclRef)   -- the nested record as its field type refers to it
json_shape.kai       jd_shape_of, jd_fields_of, jd_payload_shapes, jd_shape_of_all, jd_variant_lams, json_head_is_option, jd_slot_col, jd_enc_lams, jd_dec_lams : signatures kept
layout_synth.kai     layout_fields_of, layout_order_of, layout_width_of(name: String), lf_* : signatures kept (a width annotation `U32` is a head written bare)
derive_pick.kai, derive_reach.kai : untouched, called as at the base (protocol spellings and head texts, as protos-A wrote).
```
What the generated code names, and how (for the resolver / typer agents): the derived type `TyName(DrSym(t), [])` / `ERecordLit(DrSym(t), …)`; a sum's constructors
`PVariant(DrSym(ctor), subs)` and, to build one, `ECall(ESym(ctor, spec_none()), args)` / a bare `ESym(ctor, spec_none())` for a nullary one; the JSON readers
`ESym(sym_derive(NCFn, name, "encoding__json_bind"), spec_none())`. Everything else is text as before: `mk_name("Ok" | "Err" | "JObj" | "JStr" | "JNull" | …)`,
`PVariant(ref_written("Ok" | "Err" | "Some" | "None" | "JNull"), …)`, `ERecordLit(ref_written("Pair" | "BinCursor"), …)`, `TyName(ref_written("String" | "Result" | …), …)`,
`binder_written(…)` params / pattern binders, and the `__pimpl_<Proto>_<T>_<op>` impl functions a body calls.
CORRECTION to the tools-B section above: kind_resolve.kai `resolve_kinds_decls(decls, reg, sites, target, view)` keeps its signature (only `build_kind_ctx` takes `tab`).
(agent tools-B)

## infer part D API (patterns, match, exhaustiveness, per-declaration entry, `TypedProgram`, `infer_program*`, candidate synthesis) — owner: agent infer-D
PRELIMINARY, posted before the code so parts B / C, typer-sat, mono, driver and checks can write against it (lines are only added below).

Patterns — THE TYPER STAMPS THE CONSTRUCTOR. A pattern the typer has checked holds `PVariant(DrSym(constructor id), subs)` and
`PVariantRecord(DrSym(record type id), fields)` wherever the reference could be settled (an unknown variant stays written: it was reported).
```
type PatChecked = { st: InferState, pat: Pattern }                      -- NEW (part D)
fn check_pattern_stamped(st, pat: Pattern, expected: Ty) : PatChecked    -- NEW: binds the pattern's binders (`env_bind_local(env, b, …)`) AND returns the pattern stamped
fn check_pattern(st, pat, expected) : InferState                         -- KEPT (= check_pattern_stamped(…).st) so the calls of part C still type-check;
      -- part C `synth_stmt` SLet must store the stamped pattern: `let pc = check_pattern_stamped(dup_st, pat, binding_ty)` … `SLet(pc.pat, ann, …)` with `st: pc.st`
      -- (`refine_checked`'s `PBind` needs no stamp). `synth_arms` / `check_arms` (part D) already return stamped arms.
fn check_pattern_no_dup_binds(st, pat) : InferState                      -- unchanged; duplicates are binders SPELLED alike (the diagnostic prints the name)
fn pattern_collect_binds(pat, acc: [Binder]) : [Binder]                  -- was [String]
fn check_pfield_visible(st, ri, fnm, line, col), synth_match, check_match, check_expr_against, check_branch_for_ctor, check_body_against_ret,
   synth_assert_cond, find_matching_declared, label_unpinned_among, st_add_tail_labels, row_root, is_sig_row, finalise_typed_expr,
   is_fixed_vec_lit, fixed_vec_unbrand, ty_vec_headed                    -- signatures unchanged
fn report_ctor_ambiguity_if_any(st, cname: Written, line, col) : InferState   -- was (st, cname: String, …): the bare constructor name the resolver left written (`EName(w)`); part C `synth_stmt` passes `w`
                                                                              -- (an `ESym` callee is settled: no ambiguity to report, drop that arm)
```
Constructors of a sum (part D; the env side is infer-A's `env_ctors` / `env_sym`):
```
type CtorPick = CtorPick(SymId, TyEntry)                                 -- unchanged: the constructor's id, its entry
fn ctor_pick(st, t: Ty, cname: String) : Option[CtorPick]                -- unchanged: the constructor of `t`'s sum that a WRITTEN name means
fn sum_ctor(env, tsym: SymId, cname: String) : Option[CtorPick]          -- unchanged signature; a constructor of `tsym` whose declared name is `cname`
fn sum_ctor_scheme(env, tsym, cname) : Option[TyScheme]                  -- unchanged
fn scheme_ret_sym(sc) : SymId, scheme_builds_nominal(sc), sym_is_core_sum(s), core_sum_name(s)   -- unchanged
fn variants_of_type(env: TyEnv, tsym: SymId) : [String]                  -- unchanged signature: the NAMES a diagnostic lists (part B `emit_union_focus_notes`)
fn check_variants_nullary(env, tsym) : VariantsCheck                     -- VariantsCheck = VCOk([SymId]) | VCNotSum | VCHasPayload([SymId])  (constructor ids, declaration order; as part B reads it)
REMOVED: ty_vix_add, ty_ctor_sym, ty_ctor_name, ty_env_qualified_parents, ctor_parent_name, ctor_site_kind, ty_env_lookup_ctor_in_home, ty_env_lookup_ctor_sym (pub; = env_sym), sum_ctor_names
```
Part D calls these of part B as they stand in B's worktree (say so here if they are renamed): `name_read_sym(st, nm: String) : Option[SymId]` (what a bare
written name the resolver left reads as: the pattern fallback when the scrutinee's type declares no such constructor), `synth_sym_read(st, s, e)`.

Types other files read:
```
pub type ResolvedCS       = RCS(SymId, Int, Int, [Ty], [UnitExprT], Option[String])     -- was RCS(String name, …): the instantiated DECLARATION's id (part B `CS(SymId, …)`).
      -- monomorph.kai, inst_index.kai (`iix_chain(ix, name)` => key by `sym_raw`: `idx_prepend_sym` / `idx_chain_sym`), emit_c.kai dump_mono_* (print `decl_name(tab, s)`), cache_module_test.kai
pub type HoleOwner        = HOwner(SymId, [LocalId]) | HNoOwner                         -- was HOwner(String, [String]): the enclosing fn's id, its parameters' binder ids. cache_module_test.kai
pub type ResolvedRowHole  = RRH(Option[String], Int, Int, SymId, Row, [SymId])          -- was (…, fn name: String, Row, body labels: [String]): the enclosing fn's id; the effects its body performs, by id
      -- infer_holes_json.kai json_row_hole_report prints `decl_name(tab, _)` for both
pub type ResolvedHole     = RH(HoleKind, Int, Int, Ty, [TyEntry], Int, HoleOwner, HoleRow)   -- unchanged shape (TyEntry is infer-A's `{ key, scheme }`)
pub type TypedProgram, TypecheckedModule, ModuleEnvDelta, ModuleDecls, TcShared, HoleRow, EffCand, HoleCands   -- unchanged shape (`ModuleEnvDelta.name` / `ModuleDecls.name` are module origins)
type TypedDecl.op_res: part D stamps `OPR(<enclosing fn>, …)`: the DFn's `FnId`; a `const`'s `fnid_decl(const id)`; a test / bench / check body the ENTRY's `fnid_decl(entry_id(target))` (it was the text "main")
```
Entry points (the table is needed to turn a written name into an id and to print one; `tab` right after the declarations):
```
pub fn infer_program(file, decls, verbose)  =>  infer_program(file: String, decls: [Decl], tab: SymTab, verbose: Bool) : TypedProgram
      -- callers: driver.kai, emit_c.kai (2), evidence.kai, evidence_inject.kai, infer_dump.kai (5), infer_holes_json.kai, rawsafe.kai, …: pass the table of the program's rows (`st_program(rows)`)
pub fn infer_program_with_protos_cached(file, df, decls, verbose, protos, head_owners, kind_theories, tab, views), typecheck_module(…, tab, sh, verbose),
       tc_shared(file, df, all_program_decls, tab, views), tc_shared_extend(sh, delta), partition_decls_by_home(decls, target_mod), flatten_module_decls,
       typed_prog_with_decls, merge_env_deltas, module_env_delta_empty, the prepend_reverse_* family, drop_last_entries, hole_row_missing   -- signatures unchanged
pub fn synth_candidates(scope, expected, fresh, owner, hr, decls)  =>  synth_candidates(tab: SymTab, scope: [TyEntry], expected: Ty, fresh: Int, owner: HoleOwner, hr: HoleRow, decls: [Decl]) : HoleCands
      -- a candidate is printed from its entry's key (`env_key_text(tab, k)`); infer_dump.kai:394, infer_holes_json.kai:60
pub fn label_to_string(tab: SymTab, l: Label) : String, label_display(tab: SymTab, owners: [HeadOwnerEntry], l: Label) : String   -- as infer-A posted (emit_shared.kai:3359, monomorph.kai:407 pass their table)
```
Needed from typer-sat (part D is written against these):
```
exhaust.kai        XSib : XSib(String, String)  =>  XSib(SymId, String)       -- a constructor's id, and how a witness spells it with wildcard payload (the spelling is diagnostic text)
exhaust.kai        XCtors : XCtors((String) -> Option[[XSib]])  =>  XCtors((SymId) -> Option[[XSib]])   -- asked with the constructor id a stamped arm holds
exhaust_head.kai   XKVariant(String)  =>  XKVariant(SymId): `PVariant(DrSym(c), subs)` -> `XCon(XKVariant(c), XPos(subs))`, `PVariantRecord(DrSym(c), fs)` -> `XCon(XKVariant(c), XNamed(fs))`;
                   a reference still written (an unknown variant, already reported) -> `XOdd`. A witness spells a variant key from its `XSib` spelling.
                   `x_needs_matrix(arms)`, `x_match_verdict(arms, tab) : XVerdict`, `XMissing([String])` unchanged.
union_table.kai    ut_component_named(t, parent, name: String) : SymId  =>  no longer called by part D (a union's components are read with `ut_components` and compared by `ut_ty_sym`)
tycon_pat.kai      nothing left to stamp for a pattern the typer checked (arms and `let` patterns); see the note above
field_privacy.kai  field_hidden(v, rec_name: String, rec: SymId, fd), priv_read_diag(file, line, col, fnm, rec_name: String, mo) : called UNCHANGED from `check_pfield_visible`, the record's name read from `decl_name(tab, rec)` (as part A does)
vec_surface.kai / fixed_vec_match.kai (surface)  vec_arm_kind(p), VArmCatch(_) / VArmUnsupported, vec_match_build, fixed_vec_match_arm, fixed_vec_arm_build, vec_lit_build, fixed_vec_lit_build : called as at the base
```
(agent infer-D)

## ownership API, part 3 (agent ownership) — unbox.kai, unbox_native_raw.kai; one correction to part 2
```
unbox.kai  unbox_pass : (decls, cis, nat, entry)  =>  (decls: [Decl], tab: SymTab, specs: SpecNames, cis: [CtorIntSlots], nat: Bool, entry: SymId)   -- driver.kai:5849 (register_fn_decls needs both)
unbox.kai  expr_has_interp_use : (e, nm: String)  =>  (e: Expr, b: Binder) : Bool / Console;  uses_have_in_lam : (uses, nm: String)  =>  (uses: [Use], l: LocalId) : Bool   -- as emit-sat wrote
unbox.kai  native_raw_arm_binders(decls, cis, fns) : [RawArmBinder] unchanged;  raw_arm_binder_at : (tbl, nm: String, line, col)  =>  (tbl: [RawArmBinder], l: LocalId, line: Int, col: Int)   -- kir_lower_bind.kai:102: the PBind's `binder.id`
unbox_native_raw.kai  native_arm_binder_raw_safe : (nm: String, e, cis, fns)  =>  (l: LocalId, e: Expr, cis: [CtorIntSlots], fns: FnIndex)
reuse_ctors.kai (CORRECTION to part 2)  `ReuseCtorName` / `RCN` / `reuse_ctor_home_name` STAY as TEMPORARY shims so emit_c.kai still resolves: `reuse_ctor_home_name` answers `ReuseCtorGone`,
           which fits no match. Call `reuse_ctor_id(ctors, ctor) : Option[SymId]`; once emit_c.kai 4208 / 4632 / 4666 are rewritten the three are deleted.
```
NEED in ast.kai (lead) — two pass tables that still hold a name where an identity goes; unbox*.kai are written against the new shapes and do not type-check against the base ones:
```
ast.kai  CtorIntSlots : CIS(Option[String], String, [Bool])  =>  CIS(SymId, [Bool])   -- the CONSTRUCTOR's id and its kind-1 Int-slot flags. unbox reads it by `ref_sym` of a `PVariant` and by the `ESym(c, _)`
         of a constructor call; `ctor_int_slots_lookup`, `ctor_int_slots_lookup_in`, `ctor_home_matches` are no longer called by unbox*.kai (head_tag_decl.kai still calls ctor_home_matches).
         Builder: emit_c.kai build_ctor_int_slots (part A) writes `CIS(<constructor id>, flags)`: the id of each `Var(c, _)` of a sum, its layout from `ctor_ix_row(ix, c)`.
ast.kai  RawArmBinder : RAB(String, Int, Int)  =>  RAB(LocalId, Int, Int)   -- the binder and its PBind's position (kir already ignores the name slot)
```
What unbox*.kai call in other batches (as published by their owners):
```
fnreg.kai (mono)             register_fn_decls(decls, tab, specs, entry);  scan_uses_expr(ns: LocalNames, e, in_lam);  scan_uses_in_interp(ns, span, is_triple, in_lam)
                             -- unbox passes `ns` = the map holding the ONE binder it asks about (`ln_with_binder(ln_empty(), b)`): a spelled read in a span then counts for that binder alone
local_names.kai (front-parse) ln_with_binder(ns, b)
infer.kai (A)                Use = U(LocalId, Int, Int, Bool)
handler_alias.kai (front-resolve)  clause_lent_names(params, init_opt) : [LocalId]
fn_index.kai (mono)          fnx_build, fnx_with_effects, fnx_with_raw_prims, fnx_with_vec_raw, fnx_decl_sig(fx, fid), fnx_callee_sig(fx, callee), fnx_is_raw_prim(fx, s), fnx_is_effect(fx, s), fnx_empty, `fx.vraw` : base signatures
```
Standalone, the ownership branch halts the check on the two names other batches add (`fn_emit_name`, `ln_with_binder`); with them present and the signatures above, its files type-check.

## protos-sat (proto_ids, proto_key, proto_op_index, proto_orphan, proto_rank, proto_resolve, impl_identity, impl_index, pipe_fusion, closure_spec*, law_checks, guard_iface, fn_bounds) — agent protos-sat
Protocol registry: everything protos-B asked for under `## protocol registry` is implemented AS WRITTEN THERE (`DhSym | DhScalar`, `dh_ref_is`, `dh_same`, `dh_text`, `pir_serves`, `pir_sym : SymId`, `pir_ret_ty`, the stamp of target + every protocol arg + ret head, `impl_symbol_renames(impls, tab, root)`, `impl_renamed(rs, s)`, `rows_with_renames`, `decls_with_renames` / `reg_with_renames`, `ii_key_bucket(idx, proto, h: DispHead)`, the proto_ids / proto_rank signatures). `dh_name`, `dh_names`, `pir_ret_head` are REMOVED: until protos.kai (and monomorph.kai 1065) stop naming them the checker stops in the resolver ("cannot find" / "`dh_name` is private to deep_measure") before it types anything. Details and the rest:
```
impl_identity.kai  ii_key_bucket  : DEFINED HERE, not in impl_index.kai (impl_identity imports impl_index, and the bucket takes a DispHead) -- protos.kai imports both, the call is unchanged
impl_index.kai     ii_bucket(idx, proto: SymId, head: Int), ii_ref_key(r: DeclRef) : Int, ii_scalar_key(t: Ty) : Int  -- NEW, the Int-keyed primitives under `ii_key_bucket` / `build_impl_index`; nobody else needs them
impl_identity.kai  ImplRename  : ImplRename(String, String)  =>  ImplRename(SymId, NEnt)   -- the root impl function as minted, and the ROW of the function it becomes (its id is derived from the row)
impl_identity.kai  impl_symbol_renames(impls, tab, root)  -- `tab` must hold the rows of ALL lowered impl functions (root and non-root; part B's `st_of_entries(<their rows>)` is enough): a root function is renamed when `st_candidates(tab, NCFn, <its name>)` holds the function of a non-root entry. The root module goes into the symbol's own text right before `_<op>`, so neither the dispatch key nor the target's name is read
impl_identity.kai  decls_with_renames(ds, rs)  -- also respells a still-written `EName` of a renamed root function (json_derive / layout_derive call an impl function by its mangled name before the resolver)
impl_identity.kai  reg_with_target_ids  -- calls tycon_scheme_home.kai `tsh_sym(v: TyHomeView, nm: String) : SymId` with the written name of each `DrWritten` (typer-sat: please keep that entry point, or tell me the new one)
proto_ids.kai      pid_qualified_disp / pid_qualified_is_disp  : (ids, m: String, name: String)  =>  (ids, m: Written, name: Written)   -- the payload of an unsettled `EModCall(m, n)`. A caller holding strings / an id asks `pid_is_disp(ids, sym_derive(NCFn, name, m))` (infer.kai 10151)
proto_ids.kai      pid_named_disp  : (ids, name: String)  =>  (ids, name: Written)   -- the payload of an `EName` the resolver left to the type
proto_ids.kai      pid_disp_ref / pid_op_ref : Some(ESym(s, spec_none())); pid_disp_homed_ref : Some(EModCall(written(home), written(disp_name))) for a homed dispatcher (derive_mint.kai, pre-resolve), ESym for a root one
proto_op_index.kai OpEntry  : OpEntry(String, String)  =>  OpEntry(String, SymId);  op_protocols_fold : (ix, op: String, acc, f: (a, String) -> a)  =>  (ix, op: String, acc, f: (a, SymId) -> a)   -- the fold hands the PROTOCOL'S ID (core_reach.kai cr_op / cr_proto: `protos: [SymId]`)
proto_resolve.kai  ps_type_owner  : (p, name: String, from) : Option[String]  =>  (p: PScope, r: DeclRef, from: Option[String]) : SymId   -- the owning MODULE's id (`sym_none()` for a builtin); only proto_orphan.kai calls it
proto_orphan.kai   OrphanScope  : OrphanScope(PScope, [TypeSpelling])  =>  OrphanScope(PScope)   -- protos.kai 522: `OrphanScope(ps)`; `OrphanImpl` unchanged. It calls `type_text(tab, t)` (the form tools-A / protos-A / driver posted)
fn_bounds.kai      collect_surface_bounds  : (ds: [Decl])  =>  (ds: [Decl], tab: SymTab) : [FnBoundsRec]   -- driver.kai 4961-4962: `tab` holds the rows of the stream (the name FBR carries is read from it)
fn_bounds.kai      bounds_by_sym(ds, recs, target_mod)  -- signature KEPT, NO `tab` (driver.kai 5809 / 5816 assumed `bounds_by_sym(ds, tab, recs, target_mod)`: drop the `tab`). A record is matched to its fn by the id `sym_derive(NCFn, name, origin or target_mod)`, never by comparing names, so it reads no table
law_checks.kai     gen_law_checks  : (decls, reg, file)  =>  (decls: [Decl], ps: PScope, reg: LawReg, file: String)   -- NOT `(decls, tab, reg, file)` as the driver assumed: `law_theory_of` is keyed by the protocol's id, and an impl's written protocol only becomes an id through the scope. driver.kai 5278: pass the scope `lower_protocols` gets (`PScope(early_tab, sym_views, target_mod_name)`); an impl's protocol is resolved from the impl's own module, the names are read from `ps_tab(ps)`. Written against protos-A's posted `law_theory_of(reg, proto: SymId)`, `law_impl_axiomed(reg, home, line, col)` (the DImpl's own line / col) and `proto_type_ref(te) : DeclRef`; it no longer calls `proto_type_name`
proto_resolve.kai  NEW  ps_ref_proto(p: PScope, r: DeclRef, from: Option[String]) : SymId   -- the protocol a written or settled head denotes from `from` (what protos-A's private `proto_ref_sym` computes; usable instead of it)
law_checks.kai / guard_iface.kai  call `parse_program(file, src, toks, file)` (front-parse's 4-argument form; both only read shapes / declare nothing)
closure_spec.kai   closure_spec_pass  : (ds, target, tab, ids) : [Decl]  =>  (ds: [Decl], target: String, tab: SymTab, ids: ProtoIds) : CSpecOut = { decls: [Decl], rows: [NEnt] }   -- mono_front.kai 46: every spec is a NEW root declaration `FnId(sym_derive(NCFn, <spec name>, target), spec_none())`; extend the table with `rows` before monomorphise / the emitters read a spec's name
closure_spec_ast.kai  cs_pat_binders  : (p, acc: [String]) : [String]  =>  (p: Pattern, acc: [LocalId]) : [LocalId]   -- spawn_capture.kai 231, 250
closure_spec_ast.kai  REMOVED cs_decl_name, cs_param_names, param_name_at; NEW cs_decl_text(tab, d) : String, cs_decl_fid(d) : FnId, cs_param_ids(ps) : [LocalId], param_id_at(ps, n) : LocalId, cs_has(xs: [LocalId], l) : Bool; index_of over [LocalId]; closure_disc(tab, host: SymId, ca); retarget_callee(callee, s: SymId); cspec_ref(s: SymId)
closure_spec.kai / closure_spec_emit.kai  written against mono's posted `SelfRef(FnId)` + `is_self_callee(callee, me)` (they build `SelfRef(fid)`); they no longer import sym_read.kai
closure_spec_emit.kai  WHAT A SPEC'S BODY HOLDS: every pasted copy of the closure (and each `__csb_*` let) has binders of its own — `LocalId`s minted from (spec name, paste position, the binder's old id) — and keeps the NAME the source wrote. So one function can hold two binders of one name and different ids: an emitter that spells a local must not let one capture the other
pipe_fusion.kai    `__fuse_*` / `__fusef_*` / `__fusev_*` / `__fuse_acc_*` / `__fuse_x_*` are `binder_minted(key)` read as `ELocal(local_minted(key))`; a fused call's callee is still the unsettled `EModCall(written(core_fn_home(cf)), written(core_fn_name(cf)))` the typer settles; signatures unchanged
```
(agent protos-sat)

### tools-B CORRECTION — refinements.kai (REPLACES the `PredVar` / `subst_self` / `subst_var` / `classify_arg_vs_pred` lines of the tools-B section; it is now what front-resolve wrote its calls against)
```
refinements.kai  subst_self  : (e: Expr, name: String) : Expr  =>  (e: Expr, l: LocalId) : Expr          -- `self` becomes `ELocal(l)` (call_site_check.kai, refine_coerce.kai)
refinements.kai  classify_arg_vs_pred  : (req_pred, pname: String, arg, caller_refs)  =>  (req_pred: Expr, p: LocalId, arg: Expr, caller_refs: [RefBinding]) : CallSiteStatus
refinements.kai  NEW  classify_self_vs_pred(req_pred: Expr, arg: Expr, caller_refs: [RefBinding]) : CallSiteStatus   -- the same for a predicate still over `self` (refine_coerce.kai)
refinements.kai  NEW  subst_self_with(e: Expr, to: Expr) : Expr                                           -- `self` becomes the expression `to` (refine_coerce.kai rc_assert)
refinements.kai  subst_var  =>  PRIVATE now (its one outside caller, refine_coerce.kai, calls subst_self_with);  `PredVar` / `pv_self` are private too
```
Unchanged from the section: `RBind(LocalId, Expr)`, `subst_params` / `arg_for_param` / `classify_requires_clause` over `[LocalId]`, `CSLoose(LooseWhy)` + `loose_text(w, req, own)`,
`CtrId(LocalId, Expr, Expr)`, `wrap_with_contracts(…, fn_name: Written, …)`, `RcRefuted(LooseWhy)`, `rc_assert(b: Binder, …)`, `rc_check_var_writes(name: Written, …)`.
(agent tools-B)

### perceus API — as committed (agent perceus; corrects / completes the `## perceus API` section above)
```
ret_slots.kai (nobody's batch)  the additions are: ret_slot_binder(s: RetSlot) : Binder;  ret_slot_local(s: RetSlot) : LocalId;
                  ret_slot_id_is_fn(l: LocalId) : Bool;  ret_slot_id_is_scope(l: LocalId) : Bool;
                  ret_slot_binds_any(b: Binder) : Bool;  ret_slot_binds_tail_arg(b: Binder) : Bool
                  -- there is NO ret_slot_id_is_any: a hoisted tail-call argument is one slot per position (`__pcs_ta<i>`), so "any slot" is asked of the
                  -- BINDER (a minted binder's name is its mint key). A reader that holds only a read `ELocal(l)` compares with ret_slot_local(<slot>).
perceus_reads.kai   NEW  pcs_handle_aliases(init_opt: Option[Expr]) : [Binder]      -- a stateful handle's state aliases: binder_minted(k) for k in user_state_aliases() ++ [var_cell_alias()]
                    NEW  pcs_interp_spelled_count(w: Written, span: String, is_triple: Bool) : Int;  pcs_span_token(b: Binder) : String;  pcs_span_spells(b: Binder, span: String) : Bool
perceus_span.kai    NEW FILE  pcs_span_expr(e: Expr, scope: [Binder]) : Expr
                    -- a body parsed with parse_interp_expr names everything by spelling (EName): this returns it with each name that spells a binder of `scope`
                    -- (nearest first) read as ELocal(that binder's id), and each binder the body itself declares numbered local_minted(<its name>). Other names stay EName.
perceus_body_reads.kai  SlotRead : SR(String, Bool) => SR(LocalId, Bool) | SRSpan(String, Bool);  RoleRead : RoleR(String, ReadRole) => RoleR(LocalId, ReadRole) | RoleSpan(String)
                    pbr_only_lent(ix, nm: String) => (ix, b: Binder);  pbr_roles_of(ix, nm: String) => (ix, b: Binder)
perceus_move_sites.kai  ArgSlot : AS(String, …) => AS(LocalId, …);  pms_moves_at(ms, nm: String, ll, lc) => (ms, b: LocalId, ll, lc)
perceus_seg_request.kai pcs_clause_requested(params, body) : [Binder];  pcs_seg_request_moves(names: [Binder], body) : [Binder]
perceus_cap.kai     pcap_param_names(c, mo, params) : [Binder];  CapScope.effects / .types : [String] => [SymId]
perceus_rethread.kai / perceus_lend_tail.kai / perceus_owned_pos.kai / perceus_raw_slot.kai / perceus_tail_drop.kai / perceus_path_move.kai / perceus_lend_order.kai /
perceus_branch_pay.kai / perceus_branch_plant.kai   every `[String]` binder set => [Binder], every walked `nm: String` => Binder (only perceus.kai calls them)
perceus.kai         pcs_collect_arm_skip_binders(self_name, tl, e, bmap, cmap, borrowed: [Binder], tab: SymTab) : [Binder];
                    pcs_branch_aware_skip_locals_b(binders: [Binder], body, bmap, cmap, borrowed: [Binder], tab: SymTab) : [Binder]   -- `tab` decides "the callee is a constructor"
```
What perceus assumes of the tree it reads (say so here if your pass produces something else):
- a local read is `ELocal(l)`; a declaration (fn, constructor, const, builtin) is `ESym(s, inst)`; a perform's callee is `EOp(r)`. `EName` / `EModCall` do not reach it
  (an `EName` only inside a body it parses itself out of an `EStr`).
- a constructor is an `ESym` whose id `ctor_ix.kai sym_is_ctor(tab, s)` accepts: EVERY constructor a call site can carry needs a row of class `NCCtor` in the driver's table,
  the runtime's `Some` / `None` / `Ok` / `Err` included. An id with no row is no constructor to Perceus (no reuse, no move into its slots).
- a handler state alias read is `ELocal(local_minted(key))` (as the kir section says); a user local spelled `state` / `log` carries its own id.
- a capability parameter's annotation is `TyName(DrSym(effect id), _)`; a `DrWritten` one is climbed through the file's scope view as before.
- a return type naming an `extern "C" opaque` type is `TyName(DrSym(type id), _)` (perceus compares it with the `DType` id it was lowered to).
- binder names stay unique within a function body (`unique_binders.kai`), and it keeps respelling the reads inside `#{...}` spans: Perceus matches a span's identifier
  tokens against a binder's written name.
### front-resolve — FINAL (as committed in the front-resolve branch; where a line here differs from the front-resolve lines above, this one stands)
```
handler_alias.kai  live_state_aliases(stateful: Bool, local_scope: [String]) : [String], handle_live_aliases(init_opt, local_scope: [String]) : [String]   -- BASE signatures after all (kir, unique_binders call them so):
      -- `local_scope` is the names the caller has SPELLED (C locals, KIR registers, span text); results are mint keys. A pass that tracks scope by id calls
      -- `clause_state_aliases((key) => <its scope binds local_minted(key)>)`. The rest of handler_alias.kai is as the CORRECTION block says
      -- (`clause_lent_names` / `clause_bound_names` : [LocalId]; `clause_continuation` : Option[Binder]; `ClauseCont` over `Binder`; `clause_cont_reaches(cc, l: LocalId)`; `expr_reads_local`).
homonym.kai  HomCand = HCand(SymId, Ty);  HomPick = HPick(Int, Int, SymId);  hom_choose(cs, arg) : Option[SymId];  DeferredHom = DHom(String, Ty, Int, Int) unchanged   -- as infer-B posted
homonym.kai  hom_head  : (t: Ty) : Option[String]  =>  (t: Ty) : Option[HomHead];  NEW  HomHead = HhBuiltin(String) | HhDecl(SymId), hom_head_same(a, b) : Bool;  hom_heads(cs) : [HomHead]
      -- a declared type's head is its id (two modules' `Tok` are two heads); `hom_decidable`, `hom_first_param` unchanged; hom_retarget writes the picked callee `ESym(s, spec_none())`.
resolve.kai  collect_effect_ids(decls: [Decl]) : [SymId] is the function; `collect_effect_names` is KEPT as a TEMPORARY alias of it returning `[SymId]`
      -- (removing a name another file still writes stops the whole compiler before the type checker); rewrite each call and delete the alias.
resolve.kai  a qualified `EModCall(m, "__proto_…")` is LEFT AS WRITTEN, exactly as before (`rs_qualified_fn` answers None for a dispatcher name): the interpolation sugar's
      -- homed `show` reaches `try_rewrite_proto_call` still an `EModCall` (protos-B's OPEN point). Only a bare dispatcher one home declares becomes `ESym(dispatcher id)`, as before.
scope_walk.kai  SwHead  : KEPT as a TEMPORARY type scope_walk itself no longer uses (effect_scope.kai:104 at the base still writes its constructor); delete with that use
scope_file.kai  sel_with_ctors  : (sels, scopes, global, decls, root)  =>  (sels, scopes, global, decls: [Decl], tab: SymTab, root: String)   -- as the driver assumed: a pick is a written name, so the sum and its constructors are named from the table
site_ids_check.kai  check_site_ids  : (decls)  =>  (decls: [Decl], tab: SymTab)   -- a finding names its declaration from the table. DRIVER: driver.kai:5926 passes the table of the decls it checks (it listed this call as base)
name_uses.kai  NDisp(SymId, String, SymId) built by nu_dispatchers(reg) from `POR(proto, op, …, home)`: the dispatcher's id is `pid_fn_id("__proto_" ++ op, home, reg.root)` (proto_ids.kai, base signature)
call_site_check.kai  calls refinements.kai as tools-B posted it: `subst_self(pred, ELocal(p.binder.id))`, `classify_arg_vs_pred(pred, PvLocal(id), arg, refs)`, `classify_requires_clause(pred, ps: [LocalId], args, refs)`,
      -- `subst_params(a, binders: [LocalId], values)`, `arg_for_param(ps, args, id)`, `CSLoose(why: LooseWhy)` printed with `loose_text(why, <callee's names>, <caller's names>)`; `fmt_expr_text(e, tab, locals)` (tools-A).
scope_chain_test.kai (nobody's batch)  : updated to `Bound` (`BDecl` in `ch_bind`, `Option[Bound]` from `ch_lookup`)
```
The front-resolve files type-check with 0 errors against stubs of exactly the shapes their owners posted above: `EO(SymId, [String])` (front-load-B) · `nt_collect(decls, tab, target)` / `nt_protocols(decls, tab, target)`,
`st_program(rows)` (front-parse) · `pcs_interp_ident_count(b: Binder, span, is_triple)` (perceus) · `fmt_expr_text(e, tab, locals)` (tools-A) · the refinements.kai lines above (tools-B).

### perceus API — entry points, after the mono / checks / kir lines above (agent perceus)
```
perceus.kai  perceus_pass_in  : (decls, tab, views, root)  =>  (decls: [Decl], tab: SymTab, specs: SpecNames, views: FileViews, root: String) : [Decl] / Console
      -- driver.kai 5887: ONE more argument, `MonoFront.specs`, after the table. Perceus builds its own registry with mono's `register_fn_decls(decls, tab, specs, entry)`.
perceus.kai  pcs_build_borrow_map  : (decls, seed)  =>  (decls: [Decl], seed: [BorrowEntry], tab: SymTab) : BorrowMap     -- only perceus.kai calls it
perceus.kai  pcs_borrow_params_hybrid  : (is_pub, me, params, pnames, body)  =>  (is_pub, me: SelfRef, params, pnames: [Binder], body: Expr, tab: SymTab) : [Binder]
      -- it calls checks' `borrow_infer_params(tab, me, pnames, body)`
perceus.kai  pcs_planned_borrow_map(decls: [Decl]) : BorrowMap   -- UNCHANGED (emit_c.kai 13165): the planned map reads the stamped `pborrow`, no table
```
To kir, on reuse_uses.kai (line "perceus_read_walk.kai / perceus_reads.kai (perceus)" above) — the index is NOT callable with `[LocalId]`:
```
perceus_read_walk.kai  pcs_use_index(body: Expr, scope: [Binder]) : UseIx / Console
      -- the scope is BINDERS: the walk reaches `#{...}` spans, and a span names a binder by its spelling (perceus_span.kai). A read is `Rd(l: LocalId, line, col, in_lam, raw)`.
name_ix.kai            the index is bucketed by `local_raw`: `ni_bucket_id(ix, local_raw(l))` (NOT `ni_bucket(ix, <String>)`); compare the bucket's reads with `local_eq`
reuse_uses.kai         so: `reuse_gc_uses(cargs, gc: [Binder])`, `ru_exprs` passes `gc` on, `ru_names` reads `ni_bucket_id(ix, local_raw(g.id))`, `ru_reads_of` keeps `local_eq(l, g.id)`.
      -- what it RETURNS ([LocalId] or [Binder]) is its callers' choice; the donor binders are a pattern's `PBind(b)`, so the `Binder` is at hand in both emitters.
```

## typer-sat API (type_text, type_home / tycon_*, type_stamp, exhaust*, effect_instance*, union_table, hole_scope, infer_dump / *_json / *_collisions, kai_builtin*, ffi_v2, typed_cut, value_scope_table) — owner: agent typer-sat
PRELIMINARY, posted before the code so callers can converge (lines are only added below). Printers and key builders take the table FIRST; a pass entry takes it right after the declarations.
```
type_text.kai  type_text  : (t: TypeExpr) : String  =>  (tab: SymTab, t: TypeExpr) : String          -- as tools-A / protos-A already call it
type_text.kai  type_key / row_text / row_label_text  : (x)  =>  (tab: SymTab, x)                      -- emit_c.kai row_arg_key; kai_builtin_check. `unit_text(u)`, `unit_key(u)` unchanged
type_text.kai  type_text_with  =>  REMOVED (its one caller, protos.kai part A, calls type_text(tab, t) now)
type_text.kai  NEW  decl_ref_text(tab: SymTab, r: DeclRef) : String   -- how a reference prints: decl_name(tab, s) once settled, `q.n` / `n` as written before
scalar_ty.kai  NEW  ref_spelled(r: DeclRef) : String                  -- the name a reference still spells (a scalar, a type parameter, an unsettled name); "" once it is DrSym
tycon_ref.kai  tsh_tycon_at  : (v, r, nm, args)  =>  (v: TyHomeView, r: DeclRef, args: [Ty]) : Ty      -- as infer-A calls it. `tsh_ref_sym(v, r, nm)`  =>  `tsh_ref_sym(v: TyHomeView, r: DeclRef) : SymId` (infer part D ≈15497)
tycon_scheme_home.kai  tsh_tycon(v, nm: String, args) : Ty = TyCon(tsh_sym(v, nm), args), tsh_sym(v, nm), tsh_view, tsh_at, tsh_target, tsh_none, tsh_homes(ds, tab, views, target),
      tsh_fn_settled(hs, nm, reader), tsh_fn_visible(hs, reader, home, nm), tsh_decl_sym(hs, mo, nm), tsh_decl_home(hs, s) : signatures KEPT (text in, id out)
tycon_scheme_home.kai  tsh_slot / tsh_home_of  =>  REMOVED (a TyCon has no module slot);  tsh_sym_slot  : (v, nm: String, s)  =>  (v: TyHomeView, s: SymId) : Option[String]   -- infer part B ≈10011 (BigInt), core_shadow_diag
tycon_scheme_home.kai  tsh_reader_owns  : (v, nm: String, s)  =>  (v: TyHomeView, s: SymId) : Bool     -- field_privacy.kai field_hidden (changed by typer-sat; `field_hidden(v, rec_name, rec, fd)` keeps its signature)
tycon_scheme_home.kai  tsh_fn_contests  : (ds, target)  =>  (ds: [Decl], tab: SymTab, target: String) : String
tycon_scheme_home.kai  TyHomeEntry : THE(String, Option[String], SymId, SymId) => THE(Option[String], SymId, SymId);  TyRes : TyRes(String, String, SymId) => TyRes(String reader, SymId);  TyHomes gains the table it was built with (a name is read from it)
core_shadow_diag.kai  core_shadow_notes  : (hs, expected, found)  =>  (tab: SymTab, hs: TyHomes, expected: Ty, found: Ty)   -- infer part B ≈8081: `core_shadow_notes(env.tab, env.ty_homes, …)`
type_home.kai  type_home_table  : (ds)  =>  (ds: [Decl], tab: SymTab) : TyHomeTab                      -- emit_shared.kai ≈484. `payload_slot_kind(hs, cur, te)` unchanged. TyHome = TH(SymId, Option[String], Bool); `type_home_of` REMOVED
type_home.kai  type_home_entry  : (hs, nm: String, cur)  =>  (hs: TyHomeTab, r: DeclRef, cur: Option[String]) : Option[TyHome]   -- a DrSym by id; a written name by the reader's rule (own module, else the sole declaration)
tycon_home.kai  tag_tycon_modules  : (ds)  =>  (ds: [Decl], tab: SymTab) : [Decl]                      -- mono_front.kai:41. WHAT IT DOES NOW: every `PVariant` / `PVariantRecord` a match arm or a `let` holds and the typer left
      -- written becomes `PVariant(DrSym(constructor id), …)`: the constructor of that name the module of the pattern's expected type (or its written qualifier) declares. Nothing else is rewritten: a TyCon has no
      -- home to stamp, and a constructor call is already `ESym` out of the typer (the `EName` / `EModCall(T, Ctor)` qualification is gone).
tycon_pat.kai  tp_pat(p, ty, hs: TyHomeTab, pays) / tpay_table(ds) : signatures kept;  TPay = TPV(SymId sum, SymId ctor, [String], [TypeExpr], Option[String]) | TPR(SymId, [String], [FieldDecl], Option[String])
type_stamp.kai  stamp_te / stamp_tes / stamp_with / stamp_opt / stamp_params / stamp_row_with / te_decl_ids : signatures kept. A written name whose resolved type is a `TyCon(s, _)` with a settled `s` becomes `TyName(DrSym(s), args)`
type_ids_check.kai  check_type_ids  : (decls)  =>  (decls: [Decl], tab: SymTab)      tycon_ids_dump.kai  dump_tycon_ids  : (decls, target)  =>  (decls: [Decl], tab: SymTab)     -- driver.kai 212, 213
exhaust.kai / exhaust_head.kai  -- the matrix is keyed by the constructor's ID (a tuple / record pattern: by the record type's id):
      XKey  : … | XKVariant(String)  =>  … | XKVariant(SymId)
      XSib  : XSib(String, String)   =>  XSib(SymId, String)      -- a constructor of the sum, and how a witness spells it with wildcard payload
      XCtors : XCtors((String) -> Option[[XSib]])  =>  { place: (DeclRef) -> SymId, sibs: (SymId) -> Option[[XSib]], name: (SymId) -> String }
      -- place: the constructor (or record type) a pattern's reference names — `DrSym(s)` is `s`; a written name is the scrutinee type's constructor of that name, else the one constructor so spelled,
      --        else `sym_none()` (the column then answers unknown, as an unplaced name did);  sibs: every constructor of the type `s` belongs to (a record type: itself alone), None when it cannot be placed;
      --        name: the bare name a witness spells `s` with (`decl_name(st.env.tab, s)`).  `x_match_verdict(arms, tab: XCtors)`, `x_needs_matrix(arms)` keep their signatures (infer part D check_exhaustive_matrix / x_ctor_entry).
union_table.kai  ut_component_named  : (t, parent, name)  =>  (tab: SymTab, t: UnionTable, parent: SymId, name: String) : SymId   -- infer part D ≈17425; the rest of union_table.kai keeps its signatures (UnionRow(Ty, [Ty], Int, Int) as part A builds it)
variant_index.kai  unchanged. `vix_ctor_key` / `CtorKey` are dead once part D stops calling them on `e.name` (a constructor's entry is `KCtor(c)`); they are left in place so the name still resolves
effect_instance.kai  row_label_keys  : (d, ls)  =>  (tab: SymTab, d: Decl, ls: [RowLabel]) : [String]   -- evidence_frame.kai:64; a label's effect name is `decl_name(tab, rl_sym(l))`
effect_instance.kai  inst_key_of_tes(eff: String, args), inst_key_of_callee(eff: String, callee_ty), key_of, te_text(te, tps), ty_text(t), ty_unpinned, prim_text, inst_key_eff, inst_keys_match, inst_key_index, decl_tparams : signatures KEPT
      -- `eff` is the effect's run-time name, spelled by the caller; a nominal type argument always keys as `#<id>` (a TyCon has nothing else)
effect_instance_canon.kai  canon_handle_instances(decls), handle_instance_tparams(d) : signatures kept
hole_scope.kai  hole_visible_scope  : (scope, toplevel_len, hidden)  =>  (tab: SymTab, scope: [TyEntry], toplevel_len: Int, hidden: [String]);  hole_local_scope : (scope, toplevel_len) => (tab, scope, toplevel_len)   -- an entry is judged by `env_key_text(tab, e.key)`
infer_dump.kai  dump_infer / dump_typed / dump_typed_core / dump_last_use / dump_intervals  : (file, decls)  =>  (file: String, decls: [Decl], tab: SymTab)   -- driver.kai 5575-5607: `rd.tab`
infer_dump.kai  dump_holes  : (file, tp, hidden, full)  =>  (file: String, tp: TypedProgram, tab: SymTab, hidden: [String], full: Bool)                       -- driver.kai 205, 207
infer_holes_json.kai  dump_holes_json(file, tp, tab, doc_items, hidden, full), dump_effects_json(df, tp, tab), dump_effect_holes_json(file, tp, tab), strict_holes_check(file, decls, tab)   -- `tab` right after `tp` / `decls` (driver.kai 206-210)
infer_effects_json.kai  effects_walk_decls  : (decls, df, acc)  =>  (decls: [Decl], tab: SymTab, df: DiagFiles, acc: [EffEntry])
      NEEDED from infer part D (the dumps are written against these): `infer_program(file: String, decls: [Decl], tab: SymTab, verbose: Bool) : TypedProgram` (the table the names are read from; at the base it typed against `st_empty()`),
      `synth_candidates(tab: SymTab, scope, expected, fresh, owner, hr, decls)`, `label_to_string(tab, l)`; `RRH(…, fn_name: String, row, body_labels: [String])` read as at the base
infer_name_collisions.kai  validate_fn_ / validate_effect_ / validate_const_ / validate_axiom_name_collisions_decls, validate_module_same_module_collisions  : (decls, file)  =>  (decls: [Decl], tab: SymTab, file: String)   -- driver.kai 1388, 2383, 5190-5206.
      -- Two declarations of one class and name in one module derive ONE id, so a redeclaration is an id met twice; `tab` (of a freshly parsed file: `st_of_entries(parsed.names)`) only spells the message.
infer_type_collisions.kai  validate_type_name_collisions_decls(root_decls, tab, file), validate_constructor_uniqueness_decls(decls, tab, df), validate_variant_capacity(decls, tab, df)   -- driver.kai 5181, 5296, 5297
infer_union_collisions.kai  validate_union_collisions_decls  : (ds, df)  =>  (ds: [Decl], tab: SymTab, df: DiagFiles)   -- driver.kai 5292
kai_builtin.kai  KaiBuiltin : KB(FnId, String, String)  =>  KB(FnId, String);  kai_builtin_name  : (kb)  =>  (tab: SymTab, kb: KaiBuiltin) : String   -- kir_lower.kai 457, 463
kai_builtin.kai  kai_unshadowed  : (names, ds)  =>  (names: [String], ds: [Decl], tab: SymTab) : [String]   -- resolve.kai ≈1172.  `kai_builtins(ds)`, `kai_builtin_find`, `kai_builtin_csym` unchanged
kai_builtin_check.kai  kai_builtin_decl_diags  : (ds, df, is_core)  =>  (ds: [Decl], tab: SymTab, df: DiagFiles, is_core);  kai_builtin_manifest : (ds) => (ds: [Decl], tab: SymTab)   -- driver.kai 5214, 5215, 6165
ffi_v2.kai  collect_extern_types  : (ds)  =>  (ds: [Decl], tab: SymTab)   -- emit_c.kai (4), kir_lower_fns.kai:56, extern_fn_types.kai:21. The registry stays keyed by the declared name: it is the C typedef it emits
ffi_v2.kai  ffi_kind_of_ty_reg(t, reg, tab), ffi_ret_kind_of_reg(opt, reg, tab) (as emit-sat calls them), extern_field_c_type(fd, reg, tab)   -- emit_ffi_c.kai:52
typed_cut.kai  tcut_ctx  : (cache_dir, all_program_decls, target, protos, head_owners, kind_theories, views)  =>  (cache_dir, all_program_decls, tab: SymTab, target, protos, head_owners, kind_theories, views)   -- called only inside typed_cut.kai
value_scope_table.kai  vs_collect  : (decls, target)  =>  (decls: [Decl], tab: SymTab, target: String) : [VSEntry]   -- calls symtab_build.kai nt_collect(decls, tab, target); the rest of the file is unchanged (written names: it is a scope table)
```

## front-load-A (modules, qual_head, value_scope, missing_export, extern_fn_dup, extern_fn_types, head_tag_decl, rec_stamp, home_tu, import_uses) — agent front-load-A
All of these run on parser output, before the resolver: qualifiers, binders in scope and module names are read as written text. A declaration's name comes from the table.
`modules.kai` is NOT the loader (the per-file `parse_program` calls are in driver.kai `resolve_module` / the core loader): it hands no parser rows up. Its one `parse_program` call is the `import ?hole` scan.
```
modules.kai  pat_binders  : (pat: Pattern, acc: [String]) : [String]  =>  (pat: Pattern, acc: [Binder]) : [Binder]   -- as surface / ownership / perceus / front-resolve / emit-sat / tools-B / emit_c-B / kir asked
modules.kai  NEW  pat_binder_names(pat: Pattern, acc: [String]) : [String]   -- the written names of a pattern's binders ahead of `acc` (the OLD pat_binders, same order): for a pass before the resolver that
      -- tracks scope by text. pub_access_decl.kai:254, 329 (front-load-B expected the old `[String]` shape): call `pat_binder_names(pat, locals)`.
modules.kai  fn_param_names(params: [Param]) : [String]   -- UNCHANGED (the written names); a caller after the resolver wants `map(ps, (p) => p.binder.id)` instead (emit_c 5670, 8354, 8378, 10542…, kir_lower_fns 205, 390)
modules.kai  qualtype_decls  : (decls, mt, df) : [Decl]  =>  (decls: [Decl], tab: SymTab, mt: [ModuleEntry], df: DiagFiles) : QualTyped / Console + File
modules.kai  qualtype_segmented  : (segs, decls, scopes, global, df) : [Decl]  =>  (segs: [ImportSeg], decls: [Decl], tab: SymTab, scopes: [ModScope], global: [ModuleEntry], df: DiagFiles) : QualTyped / Console + File
modules.kai  NEW  pub type QualTyped = { decls: [Decl], rows: [NEnt] }
      -- DRIVER (driver.kai 5089-5092; it assumed `[Decl]` back): take `.decls`, and `st_extend` the table with BOTH calls' `.rows` before anything reads a sum's constructor names
      -- (nu_const_coll, vs_collect, sel_with_ctors, the typer's collect_unions). The rows are the constructors the QUALIFIED UNION MEMBERS were re-pointed to: `type U = m.T | X` arrives as
      -- `Var(sym_derive(NCCtor, "m.T", home))` (front-parse); the pass validates `m.T` and re-points the variant to `sym_derive(NCCtor, "T", home)` — the id a bare member `T` has — with the row
      -- `NEnt(NCCtor, "T", home, exported)` (`"__qualtype_bad__"` in place of `T` when `m` is unknown or does not export `T`, as before). The dotted row stays in the table, unread.
      -- `tab` must hold the parser's rows of `decls` (`early_tab`). Nothing else is re-keyed or re-tagged by this batch: no pass here changes a declaration's `module_origin`.
modules.kai  qtchk_decls  : (ds, mt)  =>  (ds: [Decl], tab: SymTab, mt: [ModuleEntry]) : Int;  qtchk_segmented  : (segs, decls, scopes, global)  =>  (segs, decls: [Decl], tab: SymTab, scopes, global) : Int   -- as the driver assumed
modules.kai  rqc_collect_fx  : (decls)  =>  (decls: [Decl], tab: SymTab) : [RqcFx]   -- as the driver assumed. `RqcFx(String, [String])` stays the written effect name + op labels (what a written `E.op` is matched against)
modules.kai  rqc_decls / rqc_segmented / rqc_expr(e, locals: [String], mt, fx, file) / unstable_check_decls / pqchk_* / report_import_holes / decl_home_hint_reset / imports_home_anchors / impl_first_fn_home / scope_imports : signatures UNCHANGED
      -- `rqc_expr`'s `locals` stays the TEXT of the binders in scope (emit_c.kai 10822 passes the interpolation context's names).
modules.kai  WHAT THE PASSES LEAVE: a validated qualified type / record-literal head is `DrWritten(Some(<the module's tagged name>), T)` (it was `ty_ref_qualified(home)` + the bare name), an invalid one
      -- `ref_written("__qualtype_bad__")` with no arguments; a qualified variant pattern `PVariant(DrWritten(Some(<tagged name>), Ctor), …)`; `mod.f` -> `EModCall(written(<tagged name>), written(f))`;
      -- `bit._and` -> `EName(written("bit_and"))`; `m.E.op` the effect pass did not settle -> `EField(EName(E), op)`. A `DrSym` reference and an `ELocal` / `ESym` / `EOp` node pass through untouched.
modules.kai  imports compiler.written, compiler.decl_names, compiler.symtab, compiler.symtab_build  -- legal only once decl_names.kai stops importing compiler.modules (front-parse's commit): at the base it is the cycle
      -- decl_names -> modules -> symtab -> decl_names, which stops the checker before any type error. Calls front-parse's `parse_program(mod_path, src, toks, module_name_of_path(path))` + `.names`.
qual_head.kai  qh_decls / qh_segmented / qh_expr / qhchk_decls / qhchk_segmented : signatures UNCHANGED. A handler head / row label `DrWritten(Some(m), E)` loses its qualifier (`DrWritten(None, E)`, valid or not, as before);
      -- an `impl m.P` head keeps a valid one re-spelled with the tagged module name (`DrWritten(Some(<tagged>), P)`), an invalid one drops it; `DrSym` heads are left alone. `DDerive`'s names are still dotted strings.
value_scope.kai  vs_segmented : UNCHANGED. A contested free read `EName(w)` -> `EModCall(written(home), w)`. Calls scope_walk as front-resolve posted: `sw_expr(e, sh, lf, tyf, (r) => r)` (also sw_hclauses, sw_hreturn_opt).
missing_export.kai  mexp_decls / mexp_segmented : UNCHANGED
extern_fn_dup.kai  validate_extern_fn_homes  : (decls, df)  =>  (decls: [Decl], tab: SymTab, df: DiagFiles) : Int / Console + File
      -- DRIVER (driver.kai 5307; listed as a base-signature call): pass the table. An extern fn with no symbol override binds the C symbol its NAME spells, and the name is the table's.
extern_fn_types.kai  validate_extern_fn_types  : (decls, df)  =>  (decls: [Decl], tab: SymTab, df: DiagFiles) : [Diagnostic] / Console + File   -- as the driver assumed
      -- calls `ffi_kind_of_ty(t, tab)` (emit-sat), `collect_extern_types(decls, tab)` (typer-sat), `extern_type_lookup(reg, name: String)` (base)
head_tag_decl.kai  impl_target_head_tag  : (home, tname, decls)  =>  (home: Option[String], tname: String, decls: [Decl], tab: SymTab) : Int   -- as emit-sat calls it; emit_c.kai 9789 (`stdlib_head_tag_c`, part B) must pass `cx.tab`
head_tag_decl.kai  ctor_owner_head_tag  : (home, ctor, decls)  =>  (home: Option[String], ctor: String, decls: [Decl], tab: SymTab) : Int   -- as emit-sat calls it
head_tag_decl.kai  NEW  decl_head_tag(s: SymId, decls: [Decl]) : Int   -- the head tag of the record / sum declaration `s`, by id (-1 when `decls` holds none). See the report: the two functions above still
      -- find a declaration by the NAME the caller parsed out of an impl symbol / variant row, and no declaration is home-spelled any more, so two modules' `T` are told apart only by the impl's home.
      -- A caller that holds the id (an impl fn's `pid_impl(ids, s)` -> PIR target `DrSym(t)`; a constructor's owner) should ask this one.
head_tag_decl.kai  decl_takes_head_tag(d) : UNCHANGED
rec_stamp.kai  stamp_targets  : (impls, derived: [String], cores)  =>  (impls: [ProtoImplReg], derived: [SymId], cores: [String]) : [SymId]   -- as protos-B posted (`lowered.derived`)
rec_stamp.kai  rec_stamps(decls, ids), rec_stamp_of(stamps, ty) : UNCHANGED (reads `PIR(_, target: DeclRef, _, impl fn: SymId, _, _, home)`; a target counts once it is `DrSym`)
home_tu.kai / import_uses.kai  : signatures UNCHANGED
```
LEAD, for the `EOp` -> `EffOp` rename: six-field effect-operation sites left untouched in this batch — modules.kai `qualtype_eops` (pattern + constructor), `qtchk_eops`, `rqc_fx_op_names`; qual_head.kai `qh_eops` (pattern + constructor), `qhchk_eops`.
The two `EOp(_)` arms in modules.kai (`qualtype_expr_kind`, `rqc_kind`) are the EXPRESSION node (one field): do not rename them.
(agent front-load-A)

## effect run-time spelling
(agent emit_c-C; `emit_c.kai` part C = `emit_nullary_enum_seed` .. end of file. PRELIMINARY, posted before the code; lines are only added below.)
No node carries a contested effect's home-spelled name any more, so every EMITTED spelling of an effect comes from ONE function, by id:
```
emit_c.kai (C)  NEW  pub fn eff_rt_name(tab: SymTab, root: Option[String], s: SymId) : String
      -- the run-time NAME of the effect `s`: exactly what the `DEffect` name slot held after the effect scope pass (`es_spell`). It is the declared name
      -- (`decl_name(tab, s)`), EXCEPT when another module declares an effect of that name (`st_candidates(tab, NCEffect, name)` holds two or more). Then:
      --   * a core module's declaration keeps the bare name (the runtime binds the bare spelling to it);
      --   * the root's keeps the bare name unless a core module declares the name too;
      --   * any other is `hs_spell(name, home)` (home_spell.kai: `<name>__<home, dots as underscores>`), `home` = `decl_home(tab, s)`.
      -- `root` is the root module's home: `EmitCtx.root_mod`, `emit_program`'s / `lower_to_kir`'s `target_origin` (None: no declaration is the root's).
      -- "is this module a core module" has no id-level source below the driver (the core list is the driver's `core_mods`): the function reads it as
      -- "the module declares a core effect" — the owners of `core_effect_id(CeMutable | CeSpawn | CeProcess)`, i.e. `effects`, `concurrent`, `os`,
      -- the three core modules that declare effects. (See the report: needs a ruling.)
emit_c.kai (C)  NEW  fn eff_label(cx: EmitCtx, s: SymId) : String = eff_rt_name(cx.tab, cx.root_mod, s)   -- the same, from the emit context
```
What is spelled FROM that name (text in, exactly as before): the evidence struct `ev_name(<rt name>, eff_mo(effs, s))`; the runtime label and the instance key
(`inst_key_of_callee(<rt name>, ty)`, `inst_key_of_tes(<rt name>, targs)`, `key_of`); `_kai_default_ev_<lower(rt name)>` / `_kai_default_node_<…>`; the
`kai_evidence_init_default(.., "<rt name>", ..)` string; KIR's `kir_eff_name` / `kir_eff_key`.
TO REWIRE AT MERGE — each of these spells `written_text(decl_name(tab, s))` today, the bare name even for a contested effect, so two homonymous effects of two modules
would share one `Ev` struct and one run-time label:
- part B `ev_name_of(tab, effs, s)`  ->  `ev_name(eff_rt_name(tab, root, s), eff_mo(effs, s))`. It needs `root`: its callers hold `cx.root_mod`; part C's calls of
  `emit_clause_forwards(cs, effs, tab)` / `effect_struct_decls(decls, tab)` have `target_origin` at hand and can pass it once the two take it.
- part A's perform / handle-install sites: wherever `decl_name(cx.tab, <effect id>)` feeds `ev_name`, `inst_key_*` or a label string  ->  `eff_label(cx, s)`.
- kir.kai `kir_eff_name(tab, s)` (and the driver's symid_dump through it): it needs the root too. kir*.kai do not import emit_c.kai; the function reads only
  symtab / decl_text / core_effects / home_spell, so the lead can move it to a file both import (it is written to move as is).
emit_c.kai import block (part A's): part C needs ONE line beyond the four part A adds (written, decl_text, local_names, spec_names): `import compiler.home_spell` (`hs_spell`).

## emit_c part C (agent emit_c-C) — signatures (PRELIMINARY)
`pub`, called by the driver:
```
emit_c.kai (C)  emit_program  : (file, src, raw_decls, bmode, target_origin, regions, stamp_ids)
                           =>  (file: String, src: String, raw_decls: [Decl], tab: SymTab, specs: SpecNames, ids: ProtoIds, bmode: BuildMode, target_origin: Option[String], regions: [RegionMark], stamp_ids: [SymId])
      -- `tab` / `specs` where the driver assumed them, and ONE MORE right after: `ids` = the protocol registry's `ProtoIds` (`proto_reg.ids`). Part B's
      -- `emit_proto_runtime_tables(decls, tab, specs, variants, fns, ids, ctors, entry)` needs it (ownership's `reusable_tags(decls, ids, ctors, entry)` -> mono's `live_decls(decls, ids, entry)`).
emit_c.kai (C)  emit_program_modular         =>  (file, src, raw_decls, tab, specs, ids: ProtoIds, bmode, target_origin, regions, stamp_ids)
emit_c.kai (C)  emit_program_modular_cached  =>  (file, src, raw_decls, tab, specs, ids: ProtoIds, bmode, target_origin, regions, cached, cacheable, stamp_ids)
emit_c.kai (C)  tcrec_rewrite_decls  : (decls, allow_trmc, cons_ok, root, tab)  =>  (decls: [Decl], allow_trmc: Bool, cons_ok: Bool, root: String, tab: SymTab, specs: SpecNames)
      -- ONE more, LAST (`MonoFront.specs`): the goto label is the fn's emitted symbol, and a specialisation's name is in `specs` (fnreg's `fn_emit_name(tab, specs, fid)`). driver.kai ~5722.
emit_c.kai (C)  dump_types(decls: [Decl], tab: SymTab);  dump_mono(file, decls, tab);  dump_mono_out(file, decls, tab)   -- as the driver assumed
emit_c.kai (C)  expand_aliases_in_decls  : (decls, aliases)  =>  (decls: [Decl], tab: SymTab, aliases: [AliasResult])
      -- NOT base (driver, 2 calls): the label dedup keys a label's type arguments with typer-sat's `type_key(tab, te)`.
emit_c.kai (C)  collect_transparent_aliases  : (decls, eff_names)  =>  (decls: [Decl], tab: SymTab, eff_names: [String], target: String, cores: [String]) : [TAEntry]
      -- TWO more than the driver assumed: `target_mod_name` and `core_mod_names(core_mods, [])`. Why: the type pass settles a reference to an alias of a non-root, non-core
      -- module to `TyName(DrSym(alias id), _)`; one that stays WRITTEN may only reach an alias the old pass left bare-spelled — the root's or a core's. The entry records which
      -- (by the owner's module id, `st_module(tab, target)` / `st_module(tab, core)`); without it a written `T` of one file would be expanded by a homonymous alias of an unrelated module.
emit_c.kai (C)  expand_ta_decls  : (decls, table, aliases)  =>  (decls: [Decl], tab: SymTab, table: [TAEntry], aliases: [AliasResult])   -- NOT base: a written reference is matched by the alias's name, read from the table
emit_c.kai (C)  ta_expand_one  : (d, table, aliases, fuel)  =>  (d: Decl, tab: SymTab, table: [TAEntry], aliases: [AliasResult], fuel: Int)   -- no caller outside emit_c.kai
emit_c.kai (C)  TAEntry  : TAE(String, [String], TypeExpr, String, Bool)  =>  TAE(SymId, [String], TypeExpr, Bool)   -- the alias's id, its tparams, its body, "a written name reaches it" (root / core). Nobody outside emit_c.kai reads it.
emit_c.kai (C)  mangle_core_segments  : (decls, segs, n_core)  =>  (decls: [Decl], tab: SymTab, segs: [CoreSegment], n_core: Int)
      -- NOT base (driver ~4934: pass the table that holds the core rows). No declaration can be renamed any more: inside a core segment, a written reference to that segment's
      -- own private sum type becomes `TyName(DrSym(type id), args)` (it was the respelled `<module>::T`); the `DType` is returned as it came. The name is read from the table.
emit_c.kai (C)  desugar_interp_decls, desugar_index_decls : signatures unchanged (both run before the resolver, on written text)
```
Part-C functions parts A / B call:
```
emit_c.kai (C)  effect_has_default_block  : (eff_name: String, decls)  =>  (eff: SymId, decls: [Decl]) : Bool                       -- part A (2 calls: the perform site, ~2908)
emit_c.kai (C)  emit_trmc_step  : (src, sym, holeslot, cname, dropmask, pnames, args_e, cx)  =>  (src: String, s: TrmcStep, args_e: [Expr], cx: EmitCtx)
      -- part A (~2762): `EMark(MkTrmcStep(s)) -> emit_trmc_step(src, s, args_e, cx)`. The step is read whole because the cell's layout and the reuse-donor test go by the
      -- constructor's ID: part C reads `s.ctor` — the `TrmcStep.ctor: SymId` field the kir agent asked the lead for (`sym_none()` on a builtin cons step) — and FILLS it where it
      -- builds the step (`TrmcStep { sym, hole, cname, ctor, dropmask, params }`; `cname` stays the text the runtime string and the dump print).
emit_c.kai (C)  UNCHANGED for callers: tcrec_emit_goto(src, sym, dropmask, pnames: [String], args_e, cx), tcrec_emit_param_ptrs(params, i), tcrec_emit_param_ptrs_masked(params, usig, i),
      tcrec_body_has_sentinel(body), trmc_body_has_step(body), name_first_is_upper(s), mangle_ty(t: Ty) (table-free: what part B's `fixed_vec_cname` needs, a scalar's mangle;
      NEW `mangle_ty_in(tab: SymTab, t: Ty)` spells a nominal head from the table).
```
(agent emit_c-C)

### infer part C — DONE, with CORRECTIONS to its section above (agent infer-C; commit 40622a6b on branch agent-a87ee23c0f6d73704)
The commit changes only part C of `infer.kai` (the comment heading `try_op_call` .. before `pattern_collect_binds`), on the lane base baf56156.
```
infer.kai  lookup_op_eff_arity  : (table: [OpEffArity], eff: SymId) : Int   -- CONFIRMED two arguments, as first posted and as part B calls it
infer.kai  a collection pipe whose head module ships no `map`/`flat_map`/`filter`  -- CORRECTION: the callee is `EName(written(op))` handed to part B's `synth`, which settles it as any bare name
      -- (not `ESym(builtin_sym(op))` built by part C). The same holds for the dispatcher fallbacks `EName("__proto_<op>")` (binop / pipe, when the registry has no id for it).
infer.kai  synth_stmt SLet  -- stores the stamped pattern, as part D asked: `let after = check_pattern_stamped(dup_st, pat, binding_ty)`; `SLet(after.pat, ann, …)`, `st: after.st`
infer.kai  report_ctor_ambiguity_if_any(st, w: Written, line, col)  -- called as part D posted it; the `ESym` arms are gone
infer.kai  collect_escaped_names(ids: ProtoIds, e, acc: [LocalId]) : [LocalId]  -- the escape walkers KEEP `ids`: an unsettled `EModCall(m, n)` callee asks
      -- `pid_qualified_is_disp(ids, m: Written, n: Written)` (protos-sat's signature); a callee `ELocal` / `ESym` / `EOp` / `EName` is a named callee
infer.kai  all_demands_local(tab: SymTab, e, locals: [LocalId], escaped: [LocalId], opaque: [LocalId], dmut: [SymId])  -- private to part C; `tab` reaches `writes_only_first_arg(tab, s)`
infer.kai imports  -- part C needs TWO lines the merged import block does not have yet: `import compiler.local_names` (LocalNames, ln_empty, ln_with_binder)
      -- and `import compiler.call_site_target` (home_fn_ref). Part C adds no import line (part A's block): LEAD, add them at merge.
```
Still expected from batches not merged when part C was checked (part C is written against exactly these; each is a part-C error until its owner lands):
```
ast.kai                 the `EOp` clash (LEAD NOTE): part C builds `EOp(OpRef { eff, op: written(op), via })` once, in `op_callee`
mutation_carriers.kai   CarrierShape = CS(SymId, [TypeExpr]); sum_shape(s: SymId, vs); mutating_type_names(shapes) : [SymId]; ty_may_carry_mutation(t, dmut: [SymId]);
                        writes_only_first_arg(tab: SymTab, s: SymId) : Bool   (agent checks: nothing posted for these yet)
mutation_callees.kai    opaque_binders(params, body) : [LocalId]; callee_is_opaque(callee, opaque: [LocalId]); fn_value_is_transparent(e, opaque: [LocalId])   (agent checks)
tycon_ref.kai           tsh_tycon_at(v, r: DeclRef, args: [Ty]) : Ty   (typer-sat, as posted)
infer.kai (part D)      check_pattern_stamped / PatChecked { st, pat }; report_ctor_ambiguity_if_any(st, Written, line, col)   (as part D posted)
evidence_inject.kai:66  spelled_head_owners(decls, tab, [])   (agent checks; part C's signature as posted)
```
Checked: on the lead's merge 326e0d1dc (the real parts A and B, protos, tools-B, resolver, driver) with part C spliced in, the two import lines added and the
signatures listed just above stubbed, part C type-checks with 0 errors and 0 warnings; no error of parts A / B names a part-C function.
(agent infer-C)

## desugar API (desugar.kai) — owner: agent desugar

Every entry of desugar.kai runs BEFORE the resolver (driver.kai 5220–5528; the resolver is at 5574). Locals stay text there: the scope
walks key on `written_text(b.name)` / `EName(w)`. A name that denotes a DECLARATION is settled to its id through the table and the
file's scope view (the only declaration of that name, else `sv_resolve` from the view of the file the name sits in), so three entries
take `tab` + views. `desugar.kai` now imports compiler.written / symtab / decl_text / scope_view / scope_file.
```
desugar.kai  NEW  pub type DsgMinted = { decls: [Decl], rows: [NEnt] }   -- a lowered stream + the rows of what the lowering minted; the caller does st_extend(tab, r.rows)
desugar.kai  desugar_pos_records_decls  : (ds: [Decl], df: DiagFiles)  =>  (ds: [Decl], tab: SymTab, views: FileViews, root: String, df: DiagFiles) : DPRDecls / Console + File
      -- driver.kai 5371: (raw_decls_enforced, <table>, sym_views, target_mod_name, diag_ctx). A literal head `DrSym(s)` is that record; a written
      -- head is settled per declaration under `fv_of(views, <the decl's module>)`. The field-probe tie-break (rec_find_with_field) is gone: scope decides.
desugar.kai  lower_axioms  : (ds: [Decl]) : [Decl]  =>  (ds: [Decl], home: String) : [Decl]   -- driver.kai 5233: home = target_mod_name (a DAxiom still in the merged stream is the root's)
desugar.kai  lower_axiom_one  : (d: Decl) : Decl  =>  (d: Decl, home: String) : Decl   -- driver.kai 2449 tag_decl_module_origin: `lower_axiom_one(d, mod)`.
      -- The DFn is `fnid_decl(sym_derive(NCFn, name, home))`: the row the parser minted for the axiom; no new row.
desugar.kai  lower_extern_opaques  : (ds: [Decl]) : [Decl]  =>  (ds: [Decl], root: String) : DsgMinted   -- driver.kai 5233: root = target_mod_name.
      -- The lowered record is `DType(false, sym_derive(NCType, name, home), …)`, home = the DExternType's module origin or `root`;
      -- row `NEnt(NCType, name, home, false)` (what nt_collect derived from the lowered DType before). The DExternType's own NCExternType row stays.
desugar.kai  synthesize_refine_pred_fns  : (decls: [Decl], target_mod: String) : [Decl]  =>  (decls: [Decl], tab: SymTab, target_mod: String) : DsgMinted   -- driver.kai 5220
      -- `__ref_pred_<T>` is `fnid_decl(sym_derive(NCFn, "__ref_pred_<T>", <home of T in tab>))`, row `NEnt(NCFn, "__ref_pred_<T>", home, false)`.
      -- `tab` must hold the alias types (the parser's rows do).
desugar.kai  refine_pred_fn_syms  : (decls: [Decl], target_mod: String) : [SymId]  =>  (decls: [Decl], tab: SymTab) : [SymId]   -- driver.kai 5221
desugar.kai  collect_refine_alias_names  : (decls) : [String]  =>  RENAMED collect_refine_alias_ids(decls: [Decl]) : [SymId]   -- driver.kai 5227. The old name stays as a TEMPORARY shim (same result) so the tree keeps resolving; delete it with the call.
desugar.kai  lower_pattern_narrow_decls  : (decls: [Decl], aliases: [String]) : [Decl]  =>  (decls: [Decl], tab: SymTab, views: FileViews, root: String, aliases: [SymId]) : [Decl]   -- driver.kai 5228
      -- a narrow `p : T` whose T is a refinement alias becomes `PBind(p)` + guard `ECall(ESym(<the alias's __ref_pred fn>, spec_none()), [EName(p)])`:
      -- the predicate fn is referenced BY ID before the resolver (the resolver leaves an `ESym` alone), so its row must be in the table the resolver gets.
desugar.kai  desugar_use_decls  : (decls: [Decl], file: String)  =>  (decls: [Decl], tab: SymTab, view: ScopeView, file: String) : UseRwProgram / Console + File
      -- driver.kai 5428: view = fv_of(sym_views, st_module_id(target_mod_name)) (only root bodies are rewritten). `use E` / `DUse` / `SUse` settle `E` to the
      -- effect's id; a bare op of an opened effect becomes `EField(ESym(effect id, spec_none()), op)` (it was `EField(EName(E), op)`): the same node the
      -- effect scope pass and the resolver write. Two homonymous effects in view no longer open the first by name.
desugar.kai  check_nursery_brand_mismatch_decls  : (ds, df, calls, brs)  =>  (ds: [Decl], tab: SymTab, df: DiagFiles, calls: [BrandCall], brs: [BrandRec]) : Int / Console + File
      -- driver.kai 5528: brand_transp_decls(ds, tab) (surface) spells each DFn's name, so `tab` must hold every DFn of `ds` (protocol-lowered fns included): `sym_tab`.
desugar.kai  fn_decl_sym  : (d: Decl, root: String) : SymId  =>  TEMPORARY shim (answers the DFn's own id; `root` ignored). No caller once refine_enforce.kai (tools-B) merges: delete.
desugar.kai  dsg_map_expr_kind, interp_span_reads_cap(span, is_triple, name: String), lower_cell_reads_decls, relower_cell_reads_decls, desugar_var_decls,
             rewrite_nursery_caps_decls, dump_brands, BrandRec / BrandCall  -- signatures unchanged (dsg_map_expr_kind treats `ELocal` / `EOp` as leaves)
```
Called with the signature its owner posted: `rc_check_var_writes(name: Written, ann, body)` (tools-B), `brand_transp_decls(ds, tab)` (surface).
Driver order note: `synthesize_refine_pred_fns` and `lower_extern_opaques` return rows AFTER `early_tab` is built — extend the table with both
(`st_extend(early_tab, list_append(synth.rows, opaques.rows))`) before any later pass or the resolver reads it.
(agent desugar)

## EmitCtx — FINAL, emit_c part A (agent emit_c-A; everything before the comment block / `type SharedCtor` above `fn arms_shared_ctor`; adds to the `## EmitCtx` block above)
Part A is written against part B's FINAL block (it defines none of part B's private helpers and calls `locals_have`, `pat_local_ids`, `emit_scope_spelled`, `emit_ctx_declaring`,
`emit_fn_text`, `capture_locals`, `ev_name_of`, `decl_ref_text`, `tcp_rc_binders`), emit-sat, ownership (part 2), mono, kir and infer-A's `idx_*_local`.
Checked: part A + part B's commit 892db93f + the current files of emit-sat / ownership / mono / kir type-check with ONE error in part A: `pat_binders(p, acc: [Binder])` (modules.kai, front-load-A, not repaired yet).
`EmitCtx`: exactly the three new fields posted (`tab`, `specs`, `locals`, declared last) — no other field was added. `lcs` / `borrowed` / `kind1_raw` are `[LocalId]`, `tcpend` is `[TclEntry]`.
Part A defines (use them, do not re-define): `emit_local_text(cx, l) : String`, `c_local_of(cx, l) : String`, `emit_ctx_with_locals(cx, ns)`, `binder_text(b: Binder) : String` (= `written_text(b.name)`),
`binder_ids(bs: [Binder]) : [LocalId]`, `pat_ctor_row(cx, r: DeclRef) : Option[VariantEntry]`, `pat_ctor_kinds(cx, r: DeclRef, arity: Int) : Option[[Int]]`, `pat_ctor_tag(cx, r) : Int`,
`emit_ctor_tag_test(scr: String, r: DeclRef, cx) : String`, `effect_declares_default(effs: [Decl], s: SymId) : Bool`.
Signatures of part A that CHANGED, for callers outside part A:
```
emit_c.kai (A)  build_ctor_int_slots  : (decls)  =>  (decls: [Decl], tab: SymTab) : [CtorIntSlots]                -- driver.kai (as the driver already calls it)
emit_c.kai (A)  mono_sum_decls  : (decls, vs: [VariantEntry])  =>  (decls: [Decl], tab: SymTab, ctors: CtorIx) : String   -- part C (2 calls): pass the constructor index built WITH its root (the one `cx.ctor_ix` holds); a ctor's slot kinds are its row's, by id
emit_c.kai (A)  ReuseDonor(String, [String])  =>  ReuseDonor(SymId, [LocalId])                                   -- part C reads `cx.reuse_donor` (the single-store TRMC step): the donor's CONSTRUCTOR id and its positional binder ids (`local_none()` = no plain binder)
emit_c.kai (A)  emit_ident_value(fid: FnId, cx), const_body_of(consts, s: SymId), eff_mo(effs, s: SymId), emit_unw_vars / emit_unw_own / emit_unw_let_owned ([LocalId]), pat_kind1_binders ([LocalId])   -- as part B posted
emit_c.kai (A)  emit_variant_call  : (home, name, n_args, slots, cx)  =>  (tag: Int, name: String, n_args: Int, slots: SeqOps, cx)        -- no caller outside part A
emit_c.kai (A)  emit_variant_call_typed  : (home, name, args_e, tys, src, cx)  =>  (tag: Int, name: String, args_e, tys, src, cx)       -- no caller outside part A; the tag is the constructor row's (`ctor_ix_row` / `ctor_ix_site`)
emit_c.kai (A)  emit_reuse_variant_body  : (src, home, cname, cargs, cx)  =>  (src, row: VariantEntry, cargs, cx)
emit_c.kai (A)  arm_ptr_binders(subs, tys, i) : [Binder];  arm_moved_binders(binders: [Binder], body) : [Binder]   (pub; no caller outside emit_c.kai)
emit_c.kai (A)  UNCHANGED for callers: nullary_ctor_tag(e, cx), emit_cons_ctor, variant_slot_inits, const_decls_only, effect_decls_only, emit_variant_tag_test(scr, home, name: String, variants) (a constructor the emitter names by literal),
      store_raw_int, emit_variant_slot_init*, emit_call_expr, emit_mark_call, emit_user_call*, emit_fn_call_seq, emit_evf_*, evf_*, and every function part B lists as BASE.
```
REMOVED from part A (no caller is left in part B; part C never called them):
```
find_ambig_efns, report_ambig_efns, collect_root_bare_callees, AmbigEntry / AmbE, walk_bare_callees*, ambig_mods_of, mods_contain_target, list_head_or   -- the bare-name ambiguity warning: NO caller anywhere at the base
      -- (fnreg.kai `efn_collect_matching` / `efn_pick_root` lose their last reader; the comment at fnreg.kai:510 names them)
spelled_ctor_arity, spelled_ctor_kinds, ec_callee, ec_local_callee, ctor_qual_home, callee_names_ctor, emit_guard_no_intrinsic_mint, emit_variant_tag_test_arity, rcn_payload_tys,
eff_mo_fallback, const_body_of_root / _first / _home, ubind_name
emit_named_call_lookup  =>  emit_sym_call(src, fid: FnId, args_e, cx)   (by id: the fn, the raw core prim, the boxed builtin)
emit_call_expr_spelled  =>  emit_callee_call(src, f, args_e, cx)        (dispatch on the callee's node: ESym / EOp / ELocal / EName / tail-step marks)
```
What part A CALLS outside itself:
```
emit_c.kai (C)   effect_has_default_block(eff_name: String, decls) : Bool   -- BASE signature, ONE caller left in part A: `evf_thunk_slot(key, effs)`, which decides from an evidence-frame SLOT KEY
      -- (a runtime string, `inst_key_eff(key)`): NEEDS DESIGN, the frames carry no effect id (see my report). Part C: keep a by-name form for this caller until the lead rules; the perform site no longer
      -- calls it (it asks `effect_declares_default(cx.effs, r.eff)`, by id).
emit_c.kai (C)   emit_trmc_step(src, sym, hole, cname, dropmask, params, args_e, cx), tcrec_emit_goto(src, sym, dropmask, params, args_e, cx)   -- BASE signatures, fed from `TrmcStep` / `TcrecStep` as at the base
emit_c.kai (B)   tcp_arm_body_cx, emit_expr, emit_expr_raw, box_wrap, raw_c_type, unbox_boxed_scalar, hoist_slot_member : unchanged
effect_default.kai (typer-sat)   effect_decl_of(s: SymId, decls) : Option[Decl], effect_decl_default(d) : Option[DefaultBlock]   -- BASE signatures
effect_instance.kai (typer-sat)  inst_key_of_callee(eff: String, callee_ty: Option[Ty]) : String, inst_key_eff(key)            -- BASE signatures (the effect's name is `written_text(decl_name(cx.tab, r.eff))`)
evidence_frame.kai (checks)      frame_slot_here(evf, enc: String, key: String) : Int, frame_slots_of(evf, csym: String) : [String]   -- BASE signatures (runtime keys)
fnreg.kai / fn_index.kai (mono)  EFn(n: Written, ar, mo, sig, fid) spelled `c_sym(written_text(n), mo)`; fnx_callee(fx, fid), fnx_of_sym(fx, fid), fnx_kai(fx, fid)
ctor_ix.kai (mono)               ctor_ix_row(ix, s), ctor_ix_site(ix, e), ctor_ix_names(ix, e), ctor_row_tag(row); `VariantEntry` read as `EV(home, name, arity, kinds, tag)`
emit_shared.kai (emit-sat)       fv_expr / fv_stmt(x, own: [Binder], outer: FvOuter, acc: [Capture]), fv_outer_of(binders), pat_bindings(pat, acc: [Binder]), register_variants(decls, tab),
                                 evar_find / evar_find_tag_arity / evar_slot_kinds_arity(variants, None, name: String, n)  -- ONLY for the callee an interpolation fragment spells (below)
emit_tcrec_live.kai (emit-sat)   tcl_filter(ns, pend, e), tcl_filter_stmts(ns, pend, ss), tcl_hold, tcl_unw_entry, tcl_scr_entry, tcl_tok_entry
emit_reuse_slots / _nested (emit-sat)  cons_direct_binders(head_pat, rest: LocalId), rv_untaken_slot_drops / rv_taken_slot_clears / rv_taken_binders(…, taken: [LocalId]),
                                 rv_slot_settles(cx.locals, subs, ks, token, taken, cx.ctor_ix), rv_slot_leaf_dups(cx.locals, subs, ks, taken, cx.ctor_ix), rv_cell_binds(sp, cell, cx.ctor_ix), rv_bind / reuse_slot_decl / cons_rest_bind (text in)
reuse_uses.kai (changed by kir)  reuse_gc_uses / reuse_taken_uses(ns: LocalNames, cargs, gc: [LocalId]) : [LocalId]   -- THREE arguments, as the file in kir's worktree has them (kir's line above says two)
reuse_ctors.kai (ownership)      reuse_ctor_id(ctors, ctor) : Option[SymId], reuse_same_ctor(p: DeclRef, b: SymId)
kir_pat_alias.kai (kir)          pat_unbound_ptr_slots(p: Pattern, ix: CtorIx) : [Int]
unwind_*.kai (ownership)         unw_admit, unw_owning, unw_call_of, unw_destructured, unw_call_unwinds(f, xs, bound: [LocalId], ucx), unw_bare_read : as posted
modules.kai (front-load-A)       pat_binders(pat, acc: [Binder]) : [Binder]   (as ownership / surface / perceus posted)
infer_index.kai (infer-A)        idx_prepend_local(ix, l, payload), idx_chain_local(ix, l)   -- the unused-binding report's read index
handler_alias.kai                canonical_continuation() : String (the mint key)
```
What part A expects of the tree it emits: a continuation call's callee is `ELocal(local_minted(canonical_continuation()))`; a perform's callee is `EOp(r)` (it reads `r.eff`, `r.op`, `r.via`, and `f.ty` for
the instance key); a constructor call / value is `ESym(c, _)` found in `cx.ctor_ix`; a reuse rebuild's constructor call is `ESym`; the owned-borrow temps are read as `ELocal(local_minted("bc<i>"))`.
The ONE by-name callee path: `emit_spelled_call` — an `EName` callee (only a fragment parsed at emit time spells one) names the binder in scope of that spelling (`emit_scope_spelled`), else the
continuation by its key, else a constructor of the variant table by name, else the C local `kaiv_<name>`. An `EModCall` callee has no arm left (it falls to the closure apply of `emit_expr`).
Imports: the block still holds `import compiler.sym_read` (parts B/C called it at the base; part A no longer does) — drop it with the shims. After part B's merge `compiler.name_scope` and
`compiler.proto_ids` are reported unused in emit_c.kai (part A's last readers died); left in place for part C to decide.
(agent emit_c-A)

## emit-sat FINAL (agent emit-sat) — the code as committed; where a line differs from the PRELIMINARY emit-sat section above, this one stands
Checked by laying the batch over the lead's merge 2daa4437 (scratch copy, stand-ins only for names of batches not merged yet): 9 errors left in the batch, every one a call
into a batch not merged then, written as its owner posted it:
```
emit_shared.kai  scan_uses_expr(ns, e, in_lam) x2, scan_uses_in_stmts(ns, stmts, in_lam)      -- fnreg.kai (mono)
emit_shared.kai  fnx_ufn_sig_first(fx, fid)                                                   -- fn_index.kai (mono)
emit_shared.kai  obl_eff_handled_anywhere(decls, decls, eff: SymId)                           -- evidence_scan.kai (checks)
emit_shared.kai  label_to_string(tab, l)                                                      -- infer.kai (D)
cache_module_test.kai  RCS(SymId, …), HOwner(SymId, [LocalId]), RRH(…, SymId, Row, [SymId]), OPR(FnId, Written, [SymId], SymId, Option[LocalId], Bool, Int, Int)   -- infer.kai (B / D)
```
What changed against the preliminary section (each to the shape the merged emit_c part B already calls):
```
emit_shared.kai      fn_emit_name                    -- NOT defined here: it is fnreg's (mono), `fn_emit_name(tab, specs, f) : Written`; emit_shared / emit_reuse_donate import compiler.fnreg for it
emit_shared.kai      block_unused_lets(stmts: [Stmt], opt_final: Option[Expr]) : [LocalId]      -- 2 args (emit_c B, kir): it names the block's own `let` binders itself
emit_shared.kai      arms_pbind_safe_for_fast_path(arms: [Arm]) : Bool                           -- 1 arg (emit_c B)
emit_shared.kai      collect_pimpl_rows(ds, fns: [EFn], tab: SymTab, specs: SpecNames, acc)      -- as first posted (emit_c B calls it so; emit_native_core / emit_native_proto follow). It no longer reads `EFn`'s
                                                                                                    -- name slot: the emitted name is `fn_emit_name(tab, specs, fid)`.
emit_shared.kai      local_among(ls: [LocalId], l: LocalId) : Bool                               -- NEW pub (id membership; emit_reuse_* use it). Not `locals_have`: emit_c part B keeps a private one of that name.
emit_shared.kai      AliasMap = AM([(LocalId, SymId)], Bool)                                     -- its own pairs, not infer-B's `AB`
emit_shared.kai      expr_has_interp_use(body, binder) is called as ownership merged it (unbox.kai)
emit_tcrec_live.kai  tcl_push_binders(ns: LocalNames, pend: [TclEntry], ls: [LocalId])           -- 3 args, as first posted and as emit_c part B calls it (`cx.locals` first).
                                                                                                    -- kir (kir_lower_walk `tcl_push_binders(with_scr, binders)`): pass the declaration's binder map first.
emit_tcrec_live.kai  tcl_local(e: TclEntry) : LocalId                                            -- as kir asked (`local_none()` for a non-binder entry). There is NO `tcl_binder`.
emit_tcrec_live.kai  tcl_cname(ns, e), tcl_filter(ns, pend, e), tcl_filter_stmts(ns, pend, ss), tcl_goto_entries(ns, pend, args), tcl_goto_drops(ns, pend, args), tcl_consumes(ns, e, l),
                     tcl_span_mentions(span, w: Written); tcl_exit_drops(pend) / tcl_exit_entries(pend) / tcl_hold / tcl_unhold / tcl_drop_unw / tcl_without_tok / tcl_scr_count take no map
emit_ffi_c.kai       every signature as at the base (its two `p.pname` reads are `written_text(p.binder.name)`)
```
TEMPORARY, at the end of emit_shared.kai (the convention of ast.kai's shim block): `build_globals`, `collect_op_qualified_names`, `const_names` are kept as pub stubs returning `[]`, so the three
callers still written as at the base resolve and show a type error instead of halting the checker. Each call becomes `lc_new(tab, specs)`; then the block is deleted:
```
emit_c.kai (part C)   `let st0 = lc_new(globals)` x2 (emit_program / emit_program_modular)  =>  lc_new(tab, specs)   -- and the `globals = build_globals(…)` let above each goes
kir_lower_fns.kai     `collect_decls(decls, lc_new(build_globals(…)))`                       =>  collect_decls(decls, lc_new(tab, specs))
```
Caches:
```
cache.kai, cache_ast.kai, core_cache.kai, user_cache.kai   -- byte-identical to what the driver's commit 4a913c82 holds for the rows / the home in the user key (no merge conflict), plus the bumps below
version stamps bumped ONCE each:  cache.kai cache_format_version 19 => 20 (the driver's bump, same line)   cache_module.kai uc_typed_key_version 13 => 14
                                  user_cache.kai uc_key_version 1 => 2   core_cache.kai cc_key_version 1 => 2   emit_cache.kai ec_format_tag "KCT1" => "KCT2"
cache_delta.kai   the interface hash sorts and keys each table by ID: tyentry_key = env_key_ord(e.key) (infer-A), unions / recs / sums by `sym_raw`, op_eff_arities by (effect id, op label),
                  op_to_eff by (op label, effect id), constructors by (type id, fn id); unit_aliases by name as before (UAE is a unit's name, not a declaration read from a node).
                  pub signatures unchanged.
cache_delta_test.kai / cache_module_test.kai   fixtures mint ids with `sym_derive_word(word, name, home)`; `Label { sym, ty_args }` literals (2 + 2) as ast.kai has `Label` today:
                  if `Label` gains `cell: LocalId`, each adds `cell: local_none()`.
```
(agent emit-sat)
desugar.kai  desugar_use_decls  : CORRECTION to the desugar line above (FINAL): (decls: [Decl], file: String)  =>  (decls: [Decl], tab: SymTab, views: FileViews, root: String, file: String) : UseRwProgram / Console + File   -- driver.kai 5428: (var_decls, <table>, sym_views, target_mod_name, path); the same (tab, views, root) triple as desugar_pos_records_decls and lower_pattern_narrow_decls (agent desugar)
emit_c.kai (part A)  DONE: commit 8c426c5b on branch agent-ad6fdaf91e7648240 (see `## EmitCtx — FINAL` above). Alone it stops at name resolution (it calls part B's private helpers and other batches' new functions); merged with part B's 892db93f it type-checks but for `pat_binders` (front-load-A). (agent emit_c-A)
desugar vs the driver's ASSUMED lines (driver section above) — what matches and which driver calls change (agent desugar):
```
MATCH    lower_axiom_one(d, home), lower_axioms(ds, home), synthesize_refine_pred_fns(decls, tab, target) with `.decls` / `.rows`
CHANGE   refine_pred_fn_syms(decls, tab, target)            ->  refine_pred_fn_syms(decls, tab)                       (the id is derived from the alias type's own row; no target)
CHANGE   collect_refine_alias_names(decls, tab)             ->  collect_refine_alias_ids(decls) : [SymId]             (no table: the DType carries its id)
CHANGE   lower_pattern_narrow_decls(decls, names)           ->  lower_pattern_narrow_decls(decls, tab, sym_views, target_mod_name, ids)
CHANGE   lower_extern_opaques(ds)                           ->  lower_extern_opaques(ds, target_mod_name) : DsgMinted (`.decls`, `.rows`)
CHANGE   desugar_pos_records_decls(ds, tab, df)             ->  desugar_pos_records_decls(ds, tab, sym_views, target_mod_name, df)
CHANGE   desugar_use_decls(decls, tab, file)                ->  desugar_use_decls(decls, tab, sym_views, target_mod_name, file)
CHANGE   check_nursery_brand_mismatch_decls(ds, df, c, b)   ->  check_nursery_brand_mismatch_decls(ds, tab, df, c, b)  (tab = the table holding every DFn of `ds`)
```
WHY views + target on three entries: a written head / narrow type / `use` name that several modules declare is settled by the scope ladder of the file
it sits in (`sv_resolve` under `fv_of(views, <file's module>)`), the decision the effect / type scope passes take; with one declaration of the name the
table alone answers and the view is never built.

## kir — FINAL (agent kir; as committed on the kir branch; where a line here differs from the kir lines above, this one stands)
```
kir_lower_fns.kai  lower_to_kir  : (raw_decls, tab, specs, regions, …)  =>  (raw_decls: [Decl], tab: SymTab, specs: SpecNames, ids: ProtoIds, regions, fns, variants, target_origin, raw_binders, traps, stamp_ids, entry)
      -- ONE more argument, right after `specs`: the protocol ids (`reg.ids`, what emit_c's emit_proto_runtime_tables takes). driver.kai: its three lower_to_kir calls.
kir_dump.kai       emit_kir(decls, tab, specs, regions, inline_on, stamp_ids, entry)   -- as posted (a dump registers no reuse tags: it lowers with pid_empty())
kir.kai            KProgram  : gains `tab: SymTab, specs: SpecNames, reuse_tags: [Int]`
      -- `reuse_tags` is reuse_ctors' reusable_tags(decls, ids, ctors, entry), computed where the lowering's CtorIx (built WITH the root) is at hand; the native startup hook registers `kp.reuse_tags`.
      -- build_native_object / build_native_modular / build_native_core_split / build_native_core_reuse / ncore_prune_unreachable / kir_lin_hook / kir_inline_program KEEP their signatures (they read kp.tab / kp.specs / kp.reuse_tags).
kir.kai            NEW  kir_op_text(op: String, via: KVia) : String   -- the op as a dump prints it (`get@a`, `op#recv`, `op!`). symid_dump.kai's kperform line keeps its text with concat_all([kir_eff_name(tab, s), ".", kir_op_text(o, via)])
kir.kai            NEW  kir_alias_id_reg(a: String) : String   -- "__alias_id__" + a (the register an aliased install stashes its handler id under)
kir_lower.kai      LowerSt.locals = the declaration's `ln_of_decl(d)` / `lam_locals(l)` / `clause_locals(c)` PLUS `binder_minted(key)` for the handler-state aliases and the continuation (`kir_body_locals`); `LowerSt.regs: [String]` is the registers spelled so far (the old `locals`)
emit_native_proto.kai (nobody's batch)  nproto_build_init(ctx, decls, variants, fns, adapters, entry)  =>  (ctx, decls, tab: SymTab, specs: SpecNames, variants, fns, adapters, reuse_tags: [Int]);  nproto_emit_adapters_of(ctx, decls, fns, only)  =>  (ctx, decls, tab, specs, fns, only)   -- both called only from emit_native.kai
emit_native_def.kai (nobody's batch)    nemit_declare_default_globals_for(ctx, tab, defaults, role), nemit_build_install_defaults(ctx, tab, defaults), ndef_synth_handlers(tab, defaults)   -- called only from emit_native.kai
emit_native_fx2.kai                     nemit_perform(ctx, cur_fn, eff_key: String, esym: SymId, op: String, via: KVia, cargs, handlers)   -- the `@` / `#` / `!` op-string readers are gone
reuse_uses.kai (nobody's batch)  CORRECTION  reuse_gc_uses / reuse_taken_uses  : (cargs, gc: [LocalId])  =>  (ns: LocalNames, cargs: [Expr], gc: [LocalId]) : [LocalId]
      -- `ns` FIRST: perceus's index takes binders and a span reads one by its text, so the binder names of the declaration are needed. emit_c.kai 3800, 3801, 4324: pass `cx.locals`. reuse_taken_binders(gc: [LocalId], uses: [LocalId]) : [LocalId] as posted.
```
What kir CALLS in other batches, each as its owner posted (this replaces the assumptions of the first kir section):
```
unbox.kai (ownership)             raw_arm_binder_at(tbl, l: LocalId, line, col) with `b.id`  (RAB(LocalId, Int, Int)) -- NOT "by position only"
emit_tcrec_live.kai (emit-sat)    TclEntry; tcl_binder(e) : Option[LocalId] (not `tcl_local`); tcl_push_binders(ns, pend, ls); tcl_filter / tcl_filter_stmts / tcl_goto_entries(ns, pend, …)
emit_shared.kai (emit-sat)        Capture / lam_locals / clause_locals, LamInfo (8 slots) / CI (11 slots), lc_new(tab, specs) (no build_globals), register_variants(decls, tab), block_unused_lets : [LocalId], the `[SymId]` effect rows,
                                  collect_pimpl_rows(ds, fns, tab, specs, acc), collect_variant_to_head(variants, decls, tab), stdlib_head_tag_int(home, tname, decls, tab)
emit_reuse_donate.kai (emit-sat)  rd_scan_capable(decls, tab, specs)
handler_alias.kai (front-resolve) clause_state_aliases((key) => list_has(st.regs, key)) (the spelled scope), canonical_continuation() / user_state_aliases() / var_cell_alias() : mint keys
local_names.kai (front-parse)     ln_with_binder(ns, b)
fnreg.kai / fn_index.kai (mono)   EFn(_, arity, mo, sig, fid) (5 slots); register_fn_decls(decls, tab, specs, entry); fnx_ufn_sig_first(fx, fid); fnx_callee / fnx_of_sym / fnx_kai / fnx_ufn_by_csym / fnx_build_decls
ctor_ix.kai (mono)                ctor_ix_build(variants, decls, tab, root), ctor_ix_row(ix, s), ctor_row_tag(row)
ffi_v2.kai (typer-sat)            collect_extern_types(ds, tab), ffi_kind_of_ty_reg(t, reg, tab), ffi_ret_kind_of_reg(opt, reg, tab)
purity.kai / purity_traps.kai / reuse_ctors.kai / reuse_cells.kai / unwind_*.kai (ownership)  purity_const_syms(marks, tab, specs), trapfree_syms(decls, tab, specs), reusable_tags(decls, ids, ctors, entry),
                                  KidBind(Int, LocalId), reuse_pattern_cells(p, ctors), the `[LocalId]` unwind API (unw_fn_frame / unw_frame take `locals` first)
perceus_read_walk.kai / perceus_reads.kai / name_ix.kai (perceus)  pcs_use_index(e, scope: [Binder]), Rd(l: LocalId, …), ni_bucket_id(ix, local_raw(l))
cell_promote.kai (tools-B)        no longer called by kir (a cell's register is read off the `ELocal` of its first argument)
```
STILL NEEDED from ast.kai (lead):
```
ast.kai  TrmcStep  : { sym, hole, cname, dropmask, params }  =>  gains `ctor: SymId`   -- the spine constructor's id (`sym_none()` for the builtin cons spine). kir_lower_walk.kai lower_trmc_step / lower_trmc_seal
      -- read `step.ctor` for the spine cell's tag and slot mask (`cname` stays the dump text): a name does not say which constructor, and the old lookup went by (the built type's home, name).
      -- The pass that plants `MkTrmcStep` holds the constructor's `ESym(c, _)`. These two reads are kir's only errors once the lines above are in.
```
(agent kir)

## mono — FINAL (agent mono; as committed on branch agent-a68b4b4c18eeca52a; where a line here differs from the PRELIMINARY `## mono API` section above, this one stands)
Checked: the batch type-checks against the shapes posted in this file by front-parse (`ln_ids_named`, `nent_sym`), infer-A (`Use = U(LocalId, …)`, `label_to_string(tab, l)`),
infer-D (`RCS(SymId, …)`), protos-B / protos-sat (`DhSym | DhScalar`, `ty_disp_head`, `validate_resolved_decls(ds, tab, reg, df, errs)`, `closure_spec_pass … : CSpecOut`),
typer-sat (`tag_tycon_modules(ds, tab)`) and perceus (`ret_slot_id_is_scope`, `pcs_mentions(b: Binder, e)`, `prc_pat_bindings : [Binder]`, `ppd_drop_name : Option[LocalId]`,
`ppd_drop_among(d, [Binder])`, `ppd_let_names : [Binder]`). Against the base files of those owners it shows exactly those mismatches and nothing else.
```
monomorph.kai  MonoOutput  : MonoOut([Decl], [MLeakRecord], [BoundViolation])  =>  MonoOut([Decl], SpecNames, [MLeakRecord], [BoundViolation])   -- only mono_front.kai reads it
monomorph.kai  monomorphise(tp, reg, fn_bounds, ps, target_mod, check_only)  -- signature kept. The table is `ps_tab(ps)`: it must hold the row of EVERY generic of `tp.decls`
      -- (a spec's emitted name is `<decl_name of the generic>__mono__<types>__umono__<units>`, the same text as before). mono_front extends it with the closure-spec rows first.
      -- A spec is `DFn(pub, FnId(<the generic's sym>, <instance>), [], …)`; a call to it is `ESym(sym, instance)`; its name is `spec_text(specs, FnId(sym, instance))`.
mono_front.kai MonoFront  =>  { prog, decls, specs: SpecNames, rows: [NEnt], verdict }   -- `rows` = the closure specialisations' rows (driver: `emit_tab = st_extend(typed_tab, mf.rows)`), as the driver assumed
mono_front.kai mono_front(typed, reg, fn_bounds, ps, target_mod, file, df, check_only)  -- signature kept; `ps` holds `typed_tab`
inst_index.kai (nobody's batch; changed by mono)  iix_chain  : (ix, name: String)  =>  (ix: InstIndex, s: SymId) : [ResolvedCS]   -- as infer-D asked; the chain holds that declaration's call sites only
fnreg.kai      as posted (EFn 5 slots, slot 1 the emitted name as `Written`; fn_emit_name; register_fn_decls(decls, tab, specs, entry); fn_of_decl_sym(d, tab, specs, entry); classify_unbox_sig(entry, tparams, params, opt_ret, row, body);
               scan_uses_*(ns, …)). The `_s` family and `fnreg_modcall_is_self` are REMOVED as posted; `any_arg_effectful(es)`, `any_arm_effectful(arms)`, `any_field_effectful(fs)`, `any_elem_effectful(els)`,
               `any_stmt_effectful(ss)` are the old `any_*_effectful_s` without the two name parameters (no caller outside fnreg.kai).
fnreg.kai      CORRECTION to the scan_uses note: the local an `EOp`'s `via` names is NOT counted as a read (as at the base, where a receiver was no node). `ln_ids_named` is front-parse's.
fn_index.kai   as posted, EXCEPT: `fnx_qualified(fx, home, name)` / `fnx_kai_qualified(fx, home, name)` STAY as TEMPORARY entry points (= `fnx_of_sym(fx, fnid_decl(sym_derive(NCFn, name, home)))`)
               so a file not rewritten yet still resolves its names. Base callers: emit_c.kai 2754 / 7207, kir_lower.kai 462 / 523, perceus.kai 758. LEAD: delete the two with their last caller.
self_call.kai  as posted, EXCEPT: `self_ref_name(me) : String` STAYS as a TEMPORARY entry point answering "" (no local shadows an id). Base callers: emit_c.kai (tcrec, 6), perceus.kai 3661. LEAD: delete it with its last caller.
tailfuse.kai   tailfuse_decls  : (decls, target, pids) : [Decl]  =>  (decls: [Decl], tab: SymTab, target: String) : TailFused = { decls: [Decl], rows: [NEnt] }
      -- NO `pids` (the driver assumed `(decls, tab, target, pids)`: drop the last argument). A fused entry is a NEW declaration `FnId(nent_sym(row), spec_none())` with
      -- `row = NEnt(NCFn, "__kai_fused_<lead's name>", <lead's home, the target for a root fn>, false)`: the driver extends its table with `.rows` (`typed_tab`). Its parameters are
      -- `binder_minted("__kai_fuse_tag")`, `binder_minted("__kai_fp<i>")`; `tailfuse_name`, `tailfuse_tag_param` keep their signatures (mint keys / emitted text).
tailgraph.kai  TgNode  : TgN(String, Option[String], Int, SymId)  =>  TgN(Option[String], Int, SymId);  tgnode_name  =>  REMOVED;  tgnode_mo / tgnode_arity / tgnode_sym, tailgraph_build(decls), tailgraph_groups, tailgraph_nodes : kept
      -- an edge is a tail call whose callee `callee_id` names a node; a call through a local is none (no scope list any more)
tailids.kai    tfid_index / tfid_lookup  =>  REMOVED (`ParamIds` is not read);  tfid_rename(body, ids: [LocalId], to: [LocalId]);  tfid_all_renamable(ms: [Decl]);  tfid_onto_slots(members, slots, slot_id: (Int) -> LocalId)
tailsig.kai    signatures kept; two signatures agree by id (`ref_sym`), a scalar by its spelling (`scalar_ty_named`)
trmc_exit_drops.kai  txd_plant  : (e, drops)  =>  (ns: LocalNames, e: Expr, drops: [Stmt]) : Expr;  txd_plant_exit  : (e, after)  =>  (ns: LocalNames, e: Expr, after: [Stmt]) : Expr;  txd_wrap_drops(e) kept
      -- emit_c.kai part C (base 13359 / 13895): pass the enclosing declaration's `ln_of_decl(d)`. A drop names its binder by id, and a `#{…}` span among a step's operands reads it by
      -- spelling: `ns` gives perceus' `pcs_mentions(Binder { id, name: ln_text(ns, id) }, operand)` the name.
unique_binders.kai  unique_binders_decls(ds, tab)  -- as posted. PRECISELY: a binder whose NAME is bound more than once in the function is respelled `name__LINE_COL` at every binding site but a
      -- parameter's (as before); a binder that was respelled, or whose ID is bound more than once, is renumbered `local_minted(<its spelling after the pass>)`; every `ELocal` / `EOp.via` read
      -- follows its binder, a read inside a `#{…}` span is respelled in the span's text. An `EName` outside a span is left as it is (it names no local). It calls handler_alias.kai
      -- `handle_live_aliases(init_opt, <the spellings in scope>)` and `canonical_continuation()` with their base signatures.
ctor_stamp.kai  ctor_stamp_decls(tab, target, ds)  -- signature kept. An `EName(n)` / `EModCall(m, n)` left written becomes `ESym(<constructor id>, spec_none())` when the sum its type names (or the module `m`)
      -- declares a constructor `n`; a site no declaration answers stays written. Patterns are not touched (the typer and `tag_tycon_modules` stamp them).
ctor_ix.kai    ctor_ix_build(variants, decls, tab, root)  -- as posted. A row is keyed `sym_derive(NCCtor, name, home)` from `EV(home, name, …)`; a constructor the table has no row of its own for is found
      -- under the core row of its name (`decl_name(tab, c)`).
live_fns.kai   live_decls(decls, ids: ProtoIds, entry)  -- as posted: an impl method (`pid_impl(ids, s)` = `PIR(proto, _, op, …)`) is reached from the dispatcher `pid_op_disp(ids, proto, written_text(op))`
ctor_dump.kai, core_builtins.kai, core_effects.kai, fwd_inline.kai, marks.kai, tailslots.kai, fn_csym.kai  -- signatures kept
```
What CANNOT stay as it was in the monomorphiser (the key changed from a name to the generic's id; the algorithm — worklist order, `ts_add` numbering, spec order by emitted name — is the same):
- typer tuples: EVERY `RCS(s, …)` whose `s` is a generic seeds a tuple. The base skipped a name two modules share (it could not tell the callee) and left those to the AST walk; and for a name the
  root also declares it seeded the ROOT's generic from every call of any homonym. So for generics that share a name across modules the discovery order — hence the instance index, and the order of
  two specs whose emitted names are equal — can differ, and the root's spurious spec is gone.
- `inst_spec_tys` erases the free slots of a `handle`-instance generic by its id; the base did so only when exactly one generic had that NAME.
- a `TyCon` in a mangled name is `decl_name(tab, s)`. A type of a non-root, non-core module whose name another module also declares was home-spelled by the type scope pass (`Item__da`); no
  declaration is respelled any more, so the mangler adds the home itself (`hs_spell(name, home)`, when `sym_contested(tab, s)`) — otherwise two specs would share a C symbol. NEEDS A LOOK by the
  lead: emit_c part C's `mangle_ty_in(tab, t)` and any other emitted name built from a type name need the same rule.
(agent mono)

## checks API — FINAL (agent checks; completes the PRELIMINARY `## checks API` block above, whose two lines stand)
These analyses read the resolved / typed tree: a local is `ELocal(l)`, a declaration `ESym(s, inst)`, a perform's callee `EOp(r)`. Sets of locals are `[LocalId]`
(the two that reach an interpolation span keep the `Binder`: borrow_infer's params for perceus `pcs_mentions`, rawsafe's cons head for fnreg's span scanner).
Signatures that CHANGED (callers in other batches):
```
cont_crossing.kai   cont_crossings  : (e, res, holds, needs)  =>  (e: Expr, tab: SymTab, res, holds, needs)   -- infer.kai part D `check_cont_crossings`: pass `st.env.tab` (the message names the handle's effect)
core_reach.kai      core_reach  : (decls, core_mods, reg)  =>  (decls: [Decl], tab: SymTab, core_mods: [String], reg: ProtocolReg) : CoreCut / Console
      -- DRIVER (driver.kai ~5720; it listed this call as base): pass the table the resolver got (`rd.tab`: it must hold `lowered.rows`). A name the resolver left to the
      -- typer (`EName`, `EModCall`, a field access `x.f`, a pipe's `map` / `filter` / …) reaches every core function `st_by_name(tab, name)` holds; an `ESym(s, _)` reaches `s` alone.
core_reach.kai      CoreCut  : CoreCut([Decl], [String])  =>  CoreCut([Decl], [SymId])   -- the dropped core functions by id (the driver's `CoreCut(all_decls, [])` and `list_length(d)` stand)
core_reach.kai      core_cut_misses  : (decls, dropped: [String]) : [String]  =>  (decls: [Decl], tab: SymTab, dropped: [SymId]) : [String]   -- driver.kai ~5919; still returns the NAMES it prints
core_reach.kai      cr_decl_exprs(d) : [Expr]  -- unchanged (live_fns.kai)
evidence.kai        dump_evidence_obligations(file, decls, tab: SymTab, entry)   -- as the driver assumed
evidence.kai        OblFrame : OF(String, Option[String])  =>  OF(SymId, Option[Binder]);  Obligation : Obl(String, String, Supplier)  =>  Obl(OblFrame, Written, LocalId, Supplier)
      -- (instance, op label, the receiver local of a `ViaRecv` perform or `local_none()`, supplier);  Supplier.SupLexical(String)  =>  SupLexical(OblFrame)
evidence.kai        obl_default_ops(decls, eff: SymId) : [String];  obl_walk(decls, entry, e, stack, acc), obl_is_collision(o, decls), obl_join : signatures kept;  NEW obl_frame_same(a: OblFrame, b: OblFrame) : Bool
evidence_scan.kai   obl_eff_backed(decls, eff: SymId), obl_eff_handled_anywhere(all, ds, eff: SymId) (as emit-sat calls it), obl_eff_instance_count(decls, eff: SymId)
evidence_inject.kai check_op_collisions(file, ors, decls, tab: SymTab), dump_evidence_arity(file, decls, tab: SymTab, entry)   -- as the driver assumed
evidence_inject.kai inject_evidence_params(decls, entry), reindex_evidence_params(meta, mono_decls) : signatures kept;  FnEvidenceParam : FEP(String, String, Supplier)  =>  FEP(OblFrame, Written, Supplier);
                    FnEvidenceParams : FEPs(String, …)  =>  FEPs(FnId, …)  (the declaration; after reindex the specialisation);  evi_demangle_base  =>  REMOVED (a specialisation is its template's id; only core_reach.kai called it)
evidence_frame.kai  build_evidence_frames  : (decls, entry)  =>  (decls: [Decl], tab: SymTab, specs: SpecNames, entry: SymId) : [FnEvidenceFrame]
      -- CORRECTION to emit_c-B's "base signature" line: the frame table stays KEYED BY EMITTED SYMBOL (`FEF(String, [String])`, `frames_index`, `frame_slot_index(frames, fsym: String, key: String)`,
      -- `frame_slots_of(frames, fsym: String)`, evidence_shadow.kai `frame_shadow` : all unchanged), but the symbol is spelled from mono's `fn_emit_name(tab, specs, fid)`.
      -- Callers: emit_c.kai ~9029 (part B/C), the two program-entry contexts ~11019 / ~11191 (part C: `emit_program`'s `tab`, `specs`), emit_native.kai:88 (`kp.tab` + the SpecNames lower_to_kir got: KProgram must carry them or the driver pass them).
evidence_frame.kai  frame_effect_is_carried(s, decls) : unchanged
lint.kai            run_lint / run_lint_json / lint_program : signatures kept (they already took the table)
lint_idiom.kai      idiom_findings  : (e)  =>  (e: Expr, tab: SymTab);  NEW pub fn lint_core_ctor(name: String) : SymId   (`Some` / `None` / `Ok` / `Err`: the one-liner infer.kai part A keeps private as `core_ctor_sym`; core_syms.kai is the natural home)
lint_deadcode.kai   deadcode_declared_fns(decls) : [SymId];  deadcode_findings(decls, tab: SymTab, declared: [SymId], root);  deadcode_file_findings(owned, all, tab: SymTab, declared: [SymId], df, root)
lint_effects.kai    effect_findings  : (decls, root_mod)  =>  (decls: [Decl], tab: SymTab, root_mod: String)
mutation_carriers.kai / mutation_callees.kai   EXACTLY as infer-C posted (`CS(SymId, [TypeExpr])`, `sum_shape(s: SymId, vs)`, `record_shape(r)`, `mutating_type_names(shapes) : [SymId]`, `ty_may_carry_mutation(t, dmut: [SymId])`,
                    `writes_only_first_arg(tab: SymTab, s: SymId)`, `opaque_binders(params, body) : [LocalId]`, `callee_is_opaque(callee, opaque: [LocalId])`, `fn_value_is_transparent(e, opaque: [LocalId])`, `pipe_as_call` unchanged);
                    also `mutating_record_type_names(recs) : [SymId]`, `ty_carries_mutation(t, dmut: [SymId])`, `is_bytes_writer(s: SymId)`; `derived_writer_prefix()`, `ty_is_mutable_ref(t)` unchanged
rawsafe.kai         dump_rawsafe / dump_borrowsafe (file, decls, tab: SymTab)   -- as the driver assumed;  RSMark : RSM(String, …)  =>  RSM(FnId, …), rsmark_fn(m) : FnId;  bs_head_borrow_safe(pat, body) : Option[Binder]
spawn_row.kai       check_spawn_rows  : (df, recs, mod_decls, all_decls)  =>  (df: DiagFiles, tab: SymTab, recs: [RecInfo], mod_decls: [Decl], all_decls: [Decl])   -- infer.kai part D ~19561: `tab` right after `df`, like part A's check_field_privacy
spawn_capture.kai   check_thunk_captures  : (file, recs, thunk)  =>  (site: SpawnSite, recs: [RecInfo], thunk: Expr);  NEW pub type SpawnSite = { file: String, tab: SymTab, owner: Decl }  (only spawn_row.kai calls it)
spawn_capture.kai   callee_is_spawn(callee) : unchanged;  the private `type Capture` is renamed `Carried` (emit_c-B's note: emit_shared's pub `Capture`)
```
What these files CALL in other batches, each as its owner posted it (so standalone the checks branch halts the check on the two names other batches ADD, `fn_emit_name` and `ln_with_binder`, like ownership's):
`fn_emit_name(tab, specs, f) : Written`, `scan_uses_in_interp(ns, span, is_triple, in_lam)` (mono) · `ln_with_binder(ns, b)` (front-parse) · `Use = U(LocalId, …)`, `inf_ty_to_string(tab, t)`,
`inf_tycon_display_ow(tab, owners, s)` (infer-A) · `OPR(FnId, Written, [SymId], SymId, Option[LocalId], Bool, Int, Int)` (infer-B) · `spelled_head_owners(ds, tab, acc)` (infer-C) · `infer_program(file, decls, tab, verbose)` (infer-D) ·
`pat_binders(pat, acc: [Binder]) : [Binder]` (front-load-A) · `cs_pat_binders(p, acc: [LocalId])`, `op_protocols_fold(ix, op, acc, f: (a, SymId) -> a)` (protos-sat) · `row_label_keys(tab, d, ls)` (typer-sat) ·
`check_call_site_refinements(file, decls, tab)` (front-resolve) · `pcs_mentions(b: Binder, e)` (perceus) · `Label.cell : LocalId` (infer-B's request to the lead: spawn_row.kai reads `l.cell` where it read the `@` of `l.eff`).
BASE signatures, called unchanged: `callee_id` (call_site_target), `known_fn_of` / `known_fn_starts_fiber` (core_fns), `builtin_of` (core_builtins), `builtin_sym` (core_syms), `sym_is_ctor` (ctor_ix), `is_self_callee` (self_call),
`ppd_has_self_call` (perceus_plant_drop), `proto_dispatch_marker_in` / `proto_dispatch_parts` / `PDP(String, String)` (emit_shared), `user_state_aliases()` (handler_alias), `st_by_name` (symtab), `is_entry` / `is_file_entry` (entry),
`ProtoDisp(protocol, dispatcher)` / `PIR(…)` rows of `reg.ids.shared` / `reg.impls` (ast.kai).
(agent checks)

## typer-sat — FINAL (agent typer-sat; as committed on branch agent-a6a4d3b09468f5e10; where a line here differs from the PRELIMINARY `## typer-sat API` section above, this one stands)
```
type_text.kai  NEW  ref_display(tab: SymTab, r: DeclRef) : String     -- how a reference PRINTS: decl_name(tab, s) once settled, `n` / `q.n` as written before. (The PRELIMINARY block called it `decl_ref_text`; that name is emit_c part B's private one.)
type_text.kai  NEW  ref_decl_name(tab: SymTab, r: DeclRef) : String   -- the bare NAME of what a reference names: decl_name(tab, s) once settled, the written name WITHOUT its qualifier before (what the name slot held at the base)
type_text.kai  type_text(tab, t), type_key(tab, t), row_text(tab, r), row_label_text(tab, l); unit_text(u), unit_key(u) unchanged; type_text_with REMOVED (protos.kai:860 `proto_type_text` is its one caller)
exhaust.kai / exhaust_head.kai  AS INFER PART D ASKED, not as the PRELIMINARY block said: XSib(SymId, String), XCtors((SymId) -> Option[[XSib]]), XKVariant(SymId). There is no `place` / `name` field:
      a pattern reference still written heads its row with `XOdd`; a witness spells a variant key with its own `XSib` spelling up to the `(`.
      exhaust_head.kai  xh_spell  : (k, names, ws)  =>  (k: XKey, cn: String, names: [String], ws: [String])   -- called only by exhaust.kai
union_table.kai  ut_component_named  =>  REMOVED (part D no longer calls it; the PRELIMINARY line that gave it a `tab` is void)
tycon_pat.kai  TPay = TPV(SymId ctor, [String], [TypeExpr], Option[String]) | TPR(SymId type, [String], [FieldDecl], Option[String])   -- one id per entry (the PRELIMINARY line showed two for TPV)
type_home.kai  type_home_entry(hs, r: DeclRef, cur): `DrSym` by id; `DrWritten(None, n)` by the reader's rule; `DrWritten(Some(_), _)` -> None (a qualified name had no entry at the base either)
type_home.kai  TyHomeTab = { tab: SymTab, by_sym: NameIndex[TyHome] }; type_home_table(ds, tab)   -- emit_shared.kai calls it with the base's one argument: add `tab`
tycon_ids_dump.kai  dump_tycon_ids(decls: [Decl], tab: SymTab)   -- TWO arguments. The driver block wrote `(decls, tab, target)`: drop `target` at the call
infer_holes_json.kai  reads `RRH(nm_opt, line, col, fn: SymId, row, body_labels: [SymId])` as infer part D declares it (the PRELIMINARY block said "as at the base")
infer_dump.kai / infer_holes_json.kai  CORRECTION to the driver block, which lists these among "base signatures": they take the table —
      dump_holes(file, tp, tab, hidden, full), dump_holes_json(file, tp, tab, doc_items, hidden, full), dump_effects_json(df, tp, tab), dump_effect_holes_json(file, tp, tab), strict_holes_check(file, decls, tab)
infer_type_collisions.kai  CORRECTION to the driver block: validate_constructor_uniqueness_decls(decls, tab, df) takes the table too
infer_unit_validate.kai  validate_unit_refs_decls  : (decls, scope, df)  =>  (decls: [Decl], tab: SymTab, scope: UnitScope, df: DiagFiles)   -- driver.kai 5259, 5359 (NOT in the PRELIMINARY block): a dimensioned type's
      head is named through `ref_decl_name`, so the habitant-slot check still finds its type once the head is a `DrSym`
infer_unit_habitant.kai  validate_habitant_slots  : (base, u, scope, …)  =>  (tab: SymTab, base: TypeExpr, u: UnitExprT, scope: UnitScope, file, line, col)   -- called only by infer_unit_validate.kai
infer_shape_validate.kai  validate_shape_refs_decls(decls, df) unchanged (a shape variable is a written name: `ref_spelled`)
effect_instance.kai  row_label_keys(tab, d, ls): a label's effect is spelled `ref_decl_name(tab, r)` — the declared name. TO REWIRE AT MERGE with emit_c part C's `eff_rt_name` (see `## effect run-time spelling`):
      evidence_frame.kai's caller must get the same spelling the perform site keys with, or a contested effect's label key and its instance key disagree
ffi_v2.kai  collect_extern_types(ds, tab), ffi_kind_of_ty_reg(t, reg, tab), ffi_ret_kind_of_reg(opt, reg, tab), extern_field_c_type(fd, reg, tab); calls emit-sat's `ffi_kind_of_ty(t, tab)`
field_privacy.kai (nobody's batch; one line changed here)  field_hidden(v, rec_name, rec, fd) unchanged; it calls `tsh_reader_owns(v, rec)`
value_scope_table.kai / tycon_scheme_home.kai / tycon_pat.kai  import compiler.symtab_build for front-parse's `nt_collect(decls, tab, target)`
NEEDED from others, exactly as they posted (this batch type-checks against them): infer part A `env_key_text(tab, k)`, `inf_ty_to_string(tab, t)`, `scheme_to_string(tab, sc)`, `collect_top_type_names(ds, tab)`, `TyEntry.key`;
      infer part D `infer_program(file, decls, tab, verbose)`, `synth_candidates(tab, scope, expected, fresh, owner, hr, decls)`, `RRH(_, _, _, SymId, Row, [SymId])`; mono `scan_uses_expr(ns: LocalNames, e, in_lam)`;
      emit-sat `variant_ctor_excess(decls, tab)`, `ffi_kind_of_ty(t, tab)`; front-parse `nt_collect(decls, tab, target)`, `ln_of_decl`, `ln_text`, `ln_with_binder`, `binder_minted`; tools-B `ival_of_decl(d)`
```
(agent typer-sat)

## infer part D API, part 2 (agent infer-D) — what changed since the first note, and what part D needs from others
EFFECT ROWS (cross-file, the one change of this note that reaches other files' signatures):
```
pub fn infer_program(file, decls, tab, verbose)                      : TypedProgram      / Console + File + Mutable     -- was / Console + File
pub fn infer_program_with_protos_cached(file, df, decls, verbose, protos, head_owners, kind_theories, tab, views) : TypedProgram / Console + File + Mutable
pub fn typecheck_module(file, df, mod, inherited, all_program_decls, protos, head_owners, kind_theories, tab, sh, verbose) : TypecheckedModule / Console + File + Mutable
      -- WHY: `TyEnv` now holds the symbol table (part A's `env.tab`), and a `SymTab` is arrays. The typer's body walk is `/ Mutable` (unification), so the
      --   masking rule no longer sees "no argument carries mutation" at `infer_all_loop -> infer_decl(…, env, …)`: the demand is the caller's. Nothing writes the table.
      -- WHO: a caller that holds the table as a PARAMETER or a FIELD READ adds `+ Mutable` to its own row:
      --   typed_cut.kai tcut_module / tcut_infer_publish / tcut_loop / tcut_typecheck_program / tcut_infer_program;  lint.kai:316;  infer_dump.kai (the 5 dump_* that call infer_program);
      --   infer_holes_json.kai strict_holes_check;  evidence.kai:449;  evidence_inject.kai:220;  rawsafe.kai 793, 806;  emit_c.kai 12906, 12933;  driver.kai compile_to_module (4079).
      --   A caller that binds the table in a local `let tab = st_…(…)` and passes that local keeps its row: the rule sees a locally built value
      --   (driver.kai does exactly this today for `tcrec_rewrite_decls(…, sym_tab) / Mutable` inside `compile_source / Console + File`).
pub fn tc_shared(…) / tc_shared_extend(…) / synth_candidates(tab, …)  -- rows unchanged (synth_candidates keeps the table out of its search context for this reason)
```
IMPORT needed in infer.kai's import block (part A's lines): `import compiler.core_fns`  -- part D asks `known_fn_of(f)` for the fiber-producing stdlib functions (it was a list of names)
Private names of other parts that part D calls (their owners posted them; listed so a rename is not silent):
```
part A  env_bind_local(env, b, sc), env_ctors(env, sum), env_key_sym / env_key_text / env_key_ord, env_ctor_names(env), ty_env_empty(), ty_env_prepend_batch(batch, entries, ix, vx), row_label(eff, ty_args),
        ref_text(tab, r), ref_name(r), core_ctor_sym(name), inf_ty_to_string(tab, t), inf_tycon_display_ow(tab, owners, s), decl_is_entry(hs, mo, fid), union_component_names(tab, unions, parent),
        build_ty_env_inherited(seed, inh, inherited, decls, unions, top_names, rk_table, tab, homes), collect_union_rows(ds, tab, hv), collect_unions(ds, tab, hv), collect_top_type_names(ds, tab),
        collect_constructors(ds, homes, acc), check_field_privacy(df, tab, view, decls, recs), check_default_coverage(df, tab, decls, all_decls, homes)
part B  name_read_sym(st, nm), modcall_sym(st, m, f), sum_find(ss, s), rec_pick(rs, decl), rec_find(rs, decl), label_is_aliased_cell(l: Label), rigid_tparams(binds, declared, owner: String), st_set_refine_ctx(st, ctx),
        CS(SymId, …), OPR(FnId, …), SI(SymId, tps, variants, mo), RI(SymId, tps, fields, mo)
part C  st_local_names(st, e) : LocalNames   -- `fmt_expr_text(f, st.env.tab, st_local_names(st, f))` in reject_predicate_effect (tools-A's 3-argument fmt_expr_text)
others  tycon_ref.kai tsh_tycon_at(v, r, args) (3 arguments, as the typer satellites wrote it): a record pattern's and a written type's reference is read through it, so a written QUALIFIER decides
        (part A's `te_ref_sym(hv, r)` drops the qualifier of a `DrWritten(Some(q), n)`; part D does not call it)
        exhaust.kai XSib(SymId, String) / XCtors((SymId) -> Option[[XSib]]) / exhaust_head.kai XKVariant(SymId): part D is written against the one-function `XCtors` the files hold today, NOT the three-field record
        of the later "exhaust.kai / exhaust_head.kai" note; resolve.kai collect_effect_ids(decls) : [SymId]; effect_alias.kai collect_alias_cands(decls, tab, df); core_fns.kai known_fn_of(s)
```
Corrections to the first part D note:
```
fn sum_ctor_names(env, tsym) : [String]   -- KEPT (private; feeds `variants_of_type`), not removed
fn variants_of_type(env, tsym) : [String] -- display names only: a match's needs are computed by id (`match_needs`), nothing decides by these strings
type SumVar = SumVar(SymId, SymId)        -- private: a variant's constructor and the type its name denotes (a declared type, else the constructor itself — the union row's component)
fn report_ctor_parents_if_ambiguous(st, cname: String, parents: [String], line, col)   -- private; takes the parents its caller already listed
print_row_ids: the verbose `row <fn> <Eff>#<id>` line prints the effect's declared name; the `@<cell>` suffix an aliased label's text carried is gone with the text
```
(agent infer-D)

## emit_c part C — FINAL (agent emit_c-C; commit 7edd11c4 on branch agent-aa80166557f1d7c80; where a line here differs from `## effect run-time spelling` / `## emit_c part C … (PRELIMINARY)` above, this one stands)
The commit changes only part C of `emit_c.kai` (`fn emit_nullary_enum_seed` .. end of file), on the lane base baf56156. Alone it stops at name resolution (it needs part A's import lines and
`EmitCtx` fields, and names other batches add). Checked on a scratch state — base parts A + B, part A's three `EmitCtx` fields / imports, and every signature below stood in as its owner posted it:
part C goes 181 errors -> 1, the `EOp` clash (LEAD NOTE): `desugar_op_calls_callee` builds `EOp(OpRef { eff: s, op: written(op_name), via: ViaAmbient })` once.
The gate (`tools/symid-census/gate.py`) reports two things for part C, both ledger rows for the lead: `eff_rt_name() acc/hs_spell` (the emitted spelling) and emit_c.kai as a `written_text` reader.
Everything in the two sections above stands as posted, EXCEPT:
```
emit_c.kai (C)  effect_has_default_block  : (eff_name: String, decls)  =>  (rt: String, decls: [Decl], lab: (SymId) -> String) : Bool
      -- NOT the `(eff: SymId, decls)` first posted: part A's perform site asks its own `effect_declares_default(cx.effs, r.eff)` by id, and the ONE caller left, part A's
      -- `evf_thunk_slot(key, effs)`, holds a run-time name (`inst_key_eff(key)` of an evidence-frame slot key). The base signature cannot be kept: a `DEffect` carries an id, so the
      -- name has to be spelled, and `lab` is that spelling (`eff_rt_name`). MERGE: `evf_thunk_slot` / `emit_evf_thunk_frame` take `lab`; `evf_terminal_slot(key, cx)` passes
      -- `(s) => eff_label(cx, s)`, part B's thunk-forward call (~8392, `tab` at hand, needs the root) `(s) => eff_rt_name(tab, root, s)`. Dropped the day frames carry effect ids.
emit_c.kai (C)  emit_trmc_step(src: String, s: TrmcStep, args_e: [Expr], cx: EmitCtx)   -- STANDS. Part A's FINAL calls the base form: its one site becomes
      -- `EMark(MkTrmcStep(s)) -> emit_trmc_step(src, s, args_e, cx)` (the variant step reads `s.ctor`: `ctor_ix_row(cx.ctor_ix, s.ctor)` for the tag and the slot kinds, `sym_same` for the donor test).
emit_c.kai (C)  mangle_ty_in(tab: SymTab, t: Ty)   -- a `TyCon` is `decl_name(tab, s)`: it does NOT apply mono's contested rule (`hs_spell(name, home)` for a non-root, non-core contested type), which
      -- needs the root and the core list. Only `--dump-mono-out` reads it (`mangle_name(tab, name, tys, units)`): for such a type the dump prints the bare name where the spec's symbol has the home.
      -- When the rule gets its one shared function, `dump_mono_out` needs (target, cores) and this arm calls it.
```
What part C CALLS, as its owner posted it (the stand-ins of the check):
```
resolve.kai (front-resolve)        collect_effect_ids(decls) : [SymId]
fnreg.kai (mono)                   register_fn_decls(decls, tab, specs, entry), fn_emit_name(tab, specs, fid) : Written   (the goto label of a rewritten fn: `c_sym(written_text(fn_emit_name(…)), mo)`)
self_call.kai / trmc_self_call.kai SelfRef(fid); tcrec_callee_is_self, tcrec_is_direct_self_call, tcrec_expr_has_self_call, tcrec_ctor_modcons_slot unchanged. `self_ref_name`: part C has NO caller left (its six are deleted).
ctor_ix.kai (mono)                 ctor_ix_build(variants, decls, tab, root), ctor_ix_row(ix, s), ctor_row_tag(row), sym_is_ctor(tab, s)
trmc_exit_drops.kai (mono)         txd_plant(ns, e, drops), txd_plant_exit(ns, e, after) with `ln_of_decl(d)` of the rewritten declaration; txd_wrap_drops(e)
emit_shared.kai (emit-sat)         register_variants(decls, tab), lc_new(tab, specs), collect_decls, evar_kinds(v), pat_bindings(pat, acc: [Binder]), the `[SymId]` effect rows (main_row_labels,
                                   ensure_unconditional_defaults, add_missing_effects, default_effects_absorbed_by_handler, runner_entry_row), find_effect_default_block / default_block_owner_mo /
                                   effect_default_block_all_extern (eff: SymId, decls), ty_to_string(tab, t), ty_string_list(tab, ts)
emit_tcrec_live.kai (emit-sat)     tcl_goto_drops(cx.locals, pend, args), tcl_without_tok(pend)
emit_reuse_donate.kai (emit-sat)   rd_scan_capable(decls, tab, specs)
perceus*.kai (perceus)             pcs_use_index(body, scope: [Binder]), pcs_explicit_borrow_names(params) : [Binder], pcs_list_union / pcs_branch_aware_skip_params over `[Binder]`,
                                   PcsParamSets { moved: [Binder], forced: [Binder] }, pcs_param_payer(l: LocalId, uses, sets), pcs_has(bs: [Binder], l), pcs_callee_key(f), pcs_borrow_lookup, pcs_planned_borrow_map,
                                   ret_slot_id_is_fn(l), ret_slot_id_is_scope(l), ret_slot_binds_tail_arg(b: Binder), ret_slot_binder(RsFn), ret_slot_local(RsFn)
evidence_frame.kai (checks)        build_evidence_frames(decls, tab, specs, entry)   (the two program-entry contexts)
purity.kai (ownership)             purity_const_syms(marks, tab, specs)
ffi_v2.kai / type_text.kai (typer-sat)  collect_extern_types(ds, tab), type_key(tab, te), te_decl_ids(te)
infer.kai (D)                      infer_program(file, decls, tab, verbose), RCS(SymId, …)
emit_c.kai (A)                     EmitCtx.tab / .specs / .locals, ReuseDonor(SymId, [LocalId]), c_local_of(cx, l), mono_sum_decls(decls, tab, ctors)
emit_c.kai (B)                     emit_proto_runtime_tables(decls, tab, specs, variants, fns, ids, ctors, entry), emit_lam_helpers(src, lams, tab, specs, …), emit_clause_forwards(cs, effs, tab),
                                   effect_struct_decls(decls, tab), fn_forward_decls(decls, tab, specs, …), thunk_forward_decls(decls, tab, specs), rd_donatable_filter(capable, decls, tab, specs, fx),
                                   decl_ref_text(tab, r) (the `impl` line of `--dump-types`; typer-sat exports one of the same name)
ast.kai (LEAD)                     TrmcStep gains `ctor: SymId` (kir's request): part C FILLS it (`TrmcStep { sym, hole, cname, ctor, dropmask, params }`; `sym_none()` on the two builtin-cons steps) and reads it.
```
Imports of emit_c.kai (part A's block): part C needs `compiler.home_spell` ADDED (`hs_spell`); it still reads `compiler.proto_ids` (`ProtoIds` in the three `emit_program*` signatures), `compiler.core_effects`,
`compiler.ctor_ix`, `compiler.symtab`; it reads nothing of `compiler.name_scope` and no longer calls `sym_read_kind` (`compiler.sym_read`).
Part C defines (do not re-define): `eff_rt_name` (pub), `eff_label`, `default_install_order`, `interp_param_names`, `interp_pat_names`, `row_label_spelled`, `ta_modules`, `ta_names_effect`, `tp_subst_ref`,
`priv_ty_among`, `mangle_ty_in`, `trmc_row_kinds`; private types `TAScope { tab, entries }`, `PrivTys { tab, types }`. REMOVED from part C (no caller anywhere): `emit_default_setups_in_order`,
`emit_default_shims_in_order`, `user_effects_in_row`, `ta_home_of`, `mangled_private_type_name`, `PrivTyEntry`, `tcrec_block_scope`.
The driver's calls into part C, in one list: `emit_program(file, src, decls, tab, specs, ids, bmode, target_origin, regions, stamp_ids)` and the two modular forms (the same three after `raw_decls`);
`tcrec_rewrite_decls(decls, allow_trmc, cons_ok, root, tab, specs)`; `collect_transparent_aliases(decls, tab, eff_names, target_mod_name, core_mod_names(core_mods, []))`; `expand_ta_decls(decls, tab, table, aliases)`;
`expand_aliases_in_decls(decls, tab, aliases)`; `mangle_core_segments(decls, tab, segs, n_core)`; `dump_types(decls, tab)`; `dump_mono(file, decls, tab)`; `dump_mono_out(file, decls, tab)`.
(agent emit_c-C)

## round 2 — agent r2-rest (every file but driver.kai)
emit_ffi_c.kai  emit_ffi_struct_typedefs  : (reg)  =>  (reg: [ExternTypeReg], tab: SymTab)  -- callers emit_c.kai (2), fixed
emit_ffi_c.kai  emit_ffi_v2_shim_body  : (sig, cname, params, ret_ty_opt, reg)  =>  (…, reg, tab: SymTab);  ffi_sig_uses_v2 : (params, ret_ty_opt, reg)  =>  (…, reg, tab: SymTab)  -- caller emit_c.kai emit_fn_body, fixed
emit_shared.kai  build_ctor_int_slots(decls: [Decl], tab: SymTab) : [CtorIntSlots]  -- MOVED here from emit_c.kai, same signature (driver.kai 5654 needs no change: it imports emit_shared). Rows are `CIS(constructor id, flags)`: one per `Var(c, …)` of `decls`, then the builtin rows under `sym_derive(NCCtor, name, home | core_type_home())`
ast.kai  ctor_int_slots_lookup_in / ctor_int_slots_lookup  =>  REMOVED (name-keyed, no caller; the by-id readers are unbox.kai `ctor_int_flags`, unbox_native_raw.kai `nab_flags_of_sym`)
`+ Mutable` added to the row (the typer's row carries it; driver callers must mask it or carry it): typed_cut.kai tcut_infer_program, tcut_typecheck_program; lint.kai run_lint, run_lint_json; infer_dump.kai dump_infer, dump_typed, dump_typed_core, dump_last_use, dump_intervals; infer_holes_json.kai strict_holes_check; evidence.kai dump_evidence_obligations; evidence_inject.kai dump_evidence_arity; rawsafe.kai dump_borrowsafe, dump_rawsafe; emit_c.kai dump_mono, dump_mono_out

## driver — round 2 (agent r2-driver): driver.kai against the signatures that landed; no `pub` signature changed
THE TABLE CHAIN in `compile_source` is now (each link is `st_extend` of the one before; all built in local `let`s, so `compile_source` keeps `/ Console + File`):
`tab0` (core + import + root parser rows) → `tab1` (+ `qualtype_decls(..).rows` ++ `qualtype_segmented(..).rows`) → `tab2` (+ `synthesize_refine_pred_fns(..).rows`; the narrow lowering reads it)
→ `tab3` (+ `lower_extern_opaques(..).rows`; kinds, alias passes, desugars, `lower_protocols`' `PScope`) → `sym_tab` (+ `lowered.rows`; the resolver) → `typed_tab` (+ tail fusion) → `emit_tab` (+ `mf.rows`).
`rd.tab` is no longer read: the resolver hands back the table it was given, and a field read is never local to the `Mutable` masking. Every pass after the resolver takes `sym_tab` / `typed_tab` / `emit_tab`.
`specs` handed to perceus / tcrec / the emitters / the dumps is `spec_names_of(spec_names_rows(mf.specs))` (same reason: `mf.specs` is a field read; `spec_names_of` is the only constructor, so the index is the same one).
`ids: ProtoIds` handed to `lower_to_kir` ×3, `emit_program`, `emit_program_modular[_cached]` is `proto_reg.ids` AFTER `reg_with_target_ids` (the registry `mono_front` got).
driver.kai private: `emit_modular_maybe_cached(.., specs, ids: ProtoIds, bmode, ..)`; `compile_to_module`, `dump_library_mode` : `/ Console + File + Mutable` (they take the table as a parameter and run `infer_program`).
Checked against the rows round 2 is to land (`+ Mutable` on tcut_infer_program, run_lint, run_lint_json, dump_infer, dump_typed, dump_typed_core, dump_last_use, dump_intervals, strict_holes_check,
dump_evidence_obligations, dump_evidence_arity, dump_rawsafe, dump_borrowsafe, dump_mono, dump_mono_out): with those rows applied on a scratch copy the driver still shows 0 errors, `compile_source` unchanged.
(agent r2-driver)
### round 3 — typer2 (UFCS on a local; mutation carriers through user types)
mutation_carriers.kai  CarrierShape  : CS(SymId, [TypeExpr])  =>  CS(SymId, [SymId], Bool)  -- the declarations the stored types name (each written reference read from the module declaring the type) and whether a stored closure's row names `Mutable`
mutation_carriers.kai  record_shape  : (r: RecInfo)  =>  (v: TyHomeView, r: RecInfo);  sum_shape : (s: SymId, vs: [Variant])  =>  (v: TyHomeView, s: SymId, tps: [String], vs: [Variant], mo: Option[String]);  mutating_record_type_names : (recs)  =>  (v: TyHomeView, recs: [RecInfo])  -- `v` is any view of the type-home table; each shape re-aims it at its own module
infer.kai  TyEnv  +carriers: [SymId] (the program's carrier types, attached by `typecheck_module`; `mask_local_mutable_demand` reads it);  TcShared  +carriers, +rec_carriers: [SymId] (built once in `tc_shared`)
spawn_row.kai  check_spawn_rows  : (df, tab, recs: [RecInfo], mod_decls, all_decls)  =>  (df, tab, carriers: [SymId], mod_decls, all_decls);  spawn_capture.kai check_thunk_captures : (site, recs, thunk)  =>  (site, carriers: [SymId], thunk)  -- the caller passes `sh.rec_carriers`

## mainmerge (merge of origin/main 737ff08a into the lane)
core_builtins.kai  CoreBuiltin  : +CbArrayTake (id 1985417026627175561 = builtin_sym("array_take"))  -- slot_take
core_prims.kai  core_minted_names  : (new) () : [String]  -- core_prim_ix (replaces main's resolve.core_emit_names)
cell_scan.kai  cp_getset_hit / CpHit  : moved here from cell_promote, pub: cp_getset_hit(slot: Binder, callee: Expr, args_: [Expr]) : CpHit  -- slot_take, cell_promote
slot_take.kai  take_slot_reads  : (nm: String, …)  =>  (slot: Binder, stmts: [Stmt], tail: Option[Expr]) : SlotScope  -- cell_promote
arm_release.kai  ArmTake  : ArmTake([String], [Int], Int)  =>  ArmTake([Binder], [Int], Int)  -- emit_c, kir_lower_walk
arm_release.kai  arm_take_plan  : (pat, vs: [VariantEntry])  =>  (pat: Pattern, ix: CtorIx) : ArmTake  -- emit_c, kir_lower_walk
kir_pat_alias.kai  pat_slot_kinds  : (p, vs: [VariantEntry])  =>  (p: Pattern, ix: CtorIx) : Option[[Int]]  -- arm_release
(not taken from main: closure_spec_capture.kai, closure_spec_ast.CsProg / cs_closure_body, core_prims.core_is_borrow_sibling, resolve.core_emit_names)
## round 4 — span (interpolation bodies are trees from the lift on; commits 0c02fd39..125b8ca5)
emit_c.kai  desugar_interp_decls  : (ds, op_names: [String], global_mt, …)  =>  (ds, global_mt, …)  -- driver; the lift no longer depends on `show` being registered and covers DConst + wrapper decls
parse.kai  parse_interp_expr  : unchanged signature; no longer reads `__perceus_dup(x)` back as a mark (marks_read_spelled removed)
unique_binders.kai  unique_binders_decls  : (ds: [Decl], tab: SymTab) : [Decl] / Console  =>  (ds: [Decl]) : [Decl]  -- driver
fnreg.kai  scan_uses_expr  : (ns: LocalNames, e, in_lam) : [Use] / Console  =>  (e: Expr, in_lam: Bool) : [Use]  -- unbox, emit_shared, infer_dump
fnreg.kai  scan_uses_in_stmts  : (ns: LocalNames, stmts, in_lam) / Console  =>  (stmts: [Stmt], in_lam: Bool) : [Use]  -- emit_shared
fnreg.kai  scan_uses_in_interp  : removed
emit_tcrec_live.kai  tcl_filter / tcl_filter_stmts / tcl_goto_entries / tcl_consumes  : (ns: LocalNames, …)  =>  the same without `ns`  -- emit_c, kir_lower_walk, unwind_reads
emit_tcrec_live.kai  tcl_span_mentions  : removed
unwind_reads.kai  unw_lent / unw_filter / unw_filter_stmt / unw_confined  : (ns: LocalNames, …)  =>  the same without `ns`  -- unwind_walk
unwind_walk.kai  unw_walk  : (ucx, ns: LocalNames, pend, …)  =>  (ucx, pend, bound, banned, params, body, traps)  -- unwind_pend
unwind_pend.kai  unw_fn_frame  : (u, locals: LocalNames, enc, row, seeds, params, body)  =>  (u, enc, row, seeds, params, body)  -- emit_c, kir_lower_fns
unwind_pend.kai  unw_frame  : (u, locals: LocalNames, seeds, params, body)  =>  (u, seeds, params, body)  -- emit_c, kir_lower_fns
emit_shared.kai  fv_interp / fv_interp_parts / collect_interp / collect_interp_parts  : removed
emit_shared.kai  arms_pbind_safe_for_fast_path / block_unused_lets  : `/ Console` dropped
unbox.kai  expr_has_interp_use  : removed
perceus_reads.kai  pcs_interp_ident_count / pcs_interp_spelled_count / pcs_span_token / pcs_span_spells  : removed (pre-lift callers use interp_span.isp_ident_count(name: String, span, is_triple))
perceus_span.kai  : file removed
perceus_body_reads.kai  SlotRead / RoleRead  : the `SRSpan` / `RoleSpan` constructors are gone
interp_span.kai  IdentEdit / isp_rebuild / isp_render  : removed; isp_ident_count added
local_names.kai  ln_ids_named  : removed
marks.kai  mark_of_name / marks_read_spelled  : removed
derive_pick.kai  shaped_gens / ShapedGens / proto_gen  : new — `shaped_gens(ds: [Decl]) : ShapedGens`, the generator each non-core protocol picks by its operations; `proto_gen(sg, p: SymId) : Option[CoreProto]`  -- derive agent (round 3)
derive_pick.kai  derive_gen  : (ps, pspell, from) : Option[CoreProto]  =>  (ps, sg: ShapedGens, pspell, from) : Option[DeriveGen]  -- derive agent
derive_pick.kai  DeriveGen  : DeriveGen(String, CoreProto)  =>  DeriveGen(SymId, CoreProto) (the protocol the entry resolved to; no name)  -- derive agent
derive_pick.kai  derive_gens / derives_gen / derive_pick_diags  : each takes `sg: ShapedGens` right after `ps`; `picks_gen` removed  -- derive agent
derive_pick.kai  derives_core  : new — (ps, names, from, g) : Bool, an entry is the core protocol `g` itself (BinSerialize / Layout / Json sites)  -- derive agent
derive_reach.kai  HeadTree / ImplReach  : HT(String, ..) / IR(String, [Int])  =>  HT(DeclRef, ..) / IR(SymId proto, DeclRef head, [Int]); `reach_key` removed  -- derive agent
derive_reach.kai  impl_reach / derived_reach / missing_path  : keyed by protocol id and settled head; `missing_path(cx: ReachCx, t) : [DeclRef]`; new `ReachCx`, `param_reach`, `reach_params`  -- derive agent
derive_mint.kai  derive_op_ref  : (ids, homed, g: CoreProto, op)  =>  (ids, homed, proto: SymId, op)  -- derive agent
interp_pattern.kai  ipat_arm / ipat_mmarm / ipat_let  : new — an interpolated string pattern becomes a fresh binder plus an equality guard (`let`: a binder nothing reads); called from surface_lower  -- span agent (follow-up)
const_init.kai  const_init_fns / ConstInits  : new — `const_init_fns(decls, tab, target) : ConstInits { decls, rows }`, post-mono: a const initialiser that declares a local or holds a lambda becomes a zero-parameter function `__kai_const_<NAME>` and the const body is the call; `rows` extend the emit table  -- span agent (follow-up)
