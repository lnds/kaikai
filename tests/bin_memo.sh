#!/bin/sh
# The memo of `kai test`'s binaries hands back the binary a build would
# produce, and misses whenever an input of the build changed.
#
# Every compiler `kai test` starts goes through a shim that logs the
# builds it really runs. Each scenario runs the package with the memo on
# and then off (the oracle): outputs and statuses must be identical, and
# an edit to any input must rebuild at least the binaries that read it.
# A warm run with nothing changed builds nothing.

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
log="$work/builds.log"
cat > "$lay/stage2/kaic2" <<EOF
#!/bin/sh
case " \$* " in
  *" --guard-inputs "*|*" --guard-core-inputs "*|*" --check "*) ;;
  *" --test "*) echo build >> "$log" ;;
esac
exec "$ROOT/stage2/kaic2" "\$@"
EOF
chmod +x "$lay/stage2/kaic2"

pkg="$work/pkg"
mkdir -p "$pkg/lib" "$pkg/tests"
printf 'name = "binmemo"\nentry = "main.kai"\n' > "$pkg/kai.toml"
printf 'import lib.util\n\nfn main() : Unit / Stdout = Stdout.print(int_to_string(util.add(1, 2)))\n' > "$pkg/main.kai"
printf '#[doc("Adds.")]\npub fn add(a: Int, b: Int) : Int = a + b\n' > "$pkg/lib/util.kai"
printf 'import lib.util\n\ntest "adds" {\n  assert util.add(2, 2) == 4\n}\n' > "$pkg/tests/a_test.kai"
printf 'test "counts" {\n  assert [1, 2, 3].length() == 3\n}\n' > "$pkg/tests/b_test.kai"
printf 'import lib.util\n\ntest "solo" {\n  assert util.add(0, 1) == 1\n}\n' > "$pkg/solo_test.kai"

fail=0
builds() { if [ -f "$log" ]; then wc -l < "$log" | tr -d ' '; else echo 0; fi; }

# run <label> <memo-mode> [env...]: the run's output, status and builds.
run() {
  rl="$1"; rmode="$2"; shift 2
  : > "$log"
  rrc=0
  (cd "$pkg" && env KAI_BACKEND=c KAI_BIN_MEMO="$rmode" "$@" "$lay/bin/kai" test -j 1) \
    > "$work/$rl.out" 2>&1 || rrc=$?
  echo "$rrc" > "$work/$rl.rc"
  builds > "$work/$rl.n"
}

# scenario <label> <min-builds> [env...]: the memo agrees with the oracle;
# a minimum of 0 means the state was built before, so nothing may rebuild.
scenario() {
  label="$1"; min="$2"; shift 2
  run "$label-memo" on "$@"
  run "$label-off" 0 "$@"
  n=$(cat "$work/$label-memo.n")
  if ! cmp -s "$work/$label-memo.out" "$work/$label-off.out" \
     || ! cmp -s "$work/$label-memo.rc" "$work/$label-off.rc"; then
    echo "  FAIL $label: the memoised run differs from the unmemoised one"
    diff "$work/$label-off.out" "$work/$label-memo.out" | head -10
    fail=$((fail + 1))
  elif [ "$min" -eq 0 ] && [ "$n" -ne 0 ]; then
    echo "  FAIL $label: back to a state already built, yet $n builds ran"
    fail=$((fail + 1))
  elif [ "$n" -lt "$min" ]; then
    echo "  FAIL $label: $n builds ran, expected at least $min (a stale hit)"
    fail=$((fail + 1))
  else
    echo "  ok   $label ($n builds ran)"
  fi
}

# The first run in a fresh layout builds the package tools it reports on.
run warmup 0
rm -rf "$pkg/.kai-cache"

scenario cold 2
if ! grep -q 'adds' "$work/cold-off.out" || ! grep -q 'counts' "$work/cold-off.out"; then
  echo "  FAIL cold: the grouped files did not both run"; fail=$((fail + 1))
fi
scenario warm 0

printf 'import lib.util\n\ntest "adds" {\n  assert util.add(2, 2) == 5\n}\n' > "$pkg/tests/a_test.kai"
scenario edit-test-file 1
printf 'import lib.util\n\ntest "adds" {\n  assert util.add(2, 2) == 4\n}\n' > "$pkg/tests/a_test.kai"
scenario edit-reverted 0

printf '#[doc("Adds.")]\npub fn add(a: Int, b: Int) : Int = a * b\n' > "$pkg/lib/util.kai"
scenario edit-import 2
printf '#[doc("Adds.")]\npub fn add(a: Int, b: Int) : Int = a + b\n' > "$pkg/lib/util.kai"
scenario import-reverted 0

printf '\n# probe\n' >> "$pkg/kai.toml"
scenario manifest-edited 1

scenario env-changed 1 KAI_MEMO_PROBE=1

# A damaged entry is a miss, never a binary.
for f in "$pkg"/.kai-cache/*/bins/*/bin; do
  [ -f "$f" ] && head -c 100 "$f" > "$f.cut" && mv "$f.cut" "$f"
done
scenario truncated-entries 2
for f in "$pkg"/.kai-cache/*/bins/*/meta; do
  [ -f "$f" ] && printf 'garbage' > "$f"
done
scenario corrupt-meta 2

run off 0
if [ "$(cat "$work/off.n")" -lt 2 ]; then
  echo "  FAIL off: KAI_BIN_MEMO=0 reused a binary"; fail=$((fail + 1))
else
  echo "  ok   off (every binary built)"
fi

if [ "$fail" -ne 0 ]; then
  echo "bin-memo FAIL ($fail)"
  exit 1
fi
echo "bin-memo OK"
