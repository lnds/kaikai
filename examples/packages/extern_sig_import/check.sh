#!/bin/sh
# examples/packages/extern_sig_import — an import used only to name a type
# in an `extern "C"` signature is not reported unused (lib/b.kai).

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
KAI="$DIR/../../../bin/kai"

out="$(cd "$DIR" && "$KAI" typecheck . 2>&1)" || { echo "$out" >&2; exit 1; }
if echo "$out" | grep -A1 "unused import" | grep -q "lib/b.kai"; then
  echo "extern_sig_import: unexpected unused-import warning" >&2
  echo "$out" >&2
  exit 1
fi
