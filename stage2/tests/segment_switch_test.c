/* Stack-segment switch primitive: correctness and cost.
 *
 * Ping-pongs between the OS stack and an mmap'd stack. Checks that values
 * cross in both directions, that callee-saved integer and floating-point
 * state survives the switch on both sides, and that a fresh frame is ABI
 * aligned. With `bench` as argv[1] it prints the round-trip cost.
 */

#include "runtime.h"
#include <stdio.h>
#include <time.h>

#if defined(__clang__)
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

#if defined(KAI_SEG_SWITCH)

static void *main_sp;
static void *seg_sp;
static int failed = 0;

static void check(const char *what, int cond) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failed = 1;
    }
}

/* Opaque to the optimiser, so the loop state below must live in registers
 * the switch preserves. */
__attribute__((noinline)) static double scale(double x, long i) {
    return x * 1.0000001 + (double) (i & 7);
}

static void echo_entry(void *arg) {
    uintptr_t x = (uintptr_t) arg;
    double acc = 0.5;
    long n = 0;
    /* A varargs call with a double faults on a misaligned x86_64 stack. */
    char buf[64];
    snprintf(buf, sizeof buf, "%.3f", acc);
    check("fresh frame is aligned", strcmp(buf, "0.500") == 0);
    check("fresh frame alignment", ((uintptr_t) __builtin_frame_address(0) & 15) == 0);
    for (;;) {
        acc = scale(acc, n++);
        x = (uintptr_t) kai_seg_switch(&seg_sp, main_sp, (void *) (x + 1));
        if (x == 0) break;
    }
    check("segment-side state survives", n > 0 && acc > 0.5);
    kai_seg_switch(&seg_sp, main_sp, (void *) (uintptr_t) n);
    __builtin_unreachable();
}

static void *map_stack(size_t size) {
    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, kai_stack_map_flags(), -1, 0);
    if (base == MAP_FAILED) { perror("mmap"); exit(2); }
    mprotect(base, kai_page_size(), PROT_NONE);
    return base;
}

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1e9 + (double) ts.tv_nsec;
}

static ucontext_t uc_main, uc_seg;
static void uc_entry(void) {
    for (;;) swapcontext(&uc_seg, &uc_main);
}

int main(int argc, char **argv) {
    size_t size = 64 * 1024;
    void *base = map_stack(size);
    seg_sp = kai_seg_frame_init((char *) base + size, echo_entry);

    long iters = 1000000;
    double facc = 1.0;
    long iacc = 0;
    for (long i = 1; i <= iters; i++) {
        uintptr_t r = (uintptr_t) kai_seg_switch(&main_sp, seg_sp, (void *) (uintptr_t) i);
        if (r != (uintptr_t) i + 1) { check("value crosses both ways", 0); break; }
        facc = scale(facc, i);
        iacc += (long) r;
    }
    long seen = (long) (uintptr_t) kai_seg_switch(&main_sp, seg_sp, NULL);
    check("segment ran every round", seen == iters);
    check("caller-side int state survives", iacc == iters * (iters + 1) / 2 + iters);
    double fexp = 1.0;
    for (long i = 1; i <= iters; i++) fexp = scale(fexp, i);
    check("caller-side float state survives", facc == fexp);

    if (argc > 1 && strcmp(argv[1], "bench") == 0) {
        long n = 20000000;
        seg_sp = kai_seg_frame_init((char *) base + size, echo_entry);
        double t0 = now_ns();
        for (long i = 1; i <= n; i++) kai_seg_switch(&main_sp, seg_sp, (void *) (uintptr_t) i);
        double t1 = now_ns();
        printf("segment switch round trip: %.2f ns\n", (t1 - t0) / (double) n);

        static char uc_stack[64 * 1024];
        getcontext(&uc_seg);
        uc_seg.uc_stack.ss_sp = uc_stack;
        uc_seg.uc_stack.ss_size = sizeof uc_stack;
        uc_seg.uc_link = NULL;
        makecontext(&uc_seg, uc_entry, 0);
        long m = 2000000;
        t0 = now_ns();
        for (long i = 0; i < m; i++) swapcontext(&uc_main, &uc_seg);
        t1 = now_ns();
        printf("swapcontext round trip:    %.2f ns\n", (t1 - t0) / (double) m);
    }

    if (failed) {
        fprintf(stderr, "segment_switch_test: FAIL\n");
        return 1;
    }
    printf("segment_switch_test: OK\n");
    return 0;
}

#else

int main(void) {
    printf("segment_switch_test: SKIP (no stack-segment switch on this target)\n");
    return 0;
}

#endif
