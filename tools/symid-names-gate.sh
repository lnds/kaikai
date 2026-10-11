#!/bin/sh
# Names stay out of identity.
#
# After name resolution a pass decides which declaration, type, effect,
# constructor or local something is by its id, never by comparing text.
# Five checks hold that:
#
#   1. no type definition holds text beside the id of the same thing;
#   2. every call of `written_text`, the one way characters leave a
#      written name, is in a listed function for a listed reason;
#   3. every string literal that name text is tested against is listed
#      with its reason;
#   4. a reference by text is built only before the resolver;
#   5. no `==` or `!=` compares a value that holds a written name, in the
#      compiler as monomorphised (`--check-written-eq`).
#
# Checks 1-4 read the sources (tools/symid-census/gate.py, which
# documents each ledger); check 5 runs the compiler on itself, after
# proving on a probe that the check still sees what it must.
#
# A site that breaks the rule and is not converted yet is a row of
# tools/symid-census/pending-written.txt. `--final` fails while that
# file has a row.
#
# Usage: symid-names-gate.sh [--final]
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CENSUS="$ROOT/tools/symid-census"
KAIC2="$ROOT/stage2/kaic2"
EDITION="$(cat "$ROOT/EDITION")"

python3 "$CENSUS/gate.py" "$ROOT/stage2/compiler" "$@"

# The functions `--check-written-eq` reports for a program, one per line.
compared() {
  "$KAIC2" --edition "$EDITION" --check-written-eq "$@" 2>&1 >/dev/null \
    | awk '$1 == "written-eq" { print $2 }' | sort -u
}

fail() {
  echo "symid-names FAIL — $1" >&2
  exit 1
}

probe=$(compared --path "$ROOT/stage2" --path "$ROOT/stdlib" "$CENSUS/probe/main.kai")
for f in cmp_direct cmp_field cmp_nested cmp_binder cmp_ref has__mono__Written; do
  printf '%s\n' "$probe" | grep -qx "$f" || fail "--check-written-eq no longer reports the probe's $f"
done
if printf '%s\n' "$probe" | grep -q '^ok_'; then
  fail "--check-written-eq reports a probe comparison that holds no written name"
fi

listed=$(grep -v '^#' "$CENSUS/written-eq.txt" | cut -f1)
extra=$(compared "$ROOT/stage2/main.kai" | grep -vxF -e "$listed" || true)
if [ -n "$extra" ]; then
  echo "symid-names FAIL — a value holding a written name is compared in:" >&2
  printf '%s\n' "$extra" | sed 's/^/  /' >&2
  exit 1
fi
echo "symid-names OK"
