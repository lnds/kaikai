#!/usr/bin/env python3
"""Differential pattern-match generator: one kaikai program, many matches.

The pattern grammar is data: variant, tuple, record, list, literal, range,
wildcard, binder and as-patterns, nested up to depth 3, with optional
guards. Each generated `match` gets test values that reach every arm (a
witness per arm) plus random ones. The generator evaluates every match
itself, so the expected output is computed here, independent of either
backend.

Each pattern class is one program, so a backend that cannot build a
class loses only that class's combinations. The gate pins, per class and
backend, how many combinations diverge from the expected table; a count
may only go down, and a lower count must lower its pin.

The exhaustiveness checker is tested the same way. Each class program
also carries matches complete without a catch-all, built to cover their
type's constructors, which must compile and run. A separate program holds
matches built around a value no arm covers, which the typechecker must
reject one by one.

  pattern-shape-gen.py emit <dir>
  pattern-shape-gen.py gate <kai> <backend> <baseline> <workdir>
"""

import random
import subprocess
import sys

SEED = 7
COVERAGE_SEED = 11
FUNCS_PER_TYPE = 3
MAX_DEPTH = 3

# --- types ------------------------------------------------------------

INT, BOOL, REAL = ("Int",), ("Bool",), ("Real",)
PT, RW, COL, SH2, SH = ("Pt",), ("Rw",), ("Col",), ("Sh2",), ("Sh",)


def opt(t):
    return ("Opt", t)


def lst(t):
    return ("List", t)


def tup(a, b):
    return ("Tup", a, b)


RECORDS = {
    "Pt": [("x", INT), ("y", INT)],
    "Rw": [("k", INT), ("s", SH2)],
}

SUMS = {
    "Col": [("Red", []), ("Grn", []), ("Blu", [])],
    "Sh2": [("A", []), ("B", [INT])],
    "Sh": [("Sq", [INT]), ("Pr", [INT, lst(INT)]), ("Bx", [PT]),
           ("Two", [COL, SH2]), ("Nul", [])],
}

DECLS = """type Pt = { x: Int, y: Int }
type Col = Red | Grn | Blu
type Sh2 = A | B(Int)
type Rw = { k: Int, s: Sh2 }
type Sh = Sq(Int) | Pr(Int, [Int]) | Bx(Pt) | Two(Col, Sh2) | Nul
"""



def ctors_of(t):
    if t[0] == "Opt":
        return [("None", []), ("Some", [t[1]])]
    return SUMS[t[0]]


def is_sum(t):
    return t[0] == "Opt" or t[0] in SUMS


def kai_type(t):
    k = t[0]
    if k == "Opt":
        return "Option[%s]" % kai_type(t[1])
    if k == "List":
        return "[%s]" % kai_type(t[1])
    if k == "Tup":
        return "(%s, %s)" % (kai_type(t[1]), kai_type(t[2]))
    return k


def mangle(t):
    k = t[0]
    if k == "Opt":
        return "opt_" + mangle(t[1])
    if k == "List":
        return "list_" + mangle(t[1])
    if k == "Tup":
        return "tup_%s_%s_e" % (mangle(t[1]), mangle(t[2]))
    return k.lower()


# --- values -----------------------------------------------------------
# Int/Bool/Real are Python scalars; a constructor is ("C", name, args);
# a record ("R", type, values in field order); a tuple ("T", a, b); a
# list is a Python tuple.

INTS = [0, 1, 2, 5]
REALS = [0.0, 1.5, 2.5]


def rand_val(rng, t, depth=0):
    k = t[0]
    if k == "Int":
        return rng.choice(INTS)
    if k == "Bool":
        return rng.choice([True, False])
    if k == "Real":
        return rng.choice(REALS)
    if k in RECORDS:
        return ("R", k, tuple(rand_val(rng, ft, depth + 1) for _, ft in RECORDS[k]))
    if is_sum(t):
        name, args = rng.choice(ctors_of(t))
        return ("C", name, tuple(rand_val(rng, a, depth + 1) for a in args))
    if k == "List":
        n = rng.choice([0, 1, 1, 2, 2, 3]) if depth < 2 else rng.choice([0, 1])
        return tuple(rand_val(rng, t[1], depth + 1) for _ in range(n))
    if k == "Tup":
        return ("T", rand_val(rng, t[1], depth + 1), rand_val(rng, t[2], depth + 1))
    raise ValueError(t)


