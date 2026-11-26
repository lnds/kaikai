#!/bin/sh
# examples/packages/native_backtrace — backtrace(3) returns from every kind
# of stack the runtime runs kaikai code on, on each backend this kaic2 has,
# at one scheduler thread and at four.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
KAI="$ROOT/bin/kai"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

fail=0
note() { echo "native_backtrace: FAIL — $1" >&2; fail=1; }

backends=c
if "$ROOT/tools/kaic2-native-capable.sh" "$ROOT/stage2/kaic2" >/dev/null 2>&1; then
  backends="c native"
fi

for backend in $backends; do
  if ! "$KAI" build --backend="$backend" "$DIR" -o "$TMP/$backend" >/dev/null 2>"$TMP/err"; then
    note "$backend build failed"; cat "$TMP/err" >&2
    continue
  fi
  for threads in 1 4; do
    status=0
    KAI_THREADS=$threads "$TMP/$backend" >"$TMP/out" 2>&1 || status=$?
    [ "$status" -eq 0 ] || note "$backend, $threads threads: exit $status"
    diff -u "$DIR/main.out.expected" "$TMP/out" >&2 \
      || note "$backend, $threads threads: output differs"
  done
done

exit "$fail"
