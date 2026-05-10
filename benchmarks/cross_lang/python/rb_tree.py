"""Functional-style red-black tree port for cross-language benchmark.

Mirrors examples/perceus/rb_tree.kai. Tuples represent immutable nodes:
``(color, left, key, value, right)`` or ``None`` for the leaf. The
algorithm rebuilds the spine on each insert exactly like the kaikai
source — Python's reference counting plays the role of Perceus.
"""

import sys
import time

RED = 0
BLACK = 1
sys.setrecursionlimit(200000)


def mk(c, l, k, v, r):
    return (c, l, k, v, r)


def balance(c, l, k, v, r):
    if c == BLACK:
        if l is not None and l[0] == RED:
            ll = l[1]
            if ll is not None and ll[0] == RED:
                return (RED,
                        (BLACK, ll[1], ll[2], ll[3], ll[4]),
                        l[2], l[3],
                        (BLACK, l[4], k, v, r))
            lr = l[4]
            if lr is not None and lr[0] == RED:
                return (RED,
                        (BLACK, l[1], l[2], l[3], lr[1]),
                        lr[2], lr[3],
                        (BLACK, lr[4], k, v, r))
        if r is not None and r[0] == RED:
            rl = r[1]
            if rl is not None and rl[0] == RED:
                return (RED,
                        (BLACK, l, k, v, rl[1]),
                        rl[2], rl[3],
                        (BLACK, rl[4], r[2], r[3], r[4]))
            rr = r[4]
            if rr is not None and rr[0] == RED:
                return (RED,
                        (BLACK, l, k, v, r[1]),
                        r[2], r[3],
                        (BLACK, rr[1], rr[2], rr[3], rr[4]))
    return (c, l, k, v, r)


def insert_loop(t, k, v):
    if t is None:
        return (RED, None, k, v, None)
    c, l, k0, v0, r = t
    if k < k0:
        return balance(c, insert_loop(l, k, v), k0, v0, r)
    if k > k0:
        return balance(c, l, k0, v0, insert_loop(r, k, v))
    return (c, l, k0, v, r)


def rb_insert(t, k, v):
    r = insert_loop(t, k, v)
    if r is None:
        return None
    return (BLACK, r[1], r[2], r[3], r[4])


def rb_size(t):
    if t is None:
        return 0
    return 1 + rb_size(t[1]) + rb_size(t[4])


def rb_height(t):
    if t is None:
        return 0
    return 1 + max(rb_height(t[1]), rb_height(t[4]))


def lcg_next(s):
    return (s * 1664525 + 1013904223) % 2147483647


def main():
    root = None
    seed = 1
    n = 1000000

    start = time.monotonic_ns()
    i = n
    while i > 0:
        seed = lcg_next(seed)
        root = rb_insert(root, seed, i)
        i -= 1
    elapsed = time.monotonic_ns() - start

    secs = elapsed // 1_000_000_000
    ms = (elapsed // 1_000_000) % 1000

    print("python (functional RB) rb-tree benchmark")
    print(f"inserts: {n}")
    print(f"size: {rb_size(root)}")
    print(f"height: {rb_height(root)}")
    print(f"elapsed: {secs}.{ms:03d}s")


main()
