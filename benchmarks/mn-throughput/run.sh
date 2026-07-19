#!/usr/bin/env bash
# Wall-clock throughput harness: kaikai M:N actors vs BEAM processes.
#
# Reported, never a gate. Wall-clock on a shared developer box is noisy;
# a perf regression should be visible without blocking merges on it.
#
# Method (the rules that make the numbers trustworthy):
#   - serial runs only, one measured process at a time;
#   - the first run of every cell is discarded (macOS cold start is
#     ~250-350ms and lands entirely in run 1);
#   - median of RUNS warm runs, not the mean;
#   - both sides time their own parallel region with a monotonic clock,
#     so BEAM VM boot is not charged to BEAM throughput. Total process
#     wall is reported separately so the startup tax stays visible.
#   - every cell must print the same checksum; a mismatch aborts.
#
# Env knobs: RUNS, SCENARIOS, KAI_THREAD_LIST, BEAM_SCHEDS, SKIP_BEAM.

set -euo pipefail
cd "$(dirname "$0")"
HERE="$(pwd)"
ROOT="$(cd ../.. && pwd)"

RUNS="${RUNS:-5}"
KAI_THREAD_LIST="${KAI_THREAD_LIST:-1 2 4 8 16}"
BEAM_SCHEDS="${BEAM_SCHEDS:-default 1}"
# scenario := <name>:<workers>:<load>
SCENARIOS="${SCENARIOS:-serial:1:120000000 parallel16:16:120000000}"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
BIN="$TMP/mn_throughput"
EBIN="$TMP/ebin"

# --- build both sides once -------------------------------------------------

echo "building kaikai load..."
"$ROOT/bin/kai" build "$HERE/mn_throughput.kai" -o "$BIN" >/dev/null

HAVE_BEAM=1
if [ -n "${SKIP_BEAM:-}" ] || ! command -v elixirc >/dev/null 2>&1; then
  HAVE_BEAM=0
  echo "elixirc not found — kaikai columns only"
else
  echo "building Elixir load..."
  mkdir -p "$EBIN"
  elixirc -o "$EBIN" "$HERE/mn_throughput.ex" >/dev/null
fi

# --- environment banner ----------------------------------------------------

echo
echo "host:     $(uname -sm), $(getconf _NPROCESSORS_ONLN) cpu"
echo "kaikai:   $("$ROOT/bin/kai" --version 2>/dev/null | head -1)"
echo "commit:   $(git -C "$ROOT" rev-parse --short HEAD)"
if [ "$HAVE_BEAM" = 1 ]; then
  echo "elixir:   $(elixir --version 2>/dev/null | grep '^Elixir' || echo unknown)"
  echo "erlang:   $(elixir --version 2>/dev/null | grep '^Erlang' || echo unknown)"
fi
echo "runs:     $RUNS warm (first run of each cell discarded), median reported"
echo

# --- measurement -----------------------------------------------------------

CHECKSUM=""
HEADER=""
declare -a ROWS=()
declare -a WROWS=()

# Runs one command RUNS+1 times, discards the first, and sets
# MED_NS / MED_WALL to the medians. Verifies the checksum is stable.
measure() {
  local label="$1"; shift
  local ns_samples=() wall_samples=()
  local i out total ns wall
  for i in $(seq 0 "$RUNS"); do
    wall="$( { /usr/bin/time -p "$@" >"$TMP/out" ; } 2>&1 | awk '/^real/{print $2}')"
    out="$(cat "$TMP/out")"
    total="$(printf '%s\n' "$out" | sed -n 's/.*total=\([0-9-]*\).*/\1/p')"
    ns="$(printf '%s\n' "$out" | sed -n 's/.*elapsed_ns=\([0-9]*\).*/\1/p')"
    if [ -z "$total" ] || [ -z "$ns" ]; then
      echo "FAIL [$label]: unparseable output: $out" >&2
      exit 1
    fi
    if [ -z "$CHECKSUM" ]; then
      CHECKSUM="$total"
    elif [ "$total" != "$CHECKSUM" ]; then
      echo "FAIL [$label]: checksum $total != $CHECKSUM — the runtime lost or duplicated work" >&2
      exit 1
    fi
    [ "$i" -eq 0 ] && continue   # cold start
    ns_samples+=("$ns")
    wall_samples+=("$wall")
  done
  MED_NS="$(printf '%s\n' "${ns_samples[@]}" | sort -n | awk -v n="$RUNS" 'NR==int((n+1)/2)')"
  MED_WALL="$(printf '%s\n' "${wall_samples[@]}" | sort -n | awk -v n="$RUNS" 'NR==int((n+1)/2)')"
}

ms() { awk -v ns="$1" 'BEGIN{printf "%.1f", ns/1000000}'; }

for scenario in $SCENARIOS; do
  name="${scenario%%:*}"; rest="${scenario#*:}"
  workers="${rest%%:*}"; load="${rest#*:}"
  CHECKSUM=""
  row="\`$name\` (${workers} x ${load})"
  wrow="$row"
  headers="scenario"

  for t in $KAI_THREAD_LIST; do
    echo "measuring $name: kaikai KAI_THREADS=$t"
    measure "kai@$t" env "KAI_THREADS=$t" "$BIN" "$workers" "$load"
    headers="$headers | kaikai@$t"
    row="$row | $(ms "$MED_NS")"
    wrow="$wrow | $MED_WALL"
  done

  if [ "$HAVE_BEAM" = 1 ]; then
    for s in $BEAM_SCHEDS; do
      echo "measuring $name: BEAM +S $s"
      if [ "$s" = default ]; then
        measure "beam@$s" elixir -pa "$EBIN" -e "MnThroughput.main($workers, $load)"
      else
        measure "beam@$s" elixir --erl "+S $s:$s" -pa "$EBIN" -e "MnThroughput.main($workers, $load)"
      fi
      headers="$headers | BEAM@$s"
      row="$row | $(ms "$MED_NS")"
      wrow="$wrow | $MED_WALL"
    done
  fi

  HEADER="$headers"
  ROWS+=("$row | $CHECKSUM")
  WROWS+=("$wrow")
done

# --- report ----------------------------------------------------------------

table() {
  local header="$1"; shift
  local sep; sep="$(printf '%s\n' "$header" | sed 's/[^|]*/---/g')"
  echo "| $header |"
  echo "| $sep |"
  printf '| %s |\n' "$@"
}

echo
echo "### Median wall-clock of the parallel region (ms, lower is better)"
echo
table "$HEADER | checksum" "${ROWS[@]}"
echo
echo "### Median total process wall (s) — same runs, including VM startup"
echo
table "$HEADER" "${WROWS[@]}"
