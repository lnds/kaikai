#!/bin/sh
# The derived AST codec round-trips real sources: each file's full parse
# is serialised, restored and re-serialised, and the two blobs must match
# byte for byte. The positive sugar / effect / protocol fixtures plus the
# compiler's own typer and parser cover nearly every expression, pattern,
# type and declaration shape the parser builds; a field the encoder and
# decoder disagree on breaks the byte identity.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"

n=0
for f in "$ROOT"/examples/sugars/*.out.expected \
         "$ROOT"/examples/effects/*.out.expected \
         "$ROOT"/examples/protocols/*.out.expected \
         "$ROOT"/stage2/compiler/infer.kai \
         "$ROOT"/stage2/compiler/parser.kai \
         "$ROOT"/stdlib/protocols.kai; do
  src="${f%.out.expected}"
  case "$src" in *.kai) ;; *) src="$src.kai" ;; esac
  [ -f "$src" ] || continue
  if ! out="$("$KAIC2" $EDITION_FLAG --cache-roundtrip-test "$src" 2>&1)"; then
    echo "corec_derive_roundtrip FAIL — $src"
    printf '%s\n' "$out"
    exit 1
  fi
  n=$((n + 1))
done

if [ "$n" -lt 100 ]; then
  echo "corec_derive_roundtrip FAIL — only $n sources found"
  exit 1
fi

echo "corec_derive_roundtrip OK — $n sources round-trip byte-identical"