def kai_val(t, v):
    k = t[0]
    if k == "Int":
        return str(v)
    if k == "Bool":
        return "true" if v else "false"
    if k == "Real":
        return repr(float(v))
    if k in RECORDS:
        fs = ", ".join("%s: %s" % (fn, kai_val(ft, fv))
                       for (fn, ft), fv in zip(RECORDS[k], v[2]))
        return "%s { %s }" % (k, fs)
    if is_sum(t):
        args = dict(ctors_of(t))[v[1]]
        if not args:
            return v[1]
        return "%s(%s)" % (v[1], ", ".join(kai_val(a, x) for a, x in zip(args, v[2])))
    if k == "List":
        return "[%s]" % ", ".join(kai_val(t[1], x) for x in v)
    if k == "Tup":
        return "(%s, %s)" % (kai_val(t[1], v[1]), kai_val(t[2], v[2]))
    raise ValueError(t)


def obs(t, v):
    """Mirror of the generated `obs_*` functions."""
    k = t[0]
    if k == "Int":
        return v
    if k == "Bool":
        return 1 if v else 0
    if k == "Real":
        return int(v * 2.0)
    if k == "Pt":
        return v[2][0] * 13 + v[2][1]
    if k == "Rw":
        return v[2][0] * 13 + obs(SH2, v[2][1])
    if k == "Col":
        return {"Red": 1, "Grn": 2, "Blu": 3}[v[1]]
    if k == "Sh2":
        return 4 if v[1] == "A" else 5 + v[2][0]
    if k == "Sh":
        n, a = v[1], v[2]
        if n == "Sq":
            return 6 + a[0]
        if n == "Pr":
            return 7 + a[0] + 3 * obs(lst(INT), a[1])
        if n == "Bx":
            return 8 + obs(PT, a[0])
        if n == "Two":
            return 9 + obs(COL, a[0]) * 2 + obs(SH2, a[1])
        return 10
    if k == "Opt":
        return 2 if v[1] == "None" else 5 + 7 * obs(t[1], v[2][0])
    if k == "List":
        return 1 if not v else obs(t[1], v[0]) + 3 * obs(t, v[1:])
    if k == "Tup":
        return obs(t[1], v[1]) * 11 + obs(t[2], v[2])
    raise ValueError(t)


def obs_fn_body(t):
    k = t[0]
    m = "obs_" + mangle(t)
    if k == "Bool":
        return "fn %s(v: Bool) : Int = if v { 1 } else { 0 }" % m
    if k == "Real":
        return "fn %s(v: Real) : Int = real_to_int(v * 2.0)" % m
    if k == "Pt":
        return "fn %s(v: Pt) : Int = v.x * 13 + v.y" % m
    if k == "Rw":
        return "fn %s(v: Rw) : Int = v.k * 13 + obs_sh2(v.s)" % m
    if k == "Col":
        return "fn %s(v: Col) : Int = match v {\n  Red -> 1\n  Grn -> 2\n  Blu -> 3\n}" % m
    if k == "Sh2":
        return "fn %s(v: Sh2) : Int = match v {\n  A -> 4\n  B(n) -> 5 + n\n}" % m
    if k == "Sh":
        return ("fn %s(v: Sh) : Int = match v {\n  Sq(n) -> 6 + n\n"
                "  Pr(n, xs) -> 7 + n + 3 * obs_list_int(xs)\n  Bx(p) -> 8 + obs_pt(p)\n"
                "  Two(c, s) -> 9 + obs_col(c) * 2 + obs_sh2(s)\n  Nul -> 10\n}" % m)
    if k == "Opt":
        return ("fn %s(v: %s) : Int = match v {\n  None -> 2\n  Some(x) -> 5 + 7 * %s\n}"
                % (m, kai_type(t), obs_call(t[1], "x")))
    if k == "List":
        return ("fn %s(v: %s) : Int = match v {\n  [] -> 1\n  [h, ...t] -> %s + 3 * %s(t)\n}"
                % (m, kai_type(t), obs_call(t[1], "h"), m))
    if k == "Tup":
        return ("fn %s(v: %s) : Int = match v {\n  (a, b) -> %s * 11 + %s\n}"
                % (m, kai_type(t), obs_call(t[1], "a"), obs_call(t[2], "b")))
    raise ValueError(t)


