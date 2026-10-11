"""f_p3b.py: an impl row holds the impl function's id; its symbol is asked of the function registry where it is
emitted. Run from stage2/compiler after f_p3a2.py."""
import re, sys
sys.dont_write_bytecode = True
def edit(p, pairs):
    s = open(p).read()
    for a, b, *n in pairs:
        want = n[0] if n else 1
        assert s.count(a) == want, (p, a[:80], s.count(a))
        s = s.replace(a, b)
    open(p, "w").write(s)

s = open("emit_shared.kai").read()
a = s.index('#[doc("Row for one impl method of a core protocol: the C symbol name,')
b = s.index('#[doc("The name an impl\'s target is emitted under: a comment beside its row.")]')
s = s[:a] + '''#[doc("Row for one impl method of a core protocol: the impl function, the protocol, the type the impl targets, the impl fn's arity, its unboxed signature (Some when scalar, None when boxed), the operation's number within the protocol, and the param positions Perceus made borrowed. The function registry spells the impl's symbol where the row is emitted.")]
pub type PimplRow = PR(FnId, CoreProto, DeclRef, Int, Option[UFnSig], Int, [Int])

#[doc("The rows of the impl methods among `ds`. `ids` says which functions are impl methods, of which protocol, type and operation; `fx` is the fn registry, holding each fn's unboxed signature (the unbox pass stamps it there).")]
pub fn collect_pimpl_rows(ds: [Decl], fx: FnIndex, ids: ProtoIds, acc: [PimplRow]) : [PimplRow] {
  match ds {
    [] -> list_reverse(acc)
    [d, ...rest] -> match d {
      DFn(_, fid, _, params, _, _, _, _, _, _) ->
        match pimpl_row_of(fx, fid, ids, params) {
          Some(row) -> collect_pimpl_rows(rest, fx, ids, [row, ...acc])
          None      -> collect_pimpl_rows(rest, fx, ids, acc)
        }
      _ -> collect_pimpl_rows(rest, fx, ids, acc)
    }
  }
}

# The row of the fn `fid` when it is an impl method of a core protocol.
# The table dispatches to the impl itself, never to a specialisation.
fn pimpl_row_of(fx: FnIndex, fid: FnId, ids: ProtoIds, params: [Param]) : Option[PimplRow] =
  if fnid_inst(fid) != spec_none() { None }
  else { match pid_impl(ids, fnid_sym(fid)) {
    Some(PIR(proto, target, op, _, _, _, _)) -> match core_proto_of(proto) {
      Some(cp) -> {
        let sig = fnx_ufn_sig_first(fx, fid)
        Some(PR(fid, cp, target, list_length(params), sig, stdlib_proto_op_id_int(cp, written_text(op)),
                pimpl_lent(params, pimpl_raw_flags(sig), 0)))
      }
      None -> None
    }
    None -> None
  } }

#[doc("The impl function of the row `pr`.")]
pub fn pimpl_row_fn(pr: PimplRow) : FnId = match pr { PR(f, _, _, _, _, _, _) -> f }

#[doc("The symbol the impl of the row `pr` is emitted under, as the registry `fx` has it.")]
pub fn pimpl_csym(fx: FnIndex, pr: PimplRow) : String = match fnx_callee(fx, pimpl_row_fn(pr)) {
  Some(e) -> efn_c_sym(e)
  None    -> panic("identity: an impl row names a function the registry does not hold")
}

''' + s[b:]
open("emit_shared.kai", "w").write(s)

