#!/bin/sh
# tests/migrate_paths.sh — `kai migrate` over several paths.
#
# Pins: files and package directories mix in one run, --write rewrites
# each, a missing path aborts before anything is written, a file that
# fails does not stop the rest, and the dry-run labels each file.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
FIX="$ROOT/examples/migrate"

if [ ! -x "$ROOT/stage2/kaic2" ]; then
  echo "migrate_paths: kaic2 not built; run 'make kaic2' first" >&2
  exit 2
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM
fails=0

run_migrate() {
  rm_dir="$1"; shift
  got="$(cd "$rm_dir" && "$KAI" migrate --from tongariki "$@" 2>"$tmp/err")" && rc=0 || rc=$?
}

check() {
  if [ "$2" != 1 ]; then
    echo "  FAIL $1" >&2
    fails=$((fails + 1))
  fi
}

fresh() {
  rm -rf "$tmp/w"
  mkdir -p "$tmp/w/pkg"
  cp "$FIX/legacy_consts.kai" "$tmp/w/"
  cp "$FIX/legacy_consts.kai" "$tmp/w/pkg/"
  printf 'name = "pkg"\nversion = "0.1.0"\nentry = "main.kai"\n' > "$tmp/w/pkg/kai.toml"
  for f in a.kai b.kai c.kai pkg/main.kai; do
    cp "$FIX/selector_const.input.kai" "$tmp/w/$f"
  done
}

# 1 when the file carries the migrated spelling.
is_migrated() { if grep -q 'LIMIT_N' "$1"; then echo 1; else echo 0; fi; }
is_untouched() { if grep -q 'limit_n' "$1"; then echo 1; else echo 0; fi; }
nonzero() { if [ "$rc" != 0 ]; then echo 1; else echo 0; fi; }

# Two files: both rewritten in place.
fresh
run_migrate "$tmp/w" --write b.kai a.kai
check "two files: exit 0" "$([ "$rc" = 0 ] && echo 1 || echo 0)"
check "two files: first rewritten" "$(is_migrated "$tmp/w/a.kai")"
check "two files: second rewritten" "$(is_migrated "$tmp/w/b.kai")"
check "two files: others untouched" "$(is_untouched "$tmp/w/c.kai")"

# A file plus a package directory.
fresh
run_migrate "$tmp/w" --write c.kai pkg
check "file + dir: exit 0" "$([ "$rc" = 0 ] && echo 1 || echo 0)"
check "file + dir: file rewritten" "$(is_migrated "$tmp/w/c.kai")"
check "file + dir: dir member rewritten" "$(is_migrated "$tmp/w/pkg/main.kai")"

# Dry-run over several files: each under its header, nothing written.
fresh
run_migrate "$tmp/w" b.kai a.kai
check "dry-run: exit 0" "$([ "$rc" = 0 ] && echo 1 || echo 0)"
check "dry-run: headers sorted" "$([ "$(printf '%s\n' "$got" | grep '^# kai migrate:')" = "# kai migrate: a.kai
# kai migrate: b.kai" ] && echo 1 || echo 0)"
check "dry-run: wrote nothing" "$(is_untouched "$tmp/w/a.kai")"

# Dry-run over one file: no header.
run_migrate "$tmp/w" a.kai
check "dry-run single: no header" "$(printf '%s\n' "$got" | grep -q '^# kai migrate:' && echo 0 || echo 1)"

# A missing path aborts before anything is written.
fresh
run_migrate "$tmp/w" --write a.kai nope.kai
check "missing path: exit non-zero" "$(nonzero)"
check "missing path: nothing written" "$(is_untouched "$tmp/w/a.kai")"

# A parse error in the middle: reported by path, the others still migrate.
fresh
printf 'fn main( : Unit =\n' > "$tmp/w/b.kai"
run_migrate "$tmp/w" --write a.kai b.kai c.kai
check "parse error: exit 1 at the end" "$([ "$rc" = 1 ] && echo 1 || echo 0)"
check "parse error: file before migrated" "$(is_migrated "$tmp/w/a.kai")"
check "parse error: file after migrated" "$(is_migrated "$tmp/w/c.kai")"

if [ "$fails" != 0 ]; then
  echo "migrate_paths: $fails check(s) failed" >&2
  exit 1
fi
echo "migrate_paths: several paths ok"
