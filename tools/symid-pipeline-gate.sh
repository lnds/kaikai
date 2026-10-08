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

# The row label a perform adds carries the same id, and two ops of one
# effect settle into one label.
row_id() {
  printf '%s\n' "$out" | grep -E "$1" | sed -E 's/.*#([0-9-]+)$/\1/'
}
[ "$(row_id '^row run Log__ra#')" = "$(id_of '^own effect Log__ra')" ] \
  || fail "a body row label carries a different id than the effect it names"
[ "$(printf '%s\n' "$out" | grep -cE '^row run Log__rb#[0-9]+$')" = 1 ] \
  || fail "two ops of one effect left two row labels"

# A root effect shares its name with a core one. From a module that
# declares no `Log` the bare perform climbs to the core, which the root
# cannot shadow there; the root's own perform keeps the root's id.
COREFIX="$ROOT/examples/effects/core_perform_id_under_root_homonym"
out=$("$ROOT/stage2/kaic2" --edition "$(cat "$ROOT/EDITION")" --dump-symids \
        --path "$COREFIX" --path "$ROOT/stdlib" "$COREFIX/main.kai" 2>/dev/null)

need '^kperform mc__go Log\.info!?#[0-9]+ Log@effects$'
need '^kperform main Log__main\.note!?#[0-9]+ Log@main$'

# The core effect stays nameable from that root as `effects.Log`: a handle
# head, a qualified perform and a row label each carry the core's id, and
# an alias keeps the components its own module settled.
QUALFIX="$ROOT/examples/effects/core_effect_named_qualified"
out=$("$ROOT/stage2/kaic2" --edition "$(cat "$ROOT/EDITION")" --dump-symids \
        --path "$QUALFIX" --path "$ROOT/stdlib" "$QUALFIX/main.kai" 2>/dev/null)

need '^kperform both Log\.info#[0-9]+ Log@effects$'
need '^kperform both Log__main\.note#[0-9]+ Log@main$'
need '^unbox main with Log#[0-9]+ Log@effects$'
need '^unbox main with Log__main#[0-9]+ Log@main$'
need '^kperform ma__go Log__ma\.note#[0-9]+ Log@ma$'
[ "$(row_id '^row both Log#')" = "$(id_of '^kperform both Log\.info')" ] \
  || fail "a qualified row label carries a different id than the core effect"
[ "$(row_id '^row main Stdout#')" = "$(id_of '^kperform ma__go Stdout\.print')" ] \
  || fail "a builtin's row label carries a different id than its core declaration"

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

# A UFCS callee carries the id of the declaration the receiver's type
# picked, cold and warm: an imported fn, one chained on another, a private
# fn called inside its own module, and a root fn. A local binder spelled
# like the root fn keeps its own reference, so the root id appears once.
UFCSFIX="$ROOT/examples/namespace-collisions/ufcs_callee_identity"
ufcs_lines() {
  "$ROOT/stage2/kaic2" --edition "$(cat "$ROOT/EDITION")" --user-cache --user-cache-dir "$CACHE" \
    --core-cache-dir "$CACHE" --dump-symids \
    --path "$UFCSFIX" --path "$ROOT/stdlib" "$UFCSFIX/main.kai" | grep -E '^perceus (main|twice) (bump|twice|scale)#'
}
for pass in cold warm; do
  out=$(ufcs_lines)
  [ "$(printf '%s\n' "$out" | grep -cE '^perceus twice bump#[0-9]+ bump@ma$')" = 2 ] \
    || fail "$pass: a private fn called by UFCS in its own module lost its id"
  [ "$(printf '%s\n' "$out" | grep -cE '^perceus main twice#[0-9]+ twice@ma$')" = 3 ] \
    || fail "$pass: an imported or chained UFCS callee lost its id"
  [ "$(printf '%s\n' "$out" | grep -cE '^perceus main twice#[0-9]+ twice@main$')" = 1 ] \
    || fail "$pass: a root fn spelled like an imported one lost its id"
  [ "$(printf '%s\n' "$out" | grep -cE '^perceus main scale#[0-9]+ scale@main$')" = 1 ] \
    || fail "$pass: the root fn and the local binder that shadows it do not resolve apart"
