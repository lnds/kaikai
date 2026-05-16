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

set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAI="$ROOT/bin/kai"

if [ ! -x "$KAI" ]; then
  echo "audit-prelude-private-records: bin/kai not executable" >&2
  exit 2
fi

tmp=$(mktemp -d)
trap "rm -rf $tmp" EXIT INT TERM

fail=0
pass=0
skip=0

# Iterate every `.kai` file under stdlib/. The grep picks
# `^type <Name>` lines followed by `{` (record body); `^pub type`
# is intentionally NOT matched (those are caught earlier by
# `validate_type_name_collisions_decls`).
while IFS= read -r -d '' f; do
  while IFS= read -r line; do
    tname="$(echo "$line" | sed -E 's/^type[[:space:]]+([A-Za-z_][A-Za-z0-9_]*).*/\1/')"
    if [ -z "$tname" ] || ! [[ "$tname" =~ ^[A-Z] ]]; then
      continue
    fi

    # Skip reserved builtin type names — the typer rejects user
    # redeclarations of these on a different code path.
    case "$tname" in
      Cont) skip=$((skip + 1)); continue ;;
    esac

    # Synthesize a minimal user program that redeclares `tname` as
    # a record with two distinct fields the prelude's declaration
    # does not use (`user_field_a_<tname>`, `user_field_b_<tname>`).
    # The field-set tie-breaker (`rec_find_with_field`, issue #648)
    # is the load-bearing mechanism: the user's `t.user_field_a_*`
    # access must resolve to the user's record, and the prelude's
    # own accesses to its private fields must keep resolving to
    # the prelude's record. If either side flips, the audit fails.
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
      pass=$((pass + 1))
    else
      echo "FAIL: redeclaring private record '${tname}' (from ${f}) is rejected" >&2
      sed 's/^/  /' "$tmp/test_${tname}.log" >&2
      fail=$((fail + 1))
    fi
  done < <(grep -aE '^type [A-Z][A-Za-z0-9_]* = \{' "$f" | grep -v '^pub ')
done < <(find stdlib -name '*.kai' -print0)

echo "audit-prelude-private-records: pass=$pass fail=$fail skip=$skip"

[ "$fail" -eq 0 ]
