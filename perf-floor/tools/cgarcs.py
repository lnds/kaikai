# Parse a callgrind out file into self costs and call arcs (inclusive cost per arc).
import sys,re,collections
def parse(path):
    names={}; selfc=collections.Counter(); arcs=collections.Counter(); calls=collections.Counter()
    fn=None; cfn=None; pending=False; ncalls=0
    def nm(s):
        m=re.match(r'\((\d+)\)(?: (.*))?',s)
        if m:
            if m.group(2) is not None: names[m.group(1)]=m.group(2)
            return names[m.group(1)]
        return s
    for line in open(path):
        line=line.rstrip('\n')
        if line.startswith('fn='): fn=nm(line[3:]); continue
        if line.startswith('cfn='): cfn=nm(line[4:]); continue
        if line.startswith('calls='): pending=True; ncalls=int(line[6:].split()[0]); continue
        if line and (line[0].isdigit() or line[0] in '+-*'):
            p=line.split()
            if len(p)<2: continue
            c=int(p[1])
            if pending: arcs[(fn,cfn)]+=c; calls[(fn,cfn)]+=ncalls; pending=False
            else: selfc[fn]+=c
    return selfc,arcs,calls
def base(n): return n.split("'")[0]
def incl(arcs,f):
    return sum(c for (a,b),c in arcs.items() if base(b)==f and base(a)!=f)
