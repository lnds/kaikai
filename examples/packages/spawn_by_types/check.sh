#!/bin/sh
# examples/packages/spawn_by_types — a function value that reaches a spawned
# fiber is judged by the row its caller instantiates, whatever value shape
# carries it (record, list, closure, forwarding helper). `ok/*` answer the
# effect inside the fiber and must build and print `hola`; `bad/*` hand the
# value to the fiber as is and must be rejected. Built as a file and as a
# package, on either backend.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
KAI="$ROOT/bin/kai"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

fail() { echo "spawn_by_types: FAIL — $1" >&2; exit 1; }

backends="c"
printf 'fn main() : Int = 0\n' > "$TMP/probe.kai"
if "$KAI" build --backend=native "$TMP/probe.kai" -o "$TMP/probe" >/dev/null 2>&1; then
  backends="c native"
fi

want="effect not handled in spawned fiber: Greet"

for backend in $backends; do
  for kind in ok bad; do
    for src in "$DIR/$kind"/*.kai; do
      name="$(basename "$src" .kai)"
      for mode in file package; do
        label="$kind/$name ($mode, $backend)"
        w="$TMP/$kind-$name-$mode-$backend"
        mkdir -p "$w"
        cp "$src" "$w/main.kai"
        printf 'name = "app"\nversion = "0.1.0"\nedition = "hanga-roa"\n' > "$w/kai.toml"
        if [ "$kind" = ok ]; then
          if [ "$mode" = file ]; then
            out="$(cd "$w" && KAI_BACKEND=$backend "$KAI" build main.kai -o "$TMP/bin" 2>&1)" \
              || fail "$label does not build: $out"
            got="$("$TMP/bin")"
          else
            got="$(cd "$w" && KAI_BACKEND=$backend "$KAI" run . 2>/dev/null)" \
              || fail "$label does not run"
          fi
          [ "$got" = "hola" ] || fail "$label printed '$got'"
        else
          if [ "$mode" = file ]; then
            out="$(cd "$w" && KAI_BACKEND=$backend "$KAI" build main.kai -o "$TMP/bin" 2>&1)" && fail "$label builds"
          else
            out="$(cd "$w" && KAI_BACKEND=$backend "$KAI" build . 2>&1)" && fail "$label builds"
          fi
          echo "$out" | grep -qF "$want" || fail "$label fails without '$want': $out"
        fi
      done
    done
  done
done

echo "spawn_by_types: OK"
