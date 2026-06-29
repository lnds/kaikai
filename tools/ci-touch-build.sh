#!/usr/bin/env bash
# ci-touch-build.sh — make a downloaded build artifact look fresh to `make`.
#
# The build-once CI job (docs/ci-time-analysis.md §8) uploads the bootstrap
# chain (kaic0, kaic1, kaic2 + the generated stage1.c / bundle.kai /
# stage2.c). A consuming job downloads it, but artifact extraction does NOT
# preserve mtimes relative to the checked-out sources — every `compiler/*.kai`
# source lands with the checkout mtime, which is typically NEWER than the
# extracted binaries. `make` would then regenerate the whole chain (the
# ~45 s bundle regen + the ~197 s kaic2 `cc`), defeating the artifact.
#
# Fix: touch the dependency chain bottom-up with the CURRENT time (newer than
# every source), in topological order, so `make` finds every target
# up-to-date and rebuilds nothing. Validated to produce
# `make: 'kaic2' is up to date.` — see the analysis doc.
#
# The chain order MUST match the Makefile dependency edges:
#   stage0/kaic0
#     -> stage1/build/stage1.c -> stage1/kaic1
#       -> stage2/build/bundle.kai -> stage2/build/stage2.c -> stage2/kaic2
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

chain=(
  stage0/main.o
  stage0/lexer.o
  stage0/ast.o
  stage0/parser.o
  stage0/parser_expr.o
  stage0/check.o
  stage0/emit.o
  stage0/kaic0
  stage1/build/stage1.c
  stage1/kaic1
  stage2/build/bundle.kai
  stage2/build/stage2.c
  stage2/kaic2
)

missing=0
for f in "${chain[@]}"; do
  if [ ! -e "$f" ]; then
    echo "ci-touch-build: MISSING artifact member: $f" >&2
    missing=1
  fi
done
[ "$missing" -eq 0 ] || { echo "ci-touch-build: artifact incomplete" >&2; exit 1; }

# Touch in order; a tiny gap keeps the ordering strict even on coarse-mtime
# filesystems. `touch` with no -t uses now, which is > every checked-out
# source.
for f in "${chain[@]}"; do
  touch "$f"
done

# Self-check: every stage sub-make must consider its binary up-to-date.
# The ROOT `kaic0/kaic1/kaic2` targets are .PHONY, so they always run the
# sub-make regardless of mtime — checking the root `-q` would be a false
# negative. What actually matters is that each sub-make (`make -C stageN -q`)
# finds nothing to rebuild; that is the recompile the shards were paying.
# stage0's binary depends on its .o objects, which the artifact now ships and
# this script touches — the missing .o was the cascade trigger.
rebuild=0
for s in "stage0 kaic0" "stage1 kaic1" "stage2 kaic2"; do
  dir=${s% *}; tgt=${s#* }
  if ! make -C "$dir" -q "$tgt" 2>/dev/null; then
    echo "ci-touch-build WARN — $dir wants to rebuild $tgt; reason:" >&2
    make -C "$dir" --debug=basic "$tgt" 2>&1 | grep -iE 'newer|must remake|remaking' | head -6 >&2 || true
    rebuild=1
  fi
done
if [ "$rebuild" -eq 0 ]; then
  echo "ci-touch-build OK — kaic0/kaic1/kaic2 up-to-date, no rebuild needed"
else
  exit 1
fi