def obs_call(t, name):
    if t == INT:
        return name
    return "obs_%s(%s)" % (mangle(t), name)


def obs_deps(t, acc):
    """Every type whose obs_* function `t`'s obs function reaches."""
    if t in acc or t == INT:
        return
    acc.append(t)
    k = t[0]
    if k == "Rw":
        obs_deps(SH2, acc)
    elif k == "Sh":
        for d in (lst(INT), PT, COL, SH2):
            obs_deps(d, acc)
    elif k in ("Opt", "List"):
        obs_deps(t[1], acc)
    elif k == "Tup":
        obs_deps(t[1], acc)
        obs_deps(t[2], acc)


# --- patterns ---------------------------------------------------------
# ("wild",) ("bind", name) ("lit", v) ("range", lo, hi) ("ctor", name, subs)
# ("rec", type, [(field, sub)]) ("tup", a, b) ("list", fixed, rest)
# ("as", name, sub). `rest` is None (exact length), "_" or a binder name.


class Names:
    def __init__(self, nested_ranges=False):
        self.n = 0
        self.used = set()
        self.nested_ranges = nested_ranges

    def fresh(self):
        name = "b%d" % self.n
        self.n += 1
        self.used.add(name)
        return name


def leaf_pat(rng, t, names, depth=0):
    k = t[0]
    r = rng.random()
    if k == "Int" and r < 0.55:
        if (depth == 0 or names.nested_ranges) and rng.random() < 0.25:
            lo = rng.choice([0, 1, 2])
            return ("range", lo, lo + rng.choice([1, 3]))
        return ("lit", rng.choice(INTS))
    if k == "Bool" and r < 0.6:
        return ("lit", rng.choice([True, False]))
    if k == "Real" and r < 0.5:
        return ("lit", rng.choice(REALS))
    if rng.random() < 0.4:
        return ("wild",)
    return ("bind", names.fresh())


def gen_pat(rng, t, depth, names):
    k = t[0]
    scalar = k in ("Int", "Bool", "Real")
    if scalar or depth >= MAX_DEPTH or rng.random() < 0.15:
        return leaf_pat(rng, t, names, depth)
    p = structured_pat(rng, t, depth, names)
    if rng.random() < 0.1:
        return ("as", names.fresh(), p)
    return p


def structured_pat(rng, t, depth, names):
    k = t[0]
    if k in RECORDS:
        fields = []
        for fname, ftype in RECORDS[k]:
            if rng.random() < 0.25 and fname not in names.used:
                names.used.add(fname)
                fields.append((fname, ("bind", fname)))
            else:
                fields.append((fname, gen_pat(rng, ftype, depth + 1, names)))
        return ("rec", k, fields)
    if is_sum(t):
        name, args = rng.choice(ctors_of(t))
        return ("ctor", name, [gen_pat(rng, a, depth + 1, names) for a in args])
    if k == "Tup":
        return ("tup", gen_pat(rng, t[1], depth + 1, names), gen_pat(rng, t[2], depth + 1, names))
    if k == "List":
        n = rng.choice([0, 1, 1, 2])
        fixed = [gen_pat(rng, t[1], depth + 1, names) for _ in range(n)]
        rest = rng.choice([None, None, "_", "name"])
        if rest == "name":
            rest = names.fresh()
        return ("list", fixed, rest)
    raise ValueError(t)


def kai_pat(t, p):
    tag = p[0]
    if tag == "wild":
        return "_"
    if tag == "bind":
        return p[1]
    if tag == "lit":
        return kai_val(t, p[1])
    if tag == "range":
        return "%d..%d" % (p[1], p[2])
    if tag == "as":
        return "%s @ %s" % (p[1], kai_pat(t, p[2]))
    if tag == "rec":
        parts = []
        for (fname, ftype), (_, sub) in zip(RECORDS[t[0]], p[2]):
            if sub == ("bind", fname):
                parts.append(fname)
            else:
                parts.append("%s: %s" % (fname, kai_pat(ftype, sub)))
        return "{ %s }" % ", ".join(parts)
    if tag == "ctor":
        args = dict(ctors_of(t))[p[1]]
        if not args:
            return p[1]
        return "%s(%s)" % (p[1], ", ".join(kai_pat(a, s) for a, s in zip(args, p[2])))
    if tag == "tup":
        return "(%s, %s)" % (kai_pat(t[1], p[1]), kai_pat(t[2], p[2]))
    if tag == "list":
        items = [kai_pat(t[1], s) for s in p[1]]
        if p[2] is not None:
            items.append("..." + p[2])
        return "[%s]" % ", ".join(items)
    raise ValueError(p)


