#!/bin/sh
# Usage: core-homonym-gate.sh [mode...]   (modes: c native modular native-modular; default c native)
#
# A bare name reaches the nearest binding of it. Three programs check that
# for every name the core answers to — the runtime builtins, the functions
# the core modules declare, and the protocol operations:
#
#   decls   a root function of one String parameter under each name, called
#           directly, through `|>` and as a value; an imported module
#           privately shadows two builtins, a third runs stdlib code that
#           calls the builtins;
#   lists   a root function of one list parameter under each core-function
#           and protocol-operation name, called the same three ways;
#   locals  a local closure under each name, called the same three ways;
#   arity   a root function under each protocol operation's name taking
#           one more argument than the operation, called directly,
#           through `|>` with the operation's own argument count (the
#           piped value counts, so only the root function fits) and as a
#           value.
#
# A call that passes over a binding the user wrote for one of another
# arity must warn, in --diags-json too.
#
# The fixtures listed in `fixtures` each pin a decision a pass after the
# resolver once made by spelling; every mode runs them cold and again on
# a warm user cache. And a root function spelled like any parameter of a
# core function must leave the core's own bodies compiling exactly as
# they do without it, monolithic and modular alike.
#
# Each program and its oracle, every homonym renamed away, must print the
# expected lines: a use that reaches the core instead of the binding, or a
# core use that reaches a binding, changes one.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
DIR="$(mktemp -d "${TMPDIR:-/tmp}/kai-core-homonym.XXXXXX")"
trap 'rm -rf "$DIR"' EXIT

builtins=$(awk '/^pub fn core_names\(\)/ { on = 1 } on && /^  \]/ { exit } on' \
             "$ROOT/stage2/compiler/resolve.kai" \
           | grep -oE '"[a-z_0-9]+"' | tr -d '"' | grep -v '^__' | sort -u)
[ -n "$builtins" ] || { echo "core-homonym-gate: no core names found"; exit 1; }
# Every protocol operation as `name:arity`, the arity counted from the
# parameters' colons.
op_arities=$(grep -rlE '^(pub )?protocol ' "$ROOT/stdlib" \
      | xargs awk '/^(pub )?protocol /{ on = 1; next } on && /^}/{ on = 0 } on && /^  [a-z_][a-z_0-9]*\(/' \
      | sed -E 's/^  ([a-z_0-9]+)\((.*)\) *:.*/\1 \2/' \
      | awk '{ n = $1; $1 = ""; print n ":" gsub(/:/, ":") }' | sort -u)
