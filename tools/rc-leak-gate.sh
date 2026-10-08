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
# tools/baselines/rc-leak/<name> holds one `<c>:<native>` line per
# fixture. A fixture whose count differs from its pinned value FAILS; a
# fixture with no file FAILS (a new fixture must record its measurement). A
# column holding `-` skips the fixture on that backend. One file per
# fixture keeps two changes to different fixtures from ever conflicting.
#
# Counts are deterministic per backend but NOT equal across them: the two
# backends disagree on how many cells survive to exit for a third of the
# corpus, so the baseline pins one column each. KAI_LEAK_BACKEND selects
# the backend (default c); KAI_LEAK_JOBS the worker count; KAI_LEAK_SHARD=I/N
# measures every N-th fixture from the I-th, while the orphan check below
# still sees the whole corpus.
#
# An exact pin cannot tell a leak from state that legitimately lives until
# exit, so a leak recorded when the pin was taken stays green forever. Each
# fixture therefore also runs main twice in one process
# (KAI_TRACE_RC_RUNS=2): growth = leaked(2 runs) - leaked(1 run) is what
# the program retains per run of its work. It must be 0, except for the
# known leaks pinned exactly in tools/baselines/rc-growth/<name>, one
# `<c>:<native>` line each.
#
# A fiber still queued at exit that never started is released by the ledger
# and reported apart, so an actor left unsynchronised costs no pin. One that
# started and is still alive at an `exit_code=0` fails the fixture: its frames
# hold references the ledger cannot settle. A runtime abort reports
# `exit_code=abort` and leaves frames alive by definition, so it is measured
# as before.
#
# examples/effects is held to growth only: its fixtures print through
# handlers, fibers and timers, so only the per-run retention is pinned, in
# tools/baselines/rc-effects-growth/<name> (a `-` column skips the backend).
#
# On the native backend every build also runs the KIR linearity check, which
# reports an owned reference not released exactly once on some path. Its
# violation count is pinned exactly in tools/baselines/kir-lin/<corpus>-<name>;
# a fixture with no file must have none.

set -u

cd "$(dirname "$0")/.."
export ROOT="$(pwd)"
export KAI="$ROOT/bin/kai"
export WORK="$ROOT/stage2/build/rc-leak-gate"
export BASELINE="$ROOT/tools/baselines/rc-leak"
export LIN_BASELINE="$ROOT/tools/baselines/kir-lin"
SKIPS="$ROOT/tools/rc-leak-skips.txt"
export BACKEND="${KAI_LEAK_BACKEND:-c}"
export RUN_TIMEOUT="${KAI_LEAK_TIMEOUT:-120}"
CORPORA="perceus effects"

# A fixture that never terminates must fail as TIMEOUT, not hang the gate:
# the shim falls back to perl where timeout(1) and gtimeout are missing.
. "$ROOT/tools/lib/timeout.sh"
export KAI_TIMEOUT_KIND _KAI_TIMEOUT_PERL
export -f kai_timeout
[ "$KAI_TIMEOUT_KIND" != none ] \
  || echo "rc-leak-gate: warning — no timeout, gtimeout or perl; fixture runs are unbounded" >&2

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
growth_dir() {
  case "$1" in
    effects) echo "$ROOT/tools/baselines/rc-effects-growth" ;;
    *)       echo "$ROOT/tools/baselines/rc-growth" ;;
  esac
}

# Whether a corpus also pins its `leaked` count exactly.
exact_pinned() { [ "$1" = perceus ]; }

# The baseline column for the backend under test; `-` means skipped there.
pinned() {
  local pin="${2:-$BASELINE}/$1" row
  [ -f "$pin" ] || return
  row="$(head -1 "$pin")"
  case "$BACKEND" in
    native) echo "${row#*:}" ;;
    *)      echo "${row%%:*}" ;;
  esac
}

# A fixture skipped on this backend carries `-` in the file that pins it.
skipped() {
  if exact_pinned "$1"; then [ "$(pinned "$2")" = - ]
  else [ "$(pinned "$2" "$(growth_dir "$1")")" = - ]; fi
}

# The pinned growth: 0 unless the fixture is a known leak.
pinned_growth() {
  local g; g="$(pinned "$2" "$(growth_dir "$1")")"
  echo "${g:-0}"
}

