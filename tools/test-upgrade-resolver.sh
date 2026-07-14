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

echo "test-upgrade-resolver: all cases passed"
