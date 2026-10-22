/* The lazy cell's states, driven from C: a shared cell runs its thunk
 * once and keeps the step, an unshared one is freed before its thunk
 * runs, an exit past a running cell gives it its thunk back, and a cell
 * read by its own thunk panics. A constant of a lazy type is built once
 * per fiber, and an exit past its initialiser leaves it unbuilt.
 *
 * `seq_cell_test self` runs the self-reading cell alone: it ends the
 * process.
 */

#include "runtime.h"
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static int failed = 0;

static void check(const char *what, int cond) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        failed = 1;
    }
}

static int     runs;
static int     bail;
static jmp_buf out;
static int64_t frees_at_run;

static KaiValue *step(KaiValue *self, KaiValue **args, int n) {
    (void) self; (void) args; (void) n;
    runs++;
    frees_at_run = kai_rc_hot_st.free_total;
    if (bail) longjmp(out, 1);
    return kai_int(7);
}

static KaiValue *reads_itself(KaiValue *self, KaiValue **args, int n) {
    (void) args; (void) n;
    return kai_seq_force(kai_incref(self->as.clo.captures[0]));
}

static KaiValue *cell(void) {
    return kai_seq_defer(kai_closure(step, 0, 0, NULL));
}

static void shared_runs_once(void) {
    runs = 0;
    KaiValue *s = kai_incref(cell());
    KaiValue *a = kai_seq_force(s);
    check("a shared cell holds its step", s->variant_tag == KAI_SEQ_DONE);
    KaiValue *b = kai_seq_force(s);
    check("two reads run the thunk once", runs == 1);
    check("both reads yield the step", kai_intf(a) == 7 && kai_intf(b) == 7);
}

static void unshared_is_freed_first(void) {
    runs = 0;
    kai_rc_traffic_on = 1;
    KaiValue *s = cell();
    int64_t before = kai_rc_hot_st.free_total;
    KaiValue *a = kai_seq_force(s);
    check("an unshared cell is freed before its thunk runs", frees_at_run == before + 1);
    check("an unshared read yields the step", runs == 1 && kai_intf(a) == 7);
}

static void exit_restores_the_thunk(void) {
    runs = 0;
    bail = 1;
    KaiValue *s = kai_incref(cell());
    KaiFiber *f = kai_current_fiber();
    uint32_t mark = f->unw_top;
    if (setjmp(out) == 0) {
        kai_seq_force(s);
        check("the thunk left through the jump", 0);
    } else {
        kai_unw_release_to(f, mark);
    }
    check("a cell left while running holds its thunk again", s->variant_tag == KAI_SEQ_THUNK);
    check("the evaluating frame's reference is released", kai_rc_load(s) == 1);
    bail = 0;
    KaiValue *a = kai_seq_force(s);
    check("the next read runs the thunk again", runs == 2 && kai_intf(a) == 7);
}

static KaiValue *build(KaiValue *self, KaiValue **args, int n) {
    (void) self; (void) args; (void) n;
    runs++;
    if (bail) longjmp(out, 1);
    return cell();
}

static KaiValue *constant(void) {
    return kai_seq_once(kai_closure(build, 0, 0, NULL));
}

static void constant_is_built_once(void) {
    runs = 0;
    KaiValue *a = constant();
    KaiValue *b = constant();
    check("two reads of a constant build it once", runs == 1 && a == b);
    kai_decref(a);
    kai_decref(b);
    kai_fiber_consts_drop(kai_current_fiber());
}

static void exit_leaves_the_constant_unbuilt(void) {
    runs = 0;
    bail = 1;
    KaiFiber *f = kai_current_fiber();
    uint32_t mark = f->unw_top;
    if (setjmp(out) == 0) {
        constant();
        check("the initialiser left through the jump", 0);
    } else {
        kai_unw_release_to(f, mark);
    }
    check("a constant left while it is built is unbuilt again", f->const_n == 0);
    bail = 0;
    KaiValue *a = constant();
    check("the next read builds it", runs == 2 && f->const_n == 1);
    kai_decref(a);
    kai_fiber_consts_drop(f);
    check("a fiber's constants go when it ends", f->const_n == 0 && f->consts == NULL);
}

static void depends_on_itself(void) {
    KaiValue *s = kai_seq_defer(kai_unit());
    s->as.seq.slot = kai_closure(reads_itself, 0, 1, &s);
    kai_seq_force(s);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "self") == 0) {
        depends_on_itself();
        return 0;
    }
    shared_runs_once();
    unshared_is_freed_first();
    exit_restores_the_thunk();
    constant_is_built_once();
    exit_leaves_the_constant_unbuilt();
    return failed;
}
