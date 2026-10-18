#!/bin/bash
# Early core cut gate. The cut empties the core functions a program
# cannot name before the front end walks them, and a derive writes calls
# and field reads no source text holds. Every fixture that carries a
# derive must compile to the same C with the cut, without it, and with
# every core body emptied; the cut must never need its whole-core retry,
# and emptying everything must. Imported library modules ride the same cut. The retry repeats no warning.

set -u
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAIC2="$ROOT/stage2/kaic2"
WORK="$ROOT/stage2/build/core-early-cut"
EDITION="$(cat "$ROOT/EDITION")"
[ -x "$KAIC2" ] || { echo "core-early-cut: SKIP — no stage2/kaic2"; exit 0; }
rm -rf "$WORK"; mkdir -p "$WORK"

compile() { # $1 output stem, $2 source, rest: flags
  local stem="$1" src="$2"; shift 2
  "$KAIC2" --edition "$EDITION" --path "$(dirname "$src")" --path "${STDLIB:-$ROOT/stdlib}" --core-cache-stats "$@" "$src" \
    > "$stem.c" 2> "$stem.raw"
  echo $? > "$stem.rc"
  grep -v '^kaic2: core-' "$stem.raw" > "$stem.err"
}

pass=0; fail=0; retried=0
for src in examples/minimal/hello.kai tools/core-early-cut-retry-warns.kai tools/core-early-cut-brand-warns.kai examples/effects/issue_682_cancel_sibling_handler.kai tools/core-early-cut-import.kai $(grep -l '#\[derive(' examples/*/*.kai) $(grep -l '^import ' examples/stdlib/*.kai | awk 'NR%3==0'| awk 'NR%6==0'); do
  compile "$WORK/off" "$src" --no-early-cut
  compile "$WORK/cut" "$src"
  compile "$WORK/all" "$src" --early-cut-all
  why=""
  cmp -s "$WORK/cut.rc" "$WORK/off.rc" && cmp -s "$WORK/cut.c" "$WORK/off.c" \
    && cmp -s "$WORK/cut.err" "$WORK/off.err" || why="the cut changes the compile"
  grep -q 'core-precut: retried' "$WORK/cut.raw" && why="the cut emptied a function the program reaches"
  cmp -s "$WORK/all.rc" "$WORK/off.rc" && cmp -s "$WORK/all.c" "$WORK/off.c" \
    && cmp -s "$WORK/all.err" "$WORK/off.err" || why="${why:+$why; }the whole-core retry does not restore the compile"
  grep -q 'core-precut: retried' "$WORK/all.raw" && retried=$((retried+1))
  if [ -n "$why" ]; then
    echo "  FAIL $src — $why"; grep 'core-precut' "$WORK/cut.raw" | sed 's/^/    /'; fail=$((fail+1))
  else pass=$((pass+1)); fi
done

# A copy of the stdlib elsewhere is the same library: same cut, same C.
cp -R "$ROOT/stdlib" "$WORK/stdlib-copy"
fx=examples/effects/stdlib_hof_effectful_callback.kai
compile "$WORK/home" "$fx"
STDLIB="$WORK/stdlib-copy" compile "$WORK/twin" "$fx"
grep -q 'core-precut: emptied' "$WORK/home.raw" || { echo "core-early-cut: FAIL — $fx reports no cut"; exit 1; }
cmp -s "$WORK/home.raw" "$WORK/twin.raw" && cmp -s "$WORK/home.c" "$WORK/twin.c" \
  || { echo "core-early-cut: FAIL — a copy of the stdlib at another path changes the compile of $fx"; exit 1; }

[ "$retried" -gt 0 ] || { echo "core-early-cut: FAIL — emptying every core body never forced the retry"; exit 1; }
[ "$fail" -eq 0 ] || { echo "core-early-cut: FAIL — $fail of $((pass+fail)) programs"; exit 1; }
echo "core-early-cut OK ($pass programs)"
