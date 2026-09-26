#!/bin/sh
# tools/test-kaic-boot.sh — hermetic test of tools/kaic-boot.sh.
#
# Drives the live script against a fake tree whose boots and cc are shell
# stubs and whose release tarball is served over file://, so every
# resolution path and every identity check runs in seconds with no network
# and no compiler. The contract under test: a stage2.c is reused only on an
# exact match of boot, hop count and input content, each KAIC_BOOT mode
# picks the boot it documents, and a kaic2-class boot's C is emitted by the
# kaic2-a linked from the boot's own C.

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BOOT_SH="$SCRIPT_DIR/kaic-boot.sh"

fail() { echo "test-kaic-boot: FAIL: $1" >&2; exit 1; }
ok()   { echo "test-kaic-boot: ok — $1"; }

work="$(mktemp -d "${TMPDIR:-/tmp}/kaic-boot-test.XXXXXX")"
trap 'rm -rf "$work"' EXIT INT TERM

root="$work/tree"
dist="$work/dist"
mkdir -p "$root/stage1" "$root/stage2/compiler" "$root/stdlib/core" "$dist"
echo 9.9.9     > "$root/VERSION"
echo hanga-roa > "$root/EDITION"
echo 'fn main() = 0'  > "$root/stage2/main.kai"
echo 'fn a() = 1'     > "$root/stage2/compiler/a.kai"
cat > "$root/stage2/compiler/driver.kai" <<'EOF'
fn core_module_files() : [String] = [
  "core/x.kai",
  "effects.kai"
]
EOF
echo 'fn x() = 1'  > "$root/stdlib/core/x.kai"
echo 'effect E {}' > "$root/stdlib/effects.kai"
echo 'fn o() = 1'  > "$root/stdlib/other.kai"

printf 'kaic1:\n\t@:\n' > "$root/stage1/Makefile"
cat > "$root/stage1/kaic1" <<'EOF'
#!/bin/sh
echo "/* kaic1 */"; cat "$1"
EOF

# A kaic2-class stub: reports self-hosted and echoes what it was run with.
make_kaic2() {
  cat > "$1" <<EOF
#!/bin/sh
[ "\$1" = --version ] && { echo "kaic2 stage 2 (self-hosted)"; exit 0; }
echo "/* $2 stdlib=\$KAIKAI_STDLIB_PATH args=\$* */"; cat "\$3"
EOF
  chmod +x "$1"
}
chmod +x "$root/stage1/kaic1"

# A stub cc for the boot-hop target: the "binary" of a C file is a
# kaic2-class stub that prints a hop-a header, then that C. Each link is
# logged with the runtime it was given ("inc=" is this tree's).
HOP_LOG="$work/cc.log"
export HOP_LOG
cat > "$work/hop-cc" <<'EOF'
#!/bin/sh
echo "$1 inc=${3:-}" >> "$HOP_LOG"
{ echo '#!/bin/sh'
  echo '[ "$1" = --version ] && { echo "kaic2 stage 2 (self-hosted)"; exit 0; }'
  echo 'echo "/* hop-a stdlib=$KAIKAI_STDLIB_PATH args=$* */"'
  echo "cat '$1'"
} > "$2"
chmod +x "$2"
EOF
chmod +x "$work/hop-cc"
printf 'boot-hop:\n\t@%s $(HOP_C) $(HOP_BIN) $(HOP_INC)\n' "$work/hop-cc" > "$root/stage2/Makefile"

# Release tarballs for every platform the script knows, so the test runs
# on any published host.
for plat in darwin-arm64 linux-x86_64; do
  name="kaikai-v9.9.9-$plat"
  mkdir -p "$work/pkg/$name/libexec/kaikai"
  make_kaic2 "$work/pkg/$name/libexec/kaikai/kaic2" release
  mkdir -p "$dist/v9.9.9"
  (cd "$work/pkg" && tar -czf "$dist/v9.9.9/$name.tar.gz" "$name")
  (cd "$dist/v9.9.9" && { sha256sum "$name.tar.gz" 2>/dev/null || shasum -a 256 "$name.tar.gz"; } > "$name.tar.gz.sha256")