def binders(t, p, acc):
    """(name, type) of every binder `p` introduces, in source order."""
    tag = p[0]
    if tag == "bind":
        acc.append((p[1], t))
    elif tag == "as":
        acc.append((p[1], t))
        binders(t, p[2], acc)
    elif tag == "rec":
        for (_, ftype), (_, sub) in zip(RECORDS[t[0]], p[2]):
            binders(ftype, sub, acc)
    elif tag == "ctor":
        for a, s in zip(dict(ctors_of(t))[p[1]], p[2]):
            binders(a, s, acc)
    elif tag == "tup":
        binders(t[1], p[1], acc)
        binders(t[2], p[2], acc)
    elif tag == "list":
        for s in p[1]:
            binders(t[1], s, acc)
        if p[2] not in (None, "_"):
            acc.append((p[2], t))
    return acc


def match_pat(t, p, v, env):
    tag = p[0]
    if tag == "wild":
        return True
    if tag == "bind":
        env[p[1]] = v
        return True
    if tag == "lit":
        return v == p[1]
    if tag == "range":
        return p[1] <= v <= p[2]
    if tag == "as":
        env[p[1]] = v
        return match_pat(t, p[2], v, env)
    if tag == "rec":
        return all(match_pat(ft, sub, fv, env)
                   for (_, ft), (_, sub), fv in zip(RECORDS[t[0]], p[2], v[2]))
    if tag == "ctor":
        if v[1] != p[1]:
            return False
        return all(match_pat(a, s, x, env)
                   for a, s, x in zip(dict(ctors_of(t))[p[1]], p[2], v[2]))
    if tag == "tup":
        return match_pat(t[1], p[1], v[1], env) and match_pat(t[2], p[2], v[2], env)
    if tag == "list":
        fixed, rest = p[1], p[2]
        if len(v) < len(fixed) or (rest is None and len(v) != len(fixed)):
            return False
        if not all(match_pat(t[1], s, x, env) for s, x in zip(fixed, v)):
            return False
        if rest not in (None, "_"):
            env[rest] = tuple(v[len(fixed):])
        return True
    raise ValueError(p)


def witness(rng, t, p):
    """A value of `t` that `p` matches."""
    tag = p[0]
    if tag in ("wild", "bind"):
        return rand_val(rng, t, 1)
    if tag == "lit":
        return p[1]
    if tag == "range":
        return rng.randint(p[1], p[2])
    if tag == "as":
        return witness(rng, t, p[2])
    if tag == "rec":
        return ("R", t[0], tuple(witness(rng, ft, sub)
                                 for (_, ft), (_, sub) in zip(RECORDS[t[0]], p[2])))
    if tag == "ctor":
        args = dict(ctors_of(t))[p[1]]
        return ("C", p[1], tuple(witness(rng, a, s) for a, s in zip(args, p[2])))
    if tag == "tup":
        return ("T", witness(rng, t[1], p[1]), witness(rng, t[2], p[2]))
    if tag == "list":
        head = [witness(rng, t[1], s) for s in p[1]]
        extra = rng.choice([0, 1]) if p[2] is not None else 0
        return tuple(head + [rand_val(rng, t[1], 2) for _ in range(extra)])
    raise ValueError(p)


# --- one match --------------------------------------------------------


class Arm:
    def __init__(self, t, pat, guard, weight):
        self.t, self.pat, self.guard, self.weight = t, pat, guard, weight
        self.bs = binders(t, pat, [])

    def body(self):
        terms = [str(self.weight)]
        for i, (name, bt) in enumerate(self.bs):
            terms.append("%d * %s" % (i + 2, obs_call(bt, name)))
        return " + ".join(terms)

    def eval_body(self, env):
        return self.weight + sum((i + 2) * obs(bt, env[name])
                                 for i, (name, bt) in enumerate(self.bs))

    def guard_src(self):
        name, bt, k = self.guard
        return "%s > %d" % (obs_call(bt, name), k)

    def guard_ok(self, env):
        name, bt, k = self.guard
        return obs(bt, env[name]) > k


