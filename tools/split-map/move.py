#!/usr/bin/env python3
"""Split a kaikai source file into modules by moving its declarations.

  move.py plan  <file.kai> [cap]    propose a plan from the call graph (cluster.py's merge)
  move.py check <file.kai> <plan>   sizes, the module DAG, and what blocks the plan
  move.py apply <file.kai> <plan>   write the modules, re-point importers, move ledger rows
                                    (--draft: leave missing docs empty, to score a plan early)
  move.py verify <old.kai> <new.kai>...   every declaration of the old file is in one new file, unchanged

A plan is tab-separated, `#` starts a comment:

  <module>  <decl>           the module a declaration moves to
  @  <module>  <text>        the module's doc
  !  <decl>  <text>          the doc of a declaration that turns `pub` and has no comment to promote
  +  <module>  <import>      an import the reference scan cannot see (a record read through a field)

Bodies are copied untouched. A declaration turns `pub` only when another module reads it, and
the comment attached to it becomes its `#[doc]`. kaikai rejects import cycles, so `check` fails
on a plan whose modules do not form a DAG and names the references that close the cycle.
"""
import collections, os, re, subprocess, sys, textwrap

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True
import cluster

ROOT = os.path.dirname(os.path.dirname(HERE))
CENSUS = os.path.join(ROOT, "tools", "symid-census")
DECL = re.compile(r"^(pub )?(?:opaque )?(fn|type|const)\s+([A-Za-z_]\w*)")
OTHER = re.compile(r"^(?:pub )?(effect|protocol|impl|test|bench|check|unit|extern|axiom)\b")
TOKEN = re.compile(r'#\[|#[^\n]*|"""[\s\S]*?"""|"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)\'')
WORD = re.compile(r"(?<![.\w])([A-Za-z_]\w*)\b(?!\s*:(?!:))")


def mask(text):
    """Comments and literals gone; an interpolation's holes stay, they are code."""
    def one(m):
        t = m.group(0)
        if t == "#[": return t
        if t.startswith("#"): return ""
        if t.startswith('"""'): return '""' + "\n" * t.count("\n")
        if t.startswith("'"): return "'c'"
        return '"" ' + " ".join(re.findall(r"#\{([^{}]*)\}", t))
    return TOKEN.sub(one, text)


def code_loc(text):
    return sum(1 for l in text.split("\n") if l.strip() and not l.lstrip().startswith("#"))


def constructors(body):
    """The constructor names of a type definition."""
    rhs = body.split("=", 1)[1] if "=" in body else ""
    prev = None
    while prev != rhs:
        prev, rhs = rhs, re.sub(r"\([^()]*\)|\[[^\[\]]*\]|\{[^{}]*\}", "", rhs)
    return re.findall(r"(?:^|\|)\s*([A-Z]\w*)", rhs)


class Item:
    """One top-level declaration: the comment above it and its text."""
    def __init__(self, pub, kind, name, lead, body, glued):
        self.pub, self.kind, self.name, self.body, self.glued = pub, kind, name, body, glued
        blocks = "\n".join(lead).strip("\n").split("\n\n") if lead else []
        attached = lead and lead[-1].strip() != ""
        self.attached = blocks[-1].split("\n") if attached and blocks else []
        floating = blocks[:-1] if attached else blocks
        self.banners = [b for b in floating if b.strip() and "# ====" in b]
        self.floating = [b for b in floating if b.strip() and "# ====" not in b]
        self.code = mask("\n".join(body))
        self.loc = code_loc(self.code)
        self.names = [name] + (constructors(self.code) if kind == "type" else [])
        self.has_doc = any(l.startswith("#[doc") for l in self.attached)


def parse(path):
    """(header, imports, items) of a source file."""
    lines = open(path).read().rstrip("\n").split("\n")
    doc, inside = set(), False
    for i, l in enumerate(lines):
        if inside: doc.add(i)
        if l.startswith('#[doc("""'): inside = '""")]' not in l[9:]
        elif inside and l.startswith('""")]'): inside = False
    bad = [l for i, l in enumerate(lines) if OTHER.match(l) and i not in doc]
    if bad: sys.exit("move.py handles fn, type and const only; found: " + bad[0])
    imports = [i for i, l in enumerate(lines) if l.startswith("import ") and i not in doc]
    starts = [i for i, l in enumerate(lines) if DECL.match(l) and i not in doc]
    floor = imports[-1] + 1 if imports else 0

    def lead_start(s, low):
        i = s
        while i > low:
            l = lines[i - 1]
            if l.startswith('""")]'):
                while not lines[i - 1].startswith("#[") and i > low: i -= 1
                i -= 1
            elif not l.strip() or l.startswith("#"): i -= 1
            else: break
        return max(i, low)

    leads = [lead_start(s, starts[k - 1] + 1 if k else floor) for k, s in enumerate(starts)]
    items = []
    for k, s in enumerate(starts):
        end = leads[k + 1] if k + 1 < len(starts) else len(lines)
        m = DECL.match(lines[s])
        lead = lines[leads[k]:s]
        glued = k > 0 and leads[k] == s and lines[s - 1].strip() != ""
        items.append(Item(bool(m.group(1)), m.group(2), m.group(3), lead, lines[s:end], glued))
    return lines[:imports[0]] if imports else [], [lines[i].split()[1] for i in imports], items


