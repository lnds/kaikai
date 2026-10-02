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

# An op written on a row alias carries the identity of the component that
# declares it, same as one written on the effect directly. The alias is an
# `NCType` and declares no ops, so settling it is the resolver's job: a
# miss leaves the perform with `sym_none()` while the program still runs,
# which only an id dump catches.
ALIASFIX="$ROOT/examples/effects/perform_through_row_alias"
out=$("$ROOT/stage2/kaic2" --edition "$(cat "$ROOT/EDITION")" --dump-symids \
        --path "$ALIASFIX" --path "$ROOT/stdlib" "$ALIASFIX/main.kai")

need '^kperform spoken Loud\.say#[0-9]+ Loud@main$'
[ "$(printf '%s\n' "$out" | grep -cE '^kperform spoken Loud\.say#[0-9]+ Loud@main$')" = 2 ] \
  || fail "the alias-written and directly written performs do not both carry Loud's id"
[ "$(printf '%s\n' "$out" | grep -E '^kperform spoken ' | sed -E 's/.*#([0-9-]+) .*/\1/' | sort -u | wc -l | tr -d ' ')" = 1 ] \
  || fail "the two performs of Loud.say carry different ids"

# A capability passed as a value carries the same identity: the parameter's
# annotation is a `TyCon` naming the effect, so the op dispatched against
# that binder's evidence node must name the declaration the others do.
need '^kperform through_cap Loud\.say#l#[0-9]+ Loud@main$'
# A named instance used inside its handle body dispatches against that
# handle's evidence node and names the effect its head resolved to.
need '^kperform named Loud\.say#n#[0-9]+ Loud@main$'

# Every way of writing the op names one declaration: on the row alias, on
# the effect, through a capability parameter, and through a named instance.
[ "$(printf '%s\n' "$out" | grep -E '^kperform (spoken|through_cap|named) Loud\.say' | sed -E 's/.*#([0-9-]+) .*/\1/' | sort -u | wc -l | tr -d ' ')" = 1 ] \
  || fail "the four ways of writing Loud.say do not all carry one id"

# Two modules each declare `effect Log`. A contested effect is spelled
# `Log__<home>` before the resolver runs, a name the symbol table does not
# hold, so its performs reached KIR with no identity and the two were told
# apart by the spelling alone. Each perform decodes to the `Log` its own
# module declares, and the two ids differ.
HOMOFIX="$ROOT/examples/effects/homonym_perform_ids"
out=$("$ROOT/stage2/kaic2" --edition "$(cat "$ROOT/EDITION")" --dump-symids \
        --path "$HOMOFIX" --path "$ROOT/stdlib" "$HOMOFIX/main.kai" 2>/dev/null)

need '^kperform ra__run Log__ra\.tally#[0-9]+ Log@ra$'
need '^kperform rb__run Log__rb\.total#[0-9]+ Log@rb$'
need '^kperform rb__run Log__rb\.tally#[0-9]+ Log@rb$'
need '^own effect Log__ra#[0-9]+ Log@ra$'
need '^own effect Log__rb#[0-9]+ Log@rb$'
[ "$(id_of '^kperform ra__run Log__ra')" != "$(id_of '^kperform rb__run Log__rb\.total')" ] \
  || fail "the performs of two homonymous effects carry one id"
[ "$(id_of '^kperform rb__run Log__rb\.total')" = "$(id_of '^kperform rb__run Log__rb\.tally')" ] \
  || fail "two ops of one effect carry different ids"
[ "$(id_of '^kperform ra__run Log__ra')" = "$(id_of '^own effect Log__ra')" ] \
  || fail "a perform carries a different id than the declaration it names"