done
printf '{\n  "schema": 1,\n  "version": "9.9.9",\n  "tag": "v9.9.9"\n}\n' > "$dist/latest.json"

out="$root/stage2/build/stage2.c"
GOOD_URL="file://$dist"
BAD_URL="file://$work/nowhere"
GOOD_LATEST="file://$dist/latest.json"
BAD_LATEST="file://$work/nowhere/latest.json"

# boot <mode> <cmd> [args] — run the script under KAIC_BOOT=<mode> ("" = unset).
boot() {
  mode="$1"; shift
  if [ -n "$mode" ]; then
    KAIC_BOOT_ROOT="$root" KAIC_BOOT_URL="${URL:-$GOOD_URL}" KAIC_BOOT_LATEST_URL="${LATEST:-$GOOD_LATEST}" \
      KAIC_BOOT="$mode" "$BOOT_SH" "$@"
  else
    (unset KAIC_BOOT; KAIC_BOOT_ROOT="$root" KAIC_BOOT_URL="${URL:-$GOOD_URL}" \
      KAIC_BOOT_LATEST_URL="${LATEST:-$GOOD_LATEST}" "$BOOT_SH" "$@")
  fi
}
# A prefix assignment to a function call persists in POSIX sh; scope it.
offline() { (URL="$BAD_URL"; LATEST="$BAD_LATEST"; boot "$@"); }
no_manifest() { (LATEST="$BAD_LATEST"; boot "$@"); }
fresh()   { boot "$1" fresh "$out" 2>/dev/null; }
is_fresh()  { fresh "$1" || fail "$2: expected fresh under KAIC_BOOT=${1:-<unset>}"; }
is_stale()  { if fresh "$1"; then fail "$2: expected stale under KAIC_BOOT=${1:-<unset>}"; fi; }
boot_of() { sed -n 's/^boot=//p' "$out.id"; }
hop_a="$root/stage2/build/stage2-a.c"

# ---- kaic1: record, content checks, strict vs lenient -------------------
boot kaic1 emit "$out" 2>/dev/null
grep -q '^/\* kaic1 \*/' "$out" || fail "kaic1 mode did not run kaic1"
case "$(boot_of)" in "kaic1 "*) ;; *) fail "kaic1 record names boot [$(boot_of)]" ;; esac
grep -q '^hops=1$' "$out.id" || fail "kaic1 record does not state one hop"
[ ! -f "$hop_a" ] || fail "a kaic1-class boot took a second hop"
is_fresh kaic1 "kaic1 record"; is_fresh auto "kaic1 record"
is_stale "" "kaic1 record, unset"; is_stale release "kaic1 record under release"
ok "kaic1 emit records its boot; only a matching mode reuses it"

echo 'fn x() = 2' > "$root/stdlib/core/x.kai"
is_fresh kaic1 "kaic1 output does not read the stdlib"
echo 'fn a() = 2' > "$root/stage2/compiler/a.kai"
is_stale kaic1 "compiler source edit"
boot kaic1 emit "$out" 2>/dev/null
echo '/* edited */' >> "$out"
is_stale kaic1 "stage2.c edited after emit"
ok "a compiler-source edit or an edited stage2.c invalidates; a stdlib edit does not (kaic1)"

rm -f "$out.id"
is_fresh auto "no record, auto"
is_stale "" "no record, unset"; is_stale kaic1 "no record, explicit kaic1"; is_stale release "no record, explicit release"
ok "without a record only auto defers to make; every other mode rebuilds"

# ---- release: download, verify, cache, stdlib, identity -----------------
boot release emit "$out" 2>/dev/null || fail "release emit failed"
[ "$(head -n 1 "$hop_a")" = "/* release stdlib=$root/stdlib args=--edition hanga-roa main.kai */" ] \
  || fail "release boot did not run with this tree's stdlib and edition: $(head -n 1 "$hop_a")"
[ "$(head -n 1 "$out")" = "/* hop-a stdlib=$root/stdlib args=--edition hanga-roa main.kai */" ] \
  || fail "stage2.c was not emitted by kaic2-a with this tree's stdlib and edition: $(head -n 1 "$out")"
