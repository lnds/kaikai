# `kai fmt` and line width

`kai fmt` lays code out against a line width. A construct that fits
on its line stays there; one that does not breaks at the construct's
own break points; and a line break the author already wrote at one of
those points is kept. This document fixes the width, states the
layout policy, and describes how the model is measured. The numbers
live in `tools/fmt-width-baseline.txt` and are gated by
`tests/fmt_width.sh`.

## The width: 100 columns, configurable

The default width is **100 columns**.

Measured over stdlib and the compiler (152k source lines) with the
width model, at three candidate widths:

| width | lines the writer emits | `hard` lines (unbreakable, over budget) |
|---|---|---|
| 80 | 210,317 | 1,580 |
| **100** | **194,725** | **762** |
| 120 | 190,353 | 481 |

100 is where the corpus stops paying much for the budget: going from
120 to 100 costs 4.4k extra broken lines and leaves 0.5% of the
source as `hard` (a string literal, an identifier, a comment tail
that no line-breaking algorithm can shorten); going on to 80 costs
another 15.6k lines and doubles the `hard` count. It also leaves room
for the shapes kaikai has and C-family languages do not: a signature
carrying an effect row (`fn f(...) : T / E1 + E2`), a refinement
predicate, a `handle ... with E` head.

The width is a setting, not a law:

- `kai fmt --width N <file>` for one run (also `--check`, `--stdin`);
- a `[fmt]` table in the package's `kai.toml`:

  ```toml
  [fmt]
  width = 80
  ```

  applies to every file under that manifest, unless `--width` says
  otherwise;
- 100 when neither is given.

Everything in-tree is measured at 100.

## The layout policy

The writer emits text directly, so a layout is decided before its
text exists: each decision renders the candidate into a probe state,
measures it, and keeps the render when it fits
(`compiler/fmt_layout.kai`). Three shapes cover every construct the
writer breaks.

**A body hanging off `=` / `->` / `:=`** — a function's `= expr`
body, a `let` / `var` right-hand side, an assignment, a match arm, a
handler clause. On the head line when it fits there; on its own line,
one level in, when it fits there whole; on the head line breaking
inside itself when it must break anyway and its first line fits; on
its own line otherwise.

```kaikai
fn build(host: String, port: Int, retries: Int, verbose: Bool) : Cfg =
  Cfg { host: host, port: port, retries: retries, verbose: verbose }
```

**A delimited sequence** — call arguments, list and record literals,
tuples, map and set literals, a signature's parameter list. Flat when
it fits (a signature's return type, row and body opener count toward
the fit); otherwise one item per line with the closer on its own line
at the outer indent, no trailing comma. Two refinements:

- a list whose elements are all literals or names packs into rows
  instead of one per line (`[2, 3, 5, 7, ...]`);
- a call whose last argument is block-like (a list or record literal,
  a block lambda, a `match`, an `if`, a `handle`) may hug: the leading
  arguments stay flat on the head line and the last one breaks inside
  its own delimiters (`string_concat_all([` … `])`).

```kaikai
fn connect(
  host: String,
  port: Int,
  user: String
) : Conn / Stdout {
```

**An operator chain** — the same-precedence spine of a binary
expression (`a + b + c`, `x and y and z`) and a pipe chain (`xs |> f
| g |? h`). Flat when it fits; otherwise one operand per line, the
operator leading each continuation line, one level in from the line
the chain starts on. A chain holding `-` or `%` trails its operators
instead: neither may open a continuation line (both can start an
expression, so the parser would end the statement). Operands are
parenthesised only where the precedence ladder requires it: `a * b +
c` prints without parentheses, `(a + b) * c` keeps them, and the two
associativity exceptions (`++` and `^` are right-associative,
comparisons do not chain) are honoured.

```kaikai
fn pipeline(xs: [Int]) : [Int] =
  xs
    | (x => x * 2)
    | (x => x + 1)
```

### The author's breaks

The open question the width corpus was written to settle: when the
author broke a line at a point where the model could have joined it,
and the joined form fits, who wins?