ops=$(echo "$op_arities" | cut -d: -f1 | sort -u)
corefns=$(cat "$ROOT"/stdlib/core/*.kai "$ROOT/stdlib/protocols.kai" \
          | grep -oE '^pub fn [a-z_][a-z_0-9]*' | awk '{ print $3 }' | sort -u)
listed=$(printf '%s\n%s\n' "$ops" "$corefns" | sort -u)
every=$(printf '%s\n%s\n' "$builtins" "$listed" | sort -u)

# $1 is the prefix every homonym carries ("" or "my_"), $2 the program dir.
gen_decls() {
  out="$DIR/$2/decls"
  mkdir -p "$out"
  cat > "$out/hm.kai" <<'KAI'
fn int_to_string(n: Int) : String = string.join(list.map(["hm", "int"], (s) => s), "-")

fn string_length(s: String) : Int = list.length([s, s, s]) + 90

pub fn via(n: Int) : String = int_to_string(n)

pub fn len(s: String) : Int = string_length(s)
KAI
  cat > "$out/probe.kai" <<'KAI'
import hm

pub fn shows() : String = "#{42} #{1.5} #{[1, 2]} #{string.length("abc")}"

pub fn via_hm() : String = "#{hm.via(5)} #{hm.len("a")}"
KAI
  {
    echo "import probe"
    echo
    for n in $every; do
      printf 'fn %s%s(a: String) : String = string.join(list.map(["%s", a], (s) => s), ":")\n\n' "$1" "$n" "$n"
    done
    echo 'fn main() : Unit / Stdout = {'
    for n in $every; do
      printf '  let v_%s = %s%s\n' "$n" "$1" "$n"
      printf '  Stdout.print(string.join([%s%s("x"), "x" |> %s%s, v_%s("x")], " "))\n' "$1" "$n" "$1" "$n" "$n"
    done
    echo '  Stdout.print(probe.shows())'
    echo '  Stdout.print(probe.via_hm())'
    echo '}'
  } > "$out/main.kai"
}

gen_lists() {
  out="$DIR/$2/lists"
  mkdir -p "$out"
  {
    for n in $listed; do
      printf 'fn %s%s(xs: [String]) : String = string.join(["%s", ...xs], ":")\n\n' "$1" "$n" "$n"
    done
    echo 'fn main() : Unit / Stdout = {'
    for n in $listed; do
      printf '  let v_%s = %s%s\n' "$n" "$1" "$n"
      printf '  Stdout.print(string.join([%s%s(["x"]), ["x"] |> %s%s, v_%s(["x"])], " "))\n' "$1" "$n" "$1" "$n" "$n"
    done
    echo '}'
  } > "$out/main.kai"
}

gen_locals() {
  out="$DIR/$2/locals"
  mkdir -p "$out"
  {
    for n in $every; do
      printf 'fn t_%s() : String = {\n' "$n"
      printf '  let %s%s = (a: String) => string.join(list.map(["%s", a], (s) => s), ":")\n' "$1" "$n" "$n"
      printf '  let v = %s%s\n' "$1" "$n"
      printf '  string.join([%s%s("x"), "x" |> %s%s, v("x")], " ")\n}\n\n' "$1" "$n" "$1" "$n"
    done
    echo 'fn main() : Unit / Stdout = {'
    for n in $every; do printf '  Stdout.print(t_%s())\n' "$n"; done
    echo '}'
  } > "$out/main.kai"
}

# `$1` the prefix, `$2` the program dir. Each operation of arity k gets a
# root function of arity k + 1; an operation two protocols declare at two
# arities gets one per arity, so the name is declared once at the largest.
gen_arity() {
  out="$DIR/$2/arity"
  mkdir -p "$out"
  {
    for op in $op_max; do
      n=${op%%:*}; k=${op##*:}
      printf 'fn %s%s(a: String%s) : String = string.join(["%s", a%s], ":")\n\n' \
        "$1" "$n" "$(params $((k + 1)))" "$n" "$(args $((k + 1)))"
    done
    echo 'fn main() : Unit / Stdout = {'
    for op in $op_max; do
      n=${op%%:*}; k=${op##*:}
      printf '  let v_%s = %s%s\n' "$n" "$1" "$n"
      printf '  Stdout.print(string.join([%s%s("x"%s), "x" |> %s%s(%s), v_%s("x"%s)], " "))\n' \
        "$1" "$n" "$(vals $k)" "$1" "$n" "$(vals $k | cut -c3-)" "$n" "$(vals $k)"
    done
    echo '}'
  } > "$out/main.kai"
}

# `, p1: String, ...` for parameters 1..$1-1; their names; `, "y"` per value.
params() { i=1; while [ $i -lt $1 ]; do printf ', p%d: String' $i; i=$((i + 1)); done; }
args()   { i=1; while [ $i -lt $1 ]; do printf ', p%d' $i; i=$((i + 1)); done; }
vals()   { i=0; while [ $i -lt $1 ]; do printf ', "y"'; i=$((i + 1)); done; }

op_max=$(echo "$op_arities" | sort -t: -k1,1 -k2,2n | awk -F: '{ m[$1] = $2 } END { for (n in m) print n ":" m[n] }' | sort)

for g in gen_decls gen_lists gen_locals gen_arity; do
  $g "" homonym
  $g "my_" oracle
done

{
  for n in $every; do echo "$n:x $n:x $n:x"; done
  echo "42 1.5 [1, 2] 3"
  echo "hm-int 93"
} > "$DIR/want-decls"
for n in $listed; do echo "$n:x $n:x $n:x"; done > "$DIR/want-lists"
for n in $every; do echo "$n:x $n:x $n:x"; done > "$DIR/want-locals"
for op in $op_max; do
  n=${op%%:*}; k=${op##*:}
  line="$n:x$(i=0; while [ $i -lt $k ]; do printf ':y'; i=$((i + 1)); done)"
  echo "$line $line $line"
done > "$DIR/want-arity"

mkdir -p "$DIR/skip"
cat > "$DIR/skip/main.kai" <<'KAI'
fn min(xs: [Int]) : Int = 100

fn main() : Unit / Stdout = Stdout.print("#{min(1, 2)}")
KAI

fail=0
for mode in ${*:-c native}; do
  case $mode in
    c)              env="KAI_BACKEND=c" ;;
    native)         env="KAI_BACKEND=native KAI_NATIVE_MODULAR=0" ;;
    modular)        env="KAI_BACKEND=c KAI_MODULAR=1" ;;
    native-modular) env="KAI_BACKEND=native KAI_NATIVE_MODULAR=1" ;;
    *) echo "core-homonym-gate: unknown mode $mode"; exit 2 ;;
  esac
  mfail=0
  for shape in decls lists locals arity; do
    for prog in oracle homonym; do
      got="$DIR/got-$mode-$prog-$shape"
      if (cd "$DIR/$prog/$shape" && env $env "$KAI" run main.kai > "$got" 2> "$got.err") \
         && diff -u "$DIR/want-$shape" "$got" > "$got.diff"; then
        :
      else
        echo "core-homonym-gate FAIL ($mode, $prog, $shape)"
        head -20 "$got.diff" 2>/dev/null || true
        grep -E '^(error|kai: error)|undefined|Undefined' "$got.err" | head -10 || true
        mfail=1
      fi
    done
  done
  [ $mfail -eq 1 ] && fail=1 || echo "core-homonym-gate OK ($mode)"
done

fixtures="proto_rung_own_import_core derive_minted_ref_private_home generic_homonyms_spec_by_identity
tail_call_to_core_homonym fused_groups_homonym_leads core_local_named_like_root_fn arity_skips_to_core_fn"
for mode in ${*:-c native}; do
  case $mode in
    c)              env="KAI_BACKEND=c" ;;
    native)         env="KAI_BACKEND=native KAI_NATIVE_MODULAR=0" ;;
    modular)        env="KAI_BACKEND=c KAI_MODULAR=1" ;;
    native-modular) env="KAI_BACKEND=native KAI_NATIVE_MODULAR=1" ;;
  esac
  mfail=0
  for fx in $fixtures; do
    work="$DIR/fx-$mode/$fx"
    mkdir -p "$work"
    cp "$ROOT/examples/namespace-collisions/$fx"/*.kai "$work/"
    for pass in cold warm; do
      got="$work/got-$pass"
      if (cd "$work" && env $env KAI_CACHE=1 "$KAI" run main.kai > "$got" 2> "$got.err") \
         && diff -u "$ROOT/examples/namespace-collisions/$fx/main.out.expected" "$got" > "$got.diff"; then
        :
      else
        echo "core-homonym-gate FAIL ($mode, $fx, $pass)"
        head -20 "$got.diff" 2>/dev/null || true
        head -10 "$got.err" 2>/dev/null || true
        mfail=1
      fi
    done
  done
  [ $mfail -eq 1 ] && fail=1 || echo "core-homonym-gate OK ($mode, fixtures cold+warm)"
done

# Every parameter name of a public core function.
pnames=$(cat "$ROOT"/stdlib/core/*.kai "$ROOT/stdlib/protocols.kai" \
         | grep -E '^pub fn ' | sed -E 's/^[^(]*\((.*)\).*/\1/' | tr ',' '\n' \
         | sed -nE 's/^ *\^?([a-z_][a-z_0-9]*) *:.*/\1/p' | sort -u)
gen_bodies() {
  out="$DIR/$2/bodies"
  mkdir -p "$out"
  {
    echo 'import collections.map'
    echo
    for n in $pnames; do printf 'fn %s%s(a: String) : Int = 0\n\n' "$1" "$n"; done
    cat <<'KAI'
fn main() : Unit / Stdout = {
  let suffix = "!"
  let p = Pair { fst: ["l", "r"], snd: 7 }
  let q = tuple.map_snd(tuple.map_fst(p, (xs) => ["n", ...xs]), (n) => n + 1)
  let xs = list.map([3, 1, 2], (x) => x * 2)
  let s = list.foldl(list.filter(xs, (x) => x > 2), 0, (acc, x) => acc + x)
  let o = option.map(Some("o"), (v) => "#{v}#{suffix}")
  let r = result.map(Ok(4), (v) => v + 1)
  let m = map.put(map.empty(), "k", 3)
  let sorted = list.sort_by(xs, (a, b) => b - a)
KAI
    printf '  let k = string.join(["%s"], "")\n' "$1"
    echo '  Stdout.print("#{q.fst} #{q.snd} #{s} #{o} #{r} #{map.get(m, "k")} #{sorted} #{k}")'
    echo '}'
  } > "$out/main.kai"
}
gen_bodies "" homonym
gen_bodies "my_" oracle
# The definitions of core functions in an emitted C file, specialisations
# excluded: a specialisation's symbol carries the root call site.
core_bodies() {
  awk '/^(static )?[A-Za-z_][A-Za-z_0-9 *]*kaiu_[a-z_0-9]+__[A-Za-z_0-9]+\(.*\) \{$/ {
         name = $0; sub(/\(.*/, "", name); sub(/.* \*?/, "", name)
         on = (name !~ /__mono__|__cspec__|__kai_fused_|_thunk$|^kaiu_main__/)
         if (on) print "@@ " name
         next }
       on { print; if ($0 ~ /^}/) on = 0 }' "$1"
}
for emit in "" "--emit=c-modular"; do
  for prog in homonym oracle; do
    (cd "$DIR/$prog/bodies" && "$ROOT/stage2/kaic2" $emit --path . --path "$ROOT/stdlib" main.kai \
       > "$DIR/bodies-$prog.c" 2> "$DIR/bodies-$prog.err") || true
    core_bodies "$DIR/bodies-$prog.c" > "$DIR/bodies-$prog.txt"
  done
  label=${emit:-whole}
  if [ -s "$DIR/bodies-oracle.txt" ] && diff -u "$DIR/bodies-oracle.txt" "$DIR/bodies-homonym.txt" > "$DIR/bodies.diff"; then
    echo "core-homonym-gate OK (core bodies, $label)"
  else
    echo "core-homonym-gate FAIL (core bodies, $label: a root name changed how a core body compiles)"
    head -30 "$DIR/bodies.diff"; head -5 "$DIR/bodies-homonym.err"
    fail=1
  fi
done

json=$(cd "$DIR/skip" && "$KAI" typecheck --diags-json main.kai 2>/dev/null || true)
case $json in
  *'"severity": "warning"'*'"message": "call to `min` with 2 arguments skips'*) echo "core-homonym-gate OK (skip warning)" ;;
  *) echo "core-homonym-gate FAIL (skip warning missing from --diags-json)"; echo "$json" | head -c 600; fail=1 ;;
