// Functional-style red-black tree port for cross-language benchmark.
// Mirrors examples/perceus/rb_tree.kai. Uses pointer-shared sub-trees
// (Go's GC stands in for kaikai's Perceus reuse-in-place at the
// memory-discipline level — both produce a directed graph of shared
// nodes with no manual frees).
package main

import (
	"fmt"
	"time"
)

type Color int

const (
	Red Color = iota
	Black
)

type Tree struct {
	color Color
	left  *Tree
	key   int64
	value int64
	right *Tree
}

func mk(c Color, l *Tree, k int64, v int64, r *Tree) *Tree {
	return &Tree{color: c, left: l, key: k, value: v, right: r}
}

func balance(c Color, l *Tree, k int64, v int64, r *Tree) *Tree {
	if c == Black {
		if l != nil && l.color == Red {
			if l.left != nil && l.left.color == Red {
				ll := l.left
				return mk(Red,
					mk(Black, ll.left, ll.key, ll.value, ll.right),
					l.key, l.value,
					mk(Black, l.right, k, v, r))
			}
			if l.right != nil && l.right.color == Red {
				lr := l.right
				return mk(Red,
					mk(Black, l.left, l.key, l.value, lr.left),
					lr.key, lr.value,
					mk(Black, lr.right, k, v, r))
			}
		}
		if r != nil && r.color == Red {
			if r.left != nil && r.left.color == Red {
				rl := r.left
				return mk(Red,
					mk(Black, l, k, v, rl.left),
					rl.key, rl.value,
					mk(Black, rl.right, r.key, r.value, r.right))
			}
			if r.right != nil && r.right.color == Red {
				rr := r.right
				return mk(Red,
					mk(Black, l, k, v, r.left),
					r.key, r.value,
					mk(Black, rr.left, rr.key, rr.value, rr.right))
			}
		}
	}
	return mk(c, l, k, v, r)
}

func insertLoop(t *Tree, k int64, v int64) *Tree {
	if t == nil {
		return mk(Red, nil, k, v, nil)
	}
	if k < t.key {
		return balance(t.color, insertLoop(t.left, k, v), t.key, t.value, t.right)
	}
	if k > t.key {
		return balance(t.color, t.left, t.key, t.value, insertLoop(t.right, k, v))
	}
	return mk(t.color, t.left, t.key, v, t.right)
}

func rbInsert(t *Tree, k int64, v int64) *Tree {
	r := insertLoop(t, k, v)
	if r == nil {
		return nil
	}
	return mk(Black, r.left, r.key, r.value, r.right)
}

func rbSize(t *Tree) int64 {
	if t == nil {
		return 0
	}
	return 1 + rbSize(t.left) + rbSize(t.right)
}

func rbHeight(t *Tree) int64 {
	if t == nil {
		return 0
	}
	l := rbHeight(t.left)
	r := rbHeight(t.right)
	if l > r {
		return 1 + l
	}
	return 1 + r
}

func lcgNext(s int64) int64 {
	return (s*1664525 + 1013904223) % 2147483647
}

func main() {
	var root *Tree
	var seed int64 = 1
	var n int64 = 1000000

	start := time.Now()
	for i := n; i > 0; i-- {
		seed = lcgNext(seed)
		root = rbInsert(root, seed, i)
	}
	elapsed := time.Since(start)

	secs := elapsed.Milliseconds() / 1000
	ms := elapsed.Milliseconds() % 1000

	fmt.Println("go (functional RB) rb-tree benchmark")
	fmt.Printf("inserts: %d\n", n)
	fmt.Printf("size: %d\n", rbSize(root))
	fmt.Printf("height: %d\n", rbHeight(root))
	fmt.Printf("elapsed: %d.%03ds\n", secs, ms)
}
