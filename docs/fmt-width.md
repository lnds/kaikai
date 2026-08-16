# `kai fmt` and line width

`kai fmt` decides line breaks from syntactic shape alone. A `match`
goes multi-line, an `=` body goes inline — never because of how long
the resulting line is. Any construct the writer considers "inline" is
therefore emitted at whatever width it reaches, and hand-wrapped code
is collapsed onto one line (issue #1812).

This document fixes the target width, defines how the gap is measured,
and states what the writer rewrite has to reach. It is the spec the
rewrite is graded against; the numbers live in
`tools/fmt-width-baseline.txt` and are gated by `tests/fmt_width.sh`.

## The budget: 100 columns, configurable

The target width is **100 columns**, and it is configurable.

100 is what the repo already almost obeys: 2.0% of the compiler's
lines and 0.9% of stdlib's exceed it today, against 6.9% and 3.6% over
80. A budget the corpus nearly meets means the writer's width model
reformats the code that is actually too wide instead of rewrapping the
whole tree, which keeps the rewrite's diff reviewable and its risk to
`kai fmt`'s in-place edits low. It also leaves room for the shapes
kaikai has and C-family languages do not: a signature carrying an
effect row (`fn f(...) : T / E1 + E2`), a refinement predicate, a
`handle ... with E` head.

Configurable, because 100 is a default and not a law: the setting
belongs to the lane that lands the width model (a `--width N` flag and
a `kai.toml` key), with 100 as the value everything in-tree is
measured at. Until then `FMT_WIDTH` steers the measurement side only —
the writer has no width to configure yet.

## The two numbers

Width alone does not capture the damage, which is why the harness
measures two things.

**`soft`** — over-budget lines the writer *could* have broken. An
over-budget line is charged to the writer only when its longest
unbreakable atom still fits inside the budget. An atom is a quoted
string with its interior spaces, a char literal, a comment from `#` to
end of line, or a run of non-space characters. A line whose indent
plus longest atom already exceeds the budget is **`hard`**: no
line-breaking algorithm can shorten it, it says nothing about the
writer, and it is reported but never gated. Without that split, a
user's 120-character string literal would read as a formatter
regression forever.

**`collapse`** — lines the writer removed relative to the hand-wrapped
source, summed over the corpus. This is the axis `soft` cannot see.
The worst case in the issue is the vertical pipe chain:

```kaikai
xs
| (x) => x * 2
| (x) => x + 1
```

flattened to `xs | (x => x * 2) | (x => x + 1)` — 65 columns, inside
any budget, `soft` zero, and the style `kai info pipes` teaches is
gone. A width model alone does not fix this; deciding whether a
group's break is the author's to keep is a policy the rewrite must
state, and `collapse` is what shows the decision took effect.

`soft_max`, the widest soft line, rides along: two runs with the same
soft count are not equally bad if one peaks at 104 and the other at
195.

## The corpus

`examples/fmt/width/` holds 18 fixtures, each hand-wrapped the way the
documentation teaches and built so that collapsing it overflows the
budget. One construct family per fixture: `=` bodies, call arguments,
pipe chains, signatures, list and record literals, match arms,
`if`/`else`, nested calls, trailing lambdas, statement bodies,
operator chains, declarations, imports, handler clauses, generic
bounds.

Two are controls, asserted absolutely rather than ratcheted:

- **`already_narrow`** — fits with room to spare and is already a
  fixed point of the writer. `soft` and `collapse` must be 0, today
  and after any rewrite: this is the net against a writer that invents
  breaks nothing needed.
- **`unbreakable_atoms`** — every over-budget line is `hard`. `soft`
  must be 0: the net against a metric that charges the writer for a
  long string literal.

Each fixture carries a `.expected.kai` golden alongside its
`.input.kai`, checked by `tests/fmt_fixtures.sh`. The goldens record
what the writer does **today**, not what it should do. They exist so
the layout change lands as a reviewable diff over one directory
instead of disappearing inside a 3,000-line rewrite; the lane that
changes the layout regenerates them, and that diff is its evidence.

One policy is already encoded there, in `already_narrow`: when an
author's break is not needed, the writer joins it. `shift` is written
on one line in that fixture precisely because the writer collapses the
two-line spelling and we accept that — join-if-it-fits is
Wadler/Prettier behaviour. `collapse` measures the cases where the
join costs the reader something.

## Running it

```sh
make test-fmt-width        # the gate (Tier 1, ~1 s)
make test-fmt-ledger       # the report: width + km, before/after a lane
VERBOSE=1 ./tests/fmt_width.sh              # per-fixture table
./tools/fmt-width-report.sh --width 80 F…   # the raw metric on any files
```

`tests/fmt_width.sh` ratchets `soft`, `soft_max` and `collapse`
against `tools/fmt-width-baseline.txt`. Measuring worse fails as a
regression; measuring **better also fails**, asking for the baseline
to be updated, so the file records progress instead of rotting — the
same discipline as `tools/coverage-baseline.txt` and the skip-lists in
`tests/fmt_selfhost.sh`.

`tests/fmt_ledger.sh` is the wider view and is a report, not a gate:
the width numbers over the width corpus, stdlib and the compiler,
plus `km score` and cognitive complexity per formatter module. Both
axes matter — a document model that fixes the output by growing a
1,500-line module into a 3,000-line one has not paid for itself.

## Where it stands

Measured at 100 columns, `kai fmt` today:

| corpus | files | soft | soft_max | hard |
|---|---|---|---|---|
| `examples/fmt/width` | 18 | 14 | 195 | 2 |
| `stdlib` | 74 | 263 | 1734 | 88 |
| `stage2/compiler` | 158 | 4990 | 2207 | 631 |

The compiler row is the one to sit with: `kai fmt` rewrites its own
sources in place, and doing so would leave ~5,000 lines past the
budget, the worst of them 2,207 columns wide.

## What the rewrite has to reach

The acceptance criterion is `FMT_WIDTH_STRICT=1` passing on the width
corpus: `soft == 0` and `collapse == 0`, with the two controls
untouched. The lane that lands the document model turns that flag on
permanently in `tests/fmt_width.sh`.

Three constraints do not move, all of them already gated:

- **Meaning preservation** (`tests/fmt_property.sh`) — the reparsed
  output denotes what the input denoted, no identifier invented, none
  dropped.
- **Idempotency** — `fmt(fmt(x)) == fmt(x)`, on the fixtures
  (`tests/fmt_fixtures.sh`) and over stdlib + the compiler
  (`tests/fmt_selfhost.sh`). Width-driven breaks must still reach a
  fixed point in one pass.
- **The code-quality bar** — the modules the rewrite adds score `km`
  A− or better, as any new file does (see CLAUDE.md). The ledger
  reports it per module.
