// Functional-style red-black tree port for cross-language benchmark.
// Mirrors examples/perceus/rb_tree.kai. Objects play the role of immutable
// nodes; the runtime GC handles memory just like Perceus does at compile time.

const RED = 0;
const BLACK = 1;

type Node = {
  color: number;
  left: Node | null;
  key: number;
  value: number;
  right: Node | null;
};

function mk(c: number, l: Node | null, k: number, v: number, r: Node | null): Node {
  return { color: c, left: l, key: k, value: v, right: r };
}

function balance(c: number, l: Node | null, k: number, v: number, r: Node | null): Node {
  if (c === BLACK) {
    if (l !== null && l.color === RED) {
      const ll = l.left;
      if (ll !== null && ll.color === RED) {
        return mk(RED,
                  mk(BLACK, ll.left, ll.key, ll.value, ll.right),
                  l.key, l.value,
                  mk(BLACK, l.right, k, v, r));
      }
      const lr = l.right;
      if (lr !== null && lr.color === RED) {
        return mk(RED,
                  mk(BLACK, l.left, l.key, l.value, lr.left),
                  lr.key, lr.value,
                  mk(BLACK, lr.right, k, v, r));
      }
    }
    if (r !== null && r.color === RED) {
      const rl = r.left;
      if (rl !== null && rl.color === RED) {
        return mk(RED,
                  mk(BLACK, l, k, v, rl.left),
                  rl.key, rl.value,
                  mk(BLACK, rl.right, r.key, r.value, r.right));
      }
      const rr = r.right;
      if (rr !== null && rr.color === RED) {
        return mk(RED,
                  mk(BLACK, l, k, v, r.left),
                  r.key, r.value,
                  mk(BLACK, rr.left, rr.key, rr.value, rr.right));
      }
    }
  }
  return mk(c, l, k, v, r);
}

function insertLoop(t: Node | null, k: number, v: number): Node {
  if (t === null) return mk(RED, null, k, v, null);
  if (k < t.key) return balance(t.color, insertLoop(t.left, k, v), t.key, t.value, t.right);
  if (k > t.key) return balance(t.color, t.left, t.key, t.value, insertLoop(t.right, k, v));
  return mk(t.color, t.left, t.key, v, t.right);
}

function rbInsert(t: Node | null, k: number, v: number): Node {
  const r = insertLoop(t, k, v);
  return mk(BLACK, r.left, r.key, r.value, r.right);
}

function rbSize(t: Node | null): number {
  if (t === null) return 0;
  return 1 + rbSize(t.left) + rbSize(t.right);
}

function rbHeight(t: Node | null): number {
  if (t === null) return 0;
  const l = rbHeight(t.left);
  const r = rbHeight(t.right);
  return 1 + (l > r ? l : r);
}

function lcgNext(s: number): number {
  // (s * 1664525 + 1013904223) mod (2^31 - 1). Safe in number (<= 2^53).
  return (s * 1664525 + 1013904223) % 2147483647;
}

function now(): number {
  // Hi-res clock — process.hrtime.bigint on Node, performance.now on Deno.
  // Use performance.now (ms) where available; fall back to Date.now.
  if (typeof performance !== "undefined" && performance.now) {
    return performance.now();
  }
  return Date.now();
}

function main() {
  let root: Node | null = null;
  let seed = 1;
  const n = 1000000;

  const startMs = now();
  let i = n;
  while (i > 0) {
    seed = lcgNext(seed);
    root = rbInsert(root, seed, i);
    i -= 1;
  }
  const elapsedMs = now() - startMs;

  const secs = Math.floor(elapsedMs / 1000);
  const ms = Math.floor(elapsedMs) % 1000;

  const label = typeof Deno !== "undefined"
    ? "deno (functional RB)"
    : "node (functional RB)";
  console.log(`${label} rb-tree benchmark`);
  console.log(`inserts: ${n}`);
  console.log(`size: ${rbSize(root)}`);
  console.log(`height: ${rbHeight(root)}`);
  console.log(`elapsed: ${secs}.${String(ms).padStart(3, "0")}s`);
}

// @ts-ignore — Deno global is optional.
declare const Deno: unknown;
main();