# A declaration keeps its identity through the typer's cache: a warm build
# must reach the late passes with the ids a cold build stamps, not `#-1` —
# both would still run. Same invocation twice over one cache dir: the second
# is warm.
CACHE="$(mktemp -d)"
trap 'rm -rf "$CACHE"' EXIT
own_lines() {
  "$ROOT/stage2/kaic2" --edition "$(cat "$ROOT/EDITION")" --user-cache --user-cache-dir "$CACHE" \
    --core-cache-dir "$CACHE" --dump-symids \
    --path "$ALIASFIX" --path "$ROOT/stdlib" "$ALIASFIX/main.kai" | grep -E '^own '
}
cold=$(own_lines)
warm=$(own_lines)
out=$(printf 'cold:\n%s\nwarm:\n%s\n' "$cold" "$warm")
need '^cold:$'
printf '%s\n' "$cold" | grep -qE '^own effect Loud#[0-9]+ Loud@main$' \
  || fail "a cold build lost an effect declaration's id"
printf '%s\n' "$warm" | grep -q '#-1' \
  && fail "a warm build reaches the late passes with a declaration stripped of its id"
[ "$cold" = "$warm" ] || fail "a warm build's declaration ids differ from a cold build's"

# An id is derived from (home, class, name), not from where the declaration
# sits in the stream: two programs whose root `main` sits among different
# declarations give it one id.
main_id() {
  "$ROOT/stage2/kaic2" --edition "$(cat "$ROOT/EDITION")" --dump-symids \
    --path "$1" --path "$ROOT/stdlib" "$1/main.kai" 2>/dev/null \
    | grep -E '^own fn main#[0-9]+ main@main$' | sed -E 's/.*#([0-9]+) .*/\1/'
}
# A root file's home is the target, which its bytes do not spell. The same
# source under another target name must miss the first one's cache entry,
# not restore declarations homed there.
TWIN="$(mktemp -d)"
trap 'rm -rf "$CACHE" "$TWIN"' EXIT
cp "$ALIASFIX/main.kai" "$TWIN/twin.kai"
twin_own() {
  "$ROOT/stage2/kaic2" --edition "$(cat "$ROOT/EDITION")" --user-cache --user-cache-dir "$1" \
    --core-cache-dir "$1" --dump-symids \
    --path "$TWIN" --path "$ROOT/stdlib" "$TWIN/twin.kai" | grep -E '^own '
}
shared=$(twin_own "$CACHE")
fresh=$(twin_own "$TWIN")
out=$(printf 'over main.kai cache:\n%s\nfresh:\n%s\n' "$shared" "$fresh")
printf '%s\n' "$shared" | grep -q '@main$' && fail "a cached root restored ids homed under another target"
[ "$shared" = "$fresh" ] || fail "a root compiled over another target's cache differs from a fresh build"

a=$(main_id "$ALIASFIX")
b=$(main_id "$HOMOFIX")
out="alias main#$a, homonym main#$b"
[ -n "$a" ] && [ "$a" = "$b" ] || fail "one declaration key carries two ids across two programs"

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

# The unbox pass classifies a callee by the declaration it names. `pkg`'s
# own `one` is all-raw while a stdlib `one` shares its name; read by name,
# `direct` took the homonym's boxed class and unboxed its own result.
MATFIX="$ROOT/examples/namespace-collisions/own_fn_call_forms_matrix"
kir=$(cd "$MATFIX" && "$ROOT/stage2/kaic2" --edition "$(cat "$ROOT/EDITION")" --emit=kir \
        --path "$MATFIX" --path "$ROOT/stdlib" main.kai 2>/dev/null)
direct=$(printf '%s\n' "$kir" | awk '/^fn pkg__direct\(/{on=1} on{print} on&&/^}/{exit}')
[ -n "$direct" ] || { out=$kir; fail "no pkg__direct in the KIR dump"; }
printf '%s\n' "$direct" | grep -q 'int.unbox' \
  && { out=$direct; fail "pkg.direct re-unboxes its own callee's result"; }

echo "symid-pipeline OK — unbox and perceus read the resolved ids, KPerform carries the effect's, two homonymous effects carry two ids, declarations keep their ids through a warm cache and across programs, every way of writing an op — row alias, effect, capability parameter, named instance — carries one id, root calls reach root declarations, each specialisation is its generic's id plus its own instance, and a callee's signature class is its own declaration's"
