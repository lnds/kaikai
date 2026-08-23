#!/bin/sh
# Native-modular routing gate for the dotted-import layout.
#
# bin/kai decides "multi-module" — and therefore whether the partitioned
# native path with its per-partition .o cache engages — by probing whether an
# entry-file import resolves to a local module. `import a.b` resolves as the
# path `a/b.kai`, the layout a package with its modules under a subdirectory
# uses. A probe that truncates at the first dot only tests `a.kai`, finds
# nothing, and silently routes the whole package onto the whole-program path,
# forfeiting the cache it was built for — a ~2x edit-loop regression that
# nothing else in the suite observes, because output stays correct.
#
# The observable is the cache dir: the partitioned path populates it with one
# .o per module, the whole-program path leaves it empty.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="$ROOT/bin/kai"
KAIC2="$ROOT/stage2/kaic2"

# A flagless kaic2 runs the oldest edition; the cache keys carry the
# edition, so this fixture must exercise the one the repo declares.
EDITION_FLAG="--edition $(cat "$ROOT/EDITION")"
PROJ="$(mktemp -d)"
trap 'rm -rf "$PROJ"' EXIT INT TERM

native_capable=0
if [ -x "$KAIC2" ]; then
  printf 'fn main() : Unit = ()\n' > "$PROJ/probe.kai"
  "$KAIC2" $EDITION_FLAG --emit=native --path "$ROOT/stdlib" --path "$PROJ" "$PROJ/probe.kai" \
    >/dev/null 2>"$PROJ/native-probe.err" || true
  grep -q "not built into this compiler" "$PROJ/native-probe.err" 2>/dev/null || native_capable=1
fi
if [ "$native_capable" != "1" ]; then
  echo "nmodular_subdir_import_routing SKIP — kaic2 has no libLLVM native backend"
  exit 0
fi

mkdir -p "$PROJ/lib"
cat > "$PROJ/lib/dom.kai" <<'EOF'
#[derive(Show)]
pub type Mov = { id: Int, glosa: String }
EOF
cat > "$PROJ/main.kai" <<'EOF'
import lib.dom

fn main() {
  let ms = [Mov { id: 1, glosa: "uno" }]
  println("#{ms}")
}
EOF

nmroot="$PROJ/nmcache"
out="$PROJ/out"

KAI_NATIVE_MODULAR_CACHE_DIR="$nmroot" KAI_CORE_CACHE_DIR="$PROJ/core-cache" \
  KAI_BACKEND=native "$KAI" build "$PROJ/main.kai" -o "$out" 2>"$PROJ/build.err" 1>/dev/null \
  || { echo "nmodular_subdir_import_routing FAIL — build failed"; cat "$PROJ/build.err"; exit 1; }

got="$("$out")"
[ "$got" = "[Mov { id: 1, glosa: uno }]" ] \
  || { echo "nmodular_subdir_import_routing FAIL — wrong output: '$got'"; exit 1; }

objs=$(find "$nmroot" -name '*.o' -not -path '*/runtime/*' 2>/dev/null | wc -l | tr -d ' ')
[ "$objs" -ge 2 ] || {
  echo "nmodular_subdir_import_routing FAIL — cached $objs partition objects (want >= 2);"
  echo "  a dotted import into a subdirectory did not route onto the native-modular path,"
  echo "  so the per-partition object cache never engaged (see nm_local_imports in bin/kai)."
  exit 1
}

echo "nmodular_subdir_import_routing OK — a dotted subdirectory import routes onto the partitioned path"
exit 0
