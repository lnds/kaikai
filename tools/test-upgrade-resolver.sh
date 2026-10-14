#!/bin/sh
# tools/test-upgrade-resolver.sh — regression test for the `kai upgrade`
# and install.sh tag resolver.
#
# The resolver reads the GitHub *tags* API (not releases/latest, which
# 404s until a Release is published) and must:
#   1. pick the newest v<major>.<minor>.<patch> tag, ignoring junk tags;
#   2. report a rate-limit (403 / X-RateLimit-Remaining: 0 / body) as a
#      graceful message, never a raw curl 403;
#   3. report a network failure and a no-tag response distinctly.
#
# It drives the live `upgrade_latest_tag` extracted from the kai-upgrade
# plugin with a curl stub serving local fixtures — no network, no GitHub
# Release.

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

fail() { echo "test-upgrade-resolver: FAIL: $1" >&2; exit 1; }

work="$(mktemp -d "${TMPDIR:-/tmp}/kai-upgrade-test.XXXXXX")"
trap 'rm -rf "$work"' EXIT INT TERM

# Extract the live resolver from the plugin so we test the shipped code.
awk '/^upgrade_latest_tag\(\) \{/{f=1} f{print} f&&/^\}/{exit}' \
  "$ROOT/tools/kai/plugins/kai-upgrade" > "$work/resolver.sh"
grep -q 'upgrade_latest_tag' "$work/resolver.sh" \
  || fail "could not extract upgrade_latest_tag from tools/kai/plugins/kai-upgrade"

# curl stub: writes $FIXTURE_HDR to the -D file and $FIXTURE_BODY to
# stdout, and exits $FIXTURE_RC. Mimics the flags the resolver passes.
run_case() {
  desc="$1"; want_tag="$2"; want_err="$3"
  UPGRADE_HDR="$work/hdr"
  rm -f "$UPGRADE_HDR" "$UPGRADE_HDR.err"

  out="$(
    FIXTURE_HDR="$FIXTURE_HDR" FIXTURE_BODY="$FIXTURE_BODY" \
    FIXTURE_RC="${FIXTURE_RC:-0}" UPGRADE_HDR="$UPGRADE_HDR" \
    sh -eu -c '
      curl() {
        hdr=""; prev=""
        for a in "$@"; do
          [ "$prev" = "-D" ] && hdr="$a"
          prev="$a"
        done
        [ -n "$hdr" ] && printf "%s" "$FIXTURE_HDR" > "$hdr"
        printf "%s" "$FIXTURE_BODY"
        return "$FIXTURE_RC"
      }
      . "$1"
      upgrade_latest_tag || true
    ' _ "$work/resolver.sh"
  )"
  err="$(cat "$UPGRADE_HDR.err" 2>/dev/null || true)"

  if [ -n "$want_tag" ]; then
    [ "$out" = "$want_tag" ] || fail "$desc: tag=[$out] want=[$want_tag]"
  else
    [ -z "$out" ] || fail "$desc: expected no tag, got [$out]"
    [ "$err" = "$want_err" ] || fail "$desc: err=[$err] want=[$want_err]"
  fi
  echo "test-upgrade-resolver: ok — $desc"
}

# 1. Newest matching tag wins; a junk tag ahead of it is ignored.
FIXTURE_HDR='HTTP/2 200
x-ratelimit-remaining: 58
'
FIXTURE_BODY='[{"name":"nightly"},{"name":"v0.99.12"},{"name":"v0.99.11"}]'
FIXTURE_RC=0
run_case "picks newest v-tag, skips junk" "v0.99.12" ""

# 2. Rate limit: 403 + remaining 0 + body → graceful, not raw 403.
FIXTURE_HDR='HTTP/2 403
x-ratelimit-remaining: 0
'
FIXTURE_BODY='{"message":"API rate limit exceeded for 1.2.3.4."}'
FIXTURE_RC=0
run_case "403 + remaining 0 → ratelimit" "" "ratelimit"

# 3. Network failure: curl non-zero → network.
FIXTURE_HDR=''
FIXTURE_BODY=''
FIXTURE_RC=6
run_case "curl failure → network" "" "network"

