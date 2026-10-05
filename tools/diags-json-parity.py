#!/usr/bin/env python3
"""Parity between a rejected fixture's stderr and its `--diags-json` document.

  diags-json-parity.py <manifest> <baseline>

Each manifest line is `<fixture> <stderr file> <json file>`. A fixture is
at parity when the errors its `--diags-json` document carries are exactly
the errors its plain run printed to stderr, by (file, message, line, col).
Fixtures not yet at parity are listed in the baseline, one per line,
sorted; the list may only shrink.
"""

import json
import re
import sys

SINGLE = re.compile(r"(.*?):(\d+):(\d+): error: (.*)$")
ARROW = re.compile(r"\s+--> (.*):(\d+):(\d+)$")


def stderr_error(lines, i):
    """The error that starts at line i, or None."""
    m = SINGLE.match(lines[i])
    if m:
        return (m.group(1), m.group(4), int(m.group(2)), int(m.group(3)))
    if not lines[i].startswith("error: "):
        return None
    a = ARROW.match(lines[i + 1]) if i + 1 < len(lines) else None
    if not a:
        return (None, lines[i][len("error: "):], None, None)
    return (a.group(1), lines[i][len("error: "):], int(a.group(2)), int(a.group(3)))


def stderr_errors(path):
    lines = open(path, errors="replace").read().splitlines()
    return sorted(e for e in (stderr_error(lines, i) for i in range(len(lines))) if e)


def json_errors(path):
    try:
        doc = json.load(open(path))
    except ValueError:
        return None
    return sorted((d["file"], d["message"], d["line"], d["col"])
                  for d in doc.get("diagnostics", []) if d.get("severity") == "error")


def off_parity(manifest):
    """(fixtures checked, fixtures whose JSON errors differ from stderr's)."""
    rows = [line.split() for line in open(manifest) if line.strip()]
    off = {f for f, err, js in rows if not stderr_errors(err) or json_errors(js) != stderr_errors(err)}
    return len(rows), off


def main(argv):
    if len(argv) != 3:
        sys.stderr.write(__doc__)
        return 2
    manifest, baseline = argv[1], argv[2]
    pinned = [l.strip() for l in open(baseline) if l.strip() and not l.startswith("#")]
    total, off = off_parity(manifest)
    fails = ["%s is not sorted and unique" % baseline] if pinned != sorted(set(pinned)) else []
    fails += ["%s — --diags-json does not report the errors kai build prints" % f for f in sorted(off - set(pinned))]
    fails += ["%s is at parity now; delete it from %s" % (f, baseline) for f in sorted(set(pinned) - off)]
    for f in fails:
        print("diags-json-parity FAIL: " + f)
    print("diags-json-parity: %d of %d rejected fixtures at parity, %d pinned" % (total - len(off), total, len(off)))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