def references(items):
    """item name -> Counter of the items it reads."""
    owner = {n: it.name for it in items for n in it.names}
    out = {}
    for it in items:
        head = it.code.split("=", 1)[0] if it.kind == "fn" else ""
        local = set(re.findall(r"\b([a-z_]\w*)\s*:", head)) | set(re.findall(r"\blet\s+([a-z_]\w*)", it.code))
        seen = collections.Counter()
        for w in WORD.findall(it.code):
            if w in owner and owner[w] != it.name and w not in local: seen[owner[w]] += 1
        out[it.name] = seen
    return out


def exports(path):
    """The names a module lets its importers read."""
    if not os.path.exists(path): return None
    _, _, items = parse_loose(path)
    names = {os.path.basename(path)[:-4]}
    for it in items:
        if it.pub: names.update(it.names)
    return names


def parse_loose(path):
    """parse, for a file that may hold declarations this tool does not move."""
    global OTHER
    keep, OTHER = OTHER, re.compile(r"^\x00")
    try: return parse(path)
    finally: OTHER = keep


def words(text):
    return set(re.findall(r"[A-Za-z_]\w*", mask(text)))


def read_plan(path):
    where, mdoc, ddoc, extra = {}, {}, {}, collections.defaultdict(list)
    for raw in open(path).read().split("\n"):
        if not raw.strip() or raw.startswith("#"): continue
        f = raw.split("\t")
        if f[0] == "@": mdoc[f[1]] = f[2]
        elif f[0] == "!": ddoc[f[1]] = f[2]
        elif f[0] == "+": extra[f[1]].append(f[2])
        else: where[f[1]] = f[0]
    return where, mdoc, ddoc, extra


def module_graph(items, refs, where):
    edges = collections.defaultdict(lambda: collections.defaultdict(list))
    for it in items:
        for callee in refs[it.name]:
            a, b = where[it.name], where[callee]
            if a != b: edges[a][b].append((it.name, callee))
    return edges


def cmd_plan(path, cap):
    _, _, items = parse(path)
    refs = references(items)
    loc = {it.name: it.loc for it in items}
    groups = cluster.split({it.name: 0 for it in items}, loc, refs, cap)
    order = {it.name: i for i, it in enumerate(items)}
    groups.sort(key=lambda g: -sum(loc[n] for n in g))
    for g in groups:
        tag = "%s_%d" % (cluster.label(g), sum(loc[n] for n in g))
        for n in sorted(g, key=order.get): print("%s\t%s" % (tag, n))
        print()


def cmd_check(path, plan):
    _, _, items = parse(path)
    where, mdoc, ddoc, _ = read_plan(plan)
    names = {it.name for it in items}
    lost = [it.name for it in items if it.name not in where]
    ghost = [n for n in where if n not in names]
    if lost or ghost: sys.exit("unplaced: %s\nunknown: %s" % (" ".join(lost), " ".join(ghost)))
    refs = references(items)
    edges = module_graph(items, refs, where)
    mods = sorted(set(where.values()))
    comps = [c for c in cluster.tarjan(mods, {m: list(edges[m]) for m in mods}) if len(c) > 1]
    size = collections.Counter()
    for it in items: size[where[it.name]] += it.loc
    depth = {}
    def d(m, trail=()):
        if m in trail: return 0
        if m not in depth: depth[m] = 1 + max([d(x, trail + (m,)) for x in edges[m]] or [0])
        return depth[m]
    for m in sorted(mods, key=lambda m: (d(m), m)):
        print("%4d LOC  layer %d  %-28s -> %s" % (size[m], d(m), m, " ".join(sorted(edges[m]))))
    for c in comps:
        print("\nCYCLE: " + " ".join(sorted(c)))
        for a in c:
            for b in c:
                for fn, callee in edges[a].get(b, [])[:6]: print("  %s.%s -> %s.%s" % (a, fn, b, callee))
    floating = [(it.name, b) for it in items for b in it.floating + it.banners]
    print("\n%d modules, %d cycles, %d detached comment blocks" % (len(mods), len(comps), len(floating)))
    return not comps


