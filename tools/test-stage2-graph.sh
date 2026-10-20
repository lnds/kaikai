#!/usr/bin/env bash
# Every `stage2/compiler/*.kai` must be reachable from `main.kai` through
# the import graph, and every `import` must name a file that exists. The
# graph is what decides compilation order, so an unreachable module is
# silently absent from the compiler rather than a build error.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

python3 - "$ROOT/stage2" <<'PY'
import os, re, sys

root = sys.argv[1]

def modname(path):
    return os.path.relpath(path, root)[:-4].replace('/', '.')

paths = {}
for base, _, files in os.walk(os.path.join(root, 'compiler')):
    for f in files:
        if f.endswith('.kai'):
            p = os.path.join(base, f)
            paths[modname(p)] = p
paths['main'] = os.path.join(root, 'main.kai')

imports = {}
dangling = []
for mod, path in paths.items():
    with open(path) as fh:
        names = re.findall(r'^\s*import\s+([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*)', fh.read(), re.M)
    imports[mod] = [n for n in names if n in paths]
    dangling += [(mod, n) for n in names if n not in paths]

seen, order = set(), []

def visit(mod):
    if mod in seen:
        return
    seen.add(mod)
    for dep in imports[mod]:
        visit(dep)
    order.append(mod)

visit('main')

def cyclic_groups():
    index, low, on, stack, out = {}, {}, set(), [], []
    def strong(v):
        index[v] = low[v] = len(index)
        stack.append(v)
        on.add(v)
        for w in imports[v]:
            if w not in index:
                strong(w)
                low[v] = min(low[v], low[w])
            elif w in on:
                low[v] = min(low[v], index[w])
        if low[v] == index[v]:
            comp = []
            while True:
                w = stack.pop()
                on.discard(w)
                comp.append(w)
                if w == v:
                    break
            if len(comp) > 1 or v in imports[v]:
                out.append(frozenset(comp))
    for v in sorted(seen):
        if v not in index:
            strong(v)
    return out

# A cycle is allowed only as a whole group named in the allowlist: one
# line per group, `name: member member ...` (module names under compiler/).
declared = {}
allowlist = os.path.join(root, '..', 'tools', 'stage2-graph-sccs.txt')
if os.path.exists(allowlist):
    with open(allowlist) as fh:
        for line in fh:
            line = line.split('#')[0].strip()
            if line:
                name, _, members = line.partition(':')
                declared[frozenset('compiler.' + m for m in members.split())] = name.strip()

found = cyclic_groups()
fail = 0
for mod, name in dangling:
    print(f"FAIL stage2-graph: {mod} imports `{name}`, which resolves to no file")
    fail = 1
for comp in found:
    if comp not in declared:
        print(f"FAIL stage2-graph: import cycle among {', '.join(sorted(comp))} is not a group declared in tools/stage2-graph-sccs.txt")
        fail = 1
for comp, name in declared.items():
    if comp not in found:
        print(f"FAIL stage2-graph: declared group `{name}` is no longer exactly one import cycle")
        fail = 1
for mod in sorted(set(paths) - seen):
    print(f"FAIL stage2-graph: {mod} is not reachable from main.kai")
    fail = 1

if fail:
    sys.exit(1)
print(f"stage2-graph OK - {len(order)} modules reachable, {len(declared)} declared cycle group(s), no other cycles, no dangling imports")
PY
