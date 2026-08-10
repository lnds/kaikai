#!/bin/sh
# examples/packages/native_shim — the [native] manifest table carries a
# dependency's vendored C shim to every verb, on the resolution layer
# below them all.
#
# Arms:
#   1. `kai test` inside the library — its own shim links.
#   2. `kai test` in the consumer — the dep's shim links.
#   3. `kai install .` of the consumer — binary built and recorded.
#   4. missing cc — one sentence naming the package that requires it,
#      before anything compiles.
#   5. KAI_NATIVE_DEPS=0 — reversion switch: link fails, and the note
#      says the channel was disabled; with the shim on CFLAGS (the
#      manual escape hatch) the build succeeds again.
#   6. a dep shipping undeclared C sources — the failed link names the
#      missing [native] contract.
#   7. `kai-pkg show` parses a manifest with [native] (array values).

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
KAI="$ROOT/bin/kai"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

fail=0
note() { echo "native_shim: FAIL — $1" >&2; fail=1; }

kai_in() {
  ki_dir="$1"; shift
  set +e
  ( cd "$ki_dir" && "$@" ) >"$TMP/.out" 2>"$TMP/.err"
  status=$?
  set -e
}

# 1. The library runs its own tests against its own shim.
kai_in "$DIR/lib_term" "$KAI" test .
[ "$status" -eq 0 ] || { note "kai test inside lib_term exited $status"; cat "$TMP/.err" >&2; }
grep -q "2/2 tests passed" "$TMP/.out" "$TMP/.err" 2>/dev/null \
  || note "lib_term tests did not report 2/2 passed"

# 2. The consumer's tests link the dep's shim too.
cp -R "$DIR/app" "$TMP/app_t"
cp -R "$DIR/lib_term" "$TMP/lib_term"
cat > "$TMP/app_t/util_test.kai" <<'EOF'
import term

test "consumer links the dep shim" {
  assert term.clamp_width(0) == 20
}
EOF
kai_in "$TMP/app_t" "$KAI" test .
[ "$status" -eq 0 ] || { note "kai test in consumer exited $status"; cat "$TMP/.err" >&2; }

# 3. Install the consumer into an isolated prefix.
HOME_DIR="$TMP/home"
mkdir -p "$HOME_DIR/bin"
kai_in "$DIR/app" env KAIKAI_HOME="$HOME_DIR" "$KAI" install .
[ "$status" -eq 0 ] || { note "kai install exited $status"; cat "$TMP/.err" >&2; }
if [ -x "$HOME_DIR/bin/native_shim_app" ]; then
  out="$("$HOME_DIR/bin/native_shim_app")"
  [ "$out" = "80" ] || note "installed binary printed '$out', want 80"
else
  note "kai install left no binary in $HOME_DIR/bin"
fi

# 4. Missing cc: named package, before compiling anything.
kai_in "$DIR/app" env CC=/nonexistent-kai-cc "$KAI" build . -o "$TMP/app_nocc"
[ "$status" -ne 0 ] || note "build with CC=/nonexistent-kai-cc succeeded"
grep -q "no C compiler found" "$TMP/.err" || note "missing-cc error absent"
grep -q "package 'term' ships native C sources" "$TMP/.err" \
  || note "missing-cc error does not name the requiring package"

# 5. Reversion: the switch disables the channel; CFLAGS still works.
kai_in "$DIR/app" env KAI_NATIVE_DEPS=0 "$KAI" build . -o "$TMP/app_off"
[ "$status" -ne 0 ] || note "KAI_NATIVE_DEPS=0 build succeeded unexpectedly"
grep -q "KAI_NATIVE_DEPS=0 disabled" "$TMP/.err" || note "disabled-channel note absent"
kai_in "$DIR/app" env KAI_NATIVE_DEPS=0 CFLAGS="-std=c99 -O2 $DIR/lib_term/c/term_shim.c" \
  "$KAI" build . -o "$TMP/app_cflags"
[ "$status" -eq 0 ] || { note "CFLAGS escape hatch failed under KAI_NATIVE_DEPS=0"; cat "$TMP/.err" >&2; }

# 6. Undeclared shim: the failed link names the missing contract.
cp -R "$DIR/app" "$TMP/app_u"
cp -R "$DIR/lib_term" "$TMP/lib_undecl"
printf 'name = "tvklite"\nversion = "0.1.0"\n' > "$TMP/lib_undecl/kai.toml"
sed 's#../lib_term#../lib_undecl#' "$DIR/app/kai.toml" > "$TMP/app_u/kai.toml"
kai_in "$TMP/app_u" "$KAI" build . -o "$TMP/app_undecl"
[ "$status" -ne 0 ] || note "build against undeclared shim succeeded unexpectedly"
grep -q "not declared in its kai.toml \[native\] table" "$TMP/.err" \
  || note "undeclared-shim hint absent"

# 7. kai-pkg parses [native] (array values in the manifest).
if [ -x "$ROOT/tools/kai-pkg/kai-pkg" ]; then
  kai_in "$DIR/lib_term" "$ROOT/tools/kai-pkg/kai-pkg" show
  [ "$status" -eq 0 ] || note "kai-pkg show failed on a [native] manifest"
  grep -q 'sources = \["c/term_shim.c"\]' "$TMP/.out" \
    || note "kai-pkg show did not render [native] sources"
fi

exit "$fail"