sed 1d "$out" | cmp -s - "$hop_a" || fail "kaic2-a was not linked from the boot's stage2-a.c"
case "$(boot_of)" in "release kaikai-v9.9.9-"*) ;; *) fail "release record names boot [$(boot_of)]" ;; esac
grep -q '^class=kaic2$' "$out.id" || fail "release record is not kaic2-class"
grep -q '^hops=2$' "$out.id" || fail "release record does not state two hops"
is_fresh release "release record"; is_fresh "" "release record, unset"; is_stale kaic1 "release record under kaic1"
boot "" emit "$out" 2>/dev/null
case "$(boot_of)" in "release "*) ;; *) fail "unset mode booted [$(boot_of)]" ;; esac
ok "release boot (the default) fetched, verified, run against this tree's stdlib; its kaic2-a emits stage2.c"

cp "$out.id" "$work/rec"
sed '/^hops=/d' "$work/rec" > "$out.id"
is_stale release "kaic2-class record without a hop count"
sed 's/^hops=2$/hops=1/' "$work/rec" > "$out.id"
is_stale release "kaic2-class record of one hop"
cp "$work/rec" "$out.id"
ok "a kaic2-class C that did not take two hops is never reused"

echo 'fn o() = 2' > "$root/stdlib/other.kai"
is_fresh release "non-core stdlib edit"
echo 'fn x() = 3' > "$root/stdlib/core/x.kai"
is_stale release "core stdlib edit"
offline release emit "$out" 2>/dev/null || fail "cached release boot needed the network"
is_fresh release "re-emit from cache"
ok "a core stdlib edit invalidates a kaic2-class stage2.c; the cached tarball is reused offline"

cached="$(ls -d "$root/stage2/build/boot"/kaikai-v9.9.9-*)"
echo tampered >> "$cached/libexec/kaikai/kaic2"
if offline release emit "$out" 2>/dev/null; then fail "a tampered cached boot was reused"; fi
boot release emit "$out" 2>/dev/null || fail "tampered cache was not re-fetched"
ok "a cached boot that no longer hashes to its verified kaic2 is re-fetched, never reused"

echo 9.9.10 > "$root/VERSION"
is_fresh release "a published release preceding VERSION"
echo 9.9.8 > "$root/VERSION"
is_stale release "a release following VERSION"
echo 9.9.9 > "$root/VERSION"
ok "a boot of a release up to VERSION is reused, of a later release never"

# ---- release: VERSION not published yet ---------------------------------
echo 9.9.10 > "$root/VERSION"
echo 'fn a() = 3' > "$root/stage2/compiler/a.kai"
boot release emit "$out" 2>/dev/null || fail "an unpublished VERSION found no release boot"
case "$(boot_of)" in "release kaikai-v9.9.9-"*) ;; *) fail "an unpublished VERSION booted [$(boot_of)]" ;; esac
is_fresh release "boot of the newest published release"
id="$(boot release release-id 2>/dev/null)" || fail "release-id failed for an unpublished VERSION"
name="${id% *}"
case "$name" in kaikai-v9.9.9-*) ;; *) fail "release-id names [$id]" ;; esac
[ "${id#* }" = "$(awk '{print $1}' "$dist/v9.9.9/$name.tar.gz.sha256")" ] \
  || fail "release-id [$id] disagrees with the published sha256"
rm -rf "$root/stage2/build/boot"
if no_manifest release emit "$out" 2>/dev/null; then fail "an unpublished VERSION booted without the release manifest"; fi
echo 9.9.8 > "$root/VERSION"
if boot release emit "$out" 2>/dev/null; then fail "a release following an unpublished VERSION was booted"; fi
echo 9.9.9 > "$root/VERSION"
ok "an unpublished VERSION boots the newest published release when it precedes VERSION, else nothing"

