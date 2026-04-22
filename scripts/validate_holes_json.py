#!/usr/bin/env python3
"""Validate --holes-json output against the stable schema.

Exit 0 iff the file parses as a JSON array and every element carries
the fixed key set from docs/typed-holes.md.
"""
import json
import sys

REQUIRED = {
    "file",
    "line",
    "col",
    "name",
    "expected_type",
    "in_scope",
    "candidates",
}


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: validate_holes_json.py <file>", file=sys.stderr)
        return 2
    with open(sys.argv[1]) as fh:
        data = json.load(fh)
    if not isinstance(data, list) or not data:
        print("expected non-empty JSON array", file=sys.stderr)
        return 1
    for i, row in enumerate(data):
        missing = REQUIRED - set(row.keys())
        if missing:
            print(f"entry {i}: missing keys {missing}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
