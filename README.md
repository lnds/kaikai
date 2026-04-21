# kaikai

A **functional**, **statically typed** programming language that compiles to **native code** via LLVM.

- **Algebraic effects** as a first-class primitive (Effekt-style + inference).
- **Elixir-style pipelines**.
- **Memory** without a global GC or borrow checker: Perceus (compile-time optimized RC) + isolated fibers (BEAM-style).
- **Portable bootstrap**: 3-stage compiler (C → kaikai-minimal → full kaikai).

## Status

Full redesign in progress. See [`docs/design.md`](docs/design.md) for the current plan.

## Bootstrap (once MVP is ready)

```sh
cc stage0/*.c -o kaic0
./kaic0 stage1/compiler.kai > stage1.c && cc stage1.c -o kaic1
./kaic1 demos/hello.kai -o hello
./hello
```

## License

To be defined.
