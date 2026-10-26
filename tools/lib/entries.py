#!/usr/bin/env python3
"""One file per baseline entry under tools/baselines/<gate>/.

  entries.py cat <dir> <nkey>       print every row, tab-separated (key columns first)
  entries.py put <dir> <nkey> <col>...   write the entry for a row
  entries.py name <nkey> <col>...   print the file name of a row's key

A file holds one row; its name is derived from the first `nkey` columns, so two branches adding
the same key collide on one path and two adding different keys never touch the same file. The
loader rejects a name that does not match its row. `README.md` and dotfiles are not entries.
"""
import hashlib, os, re, sys

SAFE = re.compile(r"[^A-Za-z0-9._-]")
MAX_STEM = 100


def name(key):
    """The file name of a key: readable, and hashed whenever the readable form is lossy."""
    stem = "__".join(SAFE.sub("_", c) for c in key)
    if stem != "__".join(key) or len(stem) > MAX_STEM or stem.startswith("."):
        digest = hashlib.sha1("\0".join(key).encode()).hexdigest()[:8]
        stem = stem[:MAX_STEM].lstrip(".") + "~" + digest
    return stem


def load(directory, nkey):
    """The rows of a baseline directory, sorted; problems are raised as ValueError."""
    rows, seen, bad = [], {}, []
    if not os.path.isdir(directory):
        return rows
    for fname in sorted(os.listdir(directory)):
        if fname == "README.md" or fname.startswith("."):
            continue
        text = open(os.path.join(directory, fname), encoding="utf-8").read()
        if not text.endswith("\n") or "\n" in text[:-1]:
            bad.append("%s: an entry is exactly one line" % fname)
            continue
        row = text[:-1].split("\t")
        if len(row) < nkey:
            bad.append("%s: %d columns, the key alone takes %d" % (fname, len(row), nkey))
            continue
        key = tuple(row[:nkey])
        if name(key) != fname:
            bad.append("%s: named for a different key, expected %s" % (fname, name(key)))
        if key in seen:
            bad.append("%s: same key as %s" % (fname, seen[key]))
        seen[key] = fname
        rows.append(row)
    if bad:
        raise ValueError("\n".join(directory + "/" + b for b in bad))
    return rows


def put(directory, nkey, row):
    os.makedirs(directory, exist_ok=True)
    for col in row:
        if "\t" in col or "\n" in col:
            raise ValueError("a column holds a tab or newline: %r" % col)
    with open(os.path.join(directory, name(tuple(row[:nkey]))), "w", encoding="utf-8") as f:
        f.write("\t".join(row) + "\n")


if __name__ == "__main__":
    sys.dont_write_bytecode = True
    cmd, args = sys.argv[1], sys.argv[2:]
    try:
        if cmd == "cat":
            for r in load(args[0], int(args[1])):
                print("\t".join(r))
        elif cmd == "put":
            put(args[0], int(args[1]), args[2:])
        elif cmd == "name":
            print(name(tuple(args[1:1 + int(args[0])])))
        else:
            sys.exit(__doc__)
    except ValueError as e:
        print(e, file=sys.stderr)
        sys.exit(1)