# 4. 200 but no matching tag → notag.
FIXTURE_HDR='HTTP/2 200
x-ratelimit-remaining: 57
'
FIXTURE_BODY='[{"name":"nightly"},{"name":"latest"}]'
FIXTURE_RC=0
run_case "no v-tag in response → notag" "" "notag"

# ---- the static manifest -----------------------------------------------
# The tags API above is now the *fallback*. The preferred path resolves
# the version from latest.json, which costs no API quota. Same technique:
# drive the live code from bin/kai, with a file:// KAIKAI_DIST_BASE
# standing in for the site so the whole thing runs offline.

awk '/^upgrade_manifest_entry\(\) \{/{f=1} f{print} f&&/^\}/{exit}' \
  "$ROOT/tools/kai/plugins/kai-upgrade" > "$work/manifest.sh"
grep -q 'upgrade_manifest_entry' "$work/manifest.sh" \
  || fail "could not extract upgrade_manifest_entry from tools/kai/plugins/kai-upgrade"
awk '/^upgrade_dist_base\(\) \{/{f=1} f{print} f&&/^\}/{exit}' \
  "$ROOT/tools/kai/plugins/kai-upgrade" >> "$work/manifest.sh"
# shellcheck disable=SC1090
. "$work/manifest.sh"

SHA_A="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
SHA_B="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

# Build the manifest with the release generator, so this gates the real
# output shape rather than a hand-written sample that could drift from it.
dist="$work/dist"; mkdir -p "$dist"
for p in darwin-arm64 linux-x86_64; do
  : > "$dist/kaikai-v0.99.12-$p.tar.gz"
done
printf '%s  kaikai-v0.99.12-darwin-arm64.tar.gz\n' "$SHA_A" >"$dist/kaikai-v0.99.12-darwin-arm64.tar.gz.sha256"
printf '%s  kaikai-v0.99.12-linux-x86_64.tar.gz\n' "$SHA_B" >"$dist/kaikai-v0.99.12-linux-x86_64.tar.gz.sha256"

site="$work/site"; mkdir -p "$site"
"$ROOT/scripts/gen-latest-json.sh" v0.99.12 "$dist" "$site/latest.json" 2>/dev/null \
  || fail "gen-latest-json.sh failed on a well-formed dist dir"

manifest_case() {
  desc="$1"; platform="$2"; base="$3"; want="$4"
  got="$(KAIKAI_DIST_BASE="$base" upgrade_manifest_entry "$platform" "$work/m.json" 2>/dev/null)" || got=""
  [ "$got" = "$want" ] || fail "$desc: got=[$got] want=[$want]"
  echo "test-upgrade-resolver: ok — $desc"
}

url_base="https://github.com/kaikailang-org/kaikai/releases/download/v0.99.12"
manifest_case "manifest resolves tag, url and sha256" darwin-arm64 "file://$site" \
  "$(printf 'v0.99.12\t%s/kaikai-v0.99.12-darwin-arm64.tar.gz\t%s' "$url_base" "$SHA_A")"

# The trap this pins: matching "sha256" across the whole document picks
# whichever platform sorts last, pairing one platform's tarball with
# another's digest — a checksum mismatch at best, wrong binary at worst.
manifest_case "picks the requested platform, not a sibling" linux-x86_64 "file://$site" \
  "$(printf 'v0.99.12\t%s/kaikai-v0.99.12-linux-x86_64.tar.gz\t%s' "$url_base" "$SHA_B")"

# Every decline below must fall through to the tags API, never abort:
# releases published before the manifest existed still have to upgrade.
manifest_case "declines a platform the manifest omits" windows-x86_64 "file://$site" ""
manifest_case "declines when latest.json is absent" darwin-arm64 "file://$work/no-such-site" ""

broken="$work/broken"; mkdir -p "$broken"
printf '{"schema":1,"tag":"not-a-tag"}\n' >"$broken/latest.json"
manifest_case "declines a malformed manifest" darwin-arm64 "file://$broken" ""

