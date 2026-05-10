/*
 * Functional-style red-black tree port for cross-language benchmark.
 *
 * Mirrors examples/perceus/rb_tree.kai (Okasaki-style four-case balance,
 * pure structural rebuild). Uses heap-allocated nodes with no manual
 * free between inserts — same memory discipline as a managed-runtime
 * port would use. 1,000,000 inserts, LCG-seeded keys identical to
 * examples/perceus/rb_tree_bench_c.c.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

typedef enum { RED, BLACK } color_t;

typedef struct node {
    color_t color;
    int64_t key;
    int64_t value;
    struct node *left;
    struct node *right;
} node_t;

static node_t *mk(color_t c, node_t *l, int64_t k, int64_t v, node_t *r) {
    node_t *n = (node_t *) malloc(sizeof(node_t));
    n->color = c; n->left = l; n->key = k; n->value = v; n->right = r;
    return n;
}

static node_t *balance(color_t c, node_t *l, int64_t k, int64_t v, node_t *r) {
    if (c == BLACK) {
        if (l && l->color == RED) {
            if (l->left && l->left->color == RED) {
                node_t *ll = l->left;
                return mk(RED,
                          mk(BLACK, ll->left, ll->key, ll->value, ll->right),
                          l->key, l->value,
                          mk(BLACK, l->right, k, v, r));
            }
            if (l->right && l->right->color == RED) {
                node_t *lr = l->right;
                return mk(RED,
                          mk(BLACK, l->left, l->key, l->value, lr->left),
                          lr->key, lr->value,
                          mk(BLACK, lr->right, k, v, r));
            }
        }
        if (r && r->color == RED) {
            if (r->left && r->left->color == RED) {
                node_t *rl = r->left;
                return mk(RED,
                          mk(BLACK, l, k, v, rl->left),
                          rl->key, rl->value,
                          mk(BLACK, rl->right, r->key, r->value, r->right));
            }
            if (r->right && r->right->color == RED) {
                node_t *rr = r->right;
                return mk(RED,
                          mk(BLACK, l, k, v, r->left),
                          r->key, r->value,
                          mk(BLACK, rr->left, rr->key, rr->value, rr->right));
            }
        }
    }
    return mk(c, l, k, v, r);
}

static node_t *insert_loop(node_t *t, int64_t k, int64_t v) {
    if (!t) return mk(RED, NULL, k, v, NULL);
    if (k < t->key) return balance(t->color, insert_loop(t->left, k, v), t->key, t->value, t->right);
    if (k > t->key) return balance(t->color, t->left, t->key, t->value, insert_loop(t->right, k, v));
    return mk(t->color, t->left, t->key, v, t->right);
}

static node_t *rb_insert(node_t *t, int64_t k, int64_t v) {
    node_t *r = insert_loop(t, k, v);
    if (!r) return NULL;
    return mk(BLACK, r->left, r->key, r->value, r->right);
}

static int64_t rb_size(node_t *t) {
    if (!t) return 0;
    return 1 + rb_size(t->left) + rb_size(t->right);
}

static int64_t rb_height(node_t *t) {
    if (!t) return 0;
    int64_t l = rb_height(t->left), r = rb_height(t->right);
    return 1 + (l > r ? l : r);
}

static int64_t lcg_next(int64_t s) {
    return (s * 1664525 + 1013904223) % 2147483647;
}

int main(void) {
    node_t *root = NULL;
    int64_t seed = 1;
    int64_t n = 1000000;

    struct timespec start, stop;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int64_t i = n; i > 0; i--) {
        seed = lcg_next(seed);
        root = rb_insert(root, seed, i);
    }
    clock_gettime(CLOCK_MONOTONIC, &stop);

    long long ns = (stop.tv_sec - start.tv_sec) * 1000000000LL
                 + (stop.tv_nsec - start.tv_nsec);
    long long secs = ns / 1000000000LL;
    long long ms   = (ns / 1000000LL) % 1000;

    printf("c (functional RB) rb-tree benchmark\n");
    printf("inserts: %lld\n", (long long) n);
    printf("size: %lld\n", (long long) rb_size(root));
    printf("height: %lld\n", (long long) rb_height(root));
    printf("elapsed: %lld.%03llds\n", secs, ms);
    return 0;
}
