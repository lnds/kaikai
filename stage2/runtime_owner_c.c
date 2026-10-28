/* Runtime owner object for the C backend.
 *
 * The default C backend emits a self-contained `.c` that #includes runtime.h
 * and calls the runtime's scheduler ops by name. The program references them
 * external (KAI_SCHED_FN under separate compilation) and this owner provides
 * them, together with the scheduler's state and its one thread-local reader,
 * kai_worker_here.
 *
 * This is deliberately NOT stage0/runtime_llvm.c (the NATIVE owner): that file
 * carries native-only baggage the C path does not want — the kaix_* ABI, an
 * `int main`, and the emit_native_slot layout pins (`_Static_assert(offsetof(
 * KaiValue, as) == 8)`) that are false when KAI_TRACE_RC grows the struct. The
 * C program references only the scheduler surface, all of which runtime.h
 * already defines under KAI_RUNTIME_OWNER. So the C owner is just runtime.h
 * instantiated as the owner TU, nothing more. KAI_PROGRAM_PROVIDES_MAIN elides
 * runtime.h's expectation of a generated main — the emitted `.c` carries it. */
#include <runtime.h>
