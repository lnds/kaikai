#!/usr/bin/env python3
"""Check that every --effects-json record names the file declaring it.

Usage: effects_json_files.py <report.json> <root.kai>

Fails unless every record carries a `file` that exists, and at least one
record belongs to another file (the auto-loaded core). Prints the root
file's records, with their row and handlers, for a golden diff.
"""
import json
import os
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: effects_json_files.py <report.json> <root.kai>", file=sys.stderr)
        return 2
    with open(sys.argv[1]) as fh:
        data = json.load(fh)
    root = os.path.realpath(sys.argv[2])
    for i, row in enumerate(data):
        if not os.path.isfile(row.get("file", "")):
            print(f"record {i} ({row.get('fn')}): no existing `file`", file=sys.stderr)
            return 1
    if all(os.path.realpath(r["file"]) == root for r in data):
        print("no record attributed to a file other than the root", file=sys.stderr)
        return 1
    for r in data:
        if os.path.realpath(r["file"]) == root:
            hs = ", ".join(f"{h['effect']}@{h['line']}:{h['col']}" for h in r["handlers_installed"])
            row = ", ".join(r["effects"]) + (" + ..." if r["row_open"] else "")
            print(f"{r['fn']} {r['line']}:{r['col']} row [{row}] handlers [{hs}]")
    return 0


if __name__ == "__main__":
    sys.exit(main())
