#!/bin/sh
# tools/test-info-blocks.sh — compile every fenced kaikai block in
# docs/info/*.md to guarantee the documented surface matches the
# compiler.
#
# Fence tags:
#   ```kaikai          full program; must compile clean.
#   ```kaikai-snippet  expression fragment; wrapped in a tiny harness
#                      before compiling.
#   ```kaikai-neg      must FAIL to compile (used in NOT IN KAIKAI
#                      sections). The script inverts the assert.
#   ```text  ```sh  ```toml ```diff  etc.   ignored.
#
# Wired into `make test-info` alongside the page-presence smoke.
#
# Parallelism (lane tier1-perf): block extraction is cheap and stays
# serial; the per-block `kai build` invocations are independent (each
# writes only to $WORK/<topic>_<num>.*) so they fan out over
# `xargs -P$JOBS`. A worker mode (`__compile <topic> <body-path>`)
# compiles one block and prints `OK`/`FAIL ...` to stdout; the summary
# is recomputed from the collected lines, so the pass/fail contract is
# identical to the old serial loop. KAI_TEST_JOBS=1 restores serial.

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
INFO_DIR="$ROOT/docs/info"
KAI="$ROOT/bin/kai"

# compile_one: compile a single extracted block; print one result line.
# `OK` for a pass, or a multi-line `FAIL ...` block. Self-contained so
# it is safe to run concurrently — every path it writes is keyed by
# <topic>_<num>.
compile_one() {
  topic="$1"
  block_file="$2"   # path to block_NNN.body
  work="$3"

  tag_path="${block_file%.body}.tag"
  tag="$(cat "$tag_path")"
  num="$(basename "$block_file" .body | sed 's/^block_//')"

  src="$work/${topic}_${num}.kai"
  out="$work/${topic}_${num}"
  log="$work/${topic}_${num}.log"

  case "$tag" in
    kaikai|kaikai-neg)
      cp "$block_file" "$src" ;;
    kaikai-snippet)
      {
        printf 'fn __snippet_main() : Int = {\n'
        printf '  let _ = '
        cat "$block_file"
        printf '\n  0\n}\n'
        printf 'fn main() : Int = __snippet_main()\n'
      } > "$src" ;;
    *)
      echo "internal: unknown tag '$tag'" >&2
      exit 2 ;;
  esac

  if "$KAI" build "$src" -o "$out" >"$log" 2>&1; then
    case "$tag" in
      kaikai-neg)
        echo "FAIL [$topic block $num, tag=$tag]: expected compile failure, but it succeeded"
        echo "---- source:"
        cat "$src"
        echo "----" ;;
      *)
        echo "OK" ;;
    esac
  else
    case "$tag" in
      kaikai-neg)
        echo "OK" ;;
      *)
        echo "FAIL [$topic block $num, tag=$tag]:"
        echo "---- source:"
        cat "$src"
        echo "---- compiler output:"
        cat "$log"
        echo "----" ;;
    esac
  fi
}

# Extract every fenced kaikai* block from a markdown file into numbered
# files inside $WORK/$topic/. block_N.tag holds the tag, block_N.body
# the raw body.
extract_blocks() {
  page="$1"
  topic="$2"
  work="$3"
  out_dir="$work/$topic"
  mkdir -p "$out_dir"
  awk -v out_dir="$out_dir" '
    BEGIN { in_code = 0; tag = ""; n = 0; out_body = ""; out_tag = "" }
    /^```/ {
      if (in_code) {
        printf "%s", body > out_body
        close(out_body)
        printf "%s\n", tag > out_tag
        close(out_tag)
        in_code = 0
        body = ""
        next
      }
      fence = substr($0, 4)
      sub(/^[[:space:]]+/, "", fence)
      sub(/[[:space:]]+$/, "", fence)
      if (fence == "kaikai" || fence == "kaikai-snippet" || fence == "kaikai-neg") {
        n++
        tag = fence
        out_body = sprintf("%s/block_%03d.body", out_dir, n)
        out_tag = sprintf("%s/block_%03d.tag", out_dir, n)
        in_code = 1
        body = ""
      }
      next
    }
    in_code { body = body $0 "\n" }
  ' "$page"
}

# ---- worker mode -----------------------------------------------------
# `$0 __compile <work> <topic> <body-path>` compiles exactly one block.
if [ "${1:-}" = "__compile" ]; then
  compile_one "$3" "$4" "$2"
  exit 0
fi

# ---- orchestrator ----------------------------------------------------
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

JOBS="${KAI_TEST_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"
self="$SCRIPT_DIR/test-info-blocks.sh"

# Phase 1 (serial, cheap): extract all blocks, build a worklist of
# "<topic>\t<body-path>" lines.
worklist="$WORK/worklist"
: > "$worklist"
for page in "$INFO_DIR"/*.md; do
  [ -f "$page" ] || continue
  topic="$(basename "$page" .md)"
  extract_blocks "$page" "$topic" "$WORK"
  for block in "$WORK/$topic"/*.body; do
    [ -f "$block" ] || continue
    printf '%s\t%s\n' "$topic" "$block" >> "$worklist"
  done
done

total=$(wc -l < "$worklist" | tr -d ' ')
results="$WORK/results"

# Phase 2 (parallel): compile each block. Worker prints OK or FAIL...;
# we keep every line and count FAILs afterwards.
if [ "$total" -gt 0 ]; then
  # Tab-delimited worklist → xargs. Each line carries topic + body path.
  while IFS="$(printf '\t')" read -r topic block; do
    printf '%s\0%s\0' "$topic" "$block"
  done < "$worklist" \
    | xargs -0 -P "$JOBS" -n2 "$self" __compile "$WORK" \
    > "$results"
else
  : > "$results"
fi

ok=$(grep -c '^OK$' "$results" 2>/dev/null || true)
fails=$(grep -c '^FAIL ' "$results" 2>/dev/null || true)

if [ "$fails" -gt 0 ]; then
  echo "test-info-blocks: $fails failure(s) out of $total block(s)" >&2
  echo "---" >&2
  grep -v '^OK$' "$results" >&2
  exit 1
fi

echo "test-info-blocks: OK — $ok/$total compile blocks pass"
