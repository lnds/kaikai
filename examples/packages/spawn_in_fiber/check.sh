#!/bin/sh
# examples/packages/spawn_in_fiber — a library function that runs its argument
# in a spawned fiber is held to the same spawn rule whatever the calling
# program looks like: one file or several, built as a file or as a package,
# on either backend. `handled` answers the effect inside the fiber and must
# build and print `hola`; `unhandled` hands the argument to the fiber as is and
# must be rejected.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
KAI="$ROOT/bin/kai"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

fail() { echo "spawn_in_fiber: FAIL — $1" >&2; exit 1; }

# Copy the fixture so no build leaves caches in the checkout.
cp -R "$DIR/handled" "$TMP/handled"
cp -R "$DIR/unhandled" "$TMP/unhandled"

backends="c"
printf 'fn main() : Int = 0\n' > "$TMP/probe.kai"
if "$KAI" build --backend=native "$TMP/probe.kai" -o "$TMP/probe" >/dev/null 2>&1; then
  backends="c native"
fi

for backend in $backends; do
  for app in app app_modules; do
    for mode in file package; do
      d="$TMP/handled/$app"
      label="handled/$app ($mode, $backend)"
      if [ "$mode" = file ]; then
        out="$(cd "$d" && KAI_BACKEND=$backend "$KAI" build main.kai -o "$TMP/bin" 2>&1)" \
          || fail "$label does not build: $out"
        got="$("$TMP/bin")"
      else
        got="$(cd "$d" && KAI_BACKEND=$backend "$KAI" run . 2>/dev/null)" \
          || fail "$label does not run"
      fi
      [ "$got" = "hola" ] || fail "$label printed '$got'"

      d="$TMP/unhandled/$app"
      label="unhandled/$app ($mode, $backend)"
      want="$(head -n 1 "$d/main.err.expected")"
      if [ "$mode" = file ]; then
        out="$(cd "$d" && KAI_BACKEND=$backend "$KAI" build main.kai -o "$TMP/bin" 2>&1)" && fail "$label builds"
      else
        out="$(cd "$d" && KAI_BACKEND=$backend "$KAI" build . 2>&1)" && fail "$label builds"
      fi
      echo "$out" | grep -qF "$want" || fail "$label fails without '$want': $out"
    done
  done
done

echo "spawn_in_fiber: OK"
