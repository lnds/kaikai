#!/usr/bin/env python3
"""Every make target whose recipe runs bin/kai must declare what builds it.

bin/kai is built by tools/kai after kaic2, and a target that depends on the
compiler alone passes locally (bin/kai left over from an earlier build) and
fails on a clean runner with `bin/kai: not found`. Pure text, no make
invocation, no compiler.

A recipe reaches bin/kai spelled in its own text, through a make variable or
`define` macro it expands, or through a repo script (tools/, tests/,
examples/, scripts/) it names anywhere. A script reaches it by spelling the
checkout root's bin/kai, or by running another script at any depth: a
`$ROOT/...` path in command position (line start, after `;` `&` `|` `(` `!`,
`sh`/`bash`/`.`/`exec`/`env`, or a `VAR=value` prefix). Not followed: a path
a script only reads (a cp, grep or sed argument), a path relative to the
working directory, and a path assembled at run time.
"""

import re
import sys
import tempfile
from pathlib import Path

SELF = Path(__file__).resolve()

# makefile -> (bin/kai spelling in its recipes, prerequisites that build it)
CHECKS = {
    "stage2/Makefile": (r"(\.\./|\bcd \.\. .*)\bbin/kai\b", {"bin-kai"}),
    "Makefile": (r"(\./|\broot/|\$\(CURDIR\)/|\bcd .*)\bbin/kai\b", {"bin/kai", "kaic2"}),
}

SCRIPT_DIRS = r"(?:tools|tests|examples|scripts)/[\w./-]*\w"
SCRIPT_USE = re.compile(r"\b(?i:root)\}?\s*/\s*\"?bin/kai\b")
NAMED = re.compile(r"\b" + SCRIPT_DIRS)
INVOKED = re.compile(
    r"(?:^|[;&|(`!]|\b(?:then|do|else|exec|sh|bash|env)\s|(?:^|\s)\.\s|\b\w+=(?:\"[^\"]*\"|'[^']*'|\S*)\s)"
    r"\s*\"?\$\{?(?i:root)\}?/(" + SCRIPT_DIRS + ")", re.M)
HEADER = re.compile(r"^([A-Za-z0-9_.$(][^:=#\t]*?):(?!=)(.*)$")
DEFINE = re.compile(r"^define\s+(\S+)")
ASSIGN = re.compile(r"^(?:override\s+|export\s+)?([A-Za-z_]\w*)\s*[:?+!]*=")
REF = re.compile(r"\$[({](?:call\s+)?([A-Za-z_]\w*)[,)} ]")


def code(text):
    return "\n".join(x for x in text.split("\n") if not x.lstrip().startswith("#"))


def continued(lines, i):
    body = [lines[i]]
    while body[-1].endswith("\\") and i + 1 < len(lines):
        i += 1
        body.append(lines[i])
    return "\n".join(body)


def split_defines(lines):
    """Return each `define` macro's body and the indices of lines outside any define."""
    macros, outside, cur = {}, [], None
    for i, line in enumerate(lines):
        m = DEFINE.match(line)
        if m:
            cur = m.group(1)
            macros[cur] = []
        elif line.startswith("endef"):
            cur = None
        elif cur:
            macros[cur].append(line)
        else:
            outside.append(i)
    return {k: "\n".join(v) for k, v in macros.items()}, outside


def assignments(lines, outside):
    bodies = {}
    for i in outside:
        m = ASSIGN.match(lines[i])
        if m:
            bodies[m.group(1)] = bodies.get(m.group(1), "") + "\n" + continued(lines, i)
    return bodies


def recipe(lines, start):
    body = []
    for nxt in lines[start:]:
        if not (nxt.startswith("\t") or (body and body[-1].endswith("\\"))):
            break
        body.append(nxt)
    return "\n".join(body)


def rules(lines, outside):
    """Yield (line number, header match, recipe text) for every explicit rule."""
    for i in outside:
        m = HEADER.match(lines[i])
        if m and not m.group(1).startswith("."):
            yield i + 1, m, recipe(lines, i + 1)


