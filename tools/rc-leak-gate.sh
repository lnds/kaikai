#!/bin/bash
# Perceus leak gate — the RC ledger of every examples/perceus fixture pinned
# to an exact allocation count, and the per-run growth of every
# examples/perceus and examples/effects fixture held to a ratchet.
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
#
# An exact pin cannot tell a leak from state that legitimately lives until
# exit, so a leak recorded when the pin was taken stays green forever. Each
# fixture therefore also runs main twice in one process
# (KAI_TRACE_RC_RUNS=2): growth = leaked(2 runs) - leaked(1 run) is what
# the program retains per run of its work. It must be 0, except for the
# known leaks pinned exactly in tools/rc-growth-baseline.txt, one
# `<name>:<c>:<native>` line each.
#
# examples/effects is held to growth only: its fixtures print through
# handlers, fibers and timers, so only the per-run retention is pinned, in
# tools/rc-effects-growth-baseline.txt (a `-` column skips the backend).

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
CORPORA="perceus effects"

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

# The growth ratchet of a corpus.
growth_file() {
  case "$1" in
    effects) echo "$ROOT/tools/rc-effects-growth-baseline.txt" ;;
    *)       echo "$ROOT/tools/rc-growth-baseline.txt" ;;
  esac
}

# Whether a corpus also pins its `leaked` count exactly.
exact_pinned() { [ "$1" = perceus ]; }

# The baseline column for the backend under test; `-` means skipped there.
pinned() {
  local row; row="$(sed -n "s/^$1://p" "${2:-$BASELINE}" | head -1)"
  [ -n "$row" ] || return
  case "$BACKEND" in
    native) echo "${row#*:}" ;;
    *)      echo "${row%%:*}" ;;
  esac
}

# A fixture skipped on this backend carries `-` in the file that pins it.
skipped() {
  if exact_pinned "$1"; then [ "$(pinned "$2")" = - ]
  else [ "$(pinned "$2" "$(growth_file "$1")")" = - ]; fi
}

# The pinned growth: 0 unless the fixture is a known leak.
pinned_growth() {
  local g; g="$(pinned "$2" "$(growth_file "$1")")"
  echo "${g:-0}"
}

# One ledger run of `$1` with main run `$2` times: its `leaked`, or a tag.
# A fixture that ignores SIGTERM is killed after a grace period.
ledger_run() {
  local bin="$1" runs="$2" rc=0
  if [ -n "$TIMEOUT_CMD" ]; then
    "$TIMEOUT_CMD" -k 10 "$RUN_TIMEOUT" env KAI_THREADS=1 KAI_TRACE_RC=1 KAI_TRACE_RC_RUNS="$runs" "$bin" >"$bin.out" 2>"$bin.err" </dev/null || rc=$?
  else
    env KAI_THREADS=1 KAI_TRACE_RC=1 KAI_TRACE_RC_RUNS="$runs" "$bin" >"$bin.out" 2>"$bin.err" </dev/null || rc=$?
  fi
  case "$rc" in 124|137) echo TIMEOUT; return ;; esac
  local leaked
  leaked="$(sed -n 's/^\[KAI_TRACE_RC\] .*leaked=\([0-9-]*\).*/\1/p' "$bin.err" | head -1)"
  echo "${leaked:-NO-LEDGER}"
}

# One fixture `<corpus>/<name>`: build, run under the ledger once and twice,
# write `<leaked>:<growth>` (or a verdict tag) for the serial comparison.
measure_one() {
  local corpus="${1%%/*}" name="${1#*/}"
  local bin="$WORK/$corpus-$name"
  if skipped "$corpus" "$name"; then echo "-:-" > "$bin.measured"; return; fi
  if ! "$KAI" build --backend="$BACKEND" "$ROOT/examples/$1.kai" -o "$bin" >"$bin.build" 2>&1; then
    echo "BUILD-FAIL:-" > "$bin.measured"; return
  fi
  local once twice growth=-
  once="$(ledger_run "$bin" 1)"
  # `leaked` goes negative when a run frees cells allocated before tracing.
  case "$once" in
    ''|*[!0-9-]*) ;;
    *) twice="$(ledger_run "$bin" 2)"
       case "$twice" in ''|*[!0-9-]*) growth="$twice" ;; *) growth=$((twice - once)) ;; esac ;;
  esac
  echo "$once:$growth" > "$bin.measured"
}
export -f measure_one ledger_run pinned pinned_growth skipped growth_file exact_pinned

# A fixture with an `.err.expected` golden is a compile-error test, and a
# `library` or `harness` line in tools/rc-leak-skips.txt names one the gate
# cannot run standalone: all of them leave the corpus. A per-backend skip
# instead carries `-` in the file that pins the fixture and stays in it.
collect_fixtures() {
  local corpus src name
  for corpus in $CORPORA; do
    for src in "$ROOT/examples/$corpus"/*.kai; do
      name="$(basename "$src" .kai)"
      [ -f "${src%.kai}.err.expected" ] && continue
      grep -qE "^$name:(library|harness):" "$SKIPS" 2>/dev/null && continue
      echo "$corpus/$name"
    done
  done
}

fixtures="$WORK/fixtures.txt"
collect_fixtures > "$fixtures"
total="$(wc -l < "$fixtures" | tr -d ' ')"

echo "rc-leak-gate: $total fixtures, $BACKEND backend, $JOBS workers"
xargs -P "$JOBS" -n 1 -I{} bash -c 'measure_one "$@"' _ {} < "$fixtures"

fail=0
while IFS= read -r id; do
  corpus="${id%%/*}"; name="${id#*/}"
  measured="$(cut -d: -f1 "$WORK/$corpus-$name.measured" 2>/dev/null)"
  growth="$(cut -d: -f2 "$WORK/$corpus-$name.measured" 2>/dev/null)"
  [ "$measured" = - ] && continue
  if exact_pinned "$corpus"; then
    expect="$(pinned "$name")"
    if [ -z "$expect" ]; then
      echo "FAIL $id — not in $(basename "$BASELINE"); measured leaked=$measured"
      fail=1
    elif [ "$measured" != "$expect" ]; then
      echo "FAIL $id — leaked=$measured, baseline $expect"
      fail=1
    fi
  else
    case "$measured" in ''|*[!0-9-]*) echo "FAIL $id — $measured"; fail=1 ;; esac
  fi
  [ "$measured" = BUILD-FAIL ] && tail -4 "$WORK/$corpus-$name.build" 2>/dev/null | sed 's/^/    /'
  # Checked even when leaked failed: a moved pin says nothing about growth.
  if [ "$growth" != - ] && [ "$growth" != "$(pinned_growth "$corpus" "$name")" ]; then
    echo "FAIL $id — leaked grows by $growth per run of main, pinned growth $(pinned_growth "$corpus" "$name")"
    fail=1
  fi
done < "$fixtures"

# A baseline line with no fixture behind it: the fixture was renamed or
# deleted and the line was left orphaned.
orphans() {
  local corpus="$1" file="$2" name
  while IFS=: read -r name _; do
    case "$name" in ''|\#*) continue ;; esac
    grep -qx "$corpus/$name" "$fixtures" || { echo "FAIL $corpus/$name — $(basename "$file") line has no fixture"; fail=1; }
  done < "$file"
}
orphans perceus "$BASELINE"
for corpus in $CORPORA; do orphans "$corpus" "$(growth_file "$corpus")"; done

[ "$fail" -eq 0 ] || { echo "rc-leak-gate: FAIL"; exit 1; }
echo "rc-leak-gate: PASS — $total fixtures match their pinned RC ledger."