done

# A root fn spelled like a stdlib export and the export itself: each UFCS
# callee carries the id of the one its receiver's type picked.
VSFIX="$ROOT/examples/namespace-collisions/ufcs_callee_vs_stdlib"
out=$("$ROOT/stage2/kaic2" --edition "$(cat "$ROOT/EDITION")" --dump-symids \
        --path "$ROOT/stdlib" "$VSFIX/main.kai")
need '^perceus main reverse#[0-9]+ reverse@main$'
need '^perceus main reverse#[0-9]+ reverse@list$'

# Whether a UFCS callee carries an id depends on which modules declare its
# name, so `ma`'s cached typed blob must not travel between a program where
# its pick is uncontested and one where it is contested, in either order.
# Both programs carry byte-identical copies of `ma` and `mc`.
CUT="$ROOT/examples/ufcs/contest_cut"
CUTCACHE="$(mktemp -d)"
trap 'rm -rf "$CACHE" "$TWIN" "$CUTCACHE"' EXIT
cut_ids() {
  "$ROOT/stage2/kaic2" --edition "$(cat "$ROOT/EDITION")" --user-cache --user-cache-dir "$CUTCACHE" \
    --core-cache-dir "$CUTCACHE" --dump-symids "$@" --path "$ROOT/stdlib" \
    | grep -cE '^perceus run twice#[0-9]+ twice@mc$' || true
}
for step in two one two one; do
  want=0; [ "$step" = one ] && want=1
  got=$(cut_ids --path "$CUT/$step" "$CUT/$step/main.kai")
  out="step $step: $got id line(s), want $want"
  [ "$got" = "$want" ] || fail "a cached typed blob carried a UFCS callee's form across programs"
done

# A constructor's home is its identity: an arm tests, and a construction
# builds, the declaration its type picked, so two modules' homonyms carry
# two ids, the same cold and warm.
CTORFIX="$ROOT/examples/namespace-collisions/ctor_match_from_third_module"
CTORCACHE="$(mktemp -d)"
trap 'rm -rf "$CACHE" "$TWIN" "$CUTCACHE" "$CTORCACHE"' EXIT
ctor_lines() {
  "$ROOT/stage2/kaic2" --edition "$(cat "$ROOT/EDITION")" --user-cache --user-cache-dir "$CTORCACHE" \
    --core-cache-dir "$CTORCACHE" --dump-symids --path "$CTORFIX" --path "$ROOT/stdlib" \
    "$CTORFIX/main.kai" | grep '^ctor ' || true
}
ctor_cold=$(ctor_lines)
ctor_warm=$(ctor_lines)
for pass in cold warm; do
  if [ "$pass" = cold ]; then out=$ctor_cold; else out=$ctor_warm; fi
  need '^ctor pat pa Tok#[0-9]+ Tok@ma$'
  need '^ctor pat pb Tok#[0-9]+ Tok@mb$'
  need '^ctor mod main Tok#[0-9]+ Tok@ma$'
  need '^ctor mod main Tok#[0-9]+ Tok@mb$'
  printf '%s\n' "$out" | grep -q ' Tok#none$' && fail "$pass: a constructor site reached unbox with no home"
  [ "$(id_of '^ctor pat pa Tok#')" != "$(id_of '^ctor pat pb Tok#')" ] \
    || fail "$pass: two modules' homonymous constructors carry one id"
  [ "$(id_of '^ctor pat pa Tok#')" = "$(id_of '^ctor mod main Tok#[0-9]+ Tok@ma$')" ] \
    || fail "$pass: the arm and the construction of one constructor carry different ids"
done
[ "$ctor_cold" = "$ctor_warm" ] || fail "a warm build's constructor ids differ from a cold build's"

# A sub-pattern is tested against its payload slot's type, so a homonym
# nested under `Some(...)` names the module the slot's type picked.
NESTFIX="$ROOT/examples/namespace-collisions/ctor_nested_option_payload"
out=$("$ROOT/stage2/kaic2" --edition "$(cat "$ROOT/EDITION")" --dump-symids \
        --path "$NESTFIX" --path "$ROOT/stdlib" "$NESTFIX/main.kai" | grep '^ctor ' || true)