edit("emit_c.kai", [
 ('''  let impl_rows = collect_pimpl_rows(decls, fns, tab, ids, [])''', '''  let fx        = fnx_build(fns)
  let impl_rows = collect_pimpl_rows(decls, fx, ids, [])'''),
 ('''  let impl_arr  = emit_impl_table_array(impl_rows, decls, tab)''', '''  let impl_arr  = emit_impl_table_array(impl_rows, fx, decls, tab)'''),
 ('''  let shim_defs = emit_impl_shim_defs(impl_rows, [])
  let fwds      = concat_all([emit_pimpl_forwards(impl_rows, []), shim_defs])''',
  '''  let shim_defs = emit_impl_shim_defs(impl_rows, fx, [])
  let fwds      = concat_all([emit_pimpl_forwards(impl_rows, fx, []), shim_defs])'''),
 ('''fn emit_impl_table_array(rows: [PimplRow], decls: [Decl], tab: SymTab) : String =
  emit_impl_table_array_loop(rows, decls, tab, "")

fn emit_impl_table_array_loop(rows: [PimplRow], decls: [Decl], tab: SymTab, acc: String) : String {
  match rows {
    [] -> acc
    [pr, ...rest] -> match pr {
      PR(csym, p, t, _, _, op, _) -> {''', '''fn emit_impl_table_array(rows: [PimplRow], fx: FnIndex, decls: [Decl], tab: SymTab) : String =
  emit_impl_table_array_loop(rows, fx, decls, tab, "")

fn emit_impl_table_array_loop(rows: [PimplRow], fx: FnIndex, decls: [Decl], tab: SymTab, acc: String) : String {
  match rows {
    [] -> acc
    [pr, ...rest] -> match pr {
      PR(_, p, t, _, _, op, _) -> {
        let csym = pimpl_csym(fx, pr)'''),
 ('''                                ", .op_id = ", stdlib_proto_op_id_c(p, op),''', '''                                ", .op_id = ", int_to_string(op),'''),
 ('''        emit_impl_table_array_loop(rest, decls, tab, concat_all([acc, line]))''', '''        emit_impl_table_array_loop(rest, fx, decls, tab, concat_all([acc, line]))'''),
 ('''fn emit_pimpl_forwards(rows: [PimplRow], _seen: [String]) : String =
  emit_pimpl_forwards_loop(rows, [])

fn emit_pimpl_forwards_loop(rows: [PimplRow], seen: [String]) : String {
  match rows {
    [] -> ""
    [PR(csym, _, _, _, _, _, _), ...rest] ->
      if list_has(seen, csym) {
        emit_pimpl_forwards_loop(rest, seen)
      } else {
        concat_all(["/* impl-table address-of for &", c_user_sym(csym), " */\\n",
                     emit_pimpl_forwards_loop(rest, [csym, ...seen])])
      }
  }
}''', '''fn emit_pimpl_forwards(rows: [PimplRow], fx: FnIndex, seen: [FnId]) : String {
  match rows {
    [] -> ""
    [pr, ...rest] ->
      if any(seen, (f) => fnid_eq(f, pimpl_row_fn(pr))) {
        emit_pimpl_forwards(rest, fx, seen)
      } else {
        concat_all(["/* impl-table address-of for &", c_user_sym(pimpl_csym(fx, pr)), " */\\n",
                     emit_pimpl_forwards(rest, fx, [pimpl_row_fn(pr), ...seen])])
      }
  }
}'''),
 ('''fn emit_impl_shim_defs(rows: [PimplRow], seen: [String]) : String {
  match rows {
    [] -> ""
    [pr, ...rest] -> match pr {
      PR(csym, _, _, arity, sig, _, lent) ->
        if list_has(seen, csym) or not pimpl_row_needs_adapter(pr) {
          emit_impl_shim_defs(rest, seen)
        } else {''', '''fn emit_impl_shim_defs(rows: [PimplRow], fx: FnIndex, seen: [FnId]) : String {
  match rows {
    [] -> ""
    [pr, ...rest] -> match pr {
      PR(ifn, _, _, arity, sig, _, lent) ->
        if any(seen, (f) => fnid_eq(f, ifn)) or not pimpl_row_needs_adapter(pr) {
          emit_impl_shim_defs(rest, fx, seen)
        } else {
          let csym = pimpl_csym(fx, pr)'''),
 ('''          concat_all([shim, emit_impl_shim_defs(rest, [csym, ...seen])])''', '''          concat_all([shim, emit_impl_shim_defs(rest, fx, [ifn, ...seen])])'''),
])

