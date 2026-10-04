#!/bin/sh
# Usage: core-homonym-gate.sh [mode...]   (modes: c native modular native-modular; default c native)
#
# Builds one program whose root file declares a function under the name
# of every core builtin and calls each; an imported module privately
# shadows two builtins in its own body; a third module runs stdlib code
# that calls the builtins. Both the program and its oracle, with every
# root declaration renamed away from the builtins, must print the
# expected lines: a call that reaches the builtin instead of the
# declaration, or a builtin call that reaches a declaration, changes one.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
DIR="$(mktemp -d "${TMPDIR:-/tmp}/kai-core-homonym.XXXXXX")"
trap 'rm -rf "$DIR"' EXIT

names=$(awk '/^pub fn core_names\(\)/ { on = 1 } on && /^  \]/ { exit } on' \
          "$ROOT/stage2/compiler/resolve.kai" \
        | grep -oE '"[a-z_0-9]+"' | tr -d '"' | grep -v '^__' | sort -u)
[ -n "$names" ] || { echo "core-homonym-gate: no core names found"; exit 1; }

# $1: the prefix a root declaration carries ("" or "my_").
gen() {
  out="$DIR/$2"
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
    for n in $names; do
      printf 'fn %s%s(a: String) : String = string.join(list.map(["%s", a], (s) => s), ":")\n\n' "$1" "$n" "$n"
    done
    echo 'fn main() : Unit / Stdout = {'
    for n in $names; do
      printf '  Stdout.print(%s%s("x"))\n' "$1" "$n"
    done
    cat <<'KAI'
  Stdout.print(probe.shows())
  Stdout.print(probe.via_hm())
}
KAI
  } > "$out/main.kai"
}

gen "" homonym
gen "my_" oracle

{
  for n in $names; do echo "$n:x"; done
  echo "42 1.5 [1, 2] 3"
  echo "hm-int 93"
} > "$DIR/want"

fail=0
for mode in ${*:-c native}; do
  case $mode in
    c)              env="KAI_BACKEND=c" ;;
    native)         env="KAI_BACKEND=native KAI_NATIVE_MODULAR=0" ;;
    modular)        env="KAI_BACKEND=c KAI_MODULAR=1" ;;
    native-modular) env="KAI_BACKEND=native KAI_NATIVE_MODULAR=1" ;;
    *) echo "core-homonym-gate: unknown mode $mode"; exit 2 ;;
  esac
  for prog in oracle homonym; do
    if (cd "$DIR/$prog" && env $env "$KAI" run main.kai > "$DIR/got-$mode-$prog" 2> "$DIR/err-$mode-$prog") \
       && diff -u "$DIR/want" "$DIR/got-$mode-$prog" > "$DIR/diff-$mode-$prog"; then
      :
    else
      echo "core-homonym-gate FAIL ($mode, $prog)"
      head -20 "$DIR/diff-$mode-$prog" 2>/dev/null || true
      grep -E '^(error|kai: error)|undefined|Undefined' "$DIR/err-$mode-$prog" | head -10 || true
      fail=1
    fi
  done
  [ $fail -eq 1 ] || echo "core-homonym-gate OK ($mode)"
done
exit $fail
