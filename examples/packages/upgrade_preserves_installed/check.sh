#!/bin/sh
# examples/packages/upgrade_preserves_installed — assert `kai upgrade`
# does not evict binaries `kai install` put in the shared bin/ (#1724).
#
# `kai upgrade` replaces bin/, libexec/ and share/ wholesale so a file
# dropped by the new release does not linger. bin/ is also where user
# binaries live, so the wipe has to carry them across. That is the
# behaviour pinned here.
#
# The real command downloads a release tarball, so this exercises the
# swap block against a fabricated prefix and a fabricated tarball: the
# network half is not what regresses, the wipe is.

set -eu

DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DIR/../../.." && pwd)"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

fail=0
note() { echo "upgrade_preserves_installed: FAIL — $1" >&2; fail=1; }

# --- a prefix as `kai install` would leave it -------------------------
prefix="$TMP/prefix"
mkdir -p "$prefix/bin" "$prefix/libexec/kaikai" "$prefix/share/kaikai"
printf '#!/bin/sh\necho old-toolchain\n' > "$prefix/bin/kai"
printf '#!/bin/sh\necho pepito\n'        > "$prefix/bin/pepito"
printf '#!/bin/sh\necho fulanito\n'      > "$prefix/bin/fulanito"
# A binary present in bin/ but absent from the ledger is not the user's
# (a stale toolchain file, say) and is expected to go with the wipe.
printf '#!/bin/sh\necho stale\n'         > "$prefix/bin/stale-toolchain-file"
chmod +x "$prefix/bin/"*
printf 'old\n' > "$prefix/libexec/kaikai/kaic2"
printf 'fulanito\tgithub.com/x/fulanito\tv1.0.0\npepito\t/local/pepito\tlocal\n' \
  > "$prefix/.kai-installed"

# --- a release tarball as upgrade would unpack it ---------------------
top="$TMP/unpack/kaikai-vNEXT"
mkdir -p "$top/bin" "$top/libexec/kaikai" "$top/share/kaikai"
printf '#!/bin/sh\necho new-toolchain\n' > "$top/bin/kai"
chmod +x "$top/bin/kai"
printf 'new\n' > "$top/libexec/kaikai/kaic2"
printf 'new\n' > "$top/share/kaikai/VERSION"

# --- run the swap block verbatim from bin/kai -------------------------
# Extracted by markers so the fixture cannot drift from the shipped code:
# if the block is edited, this runs the edited version.
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

sh "$swap" "$prefix" "$top" "$TMP" >"$TMP/.out" 2>"$TMP/.err" || {
  note "swap block exited non-zero"
  cat "$TMP/.err" >&2
}

# --- the toolchain was replaced ---------------------------------------
got="$(sh "$prefix/bin/kai" 2>/dev/null || true)"
[ "$got" = "new-toolchain" ] \
  || note "toolchain was not replaced (bin/kai printed '$got')"
grep -q '^new$' "$prefix/libexec/kaikai/kaic2" \
  || note "libexec was not replaced"

# --- user binaries survived -------------------------------------------
for b in pepito fulanito; do
  if [ ! -x "$prefix/bin/$b" ]; then
    note "kai upgrade evicted the installed binary '$b'"
    continue
  fi
  got="$(sh "$prefix/bin/$b" 2>/dev/null || true)"
  [ "$got" = "$b" ] || note "'$b' survived but its contents changed ('$got')"
done

# --- and the wipe still did its job for everything else ---------------
[ ! -e "$prefix/bin/stale-toolchain-file" ] \
  || note "a bin/ file absent from the ledger survived the wipe"

# --- install and upgrade must agree on the prefix ---------------------
# The ledger only protects binaries if both commands read the same
# directory. `kai upgrade` uses $ROOT (the wrapper's grandparent), so an
# installed layout must resolve there even when $KAIKAI_HOME points
# elsewhere — otherwise the ledger sits in one prefix and the upgrade
# wipes another.
probe="$TMP/prefix-probe.sh"
sed -n '/^install_prefix() {/,/^}/p' "$ROOT/bin/kai" > "$probe"
if [ ! -s "$probe" ]; then
  note "could not extract install_prefix from bin/kai"
else
  echo 'install_prefix' >> "$probe"
  mkdir -p "$TMP/installed/libexec/kaikai"
  : > "$TMP/installed/libexec/kaikai/kaic2"
  chmod +x "$TMP/installed/libexec/kaikai/kaic2"
  got="$(ROOT="$TMP/installed" KAIKAI_HOME="$TMP/somewhere-else" sh "$probe")"
  [ "$got" = "$TMP/installed" ] \
    || note "installed prefix resolved to '$got', not the wrapper's own root"
fi

# --- a name the new toolchain claims does not shadow it ---------------
prefix2="$TMP/prefix2"
mkdir -p "$prefix2/bin"
printf '#!/bin/sh\necho user-kai-lsp\n' > "$prefix2/bin/kai-lsp"
chmod +x "$prefix2/bin/kai-lsp"
printf 'kai-lsp\t/local/kai-lsp\tlocal\n' > "$prefix2/.kai-installed"
top2="$TMP/unpack2/kaikai-vNEXT"
mkdir -p "$top2/bin"
printf '#!/bin/sh\necho toolchain-kai-lsp\n' > "$top2/bin/kai-lsp"
chmod +x "$top2/bin/kai-lsp"

sh "$swap" "$prefix2" "$top2" "$TMP" >"$TMP/.out2" 2>"$TMP/.err2" || :
got="$(sh "$prefix2/bin/kai-lsp" 2>/dev/null || true)"
[ "$got" = "toolchain-kai-lsp" ] \
  || note "a user binary shadowed a toolchain binary of the same name ('$got')"

[ "$fail" -eq 0 ] || exit 1
echo "upgrade_preserves_installed: OK"
