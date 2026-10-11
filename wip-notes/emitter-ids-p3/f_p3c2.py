"""f_p3c2.py: step 2 of the KFn change, the KIR passes. Run from stage2/compiler after f_p3c.py."""
import sys, os
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from edk import edit, add_import

REG_OLD = "tab: kp.tab, specs: kp.specs, ids: kp.ids, reuse_tags: kp.reuse_tags }"
REG_NEW = "tab: kp.tab, specs: kp.specs, reg: kp.reg, ids: kp.ids, reuse_tags: kp.reuse_tags }"

# ---------------------------------------------------------------- kir_inline.kai
add_import("kir_inline.kai", "kir", "kir_syms")
edit("kir_inline.kai", [
 ('''    let tmpls = kin_collect(kp.fns, [])
    let fns1  = match tmpls { [] -> kp.fns  _ -> kin_fn_loop(kp.fns, tmpls, []) }
    let lams  = kin_lam_collect(fns1, [])''', '''    let nm    = kp_names(kp)
    let tmpls = kin_collect(kp.fns, nm, [])
    let fns1  = match tmpls { [] -> kp.fns  _ -> kin_fn_loop(kp.fns, nm, tmpls, []) }
    let lams  = kin_lam_collect(fns1, nm, [])'''),
 (REG_OLD, REG_NEW),
 ('''fn kin_collect(fs: [KFn], acc: [KinTmpl]) : [KinTmpl] = match fs {
  []           -> list_reverse(acc)
  [f, ...rest] -> kin_collect(rest, if kin_admit(f) { [KT(f.sym, f), ...acc] } else { acc })
}''', '''fn kin_collect(fs: [KFn], nm: KNames, acc: [KinTmpl]) : [KinTmpl] = match fs {
  []           -> list_reverse(acc)
  [f, ...rest] -> kin_collect(rest, nm, if kin_admit(f) { [KT(kfn_sym(nm, f), f), ...acc] } else { acc })
}'''),
 ('''fn kin_lam_collect(fs: [KFn], acc: [KinTmpl]) : [KinTmpl] = match fs {
  []           -> list_reverse(acc)
  [f, ...rest] -> kin_lam_collect(rest, if kin_lam_admit(f) { [KT(f.sym, f), ...acc] } else { acc })
}''', '''fn kin_lam_collect(fs: [KFn], nm: KNames, acc: [KinTmpl]) : [KinTmpl] = match fs {
  []           -> list_reverse(acc)
  [f, ...rest] -> kin_lam_collect(rest, nm, if kin_lam_admit(f) { [KT(kfn_sym(nm, f), f), ...acc] } else { acc })
}'''),
 ('''fn kin_fn_loop(fs: [KFn], tmpls: [KinTmpl], acc: [KFn]) : [KFn] = match fs {
  []           -> list_reverse(acc)
  [f, ...rest] -> kin_fn_loop(rest, tmpls, [kin_fn(f, tmpls), ...acc])
}

fn kin_fn(f: KFn, tmpls: [KinTmpl]) : KFn = match kin_fn_blocks(f.blocks, tmpls, f.sym, 0, []) {
  KBO(bs, _) -> KFn { sym: f.sym, kind: f.kind, params: f.params, ret: f.ret,
                      blocks: bs, region_id: f.region_id, stateful: f.stateful,
                      mo: f.mo, donor: f.donor, entry: f.entry, fid: f.fid }
}''', '''fn kin_fn_loop(fs: [KFn], nm: KNames, tmpls: [KinTmpl], acc: [KFn]) : [KFn] = match fs {
  []           -> list_reverse(acc)
  [f, ...rest] -> kin_fn_loop(rest, nm, tmpls, [kin_fn(f, kfn_sym(nm, f), tmpls), ...acc])
}

fn kin_fn(f: KFn, sym: String, tmpls: [KinTmpl]) : KFn = match kin_fn_blocks(f.blocks, tmpls, sym, 0, []) {
  KBO(bs, _) -> kfn_with_blocks(f, bs)
}'''),
])
edit("kir_inline_beta.kai", [
 ('''      KBB(bs, _) -> KFn { sym: f.sym, kind: f.kind, params: f.params, ret: f.ret,
                          blocks: kin_dce_closures(bs), region_id: f.region_id,
                          stateful: f.stateful, mo: f.mo, donor: f.donor, entry: f.entry, fid: f.fid }''',
  '''      KBB(bs, _) -> kfn_with_blocks(f, kin_dce_closures(bs))'''),
])

