"""f_p3a2.py: the remaining readers of a variant row's name. Run from stage2/compiler after f_p3a.py."""
import re, sys
sys.dont_write_bytecode = True
def edit(p, pairs):
    s = open(p).read()
    for a, b, *n in pairs:
        want = n[0] if n else 1
        assert s.count(a) == want, (p, a[:80], s.count(a))
        s = s.replace(a, b)
    open(p, "w").write(s)

edit("emit_c.kai", [
 ('''        EV(_, _, tag, rc) -> { let n = ctor_cell_name(cx.tab, rc)
          concat_all([
          "kai_cons(kai_variant_u(", int_to_string(tag),
          ", \\"", n, "\\", 0, 0, NULL), ",''', '''        EV(_, _, tag, rc) -> concat_all([
          "kai_cons(kai_variant_u(", int_to_string(tag),
          ", \\"", ctor_cell_name(cx.tab, rc), "\\", 0, 0, NULL), ",'''),
 ('''fn emit_payload_ctor_rows(variants: [VariantEntry]) : String = match variants {''', '''fn emit_payload_ctor_rows(tab: SymTab, variants: [VariantEntry]) : String = match variants {'''),
 ('''    EV(_, nm, _, tys, tag, _) ->
      if variant_has_primitive_slot(tys) {
        concat_all([
          "{", int_to_string(tag), ", \\"", nm, "\\", ",
          int_to_string(variant_slot_mask(tys)), "}, ",
          emit_payload_ctor_rows(rest)
        ])
      } else { emit_payload_ctor_rows(rest) }''', '''    EV(_, tys, tag, c) ->
      if variant_has_primitive_slot(tys) {
        concat_all([
          "{", int_to_string(tag), ", \\"", ctor_cell_name(tab, c), "\\", ",
          int_to_string(variant_slot_mask(tys)), "}, ",
          emit_payload_ctor_rows(tab, rest)
        ])
      } else { emit_payload_ctor_rows(tab, rest) }'''),
 ('''  let pctor_rows = emit_payload_ctor_rows(variants)''', '''  let pctor_rows = emit_payload_ctor_rows(tab, variants)'''),
 ('''fn emit_nullary_enum_seed(variants: [VariantEntry]) : String = match variants {
  [] -> ""
  [EV(_, nm, ar, _, tag, _), ...rest] ->
    if ar == 0 and tag >= user_variant_tag_base() {
      concat_all([
        "    kai_variant_u(", int_to_string(tag), ", \\"", nm, "\\", 0, 0, NULL);\\n",
        emit_nullary_enum_seed(rest)
      ])
    } else { emit_nullary_enum_seed(rest) }''', '''fn emit_nullary_enum_seed(tab: SymTab, variants: [VariantEntry]) : String = match variants {
  [] -> ""
  [EV(ar, _, tag, c), ...rest] ->
    if ar == 0 and tag >= user_variant_tag_base() {
      concat_all([
        "    kai_variant_u(", int_to_string(tag), ", \\"", ctor_cell_name(tab, c), "\\", 0, 0, NULL);\\n",
        emit_nullary_enum_seed(tab, rest)
      ])
    } else { emit_nullary_enum_seed(tab, rest) }'''),
 ('''emit_nullary_enum_seed(variants)])''', '''emit_nullary_enum_seed(tab, variants)])'''),
])

edit("emit_native_proto.kai", [
 ('''fn nproto_seed_nullary(ctx: Handle, variants: [VariantEntry]) : Unit / Console = match variants {
  [] -> ()
  [EV(_, nm, ar, _, tag, _), ...rest] -> {
    if ar == 0 and tag >= user_variant_tag_base() { nproto_seed_one_nullary(ctx, tag, nm) } else { () }
    nproto_seed_nullary(ctx, rest)''', '''fn nproto_seed_nullary(ctx: Handle, tab: SymTab, variants: [VariantEntry]) : Unit / Console = match variants {
  [] -> ()
  [EV(ar, _, tag, c), ...rest] -> {
    if ar == 0 and tag >= user_variant_tag_base() { nproto_seed_one_nullary(ctx, tag, ctor_cell_name(tab, c)) } else { () }
    nproto_seed_nullary(ctx, tab, rest)'''),
 ('''fn nproto_register_payload_ctors(ctx: Handle, variants: [VariantEntry]) : Unit / Console = match variants {
  [] -> ()
  [EV(_, nm, _, ks, tag, _), ...rest] -> {
    let mask = variant_slot_mask(ks)
    if tag >= user_variant_tag_base() and mask != 0 {
      nproto_register_one_payload_ctor(ctx, tag, nm, mask)
    } else { () }
    nproto_register_payload_ctors(ctx, rest)''', '''fn nproto_register_payload_ctors(ctx: Handle, tab: SymTab, variants: [VariantEntry]) : Unit / Console = match variants {
  [] -> ()
  [EV(_, ks, tag, c), ...rest] -> {
    let mask = variant_slot_mask(ks)
    if tag >= user_variant_tag_base() and mask != 0 {
      nproto_register_one_payload_ctor(ctx, tag, ctor_cell_name(tab, c), mask)
    } else { () }
    nproto_register_payload_ctors(ctx, tab, rest)'''),
 ('''  nproto_register_payload_ctors(ctx, variants)''', '''  nproto_register_payload_ctors(ctx, tab, variants)'''),
 ('''  nproto_seed_nullary(ctx, variants)''', '''  nproto_seed_nullary(ctx, tab, variants)'''),
])

edit("kir_lower.kai", [
 ('''#[doc("The layout of the constructor `row` lays out.")]
pub fn ctor_lay(row: VariantEntry) : CtorLay = match row {
  EV(_, name, _, ks, tag, _) ->
    if tag < user_variant_tag_base() { CtorLay { name: name, tag: tag, kinds: [], mask: 0 } }
    else { CtorLay { name: name, tag: tag, kinds: ks, mask: variant_slot_mask(ks) } }
}''', '''#[doc("The layout of the constructor `row` lays out; `tab` names it.")]
pub fn ctor_lay(tab: SymTab, row: VariantEntry) : CtorLay = match row {
  EV(_, ks, tag, c) ->
    if tag < user_variant_tag_base() { CtorLay { name: ctor_cell_name(tab, c), tag: tag, kinds: [], mask: 0 } }
    else { CtorLay { name: ctor_cell_name(tab, c), tag: tag, kinds: ks, mask: variant_slot_mask(ks) } }
}'''),
 ('''  Some(row) -> Some(ctor_lay(row))''', '''  Some(row) -> Some(ctor_lay(st.tab, row))'''),
])
print("ok")
