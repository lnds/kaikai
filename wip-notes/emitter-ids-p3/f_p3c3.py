"""f_p3c3.py: step 3 of the KFn change, the native emitter. Run from stage2/compiler after f_p3c2.py."""
import sys, os
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from edk import edit, add_import

# ---------------------------------------------------------------- emit_native_fn.kai
add_import("emit_native_fn.kai", "kir", "kir_syms")
edit("emit_native_fn.kai", [
 ('''# The emitted symbol for a KFn: the entry point becomes `kai_main` (the
# runtime's `int main` calls `kai_main()` directly); every other fn keeps
# its already-mangled `sym`. Declaration, definition, and any call must
# agree, so this is the one source they all route through.
pub fn nfn_sym(f: KFn) : String = if f.entry { "kai_main" } else { f.sym }''',
  '''# The emitted symbol for a KFn: the entry point becomes `kai_main` (the
# runtime's `int main` calls `kai_main()` directly); every other fn is
# spelled by `nm`. Declaration, definition, and any call must agree, so
# this is the one source they all route through.
pub fn nfn_sym(nm: KNames, f: KFn) : String = if f.entry { "kai_main" } else { kfn_sym(nm, f) }'''),
 ('''pub fn nfn_llvm_type(ctx: Handle, f: KFn) : Handle = {
  # A frame-carrying fn takes the evidence frame `ptr` as its first param;
  # a zero slot count leaves the legacy signature unchanged.
  let nevf = native_ctx_frame_slot_count(ctx, nfn_sym(f))''',
  '''pub fn nfn_llvm_type(ctx: Handle, sym: String, f: KFn) : Handle = {
  # A frame-carrying fn takes the evidence frame `ptr` as its first param;
  # a zero slot count leaves the legacy signature unchanged.
  let nevf = native_ctx_frame_slot_count(ctx, sym)'''),
])

# ---------------------------------------------------------------- emit_native_fnix.kai
add_import("emit_native_fnix.kai", "kir", "kir_syms")
edit("emit_native_fnix.kai", [
 ('''#[doc("A KFn list plus a hash index from each fn's symbol to the fns carrying it, the program's symbol table, and each fn's evidence frame.")]
pub type NFnIndex = { ix: NameIndex[KFn], all: [KFn], tab: SymTab, ids: ProtoIds, frames: EvFrames }

#[doc("Index the KFns so a direct call resolves its callee without scanning them.")]
pub fn nfnx_build(fns: [KFn], tab: SymTab, ids: ProtoIds, frames: EvFrames) : NFnIndex =
  NFnIndex { ix: nfnx_add(fns, idx_empty()), all: fns, tab: tab, ids: ids, frames: frames }

#[doc("The instances `f` takes evidence for, in the order of its frame's slots.")]
pub fn nfnx_frame_keys(fx: NFnIndex, f: KFn) : [InstKey] = frame_slots_of(fx.frames, f.fid)

fn nfnx_add(fns: [KFn], ix: NameIndex[KFn]) : NameIndex[KFn] = match fns {
  []           -> ix
  [f, ...rest] -> nfnx_add(rest, idx_prepend(ix, f.sym, f))
}''', '''#[doc("A KFn list plus a hash index from each fn's symbol to the fns carrying it, what spells a fn, the program's symbol table, and each fn's evidence frame.")]
pub type NFnIndex = { ix: NameIndex[KSymFn], all: [KFn], nm: KNames, tab: SymTab, ids: ProtoIds, frames: EvFrames }

#[doc("Index the KFns so a direct call resolves its callee without scanning them.")]
pub fn nfnx_build(fns: [KFn], nm: KNames, ids: ProtoIds, frames: EvFrames) : NFnIndex =
  NFnIndex { ix: nfnx_add(ksym_fns(nm, fns), idx_empty()), all: fns, nm: nm, tab: nm.tab, ids: ids, frames: frames }

#[doc("The instances `f` takes evidence for, in the order of its frame's slots.")]
pub fn nfnx_frame_keys(fx: NFnIndex, f: KFn) : [InstKey] = frame_slots_of(fx.frames, kfn_fid(f))

fn nfnx_add(fns: [KSymFn], ix: NameIndex[KSymFn]) : NameIndex[KSymFn] = match fns {
  []           -> ix
  [r, ...rest] -> nfnx_add(rest, idx_prepend(ix, r.sym, r))
}'''),
 ('''fn nfnx_oldest(chain: [KFn], sym: String, found: Option[KFn]) : Option[KFn] = match chain {
  []           -> found
  [f, ...rest] -> nfnx_oldest(rest, sym, if f.sym == sym { Some(f) } else { found })
}''', '''fn nfnx_oldest(chain: [KSymFn], sym: String, found: Option[KFn]) : Option[KFn] = match chain {
  []           -> found
  [r, ...rest] -> nfnx_oldest(rest, sym, if r.sym == sym { Some(r.f) } else { found })
}'''),
])

