#!/bin/sh
# Writes one program per pair of demand routes into <dir> and prints a
# "run <file>" line for each. A route is a way a program asks the compiler
# for core code; a pair puts two routes in one compile, the shape the corpus
# lacked when pruning bugs hid. The set is fixed: no randomness.
#
#   prune-combos.sh <dir> combos|combos-subset
set -eu
DIR=$1; SET=$2; mkdir -p "$DIR"

ROUTES="prim unit list tuple option sum sum_unit prod maxmin sort contains interp proto proto_gen derive closure"

# decl <route>: top-level declarations the route needs.
decl() {
  case $1 in
    unit|sum_unit) echo "unit m" ;;
    proto) printf 'protocol Named_proto {\n  label(x: Self) : String\n}\nimpl Named_proto for Int {\n  fn label(x: Int) : String = "int"\n}\nimpl Named_proto for String {\n  fn label(x: String) : String = "str"\n}\n' ;;
    proto_gen) printf 'protocol Named_gen {\n  label(x: Self) : String\n}\nimpl Named_gen for Int {\n  fn label(x: Int) : String = "int"\n}\nfn label_twice[t : Named_gen](x: t) : String = "#{label(x)}#{label(x)}"\n' ;;
    derive) printf '#[derive(Eq, Show, Ord)]\ntype Pt_derive = { x: Int, y: Int }\n' ;;
  esac
}

# body <route>: statements of the route's function.
body() {
  case $1 in
    prim) printf '  Stdout.print("#{1 + 2} #{3.5 * 2.0} #{7 < 8} #{"a" < "b"} #{2 == 2}")\n' ;;
    unit) printf '  let a = 2.0<m>\n  let b = 3.0<m>\n  Stdout.print("#{a + b} #{a < b} #{a * 2.0}")\n' ;;
    list) printf '  Stdout.print("#{[1] < [2]} #{[1, 2] == [1, 2]} #{[1, 2] ++ [3]}")\n' ;;
    tuple) printf '  Stdout.print("#{(1, 2) < (1, 3)} #{(1, "a") == (1, "a")}")\n' ;;
    option) printf '  Stdout.print("#{Some(1) == Some(1)} #{Some(1) == None} #{Some(2)}")\n' ;;
    sum) printf '  Stdout.print("#{sum([1, 2, 3])} #{sum([1.5, 2.5])}")\n' ;;
    sum_unit) printf '  Stdout.print("#{sum([1.0<m>, 2.0<m>])}")\n' ;;
    prod) printf '  Stdout.print("#{product([1, 2, 3, 4])} #{product([1.5, 2.0])}")\n' ;;
    maxmin) printf '  Stdout.print("#{max([3, 1, 2])} #{min([3, 1, 2])} #{max(["b", "a"])}")\n' ;;
    sort) printf '  Stdout.print("#{sort([3, 1, 2])} #{sort(["b", "a"])} #{sort([2.5, 1.5])}")\n' ;;
    contains) printf '  Stdout.print("#{contains([1, 2, 3], 2)} #{contains(["a"], "b")} #{contains([[1]], [1])}")\n' ;;
    interp) printf '  Stdout.print("#{1} #{2.5} #{true} #{"s"} #{[1, 2]} #{(1, "a")} #{Some(3)} #{[Some(1)]}")\n' ;;
    proto) printf '  Stdout.print("#{label(1)} #{label("x")}")\n' ;;
    proto_gen) printf '  Stdout.print(label_twice(1))\n' ;;
    derive) printf '  let p = Pt_derive { x: 1, y: 2 }\n  let q = Pt_derive { x: 1, y: 3 }\n  Stdout.print("#{p == q} #{p < q} #{p} #{sort([q, p])}")\n' ;;
    closure) printf '  let add = (a: Int, b: Int) => a + b\n  let addr = (a: Real, b: Real) => a + b\n  let lt = (a: [Int], b: [Int]) => a < b\n  Stdout.print("#{add(1, 2)} #{addr(1.5, 2.5)} #{lt([1], [2])} #{sort_by([3, 1, 2], (a, b) => if a < b { 0 - 1 } else { 1 })}")\n' ;;
  esac
}

# write_prog <file> <route>...
write_prog() {
  f=$1; shift
  : > "$f"
  for r in "$@"; do decl "$r" >> "$f"; done
  for r in "$@"; do printf 'fn run_%s() : Unit / Stdout = {\n' "$r" >> "$f"; body "$r" >> "$f"; printf '}\n' >> "$f"; done
  printf 'fn main() : Unit / Stdout = {\n' >> "$f"
  for r in "$@"; do printf '  run_%s()\n' "$r" >> "$f"; done
  printf '}\n' >> "$f"
}

SUBSET="list+sum_unit unit+sum list+sum tuple+maxmin option+sort derive+sort proto_gen+closure interp+unit prim+contains closure+unit"

pairs() {
  for a in $ROUTES; do for b in $ROUTES; do
    if [ "$a" != "$b" ] && [ "$(printf '%s\n%s\n' "$a" "$b" | sort | head -1)" = "$a" ]; then echo "$a+$b"; fi
  done; done
}

if [ "$SET" = combos-subset ]; then LIST=$SUBSET; else LIST=$(pairs); fi
for p in $LIST; do
  a=${p%+*}; b=${p#*+}
  f="$DIR/c_${a}_${b}.kai"
  write_prog "$f" "$a" "$b"
  echo "run $f"
done