need '^ctor pat ra Circle#[0-9]+ Circle@ma$'
need '^ctor pat ra Sq#[0-9]+ Sq@ma$'
printf '%s\n' "$out" | grep -q ' Circle#none$' && fail "a nested constructor pattern reached unbox with no home"

# A qualified call carries the id of the declaration its qualifier names,
# cold and warm alike, and a generic two modules share is specialised per
# the qualifier's home rather than per the module whose body holds the call.
QFIX="$ROOT/examples/namespace-collisions/qualified_call_carries_identity"
QCACHE="$(mktemp -d)"
trap 'rm -rf "$CACHE" "$TWIN" "$CUTCACHE" "$CTORCACHE" "$QCACHE"' EXIT
for pass in cold warm; do
  out=$("$ROOT/stage2/kaic2" --edition "$(cat "$ROOT/EDITION")" --user-cache --user-cache-dir "$QCACHE" \
          --core-cache-dir "$QCACHE" --dump-symids --path "$QFIX" --path "$ROOT/stdlib" "$QFIX/main.kai")
  need '^perceus main twice#[0-9]+ twice@qa$'
  need '^perceus main label#[0-9]+ label@qa$'
  need '^perceus main label#[0-9]+ label@qb$'
  need '^perceus main label#[0-9]+ label@main$'
  need '^efn wrap__mono__Int/0#[0-9]+ wrap@qa '
  need '^efn wrap__mono__String/0#[0-9]+ wrap@qb '
done

# A call spelled like the function whose body holds it, but naming another
# module's, carries that module's declaration's id, never the caller's.
HFIX="$ROOT/examples/namespace-collisions/qualified_call_in_homonym_body"
out=$("$ROOT/stage2/kaic2" --edition "$(cat "$ROOT/EDITION")" --dump-symids \
        --path "$HFIX" --path "$ROOT/stdlib" "$HFIX/main.kai")
need '^perceus twice twice#[0-9]+ twice@ma$'
need '^perceus half half#[0-9]+ half@ma$'
printf '%s\n' "$out" | grep -qE '^perceus (twice|half) (twice|half)#[0-9]+ (twice|half)@main$' \
  && fail "a homonym of the enclosing function reached Perceus as a self-call"
[ "$(printf '%s\n' "$out" | grep -cE '^perceus step step#[0-9]+ step@ma$')" = 2 ] \
  || fail "mb.step's call to ma.step lost ma.step's id"
printf '%s\n' "$out" | grep -qE '^perceus step step#[0-9]+ step@mb$' \
  && fail "mb.step's call to ma.step reached Perceus as a self-call"

# A capability annotated by an effect spelled like a core type names the
# effect, from the declaring module and from one that imports it, cold
# and warm: types and effects climb one ladder, nearest first.
EFIX="$ROOT/examples/effects/capability_param_core_type_homonym.kai"
ECACHE="$(mktemp -d)"
EIMP="$(mktemp -d)"
trap 'rm -rf "$CACHE" "$TWIN" "$CUTCACHE" "$CTORCACHE" "$QCACHE" "$ECACHE" "$EIMP"' EXIT
cat > "$EIMP/tasks.kai" <<'KAI'
pub effect Child {
  get() : Int
}

pub fn twice(c: Child) : Int = c.get() + c.get()
KAI
cat > "$EIMP/main.kai" <<'KAI'
import tasks

