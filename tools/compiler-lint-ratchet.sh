#!/usr/bin/env bash
# `kai lint` findings over the compiler may only go down.
#
# The baseline counts findings per (file, rule), so a finding moving lines
# does not trip the gate. A count above the baseline, or a (file, rule)
# pair the baseline does not list, fails; a count below it passes and asks
# for the baseline to be lowered so the finding cannot come back.
#
# Usage: compiler-lint-ratchet.sh [--update]   (--update rewrites the baseline)

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BASELINE="$ROOT/tools/compiler-lint-baseline.txt"
KAI="${KAI:-$ROOT/bin/kai}"
[ -x "$KAI" ] || { echo "compiler-lint-ratchet: no $KAI; make bin/kai first" >&2; exit 2; }

current="$(mktemp)"
trap 'rm -f "$current"' EXIT
(cd "$ROOT/stage2" && "$KAI" lint --json main.kai) | python3 -I -c '
import collections, json, sys
counts = collections.Counter()
for f in json.load(sys.stdin):
    path = f["file"]
    i = path.find("stage2/")
    counts[(path[i:] if i >= 0 else path, f["rule"])] += 1
for (path, rule), n in sorted(counts.items()):
    print("%s\t%s\t%d" % (path, rule, n))
' > "$current"

if [ "${1:-}" = "--update" ]; then
  { echo "# file	rule	count — tools/compiler-lint-ratchet.sh --update rewrites this"; cat "$current"; } > "$BASELINE"
  echo "compiler-lint-ratchet: baseline rewritten ($(awk -F'\t' '{s+=$3} END{print s}' "$current") findings)"
  exit 0
fi

python3 -I - "$BASELINE" "$current" <<'PY'
import sys
def load(p):
    out = {}
    for line in open(p):
        if line.startswith("#") or not line.strip():
            continue
        path, rule, n = line.rstrip("\n").split("\t")
        out[(path, rule)] = int(n)
    return out
base, cur = load(sys.argv[1]), load(sys.argv[2])
up = [(k, base.get(k, 0), n) for k, n in sorted(cur.items()) if n > base.get(k, 0)]
down = sum(n - cur.get(k, 0) for k, n in base.items() if cur.get(k, 0) < n)
total = sum(cur.values())
if up:
    print("compiler-lint-ratchet FAIL — new `kai lint` findings in the compiler:")
    for (path, rule), b, n in up:
        print("  %s  %s: %d (baseline %d)" % (path, rule, n, b))
    print("  fix them, or run `kai lint stage2/main.kai` to see where")
    sys.exit(1)
if down:
    print("compiler-lint-ratchet OK (improved) — %d findings, %d fewer than the baseline" % (total, down))
    print("  lower it: tools/compiler-lint-ratchet.sh --update")
else:
    print("compiler-lint-ratchet OK — %d findings, at baseline" % total)
PY
