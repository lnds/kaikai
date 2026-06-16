#!/bin/sh
# tools/test-packages.sh — package-mode CI harness (issue #569).
#
# Exercises kaikai as a package consumer (manifest-driven builds,
# multi-package layouts, transitive imports, cross-package effects,
# stdlib visible across deps, auto-install). The compiler's
# selfhost path never touches this code path; without this harness
# the package-manager surface stays a CI blind spot, surfacing
# regressions only in downstream consumers (ahu, henua, …).
#
# Three fixture shapes recognised under examples/packages/:
#
#   (a) Positive: <fix>/{kai.toml,main.kai,main.out.expected}
#       or <fix>/<sub>/{main.kai,main.out.expected} (any depth).
#       Build the package, run, diff stdout against the golden.
#
#   (b) Negative: <fix>/<sub>/main.err.expected (single-line
#       substring). Build must FAIL with that substring in stderr.
#
#   (c) Driver-level: <fix>/check.sh. Hands off entirely to that
#       script — it owns its own assertions.
#
# Fixtures that require rendered manifests (git-source deps) are
# rendered via examples/packages/render-fixtures.sh once at the
# start. Bare repos under tests/fixtures/git-fixtures/ must exist
# (setup.sh regenerates them).
#
# BACKEND PARITY (design decision, 2026-05-27): this harness owns
# package-mode backend parity. tools/test-backend-parity.sh is
# file-mode only and cannot resolve a package's manifest deps, so the
# `run_parity` pass below builds + runs each positive fixture under the
# C and native backends and compares stdout. The two harnesses split on
# the package/file axis: file-mode parity there, package-mode parity
# here. The parity pass SKIPs when native is unavailable (C-only kaic2).

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAI="$ROOT/bin/kai"
PKG_DIR="$ROOT/examples/packages"
PASS=0
FAIL=0
SKIP=0
FAILED_FIXTURES=""

err() { printf '  FAIL  %s\n' "$1"; FAIL=$((FAIL + 1)); FAILED_FIXTURES="$FAILED_FIXTURES $1"; }
ok()  { printf '  ok    %s\n' "$1"; PASS=$((PASS + 1)); }
skip(){ printf '  skip  %s (%s)\n' "$1" "$2"; SKIP=$((SKIP + 1)); }

# Ensure bare repos + rendered manifests exist for git-source
# fixtures. If the bare-repo setup script is missing or fails, we
# skip the git-source fixtures rather than fail the whole run.
if [ -x "$ROOT/tests/fixtures/git-fixtures/setup.sh" ]; then
  "$ROOT/tests/fixtures/git-fixtures/setup.sh" >/dev/null 2>&1 || true
fi
if [ -x "$PKG_DIR/render-fixtures.sh" ]; then
  "$PKG_DIR/render-fixtures.sh" >/dev/null 2>&1 || true
fi

# Positive: run the package and diff stdout against the golden.
# Args: <fixture-name> <run-dir-rel-to-PKG_DIR> <golden-file-rel>
run_positive() {
  name="$1"; dir="$2"; golden="$3"
  abs_dir="$PKG_DIR/$dir"
  abs_golden="$PKG_DIR/$golden"
  [ -d "$abs_dir" ]    || { skip "$name" "no dir $dir"; return; }
  [ -f "$abs_golden" ] || { skip "$name" "no golden $golden"; return; }
  out="$(cd "$abs_dir" && "$KAI" run . 2>&1)" || {
    err "$name"
    echo "    --- output ---" >&2
    echo "$out" | sed 's/^/    /' >&2
    return
  }
  # Drop the driver's "kai: building package …" preamble before
  # diffing — that line is informational, not part of the program
  # output. tail -n 1 keeps the last line, which matches the
  # single-line goldens our positive fixtures use.
  actual="$(echo "$out" | grep -v '^kai:' || true)"
  expected="$(cat "$abs_golden")"
  if [ "$actual" = "$expected" ]; then
    ok "$name"
  else
    err "$name"
    echo "    --- expected ---" >&2
    echo "$expected" | sed 's/^/    /' >&2
    echo "    --- actual ---" >&2
    echo "$actual" | sed 's/^/    /' >&2
  fi
}

