#!/bin/sh
# A build that reuses cached objects gives what a build with no cache gives.
#
# Each sequence builds a package, edits it, and builds it again twice: over
# the cache the first build left, and over an empty one. The two must agree,
# and agree with the C backend; and an object both caches hold under one
# name must be the same object. Every edit changes something an object holds
# that no function it defines names: an impl-table row, a variant,
# a block's name, the place of an effect's operation.
#
# Each sequence also names how many objects its edit emits. More means the
# cache rebuilt a module the edit did not reach. The count is of the native
# backend's objects, so it is asserted only where this kaic2 has that backend.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fail=0
native=0
"$ROOT/tools/kaic2-native-capable.sh" "$ROOT/stage2/kaic2" > /dev/null 2>&1 && native=1

manifest() { printf 'name = "%s"\nversion = "0.1.0"\nedition = "hanga-roa"\n%s' "$1" "$2" > kai.toml; }

# What a run reports, free of paths, timings and the driver's own notes.
report() {
  case "$1" in
    run)   grep -v '^kai: ' || true ;;
    test)  grep -E '^  (ok|FAIL) ' || true ;;
    bench) sed -n 's/^  \(.*\): [0-9]* iter \/ .*/\1/p' ;;
    check) grep -E ': [0-9]+ iter, ' || true ;;
  esac
}

# The report of `kai <verb>` on the package in $WORK/$1 over the cache named $2.
built() {
  pkg="$1"; cache="$2"; verb="$3"; shift 3
  (cd "$WORK/$pkg" && XDG_CACHE_HOME="$WORK/cache-$cache" KAI_BENCH_ITERS=2 "$KAI" "$verb" "$@" . 2>&1) | report "$verb"
}

# An object is named by its key, so one name in both caches is one object.
same_objects() {
  for o in "$WORK/cache-$1-fresh"/kai/nmodular/*/*.o; do
    [ -f "$o" ] || continue
    for twin in "$WORK/cache-$1"/kai/nmodular/*/"$(basename "$o")"; do
      [ -f "$twin" ] || continue
      cmp -s "$o" "$twin" || { echo "native-cache-edits FAIL ($1): $(basename "$o") differs between the two caches"; fail=1; }
    done
  done
}

# The objects the cache named $1 holds.
objects() { find "$WORK/cache-$1/kai/nmodular" -name '*.o' 2> /dev/null | wc -l | tr -d ' '; }

# sequence <name> <verb> <before> <after> <emitted>: `before` and `after` write
# the package; the edit emits `emitted` objects.
sequence() {
  name="$1"; verb="$2"; before="$3"; after="$4"; emitted="$5"
  mkdir -p "$WORK/$name"
  (cd "$WORK/$name" && "$before")
  built "$name" "$name" "$verb" > /dev/null
  held="$(objects "$name")"
  (cd "$WORK/$name" && "$after")
  cached="$(built "$name" "$name" "$verb")"
  if [ "$native" -eq 1 ] && [ "$(( $(objects "$name") - held ))" -ne "$emitted" ]; then
    echo "native-cache-edits FAIL ($name): the edit emitted $(( $(objects "$name") - held )) objects, not $emitted"; fail=1
  fi
  cp -R "$WORK/$name" "$WORK/$name-fresh"; rm -rf "$WORK/$name-fresh/.kai-cache"
  fresh="$(built "$name-fresh" "$name-fresh" "$verb")"
  oracle="$(built "$name-fresh" "$name-c" "$verb" --backend=c)"
  same_objects "$name"
  if [ -z "$fresh" ] || [ "$cached" != "$fresh" ] || [ "$fresh" != "$oracle" ]; then
    echo "native-cache-edits FAIL ($name)"
    echo "  over the cache:    $(echo "$cached" | tr '\n' '|')"
    echo "  over an empty one: $(echo "$fresh" | tr '\n' '|')"
    echo "  C backend:         $(echo "$oracle" | tr '\n' '|')"
    fail=1
  fi
}

