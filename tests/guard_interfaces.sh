#!/bin/sh
# tests/guard_interfaces.sh — the `kai test` guard memo keys on interfaces.
#
# 1. Every part of a module another module checks against moves its
#    `kaic2 --guard-interfaces` digest, the body of a `main` with no row
#    among them; an edit to a plain function's body or to a comment does not.
# 2. A body error in a module only a test imports fails `kai test` and is
#    reported once; once fixed, the run passes again (a failure is never
#    replayed from the memo).
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
KAI="$ROOT/bin/kai"
FX="$ROOT/tests/guard_iface"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fail=0

digest() {
  "$KAIC2" --path "$WORK/lib" --path "$ROOT/stdlib" --guard-interfaces "$WORK/lib/shapes_user.kai" 2>/dev/null \
    | grep '/shapes.kai	' | cut -f2
}

# mutate <expect: moves|stays> <name> <sed script> — on a fresh copy of the library.
mutate() {
  cp "$FX/shapes.kai" "$WORK/lib/shapes.kai"
  sed -e "$3" "$FX/shapes.kai" > "$WORK/lib/shapes.kai"
  if cmp -s "$FX/shapes.kai" "$WORK/lib/shapes.kai"; then
    echo "guard-interfaces FAIL ($2: the mutation did not apply)"; fail=1; return
  fi
  d="$(digest)"
  if [ "$1" = moves ] && [ "$d" = "$base" ]; then echo "guard-interfaces FAIL ($2: the digest stayed)"; fail=1; fi
  if [ "$1" = stays ] && [ "$d" != "$base" ]; then echo "guard-interfaces FAIL ($2: the digest moved)"; fail=1; fi
}

mkdir -p "$WORK/lib"
cp "$FX/shapes.kai" "$FX/shapes_user.kai" "$WORK/lib/"
base="$(digest)"
[ -n "$base" ] || { echo "guard-interfaces FAIL (no digest for the library)"; exit 1; }

mutate moves constructor        's/Circle(Int) | Square(Int)/Circle(Int) | Square(Int) | Tri(Int)/'
mutate moves constructor-field  's/Circle(Int) |/Circle(Real) |/'
mutate moves record-field       's/{ x: Int, y: Int }/{ x: Int, y: Int, z: Int }/'
mutate moves signature          's/pub fn grow(p: Point) : Point/pub fn grow(p: Point, d: Int) : Point/'
mutate moves effect-row         's/: Unit \/ Log =/: Unit \/ Log + Stdout =/'
mutate moves effect-op          's/log(s: String) : Unit/log(s: String, n: Int) : Unit/'
mutate moves protocol-op        's/area(x: Self) : Int/area(x: Self) : Real/'
mutate moves impl-dispatch      's/^impl Area for Shape/impl Area for Point/'
mutate moves derive             's/#\[derive(Eq)\]/#[derive(Eq, Show)]/'
mutate moves const-type         's/LIMIT : Int = 10/LIMIT : Real = 10.0/'
mutate moves const-value        's/LIMIT : Int = 10/LIMIT : Int = 11/'
mutate moves unit               's/^unit cm/unit mm/'
mutate moves import             's/^import math.real/import math.real\
import fs.file/'
mutate moves visibility         's/^pub fn label/fn label/'
mutate moves generic-body       's/\[h, ..._\] -> Some(h)/[h, ..._] -> Some(h) |> (x => x)/'
mutate moves generic-position   's/^pub const LIMIT/\
pub const LIMIT/'
mutate moves contract           's/requires b != 0/requires b > 0/'
mutate moves unrowed-main-body  's/^fn main() : Int = 0/fn main() : Int = 1/'
mutate stays plain-body         's/x: p.x + 1/x: p.x + 2/'
mutate stays plain-body-line    's/^  Blue -> "blue"/  Blue ->\
    "blue"/'
mutate stays impl-body          's/Circle(r) -> 3 \* r \* r/Circle(r) -> 4 * r * r/'
mutate stays comment            's/^pub fn label/# the label\
pub fn label/'

# A body error in a module only the tests import.
cp -R "$FX/gpkg" "$WORK/gpkg"
run() { (cd "$WORK/gpkg" && XDG_CACHE_HOME="$WORK/cache" "$KAI" test > "$WORK/out" 2>&1); }
run || { echo "guard-interfaces FAIL (the clean package does not pass)"; cat "$WORK/out"; fail=1; }
sed -e 's/= a + b/= a + "b"/' "$FX/gpkg/gpkg/geo.kai" > "$WORK/gpkg/gpkg/geo.kai"
if run; then
  echo "guard-interfaces FAIL (a body error in an imported module passed)"; fail=1
else
  n="$(grep -c 'geo.kai:1:' "$WORK/out" || true)"
  [ "$n" = 1 ] || { echo "guard-interfaces FAIL (the body error was reported $n times, not once)"; cat "$WORK/out"; fail=1; }
fi
cp "$FX/gpkg/gpkg/geo.kai" "$WORK/gpkg/gpkg/geo.kai"
run || { echo "guard-interfaces FAIL (the fixed package still fails)"; cat "$WORK/out"; fail=1; }

[ "$fail" = 0 ] && echo "guard-interfaces OK (18 interface edits move the digest, 4 body or comment edits do not; an imported body error fails once and clears)"
exit "$fail"
