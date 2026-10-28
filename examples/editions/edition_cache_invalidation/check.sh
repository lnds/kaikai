#!/bin/sh
# Edition cache-isolation smoke: shared-cache entries written under one
# edition must never be served to a build under another. The isolation
# lives in the cache KEY, not the directory layout — the native core
# objects (ncore-*.o) fold the edition into their content key — so
# switching editions must create a sibling entry and leave the other
# edition's entry untouched.
#
# Native, against a private core-cache root:
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

# The C backend keeps no artifact keyed by edition: its cache is content-
# addressed by the emitted translation units. The isolation to show is in the
# result: a program whose meaning depends on the edition (convention pipe
# dispatch, hanga-roa and later) must flip between rejected and running as the
# edition flips, with the cache warm from the previous build.
GATED="$WORK/gated"
mkdir -p "$GATED"
cp "$FIXTURE/pipe_main.kai.txt" "$GATED/main.kai"
cp "$FIXTURE/box.kai.txt" "$GATED/box.kai"
gated_build() {
  printf 'name = "gated"\nversion = "0.1.0"\nedition = "%s"\n' "$1" > "$GATED/kai.toml"
  (cd "$GATED" && "$KAI" build --backend=c main.kai -o "$2") >"$WORK/gated.log" 2>&1
}
for step in "hanga-roa ok" "tongariki rejected" "hanga-roa ok" "tongariki rejected"; do
  set -- $step
  if gated_build "$1" "g-$1"; then got=ok; else got=rejected; fi
  [ "$got" = "$2" ] || { echo "FAIL(c): $1 build was $got, want $2 (stale artifact across an edition switch?)"; cat "$WORK/gated.log"; exit 1; }
  if [ "$got" = ok ]; then
    [ "$("$GATED/g-$1")" = "42" ] || { echo "FAIL(c): $1 binary printed the wrong result"; exit 1; }
  fi
done
echo "edition-cache-isolation OK (c): hanga-roa runs, tongariki rejects, hanga-roa runs again, tongariki rejects again — warm cache"
rm -rf "$CACHE"

set_edition "tongariki"
if (cd "$PKG" && "$KAI" build --backend=native main.kai -o probe) >/dev/null 2>&1; then
  rm -rf "$CACHE"
  check_layer "native" "ncore-*.o" "native NCO1"
else
  echo "edition-cache-isolation SKIP (native NCO1): kaic2 cannot run the native backend"
fi
