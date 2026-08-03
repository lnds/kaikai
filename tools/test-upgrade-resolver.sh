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
# It drives the live `upgrade_latest_tag` extracted from bin/kai with a
# curl stub serving local fixtures — no network, no GitHub Release.

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

fail() { echo "test-upgrade-resolver: FAIL: $1" >&2; exit 1; }

work="$(mktemp -d "${TMPDIR:-/tmp}/kai-upgrade-test.XXXXXX")"
trap 'rm -rf "$work"' EXIT INT TERM

# Extract the live resolver from bin/kai so we test the shipped code.
awk '/^upgrade_latest_tag\(\) \{/{f=1} f{print} f&&/^\}/{exit}' \
  "$ROOT/bin/kai" > "$work/resolver.sh"
grep -q 'upgrade_latest_tag' "$work/resolver.sh" \
  || fail "could not extract upgrade_latest_tag from bin/kai"

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
  "$ROOT/bin/kai" > "$work/manifest.sh"
grep -q 'upgrade_manifest_entry' "$work/manifest.sh" \
  || fail "could not extract upgrade_manifest_entry from bin/kai"
awk '/^upgrade_dist_base\(\) \{/{f=1} f{print} f&&/^\}/{exit}' \
  "$ROOT/bin/kai" >> "$work/manifest.sh"
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
grep -q 'tag="$(upgrade_latest_tag)" || true' "$ROOT/bin/kai" \
  || fail "bin/kai must split the tag assignment from its failure test (#1569)"
echo "test-upgrade-resolver: ok — bin/kai survives a failing resolve"

grep -q 'tag="$(latest_tag "$tmp/tags.hdr" "$tmp/tags.err")" || true' "$ROOT/install.sh" \
  || fail "install.sh must split the tag assignment from its failure test (#1569)"
echo "test-upgrade-resolver: ok — install.sh survives a failing resolve"

echo "test-upgrade-resolver: all cases passed"
