#!/usr/bin/env bash
# bench-a1-spike.sh — A.1 cache "hit_simulado" upper-bound bench.
#
# Purpose: measure the wall save A.1 could deliver BEFORE committing
# ~1500 LOC of typed-Decl serdes + driver wiring. KAB2 (issue #592)
# projected 0.41 s and delivered 0.03 s after decoder overhead;
# this spike measures A.1's theoretical ceiling instead of trusting
# the design doc's projection.
#
# Mechanic (no compiler change):
#   - Cold wall      : full pipeline, KAI_PRELUDE_CACHE=0, `kai build`.
#   - Wall to parse  : `--ast` mode — short-circuits before cascade.
#   - Wall to typer  : `--check` mode — short-circuits after typer.
#   - delta_cas_typr : wall_check - wall_ast.
#     This is the upper-bound A.1 could remove from cold (because
#     A.1's saving is the cascade + typer over the prelude, and
#     empty.kai's user-side work is negligible against ~50 prelude
#     files).
#   - hit_simulado_wall = cold_wall - delta_cas_typr
#     This is the wall the build would reach if the decoder were
#     literally free.
#   - kab2_decoder_cost = wall_kab2_warm_OFF - wall_kab2_warm_ON
#     (proxy for A.1 decoder cost: A.1 payload is strictly larger
#      than KAB2's post-parse [Decl], so A.1 decoder >= KAB2
#      decoder. Use this as the lower bound on decoder overhead.)
#   - A.1 net realistic = delta_cas_typr - decoder_cost_est
#
# Usage: tools/bench-a1-spike.sh
#
# Reqs : stage2/kaic2 built (`make -C stage2 kaic2`).

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KAIC2="$ROOT/stage2/kaic2"
STDLIB_ROOT="$ROOT/stdlib"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

SRC="$TMP/empty.kai"
printf 'fn main() : Unit { () }\n' > "$SRC"

[ -x "$KAIC2" ] || { echo "kaic2 not built; run: make -C stage2 kaic2" >&2; exit 1; }

