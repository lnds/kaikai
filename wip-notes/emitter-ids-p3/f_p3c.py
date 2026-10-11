"""f_p3c.py: a lowered function is the unit it was lowered from; its symbol is spelled where it is needed.
Run from stage2/compiler after f_p3b.py. Step 1: the types and the lowering."""
import re, sys, shutil, os
sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
def edit(p, pairs):
    s = open(p).read()
    for a, b, *n in pairs:
        want = n[0] if n else 1
        assert s.count(a) == want, (p, a[:90], s.count(a))
        s = s.replace(a, b)
    open(p, "w").write(s)
def add_import(p, after, new):
    s = open(p).read()
    line = "import compiler.%s\n" % new
    if line in s: return
    a = "import compiler.%s\n" % after
    assert s.count(a) == 1, (p, after)
    open(p, "w").write(s.replace(a, a + line))

shutil.copy(os.path.join(HERE, "new", "kir_syms.kai"), "kir_syms.kai")

# ---------------------------------------------------------------- kir.kai
add_import("kir.kai", "spec_names", "fnreg")
add_import("kir.kai", "spec_names", "infer_index")
add_import("kir.kai", "spec_names", "enc_unit")
edit("kir.kai", [
 ('''# A lowered function: resolved symbol, calling kind, params, return slot,
# the block list (entry block first).''', '''# A lowered function: the unit it was lowered from, calling kind, params,
# return slot, the block list (entry block first). `kfn_sym` spells the
# symbol it is emitted under.'''),
 ('''# `fid` is the declaration a fn was lowered from, the unresolved identity
# for a lifted lambda, a clause and a runner: a table about declared fns
# is asked by it, never by `sym`.
pub type KFn = { sym: String, kind: KFnKind, params: [KParam], ret: KSlot,
                 blocks: [KBlock], region_id: Option[Int], stateful: Bool,
                 mo: Option[String], donor: Bool, entry: Bool, fid: FnId }
''', '''pub type KFn = { unit: EncUnit, kind: KFnKind, params: [KParam], ret: KSlot,
                 blocks: [KBlock], region_id: Option[Int], stateful: Bool,
                 mo: Option[String], donor: Bool, entry: Bool }

#[doc("The declaration `f` was lowered from, the unresolved identity for a lifted lambda, a clause and a runner: a table about declared functions is asked by it.")]
pub fn kfn_fid(f: KFn) : FnId = match f.unit {
  EuFn(g) -> g
  _       -> fnid_none()
}

#[doc("`f` with the blocks `bs`.")]
pub fn kfn_with_blocks(f: KFn, bs: [KBlock]) : KFn =
  KFn { unit: f.unit, kind: f.kind, params: f.params, ret: f.ret, blocks: bs, region_id: f.region_id,
        stateful: f.stateful, mo: f.mo, donor: f.donor, entry: f.entry }
'''),
 ('''# `tab` is the symbol table the program's ids index: a dump or an emitter
# spells a declaration's name from it. `ids` says which functions the
# protocol lowering generated.''', '''# `tab` is the symbol table the program's ids index: a dump or an emitter
# spells a declaration's name from it. `reg` is the function registry by
# declaration, which a function's symbol is spelled from. `ids` says
# which functions the
# protocol lowering generated.'''),
 ('''                      entry: SymId, tab: SymTab, specs: SpecNames, ids: ProtoIds, reuse_tags: [Int] }''',
  '''                      entry: SymId, tab: SymTab, specs: SpecNames, reg: NameIndex[EFn], ids: ProtoIds, reuse_tags: [Int] }'''),
])

# ---------------------------------------------------------------- the lowering
edit("kir_lower_fns.kai", [
 ('''runners: rs.runners, entry: entry, tab: tab, specs: specs, ids: ids,''', '''runners: rs.runners, entry: entry, tab: tab, specs: specs, reg: st0.fx.sx, ids: ids,'''),
 ('''    KFn { sym: ffi_wrapper_sym(name, mo), kind: KFnFfi(''', '''    KFn { unit: head.enc.unit, kind: KFnFfi('''),
 ('''          region_id: None, stateful: false, mo: mo, donor: false, entry: false, fid: enc_fn_id(head.enc) }''',
  '''          region_id: None, stateful: false, mo: mo, donor: false, entry: false }'''),
 ('''      LRes(val, st1) -> KFn { sym: sym, kind: KFnNormal, params: kparams,''', '''      LRes(val, st1) -> KFn { unit: head.enc.unit, kind: KFnNormal, params: kparams,'''),
 ('''                              region_id: None, stateful: false, mo: mo, donor: dn, entry: ent, fid: enc_fn_id(head.enc) }''',
  '''                              region_id: None, stateful: false, mo: mo, donor: dn, entry: ent }'''),
 ('''      KFn { sym: sym, kind: KFnLambda, params: kparams, ret: SBoxed,''', '''      KFn { unit: lam_unit(l).unit, kind: KFnLambda, params: kparams, ret: SBoxed,'''),
 ('''            stateful: false, mo: None, donor: false, entry: false, fid: fnid_none() } } }''', '''            stateful: false, mo: None, donor: false, entry: false } } }'''),
 ('''      KFn { sym: sym, kind: if is_fin { KFnCleanup } else { KFnClause },''', '''      KFn { unit: clause_unit(c).unit, kind: if is_fin { KFnCleanup } else { KFnClause },'''),
 ('''            blocks: blocks, region_id: None, stateful: stateful, mo: None, donor: false, entry: false, fid: fnid_none() }''',
  '''            blocks: blocks, region_id: None, stateful: stateful, mo: None, donor: false, entry: false }'''),
])
add_import("kir_lower.kai", "kir", "kir_syms")
edit("kir_lower.kai", [
 ('''pub fn ffi_wrapper_sym(name: String, mo: Option[String]) : String =
  cat2("__ffi_", c_sym(name, mo))''', '''pub fn ffi_wrapper_sym(name: String, mo: Option[String]) : String =
  ffi_wrapper_of(c_sym(name, mo))'''),
])
print("step 1 ok")

edit("kir_lower_fns.kai", [
 ('''  let name = head.name
  let sym = head.enc.sym
''', '''  let name = head.name
'''), ("    let sym = lam_symbol(l)\n", ""),
])
print("step 1b ok")