# ---------------------------------------------------------------- emit_native_term.kai
add_import("emit_native_term.kai", "kir", "kir_syms")
edit("emit_native_term.kai", [
 ('''  KFnFfi(csym, pks, rk) -> nemit_ffi_fn(ctx, f, csym, pks, rk)''', '''  KFnFfi(csym, pks, rk) -> nemit_ffi_fn(ctx, nfn_sym(fx.nm, f), f, csym, pks, rk)'''),
 ('''fn nfn_link_visible(f: KFn, fx: NFnIndex) : Bool = f.entry or pid_is_impl(fx.ids, fnid_sym(f.fid))''',
  '''fn nfn_link_visible(f: KFn, fx: NFnIndex) : Bool = f.entry or pid_is_impl(fx.ids, fnid_sym(kfn_fid(f)))'''),
 ('''  let fnval = nemit_declare_for_abi(ctx, f, abi)
  nfn_internalize_private(f, abi, fnval, fx)''', '''  let sym = nfn_sym(fx.nm, f)
  let fnval = nemit_declare_for_abi(ctx, sym, f, abi)
  nfn_internalize_private(f, abi, fnval, fx)'''),
 ('''  ndi_subprogram(ctx, fnval, nfn_sym(f), ndi_fn_line(f.blocks))''', '''  ndi_subprogram(ctx, fnval, sym, ndi_fn_line(f.blocks))'''),
 ('''  nemit_evf_alloca(ctx, f)
  nemit_seed_for_abi(ctx, f, abi, fnval)''', '''  nemit_evf_alloca(ctx, sym)
  nemit_seed_for_abi(ctx, sym, f, abi, fnval)'''),
 ('''  nemit_fill_blocks(ctx, nfn_sym(f), f.blocks, handlers, f.ret, fx)''', '''  nemit_fill_blocks(ctx, sym, f.blocks, handlers, f.ret, fx)'''),
 ('''# Reserve the `__evf` register (a `ptr` to the evidence frame array) in the
# entry head when this fn carries a frame; no-op otherwise.
fn nemit_evf_alloca(ctx: Handle, f: KFn) : Unit / Console =
  if native_ctx_frame_slot_count(ctx, nfn_sym(f)) > 0 { nfx_reserve_named(ctx, "__evf") }''',
  '''# Reserve the `__evf` register (a `ptr` to the evidence frame array) in the
# entry head when the fn emitted as `sym` carries a frame; no-op otherwise.
fn nemit_evf_alloca(ctx: Handle, sym: String) : Unit / Console =
  if native_ctx_frame_slot_count(ctx, sym) > 0 { nfx_reserve_named(ctx, "__evf") }'''),
 ('''fn nemit_declare_for_abi(ctx: Handle, f: KFn, abi: NFnAbi) : Handle = match abi {
  NFnNormal -> llvm_get_or_declare_fn(native_ctx_m(ctx), nfn_sym(f), nfn_llvm_type(ctx, f))
  NFnLambda -> nemit_declare_thunk(ctx, f.sym)
  NFnClause -> nemit_declare_clause(ctx, f.sym, f.params)
  NFnCleanup -> nemit_declare_cleanup(ctx, f.sym)
}''', '''fn nemit_declare_for_abi(ctx: Handle, sym: String, f: KFn, abi: NFnAbi) : Handle = match abi {
  NFnNormal -> llvm_get_or_declare_fn(native_ctx_m(ctx), sym, nfn_llvm_type(ctx, sym, f))
  NFnLambda -> nemit_declare_thunk(ctx, sym)
  NFnClause -> nemit_declare_clause(ctx, sym, f.params)
  NFnCleanup -> nemit_declare_cleanup(ctx, sym)
}'''),
 ('''fn nemit_seed_for_abi(ctx: Handle, f: KFn, abi: NFnAbi, fnval: Handle) : Unit / Console = match abi {''',
  '''fn nemit_seed_for_abi(ctx: Handle, sym: String, f: KFn, abi: NFnAbi, fnval: Handle) : Unit / Console = match abi {'''),
 ('''    let evf_off = nfn_evf_extra(native_ctx_frame_slot_count(ctx, nfn_sym(f)))''', '''    let evf_off = nfn_evf_extra(native_ctx_frame_slot_count(ctx, sym))'''),
 ('''fn nemit_one_fn_thunk(ctx: Handle, f: KFn, fx: NFnIndex) : Unit / Console = {
  let sym = f.sym
  let arity = list_length(f.params)
  let tsym = c_user_thunk_sym(sym)''', '''fn nemit_one_fn_thunk(ctx: Handle, f: KFn, fx: NFnIndex) : Unit / Console = {
  let callee = nfn_sym(fx.nm, f)
  let arity = list_length(f.params)
  let tsym = c_user_thunk_sym(kfn_sym(fx.nm, f))'''),
 ('''  nemit_evf_call_push(ctx, nfn_sym(f), f, fx, ptb, ab)''', '''  nemit_evf_call_push(ctx, callee, f, fx, ptb, ab)'''),
 ('''  let r = ncall_finish(ctx, nfn_sym(f), nslot_type(ctx, f.ret), ptb, ab)''', '''  let r = ncall_finish(ctx, callee, nslot_type(ctx, f.ret), ptb, ab)'''),
])

