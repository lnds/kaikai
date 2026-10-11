#!/usr/bin/env python3
"""Census extractor: every string literal that name text is tested against.

  extract.py <compiler-dir>

Name text is the characters of a `Written`. An expression is name text when it

  * mentions a producer (`written_text`, `decl_name`, `sym_name`, `sym_home`, `ln_text`, `spec_text`,
    `fn_emit_name`) or reads a `.name` slot, also inside an interpolation;
  * mentions a variable bound to name text in the same function, or a parameter that some caller
    passes name text to;
  * mentions an accessor: a function returning text that reads name text and builds no string of
    its own, so the name leaves it unchanged.

A test is `==` / `!=` against a literal, `string_starts_with` / `string_ends_with` /
`string_contains` / `list_has` with a literal, or a literal arm of a `match` over name text. The
empty literal is not a test: it asks whether there is a name, not which.

The analysis is lexical and flow-insensitive: it follows names through `let`, lambda parameters,
call arguments and accessors, and nothing else. It over-reports by design; the ledger says why each
row is legitimate.

Output: TSV  file  line  fn  test  literal
"""
import collections, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.dont_write_bytecode = True
import sources
from sources import LIT_RE

PRODUCERS = {"written_text", "decl_name", "sym_name", "sym_home", "ln_text", "spec_text", "fn_emit_name"}
LITERAL_TESTS = {"string_starts_with", "string_ends_with", "string_contains", "list_has"}
BUILDS = re.compile(r"\b(?:concat_all|string_concat|string_join|string_slice|string_replace|int_to_string|interp)\(")

ONLY_LIT = re.compile(r'\s*"\x00(\d+)\x00"\s*$')
ARM_OR_BRACKET = re.compile(r'[(\[{]|[)\]}]|"\x00(\d+)\x00"(?=\s*(?:->|\|))')
LET = re.compile(r"\b(?:let|var)\s+(?:mut\s+)?([a-z_]\w*)\b[^=\n]*?=(?!=)")
CALL = re.compile(r"(?<![\w.])([a-z_]\w*)\(")
WORD = re.compile(r"(?<![\w.])[a-z_]\w*")
NAME_SLOT = re.compile(r"\.name\b")
LAMBDA = re.compile(r"\s*\(([\w\s,]*)\)\s*=>")
MATCH = re.compile(r"\bmatch\b([^{}\n]*)\{")
SEPARATES = re.compile(r"\b(?:and|or|not|if|else|then|let|var)\b|(?<![=!<>])=(?!=)")
OPEN, CLOSE = "([{", ")]}"


def close_of(s, i):
    """Index of the bracket closing the one at s[i], or len(s)."""
    depth = 0
    for j in range(i, len(s)):
        if s[j] in OPEN: depth += 1
        elif s[j] in CLOSE:
            depth -= 1
            if depth == 0: return j
    return len(s)


def split_args(s):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in OPEN: depth += 1
        elif ch in CLOSE: depth -= 1
        if ch == "," and depth == 0:
            out.append(cur); cur = ""
        else: cur += ch
    return out + [cur] if cur.strip() else out


def operand(s, i, step):
    """The operand of a comparison that ends (step -1) or starts (step +1) at s[i]."""
    depth, j = 0, i
    inward, outward = (CLOSE, OPEN) if step < 0 else (OPEN, CLOSE)
    while 0 <= j < len(s):
        ch = s[j]
        if depth == 0 and (ch in ",;\n{}" or s[j:j + 2] in ("->", "=>")): break
        if ch in inward: depth += 1
        elif ch in outward:
            if depth == 0: break
            depth -= 1
        j += step
    text = s[j + 1:i + 1] if step < 0 else s[i:j]
    return SEPARATES.split(text)[-1 if step < 0 else 0]


class Expr:
    """A piece of code and what decides whether it is name text: its words, and a `.name` read."""
    __slots__ = ("text", "words", "slot")
    def __init__(self, text):
        self.text, self.words, self.slot = text, set(WORD.findall(text)), bool(NAME_SLOT.search(text))