# Negative: build must fail with the substring from .err.expected
# appearing in stderr.
# Args: <fixture-name> <build-dir-rel-to-PKG_DIR> <err-file-rel>
run_negative() {
  name="$1"; dir="$2"; err_file="$3"
  abs_dir="$PKG_DIR/$dir"
  abs_err="$PKG_DIR/$err_file"
  [ -d "$abs_dir" ] || { skip "$name" "no dir $dir"; return; }
  [ -f "$abs_err" ] || { skip "$name" "no err file $err_file"; return; }
  substr="$(head -n 1 "$abs_err")"
  out="$(cd "$abs_dir" && "$KAI" build . 2>&1)" || true
  if echo "$out" | grep -qF "$substr"; then
    ok "$name"
  else
    err "$name"
    echo "    --- expected substring ---" >&2
    echo "    $substr" >&2
    echo "    --- actual output ---" >&2
    echo "$out" | sed 's/^/    /' >&2
  fi
}

# Parity: build + run the package with the C and native backends and
# assert their stdout matches. This is the ONLY place package fixtures
# get native-vs-C parity coverage — the file-mode parity harness
# (tools/test-backend-parity.sh) cannot resolve a package's manifest
# deps, so package parity lives here, where the git-fixture setup +
# package-mode build already exist (decided 2026-05-27). `kai run .`
# honors KAI_BACKEND, verified on local_path + auto_install.
#
# The native backend needs a kaic2 built with libLLVM (KAI_LLVM=1). When
# it is absent (the C-only bootstrap that `tier1` uses), each fixture
# SKIPs its parity check — never a failure — so the native column runs
# only where libLLVM is present (tier1-native). $NATIVE_OK is probed once
# below, before the parity block.
# Args: <fixture-name> <run-dir-rel-to-PKG_DIR>
run_parity() {
  name="$1"; dir="$2"
  abs_dir="$PKG_DIR/$dir"
  [ -d "$abs_dir" ] || { skip "$name" "no dir $dir"; return; }
  [ "$NATIVE_OK" = "1" ] || { skip "$name" "native backend unavailable (build kaic2 with KAI_LLVM=1)"; return; }
  # Compare program STDOUT only. stderr carries the package-manager
  # preamble ("kai-pkg: resolving …", lockfile writes) and git-fixture
  # setup noise (awk warnings), which differs run-to-run (cache warm vs
  # cold) and is not program output. Drop stderr; filter any `kai:` /
  # `kai-pkg:` driver lines that reach stdout.
  c_out="$(cd "$abs_dir" && KAI_BACKEND=c      "$KAI" run . 2>/dev/null | grep -vE '^kai(-pkg)?:' || true)"
  n_out="$(cd "$abs_dir" && KAI_BACKEND=native "$KAI" run . 2>/dev/null | grep -vE '^kai(-pkg)?:' || true)"
  if [ "$c_out" = "$n_out" ]; then
    ok "$name"
  else
    err "$name"
    echo "    --- C backend ---" >&2
    echo "$c_out" | sed 's/^/    /' >&2
    echo "    --- native backend ---" >&2
    echo "$n_out" | sed 's/^/    /' >&2
  fi
}

# Driver-level: hand off to a fixture-owned check.sh.
run_check_script() {
  name="$1"; rel="$2"
  abs="$PKG_DIR/$rel"
  [ -x "$abs" ] || { skip "$name" "no check.sh"; return; }
  if "$abs" >/dev/null 2>&1; then
    ok "$name"
  else
    err "$name"
    "$abs" 2>&1 | sed 's/^/    /' >&2 || true
  fi
}

printf '== package-mode harness (issue #569) ==\n'

# Issue's coverage matrix — each entry maps to a fixture under
# examples/packages/. Keep this list in sync with that issue.

# #1 — self-import (regression guard for #567)
run_positive  "1-self_import"         "self_import/examples/demo"  "self_import/examples/demo/main.out.expected"

