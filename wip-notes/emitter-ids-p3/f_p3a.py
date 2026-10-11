"""f_p3a.py: a variant row holds its constructor's id and layout; the name its cells carry is asked of the symbol
table where it is emitted. Run from stage2/compiler."""
import re, sys
sys.dont_write_bytecode = True
def edit(p, pairs):
    s = open(p).read()
    for a, b, *n in pairs:
        want = n[0] if n else 1
        assert s.count(a) == want, (p, a[:80], s.count(a))
        s = s.replace(a, b)
    open(p, "w").write(s)
def resub(p, pat, rep, want=None):
    s = open(p).read()
    s2, n = re.subn(pat, rep, s)
    assert (want is None and n > 0) or n == want, (p, pat, n)
    open(p, "w").write(s2)

# ---------------------------------------------------------------- emit_shared.kai
edit("emit_shared.kai", [
 ('''#[doc("Emit-side variant table entry: declaring module, constructor name, arity, payload slot kinds, head tag, and the constructor it lays out. The name is what the emitted cell carries and the tag hashes; the id is what a row is found by.")]
pub type VariantEntry = EV(Option[String], String, Int, [Int], Int, SymId)''',
  '''#[doc("Emit-side variant table entry: arity, payload slot kinds, head tag, and the constructor it lays out. The id is what a row is found by; `ctor_cell_name` spells the constructor where a cell is emitted.")]
pub type VariantEntry = EV(Int, [Int], Int, SymId)'''),
 ('''pub fn builtin_variants() : [VariantEntry] = map(builtin_rows(), (r) => match r { BR(v, _) -> v })''',
  '''pub fn builtin_variants() : [VariantEntry] = map(builtin_rows(), (r) => r.row)'''),
 ('''# A constructor the runtime builds and no module declares.
fn core_row(name: String, arity: Int, tag: Int, head: RtHead) : BuiltinRow =
  BR(EV(None, name, arity, [], tag, sym_derive(NCCtor, name, core_type_home())), head)

# A constructor the core module `effects/os.kai` declares: a program
# constructor of the same name is another constructor, with its own row.
fn os_row(name: String, kinds: [Int], tag: Int, head: RtHead) : BuiltinRow =
  BR(EV(Some(os_module_home()), name, list_length(kinds), kinds, tag, sym_derive(NCCtor, name, os_module_home())), head)

fn builtin_row_of(c: SymId) : Option[BuiltinRow] =
  find(builtin_rows(), (r) => match r { BR(v, _) -> sym_same(evar_sym(v), c) })''',
  '''# A constructor the runtime builds and no module declares. Its row lays
# out every declaration spelled like it.
fn core_row(name: String, arity: Int, tag: Int, head: RtHead) : BuiltinRow =
  BuiltinRow { name: name, row: EV(arity, [], tag, sym_derive(NCCtor, name, core_type_home())), head: head, everywhere: true }

# A constructor the core module `effects/os.kai` declares: a program
# constructor of the same name is another constructor, with its own row.
fn os_row(name: String, kinds: [Int], tag: Int, head: RtHead) : BuiltinRow =
  BuiltinRow { name: name, row: EV(list_length(kinds), kinds, tag, sym_derive(NCCtor, name, os_module_home())), head: head, everywhere: false }

fn builtin_row_of(c: SymId) : Option[BuiltinRow] =
  find(builtin_rows(), (r) => sym_same(evar_sym(r.row), c))

#[doc("The name the cells of the constructor `c` carry: a fixed row's own, else the one `c` is declared with.")]
pub fn ctor_cell_name(tab: SymTab, c: SymId) : String = match builtin_row_of(c) {
  Some(r) -> r.name
  None    -> suv_ctor_text(tab, c)
}'''),
 ('''  Some(BR(v, _)) -> Some(v)
  None           -> if st_valid(tab, c) { suv_core_twin(sym_homonyms(tab, c)) } else { None }''',
  '''  Some(r) -> Some(r.row)
  None    -> if st_valid(tab, c) { suv_core_twin(sym_homonyms(tab, c)) } else { None }'''),
 ('''    Some(BR(v, _)) -> match evar_home(v) {
      None    -> Some(v)
      Some(_) -> suv_core_twin(rest)
    }
    None -> suv_core_twin(rest)''', '''    Some(r) -> if r.everywhere { Some(r.row) } else { suv_core_twin(rest) }
    None    -> suv_core_twin(rest)'''),
 ('''fn evar_key_of(v: VariantEntry) : String = match v {
  EV(hm, n, _, _, _, _) -> evar_key(hm, n)
}

''', ''),
 ('''pub fn register_variants(decls: [Decl], tab: SymTab) : [VariantEntry] {
  let pending = suv_sorted_unique(suv_pending_decls(decls, tab, type_home_table(decls, tab), []))''',
  '''pub fn register_variants(decls: [Decl], tab: SymTab) : [VariantEntry] {
  let pending = suv_sorted_unique(suv_pending_decls(decls, tab, type_home_table(decls, tab), []))'''),
 ('''# Collect user ctors newest-first, each stamped with the module that
# declares it. Tags are assigned later.
fn suv_pending_decls(ds: [Decl], tab: SymTab, hs: TyHomeTab, acc: [VariantEntry]) : [VariantEntry] {''',
  '''# A constructor awaiting its tag, under the key the tag hashes.
type SuvPending = { key: String, row: VariantEntry }

# Collect user ctors newest-first, each keyed by the module that declares
# it and its name. Tags are assigned later.
fn suv_pending_decls(ds: [Decl], tab: SymTab, hs: TyHomeTab, acc: [SuvPending]) : [SuvPending] {'''),
 ('''fn suv_pending_vs(vs: [Variant], home: Option[String], tab: SymTab, hs: TyHomeTab, acc: [VariantEntry]) : [VariantEntry] {''',
  '''fn suv_pending_vs(vs: [Variant], home: Option[String], tab: SymTab, hs: TyHomeTab, acc: [SuvPending]) : [SuvPending] {'''),
 ('''          suv_pending_vs(rest, home, tab, hs, [EV(home, suv_ctor_text(tab, c), list_length(tys), ks, 0, c), ...acc])''',
  '''          suv_pending_vs(rest, home, tab, hs, [SuvPending { key: evar_key(home, suv_ctor_text(tab, c)), row: EV(list_length(tys), ks, 0, c) }, ...acc])'''),
 ('''fn suv_sorted_unique(newest_first: [VariantEntry]) : [VariantEntry] =
  suv_unique(sort_by(list_reverse(newest_first), suv_cmp), [])

fn suv_cmp(a: VariantEntry, b: VariantEntry) : Int =
  if string_lt(evar_key_of(a), evar_key_of(b)) { 0 - 1 } else if evar_key_of(a) == evar_key_of(b) { 0 } else { 1 }

fn suv_unique(vs: [VariantEntry], acc: [VariantEntry]) : [VariantEntry] = match (vs, acc) {
  ([], _) -> list_reverse(acc)
  ([v, ...rest], [prev, ..._]) -> suv_unique(rest, if sym_same(evar_sym(prev), evar_sym(v)) { acc } else { [v, ...acc] })
  ([v, ...rest], []) -> suv_unique(rest, [v])
}

fn suv_assign(vs: [VariantEntry], used: [Int], acc: [VariantEntry]) : [VariantEntry] = match vs {
  [] -> acc
  [v, ...rest] -> match v {
    EV(hm, n, ar, tys, _, c) -> {
      let t = suv_slot(evar_key(hm, n), used)
      suv_assign(rest, [t, ...used], [EV(hm, n, ar, tys, t, c), ...acc])
    }
  }
}''', '''fn suv_sorted_unique(newest_first: [SuvPending]) : [SuvPending] =
  suv_unique(sort_by(list_reverse(newest_first), suv_cmp), [])

fn suv_cmp(a: SuvPending, b: SuvPending) : Int =
  if string_lt(a.key, b.key) { 0 - 1 } else if a.key == b.key { 0 } else { 1 }

fn suv_unique(vs: [SuvPending], acc: [SuvPending]) : [SuvPending] = match (vs, acc) {
  ([], _) -> list_reverse(acc)
  ([v, ...rest], [prev, ..._]) -> suv_unique(rest, if sym_same(evar_sym(prev.row), evar_sym(v.row)) { acc } else { [v, ...acc] })
  ([v, ...rest], []) -> suv_unique(rest, [v])
}

fn suv_assign(vs: [SuvPending], used: [Int], acc: [VariantEntry]) : [VariantEntry] = match vs {
  [] -> acc
  [v, ...rest] -> match v.row {
    EV(ar, tys, _, c) -> {
      let t = suv_slot(v.key, used)
      suv_assign(rest, [t, ...used], [EV(ar, tys, t, c), ...acc])
    }
  }
}'''),
 ('''pub fn evar_arity(v: VariantEntry) : Int = match v { EV(_, _, ar, _, _, _) -> ar }''', '''pub fn evar_arity(v: VariantEntry) : Int = match v { EV(ar, _, _, _) -> ar }'''),
 ('''pub fn evar_kinds(v: VariantEntry) : [Int] = match v { EV(_, _, _, ks, _, _) -> ks }''', '''pub fn evar_kinds(v: VariantEntry) : [Int] = match v { EV(_, ks, _, _) -> ks }'''),
 ('''pub fn evar_sym(v: VariantEntry) : SymId = match v { EV(_, _, _, _, _, c) -> c }''', '''pub fn evar_sym(v: VariantEntry) : SymId = match v { EV(_, _, _, c) -> c }'''),
 ('''[EV(_, _, _, _, tag, c), ...rest] -> collect_v2h_pairs(''', '''[EV(_, _, tag, c), ...rest] -> collect_v2h_pairs('''),
 ('''  Some(BR(_, h)) -> rt_head_tag(h)
  None           -> ctor_owner_head_tag(c, decls)''', '''  Some(r) -> rt_head_tag(r.head)
  None    -> ctor_owner_head_tag(c, decls)'''),
])
s = open("emit_shared.kai").read()
m = re.search(r'^type BuiltinRow = BR\(VariantEntry, RtHead\)\n', s, re.M); assert m
s = s[:m.start()] + '''# A constructor the runtime knows: its name, its row, the head of its sum,
# and whether the row lays out every declaration spelled like it.
type BuiltinRow = { name: String, row: VariantEntry, head: RtHead, everywhere: Bool }
''' + s[m.end():]
# the name and home accessors go with the slots
m = re.search(r'(#\[doc\("[^\n]*"\)\]\n)?pub fn evar_name\(v: VariantEntry\) : String = match v \{ EV\(_, n, _, _, _, _\) -> n \}\n\n?', s); assert m
s = s[:m.start()] + s[m.end():]
m = re.search(r'(#\[doc\("[^\n]*"\)\]\n)?pub fn evar_home\(v: VariantEntry\) : Option\[String\] = match v \{ EV\(hm, _, _, _, _, _\) -> hm \}\n\n?', s); assert m
s = s[:m.start()] + s[m.end():]
open("emit_shared.kai", "w").write(s)

