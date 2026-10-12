import sys,glob; sys.path.insert(0,sys.argv[1]); from cgarcs import *
f=[x for x in glob.glob(sys.argv[2]) if x.endswith('.out')][0]
selfc,arcs,calls=parse(f); total=sum(selfc.values())
inc=collections.Counter(); sb=collections.Counter()
for (a,b),c in arcs.items():
    if base(a)!=base(b): inc[base(b)]+=c
for k,v in selfc.items(): sb[base(k)]+=v
skip={'0x000000000001f540','(below main)','__libc_start_main@@GLIBC_2.34','main','kai_worker_loop','swapcontext','kai_bootstrap_trampoline','kaiu_main','_start'}
print(f"total Ir {total:,}")
print("TOP inclusive"); n=0
for k,v in inc.most_common(200):
    if k in skip or v>total: continue
    print(f"{v:15,} {100*v/total:6.2f}%  self {sb[k]:13,}  {k}"); n+=1
    if n>=int(sys.argv[3]): break
print("TOP self")
for k,v in sb.most_common(15): print(f"{v:15,} {100*v/total:6.2f}%  {k}")
