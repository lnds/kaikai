#!/bin/sh
# Issue #825 core-cache invalidation fixture — editing a core file
# invalidates its blob and forces a re-parse.
#
# THE catastrophic failure mode for a core cache: serving a stale core
# after a stdlib edit, so every program typechecks against old symbols.
# This fixture proves it cannot happen. We copy the stdlib to a temp
# tree (so we can edit it), point KAIKAI_STDLIB_PATH there, warm the
# cache, then edit core/list.kai. Because the blob filename embeds the
# source content hash, the edit changes list.kai's hash -> a new blob
# name -> the old blob is never read. The build after the edit must
# emit the SAME C as a fresh no-cache build of the edited stdlib (the
# oracle), i.e. the cache served the edited decls, not the stale ones.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
TMPSTD="$(mktemp -d)"
PROJ="$(mktemp -d)"
trap 'rm -rf "$TMPSTD" "$PROJ"' EXIT INT TERM

cp -R "$ROOT/stdlib/." "$TMPSTD/"

cat > "$PROJ/main.kai" <<'EOF'
fn main() : Unit / Console =
  print("sum=#{int_to_string(list.sum([1, 2, 3]))}")
EOF
mkdir -p "$PROJ/.kai-cache"

# Warm the cache against the pristine copy.
KAIKAI_STDLIB_PATH="$TMPSTD" "$KAIC2" --user-cache --path "$TMPSTD" --path "$PROJ" \
  "$PROJ/main.kai" > "$PROJ/warm_before.c" 2>/dev/null
before="$(ls "$PROJ/.kai-cache/core-"*.kab 2>/dev/null | wc -l | tr -d ' ')"
if [ "$before" = "0" ]; then
  echo "corec_edit_core_file FAIL — cache not populated on the first run"
  exit 1
fi

# Edit a core file. Any byte change flips its content hash.
printf '\n# issue #825 invalidation probe\n' >> "$TMPSTD/core/list.kai"

# Build again with the cache on. The edited list.kai must miss (new
# hash, new blob name) and re-parse; everything else stays a hit.
KAIKAI_STDLIB_PATH="$TMPSTD" "$KAIC2" --user-cache --path "$TMPSTD" --path "$PROJ" \
  "$PROJ/main.kai" > "$PROJ/after.c" 2>/dev/null

# Oracle: a fresh no-cache build of the EDITED stdlib.
KAIKAI_STDLIB_PATH="$TMPSTD" "$KAIC2" --path "$TMPSTD" --path "$PROJ" \
  "$PROJ/main.kai" > "$PROJ/oracle_edited.c" 2>/dev/null

if ! cmp -s "$PROJ/after.c" "$PROJ/oracle_edited.c"; then
  echo "corec_edit_core_file FAIL — post-edit build does not match the edited-stdlib oracle"
  echo "(the cache served a STALE core — the catastrophic failure this fixture guards)"
  diff "$PROJ/oracle_edited.c" "$PROJ/after.c" | head -20
  exit 1
fi

# A fresh blob was written for the edited module (old one orphaned).
after="$(ls "$PROJ/.kai-cache/core-"*.kab 2>/dev/null | wc -l | tr -d ' ')"
if [ "$after" -le "$before" ]; then
  echo "corec_edit_core_file FAIL — no new blob after the edit (expected $before -> $((before + 1)))"
  exit 1
fi

echo "corec_edit_core_file OK — core edit invalidated the blob, fresh decls served ($before -> $after blobs)"
exit 0
