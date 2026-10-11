"""Propose a split of a kaikai source file into modules.

kaikai forbids import cycles, so the split must be a DAG of modules: every
strongly connected component of the call graph stays whole, and two groups
merge only if the merge creates no cycle. Groups merge greedily by call
weight under a LOC cap."""
import heapq
import re
import sys
from collections import Counter, defaultdict

sys.setrecursionlimit(100000)

DECL = re.compile(r'^(?:pub |priv )?(fn|type|const|effect|protocol|impl|test|bench|check)\b\s*([A-Za-z_][A-Za-z0-9_]*)?')
IDENT = re.compile(r'\b([a-z_][A-Za-z0-9_]*)\b')


def decls(lines):
    starts = []
    for i, line in enumerate(lines):
        m = DECL.match(line)
        if m:
            starts.append((m.group(1), m.group(2) or f'<{m.group(1)}@{i + 1}>', i))
    out = []
    for k, (kind, name, s) in enumerate(starts):
        e = starts[k + 1][2] if k + 1 < len(starts) else len(lines)
        while e > s + 1 and (not lines[e - 1].strip() or lines[e - 1].lstrip().startswith('#')):
            e -= 1
        out.append((kind, name, s, e))
    return out


def code_loc(lines, s, e):
    return sum(1 for l in lines[s:e] if l.strip() and not l.lstrip().startswith('#'))


def strip_strings(text):
    return re.sub(r'"(?:[^"\\]|\\.)*"', '""', text)


def build(path):
    lines = open(path).read().split('\n')
    ds = decls(lines)
    fns = {n: (s, e) for k, n, s, e in ds if k == 'fn'}
    loc = {n: code_loc(lines, s, e) for n, (s, e) in fns.items()}
    calls = defaultdict(Counter)
    for n, (s, e) in fns.items():
        text = strip_strings('\n'.join(lines[s:e]))
        text = text.split('\n', 1)[1] if '\n' in text else text.split('=', 1)[-1]
        for ident in IDENT.findall(text):
            if ident in fns and ident != n:
                calls[n][ident] += 1
    other = sum(code_loc(lines, s, e) for k, n, s, e in ds if k != 'fn')
    return fns, loc, calls, other


def tarjan(nodes, out):
    idx, low, on, st, res, c = {}, {}, set(), [], [], [0]

    def visit(v):
        idx[v] = low[v] = c[0]
        c[0] += 1
        st.append(v)
        on.add(v)
        for w in out[v]:
            if w not in idx:
                visit(w)
                low[v] = min(low[v], low[w])
            elif w in on:
                low[v] = min(low[v], idx[w])
        if low[v] == idx[v]:
            comp = []
            while True:
                w = st.pop()
                on.discard(w)
                comp.append(w)
                if w == v:
                    break
            res.append(comp)

    for v in nodes:
        if v not in idx:
            visit(v)
    return res


def split(fns, loc, calls, cap):
    comps = tarjan(list(fns), calls)
    of = {n: i for i, c in enumerate(comps) for n in c}
    members = {i: list(c) for i, c in enumerate(comps)}
    size = {i: sum(loc[n] for n in c) for i, c in enumerate(comps)}
    succ = defaultdict(set)
    pred = defaultdict(set)
    weight = defaultdict(Counter)
    for a, cs in calls.items():
        for b, w in cs.items():
            x, y = of[a], of[b]
            if x == y:
                continue
            succ[x].add(y)
            pred[y].add(x)
            weight[x][y] += w
            weight[y][x] += w

    def reaches(src, dst):
        stack = [s for s in succ[src] if s != dst]
        seen = set(stack)
        while stack:
            v = stack.pop()
            if v == dst:
                return True
            for w in succ[v]:
                if w not in seen:
                    seen.add(w)
                    stack.append(w)
        return False

    def score(a, b):
        return weight[a][b] / ((size[a] + 1) * (size[b] + 1)) ** 0.5

    heap = [(-score(a, b), a, b) for a in weight for b in weight[a] if a < b]
    heapq.heapify(heap)
    alive = set(members)
    while heap:
        neg, a, b = heapq.heappop(heap)
        if a not in alive or b not in alive or weight[a][b] == 0:
            continue
        if abs(-neg - score(a, b)) > 1e-12 or size[a] + size[b] > cap:
            continue
        if reaches(a, b) or reaches(b, a):
            continue
        alive.discard(b)
        size[a] += size[b]
        members[a] += members.pop(b)
        for s in succ.pop(b, set()):
            pred[s].discard(b)
            if s != a:
                succ[a].add(s)
                pred[s].add(a)
        for p in pred.pop(b, set()):
            succ[p].discard(b)
            if p != a:
                pred[a].add(p)
                succ[p].add(a)
        succ[a].discard(a)
        pred[a].discard(a)
        for c, w in weight.pop(b).items():
            weight[c].pop(b, None)
            if c != a:
                weight[a][c] += w
                weight[c][a] += w
        weight[a].pop(a, None)
        for c in weight[a]:
            x, y = (a, c) if a < c else (c, a)
            heapq.heappush(heap, (-score(x, y), x, y))
    return [members[k] for k in alive]


def label(names):
    parts = Counter()
    for n in names:
        toks = n.split('_')
        for i in range(1, min(3, len(toks)) + 1):
            parts['_'.join(toks[:i])] += 1
    return max(parts.items(), key=lambda kv: (kv[1] * len(kv[0].split('_')) ** 0.5, kv[0]))[0]


def report(path, cap, verbose):
    fns, loc, calls, other = build(path)
    groups = split(fns, loc, calls, cap)
    where = {n: i for i, g in enumerate(groups) for n in g}
    edges = [(a, b, w) for a, cs in calls.items() for b, w in cs.items()]
    total = sum(w for _, _, w in edges)
    cut = sum(w for a, b, w in edges if where[a] != where[b])
    sizes = sorted(((sum(loc[n] for n in g), len(g), label(g), sorted(g, key=lambda n: -loc[n])) for g in groups), reverse=True)
    over = [s for s in sizes if s[0] > cap]
    small = sum(1 for s in sizes if s[0] < 40)
    print(f'# {path.split("/")[-1]}: {len(fns)} fns, {sum(loc.values())} fn LOC (+{other} other) -> '
          f'{len(groups)} groups, {len(over)} over {cap} LOC (call cycles), {small} under 40 LOC; '
          f'cross-group calls {100 * cut / max(total, 1):.0f}%')
    for s, n, lab, names in sizes:
        if s >= 40 or verbose:
            mark = '  OVER CAP: one call cycle, cannot be split by moving' if s > cap else ''
            shown = names if verbose else names[:3]
            print(f'  {s:5d} LOC {n:4d} fns  {lab:28s} {", ".join(shown)}{mark}')


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if a != '-v']
    cap = int(args[0])
    for p in args[1:]:
        report(p, cap, '-v' in sys.argv)
        print()
