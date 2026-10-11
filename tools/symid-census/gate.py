#!/usr/bin/env python3
"""Names stay out of identity: the source checks behind tools/symid-names-gate.sh.

  gate.py <compiler-dir> [--final]

After the resolver a declaration, a type, an effect, a constructor or a local is its id. Text
survives where a name is resolved, shown or spelled into output, and nowhere else. Four facts about
the sources hold that, each against a ledger beside this file (tab-separated, `#` starts a comment):

  1. dual-slots.txt       file  type  why
     No type definition holds text beside the id of the same thing. A type with a text slot
     (`String`, `Written`) beside an id slot (`SymId`, `FnId`, `LocalId`, `DeclRef`, `Binder`) is
     listed with what the text is instead: an op label, emitted text, a module origin.

  2. written-readers.txt  file  fn  reads
     `written_text` is the one way characters leave a `Written`. Every function calling it is
     listed with how many calls it makes for which reason, `R=2 D=1`:
       R  resolution: a pass before the resolver, the resolver's own tables, or the typer settling
          a name the resolver left written
       D  a diagnostic, a dump, tool output, a test's assertion
       E  an emitter or generator spelling a symbol, a label, a key
       L  an operation or field label looked up inside a declaration known by its id

  3. allow.tsv            file  fn  test  literal  n  cat  why
     Every string literal that name text is tested against (extract.py finds them), with the
     reason letter above and one line of why.

  4. written-mints.txt    file  fn  why
     A reference by text (`EName`, `EModCall`, `mk_name`) is built only where no id exists yet:
     before the resolver, or over source text re-parsed later. `*` as the fn lists a whole file.

pending-written.txt (file  fn  what  why) holds what still breaks the rule: a `read` of a name that
decides something after the resolver, a `mint` of a written reference there, or a literal test
(`== main`). One row per site. Such a site is in no other ledger; the gate counts the rows.

A site the ledgers do not cover fails. With --final a pending row fails too, and so does a ledger
row that covers nothing: the ledgers then describe the sources exactly.
"""
import collections, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True      # a gate leaves the tree as it found it
import extract, sources

SRC = sys.argv[1]
FINAL = "--final" in sys.argv[2:]
REASONS = "RDEL"
TEXT = {"String", "Written"}
IDENT = {"SymId", "FnId", "LocalId", "DeclRef", "Binder"}
TYPE_DEF = re.compile(r"^(?:pub\s+)?(?:opaque\s+)?type\s+([A-Z]\w*)[^=\n]*=?", re.M)
TOP_LEVEL = re.compile(r"^[a-z#]", re.M)
READ = re.compile(r"(?<!fn )\bwritten_text\b")
MINT = re.compile(r"(?<!fn )\b(EName|EModCall|mk_name)\(")

fails, stale = [], []


def ledger(name):
    """The rows of a ledger, split at tabs."""
    lines = open(os.path.join(HERE, name)).read().split("\n")
    return [l.split("\t") for l in lines if l.strip() and not l.startswith("#")]


def settle(what, found, allowed):
    """Compare what the sources hold with what the ledgers allow, key by key."""
    for key in sorted(set(found) | set(allowed)):
        have, may = found.get(key, 0), allowed.get(key, 0)
        if have > may: fails.append("%s: %s — %d found, %d listed" % (what, " ".join(key), have, may))
        if have < may: stale.append("%s: %s — %d listed, %d found" % (what, " ".join(key), may, have))


# --- the sources, read once ---------------------------------------------
lits = []
FILES = list(sources.files(SRC, lits))
FUNCTIONS = [(fname, name, body) for fname, text in FILES for name, body, _ in sources.functions(text)]

pending = collections.Counter()
for file, fn, what, _why in ledger("pending-written.txt"):
    pending[(file, fn, what)] += 1


# --- 1. text beside an id -----------------------------------------------
def slot_type(slot):
    """The type of a constructor slot or record field, without `Option[…]`."""
    t = slot.split(":", 1)[-1].strip()
    return t[len("Option["):-1].strip() if t.startswith("Option[") else t


def dual_types(text):
    """The types of a file with a constructor or record holding both a text slot and an id slot."""
    for m in TYPE_DEF.finditer(text):
        end = TOP_LEVEL.search(text, m.end())
        body = text[m.end():end.start() if end else len(text)]
        for alt in extract.split_args(body.replace("|", ",")):
            paren = re.search(r"[({]", alt)
            if not paren: continue
            slots = {slot_type(s) for s in extract.split_args(alt[paren.start() + 1:extract.close_of(alt, paren.start())])}
            if slots & TEXT and slots & IDENT:
                yield m.group(1)
                break

