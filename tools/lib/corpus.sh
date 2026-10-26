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
examples/segments
examples/actors
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
#   3. A `# fixture: test` line: tools/rc-leak-gate.sh measures it through `kai test`.
#   4. A sibling `.err.expected` / `.diag.expected` declares the fixture
#      rejected at compile time; the harness walking its directory (test-negative.sh,
#      fixture-dir.sh, ...) owns it. A `.run.err.expected` fixture builds, so it stays: its panic
#      text is pinned on C by the golden and on every other backend here.
kai_corpus_is_skipped() {
  local fixture="$1" dir base
  if [ -f "$KAI_CORPUS_SKIPS" ] && grep -q "^${fixture}:" "$KAI_CORPUS_SKIPS" 2>/dev/null; then
    return 0
  fi
  if [ -r "$fixture" ] && head -1 "$fixture" | grep -q "// skip-backend-parity"; then
    return 0
  fi
  if [ -r "$fixture" ] && grep -q '^# fixture: test' "$fixture"; then
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
  if [ -f "$base.err.expected" ] || [ -f "$base.diag.expected" ]; then
    return 0
  fi
  return 1
}

# Per-fixture facts live in a sibling env file (`main.env` beside a package
# main.kai, `<name>.env` beside a flat fixture), one KEY=value per line:
#   KAI_THREADS=1       scheduler pin; the documented property IS the one-thread rotation
#   KAI_NATIVE_OPT=2    native opt level for fixtures whose property is the optimised frame or fold
#   CFLAGS=<path>...    root-relative C shims linked through CFLAGS
# Every harness reads them here. Prints the value; non-zero when undeclared.
# Args: <fixture> <KEY>
kai_corpus_meta() {
  local fixture="$1" key="$2" envfile
  case "$fixture" in
    */main.kai) envfile="${fixture%main.kai}main.env" ;;
    *)          envfile="${fixture%.kai}.env" ;;
  esac
  [ -f "$envfile" ] || return 1
  sed -n "s/^$key=//p" "$envfile" | head -1 | grep .
}

# The CFLAGS shims of a fixture as absolute paths under $ROOT; non-zero when none.
kai_corpus_cflags() {
  local shims shim out=""
  shims="$(kai_corpus_meta "$1" CFLAGS)" || return 1
  for shim in $shims; do out="${out:+$out }${ROOT:?}/$shim"; done
  echo "$out"
}

# Rewrite ISO-8601 UTC timestamps (the stdlib Log default handler's
# `[YYYY-MM-DDTHH:MM:SSZ] LEVEL message` form) to a fixed placeholder before
# any comparison. Applied identically to both sides, so a real content
# divergence still fails; only the wall-clock second — which differs between
# two runs straddling a second boundary — is masked.
kai_corpus_normalize_timestamps() {
  sed -E 's/\[[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z\]/[TIMESTAMP]/g'
}

# Build a fixture the way `kai build` does for its shape: a package fixture
# (a manifest beside main.kai) is built as a package from its own directory,
# anything else as a standalone file. Package builds are whole-program, so an
# extern nothing calls does not reach the link. Args: <fixture> <out>
# [VAR=value ...] (the build's environment, KAI_BACKEND among them). Needs
# $KAI and $KAI_CORPUS_PKG_CACHE (a scratch dir for resolved dependencies).
kai_corpus_build() {
  local fixture="$1" out="$2" dir; shift 2
  case "$fixture" in
    */main.kai) dir="${fixture%/main.kai}" ;;
    *)          dir="" ;;
  esac
  if [ -n "$dir" ] && [ -f "$dir/kai.toml" ]; then
    ( cd "$dir" && env KAIKAI_CACHE="${KAI_CORPUS_PKG_CACHE:?}/${dir//\//_}" "$@" "$KAI" build ${KAI_CORPUS_BUILD_FLAGS:-} . -o "$out" )
  else
    env "$@" "$KAI" build ${KAI_CORPUS_BUILD_FLAGS:-} "$fixture" -o "$out"
  fi
}

# Render the manifests of the git-dependency fixtures (local bare repos).
kai_corpus_render_manifests() {
  if [ -x tests/fixtures/git-fixtures/setup.sh ]; then tests/fixtures/git-fixtures/setup.sh >/dev/null 2>&1 || true; fi
  if [ -x examples/packages/render-fixtures.sh ]; then examples/packages/render-fixtures.sh >/dev/null 2>&1 || true; fi
}
