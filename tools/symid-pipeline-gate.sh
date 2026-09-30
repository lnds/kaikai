#!/bin/sh
# Identity reaches the passes below monomorphisation.
#
# Unbox and Perceus run above the erasure, so a resolved reference still
# carries its `SymId` there, and the KIR lowering hands the effect's id to
# `KPerform`. The failure this prevents is silent: a pass that drops the
# id leaves a name that still resolves to something, so every tier stays
# green. Only a dump of the ids themselves fails.
#
# Each line decodes the id against the symbol table, so an id that
# survives but names another declaration fails as surely as a lost one.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FIX="$ROOT/examples/effects/perform_carries_effect_id"
out=$("$ROOT/stage2/kaic2" --edition "$(cat "$ROOT/EDITION")" --dump-symids \
        --path "$FIX" --path "$ROOT/stdlib" "$FIX/main.kai")

fail() {
  echo "symid-pipeline FAIL — $1" >&2
  printf '%s\n' "$out" | sed 's/^/  /' >&2
  exit 1
}

need() {
  printf '%s\n' "$out" | grep -qE "$1" || fail "no line matching: $1"
}

need '^unbox send shout#[0-9]+ shout@sink$'
need '^unbox send Sink\.put#[0-9]+ Sink@sink$'
need '^perceus send shout#[0-9]+ shout@sink$'
need '^perceus send Sink\.put#[0-9]+ Sink@sink$'
need '^kperform sink__send Sink\.put#[0-9]+ Sink@sink$'

id_of() {
  printf '%s\n' "$out" | grep -E "$1" | sed -E 's/.*#([0-9]+) .*/\1/'
}

[ "$(id_of '^perceus send Sink\.put#')" = "$(id_of '^kperform sink__send Sink\.put#')" ] \
  || fail "KPerform carries a different effect id than Perceus read"

# A root call reaches the root declaration even when a core homonym
# exists, and a name the root does not declare still reaches the core.
ROOTFIX="$ROOT/examples/namespace-collisions/root_fn_shadows_core_fn"
out=$("$ROOT/stage2/kaic2" --edition "$(cat "$ROOT/EDITION")" --dump-symids \
        --path "$ROOTFIX" --path "$ROOT/stdlib" "$ROOTFIX/main.kai")

need '^perceus helper add#[0-9]+ add@main$'
need '^perceus helper conj#[0-9]+ conj@main$'
need '^perceus helper uniq_name_x#[0-9]+ uniq_name_x@main$'
need '^perceus main helper#[0-9]+ helper@main$'
need '^perceus main mk#[0-9]+ mk@complex$'
need '^perceus main from_real#[0-9]+ from_real@complex$'

# A specialisation is its generic's id plus an instance index: the call
# sites and the registry name the same pair, and the id-keyed registry
# lookup reports each spec's own signature class, not the generic's.
SPECFIX="$ROOT/examples/type-identity/spec_instance_ids.kai"
out=$("$ROOT/stage2/kaic2" --edition "$(cat "$ROOT/EDITION")" --dump-symids \
        --path "$ROOT/stdlib" "$SPECFIX")

need '^perceus main pick__mono__Int/0#[0-9]+ pick@spec_instance_ids$'
need '^perceus main pick__mono__String/1#[0-9]+ pick@spec_instance_ids$'
need '^efn pick#[0-9]+ pick@spec_instance_ids boxed$'
need '^efn pick__mono__Int/0#[0-9]+ pick@spec_instance_ids raw=111>1$'
need '^efn pick__mono__String/1#[0-9]+ pick@spec_instance_ids raw=001>0$'

[ "$(id_of '^efn pick#')" = "$(id_of '^efn pick__mono__Int/0#')" ] \
  && [ "$(id_of '^efn pick#')" = "$(id_of '^efn pick__mono__String/1#')" ] \
  && [ "$(id_of '^efn pick#')" = "$(id_of '^perceus main pick__mono__Int/0#')" ] \
  || fail "a specialisation carries an id other than its generic's"

echo "symid-pipeline OK — unbox and perceus read the resolved ids, KPerform carries the effect's, root calls reach root declarations, and each specialisation is its generic's id plus its own instance"
