import sys,re; sys.path.insert(0,sys.argv[1]); from cgarcs import *
s,a,c=parse(sys.argv[2]); total=sum(s.values())
rows=collections.Counter()
def phase(n):
    m=re.match(r'kaiu_compiler__(\w+?)__(\w+)$',n); mod=m.group(1) if m else ''
    if n in('kai_internal_drop','kai_free_rec','kai_decref'): return '8 drop of intermediate trees'
    if mod=='parse' or mod=='lex' : return '1 lex+parse (user file)'
    if mod=='typed_cut' or mod.startswith('infer') or mod=='kai_builtin_check' or mod=='refine_enforce' or mod=='call_site_check' or mod.startswith('protos_resolved') or mod=='protos_calls': return '4 infer (post-cut) + checks'
    if mod.startswith('core_reach'): return '3 core reachability cut'
    if mod=='mono_front' or mod=='fn_bounds' and 'by_sym' in n: return '5 monomorph'
    if mod in('perceus_pass','unbox_pass','unwind_operands','unique_binders','evidence_inject','cell_promote','sym_erase','emit_shared','tailfuse','resume_decls') or (mod=='emit_c' and 'tcrec' in n): return '6 perceus / lowering (post-cut)'
    if mod=='emit_c' and n.endswith('emit_program'): return '7 emit C'
    if mod=='name_report': return '7 emit C'
    if mod.startswith(('resolve','symtab','kind_resolve','kind_validate','kinds','kind_theory','pub_access','qual_head','modules','missing_export','core_syms','tparam_bind','value_scope','type_scope','effect_scope','name_uses','home_dup','extern_fn','module_table','scope_file','entry','effect_alias','core_shadow','diag_file','verdicts','fn_bounds','interp_check','const_init','extern_fn_types','doc_attr')) or (mod=='driver' and ('expand_imports' in n or 'effect_names' in n or 'core_mod_names' in n or 'tag_target' in n or 'lex_diags' in n)): return '2a resolve / scope / module checks (pre-cut, whole core)'
    return '2b desugar / surface lowering (pre-cut, whole core)'
for (x,y),v in a.items():
    if base(x)=='kaiu_compiler__driver__compile_source' and base(y)!='kaiu_compiler__driver__compile_source': rows[phase(base(y))]+=v
lc=sum(v for (x,y),v in a.items() if base(x)=='kaiu_main' and base(y)=='kaiu_compiler__driver__load_core')
rows['0 core blob decode (load_core_cached)']=lc
rest=total-sum(rows.values()); rows['9 process start, CLI, exit, unattributed']=rest
print(f"{'phase':58} {'Ir':>15} {'share':>7}")
for k in sorted(rows): print(f"{k:58} {rows[k]:15,} {100*rows[k]/total:6.1f}%")
print(f"{'TOTAL':58} {total:15,}")
