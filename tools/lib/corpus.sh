#!/usr/bin/env bash
# Shared fixture-corpus walk.
#
# Two gates walk the same entry-point corpus under the same skip rules —
# backend parity (native vs the C oracle) and M:N corpus determinism (N=1
# vs N>1). One walk in one place is what stops them from drifting into
# covering different corpora while both report "the corpus".
#
# Entry-point detection: kaikai fixtures live in two shapes —
#   - flat:    <dir>/<name>.kai is a standalone program with main.
#   - package: <dir>/<pkg>/main.kai is the entry point; sibling files are
#              libraries loaded by main. Library files are never compiled
#              standalone — they would fail at link time (no kai_main) and
#              that is not a finding for either gate.
#
# examples/negative is intentionally absent: those fixtures must reject at
# compile time, so a harness that assumes a build succeeds does not apply.
# `quarantine/` subdirs hold fixtures that DOCUMENT broken behaviour (no
# stable golden) and are pruned explicitly.
#
# The caller must have cd'd to the repo root; paths are root-relative.

# Overridable via $KAI_CORPUS_DIRS (newline- or space-separated) so CI can
# shard across runners. Each shard walks a disjoint subset; the union must
# equal this default or the gate's coverage silently shrinks.
KAI_CORPUS_DEFAULT_DIRS="examples/effects
examples/actors
examples/spawn
examples/perceus
examples/refinements
examples/llvm
examples/packages
examples/minimal
examples/quickstart
examples/stdlib
examples/attributes
examples/unstable
demos"

KAI_CORPUS_SKIPS="${KAI_CORPUS_SKIPS:-tools/backend-parity-skips.txt}"

# One entry-point path per line, sorted and de-duplicated.
kai_corpus_entry_points() {
  local dir
  for dir in ${KAI_CORPUS_DIRS:-$KAI_CORPUS_DEFAULT_DIRS}; do
    [ -d "$dir" ] || continue
    find "$dir" -maxdepth 1 -name "*.kai" -not -name "*.err.kai" 2>/dev/null
    find "$dir" -mindepth 2 -name "main.kai" -not -path '*/quarantine/*' 2>/dev/null
  done | sort -u
}

# Skip discipline:
#   1. tools/backend-parity-skips.txt — <relative-path>:<issue>:<reason>.
#      File the issue first; the skip is the bookmark, the issue the work.
#   2. First line contains "// skip-backend-parity" — for fixtures that are
#      intentionally backend-specific.
#   3. A sibling `.err.expected` / `.diag.expected` / `.run.err.expected`
#      declares the fixture negative-by-design; tools/test-negative.sh owns
#      it against its golden.
kai_corpus_is_skipped() {
  local fixture="$1" dir base
  if [ -f "$KAI_CORPUS_SKIPS" ] && grep -q "^${fixture}:" "$KAI_CORPUS_SKIPS" 2>/dev/null; then
    return 0
  fi
  if [ -r "$fixture" ] && head -1 "$fixture" | grep -q "// skip-backend-parity"; then
    return 0
  fi
  case "$fixture" in
    */main.kai)
      dir="${fixture%/main.kai}"
      if [ -f "$dir/main.err.expected" ]; then
        return 0
      fi
      ;;
  esac
  base="${fixture%.kai}"
  if [ -f "$base.err.expected" ] || [ -f "$base.diag.expected" ] || [ -f "$base.run.err.expected" ]; then
    return 0
  fi
  return 1
}

# Rewrite ISO-8601 UTC timestamps (the stdlib Log default handler's
# `[YYYY-MM-DDTHH:MM:SSZ] LEVEL message` form) to a fixed placeholder before
# any comparison. Applied identically to both sides, so a real content
# divergence still fails; only the wall-clock second — which differs between
# two runs straddling a second boundary — is masked.
kai_corpus_normalize_timestamps() {
  sed -E 's/\[[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z\]/[TIMESTAMP]/g'
}
