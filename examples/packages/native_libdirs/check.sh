#!/bin/sh
# examples/packages/native_libdirs — [native] `libdirs` and `pkg-config`
# point the link at a library outside the default search path. The
# library is a static archive built here from vendored C, so no system
# dependency is involved.
#
# Arms:
#   1. relative `libdirs` — the consumer links the dep's archive.
#   2. absolute `libdirs` — same, with the dir spelled absolute.
#   3. no `libdirs` — the link fails and the note points at
#      `libdirs`/`pkg-config` instead of only "install it".
#   4. `pkg-config` — the .pc's Cflags compile the dep's shim and its
#      Libs link it (skipped without pkg-config).
#   5. unknown pkg-config name — error naming the package and the .pc
#      (skipped without pkg-config).
#   6. pkg-config absent — error naming the package and the .pc.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
KAI="$ROOT/bin/kai"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

fail=0
note() { echo "native_libdirs: FAIL — $1" >&2; fail=1; }

kai_in() {
  ki_dir="$1"; shift
  set +e
  ( cd "$ki_dir" && "$@" ) >"$TMP/.out" 2>"$TMP/.err"
  status=$?
  set -e
}

expect_run() {
  er_what="$1"; er_want="$2"; er_bin="$3"
  if [ "$status" -ne 0 ]; then
    note "$er_what: build exited $status"; cat "$TMP/.err" >&2
  else
    er_got="$("$er_bin")"
    [ "$er_got" = "$er_want" ] || note "$er_what: printed '$er_got', want $er_want"
  fi
}

cp -R "$DIR/." "$TMP/w"
W="$TMP/w"
VEND="$W/lib_vend"
mkdir -p "$VEND/lib"
${CC:-cc} -c "$W/vendor/vend.c" -o "$TMP/vend.o"
ar rcs "$VEND/lib/libvend.a" "$TMP/vend.o"

# 1. Relative libdirs.
kai_in "$W/app" "$KAI" build . -o "$TMP/app_rel"
expect_run "relative libdirs" 42 "$TMP/app_rel"

# 2. Absolute libdirs.
cp "$VEND/kai.toml" "$TMP/vend.toml"
sed "s#^libdirs = .*#libdirs = [\"$VEND/lib\"]#" "$TMP/vend.toml" > "$VEND/kai.toml"
kai_in "$W/app" "$KAI" build . -o "$TMP/app_abs"
expect_run "absolute libdirs" 42 "$TMP/app_abs"

# 3. No libdirs: the note names the manifest keys that fix it.
grep -v '^libdirs' "$TMP/vend.toml" > "$VEND/kai.toml"
kai_in "$W/app" "$KAI" build . -o "$TMP/app_none"
[ "$status" -ne 0 ] || note "build without libdirs succeeded unexpectedly"
grep -q "package 'vend' links system library 'vend'" "$TMP/.err" \
  || note "link note does not name the package and library"
grep -q "libdirs" "$TMP/.err" || note "link note does not mention libdirs"
cp "$TMP/vend.toml" "$VEND/kai.toml"

# 4 + 5. pkg-config, through a .pc describing the archive.
if command -v "${PKG_CONFIG:-pkg-config}" >/dev/null 2>&1; then
  mkdir -p "$TMP/pc"
  cat > "$TMP/pc/kaivend.pc" <<PC
Name: kaivend
Description: native_libdirs fixture archive
Version: 0.1.0
Cflags: -I$W/vendor
Libs: -L$VEND/lib -lvend
PC
  kai_in "$W/app_pc" env PKG_CONFIG_PATH="$TMP/pc" "$KAI" build . -o "$TMP/app_pc"
  expect_run "pkg-config" 84 "$TMP/app_pc"

  cp "$W/lib_pc/kai.toml" "$TMP/pc.toml"
  sed 's#"kaivend"#"kai-no-such-pc"#' "$TMP/pc.toml" > "$W/lib_pc/kai.toml"
  kai_in "$W/app_pc" env PKG_CONFIG_PATH="$TMP/pc" "$KAI" build . -o "$TMP/app_unknown"
  [ "$status" -ne 0 ] || note "unknown pkg-config name built unexpectedly"
  grep -q "package 'pcvend' declares pkg-config 'kai-no-such-pc'" "$TMP/.err" \
    || { note "unknown pkg-config error does not name the package"; cat "$TMP/.err" >&2; }
  grep -q "kai-no-such-pc.pc" "$TMP/.err" || note "unknown pkg-config error does not name the .pc"
  cp "$TMP/pc.toml" "$W/lib_pc/kai.toml"
fi

# 6. pkg-config absent.
kai_in "$W/app_pc" env PKG_CONFIG="$TMP/no-pkg-config" "$KAI" build . -o "$TMP/app_nopc"
[ "$status" -ne 0 ] || note "build without pkg-config succeeded unexpectedly"
grep -q "package 'pcvend' declares pkg-config 'kaivend' (kaivend.pc)" "$TMP/.err" \
  || { note "missing pkg-config error does not name the package and .pc"; cat "$TMP/.err" >&2; }

exit "$fail"
