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
        names = re.findall(r'^\s*import\s+([A-Za-z_][\w.]*)', fh.read(), re.M)
    imports[mod] = [n for n in names if n in paths]
    dangling += [(mod, n) for n in names if n not in paths]

seen, order, stack = set(), [], []
cycles = []

def visit(mod):
    if mod in seen:
        if mod in stack:
            cycles.append(' -> '.join(stack[stack.index(mod):] + [mod]))
        return
    seen.add(mod)
    stack.append(mod)
    for dep in imports[mod]:
        visit(dep)
    stack.pop()
    order.append(mod)

visit('main')

fail = 0
for mod, name in dangling:
    print(f"FAIL stage2-graph: {mod} imports `{name}`, which resolves to no file")
    fail = 1
for c in cycles:
    print(f"FAIL stage2-graph: import cycle {c}")
    fail = 1
for mod in sorted(set(paths) - seen):
    print(f"FAIL stage2-graph: {mod} is not reachable from main.kai")
    fail = 1

if fail:
    sys.exit(1)
print(f"stage2-graph OK - {len(order)} modules reachable, no cycles, no dangling imports")
PY
