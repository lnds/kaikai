#!/usr/bin/env python3
"""Write tools/tail-call-matrix's program to the file given as argv[1].

The program holds one tail-call loop per shape, each under its own function
names, and runs the shape named by its first argument: five million turns,
exit 0 on completion. A call in tail position that is not a jump overflows
the fiber stack and kills the process, so each shape runs in an exec of its
own. Prints the shape names, one per line.
"""
import sys

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

def shape_fns(tag, shape, arity, row, unread):
    if shape == "self":
        go = f"{tag}_go"
        body = (f"{sig(go, 3, unread, row)} =\n"
                f"  if n == 0 {{ acc }} else {{ {go}({args(3, unread, 'n - 1', 'acc + 1' + reads(3, unread))}) }}\n")
        return body, f"{go}({args(3, unread, str(TURNS), '0')})"
    kf, kg = 3, {"equal": 3, "smaller": 2, "larger": 4}[arity]
    ping, pong = f"{tag}_ping", f"{tag}_pong"
    body = (f"{sig(ping, kf, unread, row)} =\n"
            f"  if n == 0 {{ acc }} else {{ {pong}({args(kg, False, 'n', 'acc + 1' + reads(kf, unread))}) }}\n\n"
            f"{sig(pong, kg, False, row)} =\n"
            f"  {ping}({args(kf, unread, 'n - 1', 'acc' + reads(kg, False))})\n")
    return body, f"{ping}({args(kf, unread, str(TURNS), '0')})"

shapes = []
for shape in ("self", "mutual"):
    for arity in (("equal",) if shape == "self" else ("equal", "smaller", "larger")):
        for row in (False, True):
            for unread in (False, True):
                for fiber in ("main", "spawned"):
                    shapes.append((shape, arity, row, unread, fiber))

out = ["import spawn\n", "effect Tick {\n  next(v: Int) : Int\n}\n"]
arms = []
for i, (shape, arity, row, unread, fiber) in enumerate(shapes):
    name = "-".join([shape, arity, "row" if row else "norow", "unread" if unread else "read", fiber])
    fns, entry = shape_fns(f"t{i}", shape, arity, row, unread)
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
