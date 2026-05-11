#!/usr/bin/env bash
# Phase A.0 bench-first probe (issue #452). Measures how fast a pure-
# kaikai BinSerialize round-trip is on an AST-sized payload, to decide
# whether a post-parse [Decl] cache can actually beat the lex+parse it
# would replace.
#
# Background: docs/cache-design.md "Phase A.0" promises ~230 ms wall
# savings by deserialising a cached [Decl] instead of lex+parsing the
# stdlib. That assumes from_bytes is *faster* than lex+parse. This
# harness validates the assumption by timing to_bytes / from_bytes on
# a synthetic but realistically shaped AST proxy:
#
#   #derive(BinSerialize)
#   type Node = { tag: Int, line: Int, col: Int,
#                 name: String, children: [Int] }
#
# Two payload sizes:
#   - 100 nodes (~8 KB)   — proxy for one small prelude (option.kai).
#   - 500 nodes (~40 KB)  — proxy for one medium prelude (list.kai).
#
# Decision criterion: if from_bytes wall on the 100-node payload
# exceeds ~10 ms, the cache is unviable (lex+parse of option.kai is
# ~3.4 ms per docs/cache-design.md; replacing it with anything slower
# is a net loss).
#
# Usage: tools/bench-binserialize-roundtrip.sh
# Reqs:  bin/kai working (tier0 builds stage 0/1/2).

set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

cat > "$TMP/bench100.kai" <<'EOF'
#derive(BinSerialize)
type Node = { tag: Int, line: Int, col: Int, name: String, children: [Int] }

fn build_children(k: Int, acc: [Int]) : [Int] =
  if k <= 0 { acc } else { build_children(k - 1, [k, ...acc]) }

fn build(n: Int, acc: [Node]) : [Node] =
  if n <= 0 { acc }
  else {
    let nd = Node { tag: n % 16, line: n, col: n * 2,
                      name: string_concat("ident_", int_to_string(n)),
                      children: build_children(5, []) }
    build(n - 1, [nd, ...acc])
  }

fn main() : Int {
  let argv = args()
  let mode = match argv {
    []                -> "both"
    [a, ..._rest]     -> a
  }
  let nodes = build(100, [])
  if mode == "ser" {
    let bs = bin_list_to_bytes(nodes, { x -> to_bytes(x) })
    print(string_concat("bytes: ", int_to_string(list_length(bs))))
    0
  } else if mode == "de" {
    let bs = bin_list_to_bytes(nodes, { x -> to_bytes(x) })
    match bin_list_from_bytes(bs, 0, { buf, pos -> node_from_bytes(buf, pos) }) {
      Err(m) -> { print(m); 1 }
      Ok(c)  -> { print(string_concat("got: ", int_to_string(list_length(c.value)))); 0 }
    }
  } else {
    let bs = bin_list_to_bytes(nodes, { x -> to_bytes(x) })
    print(string_concat("bytes: ", int_to_string(list_length(bs))))
    match bin_list_from_bytes(bs, 0, { buf, pos -> node_from_bytes(buf, pos) }) {
      Err(m) -> { print(m); 1 }
      Ok(c)  -> { print(string_concat("got: ", int_to_string(list_length(c.value)))); 0 }
    }
  }
}
EOF

cat > "$TMP/bench500.kai" <<'EOF'
#derive(BinSerialize)
type Node = { tag: Int, line: Int, col: Int, name: String, children: [Int] }

fn build_children(k: Int, acc: [Int]) : [Int] =
  if k <= 0 { acc } else { build_children(k - 1, [k, ...acc]) }

fn build(n: Int, acc: [Node]) : [Node] =
  if n <= 0 { acc }
  else {
    let nd = Node { tag: n % 16, line: n, col: n * 2,
                      name: string_concat("ident_", int_to_string(n)),
                      children: build_children(5, []) }
    build(n - 1, [nd, ...acc])
  }

fn main() : Int {
  let nodes = build(500, [])
  print(string_concat("nodes: ", int_to_string(list_length(nodes))))
  let bs = bin_list_to_bytes(nodes, { x -> to_bytes(x) })
  print(string_concat("bytes: ", int_to_string(list_length(bs))))
  match bin_list_from_bytes(bs, 0, { buf, pos -> node_from_bytes(buf, pos) }) {
    Err(m) -> { print(string_concat("err: ", m)); 1 }
    Ok(c)  -> { print(string_concat("roundtrip: ", int_to_string(list_length(c.value)))); 0 }
  }
}
EOF

"$ROOT/bin/kai" build "$TMP/bench100.kai" -o "$TMP/bench100" >/dev/null
"$ROOT/bin/kai" build "$TMP/bench500.kai" -o "$TMP/bench500" >/dev/null

median_real() {
  for _ in 1 2 3 4 5; do
    /usr/bin/time -p sh -c "$1" 2>&1 | awk '/^real/{print $2}'
  done | sort -n | sed -n '3p'
}

echo "phase                                 median(s)"
echo "------------------------------------- --------"
printf "%-37s %s\n" "100 nodes / 8 KB, to_bytes only"      "$(median_real "$TMP/bench100 ser > /dev/null")"
printf "%-37s %s\n" "100 nodes / 8 KB, to+from_bytes"      "$(median_real "$TMP/bench100 de  > /dev/null")"
printf "%-37s %s\n" "500 nodes / 40 KB, to+from_bytes"     "$(median_real "$TMP/bench500     > /dev/null")"
echo
echo "Decision criterion: from_bytes wall on 100 nodes must be"
echo "below ~10 ms to beat the 3.4 ms lex+parse it replaces."
echo "See docs/lane-experience-issue-452-phase-a0.md for analysis."
