"""The check behind tools/compiler-lint-ratchet.sh: `kai lint --json` on stdin against tools/baselines/compiler-lint."""
import collections, hashlib, json, os, sys
root, mode = sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else ""
sys.path.insert(0, os.path.join(root, "tools", "lib"))
sys.dont_write_bytecode = True
import entries

directory = os.path.join(root, "tools", "baselines", "compiler-lint")
lines_of = {}


def source_line(path, line):
    if path not in lines_of:
        try:
            lines_of[path] = open(os.path.join(root, path), encoding="utf-8").read().split("\n")
        except OSError:
            lines_of[path] = []
    ls = lines_of[path]
    return ls[line - 1].strip() if 0 < line <= len(ls) else ""


counts, label = collections.Counter(), {}
for f in json.load(sys.stdin):
    path = f["file"]
    if not path.startswith("/"):
        path = "stage2/" + path.removeprefix("./")
    i = path.find("stage2/")
    path = path[i:] if i >= 0 else path
    text = source_line(path, f["line"])
    digest = hashlib.sha1((f["message"] + "\0" + text).encode()).hexdigest()[:10]
    key = (path, f["rule"], digest)
    counts[key] += 1
    label[key] = "%s:%d  %s" % (path, f["line"], f["message"][:100])

if mode == "--update":
    if os.path.isdir(directory):
        for fname in os.listdir(directory):
            if fname != "README.md":
                os.remove(os.path.join(directory, fname))
    for key, n in sorted(counts.items()):
        entries.put(directory, 3, list(key) + [str(n)])
    print("compiler-lint-ratchet: entries rewritten (%d findings in %d entries)" % (sum(counts.values()), len(counts)))
    sys.exit(0)

try:
    base = {tuple(r[:3]): int(r[3]) for r in entries.load(directory, 3)}
except ValueError as e:
    print("compiler-lint-ratchet FAIL — " + str(e))
    sys.exit(1)
up = [(k, base.get(k, 0), n) for k, n in sorted(counts.items()) if n > base.get(k, 0)]
down = [(k, b, counts.get(k, 0)) for k, b in sorted(base.items()) if counts.get(k, 0) < b]
for k, b, n in up:
    print("compiler-lint-ratchet FAIL — new finding: %s (%d, baseline %d)" % (label[k], n, b))
for k, b, n in down:
    print("compiler-lint-ratchet FAIL — fixed finding still pinned: %s %s %s (%d, baseline %d)" % (k[0], k[1], k[2], n, b))
if up:
    print("  fix them, or run `kai lint stage2/main.kai` to see where")
if down:
    print("  retire them: tools/compiler-lint-ratchet.sh --update (deletes or lowers the entry files)")
if up or down:
    sys.exit(1)
print("compiler-lint-ratchet OK — %d findings in %d entries, at baseline" % (sum(counts.values()), len(counts)))
