# Demos

Programs written in **target kaikai syntax** as probes for compiler progress.
Some compile today, some don't. The Makefile reports both gracefully.

```sh
make -C demos verify    # compile + run every demo, print PASS/FAIL table
make -C demos <topic>   # try a single demo
make -C demos list
make -C demos clean
```

Assumes the compiler is built at `../bin/kai` (run `make` at the project
root first).

See [STATUS.md](STATUS.md) for the rationale and how to read the output.

## Layout

```
demos/
  Makefile
  README.md
  STATUS.md
  <topic>/
    main.kai                 # entry point (required)
    main.out.expected        # golden stdout (optional)
    *.kai                    # additional modules (optional)
    fixtures/                # input data (optional)
```

The pre-redesign Go-frontend sketches that used to live as flat `*.kai`
files at this level have all been either migrated into per-demo
subdirectories or deleted (when their syntax / concepts had no
recovery path in the redesigned language). The full migration log is in
[STATUS.md](STATUS.md); the originals are preserved in git history.

## `use Effect` convention

Core demos do **not** open the namespace of built-in IO effects. The
sugar prelude already exposes `print` / `eprint` / `read_line` /
`read_file` / `write_file` as bare names that auto-inject the
matching atomic effect (`Stdout` / `Stderr` / `Stdin` / `File`) into
the caller's row. Forcing every `hello world` through `use Console`
adds a Tier-2-advanced concept to the floor without buying anything.

What helpers must declare:

```kai
fn greet(name: String) : Unit / Stdout = print("hola, #{name}")
```

The row obligation stays — `Stdout` is visible in the signature, so
Tier 1 #1 (effects-in-types) is honored. `main` is the pinned
exception (its row is inferred from the body, defaults handlers
auto-installed by the runtime).

`use Effect` is reserved for **m7e** — opening the namespace of a
**user-declared** effect so `Stack.push(x)` becomes `push(x)` after
`use Stack`. That feature lives in Full, not Core. Until it lands,
custom effects are called with their explicit `Eff.op(...)` form
(see `stack/main.kai`).
