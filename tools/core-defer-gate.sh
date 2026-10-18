#!/bin/bash
# Core defer gate. A comparison reaches no Eq/Ord impl before inference: an
# operand the runtime compares natively (Int, Real, Char, String, Bool for
# ==) selects none, so the first use of such a comparison, of
# string_length or of string_slice must not type the impl groups. A
# comparison the runtime cannot do natively, or a dispatcher call, must
# still carry the groups.

set -u
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAIC2="${KAIC2:-$ROOT/stage2/kaic2}"
WORK="$ROOT/stage2/build/core-defer"
EDITION="$(cat "$ROOT/EDITION")"
[ -x "$KAIC2" ] || { echo "core-defer: SKIP — no stage2/kaic2"; exit 0; }
rm -rf "$WORK"; mkdir -p "$WORK"

emit() { # $1 name, stdin: source -> prints the count of Eq/Ord impl functions in the emitted C
  cat > "$WORK/$1.kai"
  "$KAIC2" --edition "$EDITION" --path "$ROOT/stdlib" "$WORK/$1.kai" 2> "$WORK/$1.err" | grep -c '__pimpl_\(Eq\|Ord\)'
}

fail=0
expect_none() {
  n="$(emit "$1")"
  if [ "$n" != 0 ]; then echo "  FAIL $1 — $n Eq/Ord impl references in the emitted C, expected none"; fail=$((fail+1)); fi
}
expect_some() {
  n="$(emit "$1")"
  if [ "$n" = 0 ]; then echo "  FAIL $1 — no Eq/Ord impl in the emitted C, expected the groups"; fail=$((fail+1)); fi
}

expect_none int_compare <<'E'
fn big(a: Int, b: Int) : Bool = a > b
fn main() : Unit { if big(2, 1) { println("x") } else { println("y") } }
E
expect_none string_ops <<'E'
fn len(s: String) : Int = string_length(s)
fn cut(s: String) : String = string_slice(s, 0, 1)
fn main() : Unit { println("#{len("ab")}#{cut("ab")}") }
E
expect_none string_compare <<'E'
fn lead(s: String, p: String) : Bool = string_slice(s, 0, string_length(p)) == p
fn main() : Unit { if lead("ab", "a") { println("x") } else { println("y") } }
E
expect_none starts_with <<'E'
fn main() : Unit { if starts_with("ab", "a") { println("x") } else { println("y") } }
E
expect_none structural_eq <<'E'
type Color = Red | Green
fn main() : Unit { if Red == Green { println("x") } else { println("y") } }
E
expect_some compound_lt <<'E'
fn main() : Unit { if [1, 2] < [1, 3] { println("x") } else { println("y") } }
E
expect_some generic_eq <<'E'
fn same[a](x: a, y: a) : Bool = x == y
fn main() : Unit { if same(Some(1), None) { println("x") } else { println("y") } }
E
expect_some dispatcher_call <<'E'
fn main() : Unit { println("#{max(3, 7)}") }
E

if [ "$fail" -gt 0 ]; then echo "core-defer: FAIL — $fail probes"; exit 1; fi
echo "core-defer OK"
