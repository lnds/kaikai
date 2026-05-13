#!/bin/sh
# publish.sh — Reconstruct kaikailang-org/kaikai as a filtered public mirror
# of lnds/kaikai. Strips internal-only docs and replaces CLAUDE.md with the
# public-facing version. Run from a clean working tree on `main`.
#
# What this script does:
#   1. Refuse to run if the working tree is dirty or HEAD is not on main.
#   2. Clone the current repo to a scratch directory.
#   3. Use `git filter-repo` to remove internal files from ALL history:
#        - docs/lane-experience-*.md
#        - docs/lane-audit-*.md
#        - docs/*audit*.md
#        - docs/*honesty*.md
#        - docs/*followup*.md
#        - docs/nocturnal-*.md
#        - tools/coverage-baseline.txt
#        - tools/symbolize-rc-trace.sh
#        - tools/bench-phases.sh
#   4. Replace CLAUDE.md with the public-facing version.
#   5. Add CONTRIBUTING.md (new file, public-facing only).
#   6. Sed-clean docs/decisions/repl-removal-2026-05-09.md to remove
#      references to an internal lane report.
#   7. Force-push to kaikailang-org/kaikai (main + tags).
#
# Prerequisites:
#   - `git filter-repo` installed (brew install git-filter-repo).
#   - SSH access to kaikailang-org/kaikai.
#   - Public-facing CLAUDE.md.public and CONTRIBUTING.md.public available
#     alongside this script (in tools/publish/).

set -e

# ---------------------------------------------------------------------------
# 0. Preconditions
# ---------------------------------------------------------------------------

REPO_ROOT=$(git rev-parse --show-toplevel 2>/dev/null) || {
  echo "ERROR: not in a git repo"
  exit 1
}
cd "$REPO_ROOT"

if [ -n "$(git status --porcelain)" ]; then
  echo "ERROR: working tree is dirty. Commit or stash first."
  git status --short
  exit 1
fi

CURRENT_BRANCH=$(git rev-parse --abbrev-ref HEAD)
if [ "$CURRENT_BRANCH" != "main" ]; then
  echo "ERROR: HEAD is on '$CURRENT_BRANCH', not main. Switch to main first."
  exit 1
fi

# Verify filter-repo is installed
if ! command -v git-filter-repo >/dev/null 2>&1; then
  echo "ERROR: git-filter-repo not installed. Run: brew install git-filter-repo"
  exit 1
fi

# Verify public-facing files exist
PUBLISH_DIR="$REPO_ROOT/tools/publish"
if [ ! -f "$PUBLISH_DIR/CLAUDE.md.public" ]; then
  echo "ERROR: missing $PUBLISH_DIR/CLAUDE.md.public"
  exit 1
fi
if [ ! -f "$PUBLISH_DIR/CONTRIBUTING.md.public" ]; then
  echo "ERROR: missing $PUBLISH_DIR/CONTRIBUTING.md.public"
  exit 1
fi

# ---------------------------------------------------------------------------
# 1. Clone to scratch
# ---------------------------------------------------------------------------

WORK=$(mktemp -d /tmp/kaikai-publish-XXXXXX)
echo "Scratch dir: $WORK"
trap 'rm -rf "$WORK"' EXIT

echo "Cloning current repo to scratch (no-local)..."
git clone --no-local "$REPO_ROOT" "$WORK/kaikai"
cd "$WORK/kaikai"

# ---------------------------------------------------------------------------
# 2. Filter internal files from ALL history
# ---------------------------------------------------------------------------

echo "Filtering internal-only paths from history..."
git filter-repo \
  --path-glob 'docs/lane-experience-*.md' \
  --path-glob 'docs/lane-audit-*.md' \
  --path-glob 'docs/*audit*.md' \
  --path-glob 'docs/*honesty*.md' \
  --path-glob 'docs/*followup*.md' \
  --path-glob 'docs/nocturnal-*.md' \
  --path tools/coverage-baseline.txt \
  --path tools/symbolize-rc-trace.sh \
  --path tools/bench-phases.sh \
  --invert-paths \
  --force

# ---------------------------------------------------------------------------
# 3. Replace CLAUDE.md, add CONTRIBUTING.md
# ---------------------------------------------------------------------------

echo "Replacing CLAUDE.md with public version..."
cp "$PUBLISH_DIR/CLAUDE.md.public" CLAUDE.md

echo "Adding CONTRIBUTING.md..."
cp "$PUBLISH_DIR/CONTRIBUTING.md.public" CONTRIBUTING.md

# ---------------------------------------------------------------------------
# 4. Clean internal references in docs/decisions/
# ---------------------------------------------------------------------------

echo "Cleaning internal references in docs/decisions/..."
if [ -f docs/decisions/repl-removal-2026-05-09.md ]; then
  sed -i.bak \
    -e 's|See `docs/lane-experience-kai-repl-watch.md` for the|See an internal lane report for the|' \
    -e 's|`docs/lane-experience-kai-repl-watch.md` is left intact as the|the internal lane report is preserved as the|' \
    docs/decisions/repl-removal-2026-05-09.md
  rm -f docs/decisions/repl-removal-2026-05-09.md.bak
fi

# ---------------------------------------------------------------------------
# 5. Commit the publication-only edits
# ---------------------------------------------------------------------------

git add -A
if [ -n "$(git status --porcelain)" ]; then
  git commit -m "docs: prepare public mirror (CLAUDE.md trimmed, CONTRIBUTING.md added)" --no-verify
fi

# ---------------------------------------------------------------------------
# 6. Push to kaikailang-org/kaikai
# ---------------------------------------------------------------------------

echo "Adding public remote..."
git remote add public git@github.com:kaikailang-org/kaikai.git 2>/dev/null || \
  git remote set-url public git@github.com:kaikailang-org/kaikai.git

echo "About to force-push to kaikailang-org/kaikai. This rewrites public history."
echo "Press Ctrl+C to abort, ENTER to proceed."
read -r _

echo "Pushing main..."
git push public main:main --force

echo "Pushing tags..."
git push public --tags --force

echo ""
echo "DONE. Public mirror updated at https://github.com/kaikailang-org/kaikai"
echo "Scratch dir cleaned up: $WORK"
