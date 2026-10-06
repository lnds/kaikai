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
#   locals  a local closure under each name, called the same three ways.
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
ops=$(awk '/^(pub )?protocol /{ on = 1; next } on && /^}/{ on = 0 } on' "$ROOT/stdlib/protocols.kai" \
      | grep -oE '^  [a-z_][a-z_0-9]*\(' | tr -d ' (' | sort -u)
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

for g in gen_decls gen_lists gen_locals; do
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
  for shape in decls lists locals; do
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
exit $fail