# ---------------------------------------------------------------- readers that take no name
for p in ["ctor_ix.kai", "emit_c.kai", "kir_lower_bind.kai", "kir_lower_walk.kai", "kir_pat_alias.kai", "reuse_cells.kai"]:
    s = open(p).read()
    s = s.replace("EV(_, _, ", "EV(")
    open(p, "w").write(s)

# ---------------------------------------------------------------- emit_c.kai: the cell's name where it is written
edit("emit_c.kai", [
 ('''fn emit_nullary_read(row: VariantEntry) : String = match row {
  EV(_, name, _, _, tag, _) -> concat_all(["kai_nullary_fast(", int_to_string(tag), ", \\"", name, "\\")"])
}''', '''fn emit_nullary_read(tab: SymTab, row: VariantEntry) : String = match row {
  EV(_, _, tag, c) -> concat_all(["kai_nullary_fast(", int_to_string(tag), ", \\"", ctor_cell_name(tab, c), "\\")"])
}'''),
 ('''    Some(row) -> emit_nullary_read(row)''', '''    Some(row) -> emit_nullary_read(cx.tab, row)'''),
 ('''                           cx: EmitCtx) : String / Console { match row { EV(_, cname, ar, ks, tag, _) -> {
  let n = list_length(cargs)''', '''                           cx: EmitCtx) : String / Console { match row { EV(ar, ks, tag, ctor) -> {
  let cname = ctor_cell_name(cx.tab, ctor)
  let n = list_length(cargs)'''),
 ('''  EV(_, name, _, tys, tag, _) -> match emit_variant_call_typed(tag, name, args_e, tys, src, cx) {
    Some(s) -> s
    None    -> emit_variant_call(tag, name, list_length(args_e), emit_slot_ops(src, args_e, cx), cx)
  }''', '''  EV(_, tys, tag, c) -> match emit_variant_call_typed(tag, ctor_cell_name(cx.tab, c), args_e, tys, src, cx) {
    Some(s) -> s
    None    -> emit_variant_call(tag, ctor_cell_name(cx.tab, c), list_length(args_e), emit_slot_ops(src, args_e, cx), cx)
  }'''),
 ('''  Some(EV(_, name, _, _, tag, _)) -> emit_tag_test_at(scr, name, tag, Some(tag))''',
  '''  Some(EV(_, _, tag, c)) -> emit_tag_test_at(scr, ctor_cell_name(cx.tab, c), tag, Some(tag))'''),
 ('''    ReuseVariantArm(subs, EV(_, cname, _, _, tag, _), cargs, tys) -> {''', '''    ReuseVariantArm(subs, EV(_, _, tag, ctor), cargs, tys) -> {
      let cname = ctor_cell_name(cx.tab, ctor)'''),
 ('''        None    -> EV(None, written_text(decl_name(cx.tab, c)), 0, [], 0, c)
      }
      match row {
        EV(_, n, _, _, tag, _) -> concat_all([''', '''        None    -> EV(0, [], 0, c)
      }
      match row {
        EV(_, _, tag, rc) -> { let n = ctor_cell_name(cx.tab, rc)
          concat_all(['''),
 ('''    Some(EV(_, name, _, _, tag, _)) -> concat_all([
      "({ KaiValue *_b = ", inner_c, "; ",
      "if (!(_b && ", emit_tag_test_at("_b", name, tag, Some(tag)), ")) { ", owed, "return _b; } ",''',
  '''    Some(EV(_, _, tag, c)) -> concat_all([
      "({ KaiValue *_b = ", inner_c, "; ",
      "if (!(_b && ", emit_tag_test_at("_b", ctor_cell_name(cx.tab, c), tag, Some(tag)), ")) { ", owed, "return _b; } ",'''),
])
print("ok (emit_c sites 6829, 9010, 9357 and the native/KIR ones are finished by hand)")