**The author's break is kept.** A break at one of the model's own
break points — after `=` / `->`, between the items of a sequence,
before a pipe stage or an operator — makes that group render broken:
the body hangs, the sequence expands (a packed list keeps the
author's rows), the chain goes one operand per line. Two things do
not happen: the writer never joins such a break, and it never breaks a
line that fits and the author kept whole (`already_narrow`).

The reasons, in order:

1. Layout carries intent the AST does not. A vertical pipe chain is
   how `kai info pipes`, `kai info idiomatic` and the stdlib present
   pipelines; the four-line `xs | f | g` flattened to 65 columns is
   inside any budget and has still lost the shape the documentation
   teaches. `collapse` measures exactly this, and only respecting the
   author's breaks brings it to zero.
2. It is what gofmt does for composite literals and what Prettier does
   for objects; both are the formatters people trust to run on save.
3. It is a fixed point. A break the writer emits because the line did
   not fit is a break the author now has, so the second pass makes the
   same decision; a break the author had is reproduced. Idempotency is
   gated over stdlib, the compiler and every example.

The break is detected against the source token stream: the group is
author-broken when its body's first token opens its source line
(parentheses aside — the writer adds and drops those by precedence),
when an item of a sequence opens below everything before it, or when
an operator or its operand opens a line. Comments are breaks too: a
comment between a head and its body forces the hang, so the comment
lands on its own line above the body.

### What does not move

- Structural forms keep their structure: a `match`, a block body, a
  handler, a sum type's variants are always multi-line and were before
  the width model.
- Redundant parentheses the author wrote for emphasis are not
  preserved; they are not in the AST. Necessary ones are.
- A multi-line string literal or raw attribute is one atom: its inner
  newlines are not the writer's and do not count as breaks.

## The two numbers

**`soft`** — over-budget lines the writer *could* have broken. An
over-budget line is charged to the writer only when its longest
unbreakable atom still fits inside the budget. An atom is a quoted
string with its interior spaces, a char literal, a comment from `#` to
end of line, or a run of non-space characters. A line whose indent
plus longest atom already exceeds the budget is **`hard`**: no
line-breaking algorithm can shorten it, it says nothing about the
writer, and it is reported but never gated.

**`collapse`** — lines the writer removed relative to the hand-wrapped
source, summed over the corpus. This is the axis `soft` cannot see,
and the one the author-break policy answers.

`soft_max`, the widest soft line, rides along.

## The corpus

`examples/fmt/width/` holds 18 fixtures, each hand-wrapped the way the
documentation teaches and built so that collapsing it overflows the
budget. One construct family per fixture: `=` bodies, call arguments,
pipe chains, signatures, list and record literals, match arms,
`if`/`else`, nested calls, trailing lambdas, statement bodies,
operator chains, declarations, imports, handler clauses, generic
bounds. Two are controls, asserted absolutely: `already_narrow` (fits
with room to spare; `soft` and `collapse` must be 0 — the net against
a writer that invents breaks) and `unbreakable_atoms` (every
over-budget line is `hard`; `soft` must be 0 — the net against a
metric that charges the writer for a long string literal).

Each fixture carries a `.expected.kai` golden, checked by
`tests/fmt_fixtures.sh` for bytes and idempotency.

## Running it

```sh
make test-fmt-width        # the gate (Tier 1, ~1 s)
make test-fmt-ledger       # the report: width + km, before/after a lane
VERBOSE=1 ./tests/fmt_width.sh              # per-fixture table
./tools/fmt-width-report.sh --width 80 F…   # the raw metric on any files
```

`tests/fmt_width.sh` requires `soft == 0` and `collapse == 0` over
the corpus, and pins the three counters in
`tools/fmt-width-baseline.txt` so a run that measures anything else
fails and names the number that moved.

`tests/fmt_ledger.sh` is the wider view and is a report, not a gate:
the width numbers over the width corpus, stdlib and the compiler,
plus `km score` and cognitive complexity per formatter module.

## Where it stands

Measured at 100 columns over the formatter's own output:

| corpus | files | soft | soft_max | hard |
|---|---|---|---|---|
| `examples/fmt/width` | 18 | 0 | 0 | 2 |
| `stdlib` | 74 | 1 | 103 | 90 |
| `stage2/compiler` | 168 | 22 | 155 | 676 |

(Before the width model: 14 / 263 / 4,996 soft lines respectively.)
The soft lines that remain are lines whose code part is short and
whose trailing comment carries them past the budget: the comment is
one atom the writer does not reflow, and the metric charges the line
to the writer because that atom alone would have fit.

## Where the width model lives

`compiler/fmt_writer.kai` is the text substrate: the threaded
`FmtSt` now tracks the column, the width, the column of the first
token on every source line, and the probe counters a speculative
render is measured by. `compiler/fmt_layout.kai` is the model: the
probes (`fmt_try_flat`, `fmt_head_fits`) and the three shapes
(`fmt_hang`, `fmt_seq`, `fmt_chain`); nothing in it walks the AST.
`compiler/fmt_chain.kai` reads the parser's precedence ladder back
(which operand needs parentheses, how a nest flattens into a chain,
where an expression's first token sits). `compiler/fmt_expr.kai` and
`compiler/fmt_decl.kai` — the walker — pass emitters and source
facts to the model and emit nothing width-related themselves.

Three constraints hold, all gated:

- **Meaning preservation** (`tests/fmt_property.sh`) — the reparsed
  output denotes what the input denoted, no identifier invented, none
  dropped.
- **Idempotency** — `fmt(fmt(x)) == fmt(x)`, on the fixtures
  (`tests/fmt_fixtures.sh`) and over stdlib + the compiler
  (`tests/fmt_selfhost.sh`).
- **The code-quality bar** — the modules the model adds score `km`
  A− or better. The ledger reports it per module.
