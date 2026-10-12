#!/bin/sh
# The memo of `kai build`'s binaries hands back the binary a build would
# produce, and misses whenever an input of the build changed: a source the
# entry imports, a flag, the build mode, the `KAI` environment, the name
# of the output (the linker signs a binary under it). A hit prints the
# compiler's diagnostics as the build did, leaves a binary that runs where
# one already ran, and a `--release` hit is stripped like its build.
#
# Every compiler `kai build` starts goes through a shim that logs the
# builds it really runs. Each scenario builds with the memo on and then
# off (the oracle): what the build prints, its status and what the binary
# prints must be identical, and a change to any input must rebuild. A
# rebuild with nothing changed builds nothing.

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
calls="$work/calls.log"
cat > "$lay/stage2/kaic2" <<SHIM
#!/bin/sh
echo call >> "$calls"
case " \$* " in
  *" --guard-inputs "*|*" --guard-core-inputs "*) ;;
  *) echo build >> "$log"
     if [ -f "$work/write-during" ]; then
       rm -f "$work/write-during"
       touch "$work/solo/lib/util.kai"
     fi ;;
esac
exec "$ROOT/stage2/kaic2" "\$@"
SHIM
chmod +x "$lay/stage2/kaic2"

solo="$work/solo"
mkdir -p "$solo/lib" "$work/out"
printf 'import lib.util\n\nfn main() : Unit / Stdout = Stdout.print(int_to_string(util.add(1, 2)))\n' > "$solo/main.kai"
printf '#[doc("Adds.")]\npub fn add(a: Int, b: Int) : Int = a + b\n' > "$solo/lib/util.kai"

pkg="$work/pkg"
mkdir -p "$pkg"
printf 'name = "buildmemo"\nentry = "main.kai"\n' > "$pkg/kai.toml"
printf 'fn main() : Unit / Stdout = Stdout.print("pkg")\n' > "$pkg/main.kai"

fail=0
builds() { if [ -f "$log" ]; then wc -l < "$log" | tr -d ' '; else echo 0; fi; }

# build <label> <memo-mode> <dir> <binary> [env...] -- [kai build args...]
build() {
  bl="$1"; bmode="$2"; bdir="$3"; bbin="$4"; shift 4
  benv=""
  while [ "$1" != "--" ]; do benv="$benv $1"; shift; done
  shift
  : > "$log"
  brc=0
  # shellcheck disable=SC2086
  (cd "$bdir" && env KAI_BACKEND=c KAI_CORE_CACHE_DIR="$work/core" KAI_BIN_MEMO="$bmode" $benv \
    "$lay/bin/kai" build "$@") > "$work/$bl.out" 2>&1 || brc=$?
  echo "$brc" > "$work/$bl.rc"
  builds > "$work/$bl.n"
  if [ "$brc" -eq 0 ]; then "$bbin" > "$work/$bl.ran" 2>&1 || echo "status $?" >> "$work/$bl.ran"
  else : > "$work/$bl.ran"; fi
  wc -c < "$bbin" | tr -d ' ' > "$work/$bl.size" 2> /dev/null || echo 0 > "$work/$bl.size"
}

