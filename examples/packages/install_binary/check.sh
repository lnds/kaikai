#!/bin/sh
# examples/packages/install_binary — assert `kai install <spec>` builds a
# package and puts its binary in $KAIKAI_HOME/bin (issue #1724).
#
# Arms:
#   1. install from a local path — binary named after the manifest, runs
#   2. reinstall without --force — refused, existing binary untouched
#   3. reinstall with --force — replaced
#   4. --list — reports what was installed
#   5. install from a git source, with and without @<ref>
#   6. a toolchain name is refused (the name set is derived, not literal)
#   7. a library (no entry point) is refused with a message that says so
#   8. no-argument form still resolves deps, with a deprecation notice

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
KAI="$ROOT/bin/kai"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

HOME_DIR="$TMP/home"
mkdir -p "$HOME_DIR/bin"

fail=0
note() { echo "install_binary: FAIL — $1" >&2; fail=1; }

# A package whose manifest name differs from its directory name, so the
# "binary comes from the manifest, not the path" rule is actually tested.
mkpkg() {
  mkdir -p "$1"
  cat > "$1/kai.toml" <<EOF
name = "$2"
version = "0.1.0"

[dependencies]
EOF
  cat > "$1/main.kai" <<EOF
fn main() : Unit / Stdout = {
  Stdout.print("$3")
}
EOF
}

# --- 1. install from a local path -------------------------------------
mkpkg "$TMP/src-dir-name" "pepito" "hello from pepito"
set +e
( cd "$TMP/src-dir-name" && KAIKAI_HOME="$HOME_DIR" "$KAI" install . ) \
  >"$TMP/.out" 2>"$TMP/.err"
status=$?
set -e
if [ "$status" -ne 0 ]; then
  note "install from local path exited $status"
  cat "$TMP/.err" >&2
fi
if [ ! -x "$HOME_DIR/bin/pepito" ]; then
  note "expected $HOME_DIR/bin/pepito to exist and be executable"
  ls -la "$HOME_DIR/bin" >&2
else
  got="$("$HOME_DIR/bin/pepito" 2>/dev/null || true)"
  [ "$got" = "hello from pepito" ] \
    || note "installed binary printed '$got', expected 'hello from pepito'"
fi
# The binary is named from the manifest, never from the directory.
[ ! -e "$HOME_DIR/bin/src-dir-name" ] \
  || note "binary was named after the directory, not the manifest"

# --- 2. reinstall without --force is refused --------------------------
set +e
( cd "$TMP/src-dir-name" && KAIKAI_HOME="$HOME_DIR" "$KAI" install . ) \
  >"$TMP/.out" 2>"$TMP/.err"
status=$?
set -e
[ "$status" -ne 0 ] || note "reinstall without --force succeeded; expected refusal"
grep -q -- "--force" "$TMP/.err" \
  || note "refusal did not mention --force"
# The already-installed binary must survive a refused reinstall.
[ -x "$HOME_DIR/bin/pepito" ] || note "refused reinstall removed the existing binary"

# --- 3. reinstall with --force replaces -------------------------------
mkpkg "$TMP/src-dir-name" "pepito" "second version"
set +e
( cd "$TMP/src-dir-name" && KAIKAI_HOME="$HOME_DIR" "$KAI" install . --force ) \
  >"$TMP/.out" 2>"$TMP/.err"
status=$?
set -e
if [ "$status" -ne 0 ]; then
  note "install --force exited $status"
  cat "$TMP/.err" >&2
else
  got="$("$HOME_DIR/bin/pepito" 2>/dev/null || true)"
  [ "$got" = "second version" ] \
    || note "--force left the old binary (printed '$got')"
fi

# --- 4. --list reports the installed package --------------------------
set +e
KAIKAI_HOME="$HOME_DIR" "$KAI" install --list >"$TMP/.out" 2>"$TMP/.err"
status=$?
set -e
[ "$status" -eq 0 ] || note "install --list exited $status"
grep -q "pepito" "$TMP/.out" || note "--list did not report pepito"

# --- 5. install from a git source, bare and with @<ref> ---------------
# A local bare repo stands in for a remote: `kai install` clones by URL,
# and a filesystem path is a URL git accepts, so the code path is the
# same one a github.com source takes.
GITSRC="$TMP/gitsrc"
mkpkg "$GITSRC" "fulanito" "hello from fulanito"
( cd "$GITSRC" && git init -q -b main . \
  && git config user.email t@example.com && git config user.name t \
  && git add -A && git commit -qm init && git tag v1.0.0 ) >/dev/null 2>&1

set +e
KAIKAI_HOME="$HOME_DIR" "$KAI" install "$GITSRC" >"$TMP/.out" 2>"$TMP/.err"
status=$?
set -e
if [ "$status" -ne 0 ]; then
  note "install from a git source exited $status"
  cat "$TMP/.err" >&2
elif [ ! -x "$HOME_DIR/bin/fulanito" ]; then
  note "git-source install did not produce bin/fulanito"
