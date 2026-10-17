#!/bin/sh
# The shared caches stay bounded: a hit refreshes its entry, the daily
# trim removes entries unused for the window and spares fresh ones, and
# `kai clean --cache` empties the lot.
#
# Every run points XDG_CACHE_HOME at a scratch dir; the real cache is
# never read or written.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

xdg="$work/xdg"
cache="$xdg/kai"
mkdir -p "$work/pkg"
printf 'fn main() : Unit / Stdout = Stdout.print("hello")\n' > "$work/pkg/hello.kai"

fail=0
BACKEND=c
ok() { echo "  ok   $1"; }
bad() { echo "  FAIL $1"; fail=$((fail + 1)); }

# kai <env...> -- <args>: kai against the scratch cache.
kai() {
  (cd "$work/pkg" && env XDG_CACHE_HOME="$xdg" KAI_BACKEND="$BACKEND" KAI_MODULAR=1 "$@")
}

build() { kai "$@" "$KAI" build hello.kai -o "$work/hello" > "$work/build.out" 2>&1; }

# age <days> <path...>: set mtime and atime that many days back.
age() {
  d="$1"; shift
  perl -e '$t = time - $ARGV[0] * 86400; utime($t, $t, @ARGV[1..$#ARGV])' "$d" "$@"
}

mtime() { perl -e 'print((stat $ARGV[0])[9])' "$1"; }

# gone <path>: the path vanishes within a few seconds (the trim is detached).
gone() {
  i=0
  while [ -e "$1" ] && [ "$i" -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
  [ ! -e "$1" ]
}

# stays <path>: the path is still there after the trim had time to run.
stays() { sleep 1.5; [ -e "$1" ]; }

plant() {
  mkdir -p "$cache/cmodular" "$cache/core/oldtid" "$cache/pkg/host/sha1"
  : > "$cache/cmodular/old.o"
  : > "$cache/cmodular/fresh.o"
  : > "$cache/core/oldtid/blob"
  : > "$cache/pkg/host/sha1/kai.toml"
  age 9 "$cache/cmodular/old.o" "$cache/core/oldtid/blob" "$cache/core/oldtid" \
    "$cache/pkg/host/sha1/kai.toml" "$cache/pkg/host/sha1"
}

build KAI_CACHE_TRIM=0
if [ "$("$work/hello")" != "hello" ] || [ -z "$(ls "$cache"/cmodular/*.o 2>/dev/null)" ]; then
  bad "cold build left no c-modular objects"; cat "$work/build.out"; exit 1
fi
rm -f "$cache/trim-stamp"

# A hit refreshes the entry it reads.
for o in "$cache"/cmodular/*.o; do age 9 "$o"; done
build KAI_CACHE_TRIM=0 KAI_BIN_MEMO=0
fresh=0
now=$(date +%s)
for o in "$cache"/cmodular/*.o; do
  [ $((now - $(mtime "$o"))) -lt 600 ] && fresh=$((fresh + 1))
done
if [ "$fresh" -ge 1 ]; then ok "hit refreshes mtime"; else bad "hit left every object 9 days old"; fi

# An entry past the window goes, a fresh one stays, the stamp is written.
plant
rm -f "$cache/trim-stamp"
build
if gone "$cache/cmodular/old.o" && gone "$cache/core/oldtid" && gone "$cache/pkg/host/sha1"; then
  ok "aged entries trimmed (file, toolchain dir, checkout)"
else
  bad "aged entries survived the trim"
fi
if [ -e "$cache/cmodular/fresh.o" ]; then ok "fresh entry survives"; else bad "fresh entry trimmed"; fi
if [ -f "$cache/trim-stamp" ]; then ok "stamp written"; else bad "no stamp"; fi

# A stamp under a day old holds the trim back.
plant
build
if stays "$cache/cmodular/old.o"; then ok "stamp limits the trim to once a day"; else bad "trimmed twice in a day"; fi

# KAI_CACHE_TRIM=0 turns it off.
rm -f "$cache/trim-stamp"
build KAI_CACHE_TRIM=0
if stays "$cache/cmodular/old.o" && [ ! -e "$cache/trim-stamp" ]; then
  ok "KAI_CACHE_TRIM=0 disables the trim"
else
  bad "KAI_CACHE_TRIM=0 still trimmed"
fi

# KAI_CACHE_TRIM_DAYS moves the window.
age 3 "$cache/cmodular/fresh.o"
build KAI_CACHE_TRIM_DAYS=20
rm -f "$cache/trim-stamp"
build KAI_CACHE_TRIM_DAYS=20
if stays "$cache/cmodular/old.o"; then ok "a 20-day window spares a 9-day entry"; else bad "window ignored"; fi
rm -f "$cache/trim-stamp"
build KAI_CACHE_TRIM_DAYS=2
if gone "$cache/cmodular/fresh.o"; then ok "a 2-day window trims a 3-day entry"; else bad "short window ignored"; fi

# race <backend>: builds that reuse entries past the window while trims
# run all succeed.
race() {
  BACKEND="$1"
  build KAI_CACHE_TRIM=0 KAI_BIN_MEMO=0
  find "$cache" -type f -exec perl -e '$t = time - 9 * 86400; utime($t, $t, @ARGV)' {} +
  plant
  pids=""
  for n in 1 2 3 4; do
    rm -f "$cache/trim-stamp"
    (kai KAI_BIN_MEMO=0 "$KAI" build hello.kai -o "$work/race$n" > "$work/race$n.out" 2>&1 \
      && "$work/race$n" > "$work/race$n.run") &
    pids="$pids $!"
  done
  racefail=0
  for p in $pids; do wait "$p" || racefail=$((racefail + 1)); done
  for n in 1 2 3 4; do
    [ "$(cat "$work/race$n.run" 2>/dev/null)" = "hello" ] || racefail=$((racefail + 1))
  done
  if [ "$racefail" -eq 0 ]; then ok "$BACKEND: builds on aged entries concurrent with trims succeed"
  else bad "$BACKEND: $racefail racing builds failed"; cat "$work"/race*.out; fi
  BACKEND=c
}

race c
if (cd "$work/pkg" && env XDG_CACHE_HOME="$xdg" KAI_BACKEND=native KAI_CACHE_TRIM=0 "$KAI" build hello.kai -o "$work/probe") \
  > "$work/probe.out" 2>&1 && ! grep -q 'native backend unavailable' "$work/probe.out"; then
  race native
else
  echo "  skip native race (no LLVM)"
fi

# kai clean --cache empties the shared cache; kai clean the package's.
build KAI_CACHE_TRIM=0
out="$(kai "$KAI" clean --cache)"
left="$(find "$cache" -type f 2>/dev/null | wc -l | tr -d ' ')"
if [ "$left" -eq 0 ] && echo "$out" | grep -q 'removed the shared cache'; then
  ok "kai clean --cache empties the cache"
else
  bad "kai clean --cache left $left files: $out"
fi
mkdir -p "$work/pkg/.kai-cache/x"
printf 'name = "t"\nentry = "hello.kai"\n' > "$work/pkg/kai.toml"
kai "$KAI" clean > /dev/null
if [ ! -e "$work/pkg/.kai-cache" ]; then ok "kai clean removes the package cache"; else bad "package cache survived"; fi

if [ "$fail" -ne 0 ]; then
  echo "cache-trim FAIL ($fail)"
  exit 1
fi
echo "cache-trim OK"