edit("emit_native_proto.kai", [
 ('''  let rows = collect_pimpl_rows(decls, fns, tab, ids, [])
  let v2h  = collect_variant_to_head(variants, decls)''', '''  let fx   = fnx_build(fns)
  let rows = collect_pimpl_rows(decls, fx, ids, [])
  let v2h  = collect_variant_to_head(variants, decls)'''),
 ('''  nproto_emit_adapters(ctx, rows, decls, tab, adapters)
  nproto_open_init(ctx)''', '''  nproto_emit_adapters(ctx, rows, fx, decls, adapters)
  nproto_open_init(ctx)'''),
 ('''  nproto_register_impls(ctx, rows, decls, tab)
  nproto_register_payload_ctors''', '''  nproto_register_impls(ctx, rows, fx, decls)
  nproto_register_payload_ctors'''),
 ('''                               only: [String]) : Unit / Console =
  nproto_emit_adapters(ctx, collect_pimpl_rows(decls, fns, tab, ids, []), decls, tab, NAdaptOnly(only))''',
  '''                               only: [String]) : Unit / Console = {
  let fx = fnx_build(fns)
  nproto_emit_adapters(ctx, collect_pimpl_rows(decls, fx, ids, []), fx, decls, NAdaptOnly(only))
}'''),
 ('''fn nproto_emit_adapters(ctx: Handle, rows: [PimplRow], decls: [Decl], tab: SymTab,
                        adapters: NAdapters) : Unit / Console =
  match rows {
    [] -> ()
    [pr, ...rest] -> {
      match pr {
        PR(csym, _, t, arity, sig, _, lent) ->
          if impl_head_tag(t, decls) >= 0 and pimpl_row_needs_adapter(pr) and nadapt_allows(adapters, csym) {
            nproto_emit_boxed_adapter(ctx, csym, sig, arity, lent)
          } else { () }
      }
      nproto_emit_adapters(ctx, rest, decls, tab, adapters)''', '''fn nproto_emit_adapters(ctx: Handle, rows: [PimplRow], fx: FnIndex, decls: [Decl],
                        adapters: NAdapters) : Unit / Console =
  match rows {
    [] -> ()
    [pr, ...rest] -> {
      match pr {
        PR(_, _, t, arity, sig, _, lent) ->
          if impl_head_tag(t, decls) >= 0 and pimpl_row_needs_adapter(pr) and nadapt_allows(adapters, pimpl_csym(fx, pr)) {
            nproto_emit_boxed_adapter(ctx, pimpl_csym(fx, pr), sig, arity, lent)
          } else { () }
      }
      nproto_emit_adapters(ctx, rest, fx, decls, adapters)'''),
 ('''fn nproto_register_impls(ctx: Handle, rows: [PimplRow], decls: [Decl], tab: SymTab) : Unit / Console =
  match rows {
    [] -> ()
    [pr, ...rest] -> match pr {
      PR(_, p, t, arity, _, op, _) -> {
        let head_int = impl_head_tag(t, decls)
        if head_int < 0 { nproto_register_impls(ctx, rest, decls, tab) }
        else {
          nproto_register_one(ctx, stdlib_proto_id_int(p), stdlib_proto_op_id_int(p, op), head_int, pr, arity)
          nproto_register_impls(ctx, rest, decls, tab)''', '''fn nproto_register_impls(ctx: Handle, rows: [PimplRow], fx: FnIndex, decls: [Decl]) : Unit / Console =
  match rows {
    [] -> ()
    [pr, ...rest] -> match pr {
      PR(_, p, t, arity, _, op, _) -> {
        let head_int = impl_head_tag(t, decls)
        if head_int < 0 { nproto_register_impls(ctx, rest, fx, decls) }
        else {
          nproto_register_one(ctx, stdlib_proto_id_int(p), op, head_int, pimpl_fnptr_sym(fx, pr), arity)
          nproto_register_impls(ctx, rest, fx, decls)'''),
 ('''fn nproto_register_one(ctx: Handle, proto_id: Int, op_id: Int, head_int: Int,
                       pr: PimplRow, arity: Int) : Unit / Console = {
  let p = native_ctx_ptrt(ctx)
  let i32t = native_ctx_i32t(ctx)
  let fnv = nproto_impl_fnptr(ctx, pr, arity)''', '''fn nproto_register_one(ctx: Handle, proto_id: Int, op_id: Int, head_int: Int,
                       sym: String, arity: Int) : Unit / Console = {
  let p = native_ctx_ptrt(ctx)
  let i32t = native_ctx_i32t(ctx)
  let fnv = llvm_get_or_declare_fn(native_ctx_m(ctx), sym, llvm_fn_type_boxed(native_ctx_ptrt(ctx), arity))'''),
 ('''fn nproto_impl_fnptr(ctx: Handle, pr: PimplRow, arity: Int) : Handle / Console = match pr {
  PR(csym, _, _, _, _, _, _) -> {
    let sym = if pimpl_row_needs_adapter(pr) { user_boxed_adapter_sym(csym) } else { csym }
    llvm_get_or_declare_fn(native_ctx_m(ctx), sym, llvm_fn_type_boxed(native_ctx_ptrt(ctx), arity))
  }
}''', '''fn pimpl_fnptr_sym(fx: FnIndex, pr: PimplRow) : String =
  if pimpl_row_needs_adapter(pr) { user_boxed_adapter_sym(pimpl_csym(fx, pr)) } else { pimpl_csym(fx, pr) }'''),
])

edit("emit_native_core.kai", [
 ('''  kir_reachable(kp, (f) => ncore_is_core(f, stems) or ncore_is_own(f, runners),
                list_append(ncore_pimpl_syms(collect_pimpl_rows(decls, fns, kp.tab, kp.ids, [])), live_runners))''', '''  let fx = fnx_build(fns)
  kir_reachable(kp, (f) => ncore_is_core(f, stems) or ncore_is_own(f, runners),
                list_append(ncore_pimpl_syms(fx, collect_pimpl_rows(decls, fx, kp.ids, [])), live_runners))'''),
 ('''fn ncore_pimpl_syms(rows: [PimplRow]) : [String] = match rows {
  [] -> []
  [PR(csym, _, _, _, _, _, _), ...rest] -> [csym, ...ncore_pimpl_syms(rest)]
}''', '''fn ncore_pimpl_syms(fx: FnIndex, rows: [PimplRow]) : [String] = map(rows, (pr) => pimpl_csym(fx, pr))'''),
])
for p in ["emit_native_proto.kai", "emit_native_core.kai"]:
    s = open(p).read()
    if "import compiler.fn_index\n" not in s:
        m = re.search(r'^import compiler\.fnreg\n', s, re.M); assert m, p
        s = s[:m.end()] + "import compiler.fn_index\n" + s[m.end():]
        open(p, "w").write(s)
print("ok")
