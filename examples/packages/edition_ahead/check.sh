#!/bin/sh
# A manifest may name an edition the compiler does not implement yet.
# That is legal — the name is in `known_editions` — and the build is
# sound, because every gate keyed on the newer edition is still absent.
# It is also a silent trap: the manifest reads as a promise the
# toolchain cannot keep, and `edition_rank` already ranks the unbuilt
# edition ABOVE the implemented one, so the first `edition_at_least`
# gate that lands changes what these sources mean with no edit.
#
# Gates:
# 1. An edition ahead of the compiler warns, and the build still succeeds.
# 2. The implemented edition and older ones stay silent (no false positive
#    on every package in existence).
# 3. The warning survives the wrapper on both backends — compiler stderr
#    is captured to a temp file and only shown on FAILURE, so a warning
#    about a successful build has to be replayed or it is never seen.

set -eu

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
KAI="$ROOT/bin/kai"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

printf 'fn main() : Unit / Console = println("ok")\n' > "$PROJ/main.kai"

manifest() {
  printf 'name = "edtest"\nversion = "0.1.0"\nedition = "%s"\n\n[dependencies]\n' \
    "$1" > "$PROJ/kai.toml"
}

warns() {
  ( cd "$PROJ" && "$KAI" build $2 -o "$PROJ/out" 2>&1 ) \
    | grep -c '^kaic2: warning:.*does not implement yet' || true
}

# 1 + 3: ahead of the compiler warns, on the default (native) backend and
# on the C backend, whose wrapper paths capture stderr differently.
manifest orongo
for backend in "" "--backend=c"; do
  n="$(warns "$PROJ" "$backend")"
  if [ "$n" -lt 1 ]; then
    echo "edition_ahead_of_compiler: FAIL — no warning for a future edition (backend='${backend:-default}')"
    exit 1
  fi
done

# The build must still SUCCEED — this is a warning, not a rejection.
if ! ( cd "$PROJ" && "$KAI" build -o "$PROJ/out" >/dev/null 2>&1 ); then
  echo "edition_ahead_of_compiler: FAIL — future edition should warn, not fail the build"
  exit 1
fi
if [ "$("$PROJ/out")" != "ok" ]; then
  echo "edition_ahead_of_compiler: FAIL — binary did not run correctly"
  exit 1
fi

# 2: the implemented edition and older ones must stay silent.
for ed in hanga-roa tongariki; do
  manifest "$ed"
  n="$(warns "$PROJ" "")"
  if [ "$n" -ne 0 ]; then
    echo "edition_ahead_of_compiler: FAIL — spurious warning for edition '$ed'"
    exit 1
  fi
done

echo "edition_ahead_of_compiler: OK — future edition warns on both backends, current and older stay silent"
exit 0
