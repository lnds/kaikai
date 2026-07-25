#!/usr/bin/env bash
# Every `examples/negative/stage1_rejections/*.kai` must be rejected by
# kaic1, and its stderr must contain the matching `.kaic1.err.expected`
# fragment. Guards the bootstrap compiler's diagnostics: a rejection
# that silently turns into an accepted (mis)compilation is the failure
# mode these fixtures exist to catch.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAIC1="$ROOT/stage1/kaic1"
DIR="$ROOT/examples/negative/stage1_rejections"

if [ ! -x "$KAIC1" ]; then
    echo "test-stage1-rejections: missing $KAIC1 (run make kaic1)" >&2
    exit 1
fi

fail=0
count=0

for src in "$DIR"/*.kai; do
    [ -e "$src" ] || continue
    base="${src%.kai}"
    expected="$base.kaic1.err.expected"
    name="$(basename "$src")"
    count=$((count + 1))

    if [ ! -f "$expected" ]; then
        echo "FAIL $name: no $(basename "$expected")"
        fail=1
        continue
    fi

    if err="$("$KAIC1" "$src" 2>&1 >/dev/null)"; then
        echo "FAIL $name: kaic1 accepted a fixture that must be rejected"
        fail=1
        continue
    fi

    want="$(cat "$expected")"
    if [ -n "$want" ] && ! printf '%s' "$err" | grep -qF "$want"; then
        echo "FAIL $name: diagnostic mismatch"
        echo "  want (substring): $want"
        echo "  got:              $err"
        fail=1
        continue
    fi

    echo "reject OK $name"
done

if [ "$count" -eq 0 ]; then
    echo "test-stage1-rejections: no fixtures found in $DIR" >&2
    exit 1
fi

exit $fail
