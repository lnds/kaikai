#!/bin/sh
# tests/fmt_package.sh — the package mode of `kai fmt`.
#
# The sibling fmt gates all drive one file at a time, so none of them
# can see the package form (`kai fmt .`), where the driver enumerates
# what the package owns and folds each file's status into one exit
# code. Two properties are load-bearing there and invisible per-file:
#
#   status   an already-formatted package exits 0 and says nothing.
#            The enumerator walks files it deliberately drops (a
#            tests/ sibling, a nested package), and a dropped LAST
#            entry must not leave its skip behind as the run's status.
#   naming   --check reports the NAME of each file that would change,
#            never its reformatted source, in gofmt -l spirit. The
#            single-file form and the package form agree on this.
#
# The fixture package is built in a tmpdir rather than checked in: the
# point is a tests/ directory sorting last, which a corpus under
# examples/ cannot hold without the other fmt gates walking into it.
#
# Env:
#   VERBOSE=1   print each case as it passes.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
KAIC2="$ROOT/stage2/kaic2"

if [ ! -x "$KAIC2" ]; then
  echo "fmt_package: $KAIC2 not built; run 'make kaic2' first" >&2
  exit 2
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

pkg="$tmp/demo"
fails=0

# Run `kai fmt $2...` from $1 and record the status and stdout that the
# assertions below read. stderr stays out: a parse diagnostic belongs
# there, and part of the contract is that it does not reach stdout.
run_fmt() {
  rf_dir="$1"; shift
  got="$(cd "$rf_dir" && "$KAI" fmt "$@" 2>/dev/null)" && rc=0 || rc=$?
}

# Assert the last run_fmt exited $2 and printed exactly $3. $1 names
# the case.
expect() {
  if [ "$rc" != "$2" ]; then
    echo "  FAIL $1 — exit $rc, want $2" >&2
    fails=$((fails + 1))
  elif [ "$got" != "$3" ]; then
    echo "  FAIL $1 — stdout [$got], want [$3]" >&2
    fails=$((fails + 1))
  else
    [ -z "${VERBOSE:-}" ] || echo "  ok   $1"
  fi
}

# Assert $2 holds, naming the case $1.
check() {
  if [ "$2" != 1 ]; then
    echo "  FAIL $1" >&2
    fails=$((fails + 1))
  else
    [ -z "${VERBOSE:-}" ] || echo "  ok   $1"
  fi
}

write_clean_package() {
  mkdir -p "$pkg/demo" "$pkg/tests"
  printf 'name = "demo"\nversion = "0.1.0"\nentry = "main.kai"\n' > "$pkg/kai.toml"
  printf 'import demo.util\n\nfn main() : Unit / Stdout = Stdout.print(util.greet())\n' > "$pkg/main.kai"
  printf '#[doc("Greeting.")]\npub fn greet() : String = "hi"\n' > "$pkg/demo/util.kai"
  printf 'import demo.util\n\ntest "greet" {\n  assert util.greet() == "hi"\n}\n' > "$pkg/tests/util_test.kai"
}

# A package whose every file is already formatted. `tests/` sorts after
# the owned sources, so it is the entry the enumerator drops last.
write_clean_package
run_fmt "$pkg" --check .
expect "formatted package: --check is silent and exits 0" 0 ""

# Each file on its own agrees with the package verdict. A package that
# fails while every one of its files passes is the shape this gate
# exists to catch.
for rel in main.kai demo/util.kai tests/util_test.kai; do
  run_fmt "$pkg" --check "$rel"
  expect "formatted package: --check $rel exits 0" 0 ""
done

# One unformatted file, under tests/ — the entry the enumerator drops,
# so this also pins that the tests/ sibling is still checked.
printf 'import demo.util\n\ntest   "greet"  {\n  assert  util.greet( ) ==  "hi"\n}\n' > "$pkg/tests/util_test.kai"
run_fmt "$pkg" --check .
expect "unformatted tests/: --check names the file, exits 1" 1 "tests/util_test.kai"

# An owned source too: both names come out, one per line.
printf '#[doc("Greeting.")]\npub fn   greet( ) : String =   "hi"\n' > "$pkg/demo/util.kai"
run_fmt "$pkg" --check .
expect "two unformatted files: --check names both" 1 "demo/util.kai
tests/util_test.kai"

# The rewrite mode closes the loop: it formats the whole package, not
# just the entry point, and leaves --check clean.
run_fmt "$pkg" .
expect "rewrite: kai fmt . exits 0" 0 ""
run_fmt "$pkg" --check .
expect "rewrite: --check clean afterwards" 0 ""

# Single-file --check names the file rather than dumping its source,
# the same contract the package form honours, and leaves it untouched.
printf 'fn  main( ) : Unit   / Stdout =    Stdout.print("hi")\n' > "$tmp/ugly.kai"
run_fmt "$tmp" --check ugly.kai
expect "single file: --check names the file" 1 "ugly.kai"
grep -q 'fn  main( )' "$tmp/ugly.kai" && untouched=1 || untouched=0
check "single file: --check leaves the source alone" "$untouched"

# A parse error is not a formatting verdict: the diagnostic goes to
# stderr and no file name is reported as unformatted.
printf 'fn main( : Unit =\n' > "$tmp/broken.kai"
run_fmt "$tmp" --check broken.kai
expect "parse error: --check stays off stdout" 1 ""

if [ "$fails" != 0 ]; then
  echo "fmt_package: $fails check(s) failed" >&2
  exit 1
fi

echo "fmt_package: package mode ok (status, naming, rewrite)"