# Mirror bin/kai's prelude flag construction so the bench measures
# the same program shape `kai build` produces.
STDLIB_CORE_DIR="$STDLIB_ROOT/core"
prelude_flags=""
for core_file in "$STDLIB_CORE_DIR"/*.kai; do
  [ -f "$core_file" ] && prelude_flags="$prelude_flags --prelude $core_file"
done
for f in protocols.kai effects.kai array.kai random.kai random_secure.kai \
         encoding/base64.kai encoding/hex.kai \
         collections/map.kai collections/set.kai collections/queue.kai collections/stack.kai \
         math/numeric.kai math/int.kai math/real.kai math/complex.kai \
         encoding/json.kai encoding/toml.kai \
         decimal.kai money.kai fx.kai \
         uuid.kai regexp.kai path.kai \
         crypto/hash.kai crypto/mac.kai; do
  full="$STDLIB_ROOT/$f"
  [ -f "$full" ] && prelude_flags="$prelude_flags --prelude $full"
done

# Sampler: n=5 median wall in seconds.
median_real() {
  for _ in 1 2 3 4 5; do
    /usr/bin/time -p sh -c "$1" 2>&1 | awk '/^real/{print $2}'
  done | sort -n | sed -n '3p'
}

# All measurements bypass the on-disk KAB2 cache so the kaic2 wall is
# what we are measuring, not shell-driver shasum + cache I/O.
# (`bin/kai` adds those; here we call kaic2 directly.)

echo "===================================================="
echo "  A.1 cache spike — hit_simulado upper-bound bench"
echo "===================================================="
echo
echo "Source : empty.kai (single fn main)"
echo "Preludes: bin/kai's stdlib_prelude_flags set"
echo "Sampler : /usr/bin/time -p, n=5 median per row"
echo

# Row 1 — full cold compile (mode default = emit C).
cold_cmd="$KAIC2 --path $STDLIB_ROOT $prelude_flags $SRC > $TMP/out.c 2>/dev/null"
cold_wall=$(median_real "$cold_cmd")

# Row 2 — wall to parse (--ast short-circuits before cascade).
ast_cmd="$KAIC2 --ast --path $STDLIB_ROOT $prelude_flags $SRC > /dev/null 2>&1"
ast_wall=$(median_real "$ast_cmd")

# Row 3 — wall to cascade+typer (--check exits after typer).
check_cmd="$KAIC2 --check --path $STDLIB_ROOT $prelude_flags $SRC > /dev/null 2>&1"
check_wall=$(median_real "$check_cmd")

# Compute deltas using bc to avoid shell float issues.
delta_cas_typr=$(echo "$check_wall - $ast_wall" | bc -l)
hit_simulado=$(echo "$cold_wall - $delta_cas_typr" | bc -l)

printf "%-30s %s\n" "cold compile (n=5 median):" "${cold_wall}s"
printf "%-30s %s\n" "wall to --ast:" "${ast_wall}s"
printf "%-30s %s\n" "wall to --check:" "${check_wall}s"
printf "%-30s %s\n" "delta cascade+typer:" "${delta_cas_typr}s"
echo
printf "%-30s %s\n" "hit_simulado (decoder=0):" "${hit_simulado}s"
printf "%-30s %s\n" "UPPER-BOUND save:" "${delta_cas_typr}s"
echo
echo "----------------------------------------------------"
echo "  decoder overhead estimate (lower bound for A.1)"
echo "----------------------------------------------------"
echo
echo "Approximation: KAB2 post-parse [Decl] decoder cost as a"
echo "floor on A.1's typed [Decl] + ModuleEnvDelta decoder cost."
echo "A.1's payload is strictly larger, so its decoder >= KAB2's."
echo

# Use `bin/kai` machinery to get a fair KAB2 read. We do not exercise
# the cache via bin/kai (it adds shasum + cache I/O overhead).
# Instead, we estimate KAB2 decoder cost as the wall of a kaic2 run
# that consumes a pre-emitted .kab cache for one large prelude.
#
# To stay under the "no compiler change" rule of the spike, we
# approximate decoder overhead by re-using the published KAB2 retro
# number: ~0.19 s at the kaic2 level on M2 Pro for the 32-prelude
# set (docs/lane-experience-issue-592-kab2-binary.md §"What KAB2
# actually delivers").
#
# A.1 caches the TYPED [Decl] plus a ModuleEnvDelta per module.
# Empirical sizing (post-#578 typer-fold): typed nodes carry Type
# annotations on every Expr, raising the per-DFn footprint ~3-4×
# over KAB2's untyped payload. Decoder cost scales roughly with
# bytes-decoded, so an A.1 decoder is ~3× KAB2's. Estimate floor =
# 1.5× KAB2; estimate ceiling = 4× KAB2.

kab2_decoder=0.19   # M2 Pro post-KAB2 measurement, isolated kaic2 wall
floor=$(echo "$kab2_decoder * 1.5" | bc -l)
ceil=$(echo "$kab2_decoder * 4.0" | bc -l)

printf "%-30s %s\n" "KAB2 decoder (published):" "${kab2_decoder}s"
printf "%-30s %s\n" "A.1 decoder estimate floor:" "${floor}s (1.5x KAB2)"
printf "%-30s %s\n" "A.1 decoder estimate ceil:" "${ceil}s (4.0x KAB2)"
echo

net_save_optimistic=$(echo "$delta_cas_typr - $floor" | bc -l)
net_save_pessimistic=$(echo "$delta_cas_typr - $ceil" | bc -l)

printf "%-30s %s\n" "A.1 net save (optimistic):" "${net_save_optimistic}s"
printf "%-30s %s\n" "A.1 net save (pessimistic):" "${net_save_pessimistic}s"
echo
echo "===================================================="
echo "  verdict gates per brief"
echo "===================================================="
echo
echo "  >= 0.40 s save  -> A.1 lane fully worth committing"
echo "  >= 0.20 s save  -> A.1 lane is defendable"
echo "  <  0.20 s save  -> A.1 not worth 1500+ LOC; replan"
echo
