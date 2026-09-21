#!/usr/bin/env python3
"""Check a `--mutate-list-json` catalogue against the mutants it describes.

usage: check_mutate_sites.py <sites.json> <source.kai> <mutant-prefix>

Each site `i` must have a mutant at `<mutant-prefix><i>.kai` (the output
of `--mutate-apply i`) equal to the source with `[start.byte, end.byte)`
replaced by `replacement`, and `original` must be that span's text.
"""
import json
import sys


def fail(msg):
    print(f"check_mutate_sites: {msg}")
    sys.exit(1)


def check_site(site, src, prefix):
    i = site["id"]
    if site["start"] is None:
        fail(f"site {i} does not resolve to a span")
    a, b = site["start"]["byte"], site["end"]["byte"]
    if src[a:b].decode() != site["original"]:
        fail(f"site {i}: original {site['original']!r} != span text {src[a:b].decode()!r}")
    want = src[:a] + site["replacement"].encode() + src[b:]
    with open(f"{prefix}{i}.kai", "rb") as f:
        got = f.read()
    if got != want:
        fail(f"site {i}: --mutate-apply differs from the span splice")


def main():
    sites_path, src_path, prefix = sys.argv[1:4]
    with open(sites_path) as f:
        sites = json.load(f)
    with open(src_path, "rb") as f:
        src = f.read()
    for site in sites:
        check_site(site, src, prefix)
    ops = {s["operator"] for s in sites}
    missing = {"arm", "compare", "connect", "negate", "literal", "call"} - ops
    if missing:
        fail(f"no site for operator(s) {sorted(missing)}")
    if not any(s["operator"] == "arm" and s["enclosing"].count("(") for s in sites):
        fail("no arm site inside an impl method")
    print(f"mutate-site-data OK ({len(sites)} sites splice-equal)")


main()
