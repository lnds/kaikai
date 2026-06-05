// Koka column for the kaikai rb-tree benchmark.
// Algorithm taken VERBATIM from koka/test/bench/koka/rbtree-ck.kk
// (the Lean4/Koka benchmark that examples/perceus/rb_tree.kai was
// itself adapted from), with two changes only, exactly as
// docs/benchmarks/rb_tree_2026-06-02.md specifies:
//   - value : bool  ->  value : int
//   - make-tree / main  ->  fill-loop(LCG) driver matching
//     examples/perceus/rb_tree_bench.kai and rb_tree_bench_c.c
//     (Numerical Recipes ranqd1 LCG, modulus 2^31-1, 1,000,000 inserts).

import std/num/int64

type color
  Red
  Black

type tree
  Leaf()
  Node(color: color, lchild: tree, key: int, value: int, rchild: tree)


fun fold(t: tree, b: a, f: (int, int, a) -> a): a
  match t
    Leaf                -> b
    Node(_, l, k, v, r) -> r.fold( f(k, v, l.fold(b, f)), f)


fun balance1(kv: int, vv: int, t: tree, s: tree): tree
  match s
    Leaf -> Leaf
    Node(_, Node(Red, l, kx, vx, r1), ky, vy, r2)
      -> Node(Red, Node(Black, l, kx, vx, r1), ky, vy, Node(Black, r2, kv, vv, t))
    Node(_, l1, ky, vy, Node(Red, l2, kx, vx, r))
      -> Node(Red, Node(Black, l1, ky, vy, l2), kx, vx, Node(Black, r, kv, vv, t))
    Node(_, l, ky, vy, r)
      -> Node(Black, Node(Red, l, ky, vy, r), kv, vv, t)


fun balance2(t: tree, kv: int, vv: int, s: tree): tree
  match s
    Leaf -> Leaf
    Node(_, Node(Red, l, kx1, vx1, r1), ky, vy, r2)
      -> Node(Red, Node(Black, t, kv, vv, l), kx1, vx1, Node(Black, r1, ky, vy, r2))
    Node(_, l1, ky, vy, Node(Red, l2, kx2, vx2, r2))
      -> Node(Red, Node(Black, t, kv, vv, l1), ky, vy, Node(Black, l2, kx2, vx2, r2))
    Node(_, l, ky, vy, r)
      -> Node(Black, t, kv, vv, Node(Red, l, ky, vy, r))


fun red-root(t : tree) : bool
  match t
    Node(Red) -> True
    _         -> False


fun ins(t: tree, kx: int, vx: int): tree
  match t
    Leaf
      -> Node(Red, Leaf, kx, vx, Leaf)
    Node(Red, a, ky, vy, b)
      -> if kx < ky then Node(Red, ins(a, kx, vx), ky, vy, b)
         elif kx == ky then Node(Red, a, kx, vx, b)
         else Node(Red, a, ky, vy, ins(b, kx, vx))
    Node(Black, a, ky, vy, b)
      -> if kx < ky then (if red-root(a) then balance1(ky, vy, b, ins(a, kx, vx))
                                       else Node(Black, ins(a, kx, vx), ky, vy, b))
         elif kx == ky then Node(Black, a, kx, vx, b)
         elif red-root(b) then balance2(a, ky, vy, ins(b, kx, vx))
         else Node(Black, a, ky, vy, ins(b, kx, vx))


fun set-black(t: tree) : tree
  match t
    Node(_, l, k, v, r) -> Node(Black, l, k, v, r)
    _ -> t


fun insert(t: tree, k: int, v: int): tree
  if red-root(t)
    then ins(t, k, v).set-black
    else ins(t, k, v)


// LCG keystream identical to rb_tree_bench.kai / rb_tree_bench_c.c:
//   s' = (s * 1664525 + 1013904223) mod (2^31 - 1)
fun lcg-next(s : int) : int
  (s * 1664525 + 1013904223) % 2147483647


fun fill-loop(t : tree, seed : int, n : int) : div tree
  if n <= 0 then t else
    val s = lcg-next(seed)
    fill-loop(insert(t, s, n), s, n.dec)


fun size(t : tree) : int
  match t
    Leaf -> 0
    Node(_, l, _, _, r) -> 1 + size(l) + size(r)


pub fun main()
  val n = 1000000
  val t = fill-loop(Leaf, 1, n)
  println("size: " ++ t.size.show)
