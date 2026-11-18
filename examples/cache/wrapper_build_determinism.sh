#!/bin/sh
# The binary `kai build` writes is a function of the sources, the toolchain
# and the flags: the same bytes on a cold cache, on a warm one, and for the
# same package at another path. Each backend builds the package cold, warm
# and from a copy at a second path, with the binary memo off and on.

set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
KAI="$ROOT/bin/kai"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT INT TERM

fail() { echo "wrapper_build_determinism: FAIL — $1"; exit 1; }

mkdir -p "$WORK/one/pkg"
cat > "$WORK/one/pkg/kai.toml" <<'EOF'
name = "determinism_probe"
version = "0.1.0"
EOF
cat > "$WORK/one/pkg/main.kai" <<'EOF'
fn main() : Unit / Console {
  print("sum=#{int_to_string(list.sum([1, 2, 3]))}")
}
EOF

# $1: package dir, $2: cache dir, $3: memo (0|1), $4: backend flag, $5: output dir
build() {
  mkdir -p "$5"
  (cd "$1" && KAI_CORE_CACHE_DIR="$2" KAI_BIN_MEMO="$3" "$KAI" build "$4" . -o "$5/out" > "$5/log" 2>&1) \
    || { cat "$5/log"; fail "build failed ($4, memo $3)"; }
}

same() { cmp -s "$1" "$2" || fail "$3 (sizes $(wc -c < "$1") and $(wc -c < "$2"))"; }

check() {
  label="$1"; flag="$2"; memo="$3"
  base="$WORK/$label-memo$memo"
  rm -rf "$base"
  mkdir -p "$base"
  if [ "$flag" = "--backend=native" ] && [ "$NATIVE" = 0 ]; then
    echo "wrapper_build_determinism SKIP ($label) — native backend unavailable"
    return 0
  fi
  build "$WORK/one/pkg" "$base/cache" "$memo" "$flag" "$base/cold"
  build "$WORK/one/pkg" "$base/cache" "$memo" "$flag" "$base/warm"
  mkdir -p "$base/two/deeper/place"
  cp -R "$WORK/one/pkg" "$base/two/deeper/place/pkg"
  build "$base/two/deeper/place/pkg" "$base/cache" "$memo" "$flag" "$base/moved"
  build "$base/two/deeper/place/pkg" "$base/cache2" "$memo" "$flag" "$base/moved-cold"
  [ "$("$base/cold/out")" = "sum=6" ] || fail "$label program output wrong"
  same "$base/cold/out" "$base/warm/out" "$label: cold and warm builds differ (memo $memo)"
  same "$base/warm/out" "$base/moved/out" "$label: the same package at another path differs (memo $memo)"
  same "$base/cold/out" "$base/moved-cold/out" "$label: a cold build at another path differs (memo $memo)"
  echo "wrapper_build_determinism OK ($label, memo $memo) — cold, warm and moved builds are byte-identical"
}

mkdir -p "$WORK/probe"
NATIVE=1
(cd "$WORK/one/pkg" && KAI_CORE_CACHE_DIR="$WORK/probe/cache" "$KAI" build --backend=native . -o "$WORK/probe/out" > "$WORK/probe.log" 2>&1) || NATIVE=0

for memo in 0 1; do
  check c --backend=c "$memo"
  check native --backend=native "$memo"
done

# Every cache switch off, the package at two paths.
if [ "$NATIVE" = 1 ]; then
  off="$WORK/off"
  mkdir -p "$off/a" "$off/b/deeper/place"
  cp -R "$WORK/one/pkg" "$off/b/deeper/place/pkg"
  for pair in "a:$WORK/one/pkg" "b:$off/b/deeper/place/pkg"; do
    (cd "${pair#*:}" && KAI_BIN_MEMO=0 KAI_NATIVE_CORE_OBJ=0 KAI_CACHE=0 KAI_CORE_CACHE_DIR="$off/cc" \
       "$KAI" build --backend=native . -o "$off/${pair%%:*}/out" > "$off/log" 2>&1) \
      || { cat "$off/log"; fail "switches-off build failed"; }
  done
  same "$off/a/out" "$off/b/out" "switches off: the same package at two paths differs"
  echo "wrapper_build_determinism OK (switches off) — two paths are byte-identical"
fi
