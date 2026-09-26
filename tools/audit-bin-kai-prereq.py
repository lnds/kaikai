#!/usr/bin/env python3
"""Every make target whose recipe runs bin/kai must declare what builds it.

bin/kai is built by tools/kai after kaic2, and a target that depends on the
compiler alone passes locally (bin/kai left over from an earlier build) and
fails on a clean runner with `bin/kai: not found`. Recipes reach bin/kai
directly or through a `define` macro they `$(call ...)`. Pure text, no make
invocation, no compiler.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# makefile -> (bin/kai spelling in its recipes, prerequisites that build it)
CHECKS = {
    "stage2/Makefile": (r"(\.\./|\bcd \.\. .*)\bbin/kai\b", {"bin-kai"}),
    "Makefile": (r"(\./|\broot/|\bcd .*)\bbin/kai\b", {"bin/kai", "kaic2"}),
}

HEADER = re.compile(r"^([A-Za-z0-9_.$(][^:=#\t]*?):(?!=)(.*)$")
DEFINE = re.compile(r"^define\s+(\S+)")


def calls(text, names):
    return any(re.search(r"\$\(call\s+%s[,)]" % re.escape(n), text) for n in names)


def macros_using(lines, use):
    bodies, cur = {}, None
    for line in lines:
        m = DEFINE.match(line)
        if m:
            cur = m.group(1)
            bodies[cur] = []
        elif line.startswith("endef"):
            cur = None
        elif cur:
            bodies[cur].append(line)
    using = {k for k, body in bodies.items() if any(use.search(x) for x in body)}
    grew = True
    while grew:
        grew = False
        for k, body in bodies.items():
            if k not in using and calls("\n".join(body), using):
                using.add(k)
                grew = True
    return using


def rules(lines):
    """Yield (line number, header match, recipe text) for every explicit rule."""
    in_define = False
    for i, line in enumerate(lines):
        if DEFINE.match(line):
            in_define = True
        elif line.startswith("endef"):
            in_define = False
        if in_define or line.startswith("\t"):
            continue
        m = HEADER.match(line)
        if not m or m.group(1).startswith("."):
            continue
        body = []
        for nxt in lines[i + 1:]:
            if not (nxt.startswith("\t") or (body and body[-1].endswith("\\"))):
                break
            body.append(nxt)
        yield i + 1, m, "\n".join(body)


def violations(path, pattern, builders):
    lines = (ROOT / path).read_text().split("\n")
    use = re.compile(pattern)
    using = macros_using(lines, use)
    for num, m, recipe in rules(lines):
        if not (use.search(recipe) or calls(recipe, using)):
            continue
        if builders.isdisjoint(m.group(2).split()):
            yield f"{path}:{num}: {m.group(1)} runs bin/kai but depends on none of {sorted(builders)}"


def main():
    bad = [v for path, (pat, builders) in CHECKS.items() for v in violations(path, pat, builders)]
    for v in bad:
        print(v)
    if bad:
        print(f"test-bin-kai-prereq FAIL: {len(bad)} target(s)")
        return 1
    print("test-bin-kai-prereq OK — every target that runs bin/kai depends on what builds it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
