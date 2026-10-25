#!/usr/bin/env bash
# Windows side: compile the runtime and each probe, stubbing missing headers to see past them,
# then group the errors; link and run what compiles.
#   audit.sh <gen-dir> <toolchain-label> <cc> [extra cflags...]
set -uo pipefail
GEN="$1"; TC="$2"; CC="$3"; shift 3
EXTRA=("$@")
REPORT="$GEN/../report-$TC"; mkdir -p "$REPORT"
INC="$GEN/include"
OFF=(-DKAI_HOST_FIBERS=0 -DKAI_HOST_STACK_GUARD=0 -DKAI_HOST_REACTOR=0 -DKAI_HOST_SOCKETS=0
     -DKAI_HOST_PROCESS=0 -DKAI_HOST_SIGNALS=0 -DKAI_HOST_BACKTRACE=0 -DKAI_HOST_UNWIND=0)
BASE=(-std=c99 -O2 -w -ferror-limit=0 -Wno-unused-function -Wno-unused-variable -DKAI_SEPARATE_COMPILATION=1)

# compile <log> <src> <obj> <flags...>: repeat with an empty stub for each missing header.
compile() {
  local log="$1" src="$2" obj="$3"; shift 3
  local stub="$REPORT/stubs"; mkdir -p "$stub"
  : > "$log.stubs"
  for _ in $(seq 1 40); do
    "$CC" "${BASE[@]}" "${EXTRA[@]}" "$@" -I "$INC" -I "$stub" -c "$src" -o "$obj" > "$log" 2>&1 && return 0
    local miss
    miss=$(grep -oE "'[^']+' file not found" "$log" | head -1 | sed -E "s/'([^']+)' file not found/\1/")
    [ -z "$miss" ] && return 1
    mkdir -p "$stub/$(dirname "$miss")"; : > "$stub/$miss"; echo "$miss" >> "$log.stubs"
  done
  return 1
}

summarize() {
  local log="$1"
  local n; n=$(grep -cE "error:" "$log" || true)
  echo "  errors: $n; stubbed headers: $(tr '\n' ' ' < "$log.stubs")"
  grep -oE "(undeclared identifier|undeclared function|unknown type name|incomplete type|no member named) '[^']+'" "$log" \
    | sort | uniq -c | sort -rn | head -60 | sed 's/^/    /'
  grep -E "error:" "$log" | grep -vE "undeclared identifier|undeclared function|unknown type name|incomplete type|no member named" \
    | sed -E 's/^[^ ]*error: //' | sort | uniq -c | sort -rn | head -15 | sed 's/^/    other: /'
}

for cfg in default seams-off; do
  flags=(); [ "$cfg" = seams-off ] && flags=("${OFF[@]}")
  echo "=== $TC / $cfg ==="
  rt="$REPORT/rt-$cfg"
  if compile "$rt.log" "$INC/runtime_owner_c.c" "$rt.o" "${flags[@]}" -DKAI_RUNTIME_OWNER=1 -DKAI_PROGRAM_PROVIDES_MAIN=1; then
    echo "runtime: compiles (stubbed: $(tr '\n' ' ' < "$rt.log.stubs"))"
  else
    echo "runtime: FAILS"; summarize "$rt.log"
  fi
  for d in "$GEN"/probes/*/; do
    n=$(basename "$d"); o="$REPORT/$n-$cfg"
    if ! compile "$o.log" "$d/out.c" "$o.o" "${flags[@]}"; then
      echo "$n: compile FAILS ($(grep -cE 'error:' "$o.log") errors; first: $(grep -m1 -E 'error:' "$o.log" | sed -E 's/^[^ ]*error: //'))"
      continue
    fi
    [ -f "$rt.o" ] || { echo "$n: compiles; no runtime object to link"; continue; }
    if ! "$CC" "${EXTRA[@]}" "$o.o" "$rt.o" -o "$o.exe" > "$o.link" 2>&1; then
      echo "$n: compiles, link FAILS:"; grep -oE "(undefined (reference to|symbol:?)|unresolved external symbol) [^ ]+" "$o.link" | sort -u | head -30 | sed 's/^/    /'
      continue
    fi
    printf 'first line\nsecond line\n' | KAI_THREADS=1 timeout 30 "$o.exe" > "$o.out" 2> "$o.err"; rc=$?
    want=$(cat "$d/expected.rc")
    if [ "$rc" = "$want" ] && diff -q --strip-trailing-cr "$o.out" "$d/expected.out" >/dev/null; then
      echo "$n: RUNS, output and exit status match"
    else
      echo "$n: runs, DIFFERS (rc $rc, want $want)"; diff --strip-trailing-cr "$d/expected.out" "$o.out" | head -8 | sed 's/^/    /'
      head -c 400 "$o.err" | sed 's/^/    stderr: /'
    fi
  done
done
