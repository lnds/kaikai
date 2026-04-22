/*
 * LLVM-backend shim over the header-only kaikai-minimal runtime.
 *
 * `runtime.h` defines the whole runtime as `static inline` so the
 * stage 0 / stage 1 / stage 2 C backends can emit one big translation
 * unit and not worry about duplicate-symbol linkage. That trick does
 * not work for the LLVM backend: `llc` produces an object file that
 * needs externally linkable runtime symbols, and `static` functions
 * live inside a different TU with no visible name.
 *
 * This file is that visible name. It includes the header once, then
 * re-exports each runtime entry point the LLVM backend calls into as
 * a thin `kaix_*` wrapper. It also provides `main`, wired to the
 * `kai_main` symbol the LLVM output defines.
 *
 * Link line:
 *   clang -I stage0 foo.ll stage0/runtime_llvm.c -o foo
 */

#include "runtime.h"

/* String construction and printing. Enough to round-trip hello.kai. */
KaiValue *kaix_str(const char *s)              { return kai_str(s); }
KaiValue *kaix_prelude_print(KaiValue *v)      { return kai_prelude_print(v); }

/* Entry point: the LLVM output defines kai_main. Match what the C
   backend's emit_main_wrapper does. */
extern KaiValue *kai_main(void);

int main(int argc, char **argv) {
    kai_set_args(argc, argv);
    KaiValue *result = kai_main();
    kai_decref(result);
    return 0;
}