# ---------------------------------------------------------------- kir_varslot.kai
add_import("kir_varslot.kai", "kir", "kir_syms")
edit("kir_varslot.kai", [
 ('''  let canon = if kvs_program_unaliased(kp.fns) { [] } else { kvs_canonical_sites(kp.handlers, kp.fns, []) }''',
  '''  let canon = if kvs_program_unaliased(kp.fns) { [] } else { kvs_canonical_sites(kp.handlers, kvs_clauses(kp), []) }'''),
 (REG_OLD, REG_NEW),
 ('''fn kvs_canonical_sites(hs: [KHandlerDecl], fns: [KFn], acc: [String]) : [String] = match hs {''',
  '''# The clause bodies of the program, each under its symbol: an op thunk
# names its clause by it.
fn kvs_clauses(kp: KProgram) : [KSymFn] =
  ksym_fns(kp_names(kp), filter(kp.fns, (f) => match f.kind { KFnClause -> true  _ -> false }))

fn kvs_canonical_sites(hs: [KHandlerDecl], fns: [KSymFn], acc: [String]) : [String] = match hs {'''),
 ('''fn kvs_ops_canonical(ops: [KOpThunk], fns: [KFn]) : Bool = match ops {''', '''fn kvs_ops_canonical(ops: [KOpThunk], fns: [KSymFn]) : Bool = match ops {'''),
 ('''fn kvs_find_fn(fns: [KFn], sym: String) : Option[KFn] = match fns {
  []           -> None
  [f, ...rest] -> if f.sym == sym { Some(f) } else { kvs_find_fn(rest, sym) }
}''', '''fn kvs_find_fn(fns: [KSymFn], sym: String) : Option[KFn] = match fns {
  []           -> None
  [r, ...rest] -> if r.sym == sym { Some(r.f) } else { kvs_find_fn(rest, sym) }
}'''),
 ('''fn kvs_get_canonical(sym: String, fns: [KFn]) : Bool = match kvs_find_fn(fns, sym) {''', '''fn kvs_get_canonical(sym: String, fns: [KSymFn]) : Bool = match kvs_find_fn(fns, sym) {'''),
 ('''fn kvs_set_canonical(sym: String, fns: [KFn]) : Bool = match kvs_find_fn(fns, sym) {''', '''fn kvs_set_canonical(sym: String, fns: [KSymFn]) : Bool = match kvs_find_fn(fns, sym) {'''),
 ('''    _  -> KFn { sym: f.sym, kind: f.kind, params: f.params, ret: f.ret,
                blocks: kvs_rewrite_cells(f.blocks, cells), region_id: f.region_id,
                stateful: f.stateful, mo: f.mo, donor: f.donor, entry: f.entry, fid: f.fid }''',
  '''    _  -> kfn_with_blocks(f, kvs_rewrite_cells(f.blocks, cells))'''),
])

