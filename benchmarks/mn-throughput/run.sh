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
#     wall is reported separately so the startup tax stays visible;
#   - every run must print the same checksum; a mismatch aborts.
#
# One table per kaikai backend, each carrying its own BEAM columns. The
# BEAM cells are therefore measured once per backend pass — deliberately,
# as an independent replicate of the reference columns.
#
# Env knobs: RUNS, SCENARIOS, KAI_THREAD_LIST, KAI_BACKENDS, BEAM_SCHEDS,
# SKIP_BEAM.

set -euo pipefail
cd "$(dirname "$0")"
HERE="$(pwd)"
ROOT="$(cd ../.. && pwd)"

RUNS="${RUNS:-5}"
KAI_THREAD_LIST="${KAI_THREAD_LIST:-1 2 4 8 16}"
KAI_BACKENDS="${KAI_BACKENDS:-native c}"
BEAM_SCHEDS="${BEAM_SCHEDS:-default 1}"
NOISE_PCT="${NOISE_PCT:-10}"
# scenario := <name>:<workers>:<load>. The three parallel scenarios grind
# the same 1.6e9 total units, so their rows compare directly: what changes
# is how that work is cut up, i.e. how much the scheduler has to move.
SCENARIOS="${SCENARIOS:-serial:1:200000000 parallel8:8:200000000 parallel16:16:100000000 oversub64:64:25000000}"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
EBIN="$TMP/ebin"

HAVE_BEAM=1
if [ -n "${SKIP_BEAM:-}" ] || ! command -v elixirc >/dev/null 2>&1; then
  HAVE_BEAM=0
else
  mkdir -p "$EBIN"
  elixirc -o "$EBIN" "$HERE/mn_throughput.ex" >/dev/null
fi

# --- environment banner ----------------------------------------------------

echo "host:     $(uname -sm), $(getconf _NPROCESSORS_ONLN) cpu"
echo "kaikai:   $("$ROOT/bin/kai" --version 2>/dev/null | head -1)"
echo "commit:   $(git -C "$ROOT" rev-parse --short HEAD)"
if [ "$HAVE_BEAM" = 1 ]; then
  elixir --version 2>/dev/null | grep -E '^(Elixir|Erlang)' | sed 's/^/          /'
else
  echo "elixir:   not found — kaikai columns only"
fi
echo "runs:     $RUNS warm per cell (first discarded), median reported"
echo

# --- measurement -----------------------------------------------------------

CHECKSUM=""
declare -a NOISY=()

# Runs one command RUNS+1 times, discards the first, sets MED_NS/MED_WALL
# to the medians, and verifies the checksum is stable across every run.
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
      echo "FAIL [$label]: unparseable output: $out" >&2; exit 1
    fi
    if [ -z "$CHECKSUM" ]; then
      CHECKSUM="$total"
    elif [ "$total" != "$CHECKSUM" ]; then
      echo "FAIL [$label]: checksum $total != $CHECKSUM — work was lost or duplicated" >&2
      exit 1
    fi
    [ "$i" -eq 0 ] && continue   # cold start
    ns_samples+=("$ns")
    wall_samples+=("$wall")
  done
  MED_NS="$(printf '%s\n' "${ns_samples[@]}" | sort -n | awk -v n="$RUNS" 'NR==int((n+1)/2)')"
  MED_WALL="$(printf '%s\n' "${wall_samples[@]}" | sort -n | awk -v n="$RUNS" 'NR==int((n+1)/2)')"
  # Spread of the warm runs as a percentage of the median. Above
  # NOISE_PCT the host is dominating that cell and the number should be
  # re-taken on a quiet box rather than published.
  SPREAD="$(printf '%s\n' "${ns_samples[@]}" | sort -n |
    awk -v m="$MED_NS" 'NR==1{lo=$1} {hi=$1} END{printf "%.1f", (hi-lo)*100/m}')"
  if awk -v s="$SPREAD" -v t="$NOISE_PCT" 'BEGIN{exit !(s>t)}'; then
    NOISY+=("$label ${SPREAD}%")
  fi
}

ms() { awk -v ns="$1" 'BEGIN{printf "%.1f", ns/1000000}'; }

table() {
  local header="$1"; shift
  local sep; sep="$(printf '%s\n' "$header" | sed 's/[^|]*/---/g')"
  printf '| %s |\n| %s |\n' "$header" "$sep"
  printf '| %s |\n' "$@"
}

for backend in $KAI_BACKENDS; do
  BIN="$TMP/mn_throughput.$backend"
  echo "building kaikai load (--release --backend=$backend)..."
  "$ROOT/bin/kai" build --release "--backend=$backend" "$HERE/mn_throughput.kai" -o "$BIN" >/dev/null

  HEADER=""
  ROWS=(); WROWS=(); SROWS=()

  for scenario in $SCENARIOS; do
    name="${scenario%%:*}"; rest="${scenario#*:}"
    workers="${rest%%:*}"; load="${rest#*:}"
    CHECKSUM=""
    row="\`$name\` (${workers} x ${load})"; wrow="$row"; srow="$row"; headers="scenario"

    for t in $KAI_THREAD_LIST; do
      echo "  $backend/$name: kaikai KAI_THREADS=$t"
      measure "kai-$backend@$t" env "KAI_THREADS=$t" "$BIN" "$workers" "$load"
      headers="$headers | kaikai@$t"; row="$row | $(ms "$MED_NS")"; wrow="$wrow | $MED_WALL"; srow="$srow | ${SPREAD}%"
    done

    if [ "$HAVE_BEAM" = 1 ]; then
      for s in $BEAM_SCHEDS; do
        echo "  $backend/$name: BEAM +S $s"
        if [ "$s" = default ]; then
          measure "beam@$s" elixir -pa "$EBIN" -e "MnThroughput.main($workers, $load)"
        else
          measure "beam@$s" elixir --erl "+S $s:$s" -pa "$EBIN" -e "MnThroughput.main($workers, $load)"
        fi
        headers="$headers | BEAM@$s"; row="$row | $(ms "$MED_NS")"; wrow="$wrow | $MED_WALL"; srow="$srow | ${SPREAD}%"
      done
    fi

    HEADER="$headers"
    ROWS+=("$row | $CHECKSUM"); WROWS+=("$wrow"); SROWS+=("$srow")
  done

  echo
  echo "### kaikai --backend=$backend — median wall-clock of the parallel region (ms, lower is better)"
  echo
  table "$HEADER | checksum" "${ROWS[@]}"
  echo
  echo "### kaikai --backend=$backend — spread of the warm runs (max-min as % of median)"
  echo
  table "$HEADER" "${SROWS[@]}"
  echo
  echo "### kaikai --backend=$backend — median total process wall (s), including VM startup"
  echo
  table "$HEADER" "${WROWS[@]}"
  echo
done

if [ "${#NOISY[@]}" -gt 0 ]; then
  echo "WARNING: ${#NOISY[@]} cell(s) spread past ${NOISE_PCT}% of the median — host noise"
  echo "dominates them; re-take on a quiet box before publishing those points:"
  printf '  %s\n' "${NOISY[@]}"
else
  echo "All cells within ${NOISE_PCT}% spread — medians are solid."
fi
