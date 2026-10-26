# stage 1

Self-hosting compiler for kaikai-minimal, written in kaikai-minimal. Compiled by
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

Frozen. `kaic1` is a complete self-hosting compiler for kaikai-minimal
(lexer, parser, checker, emitter): stage 0 compiles it and tier 1 tests
it, but it does not compile stage 2. `stage2/kaic2` is built by the
previous release (`KAIC_BOOT=release`, the default) or, from a bare `cc`,
by the frozen C seed (`make kaic2 KAIC_BOOT=seed`); see
`docs/build-system.md` §KAIC_BOOT.

See `docs/stage1-design.md` for the design and `docs/kaikai-minimal.md`
for the subset the compiler is written in.
