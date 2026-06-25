#!/usr/bin/env bash
# StringBuilder vs naive ++-fold: build both to native binaries, time
# execution only (compiler startup excluded), print the sweep table.
# Heap-capped — the naive program is meant to blow past it at large N.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
kai="$root/bin/kai"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

gen() { # N kind file
  local n=$1 kind=$2 f=$3
  if [ "$kind" = sb ]; then
    cat > "$f" <<EOF
import string_builder
import loop
fn main() : Unit / Console = {
  var sb = string_builder.new()
  var i = 0
  while { @i < $n } { sb := string_builder.append(@sb, "frag"); i := @i + 1 }
  print("len=#{int_to_string(string.length(string_builder.build(@sb)))}")
}
EOF
  else
    cat > "$f" <<EOF
import loop
fn main() : Unit / Console = {
  var acc = ""
  var i = 0
  while { @i < $n } { acc := @acc ++ "frag"; i := @i + 1 }
  print("len=#{int_to_string(string.length(@acc))}")
}
EOF
  fi
}

time_bin() { { /usr/bin/time -p bash -c "KAI_MAX_HEAP=4g $1 >/dev/null 2>&1"; } 2>&1 | awk '/real/{print $2}'; }

printf "%-8s %-12s %-14s %-8s\n" "N" "naive(++)" "StringBuilder" "speedup"
for n in 20000 40000 80000; do
  gen "$n" naive "$tmp/n.kai"; "$kai" build "$tmp/n.kai" -o "$tmp/bn" >/dev/null 2>&1
  gen "$n" sb    "$tmp/s.kai"; "$kai" build "$tmp/s.kai" -o "$tmp/bs" >/dev/null 2>&1
  tn=$(time_bin "$tmp/bn"); ts=$(time_bin "$tmp/bs")
  sp=$(awk "BEGIN{ if ($ts>0) printf \"%.1fx\", $tn/$ts; else print \"-\" }")
  printf "%-8s %-12s %-14s %-8s\n" "$n" "${tn}s" "${ts}s" "$sp"
done