# #2 — transitive privacy (regression guard for #565)
run_positive  "2-transitive_privacy"  "transitive_privacy/consumer" "transitive_privacy/consumer/main.out.expected"

# #3 — pub leak (negative companion to #2)
run_negative  "3-pub_leak"            "pub_leak/consumer"           "pub_leak/consumer/main.err.expected"

# #4 — local-path dep
run_positive  "4-local_path"          "local_path/app"              "local_path/app/main.out.expected"

# #5 — sibling examples + tests
run_positive  "5-sibling_pkg_entry"   "sibling_examples_tests"      "sibling_examples_tests/main.out.expected"
run_positive  "5-sibling_examples"    "sibling_examples_tests/examples/demo" "sibling_examples_tests/examples/demo/main.out.expected"

# #6 — cross-package effects
run_positive  "6-cross_pkg_effects"   "cross_package_effects/consumer" "cross_package_effects/consumer/main.out.expected"

# #7 — same-name shadowing
run_positive  "7-same_name_shadow"    "same_name_shadowing"         "same_name_shadowing/main.out.expected"

# #8 — stdlib visible across deps
run_positive  "8-stdlib_across_deps"  "stdlib_across_deps/consumer" "stdlib_across_deps/consumer/main.out.expected"

# #9 — auto-install on first compile (regression guard for #512)
run_check_script "9-auto_install"     "auto_install/check.sh"

# Pre-existing driver-level checks — keep them green to confirm
# the broader package surface still works.
run_check_script "lockfile_repro"     "lockfile_reproducibility/check.sh"
run_check_script "add_failure"        "add_failure/check.sh"
run_check_script "init_invalid_names" "init_invalid_names/check.sh"
run_check_script "manifest_parse"     "manifest_parse_error/check.sh"

# --- native-vs-C parity for package builds ---
# Each positive fixture above is also built + run under the C and native
# backends and their stdout compared. This is the package-mode counterpart
# to tools/test-backend-parity.sh (which is file-mode only and cannot
# resolve manifest deps). Covers the git-dep path (auto_install,
# local_path) end to end under native, which the file-mode harness misses.
#
# Probe native availability ONCE: a C-only kaic2 (the bootstrap `tier1`
# uses) rejects --backend=native with a known sentinel, so the parity
# fixtures SKIP rather than fail. The real native-vs-C run happens in
# tier1-native, where kaic2 is built with KAI_LLVM=1.
NATIVE_OK=0
_probe="$(mktemp)"; mv "$_probe" "$_probe.kai"; _probe="$_probe.kai"
printf 'fn main() : Int = 0\n' > "$_probe"
if "$KAI" build --backend=native "$_probe" -o "${_probe%.kai}.bin" >/dev/null 2>&1; then
  NATIVE_OK=1
fi
rm -f "$_probe" "${_probe%.kai}.bin"
if [ "$NATIVE_OK" = "1" ]; then
  printf '%s\n' "-- package parity (native vs C) --"
else
  printf '%s\n' "-- package parity (native vs C) — SKIPPED, native backend unavailable --"
fi
run_parity  "parity-self_import"        "self_import/examples/demo"
run_parity  "parity-transitive_privacy" "transitive_privacy/consumer"
run_parity  "parity-local_path"         "local_path/app"
run_parity  "parity-sibling_pkg_entry"  "sibling_examples_tests"
run_parity  "parity-sibling_examples"   "sibling_examples_tests/examples/demo"
run_parity  "parity-cross_pkg_effects"  "cross_package_effects/consumer"
run_parity  "parity-same_name_shadow"   "same_name_shadowing"
run_parity  "parity-stdlib_across_deps" "stdlib_across_deps/consumer"
run_parity  "parity-auto_install"       "auto_install"

printf '== summary: %d ok, %d fail, %d skip ==\n' "$PASS" "$FAIL" "$SKIP"
if [ "$FAIL" -gt 0 ]; then
  printf 'failed:%s\n' "$FAILED_FIXTURES" >&2
  exit 1
fi
exit 0