def doc_attr(text_lines):
    text = "\n".join(text_lines)
    if len(text_lines) == 1 and '"' not in text and "\\" not in text and "#{" not in text and len(text) < 160:
        return ['#[doc("%s")]' % text]
    if '"""' in text or "#{" in text or "\\" in text: return None
    return ['#[doc("""'] + text_lines + ['""")]']


def render(it, make_pub, ddoc, problems):
    out = [b for blk in it.floating for b in blk.split("\n") + [""]]
    attached, body = list(it.attached), list(it.body)
    if make_pub:
        body[0] = "pub " + body[0]
        if not it.has_doc:
            comment = [l for l in attached if l.startswith("#") and not l.startswith("#[")]
            attrs = [l for l in attached if l not in comment]
            if it.name in ddoc: doc = doc_attr([ddoc[it.name]]); comment_out = comment
            elif comment: doc = doc_attr([re.sub(r"^# ?", "", l) for l in comment]); comment_out = []
            else: doc = None; comment_out = comment
            if doc is None:
                problems.append(it.name)
                doc = ['#[doc("")]']
            attached = comment_out + doc + attrs
    return out + attached + body


def signature(it):
    """The part of a declaration its importers see: a type's whole body, a function's header."""
    if it.kind != "fn": return it.code
    depth, start = 0, it.code.index("(")
    for i in range(start, len(it.code)):
        depth += {"(": 1, ")": -1}.get(it.code[i], 0)
        if depth == 0: return it.code[:i] + re.split(r"=|\{", it.code[i:], maxsplit=1)[0]
    return it.code


def exposed(items, public):
    """`public` closed under: a `pub` declaration's signature names only `pub` types."""
    types = {it.name: it for it in items if it.kind == "type"}
    todo = [it for it in items if it.pub or it.name in public]
    while todo:
        it = todo.pop()
        for w in set(WORD.findall(signature(it))) & set(types):
            if w != it.name and not types[w].pub and w not in public:
                public.add(w)
                todo.append(types[w])
    return public


def needed_imports(text, imports, index):
    used = words(text)
    return [imp for imp in imports if index.get(imp) is None or index[imp] & used]


def cmd_apply(path, plan):
    header, imports, items = parse(path)
    where, mdoc, ddoc, extra = read_plan(plan)
    if not cmd_check(path, plan): sys.exit("the plan has an import cycle")
    src = os.path.dirname(path)
    old = os.path.basename(path)[:-4]
    pkg = "compiler"
    refs = references(items)
    mods = list(dict.fromkeys(where[it.name] for it in items))
    owner = {n: it.name for it in items for n in it.names}

    importers = []
    above = os.path.dirname(src)
    for full in sorted(os.path.join(d, f) for d in (src, above) for f in os.listdir(d)):
        if full.endswith(".kai") and full != path and re.search(r"^import %s\.%s$" % (pkg, old), open(full).read(), re.M):
            importers.append(full)
    outside = collections.Counter()
    for full in importers:
        for w in words(open(full).read()):
            if w in owner: outside[owner[w]] += 1

    read_across = set(outside)
    for it in items:
        for callee in refs[it.name]:
            if where[callee] != where[it.name]: read_across.add(callee)
    read_across = exposed(items, read_across)

    index = {imp: exports(os.path.join(src, imp.split(".")[-1] + ".kai")) for imp in imports}
    problems, texts, made_pub = [], {}, []
    for m in mods:
        mine = [it for it in items if where[it.name] == m]
        chunks = []
        for it in mine:
            turn = it.name in read_across and not it.pub
            if turn: made_pub.append(it.name)
            chunks.append(("" if it.glued and chunks else "\n") + "\n".join(render(it, turn, ddoc, problems)))
        body = "\n".join(chunks)
        sibs = sorted({where[c] for it in mine for c in refs[it.name]} - {m}, key=mods.index)
        imps = needed_imports(body, imports, index) + ["%s.%s" % (pkg, s) for s in sibs] + extra.get(m, [])
        doc = textwrap.wrap(mdoc.get(m, ""), 72) or ["TODO"]
        if not mdoc.get(m): problems.append("@" + m)
        texts[m] = '#[doc("""\n%s\n""")]\n\n%s\n%s\n' % ("\n".join(doc), "\n".join("import " + i for i in imps), body)
    if problems and "--draft" not in sys.argv: sys.exit("no doc for: " + " ".join(problems))
    if problems: print("no doc for: " + " ".join(problems))

    taken = [m for m in mods if m != old and os.path.exists(os.path.join(src, m + ".kai"))]
    if taken: sys.exit("these modules already exist: " + " ".join(taken))
    if old not in mods: os.remove(path)
    for m, text in texts.items(): open(os.path.join(src, m + ".kai"), "w").write(text)

    exported = {m: {n for it in items if where[it.name] == m and (it.pub or it.name in read_across) for n in it.names}
                for m in mods}
    for full in importers:
        text = open(full).read()
        used = words(text)
        need = ["import %s.%s" % (pkg, m) for m in mods if exported[m] & used]
        if re.search(r"\b%s\.[a-z_]" % old, mask(text)): print("QUALIFIED use of %s in %s" % (old, full))
        line = r"^import %s\.%s\n" % (pkg, old)
        open(full, "w").write(re.sub(line, "".join(n + "\n" for n in need), text, flags=re.M))
        print("re-pointed %s -> %d imports" % (os.path.basename(full), len(need)))

    move_ledgers(old, items, where, texts)
    print("wrote %d modules; %d declarations turned pub" % (len(mods), len(made_pub)))


