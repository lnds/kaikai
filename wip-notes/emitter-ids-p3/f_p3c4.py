"""f_p3c4.py: step 4 of the KFn change, the core partition. Run from stage2/compiler after f_p3c3.py."""
import sys, os
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from edk import edit, add_import

add_import("emit_native_core.kai", "kir", "kir_syms")
add_import("emit_native_core.kai", "kir", "enc_unit")
edit("emit_native_core.kai", [
 ('''  let core_fns = ncore_core_fns(emit_fns, stems)''', '''  let nm = kp_names(kp)
  let core_fns = ncore_core_fns(nm, emit_fns, stems)'''),
 ('''                                  ncore_user_fns(emit_fns, stems), obj_path, co)''', '''                                  ncore_user_fns(nm, emit_fns, stems), obj_path, co)'''),
 ('''  let user = ncore_user_fns(kp.fns, ncore_stems(core_names))''', '''  let user = ncore_user_fns(kp_names(kp), kp.fns, ncore_stems(core_names))'''),
 ('''  let fx = fnx_build(fns)
  kir_reachable(kp, (f) => ncore_is_core(f, stems) or ncore_is_own(f, runners),''', '''  let fx = fnx_build(fns)
  let nm = kp_names(kp)
  kir_reachable(kp, (f) => ncore_is_core(nm, f, stems) or ncore_is_own(nm, f),'''),
 ('''fn ncore_is_own(f: KFn, runners: [String]) : Bool = match f.kind {
  KFnFfi(_, _, _) -> true
  KFnNormal -> match f.mo {
    None    -> not (ncore_is_mono(f.sym)) and not (list_has(runners, f.sym))
    Some(_) -> false
  }
  _ -> false
}''', '''fn ncore_is_own(nm: KNames, f: KFn) : Bool = match f.kind {
  KFnFfi(_, _, _) -> true
  KFnNormal -> match f.mo {
    None    -> ncore_is_decl(f.unit) and not (ncore_is_mono(kfn_sym(nm, f)))
    Some(_) -> false
  }
  _ -> false
}

fn ncore_is_decl(u: EncUnit) : Bool = match u {
  EuFn(_) -> true
  _       -> false
}'''),
 ('''fn ncore_core_fns(fns: [KFn], stems: [String]) : [KFn] =
  filter(fns, (f) => ncore_is_core(f, stems))

fn ncore_user_fns(fns: [KFn], stems: [String]) : [KFn] =
  filter(fns, (f) => not (ncore_is_core(f, stems)))''', '''fn ncore_core_fns(nm: KNames, fns: [KFn], stems: [String]) : [KFn] =
  filter(fns, (f) => ncore_is_core(nm, f, stems))

fn ncore_user_fns(nm: KNames, fns: [KFn], stems: [String]) : [KFn] =
  filter(fns, (f) => not (ncore_is_core(nm, f, stems)))'''),
 ('''fn ncore_is_core(f: KFn, stems: [String]) : Bool =
  if ncore_is_mono(f.sym) { false }
  else { match f.mo {
    Some(m) -> list_has(stems, ncore_dots(m, 0, string_length(m), ""))
    None -> match f.kind {
      KFnLambda -> ncore_lam_is_core(f.sym, stems)
      _         -> false
    }
  } }''', '''fn ncore_is_core(nm: KNames, f: KFn, stems: [String]) : Bool = ncore_sym_is_core(kfn_sym(nm, f), f, stems)

fn ncore_sym_is_core(sym: String, f: KFn, stems: [String]) : Bool =
  if ncore_is_mono(sym) { false }
  else { match f.mo {
    Some(m) -> list_has(stems, ncore_dots(m, 0, string_length(m), ""))
    None -> match f.kind {
      KFnLambda -> ncore_lam_is_core(sym, stems)
      _         -> false
    }
  } }'''),
])
print("step 4 ok")
