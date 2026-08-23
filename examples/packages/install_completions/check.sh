#!/bin/sh
# examples/packages/install_completions — assert `kai install` places the
# shell completions a package declares in `[completions]` (issue #1817).
#
# Arms:
#   1. declared completions land at the vendor path each shell searches
#   2. a package declaring none installs exactly as before
#   3. a path that escapes the package, or names a missing file, is
#      skipped with a warning — the binary still installs
#   4. reinstalling with --force refreshes the completion in place
#   5. a git source copies them out of the clone before it is removed
#   6. `kai upgrade`'s wholesale share/ wipe carries them across

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
KAI="$ROOT/bin/kai"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

HOME_DIR="$TMP/home"
mkdir -p "$HOME_DIR/bin"

fail=0
note() { echo "install_completions: FAIL — $1" >&2; fail=1; }

# A package carrying completions/ next to its manifest, the layout the
# issue describes. $4 is the extra manifest text, so the arms below can
# declare a bad path without a second builder.
mkpkg() {
  mkdir -p "$1/completions"
  cat > "$1/kai.toml" <<EOF
name = "$2"
version = "0.1.0"

[dependencies]

$3
EOF
  cat > "$1/main.kai" <<EOF
fn main() : Unit / Stdout = {
  Stdout.print("$2")
}
EOF
  printf '#compdef %s\n_files\n'   "$2" > "$1/completions/_$2"
  printf 'complete -F _%s %s\n'    "$2" "$2" > "$1/completions/$2.bash"
  printf 'complete -c %s -F\n'     "$2" > "$1/completions/$2.fish"
}

kai_in() {
  ki_dir="$1"; shift
  set +e
  ( cd "$ki_dir" && KAIKAI_HOME="$HOME_DIR" "$KAI" "$@" ) \
    >"$TMP/.out" 2>"$TMP/.err"
  status=$?
  set -e
}

expect_ok()   { [ "$status" -eq 0 ] || { note "$1 exited $status"; cat "$TMP/.err" >&2; }; }
expect_err()  { grep -qF -- "$1" "$TMP/.err" || note "$2"; }

# The file exists at the vendor path and holds what the package shipped.
expect_completion() {
  ec_path="$HOME_DIR/$1"
  if [ ! -f "$ec_path" ]; then
    note "expected $ec_path to exist"
    return
  fi
  grep -qF -- "$2" "$ec_path" || note "$ec_path does not contain '$2'"
}

# --- 1. declared completions land where each shell looks --------------
mkpkg "$TMP/mark" "mark" '[completions]
zsh  = "completions/_mark"
bash = "completions/mark.bash"
fish = "completions/mark.fish"'
kai_in "$TMP/mark" install .
expect_ok "install a package with completions"
[ -x "$HOME_DIR/bin/mark" ] || note "the binary was not installed"

# The names are the vendor conventions: zsh wants `_<name>`, bash the
# bare name, fish `<name>.fish`.
expect_completion "share/zsh/site-functions/_mark"              "#compdef mark"
expect_completion "share/bash-completion/completions/mark"      "complete -F _mark"
expect_completion "share/fish/vendor_completions.d/mark.fish"   "complete -c mark"

# The user is told where the files went, and never has an rc edited for
# them: a package install is not the moment to touch shell config.
expect_err "share/zsh/site-functions/_mark" "install did not report where the zsh completion went"

# --- 2. a package declaring none is unaffected ------------------------
mkpkg "$TMP/plain" "plain" ''
kai_in "$TMP/plain" install .
expect_ok "install a package without completions"
[ -x "$HOME_DIR/bin/plain" ] || note "the binary was not installed"
[ ! -e "$HOME_DIR/share/zsh/site-functions/_plain" ] \
  || note "a package declaring no completions had one installed"

# --- 3. a bad path is skipped, the install still succeeds -------------
# An absolute path or a `..` hop would read outside what was cloned, and
# a declared file the package forgot to ship is a manifest typo. Neither
# is worth failing an otherwise good install over.
mkpkg "$TMP/escape" "escaper" '[completions]
zsh  = "../../../etc/passwd"
bash = "completions/nonexistent.bash"'
kai_in "$TMP/escape" install .
expect_ok "install with a bad completions path"
[ -x "$HOME_DIR/bin/escaper" ] || note "a bad completions path blocked the binary install"
expect_err "not a path inside the package" "the escaping path was not reported"
expect_err "does not ship" "the missing file was not reported"
[ ! -e "$HOME_DIR/share/zsh/site-functions/_escaper" ] \
  || note "an escaping completions path was installed anyway"

# --- 4. --force refreshes the completion in place ---------------------
printf '#compdef mark\n_files -g "*.md"\n' > "$TMP/mark/completions/_mark"
kai_in "$TMP/mark" install . --force
expect_ok "reinstall with --force"
expect_completion "share/zsh/site-functions/_mark" '_files -g "*.md"'

# --- 5. a git source installs them from the clone before it is gone ---
# The clone lives in a temp dir the installer removes on the way out, so
# the copy has to happen while it is still there.
GITSRC="$TMP/gitsrc"
mkpkg "$GITSRC" "gitmark" '[completions]
zsh = "completions/_gitmark"'
( cd "$GITSRC" && git init -q -b main . \
  && git config user.email t@example.com && git config user.name t \
  && git add -A && git commit -qm init ) >/dev/null 2>&1

kai_in "$TMP" install "$GITSRC"
expect_ok "install a git-source package with completions"
expect_completion "share/zsh/site-functions/_gitmark" "#compdef gitmark"

# --- 6. `kai upgrade` does not evict them -----------------------------
# upgrade replaces share/ wholesale so a removed file does not linger,
# but share/ now also holds what `kai install` dropped. Same extraction
# trick as upgrade_preserves_installed: run the shipped block, not a copy.
top="$TMP/unpack/kaikai-vNEXT"
mkdir -p "$top/bin" "$top/libexec/kaikai" "$top/share/kaikai"
printf '#!/bin/sh\necho new\n' > "$top/bin/kai"
chmod +x "$top/bin/kai"
printf 'new\n' > "$top/share/kaikai/VERSION"

swap="$TMP/swap.sh"
awk '/^  # Swap contents in place\./{f=1} f{print} /^  done$/{if(f&&++d==2)exit}' \
  "$ROOT/bin/kai" > "$swap.body"
if [ ! -s "$swap.body" ]; then
  note "could not extract the swap block from bin/kai — markers moved"
  exit 1
fi
{
  printf 'set -eu\nprefix="$1"\ntop="$2"\ntmp="$3"\n'
  cat "$swap.body"
} > "$swap"

mkdir -p "$TMP/swaptmp"
sh "$swap" "$HOME_DIR" "$top" "$TMP/swaptmp" >/dev/null 2>"$TMP/.err" || {
  note "swap block exited non-zero"
  cat "$TMP/.err" >&2
}

expect_completion "share/zsh/site-functions/_mark"            "#compdef mark"
expect_completion "share/bash-completion/completions/mark"    "complete -F _mark"
expect_completion "share/fish/vendor_completions.d/mark.fish" "complete -c mark"
# The release's own share/ content is what the tarball shipped, not the old one.
grep -q '^new$' "$HOME_DIR/share/kaikai/VERSION" \
  || note "the upgrade did not replace the toolchain's own share/ content"

[ "$fail" -eq 0 ] || exit 1
echo "install_completions: OK"
