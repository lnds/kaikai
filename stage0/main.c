/*
 * stage 0: kaikai-minimal bootstrap compiler.
 *
 * Flags:
 *   --tokens   print the token stream and exit
 *   --ast      parse and print the AST and exit
 *   -h, --help show usage
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "check.h"
#include "emit.h"
#include "lexer.h"
#include "parser.h"

static char *read_file(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long n = ftell(fp);
    if (n < 0) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    char *buf = (char *) malloc((size_t) n + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t) n, fp);
    fclose(fp);
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [--tokens|--ast] <file.kai>\n"
            "  default     lex, parse, check, and emit C to stdout\n"
            "  --tokens    print the token stream and exit\n"
            "  --ast       parse and print the AST and exit\n"
            "  -h, --help  this help\n",
            prog);
}

int main(int argc, char **argv) {
    const char *path = NULL;
    int dump_tokens = 0;
    int dump_ast    = 0;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "--tokens") == 0) { dump_tokens = 1; continue; }
        if (strcmp(a, "--ast")    == 0) { dump_ast    = 1; continue; }
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { usage(argv[0]); return 0; }
        if (a[0] == '-') { fprintf(stderr, "unknown flag: %s\n", a); usage(argv[0]); return 2; }
        if (path)       { fprintf(stderr, "only one input file supported\n"); return 2; }
        path = a;
    }

    if (!path) { usage(argv[0]); return 2; }

    size_t len = 0;
    char *src = read_file(path, &len);
    if (!src) return 1;

    size_t n = 0;
    Token *toks = kai_lex(path, src, len, &n);
    if (!toks) { free(src); return 1; }

    int had_errors = 0;
    for (size_t i = 0; i < n; ++i) {
        if (toks[i].kind == TK_ERROR) {
            had_errors = 1;
            fprintf(stderr, "%s:%d:%d: error: lex error in %.*s\n",
                    path, toks[i].line, toks[i].col,
                    (int) toks[i].length, src + toks[i].start);
        }
    }

    if (dump_tokens) {
        kai_lex_dump(path, src, toks, n);
        free(toks); free(src);
        return had_errors ? 1 : 0;
    }

    if (had_errors) { free(toks); free(src); return 1; }

    Node *prog = kai_parse(path, src, toks, n);
    free(toks);
    if (!prog) { free(src); return 1; }

    if (dump_ast) {
        kai_dump_ast(prog);
        kai_free_node(prog);
        free(src);
        return 0;
    }

    int rc = kai_check(prog, path, src);
    if (rc != 0) {
        kai_free_node(prog);
        free(src);
        return rc;
    }

    /* Emit C to stdout. */
    rc = kai_emit(prog, stdout);

    kai_free_node(prog);
    free(src);
    return rc;
}
