import sys; sys.path.insert(0,sys.argv[1]); from cgarcs import *
selfc,arcs,calls=parse(sys.argv[2]); total=sum(selfc.values())
root=sys.argv[3]; k=int(sys.argv[4]) if len(sys.argv)>4 else 40
ch=collections.Counter(); nc=collections.Counter()
for (a,b),c in arcs.items():
    if base(a)==root and base(b)!=root: ch[base(b)]+=c; nc[base(b)]+=calls[(a,b)]
print(f"total {total:,}  {root} incl {incl(arcs,root):,}")
for b,c in ch.most_common(k): print(f"{c:15,} {100*c/total:6.2f}%  calls={nc[b]:<6} {b}")