# ---------------------------------------------------------------- kir_reach.kai
add_import("kir_reach.kai", "kir", "kir_syms")
edit("kir_reach.kai", [
 ('''  let by_name = kreach_index(kp.fns, idx_empty())
  let seeds = list_append(filter(kp.fns, (f) => is_root(f) or f.entry), kreach_resolve_all(by_name, pinned, []))
  let live = kreach_walk(seeds, by_name, kp.handlers, idx_empty())
  KProgram { types: kp.types, fns: filter(kp.fns, (f) => kreach_has(live, f.sym)),''',
  '''  let rows = ksym_fns(kp_names(kp), kp.fns)
  let by_name = kreach_index(rows, idx_empty())
  let seeds = list_append(filter(rows, (r) => is_root(r.f) or r.f.entry), kreach_resolve_all(by_name, pinned, []))
  let live = kreach_walk(seeds, by_name, kp.handlers, idx_empty())
  KProgram { types: kp.types, fns: map(filter(rows, (r) => kreach_has(live, r.sym)), (r) => r.f),'''),
 ('''entry: kp.entry, tab: kp.tab, specs: kp.specs, ids: kp.ids, reuse_tags: kp.reuse_tags }''',
  '''entry: kp.entry, tab: kp.tab, specs: kp.specs, reg: kp.reg, ids: kp.ids, reuse_tags: kp.reuse_tags }'''),
 ('''fn kreach_index(fns: [KFn], ix: NameIndex[KFn]) : NameIndex[KFn] = match fns {
  [] -> ix
  [f, ...rest] -> kreach_index(rest, idx_prepend(idx_prepend(ix, f.sym, f), c_user_thunk_sym(f.sym), f))
}

fn kreach_lookup(ix: NameIndex[KFn], name: String) : Option[KFn] =
  kreach_pick(idx_chain(ix, name), name)

fn kreach_pick(chain: [KFn], name: String) : Option[KFn] = match chain {
  [] -> None
  [f, ...rest] ->
    if f.sym == name or c_user_thunk_sym(f.sym) == name { Some(f) } else { kreach_pick(rest, name) }
}

fn kreach_resolve_all(ix: NameIndex[KFn], names: [String], acc: [KFn]) : [KFn] = match names {''',
  '''fn kreach_index(fns: [KSymFn], ix: NameIndex[KSymFn]) : NameIndex[KSymFn] = match fns {
  [] -> ix
  [r, ...rest] -> kreach_index(rest, idx_prepend(idx_prepend(ix, r.sym, r), c_user_thunk_sym(r.sym), r))
}

fn kreach_lookup(ix: NameIndex[KSymFn], name: String) : Option[KSymFn] =
  kreach_pick(idx_chain(ix, name), name)

fn kreach_pick(chain: [KSymFn], name: String) : Option[KSymFn] = match chain {
  [] -> None
  [r, ...rest] ->
    if r.sym == name or c_user_thunk_sym(r.sym) == name { Some(r) } else { kreach_pick(rest, name) }
}

fn kreach_resolve_all(ix: NameIndex[KSymFn], names: [String], acc: [KSymFn]) : [KSymFn] = match names {'''),
 ('''fn kreach_walk(work: [KFn], ix: NameIndex[KFn], hs: [KHandlerDecl], live: NameIndex[String]) : NameIndex[String] = match work {
  [] -> live
  [f, ...rest] ->
    if kreach_has(live, f.sym) { kreach_walk(rest, ix, hs, live) }
    else {
      let next = kreach_resolve_all(ix, kreach_refs_blocks(f.blocks, hs, []), rest)
      kreach_walk(next, ix, hs, idx_prepend(live, f.sym, f.sym))
    }''', '''fn kreach_walk(work: [KSymFn], ix: NameIndex[KSymFn], hs: [KHandlerDecl], live: NameIndex[String]) : NameIndex[String] = match work {
  [] -> live
  [r, ...rest] ->
    if kreach_has(live, r.sym) { kreach_walk(rest, ix, hs, live) }
    else {
      let next = kreach_resolve_all(ix, kreach_refs_blocks(r.f.blocks, hs, []), rest)
      kreach_walk(next, ix, hs, idx_prepend(live, r.sym, r.sym))
    }'''),
])

# ---------------------------------------------------------------- the linearity check
add_import("kir_lin_ops.kai", "kir", "kir_syms")
edit("kir_lin_ops.kai", [
 ('''pub type LinFns = NameIx[KFn]

#[doc("Index `fs` by symbol.")]
pub fn klo_index(fs: [KFn]) : LinFns = ni_wrap(klo_fill(fs, ni_buckets()))

fn klo_fill(fs: [KFn], acc: Array[[KFn]]) : Array[[KFn]] / Mutable = match fs {
  []           -> acc
  [f, ...rest] -> klo_fill(rest, ni_add(acc, f.sym, f))
}''', '''pub type LinFns = NameIx[KSymFn]

#[doc("Index `fs` by the symbol `nm` spells each with.")]
pub fn klo_index(nm: KNames, fs: [KFn]) : LinFns = ni_wrap(klo_fill(ksym_fns(nm, fs), ni_buckets()))

fn klo_fill(fs: [KSymFn], acc: Array[[KSymFn]]) : Array[[KSymFn]] / Mutable = match fs {
  []           -> acc
  [r, ...rest] -> klo_fill(rest, ni_add(acc, r.sym, r))
}'''),
 ('''fn klo_fn(fs: [KFn], sym: String) : Option[KFn] = match fs {
  []           -> None
  [f, ...rest] -> if f.sym == sym { Some(f) } else { klo_fn(rest, sym) }
}''', '''fn klo_fn(fs: [KSymFn], sym: String) : Option[KFn] = match fs {
  []           -> None
  [r, ...rest] -> if r.sym == sym { Some(r.f) } else { klo_fn(rest, sym) }
}'''),
])
add_import("kir_lin.kai", "kir", "kir_syms")
edit("kir_lin.kai", [
 ('''    let bad = filter(kp.fns, (f) => any(lines, (l) => string_starts_with_q(l, cat2(f.sym, "\\t"))))
    let _k = match bad { [] -> 0  _ -> { let _d = write_file(cat2(path, ".kir"), kir_dump_fns_pub(kp.tab, bad)); 0 } }''',
  '''    let nm = kp_names(kp)
    let bad = filter(kp.fns, (f) => any(lines, (l) => string_starts_with_q(l, cat2(kfn_sym(nm, f), "\\t"))))
    let _k = match bad { [] -> 0  _ -> { let _d = write_file(cat2(path, ".kir"), kir_dump_fns_pub(nm, bad)); 0 } }'''),
 ('''pub fn kir_lin_report(kp: KProgram) : [String] = klin_fns(kp.fns, klo_index(kp.fns))

fn klin_fns(fs: [KFn], fns: LinFns) : [String] = match fs {
  []           -> []
  [f, ...rest] -> list_append(klin_fn_lines(f, fns), klin_fns(rest, fns))
}

fn klin_fn_lines(f: KFn, fns: LinFns) : [String] = match f.kind {''', '''pub fn kir_lin_report(kp: KProgram) : [String] = klin_fns(kp_names(kp), kp.fns, klo_index(kp_names(kp), kp.fns))

fn klin_fns(nm: KNames, fs: [KFn], fns: LinFns) : [String] = match fs {
  []           -> []
  [f, ...rest] -> list_append(klin_fn_lines(nm, f, fns), klin_fns(nm, rest, fns))
}

fn klin_fn_lines(nm: KNames, f: KFn, fns: LinFns) : [String] = match f.kind {'''),
 ('''(v) => klin_line(f.sym, v)) }''', '''(v) => klin_line(kfn_sym(nm, f), v)) }'''),
])