# One ledger run of `$1` with main run `$2` times: its `leaked`, or a tag.
# A fixture that ignores SIGTERM is killed after a grace period.
ledger_run() {
  local bin="$1" runs="$2" rc=0
  kai_timeout "$RUN_TIMEOUT" env KAI_THREADS=1 KAI_TRACE_RC=1 KAI_TRACE_RC_RUNS="$runs" "$bin" >"$bin.out" 2>"$bin.err" </dev/null || rc=$?
  case "$rc" in 124|137) echo TIMEOUT; return ;; esac
  local leaked started
  started="$(sed -n 's/^\[KAI_TRACE_RC\] *fibers_started_at_exit=\([0-9]*\).*/\1/p' "$bin.err" | head -1)"
  if [ -n "$started" ] && grep -q '^\[KAI_TRACE_RC\] *exit_code=0$' "$bin.err"; then
    echo "FIBERS-STARTED-AT-EXIT=$started"; return
  fi
  if [ -n "$started" ]; then echo "$started" > "$bin.aborted-fibers"; else rm -f "$bin.aborted-fibers"; fi
  leaked="$(sed -n 's/^\[KAI_TRACE_RC\] .*leaked=\([0-9-]*\).*/\1/p' "$bin.err" | head -1)"
  echo "${leaked:-NO-LEDGER}"
}

# The verdicts on programs built here: an actor that never starts is settled
# (growth 0), one parked in `receive` at a clean exit fails, a runtime abort
# with a fiber parked is measured and noted, and a run that never ends is a
# TIMEOUT.
self_test() {
  local dir="$WORK/self-test" once twice parked aborted spun
  mkdir -p "$dir"
  cat > "$dir/unstarted.kai" <<'EOF'
import actor

fn idle() : Unit / Actor[String] = ()

fn main() : Unit / Spawn = with_mailbox {
  let _k = spawn_actor(() => idle())
  ()
}
EOF
  cat > "$dir/started.kai" <<'EOF'
import actor

fn kid(parent: Pid[String]) : Unit / Actor[String] = {
  Actor.send(parent, "ready")
  let _never = Actor.receive()
  ()
}

fn main() : Unit / Spawn = with_mailbox {
  let me = Actor.self()
  let _k = spawn_actor(() => kid(me))
  let _ready = Actor.receive()
  ()
}
EOF
  cat > "$dir/aborted.kai" <<'EOF'
import spawn

fn main() : Int / Spawn = {
  var counter := 0
  let reader = () => { let _ = counter; () }
  nursery { n ->
    let _f = n.spawn(() => { let g = reader; g(); () })
    ()
  }
  0
}
EOF
  cat > "$dir/spin.kai" <<'EOF'
fn spin(n: Int) : Int = if n < 0 { n } else { spin((n + 1) % 1000) }

fn main() = print(int_to_string(spin(0)))
EOF
  for p in unstarted started aborted spin; do
    "$KAI" build --backend="$BACKEND" "$dir/$p.kai" -o "$dir/$p" >"$dir/$p.build" 2>&1 \
      || { echo "rc-leak-gate self-test: FAIL — $p does not build"; sed 's/^/  /' "$dir/$p.build"; return 1; }
  done
  once="$(ledger_run "$dir/unstarted" 1)"
  grep -q 'fibers_unstarted_at_exit=1$' "$dir/unstarted.err" \
    || { echo "rc-leak-gate self-test: FAIL — an unstarted actor is not settled"; return 1; }
  twice="$(ledger_run "$dir/unstarted" 2)"
  [ "$((twice - once))" -eq 0 ] \
    || { echo "rc-leak-gate self-test: FAIL — an unstarted actor grows by $((twice - once))"; return 1; }
  parked="$(ledger_run "$dir/started" 1)"
  [ "$parked" = FIBERS-STARTED-AT-EXIT=1 ] \
    || { echo "rc-leak-gate self-test: FAIL — a parked actor at exit reads as $parked"; return 1; }
  aborted="$(ledger_run "$dir/aborted" 1)"
  case "$aborted" in
    ''|*[!0-9-]*) echo "rc-leak-gate self-test: FAIL — an abort with a parked fiber reads as $aborted"; return 1 ;;
  esac
  [ -f "$dir/aborted.aborted-fibers" ] \
    || { echo "rc-leak-gate self-test: FAIL — an abort with a parked fiber is not noted"; return 1; }
  if [ "$KAI_TIMEOUT_KIND" != none ]; then
    spun="$(RUN_TIMEOUT=2 ledger_run "$dir/spin" 1)"
    [ "$spun" = TIMEOUT ] \
      || { echo "rc-leak-gate self-test: FAIL — a run that never ends reads as $spun"; return 1; }
  fi
  echo "rc-leak-gate self-test OK"
}

# One fixture `<corpus>/<name>`: build, run under the ledger once and twice,
# write `<leaked>:<growth>` (or a verdict tag) for the serial comparison.
measure_one() {
  local corpus="${1%%/*}" name="${1#*/}"
  local bin="$WORK/$corpus-$name"
  if skipped "$corpus" "$name"; then echo "-:-" > "$bin.measured"; return; fi
  # On native, kaic2 also runs the KIR linearity check over what it emits.
  local verify=""
  [ "$BACKEND" = native ] && verify="$bin.kv"
  if ! KAI_KIR_VERIFY="$verify" "$KAI" build --backend="$BACKEND" "$ROOT/examples/$1.kai" -o "$bin" >"$bin.build" 2>&1; then
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
export -f measure_one ledger_run pinned pinned_growth skipped growth_dir exact_pinned

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

shard="${KAI_LEAK_SHARD:-1/1}"
si="${shard%/*}"; sn="${shard#*/}"
case "$si/$sn" in
  *[!0-9/]*|/*|*/) echo "rc-leak-gate: KAI_LEAK_SHARD='$shard' is not I/N" >&2; exit 2 ;;
