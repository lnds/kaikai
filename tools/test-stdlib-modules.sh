#!/bin/sh
# tools/test-stdlib-modules.sh — validate every stdlib module compiles
# cleanly when imported.
#
# stdlib is normally pulled into user programs via core auto-load or
# `import`, both of which route the file through `expand_imports`. The
# same-module name-collision validators run inside `load_prelude` and
# `resolve_module` (per-module, before the decls join the global
# stream), so a duplicate `fn foo` (or `type T`, `effect E`, `const N`,
# `axiom A`) inside a stdlib file is caught at load time.
#
# For every `stdlib/**/*.kai` we build a one-line trampoline
# `import <mod>` `fn main() = 0` and compile it; the act of importing
# forces kaic2 to parse and validate the module. Any failure is
# reported per-module and the script exits non-zero. Wired into tier1
# so stdlib drift cannot land via PR without surfacing.
#
# Parallelism (lane tier1-perf): each module probe is an independent
# kaic2 invocation writing to its own per-module temp files (keyed by
# the dotted module name), so the probes fan out over `xargs -P$JOBS`.
# A worker mode (`__probe <tmpdir> <module-file>`) compiles one probe
# and prints `OK <f>` or a `FAIL <f>` block; the summary is recomputed
# from the collected lines. KAI_TEST_JOBS=1 restores serial behaviour.

set -eu

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
KAIC2="$ROOT/stage2/kaic2"

if [ ! -x "$KAIC2" ]; then
  echo "test-stdlib-modules FAIL — stage2/kaic2 not built" >&2
  exit 1
fi

# probe_one: compile the import trampoline for one stdlib module.
probe_one() {
  tmpdir="$1"
  f="$2"

  rel="${f#stdlib/}"
  mod="${rel%.kai}"
  mod_dotted="$(echo "$mod" | tr '/' '.')"
  key="$(echo "$mod_dotted" | tr '.' '_')"

  probe="$tmpdir/probe_$key.kai"
  err="$tmpdir/probe_$key.err"
  printf 'import %s\n\nfn main() : Int = 0\n' "$mod_dotted" > "$probe"

  if "$KAIC2" --path stdlib "$probe" > /dev/null 2> "$err"; then
    echo "OK $f"
  else
    echo "FAIL $f"
    head -4 "$err" | sed 's/^/  /'
  fi
}

# ---- worker mode -----------------------------------------------------
if [ "${1:-}" = "__probe" ]; then
  probe_one "$2" "$3"
  exit 0
fi

# ---- orchestrator ----------------------------------------------------
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT INT TERM

JOBS="${KAI_TEST_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"
self="$ROOT/tools/test-stdlib-modules.sh"
results="$tmpdir/results"

find stdlib -name '*.kai' -print0 \
  | sort -z \
  | xargs -0 -P "$JOBS" -n1 "$self" __probe "$tmpdir" \
  > "$results"

cat "$results"

total=$(find stdlib -name '*.kai' | wc -l | tr -d ' ')
pass=$(grep -c '^OK ' "$results" 2>/dev/null || true)
fail=$(grep -c '^FAIL ' "$results" 2>/dev/null || true)

echo ""
echo "test-stdlib-modules: $pass / $total passed, $fail failed"
[ "$fail" -eq 0 ]
