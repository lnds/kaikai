#!/bin/sh
# `bin/kai`'s entry-path rewrite, exercised on its own.
#
# The native paths compile a COPY of the entry file, so kaic2 renders
# every diagnostic it attributes to the entry against that copy's path.
# `diag_rewrite_entry_path` corrects the captured stderr before anything
# reads it. The substitution runs through sed, so a project directory
# carrying `.`, `*`, `[`, `&` or `\` — all legal in a path — must survive
# it unmangled.
#
# The function is lifted out of bin/kai rather than restated here, so
# this gate cannot drift from the code it protects; bin/kai stays a
# single self-contained script, which is how a release tarball ships it.
#
# The end-to-end half lives in tests/native_diag_path.sh.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

sed -n '/^diag_rewrite_entry_path() {$/,/^}$/p' "$KAI" > "$work/rewrite.sh"
[ -s "$work/rewrite.sh" ] || {
  echo "diag-path-rewrite FAIL (diag_rewrite_entry_path not found in bin/kai)"
  exit 1
}
. "$work/rewrite.sh"

fail=0

# A diagnostic naming $2 must come back naming $3, with no trace of the
# path kaic2 was handed.
check() {
  _desc="$1"; _from="$2"; _to="$3"
  printf 'error: boom\n  --> %s:3:7\n' "$_from" > "$work/err"
  diag_rewrite_entry_path "$work/err" "$_from" "$_to"
  if grep -Fq "  --> $_to:3:7" "$work/err" && ! grep -Fq "$_from:" "$work/err"; then
    echo "  ok   $_desc"
  else
    echo "  FAIL $_desc"
    cat "$work/err"
    fail=$((fail + 1))
  fi
}

echo "diag-path-rewrite:"
check "plain path"             "/cache/ab12/main.kai"     "/home/u/proj/main.kai"
check "dots and stars"         "/cache/a.b/m*n/main.kai"  "/home/u/p.d/main.kai"
check "brackets and ampersand" "/cache/[x]/main.kai"      "/home/u/a&b/main.kai"
check "backslash in target"    "/cache/z/main.kai"        '/home/u/a\b/main.kai'

# Only the entry file is rewritten: a diagnostic attributed to an
# imported module already carries that module's real path and must come
# through untouched.
printf 'error: boom\n  --> %s:3:7\n  --> %s:9:1\n' \
  "/cache/ab12/main.kai" "/home/u/proj/helper.kai" > "$work/err"
diag_rewrite_entry_path "$work/err" "/cache/ab12/main.kai" "/home/u/proj/main.kai"
if grep -Fq "  --> /home/u/proj/main.kai:3:7" "$work/err" \
   && grep -Fq "  --> /home/u/proj/helper.kai:9:1" "$work/err"; then
  echo "  ok   other files left alone"
else
  echo "  FAIL other files left alone"
  cat "$work/err"
  fail=$((fail + 1))
fi

# An empty capture is the common case on a clean build; it must be left
# alone rather than truncated or errored on.
: > "$work/empty"
diag_rewrite_entry_path "$work/empty" "/a/x.kai" "/b/x.kai"
if [ -f "$work/empty" ] && [ ! -s "$work/empty" ]; then
  echo "  ok   empty capture untouched"
else
  echo "  FAIL empty capture untouched"
  fail=$((fail + 1))
fi

if [ "$fail" -gt 0 ]; then
  echo "diag-path-rewrite FAIL ($fail check(s))"
  exit 1
fi
echo "diag-path-rewrite OK"
