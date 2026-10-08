#!/bin/sh
# The memo of `kai test`'s per-file checks replays exactly what the check
# would print, and misses whenever an input changed.
#
# Every compiler `kai test` starts goes through a shim that logs the
# checks it really runs. Each scenario runs the package with the memo on
# and then off (the oracle): outputs and statuses must be identical, and
# an edit to any input must run at least the checks that read it. A warm
# run with nothing changed runs no check. Shadow mode (a hit also runs
# the check and must match it) is exercised last.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT INT TERM

lay="$work/root"
mkdir -p "$lay/stage2" "$lay/bin"
for e in "$ROOT"/*; do
  case "$(basename "$e")" in
    stage2|bin) ;;
    *) ln -s "$e" "$lay/$(basename "$e")" ;;
  esac
done
for e in "$ROOT"/stage2/*; do
  [ "$(basename "$e")" = kaic2 ] || ln -s "$e" "$lay/stage2/$(basename "$e")"
done
cp "$ROOT/bin/kai" "$lay/bin/kai"
log="$work/checks.log"
cat > "$lay/stage2/kaic2" <<EOF
#!/bin/sh
case " \$* " in
  *" --guard-inputs "*|*" --guard-interfaces "*) ;;
  *" --check "*) echo check >> "$log" ;;
esac
exec "$ROOT/stage2/kaic2" "\$@"
EOF
chmod +x "$lay/stage2/kaic2"

pkg="$work/pkg"
mkdir -p "$pkg/lib" "$pkg/tests"
printf 'name = "memo"\nentry = "main.kai"\n' > "$pkg/kai.toml"
printf 'import lib.util\n\nfn main() : Unit / Stdout = Stdout.print(int_to_string(util.add(1, 2)))\n' > "$pkg/main.kai"
printf '#[doc("Adds.")]\npub fn add(a: Int, b: Int) : Int = a + b\n' > "$pkg/lib/util.kai"
printf 'import lib.cb\n\n#[doc("A.")]\npub fn fa() : Int = 1\n' > "$pkg/lib/ca.kai"
printf '#[doc("B.")]\npub fn fb() : Int = 2\n' > "$pkg/lib/cb.kai"
printf 'import lib.util\n\ntest "adds" {\n  assert util.add(2, 2) == 4\n}\n' > "$pkg/tests/a_test.kai"
printf 'import lib.util\nimport lib.ca\n\ntest "warns" {\n  assert ca.fa() == 1\n}\n' > "$pkg/tests/b_test.kai"

fail=0
extra=""
checks() { if [ -f "$log" ]; then wc -l < "$log" | tr -d ' '; else echo 0; fi; }

# run <label> <memo-mode> [env...]: the run's output, status and checks.
run() {
  rl="$1"; rmode="$2"; shift 2
  : > "$log"
  rrc=0
  (cd "$pkg" && env KAI_BACKEND=c KAI_GUARD_MEMO="$rmode" "$@" "$lay/bin/kai" test -j 1 $extra) \
    > "$work/$rl.out" 2>&1 || rrc=$?
  echo "$rrc" > "$work/$rl.rc"
  checks > "$work/$rl.n"
}

# scenario <label> <min-checks> [env...]: the memo agrees with the oracle;
# a minimum of 0 means the state was seen before, so nothing may re-run.
scenario() {
  label="$1"; min="$2"; shift 2
  run "$label-memo" on "$@"
  run "$label-off" 0 "$@"
  n=$(cat "$work/$label-memo.n")
  if ! cmp -s "$work/$label-memo.out" "$work/$label-off.out" \
     || ! cmp -s "$work/$label-memo.rc" "$work/$label-off.rc"; then
    echo "  FAIL $label: the memoised run differs from the uncached one"
    diff "$work/$label-off.out" "$work/$label-memo.out" | head -10
    fail=$((fail + 1))
  elif [ "$min" -eq 0 ] && [ "$n" -ne 0 ]; then
    echo "  FAIL $label: back to a state already checked, yet $n checks ran"
    fail=$((fail + 1))
  elif [ "$n" -lt "$min" ]; then
    echo "  FAIL $label: $n checks ran, expected at least $min (a stale hit)"
    fail=$((fail + 1))
  else
    echo "  ok   $label ($n checks ran)"
  fi
}

scenario cold 2

run warm on
n=$(cat "$work/warm.n")
if [ "$n" -ne 0 ] || ! cmp -s "$work/warm.out" "$work/cold-off.out"; then
  echo "  FAIL warm: $n checks ran with nothing changed, or the output differs"
  fail=$((fail + 1))
else
  echo "  ok   warm (no check ran, output identical)"
fi

printf '#[doc("Adds.")]\npub fn add(a: Int, b: Int) : Int = b + a\n' > "$pkg/lib/util.kai"
scenario edit-import 2

mkdir -p "$pkg/tests/lib"
cp "$pkg/lib/util.kai" "$pkg/tests/lib/util.kai"
scenario same-content-earlier-dir 1
rm -rf "$pkg/tests/lib"
scenario file-removed 0

printf 'import lib.ca\n\n#[doc("B.")]\npub fn fb() : Int = 2\n' > "$pkg/lib/cb.kai"
scenario cycle-closed 1
printf 'import lib.ca\n\n#[doc("B.")]\npub fn fb() : Int = 3\n' > "$pkg/lib/cb.kai"
scenario cycle-member-edited 1
printf '#[doc("B.")]\npub fn fb() : Int = 2\n' > "$pkg/lib/cb.kai"
scenario cycle-opened 0

printf 'import lib.cb\n\n#[doc("A.")]\npub fn fa() : Int = 0 + 1\n' > "$pkg/lib/ca.kai"
scenario edit-one-closure 1
if [ "$(cat "$work/edit-one-closure-memo.n")" -ne 1 ]; then
  echo "  FAIL edit-one-closure: a module one test file imports re-ran $(cat "$work/edit-one-closure-memo.n") checks, not 1"
  fail=$((fail + 1))
fi

scenario env-changed 1 KAI_MEMO_PROBE=1

extra="--strict-holes"
scenario argv-changed 1
extra=""

cp -R "$ROOT/stdlib" "$work/stdlib"
scenario stdlib-moved 1 KAI_STDLIB="$work/stdlib"
printf '\n#[doc("Probe.")]\npub fn memo_probe() : Int = 1\n' >> "$work/stdlib/core/list.kai"
scenario stdlib-edited 1 KAI_STDLIB="$work/stdlib"

# A corrupt memo entry is a miss, never a verdict.
for f in "$pkg"/.kai-cache/*/guards/* "$pkg"/.kai-cache/*/imports/*; do
  [ -f "$f" ] && printf 'garbage' > "$f"
done
scenario corrupt-entries 2
for f in "$pkg"/.kai-cache/*/guards/* "$pkg"/.kai-cache/*/imports/*; do
  [ -f "$f" ] && head -c 20 "$f" > "$f.cut" && mv "$f.cut" "$f"
done
scenario truncated-entries 2

run shadow shadow
if grep -q "guard memo differs" "$work/shadow.out"; then
  echo "  FAIL shadow: a hit differs from its check"; fail=$((fail + 1))
else
  echo "  ok   shadow (every hit matched its check)"
fi

if [ "$fail" -ne 0 ]; then
  echo "guard-memo FAIL ($fail)"
  exit 1
fi
echo "guard-memo OK"