# ---- gen-latest-json.sh refuses to publish an unverifiable download ----
: > "$dist/kaikai-v0.99.12-darwin-x86_64.tar.gz"
if "$ROOT/scripts/gen-latest-json.sh" v0.99.12 "$dist" "$work/bad.json" >/dev/null 2>&1; then
  fail "gen-latest-json.sh accepted a tarball with no .sha256"
fi
echo "test-upgrade-resolver: ok — generator rejects a tarball with no .sha256"
rm -f "$dist/kaikai-v0.99.12-darwin-x86_64.tar.gz"

if "$ROOT/scripts/gen-latest-json.sh" not-a-tag "$dist" "$work/bad.json" >/dev/null 2>&1; then
  fail "gen-latest-json.sh accepted a malformed tag"
fi
echo "test-upgrade-resolver: ok — generator rejects a malformed tag"

# ---- #1569: a failed resolve must not exit 0 ---------------------------
# `tag="$(fn)"` under `set -e` terminates the shell at the assignment, so
# the error branches below it are dead code and the user sees silence.
grep -q 'tag="$(upgrade_latest_tag)" || true' "$ROOT/tools/kai/plugins/kai-upgrade" \
  || fail "bin/kai must split the tag assignment from its failure test (#1569)"
echo "test-upgrade-resolver: ok — bin/kai survives a failing resolve"

grep -q 'tag="$(latest_tag "$tmp/tags.hdr" "$tmp/tags.err")" || true' "$ROOT/install.sh" \
  || fail "install.sh must split the tag assignment from its failure test (#1569)"
echo "test-upgrade-resolver: ok — install.sh survives a failing resolve"

# ---- a release linked against newer system libraries is refused --------
# Both installers run the unpacked binaries before replacing anything; a
# tarball that does not start names what it needs and what the system has.
awk '/^check_requires\(\) \{/{f=1} f{print} f&&/^\}/{exit}' "$ROOT/install.sh" > "$work/req-install.sh"
awk '/^upgrade_check_requires\(\) \{/{f=1} f{print} f&&/^\}/{exit}' \
  "$ROOT/tools/kai/plugins/kai-upgrade" > "$work/req-upgrade.sh"

# fake_top <dir> <exit status of its binaries> [<REQUIRES body>]
fake_top() {
  mkdir -p "$1/bin" "$1/libexec/kaikai" "$1/share/kaikai"
  for b in bin/kai libexec/kaikai/kaic2; do
    printf '#!/bin/sh\nexit %s\n' "$2" > "$1/$b"
    chmod +x "$1/$b"
  done
  [ -z "${3:-}" ] || printf '%s\n' "$3" > "$1/share/kaikai/REQUIRES"
}
printf 'GLIBCXX_3.4.9\000GLIBCXX_3.4.30\000GLIBCXX_3.4\000' > "$work/libstdc++.so.6"

# requires_case <script> <function> <top>: the function's stderr and status,
# on a system reporting glibc 2.36 and the fake libstdc++ above.
requires_case() {
  FAKE_LIB="$work/libstdc++.so.6" sh -eu -c '
    err() { printf "%s\n" "$*" >&2; exit 1; }
    die() { printf "%s\n" "$*" >&2; exit 1; }
    getconf() { echo "glibc 2.36"; }
    ldconfig() { printf "\tlibstdc++.so.6 (libc6,AArch64) => %s\n" "$FAKE_LIB"; }
    . "$1"
    "$2" "$3"
  ' _ "$1" "$2" "$3" 2>&1
}

