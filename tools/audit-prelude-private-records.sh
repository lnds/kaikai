#!/usr/bin/env bash
# Issue #648 — institutional regression gate for the record-side
# private-type leak.
#
# For every non-`pub type X = { ... }` record declaration in stdlib/,
# synthesize a minimal user program that redeclares `X` as a record
# with a non-overlapping field set, and verify it compiles cleanly.
# A failure means the typer's record-table walker (rec_find /
# rec_find_with_field) leaked the prelude's field set into the
# user's scope (the symptom #648 closes).
#
# Companion to `audit-prelude-private-types.sh` (sum-type side,
# issue #643). The two scripts iterate the same set of private
# declarations and run them through analogous shapes; keeping them
# as siblings keeps the diagnostic output discoverable per shape
# (sum vs record) when one regresses without the other.
#
# Wired into top-level `Makefile` as `test-private-record-shadow-audit`,
# part of the tier-1 chain.
#
# Parallelism (lane tier1-perf): see audit-prelude-private-types.sh —
# same discover-then-fan-out structure. KAI_TEST_JOBS=1 → serial.

set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAI="$ROOT/bin/kai"

if [ ! -x "$KAI" ]; then
  echo "audit-prelude-private-records: bin/kai not executable" >&2
  exit 2
fi

# probe_one: synthesize + compile the record redeclaration for one
# private type name. Prints exactly one of PASS / SKIP / FAIL ....
probe_one() {
  tmp="$1"
  tname="$2"
  f="$3"

  case "$tname" in
    Cont) echo "SKIP"; return ;;
  esac

  cat > "$tmp/test_${tname}.kai" <<EOF
type ${tname} = { user_field_a_${tname}: Int, user_field_b_${tname}: String }

fn make_${tname}(a: Int, b: String) : ${tname} =
  ${tname} { user_field_a_${tname}: a, user_field_b_${tname}: b }

fn relabel_${tname}(t: ${tname}, suffix: String) : ${tname} =
  ${tname} {
    user_field_a_${tname}: t.user_field_a_${tname},
    user_field_b_${tname}: string_concat(t.user_field_b_${tname}, suffix)
  }

fn main() : Unit / Stdout = {
  let t = make_${tname}(7, "hello")
  let r = relabel_${tname}(t, "-x")
  println("#{int_to_string(r.user_field_a_${tname})}/#{r.user_field_b_${tname}}")
}
EOF

  if "$KAI" build "$tmp/test_${tname}.kai" -o "$tmp/test_${tname}.bin" \
      > "$tmp/test_${tname}.log" 2>&1; then
    echo "PASS"
  else
    echo "FAIL: redeclaring private record '${tname}' (from ${f}) is rejected"
    sed 's/^/  /' "$tmp/test_${tname}.log"
  fi
}

# ---- worker mode -----------------------------------------------------
if [ "${1:-}" = "__probe" ]; then
  probe_one "$2" "$3" "$4"
  exit 0
fi

# ---- orchestrator ----------------------------------------------------
tmp=$(mktemp -d)
trap "rm -rf $tmp" EXIT INT TERM

JOBS="${KAI_TEST_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"
self="$ROOT/tools/audit-prelude-private-records.sh"

# Phase 1 (serial): discover candidates — `^type <Name> = {` lines
# (private record decls); `^pub type` intentionally NOT matched.
worklist="$tmp/worklist"
: > "$worklist"
while IFS= read -r -d '' f; do
  while IFS= read -r line; do
    tname="$(echo "$line" | sed -E 's/^type[[:space:]]+([A-Za-z_][A-Za-z0-9_]*).*/\1/')"
    if [ -z "$tname" ] || ! [[ "$tname" =~ ^[A-Z] ]]; then
      continue
    fi
    printf '%s\t%s\n' "$tname" "$f" >> "$worklist"
  done < <(grep -aE '^type [A-Z][A-Za-z0-9_]* = \{' "$f" | grep -v '^pub ' || true)
done < <(find stdlib -name '*.kai' -print0)

results="$tmp/results"
if [ -s "$worklist" ]; then
  while IFS="$(printf '\t')" read -r tname f; do
    printf '%s\0%s\0' "$tname" "$f"
  done < "$worklist" \
    | xargs -0 -P "$JOBS" -n2 "$self" __probe "$tmp" \
    > "$results"
else
  : > "$results"
fi

grep -E '^FAIL|^  ' "$results" >&2 || true

pass=$(grep -c '^PASS$' "$results" 2>/dev/null || true)
skip=$(grep -c '^SKIP$' "$results" 2>/dev/null || true)
fail=$(grep -c '^FAIL' "$results" 2>/dev/null || true)

echo "audit-prelude-private-records: pass=$pass fail=$fail skip=$skip"
[ "$fail" -eq 0 ]
