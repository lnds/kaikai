#include <execinfo.h>
#include <stdint.h>

static int64_t frames(void) {
    void *pcs[256];
    return backtrace(pcs, 256);
}

int64_t kai_probe_frames(void) { return frames(); }

int64_t kai_probe_frames_on_thread(void) { return frames(); }
