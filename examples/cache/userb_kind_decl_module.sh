#!/bin/sh
# User-declared kind in a cached module — cold == warm == oracle.
#
# A `kind` declaration (plus its habitant decls and `use kind`) lives
# in an imported module of a cached project. The parse-level module
# cache serialises the module's decls straight after parse, BEFORE the
# kind-resolution pass strips the catalog decls from the merged stream,
# so `DKind` / `DUseKind` must round-trip through the codec: a blob
# restored without them would lose the module's kind registrations on a
# warm hit, and a codec that rejects them makes any user kind in a
# package unusable under the default cache.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"

# A flagless kaic2 runs the oldest edition; the cache keys carry the
# edition, so this fixture must exercise the one the repo declares.
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

cat > "$PROJ/walls.kai" <<'EOF'
pub kind Wall : Module over T with wt
use kind Wall

pub wt hour
pub wt minute

pub fn to_minutes(h: Int<hour>) : Int = __detach_unit(h) * 60
EOF
cat > "$PROJ/main.kai" <<'EOF'
import walls

fn main() : Unit / Console {
  print(int_to_string(walls.to_minutes(2<hour>)))
}
EOF

mkdir -p "$PROJ/.kai-cache"

# Oracle: cache disabled.
"$KAIC2" $EDITION_FLAG --path "$PROJ" "$PROJ/main.kai" > "$PROJ/oracle.c" 2>/dev/null

# Cold: cache enabled, empty cache -> miss, module blob written.
rm -f "$PROJ/.kai-cache/"*.kab 2>/dev/null || true
"$KAIC2" $EDITION_FLAG --user-cache --path "$PROJ" "$PROJ/main.kai" > "$PROJ/cold.c" 2>/dev/null

# Warm: cache enabled, blob present -> hit, parse skipped.
"$KAIC2" $EDITION_FLAG --user-cache --path "$PROJ" "$PROJ/main.kai" > "$PROJ/warm.c" 2>/dev/null

if ! cmp -s "$PROJ/cold.c" "$PROJ/oracle.c"; then
  echo "userb_kind_decl_module FAIL — cold-cache C differs from no-cache oracle"
  diff "$PROJ/oracle.c" "$PROJ/cold.c" | head -20
  exit 1
fi
if ! cmp -s "$PROJ/warm.c" "$PROJ/oracle.c"; then
  echo "userb_kind_decl_module FAIL — warm-cache C differs from no-cache oracle"
  diff "$PROJ/oracle.c" "$PROJ/warm.c" | head -20
  exit 1
fi

# Sanity: the warm run actually read a blob (the cache was populated).
n="$(ls "$PROJ/.kai-cache/"*.kab 2>/dev/null | wc -l | tr -d ' ')"
if [ "$n" = "0" ]; then
  echo "userb_kind_decl_module FAIL — no blobs written, hit path never exercised"
  exit 1
fi

echo "userb_kind_decl_module OK — cold == warm == oracle, $n blob(s) cached"
exit 0