# ---------------------------------------------------------------- the dumps
add_import("kir_dump.kai", "kir", "kir_syms")
edit("kir_dump.kai", [
 ('''    kir_dump_fns(kp.tab, kp.fns, "")''', '''    kir_dump_fns(kp_names(kp), kp.fns, "")'''),
 ('''fn kir_dump_fns(tab: SymTab, fns: [KFn], acc: String) : String = match fns {''', '''fn kir_dump_fns(nm: KNames, fns: [KFn], acc: String) : String = match fns {'''),
 ('''  [f, ...rest] -> kir_dump_fns(tab, rest, cat2(acc, kir_dump_fn(tab, f)))''', '''  [f, ...rest] -> kir_dump_fns(nm, rest, cat2(acc, kir_dump_fn(nm, f)))'''),
 ('''pub fn kir_dump_fns_pub(tab: SymTab, fns: [KFn]) : String = kir_dump_fns(tab, fns, "")''', '''pub fn kir_dump_fns_pub(nm: KNames, fns: [KFn]) : String = kir_dump_fns(nm, fns, "")'''),
 ('''pub fn kir_dump_fn_sig_pub(f: KFn) : String =
  concat_all([
    kir_fn_kind_kw(f.kind), "fn ", f.sym, "(",''', '''pub fn kir_dump_fn_sig_pub(nm: KNames, f: KFn) : String =
  concat_all([
    kir_fn_kind_kw(f.kind), "fn ", kfn_sym(nm, f), "(",'''),
 ('''fn kir_dump_fn(tab: SymTab, f: KFn) : String =
  concat_all([
    "\\n", kir_fn_kind_kw(f.kind), "fn ", f.sym, "(",''', '''fn kir_dump_fn(nm: KNames, f: KFn) : String =
  concat_all([
    "\\n", kir_fn_kind_kw(f.kind), "fn ", kfn_sym(nm, f), "(",'''),
 ('''    kir_dump_blocks(tab, f.blocks, ""),
    "}\\n"''', '''    kir_dump_blocks(nm.tab, f.blocks, ""),
    "}\\n"'''),
])
add_import("symid_dump.kai", "kir", "kir_syms")
edit("symid_dump.kai", [
 ('''pub fn symid_kir_lines(tab: SymTab, core: [String], kp: KProgram) : [String] = sd_kfns(tab, core, kp.fns)''',
  '''pub fn symid_kir_lines(tab: SymTab, core: [String], kp: KProgram) : [String] = sd_kfns(tab, kp_names(kp), core, kp.fns)'''),
 ('''fn sd_kfns(tab: SymTab, core: [String], fns: [KFn]) : [String] = match fns {
  []           -> []
  [f, ...rest] ->
    (if sd_is_user(f.mo, core) { sd_blocks(tab, f.sym, f.blocks) } else { [] }) ++ sd_kfns(tab, core, rest)''',
  '''fn sd_kfns(tab: SymTab, nm: KNames, core: [String], fns: [KFn]) : [String] = match fns {
  []           -> []
  [f, ...rest] ->
    (if sd_is_user(f.mo, core) { sd_blocks(tab, kfn_sym(nm, f), f.blocks) } else { [] }) ++ sd_kfns(tab, nm, core, rest)'''),
])
print("step 2 ok")
