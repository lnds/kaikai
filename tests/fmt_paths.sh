#!/bin/sh
# tests/fmt_paths.sh — `kai fmt` and `kai fmt --check` over several paths.
#
# Pins: files and directories mix in one run, output order is sorted,
# --check lists every unformatted file and exits non-zero once, and a
# file that fails to parse does not stop the rest from being formatted.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"

if [ ! -x "$ROOT/stage2/kaic2" ]; then
  echo "fmt_paths: kaic2 not built; run 'make kaic2' first" >&2
  exit 2
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM
fails=0

run_fmt() {
  rf_dir="$1"; shift
  got="$(cd "$rf_dir" && "$KAI" fmt "$@" 2>"$tmp/err")" && rc=0 || rc=$?
}

expect() {
  if [ "$rc" != "$2" ]; then
    echo "  FAIL $1 — exit $rc, want $2" >&2
    fails=$((fails + 1))
  elif [ "$got" != "$3" ]; then
    echo "  FAIL $1 — stdout [$got], want [$3]" >&2
    fails=$((fails + 1))
  fi
}

check() {
  if [ "$2" != 1 ]; then
    echo "  FAIL $1" >&2
    fails=$((fails + 1))
  fi
}

ugly() { printf 'fn  main( ) : Unit   / Stdout =    Stdout.print("%s")\n' "$1" > "$2"; }
# 1 when the file no longer has the unformatted spelling.
is_formatted() { if grep -q 'fn  main( )' "$1"; then echo 0; else echo 1; fi; }
is_untouched() { if grep -q 'fn  main( )' "$1"; then echo 1; else echo 0; fi; }
nonzero() { if [ "$rc" != 0 ]; then echo 1; else echo 0; fi; }

w="$tmp/w"
mkdir -p "$w/pkg"
printf 'name = "pkg"\nversion = "0.1.0"\nentry = "main.kai"\n' > "$w/pkg/kai.toml"

# Two files: both rewritten in place.
ugly a "$w/b.kai"; ugly b "$w/a.kai"
run_fmt "$w" b.kai a.kai
expect "two files: exit 0" 0 ""
check "two files: first rewritten" "$(is_formatted "$w/b.kai")"
check "two files: second rewritten" "$(is_formatted "$w/a.kai")"

# A file plus a package directory.
ugly p "$w/pkg/main.kai"; ugly c "$w/c.kai"
run_fmt "$w" c.kai pkg
expect "file + dir: exit 0" 0 ""
check "file + dir: file rewritten" "$(is_formatted "$w/c.kai")"
check "file + dir: dir member rewritten" "$(is_formatted "$w/pkg/main.kai")"

# --check: nothing written, every file listed in sorted order, one exit.
ugly z "$w/z.kai"; ugly y "$w/y.kai"; ugly p "$w/pkg/main.kai"
run_fmt "$w" --check z.kai y.kai pkg
expect "--check: all listed, sorted, exit 1" 1 "pkg/main.kai
y.kai
z.kai"
check "--check: wrote nothing" "$(is_untouched "$w/z.kai")"

run_fmt "$w" --check a.kai b.kai c.kai
expect "--check on formatted files: silent, exit 0" 0 ""

# A parse error in one of three files: reported by path, the others still format.
ugly m1 "$w/m1.kai"; ugly m3 "$w/m3.kai"
printf 'fn main( : Unit =\n' > "$w/m2.kai"
run_fmt "$w" m1.kai m2.kai m3.kai
expect "parse error: exit 1 at the end" 1 ""
if grep -q 'm2.kai' "$tmp/err"; then named=1; else named=0; fi
check "parse error: path reported" "$named"
check "parse error: file before formatted" "$(is_formatted "$w/m1.kai")"
check "parse error: file after formatted" "$(is_formatted "$w/m3.kai")"

# A missing path aborts before anything is written.
ugly n1 "$w/n1.kai"
run_fmt "$w" n1.kai nope.kai
check "missing path: exit non-zero" "$(nonzero)"
check "missing path: nothing written" "$(is_untouched "$w/n1.kai")"

# --stdin takes no paths.
run_fmt "$w" --stdin a.kai
check "--stdin with a path: rejected" "$(nonzero)"

if [ "$fails" != 0 ]; then
  echo "fmt_paths: $fails check(s) failed" >&2
  exit 1
fi
echo "fmt_paths: several paths ok"
