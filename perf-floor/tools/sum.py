import sys,collections
log,out=sys.argv[1],sys.argv[2]
n=collections.Counter(); w=collections.Counter(); c=collections.Counter()
ck=collections.Counter(); ckc=collections.Counter(); lines=[]; impl=[]
for l in open(log):
    p=l.split(); k=p[0]; d=dict(x.split('=',1) for x in p[2:] if '=' in x)
    n[k]+=1; w[k]+=int(p[1]); c[k]+=int(d.get('cpu',0))
    if k=='cc':
        kind=('compile+link' if d['link']=='1' and d['src']=='1' else 'link-only' if d['link']=='1' else 'compile-only' if d['src']=='1' else 'other')
        kind+=f" {d['opt']}"+(' sep' if d['sep']=='1' else '')+(' san' if d['san']=='1' else '')
        ck[kind]+=1; ckc[kind]+=int(d['cpu'])
        if d['src']=='1': lines.append(int(d['lines'])); impl.append(int(d['impls']))
txt=open(out).read().split('\n')
el=[int(x.split('elapsed_ns=')[1]) for x in txt if 'elapsed_ns=' in x][0]/1e9
ut=[x for x in txt if x.count('m')==2 and 's ' in x][-1].split()
def sec(s): m,r=s.split('m'); return int(m)*60+float(r.rstrip('s'))
cpu_total=sec(ut[0])+sec(ut[1])
print(f"elapsed {el:.0f} s; CPU of the whole make tree {cpu_total:.0f} s")
acc=0
for k in ['kaic2','kaic1','cc','run']:
    if n[k]: acc+=c[k]/1e9; print(f"  {k:6} n={n[k]:4}  cpu={c[k]/1e9:7.1f}s ({100*c[k]/1e9/cpu_total:4.1f}%)  mean cpu={c[k]/n[k]/1e6:6.0f}ms  wall-sum={w[k]/1e9:7.1f}s")
print(f"  other (make, sh, bin/kai, diff, python, ...) cpu={cpu_total-acc:.1f}s ({100*(cpu_total-acc)/cpu_total:.1f}%)")
for k in sorted(ck): print(f"    cc {k:28} n={ck[k]:4} cpu={ckc[k]/1e9:6.1f}s mean={ckc[k]/ck[k]/1e6:5.0f}ms")
if lines:
    s=sorted(lines); print(f"  emitted .c lines per cc source: median {s[len(s)//2]}, p10 {s[len(s)//10]}, p90 {s[9*len(s)//10]}")
    print(f"  cc sources registering >=80 protocol impls: {sum(1 for x in impl if x>=80)}/{len(impl)}; registering 0: {sum(1 for x in impl if x==0)}")