class Fn:
    def __init__(self, file, name, text, first_line):
        self.file, self.name = file, name
        paren = text.find("(")
        end = close_of(text, paren) if paren >= 0 else 0
        self.params = [a.split(":")[0].strip() for a in split_args(text[paren + 1:end])] if paren >= 0 else []
        rest = text[end + 1:]
        cut = re.search(r"=(?!=)|\{", rest)
        self.returns_text = bool(re.search(r"\b(String|Written)\b", rest[:cut.start()] if cut else ""))
        self.body = rest[cut.start():] if cut else ""
        self.whole = Expr(self.body)
        self.first_line = first_line + text[:len(text) - len(self.body)].count("\n")
        self.builds = bool(BUILDS.search(self.body))
        self.tainted = set()            # variables and parameters holding name text
        self.parsed = None
        self.seen = None                # what was known when the function was last followed

    def pieces(self):
        """(lets, calls): `(variable, right-hand side)` and `(callee, arguments, offset)`, read once."""
        if self.parsed is None:
            lets = [(m.group(1), Expr(self.statement(m.end()))) for m in LET.finditer(self.body)]
            calls = []
            for m in CALL.finditer(self.body):
                args = split_args(self.body[m.end():close_of(self.body, m.end() - 1)])
                calls.append((m.group(1), [Expr(a) for a in args], m.start()))
            self.parsed = (lets, calls)
        return self.parsed

    def statement(self, i):
        """The right-hand side starting at body[i]: to the end of the line, or of the open bracket."""
        depth, j = 0, i
        while j < len(self.body):
            ch = self.body[j]
            if ch in OPEN: depth += 1
            elif ch in CLOSE:
                if depth == 0: break
                depth -= 1
            elif ch == "\n" and depth == 0: break
            j += 1
        return self.body[i:j]

    def line(self, i):
        return self.first_line + self.body.count("\n", 0, i)


class Census:
    def __init__(self, src):
        self.lits, self.fns, self.by_name = [], [], collections.defaultdict(list)
        for fname, text in sources.files(src, self.lits):
            for name, body, line in sources.functions(text):
                f = Fn(fname, name, body, line)
                self.fns.append(f); self.by_name[name].append(f)
        self.names = set(PRODUCERS)     # producers and accessors

    def callees(self, f, name):
        """The definitions a call to `name` from `f` can reach: its own file's, else any."""
        defs = self.by_name.get(name, [])
        return [g for g in defs if g.file == f.file] or defs

    def is_name(self, f, e):
        return e.slot or not e.words.isdisjoint(self.names) or not e.words.isdisjoint(f.tainted)

    def spread(self):
        """Follow name text through lets, lambdas, arguments and accessors until nothing changes."""
        changed = True
        while changed:
            changed = False
            for f in self.fns:
                known = (len(f.tainted), len(self.names))
                if f.seen == known or not self.is_name(f, f.whole): continue
                f.seen = known
                changed = self.follow(f) or changed
                if f.returns_text and not f.builds and f.name not in self.names:
                    self.names.add(f.name); changed = True

    def follow(self, f):
        """One function: taint its lets and lambda parameters, and the parameters of what it calls."""
        lets, calls = f.pieces()
        changed, grew = False, True
        while grew:
            before = len(f.tainted)
            f.tainted.update(var for var, rhs in lets if self.is_name(f, rhs))
            for callee, args, _ in calls:
                named = [self.is_name(f, a) for a in args]
                if not any(named): continue
                for a in args:
                    lam = LAMBDA.match(a.text)
                    f.tainted.update(p.strip() for p in (lam.group(1).split(",") if lam else []) if p.strip())
                for g in self.callees(f, callee):
                    for i, yes in enumerate(named):
                        if yes and i < len(g.params) and g.params[i] not in g.tainted:
                            g.tainted.add(g.params[i]); changed = True
            grew = len(f.tainted) > before
            changed = changed or grew
        return changed

    def tests(self, f):
        """(line, test, literal) for each literal the function tests name text against."""
        b, out = f.body, []
        for m in re.finditer(r"==|!=", b):
            left, right = operand(b, m.start() - 1, -1), operand(b, m.end(), +1)
            for lit, other in ((ONLY_LIT.match(left), right), (ONLY_LIT.match(right), left)):
                if lit and self.is_name(f, Expr(other)):
                    out.append((f.line(m.start()), m.group(0), self.lits[int(lit.group(1))]))
        for callee, args, at in f.pieces()[1]:
            if callee not in LITERAL_TESTS: continue
            for k, a in enumerate(args):
                if not any(self.is_name(f, x) for i, x in enumerate(args) if i != k): continue
                items = split_args(a.text.strip()[1:-1] if a.text.strip().startswith("[") else a.text)
                if items and all(ONLY_LIT.match(x) for x in items):
                    out += [(f.line(at), callee, self.lits[int(n)]) for n in LIT_RE.findall(a.text)]
        for m in MATCH.finditer(b):
            if not self.is_name(f, Expr(m.group(1))): continue
            depth, start = 0, m.end()
            for arm in ARM_OR_BRACKET.finditer(b[start:close_of(b, start - 1)]):
                t = arm.group(0)
                if t in OPEN: depth += 1
                elif t in CLOSE: depth -= 1
                elif depth == 0: out.append((f.line(start + arm.start()), "match", self.lits[int(arm.group(1))]))
        return out


def literal_tests(src):
    """(file, line, fn, test, literal) for every literal the sources test name text against."""
    census = Census(src)
    census.spread()
    for f in census.fns:
        if not census.is_name(f, f.whole): continue
        for line, test, lit in sorted(set(census.tests(f))):
            if lit: yield f.file, line, f.name, test, lit


def main():
    for row in literal_tests(sys.argv[1]):
        print("\t".join(str(x) for x in row))


if __name__ == "__main__":
    main()
