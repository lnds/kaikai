/*
 * examples/perceus/rb_tree_bench_c.c — C reference for the
 * canonical Perceus red-black tree benchmark.
 *
 * Mirrors `examples/perceus/rb_tree_bench.kai`:
 *   - 1,000,000 inserts of (lcg_next(seed), iter)
 *   - same LCG (Numerical Recipes ranqd1: 1664525 / 1013904223)
 *   - same modulus (2^31 - 1) so the keystream is bit-identical
 *
 * Two implementations are compiled and timed:
 *   (a) hand-written intrusive red-black tree (closest apples-
 *       to-apples to the kaikai functional version, since both
 *       allocate a fixed-size node per insert);
 *   (b) std::set<int> via C++ (hash-randomised RB tree from libc++
 *       / libstdc++ — the typical "what does mainstream C++ do"
 *       baseline).
 *
 * Build:
 *
 *   clang   -std=c11   -O2 examples/perceus/rb_tree_bench_c.c -o /tmp/rb_c_bench
 *   clang++ -std=c++17 -O2 -DBENCH_STDSET examples/perceus/rb_tree_bench_c.c -o /tmp/rb_cxx_bench
 *
 * The wrapper script `rb_tree_bench.sh` does both.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

static int64_t lcg_next(int64_t s) {
    return (s * 1664525 + 1013904223) % 2147483647;
}

#ifndef BENCH_STDSET
/* ----- (a) hand-written non-recursive red-black tree --------------- */

typedef enum { RED, BLACK } color_t;

typedef struct rb_node {
    int64_t key;
    int64_t value;
    color_t color;
    struct rb_node *left, *right, *parent;
} rb_node;

static rb_node NIL_NODE = { 0, 0, BLACK, &NIL_NODE, &NIL_NODE, &NIL_NODE };
#define NIL (&NIL_NODE)

static rb_node *rb_new(int64_t k, int64_t v, rb_node *parent) {
    rb_node *n = (rb_node *) malloc(sizeof(rb_node));
    n->key = k; n->value = v; n->color = RED;
    n->left = NIL; n->right = NIL; n->parent = parent;
    return n;
}

static void rotate_left(rb_node **root, rb_node *x) {
    rb_node *y = x->right;
    x->right = y->left;
    if (y->left != NIL) y->left->parent = x;
    y->parent = x->parent;
    if (x->parent == NIL)            *root = y;
    else if (x == x->parent->left)   x->parent->left = y;
    else                             x->parent->right = y;
    y->left = x;
    x->parent = y;
}

static void rotate_right(rb_node **root, rb_node *x) {
    rb_node *y = x->left;
    x->left = y->right;
    if (y->right != NIL) y->right->parent = x;
    y->parent = x->parent;
    if (x->parent == NIL)            *root = y;
    else if (x == x->parent->right)  x->parent->right = y;
    else                             x->parent->left = y;
    y->right = x;
    x->parent = y;
}

static void rb_fixup(rb_node **root, rb_node *z) {
    while (z->parent->color == RED) {
        if (z->parent == z->parent->parent->left) {
            rb_node *y = z->parent->parent->right;
            if (y->color == RED) {
                z->parent->color = BLACK; y->color = BLACK;
                z->parent->parent->color = RED; z = z->parent->parent;
            } else {
                if (z == z->parent->right) { z = z->parent; rotate_left(root, z); }
                z->parent->color = BLACK; z->parent->parent->color = RED;
                rotate_right(root, z->parent->parent);
            }
        } else {
            rb_node *y = z->parent->parent->left;
            if (y->color == RED) {
                z->parent->color = BLACK; y->color = BLACK;
                z->parent->parent->color = RED; z = z->parent->parent;
            } else {
                if (z == z->parent->left) { z = z->parent; rotate_right(root, z); }
                z->parent->color = BLACK; z->parent->parent->color = RED;
                rotate_left(root, z->parent->parent);
            }
        }
    }
    (*root)->color = BLACK;
}

static void rb_insert(rb_node **root, int64_t k, int64_t v) {
    rb_node *y = NIL;
    rb_node *x = *root;
    while (x != NIL) {
        y = x;
        if      (k < x->key) x = x->left;
        else if (k > x->key) x = x->right;
        else { x->value = v; return; }
    }
    rb_node *z = rb_new(k, v, y);
    if      (y == NIL)    *root = z;
    else if (k < y->key)  y->left = z;
    else                  y->right = z;
    rb_fixup(root, z);
}

static int64_t rb_size(rb_node *t) {
    if (t == NIL) return 0;
    return 1 + rb_size(t->left) + rb_size(t->right);
}

static int64_t rb_height(rb_node *t) {
    if (t == NIL) return 0;
    int64_t l = rb_height(t->left), r = rb_height(t->right);
    return 1 + (l > r ? l : r);
}

int main(void) {
    rb_node *root = NIL;
    int64_t seed = 1;
    int64_t n = 1000000;

    struct timespec start, stop;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int64_t i = n; i > 0; i--) {
        seed = lcg_next(seed);
        rb_insert(&root, seed, i);
    }
    clock_gettime(CLOCK_MONOTONIC, &stop);

    long long elapsed_ns = (stop.tv_sec - start.tv_sec) * 1000000000LL
                         + (stop.tv_nsec - start.tv_nsec);
    long long secs = elapsed_ns / 1000000000LL;
    long long ms   = (elapsed_ns / 1000000LL) % 1000;

    printf("c (hand-written RB) rb-tree benchmark\n");
    printf("inserts: %lld\n", (long long) n);
    printf("size: %lld\n", (long long) rb_size(root));
    printf("height: %lld\n", (long long) rb_height(root));
    printf("elapsed: %lld.%03llds\n", secs, ms);
    return 0;
}

#else
/* ----- (b) std::set<int> --------------------------------------------- */

#include <set>
#include <chrono>

int main(void) {
    std::set<int64_t> s;
    int64_t seed = 1;
    int64_t n = 1000000;

    auto start = std::chrono::steady_clock::now();
    for (int64_t i = n; i > 0; i--) {
        seed = lcg_next(seed);
        s.insert(seed);
    }
    auto stop = std::chrono::steady_clock::now();

    long long elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
    long long secs = elapsed_ns / 1000000000LL;
    long long ms   = (elapsed_ns / 1000000LL) % 1000;

    /* std::set has no separate "value" — the workload becomes set<key>
     * insert, which is the standard mainstream-C++ comparison. */
    printf("c++ std::set rb-tree benchmark\n");
    printf("inserts: %lld\n", (long long) n);
    printf("size: %lld\n", (long long) s.size());
    printf("elapsed: %lld.%03llds\n", secs, ms);
    return 0;
}

#endif