else
  got="$("$HOME_DIR/bin/fulanito" 2>/dev/null || true)"
  [ "$got" = "hello from fulanito" ] \
    || note "git-installed binary printed '$got'"
fi

# The @<ref> form pins a tag. --force because the bare form just installed it.
set +e
KAIKAI_HOME="$HOME_DIR" "$KAI" install "$GITSRC@v1.0.0" --force \
  >"$TMP/.out" 2>"$TMP/.err"
status=$?
set -e
if [ "$status" -ne 0 ]; then
  note "install with @<ref> exited $status"
  cat "$TMP/.err" >&2
fi
KAIKAI_HOME="$HOME_DIR" "$KAI" install --list 2>/dev/null | grep -q "v1.0.0" \
  || note "--list did not record the pinned ref"

# A ref that does not exist must fail rather than silently take the default.
set +e
KAIKAI_HOME="$HOME_DIR" "$KAI" install "$GITSRC@v9.9.9" --force \
  >"$TMP/.out" 2>"$TMP/.err"
status=$?
set -e
[ "$status" -ne 0 ] || note "install at a nonexistent ref succeeded"

# --- 6. a toolchain name is refused -----------------------------------
# The reserved set is derived from what the prefix and the release ship,
# so `kai` is reserved whether or not this temp prefix has a bin/kai.
for reserved in kai kaic2 kai-pkg; do
  mkpkg "$TMP/evil" "$reserved" "pwned"
  set +e
  ( cd "$TMP/evil" && KAIKAI_HOME="$HOME_DIR" "$KAI" install . --force ) \
    >"$TMP/.out" 2>"$TMP/.err"
  status=$?
  set -e
  if [ "$status" -eq 0 ]; then
    note "installing reserved name '$reserved' succeeded"
    continue
  fi
  grep -q "reserved" "$TMP/.err" \
    || note "refusal for '$reserved' does not say the name is reserved"
done

# The set is derived, not listed: every binary the release script copies
# into the tarball's bin/ or libexec/kaikai/ must appear in it. A binary
# added to a future release is then reserved without editing bin/kai.
shipped="$(sed -n -E 's#.*"\$STAGE/(bin|libexec/kaikai)/([A-Za-z0-9._-]+)".*#\2#p' \
  "$ROOT/scripts/build-release.sh" | sort -u)"
[ -n "$shipped" ] || note "derived no binary names from scripts/build-release.sh"
for name in $shipped; do
  mkpkg "$TMP/derived" "$name" "pwned"
  set +e
  ( cd "$TMP/derived" && KAIKAI_HOME="$HOME_DIR" "$KAI" install . --force ) \
    >"$TMP/.out" 2>"$TMP/.err"
  status=$?
  set -e
  [ "$status" -ne 0 ] \
    || note "'$name' is shipped by the release but was not reserved"
done

# --- 7. a library has no binary to install ----------------------------
mkdir -p "$TMP/lib"
cat > "$TMP/lib/kai.toml" <<'EOF'
name = "libonly"
version = "0.1.0"

[dependencies]
EOF
cat > "$TMP/lib/helper.kai" <<'EOF'
pub fn twice(n: Int) : Int = n * 2
EOF
set +e
( cd "$TMP/lib" && KAIKAI_HOME="$HOME_DIR" "$KAI" install . ) \
  >"$TMP/.out" 2>"$TMP/.err"
status=$?
set -e
[ "$status" -ne 0 ] || note "installing a library succeeded; expected refusal"
grep -q "entry point" "$TMP/.err" \
  || note "library refusal does not mention the missing entry point"
[ ! -e "$HOME_DIR/bin/libonly" ] || note "library install left a binary behind"

# --- 8. the no-argument form still resolves deps, deprecated ----------
mkpkg "$TMP/deps-pkg" "depspkg" "deps"
set +e
( cd "$TMP/deps-pkg" && KAIKAI_HOME="$HOME_DIR" "$KAI" install ) \
  >"$TMP/.out" 2>"$TMP/.err"
status=$?
set -e
[ "$status" -eq 0 ] || note "no-argument 'kai install' exited $status"
grep -q "kai fetch" "$TMP/.err" \
  || note "no-argument form did not point at 'kai fetch'"
[ -f "$TMP/deps-pkg/kai.lock" ] \
  || note "no-argument form did not write kai.lock"
# It must not have installed anything.
[ ! -e "$HOME_DIR/bin/depspkg" ] \
  || note "no-argument form installed a binary"

# `kai fetch` is the same resolution under its own name.
rm -f "$TMP/deps-pkg/kai.lock"
set +e
( cd "$TMP/deps-pkg" && KAIKAI_HOME="$HOME_DIR" "$KAI" fetch ) \
  >"$TMP/.out" 2>"$TMP/.err"
status=$?
set -e
[ "$status" -eq 0 ] || note "'kai fetch' exited $status"
[ -f "$TMP/deps-pkg/kai.lock" ] || note "'kai fetch' did not write kai.lock"

[ "$fail" -eq 0 ] || exit 1
echo "install_binary: OK"
