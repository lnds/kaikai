#!/usr/bin/env bash
# prebuilt-kaic2.sh — one kaic2 build per commit, shared by every worktree of the repo.
#
#   tools/prebuilt-kaic2.sh build [<ref>]    build kaic2 + bin/kai at <ref> (default origin/main)
#                                            once, native-capable where llvm-config is found,
#                                            and store it by commit
#   tools/prebuilt-kaic2.sh seed [<worktree>] give a worktree (default .) the stored build of its
#                                            HEAD; with none, the newest stored one rebuilt by
#                                            `make kaic2-fast` (C backend only)
#   tools/prebuilt-kaic2.sh has [<ref>]      exit 0 iff a build of <ref> is stored
#
# Store: $KAI_PREBUILT_STORE, else ${XDG_CACHE_HOME:-~/.cache}/kaikai-prebuilt; the newest
# $KAI_PREBUILT_KEEP (default 3) builds are kept.
set -eu

REPO="$(dirname "$(git rev-parse --path-format=absolute --git-common-dir)")"
STORE="${KAI_PREBUILT_STORE:-${XDG_CACHE_HOME:-$HOME/.cache}/kaikai-prebuilt}"
KEEP="${KAI_PREBUILT_KEEP:-3}"
# In dependency order: touch_chain relies on it.
FILES=(
  stage0/runtime_llvm.bc stage0/runtime_llvm.bc.stamp stage0/runtime_inline.bc
  stage2/build/stage2.c stage2/build/stage2.c.id stage2/kaic2 stage2/build/kaic2.id
  bin/kai
)

die() { echo "prebuilt-kaic2: $*" >&2; exit 2; }
sha_of() { git -C "$REPO" rev-parse --verify "${1:-origin/main}^{commit}"; }
stored() { ls -1td "$STORE"/*/ 2>/dev/null | grep -v '/tree/$' || true; }

copy_files() { # from-root to-root
  local f
  for f in "${FILES[@]}"; do
    [ -e "$1/$f" ] || continue
    mkdir -p "$2/$(dirname "$f")"
    cp -p "$1/$f" "$2/$f"
  done
}

# Copies land with arbitrary mtimes relative to the checkout: touch the chain
# `make kaic2` consults, inputs first, so every target is newer than its inputs.
touch_chain() {
  local f
  for f in "${FILES[@]}"; do
    [ -e "$1/$f" ] && { touch "$1/$f"; sleep 1; }
  done
}

# Prints the env assignments that make llvm-config (and, on macOS, its libraries) visible.
llvm_env() {
  local d
  command -v llvm-config >/dev/null && return 0
  for d in /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin; do
    [ -x "$d/llvm-config" ] || continue
    echo "PATH=$d:$PATH"
    echo "LIBRARY_PATH=${d%/opt/llvm/bin}/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"
    return 0
  done
  return 1
}

cmd_build() {
  local sha tree out env=() native=0 line
  git -C "$REPO" fetch -q origin
  sha="$(sha_of "${1:-origin/main}")"
  [ -x "$STORE/$sha/stage2/kaic2" ] && { echo "prebuilt-kaic2: $sha already stored"; return 0; }
  if out="$(llvm_env)"; then
    native=1
    while IFS= read -r line; do [ -n "$line" ] && env+=("$line"); done <<<"$out"
  fi
  tree="$STORE/tree"
  mkdir -p "$STORE"
  if [ -e "$tree/.git" ]; then
    git -C "$tree" checkout -q --detach "$sha"
  else
    git -C "$REPO" worktree prune
    git -C "$REPO" worktree add -q --detach "$tree" "$sha"
  fi
  rm -f "$tree/stage2/kaic2"
  (cd "$tree" && env ${env[@]+"${env[@]}"} make KAI_LLVM=$native kaic2 bin/kai) >"$STORE/build-$sha.log" 2>&1 \
    || die "build of $sha failed; see $STORE/build-$sha.log"
  copy_files "$tree" "$STORE/$sha.tmp"
  mv "$STORE/$sha.tmp" "$STORE/$sha"
  stored | tail -n +$((KEEP + 1)) | while IFS= read -r d; do rm -rf "$d"; done
  rm -f "$STORE/build-$sha.log"
  echo "prebuilt-kaic2: stored $sha ($([ $native = 1 ] && echo native-capable || echo C backend only))"
}

cmd_seed() {
  local wt="${1:-.}" sha newest
  sha="$(git -C "$wt" rev-parse HEAD)"
  if [ -x "$STORE/$sha/stage2/kaic2" ]; then
    copy_files "$STORE/$sha" "$wt"
    touch_chain "$wt"
    echo "prebuilt-kaic2: restored the build of $sha"
    return 0
  fi
  newest="$(stored | head -1)"
  [ -n "$newest" ] && [ -x "$newest/stage2/kaic2" ] || die "nothing stored; run make kaic2"
  copy_files "${newest%/}" "$wt"
  touch_chain "$wt"
  (cd "$wt" && make kaic2-fast bin/kai >/dev/null)
  echo "prebuilt-kaic2: seeded from $(basename "$newest") and rebuilt by kaic2-fast (C backend only)"
}

case "${1:-}" in
  build) shift; cmd_build "$@" ;;
  seed)  shift; cmd_seed "$@" ;;
  has)   shift; [ -x "$STORE/$(sha_of "${1:-origin/main}")/stage2/kaic2" ] ;;
  *) sed -n '2,14p' "$0"; exit 2 ;;
esac
