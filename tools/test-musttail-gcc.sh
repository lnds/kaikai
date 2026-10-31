#!/bin/bash
# Guaranteed tail calls under gcc >= 15: the C backend's `KC_MUSTTAIL` is
# `[[gnu::musttail]]` there, and gcc applies it to every `return` that
# follows in the same block, so a tail call must sit in its own braces.
# Builds the fixtures and the compiler's own modular C with gcc.

set -eu

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
GCC="${MUSTTAIL_CC:-gcc}"

banner="$("$GCC" --version 2>/dev/null | head -1 || true)"
echo "musttail-gcc: cc=$GCC ($banner)"
major="$("$GCC" -dumpversion 2>/dev/null | cut -d. -f1 || true)"
if ! "$GCC" --version 2>/dev/null | grep -q 'Free Software Foundation' || [ "${major:-0}" -lt 15 ]; then
  echo "::error::musttail-gcc: '$GCC' is not GNU gcc >= 15; it would not exercise [[gnu::musttail]]"
  exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

fail=0
for f in examples/musttail_gcc/*.kai; do
  golden="${f%.kai}.out.expected"
  name="$(basename "${f%.kai}")"
  if ! CC="$GCC" KAI_BACKEND=c "$ROOT/bin/kai" run "$f" > "$tmp/$name.out" 2> "$tmp/$name.err"; then
    echo "::error::musttail-gcc: $f failed to build or run under $GCC"
    cat "$tmp/$name.err"
    fail=1
    continue
  fi
  if diff -u "$golden" "$tmp/$name.out"; then
    echo "musttail-gcc: PASS $f"
  else
    echo "::error::musttail-gcc: $f prints the wrong output under $GCC"
    fail=1
  fi
done

if CC="$GCC" CFLAGS="-std=c99 -O2 -DKAI_LLVM_BRIDGE" KAI_BACKEND=c KAI_CACHE=0 KAI_MODULAR=1 KAI_MODULAR_CACHE_DIR="$tmp/cache" \
     "$ROOT/bin/kai" build stage2/main.kai -o "$tmp/kaic2-gcc" 2> "$tmp/self.err" \
   && "$tmp/kaic2-gcc" --version 2>&1 | grep -q "self-hosted"; then
  echo "musttail-gcc: PASS the compiler's modular C builds under $GCC"
else
  echo "::error::musttail-gcc: the compiler's modular C does not build under $GCC"
  grep -E "error|cannot tail-call" "$tmp/self.err" | head -40
  fail=1
fi

exit "$fail"
