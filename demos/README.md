# Demos

Small, complete kaikai programs. `make -C demos verify` compiles and runs
each one and diffs its stdout against the golden when there is one; tier 0
fails if fewer demos pass than `baseline.txt` records.

```sh
make -C demos verify    # compile + run every demo, print PASS/FAIL table
make -C demos <topic>   # try a single demo
make -C demos list
make -C demos clean
```

Uses the compiler at `../stage2/kaic2` (run `make` at the project root
first).

See [STATUS.md](STATUS.md) for the rationale and how to read the output.

## Layout

```text
demos/
  Makefile
  README.md
  STATUS.md
  baseline.txt               # minimum passing count
  <topic>/
    main.kai                 # entry point (required)
    main.out.expected        # golden stdout (optional)
    main.env                 # VAR=value lines for the run (optional)
    *.kai                    # additional modules (optional)
    fixtures/                # input data (optional)
```

`wc.kai` is a single-file demo outside the `verify` set:
`./bin/kai build demos/wc.kai -o wc && ./wc <file>`. `9d9l/` holds larger
programs with their own Makefile (see [9d9l/README.md](9d9l/README.md)).

## Effect convention

Demos call the core IO functions (`print`, `println`, `eprint`,
`read_line`, `read_file`, `write_file`) by their bare names, without
`use Console`. Each carries its effect in its signature (`Stdout`,
`Stderr`, `Stdin`, `File`), so a helper that calls one declares that
effect in its row:

```kaikai
fn greet(name: String) : Unit / Stdout = println("hola, #{name}")

fn main() = greet("kaikai")
```

`main` is the exception: its row is inferred from the body, and the
runtime installs the default handlers.

Operations of a user-declared effect are called qualified
(`Stack.push(x)`, see `stack/main.kai`); `use Stack` opens them so
`push(x)` resolves by bare name.
