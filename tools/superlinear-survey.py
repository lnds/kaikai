#!/usr/bin/env python3
"""Flag superlinear list use in kaikai compiler sources.

Shapes (all require the O(n) operand to be re-evaluated per element of
some walk; the walk may live in this fn or in its caller):

  SPINE_COPY_SELF  list_append(xs, _) in a fn that walks or recurses:
                   the copied spine is carried to the next step -> O(n^2)
  SPINE_COPY_CALL  the same, in a state-threading helper that does not
                   itself loop but is reached from one           -> O(n^2)
  NESTED_SCAN      list_has/list_index over B inside a walk over A

Bound classification is NOT done here: a shape is a defect only if the
list grows with the input program. Measured on this compiler, about one
flagged function in fifty is; the inventory in
docs/superlinear-list-survey.md records which, and the bound that clears
each of the rest.

Usage:  QSURVEY_JSON=out.json tools/superlinear-survey.py stage2/compiler/*.kai
"""
import re,sys,os,json,collections

fn_re=re.compile(r'^(pub\s+)?fn\s+([a-zA-Z_][A-Za-z0-9_]*)')
SCAN=('list_has','list_index')

def split_fns(p):
    L=open(p,encoding='utf-8',errors='replace').read().split('\n')
    cur=None;out=[]
    for i,l in enumerate(L):
        m=fn_re.match(l)
        if m:
            if cur:out.append(cur)
            cur=[m.group(2),i+1,[]]
        if cur:cur[2].append(l)
    if cur:out.append(cur)
    return out

def sig(b):
    s=[]
    for l in b:
        s.append(l)
        if re.search(r'\)\s*:',l):break
    return ' '.join(s)

FNS={};CALLERS=collections.defaultdict(set)
def load(paths):
    for p in paths:
        for n,ln,b in split_fns(p):
            t='\n'.join(b)
            FNS[n]=dict(file=os.path.basename(p),line=ln,body=b,text=t,
                lp=set(re.findall(r'([a-zA-Z_][A-Za-z0-9_]*)\s*:\s*\[',sig(b))),
                walks=bool(re.search(r'\[\s*[a-zA-Z_][A-Za-z0-9_]*\s*,\s*\.\.\.',t)),
                rec=len(re.findall(r'\b'+re.escape(n)+r'\s*\(',t))>1)
    for n,f in FNS.items():
        for c in set(re.findall(r'\b([a-zA-Z_][A-Za-z0-9_]*)\s*\(',f['text'])):
            if c in FNS and c!=n: CALLERS[c].add(n)

def called_from_walk(n, depth=3):
    """Reachable from a walking/recursive frame within `depth` call levels.
    Mutual recursion (resolve_module <-> resolve_imports) hides the loop
    from a single-level test, so the search is transitive."""
    seen=set(); frontier={n}
    for _ in range(depth):
        nxt=set()
        for x in frontier:
            for c in CALLERS.get(x,()):
                if c in seen: continue
                seen.add(c)
                if FNS[c]['walks'] or FNS[c]['rec']: return True
                nxt.add(c)
        frontier=nxt
    return False

def analyse():
    out=[]
    for n,f in FNS.items():
        for i,l in enumerate(f['body']):
            code=l.split('#')[0]
            for m in re.finditer(r'list_append\(\s*([a-zA-Z_][A-Za-z0-9_.]*)',code):
                # The left operand's spine is copied. It matters when that
                # spine is carried across iterations: either this fn walks
                # (self-accumulation) or it is a state-threading helper
                # invoked from a walk.
                kind=('SPINE_COPY_SELF' if (f['rec'] or f['walks'])
                      else ('SPINE_COPY_CALL' if called_from_walk(n) else None))
                if kind: out.append(dict(kind=kind,fn=n,file=f['file'],
                    line=f['line']+i,op='list_append',scanned=m.group(1),src=l.strip()))
            if f['rec'] or f['walks']:
                for op in SCAN:
                    for m in re.finditer(re.escape(op)+r'\(\s*([a-zA-Z_][A-Za-z0-9_.]*)',code):
                        out.append(dict(kind='NESTED_SCAN',fn=n,file=f['file'],
                            line=f['line']+i,op=op,scanned=m.group(1),src=l.strip()))
    # dedupe by (fn,line,scanned)
    seen={}
    for x in out: seen.setdefault((x['fn'],x['line'],x['scanned']),x)
    return list(seen.values())

if __name__=='__main__':
    load(sys.argv[1:])
    fs=analyse()
    out=os.environ.get('QSURVEY_JSON')
    if out: json.dump(fs, open(out,'w'), indent=1)
    print('candidates:',len(fs),dict(collections.Counter(x['kind'] for x in fs)))
