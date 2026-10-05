#!/bin/sh
# Check-vs-build diagnostic-identity gate.
#
# The contract of `kai typecheck` (kaic2 --check): for any program a
# build rejects at the front-end, --check rejects it with the SAME
# exit code and byte-identical stderr. This script proves it over the
# whole compile-time negative corpus: for every fixture under
# examples/negative/** it runs kaic2 twice — build mode and --check —
# and diffs the two stderr streams. The --check run is `--diags-json`
# (the same front-end, plus the JSON document on stdout carrying the
# exit status), so the same run also feeds tools/diags-json-parity.py:
# the errors the document reports must be the ones stderr prints.
# The programs in ACCEPTS build cleanly, so their --diags-json run must
# print no error and report status 0.
#
# Fixtures whose error only surfaces in a backend (subset gaps) are
# out of --check's scope by design: build rejects, --check accepts. Those live in
# tools/check-parity-skips.txt (one relpath per line, reason after
# whitespace). A skip is ratcheted: if --check starts rejecting a
# skipped fixture, the entry is stale and the gate fails so the list
# shrinks with the compiler.
#
# Per-fixture output:
#   PASS <fixture>       — both reject, identical stderr + exit code.
#   SKIP <fixture>       — later-phase rejection, listed and still true.
#   DIFF <fixture>       — stderr or exit code differ (contract broken).
#   STALE <fixture>      — skip entry no longer matches reality.
#
# Runtime-negative (.run.err.expected) and stage1 (.kaic1.err.expected)
# goldens are out of scope; silent_contract/ documents open gaps.
# Parallelism mirrors tools/test-negative.sh (xargs -P worker mode).

set -eu

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

KAIC2="$ROOT/stage2/kaic2"

# A flagless kaic2 runs the oldest edition; the repo's EDITION is what
# this gate must exercise.
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"
SKIPS="$ROOT/tools/check-parity-skips.txt"
ACCEPTS="
examples/actors/dual_actor_request_reply.kai
examples/diags_json/main_stdout_row.kai
examples/effects/alias_inline_row.kai
examples/effects/default_block_full_user_handle.kai
examples/modules/export_derived_record/main.kai
examples/protocols/default_basic.kai
examples/protocols/free_fn_bound_satisfied.kai
"

if [ ! -x "$KAIC2" ]; then
  echo "test-check-parity FAIL — stage2/kaic2 not built (run 'make kaic2' first)"
  exit 1
fi

skip_reason() {
  [ -f "$SKIPS" ] || return 1
  awk -v p="$1" '$1 == p { $1 = ""; sub(/^[ \t]+/, ""); print; found = 1 } END { exit !found }' "$SKIPS"
}

run_one() {
  exp="$1"
  tmp="$2"

  base=$(basename "$exp")
  dir=$(dirname "$exp")

  case "$base" in
    *.run.err.expected|*.kaic1.err.expected) return ;;
    main.err.expected)  stem="main" ;;
    *.diag.expected)    stem="${base%.diag.expected}" ;;
    *.err.expected)     stem="${base%.err.expected}" ;;
    *) return ;;
  esac
  src="$dir/$stem.kai"
  [ -f "$src" ] || return

  extra_flags=""
  [ -f "$dir/$stem.flags" ] && extra_flags=$(cat "$dir/$stem.flags")

  rel="${src#$ROOT/}"
  key=$(echo "$rel" | tr '/' '_')

  rc_build=0
  # shellcheck disable=SC2086 — extra_flags is intentionally word-split.
  "$KAIC2" $EDITION_FLAG $extra_flags "$src" > /dev/null 2> "$tmp/$key.build.err" || rc_build=$?
  rc_dj=0
  # shellcheck disable=SC2086
  "$KAIC2" $EDITION_FLAG $extra_flags --diags-json "$src" > "$tmp/$key.dj" 2> "$tmp/$key.check.err" || rc_dj=$?
  if [ "$rc_dj" -ne 0 ]; then
    echo "DIFF $rel — --diags-json exit $rc_dj (expected a document and exit 0)"
    return
  fi
  rc_check=$(grep -o '"status": [0-9]*' "$tmp/$key.dj" | head -1 | cut -d' ' -f2)
  if [ -z "$rc_check" ]; then
    echo "DIFF $rel — --diags-json printed no document"
    return
  fi
  echo "DJ $rel $tmp/$key.check.err $tmp/$key.dj"

  if reason=$(skip_reason "$rel"); then
    if [ "$rc_build" -ne 0 ] && [ "$rc_check" -eq 0 ]; then
      echo "SKIP $rel — $reason"
    else
      echo "STALE $rel — skip listed but build_rc=$rc_build check_rc=$rc_check; remove or fix the entry"
    fi
    return
  fi

  if [ "$rc_build" -ne "$rc_check" ]; then
    echo "DIFF $rel — exit codes differ (build=$rc_build check=$rc_check)"
    return
  fi
  if ! cmp -s "$tmp/$key.build.err" "$tmp/$key.check.err"; then
    echo "DIFF $rel — stderr differs"
    diff "$tmp/$key.build.err" "$tmp/$key.check.err" | head -6 | sed 's/^/  /'
    return
  fi
  echo "PASS $rel"
}

