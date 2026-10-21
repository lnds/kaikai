/* Runtime owner for segment_runtime_test: the runtime plus the two segment
 * primitives the test drives directly, which no compiled program reaches. */
#include <runtime.h>

KaiValue *test_seg_suspend(KaiValue *v) {
    return kai_seg_suspend_on(kai_current_fiber(), v, KAI_SEG_SUSPENDED);
}

void test_seg_discontinue(KaiValue *k) {
    kai_seg_discontinue_seg(kai_seg_take(k));
}