# scenario <label> <min-builds> <dir> <binary> [env...] -- [kai build args...]:
# the memo agrees with the oracle; a minimum of 0 means the state was
# built before, so nothing may rebuild.
scenario() {
  label="$1"; min="$2"; shift 2
  build "$label-memo" on "$@"
  build "$label-off" 0 "$@"
  n=$(cat "$work/$label-memo.n")
  if ! cmp -s "$work/$label-memo.out" "$work/$label-off.out" \
     || ! cmp -s "$work/$label-memo.rc" "$work/$label-off.rc" \
     || ! cmp -s "$work/$label-memo.ran" "$work/$label-off.ran" \
     || ! cmp -s "$work/$label-memo.size" "$work/$label-off.size"; then
    echo "  FAIL $label: the memoised build differs from the unmemoised one"
    diff "$work/$label-off.out" "$work/$label-memo.out" | head -10
    diff "$work/$label-off.ran" "$work/$label-memo.ran" | head -5
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

app="$work/out/app"
scenario cold 1 "$solo" "$app" -- main.kai -o "$app"
grep -q '^3$' "$work/cold-off.ran" || { echo "  FAIL cold: unexpected output"; cat "$work/cold-off.ran"; fail=$((fail + 1)); }
scenario warm 0 "$solo" "$app" -- main.kai -o "$app"
# A build that is kept is found by the sources it read: the compiler is
# not started at all, not even to list them.
: > "$calls"
build quiet on "$solo" "$app" -- main.kai -o "$app"
[ ! -s "$calls" ] || { echo "  FAIL quiet: a kept build started the compiler $(wc -l < "$calls" | tr -d ' ') times"; fail=$((fail + 1)); }
scenario other-name 1 "$solo" "$work/out/tool" -- main.kai -o "$work/out/tool"
scenario other-dir 0 "$solo" "$work/app" -- main.kai -o "$work/app"

printf '#[doc("Adds.")]\npub fn add(a: Int, b: Int) : Int = a * b\n' > "$solo/lib/util.kai"
scenario edit-import 1 "$solo" "$app" -- main.kai -o "$app"
grep -q '^2$' "$work/edit-import-memo.ran" || { echo "  FAIL edit-import: stale binary"; fail=$((fail + 1)); }
printf '#[doc("Adds.")]\npub fn add(a: Int, b: Int) : Int = a + b\n' > "$solo/lib/util.kai"
scenario import-reverted 0 "$solo" "$app" -- main.kai -o "$app"
grep -q '^3$' "$work/import-reverted-memo.ran" || { echo "  FAIL import-reverted: a hit left the edited binary"; fail=$((fail + 1)); }

# A source written while its build runs is not what the binary came from:
# that build is not kept, and the next one builds again.
printf '#[doc("Adds.")]\npub fn add(a: Int, b: Int) : Int = a + b + 10\n' > "$solo/lib/util.kai"
: > "$work/write-during"
scenario written-during 1 "$solo" "$app" -- main.kai -o "$app"
scenario after-written 1 "$solo" "$app" -- main.kai -o "$app"
scenario written-settled 0 "$solo" "$app" -- main.kai -o "$app"
printf '#[doc("Adds.")]\npub fn add(a: Int, b: Int) : Int = a + b\n' > "$solo/lib/util.kai"

# A path the compiler found missing is an input too: a module that
# appears ahead of the one a build read takes its place, and the build
# comes back when it goes.
printf 'import path\n\nfn main() : Unit / Stdout = Stdout.print(path.basename("a/b"))\n' > "$solo/shadow.kai"
scenario shadow-cold 1 "$solo" "$app" -- shadow.kai -o "$app"
scenario shadow-warm 0 "$solo" "$app" -- shadow.kai -o "$app"
printf '#[doc("The local one.")]\npub fn basename(p: String) : String = "local #{p}"\n' > "$solo/path.kai"
scenario shadowed 1 "$solo" "$app" -- shadow.kai -o "$app"
grep -q '^local a/b$' "$work/shadowed-memo.ran" || { echo "  FAIL shadowed: the build did not see the module that appeared"; fail=$((fail + 1)); }
rm "$solo/path.kai"
scenario shadow-gone 0 "$solo" "$app" -- shadow.kai -o "$app"
grep -q '^b$' "$work/shadow-gone-memo.ran" || { echo "  FAIL shadow-gone: a hit left the shadowed binary"; fail=$((fail + 1)); }

# A module's imports resolve from the entry's directory, not from its
# own: the file the compile read is the input, whatever sits beside the
# importer under the same name.
mkdir -p "$solo/deep"
printf 'import deep.mid\n\nfn main() : Unit / Stdout = Stdout.print(mid.who())\n' > "$solo/nested.kai"
printf 'import leaf\n\n#[doc("Who.")]\npub fn who() : String = leaf.name()\n' > "$solo/deep/mid.kai"
printf '#[doc("Name.")]\npub fn name() : String = "one"\n' > "$solo/leaf.kai"
printf '#[doc("Name.")]\npub fn name() : String = "beside"\n' > "$solo/deep/leaf.kai"
scenario nested-cold 1 "$solo" "$app" -- nested.kai -o "$app"
printf '#[doc("Name.")]\npub fn name() : String = "two"\n' > "$solo/leaf.kai"
scenario nested-edited 1 "$solo" "$app" -- nested.kai -o "$app"
grep -q '^two$' "$work/nested-edited-memo.ran" || { echo "  FAIL nested-edited: a hit left the binary of the old module"; fail=$((fail + 1)); }

scenario flag-release 1 "$solo" "$app" -- --release main.kai -o "$app"
scenario release-warm 0 "$solo" "$app" -- --release main.kai -o "$app"
scenario mode-debug 1 "$solo" "$app" -- --debug main.kai -o "$app"
scenario env-changed 1 "$solo" "$app" KAI_MEMO_PROBE=1 -- main.kai -o "$app"
scenario stats 1 "$solo" "$app" KAI_CORE_CACHE_STATS=1 -- main.kai -o "$app"
scenario stats-again 1 "$solo" "$app" KAI_CORE_CACHE_STATS=1 -- main.kai -o "$app"
# A build asked to write something besides its binary always compiles.
scenario dump 1 "$solo" "$app" KAI_NATIVE_DUMP_IR="$work/app.ll" -- main.kai -o "$app"
scenario dump-again 1 "$solo" "$app" KAI_NATIVE_DUMP_IR="$work/app.ll" -- main.kai -o "$app"

printf 'fn main() : Unit / Stdout = {\n  let unused = 3\n  Stdout.print("w")\n}\n' > "$solo/warn.kai"
scenario warning-cold 1 "$solo" "$app" -- warn.kai -o "$app"
scenario warning-warm 0 "$solo" "$app" -- warn.kai -o "$app"
grep -q 'unused binding' "$work/warning-warm-memo.out" \
  || { echo "  FAIL warning-warm: a hit dropped the compiler's warning"; fail=$((fail + 1)); }
scenario back-to-plain 0 "$solo" "$app" -- main.kai -o "$app"

for f in "$work"/core/*/bins/*/bin; do
  [ -f "$f" ] && head -c 100 "$f" > "$f.cut" && mv "$f.cut" "$f"
done
scenario truncated-entries 1 "$solo" "$app" -- main.kai -o "$app"

# The first build in a package builds the package tools it reports on.
build pkg-warmup 0 "$pkg" "$pkg/buildmemo" -- main.kai
rm -rf "$pkg/.kai-cache"
scenario pkg-cold 1 "$pkg" "$pkg/buildmemo" --
scenario pkg-warm 0 "$pkg" "$pkg/buildmemo" --
ls -d "$pkg"/.kai-cache/*/bins/* > /dev/null 2>&1 \
  || { echo "  FAIL pkg: no entry in the package's user cache"; fail=$((fail + 1)); }

# The manifest is a link input whose digest is kept under its state on
# disk. An edit to it rebuilds, and so does a second edit of the same
# size straight after the first, which a coarse clock would not show.
printf 'name = "buildmemo"\nentry = "main.kai"\n# a\n' > "$pkg/kai.toml"
scenario manifest-edited 1 "$pkg" "$pkg/buildmemo" --
printf 'name = "buildmemo"\nentry = "main.kai"\n# b\n' > "$pkg/kai.toml"
scenario manifest-same-size 1 "$pkg" "$pkg/buildmemo" --
scenario manifest-warm 0 "$pkg" "$pkg/buildmemo" --
printf 'name = "buildmemo"\nentry = "main.kai"\n# a\n' > "$pkg/kai.toml"
scenario manifest-reverted 0 "$pkg" "$pkg/buildmemo" --

build off 0 "$solo" "$app" -- main.kai -o "$app"
if [ "$(cat "$work/off.n")" -lt 1 ]; then
  echo "  FAIL off: KAI_BIN_MEMO=0 reused a binary"; fail=$((fail + 1))
else
  echo "  ok   off (the binary built)"
fi

if [ "$fail" -ne 0 ]; then
  echo "build-memo FAIL ($fail)"
  exit 1
fi
echo "build-memo OK"
