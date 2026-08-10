#!/bin/sh
# examples/packages/native_diamond — A and B both depend on the same
# shim-binding package C. Resolution keys native sources by resolved
# package, so C's shim compiles and links exactly once: the diamond
# builds with no duplicate symbols.
#
# Self-contained: builds three bare git repos in a temp dir, then a
# consumer app on top of them, with an isolated package cache.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"
KAI="$ROOT/bin/kai"

command -v git >/dev/null 2>&1 || { echo "native_diamond: git unavailable" >&2; exit 0; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

fail=0
note() { echo "native_diamond: FAIL — $1" >&2; fail=1; }

mkrepo() {
  mr_name="$1"
  mr_src="$TMP/src-$mr_name"
  (
    cd "$mr_src"
    git init -q
    git config user.email fixtures@example.invalid
    git config user.name "kaikai fixtures"
    git add -A
    git commit -q -m "init $mr_name"
    git tag v0.1.0
  )
  git clone -q --bare "$mr_src" "$TMP/$mr_name.bare"
}

# C: the shim-binding package.
mkdir -p "$TMP/src-shimpkg/c"
cat > "$TMP/src-shimpkg/kai.toml" <<'EOF'
name = "shimpkg"
version = "0.1.0"

[native]
sources = ["c/shim7.c"]
include = ["c"]
EOF
cat > "$TMP/src-shimpkg/c/shim7.h" <<'EOF'
#include <stdint.h>

int64_t shim_seven(void);
EOF
cat > "$TMP/src-shimpkg/c/shim7.c" <<'EOF'
#include "shim7.h"

int64_t shim_seven(void) { return 7; }
EOF
cat > "$TMP/src-shimpkg/shim.kai" <<'EOF'
extern "C" fn shim_seven() : Int / Ffi

pub fn seven() : Int / Ffi = shim_seven()
EOF
mkrepo shimpkg

# A and B: both depend on C.
for side in a b; do
  mkdir -p "$TMP/src-lib$side"
  cat > "$TMP/src-lib$side/kai.toml" <<EOF
name = "lib$side"
version = "0.1.0"

[dependencies]
shimpkg = { source = "$TMP/shimpkg.bare", ref = "v0.1.0" }
EOF
done
cat > "$TMP/src-liba/a.kai" <<'EOF'
import shim

pub fn a_val() : Int / Ffi = shim.seven() + 1
EOF
cat > "$TMP/src-libb/b.kai" <<'EOF'
import shim

pub fn b_val() : Int / Ffi = shim.seven() + 2
EOF
mkrepo liba
mkrepo libb

# The consumer at the top of the diamond.
mkdir -p "$TMP/app"
cat > "$TMP/app/kai.toml" <<EOF
name = "diamond_app"
version = "0.1.0"

[dependencies]
liba = { source = "$TMP/liba.bare", ref = "v0.1.0" }
libb = { source = "$TMP/libb.bare", ref = "v0.1.0" }
EOF
cat > "$TMP/app/main.kai" <<'EOF'
import a
import b

fn main() : Unit / Stdout + Ffi = {
  Stdout.print("#{a.a_val() + b.b_val()}")
}
EOF

set +e
out="$(cd "$TMP/app" && KAIKAI_CACHE="$TMP/cache" "$KAI" run . 2>"$TMP/.err")"
status=$?
set -e
[ "$status" -eq 0 ] || { note "kai run exited $status"; cat "$TMP/.err" >&2; }
got="$(printf '%s\n' "$out" | grep -v '^kai' || true)"
[ "$got" = "17" ] || note "diamond app printed '$got', want 17"

# One resolved entry for the shared package: the lock is the dedup.
n="$(grep -c 'shimpkg.bare' "$TMP/app/kai.lock" 2>/dev/null || echo 0)"
[ "$n" = "1" ] || note "expected 1 shimpkg entry in kai.lock, got $n"

# The acceptance shape of `kai install github.com/owner/app`: install
# from a git source whose transitive git dependencies bottom out in a
# [native] package. Clone, resolve, compile the shim, link, install —
# into an isolated prefix, from a cold cache, no CFLAGS, no Makefile.
mkdir -p "$TMP/src-app"
cp "$TMP/app/kai.toml" "$TMP/app/main.kai" "$TMP/src-app/"
mkrepo app
HOME_DIR="$TMP/home"
mkdir -p "$HOME_DIR/bin"
set +e
( cd "$TMP" && KAIKAI_HOME="$HOME_DIR" KAIKAI_CACHE="$TMP/cache-install" \
    "$KAI" install "$TMP/app.bare@v0.1.0" ) >"$TMP/.iout" 2>"$TMP/.ierr"
status=$?
set -e
[ "$status" -eq 0 ] || { note "kai install from git exited $status"; cat "$TMP/.ierr" >&2; }
if [ -x "$HOME_DIR/bin/diamond_app" ]; then
  got="$("$HOME_DIR/bin/diamond_app")"
  [ "$got" = "17" ] || note "installed diamond_app printed '$got', want 17"
else
  note "kai install from git left no binary in $HOME_DIR/bin"
fi

exit "$fail"
