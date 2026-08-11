#!/bin/sh
# Edition cache-isolation smoke: shared-cache entries written under one
# edition must never be served to a build under another. The isolation
# lives in the cache KEY, not the directory layout — the c-modular
# core-emit entries (core-emit-*.kct) and the native core objects
# (ncore-*.o) both fold the edition into their content key — so
# switching editions must create a sibling entry and leave the other
# edition's entry untouched.
#
# Per backend, against a private core-cache root:
#   1. build under tongariki  -> at least one entry appears
#   2. build under hanga-roa  -> a NEW entry appears; tongariki's survive
#   3. rebuild under tongariki -> pure hit, the entry set is unchanged
#
# The native leg is skipped on a C-only kaic2 (explicit --backend=native
# errors there, which doubles as the probe).
#
# Run from the repo root:
#   sh examples/editions/edition_cache_invalidation/check.sh

set -eu
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
KAI="$ROOT/bin/kai"
FIXTURE="$ROOT/examples/editions/edition_cache_invalidation"
WORK="$(mktemp -d -t kaikai-edcache.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

PKG="$WORK/pkg"
CACHE="$WORK/cache"
mkdir -p "$PKG"
cp "$FIXTURE/main.kai" "$PKG/main.kai"
export KAI_CORE_CACHE_DIR="$CACHE"

set_edition() {
  printf 'name = "edition_cache_invalidation"\nversion = "0.1.0"\nedition = "%s"\n' \
    "$1" > "$PKG/kai.toml"
}

build() {
  backend="$1"; out="$2"
  (cd "$PKG" && "$KAI" build "--backend=$backend" main.kai -o "$out") \
    >"$WORK/build.log" 2>&1 || {
    echo "FAIL: kai build --backend=$backend failed:"
    cat "$WORK/build.log"
    exit 1
  }
}

entries() {
  find "$CACHE" -name "$1" 2>/dev/null | sed 's|.*/||' | sort
}

# One backend leg: three builds, asserting the per-edition entry sets.
check_layer() {
  backend="$1"; pattern="$2"; layer="$3"

  # Warm-up: implicit wrapper side-builds (e.g. tools/kai-pkg) also write
  # core entries under the repo edition; flush them out of the measured set.
  set_edition "tongariki"
  build "$backend" "warm"
  rm -rf "$CACHE"

  build "$backend" "m1"
  s1="$(entries "$pattern")"
  [ -n "$s1" ] || { echo "FAIL($layer): no $pattern entry after tongariki build"; exit 1; }

  set_edition "hanga-roa"
  build "$backend" "m2"
  s2="$(entries "$pattern")"
  for e in $s1; do
    echo "$s2" | grep -qx "$e" || {
      echo "FAIL($layer): tongariki entry $e gone after hanga-roa build"; exit 1; }
  done
  [ "$s1" != "$s2" ] || {
    echo "FAIL($layer): hanga-roa build reused the tongariki entry set (no new key)"
    exit 1
  }

  set_edition "tongariki"
  build "$backend" "m3"
  s3="$(entries "$pattern")"
  [ "$s2" = "$s3" ] || {
    echo "FAIL($layer): tongariki rebuild changed the entry set (expected pure hit)"
    echo "before:"; echo "$s2"; echo "after:"; echo "$s3"
    exit 1
  }

  n1="$(echo "$s1" | wc -l | tr -d ' ')"
  n2="$(echo "$s2" | wc -l | tr -d ' ')"
  echo "edition-cache-isolation OK ($layer): tongariki=$n1 entries; +hanga-roa=$n2, disjoint keys, hits stable"
  rm -rf "$CACHE"
}

check_layer "c" "core-emit-*.kct" "c-modular KCT1"

set_edition "tongariki"
if (cd "$PKG" && "$KAI" build --backend=native main.kai -o probe) >/dev/null 2>&1; then
  rm -rf "$CACHE"
  check_layer "native" "ncore-*.o" "native NCO1"
else
  echo "edition-cache-isolation SKIP (native NCO1): kaic2 cannot run the native backend"
fi
