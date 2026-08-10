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

# Run kai with this fixture's prefix, capturing streams and exit status.
# `status` is the exit code; stdout/stderr land in $TMP/.out and $TMP/.err.
# The `set +e` dance lives here so the assertions below read as assertions.
kai_in() {
  ki_dir="$1"; shift
  set +e
  ( cd "$ki_dir" && KAIKAI_HOME="$HOME_DIR" "$KAI" "$@" ) \
    >"$TMP/.out" 2>"$TMP/.err"
  status=$?
  set -e
}

expect_ok()   { [ "$status" -eq 0 ] || { note "$1 exited $status"; cat "$TMP/.err" >&2; }; }
expect_fail() { [ "$status" -ne 0 ] || note "$1"; }
expect_err()  { grep -qF -- "$1" "$TMP/.err" || note "$2"; }

# The installed binary exists and prints what its source said it would.
expect_prints() {
  if [ ! -x "$HOME_DIR/bin/$1" ]; then
    note "expected $HOME_DIR/bin/$1 to exist and be executable"
    return
  fi
  ep_got="$("$HOME_DIR/bin/$1" 2>/dev/null || true)"
  [ "$ep_got" = "$2" ] || note "'$1' printed '$ep_got', expected '$2'"
}

# Installing a package named $1 must be refused, with $2 in the message.
expect_refused() {
  mkpkg "$TMP/refused" "$1" "pwned"
  kai_in "$TMP/refused" install . --force
  expect_fail "installing '$1' succeeded; expected refusal"
  expect_err "$2" "refusal for '$1' does not mention '$2'"
}

# --- 1. install from a local path -------------------------------------
mkpkg "$TMP/src-dir-name" "pepito" "hello from pepito"
kai_in "$TMP/src-dir-name" install .
expect_ok "install from a local path"
expect_prints "pepito" "hello from pepito"
# The binary is named from the manifest, never from the directory.
[ ! -e "$HOME_DIR/bin/src-dir-name" ] \
  || note "binary was named after the directory, not the manifest"

# --- 2. reinstall without --force is refused --------------------------
kai_in "$TMP/src-dir-name" install .
expect_fail "reinstall without --force succeeded; expected refusal"
expect_err "--force" "refusal did not mention --force"
# The already-installed binary must survive a refused reinstall.
[ -x "$HOME_DIR/bin/pepito" ] || note "refused reinstall removed the existing binary"

# --- 3. reinstall with --force replaces -------------------------------
mkpkg "$TMP/src-dir-name" "pepito" "second version"
kai_in "$TMP/src-dir-name" install . --force
expect_ok "install --force"
expect_prints "pepito" "second version"

# --- 4. --list reports the installed package --------------------------
kai_in "$TMP" install --list
expect_ok "install --list"
grep -q "pepito" "$TMP/.out" || note "--list did not report pepito"

# --- 5. install from a git source, bare and with @<ref> ---------------
# A local repo stands in for a remote: `kai install` clones by URL, and a
# filesystem path is a URL git accepts, so this is the same code path a
# github.com source takes.
GITSRC="$TMP/gitsrc"
mkpkg "$GITSRC" "fulanito" "hello from fulanito"
( cd "$GITSRC" && git init -q -b main . \
  && git config user.email t@example.com && git config user.name t \
  && git add -A && git commit -qm init && git tag v1.0.0 ) >/dev/null 2>&1

kai_in "$TMP" install "$GITSRC"
expect_ok "install from a git source"
expect_prints "fulanito" "hello from fulanito"

# The @<ref> form pins a tag. --force because the bare form just installed it.
kai_in "$TMP" install "$GITSRC@v1.0.0" --force
expect_ok "install with @<ref>"
kai_in "$TMP" install --list
grep -q "v1.0.0" "$TMP/.out" || note "--list did not record the pinned ref"

# A ref that does not exist must fail rather than silently take the default.
kai_in "$TMP" install "$GITSRC@v9.9.9" --force
expect_fail "install at a nonexistent ref succeeded"

# --- 6. toolchain names are refused -----------------------------------
# The set is derived, not listed: every binary the release script copies
# into the tarball's bin/ or libexec/kaikai/ must be refused, so a binary
# added to a future release is reserved without editing bin/kai.
shipped="$(sed -n -E 's#.*"\$STAGE/(bin|libexec/kaikai)/([A-Za-z0-9._-]+)".*#\2#p' \
  "$ROOT/scripts/build-release.sh" | sort -u)"
[ -n "$shipped" ] || note "derived no binary names from scripts/build-release.sh"
for name in $shipped; do
  expect_refused "$name" "reserved"
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
kai_in "$TMP/lib" install .
expect_fail "installing a library succeeded; expected refusal"
expect_err "entry point" "library refusal does not mention the missing entry point"
[ ! -e "$HOME_DIR/bin/libonly" ] || note "library install left a binary behind"

# --- 8. the no-argument form still resolves deps, deprecated ----------
mkpkg "$TMP/deps-pkg" "depspkg" "deps"
kai_in "$TMP/deps-pkg" install
expect_ok "no-argument 'kai install'"
expect_err "kai fetch" "no-argument form did not point at 'kai fetch'"
[ -f "$TMP/deps-pkg/kai.lock" ] || note "no-argument form did not write kai.lock"
[ ! -e "$HOME_DIR/bin/depspkg" ] || note "no-argument form installed a binary"

# `kai fetch` is the same resolution under its own name.
rm -f "$TMP/deps-pkg/kai.lock"
kai_in "$TMP/deps-pkg" fetch
expect_ok "'kai fetch'"
[ -f "$TMP/deps-pkg/kai.lock" ] || note "'kai fetch' did not write kai.lock"

[ "$fail" -eq 0 ] || exit 1
echo "install_binary: OK"
