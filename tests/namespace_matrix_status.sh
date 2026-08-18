#!/usr/bin/env bash
# Reports how much of the namespace collision matrix currently passes.
#
# This is a REPORT, not a gate: the corpus is written against the model
# in docs/namespaces-design.md, so most cells are expected to fail until
# the implementation reaches them. It exits 0 whatever the count, and
# exists to make progress measurable — a step that closes a class should
# move this number, and a step that breaks a passing cell should show up
# here as a regression.
#
# A cell counts as passing only when EVERY fixture it names passes, so a
# deep cell is not carried by its easiest variant.
#
# Backend defaults to C; pass `native` to measure the other one, since a
# fix verified on one proves nothing about the other.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MATRIX="$ROOT/tests/namespace_matrix.tsv"
CORPUS="$ROOT/examples/namespace-collisions"
BACKEND="${1:-c}"

if [ "$BACKEND" = "native" ]; then
  RUN() { ( cd "$1" && KAI_BACKEND=native "$ROOT/bin/kai" run main.kai 2>"$2" ); }
  RUN_TEST() { ( cd "$1" && KAI_BACKEND=native "$ROOT/bin/kai" test main.kai 2>"$2" ); }
  RUN_CHECK() { ( cd "$1" && KAI_BACKEND=native "$ROOT/bin/kai" check main.kai 2>&1 ); }
else
  RUN() { ( cd "$1" && "$ROOT/bin/kai" run main.kai --backend=c 2>"$2" ); }
  RUN_TEST() { ( cd "$1" && "$ROOT/bin/kai" test main.kai --backend=c 2>"$2" ); }
  RUN_CHECK() { ( cd "$1" && "$ROOT/bin/kai" check main.kai --backend=c 2>&1 ); }
fi

err="$(mktemp)"
trap 'rm -f "$err"' EXIT

# A fixture may carry BOTH goldens: `main.err.expected` records the
# failure it exhibits today, which is what classifies it for the fmt
# sweep, while `main.out.expected` states the behaviour it must reach.
# The target is what counts here, so `.out` wins when both are present —
# such a fixture reads as failing until the defect is actually fixed,
# which is the point.
#
# `main.test.expected` names a fixture whose defect only shows under
# `kai test` — a collision between two blocks that are not reachable from
# `main` at all. It is matched as a substring of the run's tail, so the
# per-test lines above the summary do not have to be spelled out.
# `main.check.expected` does the same for `kai check`, where protocol law
# checks are generated per impl.
#
# A negative fixture may carry `main.err.expected` (line 1 is the needle)
# and `DIAG.expected` (every line is a needle) together; both must hold.
fixture_passes() {
  d="$CORPUS/$1"
  if [ -f "$d/main.test.expected" ]; then
    # `kai test` reports on stderr.
    RUN_TEST "$d" "$err" > /dev/null 2>&1 || true
    grep -qF "$(cat "$d/main.test.expected")" "$err"
  elif [ -f "$d/main.check.expected" ]; then
    RUN_CHECK "$d" > "$err" || true
    grep -qF "$(head -1 "$d/main.check.expected")" "$err"
  elif [ -f "$d/main.out.expected" ]; then
    [ "$(RUN "$d" /dev/null)" = "$(cat "$d/main.out.expected")" ]
  elif [ -f "$d/main.err.expected" ] || [ -f "$d/DIAG.expected" ]; then
    RUN "$d" "$err" > /dev/null 2>&1 || true
    if [ -f "$d/main.err.expected" ]; then
      grep -qF "$(head -1 "$d/main.err.expected")" "$err" || return 1
    fi
    if [ -f "$d/DIAG.expected" ]; then
      while IFS= read -r needle; do
        [ -z "$needle" ] && continue
        grep -qF "$needle" "$err" || return 1
      done < "$d/DIAG.expected"
    fi
  else
    return 1
  fi
}

pass=0
fail=0
na=0
declare -a FAILED=()

while IFS=$'\t' read -r class shape position polarity target _axes; do
  case "$class" in ''|'#'*) continue ;; esac
  case "$target" in N/A:*) na=$((na + 1)); continue ;; esac

  ok=1
  IFS=',' read -ra targets <<< "$target"
  for t in "${targets[@]}"; do
    t="$(echo "$t" | tr -d ' ')"
    [ -z "$t" ] && continue
    fixture_passes "$t" || ok=0
  done

  if [ "$ok" -eq 1 ]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    FAILED+=("$class/$shape/$position")
  fi
done < "$MATRIX"

total=$((pass + fail))
if [ "${VERBOSE:-0}" = "1" ] && [ "${#FAILED[@]}" -gt 0 ]; then
  printf '  %s\n' "${FAILED[@]}"
fi

echo "namespace_matrix_status [$BACKEND]: $pass/$total cells passing, $na not applicable"