esac
[ "$si" -ge 1 ] && [ "$si" -le "$sn" ] || { echo "rc-leak-gate: KAI_LEAK_SHARD='$shard' is not I/N with 1 <= I <= N" >&2; exit 2; }
shard_list="$WORK/shard.txt"
awk -v i="$si" -v n="$sn" '(NR - i) % n == 0' "$fixtures" > "$shard_list"
total="$(wc -l < "$shard_list" | tr -d ' ')"

self_test || exit 1
echo "rc-leak-gate: $total of $(wc -l < "$fixtures" | tr -d ' ') fixtures (shard $si/$sn), $BACKEND backend, $JOBS workers"
xargs -P "$JOBS" -n 1 -I{} bash -c 'measure_one "$@"' _ {} < "$shard_list"

fail=0
while IFS= read -r id; do
  corpus="${id%%/*}"; name="${id#*/}"
  measured="$(cut -d: -f1 "$WORK/$corpus-$name.measured" 2>/dev/null)"
  growth="$(cut -d: -f2 "$WORK/$corpus-$name.measured" 2>/dev/null)"
  [ "$measured" = - ] && continue
  if exact_pinned "$corpus"; then
    expect="$(pinned "$name")"
    if [ -z "$expect" ]; then
      echo "FAIL $id — no pin at tools/baselines/rc-leak/$name; measured leaked=$measured"
      fail=1
    elif [ "$measured" != "$expect" ]; then
      echo "FAIL $id — leaked=$measured, baseline $expect"
      fail=1
    fi
  else
    case "$measured" in ''|*[!0-9-]*) echo "FAIL $id — $measured"; fail=1 ;; esac
  fi
  [ -f "$WORK/$corpus-$name.aborted-fibers" ] && \
    echo "note $id — exit_code=abort with $(cat "$WORK/$corpus-$name.aborted-fibers") started fiber(s) alive; measured as before"
  [ "$measured" = BUILD-FAIL ] && tail -4 "$WORK/$corpus-$name.build" 2>/dev/null | sed 's/^/    /'
  # The KIR linearity check's violations, pinned exactly per fixture.
  if [ "$BACKEND" = native ] && [ "$measured" != BUILD-FAIL ]; then
    lin="no report"
    [ -f "$WORK/$corpus-$name.kv" ] && lin="$(grep -c . "$WORK/$corpus-$name.kv")"
    lin_pin="$(head -1 "$LIN_BASELINE/$corpus-$name" 2>/dev/null)"
    if [ "$lin" != "${lin_pin:-0}" ]; then
      echo "FAIL $id — $lin KIR linearity violations, pinned ${lin_pin:-0}:"
      sed 's/^/    /' "$WORK/$corpus-$name.kv"
      fail=1
    fi
  fi
  # A run cannot free more than it allocated: negative growth means the
  # ledger missed allocations, never a fixed leak.
  case "$growth" in
    -[0-9]*) echo "FAIL $id — growth $growth per run: frees counted without their allocations"; fail=1; continue ;;
  esac
  # Checked even when leaked failed: a moved pin says nothing about growth.
  if [ "$growth" != - ] && [ "$growth" != "$(pinned_growth "$corpus" "$name")" ]; then
    echo "FAIL $id — leaked grows by $growth per run of main, pinned growth $(pinned_growth "$corpus" "$name")"
    fail=1
  fi
done < "$shard_list"

# A pin with no fixture behind it: the fixture was renamed or deleted and
# the pin was left orphaned.
orphans() {
  local corpus="$1" dir="$2" pin name
  for pin in "$dir"/*; do
    [ -f "$pin" ] || continue
    name="$(basename "$pin")"
    grep -qx "$corpus/$name" "$fixtures" || { echo "FAIL $corpus/$name — ${dir#"$ROOT"/}/$name has no fixture"; fail=1; }
  done
}
orphans perceus "$BASELINE"
for pin in "$LIN_BASELINE"/*; do
  [ -f "$pin" ] || continue
  key="$(basename "$pin")"
  grep -qx "${key%%-*}/${key#*-}" "$fixtures" || { echo "FAIL ${key%%-*}/${key#*-} — tools/baselines/kir-lin/$key has no fixture"; fail=1; }
done
for corpus in $CORPORA; do orphans "$corpus" "$(growth_dir "$corpus")"; done

[ "$fail" -eq 0 ] || { echo "rc-leak-gate: FAIL"; exit 1; }
echo "rc-leak-gate: PASS — $total fixtures match their pinned RC ledger."
