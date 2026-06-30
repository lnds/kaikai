#!/bin/sh
# tools/test-info.sh — smoke test for `kai info`.
#
# Asserts:
#   1. `kai info` lists topics (non-empty output, exit 0).
#   2. `kai info --list` matches the .md files in docs/info/.
#   3. Every topic page is non-empty (`kai info <topic>` returns text).
#   4. Every topic page emits valid JSON under --json.
#   5. The `syntax` topic exists (load-bearing — CLAUDE.md cites it).
#   6. `kai info -k <keyword>` searches page bodies, not just names.
#
# Catches: deleted .md, broken cmd_info dispatcher, JSON-escape
# regressions, awk shape drift.

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
KAI="$ROOT/bin/kai"

fail() {
  echo "test-info: FAIL: $1" >&2
  exit 1
}

# 1. `kai info` returns text and exits clean.
out="$("$KAI" info 2>&1)" || fail "'kai info' exited non-zero"
echo "$out" | grep -q "^Topics:" || fail "'kai info' missing 'Topics:' header"

# 2. `--list` matches docs/info/*.md.
listed="$("$KAI" info --list 2>&1)" || fail "'kai info --list' exited non-zero"
fs_topics="$(ls "$ROOT/docs/info" 2>/dev/null | sed -n 's/\.md$//p' | sort)"
if [ "$listed" != "$fs_topics" ]; then
  echo "test-info: --list output does not match docs/info/ contents" >&2
  echo "--- listed:" >&2; echo "$listed" >&2
  echo "--- on disk:" >&2; echo "$fs_topics" >&2
  exit 1
fi

# 3. Every topic produces non-empty output.
# 4. Every topic produces valid JSON.
need_json_validator=1
if ! command -v python3 >/dev/null 2>&1; then
  echo "test-info: warning: python3 not found; skipping JSON validation" >&2
  need_json_validator=0
fi

count=0
for topic in $listed; do
  page="$("$KAI" info "$topic" 2>&1)" || fail "'kai info $topic' exited non-zero"
  [ -n "$page" ] || fail "'kai info $topic' produced empty output"
  echo "$page" | grep -q "^# " || fail "'kai info $topic' missing H1 title"

  if [ "$need_json_validator" = 1 ]; then
    "$KAI" info "$topic" --json 2>&1 | python3 -c "
import sys, json
try:
    d = json.load(sys.stdin)
except Exception as e:
    sys.stderr.write('JSON parse failure: %s\n' % e)
    sys.exit(1)
if d.get('topic') != '$topic':
    sys.stderr.write('topic mismatch: %r\n' % d.get('topic')); sys.exit(1)
if not d.get('title'):
    sys.stderr.write('missing title\n'); sys.exit(1)
if not d.get('sections'):
    sys.stderr.write('no sections\n'); sys.exit(1)
" || fail "'kai info $topic --json' invalid"
  fi

  count=$((count + 1))
done

# 5. The `syntax` topic exists (CLAUDE.md cites it as load-bearing).
"$KAI" info syntax >/dev/null 2>&1 || fail "'kai info syntax' must exist; CLAUDE.md cites it"

# 6. `-k` searches page bodies. `tuple` lives only in syntax's body
#    (§N-tuples), so a body-search must surface `syntax`.
kw_out="$("$KAI" info -k tuple 2>&1)" || fail "'kai info -k tuple' exited non-zero (body search regressed)"
echo "$kw_out" | grep -q "syntax" || fail "'kai info -k tuple' did not surface 'syntax'"
"$KAI" info -k nursery >/dev/null 2>&1 || fail "'kai info -k nursery' exited non-zero (name/tagline search regressed)"
if "$KAI" info -k zzznotarealword >/dev/null 2>&1; then
  fail "'kai info -k zzznotarealword' should exit non-zero"
fi

echo "test-info: OK — $count topics, plain + JSON output, -k body search"
