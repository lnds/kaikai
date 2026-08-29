#!/usr/bin/env bash
# Two modules each declare `scale`. Keyed on the module that declares
# them, the two get distinct C symbols and each caller reaches its own;
# flattened, the second definition collides at link time and the two
# calls both land on the first.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAIC1="$ROOT/stage1/kaic1"
DIR="$ROOT/examples/stage1-homonyms"

if [ ! -x "$KAIC1" ]; then
    echo "test-stage1-homonyms: missing $KAIC1 (run make kaic1)" >&2
    exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

if ! "$KAIC1" "$DIR/main.kai" > "$tmp/out.c"; then
    echo "FAIL stage1-homonyms: kaic1 rejected the package"
    exit 1
fi

if ! cc -std=c99 -w -I "$ROOT/stage0" "$tmp/out.c" -o "$tmp/prog" -lm; then
    echo "FAIL stage1-homonyms: two modules' scale minted one C symbol"
    exit 1
fi

if ! diff -u "$DIR/main.out.expected" <("$tmp/prog"); then
    echo "FAIL stage1-homonyms: a call reached the other module's declaration"
    exit 1
fi

echo "stage1-homonyms OK"
