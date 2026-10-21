#!/bin/sh
# tools/test-build-out-dir.sh <bin/kai> — `kai build -o` creates the missing
# parent directory before compiling, and fails early when it cannot.
set -eu

KAI="${1:?usage: test-build-out-dir.sh <bin/kai>}"
work="$(mktemp -d "${TMPDIR:-/tmp}/build-out-dir.XXXXXX")"
trap 'rm -rf "$work"' EXIT INT TERM

fail() { echo "test-build-out-dir: FAIL: $1" >&2; exit 1; }

mkdir -p "$work/pkg"
printf 'name = "repro"\nversion = "0.1.0"\nentry = "main.kai"\n' > "$work/pkg/kai.toml"
printf 'fn main() : Unit / Stdout = Stdout.print("hi")\n' > "$work/pkg/main.kai"
cp "$work/pkg/main.kai" "$work/one.kai"
: > "$work/blocker"

backends="c"
if "$KAI" build --backend=native "$work/one.kai" -o "$work/probe" > /dev/null 2>&1; then
  backends="c native"
fi

for b in $backends; do
  for spec in "$work/pkg" "$work/one.kai"; do
    out="$work/$b/nested/dir/bin"
    "$KAI" build --backend="$b" "$spec" -o "$out" || fail "$b $spec: build into a missing directory"
    [ "$("$out")" = "hi" ] || fail "$b $spec: binary does not run"

    err="$work/err"
    if "$KAI" build --backend="$b" "$spec" -o "$work/blocker/bin" 2> "$err"; then
      fail "$b $spec: built under a regular file"
    fi
    grep -q "cannot create output directory '$work/blocker'" "$err" || fail "$b $spec: diagnostic does not name the path: $(cat "$err")"
    if grep -q "link failed" "$err"; then fail "$b $spec: failed in the linker, not before compiling"; fi
  done
done
echo "test-build-out-dir: ok — $backends"