# An accepting program: --diags-json prints no error and reports status 0.
accept_one() {
  rel="$1"
  key="$2/accept_$(echo "$rel" | tr '/' '_')"
  # shellcheck disable=SC2086 — EDITION_FLAG is intentionally word-split.
  "$KAIC2" $EDITION_FLAG --path "$ROOT/stdlib" --diags-json "$ROOT/$rel" > "$key.dj" 2> "$key.err" || true
  errs=$(grep -c 'error: ' "$key.err" || true)
  status=$(grep -o '"status": [0-9]*' "$key.dj" | cut -d' ' -f2)
  if [ "$errs" = "0" ] && [ "$status" = "0" ]; then echo "ACCEPT $rel"
  else echo "REJECT $rel — --diags-json printed $errs error(s), status ${status:-none}"; fi
}

# ---- worker mode -----------------------------------------------------
if [ "${1:-}" = "__worker" ]; then
  run_one "$3" "$2"
  exit 0
fi
if [ "${1:-}" = "__accept" ]; then
  accept_one "$3" "$2"
  exit 0
fi

# ---- orchestrator ----------------------------------------------------
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM

JOBS="${KAI_TEST_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"

self="$ROOT/tools/test-check-parity.sh"
results="$tmp/results"

find "$ROOT/examples/negative" \
  \( -name '*.err.expected' -o -name '*.diag.expected' \) \
  -not -path '*/silent_contract/*' -print0 2>/dev/null \
  | sort -z \
  | xargs -0 -P "$JOBS" -n1 "$self" __worker "$tmp" \
  > "$results"

# shellcheck disable=SC2086 — ACCEPTS is intentionally word-split.
printf '%s\n' $ACCEPTS | xargs -P "$JOBS" -n1 "$self" __accept "$tmp" >> "$results"

grep -v '^DJ ' "$results" || true

pass=$(grep -c '^PASS ' "$results" 2>/dev/null || true)
skip=$(grep -c '^SKIP ' "$results" 2>/dev/null || true)
diffs=$(grep -c '^DIFF ' "$results" 2>/dev/null || true)
stale=$(grep -c '^STALE ' "$results" 2>/dev/null || true)
rejects=$(grep -c '^REJECT ' "$results" 2>/dev/null || true)

echo
accepts=$(grep -c '^ACCEPT ' "$results" 2>/dev/null || true)
echo "test-check-parity summary: $pass PASS, $skip SKIP, $diffs DIFF, $stale STALE, $accepts ACCEPT, $rejects REJECT"

dj=0
grep '^DJ ' "$results" | cut -d' ' -f2- > "$tmp/dj-manifest"
python3 "$ROOT/tools/diags-json-parity.py" "$tmp/dj-manifest" "$ROOT/tools/diags-json-parity-baseline.txt" || dj=1

if [ "$diffs" -ne 0 ] || [ "$stale" -ne 0 ] || [ "$dj" -ne 0 ] || [ "$rejects" -ne 0 ]; then
  exit 1
fi
