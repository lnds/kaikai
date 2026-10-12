# Attribute clang -ftime-trace backend time per function to: runtime.h, core (emitted stdlib), user, shim/glue.
import json,sys,re,collections
CORE={'char','char_unicode','io','tuple','option','result','list','string','numeric','complex','effects','protocols','array','concurrent','os','decimal','ty_os'}
def cls(fn, emitted, csrc):
    if fn not in emitted: return 'runtime.h'
    m=re.match(r'_?kaiu_([a-z0-9_]+?)__',fn)
    if fn.startswith('_kai_default_') or fn in('main','_kai_register_proto_tables') or fn.startswith('_kai_'): 
        if m and m.group(1) in CORE: return 'core'
        if fn.startswith('_kaiu_') and not m: return 'user'
        return 'glue'
    if m and m.group(1) in CORE: return 'core'
    return 'user'
trace,csrc=sys.argv[1],sys.argv[2]
src=open(csrc).read()
emitted=set(re.findall(r'^(?:static\s+)?[A-Za-z_][\w\s\*]*?\b(\w+)\s*\([^;{]*\)\s*\{',src,re.M))
lines=collections.Counter()
cur='glue'
for ln in src.split('\n'):
    m=re.match(r'^(?:static\s+)?[A-Za-z_][\w\s\*]*?\b(\w+)\s*\([^;{]*\)\s*\{',ln)
    if m: cur=cls(m.group(1),emitted,src)
    lines[cur]+=1
d=json.load(open(trace))
ev=[e for e in d['traceEvents'] if e.get('ph')=='X']
tot={e['name']:e['dur']/1e3 for e in ev if e['name'].startswith('Total')}
of=collections.Counter(); cg=collections.Counter()
for e in ev:
    det=e.get('args',{}).get('detail','')
    if e['name']=='OptFunction': of[cls(det,emitted,src)]+=e['dur']/1e3
    if e['name']=='RunPass' and False: pass
print(f"emitted lines by class: {dict(lines)}  emitted fn defs: {len(emitted)}")
print(f"ExecuteCompiler {tot.get('Total ExecuteCompiler',0):.0f} ms | Frontend {tot.get('Total Frontend',0):.0f} | Optimizer {tot.get('Total Optimizer',0):.0f} | CodeGenPasses {tot.get('Total CodeGenPasses',0):.0f}")
s=sum(of.values()); print("OptFunction share:", {k:f"{v:.0f}ms ({100*v/s:.0f}%)" for k,v in of.most_common()})
