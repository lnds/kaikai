#!/usr/bin/env python3
"""Wall-time A/B of `kai build` on a generated multi-module package.

cost-ab measures kaic2 on the self-compile and the empty program; neither
reaches the wrapper's per-module work (cache lookups, object holds, link), which
grows with the module count. This builds one package of N modules with the
native backend under two trees' `bin/kai`, alternating, each side with its own
warm cache, and compares medians.

  tools/pkg-cost-ab.py [--modules N] [--rounds N] [--fail-above PCT] [--markdown]
                       <base-tree> <branch-tree>

A tree is a directory whose `bin/kai` runs on its own. Exit 1 when the branch
median is above the base median by more than PCT percent.
"""
import argparse
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

NAME = "synth"


def module(i):
    n = f"{i:03d}"
    imp = f"import {NAME}.m{i - 1:03d}\n" if i > 1 else ""
    chain = (
        f"mk{n}(n).a{n} + mk{n}(n).b{n}"
        if i == 1
        else f"chain{i - 1:03d}(n) + mk{n}(n).a{n}"
    )
    return (
        f"{imp}#[derive(Eq, Show)]\n"
        f"pub type R{n} = {{ a{n}: Int, b{n}: Int, c{n}: String }}\n"
        f"pub fn mk{n}(n: Int) : R{n} = R{n} {{ a{n}: n, b{n}: n * 2, c{n}: int_to_string(n) }}\n"
        f"pub fn chain{n}(n: Int) : Int = {chain}\n"
    )


def generate(root, modules):
    os.makedirs(f"{root}/{NAME}")
    with open(f"{root}/kai.toml", "w") as f:
        f.write(f'name = "{NAME}"\nversion = "0.1.0"\n\n[dependencies]\n')
    for i in range(1, modules + 1):
        with open(f"{root}/{NAME}/m{i:03d}.kai", "w") as f:
            f.write(module(i))
    with open(f"{root}/main.kai", "w") as f:
        f.write(
            f"import {NAME}.m{modules:03d}\n\n"
            f"fn main() : Unit / Stdout = Stdout.print(int_to_string(chain{modules:03d}(7)))\n"
        )


def build(side, work):
    env = dict(
        os.environ,
        KAI_BIN_MEMO="0",
        KAI_BACKEND="native",
        XDG_CACHE_HOME=f"{work}/cache-{side['name']}",
    )
    t = time.perf_counter()
    r = subprocess.run(
        [side["kai"], "build", "-o", "out"],
        cwd=side["pkg"],
        env=env,
        capture_output=True,
        text=True,
    )
    d = time.perf_counter() - t
    if r.returncode != 0:
        sys.exit(f"pkg-cost-ab: {side['name']} build failed:\n{r.stderr[-2000:]}")
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--modules", type=int, default=41)
    ap.add_argument("--rounds", type=int, default=9)
    ap.add_argument("--fail-above", type=float, default=None)
    ap.add_argument("--markdown", action="store_true")
    ap.add_argument("base")
    ap.add_argument("branch")
    a = ap.parse_args()

    work = tempfile.mkdtemp(prefix="pkg-cost-ab.")
    try:
        sides = []
        for name, tree in (("base", a.base), ("branch", a.branch)):
            pkg = f"{work}/pkg-{name}"
            generate(pkg, a.modules)
            kai = os.path.abspath(f"{tree}/bin/kai")
            if not os.access(kai, os.X_OK):
                sys.exit(f"pkg-cost-ab: not executable: {kai}")
            sides.append({"name": name, "kai": kai, "pkg": pkg, "t": []})
        for s in sides:
            build(s, work)
        for r in range(a.rounds):
            for s in sides if r % 2 == 0 else sides[::-1]:
                s["t"].append(build(s, work))
        stat = {s["name"]: (min(s["t"]), statistics.median(s["t"])) for s in sides}
        spread = sorted(
            100.0 * (r - b) / b for b, r in zip(sides[0]["t"], sides[1]["t"])
        )
        (bmin, bmed), (rmin, rmed) = stat["base"], stat["branch"]
        pct = 100.0 * (rmed - bmed) / bmed
        over = a.fail_above is not None and pct > a.fail_above
        verdict = f"up, over the {a.fail_above}% budget" if over else "ok"
        if a.markdown:
            print("| probe | base min / median (s) | branch min / median (s) | delta (%) | per-round delta min / median / max (%) | verdict |")
            print("|---|---:|---:|---:|---:|---|")
            print(
                f"| {a.modules}-module package build | {bmin:.3f} / {bmed:.3f} | "
                f"{rmin:.3f} / {rmed:.3f} | {pct:+.2f} | "
                f"{spread[0]:+.1f} / {statistics.median(spread):+.1f} / {spread[-1]:+.1f} | {verdict} |"
            )
        else:
            print(
                f"{a.modules}-module package build: base {bmin:.3f}/{bmed:.3f} s, "
                f"branch {rmin:.3f}/{rmed:.3f} s, {pct:+.2f}% "
                f"(per-round {spread[0]:+.1f}/{statistics.median(spread):+.1f}/{spread[-1]:+.1f}%, {verdict})"
            )
        return 1 if over else 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
