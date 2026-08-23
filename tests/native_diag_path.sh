#!/bin/sh
# Diagnostics on the native build paths, end to end.
#
# `bin/kai` compiles a COPY of the entry file on both native paths (the
# whole-program copy under $tmp, the native-modular copy under the
# content-addressed cache dir), because kaic2 derives the object path and
# the cache keys from the path it is handed. Left uncorrected, every
# diagnostic attributed to the entry names that copy: a path the user
# cannot click, under a directory whose name changes between builds. The
# C path never copies and has always named the real file.
#
# Two things are pinned:
#   1. the rendered path is the user's file, on both native paths;
#   2. a compile error on the native-modular path is reported ONCE — the
#      fallback to whole-program used to recompile the same source and
#      print every diagnostic a second time.
#
# That a compile error does not kill `kai watch` is gated separately, by
# tests/watch_survives_error.sh.
#
# This needs a kaic2 with libLLVM. Where the native backend is absent
# `bin/kai` degrades to C, which was never affected, so the run reports
# SKIP rather than a hollow pass. The substitution itself is gated
# backend-independently by tests/diag_path_rewrite.sh.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
FX="$ROOT/examples/modules/issue-1881-native-diag-path"

work="$(mktemp -d)"
proj="$work/proj"
trap 'rm -rf "$work"' EXIT INT TERM

fail=0

reject() {
  echo "  FAIL $1 ($2)"
  cat "$3"
  fail=$((fail + 1))
}

# `grep -c` exits 1 on a count of zero, which is the passing case here,
# so the count is taken from its output and the status discarded. Any
# `-->` line pointing outside $proj is a leftover cache or scratch path.
stray_arrows() {
  _sa=$(grep -E '^  --> ' "$1" 2>/dev/null | grep -Fcv -- "--> $proj/" 2>/dev/null) || true
  printf '%s' "${_sa:-0}"
}

# Build $2 (a file under $proj) expecting the compile error the fixture
# carries, then assert the diagnostics: every `-->` names the fixture,
# and the error appears exactly $3 times.
check_entry_path() {
  _desc="$1"; _src="$2"; _want="$3"
  _err="$work/$_desc.err"
  _rc=0
  "$KAI" build "$proj/$_src" -o "$work/out-$_desc" >/dev/null 2>"$_err" || _rc=$?

  if [ "$_rc" -eq 0 ]; then
    reject "$_desc" "expected a compile error, got success" "$_err"
    return
  fi
  if ! grep -Fq "$proj/$_src:" "$_err"; then
    reject "$_desc" "no diagnostic names the user's source" "$_err"
    return
  fi
  if [ "$(stray_arrows "$_err")" != "0" ]; then
    reject "$_desc" "a diagnostic names something other than the fixture" "$_err"
    return
  fi
  _n=$(grep -c 'cannot find `nosuchthing`' "$_err") || true
  if [ "${_n:-0}" != "$_want" ]; then
    reject "$_desc" "error reported ${_n:-0} times, expected $_want" "$_err"
    return
  fi
  echo "  ok   $_desc names the user's source, reported $_want time(s)"
}

# The fixture always fails to compile, so the probe's exit status says
# nothing; what distinguishes a C-only kaic2 is the sentinel it prints
# instead of a diagnostic.
"$KAI" build --backend=native "$FX/solo.kai" -o "$work/probe" 2>"$work/probe.err" || true
if grep -q "not built into this compiler\|native backend is not built" "$work/probe.err" 2>/dev/null; then
  echo "native-diag-path SKIP (native backend not built into this kaic2)"
  exit 0
fi

# Compile from a copy so the path under test is the one this run owns,
# not the checkout's.
mkdir -p "$proj"
cp "$FX/main.kai" "$FX/helper.kai" "$FX/solo.kai" "$proj/"

echo "native-diag-path:"
# main.kai imports a local module, which routes the build onto the
# partitioned path; solo.kai imports none and takes whole-program.
check_entry_path native-modular main.kai 1
check_entry_path whole-program solo.kai 1

if [ "$fail" -gt 0 ]; then
  echo "native-diag-path FAIL ($fail check(s))"
  exit 1
fi
echo "native-diag-path OK"
