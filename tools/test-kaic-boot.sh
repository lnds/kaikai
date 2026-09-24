#!/bin/sh
# tools/test-kaic-boot.sh — hermetic test of tools/kaic-boot.sh.
#
# Drives the live script against a fake tree whose boots are shell stubs
# and whose release tarball is served over file://, so every resolution
# path and every identity check runs in seconds with no network and no
# compiler. The contract under test: a stage2.c is reused only on an exact
# match of boot and input content, and each KAIC_BOOT mode picks the boot
# it documents.

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

out="$root/stage2/build/stage2.c"
GOOD_URL="file://$dist"
BAD_URL="file://$work/nowhere"

# boot <mode> <cmd> [args] — run the script under KAIC_BOOT=<mode> ("" = unset).
boot() {
  mode="$1"; shift
  if [ -n "$mode" ]; then
    KAIC_BOOT_ROOT="$root" KAIC_BOOT_URL="${URL:-$GOOD_URL}" KAIC_BOOT="$mode" "$BOOT_SH" "$@"
  else
    (unset KAIC_BOOT; KAIC_BOOT_ROOT="$root" KAIC_BOOT_URL="${URL:-$GOOD_URL}" "$BOOT_SH" "$@")
  fi
}
# A prefix assignment to a function call persists in POSIX sh; scope it.
offline() { (URL="$BAD_URL"; boot "$@"); }
fresh()   { boot "$1" fresh "$out" 2>/dev/null; }
is_fresh()  { fresh "$1" || fail "$2: expected fresh under KAIC_BOOT=${1:-<unset>}"; }
is_stale()  { if fresh "$1"; then fail "$2: expected stale under KAIC_BOOT=${1:-<unset>}"; fi; }
boot_of() { sed -n 's/^boot=//p' "$out.id"; }

# ---- kaic1: record, content checks, strict vs lenient -------------------
boot "" emit "$out" 2>/dev/null
grep -q '^/\* kaic1 \*/' "$out" || fail "unset mode did not run kaic1"
case "$(boot_of)" in "kaic1 "*) ;; *) fail "kaic1 record names boot [$(boot_of)]" ;; esac
is_fresh "" "kaic1 record"; is_fresh kaic1 "kaic1 record"; is_fresh auto "kaic1 record"
is_stale release "kaic1 record under release"
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
is_fresh "" "no record, unset"; is_fresh auto "no record, auto"
is_stale kaic1 "no record, explicit kaic1"; is_stale release "no record, explicit release"
ok "without a record only unset/auto defer to make; an explicit boot rebuilds"

# ---- release: download, verify, cache, stdlib, identity -----------------
boot release emit "$out" 2>/dev/null || fail "release emit failed"
grep -q "release stdlib=$root/stdlib args=--edition hanga-roa main.kai" "$out" \
  || fail "release boot did not run with this tree's stdlib and edition: $(head -1 "$out")"
case "$(boot_of)" in "release kaikai-v9.9.9-"*) ;; *) fail "release record names boot [$(boot_of)]" ;; esac
grep -q '^class=kaic2$' "$out.id" || fail "release record is not kaic2-class"
is_fresh release "release record"; is_stale kaic1 "release record under kaic1"; is_stale "" "release record, unset"
ok "release boot fetched, verified, run against this tree's stdlib"

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
is_stale release "VERSION names another release"
echo 9.9.9 > "$root/VERSION"
ok "a boot of another release is never reused"

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
grep -q "^/\* path stdlib=$root/stdlib args=--edition" "$out" || fail "path boot not run as kaic2-class"
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

echo "test-kaic-boot: all cases passed"