def gen_guard(rng, arm_bs):
    if not arm_bs or rng.random() > 0.35:
        return None
    name, bt = rng.choice(arm_bs)
    sample = obs(bt, rand_val(rng, bt, 1))
    return (name, bt, max(0, sample + rng.choice([-1, 0, 0, 1])))


class Match:
    def __init__(self, fid, t, arms, values):
        self.fid, self.t, self.arms, self.values = fid, t, arms, values

    def source(self):
        lines = ["fn m%d(v: %s) : Int = match v {" % (self.fid, kai_type(self.t))]
        for a in self.arms:
            g = " if %s" % a.guard_src() if a.guard else ""
            lines.append("  %s%s -> %s" % (kai_pat(self.t, a.pat), g, a.body()))
        lines.append("}")
        return "\n".join(lines)

    def run(self, v):
        for a in self.arms:
            env = {}
            if match_pat(self.t, a.pat, v, env) and (a.guard is None or a.guard_ok(env)):
                return a.eval_body(env)
        raise AssertionError("non-exhaustive generated match m%d" % self.fid)


def catch_all_sub(p):
    if p[0] == "as":
        return catch_all_sub(p[2])
    return p[0] in ("wild", "bind")


def arm_head(p):
    """The typer's redundancy head: an arm repeating an earlier unguarded
    head, or following an unguarded catch-all, is a compile error."""
    if p[0] == "as":
        return arm_head(p[2])
    if p[0] in ("wild", "bind"):
        return "*"
    if p[0] == "ctor" and all(catch_all_sub(s) for s in p[2]):
        return "ctor " + p[1]
    if p[0] == "tup" and catch_all_sub(p[1]) and catch_all_sub(p[2]):
        return "ctor Pair"
    if p[0] == "lit" and not isinstance(p[1], float):
        return "lit " + repr(p[1])
    return None


def gen_match(rng, fid, t, nested_ranges):
    arms, seen = [], set()
    for _ in range(rng.randint(2, 5)):
        names = Names(nested_ranges)
        pat = structured_pat(rng, t, 0, names) if t[0] not in ("Int", "Bool", "Real") \
            else leaf_pat(rng, t, names)
        arm = Arm(t, pat, None, 1000 * (len(arms) + 1))
        arm.guard = gen_guard(rng, arm.bs)
        head = arm_head(pat)
        if arm.guard is None and head in seen:
            continue
        arms.append(arm)
        if arm.guard is None and head is not None:
            seen.add(head)
        if "*" in seen:
            break
    if "*" not in seen:
        tail = Names()
        last = ("bind", tail.fresh()) if rng.random() < 0.3 else ("wild",)
        arms.append(Arm(t, last, None, 9000))
    return Match(fid, t, arms, test_values(rng, t, arms))


def test_values(rng, t, arms):
    seen, vals = set(), []

    def add(v):
        key = repr(v)
        if key not in seen:
            seen.add(key)
            vals.append(v)
    for a in arms:
        for _ in range(2):
            add(witness(rng, t, a.pat))
    for _ in range(4):
        add(rand_val(rng, t))
    return vals


# --- coverage cases ---------------------------------------------------


def cover_pats(rng, t, depth=0):
    """Patterns that together match every value of `t`."""
    k = t[0]
    if k == "Bool":
        return [("lit", True), ("lit", False)]
    if k in ("Int", "Real") or depth >= 2:
        return [("wild",)]
    if k in RECORDS:
        fields = RECORDS[k]
        i = rng.randrange(len(fields))
        return [("rec", k, [(f, s if j == i else ("wild",)) for j, (f, _) in enumerate(fields)])
                for s in cover_pats(rng, fields[i][1], depth + 1)]
    if is_sum(t):
        out = []
        for name, args in ctors_of(t):
            if args and rng.random() < 0.5:
                i = rng.randrange(len(args))
                out += [("ctor", name, [s if j == i else ("wild",) for j in range(len(args))])
                        for s in cover_pats(rng, args[i], depth + 1)]
            else:
                out.append(("ctor", name, [("wild",)] * len(args)))
        return out
    if k == "Tup":
        if rng.random() < 0.5:
            return [("tup", s, ("wild",)) for s in cover_pats(rng, t[1], depth + 1)]
        return [("tup", ("wild",), s) for s in cover_pats(rng, t[2], depth + 1)]
    if k == "List":
        return [("list", [], None)] + [("list", [s], "_") for s in cover_pats(rng, t[1], depth + 1)]
    raise ValueError(t)


