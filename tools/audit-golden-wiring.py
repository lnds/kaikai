#!/usr/bin/env python3
"""Structural guard against goldens nobody reads and CI path filters that match nothing.

A golden (`*.expected`, `*.forbidden`) under examples/, demos/ or tests/ is
consumed when a harness reads it:
  - the directory has a FIXTURES manifest and the golden is `<name>.out.expected`,
    `<name>.err.expected`, `<name>.warn.expected` or `<name>.warn-native.expected` beside `<name>.kai` (tools/fixture-dir.sh);
  - a row of stage2/gate-variants.txt names `<name>.kai` and the golden is its `<name>.out.expected` (tools/fixture-gate.sh);
  - a harness source names the golden's path or file name; or
  - a harness source mentions the golden's suffix within WINDOW lines of a
    mention of its directory (a glob or loop that reads the suffix there).
A golden no harness reads needs an entry in tools/golden-wiring-allowlist.txt.

Every regex a workflow feeds to `grep -E` against changed paths, and every
`paths:` glob, must match at least one tracked file, per top-level alternative
and per alternative of its first group, so a rename that orphans a trigger
fails here instead of silently disabling the job.

Allowlist lines: `golden <glob> <reason>` or `filter <pattern> <reason>`.
Textual only: no compiler needed.
"""
import collections
import fnmatch
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)
WINDOW = 15
ALLOWLIST = "tools/golden-wiring-allowlist.txt"
ROOT_MAKEFILES = ("Makefile", "stage2/Makefile", "stage2/test-registry.txt")
GATE_ROWS = "stage2/gate-variants.txt"
GOLDEN_RE = re.compile(r"\.(expected|forbidden)$")
SUFFIX_RE = re.compile(r"^(.*?)(\.[a-z_-]+\.(?:expected|forbidden))$")


def tracked():
    out = subprocess.check_output(["git", "ls-files", "--cached", "--others", "--exclude-standard"], text=True)
    return [f for f in out.split("\n") if f and os.path.exists(f)]


def is_harness(f):
    if "audit-" in f:
        return False
    if f in ROOT_MAKEFILES or f.startswith(".github/workflows/"):
        return True
    if f.startswith("tools/") and not f.startswith("tools/baselines/"):
        return f.endswith((".sh", ".py", ".mk", ".txt", ".pl")) and f != ALLOWLIST
    return f.startswith("tests/") and f.endswith(".sh")


def load_allowlist():
    rows = collections.defaultdict(list)
    for raw in open(ALLOWLIST):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        kind, rest = line.split(None, 1)
        pat, _, reason = rest.partition("  ")
        if not reason.strip():
            sys.exit(f"{ALLOWLIST}: entry without a reason: {line}")
        rows[kind].append(pat.strip())
    return rows


class Corpus:
    def __init__(self, harness):
        self.lines = {h: open(h, errors="replace").read().split("\n") for h in harness}
        self.tokens = set()
        for ls in self.lines.values():
            for tok in re.findall(r"[\w./-]+", "\n".join(ls)):
                self.tokens.add(tok)
                self.tokens.add(tok.rsplit("/", 1)[-1])
        self._windows = {}
        self._seen = {}
        self._file_tokens = {h: set(re.findall(r"[\w.-]+", "\n".join(ls))) for h, ls in self.lines.items()}

    def names(self, token):
        return token in self.tokens

    def windows(self, suffix):
        if suffix not in self._windows:
            wins = []
            for ls in self.lines.values():
                for i, line in enumerate(ls):
                    if suffix in line:
                        wins.append("\n".join(ls[max(0, i - WINDOW):i + WINDOW]))
            self._windows[suffix] = wins
        return self._windows[suffix]

    def file_names_dir_and_suffix(self, suffix, leaf):
        """A harness that builds golden names from a variable names its directory and suffix."""
        return any(leaf in toks and any(suffix in t for t in toks) for toks in self._file_tokens.values())

    def suffix_near_dir(self, suffix, dir_keys):
        wins = self.windows(suffix)
        for key in dir_keys:
            ck = (suffix, key)
            if ck not in self._seen:
                pat = re.compile(r"(?<![\w.-])" + re.escape(key) + r"(?![\w-])")
                self._seen[ck] = any(pat.search(w) for w in wins)
            if self._seen[ck]:
                return True
        return False


def gate_fixtures():
    out = set()
    for raw in open(GATE_ROWS):
        parts = raw.split()
        if parts and not parts[0].startswith("#") and len(parts) > 2:
            out.update(parts[2].split(","))
    return out


GATE_FIXTURES = gate_fixtures()


def golden_consumed(g, corpus):
    d, b = os.path.dirname(g), os.path.basename(g)
    m = SUFFIX_RE.match(b)
    if not m:
        m = re.match(r"^(.*?)(\.(?:expected|forbidden))$", b)
    stem, suffix = m.group(1), m.group(2)
    if (os.path.exists(d + "/FIXTURES") and suffix in (".out.expected", ".err.expected", ".warn.expected", ".warn-native.expected")
            and os.path.exists(f"{d}/{stem}.kai")):
        return True
    if suffix == ".out.expected" and f"{d}/{stem}.kai" in GATE_FIXTURES:
        return True
    if corpus.names(g) or corpus.names(b):
        return True
    parts = d.split("/")
    keys = ["/".join(parts[:j]) for j in range(2, len(parts) + 1)]
    if corpus.suffix_near_dir(suffix, keys):
        return True
    return corpus.file_names_dir_and_suffix(suffix, parts[-1])


