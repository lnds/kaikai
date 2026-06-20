# stage 1

Self-hosting compiler for kaikai, written in kaikai-minimal. Compiled by
stage 0 (`../stage0/kaic0`), emits portable C the same way stage 0 does.

## Build

```sh
cd stage1
make
```

The Makefile runs `../stage0/kaic0 compiler.kai > build/stage1.c` and then
`cc -I ../stage0 build/stage1.c -o kaic1`.

## Use

```sh
./kaic1 path/to/file.kai > out.c
cc out.c -I ../stage0 -o out
./out
```

Flags mirror stage 0: `--tokens`, `--ast`, `--test`, `-h/--help`.

## Status

Complete and load-bearing. `kaic1` is the self-hosting kaikai-minimal
compiler the whole bootstrap depends on: stage 0 compiles it, and it in
turn compiles stage 2 (`stage2/kaic2`). The pipeline — lexer, parser,
checker, emitter — is fully implemented and exercised on every build.

See `docs/stage1-design.md` for the design and `docs/kaikai-minimal.md`
for the subset the compiler is written in.
