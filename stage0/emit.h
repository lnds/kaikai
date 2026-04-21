#ifndef KAI_EMIT_H
#define KAI_EMIT_H

#include <stdio.h>

#include "ast.h"

/*
 * Emit portable C for a checked kaikai-minimal AST.
 *
 * The output is a single translation unit that expects to be compiled
 * together with `runtime.h` (the kaikai runtime) sitting on the include
 * path:
 *     ./kaic0 hello.kai > hello.c
 *     cc hello.c -I stage0 -o hello
 *     ./hello
 *
 * Returns 0 on success. Prints diagnostics to stderr for AST shapes
 * that are not supported yet.
 */
int kai_emit(Node *program, FILE *out);

#endif /* KAI_EMIT_H */
