#!/bin/bash
# Differential independence oracle — proves core's typecheck is
# independent of the user file (#962's soundness gate).
#
# For batch-mode stdlib typecheck to be sound, the typed AST of core
# must be byte-identical whether it is typechecked alone or alongside
# an adversarial user file. Two user->core contamination paths were
# cut (root_fns isolation in infer.kai; lower_protocols proto-op filter
# in protos.kai). This oracle is the standing proof those cuts hold.
#
# Method: for each lane, typecheck `core ++ benign_user` and
# `core ++ adversary_user`, dumping ONLY core / imported decls
# (`--dump-typed-core`). The two user files differ by exactly one atom
# (the contaminating identifier). With the cuts in place the two core
# dumps are byte-identical (GREEN). Revert a cut and the adversary's
# core dump diverges — silently (Lane 1: a shifted `ty_tag`) or loudly
# (Lane 2: core is rejected, non-zero exit). Either is RED.
#
# RED rule (per the design): a lane is contaminated iff
#   (benign exit) != (adversary exit)
#   OR  (both exit 0 AND the two core dumps byte-differ).
# A non-zero adversary exit while benign stays 0 IS contamination — the
# user file broke core's typecheck. We do NOT normalize the two sides
# to a shared "failed" sentinel; that would hide the asymmetry that is
# itself the signal.
#
# Self-vs-self, no committed golden: the reference is the benign run of
# the same compiler, so a stdlib edit that legitimately changes core's
# types moves both sides together and the oracle stays green.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

KAIC2="${KAIC2:-$ROOT/stage2/kaic2}"
STDLIB="${STDLIB:-$ROOT/stdlib}"
ORACLE_DIR="${ORACLE_DIR:-$ROOT/examples/oracle}"
WORK="${WORK:-$(mktemp -d)}"

if [ ! -x "$KAIC2" ]; then
  echo "independence-oracle: compiler not found at $KAIC2" >&2
  echo "  build it first: make kaic2" >&2
  exit 2
fi

fail=0

# Dump core's typed AST for one user file. Extra --path args (for a
# lane's probe library) follow the file argument.
dump_core() {
  out="$1"; err="$2"; file="$3"; shift 3
  "$KAIC2" "$@" --path "$STDLIB" --dump-typed-core "$file" > "$out" 2> "$err"
  echo $?
}

# Run one lane: benign vs adversary, apply the RED rule.
run_lane() {
  lane="$1"; shift
  benign="$ORACLE_DIR/$lane/benign.kai"
  adversary="$ORACLE_DIR/$lane/adversary.kai"

  bo="$WORK/$lane-benign.typed"; be="$WORK/$lane-benign.err"
  ao="$WORK/$lane-adversary.typed"; ae="$WORK/$lane-adversary.err"

  bx=$(dump_core "$bo" "$be" "$benign" "$@")
  ax=$(dump_core "$ao" "$ae" "$adversary" "$@")

  if [ "$bx" != "$ax" ]; then
    echo "oracle RED $lane — exit asymmetry (benign=$bx adversary=$ax)"
    echo "  the adversary user file changed core's typecheck outcome."
    if [ "$bx" = "0" ]; then
      echo "  --- adversary typecheck errors (core contaminated) ---"
      sed 's/^/    /' "$ae" | head -20
    fi
    fail=1
    return
  fi

  if [ "$bx" != "0" ]; then
    echo "oracle SKIP $lane — both sides failed to typecheck (exit $bx); not a contamination signal"
    echo "  --- benign errors ---"
    sed 's/^/    /' "$be" | head -20
    fail=1
    return
  fi

  if diff -q "$bo" "$ao" > /dev/null; then
    echo "oracle OK $lane — core typed AST byte-identical with/without adversary ($(wc -l < "$bo" | tr -d ' ') lines)"
  else
    echo "oracle RED $lane — core typed AST diverged between benign and adversary"
    echo "  the adversary user file silently shifted core's resolved types."
    echo "  --- diff (benign vs adversary, core dump) ---"
    diff "$bo" "$ao" | head -40 | sed 's/^/    /'
    fail=1
  fi
}

# Lane 1 (root_fns): the probe library lives beside the fixtures; both
# user files import it, so its modules are imported (Some(mod)) and
# appear in the core dump.
run_lane lane1 --path "$ORACLE_DIR/lane1/lib"

# Lane 2 (lower_protocols): the collision is against core/string.kai's
# own bare `is_space` call; no extra library needed.
run_lane lane2

if [ "$fail" = "0" ]; then
  echo "independence-oracle OK — core typecheck is user-independent on every lane"
else
  echo "independence-oracle FAILED — a user file contaminated core's typecheck" >&2
fi
exit $fail
