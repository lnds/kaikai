// Functional-style red-black tree port for cross-language benchmark.
// Mirrors examples/perceus/rb_tree.kai (Okasaki four-case balance,
// pure structural rebuild). Allocates one Box per node per insert;
// nodes share children via Rc to keep the functional shape without
// deep clones. Reference counts mimic kaikai's Perceus discipline.
use std::rc::Rc;
use std::time::Instant;

#[derive(Clone, Copy, PartialEq)]
enum Color { Red, Black }

enum Tree {
    Leaf,
    Node(Color, Rc<Tree>, i64, i64, Rc<Tree>),
}

use Tree::*;
use Color::*;

fn mk(c: Color, l: Rc<Tree>, k: i64, v: i64, r: Rc<Tree>) -> Rc<Tree> {
    Rc::new(Node(c, l, k, v, r))
}

fn balance(c: Color, l: Rc<Tree>, k: i64, v: i64, r: Rc<Tree>) -> Rc<Tree> {
    if let Black = c {
        if let Node(Red, ref ll, lk, lv, ref lr) = *l {
            if let Node(Red, ref lll, llk, llv, ref llr) = **ll {
                return mk(Red,
                          mk(Black, lll.clone(), llk, llv, llr.clone()),
                          lk, lv,
                          mk(Black, lr.clone(), k, v, r));
            }
            if let Node(Red, ref lrl, lrk, lrv, ref lrr) = **lr {
                return mk(Red,
                          mk(Black, ll.clone(), lk, lv, lrl.clone()),
                          lrk, lrv,
                          mk(Black, lrr.clone(), k, v, r));
            }
        }
        if let Node(Red, ref rl, rk, rv, ref rr) = *r {
            if let Node(Red, ref rll, rlk, rlv, ref rlr) = **rl {
                return mk(Red,
                          mk(Black, l, k, v, rll.clone()),
                          rlk, rlv,
                          mk(Black, rlr.clone(), rk, rv, rr.clone()));
            }
            if let Node(Red, ref rrl, rrk, rrv, ref rrr) = **rr {
                return mk(Red,
                          mk(Black, l, k, v, rl.clone()),
                          rk, rv,
                          mk(Black, rrl.clone(), rrk, rrv, rrr.clone()));
            }
        }
    }
    mk(c, l, k, v, r)
}

fn insert_loop(t: Rc<Tree>, k: i64, v: i64) -> Rc<Tree> {
    match &*t {
        Leaf => mk(Red, Rc::new(Leaf), k, v, Rc::new(Leaf)),
        Node(c, l, k0, v0, r) => {
            if k < *k0 {
                balance(*c, insert_loop(l.clone(), k, v), *k0, *v0, r.clone())
            } else if k > *k0 {
                balance(*c, l.clone(), *k0, *v0, insert_loop(r.clone(), k, v))
            } else {
                mk(*c, l.clone(), *k0, v, r.clone())
            }
        }
    }
}

fn rb_insert(t: Rc<Tree>, k: i64, v: i64) -> Rc<Tree> {
    let r = insert_loop(t, k, v);
    match &*r {
        Node(_, l, k0, v0, r) => mk(Black, l.clone(), *k0, *v0, r.clone()),
        Leaf => r.clone(),
    }
}

fn rb_size(t: &Tree) -> i64 {
    match t {
        Leaf => 0,
        Node(_, l, _, _, r) => 1 + rb_size(l) + rb_size(r),
    }
}

fn rb_height(t: &Tree) -> i64 {
    match t {
        Leaf => 0,
        Node(_, l, _, _, r) => {
            let lh = rb_height(l);
            let rh = rb_height(r);
            1 + std::cmp::max(lh, rh)
        }
    }
}

fn lcg_next(s: i64) -> i64 {
    (s.wrapping_mul(1664525).wrapping_add(1013904223)) % 2147483647
}

fn main() {
    let mut root: Rc<Tree> = Rc::new(Leaf);
    let mut seed: i64 = 1;
    let n: i64 = 1_000_000;

    let start = Instant::now();
    let mut i = n;
    while i > 0 {
        seed = lcg_next(seed);
        root = rb_insert(root, seed, i);
        i -= 1;
    }
    let elapsed = start.elapsed();

    let secs = elapsed.as_secs();
    let ms = elapsed.subsec_millis();

    println!("rust (functional RB) rb-tree benchmark");
    println!("inserts: {}", n);
    println!("size: {}", rb_size(&root));
    println!("height: {}", rb_height(&root));
    println!("elapsed: {}.{:03}s", secs, ms);
}
