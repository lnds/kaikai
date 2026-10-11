import sys, re, os, collections
# errs.py <repo-root> <max> [file.kai ...]  — reads the checker's output on stdin.
root = sys.argv[1]; mx = int(sys.argv[2]); files = set(sys.argv[3:])
cur = None; errs = []
for l in sys.stdin.read().split("\n"):
    if l.startswith("error") or l.startswith("warning"):
        cur = l
    else:
        m = re.match(r"\s*--> (.*?):(\d+):(\d+)", l)
        if m and cur is not None:
            errs.append((os.path.basename(m.group(1)), int(m.group(2)), int(m.group(3)), cur)); cur = None
if not files:
    c = collections.Counter(e[0] for e in errs)
    for f, n in sorted(c.items(), key=lambda x: -x[1]):
        print("%5d %s" % (n, f))
    print("%d errors in %d files" % (len(errs), len(c)))
else:
    mine = [e for e in errs if e[0] in files]
    lo = os.environ.get("ERRS_FROM"); hi = os.environ.get("ERRS_TO")
    if (lo or hi) and len(files) == 1:
        f = list(files)[0]
        L = open(os.path.join(root, "stage2", "compiler", f), errors="replace").read().split("\n")
        def at(name, default):
            if not name:
                return default
            pat = re.compile(r"(?:pub )?fn " + re.escape(name) + r"\b")
            for i, x in enumerate(L):
                if pat.match(x):
                    return i + 1
            return default
        a = at(lo, 1); b = at(hi, 10 ** 9)
        mine = [e for e in mine if a <= e[1] < b]
    for f, ln, col, msg in mine[:mx]:
        print("%s:%d:%d: %s" % (f, ln, col, msg[:300]))
    if len(mine) > mx:
        print("... %d more" % (len(mine) - mx))
    print("%d errors" % len(mine))