fn main() : Unit / Stdout = handle {
  Stdout.print(int_to_string(tasks.twice(k)))
} with tasks.Child as k {
  get(resume) -> resume(21)
}
KAI
for pass in cold warm; do
  out=$("$ROOT/stage2/kaic2" --edition "$(cat "$ROOT/EDITION")" --user-cache --user-cache-dir "$ECACHE" \
          --core-cache-dir "$ECACHE" --dump-symids --path "$ROOT/stdlib" "$EFIX")
  [ "$(printf '%s\n' "$out" | grep -cE '^perceus (send|add) Child\.(get|put|rec)#[a-z0-9]+#[0-9]+ Child@capability_param_core_type_homonym$')" = 5 ] \
    || fail "a capability spelled like a core type did not name the file's effect ($pass)"
  printf '%s\n' "$out" | grep -q 'Child@os' && fail "a capability op named the core type ($pass)"
  out=$("$ROOT/stage2/kaic2" --edition "$(cat "$ROOT/EDITION")" --user-cache --user-cache-dir "$ECACHE" \
          --core-cache-dir "$ECACHE" --dump-symids --path "$EIMP" --path "$ROOT/stdlib" "$EIMP/main.kai")
  need '^perceus twice Child\.get#c#[0-9]+ Child@tasks$'
  printf '%s\n' "$out" | grep -q 'Child@os' && fail "an imported capability op named the core type ($pass)"
done

# A specialised callee is named by its spec's identity; a callee minted
# from the mangled spelling alone would reach every later pass with no id.
grep -nE 'EVar\(mangled\)|EModCall\([a-z]+, mangled\)' "$ROOT/stage2/compiler/monomorph.kai" \
  && fail "monomorph mints a spec callee from its mangled name"

# A cell perform names the core `State`/`Reader`: the native backend finds
# the op's handler by that id alone. The fixtures cover the cell shapes and
# the cells stdlib keeps; the sweep covers every corpus file declaring one.
cell_dump() {
  "$ROOT/stage2/kaic2" --edition "$(cat "$ROOT/EDITION")" --dump-symids \
    --path "$ROOT/stdlib" "$ROOT/examples/effects/$1.kai"
}
cell_core() {
  printf '%s\n' "$out" | grep -E '^kperform [^ ]+ (State|Reader)\.' \
    | grep -vqE ' (State|Reader)@concurrent$' && fail "a cell perform does not name the core effect ($1)"
  return 0
}
out=$(cell_dump cell_perform_ids)
need '^kperform _kaiu_stacked__clause_[0-9_]+_push State\.[a-z]+@xs#[0-9]+ State@concurrent$'
need '^kperform reads Reader\.ask@env#[0-9]+ Reader@concurrent$'
need '^kperform _kaiu_lam_reads_[0-9_]+ Reader\.ask@env#[0-9]+ Reader@concurrent$'
cell_core cell_perform_ids
out=$(cell_dump cell_perform_ids_stdlib)
need '^kperform _kaiu_lam_captured_[0-9_]+ State\.[a-z]+(@c)?#[0-9]+ State@concurrent$'
need '^kperform stream__count State\.[a-z]+(@[a-z_]+)?#[0-9]+ State@concurrent$'
need '^kperform math__bigint_limbs__mag_mul State\.[a-z]+(@[a-z_]+)?#[0-9]+ State@concurrent$'
cell_core cell_perform_ids_stdlib
out=$(grep -rlE '(^|[^a-z_])var |with (State|Reader)' "$ROOT/examples" --include='*.kai' \
  | grep -v /negative/ \
  | xargs -P 8 -n 1 sh -c '"$0" --edition "$1" --dump-symids --path "$(dirname "$3")" --path "$2" "$3" 2>/dev/null \
      | grep -E "^kperform [^ ]+ [^ ]+#-1 " | sed "s|^|$3: |"; exit 0' \
      "$ROOT/stage2/kaic2" "$(cat "$ROOT/EDITION")" "$ROOT/stdlib")
[ -z "$out" ] || fail "a corpus perform reached KIR with no effect id"

echo "symid-pipeline OK — unbox and perceus read the resolved ids, KPerform carries the effect's, two homonymous effects carry two ids, declarations keep their ids through a warm cache and across programs, every way of writing an op — row alias, effect, capability parameter, named instance — carries one id, root calls reach root declarations, each specialisation is its generic's id plus its own instance, a callee's signature class is its own declaration's, a UFCS callee names the declaration its receiver picked, a qualified call names the one its qualifier homes, a constructor site, nested or not, names the home its type picked, a capability spelled like a core type names its effect, a specialised callee is always its spec's id, and every cell perform names the core State or Reader"