# ---------------------------------------------------------------- emit_native_ffi.kai
edit("emit_native_ffi.kai", [
 ('''pub fn nemit_ffi_fn(ctx: Handle, f: KFn, c_symbol: String,
                    param_kinds: [KFfiKind], ret_kind: KFfiKind) : Unit / Console = {
  let arity = list_length(f.params)
  let wrapper = llvm_get_or_declare_fn(native_ctx_m(ctx), nfn_sym(f),''',
  '''pub fn nemit_ffi_fn(ctx: Handle, sym: String, f: KFn, c_symbol: String,
                    param_kinds: [KFfiKind], ret_kind: KFfiKind) : Unit / Console = {
  let arity = list_length(f.params)
  let wrapper = llvm_get_or_declare_fn(native_ctx_m(ctx), sym,'''),
])

# ---------------------------------------------------------------- emit_native.kai
add_import("emit_native.kai", "kir", "kir_syms")
edit("emit_native.kai", [
 ('''  nemit_declare_fns(ctx, all_fns, const_fns, free_fns)''', '''  let nm = kp_names(kp)
  nemit_declare_fns(ctx, nm, all_fns, const_fns, free_fns)'''),
 ('''  let fx = nfnx_build(all_fns, kp.tab, kp.ids, frames)''', '''  let fx = nfnx_build(all_fns, nm, kp.ids, frames)'''),
 ('''    NModSplitCore -> nproto_emit_adapters_of(ctx, decls, kp.tab, kp.ids, fns, nemit_syms_of(define_fns))''',
  '''    NModSplitCore -> nproto_emit_adapters_of(ctx, decls, kp.tab, kp.ids, fns, nemit_decls_of(define_fns))'''),
 ('''  NModSplitRoot -> NAdaptOnly(nemit_syms_of(define_fns))
  _             -> NAdaptAll
}

fn nemit_syms_of(fns: [KFn]) : [String] = nemit_syms_loop(fns, [])
fn nemit_syms_loop(fns: [KFn], acc: [String]) : [String] = match fns {
  [] -> list_reverse(acc)
  [f, ...rest] -> nemit_syms_loop(rest, [f.sym, ...acc])
}''', '''  NModSplitRoot -> NAdaptOnly(nemit_decls_of(define_fns))
  _             -> NAdaptAll
}

# The declarations among `fns`: an impl row is matched against them.
fn nemit_decls_of(fns: [KFn]) : [FnId] = map(fns, kfn_fid)'''),
 ('''fn nemit_declare_fns(ctx: Handle, fns: [KFn], const_fns: [FnId], free_fns: [FnId]) : Unit = match fns {''',
  '''fn nemit_declare_fns(ctx: Handle, nm: KNames, fns: [KFn], const_fns: [FnId], free_fns: [FnId]) : Unit = match fns {'''),
 ('''      let _f = llvm_get_or_declare_fn(native_ctx_m(ctx), nfn_sym(f), llvm_fn_type_boxed(native_ctx_ptrt(ctx), list_length(f.params)))
      nemit_declare_fns(ctx, rest, const_fns, free_fns)''', '''      let _f = llvm_get_or_declare_fn(native_ctx_m(ctx), nfn_sym(nm, f), llvm_fn_type_boxed(native_ctx_ptrt(ctx), list_length(f.params)))
      nemit_declare_fns(ctx, nm, rest, const_fns, free_fns)'''),
 ('''      let _d = llvm_get_or_declare_fn(native_ctx_m(ctx), nfn_sym(f), nfn_llvm_type(ctx, f))''',
  '''      let sym = nfn_sym(nm, f)
      let _d = llvm_get_or_declare_fn(native_ctx_m(ctx), sym, nfn_llvm_type(ctx, sym, f))'''),
 ('''      let _nu = if nfn_present(free_fns, f.fid) { llvm_add_nounwind(_d) } else { () }
      let _mn = if nfn_present(const_fns, f.fid) { llvm_add_memory_none(_d) } else { () }
      nemit_declare_fns(ctx, rest, const_fns, free_fns)''', '''      let _nu = if nfn_present(free_fns, kfn_fid(f)) { llvm_add_nounwind(_d) } else { () }
      let _mn = if nfn_present(const_fns, kfn_fid(f)) { llvm_add_memory_none(_d) } else { () }
      nemit_declare_fns(ctx, nm, rest, const_fns, free_fns)'''),
 ('''      let _c = nemit_declare_clause(ctx, f.sym, f.params)
      nemit_declare_fns(ctx, rest, const_fns, free_fns)''', '''      let _c = nemit_declare_clause(ctx, kfn_sym(nm, f), f.params)
      nemit_declare_fns(ctx, nm, rest, const_fns, free_fns)'''),
 ('''      let _c = nemit_declare_cleanup(ctx, f.sym)
      nemit_declare_fns(ctx, rest, const_fns, free_fns)''', '''      let _c = nemit_declare_cleanup(ctx, kfn_sym(nm, f))
      nemit_declare_fns(ctx, nm, rest, const_fns, free_fns)'''),
 ('''    _ -> nemit_declare_fns(ctx, rest, const_fns, free_fns)''', '''    _ -> nemit_declare_fns(ctx, nm, rest, const_fns, free_fns)'''),
])