def move_ledgers(old, items, where, texts):
    """Ledger rows are keyed by file and declaration: they follow the declaration."""
    oldf = old + ".kai"
    to = {it.name: where[it.name] + ".kai" for it in items}
    mints = sorted(m + ".kai" for m in texts)

    def rewrite(name, sep, one):
        full = os.path.join(CENSUS, name) if not name.startswith("tools/") else os.path.join(ROOT, name)
        lines = open(full).read().split("\n")
        out = []
        for l in lines:
            f = l.split(sep)
            out.extend(one(f, l) if l and not l.startswith("#") and f[0] == oldf else [l])
        head = [i for i, l in enumerate(out) if l and not l.startswith("#")]
        if head:
            rows = [l for l in out[head[0]:] if l]
            was = [l for l in lines if l and not l.startswith("#")]
            if was == sorted(was, key=lambda l: l.split(sep)[:2]): rows.sort(key=lambda l: l.split(sep)[:2])
            out = out[:head[0]] + rows + [""]
        open(full, "w").write("\n".join(out))

    by_decl = lambda f, l: [l.replace(oldf, to.get(f[1], oldf), 1)]
    for name in ("written-readers.txt", "allow.tsv", "dual-slots.txt", "pending-written.txt"):
        rewrite(name, "\t", by_decl)
    rewrite("written-mints.txt", "\t",
            lambda f, l: [l.replace(oldf, m, 1) for m in mints] if f[1] == "*" else by_decl(f, l))
    rewrite("tools/walker-catchall-baseline.txt", " ", by_decl)
    drop_unused_mint_rows(os.path.join(ROOT, "stage2", "compiler"))
    eq = os.path.join(CENSUS, "written-eq.txt")
    text = open(eq).read()
    text = re.sub(r"compiler__%s\.(\w+)" % old, lambda m: "compiler__%s.%s" % (to.get(m.group(1), oldf)[:-4], m.group(1)), text)
    open(eq, "w").write(text)


def drop_unused_mint_rows(src):
    """A whole-file mint row is kept only for a module the gate finds a mint in."""
    gate = subprocess.run([sys.executable, os.path.join(CENSUS, "gate.py"), src, "--final"], capture_output=True, text=True)
    unused = set(re.findall(r"reference by text built: (\S+) \* — listed, none found", gate.stdout))
    full = os.path.join(CENSUS, "written-mints.txt")
    rows = [l for l in open(full).read().split("\n") if l.split("\t")[:2] not in [[f, "*"] for f in unused]]
    open(full, "w").write("\n".join(rows))


def cmd_verify(old, new):
    """The move is pure: same declarations, same text, but for a `pub` in front."""
    bare = lambda it: "\n".join([re.sub(r"^pub ", "", it.body[0])] + it.body[1:]).rstrip()
    before = {it.name: bare(it) for it in parse_loose(old)[2]}
    after, twice = {}, []
    for f in new:
        for it in parse_loose(f)[2]:
            if it.name in after: twice.append(it.name)
            after[it.name] = bare(it)
    lost = sorted(set(before) - set(after))
    added = sorted(set(after) - set(before))
    changed = sorted(n for n in before if n in after and before[n] != after[n])
    for what, names in (("lost", lost), ("added", added), ("changed", changed), ("twice", twice)):
        if names: print("%s: %s" % (what, " ".join(names)))
    print("%d declarations, %d unchanged" % (len(before), len(before) - len(lost) - len(changed)))
    return not (lost or added or changed or twice)


if __name__ == "__main__":
    cmd, path = sys.argv[1], sys.argv[2]
    if cmd == "plan": cmd_plan(path, int(sys.argv[3]) if len(sys.argv) > 3 else 350)
    elif cmd == "check": sys.exit(0 if cmd_check(path, sys.argv[3]) else 1)
    elif cmd == "apply": cmd_apply(path, sys.argv[3])
    elif cmd == "verify": sys.exit(0 if cmd_verify(path, sys.argv[3:]) else 1)
    else: sys.exit(__doc__)
