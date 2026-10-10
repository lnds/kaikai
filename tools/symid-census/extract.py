#!/usr/bin/env python3
"""Census extractor: every place stage2/compiler reads a name out of identity data.

Families:
  arm     a pattern binds (or tests a literal against) the name slot of an identity node
  mint    an identity node is constructed with a literal name, or `mk_ref` is given one
  acc     a name accessor is called (rl_name, ty_ref_qual, hs_display, .eff, ...)
  lit     a string literal is compared (`==`, `!=`) or matched as an arm; the node column gives its shape
Output: TSV  file  line  fn  family  node  text
"""
import os, re, sys, json

SRC = sys.argv[1]

# node -> (arity, name-slot indices)
NODES = {
    "TyCon": (4, [0, 1]), "TyName": (3, [1]), "TyRef": (2, [0]),
    "ERecordLit": (3, [1]), "PVariantRecord": (3, [1]), "PVariant": (3, [0, 1]),
    "ESym": (3, [2]), "EVar": (1, [0]), "EHandle": (8, [1]), "RL": (3, [0]),
    "EModCall": (2, [0, 1]),
}
ACCESSORS = ["rl_name", "rl_names", "ty_ref_qual", "hs_display", "hs_spell", "es_spell",
             "ts_spell", "sym_name", "sym_home"]

def strip_comment(line):
    out, q = [], False
    i = 0
    while i < len(line):
        c = line[i]
        if c == '"' and (i == 0 or line[i-1] != '\\'):
            q = not q
        if c == '#' and not q:
            break
        out.append(c); i += 1
    return "".join(out)

def split_args(s):
    out, d, cur, q = [], 0, "", False
    for i, ch in enumerate(s):
        if ch == '"' and (i == 0 or s[i-1] != '\\'):
            q = not q
        if not q:
            if ch in "([{": d += 1
            if ch in ")]}": d -= 1
            if ch == "," and d == 0:
                out.append(cur.strip()); cur = ""; continue
        cur += ch
    if cur.strip(): out.append(cur.strip())
    return out

def balanced(s, i):
    d, q = 0, False
    for j in range(i, len(s)):
        ch = s[j]
        if ch == '"' and s[j-1] != '\\': q = not q
        if q: continue
        if ch in "([{": d += 1
        elif ch in ")]}":
            d -= 1
            if d == 0: return j
    return -1

def lit_shape(t):
    if t == "": return "empty"
    if re.fullmatch(r'[a-z_][A-Za-z0-9_]*', t): return "lower"
    if re.fullmatch(r'[^A-Za-z0-9_]+', t): return "symbol"
    return "other"

FN = re.compile(r'^\s*(?:pub\s+)?fn\s+([a-z_][A-Za-z0-9_]*)')
IDENT = re.compile(r'^[a-z_][A-Za-z0-9_]*$')
LIT = re.compile(r'^"[^"]*"$')

rows = []
for fname in sorted(os.listdir(SRC)):
    if not fname.endswith(".kai"): continue
    mod = fname[:-4]
    lines = open(os.path.join(SRC, fname)).read().split("\n")
    in_doc = False
    cur_fn = "<top>"
    for ln, raw in enumerate(lines, 1):
        if '"""' in raw:
            if raw.count('"""') % 2 == 1:
                in_doc = not in_doc
            continue
        if in_doc: continue
        m = FN.match(raw)
        if m: cur_fn = m.group(1)
        code = strip_comment(raw)
        if not code.strip(): continue
        head = code[:code.index("->")] if "->" in code else None
        for node, (n, slots) in NODES.items():
            for mm in re.finditer(r'\b' + node + r'\(', code):
                i = mm.end() - 1; j = balanced(code, i)
                if j < 0: continue
                args = split_args(code[i+1:j])
                if len(args) != n: continue
                in_head = head is not None and mm.start() < len(head)
                vals = [args[k] for k in slots]
                if in_head and any(v != "_" and (IDENT.match(v) or LIT.match(v) or v.startswith("Some(")) for v in vals):
                    rows.append((fname, ln, cur_fn, "arm", node, code.strip()[:200]))
                elif not in_head and any(LIT.match(v) or (v.startswith("Some(\"")) for v in vals):
                    rows.append((fname, ln, cur_fn, "mint", node, code.strip()[:200]))
        for a in ACCESSORS:
            if re.search(r'(^|[^A-Za-z0-9_])' + a + r'\(', code) and not FN.match(code):
                rows.append((fname, ln, cur_fn, "acc", a, code.strip()[:200]))
        if re.search(r'\bmk_ref\("', code) and not FN.match(code):
            rows.append((fname, ln, cur_fn, "mint", "mk_ref", code.strip()[:200]))
        if re.search(r'\.eff\b', code):
            rows.append((fname, ln, cur_fn, "acc", ".eff", code.strip()[:200]))
        if re.search(r'(==|!=)\s*"([A-Z][A-Za-z0-9_.]*|__[A-Za-z0-9_|]*)"|"([A-Z][A-Za-z0-9_.]*|__[A-Za-z0-9_|]*)"\s*(==|!=)', code):
            rows.append((fname, ln, cur_fn, "lit", "==", code.strip()[:200]))
        elif re.search(r'string_starts_with\([^)]*"__', code):
            rows.append((fname, ln, cur_fn, "lit", "__prefix", code.strip()[:200]))
        else:
            m = re.search(r'(?:==|!=)\s*"([^"]*)"|"([^"]*)"\s*(?:==|!=)', code)
            if m:
                rows.append((fname, ln, cur_fn, "lit", "==" + lit_shape(m.group(1) if m.group(1) is not None else m.group(2)), code.strip()[:200]))
            elif re.match(r'\s*"[^"]*"\s*->', code):
                rows.append((fname, ln, cur_fn, "lit", "match", code.strip()[:200]))

w = sys.stdout
for r in rows:
    w.write("\t".join(str(x) for x in r) + "\n")
