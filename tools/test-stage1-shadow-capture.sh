#!/usr/bin/env bash
# A lambda reading a local named like a global (a core builtin or a
# top-level fn) must capture the local. Skipping it binds the global
# instead: a list-match then panics as non-exhaustive, arithmetic
# reports a type mismatch.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAIC1="$ROOT/stage1/kaic1"
DIR="$ROOT/examples/stage1-shadow-capture"

if [ ! -x "$KAIC1" ]; then
    echo "test-stage1-shadow-capture: missing $KAIC1 (run make kaic1)" >&2
    exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

if ! "$KAIC1" "$DIR/main.kai" > "$tmp/out.c"; then
    echo "FAIL stage1-shadow-capture: kaic1 rejected the program"
    exit 1
fi

if ! cc -std=c99 -w -I "$ROOT/stage0" "$tmp/out.c" -o "$tmp/prog" -lm; then
    echo "FAIL stage1-shadow-capture: generated C does not compile"
    exit 1
fi

if ! diff -u "$DIR/main.out.expected" <("$tmp/prog" 2>&1); then
    echo "FAIL stage1-shadow-capture: a lambda read the global, not the captured local"
    exit 1
fi

echo "stage1-shadow-capture OK"
