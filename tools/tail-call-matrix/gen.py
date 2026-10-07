#!/usr/bin/env python3
"""Write tools/tail-call-matrix's program to the file given as argv[1].

The program holds one tail-call loop per shape, each under its own function
names, and runs the shape named by its first argument: five million turns,
exit 0 on completion. A call in tail position that is not a jump overflows
the fiber stack and kills the process, so each shape runs in an exec of its
own. Mutual shapes also come with a form ahead of the tail call (lambda,
record, list, interpolation, pipe). Prints the shape names, one per line.
"""
import sys
from itertools import product

TURNS = 5000000

def names(k, unread):
    # k parameters in all; the never-read one takes the place of an `x`.
    lead = ["_u", "n", "acc"] if unread else ["n", "acc"]
    return lead + [f"x{i}" for i in range(k - len(lead))]

def sig(fn, k, unread, row):
    ps = ", ".join(f"{p}: Int" for p in names(k, unread))
    return f"fn {fn}({ps}) : Int{' / Tick' if row else ''}"

def args(k, unread, n, acc):
    vals = {"_u": "0", "n": n, "acc": acc}
    return ", ".join(vals.get(p, str(i)) for i, p in enumerate(names(k, unread)))

def reads(k, unread):
    # every `x` is read, so only `_u` is ever unread
    return "".join(f" + {x} - {x}" for x in names(k, unread) if x.startswith("x"))

# A form in the caller's body ahead of the tail call; each yields `acc + 1`.
FORMS = {
    "lambda": ("  let f = (v: Int) => v\n", "f(acc + 1)"),
    "record": ("  let r = Box { v: acc + 1 }\n", "r.v"),
    "list":   ("  let k = match [acc + 1] {\n    [h, ...t] -> h\n    [] -> 0\n  }\n", "k"),
    "interp": ("  let s = \"#{n}\"\n", "acc + 1 + string_length(s) - string_length(s)"),
    "pipe":   ("  let p = (acc + 1) |> ident\n", "p"),
}

def shape_fns(tag, shape, arity, row, unread, form=None):
    if shape == "self":
        go = f"{tag}_go"
        body = (f"{sig(go, 3, unread, row)} =\n"
                f"  if n == 0 {{ acc }} else {{ {go}({args(3, unread, 'n - 1', 'acc + 1' + reads(3, unread))}) }}\n")
        return body, f"{go}({args(3, unread, str(TURNS), '0')})"
    if shape == "ring":
        return ring_fns(tag, row)
    if arity == "swapped":
        return swap_fns(tag, row)
    kf, kg = 3, {"equal": 3, "smaller": 2, "larger": 4}[arity]
    ping, pong = f"{tag}_ping", f"{tag}_pong"
    pre, step = FORMS[form] if form else ("", "acc + 1")
    body = (f"{sig(ping, kf, unread, row)} =\n"
            f"  if n == 0 {{ acc }} else {{\n{pre}  {pong}({args(kg, False, 'n', step + reads(kf, unread))})\n  }}\n\n"
            f"{sig(pong, kg, False, row)} =\n"
            f"  {ping}({args(kf, unread, 'n - 1', 'acc' + reads(kg, False))})\n")
    return body, f"{ping}({args(kf, unread, str(TURNS), '0')})"

# Three members of three arities: the dispatch has to tell more than two
# arms apart.
def ring_fns(tag, row):
    eff = " / Tick" if row else ""
    a, b, c = f"{tag}_a", f"{tag}_b", f"{tag}_c"
    body = (f"fn {a}(n: Int, acc: Int, x0: Int) : Int{eff} =\n"
            f"  if n == 0 {{ acc }} else {{ {b}(n, acc + 1 + x0 - x0) }}\n\n"
            f"fn {b}(n: Int, acc: Int) : Int{eff} = {c}(n, acc, 1, 2)\n\n"
            f"fn {c}(n: Int, acc: Int, x0: Int, x1: Int) : Int{eff} = {a}(n - 1, acc + x0 - x0 + x1 - x1, 3)\n")
    return body, f"{a}({TURNS}, 0, 3)"

# The members take their parameters in different orders, so one of them
# lands in the slots out of order, and its calls pass computed arguments.
def swap_fns(tag, row):
    eff = " / Tick" if row else ""
    ping, pong = f"{tag}_ping", f"{tag}_pong"
    body = (f"fn {ping}(n: Int, s: String, acc: Int) : Int{eff} =\n"
            f"  if n == 0 {{ acc }} else {{ {pong}(s ++ \"\", acc + 1, n - 1 + 1) }}\n\n"
            f"fn {pong}(s: String, acc: Int, n: Int) : Int{eff} =\n"
            f"  {ping}(n - 1, s ++ \"\", acc + string_length(s) - 1)\n")
    return body, f"{ping}({TURNS}, \"x\", 0)"

BOOLS = (False, True)
FIBERS = ("main", "spawned")
shapes = [("self", "equal", r, u, f, None) for r, u, f in product(BOOLS, BOOLS, FIBERS)]
shapes += [("mutual", a, r, u, f, None)
           for a, r, u, f in product(("equal", "smaller", "larger"), BOOLS, BOOLS, FIBERS)]
shapes += [("mutual", "equal", r, False, f, form) for form, r, f in product(FORMS, BOOLS, FIBERS)]
shapes += [(sh, ar, r, False, f, None) for (sh, ar), r, f in product((("ring", "mixed"), ("mutual", "swapped")), BOOLS, FIBERS)]

out = ["import spawn\n", "effect Tick {\n  next(v: Int) : Int\n}\n",
       "type Box = { v: Int }\n", "fn ident(v: Int) : Int = v\n"]
arms = []
for i, (shape, arity, row, unread, fiber, form) in enumerate(shapes):
    name = "-".join([shape, arity, "row" if row else "norow", "unread" if unread else "read", fiber]
                    + ([form] if form else []))
    fns, entry = shape_fns(f"t{i}", shape, arity, row, unread, form)
    out.append(fns)
    call = f"handle {{ {entry} }} with Tick {{ next(v, resume) -> resume(v) }}" if row else entry
    if fiber == "spawned":
        call = f"nursery {{ spawn.await(spawn.spawn(() => {call})) }}"
    arms.append((name, call))
    print(name)

chain = "".join(f"  if sel == \"{n}\" {{ done({c}) }} else\n" for n, c in arms)
out.append(f"fn done(v: Int) : Int = if v == {TURNS} {{ 0 }} else {{ 1 }}\n")
out.append("fn main() : Int / Spawn + Env {\n"
           "  let sel = match Env.args() {\n    [] -> \"\"\n    [s, ...rest] -> s\n  }\n"
           + chain + "  { 2 }\n}\n")
with open(sys.argv[1], "w") as f:
    f.write("\n".join(out))