fake_top "$work/top-new" 127 "glibc 2.39
glibcxx 3.4.33"
fake_top "$work/top-ok" 0 "glibc 2.39
glibcxx 3.4.33"
fake_top "$work/top-unlisted" 127
for pair in "req-install.sh check_requires" "req-upgrade.sh upgrade_check_requires"; do
  script="$work/${pair% *}"; fn="${pair#* }"
  grep -q "$fn" "$script" || fail "could not extract $fn"
  if out="$(requires_case "$script" "$fn" "$work/top-new")"; then
    fail "$fn accepted a release whose binaries do not start"
  fi
  printf '%s\n' "$out" | grep -q 'needs: glibc 2.39 and libstdc++ GLIBCXX_3.4.33' \
    || fail "$fn does not name what the release needs: $out"
  printf '%s\n' "$out" | grep -q 'found: glibc 2.36 and libstdc++ GLIBCXX_3.4.30' \
    || fail "$fn does not name what the system has: $out"
  requires_case "$script" "$fn" "$work/top-ok" >/dev/null \
    || fail "$fn refused a release whose binaries start"
  requires_case "$script" "$fn" "$work/top-unlisted" >/dev/null \
    || fail "$fn checked a release that lists no requirement"
  echo "test-upgrade-resolver: ok — $fn refuses a release that does not start, naming both sides"
done

# ---- `kai upgrade` end to end: the start check guards the install ------
# The plugin itself runs against a fake prefix, served a tarball through a
# file:// manifest, so the check is proven to sit before the swap.
awk '/^upgrade_platform\(\) \{/{f=1} f{print} f&&/^\}/{exit}' \
  "$ROOT/tools/kai/plugins/kai-upgrade" > "$work/platform.sh"
platform="$(sh -eu -c '. "$1"; upgrade_platform' _ "$work/platform.sh")" || platform=""

# upgrade_case <name> <exit status of the release's binaries>: runs the
# plugin on a fresh v0.1.0 prefix offered v0.99.12; leaves its stderr in
# $work/<name>.err and its status in $up_rc.
upgrade_case() {
  site="$work/$1-site"; prefix="$work/$1-prefix"
  fake_top "$site/kaikai-v0.99.12-$platform" "$2" "glibc 2.39
glibcxx 3.4.33"
  ( cd "$site" && tar -czf "kaikai-v0.99.12-$platform.tar.gz" "kaikai-v0.99.12-$platform" )
  sha="$(cd "$site" && { shasum -a 256 "kaikai-v0.99.12-$platform.tar.gz" 2>/dev/null \
    || sha256sum "kaikai-v0.99.12-$platform.tar.gz"; } | awk '{print $1}')"
  printf '{"schema":1,"tag":"v0.99.12","platforms":{"%s":{"url":"file://%s/kaikai-v0.99.12-%s.tar.gz","sha256":"%s"}}}\n' \
    "$platform" "$site" "$platform" "$sha" > "$site/latest.json"
  mkdir -p "$prefix/bin"
  printf '#!/bin/sh\necho old\n' > "$prefix/bin/kai"
  chmod +x "$prefix/bin/kai"
  echo 0.1.0 > "$prefix/VERSION"
  up_rc=0
  KAIKAI_HOME="$prefix" KAIKAI_DIST_BASE="file://$site" \
    sh "$ROOT/tools/kai/plugins/kai-upgrade" > /dev/null 2> "$work/$1.err" || up_rc=$?
}

if [ -z "$platform" ]; then
  echo "test-upgrade-resolver: SKIP — kai upgrade end to end (no tarball for this platform)"
else
  upgrade_case refused 127
  [ "$up_rc" -ne 0 ] || fail "kai upgrade installed a release whose binaries do not start"
  grep -q 'does not start on this system' "$work/refused.err" \
    || fail "kai upgrade refused without saying why: $(cat "$work/refused.err")"
  grep -q 'needs: glibc 2.39 and libstdc++ GLIBCXX_3.4.33' "$work/refused.err" \
    || fail "kai upgrade does not name what the release needs: $(cat "$work/refused.err")"
  [ "$("$work/refused-prefix/bin/kai")" = old ] \
    || fail "kai upgrade replaced the install before its start check"
  echo "test-upgrade-resolver: ok — kai upgrade leaves the install alone when the release does not start"

  upgrade_case accepted 0
  [ "$up_rc" -eq 0 ] || fail "kai upgrade refused a release that starts: $(cat "$work/accepted.err")"
  [ -f "$work/accepted-prefix/share/kaikai/REQUIRES" ] \
    || fail "kai upgrade did not install a release that starts"
  echo "test-upgrade-resolver: ok — kai upgrade installs a release that starts"
fi

echo "test-upgrade-resolver: all cases passed"
