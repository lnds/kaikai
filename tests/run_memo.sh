#!/bin/sh
# The memo of `kai run`'s binaries hands back the binary a build would
# produce, and misses whenever an input of the build changed: a source the
# entry imports, a flag, the build mode, the `KAI` environment.
#
# Every compiler `kai run` starts goes through a shim that logs the builds
# it really runs. Each scenario runs with the memo on and then off (the
# oracle): outputs and statuses must be identical, and a change to any
# input must rebuild. A warm run with nothing changed builds nothing, also
# when only the program's own arguments differ. A standalone file keeps
# its entries in the shared core cache, a package in its user cache.

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
cat > "$lay/stage2/kaic2" <<SHIM
#!/bin/sh
case " \$* " in
  *" --guard-inputs "*|*" --guard-core-inputs "*) ;;
  *) echo build >> "$log" ;;
esac
exec "$ROOT/stage2/kaic2" "\$@"
SHIM
chmod +x "$lay/stage2/kaic2"

solo="$work/solo"
mkdir -p "$solo/lib"
printf 'import lib.util\n\nfn main() : Unit / Stdout = Stdout.print(int_to_string(util.add(1, 2)))\n' > "$solo/main.kai"
printf '#[doc("Adds.")]\npub fn add(a: Int, b: Int) : Int = a + b\n' > "$solo/lib/util.kai"

pkg="$work/pkg"
mkdir -p "$pkg"
printf 'name = "runmemo"\nentry = "main.kai"\n' > "$pkg/kai.toml"
printf 'fn main() : Unit / Stdout = Stdout.print("pkg")\n' > "$pkg/main.kai"

fail=0
builds() { if [ -f "$log" ]; then wc -l < "$log" | tr -d ' '; else echo 0; fi; }

# run <label> <memo-mode> <dir> [env...] -- [kai run args...]
run() {
  rl="$1"; rmode="$2"; rdir="$3"; shift 3
  renv=""
  while [ "$1" != "--" ]; do renv="$renv $1"; shift; done
  shift
  : > "$log"
  rrc=0
  # shellcheck disable=SC2086
  (cd "$rdir" && env KAI_BACKEND=c KAI_CORE_CACHE_DIR="$work/core" KAI_BIN_MEMO="$rmode" $renv \
    "$lay/bin/kai" run "$@") > "$work/$rl.out" 2>&1 || rrc=$?
  echo "$rrc" > "$work/$rl.rc"
  builds > "$work/$rl.n"
}

# scenario <label> <min-builds> <dir> [env...] -- [kai run args...]: the
# memo agrees with the oracle; a minimum of 0 means the state was built
# before, so nothing may rebuild.
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

scenario cold 1 "$solo" -- main.kai
grep -q '^3$' "$work/cold-off.out" || { echo "  FAIL cold: unexpected output"; cat "$work/cold-off.out"; fail=$((fail + 1)); }
scenario warm 0 "$solo" -- main.kai
scenario program-args 0 "$solo" -- main.kai -- x y

printf '#[doc("Adds.")]\npub fn add(a: Int, b: Int) : Int = a * b\n' > "$solo/lib/util.kai"
scenario edit-import 1 "$solo" -- main.kai
grep -q '^2$' "$work/edit-import-memo.out" || { echo "  FAIL edit-import: stale output"; fail=$((fail + 1)); }
printf '#[doc("Adds.")]\npub fn add(a: Int, b: Int) : Int = a + b\n' > "$solo/lib/util.kai"
scenario import-reverted 0 "$solo" -- main.kai

scenario flag-release 1 "$solo" -- --release main.kai
scenario mode-debug 1 "$solo" -- --debug main.kai
scenario env-changed 1 "$solo" KAI_MEMO_PROBE=1 -- main.kai
scenario back-to-plain 0 "$solo" -- main.kai

for f in "$work"/core/*/bins/*/bin; do
  [ -f "$f" ] && head -c 100 "$f" > "$f.cut" && mv "$f.cut" "$f"
done
scenario truncated-entries 1 "$solo" -- main.kai

# The first run in a package builds the package tools it reports on.
run pkg-warmup 0 "$pkg" -- main.kai
rm -rf "$pkg/.kai-cache"
scenario pkg-cold 1 "$pkg" -- main.kai
scenario pkg-warm 0 "$pkg" -- main.kai
ls -d "$pkg"/.kai-cache/*/bins/* > /dev/null 2>&1 \
  || { echo "  FAIL pkg: no entry in the package's user cache"; fail=$((fail + 1)); }

run off 0 "$solo" -- main.kai
if [ "$(cat "$work/off.n")" -lt 1 ]; then
  echo "  FAIL off: KAI_BIN_MEMO=0 reused a binary"; fail=$((fail + 1))
else
  echo "  ok   off (the binary built)"
fi

if [ "$fail" -ne 0 ]; then
  echo "run-memo FAIL ($fail)"
  exit 1
fi
echo "run-memo OK"
