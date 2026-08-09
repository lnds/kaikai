#!/bin/bash
# Effects golden gate — runtime stdout vs the .out.expected oracle.
#
# Backend parity diffs the native backend against the C oracle, so a
# defect BOTH backends share is invisible to it: they agree, parity is
# green, and nothing compares the program's actual output against the
# fixture's golden. This harness closes that gap for examples/effects:
# every flat fixture with a golden is built under TARGET_BACKEND, run,
# and its stdout must match the golden byte for byte.
#
# A fixture's contract is declared by sidecars next to its source:
#   <name>.out.expected         stdout golden, exit status 0 (the default)
#   <name>.out.sorted.expected  stdout golden compared as LC_ALL=C-sorted
#                               lines — for fixtures whose line SET is
#                               stable but whose interleaving is
#                               scheduler-dependent
#   <name>.status.expected      expected exit status when not 0 — asserted
#                               exactly, so a trap fixture that stops
#                               trapping fails the gate
#   <name>.driver               external driver, e.g. `sigharness` (built
#                               from tests/sigharness.c): waits for the
#                               "ready" line, sends the signal, forwards
#                               the child's stdout and exit status
#
# bash (not sh): the xargs worker fan-out uses `export -f`, which dash
# rejects.
#
# Skip discipline mirrors tools/backend-parity-skips.txt:
# tools/effects-goldens-skips.txt, `<relative-path>:#<issue>:<reason>`.
# File the issue first; the skip is the bookmark, the issue is the work.
# A malformed entry (no issue number, or no matching fixture) fails the
# run, and so does a stale one — listed but passing — which is what
# forces the list to empty instead of rotting.

set -eu

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAI="$ROOT/bin/kai"

# kai_corpus_pinned_threads: fixtures whose documented property IS the
# one-thread cooperative rotation pin KAI_THREADS=1 in a sibling .env;
# every corpus harness runs them at the pin.
. "$ROOT/tools/lib/corpus.sh"

TARGET_BACKEND="${TARGET_BACKEND:-c}"
SKIPS="$ROOT/tools/effects-goldens-skips.txt"
FIXDIR="examples/effects"
RUN_TIMEOUT="${EFFECTS_GOLDENS_RUN_TIMEOUT:-60}"

# A C-only kaic2 rejects the native backend; SKIP (success) so the
# native gate runs only where libLLVM is present (tier1-native).
probe_native_or_skip() {
  local probe
  probe="$(mktemp)"; mv "$probe" "$probe.kai"; probe="$probe.kai"
  printf 'fn main() : Int = 0\n' > "$probe"
  if ! KAI_BACKEND=native "$KAI" build "$probe" -o "${probe%.kai}.bin" >/dev/null 2>&1; then
    echo "test-effects-goldens: SKIP (native backend unavailable; rebuild kaic2 with make KAI_LLVM=1)"
    rm -f "$probe" "${probe%.kai}.bin"
    exit 0
  fi
  rm -f "$probe" "${probe%.kai}.bin"
}

detect_jobs() {
  if [ -n "${EFFECTS_GOLDENS_JOBS:-}" ]; then echo "$EFFECTS_GOLDENS_JOBS"
  elif command -v nproc >/dev/null 2>&1; then nproc
  elif command -v sysctl >/dev/null 2>&1; then sysctl -n hw.logicalcpu 2>/dev/null || echo 4
  else echo 4
  fi
}

detect_timeout_cmd() {
  if command -v timeout >/dev/null 2>&1; then echo "timeout"
  elif command -v gtimeout >/dev/null 2>&1; then echo "gtimeout"
  fi
}

run_with_timeout() {
  if [ -n "$TIMEOUT_CMD" ]; then "$TIMEOUT_CMD" "$RUN_TIMEOUT" "$@"; else "$@"; fi
}

# Appends the failure block (header line + detail file excerpt) and
# records the F verdict. Single-line appends up to PIPE_BUF are atomic,
# and the bounded excerpts fit.
report_fail() {
  { echo "FAIL $1"; sed 's/^/    /' < "$2"; echo; } >> "$failures"
  printf 'F\n' >> "$results"
}

