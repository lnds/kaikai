#!/bin/bash
# Builtin forwarder gate.
#
# A builtin named as a value is a closure over its runtime thunk:
# `_<cname>_thunk` in stage2/runtime.h on the C backend, `kaix_<name>_thunk`
# in stage0/runtime_llvm.c on the native one. A missing thunk is otherwise a
# link error, and one passing another number of arguments a wrong call, at
# the first program that takes the builtin as a value.
#
# `kaic2 --builtin-table` prints the registry `<name> <cname> <arity>`
# after checking it against the resolver's core names and the typer's seed;
# this matches every row against both runtimes.
#
# `--self-test` feeds a row with no thunk and a row with the wrong arity
# and asserts the check rejects each.
set -u
cd "$(dirname "$0")/.."

# thunk_arity <file> <definition prefix> <macro> <macro arg>: the number of
# arguments the thunk forwards, written out or through `<macro><n>(<arg>)`;
# empty when the file defines neither.
thunk_arity() {
  local def
  def=$(grep -E "^$2\(.*\{" "$1" | head -1)
  if [ -n "$def" ]; then
    printf '%s' "$def" | grep -oE '\ba\[[0-9]+\]' | wc -l | tr -d ' '
  else
    grep -oE "^$3[0-9]+\($4\)" "$1" | head -1 | sed -E "s/^$3([0-9]+).*/\\1/"
  fi
}

# verdict <builtin> <thunk> <file> <thunk arity> <builtin arity>
verdict() {
  if [ -z "$4" ]; then
    echo "  $1: no $2 in $3"; return 1
  elif [ "$4" != "$5" ]; then
    echo "  $1: $2 passes $4 argument(s), the builtin takes $5"; return 1
  fi
}

# check <rows> <runtime.h> <runtime_llvm.c>
check() {
  local rows="$1" rt="$2" nrt="$3" status=0 name cname arity n
  while read -r name cname arity; do
    n=$(thunk_arity "$rt" "static KaiValue \*_${cname}_thunk" KAI_CORE_THUNK "${cname#kai_core_}")
    verdict "$name" "_${cname}_thunk" "$(basename "$rt")" "$n" "$arity" || status=1
    n=$(thunk_arity "$nrt" "KaiValue \*kaix_${name}_thunk" KAIX_CORE_THUNK "$name")
    verdict "$name" "kaix_${name}_thunk" "$(basename "$nrt")" "$n" "$arity" || status=1
  done < "$rows"
  return $status
}

self_test() {
  local tmp rc=0
  tmp=$(mktemp -d)
  echo "no_such_builtin kai_core_no_such_builtin 1" > "$tmp/missing"
  echo "print kai_core_print 2" > "$tmp/arity"
  for row in missing arity; do
    if check "$tmp/$row" stage2/runtime.h stage0/runtime_llvm.c > /dev/null; then
      echo "builtin-table self-test FAIL — a row with the $row defect passed"; rc=1
    fi
  done
  rm -rf "$tmp"
  [ $rc -eq 0 ] && echo "builtin-table self-test OK (a missing thunk and a wrong arity are rejected)"
  return $rc
}

if [ "${1:-}" = "--self-test" ]; then
  self_test || exit 1
fi

KAIC2=stage2/kaic2
[ -x "$KAIC2" ] || { echo "builtin-table: SKIP — no stage2/kaic2"; exit 0; }
rows=$(mktemp)
trap 'rm -f "$rows"' EXIT
"$KAIC2" --builtin-table > "$rows" || { echo "builtin-table FAIL — the registry disagrees with the resolver or the typer"; exit 1; }
count=$(wc -l < "$rows" | tr -d ' ')
[ "$count" -gt 0 ] || { echo "builtin-table FAIL — kaic2 printed no registry"; exit 1; }
if out=$(check "$rows" stage2/runtime.h stage0/runtime_llvm.c); then
  echo "builtin-table OK ($count builtins, each with its C and native thunk at its arity)"
else
  echo "builtin-table FAIL — runtime forwarders out of step with the builtin table:"
  echo "$out"
  exit 1
fi
