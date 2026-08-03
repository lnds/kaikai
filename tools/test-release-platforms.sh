#!/bin/sh
# tools/test-release-platforms.sh — assert the release matrix, the
# installer and `kai upgrade` agree on every platform token.
#
# A release names its tarball kaikai-<tag>-<platform>.tar.gz, where
# <platform> comes from scripts/build-release.sh on the runner. The
# installer and `kai upgrade` rebuild that same token from `uname` to
# construct a download URL. If the two derivations disagree by one
# character the release still succeeds and every download 404s — the
# failure mode issue #1570 called out as "asserted by a test rather
# than assumed".
#
# Three properties are checked, all against the live shipped code:
#   1. every matrix entry in release.yml is a platform the installer
#      and `kai upgrade` accept, and both derive the matrix's own
#      os_name-arch spelling from that platform's uname output;
#   2. build-release.sh accepts each matrix platform;
#   3. an unpublished platform is REJECTED by both consumers, so a
#      user on it gets a diagnostic instead of a 404.

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKFLOW="$ROOT/.github/workflows/release.yml"

fail() { echo "test-release-platforms: FAIL: $1" >&2; exit 1; }

work="$(mktemp -d "${TMPDIR:-/tmp}/kai-platforms-test.XXXXXX")"
trap 'rm -rf "$work"' EXIT INT TERM

# ---- the uname pairs a runner of each published platform reports ----
# Keyed by the matrix's "<os_name>-<arch>"; the value is the "<uname -s>
# <uname -m>" that platform's runner actually reports. This table is the
# bridge between the two derivations: the matrix names the tarball, uname
# drives the consumers, and the test asserts they land on the same token.
uname_for() {
  case "$1" in
    darwin-arm64)  echo "Darwin arm64" ;;
    linux-x86_64)  echo "Linux x86_64" ;;
    *) return 1 ;;
  esac
}

# ---- extract the matrix platforms from release.yml -------------------
# The build job's matrix lists os_name/arch pairs; pair them positionally.
os_names="$(sed -n 's/^ *os_name: *\([a-z0-9_]*\) *$/\1/p' "$WORKFLOW")"
archs="$(sed -n 's/^ *arch: *\([a-z0-9_]*\) *$/\1/p' "$WORKFLOW")"
[ -n "$os_names" ] || fail "no os_name entries found in $WORKFLOW"
n_os="$(printf '%s\n' "$os_names" | wc -l | tr -d ' ')"
n_arch="$(printf '%s\n' "$archs" | wc -l | tr -d ' ')"
[ "$n_os" = "$n_arch" ] || fail "matrix has $n_os os_name but $n_arch arch entries"

platforms=""
i=1
while [ "$i" -le "$n_os" ]; do
  o="$(printf '%s\n' "$os_names" | sed -n "${i}p")"
  a="$(printf '%s\n' "$archs"   | sed -n "${i}p")"
  platforms="$platforms $o-$a"
  i=$((i + 1))
done
echo "test-release-platforms: matrix publishes:$platforms"

# linux-x86_64 is the platform issue #1570 exists for; guard against a
# silent revert of the matrix entry.
case "$platforms" in
  *linux-x86_64*) ;;
  *) fail "release matrix no longer publishes linux-x86_64" ;;
esac

# ---- driver: run a consumer's detection under a stubbed uname --------
# Both consumers derive the token from `uname -s` / `uname -m` only, so a
# shell-function stub is a faithful runner simulation.
stub_uname='uname() {
  case "${1:-}" in
    -s) printf "%s\n" "$STUB_OS" ;;
    -m) printf "%s\n" "$STUB_ARCH" ;;
    *)  printf "%s %s\n" "$STUB_OS" "$STUB_ARCH" ;;
  esac
}'

# `kai upgrade`'s token, via the live upgrade_platform extracted from bin/kai.
awk '/^upgrade_platform\(\) \{/{f=1} f{print} f&&/^\}/{exit}' \
  "$ROOT/bin/kai" > "$work/upgrade_platform.sh"
grep -q 'upgrade_platform' "$work/upgrade_platform.sh" \
  || fail "could not extract upgrade_platform from bin/kai"

wrapper_token() {
  STUB_OS="$1" STUB_ARCH="$2" sh -c "
    $stub_uname
    . '$work/upgrade_platform.sh'
    upgrade_platform
  " 2>/dev/null
}

# install.sh's token, via the live detect_platform extracted from it.
awk '/^detect_platform\(\) \{/{f=1} f{print} f&&/^\}/{exit}' \
  "$ROOT/install.sh" > "$work/detect_platform.sh"
grep -q 'detect_platform' "$work/detect_platform.sh" \
  || fail "could not extract detect_platform from install.sh"

installer_token() {
  STUB_OS="$1" STUB_ARCH="$2" sh -c "
    $stub_uname
    err() { exit 1; }
    . '$work/detect_platform.sh'
    detect_platform
    printf '%s\n' \"\$PLATFORM\"
  " 2>/dev/null
}

# ---- 1 + 2. every published platform round-trips ---------------------
for p in $platforms; do
  pair="$(uname_for "$p")" \
    || fail "matrix publishes '$p' but this test has no uname pair for it — add one to uname_for()"
  os="${pair% *}"; arch="${pair#* }"

  got="$(wrapper_token "$os" "$arch" || true)"
  [ "$got" = "$p" ] \
    || fail "kai upgrade on '$os $arch' derives [$got], matrix names the tarball [$p]"

  got="$(installer_token "$os" "$arch" || true)"
  [ "$got" = "$p" ] \
    || fail "install.sh on '$os $arch' derives [$got], matrix names the tarball [$p]"

  # build-release.sh's own guard must accept the platform it will be
  # asked to build. It derives OS/ARCH the same way, so grep its case arm.
  grep -q "$p" "$ROOT/scripts/build-release.sh" \
    || fail "build-release.sh does not accept platform '$p'"

  echo "test-release-platforms: ok — $p agrees across matrix, install.sh, kai upgrade, build-release.sh"
done

# ---- 3. an unpublished platform is refused, not 404'd ----------------
# linux-aarch64 is the sharpest case: folding it to arm64 would silently
# build the darwin tarball's name. Both consumers must reject it while
# no matrix entry publishes it.
case "$platforms" in
  *linux-aarch64*) ;;
  *)
    if wrapper_token Linux aarch64 >/dev/null 2>&1; then
      fail "kai upgrade accepts linux-aarch64, which the matrix does not publish"
    fi
    if installer_token Linux aarch64 >/dev/null 2>&1; then
      fail "install.sh accepts linux-aarch64, which the matrix does not publish"
    fi
    echo "test-release-platforms: ok — unpublished linux-aarch64 refused by both consumers"
    ;;
esac

if wrapper_token OpenBSD amd64 >/dev/null 2>&1; then
  fail "kai upgrade accepts an unsupported OS"
fi
if installer_token OpenBSD amd64 >/dev/null 2>&1; then
  fail "install.sh accepts an unsupported OS"
fi
echo "test-release-platforms: ok — unsupported OS refused by both consumers"

echo "test-release-platforms: all cases passed"
