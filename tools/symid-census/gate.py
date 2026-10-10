#!/usr/bin/env python3
"""Names stay out of identity: the checks behind tools/symid-names-gate.sh.

  gate.py <compiler-dir> [--final]

Reads the ledgers beside this file and fails when the sources hold a name read they do not list:

  allow.tsv            file, fn, family, node, legit, pending, why
                       Every place a name is read out of identity-bearing data or a string literal is
                       compared (extract.py finds them). `legit` rows stay; `pending` rows are still to be
                       converted. A group may not grow past legit + pending, and a group the ledger does
                       not name fails.
  dual-slots.txt       Type definitions that hold a String beside an id slot.
  written-readers.txt  Files allowed to call `written_text`, the one way characters leave a `Written`.

With --final a pending row or a dual slot not marked `keep` fails too: nothing is left to convert.
"""
import collections, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = sys.argv[1]
FINAL = "--final" in sys.argv[2:]
fails = []

def ledger(name):
    path = os.path.join(HERE, name)
    return [l.rstrip("\n") for l in open(path) if l.strip() and not l.startswith("#")] if os.path.exists(path) else []

# --- name reads and literal comparisons ---------------------------------
found = collections.Counter()
where = collections.defaultdict(list)
out = subprocess.run([sys.executable, os.path.join(HERE, "extract.py"), SRC], capture_output=True, text=True, check=True).stdout
for l in out.splitlines():
    f = l.split("\t")
    k = (f[0], f[2], f[3], f[4])
    found[k] += 1
    where[k].append(f[1])
allowed, pending = {}, 0
for l in ledger("allow.tsv"):
    f = l.split("\t")
    if f[0] == "file": continue
    allowed[(f[0], f[1], f[2], f[3])] = (int(f[4]), int(f[5]))
    pending += int(f[5])
for k, n in sorted(found.items()):
    legit, pend = allowed.get(k, (0, 0))
    if n > legit + pend:
        fails.append("name read not in the ledger: %s %s() %s/%s — %d found, %d listed (lines %s)"
                     % (k[0], k[1], k[2], k[3], n, legit + pend, ",".join(where[k])))
if FINAL and pending:
    fails.append("%d name reads are still listed as pending in allow.tsv" % pending)

# --- a String beside an id ----------------------------------------------
ID = re.compile(r"\b(SymId|TyRef|FnId)\b")
def dual_slots():
    got = []
    for fn in sorted(os.listdir(SRC)):
        if not fn.endswith(".kai") or fn.endswith("_test.kai"): continue
        src = open(os.path.join(SRC, fn)).read().split("\n")
        for i, l in enumerate(src):
            code = l.split("#")[0]
            if re.match(r"\s*(\||=|pub type|type)", code):
                for m in re.finditer(r"(?:^|[|=]\s*)([A-Z]\w*)\(([^()]*(?:\([^()]*\)[^()]*)*)\)", code):
                    pay = m.group(2)
                    if ID.search(pay) and re.search(r"\bString\b", re.sub(r"Option\[String\]|\[String\]", "", pay)):
                        got.append("%s\t%s" % (fn, m.group(1)))
            r = re.match(r"\s*(?:pub )?type (\w+)\s*=\s*\{(.*)", code)
            if r:
                body, j = r.group(2), i
                while "}" not in body and j + 1 < len(src):
                    j += 1; body += " " + src[j].split("#")[0]
                if ID.search(body) and re.search(r":\s*String\b", body):
                    got.append("%s\t%s" % (fn, r.group(1)))
    return got
listed = {}
for l in ledger("dual-slots.txt"):
    f = l.split("\t")
    listed[(f[0], f[1])] = f[2] if len(f) > 2 else "pending"
for d in dual_slots():
    k = tuple(d.split("\t"))
    if k not in listed:
        fails.append("a String sits beside an id slot in %s: %s" % k)
    elif FINAL and listed[k] != "keep":
        fails.append("dual slot still pending in %s: %s" % k)

# --- the one accessor ---------------------------------------------------
readers = set(ledger("written-readers.txt"))
for fn in sorted(os.listdir(SRC)):
    if fn.endswith(".kai") and fn not in readers and re.search(r"\bwritten_text\(", open(os.path.join(SRC, fn)).read()):
        fails.append("written_text is read outside the ledger: %s" % fn)

for m in fails: print("symid-names FAIL — " + m)
print("symid-names: %d name reads (%d pending), %d dual slots listed" % (sum(found.values()), pending, len(listed)))
sys.exit(1 if fails else 0)