def random_arms(rng, t, nested_ranges, n, keep):
    """Up to `n` random arms (some guarded) that `keep` accepts, with the
    typer's redundancy rule respected; returns (arms, unguarded heads)."""
    arms, seen = [], set()
    for _ in range(n):
        names = Names(nested_ranges)
        pat = structured_pat(rng, t, 0, names) if t[0] not in ("Int", "Bool", "Real") \
            else leaf_pat(rng, t, names)
        arm = Arm(t, pat, None, 1000 * (len(arms) + 1))
        arm.guard = gen_guard(rng, arm.bs)
        head = arm_head(pat)
        if (arm.guard is None and head in seen) or "*" in seen or not keep(arm):
            continue
        arms.append(arm)
        if arm.guard is None and head is not None:
            seen.add(head)
    return arms, seen


def gen_complete(rng, fid, t, nested_ranges):
    """A match with no catch-all whose unguarded arms still cover `t`."""
    arms, seen = random_arms(rng, t, nested_ranges, rng.randint(0, 2), lambda a: True)
    if "*" not in seen:
        for pat in cover_pats(rng, t):
            head = arm_head(pat)
            if head is None or head not in seen:
                arms.append(Arm(t, pat, None, 1000 * (len(arms) + 1)))
                if head is not None:
                    seen.add(head)
    return Match(fid, t, arms, test_values(rng, t, arms))


def gen_incomplete(rng, fid, t, nested_ranges):
    """A match whose unguarded arms all miss one value: the typechecker
    must reject it."""
    v = rand_val(rng, t)
    arms, _ = random_arms(rng, t, nested_ranges, rng.randint(2, 5),
                          lambda a: a.guard is not None or not match_pat(t, a.pat, v, {}))
    if not arms:
        a = Arm(t, ("bind", "b0"), None, 1000)
        a.guard = ("b0", t, 0)
        arms = [a]
    return Match(fid, t, arms, [v])


def coverage_cases():
    """Per class: matches complete without a catch-all, and incomplete ones."""
    rng = random.Random(COVERAGE_SEED)
    cases = {}
    for name, types, nr in CLASSES:
        complete = [gen_complete(rng, 0, t, nr) for t in types if t[0] not in ("Int", "Real")]
        incomplete = [gen_incomplete(rng, 0, t, nr) for t in types for _ in range(2)]
        cases[name] = (complete, incomplete)
    return cases


# Each class is one program: a backend that cannot build a class loses only
# that class's combinations, and the ratchet pins each class separately.
CLASSES = [
    ("scalar", [INT, BOOL, REAL], False),
    ("variant", [COL, SH2, opt(INT), opt(BOOL), opt(opt(INT)), opt(SH2)], False),
    ("record", [PT, RW, tup(INT, INT), tup(BOOL, opt(INT)), tup(opt(INT), COL), tup(REAL, INT)], False),
    ("record-in-variant", [opt(PT), opt(tup(INT, INT)), opt(RW), opt(tup(SH2, COL))], False),
    ("list", [lst(INT), lst(opt(INT)), lst(lst(INT)), lst(SH2)], False),
    ("list-in-variant", [opt(lst(INT)), opt(opt(lst(INT))), opt(lst(SH2))], False),
    ("nested-range", [opt(INT), opt(opt(INT)), SH2, opt(SH2)], True),
    ("mixed", [SH, opt(SH), lst(PT), lst(tup(INT, SH2)), tup(SH2, lst(INT)),
               opt(tup(SH2, lst(INT))), tup(SH, SH2), opt(RW)], True),
]


def b(n):
    return ("bind", n)