rm -rf "$root/stage2/build/boot"
for f in "$dist"/v9.9.9/*.sha256; do
  echo "0000000000000000000000000000000000000000000000000000000000000000  x" > "$f"
done
if boot release emit "$out" 2>/dev/null; then fail "checksum mismatch accepted (release)"; fi
if boot auto emit "$out" 2>/dev/null; then fail "checksum mismatch fell back (auto)"; fi
[ ! -d "$root/stage2/build/boot/kaikai-v9.9.9-darwin-arm64" ] && [ ! -d "$root/stage2/build/boot/kaikai-v9.9.9-linux-x86_64" ] \
  || fail "a boot failing its checksum was left in the cache"
for plat in darwin-arm64 linux-x86_64; do
  name="kaikai-v9.9.9-$plat"
  (cd "$dist/v9.9.9" && { sha256sum "$name.tar.gz" 2>/dev/null || shasum -a 256 "$name.tar.gz"; } > "$name.tar.gz.sha256")
done
ok "a checksum mismatch is an error in every mode, never a fallback"

# ---- auto: self, then release, then the kaic1 chain ---------------------
make_kaic2 "$root/stage2/kaic2" self
boot release emit "$out" 2>/dev/null
boot release seal "$out" "$root/stage2/kaic2"
grep -q '^bin=' "$root/stage2/build/kaic2.id" || fail "seal wrote no bin identity"
boot auto emit "$out" 2>/dev/null
case "$(boot_of)" in "self "*) ;; *) fail "auto with a sealed kaic2 picked [$(boot_of)]" ;; esac
is_fresh auto "self record"; is_stale release "self record under release"
ok "auto picks the sealed kaic2 first"

echo '# rebuilt elsewhere' >> "$root/stage2/kaic2"
boot auto emit "$out" 2>/dev/null
case "$(boot_of)" in "release "*) ;; *) fail "auto with an unsealed kaic2 picked [$(boot_of)]" ;; esac
rm -rf "$root/stage2/build/boot"
offline auto emit "$out" 2>/dev/null
case "$(boot_of)" in "kaic1 "*) ;; *) fail "auto offline picked [$(boot_of)]" ;; esac
if offline release emit "$out" 2>/dev/null; then fail "release mode fell back offline"; fi
ok "auto falls to release when kaic2 is unsealed, to kaic1 when release is unreachable; release mode does not"

rm -f "$out.id"
boot "" seal "$out" "$root/stage2/kaic2"
[ ! -f "$root/stage2/build/kaic2.id" ] || fail "seal kept a kaic2 identity for an unrecorded stage2.c"
ok "a kaic2 linked from an unrecorded stage2.c loses its seal"

# ---- path -------------------------------------------------------------
make_kaic2 "$work/mykaic2" path
boot "$work/mykaic2" emit "$out" 2>/dev/null
grep -q "^/\* path stdlib=$root/stdlib args=--edition" "$hop_a" || fail "path boot not run as kaic2-class"
grep -q '^hops=2$' "$out.id" || fail "a kaic2-class path boot did not take two hops"
is_fresh "$work/mykaic2" "path record"
echo '# other build' >> "$work/mykaic2"
is_stale "$work/mykaic2" "path boot content changed"
boot "$root/stage1/kaic1" emit "$out" 2>/dev/null
grep -q '^class=kaic1$' "$out.id" || fail "a kaic1 path boot was not detected as kaic1-class"
ok "a path boot is identified by content and classified by --version"

# ---- an input edited while the boot runs ------------------------------
cat > "$work/editing-kaic1" <<EOF
#!/bin/sh
[ "\$1" = --version ] && exit 1
echo 'fn a() = 99' > "$root/stage2/compiler/a.kai"
echo "/* editing */"
EOF
chmod +x "$work/editing-kaic1"
boot "$work/editing-kaic1" emit "$out" 2>/dev/null
is_stale "$work/editing-kaic1" "input edited while the boot ran"
ok "an input edited mid-emit leaves the record stale, never fresh"

# ---- seed: the newest tag up to VERSION, linked against its own runtime --
g() { git -C "$root" -c user.name=t -c user.email=t@example.com -c commit.gpgsign=false -c tag.gpgsign=false "$@"; }
g init -q
g add -A
g commit -q -m base
base="$(g rev-parse HEAD)"
# seed_tag <version> [marker]: a commit off base carrying bootstrap/, tagged.
seed_tag() {
  mkdir -p "$root/bootstrap"
  echo "/* seed $1${2:-} */" > "$root/bootstrap/stage2.c"
  echo "/* runtime $1 */"    > "$root/bootstrap/runtime.h"
  g add bootstrap
  g commit -q -m "seed $1"
  g tag -f "bootstrap-seed-v$1" >/dev/null
  g checkout -q --detach "$base"
}
seed_tag 9.9.8; seed_tag 9.9.9; seed_tag 9.9.10
seed_dir="$root/stage2/build/boot/bootstrap-seed-v9.9.9"

