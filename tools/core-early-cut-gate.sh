#!/bin/bash
# Early core cut gate. The cut empties every core function before the front
# end walks them, and a body is completed when the typed program asks for
# its function. Every fixture that carries a derive must compile to the same
# C, diagnostics and exit code with the cut as without it. Imported library
# modules ride the same cut.

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

pass=0; fail=0
for src in examples/minimal/hello.kai tools/core-early-cut-retry-warns.kai tools/core-early-cut-brand-warns.kai examples/effects/issue_682_cancel_sibling_handler.kai tools/core-early-cut-import.kai $(grep -l '#\[derive(' examples/*/*.kai) $(grep -l '^import ' examples/stdlib/*.kai | awk 'NR%3==0'| awk 'NR%6==0'); do
  compile "$WORK/off" "$src" --no-early-cut
  compile "$WORK/cut" "$src"
  why=""
  cmp -s "$WORK/cut.rc" "$WORK/off.rc" && cmp -s "$WORK/cut.c" "$WORK/off.c" \
    && cmp -s "$WORK/cut.err" "$WORK/off.err" || why="the cut changes the compile"
  grep -q 'internal error' "$WORK/cut.raw" && why="${why:+$why; }the cut left a function without its body"
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

# A mutual tail pair in a library module fuses with the cut as without it.
cp tools/core-early-cut-tail-lib.kai "$WORK/stdlib-copy/tailpair.kai"
STDLIB="$WORK/stdlib-copy" compile "$WORK/tail-off" tools/core-early-cut-tail.kai --no-early-cut
STDLIB="$WORK/stdlib-copy" compile "$WORK/tail-cut" tools/core-early-cut-tail.kai
grep -q '__kai_fused_' "$WORK/tail-off.c" || { echo "core-early-cut: FAIL — the mutual tail pair is not fused"; exit 1; }
cmp -s "$WORK/tail-cut.c" "$WORK/tail-off.c" || { echo "core-early-cut: FAIL — the cut changes the compile of a mutual tail pair"; exit 1; }

[ "$fail" -eq 0 ] || { echo "core-early-cut: FAIL — $fail of $((pass+fail)) programs"; exit 1; }
echo "core-early-cut OK ($pass programs)"