# Shapes first found broken by hand, kept regardless of the random draw.
PINNED = {
    "record": [
        (PT, [(("rec", "Pt", [("x", ("lit", 0)), ("y", b("y"))]), ("y", INT, 0)),
              (("rec", "Pt", [("x", b("x")), ("y", b("y"))]), ("x", INT, 0)),
              (("wild",), None)],
         [("R", "Pt", (5, 7)), ("R", "Pt", (0, 7)), ("R", "Pt", (0, 0)), ("R", "Pt", (0, 9))]),
    ],
    "record-in-variant": [
        (opt(tup(INT, INT)),
         [(("ctor", "Some", [("tup", ("lit", 0), b("y"))]), None),
          (("ctor", "Some", [("tup", b("x"), ("wild",))]), None),
          (("ctor", "None", []), None)],
         [("C", "Some", (("T", 0, 7),)), ("C", "Some", (("T", 5, 7),)), ("C", "None", ())]),
    ],
    "list-in-variant": [
        (opt(lst(INT)),
         [(("ctor", "Some", [("list", [b("x")], None)]), ("x", INT, 0)),
          (("ctor", "Some", [("wild",)]), None),
          (("ctor", "None", []), None)],
         [("C", "Some", ((3,),)), ("C", "Some", ((3, 4),)), ("C", "Some", ((0,),)), ("C", "None", ())]),
        (opt(opt(lst(INT))),
         [(("ctor", "Some", [("ctor", "Some", [("list", [b("a"), b("c")], None)])]), None),
          (("ctor", "Some", [("wild",)]), None),
          (("wild",), None)],
         [("C", "Some", (("C", "Some", ((3, 4),)),)), ("C", "Some", (("C", "Some", ((3,),)),)),
          ("C", "None", ())]),
    ],
    "mixed": [
        (SH,
         [(("ctor", "Pr", [("lit", 0), ("list", [b("a")], "r")]), None),
          (("ctor", "Pr", [("lit", 0), ("list", [], None)]), None),
          (("ctor", "Pr", [b("n"), ("wild",)]), None),
          (("wild",), None)],
         [("C", "Pr", (0, (4, 5))), ("C", "Pr", (0, ())), ("C", "Pr", (9, (4,))), ("C", "Nul", ())]),
    ],
    "nested-range": [
        (opt(INT),
         [(("ctor", "Some", [("range", 1, 4)]), None), (("wild",), None)],
         [("C", "Some", (0,)), ("C", "Some", (2,)), ("C", "Some", (4,)), ("C", "Some", (7,)), ("C", "None", ())]),
    ],
}


def class_matches(name, types, nested_ranges, rng):
    matches = []
    for t, arm_specs, values in PINNED.get(name, []):
        arms = [Arm(t, pat, guard, 1000 * (i + 1)) for i, (pat, guard) in enumerate(arm_specs)]
        matches.append(Match(len(matches), t, arms, values))
    for t in types:
        for _ in range(FUNCS_PER_TYPE):
            matches.append(gen_match(rng, len(matches), t, nested_ranges))
    return matches


def generate():
    rng = random.Random(SEED)
    cases = coverage_cases()
    out = []
    for name, types, nr in CLASSES:
        matches = class_matches(name, types, nr, rng)
        for m in cases[name][0]:
            m.fid = len(matches)
            matches.append(m)
        out.append((name, matches))
    return out


def incomplete_program():
    """One program of every class's incomplete matches, and the line each
    match starts on."""
    matches, starts, out = [], {}, [DECLS]
    for name, (_, incomplete) in coverage_cases().items():
        for m in incomplete:
            m.fid = len(matches)
            matches.append((name, m))
    deps = []
    for _, m in matches:
        for a in m.arms:
            for _, bt in a.bs:
                obs_deps(bt, deps)
    out += [obs_fn_body(t) for t in deps]
    line = sum(chunk.count("\n") + 2 for chunk in out) + 1
    for name, m in matches:
        starts[line] = (name, m)
        src = m.source()
        out.append(src)
        line += src.count("\n") + 2
    out.append("fn main() : Unit / Stdout = Stdout.print(\"unreachable\")")
    return "\n\n".join(out) + "\n", starts


