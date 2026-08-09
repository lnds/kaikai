#!/usr/bin/env bash
# A root file whose basename is not a C identifier (`root.name-with.dots.kai`)
# must still compile and run: the module name it contributes is minted into
# every `pub` symbol, so it is sanitised where it is derived.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FIXTURE="$ROOT/examples/modules/module-name-ident/root.name-with.dots.kai"
EXPECTED="$ROOT/examples/modules/module-name-ident/root.name-with.dots.out.expected"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

backends="c"
if "$ROOT/bin/kai" build --backend=native "$FIXTURE" -o "$tmp/probe" >"$tmp/probe.err" 2>&1; then
  backends="c native"
else
  grep -q "not built into this compiler\|native backend unavailable" "$tmp/probe.err" \
    || { echo "test-module-name-ident FAIL (native build): $(cat "$tmp/probe.err")"; exit 1; }
  echo "test-module-name-ident: native backend absent from this kaic2 — C only"
fi

for backend in $backends; do
  if ! "$ROOT/bin/kai" run "--backend=$backend" "$FIXTURE" >"$tmp/out.$backend" 2>"$tmp/err.$backend"; then
    echo "test-module-name-ident FAIL ($backend): compile/run rejected the dotted basename"
    cat "$tmp/err.$backend"
    exit 1
  fi
  diff -u "$EXPECTED" "$tmp/out.$backend" \
    || { echo "test-module-name-ident FAIL ($backend): output mismatch"; exit 1; }
done

echo "test-module-name-ident OK — non-identifier basename mints valid C symbols ($backends)"
