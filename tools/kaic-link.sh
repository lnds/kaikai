#!/bin/sh
# tools/kaic-link.sh — link a compiler binary from the C a boot emitted.
#
#   CC=<cc> LDLIBS=<libs> kaic-link.sh <in.c> <bin> <compile flags...>
#
# <in.c> is either one translation unit, compiled and linked by a single cc,
# or a `--emit=c-modular` stream (first line `//KAIDELIM:<nonce>`): one TU per
# module, compiled in parallel under <in.c's dir>/tus/<bin name>/ and linked
# with the runtime owner. The form is read from the file, never from the
# environment.
#
# Objects of a stream are reused from $KAIC_OBJ_CACHE/<bin name>/, keyed by
# the compiler, its flags, and the content of the TU and of every file it
# includes (cc -M). An object is linked only under the key of exactly what
# cc would read now; a TU whose key cannot be computed is compiled uncached.
# Objects the link did not use are pruned.

set -eu
LC_ALL=C
export LC_ALL

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$@"; else shasum -a 256 "$@"; fi
}

share() { ln "$1" "$2" 2>/dev/null || cp "$1" "$2" 2>/dev/null; }

# compile_tu <src.c> <extra flags...>: one object, run from the TU directory.
compile_tu() {
  src="$1"; shift
  obj="${src%.c}.o"
  extra="$*"
  set --
  while IFS= read -r flag; do set -- "$@" "$flag"; done < flags
  # shellcheck disable=SC2086
  set -- "$@" $extra
  key=""
  if deps="$($KL_CC "$@" -M "$src" 2>/dev/null)"; then
    files="$(printf '%s\n' "$deps" | tr -d '\\\n' | sed 's/^[^:]*://')"
    # shellcheck disable=SC2086
    if sums="$(sha256 $files 2>/dev/null)"; then
      key="$(printf '%s\n' "$KL_BASE" "$extra" "$sums" | sha256 | cut -c1-64)"
    fi
  fi
  echo "$key" > "${src%.c}.key"
  if [ -n "$key" ] && share "$KL_CACHE/$key.o" "$obj"; then return; fi
  $KL_CC "$@" -c "$src" -o "$obj"
  [ -z "$key" ] || { share "$obj" "$KL_CACHE/$key.o.$$" && mv "$KL_CACHE/$key.o.$$" "$KL_CACHE/$key.o"; }
  echo "$src" >> compiled
}

if [ "${1:-}" = --tu ]; then
  cd "$KL_DIR"
  sep=-DKAI_SEPARATE_COMPILATION=1
  case "${2#./}" in
    runtime_owner_c.c) compile_tu "${2#./}" "$sep" -DKAI_RUNTIME_OWNER=1 -DKAI_PROGRAM_PROVIDES_MAIN=1 ;;
    *)                 compile_tu "${2#./}" "$sep" ;;
  esac
  exit
fi

[ $# -ge 2 ] || { echo "usage: kaic-link.sh <in.c> <bin> <compile flags...>" >&2; exit 2; }
in="$1"; bin="$2"; shift 2
CC="${CC:-cc}"
LDLIBS="${LDLIBS:-}"

nonce="$(sed -n '1s|^//KAIDELIM:||p' "$in")"
if [ -z "$nonce" ]; then
  # shellcheck disable=SC2086
  exec $CC "$@" "$in" -o "$bin" $LDLIBS
fi

here="$(pwd)"
build="$(cd "$(dirname "$in")" && pwd)"
name="$(basename "$bin")"
KL_DIR="$build/tus/$name"
KL_CACHE="${KAIC_OBJ_CACHE:-$build/obj-cache}/$name"
rm -rf "$KL_DIR"
mkdir -p "$KL_DIR" "$KL_CACHE"
awk -v dir="$KL_DIR" -v pfx="//KAIFILE:$nonce::" \
  'index($0,pfx)==1{if(out)close(out);out=dir"/"substr($0,length(pfx)+1);next} out{print > out}' "$in"
cp "$(dirname "$0")/../stage2/runtime_owner_c.c" "$KL_DIR/"

# Relative -I paths resolve from the caller's directory, not the TU's.
prev=""
for flag in "$@"; do
  case "$prev:$flag" in -I:/*) ;; -I:*) flag="$here/$flag" ;; esac
  printf '%s\n' "$flag"
  prev="$flag"
done > "$KL_DIR/flags"

began="$(date +%s)"
KL_CC="$CC"
KL_BASE="$({ $CC --version; uname -sm; cat "$KL_DIR/flags"; } | sha256 | cut -c1-64)"
export KL_DIR KL_CACHE KL_CC KL_BASE
jobs="${KAIC_LINK_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"
self="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
# Largest first, so the longest compile does not start last. The names are
# module stems, safe to pass through xargs.
: > "$KL_DIR/compiled"
# shellcheck disable=SC2011
(cd "$KL_DIR" && ls -S ./*.c | xargs -P "$jobs" -n 1 "$self" --tu)

for cached in "$KL_CACHE"/*.o; do
  grep -qsx "$(basename "$cached" .o)" "$KL_DIR"/*.key || rm -f "$cached"
done
total="$(find "$KL_DIR" -name '*.c' | wc -l | tr -d ' ')"
built="$(wc -l < "$KL_DIR/compiled" | tr -d ' ')"
# shellcheck disable=SC2086
$CC "$@" "$KL_DIR"/*.o -o "$bin" $LDLIBS
echo "kaic-link: $name: $total TUs, jobs=$jobs, $(($(date +%s) - began))s, compiled=$built" >&2
