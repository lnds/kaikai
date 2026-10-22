#include <stdint.h>

extern void on_event(void *ud, int64_t n);

void fire_events(void *ud) {
    for (int64_t i = 1; i <= 4; i++) on_event(ud, i);
    on_event(ud, -1);
}
