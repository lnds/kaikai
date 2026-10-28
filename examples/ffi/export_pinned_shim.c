#include <pthread.h>
#include <stdint.h>

extern void kai_nap(void);

int64_t stayed_on_thread(void) {
    pthread_t before = pthread_self();
    for (int i = 0; i < 4; i++) {
        kai_nap();
        if (!pthread_equal(before, pthread_self())) return 0;
    }
    return 1;
}
