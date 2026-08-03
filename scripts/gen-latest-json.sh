#!/bin/sh
# gen-latest-json.sh — build the static release manifest served at
# https://kaikai-lang.org/latest.json, so `kai upgrade` and install.sh
# can resolve the current version without touching api.github.com.
#
#   gen-latest-json.sh <tag> <dist-dir> <out-file>
#
# Platforms are derived from the tarballs actually present in <dist-dir>,
# never from a hardcoded list: a release that built fewer platforms
# publishes fewer entries rather than advertising a 404.

set -eu

die() { printf 'gen-latest-json.sh: error: %s\n' "$*" >&2; exit 1; }

[ $# -eq 3 ] || die "usage: gen-latest-json.sh <tag> <dist-dir> <out-file>"

tag="$1"
dist="$2"
out="$3"

case "$tag" in
  v[0-9]*.[0-9]*.[0-9]*) ;;
  *) die "tag '$tag' is not vX.Y.Z" ;;
esac
version="${tag#v}"

[ -d "$dist" ] || die "dist dir '$dist' does not exist"

base="https://github.com/kaikailang-org/kaikai/releases/download/$tag"

# One "<platform>\t<sha256>" line per tarball that has a sibling
# .sha256. A tarball without its checksum is a broken upload, not a
# platform to advertise.
entries=""
for tarball in "$dist"/kaikai-"$tag"-*.tar.gz; do
  [ -f "$tarball" ] || continue
  name="$(basename "$tarball")"
  platform="${name#kaikai-$tag-}"
  platform="${platform%.tar.gz}"
  [ -n "$platform" ] || die "cannot derive platform from $name"

  sum_file="$tarball.sha256"
  [ -f "$sum_file" ] || die "$name has no $name.sha256 next to it"
  sha="$(awk 'NR==1{print $1}' "$sum_file")"
  case "$sha" in
    ????????????????????????????????????????????????????????????????) ;;
    *) die "invalid sha256 for $name: '$sha'" ;;
  esac

  entries="$entries$platform	$sha
"
done

[ -n "$entries" ] || die "no kaikai-$tag-*.tar.gz found in $dist"

{
  printf '{\n'
  printf '  "schema": 1,\n'
  printf '  "version": "%s",\n' "$version"
  printf '  "tag": "%s",\n' "$tag"
  printf '  "platforms": {\n'
  first=1
  printf '%s' "$entries" | while IFS='	' read -r platform sha; do
    [ -n "$platform" ] || continue
    [ "$first" -eq 1 ] || printf ',\n'
    first=0
    printf '    "%s": {\n' "$platform"
    printf '      "url": "%s/kaikai-%s-%s.tar.gz",\n' "$base" "$tag" "$platform"
    printf '      "sha256": "%s"\n' "$sha"
    printf '    }'
  done
  printf '\n  }\n'
  printf '}\n'
} >"$out"

printf 'wrote %s\n' "$out" >&2
cat "$out" >&2