# Runs one fixture against its declared contract. On the first breach,
# writes the detail excerpt to $2 and echoes the failure header; silent
# and empty-detail on success.
check_contract() {
  local f="$1" detail="$2" base="${1%.kai}" slug golden sorted want_rc bin out rc
  slug=$(echo "$f" | tr '/.' '__')
  bin="$tmp/${slug}.bin"
  out="$tmp/${slug}.out"

  if [ -f "$base.out.sorted.expected" ]; then
    golden="$base.out.sorted.expected"; sorted=1
  else
    golden="$base.out.expected"; sorted=0
  fi
  want_rc=0
  [ -f "$base.status.expected" ] && want_rc="$(tr -d '[:space:]' < "$base.status.expected")"

  if ! KAI_BACKEND="$TARGET_BACKEND" KAI_NATIVE_MODULAR=0 "$KAI" build "$f" -o "$bin" >"$tmp/${slug}.build.log" 2>&1; then
    tail -10 "$tmp/${slug}.build.log" > "$detail"
    echo "$f — $TARGET_BACKEND build failed:"
    return 0
  fi

  local driver=""
  if [ -f "$base.driver" ]; then
    driver="$(head -1 "$base.driver")"
    case "$driver" in
      sigharness|sigharness\ *)
        driver="$SIGHARNESS_BIN ${driver#sigharness}" ;;
      *)
        echo "unknown driver '$driver' (tools/test-effects-goldens.sh knows: sigharness)" > "$detail"
        echo "$f — bad .driver sidecar:"
        return 0 ;;
    esac
  fi

  local pin
  pin="$(kai_corpus_pinned_threads "$f")" || pin=""
  rc=0
  # $driver word-splits on purpose: it carries optional args (--sig …).
  run_with_timeout env ${pin:+KAI_THREADS="$pin"} KAI_TRACE_PARK=1 $driver "$bin" \
    >"$out" 2>"$tmp/${slug}.err" </dev/null || rc=$?
  if [ "$rc" != "$want_rc" ]; then
    # Deep enough to carry the whole park/wake ring the deadlock banner dumps.
    tail -80 "$tmp/${slug}.err" > "$detail"
    echo "$f — exit status $rc (want $want_rc); stderr tail:"
    return 0
  fi

  local proj=""
  if [ "$sorted" = 1 ]; then
    LC_ALL=C sort "$out" > "$out.sorted"
    out="$out.sorted"
    proj=" (sorted-lines projection)"
  fi
  # cmp, not line-oriented diff: the byte that motivated this gate was
  # a trailing newline, which diff-family tools are prone to forgiving.
  if ! cmp -s "$golden" "$out"; then
    diff "$golden" "$out" | head -10 > "$detail" || true
    echo "$f — stdout differs from golden$proj:"
    return 0
  fi
  : > "$detail"
}

process_one() {
  local f="$1" slug listed=0 verdict
  slug=$(echo "$f" | tr '/.' '__')
  grep -q "^${f}:" "$SKIPS" 2>/dev/null && listed=1

  verdict="$(check_contract "$f" "$tmp/${slug}.detail")"
  if [ -z "$verdict" ]; then
    if [ "$listed" = 1 ]; then
      echo "skip entry passes on $TARGET_BACKEND; remove it from tools/effects-goldens-skips.txt" > "$tmp/${slug}.detail"
      report_fail "$f — STALE skip:" "$tmp/${slug}.detail"
    else
      printf 'P\n' >> "$results"
    fi
  else
    if [ "$listed" = 1 ]; then
      printf 'S\n' >> "$results"
    else
      report_fail "$verdict" "$tmp/${slug}.detail"
    fi
  fi
}

# Every skip entry must carry its owning issue and name a fixture this
# harness actually walks; anything else fails the run so the list can
# only shrink.
validate_skips() {
  local line path bad=0
  [ -f "$SKIPS" ] || return 0
  while IFS= read -r line; do
    case "$line" in ''|'#'*) continue ;; esac
    path="${line%%:*}"
    case "${line#*:}" in
      '#'[0-9]*:?*) ;;
      *) echo "test-effects-goldens FAIL — malformed skip entry (want <path>:#<issue>:<reason>): $line"; bad=1; continue ;;
    esac
    if ! grep -Fqx "$path" "$tmp/fixtures"; then
      echo "test-effects-goldens FAIL — skip entry matches no golden fixture: $path"
      bad=1
    fi
  done < "$SKIPS"
  return "$bad"
}

[ "$TARGET_BACKEND" = native ] && probe_native_or_skip
if [ ! -x "$KAI" ]; then
  echo "test-effects-goldens FAIL — bin/kai not found or not executable"
  exit 1
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM
TIMEOUT_CMD="$(detect_timeout_cmd)"
failures="$tmp/failures.log"
results="$tmp/results"
: > "$failures"
: > "$results"

SIGHARNESS_BIN="$tmp/sigharness"
if ls "$FIXDIR"/*.driver >/dev/null 2>&1; then
  ${CC:-cc} -O1 tests/sigharness.c -o "$SIGHARNESS_BIN" || {
    echo "test-effects-goldens FAIL — could not build tests/sigharness.c"
    exit 1
  }
fi

export -f process_one check_contract run_with_timeout report_fail kai_corpus_pinned_threads
export tmp results failures SKIPS KAI TARGET_BACKEND TIMEOUT_CMD RUN_TIMEOUT SIGHARNESS_BIN

for g in "$FIXDIR"/*.out.expected "$FIXDIR"/*.out.sorted.expected; do
  src="${g%.out.expected}"; src="${src%.out.sorted.expected}.kai"
  [ -f "$src" ] && echo "$src"
done | sort -u > "$tmp/fixtures"

if [ ! -s "$tmp/fixtures" ]; then
  echo "test-effects-goldens FAIL — no fixtures found under $FIXDIR"
  exit 1
fi

validate_skips

xargs -P "$(detect_jobs)" -n 1 bash -c 'process_one "$1"' _ < "$tmp/fixtures"

pass=$(grep -c '^P$' "$results" || true)
fail=$(grep -c '^F$' "$results" || true)
skip=$(grep -c '^S$' "$results" || true)

cat "$failures"
echo "test-effects-goldens ($TARGET_BACKEND): $pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