: > "$HOP_LOG"
boot seed emit "$out" 2>/dev/null || fail "seed emit failed"
case "$(boot_of)" in "seed bootstrap-seed-v9.9.9 "*) ;; *) fail "seed record names boot [$(boot_of)]" ;; esac
grep -q '^class=kaic2$' "$out.id" && grep -q '^hops=2$' "$out.id" || fail "seed record is not a two-hop kaic2-class boot"
grep -qx '/\* seed 9.9.9 \*/' "$hop_a" || fail "stage2-a.c was not emitted by the 9.9.9 seed"
[ "$(head -n 1 "$hop_a")" = "/* hop-a stdlib=$root/stdlib args=--edition hanga-roa main.kai */" ] \
  || fail "the seed did not run with this tree's stdlib and edition: $(head -n 1 "$hop_a")"
sed 1d "$out" | cmp -s - "$hop_a" || fail "kaic2-a was not linked from the seed's stage2-a.c"
grep -qx "$seed_dir/bootstrap/stage2.c inc=$seed_dir/bootstrap" "$HOP_LOG" \
  || fail "the seed was not linked against its own runtime.h: $(cat "$HOP_LOG")"
grep -qx "$(cd "$root/stage2/build" && pwd)/stage2-a.c inc=" "$HOP_LOG" || fail "hop a was not linked against this tree's runtime.h: $(cat "$HOP_LOG")"
is_fresh seed "seed record"; is_fresh auto "seed record"
is_stale "" "seed record, unset"; is_stale release "seed record under release"
ok "seed boot takes the newest tag up to VERSION, linked against its own runtime.h; its kaic2-a emits stage2.c"

: > "$HOP_LOG"
boot seed emit "$out" 2>/dev/null
if grep -q 'bootstrap/stage2.c' "$HOP_LOG"; then fail "a cached seed was relinked"; fi
echo tampered >> "$seed_dir/kaic2"
boot seed emit "$out" 2>/dev/null
grep -q 'bootstrap/stage2.c' "$HOP_LOG" || fail "a tampered cached seed was reused"
: > "$HOP_LOG"
seed_tag 9.9.9 " moved"
boot seed emit "$out" 2>/dev/null
grep -qx '/\* seed 9.9.9 moved \*/' "$hop_a" || fail "a moved seed tag reused the old seed"
ok "the linked seed is cached per tag; a tampered binary or a moved tag relinks it"

echo 9.9.8 > "$root/VERSION"
is_stale seed "a seed following VERSION"
boot seed emit "$out" 2>/dev/null
case "$(boot_of)" in "seed bootstrap-seed-v9.9.8 "*) ;; *) fail "VERSION 9.9.8 booted [$(boot_of)]" ;; esac
echo 9.9.9 > "$root/VERSION"
is_fresh seed "a seed preceding VERSION"
ok "a seed up to VERSION is reused, a later one never"

rm -rf "$root/stage2/build/boot"
offline auto emit "$out" 2>/dev/null
case "$(boot_of)" in "seed "*) ;; *) fail "auto offline with a seed picked [$(boot_of)]" ;; esac
for v in 9.9.8 9.9.9 9.9.10; do g tag -d "bootstrap-seed-v$v" >/dev/null; done
if boot seed emit "$out" 2>/dev/null; then fail "seed mode booted without a seed tag"; fi
offline auto emit "$out" 2>/dev/null
case "$(boot_of)" in "kaic1 "*) ;; *) fail "auto offline without a seed picked [$(boot_of)]" ;; esac
ok "auto falls to the seed when release is unreachable, then to kaic1; seed mode does not fall back"

echo "test-kaic-boot: all cases passed"