# ---------------------------------------------------------------- emit_native_proto.kai: an adapter share by declaration
edit("emit_native_proto.kai", [
 ('''pub type NAdapters = NAdaptAll | NAdaptOnly([String])

fn nadapt_allows(a: NAdapters, csym: String) : Bool = match a {
  NAdaptAll        -> true
  NAdaptOnly(syms) -> list_has(syms, csym)
}''', '''pub type NAdapters = NAdaptAll | NAdaptOnly([FnId])

fn nadapt_allows(a: NAdapters, impl_fn: FnId) : Bool = match a {
  NAdaptAll       -> true
  NAdaptOnly(fns) -> any(fns, (f) => fnid_eq(f, impl_fn))
}'''),
 ('''                               only: [String]) : Unit / Console = {''', '''                               only: [FnId]) : Unit / Console = {'''),
 ('''pimpl_row_needs_adapter(pr) and nadapt_allows(adapters, pimpl_csym(fx, pr)) {''', '''pimpl_row_needs_adapter(pr) and nadapt_allows(adapters, pimpl_row_fn(pr)) {'''),
])

# ---------------------------------------------------------------- emit_native_modkey.kai
add_import("emit_native_modkey.kai", "kir", "kir_syms")
edit("emit_native_modkey.kai", [
 ('''    kir_dump_fns_pub(kp.tab, define_fns),
    "; externs\\n", nmodkey_dump_referenced_sigs(kp.fns, nmodkey_callees(define_fns))''',
  '''    kir_dump_fns_pub(kp_names(kp), define_fns),
    "; externs\\n", nmodkey_dump_referenced_sigs(kp_names(kp), kp.fns, nmodkey_callees(define_fns))'''),
 ('''fn nmodkey_dump_referenced_sigs(fns: [KFn], callees: [String]) : String =
  nmodkey_dump_loop(fns, callees, "")
fn nmodkey_dump_loop(fns: [KFn], callees: [String], acc: String) : String = match fns {
  [] -> acc
  [f, ...rest] ->
    if list_has(callees, f.sym) {
      nmodkey_dump_loop(rest, callees, concat_all([acc, kir_dump_fn_sig_pub(f), "\\n"]))
    } else {
      nmodkey_dump_loop(rest, callees, acc)
    }''', '''fn nmodkey_dump_referenced_sigs(nm: KNames, fns: [KFn], callees: [String]) : String =
  nmodkey_dump_loop(nm, fns, callees, "")
fn nmodkey_dump_loop(nm: KNames, fns: [KFn], callees: [String], acc: String) : String = match fns {
  [] -> acc
  [f, ...rest] ->
    if list_has(callees, kfn_sym(nm, f)) {
      nmodkey_dump_loop(nm, rest, callees, concat_all([acc, kir_dump_fn_sig_pub(nm, f), "\\n"]))
    } else {
      nmodkey_dump_loop(nm, rest, callees, acc)
    }'''),
])
print("step 3 ok")

s = open("emit_native_proto.kai").read()
assert s.count("import compiler.util\n") == 1
open("emit_native_proto.kai", "w").write(s.replace("import compiler.util\n", ""))
print("step 3c ok")
