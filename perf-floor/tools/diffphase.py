import sys; sys.path.insert(0,sys.argv[1]); from cgarcs import *
def ch(f,root):
    s,a,c=parse(f); r=collections.Counter()
    for (x,y),v in a.items():
        if base(x)==root and base(y)!=root: r[base(y)]+=v
    return r,sum(s.values())
A,ta=ch(sys.argv[2],sys.argv[4]); B,tb=ch(sys.argv[3],sys.argv[4])
print(f"totals {ta:,} -> {tb:,}  delta {tb-ta:,}")
for k in sorted(set(A)|set(B),key=lambda k:-(A[k]))[:int(sys.argv[5])]:
    print(f"{A[k]:13,} {B[k]:13,} x{(B[k]/A[k] if A[k] else 0):5.2f}  {k}")
