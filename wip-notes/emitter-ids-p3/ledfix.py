"""ledfix.py <repo-root> [file fn reasons]...: settle the written-readers ledger with the gate's --final report.
A row the gate calls stale with 0 found is deleted; a row with fewer found than listed is rewritten when the
new reasons are given on the command line as `file:fn=REASONS` (e.g. emit_c.kai:emit_check_param_rerecord=E=1),
else reported. New readers are added from the same `file:fn=REASONS` arguments. Stale literal rows of allow.tsv
with 0 found are deleted."""
import sys, re, subprocess, os
sys.dont_write_bytecode = True
root = sys.argv[1]
given = {}
for a in sys.argv[2:]:
    k, v = a.split("=", 1); f, fn = k.split(":"); given[(f, fn)] = v.replace(",", " ")
out = subprocess.run([sys.executable, os.path.join(root, "tools/symid-census/gate.py"), os.path.join(root, "stage2/compiler"), "--final"],
                     capture_output=True, text=True)
text = out.stdout + out.stderr
stale = re.findall(r'stale ledger row — written_text read: (\S+) (\S+) — (\d+) listed, (\d+) found', text)
new = re.findall(r'FAIL — written_text read: (\S+) (\S+) — (\d+) found, (\d+) listed', text)
lit = re.findall(r'stale ledger row — literal name test: (\S+) (\S+) (\S+) (\S+) — (\d+) listed, (\d+) found', text)
p = os.path.join(root, "tools/symid-census/written-readers.txt")
rows = open(p).read().split("\n")
def find(f, fn):
    for i, r in enumerate(rows):
        c = r.split("\t")
        if len(c) >= 3 and c[0] == f and c[1] == fn: return i
    return -1
deleted = rewritten = added = 0
for f, fn, listed, found in stale:
    i = find(f, fn)
    if i < 0: print("no row for", f, fn); continue
    if found == "0": rows.pop(i); deleted += 1
    elif (f, fn) in given: rows[i] = "%s\t%s\t%s" % (f, fn, given[(f, fn)]); rewritten += 1
    else: print("PARTIAL needs reasons:", f, fn, listed, "->", found, "| row:", rows[i])
for f, fn, found, listed in new:
    if (f, fn) not in given: print("NEW needs reasons:", f, fn, found, "found,", listed, "listed"); continue
    i = find(f, fn)
    if i >= 0: rows[i] = "%s\t%s\t%s" % (f, fn, given[(f, fn)]); rewritten += 1
    else:
        # keep the file's rows together and sorted by fn
        j = 0
        for k, r in enumerate(rows):
            c = r.split("\t")
            if len(c) >= 3 and (c[0], c[1]) < (f, fn): j = k + 1
        rows.insert(j, "%s\t%s\t%s" % (f, fn, given[(f, fn)])); added += 1
open(p, "w").write("\n".join(rows))
if lit:
    q = os.path.join(root, "tools/symid-census/allow.tsv")
    arows = open(q).read().split("\n"); n0 = len(arows)
    for f, fn, test, literal, listed, found in lit:
        if found != "0": print("PARTIAL literal:", f, fn, test, literal); continue
        arows = [r for r in arows if not (r.split("\t")[:4] == [f, fn, test, literal])]
    open(q, "w").write("\n".join(arows)); print("allow.tsv rows deleted:", n0 - len(arows))
dual = re.findall(r'stale ledger row — text beside an id slot: (\S+) (\S+) — (\d+) listed, 0 found', text)
if dual:
    q = os.path.join(root, "tools/symid-census/dual-slots.txt")
    drows = open(q).read().split("\n"); n0 = len(drows)
    for f, ty, _n in dual:
        drows = [r for r in drows if r.split("\t")[:2] != [f, ty]]
    open(q, "w").write("\n".join(drows)); print("dual-slots rows deleted:", n0 - len(drows))
newlit = re.findall(r'FAIL — literal name test: (\S+) (\S+) (\S+) (\S+) — (\d+) found, (\d+) listed', text)
why = os.environ.get("LEDFIX_LIT")  # "<reason>\t<why>" for every new literal row of this run
if newlit and why:
    q = os.path.join(root, "tools/symid-census/allow.tsv")
    arows = open(q).read().split("\n")
    add = ["%s\t%s\t%s\t%s\t%s\t%s" % (f, fn, test, lit_, found, why) for f, fn, test, lit_, found, _l in newlit]
    j = max(i for i, r in enumerate(arows) if r.split("\t")[0] == newlit[0][0]) + 1
    arows[j:j] = add
    open(q, "w").write("\n".join(arows)); print("allow.tsv rows added:", len(add))
elif newlit:
    for r in newlit: print("NEW literal needs a reason:", r)
print("written-readers: deleted %d, rewritten %d, added %d" % (deleted, rewritten, added))