esac

# The skip warning names what the call reached: a declaration, a
# protocol operation, a core function.
SFX="$ROOT/examples/namespace-collisions/arity_skips_to_core_fn"
"$ROOT/stage2/kaic2" --path "$SFX" --path "$ROOT/stdlib" "$SFX/main.kai" 2>&1 >/dev/null \
  | grep '^warning: call to' > "$DIR/skips.got" || true
if diff -u "$SFX/skips.expected" "$DIR/skips.got" > "$DIR/skips.diff"; then
  echo "core-homonym-gate OK (skip warning text)"
else
  echo "core-homonym-gate FAIL (skip warning text)"; cat "$DIR/skips.diff"; fail=1
fi

# A call nothing fits gets the arity error alone, never a skip warning.
mkdir -p "$DIR/skipnone"
cat > "$DIR/skipnone/main.kai" <<'KAI'
fn main() : Unit / Stdout = {
  let f = (a: Int, b: Int) => a + b
  Stdout.print(int_to_string(f(1, 2, 3)))
}
KAI
errs=$("$ROOT/stage2/kaic2" --path "$DIR/skipnone" --path "$ROOT/stdlib" "$DIR/skipnone/main.kai" 2>&1 >/dev/null || true)
case $errs in
  *skips*) echo "core-homonym-gate FAIL (a call nothing fits earned a skip warning)"; echo "$errs" | head -8; fail=1 ;;
  *'wrong number of arguments'*) echo "core-homonym-gate OK (no skip warning when nothing fits)" ;;
  *) echo "core-homonym-gate FAIL (a call nothing fits compiled)"; echo "$errs" | head -8; fail=1 ;;
esac
exit $fail
