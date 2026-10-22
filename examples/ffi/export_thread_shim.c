#include <pthread.h>
#include <stdint.h>

extern int64_t square_via_fiber(int64_t);

static void *worker(void *arg) {
    return (void *) (intptr_t) square_via_fiber((int64_t) (intptr_t) arg);
}

int64_t run_threads(void) {
    pthread_t t[4];
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, worker, (void *) (intptr_t) (i + 1));
    int64_t sum = 0;
    for (int i = 0; i < 4; i++) {
        void *r;
        pthread_join(t[i], &r);
        sum += (int64_t) (intptr_t) r;
    }
    return sum;
}
