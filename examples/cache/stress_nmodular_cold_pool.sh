#!/bin/sh
# A cold native-modular build runs one emit job per partition on a thread pool,
# and every job parses and rewrites its own copy of the runtime bitcode. Memory
# a job frees while it still has a reference to it is another job's memory a
# moment later, so a defect there shows as an occasional failed or corrupt
# build that a quiet single build never meets. This gate repeats a cold build
# of a 24-module package with an empty object cache, a wide pool and every
# core busy, and wants every run to build and print the same result.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="$ROOT/bin/kai"
KAIC2="$ROOT/stage2/kaic2"
RUNS="${NMODULAR_STRESS_RUNS:-20}"
MODS=24

PROJ="$(mktemp -d)"
burners=""
cleanup() {
  for p in $burners; do kill "$p" 2>/dev/null || true; done
  rm -rf "$PROJ"
}
trap cleanup EXIT INT TERM

native_capable=0
if [ -x "$KAIC2" ]; then
  printf 'fn main() : Unit = ()\n' > "$PROJ/probe.kai"
  "$KAIC2" --edition "$(cat "$ROOT/EDITION")" --emit=native --path "$ROOT/stdlib" --path "$PROJ" "$PROJ/probe.kai" \
    >/dev/null 2>"$PROJ/native-probe.err" || true
  grep -q "not built into this compiler" "$PROJ/native-probe.err" 2>/dev/null || native_capable=1
fi
if [ "$native_capable" != "1" ]; then
  echo "stress_nmodular_cold_pool SKIP — kaic2 has no libLLVM native backend"
  exit 0
fi

mkdir -p "$PROJ/pkg"
k=1
while [ "$k" -le "$MODS" ]; do
  K=$(printf '%03d' "$k"); P=$(printf '%03d' $((k - 1)))
  {
    [ "$k" -gt 1 ] && echo "import pkg.m$P"
    echo "#[derive(Eq, Show)]"
    echo "pub type R$K = { a$K: Int, b$K: Int, c$K: String }"
    echo "pub fn mk$K(n: Int) : R$K = R$K { a$K: n, b$K: n * 2, c$K: int_to_string(n) }"
    if [ "$k" -eq 1 ]; then echo "pub fn chain$K(n: Int) : Int = mk$K(n).a$K + mk$K(n).b$K"
    else echo "pub fn chain$K(n: Int) : Int = chain$P(n) + mk$K(n).a$K"; fi
  } > "$PROJ/pkg/m$K.kai"
  k=$((k + 1))
done
last=$(printf '%03d' "$MODS")
printf 'import pkg.m%s\n\nfn main() : Unit / Stdout = Stdout.print(int_to_string(chain%s(7)))\n' "$last" "$last" > "$PROJ/main.kai"

cpus="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
i=0
while [ "$i" -lt "$cpus" ]; do
  yes >/dev/null &
  burners="$burners $!"
  i=$((i + 1))
done

want=""
run=1
while [ "$run" -le "$RUNS" ]; do
  rm -rf "$PROJ/nmcache" "$PROJ/out"
  if ! KAI_NATIVE_JOBS=8 KAI_BIN_MEMO=0 KAI_BACKEND=native \
       KAI_NATIVE_MODULAR_CACHE_DIR="$PROJ/nmcache" KAI_CORE_CACHE_DIR="$PROJ/core-cache" \
       "$KAI" build "$PROJ/main.kai" -o "$PROJ/out" >"$PROJ/build.out" 2>"$PROJ/build.err"; then
    echo "stress_nmodular_cold_pool FAIL — cold build $run of $RUNS failed"
    cat "$PROJ/build.err"
    exit 1
  fi
  got="$("$PROJ/out")"
  [ -n "$want" ] || want="$got"
  [ "$got" = "$want" ] || { echo "stress_nmodular_cold_pool FAIL — cold build $run printed '$got', not '$want'"; exit 1; }
  run=$((run + 1))
done

echo "stress_nmodular_cold_pool OK — $RUNS cold builds under load, every one built and printed '$want'"