def reject_gate(kai, d):
    """Every incomplete match must be rejected, naming its own line."""
    src, starts = incomplete_program()
    path = "%s/ps_incomplete.kai" % d
    with open(path, "w") as f:
        f.write(src)
    res = subprocess.run([kai, "typecheck", path], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True)
    rejected, at_header, other = set(), False, []
    for ln in res.stdout.splitlines():
        if ln.startswith("error: non-exhaustive match"):
            at_header = True
        elif ln.startswith("error"):
            other.append(ln)
        elif at_header and "-->" in ln:
            rejected.add(int(ln.rsplit(":", 2)[-2]))
            at_header = False
    rc = 1 if other else 0
    for ln in sorted(set(other))[:8]:
        print("pattern-shape reject: unexpected %s" % ln)
    for name in [c[0] for c in CLASSES]:
        mine = [(ln, m) for ln, (n, m) in starts.items() if n == name]
        missed = [(ln, m) for ln, m in mine if ln not in rejected]
        verdict = "ok" if not missed else "ACCEPTED A NON-EXHAUSTIVE MATCH"
        print("pattern-shape reject %-18s %4d/%-4d accepted  %s" % (name, len(missed), len(mine), verdict))
        for ln, m in missed:
            rc = 1
            print("  line %d: no arm covers %s\n%s" % (ln, kai_val(m.t, m.values[0]), m.source()))
    return rc


def program(matches):
    deps = []
    for m in matches:
        for a in m.arms:
            for _, bt in a.bs:
                obs_deps(bt, deps)
    out = [DECLS]
    for t in deps:
        out.append(obs_fn_body(t))
    for m in matches:
        out.append(m.source())
    for m in matches:
        body = ["  Stdout.print(int_to_string(m%d(%s)))" % (m.fid, kai_val(m.t, v)) for v in m.values]
        out.append("fn t%d() : Unit / Stdout {\n%s\n}" % (m.fid, "\n".join(body)))
    calls = "\n".join("  t%d()" % m.fid for m in matches)
    out.append("fn main() : Unit / Stdout {\n%s\n}" % calls)
    return "\n\n".join(out) + "\n"


def expected(matches):
    return ["%d" % m.run(v) for m in matches for v in m.values]


def labels(matches):
    return ["m%d(%s)" % (m.fid, kai_val(m.t, v)) for m in matches for v in m.values]


def emit(d):
    with open("%s/ps_incomplete.kai" % d, "w") as f:
        f.write(incomplete_program()[0])
    for name, matches in generate():
        with open("%s/ps_%s.kai" % (d, name.replace("-", "_")), "w") as f:
            f.write(program(matches))
        with open("%s/ps_%s.out.expected" % (d, name.replace("-", "_")), "w") as f:
            f.write("".join(line + "\n" for line in expected(matches)))


def run_class(kai, backend, d, name, matches):
    """(diverging combinations, total, detail lines) for one class."""
    src = "%s/ps_%s.kai" % (d, name.replace("-", "_"))
    with open(src, "w") as f:
        f.write(program(matches))
    exe = "%s/ps_%s_%s" % (d, name.replace("-", "_"), backend)
    exp = expected(matches)
    build = subprocess.run([kai, "build", "--backend=%s" % backend, src, "-o", exe],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if build.returncode != 0:
        errs = [ln for ln in build.stdout.splitlines()
                if ln.startswith(("error", "native:", "kir:"))]
        return len(exp), len(exp), ["  build failed: " + e for e in sorted(set(errs))[:8]]
    run = subprocess.run([exe], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    got = run.stdout.splitlines()
    names = labels(matches)
    bad = [i for i, e in enumerate(exp) if i >= len(got) or got[i] != e]
    detail = ["  %s: expected %s, got %s" % (names[i], exp[i], got[i] if i < len(got) else "<missing>")
              for i in bad[:20]]
    return len(bad), len(exp), detail


def read_baseline(path):
    pins = {}
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) == 3 and not line.startswith("#"):
                pins[(parts[0], parts[1])] = int(parts[2])
    return pins


def gate(kai, backend, baseline, d):
    pins = read_baseline(baseline)
    rc = reject_gate(kai, d) if backend == "c" else 0
    for name, matches in generate():
        bad, total, detail = run_class(kai, backend, d, name, matches)
        pin = pins.get((name, backend), 0)
        verdict = "ok"
        if bad > pin:
            verdict, rc = "REGRESSED (pin %d)" % pin, 1
        elif bad < pin:
            verdict, rc = "IMPROVED: lower the pin to %d" % bad, 1
        print("pattern-shape %s %-18s %4d/%-4d diverge  %s" % (backend, name, bad, total, verdict))
        if bad > pin:
            print("\n".join(detail))
    return rc


def main(argv):
    if len(argv) == 3 and argv[1] == "emit":
        emit(argv[2])
        return 0
    if len(argv) == 6 and argv[1] == "gate":
        return gate(argv[2], argv[3], argv[4], argv[5])
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
