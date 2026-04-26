# vs — comparative demos

Each demo showcases a case where **kaikai's design wins** against another
mainstream language. The header of every `main.kai` includes the
equivalent code in the contrasting language so the trade-off is visible
side-by-side.

The demos are written in **target syntax** (post-m7e / m8 / m12.5 /
m12.6). Most do not compile today; they document the language we are
building and the pitch that comes with it.

## The match-ups

| vs | What kaikai wins on |
|---|---|
| `rust/`    | memory safety **without** the borrow checker — Perceus + isolated fibers |
| `go/`      | error handling without the `if err != nil` chain — `!` postfix + effects |
| `scala/`   | typed effects **without** monad transformers — handlers as first-class |
| `kotlin/`  | typed cancellation in the row, not in coroutine context tags |
| `elixir/`  | typed actors with exhaustive `match` instead of dynamic `receive` |
| `python/`  | typed data pipelines with compile-time errors instead of `KeyError` |
| `ruby/`    | DSLs via effect handlers, not open classes |
| `java/`    | domain modelling with refinement types + UoM + contracts |

## Usage

```sh
make -C demos/vs verify
make -C demos/vs <lang>
make -C demos/vs list
make -C demos/vs clean
```

Same conventions as `demos/Makefile` and `demos/9d9l/Makefile`:
graceful FAIL reporting, kaic2 directly, never aborts.

## Caveat

These are **pitch-shaped** demos: each picks a corner case where
kaikai's choice is most visible. They do not claim kaikai is uniformly
better than every language at every task. Each contrasted language has
strengths kaikai does not (Rust's no-GC budget, Go's deployment
simplicity, Scala's macro power, Kotlin's IntelliJ tooling, Elixir's
20-year battle-tested BEAM, Python's library ecosystem, Ruby's Rails
inertia, Java's enterprise reach).

The point of this directory is to make the kaikai-specific value
**legible** — so a senior reading the demo sees, in 30 lines, why kaikai
exists.
