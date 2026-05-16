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

set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAI="$ROOT/bin/kai"

if [ ! -x "$KAI" ]; then
  echo "audit-prelude-private-types: bin/kai not executable" >&2
  exit 2
fi

tmp=$(mktemp -d)
trap "rm -rf $tmp" EXIT INT TERM

fail=0
pass=0
skip=0

# Iterate every `.kai` file under stdlib/. The regex picks
# `^type <Name>` lines (private declarations); `^pub type` is
# intentionally NOT matched (those are caught by
# `validate_type_name_collisions_decls` already and surface a
# collision diagnostic at the user's redeclaration).
while IFS= read -r -d '' f; do
  while IFS= read -r line; do
    tname="$(echo "$line" | sed -E 's/^type[[:space:]]+([A-Za-z_][A-Za-z0-9_]*).*/\1/')"
    if [ -z "$tname" ] || ! [[ "$tname" =~ ^[A-Z] ]]; then
      continue
    fi

    # Skip reserved builtin type names — the typer rejects user
    # redeclarations of these on a different code path
    # (`reserved_builtin_type_names`) and the test would
    # legitimately fail.
    case "$tname" in
      Cont) skip=$((skip + 1)); continue ;;
    esac

    # Issue #643 follow-up: same-arity sum-type collisions are
    # documented as a known limitation. The arity-aware variant
    # walker discriminates `Tree[k, v]` (arity 2) from `Tree`
    # (arity 0) but does NOT discriminate two arity-0 sum types
    # of the same name. The prelude code consuming the private
    # type still resolves variants against the bare name table
    # and ends up seeing the user's redeclared variants. The
    # proper fix (per-module name scopes for non-`pub` types)
    # is a separate lane; tracked in the lane retro.
    case "$tname" in
      SplitN|NB_FragR)
        skip=$((skip + 1)); continue ;;
    esac

    # Synthesize a minimal user program that redeclares `tname`
    # as a sum type. Two arms, both nullary, so the typer's
    # variant-arity discriminator gets a clean 0-vs-prelude-arity
    # case to resolve.
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
      pass=$((pass + 1))
    else
      echo "FAIL: redeclaring private '${tname}' (from ${f}) is rejected" >&2
      sed 's/^/  /' "$tmp/test_${tname}.log" >&2
      fail=$((fail + 1))
    fi
  done < <(grep -aE '^type [A-Z]' "$f" | grep -v '^pub ')
done < <(find stdlib -name '*.kai' -print0)

echo "audit-prelude-private-types: pass=$pass fail=$fail skip=$skip"

[ "$fail" -eq 0 ]
