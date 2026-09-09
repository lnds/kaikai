#!/bin/bash
# Perceus leak gate — every examples/perceus fixture's RC ledger pinned to
# an exact allocation count.
#
# The corpus's other harnesses diff stdout, which a leak survives untouched:
# the program prints the right answer and takes the memory with it. The RC
# counters are compiled in unconditionally and only the report is env-gated,
# so running an existing fixture binary under KAI_TRACE_RC=1 prints
# `alloc_total/free_total/leaked` on stderr at no build cost.
#
# `leaked > 0` is NOT by itself a defect: a program that still holds its
# structure when main returns exits without a final free walk, so whatever
# is live at exit counts as leaked. A tree the fixture deliberately keeps
# alive therefore reports `leaked` proportional to its size. What the gate
# protects is the EXACT number: a regression that retains one extra
# reference per iteration moves it, whatever its baseline was.
#
# tools/rc-leak-baseline.txt holds one `<name>:<c>:<native>` line per
# fixture. A fixture whose count differs from its pinned value FAILS; an
# unlisted fixture FAILS (a new fixture must record its measurement). A
# column holding `-` skips the fixture on that backend.
#
# Counts are deterministic per backend but NOT equal across them: the two
# backends disagree on how many cells survive to exit for a third of the
# corpus, so the baseline pins one column each. KAI_LEAK_BACKEND selects
# the backend (default c); KAI_LEAK_JOBS the worker count.

set -u

cd "$(dirname "$0")/.."
export ROOT="$(pwd)"
export KAI="$ROOT/bin/kai"
export WORK="$ROOT/stage2/build/rc-leak-gate"
export BASELINE="$ROOT/tools/rc-leak-baseline.txt"
SKIPS="$ROOT/tools/rc-leak-skips.txt"
export BACKEND="${KAI_LEAK_BACKEND:-c}"
export RUN_TIMEOUT="${KAI_LEAK_TIMEOUT:-120}"
export TIMEOUT_CMD="$(command -v timeout || command -v gtimeout || true)"

rm -rf "$WORK"; mkdir -p "$WORK"

JOBS="${KAI_LEAK_JOBS:-$( { nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null; } | head -n1 )}"
case "$JOBS" in ''|*[!0-9]*) JOBS=4 ;; esac

if [ "$BACKEND" = native ]; then
  probe="$WORK/native-probe"
  printf 'fn main() { println("ok") }\n' > "$probe.kai"
  if ! "$KAI" build --backend=native "$probe.kai" -o "$probe" 2>"$probe.err"; then
    echo "rc-leak-gate: SKIP — kaic2 cannot run the native backend:" >&2
    sed 's/^/  /' "$probe.err" >&2
    exit 2
  fi
fi

# The baseline column for the backend under test; `-` means skipped there.
pinned() {
  local row; row="$(sed -n "s/^$1://p" "$BASELINE" | head -1)"
  [ -n "$row" ] || return
  case "$BACKEND" in
    native) echo "${row#*:}" ;;
    *)      echo "${row%%:*}" ;;
  esac
}

# One fixture: build, run under the ledger, write `<name>:<leaked>` (or a
# verdict tag) for the serial comparison pass.
measure_one() {
  local name="$1" bin="$WORK/$1"
  if [ "$(pinned "$name")" = - ]; then echo "$name:-" > "$bin.measured"; return; fi
  if ! "$KAI" build --backend="$BACKEND" "$ROOT/examples/perceus/$name.kai" -o "$bin" >"$bin.build" 2>&1; then
    echo "$name:BUILD-FAIL" > "$bin.measured"; return
  fi
  local rc=0
  if [ -n "$TIMEOUT_CMD" ]; then
    "$TIMEOUT_CMD" "$RUN_TIMEOUT" env KAI_THREADS=1 KAI_TRACE_RC=1 "$bin" >"$bin.out" 2>"$bin.err" </dev/null || rc=$?
  else
    env KAI_THREADS=1 KAI_TRACE_RC=1 "$bin" >"$bin.out" 2>"$bin.err" </dev/null || rc=$?
  fi
  [ "$rc" -eq 124 ] && { echo "$name:TIMEOUT" > "$bin.measured"; return; }
  local leaked
  leaked="$(sed -n 's/^\[KAI_TRACE_RC\] .*leaked=\([0-9-]*\).*/\1/p' "$bin.err" | head -1)"
  echo "$name:${leaked:-NO-LEDGER}" > "$bin.measured"
}
export -f measure_one pinned
export BASELINE

# Fixtures the gate cannot execute standalone (negative tests, library
# modules with no main) are listed in tools/rc-leak-skips.txt. Those lines
# drop the fixture from the corpus entirely; a per-backend skip instead
# carries `-` in that column of the baseline and stays in the corpus.
collect_fixtures() {
  local src name
  for src in "$ROOT/examples/perceus"/*.kai; do
    name="$(basename "$src" .kai)"
    grep -qE "^$name:(negative|library):" "$SKIPS" 2>/dev/null && continue
    echo "$name"
  done
}

fixtures="$WORK/fixtures.txt"
collect_fixtures > "$fixtures"
total="$(wc -l < "$fixtures" | tr -d ' ')"

echo "rc-leak-gate: $total fixtures, $BACKEND backend, $JOBS workers"
xargs -P "$JOBS" -n 1 -I{} bash -c 'measure_one "$@"' _ {} < "$fixtures"

fail=0
while IFS= read -r name; do
  measured="$(cut -d: -f2- "$WORK/$name.measured" 2>/dev/null)"
  expect="$(pinned "$name")"
  if [ "$expect" = - ]; then
    continue
  elif [ -z "$expect" ]; then
    echo "FAIL $name — not in $(basename "$BASELINE"); measured leaked=$measured"
    fail=1
  elif [ "$measured" != "$expect" ]; then
    echo "FAIL $name — leaked=$measured, baseline $expect"
    [ "$measured" = BUILD-FAIL ] && tail -4 "$WORK/$name.build" 2>/dev/null | sed 's/^/    /'
    fail=1
  fi
done < "$fixtures"

# A baseline line with no fixture behind it: the fixture was renamed or
# deleted and the line was left orphaned.
while IFS=: read -r name _; do
  case "$name" in ''|\#*) continue ;; esac
  grep -qx "$name" "$fixtures" || { echo "FAIL $name — baseline line has no fixture"; fail=1; }
done < "$BASELINE"

[ "$fail" -eq 0 ] || { echo "rc-leak-gate: FAIL"; exit 1; }
echo "rc-leak-gate: PASS — $total fixtures match their pinned RC ledger."
