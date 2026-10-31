#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int sp_in_thread_stack(void) {
    char probe;
    uintptr_t sp = (uintptr_t) &probe;
    uintptr_t lo, hi;
#if defined(__APPLE__)
    pthread_t t = pthread_self();
    hi = (uintptr_t) pthread_get_stackaddr_np(t);
    lo = hi - pthread_get_stacksize_np(t);
#else
    pthread_attr_t attr;
    void *addr;
    size_t size;
    pthread_getattr_np(pthread_self(), &attr);
    pthread_attr_getstack(&attr, &addr, &size);
    pthread_attr_destroy(&attr);
    lo = (uintptr_t) addr;
    hi = lo + size;
#endif
    return sp >= lo && sp < hi;
}

int64_t stack_check(int64_t tag) {
    if (!sp_in_thread_stack()) {
        fprintf(stderr, "stack pointer outside the thread stack\n");
        abort();
    }
    return tag;
}
