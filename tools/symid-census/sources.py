#!/usr/bin/env python3
"""The compiler's sources as the census reads them: comments gone, literals masked, cut by function."""
import os, re

TOKEN = re.compile(r'#\[|#[^\n]*|"""[\s\S]*?"""|"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)\'')
FN = re.compile(r"^[ \t]*(?:pub\s+)?fn\s+([a-z_]\w*)", re.M)
LIT_RE = re.compile(r'"\x00(\d+)\x00"')              # a masked literal: its index in the table


def mask(text, lits):
    """Comments dropped, each literal replaced by its index in `lits`, interpolations kept as code.
    Line breaks survive, so an offset still tells its line."""
    def one(m):
        t = m.group(0)
        if t == "#[": return t
        if t.startswith("#"): return ""
        if t.startswith('"""'): return '""' + "\n" * t.count("\n")
        if t.startswith("'"): return "'c'"
        holes = re.findall(r"#\{([^{}]*)\}", t)
        if holes: return "interp(" + ", ".join(holes) + ")"
        lits.append(t[1:-1])
        return '"\x00%d\x00"' % (len(lits) - 1)
    return TOKEN.sub(one, text)


def files(src, lits):
    """(file name, masked text) for each source file, in name order."""
    for fname in sorted(os.listdir(src)):
        if fname.endswith(".kai"):
            yield fname, mask(open(os.path.join(src, fname)).read(), lits)


def functions(text):
    """(name, text, first line) for each function of a masked file: from its `fn` to the next one."""
    starts = [(m.start(), m.group(1)) for m in FN.finditer(text)]
    for k, (at, name) in enumerate(starts):
        end = starts[k + 1][0] if k + 1 < len(starts) else len(text)
        yield name, text[at:end], text.count("\n", 0, at) + 1
