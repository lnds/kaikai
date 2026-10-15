#!/bin/sh
# examples/packages/native_modules — `[native.<module>]` tables compile
# and link only for a build that imports that module. lib_kohau binds
# two modules; the shim of `db.pg` includes a header that does not
# exist, standing in for a system library the consumer lacks.
#
# Arms:
#   1. a consumer importing only `lite` builds and runs.
#   2. a consumer importing `db.pg` fails, naming the module table.
#   3. an unquoted nested `[native.db.pg]` fails, naming the quoted form.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
KAI="$ROOT/bin/kai"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

fail=0
note() { echo "native_modules: FAIL — $1" >&2; fail=1; }

kai_in() {
  ki_dir="$1"; shift
  set +e
  ( cd "$ki_dir" && "$@" ) >"$TMP/.out" 2>"$TMP/.err"
  status=$?
  set -e
}

cp -R "$DIR/." "$TMP/w"
W="$TMP/w"

# 1. Only the imported module's table builds.
kai_in "$W/app_lite" "$KAI" build . -o "$TMP/app_lite"
if [ "$status" -ne 0 ]; then
  note "lite consumer: build exited $status"; cat "$TMP/.err" >&2
else
  got="$("$TMP/app_lite")"
  [ "$got" = 42 ] || note "lite consumer printed '$got', want 42"
fi

# 2. Importing db.pg compiles its shim, which fails naming the table.
kai_in "$W/app_pg" "$KAI" build . -o "$TMP/app_pg"
[ "$status" -ne 0 ] || note "db.pg consumer built unexpectedly"
grep -q "pg_shim.c (package 'kohau' \[native.\"db.pg\"\])" "$TMP/.err" \
  || { note "db.pg failure does not name the module table"; cat "$TMP/.err" >&2; }

# 3. An unquoted nested path is a TOML sub-table, not a module.
sed 's/^\[native\."db\.pg"\]$/[native.db.pg]/' "$DIR/lib_kohau/kai.toml" > "$W/lib_kohau/kai.toml"
kai_in "$W/app_lite" "$KAI" build . -o "$TMP/app_nested"
[ "$status" -ne 0 ] || note "unquoted nested table built unexpectedly"
grep -q "package 'kohau' declares \[native.db.pg\]; a nested module path is quoted: \[native.\"db.pg\"\]" "$TMP/.err" \
  || { note "unquoted nested table error does not name the quoted form"; cat "$TMP/.err" >&2; }

exit "$fail"
