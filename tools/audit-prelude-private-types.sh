#!/usr/bin/env bash
# Issue #643 — institutional regression gate for private-type leaks.
#
# For every non-`pub type` declaration in stdlib/, synthesize a
# minimal user program that redeclares that name as a sum type and
# verify it compiles cleanly. A failure means the typer's name
# resolution leaked the prelude's variants / fields into the user's
# scope (the symptom that blocked book chapter 5 pre-#643).
#
# Today's fix lands the arity-aware discriminator on the variant
# table; record-side collisions of identical arity are flagged in
# the lane retro as a follow-up. The audit therefore exercises the
# **sum-type** redeclaration path explicitly; record-shape names
# are skipped to keep the gate stable until the record-side fix
# lands.
#
# Wired into `stage2/Makefile` as `test-private-type-shadow-audit`,
# part of the tier-1 chain.
#
# Parallelism (lane tier1-perf): candidate discovery (grep over
# stdlib) is cheap and stays serial; the per-candidate `kai build`
# invocations are independent (each writes to per-candidate temp
# files keyed by the type name) so they fan out over `xargs -P$JOBS`.
# A worker mode (`__probe <tmpdir> <tname> <srcfile>`) builds one
# candidate and prints `PASS`/`SKIP`/`FAIL ...`; the summary is
# recomputed from the collected lines. KAI_TEST_JOBS=1 → serial.

set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAI="$ROOT/bin/kai"

if [ ! -x "$KAI" ]; then
  echo "audit-prelude-private-types: bin/kai not executable" >&2
  exit 2
fi

# probe_one: synthesize + compile the sum-type redeclaration for one
# private type name. Prints exactly one of PASS / SKIP / FAIL ....
probe_one() {
  tmp="$1"
  tname="$2"
  f="$3"

  # Skip reserved builtin type names — the typer rejects user
  # redeclarations of these on a different code path
  # (`reserved_builtin_type_names`) and the test would legitimately
  # fail.
  case "$tname" in
    Cont) echo "SKIP"; return ;;
  esac

  cat > "$tmp/test_${tname}.kai" <<EOF
type ${tname}
  = LocalA${tname}
  | LocalB${tname}

fn classify_${tname}(x: ${tname}) : Int = match x {
  LocalA${tname} -> 1
  LocalB${tname} -> 2
}

fn main() : Unit / Stdout = {
  println("#{int_to_string(classify_${tname}(LocalA${tname}))}")
}
EOF

  if "$KAI" build "$tmp/test_${tname}.kai" -o "$tmp/test_${tname}.bin" \
      > "$tmp/test_${tname}.log" 2>&1; then
    echo "PASS"
  else
    echo "FAIL: redeclaring private '${tname}' (from ${f}) is rejected"
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
self="$ROOT/tools/audit-prelude-private-types.sh"

# Phase 1 (serial): discover candidates. Each candidate is a
# `<tname>\t<srcfile>` line. Pick `^type <Name>` lines (private decls);
# `^pub type` is intentionally NOT matched.
worklist="$tmp/worklist"
: > "$worklist"
while IFS= read -r -d '' f; do
  while IFS= read -r line; do
    tname="$(echo "$line" | sed -E 's/^type[[:space:]]+([A-Za-z_][A-Za-z0-9_]*).*/\1/')"
    if [ -z "$tname" ] || ! [[ "$tname" =~ ^[A-Z] ]]; then
      continue
    fi
    printf '%s\t%s\n' "$tname" "$f" >> "$worklist"
  done < <(grep -aE '^type [A-Z]' "$f" | grep -v '^pub ' || true)
done < <(find stdlib -name '*.kai' -print0)

results="$tmp/results"
# Phase 2 (parallel): build each candidate.
if [ -s "$worklist" ]; then
  while IFS="$(printf '\t')" read -r tname f; do
    printf '%s\0%s\0' "$tname" "$f"
  done < "$worklist" \
    | xargs -0 -P "$JOBS" -n2 "$self" __probe "$tmp" \
    > "$results"
else
  : > "$results"
fi

# Surface FAIL diagnostics on stderr (preserves old behaviour).
grep -E '^FAIL|^  ' "$results" >&2 || true

pass=$(grep -c '^PASS$' "$results" 2>/dev/null || true)
skip=$(grep -c '^SKIP$' "$results" 2>/dev/null || true)
fail=$(grep -c '^FAIL' "$results" 2>/dev/null || true)

echo "audit-prelude-private-types: pass=$pass fail=$fail skip=$skip"
[ "$fail" -eq 0 ]