def check_goldens(files, corpus, allow):
    bad = []
    used = set()
    for g in files:
        if not (g.startswith(("examples/", "demos/", "tests/")) and GOLDEN_RE.search(g)):
            continue
        hit = next((p for p in allow["golden"] if fnmatch.fnmatch(g, p)), None)
        if golden_consumed(g, corpus):
            if hit:
                print(f"audit-golden-wiring FAIL: stale allowlist entry '{hit}' (golden {g} is read now)")
                bad.append(g)
            continue
        if hit:
            used.add(hit)
            continue
        bad.append(g)
        print(f"audit-golden-wiring FAIL: no harness reads {g}")
    for p in allow["golden"]:
        if p not in used and not any(fnmatch.fnmatch(f, p) for f in files):
            print(f"audit-golden-wiring FAIL: allowlist glob '{p}' matches no file")
            bad.append(p)
    return bad


def split_top(pattern, sep="|"):
    parts, depth, cur, i = [], 0, "", 0
    while i < len(pattern):
        c = pattern[i]
        if c == "\\" and i + 1 < len(pattern):
            cur += pattern[i:i + 2]
            i += 2
            continue
        if c == "[":
            j = pattern.index("]", i + 1) if "]" in pattern[i + 1:] else len(pattern) - 1
            cur += pattern[i:j + 1]
            i = j + 1
            continue
        depth += c == "("
        depth -= c == ")"
        if c == sep and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += c
        i += 1
    parts.append(cur)
    return parts


def first_group(pattern):
    depth, start, i = 0, None, 0
    while i < len(pattern):
        c = pattern[i]
        if c == "\\":
            i += 2
            continue
        if c == "[":
            i = pattern.index("]", i + 1) + 1
            continue
        if c == "(":
            if depth == 0:
                start = i
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return start, i
        i += 1
    return None


def expand(pattern):
    """Yield the alternatives of a regex: top-level `|`, then the first group's."""
    for alt in split_top(pattern):
        alt = alt.lstrip("^").rstrip("$")
        g = first_group(alt)
        if g is None:
            yield alt
            continue
        s, e = g
        inner = alt[s + 1:e]
        if inner.startswith("?:"):
            inner = inner[2:]
        for sub in split_top(inner):
            yield from expand(alt[:s] + sub + alt[e + 1:])


def glob_to_regex(glob):
    out = ""
    i = 0
    while i < len(glob):
        if glob.startswith("**/", i):
            out += "(?:.*/)?"
            i += 3
        elif glob.startswith("**", i):
            out += ".*"
            i += 2
        elif glob[i] == "*":
            out += "[^/]*"
            i += 1
        elif glob[i] == "?":
            out += "[^/]"
            i += 1
        else:
            out += re.escape(glob[i])
            i += 1
    return out


def workflow_filters():
    """(workflow, kind, pattern) for each path regex and `paths:` glob."""
    found = []
    for wf in sorted(os.listdir(".github/workflows")):
        path = ".github/workflows/" + wf
        text = open(path).read()
        for m in re.finditer(r"grep\s+-[a-zA-Z]*E[a-zA-Z]*\s+'([^'\n]+)'", text):
            if "/" in m.group(1) or "\\." in m.group(1):
                found.append((path, "regex", m.group(1)))
        in_paths = False
        for line in text.split("\n"):
            if re.match(r"\s+paths(-ignore)?:\s*$", line):
                in_paths = line.strip() == "paths:"
                continue
            item = re.match(r"\s+-\s+'?([^'\s#][^'#]*?)'?\s*$", line)
            if in_paths and item:
                found.append((path, "glob", item.group(1)))
            elif in_paths and line.strip() and not line.strip().startswith("#"):
                in_paths = False
    return found


def check_filters(files, allow):
    bad = []
    used = set()
    for wf, kind, pat in workflow_filters():
        if kind == "glob":
            alts = [(pat, re.compile("^" + glob_to_regex(pat) + "$"))]
        else:
            alts = []
            for a in expand(pat):
                try:
                    alts.append((a, re.compile(a if a.startswith("^") else "^" + a)))
                except re.error:
                    continue
        for label, rx in alts:
            if any(rx.search(f) for f in files):
                continue
            if label in allow["filter"]:
                used.add(label)
                continue
            bad.append(label)
            print(f"audit-golden-wiring FAIL: {wf}: path filter '{label}' matches no tracked file")
    for p in allow["filter"]:
        if p not in used:
            print(f"audit-golden-wiring FAIL: allowlist filter '{p}' is unused or now matches")
            bad.append(p)
    return bad


def main():
    files = tracked()
    allow = load_allowlist()
    corpus = Corpus([f for f in files if is_harness(f)])
    bad = check_goldens(files, corpus, allow) + check_filters(files, allow)
    if bad:
        sys.exit(1)
    print("audit-golden-wiring OK")


main()