class Audit:
    def __init__(self, root):
        self.root = root
        self.memo = {}

    def files(self, paths):
        # This file's self-test fixture spells bin/kai; it runs none.
        return [self.root / p for p in paths if (self.root / p).is_file() and (self.root / p).resolve() != SELF]

    def script_runs_kai(self, path):
        if path not in self.memo:
            self.memo[path] = False
            text = code(path.read_text(errors="replace"))
            runs = self.files(INVOKED.findall(text.replace("\\\n", " ")))
            self.memo[path] = bool(SCRIPT_USE.search(text)) or any(map(self.script_runs_kai, runs))
        return self.memo[path]

    def reaches_kai(self, text, use):
        text = code(text)
        return bool(use.search(text)) or any(map(self.script_runs_kai, self.files(NAMED.findall(text))))

    def names_reaching_kai(self, bodies, use):
        using = {k for k, body in bodies.items() if self.reaches_kai(body, use)}
        refs = {k: set(REF.findall(body)) for k, body in bodies.items()}
        more = {k for k in refs if k not in using and refs[k] & using}
        while more:
            using |= more
            more = {k for k in refs if k not in using and refs[k] & using}
        return using

    def violations(self, path, pattern, builders):
        lines = (self.root / path).read_text().split("\n")
        use = re.compile(pattern)
        macros, outside = split_defines(lines)
        using = self.names_reaching_kai({**assignments(lines, outside), **macros}, use)
        for num, m, text in rules(lines, outside):
            if not (self.reaches_kai(text, use) or using.intersection(REF.findall(text))):
                continue
            if builders.isdisjoint(m.group(2).split()):
                yield f"{path}:{num}: {m.group(1)} runs bin/kai but depends on none of {sorted(builders)}"


SELF_TEST_TREE = {
    "tools/direct.sh": 'KAI="$ROOT/bin/kai"\n"$KAI" run x.kai\n',
    "tools/wrapper.sh": 'set -e\nMODE=c "$ROOT/tools/direct.sh" "$@"\n',
    "tools/reader.sh": 'cp "$ROOT/tools/direct.sh" "$tmp/"\ngrep -q KAI \\\n  "$ROOT/tools/direct.sh"\n',
    "tools/sandbox.sh": 'cd "$tmp" && bash tools/direct.sh\n',
    "stage2/Makefile": (
        "RUN = ../tools/direct.sh\n"
        "hole-direct: $(TARGET)\n\t@../tools/direct.sh\n"
        "hole-nested: $(TARGET)\n\t@bash ../tools/wrapper.sh\n"
        "hole-var: $(TARGET)\n\t@$(RUN) x\n"
        "fixed: $(TARGET) bin-kai\n\t@../tools/wrapper.sh\n"
        "reads: $(TARGET)\n\t@../tools/reader.sh\n"
        "relative: $(TARGET)\n\t@../tools/sandbox.sh\n"
    ),
}


def self_test():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for rel, text in SELF_TEST_TREE.items():
            (root / rel).parent.mkdir(parents=True, exist_ok=True)
            (root / rel).write_text(text)
        pattern, builders = CHECKS["stage2/Makefile"]
        got = {v.split()[1] for v in Audit(root).violations("stage2/Makefile", pattern, builders)}
    want = {"hole-direct", "hole-nested", "hole-var"}
    if got != want:
        print(f"test-bin-kai-prereq self-test FAIL: flagged {sorted(got)}, want {sorted(want)}")
        return 1
    print("test-bin-kai-prereq self-test OK")
    return 0


def main():
    if sys.argv[1:] == ["--self-test"]:
        return self_test()
    audit = Audit(SELF.parent.parent)
    bad = [v for path, (pat, builders) in CHECKS.items() for v in audit.violations(path, pat, builders)]
    for v in bad:
        print(v)
    if bad:
        print(f"test-bin-kai-prereq FAIL: {len(bad)} target(s)")
        return 1
    print("test-bin-kai-prereq OK — every target that runs bin/kai depends on what builds it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