entry() { manifest "$1" 'entry = "main.kai"
'; }

# A function of an imported module starts being used as a value.
value_before() {
  entry value
  printf 'import m\n\nfn main() : Unit / Stdout = Stdout.print("#{m.run()}")\n' > main.kai
  printf 'fn dbl(n: Int) : Int = n * 2\n\npub fn run() : Int = list.sum(list.map([1, 2], dbl))\n' > m.kai
}
value_after() {
  printf 'fn dbl(n: Int) : Int = n * 2\n\nfn tpl(n: Int) : Int = n * 3\n\npub fn run() : Int = list.sum(list.map([1, 2], tpl)) + dbl(0)\n' > m.kai
}

# A function of one module starts being used as a value by another.
borrowed_before() {
  entry borrowed
  printf 'import b\n\nfn main() : Unit / Stdout = Stdout.print("#{b.run()}")\n' > main.kai
  printf 'pub fn dbl(n: Int) : Int = if n > 100 { dbl(n - 100) } else { n * 2 }\n' > a.kai
  printf 'import a\n\nfn own(n: Int) : Int = if n > 100 { own(n - 100) } else { a.dbl(n) + 1 }\n\npub fn run() : Int = list.sum(list.map([1, 2], own)) + a.dbl(0)\n' > b.kai
}
borrowed_after() {
  printf 'import a\n\nfn own(n: Int) : Int = if n > 100 { own(n - 100) } else { a.dbl(n) + 1 }\n\npub fn run() : Int = list.sum(list.map([1, 2], a.dbl)) + own(0)\n' > b.kai
}

# An imported module gains a function that is only ever called.
added_before() {
  entry added
  printf 'import m\n\nfn main() : Unit / Stdout = Stdout.print("#{m.run(5)}")\n' > main.kai
  printf 'fn fact(n: Int, acc: Int) : Int = if n <= 1 { acc } else { fact(n - 1, acc * n) }\n\npub fn run(n: Int) : Int = fact(n, 1)\n' > m.kai
}
added_after() {
  printf 'fn fact(n: Int, acc: Int) : Int = if n <= 1 { acc } else { fact(n - 1, acc * n) }\n\nfn fib(n: Int) : Int = if n < 2 { n } else { fib(n - 1) + fib(n - 2) }\n\npub fn run(n: Int) : Int = fact(n, 1) + fib(n * 2)\n' > m.kai
}

# An imported module gains an impl that is reached through the impl table.
impl_before() {
  entry impl
  printf 'import shapes\n\nfn main() : Unit / Stdout = Stdout.print(shapes.describe())\n' > main.kai
  printf 'pub type Dot = Dot(Int)\n\npub fn describe() : String = "dot"\n' > shapes.kai
}
impl_after() {
  printf '#[derive(Eq, Hash)]\npub type Dot = Dot(Int)\n\npub fn describe() : String = "#{[Dot(1)] == [Dot(1)]} #{hash((Dot(1), 2)) == hash((Dot(1), 2))}"\n' > shapes.kai
}

# A type of an imported module gains a variant.
variant_before() {
  entry variant
  printf 'import shapes\n\nfn main() : Unit / Stdout = Stdout.print(shapes.describe())\n' > main.kai
  printf '#[derive(Eq)]\npub type Shape = Circle(Int) | Square(Int)\n\npub fn describe() : String = "#{[Circle(1)] == [Circle(1)]}"\n' > shapes.kai
}
variant_after() {
  printf '#[derive(Eq)]\npub type Shape = Circle(Int) | Square(Int) | Blank | Tri(Int)\n\npub fn describe() : String = "#{[Tri(3), Blank] == [Tri(3), Blank]} #{[Tri(3)] == [Blank]}"\n' > shapes.kai
}

# A type gains a variant in one module; a module that never names it stays as built.
bystander_before() {
  entry bystander
  printf 'import a\nimport b\n\nfn main() : Unit / Stdout = Stdout.print("#{a.area(a.mk())} #{b.label(b.mk())}")\n' > main.kai
  printf 'pub type Shape = Circle(Int) | Square(Int)\n\npub fn mk() : Shape = Square(3)\n\npub fn area(s: Shape) : Int = match s {\n  Circle(r) -> r * r * 3\n  Square(n) -> n * n\n}\n' > a.kai
  printf 'pub type Mark = Plain | Bold(Int)\n\npub fn mk() : Mark = Bold(2)\n\npub fn label(m: Mark) : String = match m {\n  Plain -> "plain"\n  Bold(n) -> "bold #{n}"\n}\n' > b.kai
}
bystander_after() {
  printf 'pub type Shape = Circle(Int) | Square(Int) | Strip(Int, Int)\n\npub fn mk() : Shape = Strip(3, 4)\n\npub fn area(s: Shape) : Int = match s {\n  Circle(r) -> r * r * 3\n  Square(n) -> n * n\n  Strip(w, h) -> w * h\n}\n' > a.kai
}

# An effect gains an operation ahead of the one another module performs.
effect_before() {
  entry effect
  printf 'pub effect Log {\n  note(n: Int) : Int\n}\n\npub fn with_log(f: () -> Int / Log) : Int = handle {\n  f()\n} with Log {\n  note(n, resume) -> resume(n + 1)\n}\n' > eff.kai
  printf 'import eff\n\npub fn work() : Int / eff.Log = eff.Log.note(41)\n' > user.kai
  printf 'import eff\nimport user\n\nfn main() : Unit / Stdout = Stdout.print("#{eff.with_log(() => user.work())}")\n' > main.kai
}
effect_after() {
  printf 'pub effect Log {\n  mute(n: Int) : Int\n  note(n: Int) : Int\n}\n\npub fn with_log(f: () -> Int / Log) : Int = handle {\n  f()\n} with Log {\n  mute(n, resume) -> resume(0 - 1)\n  note(n, resume) -> resume(n + 1)\n}\n' > eff.kai
}

# A block is renamed and its body stays; `$1` is the block keyword and its header.
block_package() {
  manifest "$1" ''
  printf 'pub fn inc(n: Int) : Int = n + 1\n' > calc.kai
  mkdir -p tests
}
renamed_test_before() { block_package rename-test; printf 'import calc\n\ntest "first name" {\n  assert calc.inc(1) == 2\n}\n' > tests/a_test.kai; }
renamed_test_after()  { printf 'import calc\n\ntest "second name" {\n  assert calc.inc(1) == 2\n}\n' > tests/a_test.kai; }
renamed_bench_before() { block_package rename-bench; printf 'import calc\n\nbench "first name" {\n  calc.inc(1)\n}\n' > a_test.kai; }
renamed_bench_after()  { printf 'import calc\n\nbench "second name" {\n  calc.inc(1)\n}\n' > a_test.kai; }
renamed_check_before() { block_package rename-check; printf 'import calc\n\ncheck "first name" with n: Int {\n  calc.inc(n) == n + 1\n}\n' > a_test.kai; }
renamed_check_after()  { printf 'import calc\n\ncheck "second name" with n: Int {\n  calc.inc(n) == n + 1\n}\n' > a_test.kai; }

# A second test file gains a block where the first has one, with the same body.
twin_before() { block_package twin; printf 'import calc\n\ntest "a: uses calc" {\n  assert calc.inc(1) == 2\n}\n' > tests/a_test.kai; }
twin_after()  { printf 'import calc\n\ntest "b: uses calc" {\n  assert calc.inc(1) == 2\n}\n' > tests/b_test.kai; }

sequence value     run   value_before     value_after     1
sequence borrowed  run   borrowed_before  borrowed_after  2
sequence added     run   added_before     added_after     1
sequence impl      run   impl_before      impl_after      3
sequence variant   run   variant_before   variant_after   2
sequence bystander run   bystander_before bystander_after 2
sequence effect    run   effect_before    effect_after    3
sequence rename-test  test  renamed_test_before  renamed_test_after  1
sequence rename-bench bench renamed_bench_before renamed_bench_after 1
sequence rename-check check renamed_check_before renamed_check_after 1
sequence twin      test  twin_before      twin_after      1
[ "$fail" = 0 ] && echo "native-cache-edits OK"
exit "$fail"
