#!/usr/bin/env bash
# kaic1 must resolve `import` and compile a multi-module package: the
# fixture's two modules place a lambda at the same (line, column), the
# key lambda identity is derived from. A shared line range makes the
# second lambda reuse the first one's captures and both calls return the
# same list -- silently, with no diagnostic.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAIC1="$ROOT/stage1/kaic1"
DIR="$ROOT/examples/stage1-imports"

if [ ! -x "$KAIC1" ]; then
    echo "test-stage1-imports: missing $KAIC1 (run make kaic1)" >&2
    exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

if ! "$KAIC1" "$DIR/main.kai" > "$tmp/out.c"; then
    echo "FAIL stage1-imports: kaic1 rejected the package"
    exit 1
fi

if ! cc -std=c99 -w -I "$ROOT/stage0" "$tmp/out.c" -o "$tmp/prog" -lm; then
    echo "FAIL stage1-imports: emitted C did not compile"
    exit 1
fi

if ! diff -u "$DIR/main.out.expected" <("$tmp/prog"); then
    echo "FAIL stage1-imports: output mismatch"
    exit 1
fi

echo "stage1-imports OK"
