#!/bin/bash
# The global compile-slot budget: several `kai build` at once, cold caches, never run more
# C compiler processes together than KAI_JOBS (or the make jobserver) allows.
#
# Cases: (1) four concurrent builds under KAI_JOBS=2; (2) two builds starting at the same
# instant under KAI_JOBS=2 both finish; (3) three concurrent builds under `make -j3`.
# The sampler counts compile and link processes whose command line mentions the scratch dir
# (version and target probes are not compiles).

set -eu

# An enclosing make would make kai a jobserver client and bypass KAI_JOBS.
unset MAKEFLAGS MFLAGS MAKELEVEL

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAI="${KAI:-$ROOT/bin/kai}"
SAMPLE_MS="${SAMPLE_MS:-0.02}"

W="$(mktemp -d "${TMPDIR:-/tmp}/kai-job-budget.XXXXXX")"
trap 'rm -rf "$W"' EXIT

src() {
  mkdir -p "$W/src$1"
  printf 'fn main() : Unit / Stdout { Stdout.print("hi %s") }\n' "$1" > "$W/src$1/main.kai"
}

# Highest compiler-process count seen until $W/stop appears; prints it on exit.
sampler() {
  local peak=0 n
  while [ ! -e "$W/stop" ]; do
    n=$(ps -axo pid=,ppid=,command= | awk -v w="$W" '
      $0 ~ w && $3 ~ /(^|\/)(cc|clang|gcc|cc1)$/ && ($0 ~ / -c / || $0 ~ / -o /) { c[$1] = $2 }
      END { for (p in c) if (!(c[p] in c)) n++; print n + 0 }')
    [ "$n" -gt "$peak" ] && peak=$n
    sleep "$SAMPLE_MS"
  done
  echo "$peak" > "$W/peak"
}

# Runs "$@" with a fresh cache and the sampler, then prints the peak.
measure() {
  local cache="$W/cache.$RANDOM"
  rm -f "$W/stop" "$W/peak"
  export XDG_CACHE_HOME="$cache" KAI_MODULAR_CACHE_DIR="$cache/modular" TMPDIR="$W"
  sampler &
  local sp=$!
  local rc=0
  "$@" || rc=$?
  touch "$W/stop"
  wait "$sp"
  cat "$W/peak"
  return $rc
}

build_all() {
  local pids=() i rc=0
  for i in "$@"; do
    KAI_BACKEND=c "$KAI" build "$W/src$i/main.kai" -o "$W/out$i" > /dev/null &
    pids+=($!)
  done
  for p in "${pids[@]}"; do wait "$p" || rc=1; done
  return $rc
}

fail=0
check() { # name peak limit status
  if [ "$4" -ne 0 ]; then echo "FAIL $1: a build failed"; fail=1
  elif [ "$2" -gt "$3" ]; then echo "FAIL $1: peak $2 compilers > $3"; fail=1
  elif [ "$2" -lt 1 ]; then echo "FAIL $1: sampler saw no compiler"; fail=1
  else echo "ok   $1: peak $2 <= $3"; fi
}

for i in 1 2 3 4; do src $i; done

st=0; peak=$(KAI_JOBS=2 measure build_all 1 2 3 4) || st=$?
check "four builds, KAI_JOBS=2" "$peak" 2 "$st"

st=0; peak=$(KAI_JOBS=2 measure build_all 1 2) || st=$?
check "two simultaneous builds, KAI_JOBS=2" "$peak" 2 "$st"

cat > "$W/Makefile" <<EOF
all: o1 o2 o3
o%:
	+KAI_BACKEND=c $KAI build $W/src\$*/main.kai -o $W/mk\$* > /dev/null
EOF
st=0; peak=$(unset KAI_JOBS; measure make -s -j3 -C "$W" all) || st=$?
check "three builds under make -j3" "$peak" 3 "$st"

# The fifo jobserver of GNU make >= 4.4, played by the script: two tokens, three clients each
# holding make's implicit token, so at most five compilers; every token must come back.
mkfifo "$W/jobserver"
exec 9<> "$W/jobserver"
printf '++' >&9
st=0; peak=$(unset KAI_JOBS; MAKEFLAGS="-j3 --jobserver-auth=fifo:$W/jobserver" measure build_all 1 2 3) || st=$?
check "three builds on a fifo jobserver with two tokens" "$peak" 5 "$st"
left=0
while read -r -n1 -t 0.3 -u 9 _; do left=$((left + 1)); done
[ "$left" -eq 2 ] || { echo "FAIL fifo jobserver: $left of 2 tokens returned"; fail=1; }

for i in 1 2; do
  [ "$("$W/out$i")" = "hi $i" ] || { echo "FAIL output of build $i"; fail=1; }
done

exit "$fail"