listed_dual = {(file, ty): 1 for file, ty, _why in ledger("dual-slots.txt")}
found_dual = {(fname, ty): 1 for fname, text in FILES for ty in dual_types(text)}
settle("text beside an id slot", found_dual, listed_dual)


# --- 2. the one accessor ------------------------------------------------
found_reads = collections.Counter()
for fname, name, body in FUNCTIONS:
    n = len(READ.findall(body))
    if n: found_reads[(fname, name)] = n

listed_reads, by_reason = collections.Counter(), collections.defaultdict(set)
for file, fn, reads in ledger("written-readers.txt"):
    for item in reads.split():
        reason, n = item.split("=")
        if reason not in REASONS: fails.append("written-readers.txt: no reason `%s` (%s %s)" % (reason, file, fn))
        listed_reads[(file, fn)] += int(n)
        by_reason[reason].add(file)
for (file, fn, what), n in pending.items():
    if what == "read": listed_reads[(file, fn)] += n
settle("written_text read", found_reads, listed_reads)


# --- 3. literals name text is tested against ----------------------------
found_tests = collections.Counter()
for file, _line, fn, test, lit in extract.literal_tests(SRC):
    found_tests[(file, fn, test, lit)] += 1

listed_tests = collections.Counter()
for file, fn, test, lit, n, reason, _why in ledger("allow.tsv"):
    if reason not in REASONS: fails.append("allow.tsv: no reason `%s` (%s %s)" % (reason, file, fn))
    listed_tests[(file, fn, test, lit)] += int(n)
for key in found_tests:
    if pending.get((key[0], key[1], key[2] + " " + key[3])): listed_tests[key] = found_tests[key]
settle("literal name test", found_tests, listed_tests)


# --- 4. references by text are built before the resolver ----------------
def builds(body, m):
    """False when the node at `m` is its own definition (its slots are types), or sits left of its
    arm's `->`: taken apart, not built."""
    if m.group(1) == "mk_name": return True
    close = extract.close_of(body, m.end() - 1)
    if re.fullmatch(r"[A-Z]\w*(\s*,\s*[A-Z]\w*)*", body[m.end():close].strip()): return False
    rest = body[close + 1:(body.find("\n", close) + 1 or len(body) + 1) - 1].split(" if ")[0]
    line = body[body.rfind("\n", 0, m.start()) + 1:m.start()]
    lead = line[max(line.rfind("->"), line.rfind("{")) + 1:]
    in_arm_head = "->" in rest and re.fullmatch(r"[\s)\],|\w.]*", rest[:rest.index("->")]) and not re.search(r"[a-z_]\w*\(", lead)
    return not in_arm_head

found_mints = collections.Counter()
for fname, name, body in FUNCTIONS:
    n = sum(1 for m in MINT.finditer(body) if builds(body, m))
    if n: found_mints[(fname, name)] = n

mint_rows = ledger("written-mints.txt")
whole_files = {file for file, fn, _why in mint_rows if fn == "*"}
listed_mints = collections.Counter({(file, fn): found_mints.get((file, fn), 0) or 1
                                    for file, fn, _why in mint_rows if fn != "*"})
for (file, fn), n in found_mints.items():
    if file in whole_files: listed_mints[(file, fn)] = n
for (file, fn, what), n in pending.items():
    if what == "mint": listed_mints[(file, fn)] += n
settle("reference by text built", found_mints, listed_mints)
for file in sorted(whole_files - {f for f, _ in found_mints}):
    stale.append("reference by text built: %s * — listed, none found" % file)


# --- verdict ------------------------------------------------------------
n_pending = sum(pending.values())
if FINAL:
    fails += ["stale ledger row — " + s for s in stale]
    if n_pending: fails.append("%d sites are still listed in pending-written.txt" % n_pending)
for m in fails: print("symid-names FAIL — " + m)
print("symid-names: %d dual slots listed; %d reads of written_text in %d functions (files per reason: %s); "
      "%d literal tests; %d functions build a reference by text; %d pending%s"
      % (len(listed_dual), sum(found_reads.values()), len(found_reads),
         " ".join("%s %d" % (r, len(by_reason[r])) for r in REASONS),
         sum(found_tests.values()), len(found_mints), n_pending,
         "" if FINAL or not stale else "; %d ledger rows cover less than they list" % len(stale)))
sys.exit(1 if fails else 0)
